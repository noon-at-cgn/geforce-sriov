// SPDX-License-Identifier: MIT
/*
 * The owner of the Ada VFs' framebuffer slots (driver-patches/fb-owner/gs-fb-owner.c), the file that
 * goes into nvidia.ko, compiled here as it is and run against a model of NVIDIA's resource manager:
 * clients, devices, video memory with fixed and bottom-up placement, references to one allocation,
 * and failures on request. user_ioctl() stands for nvidia_ioctl()'s default arm.
 */
#include <stdarg.h>
#include <stdlib.h>

#include "stub.h"
#include "gs-fb-owner.c"

static int checks, failures;
#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

/* ---- the kernel's side ------------------------------------------------------------------------ */

static struct task_struct manager_task = { "nvidia-vgpu-mgr", 100, &manager_task, { 0 } };
static struct task_struct host_task = { "python3", 200, &host_task, { 0 } };
static struct task_struct thread_of_manager = { "vgpu-worker", 101, &manager_task, { 0 } };
/* Root in a container that has the GPU's device files and took the manager's name: a PID namespace of its own. */
static struct task_struct odd_task = { "ev\til\033", 300, &odd_task, { 0 } };
static struct task_struct container_task = { "nvidia-vgpu-mgr", 7, &container_task, { 1 } };
struct task_struct *current = &host_task;
unsigned long jiffies = 1000;
static int is_root = 1, fail_stack, fault_to_user, fault_from_user, fail_dev_get;
static int dev_gets, dev_puts, proc_made;
static const struct proc_ops *proc_ops_seen;
static char klog[65536], shown[8192];

int capable(int cap) { (void)cap; return is_root; }
void mutex_lock(struct mutex *lock) { if (lock->locked) { printf("FAIL: the lock is taken twice\n"); exit(1); } lock->locked = 1; }
void mutex_unlock(struct mutex *lock) { lock->locked = 0; }
int printk(const char *fmt, ...)
{
	size_t at = strlen(klog);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(klog + at, sizeof(klog) - at, fmt, ap);
	va_end(ap);
	return 0;
}
unsigned long copy_from_user(void *to, const void *from, unsigned long n)
{
	if (fault_from_user)
		return n;
	memcpy(to, from, n);
	return 0;
}
unsigned long copy_to_user(void *to, const void *from, unsigned long n)
{
	if (fault_to_user)
		return n;
	memcpy(to, from, n);
	return 0;
}
static int stacks;
int nv_kmem_cache_alloc_stack(nvidia_stack_t **sp)
{
	if (fail_stack)
		return -1;
	*sp = (nvidia_stack_t *)&stacks;
	stacks++;
	return 0;
}
void nv_kmem_cache_free_stack(nvidia_stack_t *sp) { (void)sp; stacks--; }
static int fail_free;
void seq_printf(struct seq_file *m, const char *fmt, ...)
{
	size_t at = strlen(shown);
	va_list ap;

	(void)m;
	va_start(ap, fmt);
	vsnprintf(shown + at, sizeof(shown) - at, fmt, ap);
	va_end(ap);
}
int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data) { (void)file; (void)show; (void)data; return 0; }
ssize_t seq_read(struct file *file, char *buf, size_t size, loff_t *pos) { (void)file; (void)buf; (void)size; (void)pos; return 0; }
loff_t seq_lseek(struct file *file, loff_t offset, int whence) { (void)file; (void)offset; (void)whence; return 0; }
int single_release(struct inode *inode, struct file *file) { (void)inode; (void)file; return 0; }
struct proc_dir_entry *proc_create(const char *name, unsigned int mode, struct proc_dir_entry *parent, const struct proc_ops *ops)
{
	(void)parent;
	proc_made = strcmp(name, "driver/nvidia/geforce-sriov-fb") == 0 && mode == 0600;
	proc_ops_seen = ops;
	return (struct proc_dir_entry *)&proc_made;
}
void remove_proc_entry(const char *name, struct proc_dir_entry *parent) { (void)name; (void)parent; proc_made = 0; }

/* ---- the model of the RM ---------------------------------------------------------------------- */

#define FGPUS 3
#define FB_SIZE 0x4000000ull /* 64 MiB per GPU */
static nv_linux_state_t fgpu[FGPUS] = {
	{ { { 0, 3, 0, 0 }, 0x300, 0 }, &fgpu[1] }, /* 0000:03:00.0 */
	{ { { 0, 4, 0, 0 }, 0x400, 0 }, &fgpu[2] }, /* 0000:04:00.0 */
	{ { { 0, 5, 0, 0 }, 0x500, 0 }, NULL },     /* 0000:05:00.0 */
};
nv_linux_state_t *nv_linux_devices = &fgpu[0];

static struct { int used, gpu, refs; NvU64 off, size; } falloc[64];
static struct { int used; NvU32 client, handle, parent, cls; int gpu, alloc; } fobj[8192];
static NvU32 next_client = 0xc1d00001u;
static int fail_dup, place_elsewhere, rm_allocs;

