// SPDX-License-Identifier: MIT
/*
 * Read-only PCI inspection through a sysfs root: the card and bus facts every command reads,
 * the console scan, the VF BAR sizing and the daemon admission predicate. Nothing here writes
 * to the host; activation performs the writes through struct gs_host.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/file.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gs_lifecycle.h"

static void read_driver(const char *devdir, char *out, size_t len)
{
	char link[GS_PATH_MAX], target[GS_PATH_MAX];
	ssize_t n;
	const char *base;

	out[0] = '\0';
	if (gs_pathf(link, sizeof(link), "%s/driver", devdir))
		return;
	n = readlink(link, target, sizeof(target) - 1);
	if (n <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	gs_copy(out, len, base ? base + 1 : target);
}

static void read_fn(const char *sysroot, const struct gs_addr *a, struct gs_fn *fn)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX];

	memset(fn, 0, sizeof(*fn));
	fn->bdf = *a;
	if (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, a->str))
		return;
	if (!gs_pathf(p, sizeof(p), "%s/vendor", dir))
		gs_read_hex(p, &fn->vendor);
	if (!gs_pathf(p, sizeof(p), "%s/device", dir))
		gs_read_hex(p, &fn->device);
	if (!gs_pathf(p, sizeof(p), "%s/class", dir))
		gs_read_hex(p, &fn->class_code);
	if (!gs_pathf(p, sizeof(p), "%s/physfn", dir))
		fn->is_vf = gs_exists(p);
	read_driver(dir, fn->driver, sizeof(fn->driver));
}

/* DevCap2 ARI Forwarding Supported of a bridge, from its sysfs config file; -1 when unreadable. */
static int bridge_ari_forwarding(const char *bridge_dir)
{
	unsigned char cfg[256];
	char p[GS_PATH_MAX];
	unsigned int pos, guard = 0;
	ssize_t n;
	int fd;

	if (gs_pathf(p, sizeof(p), "%s/config", bridge_dir))
		return -1;
	fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = pread(fd, cfg, sizeof(cfg), 0);
	close(fd);
	/* Unprivileged reads of sysfs config stop at 64 bytes. */
	if (n < (ssize_t)sizeof(cfg) || !(cfg[0x06] & 0x10))
		return -1;
	pos = cfg[0x34] & 0xfc;
	while (pos >= 0x40 && pos < 0xfc && guard++ < 48) {
		if (cfg[pos] == 0x10) { /* PCI Express capability */
			unsigned int devcap2;

			if (pos + 0x28 > sizeof(cfg))
				return -1;
			devcap2 = (unsigned int)cfg[pos + 0x24] | (unsigned int)cfg[pos + 0x25] << 8 |
				  (unsigned int)cfg[pos + 0x26] << 16 | (unsigned int)cfg[pos + 0x27] << 24;
			return (devcap2 >> 5) & 1;
		}
		pos = cfg[pos + 1] & 0xfc;
	}
	return -1;
}

/*
 * Is MSI-X (capability 0x11) in the PF's capability list, walked as the kernel's
 * pci_find_capability() does? -1 when <dir>/config cannot tell: unreadable, shorter than 0x40
 * bytes, ending before the walk does (unprivileged reads stop at 64 bytes), or reading all-ones
 * at its start, as a powered-off (D3cold) or lost device does: there is no list to walk then.
 */
static int msix_linked(const char *dir)
{
	unsigned char cfg[256];
	char p[GS_PATH_MAX];
	unsigned int pos, guard = 0;
	ssize_t n;
	int fd;

	if (gs_pathf(p, sizeof(p), "%s/config", dir))
		return -1;
	fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = pread(fd, cfg, sizeof(cfg), 0);
	close(fd);
	if (n < 0x40 || (cfg[0] & cfg[1] & cfg[2] & cfg[3]) == 0xff)
		return -1;
	if (!(cfg[0x06] & 0x10))
		return 0;
	pos = cfg[0x34];
	while (guard++ < 48) {
		pos &= 0xfc;
		if (pos < 0x40)
			return 0;
		if ((ssize_t)pos + 1 >= n)
			return -1;
		if (cfg[pos] == 0xff)
			return 0;
		if (cfg[pos] == 0x11)
			return 1;
		pos = cfg[pos + 1];
	}
	return 0;
}

