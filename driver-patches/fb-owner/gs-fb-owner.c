// SPDX-License-Identifier: MIT
/*
 * geforce-sriov: the owner of the framebuffer slots of Ada VFs, inside nvidia.ko.
 *
 * The problem. An Ada VF's guest framebuffer is a fixed range ("slot") of the PF's memory. The vGPU
 * plugin in nvidia-vgpu-mgr allocates it when the VM starts and frees it when the VM stops, and
 * nothing reserves it in between: a host program that allocates while the VM is down is given the
 * range, and the VM then fails to start with NV_ERR_NO_MEMORY (0x51).
 *
 * What this does. A kernel RM client, made the way NVIDIA's own modeset module makes one
 * (rm_kernel_rmapi_op), allocates each slot it is told to hold and keeps the object. When the
 * manager asks for exactly that range, the request is answered with a duplicate of the held
 * object, and the RM never sees the request. Anyone else who asks for the range is refused by the
 * RM, because the range is taken. Ran on hardware as a proof before it was written like this
 * (driver 580.159.01).
 *
 * The rule every decision follows: a hold may only make a VM start more likely, never less. When
 * this code is not sure about a request of the manager's, it frees its own object ("gives the slot
 * up") and lets the call go on to the RM unchanged, where it behaves as it does without this code.
 * No path returns an error of its own to the caller.
 *
 * What it does not do. It never decides to hold anything: only a root write to
 * /proc/driver/nvidia/geforce-sriov-fb does, and `geforce-sriov activate` is what writes there,
 * after it has asked the RM where each VF's framebuffer is. A slot that was given up is not taken
 * again by itself. Nothing is written into a slot: NVIDIA's host side clears a guest's memory when
 * the guest starts.
 *
 * Who gets a duplicate. A process of the host itself (the initial PID namespace, so not one in a
 * container) that is named nvidia-vgpu-mgr and has CAP_SYS_ADMIN, for a client and a device that
 * this code saw that process's file create (the open layer's own record, kept by gs_fb_ioctl_post;
 * no question is put to the RM about it), and only for a slot that no duplicate is recorded for: a
 * slot that a guest runs on is never handed out a second time. The executable is not checked: a
 * task's executable can be read safely only under rcu_read_lock() or with a reference, and with a
 * preemptible RCU (the test host's kernel) both need symbols that are exported for GPL modules
 * only, which a module with NVIDIA's licence cannot use. So a root process of the host that takes
 * the manager's name can obtain a duplicate of a slot whose VM is stopped.
 *
 * The structures are written out here because this package's open layer does not ship nvos.h. They
 * are NVIDIA's (open-gpu-kernel-modules 580.159.03, MIT): nvos.h, cl0080.h, ctrl0000gpu.h,
 * nv-kernel-rmapi-ops.h.
 *
 * Only plain exports of the kernel are used; tests/fixtures-driver/test-fb-owner.sh keeps the list.
 */
#ifdef GS_FB_TEST
#include "stub.h"
#else
#include "os-interface.h"
#include "nv-linux.h"
#include <linux/capability.h>
#include <linux/compiler.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#endif
#include "gs-fb-owner.h"

/* What status and the driver step look for in the module file. */
#define GS_FB_ABI 1
static const char gs_fb_mark[] __attribute__((used)) = "geforce-sriov-fb-abi=1;";

#define GS_FB_PROC_NAME "driver/nvidia/geforce-sriov-fb"
#define GS_FB_MANAGER "nvidia-vgpu-mgr"

#define GS_FB_MAX_GPUS 4
#define GS_FB_MAX_SLOTS 32
#define GS_FB_MAX_DUPS 1 /* a slot is handed out once */
#define GS_FB_MAX_TRACKED 2048

#define GS_FB_NV01_FREE 0x00000000u
#define GS_FB_NV04_ALLOC 0x00000015u
#define GS_FB_NV04_CONTROL 0x00000036u
#define GS_FB_NV04_DUP_OBJECT 0x00000037u
#define GS_FB_ESC_RM_FREE 0x29
#define GS_FB_ESC_RM_ALLOC 0x2B
#define GS_FB_ESC_RM_VID_HEAP_CONTROL 0x4A
#define GS_FB_CLASS_ROOT 0x0u
#define GS_FB_CLASS_ROOT_NON_PRIV 0x1u
#define GS_FB_CLASS_ROOT_CLIENT 0x41u
#define GS_FB_CLASS_DEVICE 0x80u
#define GS_FB_CLASS_MEMORY 0x40u
#define GS_FB_CMD_GPU_GET_ID_INFO 0x202u
#define GS_FB_FLAGS 0x19110u /* the plugin's request in 580.159.01 */
#define GS_FB_FLAG_FIXED_ADDRESS 0x10u
#define GS_FB_ATTR 0x50000004u /* an uncompressed object */
#define GS_FB_ATTR2 0x00080000u
#define GS_FB_HDEV 0x4b4f0001u
#define GS_FB_HMEM 0x4b4f0100u
#define GS_FB_NO_MEMORY 0x51u
#define GS_FB_NOT_SEEN 0xffffffffu
/* NVOS32_PARAMETERS, the argument of the heap control: where the fields this code reads are. */
#define GS_FB_NVOS32_SIZE 184
#define GS_FB_NVOS32_FUNCTION_AT 8
#define GS_FB_NVOS32_STATUS_AT 20
#define GS_FB_NVOS32_FLAGS_AT 52
#define GS_FB_NVOS32_ALLOC_SIZE 2u
#define GS_FB_NVOS32_ALLOC_SIZE_SIZE_AT 88
#define GS_FB_NVOS32_ALLOC_SIZE_OFFSET_AT 104
#define GS_FB_NVOS32_ALLOC_SIZE_RANGE 14u
#define GS_FB_NVOS32_ALLOC_SIZE_RANGE_SIZE_AT 80
#define GS_FB_NVOS32_ALLOC_SIZE_RANGE_OFFSET_AT 96

typedef struct { NvU32 hRoot, hObjectParent, hObjectOld, status; } gs_fb_nvos00_t;
typedef struct {
    NvU32 hRoot, hObjectParent, hObjectNew, hClass;
    NvU64 pAllocParms, pRightsRequested;
    NvU32 paramsSize, flags, status, pad;
} gs_fb_nvos64_t;
typedef struct {
    NvU32 hRoot, hObjectParent, hObjectNew, hClass;
    NvU64 pAllocParms;
    NvU32 paramsSize, status;
} gs_fb_nvos21_t;
typedef struct {
    NvU32 hClient, hObject, cmd, flags;
    NvU64 params;
    NvU32 paramsSize, status;
} gs_fb_nvos54_t;
typedef struct { NvU32 hClient, hParent, hObject, hClientSrc, hObjectSrc, flags, status; } gs_fb_nvos55_t;
/* NV_MEMORY_ALLOCATION_PARAMS, the parameter block of class 0x40. */
typedef struct {
    NvU32 owner, type, flags, width, height;
    NvS32 pitch;
    NvU32 attr, attr2, format, comprCovg, zcullCovg, pad0;
    NvU64 rangeLo, rangeHi, size, alignment, offset, limit, address;
    NvU32 ctagOffset, hVASpace, internalflags, tag;
    NvS32 numaNode;
    NvU32 pad1;
} gs_fb_mem_t;
/* NV0080_ALLOC_PARAMETERS. */
typedef struct {
    NvU32 deviceId, hClientShare, hTargetClient, hTargetDevice, flags, pad0;
    NvU64 vaSpaceSize, vaStartInternal, vaLimitInternal;
    NvU32 vaMode, pad1;
} gs_fb_dev_t;
/* NV0000_CTRL_GPU_GET_ID_INFO_PARAMS. */
typedef struct {
    NvU32 gpuId, gpuFlags, deviceInstance, subDeviceInstance;
    NvU64 szName;
    NvU32 sliStatus, boardId, gpuInstance;
    NvS32 numaId;
} gs_fb_idinfo_t;
/* nvidia_kernel_rmapi_ops_t: the operation, then a union of the parameter structures, aligned to 8. */
typedef struct {
    NvU32 op, pad;
    union {
        gs_fb_nvos00_t free;
        gs_fb_nvos64_t alloc;
        gs_fb_nvos54_t control;
        gs_fb_nvos55_t dup;
        NvU8 room[256];
    } params;
} gs_fb_op_t;

