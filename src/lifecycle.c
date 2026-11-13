// SPDX-License-Identifier: MIT
/*
 * devices, status and check: read-only commands over any sysfs root
 * (check also runs against a fixture host in the tests). activate (src/activate.c)
 * owns every change to the host.
 *
 * install --dry-run reports what a host needs and what activation would do; status
 * reports each fact separately; check is the gate before a VM start. None of
 * them writes anything, and none of them treats a record of an earlier run as
 * proof of the current state.
 */
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
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gs_hook.h"
#include "gs_lifecycle.h"
#include "gs_internal.h"

const char *gs_sw_name(int v)
{
	return v == GS_SW_OFF ? "off" : "on";
}

const char *gs_display_mode_name(enum gs_display_mode m)
{
	return m == GS_DISPLAY_ON ? "on" : m == GS_DISPLAY_OFF ? "off" : "auto";
}

/* <pf>/power/runtime_status into buf: 0, or -1 when it is absent or unreadable (buf ""). */
int gs_pf_runtime_status(const char *sysroot, const struct gs_addr *pf, char *buf, size_t len)
{
	char p[GS_PATH_MAX];

	buf[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/power/runtime_status", sysroot, pf->str) ||
	    gs_read_attr(p, buf, len)) {
		buf[0] = '\0';
		return -1;
	}
	return 0;
}

int gs_cmd_devices(void)
{
	unsigned int i;

	printf("geforce-sriov %s device table (exact IDs only; %u VFs per GPU by default)\n", GS_VERSION,
	       GS_DEFAULT_VFS);
	printf("Every listed card is handled by geforce-sriov. nvidia-vgpud sees each generation as one card:\n");
	for (i = 0; i < GS_GENERATION_COUNT; i++)
		printf("  %-7s presented as %s 10de:%04x, default vGPU type %u\n", gs_generation_name(i),
		       gs_targets[i].name, gs_targets[i].device, gs_vgpu_type_default(i));
	printf("\n%-9s %-7s %-6s %s\n", "DEVICE", "GEN", "DIE", "NAME");
	for (i = 0; i < GS_DEVICE_COUNT; i++) {
		const struct gs_device *e = &gs_devices[i];

		printf("10de:%04x %-7s %-6s %s\n", e->device, gs_generation_name(e->generation), e->die, e->name);
	}
	printf("\n`install --vfs N` (vfs=N in the config) sets the VF count: 1 up to the card's hardware default, or up\n"
	       "to %u on an Ada card that reports 1. `status` and `install --dry-run` show both counts. ARI decoding in the GPU does not work,\n"
	       "so above %u VFs the livepatch module geforce_sriov refuses to load when the card's VF layout check fails.\n", GS_DEFAULT_VFS,
	       GS_DEFAULT_VFS);
	printf("geforce-sriov does nothing for an ID not in this table. A presented capability alone is not a working vGPU.\n");
	return GS_EXIT_OK;
}

/* ---- shared readers ---------------------------------------------------- */

int gs_is_live_root(const char *sysroot)
{
	return strcmp(sysroot, GS_LIVE_SYSFS) == 0;
}

/* NVIDIA physical functions (no physfn) under sysroot, display class only when asked; sorted. */
static unsigned int scan_nvidia_pfs(const char *sysroot, struct gs_addr *out, unsigned int max, int display_only)
{
	char p[GS_PATH_MAX];
	unsigned int n = 0;
	struct dirent *de;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot))
		return 0;
	d = opendir(p);
	if (!d)
		return 0;
	while ((de = readdir(d)) && n < max) {
		struct gs_addr a;
		unsigned int vendor = 0, class_code = 0;
		char q[GS_PATH_MAX];

		if (gs_addr_parse(de->d_name, &a))
			continue;
		if (gs_pathf(q, sizeof(q), "%s/%s/vendor", p, a.str) || gs_read_hex(q, &vendor) ||
		    vendor != GS_VENDOR_NVIDIA)
			continue;
		if (display_only && (gs_pathf(q, sizeof(q), "%s/%s/class", p, a.str) || gs_read_hex(q, &class_code) ||
				     (class_code >> 16) != 0x03))
			continue;
		if (gs_pathf(q, sizeof(q), "%s/%s/physfn", p, a.str) || gs_exists(q))
			continue;
		out[n++] = a;
	}
	closedir(d);
	/* readdir order is unspecified: sort for stable output */
	for (unsigned int i = 1; i < n; i++)
		for (unsigned int j = i; j > 0 && strcmp(out[j - 1].str, out[j].str) > 0; j--) {
			struct gs_addr t = out[j];

			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}

/*
 * Config for read-only commands: an explicit --config, else $GEFORCE_SRIOV_CONFIG,
 * else the system file only on the live root (a fixture run never reads host config).
 */