/* <pf>/resource: is BAR0 (line 0), and is any VF BAR (lines 7-12) assigned? -1 stays when unknown. */
static void read_resources(const char *dir, struct gs_card *card)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	unsigned int n = 0;
	size_t len;
	int vf = 0;

	if (gs_pathf(p, sizeof(p), "%s/resource", dir) || gs_read_whole(p, &data, &len, 1 << 14))
		return;
	for (line = strtok_r(data, "\n", &save); line && n <= 12; line = strtok_r(NULL, "\n", &save), n++) {
		unsigned long long start, end, flags;

		if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3)
			break;
		if (n == 0)
			card->bar0_assigned = start || end;
		else if (n >= 7)
			vf |= start || end;
		if (n == 12)
			card->vf_bars_assigned = vf;
	}
	free(data);
}

int gs_card_inspect(const char *sysroot, const struct gs_addr *pf, struct gs_card *card)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], real[PATH_MAX], parent[PATH_MAX];
	char busname[GS_BUS_STR_SIZE];
	struct gs_fn pffn;
	DIR *d;
	struct dirent *de;
	char *slash;
	unsigned int v;
	char buf[32];

	memset(card, 0, sizeof(*card));
	card->pf = *pf;
	card->boot_vga = -1;
	card->ari_enabled = -1;
	card->bridge_ari_fwd_supported = -1;
	card->bar0_assigned = -1;
	card->vf_bars_assigned = -1;
	card->msix_linked = -1;
	if (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, pf->str))
		return -ENAMETOOLONG;
	if (!realpath(dir, real))
		return 0;
	card->present = 1;
	read_fn(sysroot, pf, &pffn);
	card->vendor = pffn.vendor;
	card->device = pffn.device;
	card->class_code = pffn.class_code;
	card->is_vf = pffn.is_vf;
	snprintf(card->driver, sizeof(card->driver), "%s", pffn.driver);
	if (!gs_pathf(p, sizeof(p), "%s/subsystem_vendor", dir))
		gs_read_hex(p, &card->subvendor);
	if (!gs_pathf(p, sizeof(p), "%s/subsystem_device", dir))
		gs_read_hex(p, &card->subdevice);
	if (card->vendor == GS_VENDOR_NVIDIA)
		card->entry = gs_device_lookup(card->device);
	if (!gs_pathf(p, sizeof(p), "%s/boot_vga", dir) && !gs_read_uint(p, &v))
		card->boot_vga = v ? 1 : 0;
	if (!gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", dir) && !gs_read_uint(p, &v)) {
		card->has_sriov = 1;
		card->totalvfs = v;
		if (!gs_pathf(p, sizeof(p), "%s/sriov_numvfs", dir) && !gs_read_uint(p, &v))
			card->numvfs = v;
	}
	read_resources(dir, card);
	card->msix_linked = msix_linked(dir);
	if (!gs_pathf(p, sizeof(p), "%s/ari_enabled", dir) && !gs_read_uint(p, &v))
		card->ari_enabled = v ? 1 : 0;
	if (!gs_pathf(p, sizeof(p), "%s/iommu_group", dir)) {
		char target[GS_PATH_MAX];
		ssize_t n = readlink(p, target, sizeof(target) - 1);

		if (n > 0) {
			target[n] = '\0';
			slash = strrchr(target, '/');
			gs_copy(card->iommu_group, sizeof(card->iommu_group), slash ? slash + 1 : target);
		}
	}

	/* Parent of the function's device directory: a bridge function or the host bridge (pciDDDD:BB). */
	snprintf(parent, sizeof(parent), "%s", real);
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	slash = strrchr(parent, '/');
	if (slash && gs_addr_parse(slash + 1, &card->bridge) == 0) {
		card->bridge_is_pci = 1;
		snprintf(card->bridge_dir, sizeof(card->bridge_dir), "%s", parent);
		snprintf(busname, sizeof(busname), "%04x:%02x", pf->domain, pf->bus);
		if (!gs_pathf(card->rescan_path, sizeof(card->rescan_path), "%s/pci_bus/%s/rescan", parent,
			      busname))
			card->rescan_present = gs_exists(card->rescan_path);
		if (!gs_pathf(p, sizeof(p), "%s/power/control", parent) &&
		    gs_read_attr(p, buf, sizeof(buf)) == 0)
			gs_copy(card->bridge_pm, sizeof(card->bridge_pm), buf);
		if (!gs_pathf(p, sizeof(p), "%s/power/runtime_status", parent) &&
		    gs_read_attr(p, buf, sizeof(buf)) == 0)
			gs_copy(card->bridge_rt, sizeof(card->bridge_rt), buf);
		card->bridge_ari_fwd_supported = bridge_ari_forwarding(parent);
	}

	/* Every function on the PF's bus: the card's own (same slot) and anything else. */
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot))
		return 0;
	d = opendir(p);
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		struct gs_addr a;
		struct gs_fn fn;

		if (gs_addr_parse(de->d_name, &a) || a.domain != pf->domain || a.bus != pf->bus ||
		    strcmp(a.str, pf->str) == 0)
			continue;
		read_fn(sysroot, &a, &fn);
		if (fn.is_vf)
			card->nvfs_on_bus++;
		if (a.slot == pf->slot) {
			if (card->nfns < GS_MAX_BUS_FNS)
				card->fns[card->nfns++] = fn;
		} else if (card->nothers < GS_MAX_BUS_FNS) {
			card->others[card->nothers++] = fn;
		}
	}
	closedir(d);
	return 0;
}

