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

#define DKMS_SRC "/usr/src/" GS_DKMS_NAME "-" GS_VERSION
#define DKMS_ID GS_DKMS_NAME "/" GS_VERSION
#define UNIT_DIR GS_UNIT_DIR
#define VGPUD_DROPIN_DIR UNIT_DIR "/nvidia-vgpud.service.d"
#define VGPUD_DROPIN VGPUD_DROPIN_DIR "/" GS_VGPUD_DROPIN_NAME
#define MAX_ENTRIES 128

struct entry {
	char type;            /* 'f' file, 'd' directory created by install */
	char sha[65];
	char path[GS_PATH_MAX];
};

struct manifest {
	struct entry e[MAX_ENTRIES];
	unsigned int n;
};

static struct entry *manifest_find(struct manifest *m, char type, const char *path)
{
	for (unsigned int i = 0; i < m->n; i++)
		if (m->e[i].type == type && strcmp(m->e[i].path, path) == 0)
			return &m->e[i];
	return NULL;
}

static int manifest_add(struct manifest *m, char type, const char *sha, const char *path)
{
	struct entry *e = manifest_find(m, type, path);

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

/* Lines "f <sha256> <path>" and "d - <path>". A missing file is an empty manifest. */
static int manifest_load(const char *file, struct manifest *m)
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

		if (sscanf(line, "%c %64s %4095s", &type, sha, path) != 3 || (type != 'f' && type != 'd') ||
		    gs_validate_abs_path(path) || manifest_add(m, type, sha, path)) {
			free(data);
			return -EINVAL;
		}
	}
	free(data);
	return 0;
}

static int manifest_save(const char *file, const struct manifest *m)
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

/*
 * Does install still own the file at path, recorded with hash want? Only while it is the regular file
 * with that hash: gs_sha256_file never follows a link, so a link put in its place is not owned.
 */
enum owned { OWN_ABSENT, OWN_UNREADABLE, OWN_CHANGED, OWN_YES };
static enum owned manifest_owns(const char *path, const char *want)
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

struct ctx {
	const char *destdir; /* "" for live */
	struct manifest m;
	int written;         /* this run created a file or directory */
};

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
	if (!gs_exists(full) || manifest_find(&x->m, 'f', target))
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

static int copy_file(struct ctx *x, const char *src, const char *target, mode_t mode)
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

/*
 * The nvidia-vgpud drop-in, which preloads the hook, the config and, with --hold-nvidia, the hold:
 * written last, once NVIDIA's files are prepared. held: NULL without --hold-nvidia, "" to write
 * GS_NVIDIA_HOLD, else the files that hold nvidia back already, named instead. 0 or -errno, with the
 * reason printed.
 */
static int put_selection(struct ctx *x, const char *src, const char *const *subst, const struct gs_config *cfg,
			 int config_changed, const char *cfgfile, const char *held)
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
	if (!held)
		return 0;
	if (held[0]) {
		printf("%s already %s nvidia back at boot\n", held, strstr(held, " and ") ? "hold" : "holds");
		return 0;
	}
	r = put(x, GS_NVIDIA_HOLD, GS_NVIDIA_HOLD_TEXT, sizeof(GS_NVIDIA_HOLD_TEXT) - 1, 0644);
	if (r)
		fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", GS_NVIDIA_HOLD, strerror(-r));
	return r;
}

/*
 * After the config, when it no longer grants a console release that it granted before: the hold file an
 * earlier --hold-nvidia wrote is removed while unchanged and leaves the manifest; any hold still in
 * modprobe_dir (changed since install, or the user's own) is named, since nothing else says so now. 0, or
 * -errno with the reason printed when the removal failed.
 */
static int drop_hold(struct ctx *x, const char *modprobe_dir)
{
	struct entry *e = manifest_find(&x->m, 'f', GS_NVIDIA_HOLD);
	char full[GS_PATH_MAX], sha[65], files[2 * GS_PATH_MAX + 8];

	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, GS_NVIDIA_HOLD))
		return -ENAMETOOLONG;
	if (e && gs_sha256_file(full, sha) == 0 && strcmp(sha, e->sha) == 0) {
		if (unlink(full) < 0 && errno != ENOENT) {
			int err = errno;

			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", full, strerror(err));
			return -err;
		}
		x->written = 1;
		memmove(e, e + 1, (size_t)(&x->m.e[x->m.n] - (e + 1)) * sizeof(*e));
		x->m.n--;
		printf("removed %s: the console release is no longer granted, so nvidia loads at boot again\n", full);
	}
	if (!gs_nvidia_hold(modprobe_dir, files, sizeof(files)))
		printf("%s still holds nvidia back at boot; remove it if nothing else needs it\n", files);
	return 0;
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

/* ---- prerequisites and NVIDIA's package --------------------------------- */

int gs_dep_present(enum gs_dep dep, const char *release)
{
	char p[GS_PATH_MAX];

	switch (dep) {
	case GS_DEP_HEADERS:
		return release && release[0] && !gs_pathf(p, sizeof(p), "/lib/modules/%s/build/Makefile", release) &&
		       gs_exists(p);
	case GS_DEP_DKMS:
		return gs_find_program("dkms", p, sizeof(p)) == 0;
	case GS_DEP_PCIUTILS:
		return gs_find_program("lspci", p, sizeof(p)) == 0 && gs_find_program("setpci", p, sizeof(p)) == 0;
	default:
		return gs_find_program("cc", p, sizeof(p)) == 0 && gs_find_program("make", p, sizeof(p)) == 0;
	}
}

/* Live install, before anything is written: a missing prerequisite refuses. */
static int deps_step(void)
{
	char rel[128] = "";

	gs_read_attr(GS_LIVE_PROC "/sys/kernel/osrelease", rel, sizeof(rel));
	for (unsigned int d = 0; d < GS_DEP_COUNT; d++) {
		if (gs_dep_present((enum gs_dep)d, rel))
			continue;
		fprintf(stderr, "geforce-sriov: refused, nothing changed: a prerequisite is missing; `geforce-sriov doctor`\n"
			"names it. " GS_PREREQ_MSG "\n");
		return GS_EXIT_REFUSED;
	}
	return GS_EXIT_OK;
}

int gs_nvidia_package_version(const char *name, char *out, size_t len)
{
	const char *base = strrchr(name, '/'), *s;

	for (s = base ? base + 1 : name; *s; s++) {
		const char *p = s + 1;
		unsigned int parts = 0;
		size_t n;

		if (*s != '-')
			continue;
		for (;;) {
			const char *digits = p;

			while (*p >= '0' && *p <= '9')
				p++;
			if (p == digits)
				break;
			parts++;
			if (*p != '.' || !(p[1] >= '0' && p[1] <= '9'))
				break;
			p++;
		}
		if (parts < 2 || !(*p == '-' || *p == '.' || *p == '\0'))
			continue;
		n = (size_t)(p - (s + 1));
		if (n >= len)
			return -1;
		memcpy(out, s + 1, n);
		out[n] = '\0';
		return 0;
	}
	return -1;
}

#define PKG_MARKER ".geforce-sriov-sha256"

/* NVIDIA's vGPU userspace of version ver below root: libnvidia-vgpu.so.<ver> where the driver step looks for it. */
static int userspace_installed(const char *root, const char *ver)
{
	static const char *const dirs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" };
	char p[GS_PATH_MAX];
	struct stat st;

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s%s/libnvidia-vgpu.so.%s", root, dirs[i], ver) && lstat(p, &st) == 0 &&
		    S_ISREG(st.st_mode))
			return 1;
	return 0;
}

static const char *const lib_dirs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" };

/*
 * argv[0] from root's system directories (live: ""), run with a fixed environment: its exit status,
 * or -1 when it is not there or cannot run. out receives the first len - 1 bytes it printed.
 */
static int root_capture(const char *root, const char *const argv[], char *out, size_t len)
{
	static const char *const dirs[] = { "/usr/sbin", "/usr/bin", "/sbin", "/bin" };
	static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL };
	char prog[GS_PATH_MAX], rest[512];
	size_t off = 0, i;
	int fd[2], status;
	ssize_t n;
	pid_t pid;

	out[0] = '\0';
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!gs_pathf(prog, sizeof(prog), "%s%s/%s", root, dirs[i], argv[0]) && access(prog, X_OK) == 0)
			break;
	if (i == sizeof(dirs) / sizeof(dirs[0]) || pipe(fd))
		return -1;
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);

		dup2(fd[1], 1);
		if (null >= 0)
			dup2(null, 2);
		close(fd[0]);
		close(fd[1]);
		execve(prog, (char *const *)argv, envp);
		_exit(127);
	}
	close(fd[1]);
	/* Read to the end, so a long listing never blocks the child; what does not fit is dropped. */
	for (;;) {
		if (off + 1 < len)
			n = read(fd[0], out + off, len - 1 - off);
		else
			n = read(fd[0], rest, sizeof(rest));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (off + 1 < len)
			off += (size_t)n;
	}
	close(fd[0]);
	out[off] = '\0';
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Any libnvidia-vgpu.so* in root's library directories. */
static int vgpu_library_present(const char *root)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	int found = 0;
	DIR *d;

	for (size_t i = 0; !found && i < sizeof(lib_dirs) / sizeof(lib_dirs[0]); i++) {
		if (gs_pathf(p, sizeof(p), "%s%s", root, lib_dirs[i]) || !(d = opendir(p)))
			continue;
		while (!found && (de = readdir(d)))
			found = strncmp(de->d_name, "libnvidia-vgpu.so", strlen("libnvidia-vgpu.so")) == 0;
		closedir(d);
	}
	return found;
}

/* Appends name to the ", "-separated list; *count counts every name, listed or not. */
static void name_add(char *list, size_t len, const char *name, unsigned int *count)
{
	size_t n = strlen(list);

	if (*count < 3)
		snprintf(list + n, len - n, "%s%s", n ? ", " : "", name);
	(*count)++;
}

