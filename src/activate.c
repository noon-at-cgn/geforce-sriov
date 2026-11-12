// SPDX-License-Identifier: MIT
/*
 * geforce-sriov activation: the one runtime command of 0.3.0.
 *
 * It brings the selected GPUs up from a cold, NVIDIA-bound or already-enabled state, and
 * the former preparation duties are part of it. It runs in phases A-E (SPEC section 5,
 * P0-FLOW-DESIGN sections 2-4):
 *
 *   A read-only preflight: guard, selection, identity, build, clients, display plan,
 *     resource fit. `--dry-run` stops here and prints the conditional plan.
 *   B quiesce: stop the display manager only with consent, write the guard and the
 *     `activating` marker.
 *   C power and reveal: wake the cards, reuse or load the livepatch, hand the firmware
 *     framebuffer over, load nvidia and (display path) nvidia_modeset then nvidia_drm.
 *   D empty-bus resize, per card needing it: bridge power/control=on with read-back,
 *     bounded remove of companions then PF, empty-bus verification, ONE bridge-scoped
 *     rescan, identity/SR-IOV/VF BAR/MSI-X/ARI verification.
 *   E enable and finish: unload nvidia_drm then nvidia_modeset only with zero VFs,
 *     sriov-manage -e once per card, nvidia_vgpu_vfio, vgpud, manager, PF re-probe,
 *     display reload, VF type and power readback, optional nvidia_uvm, display manager
 *     restart.
 *
 * Every write and process step goes through the struct gs_host callbacks, so the whole
 * transaction can be driven by the simulated host in tests/test_lifecycle.c.
 */
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "gs_lifecycle.h"

#define POLL_MS 50u
#define LIVEPATCH_WAIT_MS 10000u
#define VF_BIND_WAIT_MS 30000u
#define MGR_ACTIVE_WAIT_MS 30000u
#define MGR_SETTLE_MS 2000u
#define PF_REBIND_WAIT_MS 10000u
#define VF_ACTIVE_WAIT_MS 5000u
#define VF_TYPES_WAIT_MS 15000u
#define REMOVE_WAIT_MS 5000u
#define RETURN_WAIT_MS 15000u
#define SRIOV_ATTR_WAIT_MS 5000u
#define FN_RETURN_WAIT_MS 5000u
#define BRIDGE_WAKE_MS 5000u
#define PF_WAKE_MS 5000u
#define DM_ACTIVE_WAIT_MS 10000u
#define SYSFS_DEADLINE_MS 10000u
#define RELEASE_WAIT_MS 5000u

static int wait_exists(const char *path, int want, unsigned int ms);
static int node_holder(const struct gs_host *h, int gpu, char *line, size_t len);
static int wait_attr_eq(const char *path, const char *want, unsigned int ms);

/* One host write through the deadline callback, or the synchronous one when there is none. */
static int sysfs_write(const struct gs_host *h, const char *path, const char *value, unsigned int ms)
{
	if (h->write_attr_deadline)
		return h->write_attr_deadline(h, path, value, ms);
	return h->write_attr ? h->write_attr(path, value) : -ENOSYS;
}

/* ---- configuration and root ---------------------------------------------- */

static int require_root(const char *cmd)
{
	if (geteuid() != 0) {
		fprintf(stderr, "geforce-sriov: %s must run as root\n", cmd);
		return GS_EXIT_REFUSED;
	}
	return 0;
}

/* Every parent directory of path must be a root-owned directory nobody else can write. */
static int parents_safe(const char *path, char *why, size_t len)
{
	char dir[GS_PATH_MAX], *slash;
	struct stat st;

	gs_copy(dir, sizeof(dir), path);
	while ((slash = strrchr(dir, '/')) != NULL) {
		*slash = '\0';
		if (!dir[0])
			break;
		if (stat(dir, &st) < 0)
			return snprintf(why, len, "%.300s: %s", dir, strerror(errno)), -1;
		if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
			return snprintf(why, len, "%.300s is not a root-owned directory others cannot write", dir), -1;
	}
	return 0;
}

/*
 * Root's configuration: opened once without following links, checked through the same
 * descriptor (owner, mode, type, parents) and parsed from it, so no check/use gap exists.
 * The same rules the old live loader used, now owned by the one runtime command.
 */
static int load_live_config(const char *path, struct gs_config *cfg)
{
	char err[512] = "cannot stat the open file", *buf;
	struct stat st;
	size_t off = 0;
	int fd, r;

	cfg->present = 0;
	if (path[0] != '/') {
		fprintf(stderr, "geforce-sriov: refusing config %s: $%s must be an absolute path\n", path, GS_CONFIG_ENV);
		return GS_EXIT_REFUSED;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == ENOENT) {
			fprintf(stderr, "geforce-sriov: %s does not exist; select devices with `geforce-sriov install --device BDF`\n",
				path);
			return GS_EXIT_REFUSED;
		}
		fprintf(stderr, "geforce-sriov: refusing config %s: %s\n", path, strerror(errno));
		return GS_EXIT_REFUSED;
	}
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "geforce-sriov: refusing config %s: %s\n", path, strerror(errno));
		close(fd);
		return GS_EXIT_REFUSED;
	}
	if (!S_ISREG(st.st_mode))
		snprintf(err, sizeof(err), "%.300s is not a regular file", path);
	else if (st.st_uid != 0)
		snprintf(err, sizeof(err), "%.300s is not owned by root", path);
	else if (st.st_mode & 022)
		snprintf(err, sizeof(err), "%.300s is group- or world-writable", path);
	else if (parents_safe(path, err, sizeof(err)))
		;
	else
		err[0] = '\0';
	if (err[0]) {
		fprintf(stderr, "geforce-sriov: refusing config: %s\n", err);
		close(fd);
		return GS_EXIT_REFUSED;
	}
	buf = malloc(GS_CONFIG_MAX_BYTES + 2);
	if (!buf) {
		close(fd);
		return GS_EXIT_FAIL;
	}
	for (;;) {
		ssize_t n = read(fd, buf + off, GS_CONFIG_MAX_BYTES + 1 - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			fprintf(stderr, "geforce-sriov: refusing config %s: read failed: %s\n", path, strerror(errno));
			free(buf);
			close(fd);
			return GS_EXIT_REFUSED;
		}
		if (n == 0)
			break;
		off += (size_t)n;
		if (off > GS_CONFIG_MAX_BYTES)
			break;
	}
	close(fd);
	buf[off] = '\0';
	r = gs_config_parse(buf, off, cfg, err, sizeof(err));
	free(buf);
	if (r) {
		fprintf(stderr, "geforce-sriov: config: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	if (cfg->nbdfs == 0) {
		fprintf(stderr, "geforce-sriov: no device selected (bdfs= in %s), so nothing is done\n", path);
		return GS_EXIT_REFUSED;
	}
	return 0;
}

/* ---- markers: the recovery guard and the activating record ---------------- */

static int boot_uuid(const char *s)
{
	if (strlen(s) != 36)
		return 0;
	for (size_t i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return 0;
		} else if (!isxdigit((unsigned char)s[i])) {
			return 0;
		}
	}
	return 1;
}

/* A different boot ID permits only the early livepatch, not activation or guard removal:
 * reboot identity does not establish a cold power cycle or a safe GPU state. Unknown,
 * malformed and duplicate IDs fail closed, including legacy markers without an ID. */
static int guard_from_previous_boot(const struct gs_host *h, const char *path)
{
	char current[64], procpath[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t len;
	int seen = 0, different = 0;

	if (gs_pathf(procpath, sizeof(procpath), "%s/sys/kernel/random/boot_id", h->proc) ||
	    gs_read_attr(procpath, current, sizeof(current)) || !boot_uuid(current) ||
	    gs_read_whole(path, &data, &len, 4096))
		return 0;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "boot_id=", 8) != 0)
			continue;
		seen++;
		different = boot_uuid(line + 8) && strcasecmp(line + 8, current) != 0;
	}
	free(data);
	return seen == 1 && different;
}

static int write_marker(const struct gs_host *h, const char *bdfs, const char *step)
{
	char boot[64] = "unknown", buf[1024], path[GS_PATH_MAX];
	int n, r;

	if (gs_pathf(path, sizeof(path), "%s/sys/kernel/random/boot_id", h->proc) ||
	    gs_read_attr(path, boot, sizeof(boot)))
		gs_copy(boot, sizeof(boot), "unknown");
	n = snprintf(buf, sizeof(buf), "boot_id=%s\ntime=%lld\nbdfs=%s\nstep=%s\n", boot, (long long)time(NULL), bdfs,
		     step);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -EOVERFLOW;
	if (gs_pathf(path, sizeof(path), "%s/" GS_ATTEMPT_NAME, h->state_dir))
		return -ENAMETOOLONG;
	r = gs_mkdir_p(h->state_dir, 0755);
	if (r)
		return r;
	return gs_write_file_atomic(path, buf, (size_t)n, 0644);
}

static int clear_marker(const struct gs_host *h)
{
	char path[GS_PATH_MAX];

	if (gs_pathf(path, sizeof(path), "%s/" GS_ATTEMPT_NAME, h->state_dir))
		return -ENAMETOOLONG;
	if (unlink(path) < 0 && errno != ENOENT)
		return -errno;
	return gs_fsync_dir(h->state_dir);
}

static int write_activating(const struct gs_host *h, const char *pf_list)
{
	char boot[64] = "unknown", buf[1024], path[GS_PATH_MAX];
	int n, r;

	if (gs_pathf(path, sizeof(path), "%s/sys/kernel/random/boot_id", h->proc) ||
	    gs_read_attr(path, boot, sizeof(boot)))
		gs_copy(boot, sizeof(boot), "unknown");
	n = snprintf(buf, sizeof(buf), "boot_id=%s\npfs=%s\n", boot, pf_list);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -EOVERFLOW;
	if (gs_pathf(path, sizeof(path), "%s/" GS_ACTIVATING_NAME, h->run_dir))
		return -ENAMETOOLONG;
	r = gs_mkdir_p(h->run_dir, 0755);
	if (r)
		return r;
	return gs_write_file_atomic(path, buf, (size_t)n, 0644);
}

static int clear_activating(const struct gs_host *h)
{
	char path[GS_PATH_MAX];

	if (gs_pathf(path, sizeof(path), "%s/" GS_ACTIVATING_NAME, h->run_dir))
		return -ENAMETOOLONG;
	if (unlink(path) < 0 && errno != ENOENT)
		return -errno;
	return gs_fsync_dir(h->run_dir);
}

/* ---- kernel command line -------------------------------------------------- */

/* Tokens of procroot/cmdline on blanks; 1 when token is one of them ("pci=noari" checked by the caller). */
static int cmdline_token(const char *procroot, const char *token)
{
	char p[GS_PATH_MAX], *data, *tok, *save = NULL;
	size_t len;
	int found = 0;

	if (gs_pathf(p, sizeof(p), "%s/cmdline", procroot) || gs_read_whole(p, &data, &len, 1 << 16))
		return 0;
	for (tok = strtok_r(data, " \t\n", &save); tok && !found; tok = strtok_r(NULL, " \t\n", &save))
		found = strcmp(tok, token) == 0;
	free(data);
	return found;
}

/* ---- driver and build identity ------------------------------------------- */

/* ---- selected cards ------------------------------------------------------- */

/* How many VFs with a vGPU the refusal lists, one per line; the command after it names them all. */
#define HOST_VGPU_SHOWN 8

struct host_vgpus {
	char first[160];
	char lines[1024];
	char bdfs[2048];
};

/* VFs with a vGPU on the host: current_vgpu_type not 0, or unreadable. Their count, 0, or -1. */
static int host_vgpu(const char *sys, struct host_vgpus *v)
{
	char p[GS_PATH_MAX], type[32], line[160];
	struct dirent **names;
	size_t lo = 0, bo = 0;
	unsigned int vendor;
	int found = 0, n, i, r;

	v->first[0] = v->lines[0] = v->bdfs[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sys) || (n = scandir(p, &names, NULL, alphasort)) < 0)
		return snprintf(v->first, sizeof(v->first), "%.100s/bus/pci/devices is unreadable", sys), -1;
	for (i = 0; i < n; i++) {
		const char *d = names[i]->d_name;

		line[0] = '\0';
		if (d[0] != '.' && !gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, d) && gs_exists(p) &&
		    !gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", sys, d) && !gs_read_hex(p, &vendor) &&
		    vendor == 0x10de) {
			r = gs_read_dev_attr(sys, d, "nvidia/current_vgpu_type", type, sizeof(type));
			if (r == 0 && strcmp(type, "0") != 0)
				snprintf(line, sizeof(line), "VF %.32s has vGPU type %.31s", d, type);
			else if (r && r != -ENOENT)
				snprintf(line, sizeof(line), "VF %.32s: nvidia/current_vgpu_type is unreadable (%.64s)", d,
					 strerror(-r));
		}
		if (line[0]) {
			if (!found++)
				gs_copy(v->first, sizeof(v->first), line);
			if (found <= HOST_VGPU_SHOWN && lo < sizeof(v->lines))
				lo += (size_t)snprintf(v->lines + lo, sizeof(v->lines) - lo, "  %s\n", line);
			if (bo < sizeof(v->bdfs))
				bo += (size_t)snprintf(v->bdfs + bo, sizeof(v->bdfs) - bo, "%s%.32s", bo ? " " : "", d);
		}
		free(names[i]);
	}
	free(names);
	if (found > HOST_VGPU_SHOWN && lo < sizeof(v->lines))
		snprintf(v->lines + lo, sizeof(v->lines) - lo, "  and %d more\n", found - HOST_VGPU_SHOWN);
	return found;
}

/* An NVIDIA VF on the host whose PF is not one of cards: 1 with which, 0 none, -1 unreadable. */
static int foreign_vf(const char *sys, const struct gs_vgpu_card *cards, unsigned int n, char *which, size_t len)
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	const char *base;
	struct dirent *de;
	unsigned int vendor, i;
	int found = 0;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sys) || !(d = opendir(p)))
		return snprintf(which, len, "%s/bus/pci/devices is unreadable", sys), -1;
	while (!found && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, de->d_name) || !realpath(p, real))
			continue;
		base = strrchr(real, '/');
		base = base ? base + 1 : real;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", sys, de->d_name) || gs_read_hex(p, &vendor) ||
		    vendor != 0x10de)
			continue;
		for (i = 0; i < n && strcmp(cards[i].pf.str, base) != 0; i++)
			;
		if (i < n)
			continue;
		snprintf(which, len, "VF %.32s of %.32s", de->d_name, base);
		found = 1;
	}
	closedir(d);
	return found;
}

/* ---- livepatch and NVIDIA module loading --------------------------------- */

static int wait_livepatch(const struct gs_host *h)
{
	char en[GS_PATH_MAX], tr[GS_PATH_MAX];
	unsigned int e, t;

	gs_pathf(en, sizeof(en), "%s/kernel/livepatch/%s/enabled", h->sys, GS_MODULE_NAME);
	gs_pathf(tr, sizeof(tr), "%s/kernel/livepatch/%s/transition", h->sys, GS_MODULE_NAME);
	for (unsigned int ms = 0;; ms += POLL_MS) {
		if (gs_read_uint(en, &e) == 0 && gs_read_uint(tr, &t) == 0 && e == 1 && t == 0)
			return 0;
		if (ms >= LIVEPATCH_WAIT_MS)
			return -1;
		gs_sleep_ms(POLL_MS);
	}
}

static const char *sw_yn(int v)
{
	return v == GS_SW_OFF ? "N" : "Y";
}

/*
 * Load the livepatch with the configured BDFs, switches and count, or reuse the loaded one when
 * its parameters are exactly those. It is normally already loaded by the early boot unit; the
 * load here covers a by-hand run after install. 0, GS_EXIT_REFUSED (already loaded with other
 * parameters, or the module refused an already-bound PF) or GS_EXIT_FAIL (it did not transition).
 */
static int ensure_livepatch(const struct gs_host *h, const struct gs_config *cfg, const char *bdfs)
{
	struct gs_module_state m;
	char a[16], x[16], v[16], b[256];
	int r;

	gs_read_module_state(h->sys, &m);
	if (m.loaded) {
		if (strcmp(m.ari, sw_yn(cfg->ari)) || strcmp(m.msix, sw_yn(cfg->msix)) ||
		    gs_module_vfs(&m) != gs_config_vfs(cfg) || strcmp(m.bdfs, bdfs)) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is already loaded with ari=%s msix=%s vfs=%u bdfs=%s, not the requested ari=%s msix=%s vfs=%u bdfs=%s; power the machine off and on to change them\n",
				GS_MODULE_NAME, m.ari, m.msix, gs_module_vfs(&m), m.bdfs, sw_yn(cfg->ari), sw_yn(cfg->msix),
				gs_config_vfs(cfg), bdfs);
			return GS_EXIT_REFUSED;
		}
		printf("%s already loaded with the requested parameters\n", GS_MODULE_NAME);
	} else {
		const char *argv[] = { "modprobe", GS_MODULE_NAME, a, x, cfg->vfs ? v : b, cfg->vfs ? b : NULL, NULL };

		snprintf(a, sizeof(a), "ari=%d", cfg->ari == GS_SW_OFF ? 0 : 1);
		snprintf(x, sizeof(x), "msix=%d", cfg->msix == GS_SW_OFF ? 0 : 1);
		snprintf(v, sizeof(v), "vfs=%u", cfg->vfs);
		if (gs_pathf(b, sizeof(b), "bdfs=%s", bdfs))
			return GS_EXIT_FAIL;
		printf("modprobe");
		for (size_t k = 1; argv[k]; k++)
			printf(" %s", argv[k]);
		putchar('\n');
		r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
		if (r == -ETIMEDOUT) {
			fprintf(stderr, "geforce-sriov: modprobe %s did not finish within %u ms; whether it loaded is unknown.\n%s\n",
				GS_MODULE_NAME, GS_MODULE_DEADLINE_MS, gs_recovery_msg);
			return GS_EXIT_COLD_CYCLE;
		}
		if (r != 0) {
			char ver[32], line[512];

			gs_nvidia_version(h->proc, ver, sizeof(ver));
			if (ver[0]) {
				gs_driver_version_line(ver, line, sizeof(line));
				fprintf(stderr, "geforce-sriov: refused, nothing changed: modprobe %s exited with %d while NVIDIA's\n"
					"driver %s was already bound. The livepatch reveals hidden MSI-X only when it loads before nvidia\n"
					"binds a selected PF, so power the machine off and on, then activation runs with the livepatch in\n"
					"place (`journalctl -k -b` names the refused GPU). %s\n",
					GS_MODULE_NAME, r, ver, line);
			} else {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: modprobe %s exited with %d; nothing was removed.\n"
					"`journalctl -k -b` names the refused GPU and the reason\n", GS_MODULE_NAME, r);
			}
			return GS_EXIT_REFUSED;
		}
	}
	if (wait_livepatch(h)) {
		fprintf(stderr, "geforce-sriov: livepatch %s did not reach enabled=1 transition=0 within %u ms; nothing was removed\n",
			GS_MODULE_NAME, LIVEPATCH_WAIT_MS);
		return GS_EXIT_FAIL;
	}
	printf("livepatch %s enabled, transition complete\n", GS_MODULE_NAME);
	return 0;
}

/* ---- display selection and the display plan ------------------------------ */

/* 1 when a is one of the selected PFs (their VFs are found by their own address too when listed). */
static int affected_bdf(const struct gs_addr *a, const struct gs_addr *sel, unsigned int nsel)
{
	for (unsigned int i = 0; i < nsel; i++)
		if (strcmp(sel[i].str, a->str) == 0)
			return 1;
	return 0;
}

/* The PCI address a class device (card0, fb0) belongs to, through its device link: 0 or -1. */
static int class_device_bdf(const struct gs_host *h, const char *cls, const char *name, struct gs_addr *out)
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	const char *base;

	if (gs_pathf(p, sizeof(p), "%s/class/%s/%s/device", h->sys, cls, name) || !realpath(p, real))
		return -1;
	base = strrchr(real, '/');
	return base ? gs_addr_parse(base + 1, out) : -1;
}