/*
 * Whether activation puts the host console (KMS) on this card. The console follows the boot
 * display GPU only: display=auto picks a selected boot display GPU, display=off none, and
 * display=on is limited to the boot display GPU as well. Measured: KMS on a card that is not the
 * boot display breaks that card's first VF (NVOS status 0x51, "Failed to alloc guest FB memory")
 * while its other VFs keep working. An unknown boot_vga is not the boot display. Activation and
 * status all report from this one rule.
 */
int gs_display_kms(const struct gs_config *cfg, const struct gs_card *c)
{
	if (cfg->display == GS_DISPLAY_OFF)
		return 0;
	return c->boot_vga == 1;
}

/* Checks for `install --device` and setup's listing: the PF may legitimately be bound to nvidia. */
int gs_check_selectable(const struct gs_card *c, char *reason, size_t len)
{
	if (!c->present)
		return snprintf(reason, len, "%s: no such PCI function", c->pf.str), -1;
	if (c->pf.func != 0 || c->is_vf)
		return snprintf(reason, len, "%s: not a physical function .0", c->pf.str), -1;
	if (c->vendor != GS_VENDOR_NVIDIA || !c->entry)
		return snprintf(reason, len, "%s (%04x:%04x) is not in the device table", c->pf.str,
				c->vendor, c->device), -1;
	if ((c->class_code >> 16) != 0x03)
		return snprintf(reason, len, "%s: not a display controller", c->pf.str), -1;
	if (c->driver[0] && strcmp(c->driver, "nvidia") != 0)
		return snprintf(reason, len, "%s is bound to %s; select it while it is bound to nvidia or to no driver",
				c->pf.str, c->driver), -1;
	if (c->nothers > 0)
		return snprintf(reason, len, "%s: bus shared with %s", c->pf.str, c->others[0].bdf.str), -1;
	if (!c->bridge_is_pci)
		return snprintf(reason, len, "%s: on a root bus without a PCI bridge", c->pf.str), -1;
	return 0;
}

/* ---- boot display consumers (read-only) ----------------------------------- */

