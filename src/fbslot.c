// SPDX-License-Identifier: MIT
/*
 * The framebuffer slots of Ada VFs, from the CLI's side.
 *
 * An Ada VF's guest framebuffer is a fixed range ("slot") of the PF's memory, which nothing
 * reserves while the VM is stopped. The owner inside nvidia.ko (driver-patches/fb-owner/) holds
 * the slots it is told to hold, and hands each to NVIDIA's vGPU manager when the VM starts. It
 * decides nothing by itself. What is decided is here:
 *   - where the slots are: NVIDIA's driver is asked, per VF, with the control its own vGPU plugin
 *     uses at a VM start, and the answers are checked before any is used;
 *   - when they are held and let go of: `activate` releases everything before it unloads or
 *     re-probes anything, and holds once the VFs have their vGPU types;
 *   - what status, doctor and check say about them.
 * Nothing here fails an activation or a check: without a held slot a VM starts as it did before
 * there was an owner.
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gs_hook.h"
#include "gs_lifecycle.h"
#include "gs_internal.h"

/* ---- NVIDIA's driver, asked where each VF's framebuffer is ------------------------------------ */

/*
 * The structures and numbers of the resource manager's ioctl interface that the question needs,
 * written out from NVIDIA's MIT-licensed open-gpu-kernel-modules headers at tag 580.159.03 (nvos.h,
 * nv-ioctl.h, ctrl0000gpu.h, cl0080.h, ctrl2080fb.h, ctrla081.h). gs_hook.h has the control escape.
 */
struct fb_nvos64 {
	uint32_t h_root, h_object_parent, h_object_new, h_class;
	uint64_t alloc_parms, rights_requested;
	uint32_t params_size, flags, status, pad;
};
struct fb_nvos00 {
	uint32_t h_root, h_object_parent, h_object_old, status;
};
struct fb_card_info {
	uint8_t valid, pad0[3];
	uint32_t domain;
	uint8_t bus, slot, function, pad1;
	uint16_t vendor_id, device_id;
	uint32_t gpu_id;
	uint8_t other0[28];
	uint64_t fb_size;
	uint32_t minor_number;
	uint8_t other1[12];
};
#define FB_MAX_DEVICES 32
struct fb_api_version {
	uint32_t cmd, reply;
	char version[64];
};
struct fb_attach_ids {
	uint32_t gpu_ids[32];
	uint32_t failed_id;
};
struct fb_id_info_v2 {
	uint32_t gpu_id, gpu_flags, device_instance, sub_device_instance;
	uint8_t other[16];
};
struct fb_device_alloc {
	uint32_t device_id;
	uint8_t other[52];
};
struct fb_info_v2 {
	uint32_t list_size;
	struct { uint32_t index, data; } list[128];
};
/* NVA081_CTRL_VGPU_CONFIG_UPDATE_PLACEMENT_INFO_PARAMS. */
struct fb_placement {
	uint8_t is_heterogeneous, pad0;
	uint16_t placement_id;
	uint32_t vgpu_type_id;
	uint8_t vgpu_dev_name[16];
	uint64_t guest_fb_length, guest_fb_offset, gsp_heap_offset, guest_bar1_pf_offset;
};
_Static_assert(sizeof(struct fb_nvos64) == 0x30 && offsetof(struct fb_nvos64, status) == 40, "NVOS64_PARAMETERS");
_Static_assert(sizeof(struct fb_nvos00) == 0x10, "NVOS00_PARAMETERS");
_Static_assert(sizeof(struct fb_card_info) == 72 && offsetof(struct fb_card_info, gpu_id) == 16 &&
	       offsetof(struct fb_card_info, minor_number) == 56, "nv_ioctl_card_info_t");
_Static_assert(sizeof(struct fb_api_version) == 72, "nv_ioctl_rm_api_version_t");
_Static_assert(sizeof(struct fb_attach_ids) == 132, "NV0000_CTRL_GPU_ATTACH_IDS_PARAMS");
_Static_assert(sizeof(struct fb_id_info_v2) == 32 && offsetof(struct fb_id_info_v2, device_instance) == 8,
	       "NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS");