/* Compose one "  ..." line onto reason without overflowing it. */
static void plan_note(char *reason, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void plan_note(char *reason, size_t len, const char *fmt, ...)
{
	size_t used = strlen(reason);
	va_list ap;

	if (used + 3 >= len)
		return;
	reason[used++] = '\n';
	reason[used++] = ' ';
	reason[used++] = ' ';
	reason[used] = '\0';
	va_start(ap, fmt);
	vsnprintf(reason + used, len - used, fmt, ap);
	va_end(ap);
}

/*
 * Everything that would be interrupted by stopping, unloading or removing a display GPU:
 * the display manager unit (resolved link), the display modules' holders, the DRM and
 * framebuffer nodes of the selected PFs, the processes holding NVIDIA/DRM/fb nodes, and the
 * logged-in sessions. An unreadable source sets inspection_unknown: it is never treated as
 * "no clients".
 */
int gs_display_plan_build(const struct gs_host *h, const struct gs_config *cfg, struct gs_display_plan *plan)
{
	static const char *const mods[] = { "nvidia_drm", "nvidia_modeset", "nvidia" };
	char link[GS_PATH_MAX];
	struct gs_addr sel[GS_CFG_MAX_BDFS];
	unsigned int nsel = 0;
	char target[PATH_MAX];
	struct dirent *de;
	ssize_t n;
	DIR *d;

	memset(plan, 0, sizeof(*plan));
	gs_copy(plan->manager_unit, sizeof(plan->manager_unit), "display-manager.service");
	for (unsigned int i = 0; i < cfg->nbdfs; i++) {
		struct gs_card c;

		gs_card_inspect(h->sys, &cfg->bdfs[i], &c);
		if (c.present && gs_display_kms(cfg, &c))
			sel[nsel++] = cfg->bdfs[i];
	}
	if (nsel == 0)
		return 0;

	/* The unit display-manager.service points at, when the link is readable. */
	if (!gs_pathf(link, sizeof(link), "%s/etc/systemd/system/display-manager.service", h->root ? h->root : "")) {
		n = readlink(link, target, sizeof(target) - 1);
		if (n > 0) {
			const char *base;

			target[n] = '\0';
			base = strrchr(target, '/');
			gs_copy(plan->manager_unit, sizeof(plan->manager_unit), base ? base + 1 : target);
		} else if (errno != ENOENT) {
			plan->inspection_unknown = 1;
			plan_note(plan->reason, sizeof(plan->reason), "cannot read %s: %s", link, strerror(errno));
		}
	}
	if (h->run) {
		const char *argv[] = { "systemctl", "is-active", "--quiet", plan->manager_unit, NULL };

		plan->manager_active = gs_host_run(h, argv, DM_ACTIVE_WAIT_MS) == 0;
	}

	/* Holders and reference counts of the display modules. */
	for (size_t k = 0; k < sizeof(mods) / sizeof(mods[0]); k++) {
		char p[GS_PATH_MAX], ref[32] = "";
		struct dirent *hd;

		if (gs_pathf(p, sizeof(p), "%s/module/%s", h->sys, mods[k]) || !gs_exists(p))
			continue;
		if (!gs_pathf(p, sizeof(p), "%s/module/%s/refcnt", h->sys, mods[k]))
			gs_read_attr(p, ref, sizeof(ref));
		if (gs_pathf(p, sizeof(p), "%s/module/%s/holders", h->sys, mods[k]) || !(d = opendir(p))) {
			plan->inspection_unknown = 1;
			plan_note(plan->reason, sizeof(plan->reason), "cannot list the holders of %s", mods[k]);
			continue;
		}
		while ((hd = readdir(d))) {
			if (hd->d_name[0] == '.')
				continue;
			plan_note(plan->reason, sizeof(plan->reason), "module %s is held by %s", mods[k], hd->d_name);
		}
		closedir(d);
		if (ref[0] && strcmp(ref, "0") != 0 && strcmp(ref, "1") != 0)
			plan_note(plan->reason, sizeof(plan->reason), "%s has refcnt %s", mods[k], ref);
	}

	/* DRM nodes of the selected PFs and their VFs. */
	if (!gs_pathf(target, sizeof(target), "%s/class/drm", h->sys) && (d = opendir(target))) {
		while ((de = readdir(d))) {
			struct gs_addr a;

			if (strncmp(de->d_name, "card", 4) != 0 || !isdigit((unsigned char)de->d_name[4]))
				continue;
			if (class_device_bdf(h, "drm", de->d_name, &a) == 0 && affected_bdf(&a, sel, nsel))
				plan_note(plan->reason, sizeof(plan->reason), "%s belongs to %s", de->d_name, a.str);
		}
		closedir(d);
	} else {
		plan->inspection_unknown = 1;
		plan_note(plan->reason, sizeof(plan->reason), "cannot list %s/class/drm", h->sys);
	}

	/* Processes holding an NVIDIA, DRM or framebuffer node. Unreadable /proc is unknown. */
	if (gs_pathf(target, sizeof(target), "%s", h->proc) || !(d = opendir(target))) {
		plan->inspection_unknown = 1;
		plan_note(plan->reason, sizeof(plan->reason), "cannot list %s: %s", h->proc, strerror(errno));
	} else {
		while ((de = readdir(d))) {
			char fdp[GS_PATH_MAX], fdlink[GS_PATH_MAX];
			struct dirent *fde;
			unsigned long pid;
			DIR *fd;
			char *end;

			if (de->d_name[0] < '0' || de->d_name[0] > '9')
				continue;
			pid = strtoul(de->d_name, &end, 10);
			if (*end || pid == 0 || pid == (unsigned long)getpid())
				continue;
			if (gs_pathf(fdp, sizeof(fdp), "%s/%s/fd", h->proc, de->d_name) || !(fd = opendir(fdp)))
				continue;
			while ((fde = readdir(fd))) {
				ssize_t m = readlinkat(dirfd(fd), fde->d_name, fdlink, sizeof(fdlink) - 1);

				if (m <= 0)
					continue;
				fdlink[m] = '\0';
				if (strncmp(fdlink, "/dev/nvidia", 11) == 0 || strncmp(fdlink, "/dev/dri/", 9) == 0 ||
				    strncmp(fdlink, "/dev/fb", 7) == 0)
					plan_note(plan->reason, sizeof(plan->reason), "process %lu holds %s", pid, fdlink);
			}
			closedir(fd);
		}
		closedir(d);
	}

	/* Login sessions: supplementary attribution; a session without a seat is not graphical. */
	if (!gs_pathf(target, sizeof(target), "%s/run/systemd/sessions", h->root ? h->root : "") && (d = opendir(target))) {
		while ((de = readdir(d))) {
			char *data, *seat, sess[GS_PATH_MAX];
			size_t len;

			if (de->d_name[0] == '.')
				continue;
			if (gs_pathf(sess, sizeof(sess), "%s/%s", target, de->d_name) || gs_read_whole(sess, &data, &len, 4096))
				continue;
			seat = strstr(data, "\nSEAT=");
			if (seat)
				seat++;
			else if (strncmp(data, "SEAT=", 5) == 0)
				seat = data;
			if (seat && seat[5] && seat[5] != '\n') {
				plan_note(plan->reason, sizeof(plan->reason), "session %s is active on a seat", de->d_name);
				plan->manager_active = 1; /* a seated session holds a display node */
			}
			free(data);
		}
		closedir(d);
	}
	return 0;
}

/* ---- VF BAR fit and DRM allowlist ---------------------------------------- */

/*
 * The RM GPU IDs of the display-selected cards for nvidia-drm's gpu_allow parameter. The list
 * holds only boot display GPUs even for display=on: see gs_display_kms(). Never empty in the
 * load path; an empty gpu_allow would mean every GPU, so display_load() refuses to load with it.
 * OBSERVED mapping: 03:00.0 -> 0x300, 04:00.0 -> 0x400 in the display runs, i.e. bus << 8.
 * Recorded as an inference, printed before use.
 */
static void display_allowlist(const struct gs_host *h, const struct gs_config *cfg, const struct gs_vgpu_card *cards,
			      unsigned int n, char *out, size_t len)
{
	size_t off = 0;

	out[0] = '\0';
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;
		int n2;

		gs_card_inspect(h->sys, &cards[i].pf, &c);
		if (!gs_display_kms(cfg, &c))
			continue;
		n2 = snprintf(out + off, len - off, "%s0x%x", off ? "," : "", cards[i].pf.bus << 8);
		if (n2 < 0 || (size_t)n2 >= len - off)
			break;
		off += (size_t)n2;
	}
}

/*
 * display=on is a request for KMS, and only a boot display GPU can carry it while the card
 * serves VFs (see gs_display_kms()). Name each excluded card and the effective allowlist once;
 * when the selection holds no boot display GPU at all, say that no display stack is loaded.
 */
static void display_on_notices(const struct gs_host *h, const struct gs_config *cfg, const struct gs_vgpu_card *cards,
			       unsigned int n)
{
	char allow[256];
	int boot = 0;

	if (cfg->display != GS_DISPLAY_ON)
		return;
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;

		gs_card_inspect(h->sys, &cards[i].pf, &c);
		if (c.boot_vga == 1)
			boot = 1;
	}
	if (!boot) {
		printf("display=on: no selected GPU is the boot display; no display stack is loaded\n");
		return;
	}
	display_allowlist(h, cfg, cards, n, allow, sizeof(allow));
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;

		gs_card_inspect(h->sys, &cards[i].pf, &c);
		if (c.boot_vga == 1)
			continue;
		printf("%s: display=on is limited to the boot display GPU while this card serves VFs%s%s\n",
		       cards[i].pf.str, allow[0] ? "; gpu_allow=" : "", allow);
	}
}

/* ---- power, bridge hold and the firmware framebuffer ---------------------- */

/* What a wake wrote, so that an abort before any removal can give it back. */
struct woken {
	struct gs_addr pf;
	int laptop;
	int wrote_control, wrote_d3cold, wrote_bridge;
	char dir[GS_PATH_MAX];
	char control[16];
	char d3cold[8];
	char bridge_dir[GS_PATH_MAX];
	char bridge_control[16];
};

/* A bounded write that failed: exit 5 when it did not finish, because its effect is then unknown. */
static int write_exit(int r)
{
	return r == -ETIMEDOUT ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
}

/* Puts one setting back. 0, or -ETIMEDOUT when the write did not finish and the setting is unknown. */
static int give_back(const struct gs_host *h, const char *path, const char *old, const char *who, const char *what)
{
	int r;

	if (!old[0])
		return 0;
	r = sysfs_write(h, path, old, SYSFS_DEADLINE_MS);
	if (r == -ETIMEDOUT)
		fprintf(stderr, "geforce-sriov: %s: restoring %s=%s did not finish within %u ms; its value is unknown\n", who,
			what, old, SYSFS_DEADLINE_MS);
	else if (r)
		fprintf(stderr, "geforce-sriov: %s: restoring %s=%s failed\n", who, what, old);
	else
		printf("%s: %s restored to %s\n", who, what, old);
	return r == -ETIMEDOUT ? r : 0;
}

/*
 * Gives back what a wake wrote. Each setting is tried once: what was tried is forgotten, so a
 * second call does not write again. 1 when a write did not finish, which is uncertain state.
 */
static int unwake(const struct gs_host *h, struct woken *w)
{
	char p[GS_PATH_MAX];
	const char *slash = strrchr(w->bridge_dir, '/');
	int timed = 0;

	if (w->wrote_d3cold && !gs_pathf(p, sizeof(p), "%s/d3cold_allowed", w->dir))
		timed |= give_back(h, p, w->d3cold, w->pf.str, "d3cold_allowed") != 0;
	if (w->wrote_control && !gs_pathf(p, sizeof(p), "%s/power/control", w->dir))
		timed |= give_back(h, p, w->control, w->pf.str, "power/control") != 0;
	if (w->wrote_bridge && strcmp(w->bridge_control, "on") != 0 && slash &&
	    !gs_pathf(p, sizeof(p), "%s/power/control", w->bridge_dir))
		timed |= give_back(h, p, w->bridge_control, slash + 1, "bridge power/control") != 0;
	w->wrote_d3cold = w->wrote_control = w->wrote_bridge = 0;
	return timed;
}

/*
 * The mandatory bridge gate: power/control=on with the value read back as exactly "on", then
 * up to BRIDGE_WAKE_MS for runtime_status to be active (or unsupported, which means runtime PM
 * is off for it and it cannot be suspended). 0 done, -1 refused. A write timeout is uncertain
 * device state. Nothing is removed before this passes.
 */
static int bridge_hold(const struct gs_host *h, const struct gs_card *c, int *uncertain, char *why, size_t len)
{
	char p[GS_PATH_MAX], v[16] = "", st[32] = "";
	unsigned int t;
	int r;

	if (!c->bridge_is_pci || !c->bridge_dir[0])
		return snprintf(why, len, "%s: no PCI bridge above the card to hold awake", c->pf.str), -1;
	if (gs_pathf(p, sizeof(p), "%s/power/control", c->bridge_dir))
		return snprintf(why, len, "%s: the bridge's power/control path is too long", c->pf.str), -1;
	r = sysfs_write(h, p, "on", SYSFS_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		*uncertain = 1;
		return snprintf(why, len, "%s: writing on to bridge %s power/control did not finish within %u ms; whether the bridge is awake is unknown",
				c->pf.str, c->bridge.str, SYSFS_DEADLINE_MS), -1;
	}
	if (r)
		return snprintf(why, len, "%s: writing on to bridge %s power/control failed: %s", c->pf.str, c->bridge.str,
				strerror(-r)), -1;
	if (gs_read_attr(p, v, sizeof(v)) || strcmp(v, "on") != 0)
		return snprintf(why, len, "%s: bridge %s power/control reads '%s' after writing on; nothing was removed",
				c->pf.str, c->bridge.str, v[0] ? v : "unreadable"), -1;
	for (t = 0;; t += POLL_MS) {
		if (gs_pathf(p, sizeof(p), "%s/power/runtime_status", c->bridge_dir) ||
		    gs_read_attr(p, st, sizeof(st)))
			st[0] = '\0';
		if (strcmp(st, "active") == 0 || strcmp(st, "unsupported") == 0)
			break;
		if (t >= BRIDGE_WAKE_MS)
			return snprintf(why, len, "%s: bridge %s power/control is on but runtime_status is '%s' after %u ms",
					c->pf.str, c->bridge.str, st[0] ? st : "unreadable", BRIDGE_WAKE_MS), -1;
		gs_sleep_ms(POLL_MS);
	}
	printf("%s: bridge %s power/control=%s runtime_status=%s (held awake)\n", c->pf.str, c->bridge.str, v, st);
	return 0;
}

/* 1 when a runtime_status value means the device is suspended. */
static int runtime_suspended(const char *status)
{
	return strcmp(status, "suspended") == 0 || strcmp(status, "suspending") == 0;
}

/*
 * Before the first write to a card: the bridge gate, the PF's power/control=on and, on a laptop
 * row, d3cold_allowed=0 so it cannot drop to D3cold in the middle of the transaction. 1 done,
 * 0 nothing to do, -1 failed (everything written is given back) or uncertain.
 */
static int wake_card(const struct gs_host *h, const struct gs_card *c, struct woken *w, int *uncertain, char *why,
		     size_t len)
{
	char p[GS_PATH_MAX], was[32] = "", now[32] = "";
	unsigned int t;
	int r;

	memset(w, 0, sizeof(*w));
	w->pf = c->pf;
	w->laptop = c->entry && c->entry->laptop;
	if (gs_pathf(w->dir, sizeof(w->dir), "%s/bus/pci/devices/%s", h->sys, c->pf.str))
		return -1;
	if (gs_pathf(p, sizeof(p), "%s/power/runtime_status", w->dir) || gs_read_attr(p, was, sizeof(was)))
		was[0] = '\0';
	if (!runtime_suspended(was) && !w->laptop)
		return 0;
	gs_copy(w->bridge_dir, sizeof(w->bridge_dir), c->bridge_dir);
	if (c->bridge_dir[0] && !gs_pathf(p, sizeof(p), "%s/power/control", c->bridge_dir))
		gs_read_attr(p, w->bridge_control, sizeof(w->bridge_control));
	if (bridge_hold(h, c, uncertain, why, len))
		return -1;
	w->wrote_bridge = 1;
	if (gs_pathf(p, sizeof(p), "%s/power/control", w->dir) || gs_read_attr(p, w->control, sizeof(w->control)))
		w->control[0] = '\0';
	if (strcmp(w->control, "on") != 0) {
		r = sysfs_write(h, p, "on", SYSFS_DEADLINE_MS);
		if (r == -ETIMEDOUT) {
			*uncertain = 1;
			*uncertain |= unwake(h, w);
			return snprintf(why, len, "%s: writing on to its power/control did not finish within %u ms; whether it is awake is unknown",
					c->pf.str, SYSFS_DEADLINE_MS), -1;
		}
		if (r) {
			*uncertain |= unwake(h, w);
			return snprintf(why, len, "%s: writing on to its power/control failed: %s", c->pf.str, strerror(-r)), -1;
		}
		w->wrote_control = 1;
	}
	if (w->laptop) {
		if (gs_pathf(p, sizeof(p), "%s/d3cold_allowed", w->dir) || gs_read_attr(p, w->d3cold, sizeof(w->d3cold)))
			w->d3cold[0] = '\0';
		if (strcmp(w->d3cold, "0") != 0) {
			r = sysfs_write(h, p, "0", SYSFS_DEADLINE_MS);
			if (r == -ETIMEDOUT) {
				*uncertain = 1;
				*uncertain |= unwake(h, w);
				return snprintf(why, len, "%s: writing 0 to its d3cold_allowed did not finish within %u ms; whether the GPU can still power off is unknown",
						c->pf.str, SYSFS_DEADLINE_MS), -1;
			}
			if (r) {
				*uncertain |= unwake(h, w);
				return snprintf(why, len, "%s: writing 0 to its d3cold_allowed failed: %s", c->pf.str,
						strerror(-r)), -1;
			}
			w->wrote_d3cold = 1;
		}
	}
	for (t = 0;; t += POLL_MS) {
		gs_pathf(p, sizeof(p), "%s/power/runtime_status", w->dir);
		if (gs_read_attr(p, now, sizeof(now)))
			now[0] = '\0';
		if (!runtime_suspended(now) && now[0])
			break;
		if (t >= PF_WAKE_MS) {
			*uncertain |= unwake(h, w);
			return snprintf(why, len, "%s is powered down (runtime_status was %s) and did not come back within %u ms of power/control=on (now '%s')",
					c->pf.str, was[0] ? was : "unreadable", PF_WAKE_MS, now[0] ? now : "unreadable"), -1;
		}
		gs_sleep_ms(POLL_MS);
	}
	printf("%s: was runtime_status=%s: power/control on, d3cold_allowed %s\n", c->pf.str, was[0] ? was : "?",
	       w->laptop ? "0" : "unchanged");
	return 1;
}

/*
 * Hand the firmware framebuffer of a display-selected card to the KMS path before the removal:
 * unbind its platform driver and verify that only this card's framebuffer was released and that
 * nothing else refuses. A release is never given back after the removal was attempted.
 */
static int handoff_fb(const struct gs_host *h, const struct gs_card *c, char *fw, size_t fwlen, char *why, size_t len)
{
	struct gs_display d;
	char p[GS_PATH_MAX];
	unsigned int t;
	int r;

	fw[0] = '\0';
	/*
	 * When nvidia-drm already drives the console (a second activation in the same boot, or a
	 * host whose display stack loaded before activation), there is no firmware framebuffer to
	 * release and the inspect below would refuse because fb0 is on the card itself.
	 */
	{
		char name[64] = "";

		if (!gs_pathf(p, sizeof(p), "%s/class/graphics/fb0/name", h->sys))
			gs_read_attr(p, name, sizeof(name));
		if (!strncmp(name, "nvidia-drmdrmfb", 15))
			return 0;
	}
	gs_display_inspect(h->sys, h->proc, c, &d);
	/*
	 * `display=on` selects every configured GPU, but only the card that owns the firmware
	 * framebuffer has one to release. A card without its own has nothing to hand off, and the
	 * inspect refuses it precisely because the framebuffer belongs to another card.
	 */
	if (gs_display_fw_state(&d) == GS_FW_NONE)
		return 0;
	if (d.refuse[0])
		return snprintf(why, len, "%s: its console is in use: %s", c->pf.str, d.refuse), -1;
	if (gs_display_fw_state(&d) != GS_FW_BOUND)
		return 0;
	if (gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s/driver/unbind", h->sys, d.fw))
		return snprintf(why, len, "%s: the firmware framebuffer path is too long", c->pf.str), -1;
	r = sysfs_write(h, p, d.fw, SYSFS_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		gs_copy(why, len, "the firmware framebuffer unbind did not finish; its state is unknown");
		return -ETIMEDOUT;
	}
	if (r)
		return snprintf(why, len, "%s: unbinding its firmware framebuffer %s from %s failed: %s", c->pf.str, d.fw,
				d.fw_driver, strerror(-r)), -1;
	gs_copy(fw, fwlen, d.fw);
	for (t = 0;; t += POLL_MS) {
		struct gs_card now;

		gs_card_inspect(h->sys, &c->pf, &now);
		gs_display_inspect(h->sys, h->proc, &now, &d);
		if (strcmp(d.fw, fw) == 0 && gs_display_fw_state(&d) == GS_FW_RELEASED && !d.refuse[0])
			break;
		if (t >= RELEASE_WAIT_MS)
			return snprintf(why, len, "%s: the console is not released: %s", c->pf.str,
					d.refuse[0] ? d.refuse : "its firmware framebuffer is still bound"), -1;
		gs_sleep_ms(POLL_MS);
	}
	printf("%s: firmware framebuffer %s released\n", c->pf.str, fw);
	return 0;
}

/* ---- empty-bus remove and resize ----------------------------------------- */

/* 1 when a device with the same domain and bus is still listed; 0 none; -1 unreadable. */
static int bus_not_empty(const struct gs_host *h, const struct gs_addr *pf, char *which, size_t len)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", h->sys) || !(d = opendir(p)))
		return snprintf(which, len, "%s/bus/pci/devices is unreadable", h->sys), -1;
	while ((de = readdir(d))) {
		struct gs_addr a;

		if (de->d_name[0] == '.' || gs_addr_parse(de->d_name, &a))
			continue;
		if (a.domain == pf->domain && a.bus == pf->bus) {
			snprintf(which, len, "%s is still on bus %04x:%02x", de->d_name, pf->domain, pf->bus);
			closedir(d);
			return 1;
		}
	}
	closedir(d);
	return 0;
}