static void display_refuse(struct gs_display *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Keep the first reason: it names what to fix first. */
static void display_refuse(struct gs_display *d, const char *fmt, ...)
{
	va_list ap;

	if (d->refuse[0])
		return;
	va_start(ap, fmt);
	vsnprintf(d->refuse, sizeof(d->refuse), fmt, ap);
	va_end(ap);
}

/* name is prefix followed by one or more decimal digits only. */
static int name_with_number(const char *name, const char *prefix)
{
	size_t n = strlen(prefix);
	const char *p = name + n;

	if (strncmp(name, prefix, n) != 0 || !*p)
		return 0;
	for (; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	return 1;
}

/* A platform device the kernel's sysfb creates for a firmware framebuffer (drivers/firmware/sysfb.c). */
static int is_sysfb_device(const char *name)
{
	static const char *const kinds[] = { GS_SIMPLEFB_PREFIX, "efi-framebuffer.", "vesa-framebuffer.",
					     "vga-framebuffer.", "ega-framebuffer.", "platform-framebuffer." };

	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
		if (name_with_number(name, kinds[i]))
			return 1;
	return 0;
}

static int path_under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static const char *base_of(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

/* The firmware framebuffer platform devices: only one, a simple-framebuffer child of this PF, is accepted. */
static void scan_firmware_fb(const char *sysroot, const char *pfreal, struct gs_display *d)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], real[PATH_MAX], parent[PATH_MAX];
	struct dirent *de;
	DIR *dd;

	if (gs_pathf(dir, sizeof(dir), "%s/bus/platform/devices", sysroot)) {
		display_refuse(d, "sysfs path too long");
		return;
	}
	dd = opendir(dir);
	if (!dd) {
		if (errno != ENOENT)
			display_refuse(d, "%s cannot be read", dir);
		return;
	}
	while ((de = readdir(dd))) {
		if (!is_sysfb_device(de->d_name))
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name) || !realpath(p, real)) {
			display_refuse(d, "firmware framebuffer %.64s does not resolve to a device; its owner cannot be proven",
				       de->d_name);
			continue;
		}
		gs_copy(parent, sizeof(parent), real);
		*strrchr(parent, '/') = '\0';
		if (strcmp(parent, pfreal) != 0) {
			display_refuse(d, "firmware framebuffer %.64s belongs to %.64s, not to this card; which card it scans out of cannot be proven",
				       de->d_name, base_of(parent));
			continue;
		}
		if (!name_with_number(de->d_name, GS_SIMPLEFB_PREFIX)) {
			display_refuse(d, "firmware framebuffer %.64s is not a simple-framebuffer device; only simple-framebuffer is released",
				       de->d_name);
			continue;
		}
		if (d->fw[0]) {
			display_refuse(d, "more than one firmware framebuffer (%s, %.64s)", d->fw, de->d_name);
			continue;
		}
		gs_copy(d->fw, sizeof(d->fw), de->d_name);
		gs_copy(d->fw_dir, sizeof(d->fw_dir), real);
		{
			char link[GS_PATH_MAX], target[GS_PATH_MAX];
			ssize_t n;

			if (!gs_pathf(link, sizeof(link), "%s/driver", real) &&
			    (n = readlink(link, target, sizeof(target) - 1)) > 0) {
				target[n] = '\0';
				gs_copy(d->fw_driver, sizeof(d->fw_driver), base_of(target));
			}
		}
		/* simpledrm and simplefb both register the platform driver "simple-framebuffer". */
		if (d->fw_driver[0] && strcmp(d->fw_driver, "simple-framebuffer") != 0)
			display_refuse(d, "%s is bound to '%s', not to simple-framebuffer (simpledrm or simplefb)", d->fw,
				       d->fw_driver);
	}
	closedir(dd);
}

/*
 * fb and DRM devices: each must belong to the PF's own firmware framebuffer or to another PCI
 * device. One on the PF itself, on another platform device, or without a resolvable device refuses.
 */
static void scan_class(const char *sysroot, const char *cls, const char *const *prefixes, const char *pfreal,
		       struct gs_display *d)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], real[PATH_MAX];
	struct dirent *de;
	DIR *dd;

	if (gs_pathf(dir, sizeof(dir), "%s/class/%s", sysroot, cls)) {
		display_refuse(d, "sysfs path too long");
		return;
	}
	dd = opendir(dir);
	if (!dd) {
		if (errno != ENOENT)
			display_refuse(d, "%s cannot be read", dir);
		return;
	}
	while ((de = readdir(dd))) {
		struct gs_addr a;
		int match = 0;

		for (size_t i = 0; prefixes[i]; i++)
			match |= name_with_number(de->d_name, prefixes[i]);
		if (!match)
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s/device", dir, de->d_name) || !realpath(p, real)) {
			display_refuse(d, "%s/%.64s has no resolvable device; which card it uses cannot be told", cls,
				       de->d_name);
			continue;
		}
		if (d->fw_dir[0] && strcmp(real, d->fw_dir) == 0) {
			d->nconsumers++;
			continue;
		}
		if (path_under(real, pfreal)) {
			display_refuse(d, "%s/%.64s is on the card itself (%.64s), not on its firmware framebuffer", cls,
				       de->d_name, base_of(real));
			continue;
		}
		if (gs_addr_parse(base_of(real), &a) != 0)
			display_refuse(d, "%s/%.64s belongs to %.64s, which is not a PCI device; it may scan out of this card",
				       cls, de->d_name, base_of(real));
	}
	closedir(dd);
}

/* The legacy VGA text console (vgacon, "VGA+") uses the boot display's legacy ranges. */
static void scan_vtconsoles(const char *sysroot, struct gs_display *d)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], name[128];
	struct dirent *de;
	unsigned int bind;
	DIR *dd;

	if (gs_pathf(dir, sizeof(dir), "%s/class/vtconsole", sysroot)) {
		display_refuse(d, "sysfs path too long");
		return;
	}
	dd = opendir(dir);
	if (!dd) {
		if (errno != ENOENT)
			display_refuse(d, "%s cannot be read", dir);
		return;
	}
	while ((de = readdir(dd))) {
		if (!name_with_number(de->d_name, "vtcon"))
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s/name", dir, de->d_name) || gs_read_attr(p, name, sizeof(name)) ||
		    gs_pathf(p, sizeof(p), "%s/%s/bind", dir, de->d_name) || gs_read_uint(p, &bind)) {
			display_refuse(d, "vtconsole %.64s cannot be read", de->d_name);
			continue;
		}
		if (bind && strstr(name, "VGA+"))
			display_refuse(d, "the legacy VGA text console (%s) is bound", name);
	}
	closedir(dd);
}