_Static_assert(sizeof(struct fb_device_alloc) == 56, "NV0080_ALLOC_PARAMETERS");
_Static_assert(sizeof(struct fb_info_v2) == 1028, "NV2080_CTRL_FB_GET_INFO_V2_PARAMS");
_Static_assert(sizeof(struct fb_placement) == 0x38 && offsetof(struct fb_placement, placement_id) == 2 &&
	       offsetof(struct fb_placement, vgpu_dev_name) == 8 && offsetof(struct fb_placement, guest_fb_length) == 0x18 &&
	       offsetof(struct fb_placement, guest_fb_offset) == 0x20, "NVA081_CTRL_VGPU_CONFIG_UPDATE_PLACEMENT_INFO_PARAMS");

#define FB_IOWR(nr, type) ((3UL << 30) | ((unsigned long)sizeof(type) << 16) | ((unsigned long)'F' << 8) | (unsigned long)(nr))
#define FB_ESC_RM_ALLOC FB_IOWR(0x2b, struct fb_nvos64)
#define FB_ESC_RM_FREE FB_IOWR(0x29, struct fb_nvos00)
#define FB_ESC_CARD_INFO FB_IOWR(0xc8, struct fb_card_info[FB_MAX_DEVICES])
#define FB_ESC_REGISTER_FD FB_IOWR(0xc9, int32_t)
#define FB_ESC_CHECK_VERSION_STR FB_IOWR(0xd2, struct fb_api_version)
#define FB_VERSION_CMD_RELAXED 0x31u
#define FB_CLASS_ROOT 0x0u
#define FB_CLASS_DEVICE 0x80u
#define FB_CLASS_SUBDEVICE 0x2080u
#define FB_CLASS_VGPU_CONFIG 0xa081u
#define FB_CTRL_GPU_ATTACH_IDS 0x00000215u
#define FB_CTRL_GPU_GET_ID_INFO_V2 0x00000205u
#define FB_CTRL_FB_GET_INFO_V2 0x20801303u
#define FB_INFO_TOTAL_RAM_SIZE 0x08u /* in KiB */
#define FB_CTRL_GPU_GET_VMMU_SEGMENT_SIZE 0x2080017eu
#define FB_CTRL_UPDATE_PLACEMENT_INFO 0xa081011bu
_Static_assert(FB_ESC_RM_ALLOC == 0xc030462bUL && FB_ESC_RM_FREE == 0xc0104629UL, "the RM's escapes");

static uint32_t rm_alloc(int ctl, uint32_t client, uint32_t parent, uint32_t handle, uint32_t cls, void *params, uint32_t *got)
{
	struct fb_nvos64 p = { .h_root = client, .h_object_parent = parent, .h_object_new = handle, .h_class = cls,
			       .alloc_parms = (uintptr_t)params, .status = 0xffffffffu };

	if (ioctl(ctl, FB_ESC_RM_ALLOC, &p) != 0)
		return 0xffffffffu;
	if (got)
		*got = p.h_object_new;
	return p.status;
}

static uint32_t rm_control(int ctl, uint32_t client, uint32_t object, uint32_t cmd, void *params, uint32_t size)
{
	struct gs_nvos54 p = { .h_client = client, .h_object = object, .cmd = cmd, .params = (uintptr_t)params,
			       .params_size = size, .status = 0xffffffffu };

	if (ioctl(ctl, GS_NV_ESC_RM_CONTROL, &p) != 0)
		return 0xffffffffu;
	return p.status;
}

/*
 * The live host's answer to "where is the framebuffer of each VF of this card": a root client of
 * its own, the control NVIDIA's vGPU plugin sends at a VM start (the name is the VF's PCI address,
 * the type is the vGPU type set on it), and the client freed again. The caller has made sure that
 * nvidia_vgpu_vfio is loaded: only then is the control a lookup that changes nothing
 * (vgpuconfigapi.c:1469-1500 of the public source). The same question, from a separate root
 * client, got the plugin's own answers on the test host.
 */
