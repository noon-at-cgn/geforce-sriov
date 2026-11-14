// SPDX-License-Identifier: MIT
/*
 * The NVIDIA GPUs that are not selected: the PCI functions NVIDIA's driver matches through its three
 * class aliases (display VGA, 3D controller, other display) that the configuration does not list.
 * `status` prints what the scan returns. The release step at the end writes to sysfs and to NVIDIA's /proc entry.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gs_lifecycle.h"

#define CLASS_VGA 0x0300u
#define CLASS_3D 0x0302u
#define CLASS_BRIDGE_OTHER 0x0680u /* NVIDIA's driver also matches this class, as the autoload file's bc06sc80 line does */

static int nvidia_gpu_class(unsigned int class_code)
{
	unsigned int c = class_code >> 8;

	return c == CLASS_VGA || c == CLASS_3D || c == CLASS_BRIDGE_OTHER;
}

unsigned int gs_unselected_scan(const char *sysroot, const struct gs_addr *sel, unsigned int nsel,
				struct gs_unselected *out, unsigned int max)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX];
	struct dirent *de;
	unsigned int n = 0;
	DIR *d;

	if (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices", sysroot) || !(d = opendir(dir)))
		return 0;
	while ((de = readdir(d)) && n < max) {
		struct gs_addr a;
		struct gs_card c;

		if (de->d_name[0] == '.' || gs_addr_parse(de->d_name, &a) || gs_addr_listed(sel, nsel, a.str))
			continue;
		gs_card_inspect(sysroot, &a, &c);
		if (!c.present || c.vendor != GS_VENDOR_NVIDIA || c.is_vf || !nvidia_gpu_class(c.class_code))
			continue;
		out[n].pf = a;
		out[n].device = c.device;
		out[n].entry = c.entry;
		out[n].boot_vga = c.boot_vga;
		gs_copy(out[n].driver, sizeof(out[n].driver), c.driver);
		out[n].override[0] = '\0';
		if (!gs_pathf(p, sizeof(p), "%s/%s/driver_override", dir, a.str) &&
		    gs_read_attr(p, out[n].override, sizeof(out[n].override)) == 0 &&
		    strcmp(out[n].override, "(null)") == 0)
			out[n].override[0] = '\0';
		n++;
	}
	closedir(d);
	for (unsigned int i = 1; i < n; i++)
		for (unsigned int j = i; j > 0 && strcmp(out[j - 1].pf.str, out[j].pf.str) > 0; j--) {
			struct gs_unselected tmp = out[j];

			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	return n;
}

/* The first line of an entry: address, ID, model, driver, driver_override, boot display. */
static void entry_head(const struct gs_unselected *u, char *out, size_t len)
{
	int n = snprintf(out, len, "%s: 10de:%04x %s driver %s", u->pf.str, u->device,
			 u->entry ? u->entry->name : "(not in device table)", u->driver[0] ? u->driver : "none");

	if (n > 0 && (size_t)n < len && u->override[0])
		n += snprintf(out + n, len - (size_t)n, ", driver_override=%s", u->override);
	if (n > 0 && (size_t)n < len && u->boot_vga == 1)
		snprintf(out + n, len - (size_t)n, ", boot display");
}

/* What it means for the entry: one sentence in the journal, the sentences and the undo on lines of their own in status. */
static void entry_meaning(const struct gs_unselected *u, char *out, size_t len, int one_line)
{
	if (strcmp(u->driver, "nvidia") == 0) {
		if (u->entry)
			snprintf(out, len, "Bound by NVIDIA's driver and not activated. `sudo geforce-sriov install --device %s` selects it.",
				 u->pf.str);
		else
			snprintf(out, len, "Bound by NVIDIA's driver and not activated.");
	} else if (u->driver[0]) {
		snprintf(out, len, "Kept by %s (your configuration). geforce-sriov does not touch it.", u->driver);
	} else if (strcmp(u->override, GS_UNSELECTED_RELEASED) == 0) {
		if (one_line)
			snprintf(out, len, "NVIDIA's driver could not start this GPU, so geforce-sriov released it. No driver binds it.");
		else
			snprintf(out, len,
				 "NVIDIA's driver could not start this GPU, so geforce-sriov released it. No driver binds it.\n"
				 "  To ask NVIDIA's driver to bind it again until the next boot, run: `echo > /sys/bus/pci/devices/%s/driver_override && echo %s > /sys/bus/pci/drivers_probe`\n"
				 "  A card that driver still cannot start stays without a driver. With VFs on the host, run `geforce-sriov check` afterwards. If it says NOT READY, stop the VMs and run `geforce-sriov activate`.\n"
				 "  To keep it on NVIDIA's driver at every boot, write `nvidia` to its driver_override with a udev rule.",
				 u->pf.str, u->pf.str);
	} else if (u->override[0]) {
		snprintf(out, len, "No driver is bound, and driver_override=%s lets only a driver of that name bind it. geforce-sriov does not touch it.",
			 u->override);
	} else {
		snprintf(out, len, "No driver is bound. geforce-sriov does not touch it.");
	}
}

void gs_unselected_print(const struct gs_unselected *u, int one_line)
{
	char head[256], meaning[640];

	entry_head(u, head, sizeof(head));
	entry_meaning(u, meaning, sizeof(meaning), one_line);
	if (one_line)
		printf("NVIDIA GPU not selected, %s; %s\n", head, meaning);
	else
		printf("%s\n  %s\n", head, meaning);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One host write with its deadline. */
static int host_write(const struct gs_host *h, const char *path, const char *value)
{
	gs_notify_extend(SYSFS_DEADLINE_MS);
	if (h->write_attr_deadline)
		return h->write_attr_deadline(h, path, value, SYSFS_DEADLINE_MS);
	return h->write_attr ? h->write_attr(path, value) : -ENOSYS;
}

/* 1 when some NVIDIA function is a VF: a PF unbind then puts other cards' VFs back to runtime suspend. */
static int nvidia_vfs_exist(const char *sysroot)
{
	char dir[GS_PATH_MAX];
	struct dirent *de;
	int found = 0;
	DIR *d;

	if (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices", sysroot) || !(d = opendir(dir)))
		return 0;
	while (!found && (de = readdir(d))) {
		struct gs_addr a;
		struct gs_card c;

		if (de->d_name[0] == '.' || gs_addr_parse(de->d_name, &a))
			continue;
		gs_card_inspect(sysroot, &a, &c);
		found = c.present && c.vendor == GS_VENDOR_NVIDIA && c.is_vf;
	}
	closedir(d);
	return found;
}

/* The journal line of one tested card: "<bdf> (10de:<id>): <what>". */
#define LINE(u, ...)                                                      \
	do {                                                              \
		printf("%s (10de:%04x): ", (u)->pf.str, (u)->device);     \
		printf(__VA_ARGS__);                                      \
		printf("\n");                                             \
	} while (0)

/* The card's device minor from NVIDIA's /proc entry; -1 when it has none. */
static long card_minor(const struct gs_host *h, const struct gs_unselected *u)
{
	char p[GS_PATH_MAX];

	if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/information", h->proc, u->pf.str))
		return -1;
	return gs_proc_field(p, "Device Minor");
}

/* Undo of the override after a release that did not complete. */
static void clear_override(const struct gs_host *h, const struct gs_unselected *u)
{
	char p[GS_PATH_MAX];

	if (!gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver_override", h->sys, u->pf.str))
		host_write(h, p, "\n");
}

static void give_back_lock(const struct gs_host *h, const char *lock)
{
	host_write(h, lock, "0");
}

/*
 * One candidate: NVIDIA's driver bound it; ask the driver whether it can start it. The node is made when it
 * is missing, and opened once. EIO is the driver's refusal; then the card is released.
 */
static int test_card(const struct gs_host *h, const struct gs_unselected *u)
{
	char node[GS_PATH_MAX], lock[GS_PATH_MAX], ovr[GS_PATH_MAX], link[GS_PATH_MAX], val[16] = "";
	const char *root = h->root ? h->root : "";
	char unbind[GS_PATH_MAX], minor_s[16];
	long minor = card_minor(h, u);
	int r, existed;
	struct stat st;

	if (minor < 0 || minor >= (long)GS_NVIDIA_CTL_MINOR) {
		LINE(u, "cannot test: NVIDIA's driver gives it no device minor; left on nvidia");
		return 0;
	}
	if (gs_pathf(node, sizeof(node), "%s/dev/nvidia%ld", root, minor)) {
		LINE(u, "cannot test: the node path is too long; left on nvidia");
		return 0;
	}
	existed = lstat(node, &st) == 0;
	if (!existed) {
		const char *const argv[] = { "nvidia-modprobe", "-c", minor_s, NULL };

		snprintf(minor_s, sizeof(minor_s), "%ld", minor);
		r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
		if (r || lstat(node, &st)) {
			LINE(u, "cannot test: nvidia-modprobe -c %ld did not make /dev/nvidia%ld; left on nvidia", minor, minor);
			return 0;
		}
	}
	gs_notify_extend(GS_UNSELECTED_OPEN_MS);
	r = h->open_node ? h->open_node(h, node, GS_UNSELECTED_OPEN_MS) : -ENOSYS;
	if (r == 0) {
		LINE(u, "NVIDIA's driver started this GPU (open /dev/nvidia%ld worked); left on nvidia", minor);
		return 0;
	}
	if (r == -ETIMEDOUT) {
		LINE(u, "cannot test: open /dev/nvidia%ld did not finish within %u s; left on nvidia", minor,
		     GS_UNSELECTED_OPEN_MS / 1000);
		return 0;
	}
	if (r != -EIO) {
		LINE(u, "cannot test: open /dev/nvidia%ld: %s; left on nvidia", minor, strerror(-r));
		return 0;
	}
	/* The driver refused it. The lock is the driver's own answer to whether anything holds the GPU. */
	if (gs_pathf(lock, sizeof(lock), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, u->pf.str) ||
	    gs_pathf(ovr, sizeof(ovr), "%s/bus/pci/devices/%s/driver_override", h->sys, u->pf.str) ||
	    gs_pathf(link, sizeof(link), "%s/bus/pci/devices/%s/driver", h->sys, u->pf.str) ||
	    gs_pathf(unbind, sizeof(unbind), "%s/bus/pci/drivers/nvidia/unbind", h->sys)) {
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but a path is too long; left on nvidia",
		     minor, strerror(EIO));
		return 0;
	}
	r = host_write(h, lock, "1");
	if (r == -ETIMEDOUT) {
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but asking it for unbindLock did not finish within %u s; left as it is",
		     minor, strerror(EIO), SYSFS_DEADLINE_MS / 1000);
		return 1;
	}
	if (r) {
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but writing unbindLock failed: %s; left on nvidia",
		     minor, strerror(EIO), strerror(-r));
		return 0;
	}
	if (gs_read_attr(lock, val, sizeof(val)) || strcmp(val, "1") != 0) {
		give_back_lock(h, lock);
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but it did not grant unbindLock, so a program holds the GPU; left on nvidia",
		     minor, strerror(EIO));
		return 0;
	}
	r = host_write(h, ovr, GS_UNSELECTED_RELEASED);
	if (r) {
		give_back_lock(h, lock);
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but writing driver_override failed: %s; left on nvidia",
		     minor, strerror(EIO), r == -ETIMEDOUT ? "no answer within the time limit" : strerror(-r));
		return r == -ETIMEDOUT;
	}
	r = host_write(h, unbind, u->pf.str);
	if (r == -ETIMEDOUT) {
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but the unbind did not finish within %u s; its state is unknown, driver_override=%s is set",
		     minor, strerror(EIO), SYSFS_DEADLINE_MS / 1000, GS_UNSELECTED_RELEASED);
		return 1;
	}
	if (lstat(link, &st) == 0) {
		clear_override(h, u);
		give_back_lock(h, lock);
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s), but the unbind failed%s%s; left on nvidia",
		     minor, strerror(EIO), r ? ": " : "", r ? strerror(-r) : "");
		return 0;
	}
	/* The node of a GPU no driver serves any more is stale. */
	if (unlink(node) && errno != ENOENT)
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s). Released: no driver binds it now (driver_override=%s); /dev/nvidia%ld could not be removed: %s",
		     minor, strerror(EIO), GS_UNSELECTED_RELEASED, minor, strerror(errno));
	else
		LINE(u, "NVIDIA's driver cannot start this GPU (open /dev/nvidia%ld: %s). Released: no driver binds it now (driver_override=%s)",
		     minor, strerror(EIO), GS_UNSELECTED_RELEASED);
	return 0;
}

