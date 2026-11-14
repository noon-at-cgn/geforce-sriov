// SPDX-License-Identifier: MIT
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

/*
 * ---- host checks: gs_hostcheck, the checks `install --dry-run` and setup run before install (GS_HOSTCHECK_PRE) and
 * status runs after it (GS_HOSTCHECK_POST). Rows that status prints itself are not repeated in POST. ----
 */

struct tally {
	unsigned int fail, warn;
	int post;          /* status's call: no OK lines, and none of the lines that only describe */
	const char *head;  /* printed before the first line that is shown */
};

static void check(struct tally *t, const char *level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void check(struct tally *t, const char *level, const char *fmt, ...)
{
	va_list ap;

	if (strcmp(level, "FAIL") == 0)
		t->fail++;
	else if (strcmp(level, "WARN") == 0)
		t->warn++;
	else if (t->post && strcmp(level, "OK") == 0)
		return;
	if (t->head) {
		fputs(t->head, stdout);
		t->head = NULL;
	}
	printf("[%-4s] ", level);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/* Each line of text, prefixed with indent (continuation lines of a check). */
static void print_indented(const char *indent, const char *text)
{
	while (*text) {
		size_t n = strcspn(text, "\n");

		printf("%s%.*s\n", indent, (int)n, text);
		text += n + (text[n] == '\n');
	}
}

/*
 * Doctor's note for every selected Ada card: what the package does not provide. With several
 * confirmed versions it names each one, and each patched plugin copy with its version.
 */
static void ada_stack_note(char *buf, size_t len)
{
	size_t count, voff = 0, doff = 0;
	const struct gs_confirmed_version *rows = gs_confirmed_versions(&count);
	char versions[256] = "", digests[1024] = "";

	for (size_t i = 0; i < count; i++) {
		int n = snprintf(versions + voff, sizeof(versions) - voff, "%s%s", voff ? " or " : "", rows[i].host);

		if (n > 0 && (size_t)n < sizeof(versions) - voff)
			voff += (size_t)n;
		if (!rows[i].patched_plugin)
			continue;
		if (count > 1)
			n = snprintf(digests + doff, sizeof(digests) - doff, "%s%s (%s)", doff ? " or " : "",
				     rows[i].patched_plugin, rows[i].host);
		else
			n = snprintf(digests + doff, sizeof(digests) - doff, "%s", rows[i].patched_plugin);
		if (n > 0 && (size_t)n < sizeof(digests) - doff)
			doff += (size_t)n;
	}
	versions[voff] = digests[doff] = 0;
	snprintf(buf, len, "Ada needs your own NVIDIA host driver setup (%s, proprietary, CPU-RM mode with NVreg_EnableGpuFirmware=0): "
		 "nvidia.ko rebuilt with the scheduler, Ada probe-gate and VF-count byte patches, and the patched plugin copy of "
		 "libnvidia-vgpu.so, SHA-256 %s (docs/driver-patch.md#revisions). `install` builds both "
		 "with geforce-sriov-driver unless --driver=skip is given", versions, digests);
}

/*
 * What the device table says about the card: the VF override is a warning; a VF count above what the
 * card offers fails (activate refuses it).
 */
static void entry_checks(struct tally *t, const struct gs_card *c, const struct gs_config *cfg)
{
	const struct gs_device *e = c->entry;
	char why[512];

	if (!t->post)
		printf("  %s (%s, %s) -> professional counterpart 10de:%04x %s\n", e->name, e->die, gs_generation_name(e->generation),
		       gs_target_of(e)->device, gs_target_of(e)->name);
	gs_vfs_describe(e, cfg, why, sizeof(why));
	if (!t->post)
		printf("  VFs: %s\n", why);
	switch (gs_vfs_check(e, cfg, why, sizeof(why))) {
	case -1:
		check(t, "FAIL", "%s: %s; activate refuses it", c->pf.str, why);
		break;
	case 1:
		check(t, "WARN", "%s: %s", c->pf.str, why);
		break;
	}
	if (e->generation == GS_ADA) {
		char note[2048];

		ada_stack_note(note, sizeof(note));
		check(t, "INFO", "%s: %s", c->pf.str, note);
	}
}

/*
 * Selected cards of an Ada card's own and another generation: the first Ada card in ada, the first
 * other one in other (its generation in *gen). 1 when both exist.
 */
static int selection_mixes_ada(const char *sysroot, const struct gs_addr *sel, unsigned int n, char *ada,
			       char *other, size_t len, unsigned int *gen)
{
	ada[0] = other[0] = '\0';
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;

		gs_card_inspect(sysroot, &sel[i], &c);
		if (!c.present || !c.entry)
			continue;
		if (c.entry->generation == GS_ADA && !ada[0]) {
			snprintf(ada, len, "%s", c.pf.str);
		} else if (c.entry->generation != GS_ADA && !other[0]) {
			snprintf(other, len, "%s", c.pf.str);
			*gen = c.entry->generation;
		}
	}
	return ada[0] && other[0];
}

/* Enabled VFs (virtfn0..numvfs-1) whose IOMMU group is the PF's; the first goes to first. */
static unsigned int vfs_sharing_pf_group(const char *sysroot, const struct gs_card *c, char *first, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	unsigned int i, n = 0;

	first[0] = '\0';
	if (!c->iommu_group[0])
		return 0;
	for (i = 0; i < c->numvfs && i < 256; i++) {
		struct gs_addr a;
		struct gs_card vf;
		const char *base;
		ssize_t k;

		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sysroot, c->pf.str, i))
			break;
		k = readlink(p, target, sizeof(target) - 1);
		if (k <= 0)
			continue;
		target[k] = '\0';
		base = strrchr(target, '/');
		if (gs_addr_parse(base ? base + 1 : target, &a))
			continue;
		gs_card_inspect(sysroot, &a, &vf);
		if (strcmp(vf.iommu_group, c->iommu_group) == 0 && !n++)
			gs_copy(first, len, a.str);
	}
	return n;
}

/* The card's IOMMU grouping as the kernel reports it. */
static void iommu_checks(struct tally *t, const char *sysroot, const struct gs_card *c)
{
	char first[256], what[512];
	unsigned int nothers;
	int with_bridge;

	if (gs_iommu_peers(sysroot, c, &with_bridge, first, sizeof(first), &nothers))
		return;
	if (!with_bridge && !nothers) {
		check(t, "INFO", "%s: IOMMU group %s holds only this card's functions", c->pf.str, c->iommu_group);
		return;
	}
	if (!nothers) {
		check(t, "INFO", "%s: IOMMU group %s also holds its bridge %s; whether VFIO accepts that depends on the bridge's driver",
		      c->pf.str, c->iommu_group, c->bridge.str);
		return;
	}
	if (with_bridge)
		snprintf(what, sizeof(what), "its bridge %s and %u other device(s), first %s", c->bridge.str, nothers, first);
	else
		snprintf(what, sizeof(what), "%u other device(s), first %s", nothers, first);
	check(t, "WARN", "%s: IOMMU group %s also holds %s. VFs in that group are not isolated from those devices or each other, and VFIO may refuse them (an ACS override adds no real isolation)",
	      c->pf.str, c->iommu_group, what);
}

/* One NVIDIA physical function as the identity hook reads it from sysfs. */
struct id_fn {
	struct gs_addr a;
	unsigned int device;
	int display;
};

#define ID_SCAN_MAX (8u * GS_MAX_PFS)

/*
 * The PCI scan the identity hook relies on, read as strictly as the hook reads it: every entry of
 * bus/pci/devices must be a canonical address with readable vendor and device, and every NVIDIA
 * physical function (no physfn) a readable class. Returns -1 and the offending entry in bad
 * otherwise; the check then cannot confirm the mapping. Sorted by address.
 */
static int scan_identity(const char *sysroot, struct id_fn *out, unsigned int *n, char *bad, size_t len)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	int r = 0;
	DIR *d;

	*n = 0;
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot) || !(d = opendir(p))) {
		snprintf(bad, len, "%s/bus/pci/devices", sysroot);
		return -1;
	}
	while (!r && (de = readdir(d))) {
		char q[GS_PATH_MAX];
		unsigned int vendor = 0, device = 0, class_code = 0;
		struct gs_addr a;

		if (de->d_name[0] == '.')
			continue;
		if (gs_addr_parse(de->d_name, &a) ||
		    gs_pathf(q, sizeof(q), "%s/%s/vendor", p, a.str) || gs_read_hex(q, &vendor) ||
		    gs_pathf(q, sizeof(q), "%s/%s/device", p, a.str) || gs_read_hex(q, &device)) {
			r = -1;
		} else if (vendor != GS_VENDOR_NVIDIA ||
			   (!gs_pathf(q, sizeof(q), "%s/%s/physfn", p, a.str) && gs_exists(q))) {
			continue;
		} else if (gs_pathf(q, sizeof(q), "%s/%s/class", p, a.str) || gs_read_hex(q, &class_code) ||
			   *n == ID_SCAN_MAX) {
			r = -1;
		} else {
			out[*n].a = a;
			out[*n].device = device;
			out[(*n)++].display = (class_code >> 16) == 0x03;
			continue;
		}
		snprintf(bad, len, "%s/%.64s", p, de->d_name);
	}
	closedir(d);
	for (unsigned int i = 1; i < *n; i++)
		for (unsigned int j = i; j > 0 && strcmp(out[j - 1].a.str, out[j].a.str) > 0; j--) {
			struct id_fn tmp = out[j];

			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	return r;
}