int gs_diag_config(const struct gs_diag_opts *o, struct gs_config *cfg, const char **used, char *err,
		       size_t errlen)
{
	const char *path = o->config_path;
	const char *env = getenv(GS_CONFIG_ENV);

	if (!path && env && env[0])
		path = env;
	if (!path && gs_is_live_root(o->sysroot ? o->sysroot : GS_LIVE_SYSFS))
		path = GS_CONFIG_PATH;
	*used = path;
	if (!path) {
		gs_config_defaults(cfg);
		return 0;
	}
	return gs_config_load(path, cfg, err, errlen);
}

unsigned int gs_selected_devices(const struct gs_diag_opts *o, const struct gs_config *cfg,
				     const char *sysroot, struct gs_addr *out, const char **source)
{
	unsigned int i;

	if (o->ndevices) {
		for (i = 0; i < o->ndevices; i++)
			out[i] = o->devices[i];
		*source = "--device";
		return o->ndevices;
	}
	if (cfg->nbdfs) {
		for (i = 0; i < cfg->nbdfs; i++)
			out[i] = cfg->bdfs[i];
		*source = "config bdfs";
		return cfg->nbdfs;
	}
	*source = "all NVIDIA display functions";
	return scan_nvidia_pfs(sysroot, out, GS_CFG_MAX_BDFS, 1);
}

/* The same addresses in any order. */
int gs_same_selection(const struct gs_addr *a, unsigned int na, const struct gs_addr *b, unsigned int nb)
{
	if (na != nb)
		return 0;
	for (unsigned int i = 0; i < na; i++)
		if (!gs_addr_listed(b, nb, a[i].str))
			return 0;
	return 1;
}

/* Does the loaded module's bdfs parameter name pf, so that a presented capability is its own? */
int gs_module_selects(const struct gs_module_state *m, const struct gs_addr *pf)
{
	struct gs_addr list[GS_CFG_MAX_BDFS];
	unsigned int n = 0;
	char err[256];

	return m->loaded && gs_parse_addr_list("bdfs", m->bdfs, list, &n, err, sizeof(err)) == 0 &&
	       gs_addr_listed(list, n, pf->str);
}

/* ---- laptop power guards ------------------------------------------------------------------------ */

/* The table entry of pf by its device attribute (the kernel's copy of the ID, readable while the card is off). */
const struct gs_device *gs_pf_entry(const char *sysroot, const struct gs_addr *pf, unsigned int *device)
{
	char p[GS_PATH_MAX];
	unsigned int d = 0;

	*device = 0;
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/device", sysroot, pf->str) || gs_read_hex(p, &d))
		return NULL;
	*device = d;
	return gs_device_lookup(d);
}

unsigned int gs_laptop_gpus(const char *sys, const struct gs_addr *list, unsigned int n, struct gs_addr *out)
{
	unsigned int k = 0, device;

	for (unsigned int i = 0; i < n; i++) {
		const struct gs_device *e = gs_pf_entry(sys, &list[i], &device);

		if (e && e->laptop)
			out[k++] = list[i];
	}
	return k;
}

int gs_fb_ada_selected(const char *sys, const struct gs_addr *sel, unsigned int n)
{
	unsigned int device;

	for (unsigned int i = 0; i < n; i++) {
		const struct gs_device *e = gs_pf_entry(sys, &sel[i], &device);

		if (e && e->generation == GS_ADA)
			return 1;
	}
	return 0;
}

int gs_ada_placement_line(const char *sys, const struct gs_addr *pf, unsigned int numvfs, char *line, size_t len)
{
	struct gs_addr vf[GS_VFS_MAX];
	char why[512], ids[96] = "", v[64];
	int unreadable = 0, fixed = 0;

	if (numvfs > GS_VFS_MAX || gs_vf_list(sys, pf, numvfs, vf, why, sizeof(why)))
		unreadable = 1;
	for (unsigned int i = 0; !unreadable && i < numvfs; i++) {
		size_t n = strlen(ids);

		if (gs_read_dev_attr(sys, vf[i].str, "nvidia/placement_id", v, sizeof(v)) != 0) {
			unreadable = 1;
		} else if (strcmp(v, "Not Applicable") != 0) {
			if (v[0] < '0' || v[0] > '9')
				unreadable = 1;
			else if (n < sizeof(ids))
				snprintf(ids + n, sizeof(ids) - n, "%s%.12s", fixed ? ", " : "", v);
			fixed = 1;
		}
	}
	if (unreadable) {
		snprintf(line, len, "VM video memory: placement_id could not be read");
		return 0;
	}
	if (fixed) {
		snprintf(line, len, "VM video memory: fixed slots (placement_id %s): the loaded nvidia.ko does not carry the Ada placement "
			 "change. Run sudo geforce-sriov install, then power the machine off and on", ids);
		return 0;
	}
	snprintf(line, len, "VM video memory: taken from free memory when a VM starts (placement_id: Not Applicable)");
	return 1;
}