/* The card's own functions other than the PF, companions first in descending function order. */
static int removal_plan(const struct gs_card *c, struct gs_addr *unbind, unsigned int *nunbind, char *why,
			size_t len)
{
	*nunbind = 0;
	if (!c->present)
		return snprintf(why, len, "%s: no such PCI function", c->pf.str), -1;
	if (c->pf.func != 0 || c->is_vf)
		return snprintf(why, len, "%s: not a physical function .0", c->pf.str), -1;
	if (!c->entry)
		return snprintf(why, len, "%s (%04x:%04x) is not in the device table (see `geforce-sriov devices`)",
				c->pf.str, c->vendor, c->device), -1;
	if (c->driver[0] && strcmp(c->driver, "nvidia") != 0)
		return snprintf(why, len, "%s is bound to driver '%s', not nvidia; activation removes only an NVIDIA-bound card",
				c->pf.str, c->driver), -1;
	if (c->has_sriov && c->numvfs > 0)
		return snprintf(why, len, "%s: %u virtual functions are active; activation never disables VFs",
				c->pf.str, c->numvfs), -1;
	if (c->nvfs_on_bus > 0)
		return snprintf(why, len, "%s: %u virtual functions exist on bus %04x:%02x", c->pf.str, c->nvfs_on_bus,
				c->pf.domain, c->pf.bus), -1;
	if (c->nothers > 0)
		return snprintf(why, len, "%s: bus %04x:%02x is shared with %s (%04x:%04x); removing the bus would touch an unrelated device",
				c->pf.str, c->pf.domain, c->pf.bus, c->others[0].bdf.str, c->others[0].vendor,
				c->others[0].device), -1;
	for (unsigned int i = 0; i < c->nfns; i++) {
		const struct gs_fn *fn = &c->fns[i];

		if (!fn->driver[0])
			continue;
		if ((fn->class_code >> 8) == 0x0403 && fn->vendor == GS_VENDOR_NVIDIA &&
		    strcmp(fn->driver, "snd_hda_intel") == 0 && *nunbind < GS_PLAN_MAX_UNBIND) {
			unbind[(*nunbind)++] = fn->bdf;
			continue;
		}
		return snprintf(why, len, "%s: function %s is bound to '%s'; only the card's HDMI audio (snd_hda_intel) is unbound automatically",
				c->pf.str, fn->bdf.str, fn->driver), -1;
	}
	if (!c->bridge_is_pci)
		return snprintf(why, len, "%s sits on a root bus without a PCI bridge, so the bridge-scoped rescan is not possible",
				c->pf.str), -1;
	if (!c->rescan_present)
		return snprintf(why, len, "%s: bridge %s has no pci_bus/%04x:%02x/rescan", c->pf.str, c->bridge.str,
				c->pf.domain, c->pf.bus), -1;
	if (c->bar0_assigned == 0)
		return snprintf(why, len, "%s: BAR0 is not assigned; a rescan cannot place it", c->pf.str), -1;
	return 0;
}

/* Give unbound audio functions back to snd_hda_intel while they are still present and unbound. */
static void rebind_audio(const struct gs_host *h, const struct gs_addr *bdf, unsigned int n)
{
	char p[GS_PATH_MAX];

	for (unsigned int i = 0; i < n; i++) {
		struct gs_card now;
		int r;

		gs_card_inspect(h->sys, &bdf[i], &now);
		if (!now.present || now.driver[0] || now.vendor != GS_VENDOR_NVIDIA) {
			fprintf(stderr, "geforce-sriov: residual state: audio function %s was unbound and is now %s; it was not rebound\n",
				bdf[i].str, !now.present ? "missing" : now.driver[0] ? "bound elsewhere" : "a different device");
			continue;
		}
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/snd_hda_intel/bind", h->sys))
			return;
		r = sysfs_write(h, p, bdf[i].str, SYSFS_DEADLINE_MS);
		if (r)
			fprintf(stderr, "geforce-sriov: residual state: rebinding audio function %s to snd_hda_intel failed: %s\n",
				bdf[i].str, strerror(-r));
		else
			printf("%s: audio function rebound to snd_hda_intel\n", bdf[i].str);
	}
}

/*
 * NVIDIA's driver waits without limit inside its remove callback while a client holds the GPU, and
 * that wait blocks every later PCI removal and rescan on the host. unbindLock is the driver's own
 * answer to whether the GPU is free: it is granted only while no client holds it, and it keeps new
 * clients out. A PF bound to nvidia is therefore removed only with the lock granted. 0 granted;
 * GS_EXIT_REFUSED with why when it is not (the lock is given back); GS_EXIT_COLD_CYCLE on a timeout.
 */
static int remove_lock(const struct gs_host *h, const struct gs_card *c, int *uncertain, char *why, size_t len)
{
	char lock[GS_PATH_MAX], val[16] = "", holder[200] = "";
	int r;

	if (gs_pathf(lock, sizeof(lock), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, c->pf.str))
		return snprintf(why, len, "the unbindLock path is too long"), GS_EXIT_REFUSED;
	r = sysfs_write(h, lock, "1", SYSFS_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		*uncertain = 1;
		return snprintf(why, len, "asking NVIDIA's driver for unbindLock did not finish within %u ms; nothing was removed",
				SYSFS_DEADLINE_MS), GS_EXIT_COLD_CYCLE;
	}
	if (!r && !gs_read_attr(lock, val, sizeof(val)) && strcmp(val, "1") == 0) {
		printf("%s: unbindLock granted, so no program holds the GPU\n", c->pf.str);
		return 0;
	}
	sysfs_write(h, lock, "0", SYSFS_DEADLINE_MS);
	node_holder(h, 1, holder, sizeof(holder));
	return snprintf(why, len, "NVIDIA's driver did not grant unbindLock (it reads '%s'%s%s), so a program still uses the GPU and it was not removed.%s%s\n"
			"Stop the program that uses it, such as nvidia-smi, nvidia-persistenced, a CUDA program or a desktop session on this GPU, then run activate again",
			val, r ? "; the write failed: " : "", r ? strerror(-r) : "", holder[0] ? " Found: " : "", holder), GS_EXIT_REFUSED;
}

/*
 * One card's empty-bus resize. Every write is bounded; a timeout is uncertain device state and
 * stops here. The rescan is issued once and never retried, and only after the bus was verified
 * empty and the bridge read back on. 0 done, GS_EXIT_* otherwise; *uncertain says exit 5.
 */
static int remove_card(const struct gs_host *h, struct gs_vgpu_card *k, const struct gs_config *cfg, const char *bdfs,
		       int display, int *uncertain, int *touched, char *why, size_t len)
{
	struct gs_card c, after;
	struct gs_addr audio[GS_PLAN_MAX_UNBIND], scratch[GS_PLAN_MAX_UNBIND];
	char p[GS_PATH_MAX], pfdir[GS_PATH_MAX], step[64];
	unsigned int naudio = 0, nscratch = 0, i, vendor = 0, device = 0;
	sigset_t block, saved;
	int r;

	gs_card_inspect(h->sys, &k->pf, &c);
	if (removal_plan(&c, audio, &naudio, why, len))
		return GS_EXIT_REFUSED;
	if (bridge_hold(h, &c, uncertain, why, len))
		return *uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_REFUSED;

	/* Only the selected card's own HDMI audio is unbound, before anything else. */
	for (i = 0; i < naudio; i++) {
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver/unbind", h->sys, audio[i].str)) {
			snprintf(why, len, "the audio function's unbind path is too long");
			rebind_audio(h, audio, i);
			return GS_EXIT_REFUSED;
		}
		r = sysfs_write(h, p, audio[i].str, SYSFS_DEADLINE_MS);
		if (r == -ETIMEDOUT) {
			*uncertain = 1;
			rebind_audio(h, audio, i);
			snprintf(why, len, "unbinding audio function %s did not finish within %u ms; its state is unknown",
				 audio[i].str, SYSFS_DEADLINE_MS);
			return GS_EXIT_COLD_CYCLE;
		}
		if (r) {
			rebind_audio(h, audio, i);
			snprintf(why, len, "unbinding audio function %s failed: %s", audio[i].str, strerror(-r));
			return GS_EXIT_REFUSED;
		}
		printf("%s: unbound audio function %s\n", k->pf.str, audio[i].str);
	}

	/* Fresh look right before removal: nothing bound, nothing shared, no VFs. */
	gs_card_inspect(h->sys, &k->pf, &c);
	if (removal_plan(&c, scratch, &nscratch, why, len) || nscratch) {
		if (!why[0])
			snprintf(why, len, "%s: a card function is still bound", k->pf.str);
		rebind_audio(h, audio, naudio);
		return GS_EXIT_REFUSED;
	}
	vendor = c.vendor;
	device = c.device;
	/*
	 * The console card is the exception: activation has just loaded nvidia_drm on it, and that
	 * module is a client of the GPU, so the lock would be refused because of activation's own
	 * step. Its other users were listed and refused by the display plan in the preflight.
	 *
	 * The guard is updated first. NVIDIA's lock is the last thing taken before the removal, so that
	 * no exit lies between a granted lock and the removal: a lock that is granted and never used
	 * would keep every program off the GPU.
	 */
	snprintf(step, sizeof(step), "remove-rescan:%s", k->pf.str);
	if ((r = write_marker(h, bdfs, step))) {
		snprintf(why, len, "cannot update the activation lock: %s; nothing removed", strerror(-r));
		rebind_audio(h, audio, naudio);
		return GS_EXIT_FAIL;
	}
	if (!display && strcmp(c.driver, "nvidia") == 0 && (r = remove_lock(h, &c, uncertain, why, len))) {
		rebind_audio(h, audio, naudio);
		return r;
	}

	/* A signal must not leave an emptied bus behind. */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGQUIT);
	sigprocmask(SIG_BLOCK, &block, &saved);

	/* From here on the card's BARs may move. */
	*touched = 1;
	for (i = c.nfns; i-- > 0;) {
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/remove", h->sys, c.fns[i].bdf.str)) {
			sigprocmask(SIG_SETMASK, &saved, NULL);
			snprintf(why, len, "the remove path of %s is too long", c.fns[i].bdf.str);
			return GS_EXIT_FAIL;
		}
		r = sysfs_write(h, p, "1", SYSFS_DEADLINE_MS);
		if (r == -ETIMEDOUT) {
			sigprocmask(SIG_SETMASK, &saved, NULL);
			*uncertain = 1;
			return snprintf(why, len, "the remove write for %s did not finish within %u ms; the bus state is uncertain",
					c.fns[i].bdf.str, SYSFS_DEADLINE_MS), GS_EXIT_COLD_CYCLE;
		}
		if (r)
			printf("%s: remove %s failed: %s\n", k->pf.str, c.fns[i].bdf.str, strerror(-r));
		else
			printf("%s: remove %s\n", k->pf.str, c.fns[i].bdf.str);
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", h->sys, c.fns[i].bdf.str);
		if (wait_exists(p, 0, REMOVE_WAIT_MS)) {
			sigprocmask(SIG_SETMASK, &saved, NULL);
			*uncertain = 1;
			return snprintf(why, len, "%s is still listed %u ms after its remove; do not rescan",
					c.fns[i].bdf.str, REMOVE_WAIT_MS), GS_EXIT_COLD_CYCLE;
		}
	}

	if (!gs_pathf(pfdir, sizeof(pfdir), "%s/bus/pci/devices/%s", h->sys, k->pf.str)) {
		gs_pathf(p, sizeof(p), "%s/remove", pfdir);
		r = sysfs_write(h, p, "1", SYSFS_DEADLINE_MS);
		if (r == -ETIMEDOUT) {
			sigprocmask(SIG_SETMASK, &saved, NULL);
			*uncertain = 1;
			return snprintf(why, len, "the remove write for %s did not finish within %u ms; the bus state is uncertain",
					k->pf.str, SYSFS_DEADLINE_MS), GS_EXIT_COLD_CYCLE;
		}
		if (r)
			printf("%s: remove failed: %s\n", k->pf.str, strerror(-r));
		else
			printf("%s: remove %s\n", k->pf.str, k->pf.str);
		if (wait_exists(pfdir, 0, REMOVE_WAIT_MS)) {
			sigprocmask(SIG_SETMASK, &saved, NULL);
			*uncertain = 1;
			return snprintf(why, len, "%s is still listed %u ms after its remove; do not rescan", k->pf.str,
					REMOVE_WAIT_MS), GS_EXIT_COLD_CYCLE;
		}
	}

	/* The bus must be actually empty before the one rescan. */
	r = bus_not_empty(h, &k->pf, why, len);
	if (r) {
		sigprocmask(SIG_SETMASK, &saved, NULL);
		*uncertain = 1;
		if (r < 0)
			return GS_EXIT_COLD_CYCLE;
		return GS_EXIT_COLD_CYCLE;
	}
	if (bridge_hold(h, &c, uncertain, why, len)) {
		sigprocmask(SIG_SETMASK, &saved, NULL);
		return *uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
	}

	printf("%s: rescan %s\n", k->pf.str, c.rescan_path);
	r = sysfs_write(h, c.rescan_path, "1", SYSFS_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		sigprocmask(SIG_SETMASK, &saved, NULL);
		*uncertain = 1;
		return snprintf(why, len, "the rescan of bridge %s did not finish within %u ms; it is not retried",
				c.bridge.str, SYSFS_DEADLINE_MS), GS_EXIT_COLD_CYCLE;
	}
	if (r)
		printf("%s: rescan exited %d; judging only the device state\n", k->pf.str, r);

	if (wait_exists(pfdir, 1, RETURN_WAIT_MS)) {
		sigprocmask(SIG_SETMASK, &saved, NULL);
		*uncertain = 1;
		return snprintf(why, len, "%s did not come back after the rescan (bridge %s power/control stays on); %s",
				k->pf.str, c.bridge.str, gs_recovery_msg), GS_EXIT_COLD_CYCLE;
	}
	gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", pfdir);
	wait_exists(p, 1, SRIOV_ATTR_WAIT_MS);
	for (i = 0; i < c.nfns; i++) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", h->sys, c.fns[i].bdf.str);
		if (wait_exists(p, 1, FN_RETURN_WAIT_MS))
			printf("%s: warning: function %s did not come back\n", k->pf.str, c.fns[i].bdf.str);
	}
	sigprocmask(SIG_SETMASK, &saved, NULL);

	gs_card_inspect(h->sys, &k->pf, &after);
	if (after.vendor != vendor || after.device != device) {
		*uncertain = 1;
		return snprintf(why, len, "%s came back as %04x:%04x, not %04x:%04x; %s", k->pf.str, after.vendor,
				after.device, vendor, device, gs_recovery_msg), GS_EXIT_COLD_CYCLE;
	}
	if (after.entry && after.entry->laptop) {
		gs_pathf(p, sizeof(p), "%s/d3cold_allowed", pfdir);
		r = sysfs_write(h, p, "0", SYSFS_DEADLINE_MS);
		if (r == -ETIMEDOUT) {
			*uncertain = 1;
			return snprintf(why, len, "%s: writing 0 to the d3cold_allowed of the rescanned PF did not finish within %u ms; whether the GPU can still power off is unknown",
					k->pf.str, SYSFS_DEADLINE_MS), GS_EXIT_COLD_CYCLE;
		}
		if (r) {
			return snprintf(why, len, "%s: writing 0 to the d3cold_allowed of the rescanned PF failed; a laptop GPU that can power off must not get VFs",
					k->pf.str), GS_EXIT_FAIL;
		}
		printf("%s: D3cold disabled (d3cold_allowed=0)\n", k->pf.str);
	}
	if (after.bar0_assigned == 0)
		return snprintf(why, len, "%s came back without BAR0: the card is unusable this boot. Do not rescan again; %s",
				k->pf.str, BAR_REMEDY), GS_EXIT_FAIL;
	if (!after.has_sriov)
		return snprintf(why, len, "%s came back without sriov_totalvfs, so SR-IOV is not presented; see `journalctl -k -b`",
				k->pf.str), GS_EXIT_FAIL;
	if (gs_check_presented(&after, cfg, why, len))
		return GS_EXIT_FAIL;
	/*
	 * MSI-X after the rescan: a hidden capability stops the transaction before any VF is
	 * enabled. It is not retried, and the livepatch is not reloaded.
	 */
	if (cfg->msix != GS_SW_OFF && after.msix_linked == 0)
		return snprintf(why, len, "%s came back with MSI-X hidden after the rescan; no VF is enabled and the rescan is not retried. Power the machine off and on (the livepatch reveals MSI-X only at load)",
				k->pf.str), GS_EXIT_FAIL;
	if (after.msix_linked < 0)
		printf("%s: warning: its MSI-X state is unreadable; the VFs are enabled anyway\n", k->pf.str);
	if (after.vf_bars_assigned == 0)
		return snprintf(why, len, "%s: no VF BAR was assigned after the rescan, so enabling VFs would fail with \"not enough MMIO resources for SR-IOV\"; %s",
				k->pf.str, BAR_REMEDY), GS_EXIT_FAIL;
	printf("%s: sriov_totalvfs=%u ari_enabled=%d msix_linked=%d, VF BARs assigned\n", k->pf.str, after.totalvfs,
	       after.ari_enabled, after.msix_linked);
	if (cfg->ari != GS_SW_OFF && after.ari_enabled != 1)
		printf("%s: ARI is not enabled: the livepatch module geforce_sriov presents ARI only when the card's hidden ARI capability names a next\n"
		       "function and the bridge supports ARI forwarding (kernel log: \"ARI stays off for this card\")\n",
		       k->pf.str);
	k->enable = 1;
	return GS_EXIT_OK;
}

/* ---- VF enable, type and power ------------------------------------------- */

static int sriov_manage_safe(const char *path, char *why, size_t len)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return snprintf(why, len, "%.300s: %s (install NVIDIA's vGPU host driver)", path, strerror(errno)), -1;
	if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
		return snprintf(why, len, "%.300s is not an executable file", path), -1;
	if (st.st_uid != 0 || (st.st_mode & 022))
		return snprintf(why, len, "%.300s is not root-owned or is group/world-writable", path), -1;
	return 0;
}

/*
 * sriov-manage is a root-owned executable, and lspci and setpci are there for it. This is the real
 * system's gs_host.enable_prereq: it vets the programs gs_run will execute as root.
 */
int gs_enable_prereq(const struct gs_config *cfg, char *why, size_t len)
{
	static const char *const deps[] = { "lspci", "setpci" };
	char p[GS_PATH_MAX];

	if (sriov_manage_safe(cfg->sriov_manage, why, len))
		return -1;
	for (size_t k = 0; k < sizeof(deps) / sizeof(deps[0]); k++)
		if (gs_find_program(deps[k], p, sizeof(p)))
			return snprintf(why, len, "%.200s needs %s (pciutils), not found in /usr/sbin:/usr/bin:/sbin:/bin",
					cfg->sriov_manage, deps[k]), -1;
	return 0;
}

static int vfs_bound(const char *sys, const struct gs_card *c, unsigned int want, char *bad, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	unsigned int i;

	for (i = 0; i < want; i++) {
		struct gs_addr a;
		struct gs_card vf;
		ssize_t n;
		const char *base;

		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sys, c->pf.str, i);
		n = readlink(p, target, sizeof(target) - 1);
		if (n <= 0)
			return snprintf(bad, len, "virtfn%u missing", i), -1;
		target[n] = '\0';
		base = strrchr(target, '/');
		if (gs_addr_parse(base ? base + 1 : target, &a))
			return snprintf(bad, len, "virtfn%u unparsable", i), -1;
		gs_card_inspect(sys, &a, &vf);
		if (strcmp(vf.driver, "nvidia") != 0)
			return snprintf(bad, len, "VF %s bound to %s", a.str, vf.driver[0] ? vf.driver : "nothing"), -1;
	}
	return 0;
}

#define SRIOV_MANAGE_NOTE \
	"note: sriov-manage briefly stops nvidia-xid-logd, moves each GPU to pci-pf-stub to set sriov_numvfs,\n" \
	"binds it and the other functions on its bus to nvidia and runs `nvidia-smi -pm 1`; the result is\n" \
	"judged from sysfs, not from its exit status\n"