typedef char gs_fb_assert_nvos00[sizeof(gs_fb_nvos00_t) == 16 ? 1 : -1];
typedef char gs_fb_assert_nvos64[sizeof(gs_fb_nvos64_t) == 48 ? 1 : -1];
typedef char gs_fb_assert_nvos21[sizeof(gs_fb_nvos21_t) == 32 ? 1 : -1];
typedef char gs_fb_assert_nvos54[sizeof(gs_fb_nvos54_t) == 32 ? 1 : -1];
typedef char gs_fb_assert_nvos55[sizeof(gs_fb_nvos55_t) == 28 ? 1 : -1];
typedef char gs_fb_assert_mem[sizeof(gs_fb_mem_t) == 128 ? 1 : -1];
typedef char gs_fb_assert_dev[sizeof(gs_fb_dev_t) == 56 ? 1 : -1];
typedef char gs_fb_assert_idinfo[sizeof(gs_fb_idinfo_t) == 40 ? 1 : -1];

/* ---- state ------------------------------------------------------------------------------------- */

enum gs_fb_slot_state {
    GS_FB_SLOT_NONE,     /* the entry is not used */
    GS_FB_SLOT_HELD,     /* hMem is the owner's object of the range */
    GS_FB_SLOT_GIVEN_UP, /* freed for the manager's sake; why says which rule */
    GS_FB_SLOT_FAILED,   /* the RM refused the hold; status says how */
};

/* The duplicate that was handed out: who has it, so that its end is seen. */
struct gs_fb_dup {
    void *key;
    NvU32 hClient, hParent, hObject;
};

struct gs_fb_slot {
    enum gs_fb_slot_state state;
    NvU64 offset, size;
    NvU32 hMem;
    NvU32 status;    /* FAILED: the RM's status */
    const char *why; /* GIVEN_UP: a fixed text */
    gs_fb_mem_t reply; /* what the RM wrote back for the held allocation */
    struct gs_fb_dup dup[GS_FB_MAX_DUPS];
    unsigned int dups, answered;
};

struct gs_fb_gpu {
    int used;
    int open; /* nvidia_dev_get() was called, and client and device exist */
    char bdf[32];
    NvU32 gpu_id, device_instance, hClient, hDevice;
    struct gs_fb_slot slot[GS_FB_MAX_SLOTS];
};

/* A client of the manager's, or a device of such a client, as this code saw it being made. */
struct gs_fb_track {
    int used;
    unsigned long seq; /* the order in which the records were made: the oldest goes when the table is full */
    void *key;       /* the control file it was made on */
    NvU32 hClient;
    NvU32 hDevice;   /* 0: the entry is the client itself */
    NvU32 device_id; /* the RM's device instance, which names the GPU */
};

static DEFINE_MUTEX(gs_fb_lock);
static struct gs_fb_gpu gs_fb_gpus[GS_FB_MAX_GPUS];
static struct gs_fb_track gs_fb_tracked[GS_FB_MAX_TRACKED];
/* How full the table is, how many records were dropped for want of room, and how many devices were not recorded. */
static unsigned long gs_fb_track_seq;
static unsigned int gs_fb_track_used, gs_fb_track_dropped, gs_fb_track_skipped;
static char gs_fb_miss[160];
/* How many slots are held. Read without the lock on the way of every ioctl: 0 means "nothing to do". */
static unsigned int gs_fb_held;
static unsigned long gs_fb_refused_at, gs_fb_nomem_at;
/* What the RM reads and writes during a call is in static storage, one call at a time under the lock. */
static gs_fb_op_t gs_fb_op;
static gs_fb_mem_t gs_fb_memparm;
static gs_fb_dev_t gs_fb_devparm;
static gs_fb_idinfo_t gs_fb_idinfo;
static NvU32 gs_fb_root;
static char gs_fb_cmd[256];

/* ---- the kernel RM client ---------------------------------------------------------------------- */

/*
 * The stack every call into the RM runs on. It is allocated when the first GPU is opened and freed
 * when the last one is released, and used under the lock only: once a slot is held, no call that
 * hands it out, gives it up or releases it can fail for want of memory.
 */
static nvidia_stack_t *gs_fb_sp;

/* One operation through the kernel RM API. 0, or -1 when no GPU is open (and so nothing is held). */
static int gs_fb_rm(void)
{
    if (!gs_fb_sp)
        return -1;
    rm_kernel_rmapi_op(gs_fb_sp, &gs_fb_op);
    return 0;
}

/* The stack goes with the last GPU. */
static void gs_fb_stack_put(void)
{
    unsigned int i;

    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        if (gs_fb_gpus[i].used)
            return;
    if (gs_fb_sp)
        nv_kmem_cache_free_stack(gs_fb_sp);
    gs_fb_sp = NULL;
}

static NvU32 gs_fb_rm_alloc(NvU32 hRoot, NvU32 hParent, NvU32 hNew, NvU32 cls, void *parms)
{
    memset(&gs_fb_op, 0, sizeof(gs_fb_op));
    gs_fb_op.op = GS_FB_NV04_ALLOC;
    gs_fb_op.params.alloc.hRoot = hRoot;
    gs_fb_op.params.alloc.hObjectParent = hParent;
    gs_fb_op.params.alloc.hObjectNew = hNew;
    gs_fb_op.params.alloc.hClass = cls;
    gs_fb_op.params.alloc.pAllocParms = (NvU64)(uintptr_t)parms;
    gs_fb_op.params.alloc.status = 0xffffffffu;
    if (gs_fb_rm() != 0)
        return 0xffffffffu;
    return gs_fb_op.params.alloc.status;
}

static NvU32 gs_fb_rm_free(NvU32 hRoot, NvU32 hParent, NvU32 hOld)
{
    memset(&gs_fb_op, 0, sizeof(gs_fb_op));
    gs_fb_op.op = GS_FB_NV01_FREE;
    gs_fb_op.params.free.hRoot = hRoot;
    gs_fb_op.params.free.hObjectParent = hParent;
    gs_fb_op.params.free.hObjectOld = hOld;
    gs_fb_op.params.free.status = 0xffffffffu;
    if (gs_fb_rm() != 0)
        return 0xffffffffu;
    return gs_fb_op.params.free.status;
}

/* ---- slots ------------------------------------------------------------------------------------- */

static int gs_fb_overlap(const struct gs_fb_slot *s, NvU64 offset, NvU64 size)
{
    return offset < s->offset + s->size && s->offset < offset + size;
}