static int query_rm(const struct gs_host *h, const struct gs_vgpu_card *c, struct gs_fb_query *q, char *why, size_t len)
{
	static struct fb_card_info cards[FB_MAX_DEVICES];
	static struct fb_info_v2 info;
	struct fb_api_version v = { .cmd = FB_VERSION_CMD_RELAXED };
	struct gs_gpu_pci_loc loc = { 0 };
	struct fb_attach_ids attach;
	struct fb_id_info_v2 id = { 0 };
	struct fb_device_alloc dev = { 0 };
	struct fb_nvos00 gone = { 0 };
	unsigned int domain, bus, slot, fn;
	uint32_t client = 0, sub = 0, gpu_id, s;
	uint64_t segment = 0;
	char path[GS_PATH_MAX];
	int ctl = -1, node = -1, found = -1, rc = -1;
	int32_t reg;

	memset(q, 0, sizeof(*q));
	if (sscanf(c->pf.str, "%x:%x:%x.%x", &domain, &bus, &slot, &fn) != 4)
		return snprintf(why, len, "%s is not a PCI address", c->pf.str), -1;
	if (gs_pathf(path, sizeof(path), "%s/dev/nvidiactl", h->root ? h->root : "") || (ctl = open(path, O_RDWR | O_CLOEXEC)) < 0)
		return snprintf(why, len, "cannot open /dev/nvidiactl: %s", strerror(errno)), -1;
	ioctl(ctl, FB_ESC_CHECK_VERSION_STR, &v);
	if (ioctl(ctl, FB_ESC_CARD_INFO, cards) != 0) {
		snprintf(why, len, "NVIDIA's driver does not list its GPUs: %s", strerror(errno));
		goto out;
	}
	for (int i = 0; i < FB_MAX_DEVICES; i++)
		if (cards[i].valid && cards[i].domain == domain && cards[i].bus == bus && cards[i].slot == slot && cards[i].function == fn)
			found = i;
	if (found < 0) {
		snprintf(why, len, "NVIDIA's driver does not have the GPU");
		goto out;
	}
	gpu_id = cards[found].gpu_id;
	/* The RM gives a device only to a process that has the GPU's own file open. */
	if (gs_pathf(path, sizeof(path), "%s/dev/nvidia%u", h->root ? h->root : "", cards[found].minor_number) ||
	    (node = open(path, O_RDWR | O_CLOEXEC)) < 0) {
		snprintf(why, len, "cannot open /dev/nvidia%u: %s", cards[found].minor_number, strerror(errno));
		goto out;
	}
	reg = ctl;
	if (ioctl(node, FB_ESC_REGISTER_FD, &reg) != 0) {
		snprintf(why, len, "cannot register /dev/nvidia%u with the control file: %s", cards[found].minor_number, strerror(errno));
		goto out;
	}
	if ((s = rm_alloc(ctl, 0, 0, 0, FB_CLASS_ROOT, NULL, &client)) != GS_NV_OK || !client) {
		snprintf(why, len, "NVIDIA's driver gave no client (status 0x%x)", s);
		client = 0;
		goto out;
	}
	loc.gpu_id = gpu_id;
	s = rm_control(ctl, client, client, GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO, &loc, sizeof(loc));
	if (s != GS_NV_OK || loc.domain != domain || loc.bus != bus || loc.slot != slot) {
		snprintf(why, len, "GPU ID 0x%x is another device to NVIDIA's driver (status 0x%x)", gpu_id, s);
		goto out;
	}
	memset(&attach, 0xff, sizeof(attach));
	attach.gpu_ids[0] = gpu_id;
	attach.failed_id = 0;
	id.gpu_id = gpu_id;
	if ((s = rm_control(ctl, client, client, FB_CTRL_GPU_ATTACH_IDS, &attach, sizeof(attach))) != GS_NV_OK ||
	    (s = rm_control(ctl, client, client, FB_CTRL_GPU_GET_ID_INFO_V2, &id, sizeof(id))) != GS_NV_OK) {
		snprintf(why, len, "NVIDIA's driver does not attach the GPU (status 0x%x)", s);
		goto out;
	}
	dev.device_id = id.device_instance;
	sub = id.sub_device_instance;
	if ((s = rm_alloc(ctl, client, client, 1, FB_CLASS_DEVICE, &dev, NULL)) != GS_NV_OK ||
	    (s = rm_alloc(ctl, client, 1, 2, FB_CLASS_SUBDEVICE, &sub, NULL)) != GS_NV_OK) {
		snprintf(why, len, "NVIDIA's driver gave no device (status 0x%x)", s);
		goto out;
	}
	memset(&info, 0, sizeof(info));
	info.list_size = 1;
	info.list[0].index = FB_INFO_TOTAL_RAM_SIZE;
	if ((s = rm_control(ctl, client, 2, FB_CTRL_FB_GET_INFO_V2, &info, sizeof(info))) != GS_NV_OK || !info.list[0].data) {
		snprintf(why, len, "NVIDIA's driver does not tell the size of the GPU's memory (status 0x%x)", s);
		goto out;
	}
	q->ram_size = (unsigned long long)info.list[0].data << 10;
	if ((s = rm_alloc(ctl, client, 2, 3, FB_CLASS_VGPU_CONFIG, NULL, NULL)) != GS_NV_OK ||
	    (s = rm_control(ctl, client, 2, FB_CTRL_GPU_GET_VMMU_SEGMENT_SIZE, &segment, sizeof(segment))) != GS_NV_OK || !segment) {
		snprintf(why, len, "NVIDIA's driver does not answer the placement questions (status 0x%x)", s);
		goto out;
	}
	q->segment = segment;
	for (unsigned int i = 0; i < c->nvf && i < GS_VFS_MAX_QUERY; i++) {
		struct fb_placement p = { .placement_id = GS_FB_NO_PLACEMENT, .vgpu_type_id = c->type };
		size_t n = strlen(c->vf[i].str);

		memcpy(p.vgpu_dev_name, c->vf[i].str, n < sizeof(p.vgpu_dev_name) ? n : sizeof(p.vgpu_dev_name));
		q->vf[i].status = rm_control(ctl, client, 3, FB_CTRL_UPDATE_PLACEMENT_INFO, &p, sizeof(p));
		q->vf[i].placement = p.placement_id;
		q->vf[i].offset = p.guest_fb_offset;
		q->vf[i].length = p.guest_fb_length;
	}
	rc = 0;
out:
	if (client) {
		gone.h_root = gone.h_object_old = client;
		ioctl(ctl, FB_ESC_RM_FREE, &gone);
	}
	if (node >= 0)
		close(node);
	close(ctl);
	return rc;
}

