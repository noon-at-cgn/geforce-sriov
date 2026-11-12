// SPDX-License-Identifier: MIT
/*
 * Readers of the host that more than one command needs: the kernel command line, NVIDIA's driver
 * and module identity, the livepatch module's state, a PF's VFs, the selected cards. Each is
 * defined here once. They read; none of them writes to the host.
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

const char *const gs_recovery_msg =
	"Recovery: do not rescan again or restart in a loop. If not already done since the failure,\n"
	"power the machine off and on. After the cold boot, run `geforce-sriov doctor` and, once the\n"
	"cards are known safe, `geforce-sriov activate --clear-lock`, then `geforce-sriov activate`.\n"
	"The early livepatch can load on the recovery boot; activation remains blocked until cleared.";

int gs_addr_listed(const struct gs_addr *list, unsigned int n, const char *s)
{
	for (unsigned int i = 0; i < n; i++)
		if (strcmp(list[i].str, s) == 0)
			return 1;
	return 0;
}

const struct gs_build_src gs_build_live = { 0 };

/* 1 when the kernel command line carries pci=...noari... (the module cannot see it). */
int gs_cmdline_noari(const char *procroot)
{
	char p[GS_PATH_MAX], *data, *tok, *save = NULL;
	size_t len;
	int found = 0;

	if (gs_pathf(p, sizeof(p), "%s/cmdline", procroot) || gs_read_whole(p, &data, &len, 1 << 16))
		return 0;
	for (tok = strtok_r(data, " \t\n", &save); tok && !found; tok = strtok_r(NULL, " \t\n", &save)) {
		char *opt, *save2 = NULL;

		if (strncmp(tok, "pci=", 4) != 0)
			continue;
		for (opt = strtok_r(tok + 4, ",", &save2); opt; opt = strtok_r(NULL, ",", &save2))
			if (strcmp(opt, "noari") == 0)
				found = 1;
	}
	free(data);
	return found;
}

/* The x.y.z version in procroot/driver/nvidia/version; "" when NVIDIA's driver is not loaded. */
void gs_nvidia_version(const char *procroot, char *out, size_t len)
{
	char p[GS_PATH_MAX], *data, *tok, *save = NULL;
	size_t dlen;

	out[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/version", procroot) || gs_read_whole(p, &data, &dlen, 1 << 16))
		return;
	for (tok = strtok_r(data, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
		unsigned int a, b, c;
		char extra;

		if (sscanf(tok, "%u.%u.%u%c", &a, &b, &c, &extra) == 3) {
			snprintf(out, len, "%s", tok);
			break;
		}
	}
	free(data);
}

/* The GNU build-id among ELF notes of the host's byte order in buf, as lowercase hex: 0, or -1. */
int gs_note_build_id(const unsigned char *buf, size_t size, char *out, size_t len)
{
	size_t off = 0;

	out[0] = '\0';
	while (off + 12 <= size) {
		Elf64_Word nh[3];
		size_t name, desc;

		memcpy(nh, buf + off, sizeof(nh));
		name = off + 12;
		desc = name + ((nh[0] + 3u) & ~3u);
		if (desc > size || nh[1] > size - desc)
			break;
		if (nh[2] == NT_GNU_BUILD_ID && nh[0] == 4 && memcmp(buf + name, "GNU", 4) == 0 && nh[1] &&
		    nh[1] * 2u < len) {
			for (Elf64_Word k = 0; k < nh[1]; k++)
				snprintf(out + 2 * k, 3, "%02x", buf[desc + k]);
			return 0;
		}
		off = desc + ((nh[1] + 3u) & ~3u);
	}
	return -1;
}

/*
 * The sentence doctor, status and check add when the installed nvidia.ko in b is the driver step's
 * build and the loaded nvidia module (its build-id note under sysroot/module) is not: 1 with line set;
 * 0 when the installed module is not the driver step's build, the module is not loaded, its build-id
 * is unknown, or they match.
 */
int gs_loaded_line(const char *sysroot, const struct gs_build *b, char *line, size_t len)
{
	char p[GS_PATH_MAX], id[129], *data;
	size_t dlen;
	int r;

	if (b->state != GS_BUILD_MATCHES || gs_pathf(p, sizeof(p), "%s/module/nvidia/notes/.note.gnu.build-id", sysroot) ||
	    gs_read_whole(p, &data, &dlen, 4096))
		return 0;
	r = gs_note_build_id((const unsigned char *)data, dlen, id, sizeof(id));
	free(data);
	if (r || strcmp(id, b->found_id) == 0)
		return 0;
	snprintf(line, len, "the running nvidia module (build-id %.12s) is not the installed one (%.12s); power the "
		 "machine off and on to load it", id, b->found_id);
	return 1;
}

/*
 * The driver step's record against the nvidia.ko the kernel would load, found as the driver step finds it;
 * the one reading doctor, status, activate and check share. src NULL: the live host's when live, else
 * nothing is judged (a fixture without sources). 1 with b set, 0 when not judged.
 */
int gs_build_identity(const struct gs_build_src *src, int live, struct gs_build *b)
{
	char release[128] = "", module[GS_PATH_MAX] = "";
	struct utsname u;

	if (!src && !live)
		return 0;
	if (src && src->release)
		gs_copy(release, sizeof(release), src->release);
	else if (uname(&u) == 0)
		gs_copy(release, sizeof(release), u.release);
	if (src && src->module)
		gs_copy(module, sizeof(module), src->module);
	else if (release[0])
		gs_nvidia_module_path(release, module, sizeof(module));
	gs_driver_build_read(src && src->record ? src->record : GS_DRIVER_BUILD, release, module, b);
	return 1;
}

void gs_read_module_state(const char *sysroot, struct gs_module_state *m)
{
	char p[GS_PATH_MAX];

	memset(m, 0, sizeof(*m));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s", sysroot, GS_MODULE_NAME))
		m->loaded = gs_exists(p);
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/ari", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->ari, sizeof(m->ari));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/msix", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->msix, sizeof(m->msix));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/vfs", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->vfs, sizeof(m->vfs));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/bdfs", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->bdfs, sizeof(m->bdfs));
	if (!gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s", sysroot, GS_MODULE_NAME))
		m->lp_present = gs_exists(p);
	if (m->lp_present) {
		int a = gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/enabled", sysroot, GS_MODULE_NAME) ||
			gs_read_uint(p, &m->lp_enabled);
		int b = gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/transition", sysroot, GS_MODULE_NAME) ||
			gs_read_uint(p, &m->lp_transition);

		if (a || b)
			m->lp_enabled = m->lp_transition = 0;
		m->lp_ok = !a && !b && m->lp_enabled == 1 && m->lp_transition == 0;
	}
}

/* The VF count the loaded module uses; a module without the vfs parameter uses GS_DEFAULT_VFS. */
unsigned int gs_module_vfs(const struct gs_module_state *m)
{
	unsigned int v;

	return m->vfs[0] && !gs_parse_vfs(m->vfs, &v) ? v : GS_DEFAULT_VFS;
}

int gs_read_dev_attr(const char *sys, const char *bdf, const char *name, char *buf, size_t len)
{
	char p[GS_PATH_MAX];

	buf[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/%s", sys, bdf, name))
		return -ENAMETOOLONG;
	return gs_read_attr(p, buf, len);
}

/* The name of the driver bdf is bound to; "" when unbound. */
void gs_dev_driver(const char *sys, const char *bdf, char *out, size_t len)
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
int gs_vf_list(const char *sys, const struct gs_addr *pf, unsigned int numvfs, struct gs_addr *out, char *why,
		   size_t len)
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
			return snprintf(why, len, "%s: the physfn link of its virtfn%u %s does not lead back to it", pf->str,
					i, out[i].str), -1;
	}
	return 0;
}