/*
 * The identity hook's decision (hook/gs_hook.c) for a selection. A selected card is mappable when
 * it is an NVIDIA display physical function in the device table and the only one in its
 * domain:bus:slot. At runtime the hook attributes each PCI-info reply to a bus address through the
 * driver's own GPU-ID and PCI-info queries and rewrites only mappable selected cards. Only when
 * those queries fail does it fall back to the device ID, and only for an ID that every NVIDIA
 * physical function having it (any class) is a mapped card. HOST is reported only for a mapped
 * card whose address query succeeds; the fallback never forces it. Whether the installed driver
 * answers those queries is unknown until nvidia-vgpud runs; the checks never issue them.
 */
static void identity_checks(struct tally *t, const char *sysroot, const struct gs_addr *sel, unsigned int nsel,
			    int force_host)
{
	struct id_fn fns[ID_SCAN_MAX];
	struct gs_addr mapped[GS_CFG_MAX_BDFS];
	unsigned int mdev[GS_CFG_MAX_BDFS], nfns, nmapped = 0, i, j;
	char bad[GS_PATH_MAX + 80];

	if (scan_identity(sysroot, fns, &nfns, bad, sizeof(bad))) {
		check(t, "WARN", "identity: cannot confirm the mapping: %s is unreadable or invalid, or there are more than %u NVIDIA functions. The hook reads the same entries and may leave the selected GPUs unchanged",
		      bad, ID_SCAN_MAX);
		return;
	}
	for (i = 0; i < nsel; i++) {
		struct gs_card c;
		unsigned int inslot = 0;

		gs_card_inspect(sysroot, &sel[i], &c);
		if (!c.present || !c.entry || c.is_vf || (c.class_code >> 16) != 0x03) {
			check(t, "INFO", "identity: selected %s is %s; the hook skips it", sel[i].str,
			      !c.present ? "absent" : "not an NVIDIA display physical function in the device table");
			continue;
		}
		for (j = 0; j < nfns; j++)
			inslot += fns[j].display && fns[j].a.domain == c.pf.domain && fns[j].a.bus == c.pf.bus &&
				  fns[j].a.slot == c.pf.slot;
		if (inslot != 1) {
			check(t, "WARN", "identity: selected %s shares its slot with another NVIDIA display function; the hook leaves an ambiguous slot unchanged",
			      c.pf.str);
			continue;
		}
		mapped[nmapped] = c.pf;
		mdev[nmapped++] = c.device;
		check(t, "OK", "identity: selected %s (10de:%04x) matches by bus address; nvidia-vgpud sees it as 10de:%04x %s (the driver's address queries are checked only when nvidia-vgpud runs)",
		      c.pf.str, c.device, gs_target_of(c.entry)->device, gs_target_of(c.entry)->name);
	}
	/* The device-ID fallback, per mapped card: refused while another, unmapped PF has the same ID. */
	for (i = 0; i < nmapped; i++) {
		for (j = 0; j < nfns; j++) {
			int selected = gs_addr_listed(sel, nsel, fns[j].a.str);

			if (fns[j].device != mdev[i] || gs_addr_listed(mapped, nmapped, fns[j].a.str))
				continue;
			if (!selected)
				check(t, "INFO", "identity: unselected %s has the same device ID; matching by bus address leaves it unchanged and rewrites only %s",
				      fns[j].a.str, mapped[i].str);
			check(t, "WARN", "identity: %s %s has the same device ID 10de:%04x as selected %s; if the driver's address queries fail, the device-ID fallback is refused and both stay 10de:%04x",
			      selected ? "selected but unmappable" : "unselected", fns[j].a.str, mdev[i], mapped[i].str,
			      mdev[i]);
			break;
		}
	}
	for (j = 0; j < nfns; j++)
		if (fns[j].display && !gs_addr_listed(sel, nsel, fns[j].a.str))
			check(t, "INFO", "identity: unselected NVIDIA GPU %s (10de:%04x) is not rewritten", fns[j].a.str,
			      fns[j].device);
	if (force_host && nmapped)
		check(t, "INFO", "identity_force_host: HOST is reported only for a selected card whose bus address the driver returns; other virtualization-mode replies stay unchanged");
}

/*
 * What the *.conf files of a modprobe.d directory say about nvidia, in name order: the first file other
 * than the driver step's own that sets NVreg_EnableGpuFirmware to a value other than 0 (on any
 * `options nvidia` line, among any other options), the first that sets it to 0, and the first file with
 * a `blacklist nvidia` line and the first with an `install nvidia /bin/false` line, which together hold
 * nvidia back.
 */
struct modprobe_view {
	char other[GS_PATH_MAX], other_value[64], zero[GS_PATH_MAX];
	char blacklist[GS_PATH_MAX], install_false[GS_PATH_MAX];
};

static void modprobe_file(const char *path, int own, struct modprobe_view *v)
{
	char *data, *line, *save = NULL;
	size_t len;

	if (gs_read_whole(path, &data, &len, GS_CONFIG_MAX_BYTES))
		return;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *tok[3], *rest = NULL, *t;
		unsigned int n = 0;

		for (t = strtok_r(line, " \t\r", &rest); t && n < 3; t = n < 3 ? strtok_r(NULL, " \t\r", &rest) : NULL)
			tok[n++] = t;
		if (n >= 2 && strcmp(tok[0], "blacklist") == 0 && strcmp(tok[1], "nvidia") == 0 && !v->blacklist[0])
			gs_copy(v->blacklist, sizeof(v->blacklist), path);
		if (n == 3 && strcmp(tok[0], "install") == 0 && strcmp(tok[1], "nvidia") == 0 &&
		    strcmp(tok[2], "/bin/false") == 0 && !strtok_r(NULL, " \t\r", &rest) && !v->install_false[0])
			gs_copy(v->install_false, sizeof(v->install_false), path);
		if (own || n < 3 || strcmp(tok[0], "options") != 0 || strcmp(tok[1], "nvidia") != 0)
			continue;
		for (t = tok[2]; t; t = strtok_r(NULL, " \t\r", &rest)) {
			const char *value;

			if (strncmp(t, "NVreg_EnableGpuFirmware", 23) != 0 || (t[23] != '=' && t[23] != '\0'))
				continue;
			value = t[23] ? t + 24 : "";
			if (strcmp(value, "0") != 0 && !v->other[0]) {
				gs_copy(v->other, sizeof(v->other), path);
				gs_copy(v->other_value, sizeof(v->other_value), value);
			} else if (strcmp(value, "0") == 0 && !v->zero[0]) {
				gs_copy(v->zero, sizeof(v->zero), path);
			}
		}
	}
	free(data);
}

static void modprobe_read(const char *dir, struct modprobe_view *v)
{
	const char *own = strrchr(GS_NVIDIA_MODPROBE, '/') + 1;
	struct dirent **names;
	char p[GS_PATH_MAX];
	int n, i;

	memset(v, 0, sizeof(*v));
	n = scandir(dir, &names, NULL, alphasort);
	for (i = 0; i < n; i++) {
		size_t len = strlen(names[i]->d_name);

		if (len > 5 && strcmp(names[i]->d_name + len - 5, ".conf") == 0 &&
		    !gs_pathf(p, sizeof(p), "%s/%s", dir, names[i]->d_name))
			modprobe_file(p, strcmp(names[i]->d_name, own) == 0, v);
		free(names[i]);
	}
	if (n >= 0)
		free(names);
}

/* The file, or "a and b" when the two lines are in different files, that holds nvidia back; 0, or -1 when none does. */
static int hold_files(const struct modprobe_view *v, char *files, size_t len)
{
	if (!v->blacklist[0] || !v->install_false[0])
		return -1;
	if (strcmp(v->blacklist, v->install_false) == 0)
		snprintf(files, len, "%s", v->blacklist);
	else
		snprintf(files, len, "%s and %s", v->blacklist, v->install_false);
	return 0;
}

int gs_nvidia_hold(const char *dir, char *files, size_t len)
{
	struct modprobe_view v;

	modprobe_read(dir, &v);
	return hold_files(&v, files, len);
}

