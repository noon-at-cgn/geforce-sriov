// SPDX-License-Identifier: MIT
/*
 * install and uninstall.
 *
 * install copies the built tree into DESTDIR+prefix (staging: nothing else),
 * or into the live system, where it also registers the module source with
 * DKMS, enables the boot units when a device is selected, and runs the driver
 * step (geforce-sriov-driver) unless --driver=skip is given; that step is the
 * only place NVIDIA's own files are rebuilt or replaced, and it keeps its own
 * backup. install never starts a unit or touches a device. Every file it writes
 * is recorded with its SHA-256 in GS_MANIFEST; uninstall removes only recorded
 * files whose hash still matches.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gs_lifecycle.h"
#include "gs_internal.h"

#define DKMS_SRC "/usr/src/" GS_DKMS_NAME "-" GS_VERSION
#define VGPUD_DROPIN_DIR UNIT_DIR "/nvidia-vgpud.service.d"
#define VGPUD_DROPIN VGPUD_DROPIN_DIR "/" GS_VGPUD_DROPIN_NAME

struct entry *gs_manifest_find(struct manifest *m, char type, const char *path)
{
	for (unsigned int i = 0; i < m->n; i++)
		if (m->e[i].type == type && strcmp(m->e[i].path, path) == 0)
			return &m->e[i];
	return NULL;
}

static int manifest_add(struct manifest *m, char type, const char *sha, const char *path)
{
	struct entry *e = gs_manifest_find(m, type, path);

	if (!e) {
		if (m->n == MAX_ENTRIES)
			return -ENOSPC;
		e = &m->e[m->n++];
	}
	e->type = type;
	snprintf(e->sha, sizeof(e->sha), "%s", sha);
	snprintf(e->path, sizeof(e->path), "%s", path);
	return 0;
}

void gs_manifest_drop(struct manifest *m, struct entry *e)
{
	memmove(e, e + 1, (size_t)(&m->e[m->n] - (e + 1)) * sizeof(*e));
	m->n--;
}

/* Lines "f <sha256> <path>", "d - <path>" and "l <target> <path>". A missing file is an empty manifest. */
int gs_manifest_load(const char *file, struct manifest *m)
{
	char *data, *line, *save = NULL;
	size_t len;
	int r = gs_read_whole(file, &data, &len, 1 << 20);

	m->n = 0;
	if (r == -ENOENT)
		return 0;
	if (r)
		return r;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char type, sha[65], path[GS_PATH_MAX];

		if (sscanf(line, "%c %64s %4095s", &type, sha, path) != 3 || (type != 'f' && type != 'd' && type != 'l') ||
		    gs_validate_abs_path(path) || manifest_add(m, type, sha, path)) {
			free(data);
			return -EINVAL;
		}
	}
	free(data);
	return 0;
}

int gs_manifest_save(const char *file, const struct manifest *m)
{
	size_t cap = (size_t)m->n * (GS_PATH_MAX + 80) + 1, off = 0;
	char *buf = malloc(cap);
	int r;

	if (!buf)
		return -ENOMEM;
	for (unsigned int i = 0; i < m->n; i++) {
		int n = snprintf(buf + off, cap - off, "%c %s %s\n", m->e[i].type, m->e[i].sha, m->e[i].path);

		if (n < 0 || (size_t)n >= cap - off) {
			free(buf);
			return -EOVERFLOW;
		}
		off += (size_t)n;
	}
	r = gs_write_file_atomic(file, buf, off, 0644);
	free(buf);
	return r;
}

enum owned gs_manifest_owns(const char *path, const char *want)
{
	char sha[65];

	if (gs_sha256_file(path, sha))
		return gs_exists(path) ? OWN_UNREADABLE : OWN_ABSENT;
	return strcmp(sha, want) == 0 ? OWN_YES : OWN_CHANGED;
}

/* ---- install ------------------------------------------------------------ */

/* Only characters that systemd passes through literally in ExecStart=/Environment=. */
static int unit_safe(const char *s)
{
	for (; *s; s++) {
		char c = *s;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '/' || c == '.' || c == '_' || c == '+' || c == '-'))
			return 0;
	}
	return 1;
}

/* 1 and the offending prefix in out when an existing component of path is a symlink. */
static int symlink_component(const char *path, char *out, size_t len)
{
	char buf[GS_PATH_MAX];
	struct stat st;
	size_t i, n;

	if (gs_pathf(buf, sizeof(buf), "%s", path))
		return 0;
	n = strlen(buf);
	for (i = 1; i <= n; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		buf[i] = '\0';
		if (lstat(buf, &st) == 0 && S_ISLNK(st.st_mode)) {
			gs_copy(out, len, buf);
			return 1;
		}
		if (i < n)
			buf[i] = '/';
	}
	return 0;
}

/* mkdir -p below destdir, recording every directory this run creates. */
static int make_dirs(struct ctx *x, const char *target_dir)
{
	char buf[GS_PATH_MAX], full[GS_PATH_MAX];
	size_t i, len;

	if (gs_pathf(buf, sizeof(buf), "%s", target_dir))
		return -ENAMETOOLONG;
	len = strlen(buf);
	for (i = 1; i <= len; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		buf[i] = '\0';
		if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, buf))
			return -ENAMETOOLONG;
		if (mkdir(full, 0755) == 0) {
			x->written = 1;
			if (manifest_add(&x->m, 'd', "-", buf))
				return -ENOSPC;
		} else if (errno != EEXIST) {
			return -errno;
		}
		/* Staging never follows a link out of DESTDIR (e.g. etc -> /etc). */
		if (x->destdir[0]) {
			struct stat st;

			if (lstat(full, &st) < 0)
				return -errno;
			if (!S_ISDIR(st.st_mode)) {
				fprintf(stderr, "geforce-sriov: %s is not a directory (symbolic links are not followed when staging)\n",
					full);
				return -ELOOP;
			}
		}
		if (i < len)
			buf[i] = '/';
	}
	return 0;
}

/*
 * An existing file at target that no earlier install recorded is never taken over, whatever it holds:
 * uninstall would remove it. -EEXIST with the reason printed, or 0.
 */
static int unowned(struct ctx *x, const char *target)
{
	char full[GS_PATH_MAX];

	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, target))
		return -ENAMETOOLONG;
	if (!gs_exists(full) || gs_manifest_find(&x->m, 'f', target))
		return 0;
	fprintf(stderr, "geforce-sriov: %s exists and was not installed by geforce-sriov; move it away first\n", full);
	return -EEXIST;
}

static int put(struct ctx *x, const char *target, const void *data, size_t len, mode_t mode)
{
	char full[GS_PATH_MAX], dir[GS_PATH_MAX], sha[65];
	char *slash;
	int r;

	if (gs_pathf(dir, sizeof(dir), "%s", target))
		return -ENAMETOOLONG;
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		if ((r = make_dirs(x, dir)))
			return r;
	}
	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, target))
		return -ENAMETOOLONG;
	if ((r = unowned(x, target)))
		return r;
	gs_sha256_buf(data, len, sha);
	if (gs_exists(full)) {
		struct stat st;
		char have[65];

		/* Already in place with the same content, mode and owner: leave it alone. */
		if (gs_sha256_file(full, have) == 0 && strcmp(have, sha) == 0 && lstat(full, &st) == 0 &&
		    S_ISREG(st.st_mode) && (st.st_mode & 07777) == mode && st.st_uid == geteuid()) {
			printf("unchanged %s%s\n", x->destdir, target);
			return manifest_add(&x->m, 'f', sha, target);
		}
	}
	if ((r = gs_write_file_atomic(full, data, len, mode)))
		return r;
	x->written = 1;
	printf("installed %s%s\n", x->destdir, target);
	return manifest_add(&x->m, 'f', sha, target);
}

int gs_install_put(struct ctx *x, const char *target, const void *data, size_t len, mode_t mode)
{
	return put(x, target, data, len, mode);
}

