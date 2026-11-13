// SPDX-License-Identifier: MIT
/*
 * Behavioural tests for the identity hook, through a real LD_PRELOAD.
 *
 * usage: run_hook_tests HOOK_SO FAKE_DRIVER_SO NVIDIA_VGPUD NVIDIA_VGPU_MGR
 *
 * Each scenario describes a machine: its PCI functions (written as a fake sysfs tree) and the
 * GPUs the driver has registered (written as the fake driver's object table, one subdevice
 * handle, one device handle and one gpuId per GPU). It writes a config file and a driver
 * version file, runs the stand-in daemon (vgpud.c) with LD_PRELOAD="HOOK_SO FAKE_DRIVER_SO",
 * and compares every reply, and every attribution query the fake driver saw the hook send,
 * with what the scenario states. HOOK_SO is the test build of hook/gs_hook.c
 * (-DGS_HOOK_TEST_ENV, the only difference: it honours the GEFORCE_SRIOV_TEST_* overrides, two
 * paths and the name of the next ioctl, and logs to stderr instead of syslog). The fake driver
 * checks every query's fd, client, object, command, size, flags, preset status and gpuId chain
 * itself and reports a broken rule as a BAD token, which no expectation contains.
 * Cards are taken from the shared device table, so the tests follow include/gs_devices.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gs_devices.h"
#include "fixture.h"

#define SUB_A 0x146d10deu /* a board partner subsystem, always kept */
#define OUT_MAX 65536
#define CLIENT_2 0xc1d00777u
/* The hook's test-only override of the symbol it looks up as the next ioctl (gs_hook.c). */
#define NEXT_IOCTL_ENV "GEFORCE_SRIOV_TEST_NEXT_IOCTL"

static const char *hook_so, *fake_so, *vgpud, *vgpu_mgr;
static char tmp[256];
static int failures;

/* The cards: A is the first Ampere row, B the first Ada row (another target). */
static const struct gs_device *card_a, *card_b;
static unsigned int unknown_id;

#define WORD(dev, ven) (((uint32_t)(dev) << 16) | (uint32_t)(ven))

/* Device IDs in machine and op descriptions, resolved once the cards are picked. */
enum { DEV_A = 0x10001, DEV_B, DEV_X, DEV_TA };

static unsigned int devid(unsigned int d)
{
	switch (d) {
	case DEV_A:
		return card_a->device;
	case DEV_B:
		return card_b->device;
	case DEV_X:
		return unknown_id;
	case DEV_TA:
		return gs_target_of(card_a)->device;
	}
	return d;
}

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

static void put_file(const char *path, const char *text, size_t len, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0 || write(fd, text, len) != (ssize_t)len || fchmod(fd, mode) != 0 || close(fd))
		die("cannot write %s: %s", path, strerror(errno));
}

static void put_text(const char *path, const char *text)
{
	put_file(path, text, strlen(text), 0644);
}

static void mkdir_p(const char *path)
{
	char buf[512];

	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST)
			die("mkdir %s: %s", buf, strerror(errno));
		*p = '/';
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST)
		die("mkdir %s: %s", buf, strerror(errno));
}

/* ---- Machines ---------------------------------------------------------------------------- */

struct fn {
	const char *bdf;
	unsigned int vendor, device, class;
	const char *physfn; /* a VF: the physfn link target */
	bool rm;            /* a GPU the driver registered (object table entries) */
	bool broken;        /* a directory without ID files */
};

#define MAX_FNS 32

struct machine {
	size_t n;
	struct fn f[MAX_FNS];
	bool late[MAX_FNS]; /* written under <sysfs>/late; an 'h' op moves it in */
};

static void add(struct machine *m, struct fn f)
{
	if (m->n == MAX_FNS)
		die("machine too large");
	m->f[m->n++] = f;
}

static void gpu(struct machine *m, const char *bdf, unsigned int dev)
{
	add(m, (struct fn){ bdf, GS_VENDOR_NVIDIA, dev, 0x030000, NULL, true, false });
}

/* A GPU that is in sysfs but not registered in the driver (e.g. bound to another driver). */
static void gpu_unregistered(struct machine *m, const char *bdf, unsigned int dev)
{
	add(m, (struct fn){ bdf, GS_VENDOR_NVIDIA, dev, 0x030000, NULL, false, false });
}

static void audio(struct machine *m, const char *bdf)
{
	add(m, (struct fn){ bdf, GS_VENDOR_NVIDIA, 0x1aef, 0x040300, NULL, false, false });
}

static void vf(struct machine *m, const char *bdf, unsigned int dev, const char *pf)
{
	add(m, (struct fn){ bdf, GS_VENDOR_NVIDIA, dev, 0x030000, pf, false, false });
}

static void bridge(struct machine *m, const char *bdf, unsigned int vendor, unsigned int dev)
{
	add(m, (struct fn){ bdf, vendor, dev, 0x060400, NULL, false, false });
}

static void broken(struct machine *m, const char *bdf)
{
	add(m, (struct fn){ bdf, 0, 0, 0, NULL, false, true });
}

/* Another NVIDIA display-class physical function in a card's slot (not a VF, not audio). */
static void second_display(struct machine *m, const char *bdf)
{
	add(m, (struct fn){ bdf, GS_VENDOR_NVIDIA, card_a->device, 0x030000, NULL, false, false });
}

static size_t fn_index(const struct machine *m, const char *bdf)
{
	for (size_t i = 0; i < m->n; i++)
		if (strcmp(m->f[i].bdf, bdf) == 0)
			return i;
	die("no function %s in the machine", bdf);
	return 0;
}

/* Object table identities of function i: handles with the low 17 bits clear, opaque gpuIds. */
static uint32_t h_sub(size_t i) { return 0x40000000u | ((uint32_t)i << 20); }
static uint32_t h_dev(size_t i) { return 0x60000000u | ((uint32_t)i << 20); }
static uint32_t gpu_id(size_t i) { return 0x00000c00u + 0x100u * (uint32_t)i; }

enum sysfs_extra {
	SYS_TWIN = 1,         /* 03:00.0: an unselected NVIDIA GPU with card A's device ID */
	SYS_FOREIGN_TWIN = 2, /* 07:00.0: a non-NVIDIA GPU with card A's device ID */
	SYS_BROKEN = 4,       /* 06:00.0: a device directory without a vendor file */
};

/*
 * The standard machine: a non-NVIDIA bridge; card A at 01:00.0 with its audio function and
 * one VF of the same device ID; card B (3D controller class) at 02:00.0; an NVIDIA GPU that
 * is not in the table at 04:00.0; plus the extras requested.
 */
static void m_std(struct machine *m, unsigned int extra)
{
	bridge(m, "0000:00:01.0", 0x8086, 0x1234);
	gpu(m, "0000:01:00.0", DEV_A);
	audio(m, "0000:01:00.1");
	vf(m, "0000:01:00.4", DEV_A, "../0000:01:00.0");
	add(m, (struct fn){ "0000:02:00.0", GS_VENDOR_NVIDIA, DEV_B, 0x030200, NULL, true, false });
	gpu(m, "0000:04:00.0", DEV_X);
	if (extra & SYS_TWIN)
		gpu(m, "0000:03:00.0", DEV_A);
	if (extra & SYS_FOREIGN_TWIN)
		add(m, (struct fn){ "0000:07:00.0", 0x1002, DEV_A, 0x030000, NULL, false, false });
	if (extra & SYS_BROKEN)
		broken(m, "0000:06:00.0");
}