/*
 * The option the driver step checks, then a hold that keeps nvidia from loading at boot: activate loads the
 * driver itself, so a hold only keeps the PF unbound and has no use in this flow.
 */
static void modprobe_checks(struct tally *t, const char *dir)
{
	struct modprobe_view v;
	char own[GS_PATH_MAX], files[2 * GS_PATH_MAX + 8];

	modprobe_read(dir, &v);
	if (v.other[0])
		check(t, "FAIL", "%s sets NVreg_EnableGpuFirmware=%s; this stack needs 0", v.other, v.other_value);
	else if (v.zero[0])
		check(t, "INFO", "%s already sets NVreg_EnableGpuFirmware=0; install keeps it%s", v.zero,
		      !gs_pathf(own, sizeof(own), "%s/%s", dir, strrchr(GS_NVIDIA_MODPROBE, '/') + 1) && gs_exists(own) ?
		      "; its own file stays until uninstall" : " and writes no file of its own");
	if (!hold_files(&v, files, sizeof(files))) {
		check(t, "WARN", "nvidia is held back at boot by %s; activate loads the driver itself, so remove "
		      "the `blacklist nvidia` and `install nvidia /bin/false` lines from %s", files, files);
	}
}

/*
 * What install needs before it can build the display or host CUDA modules: the Linux driver package,
 * and for display NVIDIA's public kernel module source. Nothing is asked for a feature the installed
 * build already has. selected: the GPUs were chosen (config or --device), so a missing input is a
 * failure; a bare scan of every GPU only notes it. bdf: the selected boot display GPU.
 */
static void build_inputs(struct tally *t, const struct gs_diag_opts *o, int live, int display, int host_cuda,
			 const char *bdf, int selected)
{
	const char *const given[][2] = { { "--companion-file", o->companion_file },
					 { "--companion-source", o->companion_source },
					 { "--nvkms-source", o->nvkms_source } };
	const char *lvl = selected ? "FAIL" : "INFO";
	/* An input an earlier install kept is used again, so it need not be given. */
	int companion = o->companion_file || o->companion_source || (live && gs_kept_input(GS_INPUTS_DIR, "companion"));
	int nvkms = o->nvkms_source || (live && gs_kept_input(GS_INPUTS_DIR, "nvkms")), have;
	char manifest[GS_PATH_MAX];
	const char *slash;
	struct gs_build b;
	unsigned int i;

	/* Only the build the kernel would load now counts: a record for another kernel means a rebuild. */
	have = gs_build_identity(o->build, live, &b);
	if (have && b.state == GS_BUILD_MATCHES && b.display == 1)
		display = 0;
	if (have && b.state == GS_BUILD_MATCHES && b.host_cuda == 1)
		host_cuda = 0;
	/*
	 * After `install --driver=skip` there is no record and the install manifest stays: the user builds
	 * NVIDIA's module by hand, so what it contains cannot be judged here and nothing is required.
	 */
	if (have && b.state == GS_BUILD_ABSENT && !b.record_read && (slash = strrchr(b.record, '/')) &&
	    !gs_pathf(manifest, sizeof(manifest), "%.*s/" GS_MANIFEST_NAME, (int)(slash - b.record), b.record) &&
	    gs_exists(manifest))
		lvl = "INFO";
	for (i = 0; i < sizeof(given) / sizeof(given[0]); i++)
		if (given[i][1] && !gs_exists(given[i][1]))
			check(t, "FAIL", "%s %s does not exist", given[i][0], given[i][1]);
	if (display && (!companion || !nvkms))
		check(t, lvl, "%s is the boot display GPU, so install builds the modules for the host console, and that\n"
		      "       build needs two more inputs:%s%s\n"
		      "       Pass them to setup or install, or use --no-display to build without these modules", bdf,
		      companion ? "" : "\n         --companion-file PATH (or --companion-source DIR): the Linux driver package",
		      nvkms ? "" : "\n         --nvkms-source PATH: NVIDIA's public kernel module source");
	else if (host_cuda && !companion)
		check(t, lvl, "host CUDA is selected, and its build needs the Linux driver package: pass --companion-file PATH\n"
		      "       (or --companion-source DIR) to setup or install, or use --no-host-cuda");
}

/*
 * The host desktop: NVIDIA's graphics libraries are complete, the loader takes them, the installed driver build
 * accepts their version, and a display server that runs on them draws with them. Nothing here fails the checks for
 * a host with host_desktop=off that has no such files.
 */
static void desktop_checks(struct tally *t, const struct gs_config *cfg, const struct gs_build *b, int want_display)
{
	char dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX], text[2 * GS_PATH_MAX], server[512], loaded[256];
	int want = cfg->host_desktop != GS_DESKTOP_OFF && want_display;
	int state = gs_nvlibs_state(&gs_nvset_desktop, GS_MANIFEST, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad));
	int ret = gs_nvlibs_line(&gs_nvset_desktop, GS_MANIFEST, want, gs_run_capture, text, sizeof(text));

	if (ret) {
		const char *lvl = ret == 1 ? "OK" : ret == 2 ? "INFO" : state == 1 ? "WARN" :
				  state == 0 && cfg->host_desktop == GS_DESKTOP_AUTO ? "INFO" : "FAIL";

		check(t, lvl, "host desktop libraries: %s", text);
	}
	if (!state)
		return;
	if (b->driver_version[0] && strcmp(b->client_accept[0] ? b->client_accept : b->driver_version, version) != 0)
		check(t, want ? "FAIL" : "WARN", "host desktop: the libraries are %s and the installed driver build accepts %s; programs will be "
		      "refused by the driver. Run `sudo ./geforce-sriov install --companion-file PATH` again with the Linux driver package",
		      version, b->client_accept[0] ? b->client_accept : b->driver_version);
	else if (b->client_accept[0])
		check(t, "OK", "host desktop: NVIDIA's driver accepts programs of version %s (this build)", b->client_accept);
	if (want) {
		char line[1024];
		int stale = gs_loaded_line(GS_LIVE_SYSFS, b, loaded, sizeof(loaded));
		int sv = gs_desktop_servers(GS_LIVE_PROC, server, sizeof(server));
		int lv = gs_desktop_draw_line(sv, server, stale, loaded, line, sizeof(line));

		if (lv)
			check(t, lv == 3 ? "FAIL" : lv == 2 ? "WARN" : "INFO", "host desktop: %s", line);
	}
}

/*
 * Doctor's line for the vGPU plugin nvidia-vgpu-mgr loads, with activate's verdict: OK for the patched
 * plugin copy, else a warning with the reason. Before the driver step has run here (no record and no
 * install manifest beside it, as for the build line) the stock plugin is expected, so that is noted only.
 */
static void plugin_checks(struct tally *t, const char *path, const char *record)
{
	char sha[65], real[GS_PATH_MAX], why[1024], manifest[GS_PATH_MAX];
	const char *slash = strrchr(record, '/');
	size_t n;
	int r = gs_sha256_file_resolved(path, sha, real);
	int fresh = slash && !gs_exists(record) &&
		    !gs_pathf(manifest, sizeof(manifest), "%.*s/" GS_MANIFEST_NAME, (int)(slash - record), record) &&
		    !gs_exists(manifest);

	if (r) {
		check(t, fresh ? "INFO" : "WARN", "cannot read the vGPU plugin %s (from NVIDIA's host package): %s", path,
		      strerror(-r));
		return;
	}
	if (!gs_plugin_verdict(path, real, sha, why, sizeof(why))) {
		check(t, "OK", "vGPU plugin %s -> %s: the patched plugin copy", path, real);
		return;
	}
	n = strcspn(why, "\n");
	check(t, fresh ? "INFO" : "WARN", "%.*s", (int)n, why);
	if (why[n])
		print_indented("       ", why + n + 1);
	if (fresh)
		print_indented("       ", "NVIDIA's module has not been built on this host yet; install makes the patched plugin copy");
}

/* The LD_PRELOAD that the Environment= lines of the drop-in at path set, the last one winning: 1 with it in value, else 0. */
static int dropin_preload(const char *path, char *value, size_t len)
{
	FILE *f = fopen(path, "re");
	char *line = NULL;
	size_t cap = 0;
	int found = 0;

	if (!f)
		return 0;
	while (getline(&line, &cap, f) > 0) {
		const char *start = line + strspn(line, " \t"), *q;

		if (strncmp(start, "Environment=", 12) != 0)
			continue;
		/* An assignment starts the value, or follows a blank or an opening quote, which also ends it. */
		for (start += 12, q = start; (q = strstr(q, "LD_PRELOAD=")); q++)
			if (q == start || strchr(" \t\"'", q[-1])) {
				const char *end = q > start && (q[-1] == '"' || q[-1] == '\'') ? (q[-1] == '"' ? "\"\r\n" : "'\r\n") :
						  " \t\"'\r\n";

				snprintf(value, len, "%.*s", (int)strcspn(q + 11, end), q + 11);
				found = 1;
			}
	}
	free(line);
	fclose(f);
	return found;
}