int gs_install_link(struct ctx *x, const char *target, const char *to)
{
	char full[GS_PATH_MAX], have[65];
	struct entry *e = gs_manifest_find(&x->m, 'l', target);
	ssize_t n;

	if (strlen(to) >= sizeof(have) || strchr(to, ' ') || gs_pathf(full, sizeof(full), "%s%s", x->destdir, target))
		return -ENAMETOOLONG;
	n = readlink(full, have, sizeof(have) - 1);
	if (n > 0) {
		have[n] = '\0';
		if (strcmp(have, to) == 0)
			return manifest_add(&x->m, 'l', to, target);
	}
	if (gs_exists(full)) {
		/* Only the link this package made, still as it made it, is replaced. */
		if (!e || n <= 0 || strcmp(have, e->sha) != 0) {
			fprintf(stderr, "geforce-sriov: %s exists and was not installed by geforce-sriov; move it away first\n", full);
			return -EEXIST;
		}
		if (unlink(full))
			return -errno;
	}
	if (symlink(to, full))
		return -errno;
	x->written = 1;
	printf("linked %s -> %s\n", full, to);
	return manifest_add(&x->m, 'l', to, target);
}

int gs_install_copy(struct ctx *x, const char *src, const char *target, mode_t mode)
{
	char *data;
	size_t len;
	int r = gs_read_whole(src, &data, &len, 256u << 20);

	if (r)
		return r;
	r = put(x, target, data, len, mode);
	free(data);
	return r;
}

/* Copy a template, replacing every @NAME@ from the table. */
static int put_template(struct ctx *x, const char *src, const char *target, const char *const *subst)
{
	char *data, *out;
	size_t len, cap, off = 0;
	int r = gs_read_whole(src, &data, &len, 1 << 20);

	if (r)
		return r;
	cap = len + 8 * GS_PATH_MAX;
	out = malloc(cap);
	if (!out) {
		free(data);
		return -ENOMEM;
	}
	for (size_t i = 0; i < len;) {
		size_t k;

		for (k = 0; subst[k]; k += 2) {
			size_t n = strlen(subst[k]);

			if (len - i >= n && memcmp(data + i, subst[k], n) == 0)
				break;
		}
		if (subst[k]) {
			size_t vn = strlen(subst[k + 1]);

			if (off + vn >= cap)
				goto overflow;
			memcpy(out + off, subst[k + 1], vn);
			off += vn;
			i += strlen(subst[k]);
		} else {
			if (off + 1 >= cap)
				goto overflow;
			out[off++] = data[i++];
		}
	}
	free(data);
	/* An unreplaced @NAME@ placeholder would ship a broken unit. */
	for (size_t i = 0; i + 1 < off; i++) {
		if (out[i] == '@' && out[i + 1] >= 'A' && out[i + 1] <= 'Z') {
			free(out);
			return -EINVAL;
		}
	}
	r = put(x, target, out, off, 0644);
	free(out);
	return r;
overflow:
	free(data);
	free(out);
	return -EOVERFLOW;
}

static int migrate_file(struct ctx *x, const char *target, int *kept, const char *why);

/* Install the daemon drop-in and the selected configuration after the driver step. */
static int put_selection(struct ctx *x, const char *src, const char *const *subst, const struct gs_config *cfg,
			 int config_changed, const char *cfgfile)
{
	char p[GS_PATH_MAX], text[GS_PATH_MAX + 1024];
	int n, r;

	gs_pathf(p, sizeof(p), "%s/systemd/nvidia-vgpud.service.d/geforce-sriov.conf.in", src);
	if ((r = put_template(x, p, VGPUD_DROPIN, subst))) {
		fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", VGPUD_DROPIN, strerror(-r));
		return r;
	}
	if (!config_changed) {
		printf("kept %s (unchanged)\n", cfgfile);
	} else {
		n = gs_config_render(cfg, text, sizeof(text));
		r = n < 0 ? -EOVERFLOW : put(x, GS_CONFIG_PATH, text, (size_t)n, 0644);
		if (r) {
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", GS_CONFIG_PATH, strerror(-r));
			return r;
		}
	}
	/*
	 * Host CUDA: udev and NVIDIA's daemons may autoload nvidia_uvm at any time, so the option
	 * belongs to the module configuration, not only to activation's own modprobe.
	 */
	if (cfg->host_cuda == GS_SW_ON) {
		static const char option[] = "options nvidia_uvm uvm_disable_hmm=1\n";

		if ((r = put(x, GS_UVM_OPTION_PATH, option, sizeof(option) - 1, 0644))) {
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", GS_UVM_OPTION_PATH, strerror(-r));
			return r;
		}
	} else if (gs_manifest_find(&x->m, 'f', GS_UVM_OPTION_PATH)) {
		int kept = 0;

		if ((r = migrate_file(x, GS_UVM_OPTION_PATH, &kept, NULL))) {
			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", GS_UVM_OPTION_PATH, strerror(-r));
			return r;
		}
		if (kept)
			printf("%s was changed since install; kept\n", GS_UVM_OPTION_PATH);
	}
	/*
	 * udev resolves nvidia_drm from its PCI class alias and inserts it as soon as a selected GPU
	 * appears. A display module that holds the GPU makes NVIDIA's driver deny unbindLock, and the
	 * VF enable then fails. Mapping the three aliases away keeps udev from loading anything for
	 * them; nvidia, nvidia_drm and nvidia_vgpu_vfio all carry them. The early unit loads nvidia
	 * (--load-driver) and activation loads the other two, so the file is written only while a GPU
	 * is selected, which is when those units are enabled. Without a selection nothing would load
	 * NVIDIA's driver at boot, so an unchanged copy from an earlier install is removed.
	 */
	if (cfg->nbdfs > 0) {
		static const char autoload[] =
			"alias pci:v000010DEd*sv*sd*bc06sc80i00* off\n"
			"alias pci:v000010DEd*sv*sd*bc03sc02i00* off\n"
			"alias pci:v000010DEd*sv*sd*bc03sc00i00* off\n";

		if ((r = put(x, GS_AUTOLOAD_PATH, autoload, sizeof(autoload) - 1, 0644))) {
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", GS_AUTOLOAD_PATH, strerror(-r));
			return r;
		}
	} else if (gs_manifest_find(&x->m, 'f', GS_AUTOLOAD_PATH)) {
		int kept = 0;

		if ((r = migrate_file(x, GS_AUTOLOAD_PATH, &kept, "it is not needed while no GPU is selected"))) {
			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", GS_AUTOLOAD_PATH, strerror(-r));
			return r;
		}
		if (kept)
			printf("%s was changed since install; kept\n", GS_AUTOLOAD_PATH);
	}
	/* A display option file written by an earlier install would make any autoload of
	 * nvidia_drm take KMS and lose NVIDIA's unbindLock during the VF enable window. It is
	 * removed if an earlier install recorded it; activation loads the stack itself. */
	if (gs_manifest_find(&x->m, 'f', GS_DRM_OPTION_PATH)) {
		int kept = 0;

		if ((r = migrate_file(x, GS_DRM_OPTION_PATH, &kept, NULL))) {
			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", GS_DRM_OPTION_PATH, strerror(-r));
			return r;
		}
		if (kept)
			printf("%s was changed since install; kept\n", GS_DRM_OPTION_PATH);
	}
	return 0;
}
/* Removes target while it is the unchanged installed copy; why says the reason, NULL means a file this version no longer uses. */
static int migrate_file(struct ctx *x, const char *target, int *kept, const char *why)
{
	char full[GS_PATH_MAX];
	struct entry *e = gs_manifest_find(&x->m, 'f', target);
	enum owned state;

	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, target))
		return -ENAMETOOLONG;
	if (!gs_exists(full))
		return 0;
	state = e ? gs_manifest_owns(full, e->sha) : OWN_CHANGED;
	if (state != OWN_YES) {
		if (kept)
			*kept = 1;
		fprintf(stderr, "geforce-sriov: kept %s: it was edited or was not installed by this package; "
			"remove it before the next boot\n", full);
		return 0;
	}
	if (unlink(full)) {
		int err = errno;

		fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", full, strerror(err));
		return -err;
	}
	x->written = 1;
	memmove(e, e + 1, (size_t)(&x->m.e[x->m.n] - (e + 1)) * sizeof(*e));
	x->m.n--;
	if (why)
		printf("removed %s: %s\n", full, why);
	else
		printf("removed obsolete %s\n", full);
	return 0;
}