/* sriov-manage -e once on a PF, judged by state alone. 0, GS_EXIT_REFUSED or GS_EXIT_FAIL. */
static int enable_pf(const struct gs_host *h, const struct gs_config *cfg, const struct gs_addr *pf,
		     struct gs_card *c, int *uncertain, char *why, size_t len)
{
	const char *argv[] = { cfg->sriov_manage, "-e", pf->str, NULL };
	char bad[256];
	int r;

	if (strcmp(c->driver, "nvidia") != 0)
		return snprintf(why, len, "%s is bound to %s, not nvidia (load NVIDIA's vGPU host driver first)", pf->str,
				c->driver[0] ? c->driver : "nothing"), GS_EXIT_REFUSED;
	printf("%s -e %s\n", cfg->sriov_manage, pf->str);
	r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		*uncertain = 1;
		return snprintf(why, len, "%.300s -e %s did not finish within %u ms; whether VFs were enabled is unknown",
				cfg->sriov_manage, pf->str, GS_MODULE_DEADLINE_MS), GS_EXIT_COLD_CYCLE;
	}
	if (r != 0)
		printf("%s exited with %d; judging only the device state\n", cfg->sriov_manage, r);
	for (unsigned int t = 0;; t += POLL_MS) {
		gs_card_inspect(h->sys, pf, c);
		if (c->numvfs == c->totalvfs && c->numvfs >= 1 && c->numvfs <= gs_config_vfs(cfg) &&
		    vfs_bound(h->sys, c, c->numvfs, bad, sizeof(bad)) == 0 && strcmp(c->driver, "nvidia") == 0)
			break;
		if (t >= VF_BIND_WAIT_MS) {
			if (c->numvfs == 0)
				snprintf(bad, sizeof(bad), "no VF was enabled");
			else if (c->numvfs != c->totalvfs || c->numvfs > gs_config_vfs(cfg))
				snprintf(bad, sizeof(bad), "VF count is not the presented 1..%u", gs_config_vfs(cfg));
			else if (strcmp(c->driver, "nvidia") != 0)
				snprintf(bad, sizeof(bad), "PF bound to %s%s", c->driver[0] ? c->driver : "nothing",
					 strcmp(c->driver, "pci-pf-stub") == 0 ?
					 " (sriov-manage stopped mid-way; its driver_override may still name pci-pf-stub)" : "");
			else
				vfs_bound(h->sys, c, c->numvfs, bad, sizeof(bad));
			return snprintf(why, len, "%s: enable did not complete: sriov_numvfs=%u of %u; %s", pf->str,
					c->numvfs, c->totalvfs, bad), GS_EXIT_FAIL;
		}
		gs_sleep_ms(POLL_MS);
	}
	printf("%s: %u VFs enabled and bound to nvidia\n", pf->str, c->numvfs);
	return GS_EXIT_OK;
}

/* The card's VFs, all of them, each on nvidia with a power/control: k->vf and k->nvf set. */
static int vfs_ready(const struct gs_host *h, struct gs_vgpu_card *k, const struct gs_card *c, char *why, size_t len)
{
	char p[GS_PATH_MAX], drv[64];
	unsigned int j;

	if (c->numvfs < 1 || c->numvfs != c->totalvfs)
		return snprintf(why, len, "%s has %u of %u VFs enabled, an unexpected state", k->pf.str, c->numvfs,
				c->totalvfs), -1;
	if (gs_vf_list(h->sys, &k->pf, c->numvfs, k->vf, why, len))
		return -1;
	k->nvf = c->numvfs;
	for (j = 0; j < k->nvf; j++) {
		gs_dev_driver(h->sys, k->vf[j].str, drv, sizeof(drv));
		if (strcmp(drv, "nvidia") != 0)
			return snprintf(why, len, "VF %s of %s is bound to %s, not nvidia", k->vf[j].str, k->pf.str,
					drv[0] ? drv : "nothing"), -1;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/power/control", h->sys, k->vf[j].str) || !gs_exists(p))
			return snprintf(why, len, "VF %s has no power/control", k->vf[j].str), -1;
	}
	return 0;
}

/* 1 when the VF's nvidia/creatable_vgpu_types lists type (lines like "942 : NVIDIA RTX6000-Ada-2Q"). */
static int type_creatable(const char *sys, const char *vf, unsigned int type)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t dlen;
	int found = 0;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/creatable_vgpu_types", sys, vf) ||
	    gs_read_whole(p, &data, &dlen, 1 << 16))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
		unsigned long v;
		char *end;

		while (*line == ' ' || *line == '\t')
			line++;
		if (*line < '0' || *line > '9')
			continue;
		v = strtoul(line, &end, 10);
		while (*end == ' ' || *end == '\t')
			end++;
		found = v == type && *end == ':';
	}
	free(data);
	return found;
}

/* The card's vGPU type on one VF: current_vgpu_type must be 0 or type, and type listed as creatable. */
static int set_type_vf(const struct gs_host *h, const struct gs_addr *vf, unsigned int type, unsigned int wait)
{
	char p[GS_PATH_MAX], cur[32], want[16];
	int r;

	snprintf(want, sizeof(want), "%u", type);
	if ((r = gs_read_dev_attr(h->sys, vf->str, "nvidia/current_vgpu_type", cur, sizeof(cur)))) {
		fprintf(stderr, "geforce-sriov: %s: nvidia/current_vgpu_type is unreadable: %s\n", vf->str, strerror(-r));
		return GS_EXIT_FAIL;
	}
	if (strcmp(cur, want) == 0) {
		printf("%s: vGPU type already %s\n", vf->str, want);
		return 0;
	}
	if (strcmp(cur, "0") != 0) {
		fprintf(stderr, "geforce-sriov: %s has vGPU type %s, not 0 or %s; nothing written\n", vf->str, cur, want);
		return GS_EXIT_FAIL;
	}
	for (unsigned int t = 0; !type_creatable(h->sys, vf->str, type); t += POLL_MS) {
		if (t >= wait) {
			fprintf(stderr, "geforce-sriov: %s: type %s is not in nvidia/creatable_vgpu_types (`cat` it for the types this GPU\n"
				"offers); nothing written. The driver keeps the vGPU types it registered first after a boot, so after a\n"
				"change of identity or professional counterpart, power the machine off and on\n", vf->str, want);
			return GS_EXIT_FAIL;
		}
		gs_sleep_ms(POLL_MS);
	}
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/current_vgpu_type", h->sys, vf->str);
	printf("%s: echo %s > nvidia/current_vgpu_type\n", vf->str, want);
	if ((r = sysfs_write(h, p, want, SYSFS_DEADLINE_MS)) == -ETIMEDOUT) {
		fprintf(stderr, "geforce-sriov: %s: writing the vGPU type did not finish within %u ms; whether it is set is unknown\n",
			vf->str, SYSFS_DEADLINE_MS);
		return GS_EXIT_COLD_CYCLE;
	}
	if (r) {
		fprintf(stderr, "geforce-sriov: %s: writing the vGPU type failed: %s\n", vf->str, strerror(-r));
		return GS_EXIT_FAIL;
	}
	if (gs_read_dev_attr(h->sys, vf->str, "nvidia/current_vgpu_type", cur, sizeof(cur)) || strcmp(cur, want) != 0) {
		fprintf(stderr, "geforce-sriov: %s: current_vgpu_type reads '%s' after writing %s\n", vf->str, cur, want);
		return GS_EXIT_FAIL;
	}
	return 0;
}

/* power/control=on on one VF, then the gate: on and active read back after the write. */
static int power_on_vf(const struct gs_host *h, const struct gs_addr *vf)
{
	char ctl[GS_PATH_MAX], st[GS_PATH_MAX], c[32] = "", s[32] = "";
	int r;

	if (gs_pathf(ctl, sizeof(ctl), "%s/bus/pci/devices/%s/power/control", h->sys, vf->str) ||
	    gs_pathf(st, sizeof(st), "%s/bus/pci/devices/%s/power/runtime_status", h->sys, vf->str))
		return GS_EXIT_FAIL;
	if (gs_read_attr(ctl, c, sizeof(c)) == 0 && strcmp(c, "on") == 0) {
		printf("%s: power/control already on\n", vf->str);
	} else if ((r = sysfs_write(h, ctl, "on", SYSFS_DEADLINE_MS)) == -ETIMEDOUT) {
		fprintf(stderr, "geforce-sriov: %s: writing on to power/control did not finish within %u ms; whether it is on is unknown.\n"
			"Do not start a VM on it\n", vf->str, SYSFS_DEADLINE_MS);
		return GS_EXIT_COLD_CYCLE;
	} else if (r) {
		fprintf(stderr, "geforce-sriov: %s: writing on to power/control failed: %s; do not start a VM on it\n", vf->str,
			strerror(-r));
		return GS_EXIT_FAIL;
	} else {
		printf("%s: echo on > power/control\n", vf->str);
	}
	wait_attr_eq(st, "active", VF_ACTIVE_WAIT_MS);
	c[0] = s[0] = '\0';
	gs_read_attr(ctl, c, sizeof(c));
	gs_read_attr(st, s, sizeof(s));
	printf("%s: gate: power/control=%s runtime_status=%s\n", vf->str, c, s);
	if (strcmp(c, "on") != 0 || strcmp(s, "active") != 0) {
		fprintf(stderr, "geforce-sriov: %s is not on/active after the write; do not start a VM on it\n", vf->str);
		return GS_EXIT_FAIL;
	}
	return 0;
}

/*
 * After the last re-probe: the same VFs again, then the card's type and power. 0; GS_EXIT_FAIL; or
 * GS_EXIT_COLD_CYCLE when a write did not finish, after which no further VF is written.
 */
static int finish_card(const struct gs_host *h, const struct gs_vgpu_card *k, unsigned int wait)
{
	struct gs_addr now[GS_VFS_MAX];
	char why[512], p[GS_PATH_MAX], drv[64];
	struct gs_card c;
	unsigned int j;
	int r;

	gs_card_inspect(h->sys, &k->pf, &c);
	if (c.numvfs != k->nvf || gs_vf_list(h->sys, &k->pf, c.numvfs, now, why, sizeof(why))) {
		fprintf(stderr, "geforce-sriov: %s: after the re-probe it has %u VFs, not the %u it had%s%s\n", k->pf.str,
			c.numvfs, k->nvf, c.numvfs == k->nvf ? ": " : "", c.numvfs == k->nvf ? why : "");
		return GS_EXIT_FAIL;
	}
	for (j = 0; j < k->nvf; j++) {
		if (strcmp(now[j].str, k->vf[j].str) != 0) {
			fprintf(stderr, "geforce-sriov: %s: virtfn%u is %s after the re-probe, not %s\n", k->pf.str, j,
				now[j].str, k->vf[j].str);
			return GS_EXIT_FAIL;
		}
	}
	for (j = 0; j < k->nvf; j++) {
		const struct gs_addr *vf = &k->vf[j];

		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/creatable_vgpu_types", h->sys, vf->str);
		for (unsigned int t = 0;; t += POLL_MS) {
			gs_dev_driver(h->sys, vf->str, drv, sizeof(drv));
			if (strcmp(drv, "nvidia") == 0 && gs_exists(p))
				break;
			if (t >= wait) {
				fprintf(stderr, "geforce-sriov: %s: no nvidia/creatable_vgpu_types %u ms after the re-probe (bound to %s);\n"
					"nvidia-vgpu-mgr publishes it (`journalctl -u nvidia-vgpu-mgr -b`). Do not start a VM on it\n",
					vf->str, wait, drv[0] ? drv : "nothing");
				return GS_EXIT_FAIL;
			}
			gs_sleep_ms(POLL_MS);
		}
		if ((r = set_type_vf(h, vf, k->type, wait)) || (r = power_on_vf(h, vf)))
			return r;
	}
	return 0;
}

/* ---- PF re-probe ---------------------------------------------------------- */

/* A laptop PF's power/control and d3cold_allowed, before and after the unbind. */
static int laptop_hold(const struct gs_host *h, const struct gs_card *c)
{
	static const char *const attrs[][2] = { { "power/control", "on" }, { "d3cold_allowed", "0" } };
	char p[GS_PATH_MAX], v[16];
	int r;

	if (!c->entry || !c->entry->laptop)
		return 0;
	if (c->bridge_dir[0]) {
		if (gs_pathf(p, sizeof(p), "%s/power/control", c->bridge_dir))
			return GS_EXIT_FAIL;
		if ((gs_read_attr(p, v, sizeof(v)) || strcmp(v, "on") != 0) && (r = sysfs_write(h, p, "on", SYSFS_DEADLINE_MS))) {
			fprintf(stderr, "geforce-sriov: %s: writing on to its bridge's power/control failed: %s\n", c->pf.str,
				strerror(-r));
			return write_exit(r);
		}
	}
	for (size_t k = 0; k < sizeof(attrs) / sizeof(attrs[0]); k++) {
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/%s", h->sys, c->pf.str, attrs[k][0]))
			return GS_EXIT_FAIL;
		if (!gs_read_attr(p, v, sizeof(v)) && strcmp(v, attrs[k][1]) == 0)
			continue;
		if ((r = sysfs_write(h, p, attrs[k][1], SYSFS_DEADLINE_MS))) {
			fprintf(stderr, "geforce-sriov: %s: writing %s to %s failed: %s\n", c->pf.str, attrs[k][1],
				attrs[k][0], strerror(-r));
			return write_exit(r);
		}
		printf("%s: %s -> %s (laptop GPU)\n", c->pf.str, attrs[k][0], attrs[k][1]);
	}
	return 0;
}

/*
 * The PF re-probe (driver unbind, then bind; never remove or rescan), behind the guard and
 * NVIDIA's unbindLock, which the driver grants only while no client holds the GPU.
 * 0; GS_EXIT_REFUSED (lock not granted); GS_EXIT_FAIL (still bound); GS_EXIT_COLD_CYCLE.
 */
static int reprobe_pf_unmasked(const struct gs_host *h, const struct gs_card *c)
{
	char lock[GS_PATH_MAX], unbind_path[GS_PATH_MAX], bind_path[GS_PATH_MAX], drv[GS_PATH_MAX], val[16] = "", now[64];
	int r;

	if (gs_pathf(lock, sizeof(lock), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, c->pf.str) ||
	    gs_pathf(unbind_path, sizeof(unbind_path), "%s/bus/pci/drivers/nvidia/unbind", h->sys) ||
	    gs_pathf(bind_path, sizeof(bind_path), "%s/bus/pci/drivers/nvidia/bind", h->sys) ||
	    gs_pathf(drv, sizeof(drv), "%s/bus/pci/devices/%s/driver", h->sys, c->pf.str))
		return GS_EXIT_FAIL;
	if ((r = laptop_hold(h, c)))
		return r;
	if ((r = write_marker(h, c->pf.str, "reprobe"))) {
		fprintf(stderr, "geforce-sriov: cannot write the activation lock: %s; %s was not re-probed\n", strerror(-r), c->pf.str);
		return GS_EXIT_FAIL;
	}
	r = sysfs_write(h, lock, "1", SYSFS_DEADLINE_MS);
	if (r || gs_read_attr(lock, val, sizeof(val)) || strcmp(val, "1") != 0) {
		sysfs_write(h, lock, "0", SYSFS_DEADLINE_MS);
		fprintf(stderr, "geforce-sriov: %s: NVIDIA's driver did not grant unbindLock (it reads '%s'%s%s), so it was not unbound.\n"
			"A client still holds the GPU, such as nvidia-smi, nvidia-persistenced or a CUDA program; stop it, then run activate again\n",
			c->pf.str, val, r ? "; the write failed: " : "", r ? strerror(-r) : "");
		return r == -ETIMEDOUT ? GS_EXIT_COLD_CYCLE : GS_EXIT_REFUSED;
	}
	printf("%s: unbindLock granted\necho %s > /sys/bus/pci/drivers/nvidia/unbind\n", c->pf.str, c->pf.str);
	r = sysfs_write(h, unbind_path, c->pf.str, SYSFS_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		sysfs_write(h, lock, "0", SYSFS_DEADLINE_MS);
		fprintf(stderr, "geforce-sriov: %s: the unbind did not finish within %u ms; its state is uncertain.\n%s\n",
			c->pf.str, SYSFS_DEADLINE_MS, gs_recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	if (wait_exists(drv, 0, PF_REBIND_WAIT_MS)) {
		sysfs_write(h, lock, "0", SYSFS_DEADLINE_MS);
		fprintf(stderr, "geforce-sriov: %s is still bound after the unbind%s%s; unbindLock released, nothing else changed\n",
			c->pf.str, r ? ": " : "", r ? strerror(-r) : "");
		return GS_EXIT_FAIL;
	}
	printf("echo %s > /sys/bus/pci/drivers/nvidia/bind\n", c->pf.str);
	r = sysfs_write(h, bind_path, c->pf.str, SYSFS_DEADLINE_MS);
	/*
	 * A bind that did not finish is uncertain even when the driver link is already there: the
	 * driver may still be probing the GPU.
	 */
	if (r == -ETIMEDOUT) {
		fprintf(stderr, "geforce-sriov: %s: the bind did not finish within %u ms; its state is uncertain.\n%s\n",
			c->pf.str, SYSFS_DEADLINE_MS, gs_recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	for (unsigned int t = 0;; t += POLL_MS) {
		gs_dev_driver(h->sys, c->pf.str, now, sizeof(now));
		if (strcmp(now, "nvidia") == 0)
			break;
		if (t >= PF_REBIND_WAIT_MS) {
			fprintf(stderr, "geforce-sriov: %s is bound to %s, not nvidia, after the re-probe%s%s. The activation lock stays set.\n%s\n",
				c->pf.str, now[0] ? now : "nothing", r ? ": " : "", r ? strerror(-r) : "", gs_recovery_msg);
			return GS_EXIT_COLD_CYCLE;
		}
		gs_sleep_ms(POLL_MS);
	}
	if ((r = laptop_hold(h, c)))
		return r;
	printf("%s: re-probed, bound to nvidia again\n", c->pf.str);
	return GS_EXIT_OK;
}

/* The re-probe with SIGINT/SIGTERM/SIGHUP/SIGQUIT held back, so a unit stop never lands between unbind and bind. */
static int reprobe_pf(const struct gs_host *h, const struct gs_card *c)
{
	sigset_t block, saved;
	int r;

	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGQUIT);
	sigprocmask(SIG_BLOCK, &block, &saved);
	r = reprobe_pf_unmasked(h, c);
	sigprocmask(SIG_SETMASK, &saved, NULL);
	return r;
}

/* ---- bounded waits -------------------------------------------------------- */

/* Wait for path to appear (want 1) or disappear: 0 reached, -1 after ms. */
static int wait_exists(const char *path, int want, unsigned int ms)
{
	for (unsigned int t = 0;; t += POLL_MS) {
		if (gs_exists(path) == want)
			return 0;
		if (t >= ms)
			return -1;
		gs_sleep_ms(POLL_MS);
	}
}

/* Wait for path to read want: 0 reached, -1 after ms. */
static int wait_attr_eq(const char *path, const char *want, unsigned int ms)
{
	char buf[64];

	for (unsigned int t = 0;; t += POLL_MS) {
		if (gs_read_attr(path, buf, sizeof(buf)) == 0 && strcmp(buf, want) == 0)
			return 0;
		if (t >= ms)
			return -1;
		gs_sleep_ms(POLL_MS);
	}
}

/* ---- display modules and host-wide VF presence ---------------------------- */

/* 1 when any NVIDIA PF on the host has VFs enabled; 0 none; -1 unreadable. */
static int any_vfs(const struct gs_host *h, char *which, size_t len);

/*
 * A block in /run/modprobe.d stops loads of nvidia_drm and nvidia_uvm by name (modprobe,
 * nvidia-modprobe) during the VF enable window: a module that holds a PF loses NVIDIA's
 * unbindLock. udev's alias loads are a separate path and are stopped by the alias overrides that
 * install writes. The file is rewritten inert once the display stack is loaded. /run is cleared
 * at reboot; a block left by a killed run keeps nvidia_uvm from loading until then.
 */
static int enable_block_path(const struct gs_host *h, char *buf, size_t len)
{
	return gs_pathf(buf, len, "%s/../modprobe.d/zz-geforce-sriov-enable.conf", h->run_dir);
}

static int enable_block_set(const struct gs_host *h, const char *content, char *why, size_t len)
{
	char p[GS_PATH_MAX], dir[GS_PATH_MAX];
	int r;

	if (enable_block_path(h, p, sizeof(p)) || gs_pathf(dir, sizeof(dir), "%s/../modprobe.d", h->run_dir))
		return snprintf(why, len, "the module block path is too long"), -1;
	/*
	 * A new file in a directory that may not exist yet. The host's attribute writer only writes
	 * existing attributes, so this is a plain file write, on the live host and on a fixture alike.
	 */
	if (gs_mkdir_p(dir, 0755))
		return snprintf(why, len, "cannot create %.200s: %s", dir, strerror(errno)), -1;
	if ((r = gs_write_file_atomic(p, content, strlen(content), 0644)))
		return snprintf(why, len, "writing %.200s failed: %s", p, strerror(-r)), -1;
	return 0;
}

/*
 * The live value of one nvidia_drm module parameter; 0 when read, -1 otherwise.
 */
static int drm_param(const struct gs_host *h, const char *name, char *out, size_t n)
{
	char p[GS_PATH_MAX];

	out[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/module/nvidia_drm/parameters/%s", h->sys, name))
		return -1;
	return gs_read_attr(p, out, n);
}

/* 1 when nvidia_drm carries modeset, fbdev and exactly this gpu_allow; what was read goes to pm, pb and pa. */
static int drm_configured(const struct gs_host *h, const char *allow, char pm[16], char pb[16], char pa[256])
{
	int unread = drm_param(h, "modeset", pm, 16) | drm_param(h, "fbdev", pb, 16) | drm_param(h, "gpu_allow", pa, 256);

	return !unread && gs_param_bool(pm) && gs_param_bool(pb) && !strcmp(pa, allow);
}

/*
 * The display stack for the display-selected cards: nvidia_modeset, then nvidia_drm with
 * modeset=1 fbdev=1 and gpu_allow naming only those cards. 0 or -1 with why.
 */
static int display_load(const struct gs_host *h, const struct gs_config *cfg, const struct gs_vgpu_card *cards,
			unsigned int n, char *why, size_t len)
{
	char allow[256], gopt[300], drm_devs[16] = "";
	int any = 0, r;

	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;

		gs_card_inspect(h->sys, &cards[i].pf, &c);
		any |= gs_display_kms(cfg, &c);
	}
	if (!any)
		return 0;
	/*
	 * An empty gpu_allow means every GPU to the module, which would put KMS on a card that is
	 * serving VFs. Never reach modprobe with it: no boot display GPU in the selection means
	 * there is no display stack to load.
	 */
	display_allowlist(h, cfg, cards, n, allow, sizeof(allow));
	if (!allow[0]) {
		printf("display: no selected GPU is the boot display; no display stack is loaded\n");
		return 0;
	}
	if (!gs_module_loaded(h, "nvidia_modeset")) {
		if ((r = gs_module_load(h, 0, "nvidia_modeset", NULL, 0, why, len)))
			return r;
	}
	printf("display allowlist gpu_allow=%s\n", allow);
	snprintf(gopt, sizeof(gopt), "gpu_allow=%s", allow);
	{
		const char *opts[] = { "modeset=1", "fbdev=1", gopt };
		char pm[16] = "", pb[16] = "", pa[256] = "";
		int loaded = gs_module_loaded(h, "nvidia_drm");
		int correct = loaded && drm_configured(h, allow, pm, pb, pa);

		/*
		 * udev autoloads nvidia_drm without parameters as soon as the PF is back on the bus, so a
		 * plain "already loaded" would accept modeset=N and lose the console. A stack that
		 * already carries the right options is kept; anything else is reloaded only while no VF
		 * exists, because unloading the display stack under a live vGPU is not allowed.
		 */
		if (correct) {
			printf("nvidia_drm is loaded with modeset=1 fbdev=1 gpu_allow=%s\n", allow);
		} else {
			if (loaded) {
				char which[256] = "";

				if (any_vfs(h, which, sizeof(which)) != 0) {
					snprintf(why, len,
						 "nvidia_drm is loaded with modeset=%s fbdev=%s gpu_allow=%s, not modeset=1 fbdev=1 gpu_allow=%s, and %s; it is not unloaded while VFs exist, so power the machine off and on",
						 pm, pb, pa, allow, which[0] ? which : "a VF state cannot be read");
					return -1;
				}
				if ((r = gs_module_unload(h, "nvidia_drm", why, len)))
					return r;
			}
			for (unsigned int attempt = 0;; attempt++) {
				if ((r = gs_module_load(h, 1, "nvidia_drm", opts, 3, why, len)))
					return r;
				if (drm_configured(h, allow, pm, pb, pa))
					break;
				if (attempt == 1 || any_vfs(h, NULL, 0) != 0) {
					snprintf(why, len,
						 "nvidia_drm is loaded without modeset=1, fbdev=1 and gpu_allow=%s (a plain autoload); the console cannot come back",
						 allow);
					return -1;
				}
				printf("nvidia_drm loaded without its options (udev autoload); reloading it\n");
				if ((r = gs_module_unload(h, "nvidia_drm", why, len)))
					return r;
			}
		}
	}
	/* The expected DRM devices: at least one card of a display-selected PF. */
	{
		char p[GS_PATH_MAX];
		struct dirent *de;
		DIR *d;

		if (gs_pathf(p, sizeof(p), "%s/class/drm", h->sys) || !(d = opendir(p))) {
			snprintf(why, len, "cannot list %s/class/drm after loading nvidia_drm", h->sys);
			return -1;
		}
		while ((de = readdir(d))) {
			struct gs_addr a;

			if (strncmp(de->d_name, "card", 4) != 0 || !isdigit((unsigned char)de->d_name[4]))
				continue;
			if (class_device_bdf(h, "drm", de->d_name, &a) == 0) {
				for (unsigned int i = 0; i < n; i++) {
					struct gs_card c;

					gs_card_inspect(h->sys, &cards[i].pf, &c);
					if (gs_display_kms(cfg, &c) && strcmp(cards[i].pf.str, a.str) == 0) {
						if (!drm_devs[0])
							gs_copy(drm_devs, sizeof(drm_devs), de->d_name);
						any = 1;
					}
				}
			}
		}
		closedir(d);
		if (!drm_devs[0]) {
			snprintf(why, len, "nvidia_drm loaded but registered no DRM device for a display-selected GPU; the gpu_allow value may not name its GPU ID (`dmesg | grep nvidia-drm`)");
			return -1;
		}
		printf("nvidia_drm: %s registered\n", drm_devs);
	}
	return 0;
}

/* 1 when any NVIDIA PF on the host has VFs enabled; 0 none; -1 unreadable. */
static int any_vfs(const struct gs_host *h, char *which, size_t len)
{
	char p[GS_PATH_MAX], nv[32];
	struct dirent *de;
	unsigned int vendor;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", h->sys) || !(d = opendir(p)))
		return snprintf(which, len, "%s/bus/pci/devices is unreadable", h->sys), -1;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", h->sys, de->d_name) || gs_exists(p))
			continue; /* a VF itself */
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", h->sys, de->d_name) || gs_read_hex(p, &vendor) ||
		    vendor != 0x10de)
			continue;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/sriov_numvfs", h->sys, de->d_name) ||
		    gs_read_attr(p, nv, sizeof(nv)))
			continue;
		if (nv[0] && strcmp(nv, "0") != 0) {
			snprintf(which, len, "%s has sriov_numvfs=%s", de->d_name, nv);
			closedir(d);
			return 1;
		}
	}
	closedir(d);
	return 0;
}