/* 1 when the LD_PRELOAD list value, separated by colons or blanks, names hook. */
static int preload_has(const char *value, const char *hook)
{
	size_t n = strlen(hook);

	for (const char *p = value; *p; p += strcspn(p, ": \t")) {
		p += strspn(p, ": \t");
		if (strncmp(p, hook, n) == 0 && (p[n] == '\0' || strchr(": \t", p[n])))
			return 1;
	}
	return 0;
}

/*
 * Drop-ins of NVIDIA's vGPU daemons, other than the package's own, that set an LD_PRELOAD without the hook.
 * systemd reads a unit's drop-ins in file name order across its unit directories, a name in a more
 * significant directory hiding it in the others, and the last LD_PRELOAD wins, so in nvidia-vgpud.service.d
 * only the files after the package's own replace the hook. dir: the one unit directory to read; NULL: systemd's.
 */
static void dropin_checks(struct tally *t, const char *dir)
{
	static const char *const units[] = { "nvidia-vgpud", "nvidia-vgpu-mgr" };
	const char *const *dirs = dir ? &dir : gs_unit_dirs;
	size_t ndirs = dir ? 1 : sizeof(gs_unit_dirs) / sizeof(gs_unit_dirs[0]);
	char hook[GS_PATH_MAX] = "", p[GS_PATH_MAX], value[GS_PATH_MAX];
	int own = 0;

	/* The hook the package's own drop-in preloads; without one, where install puts it by default. */
	for (size_t d = 0; d < ndirs; d++)
		if (!gs_pathf(p, sizeof(p), "%s/%s.service.d/" GS_VGPUD_DROPIN_NAME, dirs[d], units[0]) && gs_exists(p)) {
			own = dropin_preload(p, hook, sizeof(hook));
			break;
		}
	if (!own)
		gs_copy(hook, sizeof(hook), GS_DEFAULT_PREFIX "/lib/geforce-sriov/libgeforce_sriov.so");
	for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++)
		for (size_t d = 0; d < ndirs; d++) {
			struct dirent **names;
			int n;

			if (gs_pathf(p, sizeof(p), "%s/%s.service.d", dirs[d], units[u]))
				continue;
			n = scandir(p, &names, NULL, alphasort);
			for (int i = 0; i < n; i++) {
				const char *name = names[i]->d_name;
				size_t len = strlen(name);
				int skip = len <= 5 || strcmp(name + len - 5, ".conf") != 0 ||
					   (u == 0 && (strcmp(name, GS_VGPUD_DROPIN_NAME) == 0 ||
						       (own && strcmp(name, GS_VGPUD_DROPIN_NAME) < 0)));

				for (size_t e = 0; !skip && e < d; e++)
					skip = !gs_pathf(p, sizeof(p), "%s/%s.service.d/%s", dirs[e], units[u], name) &&
					       gs_exists(p);
				if (!skip && !gs_pathf(p, sizeof(p), "%s/%s.service.d/%s", dirs[d], units[u], name) &&
				    dropin_preload(p, value, sizeof(value)) && !preload_has(value, hook))
					check(t, "WARN", "%s sets LD_PRELOAD for %s and overrides the geforce-sriov identity hook. "
					      "Remove it or point it at %s.", p, units[u], hook);
				free(names[i]);
			}
			if (n >= 0)
				free(names);
		}
}

/*
 * What has to admit activation at boot: the early livepatch unit enabled before udev, the activation
 * unit installed, and the vgpud drop-in that runs `check --daemon-ready` before vgpud starts.
 */
static void boot_checks(struct tally *t, const struct gs_diag_opts *o, int live, int fresh)
{
	const char *units = o->unit_dir ? o->unit_dir : live ? GS_UNIT_DIR : NULL;
	char p[GS_PATH_MAX], *data;
	const char *found = NULL;
	size_t len;

	if (units && gs_unit_file_present(GS_UNIT_ACTIVATE))
		check(t, "OK", "%s is installed", GS_UNIT_ACTIVATE);
	else if (units && fresh)
		check(t, "INFO", "%s is not installed yet; install installs it", GS_UNIT_ACTIVATE);
	else if (units)
		check(t, "WARN", "%s is not installed: nothing brings the GPUs up at boot; `geforce-sriov install` installs it",
		      GS_UNIT_ACTIVATE);
	if (live && gs_exists(GS_UNIT_WANTS_LIVEPATCH))
		check(t, "OK", "%s is enabled: the livepatch loads before udev", GS_UNIT_LIVEPATCH);
	else if (live && fresh)
		check(t, "INFO", "%s is not enabled yet; install enables it once GPUs are selected", GS_UNIT_LIVEPATCH);
	else if (live)
		check(t, "WARN", "%s is not enabled: the livepatch would load after NVIDIA binds the PF", GS_UNIT_LIVEPATCH);
	/* The alias overrides stop udev from loading NVIDIA's driver; only the activate unit's first step loads it then. */
	{
		const char *mdir = o->modprobe_dir ? o->modprobe_dir : live ? "/etc/modprobe.d" : NULL;
		char wants[GS_PATH_MAX] = GS_UNIT_WANTS_ACTIVATE;

		if (o->unit_dir && gs_pathf(wants, sizeof(wants), "%s/multi-user.target.wants/%s", o->unit_dir, GS_UNIT_ACTIVATE))
			wants[0] = '\0';
		if (mdir && (live || o->unit_dir) && wants[0] && !gs_exists(wants) &&
		    !gs_pathf(p, sizeof(p), "%s/%s", mdir, strrchr(GS_AUTOLOAD_PATH, '/') + 1) && gs_exists(p))
			check(t, "WARN", "%s stops udev from loading NVIDIA's driver, and %s, whose first step loads it at boot instead, "
			      "is not enabled. This host will start without NVIDIA's driver. Run `geforce-sriov install "
			      "--device BDF` to select a GPU and enable the units, or `geforce-sriov uninstall` to remove "
			      "the file.", p, GS_UNIT_ACTIVATE);
	}
	for (size_t k = 0; units && k < (o->unit_dir ? 1u : sizeof(gs_unit_dirs) / sizeof(gs_unit_dirs[0])); k++) {
		if (gs_pathf(p, sizeof(p), "%s/nvidia-vgpud.service.d/%s", o->unit_dir ? o->unit_dir : gs_unit_dirs[k],
			     GS_VGPUD_DROPIN_NAME))
			break;
		if (gs_exists(p)) {
			found = p;
			break;
		}
	}
	if (found && !gs_read_whole(found, &data, &len, GS_CONFIG_MAX_BYTES)) {
		int ready = strstr(data, "--daemon-ready") != NULL;

		free(data);
		check(t, ready ? "OK" : "WARN", "%s %s", found, ready ? "admits vgpud with `check --daemon-ready`" :
		      "starts vgpud without `check --daemon-ready`: replace it with the package's drop-in");
	} else if (units && fresh) {
		check(t, "INFO", "no vgpud drop-in yet; install writes nvidia-vgpud.service.d/%s", GS_VGPUD_DROPIN_NAME);
	} else if (units) {
		check(t, "WARN", "no vgpud drop-in found: vgpud may start before the VFs are configured; "
		      "`geforce-sriov install` writes nvidia-vgpud.service.d/%s", GS_VGPUD_DROPIN_NAME);
	}
}

/*
 * 1 when install has not run on this host: no build record and no install manifest beside it
 * (GS_STATE_DIR). What install puts in place is then not missing, only not there yet, so the checks note
 * it instead of warning. After an install, and after `install --driver=skip`, the manifest is there.
 */
static int host_fresh(const struct gs_diag_opts *o, int live)
{
	const char *record = o->build && o->build->record ? o->build->record : live ? GS_DRIVER_BUILD : NULL;
	const char *slash = record ? strrchr(record, '/') : NULL;
	char manifest[GS_PATH_MAX];

	return slash && !gs_exists(record) &&
	       !gs_pathf(manifest, sizeof(manifest), "%.*s/" GS_MANIFEST_NAME, (int)(slash - record), record) &&
	       !gs_exists(manifest);
}

/* activate's marker in o's run directory (GS_RUN_DIR when live, none on a fixture without one): 1 when it exists. */
int gs_activating_left(const struct gs_diag_opts *o, int live, char *p, size_t len)
{
	const char *run = o->run_dir ? o->run_dir : live ? GS_RUN_DIR : NULL;

	return run && !gs_pathf(p, len, "%s/" GS_ACTIVATING_NAME, run) && gs_exists(p);
}