/* The old preparation unit, its enablement link and the hold file; removes only unchanged files. */
static int migrate_legacy(struct ctx *x, int *unit_kept)
{
	char link[GS_PATH_MAX];
	int kept = 0, r;

	if ((r = migrate_file(x, UNIT_DIR "/" GS_LEGACY_UNIT_PREPARE, unit_kept, NULL)))
		return r;
	/* systemctl enable made this link; it is not a file the manifest records. */
	if (gs_pathf(link, sizeof(link), "%s%s", x->destdir, GS_LEGACY_UNIT_WANTS))
		return -ENAMETOOLONG;
	if (gs_exists(link)) {
		if (unlink(link)) {
			int err = errno;

			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", link, strerror(err));
			return -err;
		}
		x->written = 1;
		printf("removed obsolete %s\n", link);
	}
	/*
	 * The hold that kept nvidia from loading before the old unit: the early livepatch unit now loads
	 * first, so the hold would only keep the driver from loading at all.
	 */
	if ((r = migrate_file(x, GS_LEGACY_HOLD, &kept, NULL)))
		return r;
	/* The old deferral marker of a boot whose preparation was skipped. */
	if (!x->destdir[0] && gs_exists(GS_LEGACY_DEFERRED)) {
		if (unlink(GS_LEGACY_DEFERRED)) {
			int err = errno;

			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", GS_LEGACY_DEFERRED, strerror(err));
			return -err;
		}
		printf("removed obsolete %s\n", GS_LEGACY_DEFERRED);
	}
	return 0;
}

/* Parse old package-owned configuration after removing its obsolete line. */
static int load_migrated_config(struct ctx *x, const char *path, struct gs_config *cfg,
				char *err, size_t errlen, int *changed)
{
	char *text, *line, *next, *out;
	size_t len, used = 0;
	struct entry *e;
	int r = gs_read_whole(path, &text, &len, GS_CONFIG_MAX_BYTES);

	if (r == -ENOENT)
		return gs_config_load(path, cfg, err, errlen);
	if (r)
		return r;
	out = malloc(len + 1);
	if (!out) {
		free(text);
		return -ENOMEM;
	}
	for (line = text; line < text + len; line = next) {
		char *start = line, *end = memchr(line, '\n', (size_t)(text + len - line));
		size_t n = end ? (size_t)(end - line + 1) : (size_t)(text + len - line);

		next = line + n;
		while (start < next && (*start == ' ' || *start == '\t'))
			start++;
		if ((size_t)(next - start) >= sizeof("boot_display_release") - 1 &&
		    !memcmp(start, "boot_display_release", sizeof("boot_display_release") - 1) &&
		    (start[sizeof("boot_display_release") - 1] == '=' ||
		     start[sizeof("boot_display_release") - 1] == ' ' ||
		     start[sizeof("boot_display_release") - 1] == '\t')) {
			e = gs_manifest_find(&x->m, 'f', GS_CONFIG_PATH);
			if (!e || gs_manifest_owns(path, e->sha) != OWN_YES) {
				fprintf(stderr, "geforce-sriov: obsolete config line in %s: %.*s%sRemove this line and rerun install; it does not grant display-manager consent.\n",
					path, (int)n, line, end ? "" : "\n");
				free(out);
				free(text);
				return -EEXIST;
			}
			*changed = 1;
			continue;
		}
		memcpy(out + used, line, n);
		used += n;
	}
	out[used] = '\0';
	r = gs_config_parse(out, used, cfg, err, errlen);
	if (!r)
		cfg->present = 1;
	free(out);
	free(text);
	return r;
}

static int self_dir(char *out, size_t len)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	char *slash;

	if (n <= 0)
		return -errno;
	exe[n] = '\0';
	slash = strrchr(exe, '/');
	if (!slash)
		return -EINVAL;
	*slash = '\0';
	return gs_pathf(out, len, "%s", exe[0] ? exe : "/");
}

int gs_kept_input(const char *dir, const char *name)
{
	char path[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t len, n = strlen(name);
	int found = 0;

	if (gs_pathf(path, sizeof(path), "%s/index", dir) || gs_read_whole(path, &data, &len, 65536))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save))
		found = strncmp(line, name, n) == 0 && line[n] == '=' && line[n + 1];
	free(data);
	return found;
}

/* display and host_cuda are the effective values the config holds, so the printed command builds what was chosen. */
static void print_live_steps(const char *bindir, int enable_unit, const struct gs_install_opts *o, int display,
			     int host_cuda)
{
	if (enable_unit)
		printf("  %s/geforce-sriov doctor   (checks the selected GPUs on this host; staging did not)\n", bindir);
	printf("  dkms add -m %s -v %s && dkms build -m %s -v %s && dkms install -m %s -v %s\n", GS_DKMS_NAME,
	       GS_VERSION, GS_DKMS_NAME, GS_VERSION, GS_DKMS_NAME, GS_VERSION);
	if (o->skip_driver)
		printf("  NVIDIA's module and plugin were skipped (--driver=skip): install them by hand\n"
		       "  (docs/driver-patch.md), or rerun `%s/geforce-sriov install` without it\n", bindir);
	else {
		printf("  %s/geforce-sriov-driver --source %s%s%s   (NVIDIA's module and plugin)\n", bindir,
		       o->nvidia_source ? o->nvidia_source : gs_nvidia_source_default(), display ? " --display" : "",
		       host_cuda ? " --host-cuda" : "");
		if (o->companion_file)
			printf("    with --companion-file %s\n", o->companion_file);
		if (o->companion_source)
			printf("    with --companion-source %s\n", o->companion_source);
		if (o->nvkms_source)
			printf("    with --nvkms-source %s\n", o->nvkms_source);
	}
	printf("  systemctl daemon-reload\n");
	if (enable_unit)
		printf("  systemctl enable %s %s   (they run at the next boot)\n", GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE);
	else
		printf("  select the GPUs with `%s/geforce-sriov install --device BDF`, one per GPU (it sets bdfs=,\n"
		       "  identity=on and identity_force_host=on and enables %s and %s), or set those keys in %s,\n"
		       "  then run `systemctl enable %s %s`\n",
		       bindir, GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE, GS_CONFIG_PATH, GS_UNIT_LIVEPATCH,
		       GS_UNIT_ACTIVATE);
}

#define NOTICE_RULE "========================================================================\n"

int gs_color(FILE *f)
{
	const char *no_color = getenv("NO_COLOR"), *term = getenv("TERM");

	return isatty(fileno(f)) && !(no_color && *no_color) && !(term && strcmp(term, "dumb") == 0);
}

void gs_d3cold_notice_color(FILE *f, int color)
{
	if (color)
		fputs("\033[31m", f);
	fputs(NOTICE_RULE
	      "IMPORTANT: This laptop GPU can power off when idle.\n"
	      "\n"
	      "This power state is called D3cold. When power returns, the GPU restarts,\n"
	      "losing its SR-IOV setup and stopping every running vGPU. Its firmware\n"
	      "resets too.\n"
	      "\n"
	      "setup can save your consent as d3cold=off in the config.\n"
	      "activate reapplies the power settings every time it brings the GPUs up.\n"
	      "These runtime settings do not survive a reboot.\n"
	      "\n"
	      "This keeps the GPU and its parent bridge powered, even when idle.\n"
	      "It means shorter battery life and a warmer laptop.\n"
	      "\n"
	      "You must also configure these yourself; setup does neither:\n"
	      "  - Block suspend, including on lid close and when idle. Sleep powers\n"
	      "    the GPU off.\n"
	      "  - Disable NVIDIA's runtime power management. Run as root:\n"
	      "      echo 'options nvidia NVreg_DynamicPowerManagement=0' > /etc/modprobe.d/nvidia-power.conf\n"
	      "\n"
	      "Without these safeguards, an idle GPU can power off and stop its vGPUs.\n"
	      "To revert, run `geforce-sriov uninstall`.\n"
	      "Restore any sleep or modprobe settings you changed yourself.\n"
	      NOTICE_RULE, f);
	if (color)
		fputs("\033[0m", f);
}

void gs_d3cold_notice(FILE *f)
{
	gs_d3cold_notice_color(f, gs_color(f));
}

/*
 * The closing lines of an install that found everything in place. This run put nothing in place
 * that a boot would pick up, so it asks for no power cycle of its own. An earlier install may still
 * wait for one: when the running nvidia module is not the installed build, that is said. DKMS
 * builds the livepatch module on every install; with unchanged sources that is the same module again.
 */
static void install_unchanged(enum gs_driver_result drv)
{
	if (drv == GS_DRIVER_RECORD_ONLY)
		printf("\nOnly the build record changed: it now holds the current version classification of the installed build.\n");
	else
		printf("\nNothing changed.\n");
	printf("geforce-sriov's files, NVIDIA's module and plugin, the config and the boot units were already as this\n"
	       "install leaves them. DKMS built the livepatch module again from the same source.\n");
	if (gs_loaded_not_installed(GS_LIVE_SYSFS, NULL))
		printf("The nvidia module that is running is not the installed build: an earlier install still waits for its\n"
		       "power cycle. Power the machine off and on; the boot units then bring the GPUs up.\n");
	else
		printf("This run needs no power cycle. One that an earlier install asked for still applies.\n");
}