/* NVIDIA's driver loaded with NVreg_EnableGpuFirmware=0 (GSP off): 0, or -1 with why. */
static int check_gsp_off(const char *procroot, char *why, size_t len)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	const char *v;
	size_t dlen;
	int r = -1;

	if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/params", procroot) || gs_read_whole(p, &data, &dlen, 1 << 20))
		return snprintf(why, len, "%.300s is unreadable: NVIDIA's driver is not loaded", p), -1;
	snprintf(why, len, "%.300s has no EnableGpuFirmware line; Ada and Ampere need NVreg_EnableGpuFirmware=0", p);
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "EnableGpuFirmware:", 18) != 0)
			continue;
		for (v = line + 18; *v == ' ' || *v == '\t'; v++)
			;
		if (strcmp(v, "0") == 0)
			r = 0;
		else
			snprintf(why, len, "NVIDIA's driver is loaded with EnableGpuFirmware: %.32s; Ada and Ampere need NVreg_EnableGpuFirmware=0 (GSP off, CPU-RM)",
				 v);
		break;
	}
	free(data);
	return r;
}

/*
 * The plugin nvidia-vgpu-mgr loads must be the patched plugin copy: 0, or -1 with why. The path
 * is documented as a link to the versioned copy, so it is hashed through the link.
 */
static int check_plugin(const char *path, char *why, size_t len)
{
	char sha[65], real[GS_PATH_MAX];
	int r = gs_sha256_file_resolved(path, sha, real);

	if (r)
		return snprintf(why, len, "cannot read the vGPU plugin %.300s (from NVIDIA's host package): %s", path,
				strerror(-r)), -1;
	if (gs_plugin_verdict(path, real, sha, why, len))
		return -1;
	printf("vGPU plugin %s -> %s: the patched plugin copy\n", path, real);
	return 0;
}

/*
 * 1 with the first process that holds one of NVIDIA's device nodes in line; 0 none. gpu 0: the UVM
 * node, which a CUDA client holds. gpu 1: a GPU node, /dev/nvidia0 and up. /dev/nvidiactl is not
 * counted, because NVIDIA's own daemons keep it open without holding a GPU.
 */
static int node_holder(const struct gs_host *h, int gpu, char *line, size_t len)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	line[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s", h->proc) || !(d = opendir(p)))
		return 0;
	while ((de = readdir(d))) {
		char fdp[GS_PATH_MAX], fdlink[GS_PATH_MAX];
		struct dirent *fde;
		DIR *fd;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		if (gs_pathf(fdp, sizeof(fdp), "%s/%s/fd", h->proc, de->d_name) || !(fd = opendir(fdp)))
			continue;
		while ((fde = readdir(fd))) {
			ssize_t m = readlinkat(dirfd(fd), fde->d_name, fdlink, sizeof(fdlink) - 1);

			if (m <= 0)
				continue;
			fdlink[m] = '\0';
			if (gpu ? strncmp(fdlink, "/dev/nvidia", 11) == 0 && isdigit((unsigned char)fdlink[11]) :
				  strncmp(fdlink, "/dev/nvidia-uvm", 15) == 0) {
				snprintf(line, len, "process %.16s holds %.120s", de->d_name, fdlink);
				closedir(fd);
				closedir(d);
				return 1;
			}
		}
		closedir(fd);
	}
	closedir(d);
	return 0;
}

/* 1 with the first process holding an NVIDIA UVM node in line; 0 none. */
static int cuda_client(const struct gs_host *h, char *line, size_t len)
{
	return node_holder(h, 0, line, len);
}

/* ---- daemons -------------------------------------------------------------- */

/* systemctl restart unit; 0 or -1 with why (a vgpud oneshot may be inactive after a success). */
static int daemon_restart(const struct gs_host *h, const char *unit, int *uncertain, char *why, size_t len)
{
	const char *argv[] = { "systemctl", "restart", unit, NULL };
	int r;

	printf("systemctl restart %s\n", unit);
	r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		*uncertain = 1;
		return snprintf(why, len, "systemctl restart %s did not finish within %u ms; whether it restarted is unknown",
				unit, GS_MODULE_DEADLINE_MS), -1;
	}
	if (r)
		return snprintf(why, len, "systemctl restart %s exited with %d (`journalctl -u %s -b` says why)", unit, r,
				unit), -1;
	return 0;
}

/* systemctl restart unit, then up to wait ms for is-active --quiet: 0 or -1 with why. */
static int daemon_start(const struct gs_host *h, const char *unit, unsigned int wait, int *uncertain, char *why,
			size_t len)
{
	const char *active[] = { "systemctl", "is-active", "--quiet", unit, NULL };
	int r;

	if ((r = daemon_restart(h, unit, uncertain, why, len)))
		return r;
	for (unsigned int t = 0;; t += POLL_MS) {
		if (gs_host_run(h, active, MGR_ACTIVE_WAIT_MS) == 0)
			return 0;
		if (t >= wait)
			return snprintf(why, len, "%s is not active %u ms after its restart (`journalctl -u %s -b` says why)",
					unit, wait, unit), -1;
		gs_sleep_ms(POLL_MS);
	}
}

/* ---- the Ada slot hint ---------------------------------------------------- */

#define SLOT_HINT_PERCENT 75u
static int slot_high(unsigned int k, unsigned long long profile, unsigned long long card)
{
	return (k + 1ULL) * (profile - 32) * 100 > card * SLOT_HINT_PERCENT;
}

/* The profileSize of <vgpuType id="type"> in vgpuConfig.xml text, in MiB; 0 when absent or unreadable. */
static unsigned long long profile_mib(const char *xml, unsigned int type)
{
	const char *p = xml, *end, *size;
	unsigned long long v;
	char tag[40], *e;
	size_t n;

	n = (size_t)snprintf(tag, sizeof(tag), "<vgpuType id=\"%u\"", type);
	while ((p = strstr(p, tag)) && p[n] != ' ' && p[n] != '>')
		p += n;
	if (!p || !(end = strstr(p, "</vgpuType>")) || !(size = strstr(p, "<profileSize>")) || size > end)
		return 0;
	size += strlen("<profileSize>");
	if (size[0] != '0' || (size[1] != 'x' && size[1] != 'X'))
		return 0;
	errno = 0;
	v = strtoull(size, &e, 16);
	if (errno || e == size + 2 || *e != '<' || (v >> 20) <= 32)
		return 0;
	return v >> 20;
}

/* memory.total of pf in MiB from `nvidia-smi` lines ("00000000:03:00.0, 12282"); 0 when absent. */
static unsigned long long card_mib(char *smi, const struct gs_addr *pf)
{
	char *line, *save = NULL;

	for (line = strtok_r(smi, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned int d, b, s, f;
		unsigned long long mib;
		char extra;

		if (sscanf(line, "%x:%x:%x.%x, %llu %c", &d, &b, &s, &f, &mib, &extra) == 5 && d == pf->domain &&
		    b == pf->bus && s == pf->slot && f == pf->func)
			return mib;
	}
	return 0;
}

/* One line per Ada card whose VFs' framebuffer slots end near the top of its memory. A hint only. */
static void slot_hint(const struct gs_host *h, const struct gs_activate_opts *o, const struct gs_vgpu_card *cards,
		      unsigned int n)
{
	static const char *const smi[] = { "nvidia-smi", "--query-gpu=pci.bus_id,memory.total",
					   "--format=csv,noheader,nounits", NULL };
	char out[4096], text[4096], *xml;
	unsigned long long profile[GS_CFG_MAX_BDFS] = { 0 }, mem;
	unsigned int i, j, high, any = 0;
	size_t len;

	if (!o->vgpu_config || !h->capture || gs_read_whole(o->vgpu_config, &xml, &len, 64u << 20))
		return;
	for (i = 0; i < n; i++)
		if (cards[i].generation == GS_ADA && (profile[i] = profile_mib(xml, cards[i].type)))
			any = 1;
	free(xml);
	if (!any || h->capture(smi, out, sizeof(out)) != 0)
		return;
	for (i = 0; i < n; i++) {
		const struct gs_vgpu_card *k = &cards[i];
		char list[GS_VFS_MAX * (GS_BDF_STR_SIZE + 8)] = "";
		size_t off = 0;

		memcpy(text, out, sizeof(text));
		if (!profile[i] || !(mem = card_mib(text, &k->pf)))
			continue;
		for (j = high = 0; j < k->nvf; j++) {
			if (!slot_high(j, profile[i], mem))
				continue;
			off += (size_t)snprintf(list + off, sizeof(list) - off, "%s%s", high++ == 0 ? "" : j + 1 == k->nvf ? " and " : ", ",
						k->vf[j].str);
		}
		if (high)
			printf("%s: with type %u the %s %s may not start: %s near the top of the card's %llu MiB. "
			       "Put VMs on the lower VFs, or choose a smaller type.\n", k->pf.str, k->type,
			       high == 1 ? "VF" : "VFs", list,
			       high == 1 ? "its framebuffer slot ends" : "their framebuffer slots end", mem);
	}
}

/*
 * Host CUDA, last: nvidia_uvm is loaded after every VF is configured and while no client exists,
 * always with uvm_disable_hmm=1. 0, or the exit code; *uncertain is set when a load timed out.
 */
static int host_cuda_load(const struct gs_host *h, const struct gs_config *cfg, int *changed, int *uncertain)
{
	char p[GS_PATH_MAX], why[1024];
	int r;

	if (cfg->host_cuda) {
		char pv[16] = "";

		if (cuda_client(h, p, sizeof(p))) {
			fprintf(stderr, "geforce-sriov: %s\nnvidia_uvm is not loaded while a CUDA client exists; stop it, then run activate again\n", p);
			return GS_EXIT_FAIL;
		}
		if (!gs_pathf(p, sizeof(p), "%s/module/nvidia_uvm/parameters/uvm_disable_hmm", h->sys))
			gs_read_attr(p, pv, sizeof(pv));
		/* udev and NVIDIA's daemons may autoload UVM with the default; the package option keeps
		 * later autoloads correct, and unloading here is never allowed with VFs present. */
		if (gs_module_loaded(h, "nvidia_uvm") && strcmp(pv, "Y") != 0 && strcmp(pv, "1") != 0) {
			snprintf(why, sizeof(why),
				 "nvidia_uvm is loaded with uvm_disable_hmm='%s', not 1, and it is never unloaded while the VFs exist; power the machine off and on",
				 pv);
			fprintf(stderr, "geforce-sriov: %s\n", why);
			return GS_EXIT_FAIL;
		}
		if (!gs_module_loaded(h, "nvidia_uvm")) {
			const char *opts[] = { "uvm_disable_hmm=1" };

			if ((r = gs_module_load(h, 0, "nvidia_uvm", opts, 1, why, sizeof(why)))) {
				if (r == -ETIMEDOUT)
					*uncertain = 1;
				fprintf(stderr, "geforce-sriov: %s; the VFs and the core stay as they are\n", why);
				return *uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			}
			*changed = 1;
		}
		pv[0] = '\0';
		if (!gs_pathf(p, sizeof(p), "%s/module/nvidia_uvm/parameters/uvm_disable_hmm", h->sys))
			gs_read_attr(p, pv, sizeof(pv));
		if (strcmp(pv, "Y") != 0 && strcmp(pv, "1") != 0) {
			fprintf(stderr,
				"geforce-sriov: nvidia_uvm is loaded with uvm_disable_hmm='%s', not 1; the VFs and the core stay as they are\n",
				pv);
			return GS_EXIT_FAIL;
		}
	}
	return 0;
}

/*
 * The device nodes of the modules that are loaded. No udev rule makes them: NVIDIA's programs run
 * nvidia-modprobe on their first use, and the kernel modules that activation loads do not. A host
 * where no such program has run since the boot has the modules without all of their nodes, and a
 * container that is given a node starts after this unit and fails on the one that is missing. So
 * activation runs NVIDIA's own nvidia-modprobe for exactly the nodes that are missing; every module
 * it names is loaded already, so it loads nothing. This never fails an activation: a node that
 * still does not exist is a warning with the commands.
 */
static void device_nodes(const struct gs_host *h)
{
	struct gs_dev_nodes n, after;
	char list[GS_DEV_NODES_MAX * 50], how[256] = "";
	int r;

	gs_dev_nodes_scan(h, &n);
	if (!n.nmissing)
		return;
	for (unsigned int i = 0; i < 2; i++) {
		const char *cmd = i ? n.uvm : n.core;

		if (!cmd[0])
			continue;
		printf("%s\n", cmd);
		r = gs_host_run(h, i ? n.uvm_argv : n.core_argv, GS_MODULE_DEADLINE_MS);
		if (r == -ENOENT)
			snprintf(how, sizeof(how), "nvidia-modprobe, the program of NVIDIA's host driver package that creates them, was not found.");
		else if (r == -ETIMEDOUT)
			snprintf(how, sizeof(how), "`%.120s` did not finish within %u ms.", cmd, GS_MODULE_DEADLINE_MS);
		else if (r)
			snprintf(how, sizeof(how), "`%.120s` exited with %d.", cmd, r);
	}
	gs_dev_nodes_scan(h, &after);
	if (!after.nmissing)
		return;
	gs_dev_nodes_list(&after, list, sizeof(list));
	fprintf(stderr, "geforce-sriov: warning: these device nodes of NVIDIA's loaded modules do not exist: %s.\n"
		"The GPUs and their VFs are not affected. A container or a program that is given one of these nodes does\n"
		"not start. %s%sTo create them, run as root:\n", list, how, how[0] ? " " : "");
	if (after.core[0])
		fprintf(stderr, "  %s\n", after.core);
	if (after.uvm[0])
		fprintf(stderr, "  %s\n", after.uvm);
}

/* ---- the activation transaction ------------------------------------------ */

struct act_card {
	struct gs_vgpu_card v;
	struct gs_card c;
	struct woken woke;
	int display;
	int needs_removal;
	int needs_enable;
	int removed; /* a remove/rescan was attempted */
};

/* The helpers take a plain vgpu_card array; act_card is larger, so the array is copied out
 * instead of casting it (a cast would stride by the wrong size from the second card on). */
static void collect_vcards(const struct act_card *cards, unsigned int n, struct gs_vgpu_card *out)
{
	for (unsigned int i = 0; i < n; i++)
		out[i] = cards[i].v;
}

/* One card's read-only preflight: classification, removal plan, resource fit. 0 or GS_EXIT_REFUSED. */
static int preflight_card(const struct gs_host *h, const struct gs_config *cfg, struct act_card *a, char *why,
			  size_t len)
{
	struct gs_addr unbind[GS_PLAN_MAX_UNBIND];
	struct gs_resource_fit fit;
	unsigned int nunbind = 0;
	char drv[64];
	int r;

	gs_card_inspect(h->sys, &a->v.pf, &a->c);
	a->display = gs_display_kms(cfg, &a->c);
	if (!a->c.present)
		return snprintf(why, len, "%s is missing", a->v.pf.str), GS_EXIT_REFUSED;
	if (!a->c.entry)
		return snprintf(why, len, "%s (%04x:%04x) is not in the device table", a->v.pf.str, a->c.vendor,
				a->c.device), GS_EXIT_REFUSED;
	if (a->c.driver[0] && strcmp(a->c.driver, "nvidia") != 0) {
		if (strcmp(a->c.driver, "pci-pf-stub") == 0)
			return snprintf(why, len, "%s is bound to pci-pf-stub: an earlier sriov-manage stopped mid-way. Its driver_override may still name pci-pf-stub; clear it with `echo > /sys/bus/pci/devices/%s/driver_override`, power the machine off and on, then run activate",
					a->v.pf.str, a->v.pf.str), GS_EXIT_REFUSED;
		return snprintf(why, len, "%s is bound to %s, not nvidia", a->v.pf.str, a->c.driver), GS_EXIT_REFUSED;
	}
	if (gs_read_dev_attr(h->sys, a->v.pf.str, "driver_override", drv, sizeof(drv)) == 0 && drv[0] &&
	    strcmp(drv, "(null)") != 0 && strcmp(drv, "nvidia") != 0)
		return snprintf(why, len, "%s has driver_override=%s, so the re-probe could not bind it to nvidia. Clear it first:\n`echo > /sys/bus/pci/devices/%s/driver_override`",
				a->v.pf.str, drv, a->v.pf.str), GS_EXIT_REFUSED;
	if (a->c.has_sriov && a->c.numvfs > 0 && (a->c.numvfs > a->c.totalvfs || a->c.numvfs > gs_config_vfs(cfg)))
		return snprintf(why, len, "%s has %u VFs of %u, more than the configured %u; disable them with NVIDIA's own tool (`sriov-manage -d %s`), then run activate again",
				a->v.pf.str, a->c.numvfs, a->c.totalvfs, gs_config_vfs(cfg), a->v.pf.str), GS_EXIT_REFUSED;
	if (a->c.has_sriov && (r = gs_check_presented(&a->c, cfg, why, len)))
		return GS_EXIT_REFUSED;
	if (a->c.vf_bars_assigned < 0)
		return snprintf(why, len, "%s: its VF BAR state is unreadable, so neither a removal nor an enable can be judged",
				a->v.pf.str), GS_EXIT_REFUSED;
	if (a->c.bar0_assigned == 0)
		return snprintf(why, len, "%s: BAR0 is not assigned, so the card is unusable this boot; %s", a->v.pf.str,
				BAR_REMEDY), GS_EXIT_REFUSED;
	a->needs_removal = !a->c.has_sriov || a->c.vf_bars_assigned == 0;
	if (a->needs_removal) {
		if (a->c.numvfs > 0)
			return snprintf(why, len, "%s: %u virtual functions are active; activation never disables VFs", a->v.pf.str,
					a->c.numvfs), GS_EXIT_REFUSED;
		if (removal_plan(&a->c, unbind, &nunbind, why, len))
			return GS_EXIT_REFUSED;
		if (gs_resource_fit_assess(h, &a->c, gs_config_vfs(cfg), &fit))
			return snprintf(why, len, "%s: the resource assessment failed", a->v.pf.str), GS_EXIT_REFUSED;
		if (fit.state == GS_RESOURCE_FIT_INSUFFICIENT)
			return snprintf(why, len, "%s: the VF need does not fit: source=%s count=%u nonprefetch need=%llu window=%llu, prefetch need=%llu window=%llu",
					a->v.pf.str, fit.source ? fit.source : "unknown", fit.vfs, fit.nonprefetch_need,
					fit.nonprefetch_window, fit.prefetch_need, fit.prefetch_window), GS_EXIT_REFUSED;
		if (fit.state == GS_RESOURCE_FIT_UNKNOWN && !fit.room_unknown && a->c.entry &&
		    gs_vf_bar_size_lookup(a->c.entry->generation, 1, NULL))
			return snprintf(why, len, "%s: the resource need cannot be assessed before the removal: %s", a->v.pf.str,
					fit.source ? fit.source : "unknown"), GS_EXIT_REFUSED;
		if (fit.state == GS_RESOURCE_FIT_UNKNOWN)
			printf("%s: warning: %s; proceeding without a resource check\n", a->v.pf.str,
			       fit.source ? fit.source : "no VF BAR size entry for this card");
		printf("%s: VF resources source=%s count=%u nonprefetch need=%llu window=%llu prefetch need=%llu window=%llu\n",
		       a->v.pf.str, fit.source ? fit.source : "unknown", fit.vfs, fit.nonprefetch_need,
		       fit.nonprefetch_window, fit.prefetch_need, fit.prefetch_window);
	} else {
		a->needs_enable = a->c.numvfs == 0;
		if (!a->needs_enable) {
			if (vfs_ready(h, &a->v, &a->c, why, len))
				return GS_EXIT_REFUSED;
			a->v.nvf = a->c.numvfs;
		}
	}
	return 0;
}

/* 1 when the display plan lists a process holding an NVIDIA/DRM/fb node. */
static int plan_process_client(const struct gs_display_plan *plan)
{
	return strstr(plan->reason, "process ") != NULL;
}

/* The commands that bring the GPUs up after the user frees the display by hand. */
static void print_display_commands(const struct gs_display_plan *plan)
{
	fprintf(stderr, "save your work, then run as root:\n  systemctl stop %s\n  geforce-sriov activate\n  systemctl start %s\n",
		plan->manager_unit, plan->manager_unit);
}

/*
 * How far a selected card already is: 0 when it still needs a removal, an enable, the daemons or
 * a re-probe (or when a VF carries another vGPU type, which the full path refuses); 1 when its
 * VFs are enabled and NVIDIA's daemons have registered them (each has nvidia/creatable_vgpu_types);
 * 2 when every VF also has the card's type and is powered on and active, as `check` requires.
 */
static int card_progress(const struct gs_host *h, const struct act_card *a)
{
	char p[GS_PATH_MAX], cur[32], want[16], ctl[32], st[32];
	int ready = 1;

	if (a->needs_removal || a->needs_enable || !a->v.nvf)
		return 0;
	snprintf(want, sizeof(want), "%u", a->v.type);
	for (unsigned int j = 0; j < a->v.nvf; j++) {
		const char *vf = a->v.vf[j].str;

		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/creatable_vgpu_types", h->sys, vf) || !gs_exists(p) ||
		    gs_read_dev_attr(h->sys, vf, "nvidia/current_vgpu_type", cur, sizeof(cur)))
			return 0;
		if (strcmp(cur, "0") != 0 && strcmp(cur, want) != 0)
			return 0;
		if (strcmp(cur, want) != 0 || gs_read_dev_attr(h->sys, vf, "power/control", ctl, sizeof(ctl)) ||
		    strcmp(ctl, "on") != 0 || gs_read_dev_attr(h->sys, vf, "power/runtime_status", st, sizeof(st)) ||
		    strcmp(st, "active") != 0)
			ready = 0;
	}
	return ready ? 2 : 1;
}