/* printf onto the end of buf (len bytes), cut short when it does not fit. */
static void add_to(char *buf, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void add_to(char *buf, size_t len, const char *fmt, ...)
{
	size_t n = strlen(buf);
	va_list ap;

	if (n >= len)
		return;
	va_start(ap, fmt);
	vsnprintf(buf + n, len - n, fmt, ap);
	va_end(ap);
}

/*
 * The build record at path is a sign only while the module it names is still installed with the recorded digest,
 * which is when a restore would still have something to put back. A record of a kernel that is gone, or of a module
 * that was replaced, stays after a finished uninstall of any version and says nothing. 1 with kernel set.
 */
static int record_live(const char *root, const char *path, char *kernel, size_t klen)
{
	char want[65], have[65], module[GS_PATH_MAX];

	if (!gs_exists(path) || !gs_record_value(path, "kernel", kernel, klen) || !gs_record_value(path, "module_sha256", want, sizeof(want)) ||
	    strchr(kernel, '/'))
		return 0;
	if (root[0] || gs_nvidia_module_path(kernel, module, sizeof(module)))
		gs_pathf(module, sizeof(module), "%s/lib/modules/%s/updates/dkms/nvidia.ko", root, kernel);
	return gs_sha256_file(module, have) == 0 && strcmp(have, want) == 0;
}

int gs_other_install(const char *root, const char *bindir, char *why, size_t len, char *fix, size_t fixlen)
{
	char p[GS_PATH_MAX], dir[GS_PATH_MAX], ver[64] = "", kernel[64], dkms_ver[64] = "", *data;
	size_t dlen;
	int found = 0, program = 0;
	struct dirent *de;
	DIR *d;

	why[0] = fix[0] = '\0';
	/* The stamp names the version that installed; the same version is no obstacle. */
	if (!gs_pathf(p, sizeof(p), "%s%s", root, GS_FORMAT_PATH) && !gs_read_whole(p, &data, &dlen, sizeof(ver) - 1)) {
		data[strcspn(data, "\n")] = '\0';
		gs_copy(ver, sizeof(ver), data);
		free(data);
		if (strcmp(ver, GS_VERSION) == 0)
			return 0;
	}
	/* The installed program counts only while the manifest lists it and it is unchanged: an uninstall keeps an edited one on purpose. */
	if (!gs_pathf(p, sizeof(p), "%s%s/geforce-sriov", root, bindir)) {
		struct manifest *m = calloc(1, sizeof(*m));
		char target[GS_PATH_MAX], mf[GS_PATH_MAX];
		struct entry *e = NULL;

		if (m && !gs_pathf(target, sizeof(target), "%s/geforce-sriov", bindir) &&
		    !gs_pathf(mf, sizeof(mf), "%s%s", root, GS_MANIFEST) && gs_manifest_load(mf, m) == 0)
			e = gs_manifest_find(m, 'f', target);
		program = e && gs_manifest_owns(p, e->sha) == OWN_YES;
		free(m);
	}
	if (program)
		add_to(why, len, "%sthe installed program %s/geforce-sriov", found++ ? "; " : "", bindir);
	if (!gs_pathf(p, sizeof(p), "%s%s", root, GS_DRIVER_BUILD) && record_live(root, p, kernel, sizeof(kernel))) {
		add_to(why, len, "%sthe build record %s (kernel %s)", found++ ? "; " : "", GS_DRIVER_BUILD, kernel);
		add_to(fix, fixlen, "  restore NVIDIA's module for %s: %s/geforce-sriov-driver --restore --kernel %s\n", kernel, bindir, kernel);
	}
	if (!gs_pathf(dir, sizeof(dir), "%s%s/%s", root, GS_STATE_DIR, GS_DRIVER_BUILDS_NAME) && (d = opendir(dir))) {
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.' || gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name) ||
			    !record_live(root, p, kernel, sizeof(kernel)))
				continue;
			add_to(why, len, "%sthe build record %s/%s/%s", found++ ? "; " : "", GS_STATE_DIR, GS_DRIVER_BUILDS_NAME, de->d_name);
			add_to(fix, fixlen, "  restore NVIDIA's module for %s: %s/geforce-sriov-driver --restore --kernel %s\n", kernel, bindir, kernel);
			}
		closedir(d);
	}
	if (!gs_pathf(p, sizeof(p), "%s/var/lib/dkms/%s", root, GS_DKMS_NAME) && (d = opendir(p))) {
		while ((de = readdir(d)))
			if (de->d_name[0] != '.') {
				gs_copy(dkms_ver, sizeof(dkms_ver), de->d_name);
				break;
			}
		closedir(d);
		add_to(why, len, "%sthe DKMS registration of %s", found++ ? "; " : "", GS_DKMS_NAME);
		add_to(fix, fixlen, "  dkms remove %s/%s --all\n", GS_DKMS_NAME, dkms_ver[0] ? dkms_ver : "<version>");
	}
	if (!gs_pathf(p, sizeof(p), "%s%s", root, GS_UNIT_WANTS_LIVEPATCH) && gs_exists(p)) {
		add_to(why, len, "%sthe enabled unit %s", found++ ? "; " : "", GS_UNIT_LIVEPATCH);
		add_to(fix, fixlen, "  systemctl disable %s %s\n", GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE);
	}
	if (!gs_pathf(p, sizeof(p), "%s%s/sysinit.target.wants/%s", root, GS_UNIT_DIR, GS_LEGACY_UNIT_PREPARE) && gs_exists(p)) {
		add_to(why, len, "%sthe enabled unit %s", found++ ? "; " : "", GS_LEGACY_UNIT_PREPARE);
		add_to(fix, fixlen, "  systemctl disable %s\n", GS_LEGACY_UNIT_PREPARE);
	}
	if (found && ver[0]) {
		char tmp[512];

		snprintf(tmp, sizeof(tmp), "version %s: %s", ver, why);
		gs_copy(why, len, tmp);
	}
	/* With the installed program present its uninstall does all of this; without it the user is given the commands. */
	if (program)
		fix[0] = '\0';
	return found;
}