/*
 * The owner's object of the slot goes. A duplicate that exists keeps the memory. 0, or -1 when the
 * RM did not free it: the object is then still there, the range is still taken, and the slot stays
 * what it is, so that the state never says "not held" of a range that is.
 */
static int gs_fb_slot_free(struct gs_fb_gpu *g, struct gs_fb_slot *s)
{
    NvU32 status;

    if (s->state != GS_FB_SLOT_HELD)
        return 0;
    status = gs_fb_rm_free(g->hClient, g->hDevice, s->hMem);
    if (status != NV_OK) {
        printk(KERN_ERR "geforce-sriov-fb: %s: slot 0x%llx could not be freed (status 0x%x); it stays held until \"release\"\n",
               g->bdf, (unsigned long long)s->offset, status);
        return -1;
    }
    s->hMem = 0;
    s->dups = 0;
    s->state = GS_FB_SLOT_NONE;
    if (gs_fb_held)
        WRITE_ONCE(gs_fb_held, gs_fb_held - 1);
    return 0;
}

/* The slot is given up for the manager's sake: its call is about to reach the RM, and must find the range free. */
static void gs_fb_cede(struct gs_fb_gpu *g, struct gs_fb_slot *s, const char *why)
{
    if (s->state != GS_FB_SLOT_HELD || gs_fb_slot_free(g, s) != 0)
        return;
    s->state = GS_FB_SLOT_GIVEN_UP;
    s->why = why;
    printk(KERN_WARNING "geforce-sriov-fb: %s: slot 0x%llx size 0x%llx is given up: %s\n", g->bdf,
           (unsigned long long)s->offset, (unsigned long long)s->size, why);
}

/* Everything this code has in the RM for one GPU goes, in the reverse order of its making. */
static void gs_fb_gpu_release(struct gs_fb_gpu *g, const char *why)
{
    unsigned int i, freed = 0;

    if (!g->used)
        return;
    for (i = 0; i < GS_FB_MAX_SLOTS; i++) {
        if (g->slot[i].state == GS_FB_SLOT_HELD && gs_fb_slot_free(g, &g->slot[i]) == 0)
            freed++;
    }
    if (g->open) {
        gs_fb_rm_free(g->hClient, g->hClient, g->hDevice);
        gs_fb_rm_free(g->hClient, g->hClient, g->hClient);
        nvidia_dev_put(g->gpu_id, gs_fb_sp);
    }
    printk(KERN_INFO "geforce-sriov-fb: %s: released (%u slot%s freed): %s\n", g->bdf, freed, freed == 1 ? "" : "s", why);
    /* The client is gone, and with it whatever of its objects could not be freed one by one. */
    for (i = 0; i < GS_FB_MAX_SLOTS; i++)
        if (g->slot[i].state == GS_FB_SLOT_HELD && gs_fb_held)
            WRITE_ONCE(gs_fb_held, gs_fb_held - 1);
    memset(g, 0, sizeof(*g));
    gs_fb_stack_put();
}

/* The driver's ID of the GPU at a PCI address; 0 when the driver has no such GPU. */
static NvU32 gs_fb_find_gpu(const char *bdf)
{
    unsigned int domain, bus, slot, function;
    nv_linux_state_t *nvl;
    NvU32 gpu_id = 0;

    if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot, &function) != 4)
        return 0;
    LOCK_NV_LINUX_DEVICES();
    for (nvl = nv_linux_devices; nvl != NULL; nvl = nvl->next) {
        nv_state_t *nv = NV_STATE_PTR(nvl);

        /* A GPU that is being removed from the driver is not one to open: its removal waits for every user to go. */
        if (nv->pci_info.domain == domain && nv->pci_info.bus == bus && nv->pci_info.slot == slot &&
            nv->pci_info.function == function && !(nv->flags & NV_FLAG_PCI_REMOVE_IN_PROGRESS))
            gpu_id = nv->gpu_id;
    }
    UNLOCK_NV_LINUX_DEVICES();
    return gpu_id;
}

/* Opens the GPU and makes the client and the device. 0, or a negative errno with nothing left behind. */
static int gs_fb_gpu_open(struct gs_fb_gpu *g)
{
    NvU32 status;
    int rc;

    if (!gs_fb_sp && nv_kmem_cache_alloc_stack(&gs_fb_sp) != 0) {
        gs_fb_sp = NULL;
        return -ENOMEM;
    }
    rc = nvidia_dev_get(g->gpu_id, gs_fb_sp);
    if (rc != 0) {
        printk(KERN_ERR "geforce-sriov-fb: %s: the GPU cannot be opened (%d)\n", g->bdf, rc);
        gs_fb_stack_put();
        return -EIO;
    }
    /* The root client as NVIDIA's modeset module makes it: every handle 0, the new one comes back in the parameter. */
    gs_fb_root = 0;
    status = gs_fb_rm_alloc(0, 0, 0, GS_FB_CLASS_ROOT, &gs_fb_root);
    g->hClient = gs_fb_root ? gs_fb_root : gs_fb_op.params.alloc.hObjectNew;
    if (status != NV_OK || !g->hClient) {
        printk(KERN_ERR "geforce-sriov-fb: %s: the RM gave no client (status 0x%x)\n", g->bdf, status);
        goto close;
    }
    memset(&gs_fb_idinfo, 0, sizeof(gs_fb_idinfo));
    gs_fb_idinfo.gpuId = g->gpu_id;
    memset(&gs_fb_op, 0, sizeof(gs_fb_op));
    gs_fb_op.op = GS_FB_NV04_CONTROL;
    gs_fb_op.params.control.hClient = g->hClient;
    gs_fb_op.params.control.hObject = g->hClient;
    gs_fb_op.params.control.cmd = GS_FB_CMD_GPU_GET_ID_INFO;
    gs_fb_op.params.control.params = (NvU64)(uintptr_t)&gs_fb_idinfo;
    gs_fb_op.params.control.paramsSize = sizeof(gs_fb_idinfo);
    gs_fb_op.params.control.status = 0xffffffffu;
    if (gs_fb_rm() != 0 || gs_fb_op.params.control.status != NV_OK) {
        printk(KERN_ERR "geforce-sriov-fb: %s: the RM does not tell the device of the GPU (status 0x%x)\n", g->bdf,
               gs_fb_op.params.control.status);
        goto client;
    }
    g->device_instance = gs_fb_idinfo.deviceInstance;
    memset(&gs_fb_devparm, 0, sizeof(gs_fb_devparm));
    gs_fb_devparm.deviceId = g->device_instance;
    gs_fb_devparm.hClientShare = g->hClient;
    status = gs_fb_rm_alloc(g->hClient, g->hClient, GS_FB_HDEV, GS_FB_CLASS_DEVICE, &gs_fb_devparm);
    if (status != NV_OK) {
        printk(KERN_ERR "geforce-sriov-fb: %s: the RM gave no device (status 0x%x)\n", g->bdf, status);
        goto client;
    }
    g->hDevice = GS_FB_HDEV;
    g->open = 1;
    return 0;

client:
    gs_fb_rm_free(g->hClient, g->hClient, g->hClient);
close:
    g->hClient = 0;
    nvidia_dev_put(g->gpu_id, gs_fb_sp);
    gs_fb_stack_put();
    return -EIO;
}

/*
 * "hold": the slot [offset, offset + size) of the GPU at bdf. 0 when the command was carried out,
 * whether or not the RM gave the range (the state says which); a negative errno when it could not
 * be, and then nothing changed.
 */