int gs_display_inspect(const char *sysroot, const char *procroot, const struct gs_card *c, struct gs_display *d)
{
	static const char *const fb_names[] = { "fb", NULL };
	static const char *const drm_names[] = { "card", "renderD", NULL };
	char p[GS_PATH_MAX], pfreal[PATH_MAX], why[256];
	char *text;
	size_t len;

	memset(d, 0, sizeof(*d));
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sysroot, c->pf.str) || !realpath(p, pfreal)) {
		display_refuse(d, "%s does not resolve", c->pf.str);
		return 0;
	}
	scan_firmware_fb(sysroot, pfreal, d);
	scan_class(sysroot, "graphics", fb_names, pfreal, d);
	scan_class(sysroot, "drm", drm_names, pfreal, d);
	scan_vtconsoles(sysroot, d);
	if (gs_display_fw_state(d) == GS_FW_RELEASED && d->nconsumers)
		display_refuse(d, "%s is unbound, but %u fb/DRM device(s) of it remain", d->fw, d->nconsumers);
	if (!procroot)
		return 0;
	d->proc_checked = 1;
	if (gs_pathf(p, sizeof(p), "%s/iomem", procroot) || gs_read_whole(p, &text, &len, 1 << 22)) {
		display_refuse(d, "%s/iomem cannot be read", procroot);
	} else {
		if (gs_iomem_check(text, c->pf.str, gs_display_fw_state(d), why, sizeof(why)))
			display_refuse(d, "%s", why);
		free(text);
	}
	if (gs_pathf(p, sizeof(p), "%s/consoles", procroot) || gs_read_whole(p, &text, &len, 1 << 20)) {
		display_refuse(d, "%s/consoles cannot be read", procroot);
	} else {
		if (gs_consoles_check(text, why, sizeof(why)))
			display_refuse(d, "%s", why);
		free(text);
	}
	return 0;
}

int gs_iomem_check(const char *iomem, const char *pf, enum gs_fw_state fw, char *why, size_t len)
{
	const char *line = iomem;
	long bar = -1, bootfb = -1; /* indentation of the PF BAR and of BOOTFB being walked */
	unsigned int nboot = 0, lineno = 0;

	if (!iomem || !*iomem)
		return snprintf(why, len, "/proc/iomem is empty"), -1;
	while (*line) {
		const char *nl = strchr(line, '\n'), *sep, *name;
		size_t n = nl ? (size_t)(nl - line) : strlen(line), namelen;
		long ind = 0;

		lineno++;
		while ((size_t)ind < n && line[ind] == ' ')
			ind++;
		sep = memmem(line, n, " : ", 3);
		if (!sep)
			return snprintf(why, len, "/proc/iomem line %u cannot be parsed", lineno), -1;
		name = sep + 3;
		namelen = n - (size_t)(name - line);
		if (bootfb >= 0 && ind > bootfb) {
			if (fw != GS_FW_BOUND)
				return snprintf(why, len, "'%.*s' still holds memory inside BOOTFB of %s", (int)namelen,
						name, pf), -1;
		} else if (bar >= 0 && ind > bar) {
			bootfb = -1;
			if (namelen == 6 && memcmp(name, "BOOTFB", 6) == 0 && fw != GS_FW_NONE && !nboot) {
				nboot++;
				bootfb = ind;
			} else {
				return snprintf(why, len, "'%.*s' uses memory inside a BAR of %s", (int)namelen, name,
						pf), -1;
			}
		} else {
			bar = bootfb = -1;
			if (namelen == strlen(pf) && memcmp(name, pf, namelen) == 0) {
				/*
				 * r_show() indents at most 2 * MAX_IORES_LEVEL spaces (5 levels in older
				 * kernels): from there on children print level with their parent.
				 */
				if (ind >= 10)
					return snprintf(why, len, "a BAR of %s is nested too deep for /proc/iomem to show what uses it",
							pf), -1;
				bar = ind;
			}
		}
		if (!nl)
			break;
		line = nl + 1;
	}
	return 0;
}