/* ---- the answers, checked ---------------------------------------------------------------------- */

int gs_fb_validate(const struct gs_fb_query *q, unsigned int nvf, struct gs_fb_range *out, unsigned int *n, char *why,
		   size_t len)
{
	*n = 0;
	if (!q->ram_size || !q->segment)
		return snprintf(why, len, "the size of the GPU's memory or of its memory segments is not known"), -1;
	for (unsigned int i = 0; i < nvf && i < GS_VFS_MAX; i++) {
		const struct gs_fb_answer *a = &q->vf[i];
		const char *bad = NULL;

		/* A VF the driver knows no placement for (no vGPU type on it) has no slot; the others keep theirs. */
		if (a->status != 0)
			continue;
		/* A VF without placement (Ampere) has no fixed range and needs nothing. */
		if (a->placement == GS_FB_NO_PLACEMENT && !a->length)
			continue;
		if (a->placement == GS_FB_NO_PLACEMENT || !a->length)
			bad = "a placement without a length, or a length without a placement";
		else if (a->offset % q->segment || a->length % q->segment)
			bad = "not a whole number of the driver's memory segments";
		else if (a->offset + a->length < a->offset || a->offset + a->length > q->ram_size)
			bad = "beyond the end of the GPU's memory";
		for (unsigned int k = 0; !bad && k < *n; k++) {
			if (a->offset < out[k].offset + out[k].size && out[k].offset < a->offset + a->length)
				bad = "it overlaps the range of another VF";
			else if (out[k].size != a->length)
				bad = "another length than another VF of the same vGPU type";
		}
		if (bad) {
			*n = 0;
			return snprintf(why, len, "VF %u: placement %u, offset 0x%llx, length 0x%llx: %s", i, a->placement, a->offset,
					a->length, bad), -1;
		}
		out[*n].offset = a->offset;
		out[*n].size = a->length;
		(*n)++;
	}
	return 0;
}