int gs_select_cards(const char *sys, const struct gs_config *cfg, const struct gs_activate_opts *o,
			struct gs_vgpu_card *out, unsigned int *n)
{
	const struct gs_addr *ada = NULL, *ampere = NULL;

	*n = 0;
	for (unsigned int i = 0; i < o->ndevices; i++) {
		if (!gs_addr_listed(cfg->bdfs, cfg->nbdfs, o->devices[i].str)) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: --device %s is not selected (bdfs= in %s)\n",
				o->devices[i].str, gs_config_path());
			return GS_EXIT_REFUSED;
		}
	}
	for (unsigned int i = 0; i < cfg->nbdfs; i++) {
		const struct gs_addr *pf = &cfg->bdfs[i];
		const struct gs_device *e;
		struct gs_card c;

		if (o->ndevices && !gs_addr_listed(o->devices, o->ndevices, pf->str))
			continue;
		gs_card_inspect(sys, pf, &c);
		if (!c.present) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is missing\n", pf->str);
			return GS_EXIT_REFUSED;
		}
		e = c.entry;
		if (!e) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s (%04x:%04x) is not in the device table\n",
				pf->str, c.vendor, c.device);
			return GS_EXIT_REFUSED;
		}
		if (e->generation == GS_ADA && !ada)
			ada = pf;
		else if (e->generation == GS_AMPERE && !ampere)
			ampere = pf;
		/* Every field starts zeroed: the caller must never inherit a stale VF count or address list. */
		memset(&out[*n], 0, sizeof out[*n]);
		out[*n].pf = *pf;
		out[*n].generation = e->generation;
		out[(*n)++].type = o->type ? o->type : gs_config_vgpu_type(cfg, e->generation);
	}
	if (*n == 0) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: no GPU is selected (bdfs= in %s)\n",
			gs_config_path());
		return GS_EXIT_REFUSED;
	}
	if (o->type && ada && ampere) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: --type %u would set one vGPU type on Ada (%s) and Ampere (%s)\n"
			"cards, and no type fits both (%u is not creatable on the professional counterpart of the Ampere cards, %u not on that of the Ada cards). Omit --type\n"
			"for the per-generation defaults (%u Ada, %u Ampere), or, for check, name one generation's GPUs with --device.\n"
			"Set type_ada= and type_ampere= in %s to choose one per generation\n", o->type, ada->str, ampere->str,
			GS_VGPU_TYPE_ADA, GS_VGPU_TYPE_AMPERE, gs_config_vgpu_type(cfg, GS_ADA),
			gs_config_vgpu_type(cfg, GS_AMPERE), gs_config_path());
		return GS_EXIT_REFUSED;
	}
	return 0;
}

/* Where NVIDIA's host package puts the vGPU plugin link: the first that exists, else the first. */
const char *gs_plugin_link(void)
{
	static const char *const plugins[] = { "/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so", "/usr/lib64/libnvidia-vgpu.so",
					       "/usr/lib/libnvidia-vgpu.so" };

	for (size_t k = 0; k < sizeof(plugins) / sizeof(plugins[0]); k++)
		if (gs_exists(plugins[k]))
			return plugins[k];
	return plugins[0];
}