int gs_consoles_check(const char *consoles, char *why, size_t len)
{
	const char *line = consoles;

	while (line && *line) {
		if (strncmp(line, "efifb", 5) == 0)
			return snprintf(why, len, "an early EFI framebuffer console (efifb, earlycon) is registered"), -1;
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return 0;
}

int gs_check_presented(const struct gs_card *c, const struct gs_config *cfg, char *why, size_t len)
{
	unsigned int vfs = gs_config_vfs(cfg);

	if (!c->has_sriov)
		return snprintf(why, len, "%s presents no SR-IOV (sriov_totalvfs absent)", c->pf.str), -1;
	if (c->totalvfs < 1 || c->totalvfs > vfs)
		return snprintf(why, len, "%s presents sriov_totalvfs=%u, outside 1..%u (the configured vfs); a changed vfs takes effect at the next boot",
				c->pf.str, c->totalvfs, vfs), -1;
	return 0;
}

/* ---- VF BAR sizing and the bridge growth assessment (read-only) ----------- */

/* The name of the driver bdf is bound to; "" when unbound. */
static void pci_driver(const char *sys, const char *bdf, char *out, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	const char *base;
	ssize_t n;

	out[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver", sys, bdf))
		return;
	n = readlink(p, target, sizeof(target) - 1);
	if (n <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	gs_copy(out, len, base ? base + 1 : target);
}

/* virtfn0..numvfs-1 of pf, each confirmed by its own physfn link back to pf. 0, or -1 with why. */
static int pci_vf_list(const char *sys, const struct gs_addr *pf, unsigned int numvfs, struct gs_addr *out,
		       char *why, size_t len)
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	const char *base;

	if (numvfs > GS_VFS_MAX)
		return snprintf(why, len, "%s has %u VFs, more than %u", pf->str, numvfs, GS_VFS_MAX), -1;
	for (unsigned int i = 0; i < numvfs; i++) {
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sys, pf->str, i) || !realpath(p, real))
			return snprintf(why, len, "%s: virtfn%u is missing", pf->str, i), -1;
		base = strrchr(real, '/');
		if (gs_addr_parse(base ? base + 1 : real, &out[i]))
			return snprintf(why, len, "%s: virtfn%u names no PCI address", pf->str, i), -1;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, out[i].str) || !realpath(p, real) ||
		    !(base = strrchr(real, '/')) || strcmp(base + 1, pf->str) != 0)
			return snprintf(why, len, "%s: the physfn link of its virtfn%u %s does not lead back to it", pf->str, i,
					out[i].str), -1;
	}
	return 0;
}

/*
 * The per-BAR address space a VF needs, from the device table. The table holds per-VF sizes,
 * never the aggregate the kernel already assigned (SPEC section 5, owner decision 7).
 * 1 when every implemented BAR has an entry, 0 when this card's layout is not in the table.
 */
static int vf_bar_need(unsigned int generation, unsigned int vfs, unsigned long long *nonprefetch,
		       unsigned long long *prefetch)
{
	unsigned long long np = 0, pf = 0;

	for (unsigned int bar = 0; bar <= 3; bar++) {
		struct gs_vf_bar_size s;

		if (bar == 2)
			continue; /* VF BAR2 is part of VF BAR1's 64-bit register pair */
		if (!gs_vf_bar_size_lookup(generation, bar, &s))
			return 0;
		if (s.bytes > 0xffffffffull / vfs)
			return 0;
		if (s.prefetchable)
			pf += s.bytes * vfs;
		else
			np += s.bytes * vfs;
	}
	*nonprefetch = np;
	*prefetch = pf;
	return 1;
}

/* Bridge windows from <dir>/resource lines: 0xstart 0xend 0xflags. -1 when none could be read. */
static int bridge_windows(const char *dir, unsigned long long *io, unsigned long long *mem,
			  unsigned long long *pref, int *have_mem, int *have_pref)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t len;
	int any = 0;

	*io = *mem = *pref = 0;
	*have_mem = *have_pref = 0;
	if (gs_pathf(p, sizeof(p), "%s/resource", dir) || gs_read_whole(p, &data, &len, 65536))
		return -1;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned long long start, end, flags, size;

		if (sscanf(line, "0x%llx 0x%llx 0x%llx", &start, &end, &flags) != 3)
			continue;
		if (end < start || flags == 0)
			continue;
		size = end - start + 1;
		if (flags & 0x100ull) { /* IORESOURCE_IO */
			*io += size;
			any = 1;
		} else if (flags & 0x200ull) { /* IORESOURCE_MEM */
			if (flags & 0x2000ull) { /* IORESOURCE_PREFETCH */
				*pref += size;
				*have_pref = 1;
			} else {
				*mem += size;
				*have_mem = 1;
			}
			any = 1;
		}
	}
	free(data);
	return any ? 0 : -1;
}