void gs_other_install_refusal(const char *root, const char *why, const char *fix)
{
	static const char *const keys[] = { "bdfs=", "display=", "host_cuda=", "host_desktop=", "vfs=", "type_ada=", "type_ampere=" };
	char p[GS_PATH_MAX], *data, *text, *line, *save = NULL;
	size_t dlen;

	fprintf(stderr, "geforce-sriov: refused, nothing changed: another version of geforce-sriov than %s is installed on this host\n"
		"(%s). This version does not take it over. Run the installed version's `sudo geforce-sriov uninstall`,\n"
		"power the machine off and on, then run this install again.\n", GS_VERSION, why);
	fprintf(stderr, "The installed version's uninstall removes %s (your card selection and settings; a config you edited by hand is\n"
		"kept). The new install does not know them, so note the settings you want before you uninstall.\n", "/etc/geforce-sriov");
	/* The file is another version's: read as text, so a key this version does not know cannot hide the settings. */
	if (gs_pathf(p, sizeof(p), "%s%s", root, GS_CONFIG_PATH) || gs_read_whole(p, &data, &dlen, 65536 + 1)) {
		fprintf(stderr, "%s could not be read (it may be missing); the settings are not shown.\n", GS_CONFIG_PATH);
	} else if (!(text = malloc(dlen + 1))) {
		free(data);
	} else {
		unsigned int shown = 0;

		memcpy(text, data, dlen);
		text[dlen] = '\0';
		free(data);
		fprintf(stderr, "The installed settings (%s):\n", GS_CONFIG_PATH);
		for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
			for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
				if (strncmp(line, keys[k], strlen(keys[k])) == 0) {
					fprintf(stderr, "  %s\n", line);
					shown++;
					break;
				}
		if (!shown)
			fprintf(stderr, "  (none of bdfs, display, host_cuda, host_desktop, vfs, type_ada or type_ampere is set)\n");
		free(text);
	}
	if (fix[0])
		fprintf(stderr, "If that program is gone, clear what is named above by hand, as root, then power the machine off and on:\n%s", fix);
}

int gs_d3cold_usage(void)
{
	fprintf(stderr, "geforce-sriov: --disable-d3cold requires a selected laptop GPU. Nothing changed.\n"
		"Try 'geforce-sriov --help'.\n");
	return GS_EXIT_USAGE;
}

/* s at the end of the string in buf (len bytes), cut short when it does not fit. */
static void str_add(char *buf, size_t len, const char *s)
{
	size_t n = strlen(buf);

	snprintf(buf + n, len - n, "%s", s);
}

void gs_d3cold_why(char *buf, size_t len, const struct gs_addr *laptops, unsigned int n, const struct gs_addr *sel,
		   unsigned int nsel, const char *indent)
{
	unsigned int i;

	buf[0] = '\0';
	str_add(buf, len, n > 1 ? "no D3cold consent in the config for laptop GPUs " :
				  "no D3cold consent in the config for laptop GPU ");
	for (i = 0; i < n; i++) {
		if (i)
			str_add(buf, len, ", ");
		str_add(buf, len, laptops[i].str);
	}
	str_add(buf, len, ".\n");
	str_add(buf, len, indent);
	str_add(buf, len, "Run geforce-sriov setup, or:\n");
	str_add(buf, len, indent);
	/* --device replaces the selection and its grants: name them all, so the install keeps them. */
	str_add(buf, len, "  geforce-sriov install");
	for (i = 0; i < nsel; i++) {
		str_add(buf, len, " --device ");
		str_add(buf, len, sel[i].str);
	}
	str_add(buf, len, " --disable-d3cold\n");
	str_add(buf, len, indent);
	str_add(buf, len, "Why: docs/laptops.md#d3cold.");
}