static int gs_fb_hold(const char *bdf, NvU64 offset, NvU64 size, NvU64 align)
{
    struct gs_fb_gpu *g = NULL;
    struct gs_fb_slot *s = NULL;
    unsigned int i;
    NvU32 gpu_id, status;
    int rc;

    if (!size || (size & 0xfff) || (offset & 0xfff) || offset + size < offset || !align || (align & (align - 1)))
        return -EINVAL;
    gpu_id = gs_fb_find_gpu(bdf);
    if (!gpu_id)
        return -ENODEV;
    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        if (gs_fb_gpus[i].used && gs_fb_gpus[i].gpu_id == gpu_id)
            g = &gs_fb_gpus[i];
    for (i = 0; !g && i < GS_FB_MAX_GPUS; i++)
        if (!gs_fb_gpus[i].used)
            g = &gs_fb_gpus[i];
    if (!g)
        return -ENOSPC;
    if (!g->used) {
        memset(g, 0, sizeof(*g));
        g->gpu_id = gpu_id;
        snprintf(g->bdf, sizeof(g->bdf), "%s", bdf);
        rc = gs_fb_gpu_open(g);
        if (rc != 0)
            return rc;
        g->used = 1;
    }
    /* The same slot again is the same slot; a held one that overlaps is a contradiction; any other entry is replaced. */
    for (i = 0; i < GS_FB_MAX_SLOTS; i++) {
        struct gs_fb_slot *e = &g->slot[i];

        if (e->state == GS_FB_SLOT_NONE || !gs_fb_overlap(e, offset, size))
            continue;
        if (e->offset == offset && e->size == size) {
            if (e->state == GS_FB_SLOT_HELD)
                return 0;
            s = e;
        } else if (e->state == GS_FB_SLOT_HELD) {
            return -EEXIST;
        } else {
            e->state = GS_FB_SLOT_NONE;
        }
    }
    for (i = 0; !s && i < GS_FB_MAX_SLOTS; i++)
        if (g->slot[i].state == GS_FB_SLOT_NONE)
            s = &g->slot[i];
    if (!s)
        return -ENOSPC;
    memset(s, 0, sizeof(*s));
    s->offset = offset;
    s->size = size;
    memset(&gs_fb_memparm, 0, sizeof(gs_fb_memparm));
    gs_fb_memparm.owner = g->hClient;
    gs_fb_memparm.flags = GS_FB_FLAGS;
    gs_fb_memparm.attr = GS_FB_ATTR;
    gs_fb_memparm.attr2 = GS_FB_ATTR2;
    gs_fb_memparm.size = size;
    gs_fb_memparm.alignment = align;
    gs_fb_memparm.offset = offset;
    s->hMem = GS_FB_HMEM + (NvU32)(s - g->slot);
    status = gs_fb_rm_alloc(g->hClient, g->hDevice, s->hMem, GS_FB_CLASS_MEMORY, &gs_fb_memparm);
    if (status != NV_OK) {
        s->hMem = 0;
        s->state = GS_FB_SLOT_FAILED;
        s->status = status;
        printk(KERN_INFO "geforce-sriov-fb: %s: slot 0x%llx size 0x%llx is not held: the RM answered status 0x%x%s\n", bdf,
               (unsigned long long)offset, (unsigned long long)size, status,
               status == GS_FB_NO_MEMORY ? " (the range is in use)" : "");
        return 0;
    }
    s->state = GS_FB_SLOT_HELD;
    WRITE_ONCE(gs_fb_held, gs_fb_held + 1);
    /* The RM must have given exactly the range that was asked for; anything else is not a slot. */
    if (gs_fb_memparm.offset != offset || gs_fb_memparm.size != size) {
        printk(KERN_ERR "geforce-sriov-fb: %s: asked for 0x%llx size 0x%llx, the RM placed 0x%llx size 0x%llx: not held\n", bdf,
               (unsigned long long)offset, (unsigned long long)size, (unsigned long long)gs_fb_memparm.offset,
               (unsigned long long)gs_fb_memparm.size);
        /* Should the RM not take the object back, the entry says what is really held, for "release" and for the state. */
        if (gs_fb_slot_free(g, s) != 0) {
            s->offset = gs_fb_memparm.offset;
            s->size = gs_fb_memparm.size;
            s->reply = gs_fb_memparm;
            return 0;
        }
        s->state = GS_FB_SLOT_FAILED;
        s->status = 0xffffffffu;
        return 0;
    }
    s->reply = gs_fb_memparm;
    printk(KERN_INFO "geforce-sriov-fb: %s: slot 0x%llx size 0x%llx is held (client 0x%x, memory 0x%x)\n", bdf,
           (unsigned long long)offset, (unsigned long long)size, g->hClient, s->hMem);
    return 0;
}

/* ---- who asks, and for which client ------------------------------------------------------------ */

/*
 * 1 when the calling process is named as the manager is, may administer the system, and is a
 * process of the host itself: root in a container that has the GPU's device files has the
 * capability too, and has a PID namespace of its own.
 */
static int gs_fb_is_manager(void)
{
    return strcmp(current->group_leader->comm, GS_FB_MANAGER) == 0 && task_pid(current)->level == 0 &&
           capable(CAP_SYS_ADMIN);
}

/* The caller's name for the log: a process chooses its name, so only printable characters of it are shown. */
static void gs_fb_name(char *out, size_t len)
{
    const char *comm = current->group_leader->comm;
    size_t i;

    for (i = 0; i + 1 < len && comm[i]; i++)
        out[i] = comm[i] >= 0x20 && comm[i] < 0x7f ? comm[i] : '?';
    out[i] = '\0';
}

/* The control file a call belongs to: the expression the RM itself uses for the owner of a client. */
static void *gs_fb_key(nv_linux_file_private_t *nvlfp)
{
    return nvlfp->nvfp.ctl_nvfp ? (void *)nvlfp->nvfp.ctl_nvfp : (void *)&nvlfp->nvfp;
}

static struct gs_fb_track *gs_fb_track_find(void *key, NvU32 hClient, NvU32 hDevice)
{
    unsigned int i;

    for (i = 0; i < GS_FB_MAX_TRACKED; i++)
        if (gs_fb_tracked[i].used && gs_fb_tracked[i].key == key && gs_fb_tracked[i].hClient == hClient &&
            gs_fb_tracked[i].hDevice == hDevice)
            return &gs_fb_tracked[i];
    return NULL;
}

/*
 * The manager makes many clients: each of its processes (one per VM) makes several, for cards of
 * every generation, and keeps them while the VM runs. A record is needed from the moment a client
 * or device is made until the framebuffer request under it, which follows shortly. So when the
 * table is full the oldest record goes: the request of a VM that is starting now finds its records,
 * and a record that is dropped can only cost a slot its hold, never hand one out.
 */
static void gs_fb_track_add(void *key, NvU32 hClient, NvU32 hDevice, NvU32 device_id)
{
    struct gs_fb_track *t = gs_fb_track_find(key, hClient, hDevice), *oldest = NULL;
    unsigned int i;

    for (i = 0; !t && i < GS_FB_MAX_TRACKED; i++) {
        if (!gs_fb_tracked[i].used) {
            t = &gs_fb_tracked[i];
            WRITE_ONCE(gs_fb_track_used, gs_fb_track_used + 1);
        } else if (!oldest || gs_fb_tracked[i].seq < oldest->seq) {
            oldest = &gs_fb_tracked[i];
        }
    }
    if (!t) {
        t = oldest;
        if (!gs_fb_track_dropped++)
            printk(KERN_WARNING "geforce-sriov-fb: the record of the manager's clients is full (%u); the oldest entries are dropped from now on\n",
                   GS_FB_MAX_TRACKED);
    }
    t->used = 1;
    t->seq = ++gs_fb_track_seq;
    t->key = key;
    t->hClient = hClient;
    t->hDevice = hDevice;
    t->device_id = device_id;
}