static int range_taken(int gpu, NvU64 off, NvU64 size)
{
	for (unsigned int i = 0; i < 64; i++)
		if (falloc[i].used && falloc[i].gpu == gpu && off < falloc[i].off + falloc[i].size && falloc[i].off < off + size)
			return 1;
	return 0;
}
static int find_obj(NvU32 client, NvU32 handle)
{
	for (int i = 0; i < (int)(sizeof(fobj) / sizeof(fobj[0])); i++)
		if (fobj[i].used && fobj[i].client == client && fobj[i].handle == handle)
			return i;
	return -1;
}
static int new_obj(NvU32 client, NvU32 handle, NvU32 parent, NvU32 cls, int gpu, int alloc)
{
	for (int i = 0; i < (int)(sizeof(fobj) / sizeof(fobj[0])); i++)
		if (!fobj[i].used) {
			fobj[i].used = 1;
			fobj[i].client = client;
			fobj[i].handle = handle;
			fobj[i].parent = parent;
			fobj[i].cls = cls;
			fobj[i].gpu = gpu;
			fobj[i].alloc = alloc;
			return i;
		}
	return -1;
}
static void free_obj(int i)
{
	if (fobj[i].cls == 0x40 && --falloc[fobj[i].alloc].refs == 0)
		falloc[fobj[i].alloc].used = 0;
	fobj[i].used = 0;
	for (int k = 0; k < (int)(sizeof(fobj) / sizeof(fobj[0])); k++)
		if (fobj[k].used && fobj[k].client == fobj[i].client && (fobj[k].parent == fobj[i].handle || fobj[i].cls == 0))
			free_obj(k);
}
/* Class 0x40 under a device: at the fixed address, or the lowest free place. */
static NvU32 rm_memory(NvU32 client, NvU32 parent, NvU32 handle, gs_fb_mem_t *p)
{
	int dev = find_obj(client, parent), a = -1;
	NvU64 off = p->offset, align = p->alignment ? p->alignment : 0x1000;

	rm_allocs++;
	if (dev < 0 || fobj[dev].cls != 0x80 || !p->size)
		return 0x1f;
	if (find_obj(client, handle) >= 0)
		return 0x30;
	if (p->flags & 0x10) {
		if (place_elsewhere)
			off += 0x100000;
		if (off + p->size > FB_SIZE || range_taken(fobj[dev].gpu, off, p->size))
			return 0x51;
	} else {
		for (off = 0; off + p->size <= FB_SIZE && range_taken(fobj[dev].gpu, off, p->size); off += align)
			;
		if (off + p->size > FB_SIZE)
			return 0x51;
	}
	for (int i = 0; i < 64 && a < 0; i++)
		if (!falloc[i].used)
			a = i;
	falloc[a].used = 1;
	falloc[a].gpu = fobj[dev].gpu;
	falloc[a].refs = 1;
	falloc[a].off = off;
	falloc[a].size = p->size;
	new_obj(client, handle, parent, 0x40, fobj[dev].gpu, a);
	p->offset = off;
	p->limit = p->size - 1;
	/* The reply names what the object is: the owner's is uncompressed (format 6), the plugin's own is not (9). */
	p->format = (p->attr & 0x3000u) ? 9 : 6;
	p->attr |= 0x01000000u;
	return NV_OK;
}
static NvU32 rm_alloc(NvU32 *hRoot, NvU32 parent, NvU32 *hNew, NvU32 cls, void *parms, int kernel)
{
	if (cls == 0x0 || cls == 0x1 || cls == 0x41) {
		NvU32 h = next_client++;

		new_obj(h, h, 0, 0, -1, -1);
		*hNew = h;
		if (kernel && parms)
			*(NvU32 *)parms = h;
		return NV_OK;
	}
	if (find_obj(*hRoot, *hRoot) < 0)
		return 0x23;
	if (cls == 0x80) {
		NvU32 id = parms ? ((gs_fb_dev_t *)parms)->deviceId : 0;

		if (id >= FGPUS || find_obj(*hRoot, *hNew) >= 0)
			return 0x1f;
		new_obj(*hRoot, *hNew, *hRoot, 0x80, (int)id, -1);
		return NV_OK;
	}
	if (cls == 0x40 && parms)
		return rm_memory(*hRoot, parent, *hNew, parms);
	return 0x1f;
}
static NvU32 rm_free(NvU32 client, NvU32 handle)
{
	int i = find_obj(client, handle);

	if (fail_free)
		return 0x65;
	/* As the real RM is built: a handle that does not exist is no error. */
	if (find_obj(client, client) < 0)
		return 0x23;
	if (i >= 0)
		free_obj(i);
	return NV_OK;
}
void rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd)
{
	gs_fb_op_t *op = ops_cmd;

	(void)sp;
	if (op->op == GS_FB_NV04_ALLOC) {
		gs_fb_nvos64_t *a = &op->params.alloc;

		a->status = rm_alloc(&a->hRoot, a->hObjectParent, &a->hObjectNew, a->hClass, (void *)(uintptr_t)a->pAllocParms, 1);
	} else if (op->op == GS_FB_NV01_FREE) {
		op->params.free.status = rm_free(op->params.free.hRoot, op->params.free.hObjectOld);
	} else if (op->op == GS_FB_NV04_CONTROL) {
		gs_fb_idinfo_t *id = (void *)(uintptr_t)op->params.control.params;

		op->params.control.status = 0x1f;
		for (NvU32 i = 0; op->params.control.cmd == 0x202 && i < FGPUS; i++)
			if (fgpu[i].nv_state.gpu_id == id->gpuId) {
				id->deviceInstance = i;
				op->params.control.status = NV_OK;
			}
	} else if (op->op == GS_FB_NV04_DUP_OBJECT) {
		gs_fb_nvos55_t *d = &op->params.dup;
		int src = find_obj(d->hClientSrc, d->hObjectSrc), par = find_obj(d->hClient, d->hParent);

		if (fail_dup || src < 0 || par < 0 || fobj[par].cls != 0x80 || fobj[par].gpu != fobj[src].gpu ||
		    find_obj(d->hClient, d->hObject) >= 0) {
			d->status = 0x1b;
		} else {
			new_obj(d->hClient, d->hObject, d->hParent, 0x40, fobj[src].gpu, fobj[src].alloc);
			falloc[fobj[src].alloc].refs++;
			d->status = NV_OK;
		}
	}
}
int nvidia_dev_get(NvU32 gpu_id, nvidia_stack_t *sp) { (void)gpu_id; (void)sp; if (fail_dev_get) return -5; dev_gets++; return 0; }
void nvidia_dev_put(NvU32 gpu_id, nvidia_stack_t *sp) { (void)gpu_id; (void)sp; dev_puts++; }

/* ---- a program on a control file --------------------------------------------------------------- */

struct prog {
	nv_linux_file_private_t file;
	struct task_struct *task;
	NvU32 client;
};
static int answered_by_owner;
static NvU32 *swap_after_rm, swap_value;