/* n bytes of <dir>/config from off into buf: 0, or -1 when fewer can be read (unprivileged reads stop at 64). */
static int config_read(const char *dir, unsigned int off, void *buf, size_t n)
{
	char p[GS_PATH_MAX];
	ssize_t got;
	int fd;

	if (gs_pathf(p, sizeof(p), "%s/config", dir) || (fd = open(p, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	got = pread(fd, buf, n, (off_t)off);
	close(fd);
	return got == (ssize_t)n ? 0 : -1;
}

int gs_config_dword(const char *dir, unsigned int off, unsigned int *out)
{
	unsigned char b[4];

	if (config_read(dir, off, b, sizeof(b)))
		return -1;
	*out = (unsigned int)b[0] | (unsigned int)b[1] << 8 | (unsigned int)b[2] << 16 | (unsigned int)b[3] << 24;
	return 0;
}

/*
 * The config-read gate. A sysfs config read powers a D3cold device up first, so what it returns is the card's own
 * answer, which runtime_status is not: the first 256 bytes must read vendor 10de with device, and the control
 * register of the PCI power management capability must say D0. 0, or -1 with what was read in why.
 */
int gs_config_gate(const char *dir, unsigned int device, char *why, size_t len)
{
	unsigned char cfg[256];
	unsigned int pos, guard = 0;

	if (config_read(dir, 0, cfg, sizeof(cfg))) {
		snprintf(why, len, "its config space cannot be read");
		return -1;
	}
	if (((unsigned int)cfg[0] | (unsigned int)cfg[1] << 8) != GS_VENDOR_NVIDIA ||
	    ((unsigned int)cfg[2] | (unsigned int)cfg[3] << 8) != device) {
		snprintf(why, len, "its config space reads %02x%02x:%02x%02x, not 10de:%04x", cfg[1], cfg[0], cfg[3], cfg[2],
			 device);
		return -1;
	}
	pos = cfg[0x06] & 0x10 ? cfg[0x34] & 0xfcu : 0;
	while (pos >= 0x40 && pos < 0xf8 && cfg[pos] != 0x01 && guard++ < 48)
		pos = cfg[pos + 1] & 0xfcu;
	if (pos < 0x40 || pos >= 0xf8 || cfg[pos] != 0x01) {
		snprintf(why, len, "its config space shows no power management capability");
		return -1;
	}
	if (cfg[pos + 4] & 3) {
		snprintf(why, len, "its power management control register says D%u, not D0", cfg[pos + 4] & 3u);
		return -1;
	}
	return 0;
}

/* The sysfs directory of the PCI bridge above pf into out: 0, or -1 when its parent is not a PCI function. */
int gs_pf_bridge_dir(const char *sysroot, const struct gs_addr *pf, char *out, size_t len)
{
	char p[GS_PATH_MAX], real[PATH_MAX], *slash;
	struct gs_addr a;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sysroot, pf->str) || !realpath(p, real) ||
	    !(slash = strrchr(real, '/')))
		return -1;
	*slash = '\0';
	slash = strrchr(real, '/');
	if (!slash || gs_addr_parse(slash + 1, &a))
		return -1;
	gs_copy(out, len, real);
	return 0;
}

/*
 * Signs in the config space of a laptop GPU that it was powered off (D3cold, a cold reset) while its VFs were
 * enabled: VF Enable clear while the kernel still counts VFs. Reads need root (offsets above 64); what cannot
 * be read says nothing. Each finding goes to out[n]; returns how many.
 */
unsigned int gs_laptop_lost(const char *sysroot, const struct gs_card *c, char out[2][256])
{
	char dir[GS_PATH_MAX];
	unsigned int v, n = 0;

	if (!c->entry || !c->entry->laptop || gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, c->pf.str))
		return 0;
	if (c->has_sriov && c->numvfs > 0 && !gs_config_dword(dir, c->entry->sriov_pos + 8u, &v) && !(v & 1))
		snprintf(out[n++], 256, "%s: the GPU lost its SR-IOV state (D3cold reset); power the machine off and on, then "
			 "run setup again", c->pf.str);
	return n;
}

/*
 * Members of the card's IOMMU group other than its own functions (its domain:bus:slot): whether
 * its immediate bridge is one, how many others there are and the first of them. -1 when the group
 * is unknown or cannot be listed.
 */
int gs_iommu_peers(const char *sysroot, const struct gs_card *c, int *with_bridge, char *first, size_t len,
		       unsigned int *nothers)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	*with_bridge = 0;
	*nothers = 0;
	first[0] = '\0';
	if (!c->iommu_group[0] ||
	    gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/%s/devices", sysroot, c->iommu_group))
		return -1;
	d = opendir(p);
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		struct gs_addr a;

		if (de->d_name[0] == '.')
			continue;
		if (gs_addr_parse(de->d_name, &a) == 0 && a.domain == c->pf.domain && a.bus == c->pf.bus &&
		    a.slot == c->pf.slot)
			continue;
		if (c->bridge_is_pci && strcmp(de->d_name, c->bridge.str) == 0) {
			*with_bridge = 1;
			continue;
		}
		if (!(*nothers)++)
			gs_copy(first, len, de->d_name);
	}
	closedir(d);
	return 0;
}