/* Forgets what is recorded for a file (hClient 0), a client of it (hObject == hClient), or one object of a client. */
static void gs_fb_forget(void *key, NvU32 hClient, NvU32 hObject)
{
    unsigned int i, k, d;

    for (i = 0; i < GS_FB_MAX_TRACKED; i++) {
        struct gs_fb_track *t = &gs_fb_tracked[i];

        if (!t->used || t->key != key)
            continue;
        if (!hClient || (t->hClient == hClient && (hObject == hClient || t->hDevice == hObject))) {
            t->used = 0;
            WRITE_ONCE(gs_fb_track_used, gs_fb_track_used - 1);
        }
    }
    for (i = 0; i < GS_FB_MAX_GPUS; i++) {
        for (k = 0; gs_fb_gpus[i].used && k < GS_FB_MAX_SLOTS; k++) {
            struct gs_fb_slot *s = &gs_fb_gpus[i].slot[k];

            for (d = 0; d < s->dups;) {
                const struct gs_fb_dup *u = &s->dup[d];

                if (u->key == key && (!hClient || (u->hClient == hClient && (hObject == hClient || u->hObject == hObject ||
                                                                              u->hParent == hObject))))
                    s->dup[d] = s->dup[--s->dups];
                else
                    d++;
            }
        }
    }
}

/* ---- the decision ------------------------------------------------------------------------------ */

enum gs_fb_form { GS_FB_FORM_NVOS64, GS_FB_FORM_NVOS21, GS_FB_FORM_HEAP };

/* A request for video memory at a fixed address, in any of the forms the RM takes. */
struct gs_fb_req {
    enum gs_fb_form form;
    NvU32 hRoot, hParent, hNew, flags;
    NvU64 offset, size;
};

enum gs_fb_what { GS_FB_PASS, GS_FB_ANSWER, GS_FB_CEDE };

struct gs_fb_verdict {
    enum gs_fb_what what;
    int refused;                /* PASS of a request that the RM will refuse because the range is held */
    int gpu, slot;              /* ANSWER: which */
    NvU32 cede[GS_FB_MAX_GPUS]; /* CEDE: one bit per slot */
    const char *why;
};

#define GS_FB_GPU_UNTRACKED (-2) /* the caller's client or device was not seen being made */
#define GS_FB_GPU_OTHER (-1)     /* a device of a GPU on which nothing is held */

/*
 * What to do with a fixed-address request. Pure: it reads the table and changes nothing.
 *
 *   1  the range overlaps no held slot                               PASS
 *   2  the caller is not the manager                                 PASS; the holds stay, and the RM refuses
 *   3  the manager's client or device is not a recorded one          CEDE whatever the range overlaps, on every GPU
 *   4  the device is of a GPU on which the range overlaps nothing    PASS
 *   5  the range is not exactly one held slot of that GPU            CEDE what it overlaps on that GPU
 *   6  exact, but as a heap control, with other flags, or no handle  CEDE that slot
 *   7  exact, and a duplicate of the slot is recorded                CEDE that slot
 *   8  exact, class 0x40, the plugin's flags, a handle               ANSWER
 *
 * Row 7: a slot is handed out once. While its duplicate exists, a guest runs on the memory, and a
 * second duplicate would let another client read and write it. Giving the slot up is right in both
 * possible cases: if the duplicate exists the RM refuses the request, and if the record is stale
 * (a free that was not seen) the range is free and the manager's own allocation succeeds.
 */
static void gs_fb_decide(const struct gs_fb_req *r, int manager, int gpu, struct gs_fb_verdict *v)
{
    unsigned int i, k;
    int any = 0, exact = -1;

    memset(v, 0, sizeof(*v));
    v->what = GS_FB_PASS;
    for (i = 0; i < GS_FB_MAX_GPUS; i++) {
        for (k = 0; gs_fb_gpus[i].used && k < GS_FB_MAX_SLOTS; k++) {
            const struct gs_fb_slot *s = &gs_fb_gpus[i].slot[k];

            if (s->state == GS_FB_SLOT_HELD && gs_fb_overlap(s, r->offset, r->size)) {
                v->cede[i] |= (NvU32)1 << k;
                any = 1;
            }
        }
    }
    if (!any)
        return;
    if (!manager) {
        v->refused = 1;
        return;
    }
    if (gpu == GS_FB_GPU_UNTRACKED) {
        v->what = GS_FB_CEDE;
        v->why = gs_fb_miss[0] ? gs_fb_miss : "the manager asked with a client or a device that was not seen being made";
        return;
    }
    if (gpu < 0 || gpu >= GS_FB_MAX_GPUS || !v->cede[gpu]) {
        memset(v->cede, 0, sizeof(v->cede));
        return;
    }
    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        if ((int)i != gpu)
            v->cede[i] = 0;
    for (k = 0; k < GS_FB_MAX_SLOTS; k++) {
        const struct gs_fb_slot *s = &gs_fb_gpus[gpu].slot[k];

        if ((v->cede[gpu] & ((NvU32)1 << k)) && s->offset == r->offset && s->size == r->size)
            exact = (int)k;
    }
    v->what = GS_FB_CEDE;
    if (exact < 0) {
        v->why = "the manager's request overlaps the slot and is not equal to it";
        return;
    }
    v->cede[gpu] = (NvU32)1 << exact;
    if (r->form == GS_FB_FORM_HEAP)
        v->why = "the manager asked for the slot through the heap control";
    else if (r->flags != GS_FB_FLAGS)
        v->why = "the manager's request has other flags than the plugin's";
    else if (!r->hNew)
        v->why = "the manager's request names no handle";
    else if (gs_fb_gpus[gpu].slot[exact].dups)
        v->why = "the manager asked for the slot while a duplicate of it is recorded";
    else {
        v->what = GS_FB_ANSWER;
        v->gpu = gpu;
        v->slot = exact;
        memset(v->cede, 0, sizeof(v->cede));
    }
}

/* ---- the calls on the way of an ioctl ---------------------------------------------------------- */

/*
 * Reads a request for video memory at a fixed address out of an ioctl's argument. 1 when it is one;
 * *user is then the caller's parameter block for the two class forms, and *mp a copy of it.
 */