/*
 * The framebuffer slots of the Ada VFs are held as soon as the VFs have their vGPU types, which is
 * when NVIDIA's driver can say where they are, and before nvidia_uvm is loaded: whatever allocates
 * on an Ada PF after this (host CUDA, a display manager, containers, the VMs' own start) finds the
 * slots taken. The question needs the GPU's device file, so the nodes are made first.
 */
static void fb_slots_hold(const struct gs_host *h, const struct gs_config *cfg, const struct gs_vgpu_card *vc, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		if (vc[i].generation == GS_ADA && vc[i].nvf && cfg->fb_slots != GS_FB_SLOTS_OFF) {
			device_nodes(h);
			break;
		}
	}
	gs_fb_hold(h, cfg, vc, n);
}

/*
 * Activation on a host that is already active. Every selected card has its VFs enabled and
 * registered, so nothing structural is left to do: with ready set and the display and CUDA modules
 * in place this changes nothing, and otherwise it finishes what is missing (the vGPU types, the VF
 * power state, a display or CUDA module that is not loaded). NVIDIA's daemons are not restarted and
 * no GPU is re-probed, so a running VM is not disturbed. The activating record covers the short
 * time the types are being written and is removed either way: nothing here leaves the cards in a
 * state that `check` cannot read for itself.
 */
static int finish_existing(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o,
			   const struct gs_vgpu_card *vc, unsigned int n, const char *bdfs, int need_display, int ready)
{
	char why[1024], allow[256] = "", pm[16], pb[16], pa[256], pv[16] = "", p[GS_PATH_MAX];
	int changed = 0, uncertain = 0, display_ok = 1, uvm_ok = 1, rc = GS_EXIT_FAIL, r;

	if (need_display) {
		display_allowlist(h, cfg, vc, n, allow, sizeof(allow));
		display_ok = !allow[0] || (gs_module_loaded(h, "nvidia_drm") && drm_configured(h, allow, pm, pb, pa));
	}
	if (cfg->host_cuda) {
		if (!gs_pathf(p, sizeof(p), "%s/module/nvidia_uvm/parameters/uvm_disable_hmm", h->sys))
			gs_read_attr(p, pv, sizeof(pv));
		uvm_ok = gs_module_loaded(h, "nvidia_uvm") && gs_param_bool(pv);
	}
	if (ready && display_ok && uvm_ok) {
		/* A node that is missing is made first, so that the line below is true when it is printed. */
		if (!o->dry_run) {
			device_nodes(h);
			/* Slots that are not held yet, or were given up since, are held now. Nothing is released or restarted. */
			gs_fb_hold(h, cfg, vc, n);
		}
		printf("Every selected VF is already configured, so there is nothing to do.\n");
		return o->dry_run ? GS_EXIT_OK : gs_check_vfs(h, cfg, o);
	}
	printf("The VFs of every selected GPU already exist and NVIDIA's vGPU daemons already serve them.\n"
	       "Activation only finishes what is missing. The daemons are not restarted and no GPU is re-probed.\n");
	if (o->dry_run) {
		printf("dry run: every check passed; activation would run, in this order:\n");
		if (!display_ok)
			printf("  load nvidia_modeset, then nvidia_drm modeset=1 fbdev=1 gpu_allow=%s\n", allow);
		for (unsigned int i = 0; i < n; i++)
			for (unsigned int j = 0; j < vc[i].nvf; j++)
				printf("  %s: VF %u: echo %u > nvidia/current_vgpu_type, echo on > power/control, read back on and active\n",
				       vc[i].pf.str, j, vc[i].type);
		for (unsigned int i = 0; i < n; i++)
			if (vc[i].generation == GS_ADA && cfg->fb_slots != GS_FB_SLOTS_OFF)
				printf("  %s: hold the framebuffer slot of each VF (NVIDIA's driver is asked where it is)\n", vc[i].pf.str);
		if (!uvm_ok)
			printf("  modprobe nvidia_uvm uvm_disable_hmm=1 (after every VF is configured)\n");
		printf("  nvidia-modprobe, for each device node of a loaded NVIDIA module that does not exist\n");
		return GS_EXIT_OK;
	}
	if ((r = write_activating(h, bdfs))) {
		fprintf(stderr, "geforce-sriov: cannot write %s/" GS_ACTIVATING_NAME ": %s. Nothing was changed.\n", h->run_dir,
			strerror(-r));
		return GS_EXIT_FAIL;
	}
	if ((r = display_load(h, cfg, vc, n, why, sizeof(why)))) {
		if (r == -ETIMEDOUT)
			uncertain = 1;
		fprintf(stderr, "geforce-sriov: display stack: %s\n", why);
		goto fail;
	}
	for (unsigned int i = 0; i < n; i++)
		if ((r = finish_card(h, &vc[i], o->types_wait_ms ? o->types_wait_ms : VF_TYPES_WAIT_MS))) {
			if (r == GS_EXIT_COLD_CYCLE)
				uncertain = 1;
			goto fail;
		}
	slot_hint(h, o, vc, n);
	fb_slots_hold(h, cfg, vc, n);
	if ((r = host_cuda_load(h, cfg, &changed, &uncertain))) {
		rc = r;
		goto fail;
	}
	if ((r = clear_activating(h))) {
		fprintf(stderr, "geforce-sriov: cannot remove %s/" GS_ACTIVATING_NAME ": %s\n", h->run_dir, strerror(-r));
		return GS_EXIT_FAIL;
	}
	device_nodes(h);
	printf("activation finished\n");
	return gs_check_vfs(h, cfg, o);
fail:
	/* A module load or a VF write that timed out is uncertain state: exit 5, and exit 5 always leaves the guard set. */
	if (uncertain || rc == GS_EXIT_COLD_CYCLE) {
		if ((r = write_marker(h, bdfs, "finish")))
			fprintf(stderr, "geforce-sriov: cannot write the activation lock: %s\n", strerror(-r));
		fprintf(stderr, "geforce-sriov: an operation did not finish in time, so the state of the VFs or of NVIDIA's modules is\n"
			"uncertain. The activation lock is set.\n%s\n", gs_recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	clear_activating(h);
	fprintf(stderr, "geforce-sriov: activation stopped before every VF was configured. NVIDIA's daemons were not restarted\n"
		"and no GPU was re-probed. Fix the problem named above, then run `geforce-sriov activate` again.\n");
	return rc;
}

/*
 * One activation: what gs_activate() was given, the selected cards, and what has happened so far.
 * The phases below share it. rc is the exit status; the flags say what the stop path has to undo
 * or keep.
 */
struct activation {
	const struct gs_host *h;
	const struct gs_config *cfg;
	const struct gs_activate_opts *o;
	struct act_card *cards;
	struct gs_vgpu_card vc[GS_CFG_MAX_BDFS];
	unsigned int n;
	struct gs_display_plan plan;
	struct host_vgpus vgpus;
	char why[1024], p[GS_PATH_MAX], bdfs[GS_BDF_LIST_STR_SIZE], ver[32], allow[256];
	int rc;
	int changed;       /* something on the host was changed */
	int destructive;   /* a module was unloaded or a GPU removed or re-probed: the guard stays */
	int uncertain;     /* an operation did not finish: exit 5 */
	int guard, activating; /* the guard and the activating record are written */
	int dm_stopped;    /* the display manager was stopped and has to be started again */
	int need_display, display_work, uvm_unload, other_version, ari_eff;
	int enable_blocked; /* the enable-window module block is in place */
};

/* What a phase tells the runner: go on, take the stop path, or return rc as it is. */
enum act_next { ACT_GO, ACT_STOP, ACT_DONE };

static enum act_next act_done(struct activation *a, int rc)
{
	a->rc = rc;
	return ACT_DONE;
}

/* Before the checks: the skip token, the guard of an earlier run, the selection, the laptop consent, the boot wait. */
static enum act_next act_begin(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	const struct gs_activate_opts *o = a->o;
	unsigned int i;
	int r;

	if (o->boot && cmdline_token(h->proc, "geforce_sriov.skip")) {
		printf("geforce_sriov.skip is on the kernel command line: activation changes nothing this boot\n");
		return act_done(a, GS_EXIT_OK);
	}
	if (gs_pathf(a->p, sizeof(a->p), "%s/" GS_ATTEMPT_NAME, h->state_dir) || gs_exists(a->p)) {
		fprintf(stderr, "geforce-sriov: activation lock %s is set; nothing done.\n%s\n", a->p, gs_recovery_msg);
		return act_done(a, GS_EXIT_COLD_CYCLE);
	}
	a->cards = calloc(GS_CFG_MAX_BDFS, sizeof(*a->cards));
	if (!a->cards)
		return act_done(a, GS_EXIT_FAIL);
	if ((r = gs_select_cards(h->sys, cfg, o, a->vc, &a->n))) {
		a->rc = r;
		return ACT_DONE;
	}
	for (i = 0; i < a->n; i++)
		a->cards[i].v = a->vc[i];
	gs_addr_list_str(cfg->bdfs, cfg->nbdfs, a->bdfs, sizeof(a->bdfs));

	/*
	 * A laptop GPU is kept powered while it is managed (d3cold_allowed=0, power/control=on), and that
	 * costs battery life. It needs the user's consent, which is d3cold=off in the config: `install
	 * --disable-d3cold` or setup's question writes it. Without it no laptop GPU is touched.
	 */
	if (!cfg->d3cold_off) {
		struct gs_addr laptops[GS_CFG_MAX_BDFS];
		unsigned int nlap = gs_laptop_gpus(h->sys, cfg->bdfs, cfg->nbdfs, laptops);

		if (nlap) {
			char d3[GS_D3COLD_WHY_SIZE];

			gs_d3cold_why(d3, sizeof(d3), laptops, nlap, cfg->bdfs, cfg->nbdfs, NULL, 0, "");
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", d3);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
	}

	/* --boot waits for the version file and each selected PF to bind, as the old unit did. */
	if (o->boot && !o->dry_run) {
		unsigned int budget = o->boot_wait_ms ? o->boot_wait_ms : GS_BOOT_BIND_WAIT_MS;

		for (i = 0; i < a->n; i++) {
			struct gs_card c;
			unsigned int waited = 0;

			gs_card_inspect(h->sys, &a->cards[i].v.pf, &c);
			while (waited < budget) {
				gs_nvidia_version(h->proc, a->ver, sizeof(a->ver));
				if (a->ver[0] && strcmp(c.driver, "nvidia") == 0)
					break;
				gs_sleep_ms(POLL_MS);
				waited += POLL_MS;
				gs_card_inspect(h->sys, &a->cards[i].v.pf, &c);
			}
			if (strcmp(c.driver, "nvidia") != 0)
				printf("%s: still bound to %s after %u ms\n", a->cards[i].v.pf.str,
				       c.driver[0] ? c.driver : "nothing", budget);
		}
	}
	return ACT_GO;
}

/* A: read-only preflight. */
static enum act_next act_preflight(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	const struct gs_activate_opts *o = a->o;
	unsigned int i;
	int r;

	gs_nvidia_version(h->proc, a->ver, sizeof(a->ver));
	if (a->ver[0]) {
		if ((a->other_version = gs_driver_version_line(a->ver, a->why, sizeof(a->why))))
			fprintf(stderr, "geforce-sriov: %s; continuing\n", a->why);
		if (check_gsp_off(h->proc, a->why, sizeof(a->why))) {
			if (!a->other_version) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", a->why);
				a->rc = GS_EXIT_REFUSED;
				return ACT_DONE;
			}
			fprintf(stderr, "geforce-sriov: %s; continuing on this driver version\n", a->why);
		}
	}
	if (o->plugin && check_plugin(o->plugin, a->why, sizeof(a->why))) {
		if (!a->other_version) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", a->why);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
		fprintf(stderr, "geforce-sriov: %s; continuing on this driver version\n", a->why);
	}
	/*
	 * After a kernel update DKMS puts its stock nvidia.ko back, so activation would bring up a
	 * module without the driver step's changes. Whenever the driver step's record exists, the
	 * module must be the recorded build; a host without any record is warned and goes on.
	 */
	{
		struct gs_build b;
		char line[3 * GS_PATH_MAX];

		if (gs_build_identity(o->build ? o->build : h->build, 0, &b)) {
			if (gs_build_line(&b, line, sizeof(line))) {
				if (b.record_read) {
					fprintf(stderr, "geforce-sriov: refused, nothing changed: %.800s\n", line);
					a->rc = GS_EXIT_REFUSED;
					return ACT_DONE;
				}
				fprintf(stderr, "geforce-sriov: warning: %s\n", line);
			} else if (gs_loaded_line(h->sys, &b, line, sizeof(line))) {
				fprintf(stderr, "geforce-sriov: warning: %s\n", line);
			}
		}
	}
	for (i = 0; i < a->n; i++) {
		if ((r = preflight_card(h, cfg, &a->cards[i], a->why, sizeof(a->why)))) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", a->why);
			a->rc = r;
			return ACT_DONE;
		}
		a->need_display |= a->cards[i].display;
		a->display_work |= a->cards[i].display && (a->cards[i].needs_removal || a->cards[i].needs_enable);
	}
	/*
	 * The console handoff of a display card that needs a removal unloads NVIDIA's display modules,
	 * and nvidia itself when nothing uses it. That is allowed only while no GPU on the host has
	 * VFs: another selected card may already have its VFs up, and its preflight accepts them.
	 */
	for (i = 0; i < a->n; i++) {
		if (!a->cards[i].display || !a->cards[i].needs_removal)
			continue;
		if (any_vfs(h, a->p, sizeof(a->p)) != 0) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s. %s carries the host console and has to be removed and\n"
				"rescanned, which unloads NVIDIA's display modules, and they are unloaded only while no GPU on the\n"
				"host has VFs. Power the machine off and on, then run `geforce-sriov activate` again, so that every\n"
				"selected GPU is brought up in one run.\n", a->p, a->cards[i].v.pf.str);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
		break;
	}
	display_on_notices(h, cfg, a->vc, a->n);
	/*
	 * The livepatch must already be in place: the early boot unit loads it before udev. Loading it
	 * now works only while NVIDIA's driver is not bound to any selected PF (owner decision 4 keeps
	 * the unbind-before-livepatch path out of 0.3.0), so a mismatch or a late run refuses here,
	 * before the guard or any write, with the one reboot that fixes it.
	 */
	{
		struct gs_module_state m;
		int ari = cfg->ari;

		if (ari != GS_SW_OFF && gs_cmdline_noari(h->proc)) {
			printf("kernel command line has pci=noari: the livepatch runs with ari=0\n");
			ari = GS_SW_OFF;
		}
		a->ari_eff = ari;
		gs_read_module_state(h->sys, &m);
		if (m.lp_present) {
			if (strcmp(m.ari, sw_yn(ari)) || strcmp(m.msix, sw_yn(cfg->msix)) ||
			    gs_module_vfs(&m) != gs_config_vfs(cfg) || strcmp(m.bdfs, a->bdfs)) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is loaded with ari=%s msix=%s vfs=%u bdfs=%s, not the configured ari=%s msix=%s vfs=%u bdfs=%s; power the machine off and on to change them\n",
					GS_MODULE_NAME, m.ari, m.msix, gs_module_vfs(&m), m.bdfs, sw_yn(ari), sw_yn(cfg->msix),
					gs_config_vfs(cfg), a->bdfs);
				a->rc = GS_EXIT_REFUSED;
				return ACT_DONE;
			}
		} else {
			if (gs_pathf(a->p, sizeof(a->p), "%s/kernel/livepatch", h->sys) || !gs_exists(a->p)) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: the kernel has no livepatch support (CONFIG_LIVEPATCH)\n");
				a->rc = GS_EXIT_REFUSED;
				return ACT_DONE;
			}
			if (gs_module_loaded(h, "nvidia") && cmdline_token(h->proc, "geforce_sriov.skip")) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: geforce_sriov.skip is on the kernel command line, so the\n"
					"livepatch %s was not loaded at boot, and NVIDIA's driver is loaded now. Remove geforce_sriov.skip,\n"
					"then power the machine off and on; activation runs at that boot\n", GS_MODULE_NAME);
				a->rc = GS_EXIT_REFUSED;
				return ACT_DONE;
			}
			if (gs_module_loaded(h, "nvidia")) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: the livepatch %s is not loaded and NVIDIA's driver is\n"
					"already loaded. The early boot unit loads the livepatch before udev lets nvidia bind a selected PF;\n"
					"power the machine off and on, and activation then runs with it in place\n", GS_MODULE_NAME);
				a->rc = GS_EXIT_REFUSED;
				return ACT_DONE;
			}
		}
	}
	for (i = 0; i < a->n; i++)
		if ((a->cards[i].needs_enable || a->cards[i].needs_removal) && h->enable_prereq &&
		    (r = h->enable_prereq(cfg, a->why, sizeof(a->why)))) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", a->why);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
	/*
	 * A host that is already active takes none of the steps below. The short path applies when
	 * every selected card is that far and no earlier run left its record: a run that stopped
	 * half-way may have restarted the daemons without the re-probe that follows, and only the
	 * full path below is known to bring such a card up.
	 */
	{
		int progress = 2;

		for (i = 0; i < a->n; i++) {
			int c = card_progress(h, &a->cards[i]);

			if (c < progress)
				progress = c;
		}
		if (progress && (gs_pathf(a->p, sizeof(a->p), "%s/" GS_ACTIVATING_NAME, h->run_dir) || gs_exists(a->p)))
			progress = 0;
		if (progress) {
			collect_vcards(a->cards, a->n, a->vc);
			a->rc = finish_existing(h, cfg, o, a->vc, a->n, a->bdfs, a->need_display, progress == 2);
			return ACT_DONE;
		}
	}
	/* The daemons serve every GPU on the host: a restart would end any vGPU there. */
	if ((r = host_vgpu(h->sys, &a->vgpus)) < 0) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", a->vgpus.first);
		a->rc = GS_EXIT_REFUSED;
		return ACT_DONE;
	}
	if (r) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: each VF below has a vGPU type set, and restarting "
			"NVIDIA's vGPU daemons would clear it and end any VM that uses it:\n%s"
			"Stop the VMs that use them, then write 0 to each nvidia/current_vgpu_type, for example:\n"
			"  for f in %s; do echo 0 > /sys/bus/pci/devices/$f/nvidia/current_vgpu_type; done\n",
			a->vgpus.lines, a->vgpus.bdfs);
		a->rc = GS_EXIT_REFUSED;
		return ACT_DONE;
	}
	collect_vcards(a->cards, a->n, a->vc);
	if ((r = foreign_vf(h->sys, a->vc, a->n, a->why, sizeof(a->why)))) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: %s%s\n", a->why,
			r < 0 ? "" : ": that GPU is not selected, and the restart of NVIDIA's vGPU daemons would re-register it too. Disable its VFs first");
		a->rc = GS_EXIT_REFUSED;
		return ACT_DONE;
	}
	/* Host CUDA: only a transaction that unloads modules needs to be free of CUDA clients, and a
	 * removal is allowed only with zero VFs, so UVM is never unloaded under a live vGPU. */
	if (gs_module_loaded(h, "nvidia_uvm")) {
		for (i = 0; i < a->n; i++)
			if (a->cards[i].needs_removal)
				a->uvm_unload = 1;
		if (a->uvm_unload && cuda_client(h, a->p, sizeof(a->p))) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s. Stop it, then run activate again\n"
				"(`nvidia-smi --query-compute-apps=pid,process_name,used_memory` lists CUDA programs)\n", a->p);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
		if (a->uvm_unload && any_vfs(h, a->p, sizeof(a->p)) != 0) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: nvidia_uvm is loaded and %s; UVM is unloaded only while no VF exists and no user holds it\n", a->p);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
	}
	/* Display: only the manager that is actually running is stopped, and only with consent, and
	 * only when this transaction will unload or reload the display stack. A host that is already
	 * active never gets here: finish_existing() above has handled it. */
	if (a->need_display && a->display_work) {
		gs_display_plan_build(h, cfg, &a->plan);
		if (a->plan.inspection_unknown) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: the display clients cannot all be inspected:%s\n",
				a->plan.reason);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
		if (a->plan.manager_active && !o->stop_display_manager) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s holds a selected GPU. %s does not stop graphical sessions.%s\n",
				a->plan.manager_unit,
				o->boot ? "The boot unit" : "Activation",
				o->boot ? " Boot never prompts and never stops one." : "");
			print_display_commands(&a->plan);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
		if (plan_process_client(&a->plan) && !a->plan.manager_active) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: a process holds a selected GPU's device node:%s\n"
				"Stop it, then run activate again\n", a->plan.reason);
			a->rc = GS_EXIT_REFUSED;
			return ACT_DONE;
		}
	}
	return ACT_GO;
}