/* Every parent directory of path must be a root-owned directory nobody else can write. */
static int parents_safe(const char *path, char *why, size_t len)
{
	char dir[GS_PATH_MAX], *slash;
	struct stat st;

	if (gs_pathf(dir, sizeof(dir), "%s", path))
		return snprintf(why, len, "config path too long"), -1;
	while ((slash = strrchr(dir, '/'))) {
		if (slash == dir)
			slash[1] = '\0';
		else
			*slash = '\0';
		if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
			return snprintf(why, len, "%.300s must be a directory owned by root and writable only by root", dir),
			       -1;
		if (strcmp(dir, "/") == 0)
			break;
	}
	return 0;
}

static int stat_safe(const char *path, const struct stat *st, char *why, size_t len)
{
	if (!S_ISREG(st->st_mode))
		return snprintf(why, len, "%.300s is not a regular file", path), -1;
	if (st->st_uid != 0)
		return snprintf(why, len, "%.300s is not owned by root", path), -1;
	if (st->st_mode & 022)
		return snprintf(why, len, "%.300s is group- or world-writable", path), -1;
	return parents_safe(path, why, len);
}

int gs_config_file_safe(const char *path, char *why, size_t len)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return snprintf(why, len, "%.300s: %s", path, strerror(errno)), -1;
	return stat_safe(path, &st, why, len);
}

int gs_module_present_for(const char *root, const char *release)
{
	static const char *const dirs[] = { "updates/dkms", "extra", "updates" };
	static const char *const exts[] = { ".ko", ".ko.xz", ".ko.zst", ".ko.gz" };
	char p[GS_PATH_MAX];

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		for (size_t j = 0; j < sizeof(exts) / sizeof(exts[0]); j++)
			if (!gs_pathf(p, sizeof(p), "%s/lib/modules/%s/%s/%s%s", root, release, dirs[i],
				      GS_MODULE_NAME, exts[j]) && gs_exists(p))
				return 1;
	return 0;
}

int gs_installed_module_present(void)
{
	struct utsname u;

	return uname(&u) == 0 && gs_module_present_for("", u.release);
}

/* One kernel for gs_kernels_without_livepatch: added to list when it is installed, has no livepatch module, and is not listed yet. */
static unsigned int kernel_without(const char *root, const char *release, char *list, size_t len, unsigned int n)
{
	char p[GS_PATH_MAX], name[GS_PATH_MAX];
	size_t at = strlen(list), rl = strlen(release);

	/* A kernel that is no longer installed needs nothing. */
	if (!release[0] || release[0] == '.' || strchr(release, '/') || gs_pathf(p, sizeof(p), "%s/lib/modules/%s/kernel", root, release) ||
	    !gs_exists(p) || gs_module_present_for(root, release))
		return n;
	for (const char *have = list; (have = strstr(have, release)); have += rl)
		if ((have == list || have[-1] == ' ') && (have[rl] == ',' || !have[rl]))
			return n;
	if (gs_pathf(name, sizeof(name), "%s%s", n ? ", " : "", release) || at + strlen(name) >= len)
		return n;
	memcpy(list + at, name, strlen(name) + 1);
	return n + 1;
}

unsigned int gs_kernels_without_livepatch(const char *root, const char *state_dir, char *list, size_t len)
{
	char dir[GS_PATH_MAX], record[GS_PATH_MAX], release[256];
	unsigned int n = 0;
	struct dirent *e;
	DIR *d;

	list[0] = '\0';
	/* The published record names the kernel of the latest build; the records of the others are kept beside it. */
	if (!gs_pathf(record, sizeof(record), "%s/driver-build", state_dir) && gs_record_value(record, "kernel", release, sizeof(release)))
		n = kernel_without(root, release, list, len, n);
	if (gs_pathf(dir, sizeof(dir), "%s/" GS_DRIVER_BUILDS_NAME, state_dir) || !(d = opendir(dir)))
		return n;
	while ((e = readdir(d)))
		n = kernel_without(root, e->d_name, list, len, n);
	closedir(d);
	return n;
}

static int has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n > m && strcmp(s + n - m, suffix) == 0;
}

/*
 * Does this file name an nvidia module? anywhere = 0: a module-list line starting with
 * "nvidia" (modules-load.d, /etc/modules, initramfs-tools/modules); anywhere = 1: any
 * non-comment line mentioning nvidia (dracut add_drivers/force_drivers).
 */
static int file_names_nvidia(const char *path, int anywhere)
{
	char *data, *line, *save = NULL;
	size_t dlen;
	int found = 0;

	if (gs_read_whole(path, &data, &dlen, 1 << 20))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line == '#')
			continue;
		found = anywhere ? strstr(line, "nvidia") != NULL : strncmp(line, "nvidia", 6) == 0;
	}
	free(data);
	return found;
}