/* ---- the owner in the kernel ------------------------------------------------------------------- */

static int proc_path(const char *procroot, char *p, size_t len)
{
	return gs_pathf(p, len, "%s/" GS_FB_PROC, procroot);
}

int gs_fb_state_read(const char *procroot, struct gs_fb_state *st)
{
	char p[GS_PATH_MAX], *data = NULL, *line, *save = NULL;
	size_t len = 0;
	int r;

	memset(st, 0, sizeof(*st));
	if (proc_path(procroot, p, sizeof(p)))
		return -ENAMETOOLONG;
	if (!gs_exists(p))
		return -ENOENT;
	if ((r = gs_read_whole(p, &data, &len, 1 << 16)))
		return r == -ENOENT ? -EACCES : r;
	st->present = 1;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		struct gs_fb_slot_state *s = &st->slot[st->n];
		char word[16] = "", rest[sizeof(s->why)] = "";
		int at = 0;

		if (sscanf(line, "abi %d", &st->abi) == 1 || st->n >= GS_FB_STATE_MAX)
			continue;
		memset(s, 0, sizeof(*s));
		if (sscanf(line, "slot %15s %llx %llx %15s %n", s->bdf, &s->offset, &s->size, word, &at) < 4 || !at)
			continue;
		gs_copy(rest, sizeof(rest), line + at);
		if (strcmp(word, "held") == 0) {
			s->state = GS_FB_HELD;
			sscanf(rest, "dups %u answered %u", &s->dups, &s->answered);
		} else if (strcmp(word, "given-up") == 0) {
			s->state = GS_FB_GIVEN_UP;
			gs_copy(s->why, sizeof(s->why), rest);
		} else if (strcmp(word, "failed") == 0) {
			s->state = GS_FB_FAILED;
			sscanf(rest, "%x", &s->status);
		} else {
			continue;
		}
		st->n++;
	}
	free(data);
	return 0;
}

/* One command to the owner: 0, or a negative errno. */
static int command(const struct gs_host *h, const char *text)
{
	char p[GS_PATH_MAX];

	if (proc_path(h->proc, p, sizeof(p)))
		return -ENAMETOOLONG;
	if (h->write_attr_deadline)
		return h->write_attr_deadline(h, p, text, GS_MODULE_DEADLINE_MS);
	return h->write_attr ? h->write_attr(p, text) : -ENOSYS;
}

void gs_fb_release(const struct gs_host *h)
{
	struct gs_fb_state st;
	int r;

	/* A module without the owner, or an owner that holds nothing, has nothing to let go of. */
	if (gs_fb_state_read(h->proc, &st) || !st.n)
		return;
	printf("framebuffer slots: released (the owner in nvidia.ko has the GPU open while it holds one)\n");
	if ((r = command(h, "release all")))
		fprintf(stderr, "geforce-sriov: warning: the framebuffer slots could not be released (%s). A GPU that the owner has open\n"
			"cannot be re-probed; if a step below fails for that reason, reboot.\n", strerror(-r));
}