/* --dry-run: every check passed; print what would run, and stop. */
static enum act_next act_dry_run(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	const struct gs_activate_opts *o = a->o;
	unsigned int i, j;

	if (o->dry_run) {
		if (a->need_display)
			display_allowlist(h, cfg, a->vc, a->n, a->allow, sizeof(a->allow));
		printf("dry run: every check passed; activation would run, in this order:\n");
		for (i = 0; i < a->n; i++) {
			const char *what = a->cards[i].needs_removal ? "remove and rescan the empty bus (bridge held awake, one rescan)" :
					    a->cards[i].needs_enable ? "enable its VFs" : "keep its VFs";

			printf("  %s: %s; %s\n", a->cards[i].v.pf.str, what,
			       a->cards[i].display ? "display selected" : "no display");
		}
		if (gs_module_loaded(h, "nvidia_drm") || gs_module_loaded(h, "nvidia_modeset"))
			printf("  unload nvidia_drm, then nvidia_modeset (only while no affected GPU has VFs)\n");
		else if (a->need_display)
			printf("  load nvidia_modeset, then nvidia_drm modeset=1 fbdev=1 gpu_allow=%s\n", a->allow);
		for (i = 0; i < a->n; i++)
			if (a->cards[i].needs_removal || a->cards[i].needs_enable)
				printf("  %s: %s -e %s\n", a->cards[i].v.pf.str, cfg->sriov_manage, a->cards[i].v.pf.str);
		if (!gs_module_loaded(h, "nvidia_vgpu_vfio"))
			printf("  modprobe nvidia_vgpu_vfio\n");
		printf("  systemctl restart nvidia-vgpud.service\n"
		       "  systemctl restart nvidia-vgpu-mgr.service, then wait until it is active\n");
		for (i = 0; i < a->n; i++)
			printf("  %s: unbindLock, then unbind and bind through NVIDIA's driver\n", a->cards[i].v.pf.str);
		if (a->need_display)
			printf("  load nvidia_modeset, then nvidia_drm modeset=1 fbdev=1 gpu_allow=%s\n", a->allow);
		for (i = 0; i < a->n; i++)
			for (j = 0; j < (a->cards[i].v.nvf ? a->cards[i].v.nvf : gs_config_vfs(cfg)); j++)
				printf("  %s: VF %u: echo %u > nvidia/current_vgpu_type, echo on > power/control, read back on and active\n",
				       a->cards[i].v.pf.str, j, a->cards[i].v.type);
		for (i = 0; i < a->n; i++)
			if (a->cards[i].v.generation == GS_ADA && cfg->fb_slots != GS_FB_SLOTS_OFF)
				printf("  %s: hold the framebuffer slot of each VF (NVIDIA's driver is asked where it is)\n",
				       a->cards[i].v.pf.str);
		if (cfg->host_cuda)
			printf("  modprobe nvidia_uvm uvm_disable_hmm=1 (after every VF is configured)\n");
		printf("  nvidia-modprobe, for each device node of a loaded NVIDIA module that does not exist\n");
		if (a->need_display && o->stop_display_manager)
			printf("  systemctl stop, then start %s\n", a->plan.manager_unit);
		a->rc = GS_EXIT_OK;
		return ACT_DONE;
	}
	return ACT_GO;
}

/* B: quiesce only what the transaction needs. */
static enum act_next act_quiesce(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	const struct gs_activate_opts *o = a->o;
	int r;

	if (a->need_display && o->stop_display_manager) {
		gs_display_plan_build(h, cfg, &a->plan);
		if (a->plan.manager_active) {
			const char *argv[] = { "systemctl", "stop", a->plan.manager_unit, NULL };

			printf("systemctl stop %s\n", a->plan.manager_unit);
			r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
			if (r) {
				fprintf(stderr, "geforce-sriov: systemctl stop %s exited with %d; nothing changed\n",
					a->plan.manager_unit, r);
				a->rc = GS_EXIT_REFUSED;
				return ACT_DONE;
			}
			a->dm_stopped = 1;
			a->changed = 1;
			gs_display_plan_build(h, cfg, &a->plan);
			if (plan_process_client(&a->plan)) {
				fprintf(stderr, "geforce-sriov: %s was stopped, but a process still holds a selected GPU's device node:%s\n"
					"Nothing else changed. Restore the display with `systemctl start %s` after stopping it\n",
					a->plan.manager_unit, a->plan.reason, a->plan.manager_unit);
				a->rc = GS_EXIT_FAIL;
				return ACT_STOP;
			}
		}
	}
	/*
	 * The owner of the Ada framebuffer slots lets go: it has each GPU open while it holds a slot,
	 * and NVIDIA's driver does not let go of a GPU that is open. Here and not earlier: the two
	 * ways out above change nothing else, and must not leave the slots released. Nothing is
	 * unloaded or re-probed before this point.
	 */
	gs_fb_release(h);
	if ((r = write_marker(h, a->bdfs, "activate"))) {
		fprintf(stderr, "geforce-sriov: cannot write the activation lock: %s; the transaction stops here\n", strerror(-r));
		a->rc = GS_EXIT_FAIL;
		/* The display manager may be stopped by now: the stop path starts it again. */
		return ACT_STOP;
	}
	a->guard = 1;
	if ((r = write_activating(h, a->bdfs))) {
		fprintf(stderr, "geforce-sriov: cannot write %s/" GS_ACTIVATING_NAME "; the transaction stops here\n", h->run_dir);
		a->rc = GS_EXIT_FAIL;
		return ACT_STOP;
	}
	a->activating = 1;
	return ACT_GO;
}

/* C: power, reveal and load. */
static enum act_next act_power_load(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	unsigned int i;
	int r;

	for (i = 0; i < a->n; i++) {
		if ((r = wake_card(h, &a->cards[i].c, &a->cards[i].woke, &a->uncertain, a->why, sizeof(a->why))) < 0) {
			fprintf(stderr, "geforce-sriov: stopped before any removal: %s\n", a->why);
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			return ACT_STOP;
		}
	}
	{
		struct gs_module_state m;
		struct gs_config lpcfg = *cfg;

		lpcfg.ari = a->ari_eff;
		gs_read_module_state(h->sys, &m);
		if (!m.lp_present) {
			if ((r = ensure_livepatch(h, &lpcfg, a->bdfs))) {
				a->rc = r;
				return ACT_STOP;
			}
			a->changed = 1;
		} else {
			printf("livepatch %s already loaded\n", GS_MODULE_NAME);
		}
	}
	/*
	 * The firmware framebuffer is released before the driver binds the card: once nvidia or its
	 * display modules own it, the firmware framebuffer is no longer the only user and the handoff
	 * is refused. This is the order the old console release used.
	 *
	 * udev autoloads nvidia_drm at boot (no parameters), and its DRM nodes make the inspect
	 * below refuse the card itself. Nothing else uses it at this point (zero VFs, checked in
	 * preflight), and display_load reloads it with its options afterwards.
	 */
	{
		int need_handoff = 0;

		for (i = 0; i < a->n; i++)
			need_handoff |= a->cards[i].display && a->cards[i].needs_removal;
		if (need_handoff) {
			const char *const *stack = gs_nvidia_clients;
			char ref[16] = "";

			/* Checked in the preflight; checked again here because VFs may have appeared since. */
			if (any_vfs(h, a->p, sizeof(a->p)) != 0) {
				fprintf(stderr, "geforce-sriov: %s appeared after the checks, so NVIDIA's display modules are not unloaded.\n"
					"No GPU was removed.\n", a->p);
				a->rc = GS_EXIT_FAIL;
				return ACT_STOP;
			}
			/*
			 * udev autoloads nvidia_drm (no parameters) and nvidia at boot. Their DRM nodes and
			 * BAR mappings make the inspect below refuse the card itself, so the stack goes
			 * first; nvidia is reloaded after the handoff. refcnt guards a card in use.
			 */
			for (unsigned int k = 0; k < GS_NVIDIA_CLIENT_MODULES; k++) {
				if (!gs_module_loaded(h, stack[k]))
					continue;
				printf("%s is loaded (a boot autoload); unloading it so the firmware framebuffer can be released\n",
				       stack[k]);
				if ((r = gs_module_unload(h, stack[k], a->why, sizeof(a->why)))) {
					fprintf(stderr, "geforce-sriov: %s\n", a->why);
					a->rc = r == -ETIMEDOUT ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
					return ACT_STOP;
				}
			}
			if (!gs_pathf(a->p, sizeof(a->p), "%s/module/nvidia/refcnt", h->sys))
				gs_read_attr(a->p, ref, sizeof(ref));
			/* Only a reference count that reads exactly 0 shows that nothing uses the driver. */
			if (gs_module_loaded(h, "nvidia") && !strcmp(ref, "0")) {
				printf("nvidia is loaded (a boot autoload); unloading it so the firmware framebuffer can be released\n");
				if ((r = gs_module_unload(h, "nvidia", a->why, sizeof(a->why)))) {
					fprintf(stderr, "geforce-sriov: %s\n", a->why);
					a->rc = r == -ETIMEDOUT ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
					return ACT_STOP;
				}
			}
		}
	}
	for (i = 0; i < a->n; i++) {
		if (!a->cards[i].display || !a->cards[i].needs_removal)
			continue;
		r = handoff_fb(h, &a->cards[i].c, a->p, sizeof(a->p), a->why, sizeof(a->why));
		if (r) {
			if (r == -ETIMEDOUT)
				a->uncertain = 1;
			fprintf(stderr, "geforce-sriov: %s\n", a->why);
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			return ACT_STOP;
		}
	}
	/* UVM must go before a PF removal; only with no VFs and no clients, as checked above. */
	if (a->uvm_unload) {
		if ((r = gs_module_unload(h, "nvidia_uvm", a->why, sizeof(a->why)))) {
			if (r == -ETIMEDOUT)
				a->uncertain = 1;
			fprintf(stderr, "geforce-sriov: stopped before any removal: %s\n", a->why);
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			return ACT_STOP;
		}
		a->changed = 1;
	}
	if (!gs_module_loaded(h, "nvidia")) {
		if ((r = gs_module_load(h, 0, "nvidia", NULL, 0, a->why, sizeof(a->why)))) {
			if (r == -ETIMEDOUT)
				a->uncertain = 1;
			fprintf(stderr, "geforce-sriov: %s\n", a->why);
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			return ACT_STOP;
		}
		a->changed = 1;
	}
	collect_vcards(a->cards, a->n, a->vc);
	if ((r = display_load(h, cfg, a->vc, a->n, a->why, sizeof(a->why)))) {
		/* A module load that did not finish leaves the display modules in an unknown state. */
		if (r == -ETIMEDOUT)
			a->uncertain = 1;
		fprintf(stderr, "geforce-sriov: display stack: %s\n", a->why);
		a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
		return ACT_STOP;
	}
	return ACT_GO;
}

/* D: empty-bus resize, per card that needs it. */
static enum act_next act_resize(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	unsigned int i;

	for (i = 0; i < a->n; i++) {
		char bridge_was[16] = "";
		int touched = 0;

		if (!a->cards[i].needs_removal)
			continue;
		/* remove_card() holds the bridge awake; a refusal before the removal gives its setting back. */
		if (a->cards[i].c.bridge_dir[0] && !gs_pathf(a->p, sizeof(a->p), "%s/power/control", a->cards[i].c.bridge_dir))
			gs_read_attr(a->p, bridge_was, sizeof(bridge_was));
		a->rc = remove_card(h, &a->cards[i].v, cfg, a->bdfs, a->cards[i].display, &a->uncertain, &touched, a->why, sizeof(a->why));
		if (a->rc != GS_EXIT_OK && !touched && !a->uncertain && bridge_was[0] && strcmp(bridge_was, "on") != 0 &&
		    !gs_pathf(a->p, sizeof(a->p), "%s/power/control", a->cards[i].c.bridge_dir) &&
		    give_back(h, a->p, bridge_was, a->cards[i].c.bridge.str, "bridge power/control"))
			a->uncertain = 1;
		if (touched) {
			a->cards[i].removed = 1;
			a->destructive = 1;
			a->changed = 1;
		}
		if (a->rc != GS_EXIT_OK) {
			fprintf(stderr, "geforce-sriov: %s: %s\n", a->cards[i].v.pf.str, a->why);
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : a->changed ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
			return ACT_STOP;
		}
		a->cards[i].needs_enable = 1;
	}
	return ACT_GO;
}

