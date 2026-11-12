/* SPDX-License-Identifier: MIT */
/*
 * What driver-patches/fb-owner/gs-fb-owner.c uses of the kernel and of NVIDIA's open layer, as
 * declarations for a userspace build: the fixtures compile the very file that goes into nvidia.ko
 * and run it against a model of the RM (test_fbowner.c). No kernel headers are needed for that.
 * It shows what the code does with what it is given; whether the kernel and NVIDIA's driver give
 * it that is seen on hardware only.
 */
#ifndef GS_FB_STUB_H
#define GS_FB_STUB_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

typedef uint8_t NvU8;
typedef uint32_t NvU32;
typedef int32_t NvS32;
typedef uint64_t NvU64;
typedef uint32_t NV_STATUS;
#define NV_OK 0u

typedef struct nvidia_stack_s nvidia_stack_t;
typedef struct {
	struct { NvU32 domain; NvU8 bus, slot, function; } pci_info;
	NvU32 gpu_id;
	NvU32 flags;
} nv_state_t;
#define NV_FLAG_PCI_REMOVE_IN_PROGRESS 0x2000
typedef struct nv_linux_state_s {
	nv_state_t nv_state;
	struct nv_linux_state_s *next;
} nv_linux_state_t;
typedef struct nv_file_private_t {
	struct nv_file_private_t *ctl_nvfp;
} nv_file_private_t;
typedef struct { nv_file_private_t nvfp; } nv_linux_file_private_t;
extern nv_linux_state_t *nv_linux_devices;
#define NV_STATE_PTR(nvl) (&(nvl)->nv_state)
#define LOCK_NV_LINUX_DEVICES() ((void)0)
#define UNLOCK_NV_LINUX_DEVICES() ((void)0)

int nv_kmem_cache_alloc_stack(nvidia_stack_t **sp);
void nv_kmem_cache_free_stack(nvidia_stack_t *sp);
void rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd);
int nvidia_dev_get(NvU32 gpu_id, nvidia_stack_t *sp);
void nvidia_dev_put(NvU32 gpu_id, nvidia_stack_t *sp);

struct mutex { int locked; };
#define DEFINE_MUTEX(name) struct mutex name
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);

#define KERN_INFO "<6>"
#define KERN_WARNING "<4>"
#define KERN_ERR "<3>"
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define __user
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
extern unsigned long jiffies;
#define HZ 100
#define time_after(a, b) ((long)((b) - (a)) < 0)
unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
#define CAP_SYS_ADMIN 21
int capable(int cap);
struct pid { unsigned int level; };
struct task_struct { char comm[16]; int pid; struct task_struct *group_leader; struct pid thread_pid; };
extern struct task_struct *current;
static inline struct pid *task_pid(struct task_struct *task) { return &task->thread_pid; }

struct inode;
struct file { void *private_data; };
struct seq_file { void *private; };
void seq_printf(struct seq_file *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data);
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *pos);
loff_t seq_lseek(struct file *file, loff_t offset, int whence);
int single_release(struct inode *inode, struct file *file);
struct proc_ops {
	int (*proc_open)(struct inode *, struct file *);
	ssize_t (*proc_read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*proc_write)(struct file *, const char __user *, size_t, loff_t *);
	loff_t (*proc_lseek)(struct file *, loff_t, int);
	int (*proc_release)(struct inode *, struct file *);
};
struct proc_dir_entry;
struct proc_dir_entry *proc_create(const char *name, unsigned int mode, struct proc_dir_entry *parent,
				   const struct proc_ops *ops);
void remove_proc_entry(const char *name, struct proc_dir_entry *parent);
#endif