/* The boot display GPU at 01:00.0 and the selected card at 02:00.0 are identical. */
static void m_boot(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	audio(m, "0000:01:00.1");
	gpu(m, "0000:02:00.0", DEV_A);
	audio(m, "0000:02:00.1");
	vf(m, "0000:02:00.4", DEV_A, "../0000:02:00.0");
	vf(m, "0000:02:00.5", DEV_A, "../0000:02:00.0");
	vf(m, "0000:02:00.6", DEV_A, "../0000:02:00.0");
	vf(m, "0000:02:00.7", DEV_A, "../0000:02:00.0");
}

static void m_mixed(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	gpu(m, "0000:02:00.0", DEV_B);
	gpu(m, "0000:03:00.0", DEV_A);
	gpu(m, "0000:04:00.0", DEV_B);
}

static void m_three(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	gpu(m, "0000:02:00.0", DEV_A);
	gpu(m, "0000:03:00.0", DEV_A);
	gpu(m, "0000:04:00.0", DEV_B);
}

/* The same bus and slot in three PCI domains, one above ffff (as behind Intel VMD). */
static void m_domains(struct machine *m)
{
	gpu(m, "0000:81:00.0", DEV_A);
	gpu(m, "0001:81:00.0", DEV_A);
	gpu(m, "10000:81:00.0", DEV_A);
}

/* Three identical cards behind one PCIe switch, one per downstream port. */
static void m_switch(struct machine *m)
{
	bridge(m, "0000:00:01.0", 0x8086, 0x1234);
	bridge(m, "0000:04:00.0", 0x10b5, 0x8747);
	bridge(m, "0000:05:08.0", 0x10b5, 0x8747);
	bridge(m, "0000:05:10.0", 0x10b5, 0x8747);
	bridge(m, "0000:05:18.0", 0x10b5, 0x8747);
	gpu(m, "0000:06:00.0", DEV_A);
	audio(m, "0000:06:00.1");
	gpu(m, "0000:07:00.0", DEV_A);
	audio(m, "0000:07:00.1");
	gpu(m, "0000:08:00.0", DEV_A);
	audio(m, "0000:08:00.1");
}

static void m_slot(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	second_display(m, "0000:01:00.1");
	gpu(m, "0000:02:00.0", DEV_B);
}

/* Selected card A at 01:00.0 and an identical unselected twin at 03:00.0. */
static void m_twin(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	audio(m, "0000:01:00.1");
	gpu(m, "0000:03:00.0", DEV_A);
}

/* Card A at 01:00.0, card B at 02:00.0: no two GPUs share an ID. */
static void m_alone(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	gpu(m, "0000:02:00.0", DEV_B);
}

static void m_alone_broken(struct machine *m)
{
	m_alone(m);
	broken(m, "0000:06:00.0");
}

/* m_alone, plus an unselected twin of card A that is hot-plugged while the daemon runs. */
static void m_late_twin(struct machine *m)
{
	m_alone(m);
	gpu_unregistered(m, "0000:03:00.0", DEV_A);
	m->late[m->n - 1] = true;
}

/* m_alone, plus an unreadable PCI entry that appears while the daemon runs. */
static void m_late_broken(struct machine *m)
{
	m_alone(m);
	broken(m, "0000:06:00.0");
	m->late[m->n - 1] = true;
}

/* Identical cards on the same domain and bus, in the highest slots of the highest bus. */
static void m_slots(struct machine *m)
{
	gpu(m, "0000:ff:1e.0", DEV_A);
	gpu(m, "0000:ff:1f.0", DEV_A);
}

/* Identical cards at 01:00.0 and 03:00.0; 03:00.0 has a second display function. */
static void m_pair_refused(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	gpu(m, "0000:03:00.0", DEV_A);
	second_display(m, "0000:03:00.1");
}

/* The selected card is in sysfs but not registered in the driver; its twin is. */
static void m_not_in_driver(struct machine *m)
{
	gpu_unregistered(m, "0000:01:00.0", DEV_A);
	gpu(m, "0000:03:00.0", DEV_A);
}

/* A VF that sits at function 0 of its own bus (ARI routing), with its PF at 01:00.0. */
static void m_vf_fn0(struct machine *m)
{
	gpu(m, "0000:01:00.0", DEV_A);
	vf(m, "0000:05:00.0", DEV_A, "../0000:01:00.0");
}

/* A table device ID on a function whose class is not display. */
static void m_not_display(struct machine *m)
{
	add(m, (struct fn){ "0000:01:00.0", GS_VENDOR_NVIDIA, DEV_A, 0x068000, NULL, true, false });
}

static void write_sysfs(const char *root, const struct machine *m)
{
	for (size_t i = 0; i < m->n; i++) {
		const struct fn *f = &m->f[i];
		char dir[512], path[600], id[16];

		snprintf(dir, sizeof(dir), "%s/%s/%s", root, m->late[i] ? "late" : "bus/pci/devices",
			 f->bdf);
		mkdir_p(dir);
		if (f->broken)
			continue;
		snprintf(path, sizeof(path), "%s/vendor", dir);
		snprintf(id, sizeof(id), "0x%04x\n", f->vendor);
		put_text(path, id);
		snprintf(path, sizeof(path), "%s/device", dir);
		snprintf(id, sizeof(id), "0x%04x\n", devid(f->device));
		put_text(path, id);
		snprintf(path, sizeof(path), "%s/class", dir);
		snprintf(id, sizeof(id), "0x%06x\n", f->class);
		put_text(path, id);
		if (f->physfn) {
			snprintf(path, sizeof(path), "%s/physfn", dir);
			if (symlink(f->physfn, path) != 0)
				die("symlink %s: %s", path, strerror(errno));
		}
	}
}

/* A fault injected into one attribution query of one GPU. */
struct fault {
	const char *cmd; /* id, pci, num, find */
	const char *gpu;
	const char *kind;
	unsigned int value;
};

/*
 * The fake driver's object table. With swap_a/swap_b set, the handles of those two GPUs
 * belong to each other's GPU (a handle freed and allocated again for another GPU).
 */
static void write_table(const char *path, const struct machine *m, const struct fault *faults,
			const char *swap_a, const char *swap_b)
{
	size_t a = swap_a ? fn_index(m, swap_a) : MAX_FNS, b = swap_b ? fn_index(m, swap_b) : MAX_FNS;
	char text[8192];
	size_t n = 0;

#define T(...) (n += (size_t)snprintf(text + n, sizeof(text) - n, __VA_ARGS__))
	for (size_t i = 0; i < m->n; i++) {
		const struct fn *f = &m->f[i];
		size_t owner = i == a ? b : i == b ? a : i;
		unsigned int dom, bus, slot, fnum;

		if (!f->rm)
			continue;
		if (sscanf(f->bdf, "%x:%x:%x.%x", &dom, &bus, &slot, &fnum) != 4)
			die("bad bdf %s", f->bdf);
		T("sub %#x %#x\n", h_sub(i), gpu_id(owner));
		T("dev %#x 1 %#x\n", h_dev(i), h_sub(i));
		T("gpu %#x %#x %#x %#x\n", gpu_id(i), dom, bus, slot);
	}
	for (const struct fault *f = faults; f && f->cmd; f++) {
		size_t i = fn_index(m, f->gpu);
		uint32_t key = strcmp(f->cmd, "pci") == 0 ? gpu_id(i)
			       : strcmp(f->cmd, "id") == 0 ? h_sub(i) : h_dev(i);

		T("fault %s %#x %s %#x\n", f->cmd, key, f->kind, f->value);
	}
#undef T
	if (n >= sizeof(text))
		die("table too large");
	put_text(path, text);
}