static int gs_fb_parse(int cmd, void *arg, size_t size, struct gs_fb_req *r, NvU64 *user, gs_fb_mem_t *mp)
{
    const NvU32 *head = arg; /* hRoot, hObjectParent, hObjectNew, hClass: the same in both class forms */
    const NvU8 *p = arg;
    NvU32 function;

    memset(r, 0, sizeof(*r));
    memset(mp, 0, sizeof(*mp));
    *user = 0;
    if (cmd == GS_FB_ESC_RM_VID_HEAP_CONTROL) {
        if (size != GS_FB_NVOS32_SIZE)
            return 0;
        memcpy(&function, p + GS_FB_NVOS32_FUNCTION_AT, sizeof(function));
        memcpy(&r->flags, p + GS_FB_NVOS32_FLAGS_AT, sizeof(r->flags));
        if ((function != GS_FB_NVOS32_ALLOC_SIZE && function != GS_FB_NVOS32_ALLOC_SIZE_RANGE) ||
            !(r->flags & GS_FB_FLAG_FIXED_ADDRESS))
            return 0;
        r->form = GS_FB_FORM_HEAP;
        r->hRoot = head[0];
        r->hParent = head[1];
        memcpy(&r->size, p + (function == GS_FB_NVOS32_ALLOC_SIZE ? GS_FB_NVOS32_ALLOC_SIZE_SIZE_AT :
                                                                    GS_FB_NVOS32_ALLOC_SIZE_RANGE_SIZE_AT),
               sizeof(r->size));
        memcpy(&r->offset, p + (function == GS_FB_NVOS32_ALLOC_SIZE ? GS_FB_NVOS32_ALLOC_SIZE_OFFSET_AT :
                                                                      GS_FB_NVOS32_ALLOC_SIZE_RANGE_OFFSET_AT),
               sizeof(r->offset));
        return 1;
    }
    if (cmd != GS_FB_ESC_RM_ALLOC)
        return 0;
    if (size == sizeof(gs_fb_nvos64_t)) {
        r->form = GS_FB_FORM_NVOS64;
        *user = ((const gs_fb_nvos64_t *)arg)->pAllocParms;
    } else if (size == sizeof(gs_fb_nvos21_t)) {
        r->form = GS_FB_FORM_NVOS21;
        *user = ((const gs_fb_nvos21_t *)arg)->pAllocParms;
    } else {
        return 0;
    }
    if (head[3] != GS_FB_CLASS_MEMORY || !*user)
        return 0;
    /* The size field of the escape is not looked at: the plugin sends 0 and the RM takes the size from the class. */
    if (copy_from_user(mp, (void __user *)(uintptr_t)*user, sizeof(*mp)))
        return 0;
    if (!(mp->flags & GS_FB_FLAG_FIXED_ADDRESS))
        return 0;
    r->hRoot = head[0];
    r->hParent = head[1];
    r->hNew = head[2];
    r->flags = mp->flags;
    r->offset = mp->offset;
    r->size = mp->size;
    return 1;
}

/* The GPU of a request, from the record of who made its client and device. Called with the lock held. */
static int gs_fb_request_gpu(void *key, const struct gs_fb_req *r)
{
    const struct gs_fb_track *t = NULL;
    const char *what = NULL;
    unsigned int i;


    /* Which record is missing is said in the reason, with the state of the table when records were dropped. */
    if (!gs_fb_track_find(key, r->hRoot, 0))
        what = "with a client that was not seen being made";
    else if (!r->hParent)
        what = "without naming a device";
    else if (!(t = gs_fb_track_find(key, r->hRoot, r->hParent)))
        what = "with a device that was not seen being made";
    if (what) {
        if (gs_fb_track_dropped)
            snprintf(gs_fb_miss, sizeof(gs_fb_miss), "the manager asked %s (%u records were dropped: the table of %u was full)",
                     what, gs_fb_track_dropped, GS_FB_MAX_TRACKED);
        else
            snprintf(gs_fb_miss, sizeof(gs_fb_miss), "the manager asked %s", what);
        return GS_FB_GPU_UNTRACKED;
    }
    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        if (gs_fb_gpus[i].used && gs_fb_gpus[i].open && gs_fb_gpus[i].device_instance == t->device_id)
            return (int)i;
    return GS_FB_GPU_OTHER;
}

static void gs_fb_cede_set(const NvU32 *set, const char *why)
{
    unsigned int i, k;

    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        for (k = 0; k < GS_FB_MAX_SLOTS; k++)
            if (set[i] & ((NvU32)1 << k))
                gs_fb_cede(&gs_fb_gpus[i], &gs_fb_gpus[i].slot[k], why);
}

int gs_fb_ioctl_pre(nv_linux_file_private_t *nvlfp, int cmd, void *arg, size_t size, NvU32 *seen)
{
    char name[20];
    struct gs_fb_verdict v;
    struct gs_fb_req r;
    struct gs_fb_gpu *g;
    struct gs_fb_slot *s;
    gs_fb_mem_t mp;
    NvU32 *head = arg;
    NvU64 user;
    NvU32 status;
    void *key;
    int manager, answered = 0;

    /*
     * A device is about to be made: which GPU it is of is read here, before the RM reads it, and
     * compared after the call (gs_fb_ioctl_post). A caller that changes the block in between is
     * not recorded.
     */
    *seen = GS_FB_NOT_SEEN;
    if (cmd == GS_FB_ESC_RM_ALLOC && (size == sizeof(gs_fb_nvos64_t) || size == sizeof(gs_fb_nvos21_t)) &&
        head[3] == GS_FB_CLASS_DEVICE) {
        user = size == sizeof(gs_fb_nvos64_t) ? ((const gs_fb_nvos64_t *)arg)->pAllocParms :
                                               ((const gs_fb_nvos21_t *)arg)->pAllocParms;
        /* NV0080_ALLOC_PARAMETERS begins with the device instance; without a block it is 0. */
        if (!user)
            *seen = 0;
        else if (copy_from_user(&status, (void __user *)(uintptr_t)user, sizeof(status)) == 0)
            *seen = status;
        return 0;
    }
    /* The way of every ioctl: nothing is held, or this is no allocation. */
    if (!READ_ONCE(gs_fb_held) || (cmd != GS_FB_ESC_RM_ALLOC && cmd != GS_FB_ESC_RM_VID_HEAP_CONTROL))
        return 0;
    if (!gs_fb_parse(cmd, arg, size, &r, &user, &mp))
        return 0;
    manager = gs_fb_is_manager();
    key = gs_fb_key(nvlfp);
    mutex_lock(&gs_fb_lock);
    gs_fb_miss[0] = '\0';
    gs_fb_decide(&r, manager, manager ? gs_fb_request_gpu(key, &r) : GS_FB_GPU_UNTRACKED, &v);
    if (v.what == GS_FB_PASS) {
        /* Not the manager, for a held range: the RM refuses it, which is what the hold is for. */
        /* Anyone can make this line appear, so it is written at most once in ten seconds. */
        if (v.refused && (!gs_fb_refused_at || time_after(jiffies, gs_fb_refused_at + 10 * HZ))) {
            gs_fb_refused_at = jiffies ? jiffies : 1;
            gs_fb_name(name, sizeof(name));
            printk(KERN_INFO "geforce-sriov-fb: %s[%d] asks for 0x%llx size 0x%llx, which is held: left to the RM, which refuses it\n",
                   name, (int)current->pid, (unsigned long long)r.offset, (unsigned long long)r.size);
        }
        goto out;
    }
    gs_fb_name(name, sizeof(name));
    printk(KERN_INFO "geforce-sriov-fb: %s[%d] asks for 0x%llx size 0x%llx flags 0x%x: client 0x%x parent 0x%x handle 0x%x\n",
           name, (int)current->pid, (unsigned long long)r.offset, (unsigned long long)r.size, r.flags,
           r.hRoot, r.hParent, r.hNew);
    if (v.what == GS_FB_CEDE) {
        gs_fb_cede_set(v.cede, v.why);
        goto out;
    }
    g = &gs_fb_gpus[v.gpu];
    s = &g->slot[v.slot];
    memset(&gs_fb_op, 0, sizeof(gs_fb_op));
    gs_fb_op.op = GS_FB_NV04_DUP_OBJECT;
    gs_fb_op.params.dup.hClient = r.hRoot;
    gs_fb_op.params.dup.hParent = r.hParent;
    gs_fb_op.params.dup.hObject = r.hNew;
    gs_fb_op.params.dup.hClientSrc = g->hClient;
    gs_fb_op.params.dup.hObjectSrc = s->hMem;
    gs_fb_op.params.dup.status = 0xffffffffu;
    status = gs_fb_rm() != 0 ? 0xffffffffu : gs_fb_op.params.dup.status;
    if (status != NV_OK) {
        printk(KERN_WARNING "geforce-sriov-fb: %s: the duplicate of slot 0x%llx answered status 0x%x\n", g->bdf,
               (unsigned long long)s->offset, status);
        gs_fb_cede(g, s, "the duplicate for the manager failed");
        goto out;
    }
    /* What the RM writes back for an allocation, as it wrote it for the held one. The caller's inputs stay. */
    mp.pitch = s->reply.pitch;
    mp.attr = s->reply.attr;
    mp.attr2 = s->reply.attr2;
    mp.format = s->reply.format;
    mp.comprCovg = s->reply.comprCovg;
    mp.zcullCovg = s->reply.zcullCovg;
    mp.limit = s->reply.limit;
    mp.address = s->reply.address;
    mp.ctagOffset = s->reply.ctagOffset;
    if (copy_to_user((void __user *)(uintptr_t)user, &mp, sizeof(mp))) {
        /* The caller would see an allocation without its reply: the duplicate goes again. */
        gs_fb_rm_free(r.hRoot, r.hParent, gs_fb_op.params.dup.hObject);
        gs_fb_cede(g, s, "the reply could not be written to the manager");
        goto out;
    }
    head[2] = gs_fb_op.params.dup.hObject;
    if (r.form == GS_FB_FORM_NVOS64)
        ((gs_fb_nvos64_t *)arg)->status = NV_OK;
    else
        ((gs_fb_nvos21_t *)arg)->status = NV_OK;
    s->dup[0].key = key;
    s->dup[0].hClient = r.hRoot;
    s->dup[0].hParent = r.hParent;
    s->dup[0].hObject = head[2];
    s->dups = 1;
    s->answered++;
    printk(KERN_INFO "geforce-sriov-fb: %s: slot 0x%llx: answered with a duplicate at handle 0x%x (%u so far)\n", g->bdf,
           (unsigned long long)s->offset, head[2], s->answered);
    answered = 1;
out:
    mutex_unlock(&gs_fb_lock);
    return answered;
}