void gs_unselected_release(const struct gs_host *h, const struct gs_addr *sel, unsigned int nsel)
{
	struct gs_unselected u[GS_UNSELECTED_MAX];
	unsigned int n = gs_unselected_scan(h->sys, sel, nsel, u, GS_UNSELECTED_MAX);
	int vfs = 0, vfio = 0, tested = 0, stuck = 0;
	long start = now_ms();
	char mark[GS_PATH_MAX];
	int have_mark;

	for (unsigned int i = 0; i < n; i++) {
		int candidate = strcmp(u[i].driver, "nvidia") == 0 && !u[i].override[0] && !u[i].entry;

		if (!candidate) {
			gs_unselected_print(&u[i], 1);
			continue;
		}
		if (u[i].boot_vga == 1) {
			printf("NVIDIA GPU not selected, %s (10de:%04x): not tested; it is the boot display GPU. Left on nvidia\n",
			       u[i].pf.str, u[i].device);
			continue;
		}
		/*
		 * A card is opened once per boot: a rerun of activation after one that stopped before it finished
		 * must not open it again.
		 */
		have_mark = h->run_dir && !gs_pathf(mark, sizeof(mark), "%s/unselected-tested-%s", h->run_dir, u[i].pf.str);
		if (have_mark && gs_exists(mark)) {
			LINE(&u[i], "not tested again: it was tested earlier in this boot; left as it is");
			continue;
		}
		if (!tested++) {
			vfs = nvidia_vfs_exist(h->sys);
			vfio = gs_module_loaded(h, "nvidia_vgpu_vfio");
		}
		if (vfio) {
			LINE(&u[i], "not tested: nvidia_vgpu_vfio is loaded; left on nvidia");
			continue;
		}
		if (vfs) {
			LINE(&u[i], "not tested: VFs exist on this host, and unbinding a GPU would put their power management back to auto; left on nvidia");
			continue;
		}
		if (stuck) {
			LINE(&u[i], "not tested: a write to NVIDIA's driver for an earlier card did not finish; left on nvidia");
			continue;
		}
		if (now_ms() - start >= (long)GS_UNSELECTED_BUDGET_MS) {
			LINE(&u[i], "not tested: the step used its %u s; left on nvidia", GS_UNSELECTED_BUDGET_MS / 1000);
			continue;
		}
		/* The marker is made before the open, so an interrupted test counts as made. */
		if (have_mark) {
			int fd;

			gs_mkdir_p(h->run_dir, 0755);
			fd = open(mark, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
			if (fd < 0) {
				LINE(&u[i], "not tested: cannot record the test in %s: %s; left on nvidia", h->run_dir, strerror(errno));
				continue;
			}
			close(fd);
		}
		stuck = test_card(h, &u[i]);
	}
}