/* ---- Operations and what they must return ------------------------------------------------- */

struct op {
	char kind;         /* p pci, v virt, o other, t tty, T threads, s table swap, c client,
			      d dupfd, h hot-plug gpu (late -> sysfs), u unplug gpu (sysfs -> late) */
	const char *gpu;   /* p: its subdevice handle; v: its device handle; T: even threads;
			      s: swap with gpu2 (NULL: the original table) */
	const char *gpu2;  /* T: odd threads; s: the other GPU */
	uint32_t handle;   /* p, v without gpu: this hObject (fixture.h status convention) */
	unsigned int dev;  /* p, o, T: device ID in the reply; 0: the GPU's own */
	uint32_t vendor;   /* p: 0 is NVIDIA */
	uint32_t size;     /* p, v: 0 is the header size */
	uint32_t flags;    /* p */
	uint32_t mode;     /* v */
	uint32_t client;   /* c */
	bool change;       /* p, T even: rewritten; v: forced to HOST */
	bool change2;      /* T odd: rewritten */
	const char *q;     /* the queries the hook must send, e.g. "id,pci"; NULL: none */
	const char *q2;    /* T odd threads */
};

#define VIRT_Q "num,find,id,pci"
#define PCI_Q "id,pci"
#define THREADS 8u
#define CALLS 500u

/* Standard ops on the standard machine (A 01:00.0, B 02:00.0, X 04:00.0). */
static size_t std_ops(struct op *o, bool map_a, bool map_b, bool force, bool virt_q)
{
	const char *qa = map_a ? PCI_Q : NULL, *qv = virt_q ? VIRT_Q : NULL;
	const struct op ops[] = {
		{ .kind = 'p', .gpu = "0000:01:00.0", .change = map_a, .q = qa },
		{ .kind = 'p', .gpu = "0000:02:00.0", .change = map_b, .q = map_b ? PCI_Q : NULL },
		{ .kind = 'p', .gpu = "0000:04:00.0" },              /* not in the table */
		{ .kind = 'p', .gpu = "0000:01:00.0", .dev = DEV_TA }, /* already a target ID */
		{ .kind = 'p', .gpu = "0000:01:00.0", .size = 12 },
		{ .kind = 'p', .handle = 0x25, .dev = DEV_A },       /* failed status */
		{ .kind = 'p', .handle = FAKE_FAIL_EIO, .dev = DEV_A }, /* failed ioctl */
		{ .kind = 'p', .gpu = "0000:01:00.0", .flags = GS_NVOS54_FLAGS_FINN_SERIALIZED },
		{ .kind = 'p', .gpu = "0000:01:00.0", .vendor = 0x1234 },
		{ .kind = 'v', .gpu = "0000:01:00.0", .change = force, .q = qv },
		{ .kind = 'v', .gpu = "0000:01:00.0", .size = 4 },
		{ .kind = 'v', .handle = 0x25 },
		{ .kind = 'v', .gpu = "0000:01:00.0", .mode = 3 },   /* already HOST */
		{ .kind = 'v', .gpu = "0000:04:00.0", .q = qv },     /* never forced */
		{ .kind = 'o', .dev = DEV_A },
		{ .kind = 't' },
		{ .kind = 'T', .gpu = "0000:01:00.0", .gpu2 = "0000:01:00.0", .change = map_a,
		  .change2 = map_a, .q = qa, .q2 = qa },
	};

	memcpy(o, ops, sizeof(ops));
	return sizeof(ops) / sizeof(ops[0]);
}

static unsigned int count_queries(const char *q)
{
	unsigned int n = 0;

	for (; q && *q; q++)
		n += *q == ',';
	return q ? n + 1 : 0;
}

/* The expected trace: the GPU's own handles, the daemon's client for the root query. */
static void expect_queries(char *out, size_t len, const char *spec, const struct machine *m,
			   const char *gpu_bdf, uint32_t client)
{
	char copy[64], *save = NULL;
	size_t n = 0, i;

	if (!spec) {
		snprintf(out, len, "-");
		return;
	}
	i = fn_index(m, gpu_bdf);
	snprintf(copy, sizeof(copy), "%s", spec);
	out[0] = '\0';
	for (char *w = strtok_r(copy, ",", &save); w; w = strtok_r(NULL, ",", &save)) {
		uint32_t obj = strcmp(w, "id") == 0 ? h_sub(i)
			       : strcmp(w, "pci") == 0 ? client : h_dev(i);

		n += (size_t)snprintf(out + n, len - n, "%s%s:%x", n ? "," : "", w, obj);
	}
}

static uint32_t presented_word(unsigned int dev)
{
	return WORD(gs_target_of(gs_device_lookup(dev))->device, GS_VENDOR_NVIDIA);
}

static uint32_t op_handle(const struct machine *m, const struct op *o)
{
	if (!o->gpu)
		return o->handle;
	return o->kind == 'v' ? h_dev(fn_index(m, o->gpu)) : h_sub(fn_index(m, o->gpu));
}

static unsigned int op_dev(const struct machine *m, const struct op *o)
{
	return devid(o->dev ? o->dev : m->f[fn_index(m, o->gpu)].device);
}

/* Appends the op's arguments to argv and its expected output to want. */
struct plan {
	char args[600][320];
	char *argv[610];
	size_t argc, n_args;
	char want[OUT_MAX];
	size_t want_len;
	uint32_t client;
	unsigned int n_tables;
};

__attribute__((format(printf, 2, 3))) static void arg(struct plan *p, const char *fmt, ...)
{
	va_list ap;

	if (p->n_args == sizeof(p->args) / sizeof(p->args[0]))
		die("too many arguments");
	va_start(ap, fmt);
	vsnprintf(p->args[p->n_args], sizeof(p->args[0]), fmt, ap);
	va_end(ap);
	p->argv[p->argc++] = p->args[p->n_args++];
}

__attribute__((format(printf, 2, 3))) static void want(struct plan *p, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	p->want_len += (size_t)vsnprintf(p->want + p->want_len, sizeof(p->want) - p->want_len,
					 fmt, ap);
	va_end(ap);
	if (p->want_len >= sizeof(p->want))
		die("expected output too large");
}