/* 1 when a *.conf file in the modprobe.d directory dir has `options nvidia ... NVreg_DynamicPowerManagement=0` (file names it). */
static int nvidia_rtd3_off(const char *dir, char *file, size_t len)
{
	struct dirent *de;
	DIR *d = opendir(dir);
	int found = 0;

	if (!d)
		return 0;
	while (!found && (de = readdir(d))) {
		char p[GS_PATH_MAX], *data, *line, *save = NULL;
		size_t n = strlen(de->d_name);

		if (n < 6 || strcmp(de->d_name + n - 5, ".conf") != 0 || gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name) ||
		    gs_read_whole(p, &data, &n, 1 << 16))
			continue;
		for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
			char *tok, *save2 = NULL;
			unsigned int k = 0;

			for (tok = strtok_r(line, " \t", &save2); tok && !found; tok = strtok_r(NULL, " \t", &save2), k++) {
				size_t tl = strlen(tok);

				if ((k == 0 && strcmp(tok, "options") != 0) || (k == 1 && strcmp(tok, "nvidia") != 0))
					break;
				/* NVIDIA's documentation writes the option in double quotes. */
				if (tl >= 2 && tok[0] == '"' && tok[tl - 1] == '"') {
					tok[tl - 1] = '\0';
					tok++;
				}
				found = k > 1 && (strcmp(tok, "NVreg_DynamicPowerManagement=0") == 0 ||
						  strcmp(tok, "NVreg_DynamicPowerManagement=0x00") == 0);
			}
		}
		free(data);
		if (found)
			gs_copy(file, len, p);
	}
	closedir(d);
	return found;
}

/* 1 when sleep.target in the unit directory dir is masked (a link to /dev/null). */
static int sleep_masked(const char *dir)
{
	char p[GS_PATH_MAX], target[64];
	ssize_t n;

	if (gs_pathf(p, sizeof(p), "%s/sleep.target", dir) || (n = readlink(p, target, sizeof(target) - 1)) < 0)
		return 0;
	target[n] = '\0';
	return strcmp(target, "/dev/null") == 0;
}

/*
 * A host with the laptop rows laptops (n) selected: nvidia's runtime power management and system sleep each power
 * the GPU off (D3cold), which resets its SR-IOV state and every vGPU. The checks only name the change, once per GPU
 * while it is missing; the package makes neither.
 */
static void laptop_host_checks(struct tally *t, const struct gs_diag_opts *o, int live, const struct gs_addr *laptops,
			       unsigned int n)
{
	const char *mpd = o->modprobe_dir ? o->modprobe_dir : live ? "/etc/modprobe.d" : NULL;
	const char *units = o->unit_dir ? o->unit_dir : live ? GS_UNIT_DIR : NULL;
	char file[GS_PATH_MAX];
	unsigned int i;

	if (mpd && nvidia_rtd3_off(mpd, file, sizeof(file)))
		check(t, "OK", "laptop GPU: nvidia's runtime power management is off (NVreg_DynamicPowerManagement=0 in %s)",
		      file);
	else if (mpd)
		for (i = 0; i < n; i++)
			check(t, "WARN", "%s: nvidia may power off this GPU: set options nvidia "
			      "NVreg_DynamicPowerManagement=0 (docs/laptops.md#d3cold)", laptops[i].str);
	if (units && sleep_masked(units))
		check(t, "OK", "laptop host: system sleep is blocked (sleep.target is masked)");
	else if (units)
		for (i = 0; i < n; i++)
			check(t, "WARN", "%s: suspend is not blocked; sleep powers off this GPU (docs/laptops.md#d3cold)",
			      laptops[i].str);
}