/*
 * The spare address space above the bridge: the parent bridge's window minus what its children
 * already decode, which bounds how far a rescan can grow this bridge. -1 when not determinable.
 */
static int growth_room(const char *sysroot, const char *bridge_dir, int prefetch, unsigned long long *room)
{
	char p[GS_PATH_MAX], parent[PATH_MAX], real[GS_PATH_MAX], child[GS_PATH_MAX], *slash;
	unsigned long long pio, pmem, ppref, used = 0;
	struct dirent *de;
	int hmem, hpref, found = 0;
	DIR *d;

	if (!realpath(bridge_dir, real) || !(slash = strrchr(real, '/')))
		return -1;
	*slash = '\0';
	gs_copy(parent, sizeof(parent), real);
	if (bridge_windows(parent, &pio, &pmem, &ppref, &hmem, &hpref) || (prefetch ? !hpref : !hmem))
		return -1;
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot) || !(d = opendir(p)))
		return -1;
	while ((de = readdir(d))) {
		unsigned long long cio, cmem, cpref, *window;
		char cdir[GS_PATH_MAX], creal[PATH_MAX], *cslash;
		int chm, chp;

		if (de->d_name[0] == '.' || gs_pathf(cdir, sizeof(cdir), "%s/bus/pci/devices/%s", sysroot, de->d_name) ||
		    !realpath(cdir, creal) || !(cslash = strrchr(creal, '/')) || gs_pathf(child, sizeof(child), "%s", creal))
			continue;
		*cslash = '\0';
		if (strcmp(creal, parent) != 0)
			continue;
		if (bridge_windows(child, &cio, &cmem, &cpref, &chm, &chp))
			continue;
		window = prefetch ? &cpref : &cmem;
		used += *window;
		found = 1;
	}
	closedir(d);
	if (!found)
		return -1;
	*room = (prefetch ? ppref : pmem) > used ? (prefetch ? ppref : pmem) - used : 0;
	return 0;
}

int gs_resource_fit_assess(const struct gs_host *h, const struct gs_card *card, unsigned int vfs,
			   struct gs_resource_fit *fit)
{
	unsigned long long np = 0, pf = 0, io, mem, pref;
	int have_table, have_mem = 0, have_pref = 0;

	memset(fit, 0, sizeof(*fit));
	fit->vfs = vfs;
	fit->state = GS_RESOURCE_FIT_UNKNOWN;
	if (vfs < 1)
		return -1;
	have_table = card->entry && vf_bar_need(card->entry->generation, vfs, &np, &pf);
	fit->nonprefetch_need = np;
	fit->prefetch_need = pf;
	if (!card->bridge_dir[0] || !card->bridge_is_pci ||
	    bridge_windows(card->bridge_dir, &io, &mem, &pref, &have_mem, &have_pref)) {
		fit->source = "the bridge windows are unreadable";
	} else {
		fit->source = "the bridge's current windows";
		fit->nonprefetch_window = have_mem ? mem : 0;
		fit->prefetch_window = have_pref ? pref : 0;
	}

	/* Already placed by the firmware or an earlier rescan: nothing has to fit. */
	if (card->vf_bars_assigned == 1) {
		fit->state = GS_RESOURCE_FIT_OK;
		fit->source = "the VF BARs are assigned on this bus";
		return 0;
	}
	if (!have_table) {
		/* Owner decision 7: no table entry for this card, proceed with a warning that names it. */
		fit->source = card->entry ? "no VF BAR size entry for this card's layout" :
					    "the card is not in the device table";
		return 0;
	}
	if (!have_mem || !have_pref)
		return 0; /* unknown, and the caller refuses a removal it cannot size */

	if (np <= fit->nonprefetch_window && pf <= fit->prefetch_window) {
		fit->state = GS_RESOURCE_FIT_OK;
		return 0;
	}
	/* The rescan grows the bridge: judge against the room above it, not only the window. */
	{
		unsigned long long npgap = np > fit->nonprefetch_window ? np - fit->nonprefetch_window : 0;
		unsigned long long pfgap = pf > fit->prefetch_window ? pf - fit->prefetch_window : 0;
		unsigned long long room;

		if (growth_room(h->sys, card->bridge_dir, pfgap ? 1 : 0, &room)) {
			/* A root port sits directly under the host bridge, which exposes no window: the
			 * space the kernel can reclaim is not computable here. Leave the fit UNKNOWN (a
			 * warning, not a refusal) and let activation judge the rescan's result, which it
			 * already does by requiring the VF BARs to be assigned before it enables a VF. */
			fit->state = GS_RESOURCE_FIT_UNKNOWN;
			fit->room_unknown = 1;
			fit->source = "the room above the bridge is not determinable (root port); the rescan decides";
		} else if (npgap <= room && pfgap <= room) {
			fit->state = GS_RESOURCE_FIT_OK;
			fit->source = "the bridge grows on rescan; the room above it suffices";
		} else {
			fit->state = GS_RESOURCE_FIT_INSUFFICIENT;
			fit->source = "the need exceeds the bridge windows and the room above them";
		}
	}
	return 0;
}