/*
 * An allocation of video memory by the manager came back "no memory" from the RM although this
 * code let it through.
 *   - A request at a fixed address (r is set): the held slots it overlaps and nobody has a
 *     duplicate of are given up. One that overlaps no held slot was not refused because of a hold,
 *     and nothing changes: another slot that is not held must not cost the others their protection.
 *   - Any other request for video memory (r is NULL): one line in the log, and nothing is given
 *     up. Anyone who fills the GPU's free memory can make such a request fail, and that must not
 *     cost every idle slot its protection.
 * On every GPU when the GPU of the request is not known.
 */
static void gs_fb_no_memory(void *key, NvU32 hRoot, NvU32 hParent, const struct gs_fb_req *r)
{
    struct gs_fb_req who = { .hRoot = hRoot, .hParent = hParent };
    int gpu = gs_fb_request_gpu(key, &who);
    unsigned int i, k;

    if (!r) {
        if (!gs_fb_nomem_at || time_after(jiffies, gs_fb_nomem_at + 10 * HZ)) {
            gs_fb_nomem_at = jiffies ? jiffies : 1;
            printk(KERN_INFO "geforce-sriov-fb: an allocation of the manager's that is not a fixed one answered \"no memory\" while slots are held; nothing is given up for it\n");
        }
        return;
    }
    for (i = 0; i < GS_FB_MAX_GPUS; i++) {
        if (!gs_fb_gpus[i].used || (gpu >= 0 && (int)i != gpu) || gpu == GS_FB_GPU_OTHER)
            continue;
        for (k = 0; k < GS_FB_MAX_SLOTS; k++) {
            struct gs_fb_slot *s = &gs_fb_gpus[i].slot[k];

            if (s->state == GS_FB_SLOT_HELD && !s->dups && gs_fb_overlap(s, r->offset, r->size))
                gs_fb_cede(&gs_fb_gpus[i], s, "the manager's request for it answered \"no memory\" while it was held");
        }
    }
}

void gs_fb_ioctl_post(nv_linux_file_private_t *nvlfp, int cmd, void *arg, size_t size, NV_STATUS rm_status, NvU32 seen)
{
    const NvU32 *head = arg;
    struct gs_fb_req r;
    gs_fb_mem_t mp;
    NvU64 user = 0;
    NvU32 status, device_id = 0;
    void *key;

    if (cmd != GS_FB_ESC_RM_ALLOC && cmd != GS_FB_ESC_RM_FREE && cmd != GS_FB_ESC_RM_VID_HEAP_CONTROL)
        return;
    if (rm_status != NV_OK)
        return;
    key = gs_fb_key(nvlfp);
    if (cmd == GS_FB_ESC_RM_FREE) {
        const gs_fb_nvos00_t *f = arg;

        if (size != sizeof(*f) || f->status != NV_OK)
            return;
        /*
         * The RM freed it, so the record goes, whoever asked. The end of a whole client is followed
         * for every caller: a control file can outlive the process that made a client on it (a file
         * that a parent keeps open), and the client may then be freed by a process that does not
         * pass as the manager. Its records must not stay behind for as long as that file is open.
         * Other objects are followed for the manager only, which keeps this off the path of every
         * other program's frees.
         */
        if (f->hObjectOld != f->hRoot && !gs_fb_is_manager())
            return;
        if (f->hObjectOld == f->hRoot && !READ_ONCE(gs_fb_track_used))
            return;
        mutex_lock(&gs_fb_lock);
        gs_fb_forget(key, f->hRoot, f->hObjectOld);
        mutex_unlock(&gs_fb_lock);
        return;
    }
    if (!gs_fb_is_manager())
        return;
    if (cmd == GS_FB_ESC_RM_VID_HEAP_CONTROL) {
        if (size != GS_FB_NVOS32_SIZE)
            return;
        memcpy(&status, (const NvU8 *)arg + GS_FB_NVOS32_STATUS_AT, sizeof(status));
    } else if (size == sizeof(gs_fb_nvos64_t)) {
        status = ((const gs_fb_nvos64_t *)arg)->status;
        user = ((const gs_fb_nvos64_t *)arg)->pAllocParms;
    } else if (size == sizeof(gs_fb_nvos21_t)) {
        status = ((const gs_fb_nvos21_t *)arg)->status;
        user = ((const gs_fb_nvos21_t *)arg)->pAllocParms;
    } else {
        return;
    }
    if (status == GS_FB_NO_MEMORY) {
        int fixed;

        /* Video memory only: class 0x40, or the heap control. */
        if (!READ_ONCE(gs_fb_held) || (cmd == GS_FB_ESC_RM_ALLOC && head[3] != GS_FB_CLASS_MEMORY))
            return;
        fixed = gs_fb_parse(cmd, arg, size, &r, &user, &mp);
        mutex_lock(&gs_fb_lock);
        gs_fb_no_memory(key, head[0], head[1], fixed ? &r : NULL);
        mutex_unlock(&gs_fb_lock);
        return;
    }
    if (status != NV_OK || cmd != GS_FB_ESC_RM_ALLOC)
        return;
    /* The manager made a client, or a device of one of its clients: that is remembered, with the file it was made on. */
    if (head[3] == GS_FB_CLASS_ROOT || head[3] == GS_FB_CLASS_ROOT_NON_PRIV || head[3] == GS_FB_CLASS_ROOT_CLIENT) {
        mutex_lock(&gs_fb_lock);
        gs_fb_track_add(key, head[2] ? head[2] : head[0], 0, 0);
        mutex_unlock(&gs_fb_lock);
    } else if (head[3] == GS_FB_CLASS_DEVICE && head[2]) {
        /* The device instance as it was before the call and as it is now: recorded only when they agree. */
        if (user && copy_from_user(&device_id, (void __user *)(uintptr_t)user, sizeof(device_id)))
            return;
        mutex_lock(&gs_fb_lock);
        if (seen != GS_FB_NOT_SEEN && seen == device_id && gs_fb_track_find(key, head[0], 0))
            gs_fb_track_add(key, head[0], head[2], device_id);
        else
            gs_fb_track_skipped++;
        mutex_unlock(&gs_fb_lock);
    }
}