void gs_fb_hold(const struct gs_host *h, const struct gs_config *cfg, const struct gs_vgpu_card *vc, unsigned int n)
{
	struct gs_fb_state st;
	char why[512], ver[32] = "", note[512];
	unsigned int ada = 0;
	int said = 0, r;

	for (unsigned int i = 0; i < n; i++)
		ada += vc[i].generation == GS_ADA && vc[i].nvf;
	if (!ada)
		return;
	if (cfg->fb_slots == GS_FB_SLOTS_OFF) {
		/* Slots that an earlier run held are let go of; nothing is said when there are none. */
		gs_fb_release(h);
		return;
	}
	r = gs_fb_state_read(h->proc, &st);
	if (r) {
		fprintf(stderr, "geforce-sriov: warning: the framebuffer slots of the Ada VFs are not held: %s.\n"
			"An Ada VM may fail to start while a host program uses its GPU; `geforce-sriov status` says more.\n",
			r == -ENOENT ? "the nvidia module that is loaded does not carry their owner" : "the owner in nvidia.ko cannot be read");
		return;
	}
	if (!gs_module_loaded(h, "nvidia_vgpu_vfio")) {
		fprintf(stderr, "geforce-sriov: warning: the framebuffer slots of the Ada VFs are not held: nvidia_vgpu_vfio is not loaded,\n"
			"and without it NVIDIA's driver cannot be asked where they are.\n");
		return;
	}
	gs_nvidia_version(h->proc, ver, sizeof(ver));
	for (unsigned int i = 0; i < n; i++) {
		struct gs_fb_range range[GS_VFS_MAX];
		struct gs_fb_query q;
		unsigned int nr = 0, held = 0;
		char cmd[128];

		if (vc[i].generation != GS_ADA || !vc[i].nvf)
			continue;
		if ((h->fb_query ? h->fb_query(h, &vc[i], &q, why, sizeof(why)) : query_rm(h, &vc[i], &q, why, sizeof(why))) != 0) {
			fprintf(stderr, "geforce-sriov: warning: %s: the framebuffer slots are not held: NVIDIA's driver could not be asked\n"
				"where they are (%s).\n", vc[i].pf.str, why);
			continue;
		}
		if (gs_fb_validate(&q, vc[i].nvf, range, &nr, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: warning: %s: the framebuffer slots are not held: the answer of NVIDIA's driver is\n"
				"not usable (%s).\n", vc[i].pf.str, why);
			continue;
		}
		if (!nr) {
			printf("%s: NVIDIA's driver names no framebuffer slot for its VFs; nothing is held\n", vc[i].pf.str);
			continue;
		}
		/*
		 * EEXIST: a range that is held overlaps the one asked for without being it. What is held
		 * is then from another layout (the vGPU types were changed by hand since). The GPU's slots
		 * are released and held again, once. A VM that runs keeps its memory through its duplicate.
		 */
		for (int again = 0; again < 2; again++) {
			int stale = 0;

			for (unsigned int k = 0; k < nr && !stale; k++) {
				snprintf(cmd, sizeof(cmd), "hold %s %llx %llx %llx", vc[i].pf.str, range[k].offset, range[k].size, GS_FB_ALIGN);
				r = command(h, cmd);
				if (r == -EEXIST && !again)
					stale = 1;
				else if (r)
					fprintf(stderr, "geforce-sriov: warning: %s: the framebuffer slot at 0x%llx is not held: the owner in nvidia.ko\n"
						"refused the command (%s).\n", vc[i].pf.str, range[k].offset, strerror(-r));
			}
			if (!stale)
				break;
			printf("%s: the framebuffer slots that are held are not where NVIDIA's driver places the VFs now; they are released and held again\n",
			       vc[i].pf.str);
			snprintf(cmd, sizeof(cmd), "release %s", vc[i].pf.str);
			if ((r = command(h, cmd)))
				fprintf(stderr, "geforce-sriov: warning: %s: the framebuffer slots could not be released (%s).\n", vc[i].pf.str, strerror(-r));
		}
		/* What the owner says it holds now, not what was asked of it. */
		if (gs_fb_state_read(h->proc, &st) == 0)
			for (unsigned int k = 0; k < st.n; k++)
				held += strcmp(st.slot[k].bdf, vc[i].pf.str) == 0 && st.slot[k].state == GS_FB_HELD;
		if (held >= nr)
			printf("%s: %u framebuffer slot%s held, so that a host program cannot take the memory of a stopped VM\n",
			       vc[i].pf.str, held, held == 1 ? " is" : "s are");
		else
			fprintf(stderr, "geforce-sriov: warning: %s: %u of %u framebuffer slots are held; a VM on another one may fail to\n"
				"start while a host program uses this GPU. `geforce-sriov status` names the reason for each.\n",
				vc[i].pf.str, held, nr);
		if (!said && ver[0] && gs_driver_version_line(ver, note, sizeof(note))) {
			printf("note: holding framebuffer slots is confirmed for the supported driver only; with NVIDIA's driver %s it is\n"
			       "not guaranteed to work. fb_slots=off in %s turns it off.\n", ver, GS_CONFIG_PATH);
			said = 1;
		}
	}
}