void gs_install_next(const char *bindir, const struct gs_config *cfg, int from_setup)
{
	printf("\n%s and %s are enabled for the next boot, not started.\n", GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE);
	printf("To skip geforce-sriov for one boot, add geforce_sriov.skip to the kernel command line. That boot loads no\n"
	       "NVIDIA driver for any NVIDIA GPU; `modprobe nvidia` loads it by hand, without vGPU\n"
	       "(docs/recovery.md#turn-features-off).\n");
	/* Under setup the instructions are setup's: one closing block, not two that differ. */
	if (!from_setup)
		printf("\nNext: run `%s/geforce-sriov doctor`, then power the machine off and on. A restart is not\n"
		       "enough: the card must come up with the new module and the early livepatch unit must load\n"
		       "before udev.\n", bindir);
	if (cfg->display != GS_DISPLAY_OFF)
		printf("With display selected, the first boot may briefly switch the console to the selected\n"
		       "display GPU; activation restarts the display manager it stopped.\n");
	if (!from_setup)
		printf("After the boot, `geforce-sriov status` shows each GPU and its next step.\n");
}

/* The supported VM autostart units this package orders after activation; unrelated ones stay untouched. */
const char *const gs_vm_autostart_units[] = { "libvirt-guests.service", "pve-guests.service", NULL };

/* 1 when name states a dotted numeric version (580.159.01, 580.159.03) and nothing else. */
static int version_name(const char *name)
{
	unsigned int dots = 0, digits = 0;

	for (; *name; name++) {
		if (*name >= '0' && *name <= '9') {
			digits++;
			continue;
		}
		if (*name == '.' && digits) {
			dots++;
			digits = 0;
			continue;
		}
		return 0;
	}
	return dots >= 2 && digits > 0;
}

/*
 * The NVIDIA host version the selected input states in its name, if it does: the .run package's
 * name, a tree named nvidia-<version>, or an extracted package directory named <version>. A name
 * that states none says nothing about the version.
 */
int gs_install_version_named(const struct gs_install_opts *o, char *out, size_t len)
{
	const char *path = o->driver_file ? o->driver_file : o->nvidia_source ? o->nvidia_source : gs_nvidia_source_default();
	const char *name;

	if (o->driver_file)
		return gs_nvidia_package_version(path, out, len);
	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (strncmp(name, "nvidia-", 7) == 0)
		name += 7;
	if (!version_name(name))
		return -1;
	gs_copy(out, len, name);
	return 0;
}

/* 1 when the selected package or tree names NVIDIA's open flavour, which this build cannot use. */
int gs_nvidia_open_flavour(const struct gs_install_opts *o)
{
	const char *path = o->driver_file ? o->driver_file : o->nvidia_source;
	const char *name;

	if (!path)
		return 0;
	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	return strstr(name, "-open") != NULL;
}

/* The version notice install prints once, before anything is written. */
static void version_notice(const struct gs_install_opts *o)
{
	char version[64], line[512];

	if (gs_install_version_named(o, version, sizeof(version)))
		return;
	gs_version_format(gs_version_confirmed(GS_VERSION_HOST, version) ? GS_VERSION_MSG_CONFIRMED
									 : GS_VERSION_MSG_INSTALL_NOTICE,
			  version, line, sizeof(line));
	printf("%s\n", line);
}

/* 1 when the effective build needs display, with the line that says why; 0 otherwise. */
int gs_display_effective(const char *sys, const struct gs_config *cfg, char *why, size_t len)
{
	if (cfg->display == GS_DISPLAY_OFF) {
		gs_copy(why, len, "Display: not enabled.");
		return 0;
	}
	for (unsigned int i = 0; i < cfg->nbdfs; i++) {
		struct gs_card c;

		if (sys)
			gs_card_inspect(sys, &cfg->bdfs[i], &c);
		if (sys && c.present && c.boot_vga == 1) {
			if (cfg->display == GS_DISPLAY_ON)
				gs_copy(why, len, "Display: explicitly enabled.");
			else
				snprintf(why, len, "Display: automatic for selected boot-display GPU %s.", cfg->bdfs[i].str);
			return 1;
		}
	}
	/* A staged tree reads no card: the request stands and the target host judges it. */
	if (cfg->display == GS_DISPLAY_ON && !sys) {
		gs_copy(why, len, "Display: explicitly enabled.");
		return 1;
	}
	if (cfg->display == GS_DISPLAY_ON) {
		gs_copy(why, len, "Display: display=on is set, but no selected GPU is the boot display. The host console is only "
			"ever put on the boot display GPU, so the display modules are not built.");
		return 0;
	}
	gs_copy(why, len, "Display: not enabled.");
	return 0;
}

/* 1 when a VM autostart unit name exists, below destdir ("" for a live host) in any system unit directory. */
static int vm_unit_present(const char *destdir, const char *name)
{
	static const char *const dirs[] = {
		"/etc/systemd/system", "/run/systemd/system", "/usr/local/lib/systemd/system",
		"/usr/lib/systemd/system", "/lib/systemd/system",
	};
	char p[GS_PATH_MAX];

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s%s/%s", destdir, dirs[i], name) && gs_exists(p))
			return 1;
	return 0;
}

/* How many regular files below src/sub start with prefix and end with suffix (prefix may be NULL). */
static unsigned int count_entries(const char *src, const char *sub, const char *prefix, const char *suffix)
{
	char p[GS_PATH_MAX];
	unsigned int n = 0;
	struct dirent *de;
	struct stat st;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/%s", src, sub) || !(d = opendir(p)))
		return 0;
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name), slen = strlen(suffix);

		if (prefix && strncmp(de->d_name, prefix, strlen(prefix)) != 0)
			continue;
		if (len < slen || strcmp(de->d_name + len - slen, suffix) != 0)
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s/%s", src, sub, de->d_name) || stat(p, &st) || !S_ISREG(st.st_mode))
			continue;
		n++;
	}
	closedir(d);
	return n;
}

/* Copy each regular file below src/sub that starts with prefix and ends with suffix to target_dir/name. */
static int copy_entries(struct ctx *x, const char *src, const char *sub, const char *target_dir, const char *prefix,
			const char *suffix, mode_t mode)
{
	char dp[GS_PATH_MAX], fp[GS_PATH_MAX], q[GS_PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int r;

	if (gs_pathf(dp, sizeof(dp), "%s/%s", src, sub) || !(d = opendir(dp)))
		return -ENOENT;
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name), slen = strlen(suffix);

		if (prefix && strncmp(de->d_name, prefix, strlen(prefix)) != 0)
			continue;
		if (len < slen || strcmp(de->d_name + len - slen, suffix) != 0)
			continue;
		if (gs_pathf(fp, sizeof(fp), "%s/%s", dp, de->d_name) || stat(fp, &st) || !S_ISREG(st.st_mode))
			continue;
		if (gs_pathf(q, sizeof(q), "%s/%s", target_dir, de->d_name) || (r = gs_install_copy(x, fp, q, mode))) {
			closedir(d);
			return r ? r : -ENAMETOOLONG;
		}
	}
	closedir(d);
	return 0;
}