/*
 * Another NVIDIA driver on root, which NVIDIA's vGPU installer would collide with: the nvidia module
 * answers `modinfo -F version nvidia` while no libnvidia-vgpu.so* exists, or while distribution
 * packages own the driver. GS_EXIT_REFUSED with what was found and the command that removes it.
 */
static int foreign_driver(const char *root)
{
	const char *const modinfo[] = { "modinfo", "-F", "version", "nvidia", NULL };
	const char *const dpkg[] = { "dpkg-query", "-W", "-f=${Package} ${Status}\\n", "nvidia*", NULL };
	const char *const rpm[] = { "rpm", "-qa", "nvidia*", NULL };
	static const char owned[] = " ok installed";
	char ver[64], out[8192], pkgs[256] = "", why[512], *line, *save = NULL;
	const char *remove = NULL;
	unsigned int npkgs = 0;
	int lib;

	if (root_capture(root, modinfo, ver, sizeof(ver)) != 0)
		return 0;
	ver[strcspn(ver, "\n")] = '\0';
	if (!ver[0])
		return 0;
	/* dpkg-query exits 1 when nothing matches; its lines are "<package> <want> ok <state>". */
	if (root_capture(root, dpkg, out, sizeof(out)) >= 0)
		for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			size_t n = strlen(line);
			char *sp = strchr(line, ' ');

			if (!sp || n < sizeof(owned) - 1 || strcmp(line + n - (sizeof(owned) - 1), owned) != 0)
				continue;
			*sp = '\0';
			name_add(pkgs, sizeof(pkgs), line, &npkgs);
			remove = "apt-get remove --purge 'nvidia-*'";
		}
	if (!remove && root_capture(root, rpm, out, sizeof(out)) == 0)
		for (save = NULL, line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			name_add(pkgs, sizeof(pkgs), line, &npkgs);
			remove = "dnf remove 'nvidia*'";
		}
	lib = vgpu_library_present(root);
	if (lib && !remove)
		return 0;
	why[0] = '\0';
	if (!lib)
		snprintf(why, sizeof(why), "no libnvidia-vgpu.so* is in %s, %s or %s", lib_dirs[0], lib_dirs[1], lib_dirs[2]);
	if (remove) {
		size_t n = strlen(why);
		char more[32] = "";

		if (npkgs > 3)
			snprintf(more, sizeof(more), " and %u more", npkgs - 3);
		snprintf(why + n, sizeof(why) - n, "%sdistribution packages own it (%s%s)", n ? "; " : "", pkgs, more);
	}
	fprintf(stderr, "geforce-sriov: refused, nothing changed: NVIDIA's driver %s is installed (modinfo -F version nvidia)\n"
		"and is not the vGPU host driver: %s. NVIDIA's vGPU installer would overwrite its module and\n"
		"libraries%s.\n"
		"Remove it with `%s`%s, then rerun.\n",
		ver, why, remove ? ", and the package manager would put them back" : "",
		remove ? remove : "nvidia-uninstall",
		remove ? "" : " if NVIDIA's .run installer put it there, or with the package manager that installed it");
	return GS_EXIT_REFUSED;
}

/* When NVIDIA's installer is still needed for the package file, no other NVIDIA driver may be on root. */
static int package_conflict(const char *file, const char *root, int accept_license)
{
	char ver[80];
	int have_ver = gs_nvidia_package_version(file, ver, sizeof(ver)) == 0;

	if (userspace_installed(root, have_ver ? ver : GS_NVIDIA_VERSION) && (have_ver || !accept_license))
		return 0;
	return foreign_driver(root);
}

int gs_nvidia_package(const char *file, const char *base, const char *root, int accept_license, char *dir, size_t len,
		      int *changed)
{
	char real[PATH_MAX], sha[65], tag[80], ver[80], tmp[GS_PATH_MAX], p[GS_PATH_MAX], line[80];
	struct stat st;
	int r, have_ver, installed, extracted = 0;

	*changed = 0;
	if (!realpath(file, real) || stat(real, &st) || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: --driver-file %s is not a regular file\n", file);
		return GS_EXIT_REFUSED;
	}
	if ((r = gs_sha256_file(real, sha))) {
		fprintf(stderr, "geforce-sriov: cannot read %s: %s\n", real, strerror(-r));
		return GS_EXIT_FAIL;
	}
	have_ver = gs_nvidia_package_version(real, ver, sizeof(ver)) == 0;
	if (have_ver)
		gs_copy(tag, sizeof(tag), ver);
	else
		snprintf(tag, sizeof(tag), "%.16s", sha);
	if (gs_pathf(dir, len, "%s/%s", base, tag) || gs_pathf(tmp, sizeof(tmp), "%s.partial", dir)) {
		fprintf(stderr, "geforce-sriov: %s/%s: path too long\n", base, tag);
		return GS_EXIT_FAIL;
	}
	printf("\n== NVIDIA's package %s (SHA-256 %s)\n", real, sha);
	/* Before anything is written. */
	if ((r = package_conflict(real, root, accept_license)))
		return r;
	if (gs_exists(dir)) {
		char *have = NULL;
		size_t hlen;
		int same = !gs_pathf(p, sizeof(p), "%s/" PKG_MARKER, dir) && gs_read_whole(p, &have, &hlen, 128) == 0 &&
			   strncmp(have, sha, 64) == 0;

		free(have);
		if (!same) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s holds another extraction; remove it, then rerun\n",
				dir);
			return GS_EXIT_REFUSED;
		}
		printf("reusing %s, extracted from this file earlier\n", dir);
	} else {
		const char *rm_argv[] = { "rm", "-rf", "--", tmp, NULL };
		const char *ex_argv[] = { "sh", real, "--extract-only", "--target", tmp, NULL };

		if ((r = gs_mkdir_p(base, 0755))) {
			fprintf(stderr, "geforce-sriov: cannot create %s: %s\n", base, strerror(-r));
			return GS_EXIT_FAIL;
		}
		if (gs_exists(tmp) && gs_run(rm_argv) != 0) {
			fprintf(stderr, "geforce-sriov: cannot remove the unfinished extraction %s\n", tmp);
			return GS_EXIT_FAIL;
		}
		printf("sh %s --extract-only --target %s\n", real, tmp);
		fflush(stdout);
		r = gs_run(ex_argv);
		if (r != 0) {
			fprintf(stderr, "geforce-sriov: extracting %s failed (exit %d); give NVIDIA's vGPU host .run package\n",
				real, r);
			return GS_EXIT_FAIL;
		}
		if (gs_pathf(p, sizeof(p), "%s/kernel", tmp) || stat(p, &st) || !S_ISDIR(st.st_mode)) {
			fprintf(stderr, "geforce-sriov: refused: %s has no kernel/ directory, so it is not NVIDIA's Linux driver package\n",
				real);
			return GS_EXIT_FAIL; /* the extraction below base was written */
		}
		snprintf(line, sizeof(line), "%s\n", sha);
		if (gs_pathf(p, sizeof(p), "%s/" PKG_MARKER, tmp) ||
		    (r = gs_write_file_atomic(p, line, strlen(line), 0644)) || rename(tmp, dir)) {
			fprintf(stderr, "geforce-sriov: cannot finish the extraction in %s\n", dir);
			return GS_EXIT_FAIL;
		}
		printf("extracted to %s\n", dir);
		extracted = *changed = 1;
	}
	/* The driver step needs the userspace of the version it patches; an unnamed package is taken to be that one. */
	installed = userspace_installed(root, have_ver ? ver : GS_NVIDIA_VERSION);
	if (installed && (have_ver || !accept_license)) {
		printf("NVIDIA's vGPU userspace %s is already installed; NVIDIA's installer not run\n",
		       have_ver ? ver : GS_NVIDIA_VERSION);
		return GS_EXIT_OK;
	}
	if (!accept_license) {
		fprintf(stderr, "geforce-sriov: refused: extracted only. The driver step needs NVIDIA's vGPU userspace\n"
			"(nvidia-vgpud, nvidia-vgpu-mgr, libnvidia-vgpu.so and sriov-manage), which is not installed.\n"
			"Run NVIDIA's installer yourself (`sh %s --dkms --skip-module-load`), or rerun with\n"
			"--accept-nvidia-license to have install run it. Nothing else changed.\n", real);
		return extracted ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
	}
	if (!gs_pathf(p, sizeof(p), "%s/sys/module/nouveau", root) && gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: refused: nouveau is loaded, and NVIDIA's installer cannot run while it is.\n"
			"Put `blacklist nouveau` and `options nouveau modeset=0` in /etc/modprobe.d/blacklist-nouveau.conf,\n"
			"rebuild the initramfs (`update-initramfs -u`, or `dracut -f`), power the machine off and on, then rerun.\n"
			"Nothing else changed.\n");
		return extracted ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
	}
	{
		/* --skip-module-load: nothing may load nvidia before the boot unit prepares the cards.
		 * --kernel-module-type=proprietary: install proprietary driver by default*/
		const char *inst_argv[] = { "sh", real, "--silent", "--dkms", "--skip-module-load", "--kernel-module-type=proprietary", NULL };

		gs_pathf(p, sizeof(p), "%s/LICENSE", dir);
		printf("NVIDIA's licence: --accept-nvidia-license accepts the licence agreement of this package for you\n"
		       "(%s). NVIDIA's installer runs with --silent and asks nothing.\n"
		       "sh %s --silent --dkms --skip-module-load\n",
		       gs_exists(p) ? p : "NVIDIA's installer shows it when run without --silent", real);
		fflush(stdout);
		*changed = 1;
		r = gs_run(inst_argv);
		if (r != 0) {
			fprintf(stderr, "geforce-sriov: NVIDIA's installer exited with %d; see /var/log/nvidia-installer.log.\n"
				"geforce-sriov's own files are not installed\n", r);
			return GS_EXIT_FAIL;
		}
	}
	return GS_EXIT_OK;
}

/*
 * --driver-file on a live install: NVIDIA's package extracted (and installed, with the licence
 * accepted), then its vGPU daemon units and sriov-manage put in place where systemd and the config
 * do not find them yet. dir receives the extracted root; x->written is set once anything was written.
 */