/* ---- what status, doctor and check print ------------------------------------------------------- */

int gs_fb_line(const char *procroot, const struct gs_config *cfg, const char *record, const char *pf, const char *ver,
	       char *line, size_t len)
{
	struct gs_fb_state st;
	char owner[8] = "", reason[256] = "", note[64] = "", vline[512];
	const char *first = NULL;
	unsigned int held = 0, in_use = 0, lost = 0;
	static const char risk[] = "a VM may fail to start while host programs use this GPU";
	int r;

	if (cfg->fb_slots == GS_FB_SLOTS_OFF)
		return snprintf(line, len, "not held (fb_slots=off in the config); %s", risk), 0;
	if (ver[0] && gs_driver_version_line(ver, vline, sizeof(vline)))
		snprintf(note, sizeof(note), "; not guaranteed to work with this driver build");
	r = gs_fb_state_read(procroot, &st);
	if (r == -ENOENT) {
		/* No owner in the module that is loaded: the build record of the installed one says why, when it knows. */
		if (record && gs_record_value(record, "fb_owner", owner, sizeof(owner)) && strcmp(owner, "0") == 0) {
			gs_record_value(record, "fb_owner_reason", reason, sizeof(reason));
			return snprintf(line, len, "not held (the installed driver was built without their owner: %s); %s",
					reason[0] ? reason : "the build record gives no reason", risk), -1;
		}
		if (owner[0])
			return snprintf(line, len, "not held (the nvidia module that is loaded does not carry their owner; the installed "
					"one does, and is loaded at the next boot); %s", risk), -1;
		return snprintf(line, len, "not held (the nvidia module that is loaded does not carry their owner; `sudo geforce-sriov "
				"install` builds it in); %s", risk), -1;
	}
	if (r)
		return snprintf(line, len, "not known: only root can read /proc/" GS_FB_PROC "; run this command with sudo"), -2;
	for (unsigned int i = 0; i < st.n; i++) {
		const struct gs_fb_slot_state *s = &st.slot[i];

		if (strcmp(s->bdf, pf) != 0)
			continue;
		if (s->state == GS_FB_HELD) {
			held++;
			in_use += s->dups > 0;
			continue;
		}
		lost++;
		if (first)
			continue;
		if (s->state == GS_FB_GIVEN_UP)
			snprintf(reason, sizeof(reason), "given up: %s", s->why);
		else if (s->status == 0x51)
			snprintf(reason, sizeof(reason), "it was in use by a VM or a host program when activation asked for it");
		else
			snprintf(reason, sizeof(reason), "NVIDIA's driver refused it (status 0x%x)", s->status);
		first = reason;
	}
	if (!held && !lost)
		return snprintf(line, len, "not held (activation has not held them; `sudo geforce-sriov activate` does); %s", risk), -1;
	if (!held)
		return snprintf(line, len, "not held (%s); `sudo geforce-sriov activate` holds them again; %s", first, risk), -1;
	if (lost)
		return snprintf(line, len, "%u held (%u in use, %u free), %u not held: %s; `sudo geforce-sriov activate` holds %s again%s",
				held, in_use, held - in_use, lost, first, lost == 1 ? "it" : "them", note), -1;
	return snprintf(line, len, "%u held (%u in use, %u free)%s", held, in_use, held - in_use, note), 1;
}