int gs_cmd_install(const struct gs_install_opts *o)
{
	const char *prefix = o->prefix ? o->prefix : GS_DEFAULT_PREFIX;
	const char *destdir = o->destdir ? o->destdir : "";
	int live = destdir[0] == '\0';
	char src[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], bindir[GS_PATH_MAX], libdir[GS_PATH_MAX];
	char hook[GS_PATH_MAX], sharedir[GS_PATH_MAX], cfgfile[GS_PATH_MAX], manifest[GS_PATH_MAX];
	char err[512], prog[GS_PATH_MAX], pkgdir[GS_PATH_MAX], pkgkernel[GS_PATH_MAX] = "";
	char display_why[256] = "";
	struct gs_config cfg;
	int config_changed = 0, cfg_exists, migrated = 0, legacy_kept = 0, d3cold_dropped, r, display;
	int feature_change = 0, changed_files, cuda_written = 0, cuda_stage = 0;
	char cudadir[GS_PATH_MAX];
	const struct gs_cuda_io cuda_io = { gs_run, gs_run_capture };
	enum gs_driver_result drv = GS_DRIVER_UNCHANGED;
	struct ctx *x;
	DIR *d;
	struct dirent *de;
	static const char *const artifacts[] = {
		"geforce-sriov", "driver/geforce-sriov-patch", "hook/libgeforce_sriov.so",
		"include/gs_devices.h", "systemd/geforce-sriov-livepatch.service.in",
		"systemd/geforce-sriov-activate.service.in",
		"systemd/nvidia-vgpud.service.d/geforce-sriov.conf.in",
		"systemd/geforce-sriov-vm-autostart.conf.in",
		"scripts/geforce-sriov-driver.sh", "scripts/gs-driver-versions.tbl", "config/config.example",
		"driver-patches/fb-owner/gs-fb-owner.c", "driver-patches/fb-owner/gs-fb-owner.h",
		"driver-patches/fb-owner/fb-owner.patch", NULL,
	};

	if (gs_validate_abs_path(prefix) || !unit_safe(prefix)) {
		fprintf(stderr, "geforce-sriov: --prefix must be a normalized absolute path of letters, digits and / . _ + - only (it is written into systemd units)\n");
		return GS_EXIT_USAGE;
	}
	if (!live && (gs_validate_abs_path(destdir) || strcmp(destdir, "/") == 0)) {
		fprintf(stderr, "geforce-sriov: DESTDIR must be a normalized absolute path other than / (omit it for a live install)\n");
		return GS_EXIT_USAGE;
	}
	if (!live && symlink_component(destdir, p, sizeof(p))) {
		fprintf(stderr, "geforce-sriov: DESTDIR component %s is a symbolic link; staging does not follow links\n", p);
		return GS_EXIT_REFUSED;
	}
	if (!live && o->driver_file) {
		fprintf(stderr, "geforce-sriov: --driver-file acts on the live system; omit it when staging and\n"
			"pass it to the live install on the target host\n");
		return GS_EXIT_USAGE;
	}
	if (o->source)
		r = gs_pathf(src, sizeof(src), "%s", o->source);
	else
		r = self_dir(src, sizeof(src));
	if (r) {
		fprintf(stderr, "geforce-sriov: cannot locate the built tree\n");
		return GS_EXIT_FAIL;
	}
	/*
	 * The installed copy sits beside the driver step, not in a build tree. It cannot install itself
	 * again; what a user wants from it after a kernel update is the driver step, which is rebuild.
	 */
	if (!o->source && !gs_pathf(p, sizeof(p), "%s/geforce-sriov-driver", src) && gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: %s/geforce-sriov is the installed copy, and install runs from the\n"
			"source folder it was built in.\n"
			"After a kernel update, run `sudo geforce-sriov rebuild`: it builds NVIDIA's modules again for the running\n"
			"kernel, with the features the config selects and the inputs the install kept.\n"
			"To change the installation, run `sudo ./geforce-sriov install` in the source folder.\n", src);
		return GS_EXIT_REFUSED;
	}
	for (size_t i = 0; artifacts[i]; i++) {
		if (gs_pathf(p, sizeof(p), "%s/%s", src, artifacts[i]) || !gs_exists(p)) {
			fprintf(stderr, "geforce-sriov: %s/%s is missing; run make first\n", src, artifacts[i]);
			return GS_EXIT_REFUSED;
		}
	}
	for (size_t i = 0; kernel_required[i]; i++) {
		if (gs_pathf(p, sizeof(p), "%s/kernel/%s", src, kernel_required[i]) || !gs_exists(p)) {
			fprintf(stderr, "geforce-sriov: %s is missing from the source tree\n", p);
			return GS_EXIT_REFUSED;
		}
	}
	/* The driver step sources its libraries and applies the source patches beside itself. */
	if (!count_entries(src, "scripts", "gs-driver-", ".sh")) {
		fprintf(stderr, "geforce-sriov: %s/scripts/gs-driver-*.sh is missing from the source tree\n", src);
		return GS_EXIT_REFUSED;
	}
	if (!count_entries(src, "driver-patches", NULL, ".patch")) {
		fprintf(stderr, "geforce-sriov: %s/driver-patches/*.patch is missing from the source tree\n", src);
		return GS_EXIT_REFUSED;
	}
	/* Open flavour first, independently of the version; then the version notice, once. */
	if (gs_nvidia_open_flavour(o)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: NVIDIA's open flavour cannot be used by this build. "
			"--allow-unsupported-driver does not override this check.\n");
		return GS_EXIT_REFUSED;
	}
	version_notice(o);
	if (live) {
		if (geteuid() != 0) {
			fprintf(stderr, "geforce-sriov: a live install must run as root (set DESTDIR to stage)\n");
			return GS_EXIT_REFUSED;
		}
		if ((r = gs_lock()))
			return r;
		/* Another NVIDIA driver stops the run first, before the prerequisites are checked and before any write. */
		if (o->driver_file && (r = gs_package_conflict(o->driver_file, "", o->accept_nvidia_license)))
			return r;
		if ((r = gs_deps_step()))
			return r;
		if (gs_find_program("dkms", prog, sizeof(prog)) || gs_find_program("systemctl", prog, sizeof(prog))) {
			fprintf(stderr, "geforce-sriov: a live install needs dkms and systemctl\n");
			return GS_EXIT_REFUSED;
		}
		if (gs_exists(GS_ATTEMPT_MARKER))
			printf("warning: activation lock %s is set by an unfinished activation. Activation refuses to run until you power the machine off and on and run `geforce-sriov activate --clear-lock`\n",
			       GS_ATTEMPT_MARKER);
	}

	if (!live && (r = gs_mkdir_p(destdir, 0755))) {
		fprintf(stderr, "geforce-sriov: cannot create DESTDIR %s: %s\n", destdir, strerror(-r));
		return GS_EXIT_FAIL;
	}

	gs_pathf(bindir, sizeof(bindir), "%s/bin", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(libdir, sizeof(libdir), "%s/lib/geforce-sriov", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(cudadir, sizeof(cudadir), "%s/cuda", libdir);
	gs_pathf(sharedir, sizeof(sharedir), "%s/share/geforce-sriov", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(hook, sizeof(hook), "%s/libgeforce_sriov.so", libdir);
	gs_pathf(cfgfile, sizeof(cfgfile), "%s%s", destdir, GS_CONFIG_PATH);
	gs_pathf(manifest, sizeof(manifest), "%s%s", destdir, GS_MANIFEST);

	x = calloc(1, sizeof(*x));
	if (!x)
		return GS_EXIT_FAIL;
	x->destdir = destdir;
	if ((r = gs_manifest_load(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: %s is unreadable or corrupt (%s)\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}

	/* Configuration: keep the existing file unless the command line changes it. */
	cfg_exists = gs_exists(cfgfile);
	r = load_migrated_config(x, cfgfile, &cfg, err, sizeof(err), &migrated);
	if (r == -EEXIST) {
		free(x);
		return GS_EXIT_REFUSED;
	}
	if (r) {
		fprintf(stderr, "geforce-sriov: existing config is invalid, fix or remove it: %s\n", err);
		free(x);
		return GS_EXIT_REFUSED;
	}
	if (migrated) {
		config_changed = 1;
		printf("boot_display_release is obsolete; migration removes console-release deferral.\n");
	}
	if (o->ndevices) {
		for (unsigned int i = 0; i < o->ndevices; i++)
			cfg.bdfs[i] = o->devices[i];
		cfg.nbdfs = o->ndevices;
		/*
		 * A user-selected card gets the identity configuration these settings produce: the PCI
		 * info rewritten and the virtualization mode forced to HOST.
		 */
		cfg.identity = GS_SW_ON;
		cfg.identity_force_host = GS_SW_ON;
		config_changed = 1;
	}
	/* Feature selection: the config keeps it, the driver step builds it. */
	feature_change = (o->display_set && o->display != cfg.display) ||
			 (o->host_cuda_set && (cfg.host_cuda == GS_SW_ON) != o->host_cuda);
	if (o->display_set && o->display != cfg.display)
		cfg.display = o->display, config_changed = 1;
	if (o->host_cuda_set && (cfg.host_cuda == GS_SW_ON) != o->host_cuda)
		cfg.host_cuda = o->host_cuda ? GS_SW_ON : GS_SW_OFF, config_changed = 1;
	/* --device without --disable-d3cold drops the D3cold consent too; without --device the config's stays. */
	d3cold_dropped = cfg.d3cold_off && o->ndevices && !o->disable_d3cold;
	if (d3cold_dropped)
		cfg.d3cold_off = 0;
	if (o->disable_d3cold && !cfg.d3cold_off)
		cfg.d3cold_off = 1, config_changed = 1;
	if (o->ov.ari != GS_SW_UNSET && o->ov.ari != cfg.ari)
		cfg.ari = o->ov.ari, config_changed = 1;
	if (o->ov.msix != GS_SW_UNSET && o->ov.msix != cfg.msix)
		cfg.msix = o->ov.msix, config_changed = 1;
	if (o->vfs && o->vfs != cfg.vfs)
		cfg.vfs = o->vfs, config_changed = 1;
	if (!cfg_exists)
		config_changed = 1;
	/* The consent needs a laptop GPU in the selection; staging reads no card, so it records the flag unchecked. */
	if (o->disable_d3cold && live) {
		struct gs_addr laptops[GS_CFG_MAX_BDFS];

		if (!gs_laptop_gpus(GS_LIVE_SYSFS, cfg.bdfs, cfg.nbdfs, laptops)) {
			free(x);
			return gs_d3cold_usage();
		}
	}
	/* Never take over a config this tool did not write. */
	if (cfg_exists && config_changed && !gs_manifest_find(&x->m, 'f', GS_CONFIG_PATH)) {
		fprintf(stderr, "geforce-sriov: %s exists and was not written by geforce-sriov; edit it yourself (see config.example) or remove it, then rerun\n",
			cfgfile);
		free(x);
		return GS_EXIT_REFUSED;
	}
	/* The vgpud drop-in is written after NVIDIA's files: one that is not this tool's refuses now, before any change. */
	if ((r = unowned(x, VGPUD_DROPIN))) {
		if (r != -EEXIST)
			fprintf(stderr, "geforce-sriov: %s%s: %s\n", destdir, VGPUD_DROPIN, strerror(-r));
		free(x);
		return GS_EXIT_REFUSED;
	}

	if (o->ndevices && live) {
		for (unsigned int i = 0; i < o->ndevices; i++) {
			struct gs_card c;

			gs_card_inspect(GS_LIVE_SYSFS, &o->devices[i], &c);
			/* A boot display card is selectable; activation handles its console as part of the transaction. */
			if (gs_check_selectable(&c, 1, err, sizeof(err))) {
				fprintf(stderr, "geforce-sriov: refused --device: %s\n", err);
				free(x);
				return GS_EXIT_REFUSED;
			}
		}
	} else if (o->ndevices) {
		printf("staging: the --device addresses are recorded unchecked; run `geforce-sriov doctor` on the\n"
		       "target host before enabling the boot unit\n");
	}
	/* The VF count against every selected card, the existing selection included. */
	for (unsigned int i = 0; live && i < cfg.nbdfs; i++) {
		struct gs_card c;
		char vfs[128];
		int v;

		gs_card_inspect(GS_LIVE_SYSFS, &cfg.bdfs[i], &c);
		if (!c.present || !c.entry)
			continue;
		gs_vfs_describe(c.entry, &cfg, vfs, sizeof(vfs));
		printf("%s: VFs: %s\n", c.pf.str, vfs);
		v = gs_vfs_check(c.entry, &cfg, err, sizeof(err));
		if (v < 0) {
			fprintf(stderr, "geforce-sriov: refused --vfs: %s: %s\n", c.pf.str, err);
			free(x);
			return GS_EXIT_REFUSED;
		}
		if (v > 0)
			printf("%s: warning: %s\n", c.pf.str, err);
	}
	if (o->vfs)
		printf("vfs=%u takes effect when the livepatch module geforce_sriov loads at the next boot: power the machine off and on%s\n", o->vfs,
		       live ? "" : ". Staging checks no card: run `geforce-sriov doctor` on the target host");
	display = gs_display_effective(live ? GS_LIVE_SYSFS : NULL, &cfg, display_why, sizeof(display_why));
	printf("%s\n", display_why);
	printf("Host CUDA: %s.\n", cfg.host_cuda == GS_SW_ON ? "enabled" : "disabled");
	if (o->skip_driver && feature_change)
		printf("--driver=skip: the config records the feature selection, but NVIDIA's files are not rebuilt for it\n");
	if (!live && cfg.display == GS_DISPLAY_AUTO)
		printf("staging: display=auto is judged on the target host; install there builds the display modules only\n"
		       "when a selected GPU is its boot display\n");
	if (d3cold_dropped)
		printf("--device without --disable-d3cold: the D3cold consent (d3cold=off) is removed from the config\n");

	const char *const subst[] = { "@BINDIR@", bindir, "@HOOK@", hook, "@VERSION@", GS_VERSION, NULL };

	if (live && o->driver_file) {
		r = gs_nvidia_files(x, o, &cfg, pkgdir, sizeof(pkgdir));
		if (r) {
			/* Kept so uninstall can remove what was written; a refusal that changed nothing writes nothing. */
			int ms = x->written || r != GS_EXIT_REFUSED ? gs_manifest_save(manifest, &x->m) : 0;

			if (ms)
				fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-ms));
			free(x);
			return r;
		}
		gs_pathf(pkgkernel, sizeof(pkgkernel), "%s/kernel", pkgdir);
	}
	{
#define TRY(expr, what) do { \
		if ((r = (expr))) { \
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", (what), strerror(-r)); \
			goto fail; \
		} \
	} while (0)
		gs_pathf(p, sizeof(p), "%s/geforce-sriov", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov", bindir);
		TRY(gs_install_copy(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/driver/geforce-sriov-patch", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov-patch", bindir);
		TRY(gs_install_copy(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/scripts/geforce-sriov-driver.sh", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov-driver", bindir);
		TRY(gs_install_copy(x, p, q, 0755), q);
		/*
		 * The driver step sources these libraries, reads the table of confirmed versions and
		 * applies these patches, all from beside itself.
		 */
		if ((r = copy_entries(x, src, "scripts", bindir, "gs-driver-", ".sh", 0755)))
			TRY(r, "scripts/gs-driver-*.sh");
		gs_pathf(p, sizeof(p), "%s/scripts/gs-driver-versions.tbl", src);
		gs_pathf(q, sizeof(q), "%s/gs-driver-versions.tbl", bindir);
		TRY(gs_install_copy(x, p, q, 0644), q);
		gs_pathf(q, sizeof(q), "%s/driver-patches", bindir);
		if ((r = copy_entries(x, src, "driver-patches", q, NULL, ".patch", 0644)))
			TRY(r, "driver-patches/");
		/* The owner of the Ada VFs' framebuffer slots, which the driver step builds into nvidia.ko. */
		{
			static const char *const owner[] = { "gs-fb-owner.c", "gs-fb-owner.h", "fb-owner.patch" };

			for (size_t k = 0; k < sizeof(owner) / sizeof(owner[0]); k++) {
				gs_pathf(p, sizeof(p), "%s/driver-patches/fb-owner/%s", src, owner[k]);
				gs_pathf(q, sizeof(q), "%s/driver-patches/fb-owner/%s", bindir, owner[k]);
				TRY(gs_install_copy(x, p, q, 0644), q);
			}
		}
		gs_pathf(p, sizeof(p), "%s/hook/libgeforce_sriov.so", src);
		TRY(gs_install_copy(x, p, hook, 0755), hook);
		gs_pathf(p, sizeof(p), "%s/config/config.example", src);
		gs_pathf(q, sizeof(q), "%s/config.example", sharedir);
		TRY(gs_install_copy(x, p, q, 0644), q);

		gs_pathf(p, sizeof(p), "%s/kernel", src);
		d = opendir(p);
		if (!d) {
			r = -errno;
			fprintf(stderr, "geforce-sriov: cannot read %s\n", p);
			goto fail;
		}
		while ((de = readdir(d))) {
			char sp[GS_PATH_MAX], tp[GS_PATH_MAX];
			struct stat st;

			if (!gs_is_kernel_source(de->d_name) ||
			    gs_pathf(sp, sizeof(sp), "%s/kernel/%s", src, de->d_name) ||
			    stat(sp, &st) || !S_ISREG(st.st_mode) ||
			    gs_pathf(tp, sizeof(tp), "%s/%s", DKMS_SRC, de->d_name))
				continue;
			if ((r = gs_install_copy(x, sp, tp, 0644))) {
				closedir(d);
				fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", tp, strerror(-r));
				goto fail;
			}
		}
		closedir(d);
		gs_pathf(p, sizeof(p), "%s/include/gs_devices.h", src);
		TRY(gs_install_copy(x, p, DKMS_SRC "/include/gs_devices.h", 0644), "gs_devices.h");

		gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-livepatch.service.in", src);
		TRY(put_template(x, p, UNIT_DIR "/" GS_UNIT_LIVEPATCH, subst), GS_UNIT_LIVEPATCH);
		gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-activate.service.in", src);
		TRY(put_template(x, p, UNIT_DIR "/" GS_UNIT_ACTIVATE, subst), GS_UNIT_ACTIVATE);
		/* Order a VM autostart unit this host has after activation; unrelated hypervisors are not touched. */
		for (size_t u = 0; gs_vm_autostart_units[u]; u++) {
			char target[GS_PATH_MAX];

			if (!vm_unit_present(destdir, gs_vm_autostart_units[u]))
				continue;
			gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-vm-autostart.conf.in", src);
			gs_pathf(target, sizeof(target), UNIT_DIR "/%s.d/" GS_VM_DROPIN_NAME, gs_vm_autostart_units[u]);
			TRY(put_template(x, p, target, subst), target);
		}
		TRY(make_dirs(x, GS_STATE_DIR), GS_STATE_DIR);
#undef TRY
		/* Staging has no driver step: the drop-in and the config follow the copies. */
		if (!live && (r = put_selection(x, src, subst, &cfg, config_changed, cfgfile)))
			goto fail;
		if (!live && (r = migrate_legacy(x, &legacy_kept)))
			goto fail;
	}
	if ((r = gs_manifest_save(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}

	if (!live) {
		free(x);
		printf("\nstaged below %s; nothing outside it changed. On the target host, as root:\n", destdir);
		print_live_steps(bindir, cfg.nbdfs > 0, o, display, cfg.host_cuda == GS_SW_ON);
		return GS_EXIT_OK;
	}

	/*
	 * NVIDIA's files next. The step refuses before it writes anything and rolls its own changes back
	 * when it fails, so a failure here leaves NVIDIA's module and plugin as they were. The vgpud
	 * drop-in and the config, which make nvidia-vgpud load the hook, are written only after it.
	 */
	if (!o->skip_driver) {
		char driver[GS_PATH_MAX];
		const char *source = pkgkernel[0] ? pkgkernel : o->nvidia_source ? o->nvidia_source : gs_nvidia_source_default();
		const char *argv[20];
		char why[1024];
		struct utsname u;
		unsigned int n = 0;
		int dr;
		char have_dir[GS_PATH_MAX], have_version[32];

		/*
		 * NVIDIA's CUDA libraries are asked of the driver step when host CUDA is on and they are not
		 * all in place, or when a second package is named, which may be another version.
		 */
		cuda_stage = cfg.host_cuda == GS_SW_ON &&
			     (o->companion_file || o->companion_source ||
			      gs_cuda_libs_state(manifest, have_dir, sizeof(have_dir), have_version, sizeof(have_version), why, sizeof(why)) != 1 ||
			      strcmp(have_dir, cudadir) != 0);

		gs_pathf(driver, sizeof(driver), "%s/geforce-sriov-driver", bindir);
		argv[n++] = driver;
		/* An input that is not given is the one an earlier install kept, so a rerun needs no path again. */
		argv[n++] = "--retained";
		argv[n++] = "--source";
		argv[n++] = source;
		argv[n++] = display ? "--display" : "--no-display";
		argv[n++] = cfg.host_cuda == GS_SW_ON ? "--host-cuda" : "--no-host-cuda";
		/* fb_slots=off: NVIDIA's module is built without the code that holds the Ada framebuffer slots. */
		if (cfg.fb_slots == GS_FB_SLOTS_OFF)
			argv[n++] = "--no-fb-owner";
		if (o->companion_file)
			argv[n++] = "--companion-file", argv[n++] = o->companion_file;
		if (o->companion_source)
			argv[n++] = "--companion-source", argv[n++] = o->companion_source;
		if (o->nvkms_source)
			argv[n++] = "--nvkms-source", argv[n++] = o->nvkms_source;
		if (cuda_stage)
			argv[n++] = "--cuda-stage", argv[n++] = GS_CUDA_STAGE;
		argv[n] = NULL;
		printf("\n== NVIDIA's files: %s --source %s", driver, source);
		if (display)
			printf(" --display");
		if (cfg.host_cuda == GS_SW_ON)
			printf(" --host-cuda");
		printf("\n");
		printf("This may build NVIDIA's module for %s, which can take a few minutes; progress is in %s\n",
		       uname(&u) == 0 ? u.release : "the running kernel", GS_DRIVER_LOG);
		fflush(stdout);
		dr = gs_driver_step(argv, GS_DRIVER_LOG, why, sizeof(why), &drv);
		if (dr != 0) {
			if (dr < 0)
				fprintf(stderr, "geforce-sriov: %s could not run", driver);
			else
				fprintf(stderr, "geforce-sriov: %s exited with %d", driver, dr);
			/* A refusal comes before the first change; a failure is rolled back. Either way nothing of NVIDIA's moved. */
			fprintf(stderr, ": NVIDIA's module and plugin are as they were%s.\n",
				o->driver_file ? "; what --driver-file extracted and installed stays" : "");
			/* The log's last lines, printed by gs_driver_step, already end with the cause. */
			fprintf(stderr, "geforce-sriov's own files are installed (uninstall removes them); %s and\n"
				"%s are not written.\n"
				"The lines above give the reason. Fix it, then run the same command again.\n",
				VGPUD_DROPIN, GS_CONFIG_PATH);
			if (display || cfg.host_cuda == GS_SW_ON)
				fprintf(stderr, "The display modules need --companion-file (or --companion-source) and --nvkms-source, and host CUDA\n"
					"needs --companion-file (or --companion-source). --no-display and --no-host-cuda leave those modules out.\n");
			else
				fprintf(stderr, "To install geforce-sriov without building NVIDIA's module, use --driver=skip\n"
					"(manual path: docs/driver-patch.md).\n");
			r = x->written || dr != GS_EXIT_REFUSED ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
			free(x);
			return r;
		}
		/*
		 * Unless the step found its build already installed, it replaced NVIDIA's module or plugin: a
		 * later refusal no longer changed nothing.
		 */
		if (drv == GS_DRIVER_CHANGED)
			x->written = 1;
	}
	/*
	 * Host CUDA: NVIDIA's CUDA libraries, which the driver step took from the second package, or
	 * their removal. They change nothing that a boot picks up, so they do not count as a change
	 * that asks for a power cycle; they are in the manifest all the same.
	 */
	{
		int written = x->written;

		cuda_written = gs_cuda_libs_sync(x, cudadir, GS_CUDA_CONF, GS_CUDA_STAGE, cfg.host_cuda == GS_SW_ON,
						 live ? &cuda_io : NULL);
		x->written = written;
	}
	if ((r = put_selection(x, src, subst, &cfg, config_changed, cfgfile)))
		goto fail;
	/* After the new units and config are in place: the old preparation flow must not run again. */
	if ((r = migrate_legacy(x, &legacy_kept)))
		goto fail;
	if ((r = gs_manifest_save(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}
	/* No file was installed, replaced or removed, and the driver step built nothing. */
	changed_files = x->written;
	free(x);

	{
		const char *reload_argv[] = { "systemctl", "daemon-reload", NULL };
		const char *enable_argv[] = { "systemctl", "enable", GS_UNIT_LIVEPATCH, NULL };
		const char *disable_old[] = { "systemctl", "disable", GS_LEGACY_UNIT_PREPARE, NULL };
		const char *const status_argv[] = { "dkms", "status", "-m", GS_DKMS_NAME, NULL };
		char status[4096] = "";

		/* Another version registered (an upgrade) is replaced, not left to build next to this one. */
		if (gs_exists("/var/lib/dkms/" GS_DKMS_NAME) && gs_run_capture(status_argv, status, sizeof(status)) != 0) {
			fprintf(stderr, "geforce-sriov: `dkms status -m %s` failed; another version of %s registered in DKMS,\n"
				"if any, stays there (`dkms status` lists it, `dkms remove %s/VERSION --all` removes it)\n",
				GS_DKMS_NAME, GS_DKMS_NAME, GS_DKMS_NAME);
			status[0] = '\0';
		}
		if ((r = gs_dkms_step(gs_run, gs_exists("/var/lib/dkms/" GS_DKMS_NAME "/" GS_VERSION), status, "/tmp", "",
				      manifest)))
			return r;
		/* That step built for the running kernel alone; the other kernels with a build of NVIDIA's module follow. */
		gs_dkms_other_kernels(gs_run, "", GS_STATE_DIR);
		/*
		 * The old preparation unit must not be started again. Its unchanged file and its enablement
		 * link were removed above, so there is something to disable only when a changed file was kept.
		 * On a host that never had the unit, systemctl would only report that it does not exist.
		 */
		if (legacy_kept && gs_run(disable_old) != 0)
			fprintf(stderr, "geforce-sriov: systemctl disable %s failed; it stays installed, so disable it\n"
				"yourself before the next boot: systemctl disable %s\n", GS_LEGACY_UNIT_PREPARE,
				GS_LEGACY_UNIT_PREPARE);
		if (gs_run(reload_argv) != 0) {
			fprintf(stderr, "geforce-sriov: systemctl daemon-reload failed\n");
			return GS_EXIT_FAIL;
		}
		if (cfg.nbdfs) {
			const char *enable_act[] = { "systemctl", "enable", GS_UNIT_ACTIVATE, NULL };
			const char *enabled_lp[] = { "systemctl", "is-enabled", "--quiet", GS_UNIT_LIVEPATCH, NULL };
			const char *enabled_act[] = { "systemctl", "is-enabled", "--quiet", GS_UNIT_ACTIVATE, NULL };
			/* Asked before the units are enabled: enabling them is a change the next boot picks up. */
			int were_enabled = gs_run(enabled_lp) == 0 && gs_run(enabled_act) == 0;

			if (gs_run(enable_argv) != 0) {
				fprintf(stderr, "geforce-sriov: systemctl enable %s failed\n", GS_UNIT_LIVEPATCH);
				return GS_EXIT_FAIL;
			}
			if (gs_run(enable_act) != 0) {
				fprintf(stderr, "geforce-sriov: systemctl enable %s failed\n", GS_UNIT_ACTIVATE);
				return GS_EXIT_FAIL;
			}
			if (!changed_files && were_enabled && cuda_written)
				printf("\nOnly NVIDIA's CUDA libraries for host CUDA changed. This run needs no power cycle; one that an earlier\n"
				       "install asked for still applies.\n");
			else if (!changed_files && were_enabled)
				install_unchanged(drv);
			else
				gs_install_next(bindir, &cfg, o->from_setup);
		} else {
			const char *disable_lp[] = { "systemctl", "disable", GS_UNIT_LIVEPATCH, NULL };
			const char *disable_act[] = { "systemctl", "disable", GS_UNIT_ACTIVATE, NULL };
			int dp = gs_run(disable_lp), da = gs_run(disable_act);

			if (dp || da) {
				fprintf(stderr, "geforce-sriov: no GPU is selected, but `systemctl disable` failed, so %s or %s may\n"
					"still be enabled from an earlier install. Run `systemctl disable %s %s`.\n",
					GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE, GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE);
				return GS_EXIT_FAIL;
			}
			printf("\nNo GPU selected: %s and %s are installed but not enabled. Select GPUs with\n"
			       "`geforce-sriov install --device BDF`, one per GPU; it also sets identity=on and\n"
			       "identity_force_host=on and enables the units.\n",
			       GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE);
		}
	}
	return GS_EXIT_OK;
fail:
	x->written |= cuda_written;
	/* A refusal before anything was written changes nothing, the manifest included. */
	if (!x->written && r == -EEXIST) {
		free(x);
		return GS_EXIT_REFUSED;
	}
	/* Keep a manifest of what was written so uninstall can remove it; its directory may not be made yet. */
	if ((r = make_dirs(x, GS_STATE_DIR)) || (r = gs_manifest_save(manifest, &x->m)))
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
	free(x);
	return GS_EXIT_FAIL;
}

int gs_cmd_rebuild(const char *release)
{
	/* The kernel the user named: the advice after a refusal repeats it, or the next run would be for another kernel. */
	const char *named = release;
	char bindir[GS_PATH_MAX], driver[GS_PATH_MAX], err[512], why[1024], display_why[256];
	enum gs_driver_result drv = GS_DRIVER_CHANGED;
	const char *argv[8];
	struct gs_config cfg;
	struct utsname u;
	unsigned int n = 0;
	int r, display, dr;

	if (geteuid() != 0) {
		fprintf(stderr, "geforce-sriov: rebuild must run as root\n");
		return GS_EXIT_REFUSED;
	}
	if (self_dir(bindir, sizeof(bindir)) || gs_pathf(driver, sizeof(driver), "%s/geforce-sriov-driver", bindir) ||
	    !gs_exists(driver)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: rebuild runs from the installed copy, which has\n"
			"geforce-sriov-driver beside it. In the source folder, run `sudo ./geforce-sriov install`.\n");
		return GS_EXIT_REFUSED;
	}
	if ((r = gs_lock()))
		return r;
	if (gs_config_load(gs_config_path(), &cfg, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: the config is invalid, so the features to build are not\n"
			"known: %s\n", err);
		return GS_EXIT_REFUSED;
	}
	if (!release && uname(&u) == 0)
		release = u.release;
	if (!release) {
		fprintf(stderr, "geforce-sriov: cannot read the release of the running kernel\n");
		return GS_EXIT_FAIL;
	}
	/* First the livepatch module for that kernel: without it NVIDIA's module there is of no use. */
	if ((r = gs_dkms_ensure(gs_run, gs_run_capture,
				gs_find_program("dkms", why, sizeof(why)) == 0 && gs_exists("/var/lib/dkms/" GS_DKMS_NAME "/" GS_VERSION),
				release, named)))
		return r;
	display = gs_display_effective(GS_LIVE_SYSFS, &cfg, display_why, sizeof(display_why));
	printf("%s\n", display_why);
	printf("Host CUDA: %s.\n", cfg.host_cuda == GS_SW_ON ? "enabled" : "disabled");
	argv[n++] = driver;
	argv[n++] = "--retained";
	argv[n++] = "--kernel";
	argv[n++] = release;
	argv[n++] = display ? "--display" : "--no-display";
	argv[n++] = cfg.host_cuda == GS_SW_ON ? "--host-cuda" : "--no-host-cuda";
	if (cfg.fb_slots == GS_FB_SLOTS_OFF)
		argv[n++] = "--no-fb-owner";
	argv[n] = NULL;
	printf("\n== NVIDIA's files: %s --retained --kernel %s%s%s%s\n", driver, release, display ? " --display" : "",
	       cfg.host_cuda == GS_SW_ON ? " --host-cuda" : "", cfg.fb_slots == GS_FB_SLOTS_OFF ? " --no-fb-owner" : "");
	printf("This may build NVIDIA's module for %s, which can take a few minutes; progress is in %s\n", release,
	       GS_DRIVER_LOG);
	fflush(stdout);
	dr = gs_driver_step(argv, GS_DRIVER_LOG, why, sizeof(why), &drv);
	if (dr != 0) {
		if (dr < 0)
			fprintf(stderr, "geforce-sriov: %s could not run", driver);
		else
			fprintf(stderr, "geforce-sriov: %s exited with %d", driver, dr);
		/* The log's last lines, printed by gs_driver_step, already end with the cause and the next step. */
		fprintf(stderr, ": NVIDIA's module and plugin for %s are as they were.\n"
			"The lines above give the reason. Fix it, then run `sudo geforce-sriov rebuild%s%s` again.\n", release,
			named ? " --kernel " : "", named ? named : "");
		return dr == GS_EXIT_REFUSED ? GS_EXIT_REFUSED : GS_EXIT_FAIL;
	}
	if (drv == GS_DRIVER_RECORD_ONLY) {
		printf("\nOnly the build record changed: it now holds the current version classification of the installed build.\n");
	} else if (drv == GS_DRIVER_UNCHANGED) {
		printf("\nNothing changed: NVIDIA's module and plugin for %s already are the build geforce-sriov makes.\n", release);
	} else {
		printf("\nNVIDIA's module and plugin for %s are now the build geforce-sriov makes.\n"
		       "Next: power the machine off and on. A restart is not enough: the card must come up with the new\n"
		       "module, and the early livepatch unit must load before udev.\n", release);
	}
	return GS_EXIT_OK;
}