/* First *.conf in dir that names nvidia, copied to where. */
static int dir_names_nvidia(const char *dir, int anywhere, char *where, size_t len)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char p[GS_PATH_MAX];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (de = readdir(d))) {
		if (!has_suffix(de->d_name, ".conf") || gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name))
			continue;
		if (file_names_nvidia(p, anywhere)) {
			gs_copy(where, len, p);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

/* Boot-time loading of nvidia: 1 = modules-load list, 2 = initramfs configuration, 0 = none found. */
int gs_nvidia_autoload(char *where, size_t len)
{
	static const char *const load_dirs[] = { "/etc/modules-load.d", "/run/modules-load.d",
						 "/usr/lib/modules-load.d", "/lib/modules-load.d" };
	static const char *const dracut_dirs[] = { "/etc/dracut.conf.d", "/usr/lib/dracut/dracut.conf.d" };
	size_t i;

	for (i = 0; i < sizeof(load_dirs) / sizeof(load_dirs[0]); i++)
		if (dir_names_nvidia(load_dirs[i], 0, where, len))
			return 1;
	if (file_names_nvidia("/etc/modules", 0)) {
		gs_copy(where, len, "/etc/modules");
		return 1;
	}
	if (file_names_nvidia("/etc/initramfs-tools/modules", 0)) {
		gs_copy(where, len, "/etc/initramfs-tools/modules");
		return 2;
	}
	if (file_names_nvidia("/etc/dracut.conf", 1)) {
		gs_copy(where, len, "/etc/dracut.conf");
		return 2;
	}
	for (i = 0; i < sizeof(dracut_dirs) / sizeof(dracut_dirs[0]); i++)
		if (dir_names_nvidia(dracut_dirs[i], 1, where, len))
			return 2;
	return 0;
}

int gs_unit_file_present(const char *name)
{
	char p[GS_PATH_MAX];

	for (size_t i = 0; i < sizeof(gs_unit_dirs) / sizeof(gs_unit_dirs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s/%s", gs_unit_dirs[i], name) && gs_exists(p))
			return 1;
	return 0;
}

int gs_driver_version_line(const char *ver, char *line, size_t len)
{
	int confirmed = gs_version_confirmed(GS_VERSION_HOST, ver);

	gs_version_format(confirmed ? GS_VERSION_MSG_CONFIRMED : GS_VERSION_MSG_STATUS, ver, line, len);
	return !confirmed;
}

/* The GNU build-id of an ELF64 file of the host's byte order (a kernel module) as lowercase hex: 0, or -1. */
static int elf_build_id(const char *path, char *out, size_t len)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	const unsigned char order = ELFDATA2LSB;
#else
	const unsigned char order = ELFDATA2MSB;
#endif
	Elf64_Ehdr eh;
	int fd = open(path, O_RDONLY | O_CLOEXEC), r = -1;

	out[0] = '\0';
	if (fd < 0)
		return -1;
	if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != order || eh.e_shentsize != sizeof(Elf64_Shdr))
		goto out;
	for (unsigned int i = 0; i < eh.e_shnum && r; i++) {
		Elf64_Shdr sh;
		unsigned char *buf;

		if (pread(fd, &sh, sizeof(sh), (off_t)(eh.e_shoff + (Elf64_Off)i * sizeof(sh))) != (ssize_t)sizeof(sh))
			break;
		if (sh.sh_type != SHT_NOTE || sh.sh_size > 65536 || !(buf = malloc(sh.sh_size ? sh.sh_size : 1)))
			continue;
		if (pread(fd, buf, sh.sh_size, (off_t)sh.sh_offset) == (ssize_t)sh.sh_size)
			r = gs_note_build_id(buf, sh.sh_size, out, len);
		free(buf);
	}
out:
	close(fd);
	return r;
}

static enum gs_build_state build_read(const char *record, const char *release, const char *module, struct gs_build *b)
{
	char *data, *line, *save = NULL;
	size_t dlen;

	memset(b, 0, sizeof(*b));
	b->driver_version_confirmed = -1;
	b->display = -1;
	b->host_cuda = -1;
	gs_copy(b->record, sizeof(b->record), record);
	gs_copy(b->release, sizeof(b->release), release);
	gs_copy(b->module, sizeof(b->module), module ? module : "");
	if (b->module[0])
		elf_build_id(b->module, b->found_id, sizeof(b->found_id));
	if (gs_read_whole(record, &data, &dlen, 1 << 16))
		return b->state = GS_BUILD_ABSENT;
	b->record_read = 1;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "kernel=", 7) == 0)
			gs_copy(b->rec_kernel, sizeof(b->rec_kernel), line + 7);
		else if (strncmp(line, "module_sha256=", 14) == 0)
			gs_copy(b->rec_sha256, sizeof(b->rec_sha256), line + 14);
		else if (strncmp(line, "module_build_id=", 16) == 0)
			gs_copy(b->rec_id, sizeof(b->rec_id), line + 16);
		else if (strncmp(line, "driver_version=", 15) == 0)
			gs_copy(b->driver_version, sizeof(b->driver_version), line + 15);
		else if (strcmp(line, "driver_version_confirmed=0") == 0)
			b->driver_version_confirmed = 0;
		else if (strcmp(line, "driver_version_confirmed=1") == 0)
			b->driver_version_confirmed = 1;
		else if (strcmp(line, "display=0") == 0)
			b->display = 0;
		else if (strcmp(line, "display=1") == 0)
			b->display = 1;
		else if (strcmp(line, "host_cuda=0") == 0)
			b->host_cuda = 0;
		else if (strcmp(line, "host_cuda=1") == 0)
			b->host_cuda = 1;
		else if (strncmp(line, "client_accept=", 14) == 0 && strspn(line + 14, "0123456789.") == strlen(line + 14))
			gs_copy(b->client_accept, sizeof(b->client_accept), line + 14);
	}
	free(data);
	if (strcmp(b->rec_kernel, release) != 0)
		return b->state = GS_BUILD_ABSENT;
	return b->state = b->found_id[0] && strcmp(b->found_id, b->rec_id) == 0 ? GS_BUILD_MATCHES : GS_BUILD_PRESENT;
}