static void plan_op(struct plan *p, const struct op *o, const struct machine *m,
		    const struct fault *faults, const char *dir)
{
	const int ok = FAKE_SUCCESS_ERRNO;
	char q[FAKE_TRACE_MAX], path[600];
	uint32_t h = 0;

	if (o->kind == 'p' || o->kind == 'v')
		h = op_handle(m, o);
	switch (o->kind) {
	case 'p': {
		unsigned int dev = op_dev(m, o);
		uint32_t word = WORD(dev, o->vendor ? o->vendor : GS_VENDOR_NVIDIA);
		bool eio = h & FAKE_FAIL_EIO;

		arg(p, "pci");
		arg(p, "%#x", word);
		arg(p, "%#x", SUB_A);
		arg(p, "%u", o->size ? o->size : (uint32_t)sizeof(struct gs_pci_info));
		arg(p, "%#x", h);
		arg(p, "%#x", o->flags);
		expect_queries(q, sizeof(q), o->q, m, o->gpu, p->client);
		want(p, FIX_PCI_LINE, o->change ? presented_word(dev) : word,
		     SUB_A, FIX_REVISION, word,
		     eio ? -1 : 0, eio ? EIO : ok,
		     eio ? FIX_STATUS_UNSET : (unsigned int)(h & FAKE_STATUS_MASK), q);
		break;
	}
	case 'v':
		arg(p, "virt");
		arg(p, "%u", o->mode);
		arg(p, "%u", o->size ? o->size : (uint32_t)sizeof(struct gs_virt_mode));
		arg(p, "%#x", h);
		expect_queries(q, sizeof(q), o->q, m, o->gpu, p->client);
		want(p, FIX_VIRT_LINE, o->change ? 3u : o->mode, 1u, 0, ok,
		     (unsigned int)(h & FAKE_STATUS_MASK), q);
		break;
	case 'o':
		arg(p, "other");
		arg(p, "%#x", WORD(devid(o->dev), GS_VENDOR_NVIDIA));
		want(p, FIX_OTHER_LINE, WORD(devid(o->dev), GS_VENDOR_NVIDIA), 0x10deu, 0, "-");
		break;
	case 't':
		arg(p, "tty");
		want(p, FIX_TTY_LINE, -1, ENOTTY, "-");
		break;
	case 'T': {
		unsigned int dev = op_dev(m, o);
		uint32_t word = WORD(dev, GS_VENDOR_NVIDIA);
		unsigned int per_group = THREADS / 2 * CALLS;

		arg(p, "threads");
		arg(p, "%#x", word);
		arg(p, "%#x", SUB_A);
		arg(p, "%u", THREADS);
		arg(p, "%u", CALLS);
		arg(p, "%#x", h_sub(fn_index(m, o->gpu)));
		arg(p, "%#x", h_sub(fn_index(m, o->gpu2)));
		want(p, FIX_THREADS_LINE, o->change ? presented_word(dev) : word,
		     SUB_A,
		     o->change2 ? presented_word(dev) : word,
		     SUB_A, THREADS * CALLS, 0u,
		     per_group * (count_queries(o->q) + count_queries(o->q2)), 0u);
		break;
	}
	case 's':
		snprintf(path, sizeof(path), "%s/table%u", dir, ++p->n_tables);
		write_table(path, m, faults, o->gpu, o->gpu2);
		arg(p, "table");
		arg(p, "%s", path);
		want(p, FIX_TABLE_LINE, 0);
		break;
	case 'c':
		p->client = o->client;
		arg(p, "client");
		arg(p, "%#x", o->client);
		want(p, FIX_CLIENT_LINE, o->client);
		break;
	case 'd':
		arg(p, "dupfd");
		want(p, FIX_DUPFD_LINE, 1);
		break;
	case 'h':
	case 'u': {
		char late[400], live[400];

		snprintf(late, sizeof(late), "%s/sys/late/%s", dir, o->gpu);
		snprintf(live, sizeof(live), "%s/sys/bus/pci/devices/%s", dir, o->gpu);
		arg(p, "rename");
		arg(p, "%s", o->kind == 'h' ? late : live);
		arg(p, "%s", o->kind == 'h' ? live : late);
		want(p, FIX_RENAME_LINE, 0);
		break;
	}
	default:
		die("bad op %c", o->kind);
	}
}

/* ---- Scenarios ---------------------------------------------------------------------------- */

struct scenario {
	const char *name;
	const char *config;  /* NULL: no config file at the configured path */
	mode_t config_mode;  /* 0: 0644 */
	bool config_is_dir;
	const char *version; /* contents of the driver version file; NULL: no file */
	void (*machine)(struct machine *); /* NULL: the standard machine with extra */
	unsigned int extra;
	const struct fault *faults;
	const struct op *ops; /* NULL: the standard ops with the flags below */
	size_t n_ops;
	bool map_a, map_b, force, virt_q;
	bool manager;        /* run as nvidia-vgpu-mgr */
	bool no_hook;        /* preload only the fake driver */
	const char *log_has;  /* substrings the hook's stderr must contain */
	const char *log_has2;
	bool no_next_ioctl;  /* the hook's lookup of the next ioctl fails */
};

#define OPS(a) .ops = (a), .n_ops = sizeof(a) / sizeof((a)[0])

static size_t slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t len = 0;
	ssize_t n;

	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	while (len + 1 < cap && (n = read(fd, buf + len, cap - 1 - len)) > 0)
		len += (size_t)n;
	close(fd);
	buf[len] = '\0';
	return len;
}