static int nvidia_files(struct ctx *x, const struct gs_install_opts *o, const struct gs_config *cfg, char *dir,
			size_t len)
{
	static const char *const units[] = { "nvidia-vgpud.service", "nvidia-vgpu-mgr.service" };
	static const char *const unit_dirs[] = { "systemd", "init-scripts/systemd" };
	char src[GS_PATH_MAX], dst[GS_PATH_MAX];
	int changed, copied = 0;
	int r = gs_nvidia_package(o->driver_file, GS_NVIDIA_PKG_DIR, "", o->accept_nvidia_license, dir, len, &changed);

	x->written |= changed;
	if (r)
		return r;
	for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
		size_t k;

		if (gs_unit_file_present(units[u]))
			continue;
		for (k = 0; k < sizeof(unit_dirs) / sizeof(unit_dirs[0]); k++)
			if (!gs_pathf(src, sizeof(src), "%s/%s/%s", dir, unit_dirs[k], units[u]) && gs_exists(src))
				break;
		if (k == sizeof(unit_dirs) / sizeof(unit_dirs[0])) {
			printf("warning: %s is not installed and not in %s/systemd/; `activate` needs it\n", units[u], dir);
			continue;
		}
		gs_pathf(dst, sizeof(dst), "%s/%s", UNIT_DIR, units[u]);
		if ((r = copy_file(x, src, dst, 0644))) {
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", dst, strerror(-r));
			return GS_EXIT_FAIL;
		}
		copied = 1;
	}
	if (copied) {
		const char *reload_argv[] = { "systemctl", "daemon-reload", NULL };

		if (gs_run(reload_argv) != 0) {
			fprintf(stderr, "geforce-sriov: systemctl daemon-reload failed\n");
			return GS_EXIT_FAIL;
		}
	}
	if (!gs_exists(cfg->sriov_manage) && !gs_exists(GS_SRIOV_MANAGE_DEFAULT) &&
	    !gs_pathf(src, sizeof(src), "%s/sriov-manage", dir) && gs_exists(src) &&
	    (r = copy_file(x, src, GS_SRIOV_MANAGE_DEFAULT, 0755))) {
		fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", GS_SRIOV_MANAGE_DEFAULT, strerror(-r));
		return GS_EXIT_FAIL;
	}
	return GS_EXIT_OK;
}

static const char *const kernel_required[] = { "geforce_sriov.c", "Kbuild", "Makefile", "dkms.conf", NULL };

static int is_kernel_source(const char *name)
{
	size_t n = strlen(name);

	if (name[0] == '.')
		return 0;
	if (strcmp(name, "Kbuild") == 0 || strcmp(name, "Makefile") == 0 || strcmp(name, "dkms.conf") == 0)
		return 1;
	if (strstr(name, ".mod."))
		return 0;
	return n > 2 && name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'h');
}

#define DRIVER_TAG "geforce-sriov-driver: "
#define DRIVER_LOG_TAIL 40

/* One line of the driver step's output: a refused or failed line's reason into why; filter: its own lines to the screen. */
static void driver_line(const char *line, int filter, char *why, size_t len)
{
	static const char *const tags[] = { DRIVER_TAG "refused: ", DRIVER_TAG "failed: " };

	for (size_t k = 0; k < sizeof(tags) / sizeof(tags[0]); k++)
		if (strncmp(line, tags[k], strlen(tags[k])) == 0)
			snprintf(why, len, "%s", line + strlen(tags[k]));
	if (filter && strncmp(line, DRIVER_TAG, strlen(DRIVER_TAG)) == 0) {
		printf("%s\n", line);
		fflush(stdout);
	}
}

/* The last DRIVER_LOG_TAIL lines of the log open at fd, to standard error. */
static void log_tail(int fd)
{
	char buf[65536];
	off_t size = lseek(fd, 0, SEEK_END);
	unsigned int lines = 0;
	size_t start;
	ssize_t n;

	if (size <= 0)
		return;
	n = pread(fd, buf, sizeof(buf), size > (off_t)sizeof(buf) ? size - (off_t)sizeof(buf) : 0);
	if (n <= 0)
		return;
	/* Back from the end, past its own newline, to the start of the DRIVER_LOG_TAIL-th line. */
	for (start = (size_t)n - (buf[n - 1] == '\n'); start > 0; start--)
		if (buf[start - 1] == '\n' && ++lines == DRIVER_LOG_TAIL)
			break;
	fwrite(buf + start, 1, (size_t)n - start, stderr);
	if (buf[n - 1] != '\n')
		fputc('\n', stderr);
}