/*
 * The published record names one kernel. The driver step keeps the record of every other kernel it
 * built for beside it, in GS_DRIVER_BUILDS_NAME/<release>, so the build of the running kernel is
 * still known after a build for a new one.
 */
enum gs_build_state gs_driver_build_read(const char *record, const char *release, const char *module,
					 struct gs_build *b)
{
	const char *slash = strrchr(record, '/');
	char other[GS_PATH_MAX];
	struct gs_build kept;

	if (build_read(record, release, module, b) != GS_BUILD_ABSENT || !slash || strchr(release, '/') ||
	    gs_pathf(other, sizeof(other), "%.*s/" GS_DRIVER_BUILDS_NAME "/%s", (int)(slash - record), record, release) ||
	    !gs_exists(other))
		return b->state;
	if (build_read(other, release, module, &kept) != GS_BUILD_ABSENT)
		*b = kept;
	return b->state;
}

/* The first line `modinfo -k release -n nvidia` prints: the module file the kernel would load. 0, or -1. */
static int modinfo_path(const char *release, char *out, size_t len)
{
	const char *const argv[] = { "modinfo", "-k", release, "-n", "nvidia", NULL };

	gs_run_capture(argv, out, len);
	out[strcspn(out, "\n")] = '\0';
	if (out[0] != '/') {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

int gs_nvidia_module_path(const char *release, char *out, size_t len)
{
	if (modinfo_path(release, out, len) == 0)
		return 0;
	if (gs_pathf(out, len, "/lib/modules/%s/updates/dkms/nvidia.ko", release) || !gs_exists(out)) {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

int gs_loaded_not_installed(const char *sysroot, const struct gs_build_src *src)
{
	char line[256];
	struct gs_build b;

	return gs_build_identity(src, gs_is_live_root(sysroot), &b) && gs_loaded_line(sysroot, &b, line, sizeof(line));
}

/*
 * The sentence status, activate and check print: 0 when the module is the driver step's build. A
 * mismatch names both identities, the module's and the recorded one.
 */
int gs_build_line(const struct gs_build *b, char *line, size_t len)
{
	char found[GS_PATH_MAX + 160];
	/* The command that builds for this kernel what the config selects, from the inputs install kept. */
	const char *again = "`sudo geforce-sriov rebuild`";

	if (b->state == GS_BUILD_MATCHES)
		return snprintf(line, len, "nvidia.ko for %s is the build geforce-sriov installed: %s, build-id %s", b->release,
				b->module, b->found_id) < 0;
	if (b->state == GS_BUILD_ABSENT) {
		if (!b->record_read)
			snprintf(found, sizeof(found), "%s is absent", b->record);
		else if (b->rec_kernel[0])
			snprintf(found, sizeof(found), "%s records kernel %s", b->record, b->rec_kernel);
		else
			snprintf(found, sizeof(found), "%s names no kernel", b->record);
		snprintf(line, len, "nvidia.ko for %s is not known to be the build geforce-sriov installed: %s; build it again "
			 "for this kernel (%s)%s", b->release, found, again,
			 b->record_read ? "" :
			 "; if the module already is that build, `install --driver=skip` keeps it");
		return 1;
	}
	if (!b->module[0])
		snprintf(found, sizeof(found), "no nvidia.ko found (modinfo -k %s -n nvidia)", b->release);
	else if (!b->found_id[0])
		snprintf(found, sizeof(found), "%s has no readable GNU build-id", b->module);
	else
		snprintf(found, sizeof(found), "%s has build-id %s", b->module, b->found_id);
	snprintf(line, len, "nvidia.ko for %s is not the build geforce-sriov installed: %s, %s records build-id %s; build it "
		 "again for this kernel (%s)", b->release, found, b->record,
		 b->rec_id[0] ? b->rec_id : "(none)", again);
	return 1;
}