/* E: unload the display stack only when a VF has to be enabled, and never with any VF present. */
static enum act_next act_unload_enable(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	unsigned int i;
	int r;

	if (gs_module_loaded(h, "nvidia_drm") || gs_module_loaded(h, "nvidia_modeset")) {
		const char *const *mods = gs_nvidia_clients;
		int enable_needed = 0;

		for (i = 0; i < a->n; i++)
			enable_needed |= a->cards[i].needs_enable;
		if (enable_needed && (r = any_vfs(h, a->p, sizeof(a->p))) != 0) {
			fprintf(stderr, "geforce-sriov: %s; NVIDIA's display modules are unloaded only while no affected GPU has VFs.\n"
				"Stop the VMs that use them, disable the VFs with NVIDIA's own tool (`sriov-manage -d <BDF>`),\n"
				"then run activate again. This command never disables VFs itself. The activating record stays,\n"
				"so check reports NOT READY meanwhile\n", a->p);
			a->rc = GS_EXIT_FAIL;
			return ACT_STOP;
		}
		for (size_t k = 0; enable_needed && k < GS_NVIDIA_DISPLAY_MODULES; k++) {
			if ((r = any_vfs(h, a->p, sizeof(a->p))) != 0) {
				fprintf(stderr, "geforce-sriov: %s appeared during the unload; stopping\n", a->p);
				a->rc = GS_EXIT_FAIL;
				return ACT_STOP;
			}
			a->destructive = 1;
			if ((r = gs_module_unload(h, mods[k], a->why, sizeof(a->why)))) {
				if (r == -ETIMEDOUT)
					a->uncertain = 1;
				fprintf(stderr, "geforce-sriov: %s\n", a->why);
				a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
				return ACT_STOP;
			}
		}
		if (enable_needed)
			a->changed = 1;
	}
	for (i = 0; i < a->n; i++) {
		if (!a->cards[i].needs_enable)
			continue;
		printf(SRIOV_MANAGE_NOTE);
		break;
	}
	{
		int any_enable = 0;

		for (i = 0; i < a->n; i++)
			any_enable |= a->cards[i].needs_enable;
		if (any_enable && (a->need_display || cfg->host_cuda)) {
			if ((r = enable_block_set(h, "install nvidia_drm /bin/false\n"
						     "install nvidia_uvm /bin/false\n", a->why, sizeof(a->why))) == 0) {
				a->enable_blocked = 1;
				printf("nvidia_drm and nvidia_uvm autoloading is blocked while the VFs are enabled\n");
			} else {
				fprintf(stderr, "geforce-sriov: warning: cannot block module autoloading: %s\n", a->why);
			}
		}
	}
	for (i = 0; i < a->n; i++) {
		struct gs_card c;

		if (!a->cards[i].needs_enable)
			continue;
		gs_card_inspect(h->sys, &a->cards[i].v.pf, &c);
		a->rc = enable_pf(h, cfg, &a->cards[i].v.pf, &c, &a->uncertain, a->why, sizeof(a->why));
		if (a->rc != GS_EXIT_OK) {
			fprintf(stderr, "geforce-sriov: %s\n", a->why);
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			return ACT_STOP;
		}
		a->changed = 1;
		a->cards[i].v.enable = 0;
	}
	for (i = 0; i < a->n; i++) {
		struct gs_card c;

		gs_card_inspect(h->sys, &a->cards[i].v.pf, &c);
		if (vfs_ready(h, &a->cards[i].v, &c, a->why, sizeof(a->why))) {
			fprintf(stderr, "geforce-sriov: %s\nno GPU was re-probed\n", a->why);
			a->rc = GS_EXIT_FAIL;
			return ACT_STOP;
		}
	}
	return ACT_GO;
}

/* The vfio module and NVIDIA's two daemons, which then serve the new VFs. */
static enum act_next act_daemons(struct activation *a)
{
	const struct gs_host *h = a->h;
	int r;

	if (!gs_module_loaded(h, "nvidia_vgpu_vfio")) {
		if ((r = gs_module_load(h, 0, "nvidia_vgpu_vfio", NULL, 0, a->why, sizeof(a->why)))) {
			if (r == -ETIMEDOUT)
				a->uncertain = 1;
			fprintf(stderr, "geforce-sriov: %s; nvidia-vgpud and nvidia-vgpu-mgr were not restarted and no GPU was\n"
				"re-probed%s. Install NVIDIA's vGPU host package so that `modprobe nvidia_vgpu_vfio` works, then\n"
				"run activate again\n", a->why, a->changed ? "; the changes made so far stay" : "");
			a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : a->changed ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
			return ACT_STOP;
		}
		a->changed = 1;
	}
	if ((r = daemon_restart(h, "nvidia-vgpud.service", &a->uncertain, a->why, sizeof(a->why)))) {
		if (r == -ETIMEDOUT)
			a->uncertain = 1;
		fprintf(stderr, "geforce-sriov: %s; no GPU was re-probed\n", a->why);
		a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
		return ACT_STOP;
	}
	a->changed = 1;
	/* A VM may have started since the preflight: vgpud restarted, nothing else changed yet. */
	if ((r = host_vgpu(h->sys, &a->vgpus))) {
		fprintf(stderr, "geforce-sriov: %s since the checks; nvidia-vgpud was restarted, nvidia-vgpu-mgr was not and no\n"
			"GPU was re-probed\n", r > 0 ? a->vgpus.first : "the VF list is unreadable");
		a->rc = GS_EXIT_FAIL;
		return ACT_STOP;
	}
	if ((r = daemon_start(h, "nvidia-vgpu-mgr.service", MGR_ACTIVE_WAIT_MS, &a->uncertain, a->why, sizeof(a->why)))) {
		if (r == -ETIMEDOUT)
			a->uncertain = 1;
		fprintf(stderr, "geforce-sriov: %s; no GPU was re-probed\n", a->why);
		a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
		return ACT_STOP;
	}
	a->changed = 1;
	return ACT_GO;
}

/* After the daemons have settled: each PF is re-probed, behind NVIDIA's lock. */
static enum act_next act_reprobe(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_activate_opts *o = a->o;
	unsigned int i;
	int r;

	if (o->settle_ms)
		gs_sleep_ms(o->settle_ms);
	for (i = 0; i < a->n; i++) {
		struct gs_card c;

		gs_card_inspect(h->sys, &a->cards[i].v.pf, &c);
		a->destructive = 1;
		if ((r = reprobe_pf(h, &c))) {
			if (r == GS_EXIT_REFUSED) {
				/* A client holds the GPU: the device state is known, so the guard is not needed. */
				if (!clear_marker(h))
					a->guard = 0;
				a->rc = GS_EXIT_FAIL;
			} else {
				a->rc = r == GS_EXIT_COLD_CYCLE ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			}
			fprintf(stderr, "geforce-sriov: activate stopped at %s after nvidia-vgpud and nvidia-vgpu-mgr were restarted;\n"
				"%u GPU(s) before it were re-probed, but no GPU has its vGPU type or VF power set yet; none is ready\n",
				c.pf.str, i);
			return ACT_STOP;
		}
	}
	return ACT_GO;
}

/* Every re-probe has run: the display stack comes back before the VFs' final configuration. */
static enum act_next act_display_finish(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	const struct gs_activate_opts *o = a->o;
	unsigned int i;
	int r;

	collect_vcards(a->cards, a->n, a->vc);
	if ((r = display_load(h, cfg, a->vc, a->n, a->why, sizeof(a->why)))) {
		/* A module load that did not finish leaves the display modules in an unknown state. */
		if (r == -ETIMEDOUT)
			a->uncertain = 1;
		fprintf(stderr, "geforce-sriov: display stack: %s\n", a->why);
		a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
		return ACT_STOP;
	}
	if (a->enable_blocked) {
		if ((r = enable_block_set(h, "# released by geforce-sriov after the enable window\n", a->why,
					  sizeof(a->why)))) {
			fprintf(stderr, "geforce-sriov: %s\nreleasing the module block failed\n", a->why);
			a->rc = GS_EXIT_FAIL;
			return ACT_STOP;
		}
		a->enable_blocked = 0;
	}
	for (i = 0; i < a->n; i++) {
		if ((r = finish_card(h, &a->cards[i].v, o->types_wait_ms ? o->types_wait_ms : VF_TYPES_WAIT_MS))) {
			a->rc = r == GS_EXIT_COLD_CYCLE ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
			return ACT_STOP;
		}
	}
	collect_vcards(a->cards, a->n, a->vc);
	slot_hint(h, o, a->vc, a->n);
	fb_slots_hold(h, cfg, a->vc, a->n);
	/* Host CUDA last: UVM is loaded after every VF is configured and no client exists. */
	if ((a->rc = host_cuda_load(h, cfg, &a->changed, &a->uncertain)))
		return ACT_STOP;
	return ACT_GO;
}

/* Complete success: the activating record and the guard go, then the display returns. */
static enum act_next act_success(struct activation *a)
{
	const struct gs_host *h = a->h;
	const struct gs_config *cfg = a->cfg;
	const struct gs_activate_opts *o = a->o;
	int r;

	if ((r = clear_activating(h))) {
		fprintf(stderr, "geforce-sriov: cannot remove %s/" GS_ACTIVATING_NAME ": %s\n", h->run_dir, strerror(-r));
		a->rc = GS_EXIT_FAIL;
		return ACT_STOP;
	}
	a->activating = 0;
	if ((r = clear_marker(h))) {
		fprintf(stderr, "geforce-sriov: cannot remove the activation lock: %s\n", strerror(-r));
		a->rc = GS_EXIT_FAIL;
		return ACT_STOP;
	}
	a->guard = 0;
	/* Before the display manager returns and before the units ordered after this one start. */
	device_nodes(h);
	if (a->dm_stopped) {
		const char *argv[] = { "systemctl", "start", a->plan.manager_unit, NULL };

		printf("systemctl start %s\n", a->plan.manager_unit);
		if (gs_host_run(h, argv, GS_MODULE_DEADLINE_MS)) {
			fprintf(stderr, "geforce-sriov: systemctl start %s failed; the GPUs are up and the display manager is not\n",
				a->plan.manager_unit);
			a->rc = GS_EXIT_FAIL;
		}
		a->dm_stopped = 0;
	}
	printf("activation finished\n");
	{
		int verdict = gs_check_vfs(h, cfg, o);

		if (a->rc == GS_EXIT_OK)
			a->rc = verdict;
	}
	return ACT_DONE;
}

/*
 * The stop path: release what a failed run still holds, give back what can be given back, and settle
 * the exit status and whether the guard stays.
 */
static void act_stop(struct activation *a)
{
	const struct gs_host *h = a->h;
	unsigned int i;

	/* Exit 5 means the guard is set. Whatever reported it, the guard is kept below. */
	if (a->rc == GS_EXIT_COLD_CYCLE)
		a->uncertain = 1;
	if (a->enable_blocked) {
		char w[256];

		if (enable_block_set(h, "# released by geforce-sriov after the enable window\n", w, sizeof(w)))
			fprintf(stderr, "geforce-sriov: warning: releasing the module block failed: %s\n", w);
		a->enable_blocked = 0;
	}
	/* An abort before any removal/unload/rescan gives the wake back and clears the guard. */
	if (!a->destructive) {
		for (i = 0; i < a->n; i++)
			if (a->cards[i].woke.pf.str[0])
				a->uncertain |= unwake(h, &a->cards[i].woke);
		if (a->guard && !a->uncertain && !clear_marker(h))
			a->guard = 0;
	}
	/* A write of the rollback that did not finish is uncertain state too: exit 5, with the guard set. */
	if (a->uncertain && !a->guard && !write_marker(h, a->bdfs, "stop"))
		a->guard = 1;
	if (a->dm_stopped) {
		const char *argv[] = { "systemctl", "start", a->plan.manager_unit, NULL };

		printf("systemctl start %s\n", a->plan.manager_unit);
		if (gs_host_run(h, argv, GS_MODULE_DEADLINE_MS))
			fprintf(stderr, "geforce-sriov: systemctl start %s failed; restore the display manager by hand\n",
				a->plan.manager_unit);
	}
	if (a->destructive && !a->guard && !a->uncertain) {
		/* A refused unbindLock: nothing was unbound, so the guard was removed above. */
		fprintf(stderr, "geforce-sriov: activation stopped before it finished. The state of the GPUs is known, so the activation\n"
			"lock was removed. Fix the problem named above, then run `geforce-sriov activate` again.\n");
	} else if (a->destructive) {
		fprintf(stderr, "geforce-sriov: activation stopped after it had started to change the GPUs or NVIDIA's modules. %s\n%s\n",
			a->uncertain ? "An operation did not finish in time, so the state of the GPUs is uncertain. The activation lock stays set." :
				    "The activation lock stays set.", gs_recovery_msg);
	} else if (a->guard) {
		fprintf(stderr, "geforce-sriov: activation stopped; the activation lock stays set.\n%s\n", gs_recovery_msg);
	}
	if (a->activating)
		fprintf(stderr, "geforce-sriov: %s/" GS_ACTIVATING_NAME " stays, so `geforce-sriov check` reports NOT READY until\n"
			"an activation finishes or the host reboots\n", h->run_dir);
	if (a->rc == GS_EXIT_OK)
		a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
	else if (a->changed && a->rc == GS_EXIT_REFUSED)
		a->rc = a->uncertain ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
	if (a->uncertain)
		a->rc = GS_EXIT_COLD_CYCLE;
}

int gs_activate(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	/* The phases, in the order they run. Each one goes on only when the one before it said so. */
	static enum act_next (*const phases[])(struct activation *) = {
		act_begin, act_preflight, act_dry_run, act_quiesce, act_power_load, act_resize, 
		act_unload_enable, act_daemons, act_reprobe, act_display_finish, act_success,
	};
	struct activation *a = calloc(1, sizeof(*a));
	enum act_next next = ACT_GO;
	int rc;

	if (!a)
		return GS_EXIT_FAIL;
	a->h = h;
	a->cfg = cfg;
	a->o = o;
	a->ari_eff = cfg->ari;
	for (size_t k = 0; next == ACT_GO && k < sizeof(phases) / sizeof(phases[0]); k++)
		next = phases[k](a);
	if (next == ACT_STOP)
		act_stop(a);
	rc = a->rc;
	free(a->cards);
	free(a);
	return rc;
}

/* ---- check --daemon-ready and the commands -------------------------------- */

int gs_cmd_activate(const struct gs_activate_opts *in)
{
	struct gs_activate_opts o = *in;
	struct gs_config cfg;
	char prog[GS_PATH_MAX];
	int r;

	if ((r = require_root("activate")))
		return r;
	if ((r = gs_lock()))
		return r;
	if (o.clear_guard) {
		char path[GS_PATH_MAX], *data, boot[64] = "", *line, *save = NULL;
		size_t len;

		if (gs_pathf(path, sizeof(path), "%s/" GS_ATTEMPT_NAME, GS_STATE_DIR)) {
			fprintf(stderr, "geforce-sriov: the activation lock path is too long\n");
			return GS_EXIT_FAIL;
		}
		if (gs_read_whole(path, &data, &len, 4096)) {
			printf("activation lock %s is not set\n", path);
			return GS_EXIT_OK;
		}
		printf("activation lock %s:\n%s", path, data);
		/* Without this boot's ID the attempt cannot be shown to be from an earlier boot: the guard stays. */
		if (gs_boot_id(boot, sizeof(boot)) || !boot[0]) {
			free(data);
			fprintf(stderr, "geforce-sriov: the ID of this boot cannot be read, so the unfinished attempt cannot be shown to be\n"
				"from an earlier boot. The activation lock stays.\n%s\n", gs_recovery_msg);
			return GS_EXIT_COLD_CYCLE;
		}
		for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (strncmp(line, "boot_id=", 8) == 0 && strcasecmp(line + 8, boot) == 0) {
				free(data);
				fprintf(stderr, "geforce-sriov: the unfinished attempt is from this boot, so the activation lock stays.\n%s\n",
					gs_recovery_msg);
				return GS_EXIT_COLD_CYCLE;
			}
		}
		free(data);
		if (clear_marker(&gs_host_live)) {
			fprintf(stderr, "geforce-sriov: cannot remove %s\n", path);
			return GS_EXIT_FAIL;
		}
		printf("activation lock cleared. A changed or unavailable boot ID cannot establish that the machine was powered off and on.\n"
		       "Run `geforce-sriov activate` when the cards are in a known state\n");
		return GS_EXIT_OK;
	}
	if ((r = load_live_config(gs_config_path(), &cfg)))
		return r;
	if (gs_find_program("systemctl", prog, sizeof(prog))) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: systemctl not found in /usr/sbin:/usr/bin:/sbin:/bin\n");
		return GS_EXIT_REFUSED;
	}
	o.plugin = gs_plugin_link();
	o.vgpu_config = GS_VGPU_CONFIG;
	if (!o.clear_guard && o.settle_ms == 0)
		o.settle_ms = MGR_SETTLE_MS;
	return gs_activate(&gs_host_live, &cfg, &o);
}

/* A module load of the early unit that timed out: exit 5, with the guard set as exit 5 promises. */
static void livepatch_guard(const struct gs_host *h, const char *bdfs)
{
	int r = write_marker(h, bdfs, "livepatch");

	if (r)
		fprintf(stderr, "geforce-sriov: cannot write the activation lock: %s\n", strerror(-r));
	else
		fprintf(stderr, "geforce-sriov: the activation lock is set, so activation does not run until it is cleared.\n%s\n", gs_recovery_msg);
}

/*
 * The early boot step: load the livepatch before udev lets nvidia bind. No removal, no rescan,
 * no deferral. With load_driver the unit also modprobes nvidia once the livepatch is verified
 * enabled with its transition complete: the autoload alias override still keeps nvidia_drm from
 * probing a selected PF before activation, but nvidia itself is loaded here instead of by udev.
 * Idempotent when the livepatch is already loaded with the requested parameters;
 * `geforce_sriov.skip` skips it.
 */
int gs_cmd_livepatch(const struct gs_livepatch_opts *o)
{
	struct gs_host fixture;
	const struct gs_host *h = o->host ? o->host : &gs_host_live;
	struct gs_config cfg;
	char bdfs[GS_BDF_LIST_STR_SIZE], p[GS_PATH_MAX], why[512];
	int r;

	if (h->system) {
		if ((r = require_root("livepatch")))
			return r;
		if ((r = gs_lock()))
			return r;
	}
	if (o->sysroot) {
		fixture = *h;
		fixture.sys = o->sysroot;
		/* Another sysfs tree is not the real system: its config is read as given. */
		fixture.system = 0;
		h = &fixture;
	}
	if (h->system) {
		if ((r = load_live_config(o->config_path ? o->config_path : gs_config_path(), &cfg)))
			return r;
	} else {
		char err[512];

		if (gs_config_load(o->config_path ? o->config_path : gs_config_path(), &cfg, err, sizeof(err))) {
			fprintf(stderr, "geforce-sriov: config: %s\n", err);
			return GS_EXIT_REFUSED;
		}
		if (cfg.nbdfs == 0) {
			fprintf(stderr, "geforce-sriov: no device selected (bdfs= in the config)\n");
			return GS_EXIT_REFUSED;
		}
	}
	if (cmdline_token(h->proc, "geforce_sriov.skip")) {
		printf("geforce_sriov.skip is on the kernel command line: the livepatch and nvidia are not loaded\n"
		       "and nothing changes. This boot has no NVIDIA driver; run `modprobe nvidia` if it is needed\n");
		return GS_EXIT_OK;
	}
	if (gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, h->state_dir)) {
		fprintf(stderr, "geforce-sriov: the activation lock path is too long\n");
		return GS_EXIT_COLD_CYCLE;
	}
	if (gs_exists(p)) {
		if (!guard_from_previous_boot(h, p)) {
			fprintf(stderr, "geforce-sriov: activation lock %s is set: an earlier activation did not finish. Nothing done.\n%s\n", p,
				gs_recovery_msg);
			return GS_EXIT_COLD_CYCLE;
		}
		printf("activation lock %s is from a previous boot; loading %s, the lock remains set\n", p,
		       o->load_driver ? "the livepatch and nvidia" : "the livepatch only");
	}
	if (cfg.ari != GS_SW_OFF && gs_cmdline_noari(h->proc)) {
		printf("kernel command line has pci=noari: loading %s with ari=0\n", GS_MODULE_NAME);
		cfg.ari = GS_SW_OFF;
	}
	if (gs_pathf(p, sizeof(p), "%s/kernel/livepatch", h->sys) || !gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: the kernel has no livepatch support (CONFIG_LIVEPATCH)\n");
		return GS_EXIT_REFUSED;
	}
	gs_addr_list_str(cfg.bdfs, cfg.nbdfs, bdfs, sizeof(bdfs));
	r = ensure_livepatch(h, &cfg, bdfs);
	if (r == GS_EXIT_COLD_CYCLE)
		livepatch_guard(h, bdfs);
	if (r)
		return r;
	if (o->load_driver) {
		struct gs_module_state m;

		gs_read_module_state(h->sys, &m);
		if (!m.lp_present || m.lp_enabled != 1 || m.lp_transition != 0) {
			fprintf(stderr, "geforce-sriov: refused, nvidia was not loaded: livepatch %s is %s (enabled=%u transition=%u), not present and enabled\n",
				GS_MODULE_NAME, m.lp_present ? "present but not ready" : "not present", m.lp_enabled,
				m.lp_transition);
			return GS_EXIT_REFUSED;
		}
		if (gs_module_loaded(h, "nvidia"))
			printf("nvidia is already loaded; not loading it again\n");
		else if ((r = gs_module_load(h, 0, "nvidia", NULL, 0, why, sizeof(why)))) {
			fprintf(stderr, "geforce-sriov: nvidia: %s\n", why);
			if (r == -ETIMEDOUT)
				livepatch_guard(h, bdfs);
			return r == -ETIMEDOUT ? GS_EXIT_COLD_CYCLE : GS_EXIT_FAIL;
		} else {
			printf("nvidia loaded after the livepatch, before udev binds a selected PF\n");
		}
	}
	printf("livepatch %s loaded for %s; activation does the rest\n", GS_MODULE_NAME, bdfs);
	return GS_EXIT_OK;
}