int gs_hostcheck(const struct gs_diag_opts *o, enum gs_hostcheck_mode mode)
{
	const char *sysroot = o->sysroot ? o->sysroot : GS_LIVE_SYSFS;
	int live = gs_is_live_root(sysroot);
	struct gs_addr devs[GS_CFG_MAX_BDFS], laptops[GS_CFG_MAX_BDFS];
	struct gs_module_state m;
	struct gs_config cfg;
	struct tally t = { 0, 0, 0, NULL };
	const char *cfg_path, *source, *isrc;
	const struct gs_addr *isel;
	char err[512], p[GS_PATH_MAX], ver[64] = "";
	unsigned int n, i, nisel, nlaptops;
	int id_on, id_force;
	int pre = mode == GS_HOSTCHECK_PRE, post = !pre;

	t.post = post;
	if (pre)
		printf("geforce-sriov %s: checks before install (sysfs root %s%s)\n", GS_VERSION, sysroot,
		       live ? "" : ", diagnostic fixture: host checks skipped");
	else
		t.head = "\nOther host checks (OK lines are not shown):\n";
	if (gs_diag_config(o, &cfg, &cfg_path, err, sizeof(err))) {
		if (pre)
			check(&t, "FAIL", "config: %s", err);
		gs_config_defaults(&cfg);
	} else if (pre && !cfg_path) {
		check(&t, "INFO", "config: not read (fixture root; pass --config)");
	} else if (pre && !cfg.present) {
		check(&t, "INFO", "config: %s absent; %s", cfg_path,
		      o->ndevices ? "the selected devices come from --device" : "defaults apply and no device is selected");
	} else if (cfg_path && cfg.present) {
		if (pre)
			check(&t, "OK", "config: %s: %u device(s) selected; display=%s host_cuda=%s ari=%s msix=%s vfs=%u%s identity=%s",
			      cfg_path, cfg.nbdfs, gs_display_mode_name(cfg.display), gs_sw_name(cfg.host_cuda), gs_sw_name(cfg.ari), gs_sw_name(cfg.msix),
			      gs_config_vfs(&cfg), cfg.vfs ? "" : " (default)", cfg.identity == GS_SW_ON ? "on" : "off");
		if (post && live && gs_config_file_safe(cfg_path, err, sizeof(err)))
			check(&t, "FAIL", "config: %s; activate refuses it", err);
	}
	n = gs_selected_devices(o, &cfg, sysroot, devs, &source);
	nlaptops = gs_laptop_gpus(sysroot, devs, n, laptops);
	/* Like install, --disable-d3cold needs a laptop GPU among the GPUs judged. */
	if (o->disable_d3cold && !nlaptops)
		return gs_d3cold_usage();
	/*
	 * Identity as the hook will see it: the config's settings or, for a --device selection the
	 * config does not hold yet, what `install --device` writes (identity=on, identity_force_host=on).
	 */
	isel = cfg.bdfs;
	nisel = cfg.nbdfs;
	id_on = cfg.identity == GS_SW_ON;
	id_force = cfg.identity_force_host == GS_SW_ON;
	isrc = cfg_path ? cfg_path : "defaults";
	if (o->ndevices && !gs_same_selection(o->devices, o->ndevices, cfg.bdfs, cfg.nbdfs)) {
		isel = o->devices;
		nisel = o->ndevices;
		id_on = id_force = 1;
		isrc = "planned: `install --device` writes identity=on and identity_force_host=on";
	}

	if (!gs_pathf(p, sizeof(p), "%s/kernel/livepatch", sysroot) && gs_exists(p))
		check(&t, "OK", "kernel livepatch support present");
	else
		check(&t, "FAIL", "kernel: no %s/kernel/livepatch (CONFIG_LIVEPATCH is required)", sysroot);
	if (post && live && gs_cmdline_noari(GS_LIVE_PROC))
		check(&t, "INFO", "kernel command line has pci=noari: activate loads %s with ari=0", GS_MODULE_NAME);
	if (post && !gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups", sysroot)) {
		DIR *d = opendir(p);
		int any = 0;
		struct dirent *de;

		while (d && (de = readdir(d)))
			if (de->d_name[0] != '.')
				any = 1;
		if (d)
			closedir(d);
		check(&t, any ? "OK" : "WARN", "IOMMU groups %s", any ? "present" : "absent: VFs cannot be assigned to VMs");
	}
	gs_read_module_state(sysroot, &m);
	if (!m.loaded && pre)
		check(&t, "INFO", "module %s not loaded (activate loads it)", GS_MODULE_NAME);
	else if (m.loaded)
		check(&t, m.lp_ok ? "OK" : "WARN",
		      "module %s loaded: ari=%s msix=%s vfs=%s bdfs=%s; livepatch %s",
		      GS_MODULE_NAME, m.ari, m.msix, m.vfs[0] ? m.vfs : "(none)",
		      m.bdfs[0] ? m.bdfs : "(none)",
		      !m.lp_present ? "directory missing" : m.lp_ok ? "enabled, transition complete" :
		      "not enabled or still in transition");
	if (m.loaded && gs_module_vfs(&m) != gs_config_vfs(&cfg))
		check(&t, "WARN", "module %s was loaded with vfs=%u, the config has vfs=%u; the new count takes effect at the next boot",
		      GS_MODULE_NAME, gs_module_vfs(&m), gs_config_vfs(&cfg));

	if (live) {
		char where[GS_PATH_MAX];
		int mod = gs_installed_module_present();
		int dkms = gs_find_program("dkms", p, sizeof(p)) == 0;
		int sysctl = gs_find_program("systemctl", p, sizeof(p)) == 0;
		int deps_ok = 1;
		static const char *const deps[] = { "lspci", "setpci" };

		if (post && getuid() != 0)
			check(&t, "INFO", "not root: config space beyond 64 bytes is unreadable; bridge ARI support unknown");
		if (post) {
			if (!mod && host_fresh(o, live))
				check(&t, "INFO", "livepatch module geforce_sriov for the running kernel is not built yet; install builds it with DKMS");
			else
				check(&t, mod ? "OK" : "WARN", "livepatch module geforce_sriov for the running kernel %s",
				      mod ? "installed" : "not found under /lib/modules/$(uname -r) (geforce-sriov install builds it with DKMS)");
		}
		/* What the driver step needs: headers for the running kernel, a C compiler and make. */
		if (pre) {
			char rel[64] = "", hp[GS_PATH_MAX];
			const char *lvl = mod ? "WARN" : "FAIL";
			const char *why = mod ? ": only a later `install` needs it (the livepatch module is installed for this kernel)"
					      : ": install cannot build the livepatch module";

			gs_read_attr(GS_LIVE_PROC "/sys/kernel/osrelease", rel, sizeof(rel));
			if (rel[0] && (hp[0] = '\0', !gs_pathf(hp, sizeof(hp), "/lib/modules/%s/build/Makefile", rel)) &&
			    gs_exists(hp))
				check(&t, "OK", "kernel headers for %s present", rel);
			else
				check(&t, lvl, "kernel headers for %s missing (%s)%s", rel[0] ? rel : "the running kernel",
				      rel[0] ? hp : "/lib/modules/<release>/build", why);
			if (gs_find_program("cc", p, sizeof(p)) == 0)
				check(&t, "OK", "C compiler found at %s", p);
			else
				check(&t, lvl, "cc not found%s", why);
			if (gs_find_program("make", p, sizeof(p)) == 0)
				check(&t, "OK", "make found at %s", p);
			else
				check(&t, lvl, "make not found%s", why);
			const char *nsrc = o->nvidia_source ? o->nvidia_source : gs_nvidia_source_default();
			char npath[GS_PATH_MAX];

			/* DIR may be the kernel tree or an extracted package root, whose tree is in kernel/. */
			if (gs_pathf(npath, sizeof(npath), "%s/nvidia/nv-kernel.o_binary", nsrc))
				gs_copy(npath, sizeof(npath), nsrc);
			if (!gs_exists(npath) && gs_pathf(npath, sizeof(npath), "%s/kernel/nvidia/nv-kernel.o_binary", nsrc) == 0 &&
			    gs_exists(npath))
				gs_pathf(npath, sizeof(npath), "%s/kernel", nsrc);
			if (o->driver_file) {
				check(&t, "INFO", "NVIDIA's kernel source tree: install extracts it from %s into %s/<version>/kernel",
				      o->driver_file, GS_NVIDIA_PKG_DIR);
				if (gs_exists("/sys/module/nouveau"))
					check(&t, "WARN", "nouveau is loaded: install refuses to run NVIDIA's installer while it is. Put\n"
					      "       `blacklist nouveau` and `options nouveau modeset=0` in /etc/modprobe.d/blacklist-nouveau.conf,\n"
					      "       rebuild the initramfs (`update-initramfs -u`, or `dracut -f`) and power the machine off and on");
			} else if (gs_exists(npath)) {
				check(&t, "OK", "NVIDIA's kernel source tree found at %s", nsrc);
			} else {
				check(&t, mod ? "WARN" : "FAIL",
				      "%s not found: install needs NVIDIA's %s kernel source tree (/usr/src/nvidia-%s\n"
				      "       after a DKMS install, else <extracted package>/kernel). Pass `--driver-file PATH` (NVIDIA's\n"
				      "       .run package) to extract it, `--nvidia-source DIR` to name an extracted tree, or\n"
				      "       `--driver=skip` to install geforce-sriov alone",
				      nsrc, gs_default_host_version(), gs_default_host_version());
			}
		}
		/* A kernel that NVIDIA's module was built for and that has no livepatch module boots with no VFs. */
		if (post) {
			char kernels[512];

			if (gs_kernels_without_livepatch("", GS_STATE_DIR, kernels, sizeof(kernels)))
				check(&t, "WARN", "NVIDIA's module is built for %s, but the livepatch module %s is not installed for it, so "
				      "that kernel boots with no VFs; run `sudo geforce-sriov rebuild --kernel RELEASE` for it",
				      kernels, GS_MODULE_NAME);
		}
		/* What the later steps need: a live install (dkms, systemctl). */
		if (pre) {
		if (dkms && sysctl)
			check(&t, "OK", "dkms and systemctl found (a live install needs both)");
		else if (!dkms && sysctl)
			check(&t, "WARN", "dkms not found: a live `geforce-sriov install` refuses without it%s",
			      mod ? " (the livepatch module is already installed for this kernel)" : "");
		else
			check(&t, "WARN", "%s not found: a live `geforce-sriov install`, with or without --device, needs dkms and systemctl%s",
			      !dkms ? "dkms and systemctl" : "systemctl",
			      mod ? " (the livepatch module is already installed for this kernel)" : "");
		{
			char rel[64] = "";
			int missing = 0;

			gs_read_attr(GS_LIVE_PROC "/sys/kernel/osrelease", rel, sizeof(rel));
			for (unsigned int d = 0; d < GS_DEP_COUNT; d++)
				missing |= !gs_dep_present((enum gs_dep)d, rel);
			if (missing)
				check(&t, "INFO", "%s", GS_PREREQ_MSG);
		}
		}
		if (post && gs_exists(GS_ATTEMPT_MARKER)) {
			check(&t, "FAIL", "activation lock %s is set: an activation attempt did not finish.", GS_ATTEMPT_MARKER);
			print_indented("       ", gs_recovery_msg);
		}
		if (post)
		switch (gs_nvidia_autoload(where, sizeof(where))) {
		case 1:
		case 2:
			check(&t, "INFO", "%s loads nvidia at boot, so it can take a card before your udev rules do; the livepatch must load first", where);
			break;
		default:
			check(&t, "INFO", "no nvidia module in boot module configuration (built initramfs images are not inspected)");
		}
		if (gs_find_program(cfg.sriov_manage, p, sizeof(p)) == 0)
			check(&t, "OK", "sriov-manage found at %s", cfg.sriov_manage);
		else if (o->driver_file)
			check(&t, "INFO", "%s not found: install takes it from %s when the package has it; with\n"
			      "       --accept-nvidia-license, NVIDIA's installer installs it", cfg.sriov_manage, o->driver_file);
		else
			check(&t, "WARN", "%s not found: `activate` needs it, from NVIDIA's vGPU host driver package. `geforce-sriov\n"
			      "       install --driver-file PATH --accept-nvidia-license` installs it", cfg.sriov_manage);
		{
			int vgpud = gs_unit_file_present("nvidia-vgpud.service");
			int mgr = gs_unit_file_present("nvidia-vgpu-mgr.service");

			if (vgpud && mgr)
				check(&t, "OK", "nvidia-vgpud.service and nvidia-vgpu-mgr.service unit files found");
			else if (o->driver_file)
				check(&t, "INFO", "%s not found in systemd's unit directories: install copies them from %s",
				      !vgpud && !mgr ? "nvidia-vgpud.service and nvidia-vgpu-mgr.service" :
				      !vgpud ? "nvidia-vgpud.service" : "nvidia-vgpu-mgr.service", o->driver_file);
			else
				check(&t, "WARN", "%s not found in systemd's unit directories; without NVIDIA's vGPU daemons no vGPU types appear.\n"
				      "       `geforce-sriov install --driver-file PATH` copies them from NVIDIA's package. By hand: copy\n"
				      "       them unchanged to /etc/systemd/system and run `systemctl daemon-reload`",
				      !vgpud && !mgr ? "nvidia-vgpud.service and nvidia-vgpu-mgr.service" :
				      !vgpud ? "nvidia-vgpud.service" : "nvidia-vgpu-mgr.service");
		}
		/* `activate`: sriov-manage needs lspci and setpci, found through the PATH gs_run gives it. */
		for (size_t k = 0; k < sizeof(deps) / sizeof(deps[0]); k++) {
			if (gs_find_program(deps[k], p, sizeof(p)) == 0)
				continue;
			deps_ok = 0;
			check(&t, "WARN", "%s (pciutils) not found: sriov-manage needs lspci and setpci, so `activate` refuses to enable VFs",
			      deps[k]);
		}
		if (deps_ok)
			check(&t, "OK", "lspci and setpci found (sriov-manage needs them for `activate`)");
		if (post) {
			gs_nvidia_version(GS_LIVE_PROC, ver, sizeof(ver));
			if (ver[0]) {
				int known = 0;

				for (i = 0; i < GS_HOOK_DRIVER_VERSION_COUNT; i++)
					known |= strcmp(ver, gs_hook_driver_versions[i]) == 0;
				check(&t, known || !id_on ? "INFO" : "WARN",
				      "NVIDIA driver %s loaded%s", ver,
				      known ? "; the identity hook acts on this version" :
				      "; the identity hook passes this version through unchanged");
			}
		}
	}
	if (post && (o->unit_dir || live))
		dropin_checks(&t, o->unit_dir);
	if (post)
		boot_checks(&t, o, live, host_fresh(o, live));
	if (o->modprobe_dir || live)
		modprobe_checks(&t, o->modprobe_dir ? o->modprobe_dir : "/etc/modprobe.d");
	if (post && (o->plugin || live))
		plugin_checks(&t, o->plugin ? o->plugin : gs_plugin_link(),
			      o->build && o->build->record ? o->build->record : GS_DRIVER_BUILD);
	{
		struct gs_build b;
		char line[BUILD_LINE_MAX], manifest[GS_PATH_MAX];
		const char *slash;
		int boot_display = 0, want_display, needs_build = 0;
		const char *boot_bdf = "";

		/* The selection install would write: its flags over the config. */
		if (o->display_set)
			cfg.display = o->display;
		if (o->host_cuda_set)
			cfg.host_cuda = o->host_cuda ? GS_SW_ON : GS_SW_OFF;
		if (o->host_desktop_set)
			cfg.host_desktop = o->host_desktop ? GS_DESKTOP_ON : GS_DESKTOP_OFF;
		for (i = 0; i < n; i++) {
			struct gs_card c;

			gs_card_inspect(sysroot, &devs[i], &c);
			if (c.present && c.boot_vga == 1 && !boot_display)
				boot_bdf = devs[i].str;
			boot_display |= c.present && c.boot_vga == 1;
			/* The display build follows the KMS rule: the console card, or another KMS card when its inputs are given. */
			needs_build |= c.present && gs_display_kms(&cfg, &c, NULL) &&
				       (gs_display_console(&cfg, &c) ||
					gs_display_inputs(live ? GS_INPUTS_DIR : NULL, o->companion_file, o->companion_source,
							  o->nvkms_source));
		}
		/* What the config asks for against what the installed build provides. */
		want_display = cfg.display != GS_DISPLAY_OFF && needs_build;
		if (pre && !o->skip_driver)
			build_inputs(&t, o, live, want_display, cfg.host_cuda == GS_SW_ON, boot_bdf, cfg.nbdfs || o->ndevices);
		if (post && gs_build_identity(o->build, live, &b)) {
			/*
			 * No record and no install manifest beside it (GS_STATE_DIR): install has not run here, or
			 * uninstall removed it, and install runs the driver step. After `install --driver=skip` the
			 * manifest stays, and so does the usual line.
			 */
			slash = strrchr(b.record, '/');
			if (b.state == GS_BUILD_ABSENT && !b.record_read && slash &&
			    !gs_pathf(manifest, sizeof(manifest), "%.*s/" GS_MANIFEST_NAME, (int)(slash - b.record), b.record) &&
			    !gs_exists(manifest))
				check(&t, "INFO", "NVIDIA's module has not been built on this host yet; install builds it");
			else
				check(&t, gs_build_line(&b, line, sizeof(line)) ? "WARN" : "OK", "%s", line);
			if (gs_loaded_line(sysroot, &b, line, sizeof(line)))
				check(&t, "WARN", "%s", line);
			/* On a host that has an install: before it, NVIDIA's own programs make the nodes as they need them. */
			if (live && !host_fresh(o, live)) {
				struct gs_dev_nodes nodes;

				gs_dev_nodes_scan(&gs_host_live, &nodes);
				if (nodes.nmissing) {
					char list[GS_DEV_NODES_MAX * 50];

					gs_dev_nodes_list(&nodes, list, sizeof(list));
					check(&t, "WARN", "device nodes of NVIDIA's loaded modules do not exist: %s. A container or a program "
					      "that is given one of them does not start; `sudo geforce-sriov activate` creates them", list);
				}
			}
			if (live && b.display == 1) {
				struct gs_loaded_patch_result lp;

				gs_loaded_patch_verify(GS_LIVE_PROC, &b, &lp);
				check(&t, lp.state == GS_LOADED_MATCH ? "OK" :
				      lp.state == GS_LOADED_MISMATCH ? "FAIL" : "WARN",
				      "loaded display patch: %s%s%s", lp.state == GS_LOADED_MATCH ? "MATCH" :
				      lp.state == GS_LOADED_MISMATCH ? "MISMATCH" : "UNKNOWN",
				      lp.why[0] ? ": " : "", lp.why);
			}
			if (b.display >= 0) {
				if (want_display && !b.display)
					check(&t, "FAIL", "display=%s asks for the display build, and the installed build has none; "
					      "run `sudo geforce-sriov install` again with --companion-file and --nvkms-source", gs_display_mode_name(cfg.display));
			} else if (want_display && b.record_read) {
				check(&t, "WARN", "display is requested and the build record does not say whether the display "
				      "stack is installed; run `sudo geforce-sriov install` again to record it");
			}
			if (b.host_cuda >= 0 && cfg.host_cuda && !b.host_cuda)
				check(&t, "FAIL", "host_cuda=on asks for host CUDA, and the installed build has no nvidia-uvm; "
				      "run `sudo geforce-sriov install` again with --companion-file");
			/* NVIDIA's graphics libraries for the host desktop, and the driver build that has to accept them. */
			if (live)
				desktop_checks(&t, &cfg, &b, want_display);
		}
	}

	if (pre && n == 0)
		check(&t, "WARN", "no NVIDIA display function found (%s)", source);

	for (i = 0; i < n; i++) {
		struct gs_card c;

		if (pre)
			printf("\n%s (%s)\n", devs[i].str, source);
		/*
		 * A laptop row's power state, read only: the checks write nothing. A sysfs config read resumes a
		 * runtime-suspended device by itself, so activation's config-read gate is a plain read here.
		 */
		{
			unsigned int dev;
			const struct gs_device *pe = gs_pf_entry(sysroot, &devs[i], &dev);
			char dir[GS_PATH_MAX], bdir[GS_PATH_MAX], q[GS_PATH_MAX], gate[96], rt[32];
			char pc[16] = "?", d3[8] = "?", bpc[16] = "?";

			/*
			 * activate refuses a laptop row without D3cold consent: in the config, or planned by --disable-d3cold
			 * (install writes it).
			 */
			if (pe && pe->laptop && o->disable_d3cold) {
				check(&t, "INFO", "%s: laptop GPU: D3cold consent planned by --disable-d3cold; install writes "
				      "d3cold=off, and activation keeps this GPU powered", devs[i].str);
			} else if (pe && pe->laptop && cfg.d3cold_off) {
				check(&t, "INFO", "%s: laptop GPU: D3cold consent in the config (d3cold=off); activation keeps this "
				      "GPU powered", devs[i].str);
			} else if (pe && pe->laptop) {
				char why[GS_D3COLD_WHY_SIZE];

				gs_d3cold_why(why, sizeof(why), &devs[i], 1, devs, n, "       ");
				check(&t, "FAIL", "%s", why);
			}
			if (pe && pe->laptop && live && getuid() != 0) {
				check(&t, "INFO", "%s: laptop GPU: run as root to check that it answers", devs[i].str);
			} else if (pe && pe->laptop && !gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, devs[i].str)) {
				gs_pf_runtime_status(sysroot, &devs[i], rt, sizeof(rt));
				if (gs_config_gate(dir, dev, gate, sizeof(gate))) {
					check(&t, "FAIL", "%s does not answer although its runtime_status is %s: %s (a laptop GPU that did "
					      "not come back from D3cold); activate refuses this card. " WAKE_REMEDY, devs[i].str,
					      rt[0] ? rt : "absent", gate);
					continue;
				}
				if (!gs_pathf(q, sizeof(q), "%s/power/control", dir) && gs_read_attr(q, pc, sizeof(pc)))
					gs_copy(pc, sizeof(pc), "?");
				if (!gs_pathf(q, sizeof(q), "%s/d3cold_allowed", dir) && gs_read_attr(q, d3, sizeof(d3)))
					gs_copy(d3, sizeof(d3), "?");
				if (!gs_pf_bridge_dir(sysroot, &devs[i], bdir, sizeof(bdir)) &&
				    !gs_pathf(q, sizeof(q), "%s/power/control", bdir) && gs_read_attr(q, bpc, sizeof(bpc)))
					gs_copy(bpc, sizeof(bpc), "?");
				check(&t, "INFO", "%s: laptop GPU answers (10de:%04x in D0): runtime_status %s, power/control %s, "
				      "d3cold_allowed %s, bridge power/control %s; activation sets power/control and the bridge to on "
				      "and d3cold_allowed to 0", devs[i].str, dev, rt[0] ? rt : "absent", pc, d3, bpc);
			}
		}
		gs_card_inspect(sysroot, &devs[i], &c);
		if (c.present) {
			if (pre) {
				printf("  id 10de:%04x subsystem %04x:%04x class 0x%06x driver %s boot_vga %d iommu_group %s\n",
				       c.device, c.subvendor, c.subdevice, c.class_code, c.driver[0] ? c.driver : "none",
				       c.boot_vga, c.iommu_group[0] ? c.iommu_group : "none");
				printf("  display: boot_vga=%d, requested=%s, effective=%s\n", c.boot_vga, gs_display_mode_name(cfg.display),
				       gs_display_kms(&cfg, &c, NULL) ?
				       (gs_display_console(&cfg, &c) ? "on (the boot display)" : "on") :
				       cfg.display == GS_DISPLAY_ON ? "off (the display modules are not used for this card)" :
				       "off");
			}
			if (c.entry)
				entry_checks(&t, &c, &cfg);
			if (c.entry && c.entry->laptop) {
				char lost[2][256];
				unsigned int nlost = gs_laptop_lost(sysroot, &c, lost);

				for (unsigned int k = 0; k < nlost; k++)
					check(&t, "WARN", "%s", lost[k]);
			}
			if (pre && c.bridge_is_pci)
				printf("  bridge %s power/control=%s runtime_status=%s ARI forwarding supported=%s\n",
				       c.bridge.str, c.bridge_pm[0] ? c.bridge_pm : "?", c.bridge_rt[0] ? c.bridge_rt : "?",
				       c.bridge_ari_fwd_supported < 0 ? "unknown" :
				       c.bridge_ari_fwd_supported ? "yes" : "no");
			if (pre && c.has_sriov)
				printf("  sriov_totalvfs=%u sriov_numvfs=%u ari_enabled=%d\n", c.totalvfs, c.numvfs,
				       c.ari_enabled);
			if (c.bridge_ari_fwd_supported == 0 && cfg.ari != GS_SW_OFF)
				check(&t, "WARN", "%s: the bridge does not support ARI forwarding; ari=on has no effect there",
				      c.pf.str);
			if (c.bar0_assigned == 0)
				check(&t, "WARN", "%s: BAR0 is not assigned, so the card is unusable this boot. Do not rescan again; " BAR_REMEDY,
				      c.pf.str);
			if (c.has_sriov && c.vf_bars_assigned == 0)
				check(&t, "WARN", "%s: no VF BAR is assigned: enabling VFs fails with \"not enough MMIO resources for SR-IOV\"; " BAR_REMEDY,
				      c.pf.str);
			iommu_checks(&t, sysroot, &c);
			{
				char vf1[GS_BDF_STR_SIZE];
				unsigned int nshare = vfs_sharing_pf_group(sysroot, &c, vf1, sizeof(vf1));

				if (nshare)
					check(&t, "WARN", "%s: %u enabled VF(s), first %s, share IOMMU group %s with the PF: they are not isolated from the PF, and VFIO may refuse them",
					      c.pf.str, nshare, vf1, c.iommu_group);
			}
			if (c.msix_linked >= 0)
				check(&t, (c.msix_linked || !m.loaded) ? "INFO" : "WARN",
				      "%s: MSI-X capability %s%s", c.pf.str, c.msix_linked ? "linked" : "hidden",
				      c.msix_linked ? "" : m.loaded ? "; activation stops before VF enable" :
				      "; the livepatch reveals it when it loads");
			if (c.boot_vga == 1 && c.entry) {
				struct gs_display disp;
				int r = gs_display_inspect(sysroot, live ? GS_LIVE_PROC : NULL, &c, &disp);

				if (r == 0) {
					if (pre)
						printf("  display/KMS: %s%s%s%s, %u fb/DRM device(s) on it\n",
						       disp.fw[0] ? disp.fw : "no firmware framebuffer", disp.fw[0] ? " (" : "",
						       disp.fw[0] ? (disp.fw_driver[0] ? disp.fw_driver : "unbound: released") : "",
						       disp.fw[0] ? ")" : "", disp.nconsumers);
					if (disp.nconsumers)
						check(&t, "INFO", "%s: display clients hold the card; activation needs them gone (and asks "
						      "before it stops a display manager)", c.pf.str);
					if (!disp.proc_checked)
						check(&t, "INFO", "%s: /proc/iomem and /proc/consoles are checked on the live system only",
						      c.pf.str);
				} else {
					check(&t, "INFO", "%s: display users: %s", c.pf.str,
					      disp.refuse[0] ? disp.refuse : "the inspection is incomplete");
				}
			}
		}
		if (!c.present || !c.entry) {
			check(&t, "FAIL", "%s: selected GPU is absent or not in the device table", c.pf.str);
		} else if (c.driver[0] && strcmp(c.driver, "nvidia") != 0) {
			check(&t, "FAIL", "%s: bound to %s, not nvidia", c.pf.str, c.driver);
		} else {
			struct gs_host h = { .sys = sysroot, .proc = live ? GS_LIVE_PROC : NULL };
			struct gs_resource_fit fit;

			if (gs_resource_fit_assess(&h, &c, gs_config_vfs(&cfg), &fit))
				check(&t, "FAIL", "%s: resource assessment failed", c.pf.str);
			else
				check(&t, fit.state == GS_RESOURCE_FIT_INSUFFICIENT ? "FAIL" :
				      fit.state == GS_RESOURCE_FIT_UNKNOWN ? "WARN" : "OK",
				      "%s: VF resources source=%s count=%u nonprefetch need=%llu window=%llu "
				      "prefetch need=%llu window=%llu", c.pf.str, fit.source ? fit.source : "unknown",
				      fit.vfs, fit.nonprefetch_need, fit.nonprefetch_window,
				      fit.prefetch_need, fit.prefetch_window);
			if (c.has_sriov && gs_check_presented(&c, &cfg, err, sizeof(err)) == 0 && c.numvfs > 0)
				check(&t, "OK", "%s: SR-IOV presented, VF resources assigned, %u VF(s) enabled", c.pf.str,
				      c.numvfs);
		else if (pre)
				check(&t, "INFO", "%s: activation plan: check and stop display clients as needed, load the "
				      "livepatch and the driver, remove and rescan the empty bus when needed, enable VFs and "
				      "configure them", c.pf.str);
		}
	}
	if (nlaptops)
		laptop_host_checks(&t, o, live, laptops, nlaptops);
	if (post && live && n) {
		struct gs_host h = { .sys = sysroot, .proc = GS_LIVE_PROC, .run = gs_run, .capture = gs_run_capture };
		struct gs_display_plan dp;

		gs_display_plan_build(&h, &cfg, &dp);
		if (dp.inspection_unknown)
			check(&t, "WARN", "display clients: %s",
			      dp.reason[0] ? dp.reason : "the inspection is incomplete; activation refuses rather than assume none");
		else if (dp.reason[0])
			check(&t, "INFO", "display clients: %s", dp.reason);
		else
			check(&t, "OK", "display clients: nothing holds the selected GPUs");
		if (dp.manager_active)
			check(&t, "INFO", "display manager: %s is active; activate stops it only after consent and restarts it afterwards",
			      dp.manager_unit);
	}
	{
		char ada[GS_BDF_STR_SIZE], other[GS_BDF_STR_SIZE];
		unsigned int gen = 0;

		if (selection_mixes_ada(sysroot, devs, n, ada, other, sizeof(ada), &gen))
			check(&t, "WARN", "selection mixes Ada (%s) with %s (%s): one host module drives both generations, "
			      "with %u VFs per card, and one vGPU plugin copy serves both",
			      ada, gs_generation_name(gen), other, gs_config_vfs(&cfg));
	}
	if (nisel && !id_on) {
		check(&t, "INFO", "identity: off (%s): the hook passes every call through, so nvidia-vgpud sees the GeForce IDs and registers no vGPU types for them",
		      isrc);
	} else if (nisel) {
		if (pre)
			printf("\nidentity (%s)\n", isrc);
		identity_checks(&t, sysroot, isel, nisel, id_force);
	}
	if (pre)
		printf("\n%u failure(s), %u warning(s)\n", t.fail, t.warn);
	return t.fail ? GS_EXIT_REFUSED : GS_EXIT_OK;
}