/* All of data to fd: 0, or -1 with errno set. */
static int write_all(int fd, const char *data, size_t len)
{
	while (len) {
		ssize_t n = write(fd, data, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

int gs_driver_step(const char *const argv[], const char *log, char *why, size_t len)
{
	static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL };
	char buf[4096], line[4096];
	int fd[2], status, rc, logfd = -1, log_ok = 1;
	size_t fill = 0;
	ssize_t n;
	pid_t pid;

	why[0] = '\0';
	if (log && ((logfd = open(log, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600)) < 0 ||
		    fchmod(logfd, 0600))) {
		fprintf(stderr, "geforce-sriov: cannot write %s: %s\n", log, strerror(errno));
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	if (pipe(fd)) {
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	if (pid == 0) {
		/* Without a log, standard output passes through and standard error is read for the reason. */
		if (logfd >= 0)
			dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);
		execve(argv[0], (char *const *)argv, envp);
		_exit(127);
	}
	close(fd[1]);
	for (;;) {
		n = read(fd[0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (logfd < 0)
			fwrite(buf, 1, (size_t)n, stderr);
		else if (log_ok && write_all(logfd, buf, (size_t)n)) {
			fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", log, strerror(errno));
			log_ok = 0;
		}
		/* Lines longer than line keep their start, which is all the tags need. */
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != '\n') {
				if (fill < sizeof(line) - 1)
					line[fill++] = buf[i];
				continue;
			}
			line[fill] = '\0';
			driver_line(line, logfd >= 0, why, len);
			fill = 0;
		}
	}
	close(fd[0]);
	if (fill) {
		line[fill] = '\0';
		driver_line(line, logfd >= 0, why, len);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR) {
			if (logfd >= 0)
				close(logfd);
			return -1;
		}
	rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	if (rc && logfd >= 0) {
		fprintf(stderr, "Last %d lines of the driver step (full output: %s):\n", DRIVER_LOG_TAIL, log);
		log_tail(logfd);
	}
	if (logfd >= 0)
		close(logfd);
	return rc;
}

static void print_live_steps(const char *bindir, int enable_unit, const struct gs_install_opts *o)
{
	if (enable_unit)
		printf("  %s/geforce-sriov doctor   (checks the selected GPUs on this host; staging did not)\n", bindir);
	printf("  dkms add -m %s -v %s && dkms build -m %s -v %s && dkms install -m %s -v %s\n", GS_DKMS_NAME,
	       GS_VERSION, GS_DKMS_NAME, GS_VERSION, GS_DKMS_NAME, GS_VERSION);
	if (o->skip_driver)
		printf("  NVIDIA's module and plugin were skipped (--driver=skip): prepare them by hand\n"
		       "  (docs/driver-patch.md), or rerun `%s/geforce-sriov install` without it\n", bindir);
	else
		printf("  %s/geforce-sriov-driver --source %s   (NVIDIA's module and plugin)\n", bindir,
		       o->nvidia_source ? o->nvidia_source : GS_NVIDIA_SRC);
	printf("  systemctl daemon-reload\n");
	if (enable_unit)
		printf("  systemctl enable %s %s   (they run at the next boot)\n", GS_UNIT_PREPARE, GS_UNIT_ACTIVATE);
	else
		printf("  select the GPUs with `%s/geforce-sriov install --device BDF`, one per GPU (it sets bdfs=,\n"
		       "  identity=on and identity_force_host=on and enables %s and %s), or set those keys in %s,\n"
		       "  then run `systemctl enable %s %s`\n",
		       bindir, GS_UNIT_PREPARE, GS_UNIT_ACTIVATE, GS_CONFIG_PATH, GS_UNIT_PREPARE, GS_UNIT_ACTIVATE);
}

#define HOLD_RULE "========================================================================\n"

int gs_color(FILE *f)
{
	const char *no_color = getenv("NO_COLOR"), *term = getenv("TERM");

	return isatty(fileno(f)) && !(no_color && *no_color) && !(term && strcmp(term, "dumb") == 0);
}

void gs_hold_notice_color(FILE *f, int by_hand, int color)
{
	if (color)
		fputs("\033[31m", f);
	fputs(HOLD_RULE
	      "IMPORTANT: This GPU is also used for the host display.\n"
	      "\n"
	      "When setup releases this GPU's Linux console, the monitor may go blank\n"
	      "or freeze on the last frame. You will have no local text console until\n"
	      "the next boot.\n"
	      "\n"
	      "Set up and test SSH or serial console access before proceeding.\n"
	      "You will need it to manage the host after setup.\n"
	      "\n"
	      "This GPU requires manual setup after every boot.\n"
	      "To keep NVIDIA's driver from loading before setup, create:\n"
	      "\n"
	      "  " GS_NVIDIA_HOLD "\n"
	      "\n"
	      "With the following contents:\n"
	      "\n"
	      "  blacklist nvidia\n"
	      "  install nvidia /bin/false\n"
	      "\n"
	      "Without this file, NVIDIA's driver may load first and cause setup to fail.\n"
	      "If that happens, another cold power cycle is required before you retry.\n", f);
	/* uninstall removes the file only when install wrote it. */
	fputs(by_hand ? "To revert, delete the file.\n" : "To revert, run `geforce-sriov uninstall`.\n", f);
	fputs(HOLD_RULE, f);
	if (color)
		fputs("\033[0m", f);
}

void gs_hold_notice(FILE *f, int by_hand)
{
	gs_hold_notice_color(f, by_hand, gs_color(f));
}

void gs_d3cold_notice_color(FILE *f, int color)
{
	if (color)
		fputs("\033[31m", f);
	fputs(HOLD_RULE
	      "IMPORTANT: This laptop GPU can power off when idle.\n"
	      "\n"
	      "This power state is called D3cold. When power returns, the GPU restarts,\n"
	      "losing its SR-IOV setup and stopping every running vGPU. Its firmware\n"
	      "resets too.\n"
	      "\n"
	      "setup can save your consent as d3cold=off in the config.\n"
	      "prepare reapplies the power settings whenever it prepares the GPU.\n"
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
	      HOLD_RULE, f);
	if (color)
		fputs("\033[0m", f);
}

void gs_d3cold_notice(FILE *f)
{
	gs_d3cold_notice_color(f, gs_color(f));
}

/*
 * What a console-release grant needs: the same file, lines and commands as docs/installation.md#boot-display-card.
 * When the modprobe.d directory modprobe_dir already holds nvidia back, the file that does instead of the file to
 * create, named as on the target host (without destdir, "" for a live install). hold_shown: setup prints the
 * notice of the file to create after install, so it is left out here.
 */
static void release_hold(const struct gs_config *cfg, const char *modprobe_dir, const char *destdir, int hold_shown)
{
	char files[2 * GS_PATH_MAX + 8];

	if (!gs_nvidia_hold(modprobe_dir, files, sizeof(files))) {
		size_t dl = strlen(destdir);
		const char *s = files, *at;

		printf("nvidia is held back at boot by ");
		while (dl && (at = strstr(s, destdir))) {
			printf("%.*s", (int)(at - s), s);
			s = at + dl;
		}
		printf("%s.\n", s);
	} else {
		if (!hold_shown)
			gs_hold_notice(stdout, 1);
		printf("Keep nvidia-persistenced, nvidia-vgpu-mgr and similar services disabled.\n");
	}
	printf(GS_RELEASE_STEPS_LEAD "\n");
	gs_release_steps(stdout, "  ", cfg, 1);
	printf("Details: docs/installation.md#boot-display-card\n");
}

void gs_install_next(const char *bindir, const struct gs_config *cfg, const char *modprobe_dir, int hold_shown)
{
	printf("\n%s and %s are enabled for the next boot, not started.\n", GS_UNIT_PREPARE, GS_UNIT_ACTIVATE);
	/* With a console release granted the boot unit prepares nothing, so there is nothing to skip. */
	if (!cfg->nrelease) {
		printf("To skip preparation for one boot, add geforce_sriov.skip to the kernel command line\n"
		       "(docs/recovery.md#turn-features-off).\n");
	} else {
		printf("\n");
		release_hold(cfg, modprobe_dir, "", hold_shown);
	}
	printf("\nNext: run `%s/geforce-sriov doctor`, then power the machine off and on. A restart is not\n"
	       "enough: the card must come up with the new module.\n", bindir);
	if (cfg->nrelease)
		printf("After the boot, run the commands above, or `sudo geforce-sriov setup`, which asks first, then\n"
		       "runs them.\n");
	else
		printf("After the boot, run `sudo geforce-sriov setup` again; it continues from there.\n");
}

#define DKMS_OTHER_MAX 8
#define DKMS_VER_LEN 64

/* Does a line of status before line name the same GS_DKMS_NAME version as v (n bytes, ended by ',' or ':')? */
static int dkms_seen_before(const char *status, const char *line, const char *v, size_t n)
{
	const size_t nl = strlen(GS_DKMS_NAME);

	for (const char *l = status; l && l < line; l = strchr(l, '\n')) {
		const char *w;

		l += *l == '\n';
		if (l >= line || strncmp(l, GS_DKMS_NAME, nl) != 0)
			continue;
		w = l + nl;
		if (*w == '/')
			w++;
		else if (w[0] == ',' && w[1] == ' ')
			w += 2;
		else
			continue;
		if (strncmp(w, v, n) == 0 && strcspn(w, ",:\n") == n)
			return 1;
	}
	return 0;
}

/*
 * The versions of GS_DKMS_NAME other than GS_VERSION in status, the output of `dkms status -m`: lines
 * "geforce-sriov/0.2.0, <kernel>, <arch>: installed", "geforce-sriov/0.2.0: added" (DKMS 3) or
 * "geforce-sriov, 0.2.0, <kernel>, <arch>: installed" (DKMS 2), each version once, at most max. A version
 * past max or as long as DKMS_VER_LEN is not taken: one warning names it with the command that removes it
 * by hand. A name with a character outside ok is not echoed: one warning says to look in `dkms status`.
 */
static unsigned int dkms_other_versions(const char *status, char ver[][DKMS_VER_LEN], unsigned int max)
{
	static const char ok[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._+~-";
	const size_t nl = strlen(GS_DKMS_NAME);
	unsigned int n = 0;
	int bad_named = 0;

	for (const char *line = status; line; line = strchr(line, '\n')) {
		const char *v;
		size_t len;
		unsigned int i = 0;

		line += *line == '\n';
		if (strncmp(line, GS_DKMS_NAME, nl) != 0)
			continue;
		v = line + nl;
		if (*v == '/')
			v++;
		else if (v[0] == ',' && v[1] == ' ')
			v += 2;
		else
			continue;
		len = strspn(v, ok);
		if (len == strlen(GS_VERSION) && strncmp(v, GS_VERSION, len) == 0 && (v[len] == ',' || v[len] == ':'))
			continue;
		while (i < n && !(strlen(ver[i]) == len && strncmp(ver[i], v, len) == 0))
			i++;
		if (i < n && (v[len] == ',' || v[len] == ':'))
			continue;
		if (len == 0 || len >= DKMS_VER_LEN || (v[len] != ',' && v[len] != ':') || n == max) {
			/* One line per version (DKMS lists it once per kernel); a name install does not accept is not echoed. */
			if (len == 0 || (v[len] != ',' && v[len] != ':')) {
				if (!bad_named++)
					fprintf(stderr, "geforce-sriov: warning: DKMS also holds a %s version whose name install does "
						"not accept, so install does not remove it; find it with `dkms status -m %s` and remove "
						"it with `dkms remove %s/<version> --all`\n", GS_DKMS_NAME, GS_DKMS_NAME, GS_DKMS_NAME);
			} else if (!dkms_seen_before(status, line, v, len)) {
				fprintf(stderr, "geforce-sriov: warning: DKMS also holds %s version %.*s, which install does not "
					"remove; remove it yourself: dkms remove %s/%.*s --all\n", GS_DKMS_NAME, (int)len, v,
					GS_DKMS_NAME, (int)len, v);
			}
			continue;
		}
		memcpy(ver[n], v, len);
		ver[n++][len] = '\0';
	}
	return n;
}

#define OLD_TREE_DEPTH 8

/*
 * 0 when everything below the directory path (manifest path; root + path on disk) is a directory or a
 * file m records that install still owns (manifest_owns()); otherwise -1 with in why the first other entry.
 */
static int old_tree_owned(struct manifest *m, const char *root, const char *path, unsigned int depth, char *why,
			  size_t len)
{
	char full[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;
	int r = 0;

	if (gs_pathf(full, sizeof(full), "%s%s", root, path) || !(d = opendir(full)))
		return snprintf(why, len, "%s%s cannot be read", root, path), -1;
	while (!r && (de = readdir(d))) {
		char sub[GS_PATH_MAX], subfull[GS_PATH_MAX];
		const struct entry *e;
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (gs_pathf(sub, sizeof(sub), "%s/%s", path, de->d_name) ||
		    gs_pathf(subfull, sizeof(subfull), "%s%s", root, sub) || lstat(subfull, &st)) {
			snprintf(why, len, "an entry of %s cannot be read", full);
			r = -1;
		} else if (S_ISDIR(st.st_mode)) {
			if (depth < OLD_TREE_DEPTH) {
				r = old_tree_owned(m, root, sub, depth + 1, why, len);
			} else {
				snprintf(why, len, "%s is nested too deep", subfull);
				r = -1;
			}
		} else if (!(e = manifest_find(m, 'f', sub))) {
			snprintf(why, len, "%s was not installed by geforce-sriov", subfull);
			r = -1;
		} else if (manifest_owns(subfull, e->sha) != OWN_YES) {
			snprintf(why, len, "%s changed since install", subfull);
			r = -1;
		}
	}
	closedir(d);
	return r;
}

/* Remove the directory full and everything below it, which old_tree_owned() accepted. 0 or -errno. */
static int old_tree_remove(const char *full, unsigned int depth)
{
	struct dirent *de;
	DIR *d = opendir(full);
	int r = 0;

	if (!d)
		return -errno;
	while (!r && (de = readdir(d))) {
		char sub[GS_PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (gs_pathf(sub, sizeof(sub), "%s/%s", full, de->d_name))
			r = -ENAMETOOLONG;
		else if (lstat(sub, &st))
			r = -errno;
		else if (S_ISDIR(st.st_mode))
			r = depth >= OLD_TREE_DEPTH ? -ELOOP : old_tree_remove(sub, depth + 1);
		else if (unlink(sub))
			r = -errno;
	}
	closedir(d);
	if (!r && rmdir(full))
		r = -errno;
	return r;
}

/*
 * After the DKMS step installed this version: the source tree /usr/src/GS_DKMS_NAME-<v> (below root) of
 * each version v it removed from DKMS goes too, when it is a real directory (not a link) that holds only
 * directories and files the install manifest at manifest records and install still owns, as uninstall
 * decides (manifest_owns()). Its manifest entries that are gone are dropped. One line per removed tree;
 * any tree that stays is a warning, never a failure.
 */
static void dkms_old_trees(const char *root, const char *manifest, char ver[][DKMS_VER_LEN], unsigned int n)
{
	struct manifest *m;
	unsigned int kept = 0;
	int r, dropped = 0;

	if (!n)
		return;
	m = calloc(1, sizeof(*m));
	if (!m || manifest_load(manifest, m)) {
		fprintf(stderr, "geforce-sriov: warning: %s cannot be read, so the source trees of the removed versions stay in "
			"/usr/src\n", manifest);
		free(m);
		return;
	}
	for (unsigned int i = 0; i < n; i++) {
		char path[GS_PATH_MAX], full[GS_PATH_MAX], why[GS_PATH_MAX + 64];
		struct stat st;
		size_t len;

		if (gs_pathf(path, sizeof(path), "/usr/src/%s-%s", GS_DKMS_NAME, ver[i]) ||
		    gs_pathf(full, sizeof(full), "%s%s", root, path))
			continue;
		len = strlen(path);
		if (lstat(full, &st) != 0) {
			/* Already gone: only its manifest entries go. */
		} else if (!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "geforce-sriov: warning: kept %s: it is %s, not the directory install wrote\n", full,
				S_ISLNK(st.st_mode) ? "a symbolic link" : "not a directory");
		} else if (old_tree_owned(m, root, path, 0, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: warning: kept %s: %s\n", full, why);
		} else if ((r = old_tree_remove(full, 0))) {
			fprintf(stderr, "geforce-sriov: warning: removing %s failed: %s; remove it yourself\n", full,
				strerror(-r));
		} else {
			printf("removed %s, the source of %s/%s, which DKMS no longer holds\n", full, GS_DKMS_NAME, ver[i]);
		}
		/* Its entries that are gone leave the manifest, so uninstall does not look for them. */
		for (unsigned int j = 0; j < m->n; j++) {
			char p[GS_PATH_MAX];
			int below = strncmp(m->e[j].path, path, len) == 0 && (m->e[j].path[len] == '/' || !m->e[j].path[len]);

			if (below && !gs_pathf(p, sizeof(p), "%s%s", root, m->e[j].path) && !gs_exists(p))
				m->e[j].type = 0, dropped = 1;
		}
	}
	if (dropped) {
		for (unsigned int j = 0; j < m->n; j++)
			if (m->e[j].type)
				m->e[kept++] = m->e[j];
		m->n = kept;
		if ((r = manifest_save(manifest, m)))
			fprintf(stderr, "geforce-sriov: warning: writing %s failed: %s\n", manifest, strerror(-r));
	}
	free(m);
}

int gs_dkms_step(int (*run)(const char *const argv[]), int registered, const char *status, const char *scratch,
		 const char *root, const char *manifest)
{
	const char *const add_argv[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const build_argv[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const inst_argv[] = { "dkms", "install", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const disable_argv[] = { "systemctl", "disable", GS_UNIT_PREPARE, GS_UNIT_ACTIVATE, NULL };
	char other[DKMS_OTHER_MAX][DKMS_VER_LEN];
	/* What install removes from DKMS before it adds this version: the other versions, then this one. */
	char ids[DKMS_OTHER_MAX + 1][DKMS_VER_LEN + sizeof(GS_DKMS_NAME) + 1];
	unsigned int nother = dkms_other_versions(status, other, DKMS_OTHER_MAX), nids = 0;
	char removed[1024] = "";
	int disabled;

	for (unsigned int i = 0; i < nother; i++)
		gs_pathf(ids[nids++], sizeof(ids[0]), "%s/%s", GS_DKMS_NAME, other[i]);
	if (registered)
		gs_pathf(ids[nids++], sizeof(ids[0]), "%s", DKMS_ID);
	if (nids) {
		char tree[GS_PATH_MAX];
		const char *const try_add[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "--dkmstree", tree, NULL };
		const char *const try_build[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "--dkmstree", tree, NULL };
		const char *const clean_argv[] = { "rm", "-rf", "--", tree, NULL };
		int built;

		if (gs_pathf(tree, sizeof(tree), "%s/geforce-sriov-dkms.XXXXXX", scratch) || !mkdtemp(tree)) {
			fprintf(stderr, "geforce-sriov: cannot create a scratch DKMS tree in %s; the installed %s module is unchanged\n",
				scratch, ids[0]);
			return GS_EXIT_FAIL;
		}
		if (!nother)
			printf("\n== %s is registered: a trial build in %s first; the installed module stays until it succeeds\n",
			       DKMS_ID, tree);
		else
			printf("\n== %s%s registered: a trial build of %s in %s first; the installed module stays until it\n"
			       "succeeds, then install replaces it\n",
			       ids[0], nids > 1 ? " and others are" : " is", DKMS_ID, tree);
		fflush(stdout);
		built = run(try_add) == 0 && run(try_build) == 0;
		if (run(clean_argv) != 0)
			fprintf(stderr, "geforce-sriov: removing the scratch tree %s failed; remove it yourself\n", tree);
		if (!built) {
			fprintf(stderr, "geforce-sriov: %s does not build for the running kernel (DKMS's output is above). Nothing\n"
				"was removed: the module installed earlier stays registered and loads at the next boot, and the boot\n"
				"units are unchanged. Fix the build, then run `geforce-sriov install` again.\n", DKMS_ID);
			return GS_EXIT_FAIL;
		}
		for (unsigned int i = 0; i < nids; i++) {
			const char *const rm_argv[] = { "dkms", "remove", ids[i], "--all", NULL };
			size_t at = strlen(removed);

			if (run(rm_argv) != 0) {
				fprintf(stderr, "geforce-sriov: dkms remove %s --all failed (DKMS's output is above), so the module installed\n"
					"earlier may be gone. Fix the cause, then run `geforce-sriov install` again.\n", ids[i]);
				return GS_EXIT_FAIL;
			}
			gs_pathf(removed + at, sizeof(removed) - at, "%s`dkms remove %s --all`", i ? " and " : "", ids[i]);
		}
	}
	if (run(add_argv) == 0 && run(build_argv) == 0 && run(inst_argv) == 0) {
		dkms_old_trees(root, manifest, other, nother);
		return GS_EXIT_OK;
	}
	if (!nids) {
		fprintf(stderr, "geforce-sriov: DKMS could not add/build/install %s for the running kernel; files are installed, `geforce-sriov uninstall` removes them\n",
			DKMS_ID);
		return GS_EXIT_FAIL;
	}
	/* The module installed earlier is gone: the next boot must not try to load it. */
	disabled = run(disable_argv) == 0;
	fprintf(stderr, "geforce-sriov: DKMS could not add, build or install %s for the running kernel although the trial\n"
		"build succeeded, and %s had already removed the module installed earlier.\n"
		"%s\nFix the cause in DKMS's output above, then run `geforce-sriov install` again%s.\n",
		DKMS_ID, removed,
		disabled ? "So that the next boot does not try to load it, " GS_UNIT_PREPARE " and " GS_UNIT_ACTIVATE
			   " are disabled."
			 : "Disabling " GS_UNIT_PREPARE " and " GS_UNIT_ACTIVATE " failed: the next boot's prepare fails to load "
			   "the module unless you run `systemctl disable " GS_UNIT_PREPARE " " GS_UNIT_ACTIVATE "`.",
		disabled ? "; it enables them when GPUs are selected" : "");
	return GS_EXIT_FAIL;
}

int gs_cmd_install(const struct gs_install_opts *o)
{
	const char *prefix = o->prefix ? o->prefix : GS_DEFAULT_PREFIX;
	const char *destdir = o->destdir ? o->destdir : "";
	int live = destdir[0] == '\0';
	char src[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], bindir[GS_PATH_MAX], libdir[GS_PATH_MAX];
	char hook[GS_PATH_MAX], sharedir[GS_PATH_MAX], cfgfile[GS_PATH_MAX], manifest[GS_PATH_MAX];
	char err[512], prog[GS_PATH_MAX], pkgdir[GS_PATH_MAX], pkgkernel[GS_PATH_MAX] = "", modprobe_dir[GS_PATH_MAX];
	char held[2 * GS_PATH_MAX + 8] = "";
	const char *hold = NULL; /* put_selection's held: NULL without --hold-nvidia */
	struct gs_config cfg;
	int config_changed = 0, cfg_exists, dropped = 0, d3cold_dropped, r;
	struct ctx *x;
	DIR *d;
	struct dirent *de;
	static const char *const artifacts[] = {
		"geforce-sriov", "driver/geforce-sriov-patch", "hook/libgeforce_sriov.so",
		"include/gs_devices.h", "systemd/geforce-sriov-prepare.service.in",
		"systemd/geforce-sriov-activate.service.in", "scripts/geforce-sriov-driver.sh",
		"systemd/nvidia-vgpud.service.d/geforce-sriov.conf.in", "config/config.example", NULL,
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
	/* Where the hold of nvidia is looked for: the staged tree's own when staging. */
	if (gs_pathf(modprobe_dir, sizeof(modprobe_dir), "%s/etc/modprobe.d", destdir)) {
		fprintf(stderr, "geforce-sriov: DESTDIR is too long\n");
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
	if (live) {
		if (geteuid() != 0) {
			fprintf(stderr, "geforce-sriov: a live install must run as root (set DESTDIR to stage)\n");
			return GS_EXIT_REFUSED;
		}
		if ((r = gs_lock()))
			return r;
		/* Another NVIDIA driver stops the run first, before the prerequisites are checked and before any write. */
		if (o->driver_file && (r = package_conflict(o->driver_file, "", o->accept_nvidia_license)))
			return r;
		if ((r = deps_step()))
			return r;
		if (gs_find_program("dkms", prog, sizeof(prog)) || gs_find_program("systemctl", prog, sizeof(prog))) {
			fprintf(stderr, "geforce-sriov: a live install needs dkms and systemctl\n");
			return GS_EXIT_REFUSED;
		}
		if (gs_exists(GS_ATTEMPT_MARKER))
			printf("warning: guard %s is set by an unfinished preparation. The boot unit refuses to run until you power the machine off and on and run `geforce-sriov prepare --clear-guard`\n",
			       GS_ATTEMPT_MARKER);
	}

	if (!live && (r = gs_mkdir_p(destdir, 0755))) {
		fprintf(stderr, "geforce-sriov: cannot create DESTDIR %s: %s\n", destdir, strerror(-r));
		return GS_EXIT_FAIL;
	}

	gs_pathf(bindir, sizeof(bindir), "%s/bin", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(libdir, sizeof(libdir), "%s/lib/geforce-sriov", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(sharedir, sizeof(sharedir), "%s/share/geforce-sriov", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(hook, sizeof(hook), "%s/libgeforce_sriov.so", libdir);
	gs_pathf(cfgfile, sizeof(cfgfile), "%s%s", destdir, GS_CONFIG_PATH);
	gs_pathf(manifest, sizeof(manifest), "%s%s", destdir, GS_MANIFEST);

	x = calloc(1, sizeof(*x));
	if (!x)
		return GS_EXIT_FAIL;
	x->destdir = destdir;
	if ((r = manifest_load(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: %s is unreadable or corrupt (%s)\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}

	/* Configuration: keep the existing file unless the command line changes it. */
	cfg_exists = gs_exists(cfgfile);
	if (gs_config_load(cfgfile, &cfg, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: existing config is invalid, fix or remove it: %s\n", err);
		free(x);
		return GS_EXIT_REFUSED;
	}
	/* --device without --boot-display-release drops a grant the config had: the hold install wrote goes too. */
	dropped = cfg.nrelease && o->ndevices && !o->nrelease;
	if (o->ndevices) {
		for (unsigned int i = 0; i < o->ndevices; i++)
			cfg.bdfs[i] = o->devices[i];
		cfg.nbdfs = o->ndevices;
		/* --device replaces the selection, and with it every console-release grant not given again. */
		for (unsigned int i = 0; i < o->nrelease; i++)
			cfg.boot_display_release[i] = o->release[i];
		cfg.nrelease = o->nrelease;
		/*
		 * A user-selected card gets the identity configuration these settings produce: the PCI
		 * info rewritten and the virtualization mode forced to HOST.
		 */
		cfg.identity = GS_SW_ON;
		cfg.identity_force_host = GS_SW_ON;
		config_changed = 1;
	}
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
	if (o->hold_nvidia && !cfg.nrelease) {
		fprintf(stderr, "geforce-sriov: --hold-nvidia requires a console-release grant.\n"
			"Only a boot display GPU can use it. Supply --boot-display-release BDF,\n"
			"or a boot_display_release= grant in %s.\n"
			"Try 'geforce-sriov --help'.\n", cfgfile);
		free(x);
		return GS_EXIT_USAGE;
	}
	/* The consent needs a laptop GPU in the selection; staging reads no card, so it records the flag unchecked. */
	if (o->disable_d3cold && live) {
		struct gs_addr laptops[GS_CFG_MAX_BDFS];

		if (!gs_laptop_gpus(GS_LIVE_SYSFS, cfg.bdfs, cfg.nbdfs, laptops)) {
			free(x);
			return gs_d3cold_usage();
		}
	}
	/* Never take over a config this tool did not write. */
	if (cfg_exists && config_changed && !manifest_find(&x->m, 'f', GS_CONFIG_PATH)) {
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
	/*
	 * --hold-nvidia: a hold already in modprobe.d is named instead, unless it is the hold file an earlier install
	 * wrote, unchanged since (an edited copy is the user's, as uninstall treats it). A hold file that is not this
	 * tool's refuses now, as the drop-in does.
	 */
	if (o->hold_nvidia) {
		hold = "";
		if (!gs_nvidia_hold(modprobe_dir, held, sizeof(held))) {
			struct entry *e = manifest_find(&x->m, 'f', GS_NVIDIA_HOLD);

			if (!(e && !gs_pathf(p, sizeof(p), "%s%s", destdir, GS_NVIDIA_HOLD) && strcmp(held, p) == 0 &&
			      manifest_owns(p, e->sha) == OWN_YES))
				hold = held;
		}
		if (!hold[0]) {
			struct entry *e = manifest_find(&x->m, 'f', GS_NVIDIA_HOLD);

			/* The recorded file edited so that it no longer holds nvidia back: the user's, never overwritten. */
			if (e && !gs_pathf(p, sizeof(p), "%s%s", destdir, GS_NVIDIA_HOLD) && gs_exists(p) &&
			    manifest_owns(p, e->sha) != OWN_YES) {
				fprintf(stderr, "geforce-sriov: %s was changed since install and no longer holds nvidia back (the "
					"lines `blacklist nvidia` and `install nvidia /bin/false`); fix it or remove it, then run "
					"install again. Nothing changed.\n", p);
				free(x);
				return GS_EXIT_REFUSED;
			}
		}
		if (!hold[0] && (r = unowned(x, GS_NVIDIA_HOLD))) {
			if (r != -EEXIST)
				fprintf(stderr, "geforce-sriov: %s%s: %s\n", destdir, GS_NVIDIA_HOLD, strerror(-r));
			free(x);
			return GS_EXIT_REFUSED;
		}
	}

	if (o->ndevices && live) {
		for (unsigned int i = 0; i < o->ndevices; i++) {
			struct gs_card c;

			gs_card_inspect(GS_LIVE_SYSFS, &o->devices[i], &c);
			if (gs_check_selectable(&c, gs_config_grants_release(&cfg, c.pf.str), err, sizeof(err))) {
				fprintf(stderr, "geforce-sriov: refused --device: %s\n", err);
				free(x);
				return GS_EXIT_REFUSED;
			}
			if (gs_config_grants_release(&cfg, c.pf.str) && c.boot_vga != 1)
				printf("%s: warning: --boot-display-release names it, but it is not the boot display (boot_vga=%d); prepare refuses this card while boot_display_release grants it\n",
				       c.pf.str, c.boot_vga);
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
		printf("vfs=%u takes effect when the module loads at the next boot: power the machine off and on%s\n", o->vfs,
		       live ? "" : ". Staging checks no card: run `geforce-sriov doctor` on the target host");
	if (o->nrelease) {
		char rel[GS_BDF_LIST_STR_SIZE];

		gs_addr_list_str(cfg.boot_display_release, cfg.nrelease, rel, sizeof(rel));
		printf("boot_display_release=%s: the boot unit defers and prepares none of the selected GPUs, and\n"
		       "nvidia-vgpud is held back.\n", rel);
		if (!o->release_note_shown)
			printf("Console release: granted; nothing is released now.\n"
			       "After the power cycle, run the commands below or `geforce-sriov setup`\n"
			       "to release the console. `geforce-sriov setup` asks again before releasing it.\n"
			       "The monitor may go blank or freeze on the last frame. You will have no\n"
			       "local text console until the following boot.\n"
			       "Set up and test SSH or a serial console before you power cycle.\n"
			       "Without that access, you cannot see the result or fix a problem.\n"
			       "See docs/installation.md#what-the-opt-in-changes.\n");
	}
	if (d3cold_dropped)
		printf("--device without --disable-d3cold: the D3cold consent (d3cold=off) is removed from the config\n");

	const char *const subst[] = { "@BINDIR@", bindir, "@HOOK@", hook, "@VERSION@", GS_VERSION, NULL };

	if (live && o->driver_file) {
		r = nvidia_files(x, o, &cfg, pkgdir, sizeof(pkgdir));
		if (r) {
			/* Kept so uninstall can remove what was written; a refusal that changed nothing writes nothing. */
			int ms = x->written || r != GS_EXIT_REFUSED ? manifest_save(manifest, &x->m) : 0;

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
		TRY(copy_file(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/driver/geforce-sriov-patch", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov-patch", bindir);
		TRY(copy_file(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/scripts/geforce-sriov-driver.sh", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov-driver", bindir);
		TRY(copy_file(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/hook/libgeforce_sriov.so", src);
		TRY(copy_file(x, p, hook, 0755), hook);
		gs_pathf(p, sizeof(p), "%s/config/config.example", src);
		gs_pathf(q, sizeof(q), "%s/config.example", sharedir);
		TRY(copy_file(x, p, q, 0644), q);

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

			if (!is_kernel_source(de->d_name) ||
			    gs_pathf(sp, sizeof(sp), "%s/kernel/%s", src, de->d_name) ||
			    stat(sp, &st) || !S_ISREG(st.st_mode) ||
			    gs_pathf(tp, sizeof(tp), "%s/%s", DKMS_SRC, de->d_name))
				continue;
			if ((r = copy_file(x, sp, tp, 0644))) {
				closedir(d);
				fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", tp, strerror(-r));
				goto fail;
			}
		}
		closedir(d);
		gs_pathf(p, sizeof(p), "%s/include/gs_devices.h", src);
		TRY(copy_file(x, p, DKMS_SRC "/include/gs_devices.h", 0644), "gs_devices.h");

		gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-prepare.service.in", src);
		TRY(put_template(x, p, UNIT_DIR "/" GS_UNIT_PREPARE, subst), GS_UNIT_PREPARE);
		gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-activate.service.in", src);
		TRY(put_template(x, p, UNIT_DIR "/" GS_UNIT_ACTIVATE, subst), GS_UNIT_ACTIVATE);
		TRY(make_dirs(x, GS_STATE_DIR), GS_STATE_DIR);
#undef TRY
		/* Staging has no driver step: the drop-in, the config and the hold follow the copies. */
		if (!live && (r = put_selection(x, src, subst, &cfg, config_changed, cfgfile, hold)))
			goto fail;
		if (!live && dropped && (r = drop_hold(x, modprobe_dir)))
			goto fail;
	}
	if ((r = manifest_save(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}

	if (!live) {
		free(x);
		printf("\nstaged below %s; nothing outside it changed. On the target host, as root:\n", destdir);
		print_live_steps(bindir, cfg.nbdfs > 0, o);
		if (cfg.nrelease) {
			printf("\n");
			release_hold(&cfg, modprobe_dir, destdir, o->hold_note_shown);
		}
		return GS_EXIT_OK;
	}

	/*
	 * NVIDIA's files next. The step refuses before it writes anything and rolls its own changes back
	 * when it fails, so a failure here leaves NVIDIA's module and plugin as they were. The vgpud
	 * drop-in and the config, which make nvidia-vgpud load the hook, are written only after it.
	 */
	if (!o->skip_driver) {
		char driver[GS_PATH_MAX];
		const char *source = pkgkernel[0] ? pkgkernel : o->nvidia_source ? o->nvidia_source : GS_NVIDIA_SRC;
		const char *argv[] = { driver, "--source", source, NULL };
		char why[1024];
		struct utsname u;
		int dr;

		gs_pathf(driver, sizeof(driver), "%s/geforce-sriov-driver", bindir);
		printf("\n== NVIDIA's files: %s --source %s\n", driver, source);
		printf("This may build NVIDIA's module for %s, which can take a few minutes; progress is in %s\n",
		       uname(&u) == 0 ? u.release : "the running kernel", GS_DRIVER_LOG);
		fflush(stdout);
		dr = gs_driver_step(argv, GS_DRIVER_LOG, why, sizeof(why));
		if (dr != 0) {
			if (dr < 0)
				fprintf(stderr, "geforce-sriov: %s could not run", driver);
			else
				fprintf(stderr, "geforce-sriov: %s exited with %d", driver, dr);
			fprintf(stderr, ": NVIDIA's module and plugin are not prepared%s.\n",
				o->driver_file ? "; what --driver-file extracted and installed stays" : "");
			/* The log's last lines, printed by gs_driver_step, already end with the cause. */
			fprintf(stderr, "geforce-sriov's own files are installed (uninstall removes them); %s%s and\n"
				"%s are not written.\n"
				"To install geforce-sriov alone, use --driver=skip (manual path: docs/driver-patch.md)\n",
				VGPUD_DROPIN, hold && !hold[0] ? ", " GS_CONFIG_PATH : "",
				hold && !hold[0] ? GS_NVIDIA_HOLD : GS_CONFIG_PATH);
			r = x->written || dr != GS_EXIT_REFUSED ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
			free(x);
			return r;
		}
		/* The step may have replaced NVIDIA's module and plugin: a later refusal no longer changed nothing. */
		x->written = 1;
	}
	if ((r = put_selection(x, src, subst, &cfg, config_changed, cfgfile, hold)))
		goto fail;
	if (dropped && (r = drop_hold(x, modprobe_dir)))
		goto fail;
	if ((r = manifest_save(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}
	free(x);

	{
		const char *reload_argv[] = { "systemctl", "daemon-reload", NULL };
		const char *enable_argv[] = { "systemctl", "enable", GS_UNIT_PREPARE, NULL };
		const char *disable_argv[] = { "systemctl", "disable", GS_UNIT_PREPARE, NULL };
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
		if (gs_run(reload_argv) != 0) {
			fprintf(stderr, "geforce-sriov: systemctl daemon-reload failed\n");
			return GS_EXIT_FAIL;
		}
		if (cfg.nbdfs) {
			const char *enable_act[] = { "systemctl", "enable", GS_UNIT_ACTIVATE, NULL };

			if (gs_run(enable_argv) != 0) {
				fprintf(stderr, "geforce-sriov: systemctl enable %s failed\n", GS_UNIT_PREPARE);
				return GS_EXIT_FAIL;
			}
			if (gs_run(enable_act) != 0) {
				fprintf(stderr, "geforce-sriov: systemctl enable %s failed\n", GS_UNIT_ACTIVATE);
				return GS_EXIT_FAIL;
			}
			gs_install_next(bindir, &cfg, modprobe_dir, o->hold_note_shown);
		} else {
			const char *disable_act[] = { "systemctl", "disable", GS_UNIT_ACTIVATE, NULL };
			int dp = gs_run(disable_argv), da = gs_run(disable_act);

			if (dp || da) {
				fprintf(stderr, "geforce-sriov: no GPU is selected, but `systemctl disable` failed, so %s or %s may\n"
					"still be enabled from an earlier install. Run `systemctl disable %s %s`.\n",
					GS_UNIT_PREPARE, GS_UNIT_ACTIVATE, GS_UNIT_PREPARE, GS_UNIT_ACTIVATE);
				return GS_EXIT_FAIL;
			}
			printf("\nNo GPU selected: %s and %s are installed but not enabled. Select GPUs with\n"
			       "`geforce-sriov install --device BDF`, one per GPU; it also sets identity=on and\n"
			       "identity_force_host=on and enables the units.\n",
			       GS_UNIT_PREPARE, GS_UNIT_ACTIVATE);
		}
	}
	return GS_EXIT_OK;
fail:
	/* A refusal before anything was written changes nothing, the manifest included. */
	if (!x->written && r == -EEXIST) {
		free(x);
		return GS_EXIT_REFUSED;
	}
	/* Keep a manifest of what was written so uninstall can remove it; its directory may not be made yet. */
	if ((r = make_dirs(x, GS_STATE_DIR)) || (r = manifest_save(manifest, &x->m)))
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
	free(x);
	return GS_EXIT_FAIL;
}

/* ---- uninstall ------------------------------------------------------------ */

/*
 * Active VFs on a GPU the module acts on. Its bdfs parameter is the authoritative list
 * (the config only adds to it). When that list cannot be read or parsed, safety cannot be
 * proven and the caller refuses. Other SR-IOV devices (network cards) never count.
 */
static int any_active_vfs(char *which, size_t len)
{
	struct gs_addr list[2 * GS_CFG_MAX_BDFS];
	unsigned int n = 0, k;
	struct gs_config cfg;
	char param[256], err[512];

	if (gs_read_attr(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME "/parameters/bdfs", param, sizeof(param)) ||
	    gs_parse_addr_list("bdfs", param, list, &n, err, sizeof(err))) {
		snprintf(which, len, "the loaded module's bdfs parameter cannot be read or parsed, so active VFs on its GPUs cannot be ruled out");
		return 1;
	}
	if (gs_config_load(gs_config_path(), &cfg, err, sizeof(err)) == 0)
		for (k = 0; k < cfg.nbdfs && n < 2 * GS_CFG_MAX_BDFS; k++)
			list[n++] = cfg.bdfs[k];
	for (k = 0; k < n; k++) {
		char p[GS_PATH_MAX];
		unsigned int v = 0;

		gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/bus/pci/devices/%s/sriov_numvfs", list[k].str);
		if (gs_read_uint(p, &v) == 0 && v > 0) {
			snprintf(which, len, "%s has %u VFs", list[k].str, v);
			return 1;
		}
	}
	return 0;
}

static int any_prepared(void)
{
	DIR *d = opendir(GS_PREPARED_DIR);
	struct dirent *de;
	int found = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)))
		if (de->d_name[0] != '.')
			found = 1;
	closedir(d);
	return found;
}

static int unload_module(void)
{
	char p[GS_PATH_MAX], why[256];
	/* rmmod needs no modules index, so it works whatever state DKMS is in. */
	const char *argv[] = { "rmmod", GS_MODULE_NAME, NULL };

	if (!gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME)) {
		printf("%s is not loaded\n", GS_MODULE_NAME);
		return 0;
	}
	if (any_active_vfs(why, sizeof(why))) {
		fprintf(stderr, "geforce-sriov: refused --unload: %s; disable the VFs, or power the machine off and on instead\n", why);
		return GS_EXIT_REFUSED;
	}
	if (any_prepared()) {
		fprintf(stderr, "geforce-sriov: refused --unload: a GPU prepared in this boot depends on the livepatch for its enumeration; power the machine off and on instead\n");
		return GS_EXIT_REFUSED;
	}
	gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/kernel/livepatch/%s/enabled", GS_MODULE_NAME);
	if (gs_exists(p) && gs_write_attr(p, "0")) {
		fprintf(stderr, "geforce-sriov: disabling livepatch %s failed\n", GS_MODULE_NAME);
		return GS_EXIT_FAIL;
	}
	gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/kernel/livepatch/%s", GS_MODULE_NAME);
	for (unsigned int ms = 0; gs_exists(p); ms += 100) {
		char tp[GS_PATH_MAX];
		unsigned int t = 1, e = 1;

		gs_pathf(tp, sizeof(tp), "%s/transition", p);
		if (gs_read_uint(tp, &t) == 0 && t == 0) {
			gs_pathf(tp, sizeof(tp), "%s/enabled", p);
			if (gs_read_uint(tp, &e) == 0 && e == 0)
				break;
		}
		if (ms >= 15000) {
			fprintf(stderr, "geforce-sriov: livepatch %s did not finish unpatching; module left loaded\n",
				GS_MODULE_NAME);
			return GS_EXIT_FAIL;
		}
		gs_sleep_ms(100);
	}
	if (gs_run(argv) != 0) {
		fprintf(stderr, "geforce-sriov: rmmod %s failed\n", GS_MODULE_NAME);
		return GS_EXIT_FAIL;
	}
	printf("%s unloaded\n", GS_MODULE_NAME);
	return 0;
}

/*
 * Uninstall's removal of one manifest file: removed only while install still owns it (manifest_owns()),
 * so a link put in its place is kept even when it points at identical content. 1 removed; 0 kept
 * (saying why) or absent; -errno when the unlink failed.
 */
int gs_uninstall_file(const char *path, const char *want)
{
	switch (manifest_owns(path, want)) {
	case OWN_ABSENT:
		return 0;
	case OWN_UNREADABLE:
		printf("kept %s (unreadable)\n", path);
		return 0;
	case OWN_CHANGED:
		printf("kept %s (changed since install)\n", path);
		return 0;
	case OWN_YES:
		break;
	}
	if (unlink(path) < 0) {
		int e = errno;

		fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", path, strerror(e));
		return -e;
	}
	printf("removed %s\n", path);
	return 1;
}

/* driver --restore: 0, or its exit status with in why its reason, or its exit status when it gave none. */
static int run_restore(const char *driver, char *why, size_t len)
{
	const char *const argv[] = { driver, "--restore", NULL };
	int rc = gs_driver_step(argv, NULL, why, len);

	if (rc < 0)
		snprintf(why, len, "%s --restore could not run", driver);
	else if (rc && !why[0])
		snprintf(why, len, "%s --restore exited with %d", driver, rc);
	return rc;
}

/* The nvidia.ko the driver step restores for kernel release: modinfo's, else the DKMS path. */
static void restore_target(const char *release, char *module, size_t len)
{
	if (gs_nvidia_module_path(release, module, len))
		gs_pathf(module, len, "/lib/modules/%s/updates/dkms/nvidia.ko", release);
}

/* After a restore: each stock/<kernel>/ left in state (other kernels), with the command that puts it back. */
static void other_kernels(const char *state)
{
	char stock[GS_PATH_MAX], p[GS_PATH_MAX], module[GS_PATH_MAX];
	struct dirent **names;
	struct stat st;
	int n, i, any = 0;

	if (gs_pathf(stock, sizeof(stock), "%s/stock", state))
		return;
	n = scandir(stock, &names, NULL, alphasort);
	for (i = 0; i < n; i++) {
		const char *k = names[i]->d_name;

		if (k[0] != '.' && !gs_pathf(p, sizeof(p), "%s/%s", stock, k) && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			if (!any++)
				printf("NVIDIA's module backups stay for these kernels; to put one back, as root:\n");
			restore_target(k, module, sizeof(module));
			printf("restore %s: cp %s/nvidia.ko %s && depmod -a %s\n", k, p, module, k);
		}
		free(names[i]);
	}
	if (n >= 0)
		free(names);
}

/*
 * The hold of nvidia in modprobe_dir, which is the user's file and stays. A hold file install wrote and
 * uninstall kept (changed since install) is still listed in m, and its "kept" line already names it.
 */
static void hold_left(const char *modprobe_dir, struct manifest *m)
{
	char files[2 * GS_PATH_MAX + 8];

	if (!gs_nvidia_hold(modprobe_dir, files, sizeof(files)) && !manifest_find(m, 'f', files))
		printf("%s still holds nvidia back at boot; remove it if nothing else needs it\n", files);
}

int gs_uninstall_files(const char *path, const char *modprobe_dir, const char *const keep[])
{
	static const char suffix[] = "/geforce-sriov-driver";
	struct manifest *m = calloc(1, sizeof(*m));
	const char *slash = strrchr(path, '/');
	char driver[GS_PATH_MAX] = "", driver_sha[65] = "", why[1024] = "", dir[GS_PATH_MAX] = "";
	unsigned int i, kept = 0, files = 0;
	int r, rc = GS_EXIT_OK, restored = 1, changed = 0;

	if (!m)
		return GS_EXIT_FAIL;
	if ((r = manifest_load(path, m))) {
		fprintf(stderr, "geforce-sriov: %s is corrupt (%s); nothing removed\n", path, strerror(-r));
		free(m);
		return GS_EXIT_FAIL;
	}
	/*
	 * The driver step keeps NVIDIA's backup and its own record outside the install manifest, so ask it
	 * to put the stock module back. It restores only while the installed module is still the one it
	 * installed, and reports when there is nothing to do. The path comes from the manifest, because
	 * --prefix can put it anywhere, and the script runs as root only while it is still the file install
	 * wrote (manifest_owns()). When it fails or is not run, it stays, so the user can run it again.
	 */
	for (i = 0; i < m->n && !driver[0]; i++) {
		size_t len = strlen(m->e[i].path);

		if (m->e[i].type == 'f' && len > sizeof(suffix) - 1 &&
		    strcmp(m->e[i].path + len - (sizeof(suffix) - 1), suffix) == 0) {
			gs_copy(driver, sizeof(driver), m->e[i].path);
			gs_copy(driver_sha, sizeof(driver_sha), m->e[i].sha);
		}
	}
	if (driver[0] && !gs_exists(driver)) {
		printf("kept NVIDIA's files: %s is already gone\n", driver);
	} else if (driver[0] && manifest_owns(driver, driver_sha) != OWN_YES) {
		snprintf(why, sizeof(why), "%s changed since install, not run", driver);
		restored = 0;
		changed = 1;
	} else if (driver[0] && run_restore(driver, why, sizeof(why)) != 0) {
		restored = 0;
	}
	for (i = 0; i < m->n; i++) {
		size_t k = 0;

		if (m->e[i].type != 'f' || (!restored && strcmp(m->e[i].path, driver) == 0))
			continue;
		while (keep && keep[k] && strcmp(keep[k], m->e[i].path) != 0)
			k++;
		if ((!keep || !keep[k]) && gs_uninstall_file(m->e[i].path, m->e[i].sha) < 0)
			rc = GS_EXIT_FAIL;
	}
	for (i = m->n; i-- > 0;)
		if (m->e[i].type == 'd' && rmdir(m->e[i].path) == 0)
			printf("removed %s/\n", m->e[i].path);
	/* What is still there stays in the manifest, so a later install takes it over and a later uninstall removes it. */
	for (i = 0; i < m->n; i++) {
		if (!gs_exists(m->e[i].path))
			continue;
		files += m->e[i].type == 'f';
		m->e[kept++] = m->e[i];
	}
	m->n = kept;
	if (slash && slash != path)
		gs_pathf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
	if (restored) {
		if (files) {
			if ((r = manifest_save(path, m))) {
				fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", path, strerror(-r));
				rc = GS_EXIT_FAIL;
			} else {
				printf("kept %s: it lists the files kept above, so a later install or uninstall still owns them\n",
				       path);
			}
		} else if (unlink(path) < 0 && errno != ENOENT) {
			rc = GS_EXIT_FAIL;
		} else if (dir[0] && rmdir(dir) == 0) {
			/* The manifest's directory (GS_STATE_DIR) goes too once nothing else is in it. */
			printf("removed %s/\n", dir);
		}
		if (dir[0])
			other_kernels(dir);
		hold_left(modprobe_dir, m);
		free(m);
		return rc;
	}
	if ((r = manifest_save(path, m)))
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", path, strerror(-r));
	{
		struct utsname u;
		char module[GS_PATH_MAX] = "";
		const char *release = uname(&u) == 0 ? u.release : "<kernel>";

		restore_target(release, module, sizeof(module));
		/* A script changed since install is not to be run as root: point at the reinstall instead. */
		if (changed)
			fprintf(stderr, "NVIDIA's module was not restored: %s. Check that file, or put the installed copy back "
				"with `geforce-sriov install`, then run uninstall again; or as root: cp ", why);
		else
			fprintf(stderr, "NVIDIA's module was not restored: %s. Fix that, then run %s --restore, or as root: cp ",
				why, driver);
		fprintf(stderr, GS_STATE_DIR "/stock/%s/nvidia.ko %s && depmod -a %s. For the plugin link and the modprobe "
			"file, see docs/driver-patch.md#reverting.\n", release, module, release);
	}
	hold_left(modprobe_dir, m);
	free(m);
	return GS_EXIT_FAIL;
}

int gs_cmd_uninstall(int unload)
{
	static const char *const units[] = { GS_UNIT_PREPARE, GS_UNIT_ACTIVATE };
	static const char *const files[] = { UNIT_DIR "/" GS_UNIT_PREPARE, UNIT_DIR "/" GS_UNIT_ACTIVATE };
	const char *keep[3] = { NULL, NULL, NULL };
	unsigned int nkeep = 0;
	struct manifest *m;
	char prog[GS_PATH_MAX];
	int r, rc = GS_EXIT_OK;

	if (geteuid() != 0) {
		fprintf(stderr, "geforce-sriov: uninstall must run as root\n");
		return GS_EXIT_REFUSED;
	}
	if ((r = gs_lock()))
		return r;
	/*
	 * Unload first: after `dkms remove` the module is gone from the modprobe index and
	 * `modprobe -r` can no longer find it. unload_module() refuses with active VFs or a
	 * GPU prepared this boot, and then nothing is removed.
	 */
	if (unload && (r = unload_module())) {
		fprintf(stderr, "geforce-sriov: nothing removed\n");
		return r;
	}
	m = calloc(1, sizeof(*m));
	if (!m)
		return GS_EXIT_FAIL;
	if ((r = manifest_load(GS_MANIFEST, m))) {
		fprintf(stderr, "geforce-sriov: %s is corrupt (%s); nothing removed\n", GS_MANIFEST, strerror(-r));
		free(m);
		return GS_EXIT_FAIL;
	}
	r = m->n;
	free(m);
	if (r == 0) {
		printf("nothing installed by geforce-sriov (%s absent)\n", GS_MANIFEST);
		return GS_EXIT_OK;
	}

	/* A unit whose disable failed keeps its file, so that uninstall run again still finds it to disable. */
	for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
		const char *argv[] = { "systemctl", "disable", units[u], NULL };

		if (gs_find_program("systemctl", prog, sizeof(prog)) || !gs_exists(files[u]) || gs_run(argv) == 0)
			continue;
		fprintf(stderr, "geforce-sriov: systemctl disable %s failed; kept %s so that uninstall run again\n"
			"disables and removes it\n", units[u], files[u]);
		keep[nkeep++] = files[u];
		rc = GS_EXIT_FAIL;
	}
	if (gs_exists("/var/lib/dkms/" GS_DKMS_NAME)) {
		const char *argv[] = { "dkms", "remove", DKMS_ID, "--all", NULL };

		if (gs_run(argv) != 0) {
			fprintf(stderr, "geforce-sriov: dkms remove %s failed\n", DKMS_ID);
			rc = GS_EXIT_FAIL;
		}
	}
	/* The driver step's log is not an installed file; it goes with the rest. */
	if (unlink(GS_DRIVER_LOG) == 0)
		printf("removed %s\n", GS_DRIVER_LOG);
	if (gs_uninstall_files(GS_MANIFEST, "/etc/modprobe.d", keep) != GS_EXIT_OK)
		rc = GS_EXIT_FAIL;
	/* The preparation guard is not an installed file: a reinstall must still see it. */
	if (gs_exists(GS_ATTEMPT_MARKER))
		printf("kept guard %s: an unfinished preparation needs you to power the machine off and on, then run `geforce-sriov prepare --clear-guard`\n",
		       GS_ATTEMPT_MARKER);
	if (gs_find_program("systemctl", prog, sizeof(prog)) == 0) {
		const char *argv[] = { "systemctl", "daemon-reload", NULL };

		gs_run(argv);
	}
	if (!unload && gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME))
		printf("%s stays loaded until the next boot, which runs without preparation\n",
		       GS_MODULE_NAME);
	return rc;
}