static void run(const struct scenario *s)
{
	static struct plan plan;
	static char got[OUT_MAX], log[OUT_MAX];
	char dir[300], cfg[400], ver[400], sys[400], table[400], out[400], err[400];
	char preload[1024], env_cfg[512], env_ver[512], env_sys[512], env_pre[1100], env_tab[512];
	char env_next[64] = NEXT_IOCTL_ENV "=gs_no_such_ioctl";
	char *envp[] = { env_pre, env_cfg, env_ver, env_sys, env_tab, s->no_next_ioctl ? env_next : NULL, NULL };
	struct op std[32];
	const struct op *ops = s->ops;
	size_t n_ops = s->n_ops;
	struct machine m = { 0 };
	int status;
	pid_t pid;
	bool ok;

	snprintf(dir, sizeof(dir), "%s/%s", tmp, s->name);
	mkdir_p(dir);
	snprintf(cfg, sizeof(cfg), "%s/config", dir);
	snprintf(ver, sizeof(ver), "%s/version", dir);
	snprintf(sys, sizeof(sys), "%s/sys", dir);
	snprintf(table, sizeof(table), "%s/table", dir);
	snprintf(out, sizeof(out), "%s/stdout", dir);
	snprintf(err, sizeof(err), "%s/stderr", dir);
	if (s->config_is_dir)
		mkdir_p(cfg);
	else if (s->config)
		put_file(cfg, s->config, strlen(s->config), s->config_mode ? s->config_mode : 0644);
	if (s->version)
		put_text(ver, s->version);
	if (s->machine)
		s->machine(&m);
	else
		m_std(&m, s->extra);
	write_sysfs(sys, &m);
	write_table(table, &m, s->faults, NULL, NULL);
	if (!ops) {
		n_ops = std_ops(std, s->map_a, s->map_b, s->force, s->virt_q);
		ops = std;
	}

	memset(&plan, 0, sizeof(plan));
	plan.client = FIX_CLIENT;
	plan.argc = 1;
	for (size_t i = 0; i < n_ops; i++)
		plan_op(&plan, &ops[i], &m, s->faults, dir);
	plan.argv[0] = (char *)(s->manager ? vgpu_mgr : vgpud);
	plan.argv[plan.argc] = NULL;

	if (s->no_hook)
		snprintf(preload, sizeof(preload), "%s", fake_so);
	else
		snprintf(preload, sizeof(preload), "%s %s", hook_so, fake_so);
	snprintf(env_pre, sizeof(env_pre), "LD_PRELOAD=%s", preload);
	snprintf(env_cfg, sizeof(env_cfg), "%s=%s", GS_HOOK_ENV_CONFIG, cfg);
	snprintf(env_ver, sizeof(env_ver), "%s=%s", GS_HOOK_TEST_ENV_DRIVER_VERSION, ver);
	snprintf(env_sys, sizeof(env_sys), "%s=%s", GS_HOOK_TEST_ENV_SYSFS_ROOT, sys);
	snprintf(env_tab, sizeof(env_tab), "%s=%s", FAKE_ENV_TABLE, table);

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid == 0) {
		int o = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		int e = open(err, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

		if (o < 0 || e < 0 || dup2(o, 1) < 0 || dup2(e, 2) < 0)
			_exit(126);
		execve(plan.argv[0], plan.argv, envp);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		die("waitpid: %s", strerror(errno));

	slurp(out, got, sizeof(got));
	slurp(err, log, sizeof(log));
	ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && strcmp(plan.want, got) == 0;
	if (s->log_has && !strstr(log, s->log_has))
		ok = false;
	if (s->log_has2 && !strstr(log, s->log_has2))
		ok = false;
	if ((s->manager || s->no_hook) && strstr(log, "geforce-sriov-hook"))
		ok = false; /* another process, or no hook: not even a log line */
	printf("%s %s\n", ok ? "PASS" : "FAIL", s->name);
	if (!ok) {
		failures++;
		printf("  exit status %#x\n  expected log substrings: %s | %s\n--- want\n%s--- got\n%s"
		       "--- hook log\n%s---\n",
		       (unsigned int)status, s->log_has ? s->log_has : "(none)",
		       s->log_has2 ? s->log_has2 : "(none)", plan.want, got, log);
	}
}

static void pick_cards(void)
{
	for (unsigned int i = 0; i < GS_DEVICE_COUNT; i++) {
		const struct gs_device *d = &gs_devices[i];

		if (!card_a && d->generation == GS_AMPERE)
			card_a = d;
		if (!card_b && d->generation == GS_ADA)
			card_b = d;
	}
	if (!card_a || !card_b)
		die("the device table needs an Ampere and an Ada row");
	for (unknown_id = 0x1e00; gs_device_lookup(unknown_id); unknown_id++)
		;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

/* Shorthands for custom op lists. */
#define PCI(g, ch) { .kind = 'p', .gpu = (g), .change = (ch), .q = PCI_Q }
#define PCI_Q_(g, ch, qq) { .kind = 'p', .gpu = (g), .change = (ch), .q = (qq) }
#define PCI_NOQ(g) { .kind = 'p', .gpu = (g) }
#define VIRT(g, ch) { .kind = 'v', .gpu = (g), .change = (ch), .q = VIRT_Q }
#define VIRT_Q_(g, qq) { .kind = 'v', .gpu = (g), .q = (qq) }
#define SWAP(a, b) { .kind = 's', .gpu = (a), .gpu2 = (b) }
#define UNSWAP { .kind = 's' }

static const struct op ops_boot[] = {
	PCI("0000:01:00.0", false), PCI("0000:02:00.0", true),
	VIRT("0000:01:00.0", false), VIRT("0000:02:00.0", true),
	{ .kind = 'T', .gpu = "0000:02:00.0", .gpu2 = "0000:01:00.0", .change = true,
	  .q = PCI_Q, .q2 = PCI_Q },
};

static const struct op ops_boot_lower[] = {
	PCI("0000:01:00.0", true), PCI("0000:02:00.0", false),
	VIRT("0000:01:00.0", true), VIRT("0000:02:00.0", false),
};

/* A1 01 and B2 04 selected, A 03 and B 02 not; the daemon changes client and fd midway. */
static const struct op ops_mixed[] = {
	PCI("0000:01:00.0", true), PCI("0000:02:00.0", false),
	PCI("0000:03:00.0", false), PCI("0000:04:00.0", true),
	VIRT("0000:01:00.0", true), VIRT("0000:02:00.0", false),
	VIRT("0000:03:00.0", false), VIRT("0000:04:00.0", true),
	{ .kind = 'c', .client = CLIENT_2 },
	PCI("0000:04:00.0", true), VIRT("0000:03:00.0", false),
	{ .kind = 'd' },
	PCI("0000:01:00.0", true), PCI("0000:03:00.0", false), VIRT("0000:01:00.0", true),
};

static const struct op ops_three_two[] = {
	PCI("0000:01:00.0", true), PCI("0000:02:00.0", false), PCI("0000:03:00.0", true),
	PCI_NOQ("0000:04:00.0"), /* B is not selected: no card has its ID, nothing to ask */
	VIRT("0000:01:00.0", true), VIRT("0000:02:00.0", false), VIRT("0000:03:00.0", true),
	VIRT("0000:04:00.0", false),
};

static const struct op ops_three_one[] = {
	PCI("0000:01:00.0", false), PCI("0000:02:00.0", true), PCI("0000:03:00.0", false),
	VIRT("0000:01:00.0", false), VIRT("0000:02:00.0", true), VIRT("0000:03:00.0", false),
};

static const struct op ops_domain_0001[] = {
	PCI("0000:81:00.0", false), PCI("0001:81:00.0", true), PCI("10000:81:00.0", false),
	VIRT("0000:81:00.0", false), VIRT("0001:81:00.0", true), VIRT("10000:81:00.0", false),
};

static const struct op ops_domain_10000[] = {
	PCI("0000:81:00.0", false), PCI("0001:81:00.0", false), PCI("10000:81:00.0", true),
	VIRT("0000:81:00.0", false), VIRT("0001:81:00.0", false), VIRT("10000:81:00.0", true),
};

static const struct op ops_switch[] = {
	PCI("0000:06:00.0", false), PCI("0000:07:00.0", true), PCI("0000:08:00.0", false),
	VIRT("0000:06:00.0", false), VIRT("0000:07:00.0", true), VIRT("0000:08:00.0", false),
};

static const struct op ops_slots[] = {
	PCI("0000:ff:1e.0", false), PCI("0000:ff:1f.0", true),
	VIRT("0000:ff:1e.0", false), VIRT("0000:ff:1f.0", true),
};

static const struct op ops_slot[] = {
	PCI_NOQ("0000:01:00.0"), PCI("0000:02:00.0", true),
	VIRT("0000:01:00.0", false), VIRT("0000:02:00.0", true),
};

static const struct op ops_inert_01[] = {
	PCI_NOQ("0000:01:00.0"), { .kind = 'v', .gpu = "0000:01:00.0" },
};

/* Query failures on the selected card, whose unselected twin rules out the ID fallback. */
static const struct op ops_fail_id[] = {
	PCI_Q_("0000:01:00.0", false, "id"), VIRT_Q_("0000:01:00.0", "num,find,id"),
	PCI("0000:03:00.0", false),
};
static const struct op ops_fail_pci[] = {
	PCI_Q_("0000:01:00.0", false, PCI_Q), VIRT_Q_("0000:01:00.0", VIRT_Q),
	PCI("0000:03:00.0", false),
};
static const struct op ops_fail_num[] = {
	PCI("0000:01:00.0", true), VIRT_Q_("0000:01:00.0", "num"),
};
static const struct op ops_fail_find[] = {
	PCI("0000:01:00.0", true), VIRT_Q_("0000:01:00.0", "num,find"),
};

/* The fallback by device ID: GET_PCI_INFO only, never HOST. */
static const struct op ops_fallback[] = {
	PCI_Q_("0000:01:00.0", true, "id"), VIRT_Q_("0000:01:00.0", "num,find,id"),
	PCI_NOQ("0000:02:00.0"),
};
static const struct op ops_fallback_pair[] = {
	PCI_Q_("0000:03:00.0", true, "id"), PCI("0000:01:00.0", true),
};
static const struct op ops_no_fallback[] = {
	PCI_Q_("0000:01:00.0", false, "id"),
};

/* The address is a selected card, but the reply's device ID is the other card's. */
static const struct op ops_mismatch[] = {
	{ .kind = 'p', .gpu = "0000:01:00.0", .dev = DEV_B, .q = PCI_Q },
	{ .kind = 'p', .gpu = "0000:02:00.0", .dev = DEV_A, .q = PCI_Q },
	PCI("0000:01:00.0", true), PCI("0000:02:00.0", true),
};

/* The same handles answer for another GPU after a table swap: nothing may be cached. */
static const struct op ops_reuse[] = {
	PCI("0000:01:00.0", true), VIRT("0000:01:00.0", true), PCI("0000:03:00.0", false),
	SWAP("0000:01:00.0", "0000:03:00.0"),
	PCI("0000:01:00.0", false), PCI("0000:03:00.0", true),
	VIRT("0000:01:00.0", false), VIRT("0000:03:00.0", true),
	UNSWAP,
	PCI("0000:01:00.0", true), PCI("0000:03:00.0", false),
	VIRT("0000:01:00.0", true), VIRT("0000:03:00.0", false),
};

/*
 * The device-ID fallback is checked again against the PCI devices each time it is needed:
 * a twin plugged in later, or the selected card gone, stops it; both undone, it resumes.
 */
static const struct op ops_hotplug[] = {
	PCI_Q_("0000:01:00.0", true, "id"),
	{ .kind = 'h', .gpu = "0000:03:00.0" },
	PCI_Q_("0000:01:00.0", false, "id"),
	{ .kind = 'u', .gpu = "0000:03:00.0" },
	PCI_Q_("0000:01:00.0", true, "id"),
	{ .kind = 'u', .gpu = "0000:01:00.0" },
	PCI_Q_("0000:01:00.0", false, "id"),
};

/* An unreadable entry appearing later stops the fallback just as a twin does. */
static const struct op ops_late_broken[] = {
	PCI_Q_("0000:01:00.0", true, "id"),
	{ .kind = 'h', .gpu = "0000:06:00.0" },
	PCI_Q_("0000:01:00.0", false, "id"),
};

/* Attributed to an unselected GPU while the device-ID fallback is available: never used. */
static const struct op ops_unmapped_with_fallback[] = {
	{ .kind = 'p', .gpu = "0000:02:00.0", .dev = DEV_A, .q = PCI_Q },
	PCI("0000:01:00.0", true),
};

static const struct op ops_threads[] = {
	{ .kind = 'T', .gpu = "0000:01:00.0", .gpu2 = "0000:03:00.0", .change = true,
	  .q = PCI_Q, .q2 = PCI_Q },
	{ .kind = 'T', .gpu = "0000:03:00.0", .gpu2 = "0000:01:00.0", .change2 = true,
	  .q = PCI_Q, .q2 = PCI_Q },
};

/* The selected card is not registered in the driver: its handle is unknown there. */
static const struct op ops_not_in_driver[] = {
	PCI("0000:03:00.0", false), VIRT("0000:03:00.0", false),
};

/* A plain non-RM ioctl: the kernel's answer for /dev/null reaches the daemon. */
static const struct op ops_tty[] = {
	{ .kind = 't' },
};

#define F_LIST(...) ((const struct fault[]){ __VA_ARGS__, { 0 } })
#define G01 "0000:01:00.0"

int main(int argc, char **argv)
{
	static char big[GS_HOOK_CONFIG_MAX_BYTES + 64];
	const char *tmpdir = getenv("TMPDIR");
	char too_many[600] = "identity=on\nbdfs=";

#define V580 "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.159.01  Fri Apr 24 06:16:47 UTC 2026\nGCC version:  gcc version 14.2.0\n"
#define V578 "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.178.05  Thu Jun  4 00:00:00 UTC 2026\n"
#define V595 "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  595.91.04  Release Build  (builder@host)  Fri Jul 17 18:53:03 UTC 2026\n"
#define AB "bdfs=0000:01:00.0,0000:02:00.0\n"
#define ON "identity=on\n" AB
#define FORCE "identity=on\nidentity_force_host=on\n"
#define FORCE_ON FORCE AB
#define FORCE_01 FORCE "bdfs=0000:01:00.0\n"

	if (argc != 5)
		die("usage: %s HOOK_SO FAKE_DRIVER_SO NVIDIA_VGPUD NVIDIA_VGPU_MGR", argv[0]);
	hook_so = argv[1];
	fake_so = argv[2];
	vgpud = argv[3];
	vgpu_mgr = argv[4];
	snprintf(tmp, sizeof(tmp), "%s/gs-hook-tests.XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(tmp))
		die("mkdtemp: %s", strerror(errno));
	pick_cards();
	printf("card A %04x -> %04x, card B %04x -> %04x (subsystems kept), unlisted %04x\n",
	       card_a->device, gs_target_of(card_a)->device, card_b->device, gs_target_of(card_b)->device,
	       unknown_id);

	for (int i = 1; i <= (int)GS_MAX_PFS + 1; i++)
		snprintf(too_many + strlen(too_many), sizeof(too_many) - strlen(too_many), "%s0000:%02x:00.0",
			 i > 1 ? "," : "", i);
	memset(big, '#', sizeof(big) - 1);
	memcpy(big, ON, strlen(ON));
	big[sizeof(big) - 1] = '\0';

	const struct scenario scenarios[] = {
		/* The fixture itself: what every inert scenario must equal. */
		{ .name = "baseline_without_hook", .config = ON, .version = V580, .no_hook = true },

		/* Active, standard machine. */
		{ .name = "active_580_159_01", .config = ON, .version = V580, .map_a = true,
		  .map_b = true, .log_has = "active for driver 580.159.01" },
		{ .name = "active_595_open_module_force_host",
		  .config = "# written by geforce-sriov\nari=on\nmsix=on\nvfs=2\n"
			    "identity=on\nidentity_force_host=on\n\n"
			    "bdfs=0000:02:00.0,0000:01:00.0\nboot_display_release=0000:01:00.0\n"
			    "sriov_manage=/usr/lib/nvidia/sriov-manage",
		  .version = V595, .extra = SYS_FOREIGN_TWIN, .map_a = true, .map_b = true,
		  .force = true, .virt_q = true, .log_has = "card 0000:01:00.0 reply forced to HOST",
		  .log_has2 = "GET_VIRTUALIZATION_MODE: reply from GPU 0000:04:00, not a mapped card" },
		{ .name = "active_580_178_05_only_selected_card",
		  .config = "identity=on\nbdfs=0000:01:00.0\nidentity_force_host=off\n",
		  .version = V578, .map_a = true, .log_has = "active for driver 580.178.05" },
		{ .name = "unselected_twin_no_longer_refused", .config = ON, .version = V580,
		  .extra = SYS_TWIN, .map_a = true, .map_b = true },
		{ .name = "non_nvidia_same_id_does_not_block", .config = ON, .version = V580,
		  .extra = SYS_FOREIGN_TWIN, .map_a = true, .map_b = true,
		  .log_has = "active for driver 580.159.01" },
		{ .name = "incomplete_sysfs_keeps_attribution", .config = ON, .version = V580,
		  .extra = SYS_BROKEN, .map_a = true, .map_b = true,
		  .log_has = "cannot scan the PCI devices; no device-ID fallback" },
		{ .name = "force_host_per_gpu_unselected_not_forced", .config = FORCE_ON,
		  .version = V580, .map_a = true, .map_b = true, .force = true, .virt_q = true,
		  .log_has = "forced to HOST for mapped cards only" },
		{ .name = "force_host_selected_unmapped_gpu_not_forced",
		  .config = FORCE "bdfs=0000:01:00.0,0000:02:00.0,0000:04:00.0\n",
		  .version = V580, .map_a = true, .map_b = true, .force = true, .virt_q = true,
		  .log_has = "card 0000:04:00.0: 10de:" },
		{ .name = "force_host_driver_unlisted", .config = FORCE_ON,
		  .version = "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.159.03  Fri Apr 24 2026\n",
		  .log_has = "is not supported: every ioctl passes through" },
		{ .name = "missing_selected_card_skipped",
		  .config = "identity=on\nbdfs=0000:05:00.0,0000:02:00.0\n", .version = V580,
		  .map_b = true, .log_has = "cannot read" },

		/* Identical and mixed cards: every reply attributed by bus address. */
		{ .name = "identical_boot_gpu_unselected", .config = FORCE "bdfs=0000:02:00.0\n",
		  .version = V580, .machine = m_boot, OPS(ops_boot),
		  .log_has = "GET_PCI_INFO: reply from GPU 0000:01:00, not a mapped card" },
		{ .name = "identical_selected_lower_bus", .config = FORCE_01, .version = V580,
		  .machine = m_boot, OPS(ops_boot_lower) },
		{ .name = "mixed_ids_subsets_client_and_fd",
		  .config = FORCE "bdfs=0000:04:00.0,0000:01:00.0\n", .version = V580,
		  .machine = m_mixed, OPS(ops_mixed) },
		{ .name = "three_identical_two_selected",
		  .config = FORCE "bdfs=0000:01:00.0,0000:03:00.0\n", .version = V580,
		  .machine = m_three, OPS(ops_three_two) },
		{ .name = "three_identical_middle_selected", .config = FORCE "bdfs=0000:02:00.0\n",
		  .version = V580, .machine = m_three, OPS(ops_three_one) },
		{ .name = "same_bus_other_domains", .config = FORCE "bdfs=0001:81:00.0\n",
		  .version = V580, .machine = m_domains, OPS(ops_domain_0001),
		  .log_has = "card 0001:81:00.0: " },
		{ .name = "domain_above_ffff_selected", .config = FORCE "bdfs=10000:81:00.0\n",
		  .version = V580, .machine = m_domains, OPS(ops_domain_10000),
		  .log_has = "card 10000:81:00.0 reply forced to HOST" },
		{ .name = "switch_downstream_buses", .config = FORCE "bdfs=0000:07:00.0\n",
		  .version = V580, .machine = m_switch, OPS(ops_switch) },
		{ .name = "ambiguous_slot_refused", .config = FORCE AB, .version = V580,
		  .machine = m_slot, OPS(ops_slot),
		  .log_has = "card 0000:01:00.0: ambiguous slot, 0000:01:00.1" },
		{ .name = "ambiguous_slot_only_card_inert", .config = FORCE_01, .version = V580,
		  .machine = m_slot, OPS(ops_inert_01), .log_has = "no selected card can be mapped" },
		{ .name = "selected_card_not_in_driver", .config = FORCE_01, .version = V580,
		  .machine = m_not_in_driver, OPS(ops_not_in_driver),
		  .log_has = "reply from GPU 0000:03:00, not a mapped card" },

		/* Every attribution query failing; the twin rules out the device-ID fallback. */
		{ .name = "query_fail_get_id_status", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_id),
		  .faults = F_LIST({ "id", G01, "status", FAKE_NV_ERR_OBJECT }),
		  .log_has = "GET_PCI_INFO: cannot attribute the reply to a GPU (query 20800142: "
			     "status 0x57); unchanged",
		  .log_has2 = "GET_VIRTUALIZATION_MODE: cannot attribute the reply to a GPU" },
		{ .name = "query_fail_get_id_eio", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_id), .faults = F_LIST({ "id", G01, "eio", 0 }),
		  .log_has = "(query 20800142: ioctl errno 5)" },
		{ .name = "query_fail_pci_info_status", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_pci),
		  .faults = F_LIST({ "pci", G01, "status", FAKE_NV_ERR_ARG }),
		  .log_has = "(query 0000021b: status 0x1f)" },
		{ .name = "query_fail_pci_info_eio", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_pci), .faults = F_LIST({ "pci", G01, "eio", 0 }),
		  .log_has = "(query 0000021b: ioctl errno 5)" },
		{ .name = "query_fail_pci_info_bus_out_of_range", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_pci),
		  .faults = F_LIST({ "pci", G01, "bus", 0x100 }),
		  .log_has = "(query 0000021b: malformed PCI location)" },
		{ .name = "query_fail_pci_info_slot_out_of_range", .config = FORCE_01,
		  .version = V580, .machine = m_twin, OPS(ops_fail_pci),
		  .faults = F_LIST({ "pci", G01, "slot", 0x20 }),
		  .log_has = "malformed PCI location" },
		{ .name = "query_fail_pci_info_gpu_id_changed", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_pci), .faults = F_LIST({ "pci", G01, "echo", 0 }),
		  .log_has = "malformed PCI location" },
		{ .name = "query_fail_num_subdevices_status", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_num),
		  .faults = F_LIST({ "num", G01, "status", FAKE_NV_ERR_OBJECT }),
		  .log_has = "(query 00800280: status 0x57); unchanged" },
		{ .name = "query_fail_num_subdevices_eio", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_num), .faults = F_LIST({ "num", G01, "eio", 0 }),
		  .log_has = "(query 00800280: ioctl errno 5)" },
		{ .name = "query_fail_num_subdevices_two", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_num), .faults = F_LIST({ "num", G01, "count", 2 }),
		  .log_has = "(query 00800280: not exactly one subdevice)" },
		{ .name = "query_fail_num_subdevices_zero", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_num), .faults = F_LIST({ "num", G01, "count", 0 }),
		  .log_has = "not exactly one subdevice" },
		{ .name = "query_fail_find_subdevice_status", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_find),
		  .faults = F_LIST({ "find", G01, "status", FAKE_NV_ERR_OBJECT }),
		  .log_has = "(query 00800293: status 0x57)" },
		{ .name = "query_fail_find_subdevice_eio", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_fail_find), .faults = F_LIST({ "find", G01, "eio", 0 }),
		  .log_has = "(query 00800293: ioctl errno 5)" },
		{ .name = "query_fail_find_subdevice_null_handle", .config = FORCE_01,
		  .version = V580, .machine = m_twin, OPS(ops_fail_find),
		  .faults = F_LIST({ "find", G01, "zero", 0 }),
		  .log_has = "(query 00800293: null subdevice handle)" },

		/* The device-ID fallback when the driver cannot attribute a GET_PCI_INFO reply. */
		{ .name = "fallback_id_unique_pci_only", .config = FORCE_01, .version = V580,
		  .machine = m_alone, OPS(ops_fallback), .faults = F_LIST({ "id", G01, "eio", 0 }),
		  .log_has = "by device ID, which only mapped cards have",
		  .log_has2 = "GET_VIRTUALIZATION_MODE: cannot attribute the reply to a GPU" },
		{ .name = "fallback_two_selected_identical",
		  .config = "identity=on\nbdfs=0000:01:00.0,0000:03:00.0\n", .version = V580,
		  .machine = m_twin, OPS(ops_fallback_pair),
		  .faults = F_LIST({ "id", "0000:03:00.0", "status", FAKE_NV_ERR_OBJECT }),
		  .log_has = "by device ID" },
		{ .name = "fallback_off_same_id_card_refused",
		  .config = "identity=on\nbdfs=0000:01:00.0,0000:03:00.0\n", .version = V580,
		  .machine = m_pair_refused, OPS(ops_no_fallback),
		  .faults = F_LIST({ "id", G01, "eio", 0 }), .log_has = "ambiguous slot" },
		{ .name = "fallback_off_sysfs_unreadable", .config = FORCE_01, .version = V580,
		  .machine = m_alone_broken, OPS(ops_no_fallback),
		  .faults = F_LIST({ "id", G01, "eio", 0 }),
		  .log_has = "cannot scan the PCI devices", .log_has2 = "unchanged" },
		{ .name = "fallback_rechecked_on_hotplug", .config = FORCE_01, .version = V580,
		  .machine = m_late_twin, OPS(ops_hotplug), .faults = F_LIST({ "id", G01, "eio", 0 }),
		  .log_has = "by device ID, which only mapped cards have" },
		{ .name = "fallback_rescan_unreadable_entry", .config = FORCE_01, .version = V580,
		  .machine = m_late_broken, OPS(ops_late_broken),
		  .faults = F_LIST({ "id", G01, "eio", 0 }),
		  .log_has = "by device ID, which only mapped cards have",
		  .log_has2 = "device-ID fallback refused: cannot scan the PCI devices" },

		/* Attribution details. */
		{ .name = "unmapped_gpu_no_fallback", .config = FORCE_01, .version = V580,
		  .machine = m_alone, OPS(ops_unmapped_with_fallback),
		  .log_has = "reply from GPU 0000:02:00, not a mapped card" },
		{ .name = "adjacent_slots_highest_bus", .config = FORCE "bdfs=0000:ff:1f.0\n",
		  .version = V580, .machine = m_slots, OPS(ops_slots),
		  .log_has = "card 0000:ff:1f.0 reply forced to HOST" },
		{ .name = "device_id_mismatch_unchanged", .config = FORCE AB, .version = V580,
		  .machine = m_alone, OPS(ops_mismatch),
		  .log_has = "GET_PCI_INFO: reply from card 0000:01:00.0 has device word" },
		{ .name = "handle_reuse_not_cached", .config = FORCE_01, .version = V580,
		  .machine = m_twin, OPS(ops_reuse) },
		{ .name = "threads_selected_and_unselected_identical", .config = FORCE_01,
		  .version = V580, .machine = m_twin, OPS(ops_threads) },

		/* Inert: config. */
		{ .name = "config_absent", .version = V580, .log_has = "no config" },
		{ .name = "identity_off", .config = "identity=off\n" AB, .version = V580,
		  .log_has = "identity is not on" },
		{ .name = "identity_missing", .config = AB, .version = V580,
		  .log_has = "identity is not on" },
		{ .name = "identity_without_bdfs", .config = "identity=on\n", .version = V580,
		  .log_has = "without bdfs" },
		{ .name = "bdfs_unlisted_card", .config = "identity=on\nbdfs=0000:04:00.0\n",
		  .version = V580, .log_has = "not in the device table" },
		{ .name = "bdfs_virtual_function", .config = "identity=on\nbdfs=0000:05:00.0\n",
		  .version = V580, .machine = m_vf_fn0, OPS(ops_inert_01),
		  .log_has = "card 0000:05:00.0: is a virtual function; skipped" },
		{ .name = "bdfs_not_display_class", .config = FORCE_01, .version = V580,
		  .machine = m_not_display, OPS(ops_inert_01),
		  .log_has = "PCI base class 06 is not display" },
		{ .name = "invalid_leading_space", .config = " identity=on\n" AB, .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_space_around_equals", .config = "identity = on\n" AB,
		  .version = V580, .log_has = "invalid" },
		{ .name = "invalid_crlf", .config = "identity=on\r\n" AB, .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_value", .config = "identity=yes\n" AB, .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_duplicate_key", .config = ON "identity=on\n", .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_key_case", .config = "Identity=on\n" AB, .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_no_equals", .config = "identity\n" AB, .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_empty_value", .config = "identity=on\nbdfs=\n", .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_bdf_function", .config = "identity=on\nbdfs=0000:01:00.1\n",
		  .version = V580, .log_has = "invalid" },
		{ .name = "invalid_bdf_uppercase", .config = "identity=on\nbdfs=0000:0A:00.0\n",
		  .version = V580, .log_has = "invalid" },
		{ .name = "invalid_bdf_domain_leading_zero",
		  .config = "identity=on\nbdfs=00000:01:00.0\n", .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_bdf_duplicate",
		  .config = "identity=on\nbdfs=0000:01:00.0,0000:01:00.0\n", .version = V580,
		  .log_has = "invalid" },
		{ .name = "invalid_bdf_trailing_comma", .config = "identity=on\nbdfs=0000:01:00.0,\n",
		  .version = V580, .log_has = "invalid" },
		{ .name = "invalid_too_many_bdfs", .config = too_many, .version = V580, .log_has = "invalid" },
		{ .name = "invalid_too_large", .config = big, .version = V580,
		  .log_has = "larger than 65536 bytes" },
		{ .name = "invalid_group_writable", .config = ON, .config_mode = 0664,
		  .version = V580, .log_has = "writable by group/others" },
		{ .name = "invalid_config_is_directory", .config_is_dir = true, .version = V580,
		  .log_has = "not a regular file" },

		/* Inert: driver. */
		{ .name = "driver_unlisted_build", .config = ON,
		  .version = "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.159.03  Fri Apr 24 2026\n",
		  .log_has = "driver 580.159.03 is not supported" },
		{ .name = "driver_version_extra_component", .config = ON,
		  .version = "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.159.01.1  x\n",
		  .log_has = "driver version unknown" },
		{ .name = "driver_version_garbage", .config = ON, .version = "580.159.01\n",
		  .log_has = "driver version unknown" },
		{ .name = "driver_version_missing", .config = ON, .log_has = "driver version unknown" },
		/* No next ioctl: the hook forwards every call to the kernel instead of failing it. */
		{ .name = "next_ioctl_missing_forwards_to_kernel", .config = ON, .version = V580,
		  .no_next_ioctl = true, OPS(ops_tty), .log_has = "every ioctl goes straight to the kernel" },

		/* Inert: process. */
		{ .name = "manager_process", .config = FORCE_ON, .version = V580, .manager = true },
	};

	for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
		run(&scenarios[i]);

	nftw(tmp, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