void gs_fb_file_closed(nv_linux_file_private_t *nvlfp)
{
    mutex_lock(&gs_fb_lock);
    gs_fb_forget(&nvlfp->nvfp, 0, 0);
    mutex_unlock(&gs_fb_lock);
}

void gs_fb_gpu_remove(NvU32 gpu_id)
{
    unsigned int i;

    mutex_lock(&gs_fb_lock);
    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        if (gs_fb_gpus[i].used && gs_fb_gpus[i].gpu_id == gpu_id)
            gs_fb_gpu_release(&gs_fb_gpus[i], "the GPU is removed from the driver");
    mutex_unlock(&gs_fb_lock);
}

/* ---- /proc/driver/nvidia/geforce-sriov-fb ------------------------------------------------------ */

static int gs_fb_show(struct seq_file *m, void *unused)
{
    unsigned int i, k, n, most;

    mutex_lock(&gs_fb_lock);
    seq_printf(m, "abi %d\n", GS_FB_ABI);
    /*
     * The record of the manager's clients and devices: entries in use, the files they belong to, the
     * most that one file has (records that pile up on one file that stays open show here), what was lost.
     */
    for (i = 0, n = 0, most = 0; i < GS_FB_MAX_TRACKED; i++) {
        unsigned int same = 0;

        if (!gs_fb_tracked[i].used)
            continue;
        for (k = 0; k < i; k++)
            if (gs_fb_tracked[k].used && gs_fb_tracked[k].key == gs_fb_tracked[i].key)
                break;
        if (k != i)
            continue;
        n++;
        for (k = i; k < GS_FB_MAX_TRACKED; k++)
            same += gs_fb_tracked[k].used && gs_fb_tracked[k].key == gs_fb_tracked[i].key;
        if (same > most)
            most = same;
    }
    seq_printf(m, "tracking %u of %u files %u most-on-one-file %u dropped %u devices-not-recorded %u\n", gs_fb_track_used,
               GS_FB_MAX_TRACKED, n, most, gs_fb_track_dropped, gs_fb_track_skipped);
    for (i = 0; i < GS_FB_MAX_GPUS; i++) {
        const struct gs_fb_gpu *g = &gs_fb_gpus[i];

        if (!g->used)
            continue;
        for (n = 0, k = 0; k < GS_FB_MAX_SLOTS; k++)
            n += g->slot[k].state != GS_FB_SLOT_NONE;
        seq_printf(m, "gpu %s device %u client 0x%x slots %u\n", g->bdf, g->device_instance, g->hClient, n);
        for (k = 0; k < GS_FB_MAX_SLOTS; k++) {
            const struct gs_fb_slot *s = &g->slot[k];

            if (s->state == GS_FB_SLOT_HELD)
                seq_printf(m, "slot %s 0x%llx 0x%llx held dups %u answered %u\n", g->bdf, (unsigned long long)s->offset,
                           (unsigned long long)s->size, s->dups, s->answered);
            else if (s->state == GS_FB_SLOT_GIVEN_UP)
                seq_printf(m, "slot %s 0x%llx 0x%llx given-up %s\n", g->bdf, (unsigned long long)s->offset,
                           (unsigned long long)s->size, s->why);
            else if (s->state == GS_FB_SLOT_FAILED)
                seq_printf(m, "slot %s 0x%llx 0x%llx failed 0x%x\n", g->bdf, (unsigned long long)s->offset,
                           (unsigned long long)s->size, s->status);
        }
    }
    mutex_unlock(&gs_fb_lock);
    return 0;
}

static int gs_fb_proc_open(struct inode *inode, struct file *file)
{
    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;
    return single_open(file, gs_fb_show, NULL);
}

/*
 * One command per write:
 *   hold <dddd:bb:dd.f> <offset> <size> <align>     hexadecimal, without 0x
 *   release <dddd:bb:dd.f>
 *   release all
 * Anything else is refused and changes nothing.
 */
static ssize_t gs_fb_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
    unsigned long long offset, size, align;
    unsigned int i;
    char bdf[32], extra;
    int rc = -EINVAL;

    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;
    if (!count || count >= sizeof(gs_fb_cmd))
        return -EINVAL;
    mutex_lock(&gs_fb_lock);
    if (copy_from_user(gs_fb_cmd, buf, count)) {
        mutex_unlock(&gs_fb_lock);
        return -EFAULT;
    }
    gs_fb_cmd[count] = '\0';
    if (gs_fb_cmd[count - 1] == '\n')
        gs_fb_cmd[count - 1] = '\0';
    if (strcmp(gs_fb_cmd, "release all") == 0) {
        for (i = 0; i < GS_FB_MAX_GPUS; i++)
            gs_fb_gpu_release(&gs_fb_gpus[i], "\"release all\" was written");
        rc = 0;
    } else if (sscanf(gs_fb_cmd, "release %31s %c", bdf, &extra) == 1) {
        for (i = 0; i < GS_FB_MAX_GPUS; i++)
            if (gs_fb_gpus[i].used && strcmp(gs_fb_gpus[i].bdf, bdf) == 0)
                gs_fb_gpu_release(&gs_fb_gpus[i], "\"release\" was written");
        rc = 0;
    } else if (sscanf(gs_fb_cmd, "hold %31s %llx %llx %llx %c", bdf, &offset, &size, &align, &extra) == 4) {
        rc = gs_fb_hold(bdf, offset, size, align);
    }
    mutex_unlock(&gs_fb_lock);
    return rc ? rc : (ssize_t)count;
}

static const struct proc_ops gs_fb_proc_ops = {
    .proc_open = gs_fb_proc_open,
    .proc_read = seq_read,
    .proc_write = gs_fb_proc_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

void gs_fb_init(void)
{
    if (!proc_create(GS_FB_PROC_NAME, 0600, NULL, &gs_fb_proc_ops))
        printk(KERN_WARNING "geforce-sriov-fb: /proc/" GS_FB_PROC_NAME " could not be made: no framebuffer slot can be held\n");
}

/* The module goes: nothing of this code may be left in the RM, and the GPUs must be closed before the devices go. */
void gs_fb_exit(void)
{
    unsigned int i;

    remove_proc_entry(GS_FB_PROC_NAME, NULL);
    mutex_lock(&gs_fb_lock);
    for (i = 0; i < GS_FB_MAX_GPUS; i++)
        gs_fb_gpu_release(&gs_fb_gpus[i], "the module is unloaded");
    mutex_unlock(&gs_fb_lock);
}