/* nvidia_ioctl()'s default arm, with the model in the place of rm_ioctl(). */
static void user_ioctl(struct prog *p, int cmd, void *arg, size_t size)
{
	struct task_struct *before = current;

	NvU32 seen = 0;

	current = p->task;
	answered_by_owner = gs_fb_ioctl_pre(&p->file, cmd, arg, size, &seen);
	if (!answered_by_owner) {
		if (cmd == GS_FB_ESC_RM_ALLOC && size == sizeof(gs_fb_nvos64_t)) {
			gs_fb_nvos64_t *a = arg;

			a->status = rm_alloc(&a->hRoot, a->hObjectParent, &a->hObjectNew, a->hClass, (void *)(uintptr_t)a->pAllocParms, 0);
		} else if (cmd == GS_FB_ESC_RM_ALLOC) {
			gs_fb_nvos21_t *a = arg;

			a->status = rm_alloc(&a->hRoot, a->hObjectParent, &a->hObjectNew, a->hClass, (void *)(uintptr_t)a->pAllocParms, 0);
		} else if (cmd == GS_FB_ESC_RM_FREE) {
			gs_fb_nvos00_t *f = arg;

			f->status = rm_free(f->hRoot, f->hObjectOld);
		} else if (cmd == GS_FB_ESC_RM_VID_HEAP_CONTROL) {
			NvU8 *h = arg;
			gs_fb_mem_t m = { 0 };
			NvU32 root, parent, status;

			memcpy(&root, h, 4);
			memcpy(&parent, h + 4, 4);
			memcpy(&m.flags, h + GS_FB_NVOS32_FLAGS_AT, 4);
			memcpy(&m.size, h + GS_FB_NVOS32_ALLOC_SIZE_SIZE_AT, 8);
			memcpy(&m.offset, h + GS_FB_NVOS32_ALLOC_SIZE_OFFSET_AT, 8);
			m.attr = 0x3000;
			status = rm_memory(root, parent, 0xcaf00000u + (NvU32)rm_allocs, &m);
			memcpy(h + GS_FB_NVOS32_STATUS_AT, &status, 4);
		}
		/* A caller that changes its parameters while the RM works on them. */
		if (swap_after_rm)
			*swap_after_rm = swap_value;
		gs_fb_ioctl_post(&p->file, cmd, arg, size, NV_OK, seen);
	}
	current = before;
}
static void prog_start(struct prog *p, struct task_struct *task, NvU32 gpu)
{
	gs_fb_nvos64_t a = { .hClass = 0x41, .status = 0xffffffffu };
	gs_fb_dev_t d = { .deviceId = gpu };

	memset(p, 0, sizeof(*p));
	p->task = task;
	user_ioctl(p, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	p->client = a.hObjectNew;
	a = (gs_fb_nvos64_t){ .hRoot = p->client, .hObjectParent = p->client, .hObjectNew = 1, .hClass = 0x80,
			      .pAllocParms = (uintptr_t)&d, .status = 0xffffffffu };
	user_ioctl(p, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
}
static gs_fb_mem_t last;
/* The plugin's request (class 0x40, the given flags), or a host program's when flags lack the fixed bit. */
static NvU32 prog_alloc(struct prog *p, NvU32 handle, NvU64 off, NvU64 size, NvU32 flags)
{
	gs_fb_nvos64_t a = { .hRoot = p->client, .hObjectParent = 1, .hObjectNew = handle, .hClass = 0x40,
			     .pAllocParms = (uintptr_t)&last, .status = 0xffffffffu };

	last = (gs_fb_mem_t){ .owner = p->client, .flags = flags, .attr = 0x50003004, .attr2 = 0xa0001, .size = size,
			      .alignment = 0x100000, .offset = off };
	user_ioctl(p, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	return a.status;
}
static NvU32 prog_alloc21(struct prog *p, NvU32 handle, NvU64 off, NvU64 size)
{
	gs_fb_nvos21_t a = { .hRoot = p->client, .hObjectParent = 1, .hObjectNew = handle, .hClass = 0x40,
			     .pAllocParms = (uintptr_t)&last, .status = 0xffffffffu };

	last = (gs_fb_mem_t){ .owner = p->client, .flags = 0x19110, .attr = 0x50003004, .attr2 = 0xa0001, .size = size,
			      .alignment = 0x100000, .offset = off };
	user_ioctl(p, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	return a.status;
}
static NvU32 prog_heap(struct prog *p, NvU64 off, NvU64 size)
{
	NvU8 h[GS_FB_NVOS32_SIZE] = { 0 };
	NvU32 v = p->client, status;

	memcpy(h, &v, 4);
	v = 1;
	memcpy(h + 4, &v, 4);
	v = GS_FB_NVOS32_ALLOC_SIZE;
	memcpy(h + GS_FB_NVOS32_FUNCTION_AT, &v, 4);
	v = 0x19110;
	memcpy(h + GS_FB_NVOS32_FLAGS_AT, &v, 4);
	memcpy(h + GS_FB_NVOS32_ALLOC_SIZE_SIZE_AT, &size, 8);
	memcpy(h + GS_FB_NVOS32_ALLOC_SIZE_OFFSET_AT, &off, 8);
	user_ioctl(p, GS_FB_ESC_RM_VID_HEAP_CONTROL, h, sizeof(h));
	memcpy(&status, h + GS_FB_NVOS32_STATUS_AT, 4);
	return status;
}
static NvU32 prog_free(struct prog *p, NvU32 handle)
{
	gs_fb_nvos00_t f = { .hRoot = p->client, .hObjectParent = 1, .hObjectOld = handle, .status = 0xffffffffu };

	user_ioctl(p, GS_FB_ESC_RM_FREE, &f, sizeof(f));
	return f.status;
}
static void prog_close(struct prog *p)
{
	struct task_struct *before = current;

	current = p->task;
	gs_fb_file_closed(&p->file);
	current = before;
	if (find_obj(p->client, p->client) >= 0)
		free_obj(find_obj(p->client, p->client));
}

/* ---- the trigger -------------------------------------------------------------------------------- */

static long command(const char *text)
{
	loff_t pos = 0;

	return (long)gs_fb_proc_write(NULL, text, strlen(text), &pos);
}
static const char *state(void)
{
	shown[0] = '\0';
	gs_fb_show(NULL, NULL);
	return shown;
}
static int has(const char *text, const char *part) { return strstr(text, part) != NULL; }

#define S 0x400000ull /* a slot of 4 MiB */
static void fresh(void)
{
	gs_fb_exit();
	memset(falloc, 0, sizeof(falloc));
	memset(fobj, 0, sizeof(fobj));
	memset(gs_fb_tracked, 0, sizeof(gs_fb_tracked));
	gs_fb_track_seq = 0;
	gs_fb_track_used = gs_fb_track_dropped = gs_fb_track_skipped = 0;
	klog[0] = '\0';
	is_root = 1;
	CHECK(stacks == 0, "no stack is kept while no GPU is open: %d", stacks);
	fail_stack = fault_to_user = fault_from_user = fail_dev_get = fail_dup = place_elsewhere = fail_free = 0;
	dev_gets = dev_puts = rm_allocs = 0;
	next_client = 0xc1d00001u;
	gs_fb_refused_at = gs_fb_nomem_at = 0;
	current = &host_task;
	gs_fb_init();
}
static void hold_four(void)
{
	CHECK(command("hold 0000:03:00.0 0 400000 100000\n") > 0 && command("hold 0000:03:00.0 400000 400000 100000\n") > 0 &&
	      command("hold 0000:03:00.0 800000 400000 100000\n") > 0 && command("hold 0000:03:00.0 c00000 400000 100000\n") > 0,
	      "four slots are held");
}

static void test_trigger(void)
{
	static const char *const bad[] = {
		"", "hold", "hold 0000:03:00.0", "hold 0000:03:00.0 0 400000", "hold 0000:03:00.0 0 400000 100000 1",
		"hold 0000:03:00.0 0 0 100000", "hold 0000:03:00.0 0 400001 100000", "hold 0000:03:00.0 1 400000 100000",
		"hold 0000:03:00.0 0 400000 0", "hold 0000:03:00.0 0 400000 300000", "hold 0000:03:00.0 fffffffffffff000 2000 1000",
		"hold 0000:03:00.0 zz 400000 100000", "HOLD 0000:03:00.0 0 400000 100000", "release", "release 0000:03:00.0 now",
		"manager /usr/bin/nvidia-vgpu-mgr", "hold 0000:03:00.0 0 400000 100000\nhold 0000:03:00.0 400000 400000 100000",
	};
	char longer[400];

	fresh();
	CHECK(proc_made && proc_ops_seen == &gs_fb_proc_ops, "the file is /proc/driver/nvidia/geforce-sriov-fb, mode 0600");
	CHECK(strcmp(state(), "abi 1\ntracking 0 of 2048 files 0 most-on-one-file 0 dropped 0 devices-not-recorded 0\n") == 0, "nothing is held at first: %s", shown);
	for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		long rc = bad[i][0] ? command(bad[i]) : (long)gs_fb_proc_write(NULL, "", 0, NULL);

		CHECK(rc == -EINVAL, "\"%s\" is refused: %ld", bad[i], rc);
	}
	memset(longer, 'a', sizeof(longer) - 1);
	longer[sizeof(longer) - 1] = '\0';
	CHECK(command(longer) == -EINVAL, "a command that is too long is refused");
	CHECK(command("hold 0000:09:00.0 0 400000 100000") == -ENODEV, "a GPU the driver does not have is refused");
	CHECK(strcmp(state(), "abi 1\ntracking 0 of 2048 files 0 most-on-one-file 0 dropped 0 devices-not-recorded 0\n") == 0 && dev_gets == 0 && rm_allocs == 0, "none of them changed anything: %s", shown);
	is_root = 0;
	CHECK(command("hold 0000:03:00.0 0 400000 100000") == -EPERM && gs_fb_proc_open(NULL, NULL) == -EPERM,
	      "without CAP_SYS_ADMIN the file can be neither written nor opened");
	is_root = 1;
	fault_from_user = 1;
	CHECK(command("release all") == -EFAULT, "a command that cannot be read is refused");
	fault_from_user = 0;
	fail_dev_get = 1;
	CHECK(command("hold 0000:03:00.0 0 400000 100000") == -EIO && strcmp(state(), "abi 1\ntracking 0 of 2048 files 0 most-on-one-file 0 dropped 0 devices-not-recorded 0\n") == 0, "a GPU that cannot be opened: %s", shown);
	fail_dev_get = 0;
	fail_stack = 1;
	CHECK(command("hold 0000:03:00.0 0 400000 100000") == -ENOMEM && strcmp(state(), "abi 1\ntracking 0 of 2048 files 0 most-on-one-file 0 dropped 0 devices-not-recorded 0\n") == 0, "no stack: %s", shown);
	fail_stack = 0;
	CHECK(command("release all\n") > 0 && command("release 0000:03:00.0") > 0, "releasing what is not held is no error");
}

static void test_hold(void)
{
	struct prog host;

	fresh();
	hold_four();
	CHECK(dev_gets == 1 && gs_fb_held == 4, "the GPU is opened once: %d, %u held", dev_gets, gs_fb_held);
	CHECK(has(state(), "gpu 0000:03:00.0 device 0 client 0xc1d00001 slots 4\n") &&
	      has(shown, "slot 0000:03:00.0 0x0 0x400000 held dups 0 answered 0\n") &&
	      has(shown, "slot 0000:03:00.0 0xc00000 0x400000 held dups 0 answered 0\n"), "the state names them: %s", shown);
	CHECK(range_taken(0, 0, 4 * S) && !range_taken(0, 4 * S, S) && !range_taken(1, 0, S), "the RM has the four ranges taken, and no other");
	rm_allocs = 0;
	CHECK(command("hold 0000:03:00.0 400000 400000 100000") > 0 && rm_allocs == 0 && gs_fb_held == 4, "the same slot again is the same slot");
	CHECK(command("hold 0000:03:00.0 600000 400000 100000") == -EEXIST && gs_fb_held == 4, "a range that overlaps a held slot is refused");
	/* A host program: a fixed request for a slot is refused by the RM, and one without an address lands above. */
	prog_start(&host, &host_task, 0);
	CHECK(prog_alloc(&host, 0x10, S, S, 0x19110) == 0x51 && !answered_by_owner && gs_fb_held == 4,
	      "a host program that asks for a held slot gets \"no memory\", and the slot stays held");
	CHECK(has(klog, "python3[200] asks for 0x400000 size 0x400000, which is held: left to the RM, which refuses it"), "that is logged: %s", klog);
	CHECK(prog_alloc(&host, 0x11, 0, S, 0x19100) == NV_OK && last.offset == 4 * S, "a host allocation lands above the slots: 0x%llx",
	      (unsigned long long)last.offset);
	/* Its range is then in use: a hold of it fails and says so, and a later one succeeds. */
	CHECK(command("hold 0000:03:00.0 1000000 400000 100000") > 0 && has(state(), "slot 0000:03:00.0 0x1000000 0x400000 failed 0x51\n") &&
	      gs_fb_held == 4, "a range that is in use is not held: %s", shown);
	prog_free(&host, 0x11);
	CHECK(command("hold 0000:03:00.0 1000000 400000 100000") > 0 && has(state(), "slot 0000:03:00.0 0x1000000 0x400000 held") &&
	      gs_fb_held == 5, "held once it is free: %s", shown);
	place_elsewhere = 1;
	CHECK(command("hold 0000:03:00.0 2000000 400000 100000") > 0 && has(state(), "slot 0000:03:00.0 0x2000000 0x400000 failed 0xffffffff\n") &&
	      !range_taken(0, 0x2000000, 0x800000), "a range the RM places elsewhere is not a slot, and is freed: %s", shown);
	/* The same, and the RM does not take the misplaced object back: the state shows what is held, where it is. */
	fail_free = 1;
	CHECK(command("hold 0000:03:00.0 2800000 400000 100000") > 0 && has(state(), "slot 0000:03:00.0 0x2900000 0x400000 held") &&
	      range_taken(0, 0x2900000, S), "a misplaced range that cannot be freed is shown as held, at its real place: %s", shown);
	fail_free = 0;
	place_elsewhere = 0;
	prog_close(&host);
}

static void test_answer(void)
{
	struct prog mgr;

	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	rm_allocs = 0;
	CHECK(prog_alloc(&mgr, 0x100, S, S, 0x19110) == NV_OK && answered_by_owner && rm_allocs == 0,
	      "the manager's request for a slot is answered, and the RM allocates nothing");
	CHECK(last.offset == S && last.size == S && last.limit == S - 1 && last.format == 6 && last.attr == 0x51000004 &&
	      last.flags == 0x19110 && last.owner == mgr.client && last.alignment == 0x100000,
	      "the reply is the held allocation's, and the manager's inputs stay: attr 0x%x format %u", last.attr, last.format);
	CHECK(find_obj(mgr.client, 0x100) >= 0 && falloc[fobj[find_obj(mgr.client, 0x100)].alloc].refs == 2,
	      "the manager has the object at its own handle, as a second reference");
	CHECK(has(state(), "slot 0000:03:00.0 0x400000 0x400000 held dups 1 answered 1\n"), "the state counts it: %s", shown);
	CHECK(prog_free(&mgr, 0x100) == NV_OK && has(state(), "0x400000 0x400000 held dups 0 answered 1\n") && range_taken(0, S, S),
	      "the guest stops: the duplicate goes, the range stays the owner's: %s", shown);
	CHECK(prog_alloc21(&mgr, 0x100, S, S) == NV_OK && answered_by_owner && has(state(), "0x400000 0x400000 held dups 1 answered 2\n"),
	      "the next start is answered again, in the other form of the request: %s", shown);
	/* The request may come from any thread of the manager. */
	mgr.task = &thread_of_manager;
	CHECK(prog_alloc(&mgr, 0x101, 2 * S, S, 0x19110) == NV_OK && answered_by_owner, "a thread of the manager under another name");
	mgr.task = &manager_task;
	/*
	 * A slot is handed out once. A second request while its duplicate exists gets none: the owner
	 * lets go, the RM refuses the request because the guest's duplicate has the memory, and the
	 * guest keeps running on it.
	 */
	CHECK(prog_alloc(&mgr, 0x200, 3 * S, S, 0x19110) == NV_OK && answered_by_owner, "a guest starts on the last slot");
	CHECK(prog_alloc(&mgr, 0x201, 3 * S, S, 0x19110) == 0x51 && !answered_by_owner && find_obj(mgr.client, 0x201) < 0 &&
	      has(state(), "0xc00000 0x400000 given-up the manager asked for the slot while a duplicate of it is recorded\n") &&
	      range_taken(0, 3 * S, S) && falloc[fobj[find_obj(mgr.client, 0x200)].alloc].refs == 1,
	      "a second request for a slot that a guest runs on gets no duplicate: %s", shown);
	/* The manager dies: its file is closed, and what was recorded for it goes. */
	prog_close(&mgr);
	CHECK(has(state(), "0x400000 0x400000 held dups 0 answered 2\n") && has(shown, "0x800000 0x400000 held dups 0 answered 1\n") &&
	      gs_fb_held == 3 && range_taken(0, 0, 3 * S),
	      "the file is closed: no duplicate is counted, every slot that was held stays held: %s", shown);
	for (unsigned int i = 0; i < GS_FB_MAX_TRACKED; i++)
		CHECK(!gs_fb_tracked[i].used, "nothing stays recorded for a closed file");
}

/* One request against the table as it is, with the caller's facts given: the rows of gs_fb_decide(). */
static struct gs_fb_verdict decide(enum gs_fb_form form, NvU64 off, NvU64 size, NvU32 flags, NvU32 handle, int manager, int gpu)
{
	struct gs_fb_req r = { .form = form, .hRoot = 1, .hParent = 1, .hNew = handle, .flags = flags, .offset = off, .size = size };
	struct gs_fb_verdict v;

	gs_fb_decide(&r, manager, gpu, &v);
	return v;
}
static void test_decide(void)
{
	static const NvU32 bits[] = { 0x1, 0x2, 0x4, 0x8, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x8000, 0x10000, 0x20000, 0x80000000u };
	struct gs_fb_verdict v;

	fresh();
	hold_four();
	CHECK(command("hold 0000:04:00.0 0 400000 100000") > 0 && command("hold 0000:04:00.0 400000 400000 100000") > 0, "two slots on a second GPU");
	/* 1: no overlap, also right at the edges. */
	v = decide(GS_FB_FORM_NVOS64, 4 * S, S, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_PASS && !v.refused, "1: a range next to the last slot");
	v = decide(GS_FB_FORM_NVOS64, 4 * S, 0, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_PASS, "1: a request of no size");
	/* 2: not the manager. */
	v = decide(GS_FB_FORM_NVOS64, S, S, 0x19110, 0x100, 0, GS_FB_GPU_UNTRACKED);
	CHECK(v.what == GS_FB_PASS && v.refused, "2: another program, exact range: left to the RM");
	v = decide(GS_FB_FORM_HEAP, 4 * S - 0x1000, 0x2000, 0x19110, 0, 0, GS_FB_GPU_UNTRACKED);
	CHECK(v.what == GS_FB_PASS && v.refused, "2: another program, one page of a slot");
	/* 3: the manager with a client that was not seen. */
	v = decide(GS_FB_FORM_NVOS64, 0, S, 0x19110, 0x100, 1, GS_FB_GPU_UNTRACKED);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x1 && v.cede[1] == 0x1, "3: unknown client: the range is given up on every GPU: %x %x", v.cede[0], v.cede[1]);
	/* 4: a device of a GPU on which the range overlaps nothing. */
	v = decide(GS_FB_FORM_NVOS64, 2 * S, S, 0x19110, 0x100, 1, 1);
	CHECK(v.what == GS_FB_PASS && !v.cede[0] && !v.cede[1], "4: the range is a slot of another GPU only");
	v = decide(GS_FB_FORM_NVOS64, 0, S, 0x19110, 0x100, 1, GS_FB_GPU_OTHER);
	CHECK(v.what == GS_FB_PASS && !v.cede[0], "4: a device of a GPU with nothing held");
	/* 5: not exactly one slot. */
	v = decide(GS_FB_FORM_NVOS64, S - 0x1000, S, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x3 && !v.cede[1], "5: shifted by one page: both slots it touches, on that GPU only: %x", v.cede[0]);
	v = decide(GS_FB_FORM_NVOS64, S, S + 0x1000, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x6, "5: one page longer: %x", v.cede[0]);
	v = decide(GS_FB_FORM_NVOS64, S, S - 0x1000, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x2, "5: one page shorter: %x", v.cede[0]);
	v = decide(GS_FB_FORM_NVOS64, 0, 4 * S, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0xf, "5: all four slots in one request: %x", v.cede[0]);
	/* 6: exact, but not the plugin's request. */
	v = decide(GS_FB_FORM_HEAP, S, S, 0x19110, 0, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x2 && has(v.why, "heap control"), "6: through the heap control");
	v = decide(GS_FB_FORM_NVOS64, S, S, 0x19110, 0, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x2 && has(v.why, "no handle"), "6: no handle");
	for (unsigned int i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
		v = decide(GS_FB_FORM_NVOS64, S, S, 0x19110 ^ bits[i], 0x100, 1, 0);
		CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x2 && has(v.why, "other flags"), "6: flags 0x%x", 0x19110 ^ bits[i]);
	}
	/* 7: a slot whose duplicate is out is not handed out again. */
	gs_fb_gpus[0].slot[1].dups = 1;
	v = decide(GS_FB_FORM_NVOS64, S, S, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_CEDE && v.cede[0] == 0x2 && has(v.why, "while a duplicate of it is recorded"), "7: a duplicate is recorded");
	v = decide(GS_FB_FORM_NVOS64, S, S, 0x19110, 0x100, 0, GS_FB_GPU_UNTRACKED);
	CHECK(v.what == GS_FB_PASS && v.refused, "7: and anyone else is still left to the RM");
	gs_fb_gpus[0].slot[1].dups = 0;
	/* 8: the plugin's request, in both forms, on the GPU its device names. */
	v = decide(GS_FB_FORM_NVOS64, S, S, 0x19110, 0x100, 1, 0);
	CHECK(v.what == GS_FB_ANSWER && v.gpu == 0 && v.slot == 1 && !v.cede[0], "8: answered from GPU 0");
	v = decide(GS_FB_FORM_NVOS21, S, S, 0x19110, 0x100, 1, 1);
	CHECK(v.what == GS_FB_ANSWER && v.gpu == 1 && v.slot == 1 && !v.cede[0] && !v.cede[1], "8: the same range, answered from GPU 1");
	CHECK(gs_fb_held == 6, "deciding changes nothing");
}

static void test_cede(void)
{
	struct prog mgr, other, early;
	gs_fb_dev_t d = { .deviceId = 0 };
	gs_fb_nvos64_t a;
	NvU32 unused;

	/* Each case: the owner lets go, and the manager's own call then succeeds as it does without the owner. */
	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	CHECK(prog_alloc(&mgr, 0x100, S / 2, S, 0x19110) == NV_OK && !answered_by_owner && last.format == 9,
	      "a range that is not a slot: the manager's own allocation succeeds");
	CHECK(has(state(), "slot 0000:03:00.0 0x0 0x400000 given-up the manager's request overlaps the slot and is not equal to it\n") &&
	      has(shown, "slot 0000:03:00.0 0x400000 0x400000 given-up ") && has(shown, "0x800000 0x400000 held") && gs_fb_held == 2,
	      "both slots it touched are given up, with the reason; the others stay: %s", shown);
	prog_free(&mgr, 0x100);
	CHECK(gs_fb_held == 2 && !range_taken(0, 0, 2 * S), "a slot that was given up is not taken again by itself");
	CHECK(command("hold 0000:03:00.0 0 400000 100000") > 0 && command("hold 0000:03:00.0 400000 400000 100000") > 0 && gs_fb_held == 4,
	      "the next activation holds it again");
	CHECK(prog_alloc(&mgr, 0x100, 2 * S, S, 0x19130) == NV_OK && !answered_by_owner &&
	      has(state(), "0x800000 0x400000 given-up the manager's request has other flags than the plugin's\n"), "other flags: %s", shown);
	CHECK(prog_heap(&mgr, 3 * S, S) == NV_OK && !answered_by_owner &&
	      has(state(), "0xc00000 0x400000 given-up the manager asked for the slot through the heap control\n"), "the heap control: %s", shown);
	CHECK(prog_alloc(&mgr, 0, 0, S, 0x19110) == NV_OK && !answered_by_owner &&
	      has(state(), "0x0 0x400000 given-up the manager's request names no handle\n"), "no handle: %s", shown);
	fail_dup = 1;
	CHECK(prog_alloc(&mgr, 0x102, S, S, 0x19110) == NV_OK && !answered_by_owner && last.format == 9 &&
	      has(state(), "0x400000 0x400000 given-up the duplicate for the manager failed\n"), "the duplicate fails: %s", shown);
	fail_dup = 0;
	CHECK(gs_fb_held == 0, "nothing is held any more");
	prog_close(&mgr);

	/* The reply cannot be written: the duplicate is taken back before the slot is given up. */
	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	fault_to_user = 1;
	CHECK(prog_alloc(&mgr, 0x100, S, S, 0x19110) == NV_OK && !answered_by_owner && falloc[fobj[find_obj(mgr.client, 0x100)].alloc].refs == 1 &&
	      has(state(), "given-up the reply could not be written to the manager\n"), "the reply cannot be written: %s", shown);
	fault_to_user = 0;
	/*
	 * No memory for a stack is no obstacle once a GPU is open: the one stack of the owner was made
	 * with the first hold, and every later call into the RM runs on it.
	 */
	fail_stack = 1;
	CHECK(prog_alloc(&mgr, 0x101, 2 * S, S, 0x19110) == NV_OK && answered_by_owner && stacks == 1, "no memory for a stack: still answered");
	fail_stack = 0;
	/*
	 * The RM does not free the owner's object: the range is still taken, so the slot is still
	 * reported as held, and the log says why. It is not shown as given up while it is not.
	 */
	fail_free = 1;
	CHECK(prog_alloc(&mgr, 0x104, 3 * S, S, 0x19130) == 0x51 && !answered_by_owner && has(state(), "0xc00000 0x400000 held") &&
	      has(klog, "slot 0xc00000 could not be freed (status 0x65); it stays held until \"release\""),
	      "a free that the RM refuses: the slot is reported as it is: %s", shown);
	fail_free = 0;
	/* A request whose parameters cannot be read is not looked at; the RM cannot read them either. */
	fault_from_user = 1;
	CHECK(!gs_fb_ioctl_pre(&mgr.file, GS_FB_ESC_RM_ALLOC, &(gs_fb_nvos64_t){ .hClass = 0x40, .pAllocParms = 1 }, sizeof(gs_fb_nvos64_t), &unused) &&
	      gs_fb_held == 3, "parameters that cannot be read");
	fault_from_user = 0;

	/* A client the owner did not see being made: another file with the manager's handles, and a client from before. */
	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	other = mgr;
	memset(&other.file, 0, sizeof(other.file));
	CHECK(prog_alloc(&other, 0x100, S, S, 0x19110) == NV_OK && !answered_by_owner &&
	      has(state(), "0x400000 0x400000 given-up the manager asked with a client that was not seen being made\n"),
	      "another file that names the manager's client gets no duplicate: %s", shown);
	memset(&early, 0, sizeof(early));
	early.task = &manager_task;
	early.client = next_client++;
	new_obj(early.client, early.client, 0, 0, -1, -1);
	new_obj(early.client, 1, early.client, 0x80, 0, -1);
	CHECK(prog_alloc(&early, 0x100, 2 * S, S, 0x19110) == NV_OK && !answered_by_owner && has(state(), "0x800000 0x400000 given-up"),
	      "a client from before the module could see it: %s", shown);
	/* A device that was freed and made again is known again; a client that was freed is not. */
	prog_free(&mgr, 1);
	CHECK(prog_alloc(&mgr, 0x103, 3 * S, S, 0x19110) != NV_OK || !answered_by_owner, "the manager's device was freed: no duplicate");
	prog_close(&mgr);

	/*
	 * Root in a container with the GPU's device files that takes the manager's name is not the
	 * manager: nothing of it is recorded, it gets no duplicate, and it cannot make a slot go.
	 */
	fresh();
	hold_four();
	prog_start(&other, &container_task, 0);
	for (unsigned int i = 0; i < GS_FB_MAX_TRACKED; i++)
		CHECK(!gs_fb_tracked[i].used, "a process in a container is not recorded as the manager");
	CHECK(prog_alloc(&other, 0x100, S, S, 0x19110) == 0x51 && !answered_by_owner && gs_fb_held == 4 &&
	      has(klog, "which is held: left to the RM, which refuses it"),
	      "a process named as the manager in a container: no duplicate, and the slot stays held");
	prog_close(&other);

	/*
	 * The device's GPU is read from the caller's memory. A caller that shows the RM one GPU and this
	 * code another is not recorded, so a request under that device gets no duplicate.
	 */
	fresh();
	hold_four();
	memset(&mgr, 0, sizeof(mgr));
	mgr.task = &manager_task;
	a = (gs_fb_nvos64_t){ .hClass = 0x41, .status = 0xffffffffu };
	user_ioctl(&mgr, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	mgr.client = a.hObjectNew;
	a = (gs_fb_nvos64_t){ .hRoot = mgr.client, .hObjectParent = mgr.client, .hObjectNew = 1, .hClass = 0x80,
			      .pAllocParms = (uintptr_t)&d, .status = 0xffffffffu };
	d.deviceId = 1;
	swap_after_rm = &d.deviceId;
	swap_value = 0;
	user_ioctl(&mgr, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	swap_after_rm = NULL;
	CHECK(a.status == NV_OK && !gs_fb_track_find(&mgr.file.nvfp, mgr.client, 1), "a device whose GPU changed during the call is not recorded");
	prog_close(&mgr);

	/* The line about a refused request is written at most once in ten seconds, and shows a name's printable characters only. */
	fresh();
	hold_four();
	prog_start(&other, &odd_task, 0);
	prog_alloc(&other, 0x10, S, S, 0x19110);
	prog_alloc(&other, 0x11, S, S, 0x19110);
	CHECK(has(klog, "ev?il?[300] asks for 0x400000") && strstr(strstr(klog, "which is held") + 1, "which is held") == NULL,
	      "two refusals in a row: one line, with the name made printable: %s", klog);
	jiffies += 11 * HZ;
	prog_alloc(&other, 0x12, S, S, 0x19110);
	CHECK(strstr(strstr(klog, "which is held") + 1, "which is held") != NULL, "ten seconds later: another line");
	prog_close(&other);

	/* A GPU that is being removed from the driver is not opened again. */
	fresh();
	fgpu[0].nv_state.flags = NV_FLAG_PCI_REMOVE_IN_PROGRESS;
	CHECK(command("hold 0000:03:00.0 0 400000 100000") == -ENODEV && dev_gets == 0, "a GPU in removal is not held on");
	fgpu[0].nv_state.flags = 0;

	/* A device file registered to the control file speaks for the same client. */
	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	other = mgr;
	memset(&other.file, 0, sizeof(other.file));
	other.file.nvfp.ctl_nvfp = &mgr.file.nvfp;
	CHECK(prog_alloc(&other, 0x100, S, S, 0x19110) == NV_OK && answered_by_owner, "a device file of the same control file is answered");
	prog_close(&mgr);
}

static void test_gpus(void)
{
	struct prog ada, ampere, third;

	fresh();
	hold_four();
	CHECK(command("hold 0000:04:00.0 0 400000 100000") > 0 && dev_gets == 2, "a slot on a second GPU, with a client of its own");
	CHECK(has(state(), "gpu 0000:04:00.0 device 1 client 0xc1d00002 slots 1\n"), "%s", shown);
	prog_start(&ada, &manager_task, 1);
	prog_start(&ampere, &manager_task, 2);
	prog_start(&third, &manager_task, 0);
	/* The same range on three GPUs: each request is about the GPU its device is of. */
	CHECK(prog_alloc(&ampere, 0x100, 0, S, 0x19110) == NV_OK && !answered_by_owner && last.format == 9 && gs_fb_held == 5,
	      "a GPU on which nothing is held: the manager's own allocation, and no slot is touched");
	CHECK(prog_alloc(&ada, 0x100, 0, S, 0x19110) == NV_OK && answered_by_owner &&
	      has(state(), "slot 0000:04:00.0 0x0 0x400000 held dups 1 answered 1\n") && has(shown, "slot 0000:03:00.0 0x0 0x400000 held dups 0 answered 0\n"),
	      "the second GPU's slot is answered from the second GPU: %s", shown);
	CHECK(prog_alloc(&ada, 0x101, S, S, 0x19110) == NV_OK && !answered_by_owner && gs_fb_held == 5,
	      "a range that is a slot on the first GPU only, asked for on the second: not this owner's business");
	/* One GPU is released: the other keeps its slots, and a duplicate keeps its memory. */
	CHECK(command("release 0000:04:00.0") > 0 && dev_puts == 1 && gs_fb_held == 4 && !has(state(), "0000:04:00.0") && range_taken(1, 0, S),
	      "release of one GPU: %s", shown);
	CHECK(command("hold 0000:04:00.0 0 400000 100000") > 0 && has(state(), "slot 0000:04:00.0 0x0 0x400000 failed 0x51\n"),
	      "a slot that a guest runs on is not held: %s", shown);
	/* The driver lets go of a GPU: what is held on it goes first. */
	gs_fb_gpu_remove(0x300);
	CHECK(gs_fb_held == 0 && !has(state(), "0000:03:00.0") && !range_taken(0, 0, 4 * S) && dev_puts == 2, "a GPU is removed: %s", shown);
	gs_fb_gpu_remove(0x900);
	hold_four();
	gs_fb_exit();
	CHECK(gs_fb_held == 0 && !range_taken(0, 0, 4 * S) && !proc_made && !has(state(), "gpu ") && !has(shown, "slot "), "the module is unloaded: nothing is left: %s", shown);
	gs_fb_init();
	prog_close(&ada);
	prog_close(&ampere);
	prog_close(&third);
}

static void test_net(void)
{
	struct prog mgr, host;
	gs_fb_nvos64_t a;

	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	prog_start(&host, &host_task, 0);
	CHECK(prog_alloc(&mgr, 0x100, 0, S, 0x19110) == NV_OK && answered_by_owner, "one guest runs");
	CHECK(prog_alloc(&host, 0x10, 0, S, 0x19100) == NV_OK && last.offset == 4 * S, "a host program has the range above the slots");
	/* The manager's fixed request for a range that no hold is in fails: no hold was the reason, and none goes. */
	CHECK(prog_alloc(&mgr, 0x101, 4 * S, S, 0x19110) == 0x51 && !answered_by_owner && gs_fb_held == 4 && !has(state(), "given-up"),
	      "a fixed request outside every slot fails: the holds stay: %s", shown);
	/* A fixed request that reaches the RM while its slot is held and comes back "no memory": that slot goes, and no other. */
	last = (gs_fb_mem_t){ .flags = 0x19110, .size = S, .offset = 2 * S };
	a = (gs_fb_nvos64_t){ .hRoot = mgr.client, .hObjectParent = 1, .hObjectNew = 0x102, .hClass = 0x40,
			      .pAllocParms = (uintptr_t)&last, .status = 0x51 };
	current = &manager_task;
	gs_fb_ioctl_post(&mgr.file, GS_FB_ESC_RM_ALLOC, &a, sizeof(a), NV_OK, 0);
	current = &host_task;
	CHECK(has(state(), "0x800000 0x400000 given-up the manager's request for it answered \"no memory\" while it was held\n") && gs_fb_held == 3,
	      "a fixed request for a held slot that the RM refused: that slot is given up: %s", shown);
	/*
	 * An allocation of video memory that is no fixed one: a line in the log, and every hold stays.
	 * Whoever fills the GPU's free memory can make it fail.
	 */
	CHECK(prog_alloc(&mgr, 0x103, 0, 61 * 0x100000ull, 0x19100) == 0x51 && gs_fb_held == 3 &&
	      has(state(), "0x400000 0x400000 held dups 0 answered 0\n") &&
	      has(klog, "that is not a fixed one answered \"no memory\" while slots are held; nothing is given up for it"),
	      "the manager's allocation without an address fails: the holds stay: %s", shown);
	/* A host program's "no memory" changes nothing, nor does the manager's for another class. */
	fresh();
	hold_four();
	prog_start(&mgr, &manager_task, 0);
	prog_start(&host, &host_task, 0);
	CHECK(prog_alloc(&host, 0x10, 0, 61 * 0x100000ull, 0x19100) == 0x51 && gs_fb_held == 4, "a host program's \"no memory\": the holds stay");
	a = (gs_fb_nvos64_t){ .hRoot = mgr.client, .hObjectParent = 1, .hObjectNew = 0x104, .hClass = 0x3e, .status = 0x51 };
	current = &manager_task;
	gs_fb_ioctl_post(&mgr.file, GS_FB_ESC_RM_ALLOC, &a, sizeof(a), NV_OK, 0);
	current = &host_task;
	CHECK(gs_fb_held == 4, "\"no memory\" for another class of the manager's: the holds stay");
	prog_close(&mgr);
	prog_close(&host);
}

/* One more client and device of a program, as the manager's processes make many. */
static void prog_more(struct prog *p, NvU32 device)
{
	gs_fb_nvos64_t a = { .hClass = 0x41, .status = 0xffffffffu };
	gs_fb_dev_t d = { .deviceId = 1 };
	NvU32 client;

	user_ioctl(p, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	client = a.hObjectNew;
	a = (gs_fb_nvos64_t){ .hRoot = client, .hObjectParent = client, .hObjectNew = device, .hClass = 0x80,
			      .pAllocParms = (uintptr_t)&d, .status = 0xffffffffu };
	user_ioctl(p, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
}

static void test_table(void)
{
	static struct prog vm[8];
	struct prog old, late;
	gs_fb_nvos64_t a;
	gs_fb_dev_t d = { .deviceId = 0 };

	/*
	 * What a host with eight VMs does: one manager process per VM, each with dozens of clients and
	 * devices that it keeps while its VM runs. Seven are running; the eighth starts. Its request must
	 * find its own client and device however many the others have recorded.
	 */
	fresh();
	hold_four();
	for (unsigned int i = 0; i < 7; i++) {
		prog_start(&vm[i], &manager_task, 1);
		for (unsigned int k = 0; k < 40; k++)
			prog_more(&vm[i], 1);
	}
	prog_start(&vm[7], &manager_task, 0);
	for (unsigned int k = 0; k < 40; k++)
		prog_more(&vm[7], 1);
	CHECK(gs_fb_track_used == 8 * 82 && has(state(), "tracking 656 of 2048 files 8 most-on-one-file 82 dropped 0 devices-not-recorded 0\n"),
	      "eight manager processes with 41 clients and devices each: %s", shown);
	CHECK(prog_alloc(&vm[7], 0x100, 3 * S, S, 0x19110) == NV_OK && answered_by_owner, "the eighth VM's start is answered");
	for (unsigned int i = 0; i < 8; i++)
		prog_close(&vm[i]);
	CHECK(gs_fb_track_used == 0 && has(state(), "tracking 0 of 2048 files 0 "), "every file closed: nothing stays recorded: %s", shown);

	/*
	 * A table that is full all the same drops its oldest record, so the VM that starts now is still
	 * answered. The owner of a dropped record gets no duplicate: its slot is given up, the reason
	 * says that records were dropped, and its start succeeds through the RM.
	 */
	fresh();
	hold_four();
	prog_start(&old, &manager_task, 0);
	for (unsigned int k = 0; k < GS_FB_MAX_TRACKED / 2; k++)
		prog_more(&old, 1);
	prog_start(&late, &manager_task, 0);
	CHECK(gs_fb_track_used == GS_FB_MAX_TRACKED && gs_fb_track_dropped == 4 && has(klog, "the record of the manager's clients is full (2048)"),
	      "the full table drops its oldest records: used %u dropped %u", gs_fb_track_used, gs_fb_track_dropped);
	CHECK(prog_alloc(&late, 0x100, 0, S, 0x19110) == NV_OK && answered_by_owner, "the program that came last is answered");
	CHECK(prog_alloc(&old, 0x100, S, S, 0x19110) == NV_OK && !answered_by_owner &&
	      has(state(), "0x400000 0x400000 given-up the manager asked with a client that was not seen being made (4 records were dropped: the table of 2048 was full)\n") &&
	      has(shown, "tracking 2048 of 2048 files 2 most-on-one-file 2046 dropped 4 "),
	      "the program whose record was dropped: its slot is given up, with the reason, and its start succeeds: %s", shown);
	prog_close(&old);
	prog_close(&late);

	/* Which record is missing is named. */
	fresh();
	hold_four();
	memset(&late, 0, sizeof(late));
	late.task = &manager_task;
	a = (gs_fb_nvos64_t){ .hClass = 0x41, .status = 0xffffffffu };
	user_ioctl(&late, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	late.client = a.hObjectNew;
	/* The device is made by a thread that is not the manager's: the RM has it, the record does not. */
	late.task = &host_task;
	a = (gs_fb_nvos64_t){ .hRoot = late.client, .hObjectParent = late.client, .hObjectNew = 1, .hClass = 0x80,
			      .pAllocParms = (uintptr_t)&d, .status = 0xffffffffu };
	user_ioctl(&late, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
	late.task = &manager_task;
	CHECK(prog_alloc(&late, 0x100, 2 * S, S, 0x19110) == NV_OK && !answered_by_owner &&
	      has(state(), "0x800000 0x400000 given-up the manager asked with a device that was not seen being made\n"),
	      "a device that is not recorded: named in the reason: %s", shown);
	prog_close(&late);

	/*
	 * A control file that outlives the processes that use it: the manager's daemon keeps it open, and
	 * the process of each VM makes its client on it. The file is never closed, so nothing but the
	 * frees can take the records away, and the client is freed by a process that does not pass as
	 * the manager. Twenty VM starts and stops leave nothing behind.
	 */
	fresh();
	hold_four();
	memset(&late, 0, sizeof(late));
	for (unsigned int round = 0; round < 20; round++) {
		gs_fb_nvos00_t f;

		late.task = &manager_task;
		a = (gs_fb_nvos64_t){ .hClass = 0x41, .status = 0xffffffffu };
		user_ioctl(&late, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
		late.client = a.hObjectNew;
		a = (gs_fb_nvos64_t){ .hRoot = late.client, .hObjectParent = late.client, .hObjectNew = 1, .hClass = 0x80,
				      .pAllocParms = (uintptr_t)&d, .status = 0xffffffffu };
		user_ioctl(&late, GS_FB_ESC_RM_ALLOC, &a, sizeof(a));
		CHECK(prog_alloc(&late, 0x100, 0, S, 0x19110) == NV_OK && answered_by_owner, "shared file, round %u: the start is answered", round);
		f = (gs_fb_nvos00_t){ .hRoot = late.client, .hObjectParent = 1, .hObjectOld = 0x100, .status = 0xffffffffu };
		user_ioctl(&late, GS_FB_ESC_RM_FREE, &f, sizeof(f));
		/* The VM's process is gone; something else frees its client on the file that is still open. */
		late.task = &host_task;
		f = (gs_fb_nvos00_t){ .hRoot = late.client, .hObjectOld = late.client, .status = 0xffffffffu };
		user_ioctl(&late, GS_FB_ESC_RM_FREE, &f, sizeof(f));
	}
	CHECK(gs_fb_track_used == 0 && has(state(), "tracking 0 of 2048 files 0 most-on-one-file 0 dropped 0 ") &&
	      has(shown, "0x0 0x400000 held dups 0 answered 20\n"),
	      "a file that stays open: twenty starts and stops leave no record behind: %s", shown);
	/* A free of another object by a program that is not the manager is none of this code's business. */
	prog_start(&old, &manager_task, 0);
	old.task = &host_task;
	{
		gs_fb_nvos00_t f = { .hRoot = old.client, .hObjectParent = old.client, .hObjectOld = 1, .status = 0xffffffffu };

		user_ioctl(&old, GS_FB_ESC_RM_FREE, &f, sizeof(f));
	}
	CHECK(gs_fb_track_used == 2, "another program's free of a device is not followed: %u", gs_fb_track_used);
	old.task = &manager_task;
	prog_close(&old);

	/* A host program's clients are not recorded at all. */
	fresh();
	prog_start(&late, &host_task, 0);
	CHECK(gs_fb_track_used == 0, "only the manager's clients are recorded");
	for (unsigned int i = 0; i < GS_FB_MAX_TRACKED; i++)
		CHECK(!gs_fb_tracked[i].used, "only the manager's clients are recorded");
}

int main(void)
{
	gs_fb_init();
	test_trigger();
	test_hold();
	test_answer();
	test_decide();
	test_cede();
	test_gpus();
	test_net();
	test_table();
	printf("test_fbowner: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