/* ---- daemon admission (read-only) ---------------------------------------- */

/*
 * 1 while a geforce-sriov process holds the lock in h->run_dir, which activation does for its whole
 * run. The activating record is trusted only then: a record left by a killed run must not make
 * the guard invisible.
 */
static int activation_running(const struct gs_host *h)
{
	char p[GS_PATH_MAX];
	int fd, held;

	if (gs_pathf(p, sizeof(p), "%s/lock", h->run_dir))
		return 0;
	fd = open(p, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return 0;
	held = flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK;
	close(fd);
	return held;
}

/*
 * The predicate vgpud's ExecStartPre runs through `check --daemon-ready`: every selected PF
 * presents SR-IOV with the configured count, its VF resources are assigned, and its VFs are
 * enabled with valid parent links and bound to nvidia. It deliberately does not look at the
 * vGPU types the daemons publish (they do not exist before vgpud runs) and it ignores the
 * activating marker that activate itself writes.
 */
int gs_daemon_ready(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	char p[GS_PATH_MAX], act[GS_PATH_MAX], why[512];
	unsigned int bad = 0;
	int activating;

	(void)o;
	if (gs_pathf(act, sizeof(act), "%s/" GS_ACTIVATING_NAME, h->run_dir))
		act[0] = '\0';
	activating = act[0] && gs_exists(act) && activation_running(h);
	/*
	 * The guard of a running activation is not persistent: activation writes it before its first
	 * change and clears it on success, and `activating` names that run (SPEC: this predicate must
	 * not reject activation's own marker). Without the marker, or with a marker that no running
	 * activation stands behind, a guard means an earlier run did not finish, and vgpud must not
	 * start.
	 */
	if (!activating && (!gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, h->state_dir) && gs_exists(p))) {
		fprintf(stderr, "geforce-sriov: not ready: the activation lock %s is set (an earlier activation did not finish)\n",
			p);
		return GS_EXIT_REFUSED;
	}
	if (gs_selection_verify(h, cfg, NULL))
		return GS_EXIT_REFUSED;
	for (unsigned int i = 0; i < cfg->nbdfs; i++) {
		struct gs_addr vf[GS_VFS_MAX];
		struct gs_card c;
		char drv[64];

		gs_card_inspect(h->sys, &cfg->bdfs[i], &c);
		if (!c.present) {
			fprintf(stderr, "geforce-sriov: not ready: %s is missing\n", cfg->bdfs[i].str);
			bad++;
			continue;
		}
		why[0] = '\0';
		if (gs_check_presented(&c, cfg, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: not ready: %s\n", why);
			bad++;
			continue;
		}
		if (c.numvfs < 1 || c.numvfs != c.totalvfs || c.numvfs > GS_VFS_MAX ||
		    pci_vf_list(h->sys, &c.pf, c.numvfs, vf, why, sizeof(why))) {
			if (!why[0])
				snprintf(why, sizeof(why), "%s has %u of %u VFs enabled", c.pf.str, c.numvfs, c.totalvfs);
			fprintf(stderr, "geforce-sriov: not ready: %s\n", why);
			bad++;
			continue;
		}
		if (c.bar0_assigned != 1 || c.vf_bars_assigned != 1) {
			fprintf(stderr, "geforce-sriov: not ready: %s: BAR0 or a VF BAR is unassigned or unreadable\n", c.pf.str);
			bad++;
			continue;
		}
		for (unsigned int j = 0; j < c.numvfs; j++) {
			pci_driver(h->sys, vf[j].str, drv, sizeof(drv));
			if (strcmp(drv, "nvidia") != 0) {
				fprintf(stderr, "geforce-sriov: not ready: VF %s of %s is bound to %s, not nvidia\n",
					vf[j].str, c.pf.str, drv[0] ? drv : "nothing");
				bad++;
				break;
			}
		}
	}
	return bad ? GS_EXIT_REFUSED : GS_EXIT_OK;
}
