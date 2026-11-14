// SPDX-License-Identifier: MIT
/*
 * CLI and lifecycle tests for geforce-sriov 0.4.1.
 *
 * Fixture sysfs trees are built in a temporary directory; only read-only commands and DESTDIR
 * staging run against them. The activation flow is exercised in-process against a simulated
 * kernel behind struct gs_host: its writes and commands move fixture files, and the deadline
 * callbacks can inject a hang (a bounded operation whose completion is uncertain).
 * Environment: GS_CLI = built geforce-sriov binary, GS_SRC = source tree.
 */
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gs_lifecycle.h"
#include "tmpdir.h"

static int failures, checks, skips;
static char tmp[256];
static const char *cli, *srcdir;

#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

/*
 * A check that documents a confirmed product bug: the behaviour the spec requires is stated once,
 * the suite records that it was not asserted. Used only where the report names file and line.
 */
#define SKIP_BUG(what, ...) do { \
		skips++; \
		fprintf(stderr, "SKIP (product bug: " what ") %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} while (0)

static void die(const char *what) __attribute__((noreturn));

static void die(const char *what)
{
	perror(what);
	exit(2);
}
static void writef(const char *path, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void writef(const char *path, const char *fmt, ...)
{
	char dir[GS_PATH_MAX], *slash;
	va_list ap;
	FILE *f;

	gs_pathf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (gs_mkdir_p(dir, 0755))
			die(dir);
	}
	f = fopen(path, "w");
	if (!f)
		die(path);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

/* Binary content (a fixture config space). */
static void write_bin(const char *path, const void *data, size_t len)
{
	char dir[GS_PATH_MAX], *slash;
	FILE *f;

	gs_pathf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (gs_mkdir_p(dir, 0755))
			die(dir);
	}
	f = fopen(path, "wb");
	if (!f)
		die(path);
	if (fwrite(data, 1, len, f) != len)
		die(path);
	fclose(f);
}

static void link_to(const char *target, const char *path)
{
	unlink(path);
	if (symlink(target, path))
		die(path);
}

static char *slurp(const char *path)
{
	char *data;
	size_t len;

	if (gs_read_whole(path, &data, &len, 1 << 22))
		return NULL;
	return data;
}

static int contains(const char *hay, const char *needle)
{
	return hay && needle && strstr(hay, needle) != NULL;
}

/* -1 when absent. */
static long pos_of(const char *hay, const char *needle)
{
	const char *p;

	if (!hay || !needle || !(p = strstr(hay, needle)))
		return -1;
	return (long)(p - hay);
}

/* -1 when absent. */
static long pos_of_last(const char *hay, const char *needle)
{
	const char *p, *last = NULL;
	size_t len;

	if (!hay || !needle || !(len = strlen(needle)))
		return -1;
	for (p = hay; (p = strstr(p, needle)); p += len)
		last = p;
	return last ? (long)(last - hay) : -1;
}

static int count_of(const char *hay, const char *needle)
{
	int n = 0;
	size_t len;

	if (!hay || !needle || !(len = strlen(needle)))
		return 0;
	for (const char *p = hay; (p = strstr(p, needle)); p += len)
		n++;
	return n;
}

static int ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Everything the child writes to fd until it exits; returns its exit status. */
static int collect(int fd, pid_t pid, char **out)
{
	size_t cap = 65536, len = 0;
	char *buf = malloc(cap);
	int status;

	if (!buf)
		die("malloc");
	for (;;) {
		ssize_t r;

		if (len + 1 >= cap) {
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf)
				die("realloc");
		}
		r = read(fd, buf + len, cap - len - 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	buf[len] = '\0';
	close(fd);
	waitpid(pid, &status, 0);
	*out = buf;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/*
 * The golden trace (`make trace`): with GS_TRACE_DIR set, every CLI run and every run against the
 * simulated host saves what it printed, and the simulated host also saves its log of writes and
 * commands. Files are named after the running section and numbered within it; the temporary
 * directory, the CLI and the source tree are replaced by fixed words, so two runs of the same
 * sources give identical directories and `make trace-diff` shows what a change did.
 */
static const char *trace_section(void);
static unsigned int trace_seq;

/* Runs are numbered within their section, so a new fixture does not rename the files after it. */
static void trace_next(void)
{
	static char last[96];

	if (strcmp(last, trace_section()) != 0) {
		snprintf(last, sizeof(last), "%s", trace_section());
		trace_seq = 0;
	}
	trace_seq++;
}

static void trace_replace(FILE *f, const char *text)
{
	const struct { const char *from, *to; } map[] = { { cli, "@CLI@" }, { srcdir, "@SRC@" }, { tmp, "@TMP@" } };

	while (*text) {
		size_t k;

		for (k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
			size_t n = map[k].from ? strlen(map[k].from) : 0;

			if (n && strncmp(text, map[k].from, n) == 0) {
				fputs(map[k].to, f);
				text += n;
				break;
			}
		}
		if (k == sizeof(map) / sizeof(map[0]))
			fputc(*text++, f);
	}
}

/* One trace file of this run: kind is "out", "log" or "cli"; rc < 0 writes no exit line. */
static void trace_put(const char *kind, int rc, const char *text)
{
	const char *dir = getenv("GS_TRACE_DIR");
	char p[GS_PATH_MAX], name[128];
	FILE *f;

	if (!dir || !dir[0])
		return;
	snprintf(name, sizeof(name), "%s", trace_section());
	for (char *c = name; *c; c++)
		if (*c == '/' || *c == ' ')
			*c = '_';
	if (gs_pathf(p, sizeof(p), "%s/%s.%02u.%s", dir, name, trace_seq, kind) || !(f = fopen(p, "w")))
		die("GS_TRACE_DIR");
	if (rc >= 0)
		fprintf(f, "exit=%d\n", rc);
	trace_replace(f, text ? text : "");
	fclose(f);
}

/* Run the CLI; returns exit status, output (stdout+stderr) in *out (caller frees). */
static int run_cli(char **out, const char *destdir, const char *const args[]) __attribute__((unused));
static int run_cli(char **out, const char *destdir, const char *const args[])
{
	const char *argv[8 + 2 * GS_MAX_PFS + 8];
	int pipefd[2];
	size_t n;
	pid_t pid;

	argv[0] = cli;
	for (n = 0; args[n]; n++)
		argv[n + 1] = args[n];
	argv[n + 1] = NULL;
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		unsetenv(GS_CONFIG_ENV);
		if (destdir)
			setenv("DESTDIR", destdir, 1);
		else
			unsetenv("DESTDIR");
		execv(cli, (char *const *)argv);
		_exit(127);
	}
	close(pipefd[1]);
	{
		int rc = collect(pipefd[0], pid, out);

		trace_next();
		trace_put("cli", rc, *out);
		return rc;
	}
}

/* ---- fixture ---------------------------------------------------------- */

struct fx_fn {
	const char *bdf;
	const char *device;
	const char *class_code;
	const char *driver; /* NULL: unbound */
};

/* Where a card sits: its immediate bridge (path below root/devices, ending in the bridge's name) and its PF. */
struct fx_topo {
	const char *bridge_path;
	const char *pf;
};

static const struct fx_topo topo_default = { "pci0000:00/0000:00:02.0", "0000:03:00.0" };
static const struct fx_topo topo_display = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };

/*
 * A PCI config space of 256 bytes with the capability list at 0x40: a PCIe capability whose
 * DevCap2 has ARI Forwarding Supported (bit 5 of the dword at 0x64) when ari_fwd, then MSI-X at
 * 0x80 when msix. Both walkers read no more than this: msix_linked() and bridge_ari_forwarding().
 */
static void put_config(const char *dir, int msix, int ari_fwd)
{
	unsigned char cfg[256] = { 0 };
	unsigned int devcap2 = ari_fwd ? 1u << 5 : 0;
	char p[GS_PATH_MAX];

	cfg[0x06] = 0x10;               /* status: capability list present */
	cfg[0x34] = 0x40;               /* first capability */
	cfg[0x40] = 0x10;               /* PCI Express */
	cfg[0x41] = msix ? 0x80 : 0x00; /* next capability */
	cfg[0x64] = (unsigned char)(devcap2 & 0xff);
	cfg[0x65] = (unsigned char)((devcap2 >> 8) & 0xff);
	cfg[0x66] = (unsigned char)((devcap2 >> 16) & 0xff);
	cfg[0x67] = (unsigned char)((devcap2 >> 24) & 0xff);
	cfg[0x80] = 0x11;               /* MSI-X */
	cfg[0x81] = 0x00;
	gs_pathf(p, sizeof(p), "%s/config", dir);
	write_bin(p, cfg, sizeof(cfg));
}

/* <dev>/resource: 13 lines. BAR0 assigned; VF BAR rows 7-12 assigned when vf_bars. */
static void put_resource(const char *dir, int vf_bars)
{
	char text[13 * 128];
	char p[GS_PATH_MAX];
	size_t off = 0;

	off += (size_t)snprintf(text + off, sizeof(text) - off,
				"0x0000000038000000 0x0000000038000fff 0x0000000000040200\n");
	for (unsigned int i = 1; i <= 12; i++) {
		if (i >= 7 && vf_bars) {
			unsigned long long start = 0x387fc0000000ull + (i - 7) * 0x1000000ull;

			off += (size_t)snprintf(text + off, sizeof(text) - off, "0x%016llx 0x%016llx 0x000000000014220c\n",
						start, start + 0xffffffull);
		} else {
			off += (size_t)snprintf(text + off, sizeof(text) - off,
						"0x0000000000000000 0x0000000000000000 0x0000000000000000\n");
		}
	}
	gs_pathf(p, sizeof(p), "%s/resource", dir);
	write_bin(p, text, off);
}

/* The bridge-scoped rescan file of the bus the PF sits on ("0000:03"). */
static void make_rescan_file(const char *root, const struct fx_topo *k)
{
	char bridge[GS_PATH_MAX], p[GS_PATH_MAX], bus[GS_BUS_STR_SIZE];
	struct gs_addr a;

	gs_addr_parse(k->pf, &a);
	snprintf(bus, sizeof(bus), "%04x:%02x", a.domain, a.bus);
	gs_pathf(bridge, sizeof(bridge), "%s/devices/%s", root, k->bridge_path);
	gs_pathf(p, sizeof(p), "%s/pci_bus/%s/rescan", bridge, bus);
	writef(p, "%s", "");
}

/* The bridge function: vendor/device/class, power, windows and a config space. */
static void make_bridge(const char *root, const char *bridge_path)
{
	char bridge[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX];
	const char *bname = strrchr(bridge_path, '/');

	bname = bname ? bname + 1 : bridge_path;
	gs_pathf(bridge, sizeof(bridge), "%s/devices/%s", root, bridge_path);
	gs_pathf(p, sizeof(p), "%s/vendor", bridge);
	writef(p, "0x8086\n");
	gs_pathf(p, sizeof(p), "%s/device", bridge);
	writef(p, "0x1901\n");
	gs_pathf(p, sizeof(p), "%s/class", bridge);
	writef(p, "0x060400\n");
	gs_pathf(p, sizeof(p), "%s/power/control", bridge);
	writef(p, "auto\n");
	gs_pathf(p, sizeof(p), "%s/power/runtime_status", bridge);
	writef(p, "suspended\n");
	gs_pathf(p, sizeof(p), "%s/resource", bridge);
	writef(p, "0x0000000000001000 0x0000000000001fff 0x0000000000000101\n"
		  "0x0000000090000000 0x0000000091ffffff 0x0000000000000200\n"
		  "0x0000000380000000 0x00000003d9ffffff 0x0000000000002200\n");
	put_config(bridge, 0, 1);
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/nvidia", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/snd_hda_intel", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/pci-pf-stub", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/xhci_hcd", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/platform/drivers/simple-framebuffer", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/class/drm", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/class/graphics", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/class/vtconsole", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", root, bname);
	gs_pathf(q, sizeof(q), "../../../devices/%s", bridge_path);
	link_to(q, p);
}

/* A GPU (+ functions) at k->pf behind the bridge k->bridge_path; several cards may share one root. */
static void make_card(const char *root, const struct fx_topo *k, const struct fx_fn *fns, int boot_vga,
		      const char *sriov)
{
	char bridge[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX];

	gs_pathf(bridge, sizeof(bridge), "%s/devices/%s", root, k->bridge_path);
	make_bridge(root, k->bridge_path);
	make_rescan_file(root, k);
	for (; fns->bdf; fns++) {
		char dev[GS_PATH_MAX];
		int is_pf = strcmp(fns->bdf, k->pf) == 0;

		gs_pathf(dev, sizeof(dev), "%s/%s", bridge, fns->bdf);
		gs_pathf(p, sizeof(p), "%s/vendor", dev);
		writef(p, "0x%s\n", strncmp(fns->device, "8086", 4) == 0 ? "8086" : "10de");
		gs_pathf(p, sizeof(p), "%s/device", dev);
		writef(p, "0x%s\n", strncmp(fns->device, "8086:", 5) == 0 ? fns->device + 5 : fns->device);
		gs_pathf(p, sizeof(p), "%s/class", dev);
		writef(p, "0x%s\n", fns->class_code);
		gs_pathf(p, sizeof(p), "%s/subsystem_vendor", dev);
		writef(p, "0x10de\n");
		gs_pathf(p, sizeof(p), "%s/subsystem_device", dev);
		writef(p, "0x146d\n");
		if (is_pf && fns->driver && strcmp(fns->driver, "nvidia") == 0) {
			/* NVIDIA's driver publishes one unbindLock per GPU it is bound to. */
			gs_pathf(p, sizeof(p), "%s/proc/driver/nvidia/gpus/%s/unbindLock", root, fns->bdf);
			writef(p, "0\n");
		}
		if (is_pf) {
			gs_pathf(p, sizeof(p), "%s/boot_vga", dev);
			writef(p, "%d\n", boot_vga);
			gs_pathf(p, sizeof(p), "%s/ari_enabled", dev);
			writef(p, "%d\n", 0);
			if (sriov) {
				unsigned int total = 0, num = 0;

				sscanf(sriov, "%u/%u", &num, &total);
				gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", dev);
				writef(p, "%u\n", total);
				gs_pathf(p, sizeof(p), "%s/sriov_numvfs", dev);
				writef(p, "%u\n", num);
			}
		}
		gs_pathf(p, sizeof(p), "%s/power/control", dev);
		writef(p, "auto\n");
		gs_pathf(p, sizeof(p), "%s/power/runtime_status", dev);
		writef(p, "suspended\n");
		put_resource(dev, 0);
		put_config(dev, 1, is_pf);
		if (fns->driver) {
			gs_pathf(p, sizeof(p), "%s/driver", dev);
			gs_pathf(q, sizeof(q), "../../../../bus/pci/drivers/%s", fns->driver);
			link_to(q, p);
		}
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", root, fns->bdf);
		gs_pathf(q, sizeof(q), "../../../devices/%s/%s", k->bridge_path, fns->bdf);
		link_to(q, p);
	}
}

/* Replace a card's resource file: VF BAR rows assigned (1) or zero. */
static void set_vf_bars(const char *root, const struct fx_topo *k, const char *bdf, int assigned)
{
	char dev[GS_PATH_MAX];

	gs_pathf(dev, sizeof(dev), "%s/devices/%s/%s", root, k->bridge_path, bdf);
	put_resource(dev, assigned);
}

/*
 * Bridge windows (and the room above them): growth_room() reads the bridge's parent (here the
 * root bus 0000:00) minus its children's windows, so a small bridge under a large root can grow
 * on rescan. Values in MiB; 0 leaves that resource out.
 */
static void set_bridge_windows(const char *root, const struct fx_topo *k, unsigned int np_mib, unsigned int pref_mib)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/devices/%s/resource", root, k->bridge_path);
	/* Rewrite in one go: every writef() truncates. */
	{
		char text[3 * 128];

		snprintf(text, sizeof(text), "0x0000000000001000 0x0000000000001fff 0x0000000000000101\n");
		if (np_mib)
			snprintf(text + strlen(text), sizeof(text) - strlen(text),
				 "0x0000000090000000 0x%016llx 0x0000000000000200\n",
				 0x90000000ull + (unsigned long long)np_mib * 0x100000ull - 1);
		if (pref_mib)
			snprintf(text + strlen(text), sizeof(text) - strlen(text),
				 "0x%016llx 0x%016llx 0x0000000000002200\n", 0x380000000ull,
				 0x380000000ull + (unsigned long long)pref_mib * 0x100000ull - 1);
		write_bin(p, text, strlen(text));
	}
}

static void set_root_windows(const char *root, unsigned int mem_mib, unsigned int pref_mib)
{
	char p[GS_PATH_MAX], text[3 * 128];

	gs_pathf(p, sizeof(p), "%s/devices/pci0000:00/resource", root);
	snprintf(text, sizeof(text), "0x0000000000000000 0x0000000000000fff 0x0000000000000101\n");
	if (mem_mib)
		snprintf(text + strlen(text), sizeof(text) - strlen(text),
			 "0x0000000080000000 0x%016llx 0x0000000000000200\n",
			 0x80000000ull + (unsigned long long)mem_mib * 0x100000ull - 1);
	if (pref_mib)
		snprintf(text + strlen(text), sizeof(text) - strlen(text),
			 "0x0000000000000000 0x%016llx 0x0000000000002200\n",
			 (unsigned long long)pref_mib * 0x100000ull - 1);
	write_bin(p, text, strlen(text));
}

/* The PF's firmware framebuffer: a simple-framebuffer platform device child of the PF. */
static void add_firmware_fb(const char *root, const struct fx_topo *k, const char *name, const char *driver)
{
	char dev[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX];

	gs_pathf(dev, sizeof(dev), "%s/devices/%s/%s/%s", root, k->bridge_path, k->pf, name);
	gs_pathf(p, sizeof(p), "%s/subsystem", dev);
	writef(p, "platform\n");
	if (driver) {
		gs_pathf(p, sizeof(p), "%s/driver", dev);
		gs_pathf(q, sizeof(q), "../../../../bus/platform/drivers/%s", driver);
		link_to(q, p);
	}
	/* What /sys/bus/platform/devices lists: the platform device the kernel created for it. */
	{
		char dir[GS_PATH_MAX], link[GS_PATH_MAX];

		gs_pathf(dir, sizeof(dir), "%s/bus/platform/devices", root);
		gs_mkdir_p(dir, 0755);
		gs_pathf(link, sizeof(link), "%s/%s", dir, name);
		gs_pathf(q, sizeof(q), "../../../devices/%s/%s/%s", k->bridge_path, k->pf, name);
		link_to(q, link);
	}
}

/* /proc/iomem with the PF's BAR containing BOOTFB, as a bound firmware framebuffer leaves it. */
static void add_iomem_fw(const char *root, const struct fx_topo *k, int bootfb)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/proc/iomem", root);
	writef(p, "00000000-00000fff : Reserved\n"
		  "80000000-fbffffff : PCI Bus 0000:00\n"
		  "  90000000-91ffffff : PCI Bus 0000:03\n"
		  "    90000000-90ffffff : %s\n"
		  "%s",
	       k->pf, bootfb ? "      90000000-9007ffff : BOOTFB\n" : "");
	gs_pathf(p, sizeof(p), "%s/proc/consoles", root);
	writef(p, "tty0                 -WU (EC p  )    4:7\nnetcon0              EW    (EC    )\n");
	gs_pathf(p, sizeof(p), "%s/proc/cmdline", root);
	writef(p, "BOOT_IMAGE=/vmlinuz root=/dev/sda1\n");
}

/* ---- the simulated kernel ---------------------------------------------- */

/*
 * The rules of the model, and where each comes from. MEASURED: seen on the hardware this project
 * runs on. KNOWN: documented behaviour of Linux or kmod. ASSUMED: a choice of the fixtures, not
 * established; a test result must not depend on it unless the test says so.
 *
 * | Rule                                                                         | Source   |
 * |------------------------------------------------------------------------------|----------|
 * | A sysfs write changes an attribute that exists and never creates a file      | MEASURED |
 * | Boolean module parameters read back as Y or N                                | MEASURED |
 * | A restart of nvidia-vgpud or nvidia-vgpu-mgr sets every vGPU type to 0       | MEASURED |
 * | udev loads nvidia_drm for an NVIDIA display device through its PCI class     | MEASURED |
 * |   alias when the device appears; `blacklist` and `install` lines do not stop |          |
 * |   that load, an `alias <pattern> off` line does (sim.udev)                   |          |
 * | A module loaded that way has no parameters from a command line               | MEASURED |
 * | modprobe loads what a module depends on first                                | KNOWN    |
 * | modprobe runs an `install NAME command` line instead of loading, unless      | KNOWN    |
 * |   --ignore-install is given                                                  |          |
 * | rmmod refuses a module that another module holds or whose refcount is not 0, | KNOWN    |
 * |   and unloading drops the refcount of what the module depended on            |          |
 * | nvidia_drm needs nvidia_modeset and nvidia; nvidia_modeset needs nvidia;      | MEASURED |
 * |   nvidia_uvm needs nvidia; nvidia_vgpu_vfio needs none of NVIDIA's modules   |          |
 * |   (`modinfo -F depends` of the 580.159.01 build on the project's host)       |          |
 * | Unloading nvidia unbinds its devices and removes /proc/driver/nvidia;        | KNOWN    |
 * |   loading it binds every NVIDIA display PF that has no driver and whose      |          |
 * |   driver_override is empty or names nvidia (pci_match_device)                |          |
 * | NVIDIA's unbindLock is denied while a client holds the GPU; which client is  | ASSUMED  |
 * |   named by each fixture (sim.lock_holder)                                    |          |
 * | Whether a program has nvidia open when activation starts. Both are possible  | ASSUMED  |
 * |   host states and neither is established for the boot path. sim_module()     |          |
 * |   gives nvidia one reference for such a program; sim_core_unused() takes it  |          |
 * |   away, and the display fixtures run both ways                               |          |
 * | A rescan presents SR-IOV on a card the loaded livepatch module names, with   | ASSUMED  |
 * |   no regard to whether the livepatch has finished its transition             |          |
 * | sriov-manage -e creates the VFs and binds them at once                       | ASSUMED  |
 * | Everything is synchronous: nothing appears later than the call that causes it| ASSUMED  |
 *
 * Not modelled: why a first VF can fail to start on a non-boot Ada card with KMS (NVOS status 0x51). Its
 * mechanism is unknown, and no fixture claims to reproduce it; display=boot is the way back.
 */

static struct {
	char root[GS_PATH_MAX];
	char proc[GS_PATH_MAX], state[GS_PATH_MAX], run[GS_PATH_MAX];
	char display_pf[GS_BDF_STR_SIZE]; /* the card nvidia_drm registers after loading */
	/* Injection points. A suffix match keeps them readable in the tests. */
	const char *write_fail;    /* a write to a path ending in this fails with EIO */
	const char *write_lie;     /* a write to this path stores "auto" instead of the value */
	const char *write_timeout; /* a write to this path returns -ETIMEDOUT once */
	const char *run_timeout;   /* a command containing this text returns -ETIMEDOUT once */
	const char *run_fail;      /* a command containing this text returns 1 */
	const char *run_noisy;     /* a command containing this text takes effect and then returns 1 */
	int lock_denied;           /* NVIDIA's unbindLock reads 0 after writing 1 */
	int settle_fail;           /* `udevadm settle` exits 1 */
	int prereq_missing;        /* the host says sriov-manage is not installed */
	int no_nvidia_modprobe;    /* nvidia-modprobe is not installed */
	int gpu_info;              /* a loaded nvidia lists each GPU's /proc information with Device Minor = its bus number */
	const char *open_fail;     /* an open of a node whose path ends in this fails with open_err */
	int open_err;              /* -errno, or -ETIMEDOUT */
	/*
	 * Who holds the GPU, so that unbindLock is denied: "mgr" while nvidia-vgpu-mgr is active, or
	 * the name of a module (nvidia_drm, nvidia_uvm) while that module is loaded. NULL: nobody.
	 * Reported on hardware: sriov-manage and the PF re-probe lose the lock on an activated host.
	 * Which client denies it is not established, so each fixture names the one it assumes and
	 * no test result may depend on the choice.
	 */
	const char *lock_holder;
	int mgr_active;            /* nvidia-vgpu-mgr is active after its restart */
	int vgpud_active;          /* vgpud is active (a oneshot may stay inactive) */
	int is_active_delay_ms;    /* each `systemctl is-active` takes this long */
	int mgr_stays_down;        /* nvidia-vgpu-mgr is not active after its restart */
	int manager_active;        /* the display manager is active */
	int msix_hidden_after_rescan;
	int no_sriov_after_rescan;
	int zero_vf_bars_after_rescan;
	int keep_removed;          /* a remove write leaves the device in place */
	int neighbour_on_bus;      /* a PF remove adds an unrelated device to its bus */
	int grow_windows;          /* a rescan grows the bridge's windows */
	int sticky_suspend;        /* power/control=on does not change runtime_status */
	/*
	 * Something that happens on the host while activation runs: after a command containing on_run,
	 * the file on_run_path (below the root) is written with on_run_write, or removed when that is NULL.
	 */
	int udev;                  /* udev loads modules for a device that appears, as on a real host */
	const char *on_run, *on_run_path, *on_run_write;
	const char *on_write;            /* the same, after a write to a path ending in this */
	const char *on_mkdir;            /* instead of a file: on_run_path becomes a directory */
	const char *write_timeout_value; /* write_timeout applies only to a write of this value */
	int write_timeout_after;         /* the write takes effect and then reports the timeout */
} sim;

static void sim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void rm_rf(const char *path);

static void sim_log(const char *fmt, ...)
{
	char p[GS_PATH_MAX];
	va_list ap;
	FILE *f;

	gs_pathf(p, sizeof(p), "%s/sim.log", sim.root);
	f = fopen(p, "a");
	if (!f)
		die(p);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

static char *sim_text(const char *rel)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	return slurp(p);
}

static int sim_exists(const char *rel)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	return gs_exists(p);
}

static void sim_writef(const char *rel, const char *text)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	writef(p, "%s", text);
}

static void sim_remove(const char *rel)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	rm_rf(p);
}

static void sim_reset(const char *root)
{
	memset(&sim, 0, sizeof(sim));
	gs_copy(sim.root, sizeof(sim.root), root);
	gs_pathf(sim.proc, sizeof(sim.proc), "%s/proc", root);
	gs_pathf(sim.state, sizeof(sim.state), "%s/state", root);
	gs_pathf(sim.run, sizeof(sim.run), "%s/run", root);
	gs_mkdir_p(sim.state, 0755);
	gs_mkdir_p(sim.run, 0755);
	gs_mkdir_p(sim.proc, 0755);
}

/* The command line as one line ("systemctl restart x") for run_timeout/run_fail. */
static void joined(const char *const argv[], char *buf, size_t len)
{
	size_t off = 0;

	buf[0] = '\0';
	for (unsigned int i = 0; argv[i] && off + 1 < len; i++)
		off += (size_t)snprintf(buf + off, len - off, "%s%s", i ? " " : "", argv[i]);
}

/* Recursively remove a fixture path ("modprobe -r", class devices); never follows a symlink. */
static void rm_rf(const char *path)
{
	struct stat st;
	DIR *d;
	struct dirent *de;

	if (lstat(path, &st) < 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path);
		return;
	}
	d = opendir(path);
	if (!d) {
		rmdir(path);
		return;
	}
	while ((de = readdir(d))) {
		char p[GS_PATH_MAX];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		gs_pathf(p, sizeof(p), "%s/%s", path, de->d_name);
		rm_rf(p);
	}
	closedir(d);
	rmdir(path);
}

static int systemctl_is_active(const char *unit)
{
	if (strncmp(unit, "display-manager", 15) == 0)
		return sim.manager_active;
	if (strcmp(unit, "nvidia-vgpu-mgr.service") == 0)
		return sim.mgr_active;
	if (strcmp(unit, "nvidia-vgpud.service") == 0)
		return sim.vgpud_active;
	return 1;
}

/* 1: the VFs have the fixed slots of a module without the Ada placement change (placement_id 0, 2, 4, 6). */
static int sim_placement_ids;

/* Create one VF: its device dir, driver link, physfn link and nvidia attributes. */
static void sim_make_vf(const char *pf, const char *vf, unsigned int generation)
{
	char dev[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], bridge_dir[GS_PATH_MAX], rel[GS_PATH_MAX];
	char *slash, *text;
	const char *devices;

	gs_pathf(dev, sizeof(dev), "%s/bus/pci/devices/%s", sim.root, pf);
	if (!realpath(dev, bridge_dir))
		return;
	slash = strrchr(bridge_dir, '/');
	if (!slash)
		return;
	*slash = '\0';
	gs_pathf(dev, sizeof(dev), "%s/%s", bridge_dir, vf);
	gs_pathf(p, sizeof(p), "%s/vendor", dev);
	writef(p, "0x10de\n");
	gs_pathf(p, sizeof(p), "%s/device", dev);
	gs_pathf(q, sizeof(q), "%s/bus/pci/devices/%s/device", sim.root, pf);
	text = slurp(q);
	writef(p, "%s", text ? text : "0x2206\n");
	free(text);
	gs_pathf(p, sizeof(p), "%s/class", dev);
	writef(p, "0x030000\n");
	gs_pathf(p, sizeof(p), "%s/power/control", dev);
	writef(p, "on\n");
	gs_pathf(p, sizeof(p), "%s/power/runtime_status", dev);
	writef(p, "active\n");
	gs_pathf(p, sizeof(p), "%s/nvidia/current_vgpu_type", dev);
	writef(p, "0\n");
	gs_pathf(p, sizeof(p), "%s/nvidia/placement_id", dev);
	{
		struct gs_addr a;

		if (sim_placement_ids && gs_addr_parse(vf, &a) == 0)
			writef(p, "%u\n", 2 * (a.func - 4));
		else
			writef(p, "Not Applicable\n");
	}
	gs_pathf(p, sizeof(p), "%s/nvidia/creatable_vgpu_types", dev);
	writef(p, "ID    : vGPU Name\n%u   : %s\n%s", generation == GS_ADA ? GS_VGPU_TYPE_ADA : GS_VGPU_TYPE_AMPERE,
	       generation == GS_ADA ? "NVIDIA RTX6000-Ada-2Q" : "NVIDIA RTXA6000-2Q",
	       generation == GS_ADA ? "941   : NVIDIA RTX6000-Ada-1Q\n" : "");
	put_resource(dev, 0);
	put_config(dev, 1, 0);
	gs_pathf(p, sizeof(p), "%s/physfn", dev);
	gs_pathf(q, sizeof(q), "../%s", pf);
	link_to(q, p);
	gs_pathf(p, sizeof(p), "%s/driver", dev);
	link_to("../../../../bus/pci/drivers/nvidia", p);
	devices = strstr(dev, "/devices/");
	gs_copy(rel, sizeof(rel), devices ? devices + 1 : dev);
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, vf);
	gs_pathf(q, sizeof(q), "../../../%s", rel);
	link_to(q, p);
}

/* sriov-manage -e pf: enable the presented count, create the VFs and bind them to nvidia. */
static void sim_enable_vfs(const char *pf)
{
	struct gs_addr a;
	struct gs_card c;
	char p[GS_PATH_MAX], total[32] = "", vf[GS_BDF_STR_SIZE], *text;
	unsigned int n, generation;

	if (gs_addr_parse(pf, &a))
		return;
	gs_card_inspect(sim.root, &a, &c);
	generation = c.entry ? c.entry->generation : GS_AMPERE;
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/sriov_totalvfs", sim.root, pf);
	text = slurp(p);
	if (text) {
		gs_copy(total, sizeof(total), text);
		free(text);
	}
	n = (unsigned int)strtoul(total, NULL, 10);
	for (unsigned int i = 0; i < n && i < 4; i++) {
		struct gs_addr vfa;
		char link[64];

		vfa = a;
		vfa.func = 4 + i;
		snprintf(vfa.str, sizeof(vfa.str), "%04x:%02x:%02x.%u", vfa.domain, vfa.bus, vfa.slot, vfa.func);
		snprintf(vf, sizeof(vf), "%s", vfa.str);
		sim_make_vf(pf, vf, generation);
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sim.root, pf, i);
		snprintf(link, sizeof(link), "../%s", vf);
		link_to(link, p);
	}
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/sriov_numvfs", sim.root, pf);
	writef(p, "%u\n", n);
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/ari_enabled", sim.root, pf);
	writef(p, "1\n");
}

/* The bus part of a BDF: 0000:03. */
static void bus_of(const char *bdf, char *out, size_t len)
{
	const char *colon = strrchr(bdf, ':');

	gs_copy(out, len, bdf);
	if (colon)
		out[(size_t)(colon - bdf)] = '\0';
}

/* A listed device on the bus ("0000:03")? 1 with its name, 0 none. */
static int bus_has_device(const char *bus, char *which, size_t len)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sim.root);
	if (!(d = opendir(p)))
		return 0;
	while ((de = readdir(d))) {
		struct gs_addr a;
		char b[32];

		if (de->d_name[0] == '.' || gs_addr_parse(de->d_name, &a))
			continue;
		bus_of(de->d_name, b, sizeof(b));
		if (strcmp(b, bus) == 0) {
			gs_copy(which, len, de->d_name);
			closedir(d);
			return 1;
		}
	}
	closedir(d);
	return 0;
}

static void sim_udev_add(const char *devdir, const char *bdf);

/* Restore one stashed device directory, with the injections a rescan applies. */
static void sim_restore(const char *bdf, int is_pf)
{
	char pathfile[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], *orig, *drv;
	char modvfs[32] = "4", bdfs[512] = "";

	gs_pathf(pathfile, sizeof(pathfile), "%s/.stash/%s.path", sim.root, bdf);
	orig = slurp(pathfile);
	if (!orig)
		return;
	gs_pathf(q, sizeof(q), "%s/.stash/%s", sim.root, bdf);
	if (rename(q, orig)) {
		free(orig);
		return;
	}
	unlink(pathfile);
	gs_pathf(p, sizeof(p), "%s/.stash/%s.driver", sim.root, bdf);
	drv = slurp(p);
	unlink(p);
	if (drv) {
		drv[strcspn(drv, "\n")] = '\0';
		gs_pathf(p, sizeof(p), "%s/driver", orig);
		if (drv[0] && strcmp(drv, "(none)") != 0) {
			gs_pathf(q, sizeof(q), "../../../../bus/pci/drivers/%s", drv);
			link_to(q, p);
		}
		free(drv);
	}
	{
		char rel[GS_PATH_MAX];
		const char *devices = strstr(orig, "/devices/");

		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, bdf);
		gs_copy(rel, sizeof(rel), devices ? devices + 1 : orig);
		gs_pathf(q, sizeof(q), "../../../%s", rel);
		link_to(q, p);
	}
	put_resource(orig, !sim.zero_vf_bars_after_rescan);
	put_config(orig, !sim.msix_hidden_after_rescan, 1);
	gs_pathf(p, sizeof(p), "%s/d3cold_allowed", orig);
	writef(p, "1\n");
	if (!is_pf) {
		free(orig);
		return;
	}
	gs_pathf(p, sizeof(p), "%s/module/%s/parameters/vfs", sim.root, GS_MODULE_NAME);
	{
		char *v = slurp(p);

		if (v) {
			v[strcspn(v, "\n")] = '\0';
			gs_copy(modvfs, sizeof(modvfs), v);
			free(v);
		}
	}
	gs_pathf(p, sizeof(p), "%s/module/%s/parameters/bdfs", sim.root, GS_MODULE_NAME);
	{
		char *v = slurp(p);

		if (v) {
			v[strcspn(v, "\n")] = '\0';
			gs_copy(bdfs, sizeof(bdfs), v);
			free(v);
		}
	}
	if (!sim.no_sriov_after_rescan && contains(bdfs, bdf)) {
		gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", orig);
		writef(p, "%s\n", modvfs);
		gs_pathf(p, sizeof(p), "%s/sriov_numvfs", orig);
		writef(p, "0\n");
		gs_pathf(p, sizeof(p), "%s/ari_enabled", orig);
		writef(p, "1\n");
	} else {
		gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", orig);
		unlink(p);
		gs_pathf(p, sizeof(p), "%s/sriov_numvfs", orig);
		unlink(p);
		gs_pathf(p, sizeof(p), "%s/ari_enabled", orig);
		unlink(p);
	}
	sim_udev_add(orig, bdf);
	/* The firmware framebuffer is not recreated by a rescan. */
	{
		char fw[GS_PATH_MAX], *slash;

		gs_pathf(fw, sizeof(fw), "%s", orig);
		while ((slash = strrchr(fw, '/')) != NULL) {
			char name[64];

			gs_copy(name, sizeof(name), slash + 1);
			if (strncmp(name, "simple-framebuffer.", 19) != 0)
				break;
			rm_rf(fw);
			break;
		}
	}
	free(orig);
}

/* Drop the class devices of a released firmware framebuffer, as kernfs does when its driver goes. */
static void sim_drop_consumers(const char *fwreal)
{
	static const char *const cls[] = { "graphics/fb0", "drm/card0" };
	char p[GS_PATH_MAX], real[PATH_MAX];

	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/class/%s/device", sim.root, cls[i]);
		if (realpath(p, real) && strcmp(real, fwreal) == 0) {
			gs_pathf(p, sizeof(p), "%s/class/%s", sim.root, cls[i]);
			rm_rf(p);
		}
	}
}

/* Add an unrelated device (0000:03:00.2) to the bus of bdf, for the empty-bus check. */
static void sim_add_neighbour(const char *real, const char *bdf)
{
	struct gs_addr a;
	char bridge[GS_PATH_MAX], nb[GS_BDF_STR_SIZE], p[GS_PATH_MAX], q[GS_PATH_MAX], rel[GS_PATH_MAX];
	char *slash;
	const char *devices;

	gs_addr_parse(bdf, &a);
	gs_copy(bridge, sizeof(bridge), real);
	slash = strrchr(bridge, '/');
	if (!slash)
		return;
	*slash = '\0';
	snprintf(nb, sizeof(nb), "%04x:%02x:%02x.2", a.domain, a.bus, a.slot);
	gs_pathf(q, sizeof(q), "%s/%s", bridge, nb);
	gs_pathf(p, sizeof(p), "%s/vendor", q);
	writef(p, "0x8086\n");
	gs_pathf(p, sizeof(p), "%s/device", q);
	writef(p, "0x1234\n");
	gs_pathf(p, sizeof(p), "%s/class", q);
	writef(p, "0x020000\n");
	devices = strstr(q, "/devices/");
	gs_copy(rel, sizeof(rel), devices ? devices + 1 : q);
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, nb);
	gs_pathf(q, sizeof(q), "../../../%s", rel);
	link_to(q, p);
}

static int sim_write(const char *path, const char *value);

/* 1 when this write is the one write_timeout names. */
static int sim_timeout_hit(const char *r, const char *value)
{
	return sim.write_timeout && ends_with(r, sim.write_timeout) &&
	       (!sim.write_timeout_value || strcmp(value, sim.write_timeout_value) == 0);
}

/* What on_run or on_write does to the host. */
static void sim_hook(void)
{
	char p[GS_PATH_MAX];

	if (sim.on_mkdir) {
		gs_pathf(p, sizeof(p), "%s/%s", sim.root, sim.on_run_path);
		rm_rf(p);
		gs_mkdir_p(p, 0755);
	} else if (sim.on_run_write) {
		sim_writef(sim.on_run_path, sim.on_run_write);
	} else {
		sim_remove(sim.on_run_path);
	}
}

static int sim_write_impl(const char *path, const char *value)
{
	size_t n = strlen(sim.root);
	const char *r = strncmp(path, sim.root, n) == 0 && path[n] == '/' ? path + n + 1 : path;
	char p[GS_PATH_MAX], q[GS_PATH_MAX], real[PATH_MAX];

	sim_log("W %s %s\n", r, value);
	if (sim.write_fail && ends_with(r, sim.write_fail))
		return -EIO;
	if (!sim.write_timeout_after && sim_timeout_hit(r, value)) {
		sim.write_timeout = NULL;
		return -ETIMEDOUT;
	}
	if (sim.write_lie && ends_with(r, sim.write_lie)) {
		writef(path, "auto\n");
		return 0;
	}
	if (ends_with(r, "/power/control")) {
		writef(path, "%s\n", value);
		if (strcmp(value, "on") == 0 && !sim.sticky_suspend) {
			gs_pathf(p, sizeof(p), "%.*s/runtime_status", (int)(strlen(path) - 8), path);
			writef(p, "active\n");
		} else if (strcmp(value, "auto") == 0) {
			gs_pathf(p, sizeof(p), "%.*s/runtime_status", (int)(strlen(path) - 8), path);
			writef(p, "suspended\n");
		}
		return 0;
	}
	if (ends_with(r, "/unbindLock")) {
		char holder[128];
		int held = 0;

		if (sim.lock_holder && strcmp(sim.lock_holder, "mgr") == 0) {
			held = sim.mgr_active;
		} else if (sim.lock_holder) {
			snprintf(holder, sizeof(holder), "module/%s", sim.lock_holder);
			held = sim_exists(holder);
		}

		writef(path, "%s\n", sim.lock_denied || held ? "0" : value);
		return 0;
	}
	if (strncmp(r, "bus/platform/devices/", 21) == 0 && ends_with(r, "/driver/unbind")) {
		gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s", sim.root, value);
		if (!realpath(p, real))
			return -ENODEV;
		gs_pathf(q, sizeof(q), "%s/driver", real);
		unlink(q);
		sim_drop_consumers(real);
		return 0;
	}
	if ((strncmp(r, "bus/platform/drivers/", 21) == 0 || strncmp(r, "bus/pci/drivers/", 16) == 0) &&
	    ends_with(r, "/bind")) {
		gs_pathf(p, sizeof(p), "%s/bus/%s/devices/%s", sim.root,
			 strncmp(r, "bus/platform/", 13) == 0 ? "platform" : "pci", value);
		if (!realpath(p, real))
			return -ENODEV;
		gs_pathf(q, sizeof(q), "%s/driver", real);
		gs_pathf(p, sizeof(p), "%.*s", (int)(strlen(path) - 5), path);
		if (symlink(p, q))
			return -errno;
		return 0;
	}
	if (strncmp(r, "bus/pci/drivers/", 16) == 0 && ends_with(r, "/unbind")) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver", sim.root, value);
		if (unlink(p))
			return -errno;
		/* The driver's own entry for a GPU goes when the GPU is unbound (when the simulator made it). */
		gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/information", sim.proc, value);
		if (sim.gpu_info)
			unlink(p);
		return 0;
	}
	if (strncmp(r, "bus/pci/devices/", 16) == 0 && ends_with(r, "/driver/unbind")) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver", sim.root, value);
		return unlink(p) ? -errno : 0;
	}
	if (strncmp(r, "bus/pci/devices/", 16) == 0 && ends_with(r, "/remove")) {
		char bdf[32], drv[64] = "(none)";
		char dlink[GS_PATH_MAX], target[GS_PATH_MAX];
		struct gs_addr a;
		ssize_t m;

		gs_copy(bdf, sizeof(bdf), r + 16);
		*strchr(bdf, '/') = '\0';
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, bdf);
		if (!realpath(p, real))
			return -ENODEV;
		if (sim.keep_removed)
			return 0;
		if (gs_addr_parse(bdf, &a))
			return -EINVAL;
		gs_pathf(dlink, sizeof(dlink), "%s/driver", real);
		m = readlink(dlink, target, sizeof(target) - 1);
		if (m > 0) {
			const char *base;

			target[m] = '\0';
			base = strrchr(target, '/');
			gs_copy(drv, sizeof(drv), base ? base + 1 : target);
		}
		gs_pathf(q, sizeof(q), "%s/.stash/%s.driver", sim.root, bdf);
		writef(q, "%s\n", drv);
		gs_pathf(q, sizeof(q), "%s/.stash/%s.path", sim.root, bdf);
		writef(q, "%s", real);
		gs_pathf(q, sizeof(q), "%s/.stash/%s", sim.root, bdf);
		if (rename(real, q))
			return -errno;
		unlink(p);
		if (sim.neighbour_on_bus && a.func == 0)
			sim_add_neighbour(real, bdf);
		return 0;
	}
	if (ends_with(r, "/rescan")) {
		char bus[32], which[32];
		char *prefix = strstr(r, "/pci_bus/");

		if (prefix) {
			gs_copy(bus, sizeof(bus), prefix + strlen("/pci_bus/"));
			*strchr(bus, '/') = '\0';
		} else {
			bus[0] = '\0';
		}
		if (bus_has_device(bus, which, sizeof(which))) {
			sim_log("rescan skipped: %s still on bus %s\n", which, bus);
			return 0;
		}
		{
			struct dirent *de;
			DIR *d;
			char stash[GS_PATH_MAX];
			char names[32][32];
			unsigned int k = 0;

			gs_pathf(stash, sizeof(stash), "%s/.stash", sim.root);
			d = opendir(stash);
			while (d && (de = readdir(d)) && k < 32)
				if (de->d_name[0] != '.' && !ends_with(de->d_name, ".path") &&
				    !ends_with(de->d_name, ".driver"))
					gs_copy(names[k++], sizeof(names[0]), de->d_name);
			if (d)
				closedir(d);
			for (unsigned int i = 0; i < k; i++) {
				struct gs_addr a;
				char b[32];

				if (gs_addr_parse(names[i], &a))
					continue;
				bus_of(names[i], b, sizeof(b));
				if (strcmp(b, bus) == 0)
					sim_restore(names[i], a.func == 0);
			}
			if (sim.grow_windows) {
				char res[GS_PATH_MAX], *at = strstr(r, "/pci_bus/");

				gs_pathf(res, sizeof(res), "%s", r);
				if (at)
					res[at - r] = '\0';
				gs_pathf(p, sizeof(p), "%s/%s/resource", sim.root, res);
				writef(p, "0x0000000000001000 0x0000000000001fff 0x0000000000000101\n"
					  "0x0000000090000000 0x0000000091ffffff 0x0000000000000200\n"
					  "0x0000000380000000 0x00000003d9ffffff 0x0000000000002200\n");
			}
		}
		return 0;
	}
	return gs_write_attr(path, value);
}

/* What a module needs loaded first, among NVIDIA's modules; at most two, NULL where there is none. */
static const char *sim_dep(const char *name, unsigned int k)
{
	static const char *const deps[][3] = { { "nvidia_drm", "nvidia_modeset", "nvidia" }, { "nvidia_modeset", "nvidia", NULL },
					       { "nvidia_uvm", "nvidia", NULL } };

	for (size_t i = 0; i < sizeof(deps) / sizeof(deps[0]); i++)
		if (strcmp(name, deps[i][0]) == 0)
			return k < 2 ? deps[i][1 + k] : NULL;
	return NULL;
}

static int sim_refcnt(const char *name)
{
	char p[GS_PATH_MAX], *text;
	int v;

	gs_pathf(p, sizeof(p), "%s/module/%s/refcnt", sim.root, name);
	if (!(text = slurp(p)))
		return 0;
	v = atoi(text);
	free(text);
	return v;
}

static void sim_add_refcnt(const char *name, int by)
{
	char p[GS_PATH_MAX];
	int v = sim_refcnt(name) + by;

	gs_pathf(p, sizeof(p), "%s/module/%s/refcnt", sim.root, name);
	writef(p, "%d\n", v < 0 ? 0 : v);
}

/* Every PCI device: fn(real directory, bdf). */
static void sim_each_device(void (*fn)(const char *dir, const char *bdf))
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	struct dirent *de;
	DIR *d;

	gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sim.root);
	if (!(d = opendir(p)))
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, de->d_name);
		if (realpath(p, real))
			fn(real, de->d_name);
	}
	closedir(d);
}

/* nvidia is gone: the devices it drove have no driver. */
static void sim_nvidia_release(const char *dir, const char *bdf)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	ssize_t n;

	(void)bdf;
	gs_pathf(p, sizeof(p), "%s/driver", dir);
	n = readlink(p, target, sizeof(target) - 1);
	if (n > 0 && (target[n] = '\0', ends_with(target, "/nvidia")))
		unlink(p);
}

/* nvidia is loaded: it binds each NVIDIA display PF that has no driver and lists it under /proc. */
static void sim_nvidia_claim(const char *dir, const char *bdf)
{
	char p[GS_PATH_MAX], q[GS_PATH_MAX], *vendor, *class;
	int ours;

	gs_pathf(p, sizeof(p), "%s/vendor", dir);
	vendor = slurp(p);
	gs_pathf(p, sizeof(p), "%s/class", dir);
	class = slurp(p);
	ours = vendor && class && strtoul(vendor, NULL, 16) == 0x10de && (strtoul(class, NULL, 16) >> 16) == 0x03;
	free(vendor);
	free(class);
	gs_pathf(p, sizeof(p), "%s/physfn", dir);
	gs_pathf(q, sizeof(q), "%s/driver", dir);
	if (!ours || gs_exists(p) || lstat(q, &(struct stat){ 0 }) == 0)
		return;
	/* A driver_override names the only driver the PCI core offers the device to. */
	{
		char *ov;
		int other;

		gs_pathf(p, sizeof(p), "%s/driver_override", dir);
		ov = slurp(p);
		other = ov && ov[0] && ov[0] != '\n' && strncmp(ov, "(null)", 6) != 0 && strncmp(ov, "nvidia\n", 7) != 0 &&
			strcmp(ov, "nvidia") != 0;
		free(ov);
		if (other)
			return;
	}
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/nvidia", sim.root);
	gs_mkdir_p(p, 0755);
	if (symlink(p, q))
		return;
	gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/unbindLock", sim.proc, bdf);
	if (!gs_exists(p))
		writef(p, "0\n");
	if (sim.gpu_info) {
		gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/information", sim.proc, bdf);
		writef(p, "Model: \t\t NVIDIA test GPU\nBus Location: \t %s\nDevice Minor: \t %lu\n", bdf, strtoul(bdf + 5, NULL, 16));
	}
}

/*
 * A loaded module as sysfs shows it: the directory, its holders and a refcount. What it depends on
 * is loaded with it and lists it as a holder. Calling this for a loaded module changes nothing, so
 * a fixture may write a refcount of its own afterwards. nvidia starts with one reference, for a
 * program that has the driver open; see sim_core_unused().
 */
static void sim_module(const char *name)
{
	char p[GS_PATH_MAX];
	const char *dep;

	gs_pathf(p, sizeof(p), "%s/module/%s/holders", sim.root, name);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/module/%s/refcnt", sim.root, name);
	if (!gs_exists(p))
		writef(p, "%d\n", strcmp(name, "nvidia") == 0 ? 1 : 0);
	for (unsigned int k = 0; (dep = sim_dep(name, k)); k++) {
		sim_module(dep);
		gs_pathf(p, sizeof(p), "%s/module/%s/holders/%s", sim.root, dep, name);
		if (!gs_exists(p)) {
			writef(p, "%s", "");
			sim_add_refcnt(dep, 1);
		}
	}
}

/* What loading nvidia does besides appearing under /sys/module: its /proc tree is back and it binds the GPUs. */
static void sim_nvidia_up(void)
{
	char p[GS_PATH_MAX], q[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/.stash/proc-driver-nvidia", sim.root);
	if (gs_exists(p)) {
		gs_pathf(q, sizeof(q), "%s/driver/nvidia", sim.proc);
		rm_rf(q);
		if (rename(p, q))
			die(q);
	}
	sim_each_device(sim_nvidia_claim);
}

/* No program has nvidia open: only the modules on top of it hold it. */
static void sim_core_unused(void)
{
	sim_add_refcnt("nvidia", -1);
}

/* rmmod: 0, or 1 while another module holds this one or its refcount is not 0. */
static int sim_unload(const char *name)
{
	char p[GS_PATH_MAX], q[GS_PATH_MAX];
	const char *dep;
	struct dirent *de;
	int held = 0;
	DIR *d;

	gs_pathf(p, sizeof(p), "%s/module/%s", sim.root, name);
	if (!gs_exists(p))
		return 1;
	gs_pathf(p, sizeof(p), "%s/module/%s/holders", sim.root, name);
	if ((d = opendir(p))) {
		while ((de = readdir(d)))
			held |= de->d_name[0] != '.';
		closedir(d);
	}
	if (held || sim_refcnt(name) > 0)
		return 1;
	for (unsigned int k = 0; (dep = sim_dep(name, k)); k++) {
		gs_pathf(p, sizeof(p), "%s/module/%s/holders/%s", sim.root, dep, name);
		if (gs_exists(p)) {
			unlink(p);
			sim_add_refcnt(dep, -1);
		}
	}
	gs_pathf(p, sizeof(p), "%s/module/%s", sim.root, name);
	rm_rf(p);
	if (strcmp(name, "nvidia") == 0) {
		sim_each_device(sim_nvidia_release);
		gs_pathf(p, sizeof(p), "%s/driver/nvidia", sim.proc);
		gs_pathf(q, sizeof(q), "%s/.stash/proc-driver-nvidia", sim.root);
		rm_rf(q);
		gs_pathf(q, sizeof(q), "%s/.stash", sim.root);
		gs_mkdir_p(q, 0755);
		gs_pathf(q, sizeof(q), "%s/.stash/proc-driver-nvidia", sim.root);
		if (gs_exists(p) && rename(p, q))
			die(q);
	}
	return 0;
}

/* One line of the *.conf files in dir matches: fn(line, arg) returned 1. */
static int sim_conf_any(const char *dir, int (*fn)(const char *line, const char *arg), const char *arg)
{
	char p[GS_PATH_MAX], *text, *line, *save = NULL;
	struct dirent *de;
	int hit = 0;
	DIR *d;

	if (!(d = opendir(dir)))
		return 0;
	while (!hit && (de = readdir(d))) {
		if (!ends_with(de->d_name, ".conf"))
			continue;
		gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (!(text = slurp(p)))
			continue;
		for (line = strtok_r(text, "\n", &save); line && !hit; line = strtok_r(NULL, "\n", &save))
			hit = fn(line, arg);
		free(text);
	}
	closedir(d);
	return hit;
}

static int sim_modprobe_dirs(int (*fn)(const char *line, const char *arg), const char *arg)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/etc/modprobe.d", sim.root);
	if (sim_conf_any(p, fn, arg))
		return 1;
	gs_pathf(p, sizeof(p), "%s/../modprobe.d", sim.run);
	return sim_conf_any(p, fn, arg);
}

/* `install NAME /bin/false`: modprobe runs that command instead of loading NAME, and it fails. */
static int line_install_false(const char *line, const char *name)
{
	char mod[64] = "", cmd[64] = "";

	return sscanf(line, "install %63s %63s", mod, cmd) == 2 && strcmp(mod, name) == 0 && strcmp(cmd, "/bin/false") == 0;
}

/* `alias PATTERN off`: the alias no longer resolves to a module. */
static int line_alias_off(const char *line, const char *modalias)
{
	char pattern[160] = "", target[64] = "";

	return sscanf(line, "alias %159s %63s", pattern, target) == 2 && strcmp(target, "off") == 0 &&
	       fnmatch(pattern, modalias, 0) == 0;
}

/* The boolean parameters of the modules: sysfs prints Y or N for them, whatever was passed. */
static int sim_param_bool(const char *module, const char *param, size_t len)
{
	static const char *const bools[][2] = { { GS_MODULE_NAME, "ari" }, { GS_MODULE_NAME, "msix" },
						{ "nvidia_drm", "modeset" }, { "nvidia_drm", "fbdev" },
						{ "nvidia_uvm", "uvm_disable_hmm" } };

	for (size_t k = 0; k < sizeof(bools) / sizeof(bools[0]); k++)
		if (strcmp(module, bools[k][0]) == 0 && strlen(bools[k][1]) == len && strncmp(param, bools[k][1], len) == 0)
			return 1;
	return 0;
}

/* Create module/name with the parameters of a modprobe argv. */
static void sim_load(const char *name, const char *const argv[], unsigned int from)
{
	char p[GS_PATH_MAX];
	int had_core;

	gs_pathf(p, sizeof(p), "%s/module/nvidia", sim.root);
	had_core = gs_exists(p);
	sim_module(name);
	if (!had_core && gs_exists(p))
		sim_nvidia_up();
	for (unsigned int i = from; argv[i]; i++) {
		const char *eq = strchr(argv[i], '=');

		if (!eq)
			continue;
		gs_pathf(p, sizeof(p), "%s/module/%s/parameters/%.*s", sim.root, name, (int)(eq - argv[i]), argv[i]);
		if (sim_param_bool(name, argv[i], (size_t)(eq - argv[i])) && (strcmp(eq + 1, "1") == 0 || strcmp(eq + 1, "0") == 0))
			writef(p, "%s\n", strcmp(eq + 1, "1") == 0 ? "Y" : "N");
		else
			writef(p, "%s\n", eq + 1);
	}
	if (strcmp(name, "nvidia_drm") == 0 && sim.display_pf[0]) {
		gs_pathf(p, sizeof(p), "%s/class/drm/card0", sim.root);
		rm_rf(p);
		gs_mkdir_p(p, 0755);
		gs_pathf(p, sizeof(p), "%s/class/drm/card0/device", sim.root);
		{
			char target[GS_PATH_MAX];

			gs_pathf(target, sizeof(target), "../../../bus/pci/devices/%s", sim.display_pf);
			link_to(target, p);
		}
	}
}

/*
 * udev's part when an NVIDIA display device appears: its class alias resolves to nvidia_drm, and
 * udev inserts that module and what it needs. It does not go through modprobe's `install` and
 * `blacklist` rules; only an `alias <pattern> off` line makes the alias resolve to nothing. The
 * module then has its built-in defaults, not the options activation passes.
 */
static void sim_udev_add(const char *devdir, const char *bdf)
{
	static const char *const none[] = { NULL };
	char p[GS_PATH_MAX], modalias[160], *vendor, *device, *class;
	unsigned int v = 0, dev = 0, cls = 0;

	if (!sim.udev)
		return;
	gs_pathf(p, sizeof(p), "%s/vendor", devdir);
	vendor = slurp(p);
	gs_pathf(p, sizeof(p), "%s/device", devdir);
	device = slurp(p);
	gs_pathf(p, sizeof(p), "%s/class", devdir);
	class = slurp(p);
	if (vendor && device && class) {
		v = (unsigned int)strtoul(vendor, NULL, 16);
		dev = (unsigned int)strtoul(device, NULL, 16);
		cls = (unsigned int)strtoul(class, NULL, 16);
	}
	free(vendor);
	free(device);
	free(class);
	if (v != 0x10de || (cls >> 16) != 0x03)
		return;
	snprintf(modalias, sizeof(modalias), "pci:v%08Xd%08Xsv00000000sd00000000bc%02Xsc%02Xi%02X", v, dev, cls >> 16,
		 (cls >> 8) & 0xff, cls & 0xff);
	if (sim_modprobe_dirs(line_alias_off, modalias))
		return;
	sim_log("U udev loads nvidia_drm for %s\n", bdf);
	sim_load("nvidia_drm", none, 0);
}

/* A restart of either vGPU daemon drops every vGPU type (reported on hardware: a rerun left the types at 0). */
static void sim_clear_types(void)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sim.root);
	if (!(d = opendir(p)))
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sim.root, de->d_name);
		if (!gs_exists(p))
			continue;
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/current_vgpu_type", sim.root, de->d_name);
		if (gs_exists(p))
			writef(p, "0\n");
	}
	closedir(d);
}

static int sim_run_impl(const char *const argv[])
{
	char line[1024], p[GS_PATH_MAX];
	unsigned int n = 0;

	while (argv[n])
		n++;
	joined(argv, line, sizeof(line));
	sim_log("R %s\n", line);
	if (sim.run_timeout && strstr(line, sim.run_timeout)) {
		sim.run_timeout = NULL;
		return -ETIMEDOUT;
	}
	if (sim.run_fail && strstr(line, sim.run_fail))
		return 1;
	if (strcmp(argv[0], "udevadm") == 0 && n > 1 && strcmp(argv[1], "settle") == 0)
		return sim.settle_fail == 2 ? -ENOENT : sim.settle_fail ? 1 : 0;
	if (ends_with(argv[0], "sriov-manage") && argv[1] && strcmp(argv[1], "-e") == 0) {
		char *block;

		/* What the enable-window module block holds while the VFs are enabled, for the tests to read. */
		gs_pathf(p, sizeof(p), "%s/../modprobe.d/zz-geforce-sriov-enable.conf", sim.run);
		if ((block = slurp(p))) {
			sim_writef("sim.block", block);
			free(block);
		}
		sim_enable_vfs(argv[2]);
		return 0;
	}
	if (strcmp(argv[0], "systemctl") == 0 && n > 1) {
		const char *verb = argv[1], *unit = argv[n - 1];

		if (strcmp(verb, "is-active") == 0) {
			if (sim.is_active_delay_ms)
				gs_sleep_ms((unsigned int)sim.is_active_delay_ms);
			return systemctl_is_active(unit) ? 0 : 3;
		}
		if (strcmp(verb, "restart") == 0 && strcmp(unit, "nvidia-vgpu-mgr.service") == 0) {
			sim.mgr_active = !sim.mgr_stays_down;
			sim_clear_types();
			return 0;
		}
		if (strcmp(verb, "restart") == 0 && strcmp(unit, "nvidia-vgpud.service") == 0) {
			sim_clear_types();
			return 0;
		}
		if ((strcmp(verb, "stop") == 0 || strcmp(verb, "start") == 0) &&
		    strncmp(unit, "display-manager", 15) == 0) {
			sim.manager_active = strcmp(verb, "stop") != 0;
			return 0;
		}
		return 0;
	}
	/* NVIDIA's node maker, as its source reads: -c minors are UVM's base minor after -u, and 255 is nvidiactl. */
	if (strcmp(argv[0], "nvidia-modprobe") == 0) {
		int uvm = 0;

		if (sim.no_nvidia_modprobe)
			return -ENOENT;
		for (unsigned int i = 1; i < n; i++)
			uvm |= strcmp(argv[i], "-u") == 0;
		for (unsigned int i = 1; i < n; i++) {
			char rel[128];

			if (strcmp(argv[i], "-m") == 0) {
				sim_writef("dev/nvidia-modeset", "");
			} else if (strcmp(argv[i], "-c") == 0 && i + 1 < n) {
				if (uvm) {
					sim_writef("dev/nvidia-uvm", "");
					sim_writef("dev/nvidia-uvm-tools", "");
				} else if (strcmp(argv[i + 1], "255") == 0) {
					sim_writef("dev/nvidiactl", "");
				} else {
					snprintf(rel, sizeof(rel), "dev/nvidia%s", argv[i + 1]);
					sim_writef(rel, "");
				}
				i++;
			} else if (strcmp(argv[i], "-f") == 0 && i + 1 < n) {
				char *cap = slurp(argv[i + 1]);
				const char *minor = cap ? strstr(cap, "DeviceFileMinor: ") : NULL;

				if (!minor) {
					free(cap);
					return 1;
				}
				snprintf(rel, sizeof(rel), "dev/nvidia-caps/nvidia-cap%d", atoi(minor + 17));
				sim_writef(rel, "");
				free(cap);
				i++;
			}
		}
		return 0;
	}
	if (strcmp(argv[0], "rmmod") == 0 && n >= 2)
		return sim_unload(argv[1]);
	if (strcmp(argv[0], "modprobe") == 0 && n >= 2) {
		if (strcmp(argv[1], "--ignore-install") == 0 && n >= 3) {
			sim_load(argv[2], argv, 3);
			return 0;
		}
		if (strcmp(argv[1], "-r") == 0 && n >= 3)
			return sim_unload(argv[2]);
		if (sim_modprobe_dirs(line_install_false, argv[1]))
			return 1;
		sim_load(argv[1], argv, 2);
		return 0;
	}
	return 0;
}

static int sim_run(const char *const argv[])
{
	char line[1024];
	int r = sim_run_impl(argv);

	joined(argv, line, sizeof(line));
	if (sim.on_run && strstr(line, sim.on_run))
		sim_hook();
	if (sim.run_noisy && strstr(line, sim.run_noisy) && r == 0)
		return 1;
	return r;
}

static int sim_write(const char *path, const char *value)
{
	size_t n = strlen(sim.root);
	const char *rel = strncmp(path, sim.root, n) == 0 && path[n] == '/' ? path + n + 1 : path;
	int late = sim.write_timeout_after && sim_timeout_hit(rel, value), r;

	if (late)
		sim.write_timeout = NULL;
	r = sim_write_impl(path, value);
	if (sim.on_write && ends_with(rel, sim.on_write))
		sim_hook();
	return late ? -ETIMEDOUT : r;
}

/* nvidia-smi answers with the fixture's nvidia-smi.out; without it, nvidia-smi is not installed. */
static int sim_capture(const char *const argv[], char *out, size_t len)
{
	char line[1024], p[GS_PATH_MAX], *text;

	joined(argv, line, sizeof(line));
	sim_log("R %s\n", line);
	out[0] = '\0';
	gs_pathf(p, sizeof(p), "%s/nvidia-smi.out", sim.root);
	if (strcmp(argv[0], "nvidia-smi") != 0 || !(text = slurp(p)))
		return -1;
	gs_copy(out, len, text);
	free(text);
	return 0;
}

static int sim_write_deadline(const struct gs_host *h, const char *path, const char *value, unsigned int ms)
{
	(void)h;
	(void)ms;
	return sim_write(path, value);
}

static int sim_run_deadline(const struct gs_host *h, const char *const argv[], unsigned int ms)
{
	(void)h;
	(void)ms;
	return sim_run(argv);
}

/* An open of a node: it must exist; open_fail names a node whose open the driver refuses. */
static int sim_open_node(const struct gs_host *h, const char *path, unsigned int ms)
{
	(void)h;
	(void)ms;
	sim_log("O %s\n", path + strlen(sim.root));
	if (access(path, F_OK))
		return -ENOENT;
	if (sim.open_fail && ends_with(path, sim.open_fail))
		return sim.open_err;
	return 0;
}

/* The simulated host's programs are the simulator's own; a test can still report one as missing. */
static int sim_prereq(const struct gs_config *cfg, char *why, size_t len)
{
	if (!sim.prereq_missing)
		return 0;
	return snprintf(why, len, "%.300s: No such file or directory (install NVIDIA's vGPU host driver)", cfg->sriov_manage), -1;
}

static struct gs_host sim_host(void)
{
	struct gs_host h;

	memset(&h, 0, sizeof(h));
	h.sys = sim.root;
	h.proc = sim.proc;
	h.state_dir = sim.state;
	h.run_dir = sim.run;
	h.write_attr = sim_write;
	h.run = sim_run;
	h.capture = sim_capture;
	h.write_attr_deadline = sim_write_deadline;
	h.run_deadline = sim_run_deadline;
	h.open_node = sim_open_node;
	h.root = sim.root;
	h.enable_prereq = sim_prereq;
	return h;
}

/* fn against the simulated host in a child; returns its exit status and output. */
typedef int (*host_fn)(void);

static int run_host(host_fn fn, char **out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = fn();
		fflush(stdout);
		fflush(stderr);
		/* exit, not _exit: the leak checker and the coverage counters run at a normal exit only. */
		exit(rc);
	}
	close(pipefd[1]);
	{
		int rc = collect(pipefd[0], pid, out);
		char *log = sim_text("sim.log");

		trace_next();
		trace_put("out", rc, *out);
		trace_put("log", -1, log);
		free(log);
		return rc;
	}
}

/* ---- addresses and configuration --------------------------------------- */

static void test_addr(void)
{
	struct gs_addr a;
	char list[GS_BDF_LIST_STR_SIZE];

	CHECK(gs_addr_parse("0000:03:00.0", &a) == 0 && a.domain == 0 && a.bus == 3 && a.slot == 0 && a.func == 0 &&
	      strcmp(a.str, "0000:03:00.0") == 0, "canonical address");
	CHECK(gs_addr_parse("10000:03:00.0", &a) == 0 && a.domain == 0x10000, "five-digit domain");
	CHECK(gs_addr_parse("0000:03:00.0", &a) == 0 && gs_addr_parse("0000:03:00.00", &a) != 0, "leading zero function");
	CHECK(gs_addr_parse("0000:3:00.0", &a) != 0, "short bus");
	CHECK(gs_addr_parse("0000:03:00.0\n", &a) != 0, "trailing newline");
	CHECK(gs_addr_parse("0000:03:00.8", &a) != 0, "function 8");
	CHECK(gs_addr_parse("0000:03:00.0", &a) == 0 && gs_addr_parse("00000000:03:00.0", &a) != 0,
	      "a domain over 4 digits has no leading zero");
	gs_addr_parse("0000:03:00.0", &a);
	CHECK(gs_addr_parse(a.str, &a) == 0 && strcmp(a.str, "0000:03:00.0") == 0, "parse str round trip");
	{
		struct gs_addr l[2];

		gs_addr_parse("0000:03:00.0", &l[0]);
		gs_addr_parse("0000:04:00.0", &l[1]);
		gs_addr_list_str(l, 2, list, sizeof(list));
		CHECK(strcmp(list, "0000:03:00.0,0000:04:00.0") == 0, "address list: %s", list);
	}
}

static void test_config(void)
{
	struct gs_config c, back;
	char err[512], text[4096];
	const char *bad[] = {
		"max_vfs=8\n",
		"display=sometimes\n",
		"host_cuda=maybe\n",
		"vfs=0\n",
		"vfs=17\n",
		"d3cold=on\n",
		"bdfs=0000:03:00.0\nbdfs=0000:04:00.0\n",
		"bdfs=0000:03:00.1\n",
		"bdfs=0000:03:00.0,0000:03:00.0\n",
		"ari =on\n",
		"unknown_key=1\n",
		NULL
	};

	gs_config_defaults(&c);
	CHECK(c.display == GS_DISPLAY_AUTO && c.host_cuda == GS_SW_OFF && c.ari == GS_SW_ON && c.msix == GS_SW_ON &&
	      c.vfs == 0 && c.d3cold_off == 0 && !c.present, "defaults");
	CHECK(gs_config_vfs(&c) == GS_DEFAULT_VFS, "default VF count");
	CHECK(strcmp(c.sriov_manage, GS_SRIOV_MANAGE_DEFAULT) == 0, "default sriov-manage");

	CHECK(gs_config_parse("bdfs=0000:03:00.0,0000:04:00.0\ndisplay=on\nhost_cuda=on\nari=off\nmsix=off\n"
			      "vfs=8\nd3cold=off\ntype_ada=942\ntype_ampere=525\n"
			      "sriov_manage=/opt/sriov-manage\nidentity=on\nidentity_force_host=on\n",
			      strlen("bdfs=0000:03:00.0,0000:04:00.0\ndisplay=on\nhost_cuda=on\nari=off\nmsix=off\n"
				     "vfs=8\nd3cold=off\ntype_ada=942\ntype_ampere=525\n"
				     "sriov_manage=/opt/sriov-manage\nidentity=on\nidentity_force_host=on\n"),
			      &c, err, sizeof(err)) == 0,
	      "new keys parse: %s", err);
	CHECK(c.nbdfs == 2 && strcmp(c.bdfs[1].str, "0000:04:00.0") == 0 && c.display == GS_DISPLAY_ON &&
	      c.host_cuda == GS_SW_ON && c.ari == GS_SW_OFF && c.msix == GS_SW_OFF && c.vfs == 8 && c.d3cold_off &&
	      c.type_ada == 942 && c.type_ampere == 525 && strcmp(c.sriov_manage, "/opt/sriov-manage") == 0 &&
	      c.identity == GS_SW_ON && c.identity_force_host == GS_SW_ON, "parsed values");
	CHECK(gs_config_vgpu_type(&c, GS_ADA) == 942 && gs_config_vgpu_type(&c, GS_AMPERE) == 525, "per-generation type");
	memset(&back, 0, sizeof(back));
	CHECK(gs_config_render(&c, text, sizeof(text)) > 0, "render");
	CHECK(gs_config_parse(text, strlen(text), &back, err, sizeof(err)) == 0, "reparse render: %s", err);
	CHECK(back.display == c.display && back.host_cuda == c.host_cuda && back.ari == c.ari && back.msix == c.msix &&
	      back.vfs == c.vfs && back.d3cold_off == c.d3cold_off && back.nbdfs == c.nbdfs &&
	      back.type_ada == c.type_ada && back.type_ampere == c.type_ampere, "render round trip");

	for (unsigned int i = 0; bad[i]; i++)
		CHECK(gs_config_parse(bad[i], strlen(bad[i]), &c, err, sizeof(err)) != 0, "rejected: %s", bad[i]);
	gs_config_defaults(&c);
	CHECK(gs_config_parse("max_vfs=8\n", strlen("max_vfs=8\n"), &c, err, sizeof(err)) != 0 &&
	      contains(err, "unknown key 'max_vfs'; the VF count key is vfs"), "vf key hint: %s", err);
	CHECK(gs_config_parse("vfs=4 \n", strlen("vfs=4 \n"), &c, err, sizeof(err)) != 0 &&
	      contains(err, "whitespace is not allowed"), "whitespace: %s", err);
	CHECK(gs_config_parse("display=auto", strlen("display=auto"), &c, err, sizeof(err)) == 0 &&
	      c.display == GS_DISPLAY_AUTO, "last line needs no newline");
	CHECK(gs_config_parse("", 0, &c, err, sizeof(err)) == 0 && c.present && c.nbdfs == 0, "empty file selects nothing");
	{
		struct gs_config bc, bback;
		char btext[2048];
		int blen;

		CHECK(gs_config_parse("bdfs=0000:03:00.0\ndisplay=boot\n", strlen("bdfs=0000:03:00.0\ndisplay=boot\n"), &bc, err,
				      sizeof(err)) == 0 && bc.display == GS_DISPLAY_BOOT, "display=boot parses: %s", err);
		blen = gs_config_render(&bc, btext, sizeof(btext));
		CHECK(blen > 0 && contains(btext, "\ndisplay=boot\n") &&
		      gs_config_parse(btext, (size_t)blen, &bback, err, sizeof(err)) == 0 && bback.display == GS_DISPLAY_BOOT,
		      "display=boot is written and reads back: %s", btext);
	}

	CHECK(gs_parse_vfs("4", &c.vfs) == 0 && c.vfs == 4, "vfs 4");
	CHECK(gs_parse_vfs("0", &c.vfs) != 0 && gs_parse_vfs("01", &c.vfs) != 0 && gs_parse_vfs("17", &c.vfs) != 0 &&
	      gs_parse_vfs("", &c.vfs) != 0, "vfs bounds");
	CHECK(gs_parse_vgpu_type("525", &c.type_ada) == 0 && c.type_ada == 525, "type 525");
	CHECK(gs_parse_vgpu_type("0", &c.type_ada) != 0 && gs_parse_vgpu_type("0525", &c.type_ada) != 0,
	      "type bounds");

	{
		char path[GS_PATH_MAX];

		gs_pathf(path, sizeof(path), "%s/config-missing", tmp);
		CHECK(gs_config_load(path, &c, err, sizeof(err)) == 0 && !c.present, "missing config: defaults");
		gs_pathf(path, sizeof(path), "%s/config-bad", tmp);
		writef(path, "display=bogus\n");
		CHECK(gs_config_load(path, &c, err, sizeof(err)) != 0 && contains(err, path) &&
		      contains(err, "display must be auto, on, boot or off"), "invalid config names the file: %s", err);
	}
}

/* The switched-off switch and the switch names the config calls out. */
static void test_switches(void)
{
	struct gs_config c;
	char err[256];

	gs_config_defaults(&c);
	CHECK(gs_parse_switch("on", &c.ari) == 0 && c.ari == GS_SW_ON, "on");
	CHECK(gs_parse_switch("off", &c.ari) == 0 && c.ari == GS_SW_OFF, "off");
	CHECK(gs_parse_switch("ON", &c.ari) != 0 && gs_parse_switch("1", &c.ari) != 0, "other values");
	CHECK(gs_config_parse("ari=on\nmsix=on\n", strlen("ari=on\nmsix=on\n"), &c, err, sizeof(err)) == 0 &&
	      c.ari == GS_SW_ON && c.msix == GS_SW_ON, "switches: %s", err);
}

/* A temporary file an earlier process left (its PID reused after a power loss) never blocks an atomic write. */
static void test_write_atomic(void)
{
	char dir[GS_PATH_MAX], path[GS_PATH_MAX], stale[GS_PATH_MAX], *text;
	struct dirent *de;
	DIR *d;
	int found = 0;

	gs_pathf(dir, sizeof(dir), "%s/atomic", tmp);
	gs_mkdir_p(dir, 0755);
	gs_pathf(path, sizeof(path), "%s/file", dir);
	gs_pathf(stale, sizeof(stale), "%s/file.gs-tmp.ABC123", dir);
	writef(stale, "stale");
	CHECK(gs_write_file_atomic(path, "hello\n", 6, 0640) == 0, "atomic write");
	text = slurp(path);
	CHECK(text && strcmp(text, "hello\n") == 0, "content: %s", text ? text : "(none)");
	free(text);
	d = opendir(dir);
	while (d && (de = readdir(d)))
		found += strncmp(de->d_name, "file.gs-tmp.", 12) == 0;
	if (d)
		closedir(d);
	CHECK(found == 1, "a stale temporary file is not the new one (found %d)", found);
	CHECK(gs_write_file_atomic(path, "x", 1, 0600) == 0, "overwrite");
	text = slurp(path);
	CHECK(text && strcmp(text, "x") == 0, "overwritten: %s", text ? text : "(none)");
	free(text);
}

static void test_sha256(void)
{
	char hex[65], path[GS_PATH_MAX], lnk[GS_PATH_MAX], real[GS_PATH_MAX];
	int r;

	gs_sha256_buf("abc", 3, hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256(abc): %s", hex);
	gs_pathf(path, sizeof(path), "%s/sha-file", tmp);
	writef(path, "abc");
	CHECK(gs_sha256_file(path, hex) == 0 && strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
	      "file sha: %s", hex);
	gs_pathf(lnk, sizeof(lnk), "%s/sha-link", tmp);
	link_to(path, lnk);
	CHECK(gs_sha256_file(lnk, hex) == -ELOOP, "a symlink is never followed by the strict hash");
	CHECK(gs_sha256_file_resolved(lnk, hex, real) == 0 && strcmp(real, path) == 0, "resolved path: %s", real);
	r = gs_sha256_file(tmp, hex);
	CHECK(r == -EINVAL || r == -EISDIR || r < 0, "a directory is refused: %d", r);
}

/*
 * gs_write_attr writes regular files only: a FIFO with no reader fails at the open (ENXIO, no hang), a device that
 * opens is refused by the fstat check (EINVAL), and a link is not followed.
 */
static void test_write_attr(void)
{
	char p[GS_PATH_MAX], q[GS_PATH_MAX], *t;
	int r;

	gs_pathf(p, sizeof(p), "%s/attr-fifo", tmp);
	unlink(p);
	if (mkfifo(p, 0600))
		die(p);
	r = gs_write_attr(p, "on");
	CHECK(r == -ENXIO, "FIFO refused at the open without blocking: %d", r);
	r = gs_write_attr("/dev/null", "on");
	CHECK(r == -EINVAL, "a device that opens is refused by the fstat check: %d", r);
	gs_pathf(p, sizeof(p), "%s/attr-file", tmp);
	writef(p, "auto\n");
	CHECK(gs_write_attr(p, "on") == 0, "regular file written");
	t = slurp(p);
	CHECK(t && strncmp(t, "on", 2) == 0, "regular file holds the value: %s", t ? t : "(none)");
	free(t);
	gs_pathf(q, sizeof(q), "%s/attr-link", tmp);
	link_to(p, q);
	CHECK(gs_write_attr(q, "on") == -ELOOP, "link not followed");
}

/* ---- shared fixtures ---------------------------------------------------- */

/* A sysfs write that stays in the kernel: it returns long after any deadline the test gives. */
static int slow_write(const char *path, const char *value)
{
	gs_sleep_ms(3000);
	return gs_write_attr(path, value);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* The live host's bounded write: it returns at the deadline even when the write itself does not. */
static void test_live_deadline(void)
{
	struct gs_host h = gs_host_live;
	char p[GS_PATH_MAX], *text;
	long t0;
	int r;

	gs_pathf(p, sizeof(p), "%s/deadline/attr", tmp);
	writef(p, "0\n");
	r = h.write_attr_deadline(&h, p, "1", 2000);
	text = slurp(p);
	CHECK(r == 0 && text && text[0] == '1', "a write that finishes in time: r=%d text=%s", r, text ? text : "");
	free(text);

	h.write_attr = slow_write;
	t0 = now_ms();
	r = h.write_attr_deadline(&h, p, "2", 200);
	CHECK(r == -ETIMEDOUT, "a write that outlasts its deadline reports a timeout: r=%d", r);
	CHECK(now_ms() - t0 < 1500, "and the caller is back near the deadline, not after the write: %ld ms", now_ms() - t0);

	/* A command that outlasts its deadline is killed with what it started; nothing of it runs on. */
	{
		char mark[GS_PATH_MAX], script[GS_PATH_MAX + 64];
		const char *argv[] = { "sh", "-c", script, NULL };

		h = gs_host_live;
		gs_pathf(mark, sizeof(mark), "%s/deadline/late", tmp);
		snprintf(script, sizeof(script), "(sleep 1; : >'%s') & wait", mark);
		t0 = now_ms();
		r = h.run_deadline(&h, argv, 200);
		CHECK(r == -ETIMEDOUT && now_ms() - t0 < 900, "a command that outlasts its deadline: r=%d after %ld ms", r,
		      now_ms() - t0);
		gs_sleep_ms(1300);
		CHECK(!gs_exists(mark), "its child was killed with it and did not finish its work");
		snprintf(script, sizeof(script), "exit 7");
		r = h.run_deadline(&h, argv, 2000);
		CHECK(r == 7, "a command that finishes in time returns its status: %d", r);
	}
	/*
	 * A caller that ignores SIGCHLD: the kernel reaps each child itself, so it cannot be waited for.
	 * A write whose result arrived is still that result, and a command whose status is lost is a
	 * failure. Neither is a timeout, which would mean uncertain state and a guard.
	 */
	{
		const char *argv[] = { "sh", "-c", "exit 7", NULL };
		pid_t pid = fork();
		int st = 0;

		if (pid == 0) {
			struct gs_host hh = gs_host_live;
			int bad = 0, w, c;

			signal(SIGCHLD, SIG_IGN);
			w = hh.write_attr_deadline(&hh, p, "3", 2000);
			c = hh.run_deadline(&hh, argv, 2000);
			if (w != 0)
				bad |= 1;
			if (c == -ETIMEDOUT || c == 0)
				bad |= 2;
			_exit(bad);
		}
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
		CHECK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "with SIGCHLD ignored no finished operation is a timeout: %d",
		      WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		text = slurp(p);
		CHECK(text && text[0] == '3', "and the write took effect: %s", text ? text : "");
		free(text);
	}
	/* The CLI itself takes the default back, whatever its caller left. */
	{
		pid_t pid = fork();
		int st = 0;

		if (pid == 0) {
			signal(SIGCHLD, SIG_IGN);
			execl(cli, cli, "--version", (char *)NULL);
			_exit(127);
		}
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
		CHECK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "the CLI runs under a caller that ignores SIGCHLD");
	}
}

static const struct fx_fn gpu_ampere[] = {
	{ "0000:03:00.0", "2206", "030000", "nvidia" },
	{ "0000:03:00.1", "1aef", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL }
};

static const struct fx_fn gpu_ada[] = {
	{ "0000:03:00.0", "2783", "030000", "nvidia" },
	{ "0000:03:00.1", "22bc", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL }
};

static const struct fx_fn gpu_display[] = {
	{ "0000:04:00.0", "2206", "030000", "nvidia" },
	{ "0000:04:00.1", "1aef", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL }
};

/* A fresh fixture root with one card, and the simulator pointed at it. */
static void fixture(const char *name, const struct fx_topo *k, const struct fx_fn *fns, int boot_vga, const char *sriov,
		    char *root, size_t len)
{
	snprintf(root, len, "%s/%s", tmp, name);
	make_card(root, k, fns, boot_vga, sriov);
	sim_reset(root);
}

static void config_of(struct gs_config *cfg, const char *text)
{
	char err[256];

	gs_config_defaults(cfg);
	if (gs_config_parse(text, strlen(text), cfg, err, sizeof(err)))
		die(text);
}

/* ---- VF BAR sizing and the bridge growth envelope ---------------------- */

static struct gs_config act_cfg;
static struct gs_activate_opts act_opts;

static int activate_host(void)
{
	struct gs_host h = sim_host();

	return gs_activate(&h, &act_cfg, &act_opts);
}

static void fit_one(const char *name, const struct fx_topo *k, const struct fx_fn *fns, int vf_bars,
		    unsigned int vfs, struct gs_resource_fit *fit, char *root, size_t len)
{
	struct gs_host h;
	struct gs_addr pf;
	struct gs_card c;

	fixture(name, k, fns, 0, NULL, root, len);
	if (vf_bars)
		set_vf_bars(root, k, k->pf, 1);
	set_bridge_windows(root, k, 18, 1440);
	set_root_windows(root, 64, 4096);
	gs_addr_parse(k->pf, &pf);
	gs_card_inspect(root, &pf, &c);
	memset(&h, 0, sizeof(h));
	h.sys = root;
	CHECK(gs_resource_fit_assess(&h, &c, vfs, fit) == 0, "%s: assessment runs", name);
}

static void test_resource_fit(void)
{
	struct gs_resource_fit fit;
	char root[GS_PATH_MAX];

	/* Ampere, four VFs: need = 4 * 256 KiB non-prefetch, 4 * (256 + 32) MiB prefetch. */
	fit_one("fit-ampere", &topo_default, gpu_ampere, 0, 4, &fit, root, sizeof(root));
	CHECK(fit.state == GS_RESOURCE_FIT_OK && fit.vfs == 4 && fit.nonprefetch_need == 4ull * 262144 &&
	      fit.prefetch_need == 4ull * (268435456 + 33554432) && fit.nonprefetch_window == 18ull * 0x100000 &&
	      fit.prefetch_window == 1440ull * 0x100000,
	      "Ampere need is per-VF x count, not the aggregate rows: np=%llu pf=%llu win=%llu/%llu source=%s",
	      fit.nonprefetch_need, fit.prefetch_need, fit.nonprefetch_window, fit.prefetch_window, fit.source);
	CHECK(contains(fit.source, "current windows"), "Ampere source: %s", fit.source);

	/* Two VFs must halve the need (the per-VF value is never multiplied twice). */
	fit_one("fit-ampere-2", &topo_default, gpu_ampere, 0, 2, &fit, root, sizeof(root));
	CHECK(fit.prefetch_need == 2ull * (268435456 + 33554432), "two VFs: %llu", fit.prefetch_need);

	/* Ada's table is its own row set; one VF. */
	fit_one("fit-ada", &topo_default, gpu_ada, 0, 1, &fit, root, sizeof(root));
	CHECK(fit.state == GS_RESOURCE_FIT_OK && fit.nonprefetch_need == 262144 &&
	      fit.prefetch_need == 268435456 + 33554432 && contains(fit.source, "current windows"),
	      "Ada layout: np=%llu pf=%llu source=%s", fit.nonprefetch_need, fit.prefetch_need, fit.source);

	/* Already placed: nothing has to fit, whatever the windows say. */
	fixture("fit-assigned", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	{
		struct gs_host h = { .sys = root };
		struct gs_addr pf;
		struct gs_card c;

		gs_addr_parse("0000:03:00.0", &pf);
		gs_card_inspect(root, &pf, &c);
		CHECK(gs_resource_fit_assess(&h, &c, 4, &fit) == 0 && fit.state == GS_RESOURCE_FIT_OK &&
		      contains(fit.source, "assigned on this bus"), "assigned: %s", fit.source);
	}

	/* A window too small now but inside the room above the bridge: OK, the rescan grows it. */
	fixture("fit-grow", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	set_bridge_windows(root, &topo_default, 18, 128);
	set_root_windows(root, 64, 4096);
	{
		struct gs_host h = { .sys = root };
		struct gs_addr pf;
		struct gs_card c;

		gs_addr_parse("0000:03:00.0", &pf);
		gs_card_inspect(root, &pf, &c);
		CHECK(gs_resource_fit_assess(&h, &c, 4, &fit) == 0 && fit.state == GS_RESOURCE_FIT_OK &&
		      contains(fit.source, "grows on rescan; the room above it suffices"),
		      "growable window: state=%d source=%s", fit.state, fit.source);
	}

	/* Neither the window nor the room above it fits: insufficient, before any removal. */
	set_root_windows(root, 64, 256);
	{
		struct gs_host h = { .sys = root };
		struct gs_addr pf;
		struct gs_card c;

		gs_addr_parse("0000:03:00.0", &pf);
		gs_card_inspect(root, &pf, &c);
		CHECK(gs_resource_fit_assess(&h, &c, 4, &fit) == 0 && fit.state == GS_RESOURCE_FIT_INSUFFICIENT &&
		      contains(fit.source, "exceeds the bridge windows and the room above them"),
		      "insufficient: state=%d source=%s", fit.state, fit.source);
	}

	/* Unreadable windows: UNKNOWN, never OK. */
	fixture("fit-unknown", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	{
		struct gs_host h = { .sys = root };
		struct gs_addr pf;
		struct gs_card c;
		char p[GS_PATH_MAX];

		gs_pathf(p, sizeof(p), "%s/devices/%s/resource", root, topo_default.bridge_path);
		unlink(p);
		gs_addr_parse("0000:03:00.0", &pf);
		gs_card_inspect(root, &pf, &c);
		CHECK(gs_resource_fit_assess(&h, &c, 4, &fit) == 0 && fit.state == GS_RESOURCE_FIT_UNKNOWN &&
		      contains(fit.source, "bridge windows are unreadable"), "unknown: state=%d source=%s", fit.state,
		      fit.source);
	}

	/* Checked arithmetic: 16 VFs overflow the table's per-VF x count test, so no need is claimed. */
	fixture("fit-overflow", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	set_bridge_windows(root, &topo_default, 18, 4096);
	set_root_windows(root, 64, 8192);
	{
		struct gs_host h = { .sys = root };
		struct gs_addr pf;
		struct gs_card c;

		gs_addr_parse("0000:03:00.0", &pf);
		gs_card_inspect(root, &pf, &c);
		CHECK(gs_resource_fit_assess(&h, &c, 16, &fit) == 0 && fit.state == GS_RESOURCE_FIT_UNKNOWN &&
		      fit.nonprefetch_need == 0 && fit.prefetch_need == 0 && contains(fit.source, "no VF BAR size entry"),
		      "16 VFs: state=%d source=%s", fit.state, fit.source);
	}

	/* A removal whose need cannot be sized refuses before any write; nothing is changed. */
	fixture("fit-refuse", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	set_bridge_windows(root, &topo_default, 18, 128);
	set_root_windows(root, 64, 256);
	{
		char *out;

		config_of(&act_cfg, "bdfs=0000:03:00.0\n");
		memset(&act_opts, 0, sizeof(act_opts));
		CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
		      contains(out, "the VF need does not fit: source=the need exceeds the bridge windows and the room above them") &&
		      contains(out, "refused, nothing changed"), "insufficient refuses: %s", out);
		CHECK(!sim_text("sim.log"), "insufficient: nothing written");
		free(out);
	}
}

/* ---- display clients, holders and the runtime display plan -------------- */

static void test_display_plan(void)
{
	struct gs_config cfg;
	struct gs_display_plan plan;
	struct gs_host h;
	char root[GS_PATH_MAX];

	fixture("plan-clean", &topo_display, gpu_display, 1, "0/4", root, sizeof(root));
	config_of(&cfg, "bdfs=0000:04:00.0\n");
	h = sim_host();
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && !plan.inspection_unknown && !plan.manager_active &&
	      plan.reason[0] == '\0', "no clients: unknown=%d active=%d reason=%s", plan.inspection_unknown,
	      plan.manager_active, plan.reason);

	/* The manager is active; the unit it resolves to is named when the link is readable (live only). */
	sim.manager_active = 1;
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && plan.manager_active &&
	      strcmp(plan.manager_unit, "display-manager.service") == 0, "manager active: %s", plan.manager_unit);
	sim.manager_active = 0;

	/* The unit that display-manager.service points at is the one that is asked about and stopped. */
	{
		char link[GS_PATH_MAX], dir[GS_PATH_MAX];

		gs_pathf(dir, sizeof(dir), "%s/etc/systemd/system", sim.root);
		gs_mkdir_p(dir, 0755);
		gs_pathf(link, sizeof(link), "%s/display-manager.service", dir);
		link_to("/usr/lib/systemd/system/gdm.service", link);
		CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && strcmp(plan.manager_unit, "gdm.service") == 0 &&
		      !plan.inspection_unknown, "the display manager's own unit: %s", plan.manager_unit);
		unlink(link);
	}
	/* A session on a seat holds a display node, whatever the display manager's unit says; one without a seat does not. */
	sim_writef("run/systemd/sessions/4", "UID=0\nSEAT=\nTYPE=tty\n");
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && !plan.manager_active && !contains(plan.reason, "session 4"),
	      "a session without a seat: active=%d reason=%s", plan.manager_active, plan.reason);
	sim_writef("run/systemd/sessions/3", "UID=1000\nSEAT=seat0\nTYPE=wayland\n");
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && plan.manager_active &&
	      contains(plan.reason, "session 3 is active on a seat"), "a session on a seat: active=%d reason=%s",
	      plan.manager_active, plan.reason);
	sim_remove("run/systemd/sessions/3");
	sim_remove("run/systemd/sessions/4");

	/* A module holder graph is reported; a readable tree is not unknown. */
	sim_writef("module/nvidia_modeset/holders/nvidia_drm", "");
	sim_writef("module/nvidia_modeset/refcnt", "2\n");
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && !plan.inspection_unknown &&
	      contains(plan.reason, "module nvidia_modeset is held by nvidia_drm") &&
	      contains(plan.reason, "nvidia_modeset has refcnt 2"), "holders: unknown=%d reason=%s",
	      plan.inspection_unknown, plan.reason);

	/* A compositor that the unit does not own: a process holding a DRM node. */
	{
		char fd[GS_PATH_MAX], link[GS_PATH_MAX];

		gs_pathf(fd, sizeof(fd), "%s/proc/4242/fd", sim.root);
		gs_mkdir_p(fd, 0755);
		gs_pathf(link, sizeof(link), "%s/3", fd);
		link_to("/dev/dri/card0", link);
	}
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && contains(plan.reason, "process 4242 holds /dev/dri/card0"),
	      "process client: %s", plan.reason);

	/* Unreadable /proc is unknown, never "no clients". */
	{
		char proc[GS_PATH_MAX];

		gs_pathf(proc, sizeof(proc), "%s/proc", sim.root);
		if (chmod(proc, 0000))
			die(proc);
		CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && plan.inspection_unknown &&
		      contains(plan.reason, "cannot list"), "unreadable proc: unknown=%d reason=%s", plan.inspection_unknown,
		      plan.reason);
		if (chmod(proc, 0755))
			die(proc);
		gs_pathf(proc, sizeof(proc), "%s/proc/4242", sim.root);
		rm_rf(proc);
	}

	/* An unreadable holders directory is UNKNOWN, with the module named; a readable one is not. */
	{
		char holders[GS_PATH_MAX];

		gs_pathf(holders, sizeof(holders), "%s/module/nvidia_modeset/holders", sim.root);
		if (chmod(holders, 0000))
			die(holders);
		CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && plan.inspection_unknown &&
		      contains(plan.reason, "cannot list the holders of nvidia_modeset"),
		      "unreadable holders: unknown=%d reason=%s", plan.inspection_unknown, plan.reason);
		if (chmod(holders, 0755))
			die(holders);
	}
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && !plan.inspection_unknown &&
	      contains(plan.reason, "module nvidia_modeset is held by nvidia_drm"),
	      "readable again: unknown=%d reason=%s", plan.inspection_unknown, plan.reason);

	/* No DRM class directory at all is unknown as well. */
	{
		char drm[GS_PATH_MAX];

		gs_pathf(drm, sizeof(drm), "%s/class/drm", sim.root);
		rm_rf(drm);
	}
	CHECK(gs_display_plan_build(&h, &cfg, &plan) == 0 && plan.inspection_unknown &&
	      contains(plan.reason, "cannot list"), "missing class/drm: %s", plan.reason);
}

/* ---- boot display consumers and the firmware framebuffer handoff -------- */

static void display_inspect_case(const char *name, const struct fx_fn *fns, int boot_vga, const char *fw_kind,
				 const char *fw_driver, int consumers, const char *iomem_extra, const char *want_refuse,
				 char *root, size_t len)
{
	struct gs_addr pf;
	struct gs_card c;
	struct gs_display d;

	fixture(name, &topo_display, fns, boot_vga, NULL, root, len);
	if (fw_kind)
		add_firmware_fb(root, &topo_display, fw_kind, fw_driver);
	if (consumers) {
		char p[GS_PATH_MAX], dir[GS_PATH_MAX], target[GS_PATH_MAX];

		gs_pathf(dir, sizeof(dir), "%s/class/graphics/fb0", root);
		gs_mkdir_p(dir, 0755);
		gs_pathf(p, sizeof(p), "%s/device", dir);
		gs_pathf(target, sizeof(target), "../../../devices/%s/%s/%s", topo_display.bridge_path, topo_display.pf,
			 fw_kind);
		link_to(target, p);
	}
	{
		char p[GS_PATH_MAX];

		gs_pathf(p, sizeof(p), "%s/proc/iomem", root);
		writef(p, "00000000-00000fff : Reserved\n"
			  "80000000-fbffffff : PCI Bus 0000:00\n"
			  "  90000000-91ffffff : PCI Bus 0000:04\n"
			  "    90000000-90ffffff : %s\n"
			  "%s",
		       topo_display.pf, iomem_extra ? iomem_extra : "");
		gs_pathf(p, sizeof(p), "%s/proc/consoles", root);
		writef(p, "tty0                 -WU (EC p  )    4:7\n");
	}
	gs_addr_parse(topo_display.pf, &pf);
	gs_card_inspect(root, &pf, &c);
	memset(&d, 0, sizeof(d));
	CHECK(gs_display_inspect(root, sim.proc, &c, &d) == 0, "%s: inspect runs", name);
	if (want_refuse) {
		CHECK(contains(d.refuse, want_refuse), "%s: refuse=%s", name, d.refuse);
	} else {
		CHECK(d.refuse[0] == '\0', "%s: no refusal: %s", name, d.refuse);
	}
	(void)consumers;
}

static void test_display_inspect(void)
{
	char root[GS_PATH_MAX];

	/* Bound simple-framebuffer with one fb consumer and BOOTFB inside the PF's BAR: safe to release. */
	display_inspect_case("disp-ok", gpu_display, 1, "simple-framebuffer.0", "simple-framebuffer", 1,
			     "      90000000-9007ffff : BOOTFB\n", NULL, root, sizeof(root));
	{
		struct gs_addr pf;
		struct gs_card c;
		struct gs_display d;

		gs_addr_parse(topo_display.pf, &pf);
		gs_card_inspect(root, &pf, &c);
		gs_display_inspect(root, sim.proc, &c, &d);
		CHECK(strcmp(d.fw, "simple-framebuffer.0") == 0 && gs_display_fw_state(&d) == GS_FW_BOUND &&
		      d.nconsumers == 1, "fw=%s state=%d consumers=%u", d.fw, gs_display_fw_state(&d), d.nconsumers);
	}

	/* Released, but a framebuffer device of it remains: refuse. */
	display_inspect_case("disp-consumers", gpu_display, 1, "simple-framebuffer.0", NULL, 1, NULL,
			     "is unbound, but 1 fb/DRM device(s) of it remain", root, sizeof(root));
	/* Bound to simpledrm: only simple-framebuffer may be released. */
	display_inspect_case("disp-driver", gpu_display, 1, "simple-framebuffer.0", "simpledrm", 0, NULL,
			     "is bound to 'simpledrm', not to simple-framebuffer", root, sizeof(root));
	/* An EFI framebuffer is not a simple-framebuffer device. */
	display_inspect_case("disp-efi", gpu_display, 1, "efi-framebuffer.0", "simple-framebuffer", 0, NULL,
			     "is not a simple-framebuffer device; only simple-framebuffer is released", root, sizeof(root));
	/* A firmware framebuffer of another card cannot be attributed. */
	{
		struct gs_addr pf;
		struct gs_card c;
		struct gs_display d;
		char other[GS_PATH_MAX];

		fixture("disp-other", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
		add_firmware_fb(root, &topo_default, "simple-framebuffer.1", "simple-framebuffer");
		gs_pathf(other, sizeof(other), "%s/proc/iomem", root);
		writef(other, "80000000-fbffffff : PCI Bus 0000:00\n"
			      "  90000000-90ffffff : %s\n"
			      "    90000000-9007ffff : BOOTFB\n",
		       topo_display.pf);
		gs_pathf(other, sizeof(other), "%s/proc/consoles", root);
		writef(other, "tty0                 -WU (EC p  )    4:7\n");
		gs_addr_parse(topo_display.pf, &pf);
		gs_card_inspect(root, &pf, &c);
		gs_display_inspect(root, sim.proc, &c, &d);
		CHECK(contains(d.refuse, "belongs to") && contains(d.refuse, "not to this card"), "other card's fw: %s",
		      d.refuse);
	}
	/* An unrelated resource inside a BAR refuses. */
	display_inspect_case("disp-iomem", gpu_display, 1, "simple-framebuffer.0", "simple-framebuffer", 0,
			     "      90000000-9007ffff : BAR 0\n", "uses memory inside a BAR", root, sizeof(root));
	/* A released BOOTFB that still has a child refuses. */
	display_inspect_case("disp-bootfb", gpu_display, 1, "simple-framebuffer.0", NULL, 0,
			     "      90000000-9007ffff : BOOTFB\n"
			     "        90000000-90000fff : simpledrm\n",
			     "still holds memory inside BOOTFB", root, sizeof(root));
	/* An early efifb console refuses. */
	{
		struct fx_fn fns[] = { { "0000:04:00.0", "2206", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };
		struct gs_addr pf;
		struct gs_card c;
		struct gs_display d;
		char p[GS_PATH_MAX];

		fixture("disp-consoles", &topo_display, fns, 1, NULL, root, sizeof(root));
		add_firmware_fb(root, &topo_display, "simple-framebuffer.0", "simple-framebuffer");
		gs_pathf(p, sizeof(p), "%s/proc/iomem", root);
		writef(p, "80000000-fbffffff : PCI Bus 0000:00\n"
			  "  90000000-90ffffff : %s\n"
			  "    90000000-9007ffff : BOOTFB\n",
		       topo_display.pf);
		gs_pathf(p, sizeof(p), "%s/proc/consoles", root);
		writef(p, "efifb                -WU (EC p  )    4:7\n");
		gs_addr_parse(topo_display.pf, &pf);
		gs_card_inspect(root, &pf, &c);
		gs_display_inspect(root, sim.proc, &c, &d);
		CHECK(contains(d.refuse, "an early EFI framebuffer console"), "efifb console: %s", d.refuse);
	}
}

/* ---- check and check --daemon-ready ------------------------------------ */

static struct gs_config check_cfg;
static struct gs_activate_opts check_opts;

static int check_host(void)
{
	struct gs_host h = sim_host();

	return gs_check_vfs(&h, &check_cfg, &check_opts);
}

static int daemon_host(void)
{
	struct gs_host h = sim_host();

	return gs_daemon_ready(&h, &check_cfg, &check_opts);
}

/* An Ampere host whose four VFs are enabled and bound, with the given vGPU type on each. */
static void ready_host(const char *name, const char *type, char *root, size_t len)
{
	fixture(name, &topo_default, gpu_ampere, 0, "0/4", root, len);
	memset(&check_opts, 0, sizeof(check_opts));
	config_of(&check_cfg, "bdfs=0000:03:00.0\n");
	sim_enable_vfs("0000:03:00.0");
	set_vf_bars(sim.root, &topo_default, "0000:03:00.0", 1);
	if (type) {
		char rel[128];

		for (unsigned int i = 4; i <= 7; i++) {
			snprintf(rel, sizeof(rel), "bus/pci/devices/0000:03:00.%u/nvidia/current_vgpu_type", i);
			sim_writef(rel, type);
		}
	}
}

static void test_check(void)
{
	char root[GS_PATH_MAX], *out;

	ready_host("check-ready", "525", root, sizeof(root));
	CHECK(run_host(check_host, &out) == GS_EXIT_OK &&
	      contains(out, "OK 0000:03:00.4 (VF of 0000:03:00.0): driver=nvidia type=525 expected=525 "
			    "power/control=on runtime_status=active") &&
	      contains(out, "OK 0000:03:00.7 (VF of 0000:03:00.0)"), "ready: %s", out);
	free(out);

	/* One VF with another type: NOT READY, exit 1, and the guidance names what to do. */
	sim_writef("bus/pci/devices/0000:03:00.5/nvidia/current_vgpu_type", "942\n");
	CHECK(run_host(check_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "NOT READY 0000:03:00.5 (VF of 0000:03:00.0): driver=nvidia type=942 expected=525") &&
	      contains(out, "not ready: start no VM on a VF marked NOT READY"), "wrong type: %s", out);
	free(out);
	sim_writef("bus/pci/devices/0000:03:00.5/nvidia/current_vgpu_type", "525\n");

	/* The PF unbound, no VF at all, and a VF off nvidia. */
	sim_remove("bus/pci/devices/0000:03:00.0/driver");
	CHECK(run_host(check_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "NOT READY 0000:03:00.0: bound to nothing, not nvidia"), "PF unbound: %s", out);
	free(out);

	ready_host("check-novf", "525", root, sizeof(root));
	sim_writef("bus/pci/devices/0000:03:00.0/sriov_numvfs", "0\n");
	CHECK(run_host(check_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "NOT READY 0000:03:00.0: no VF is enabled (`geforce-sriov activate` enables them)"),
	      "no VF: %s", out);
	free(out);

	/* The guard blocks readiness; the activating marker names activate's own run. */
	ready_host("check-guard", "525", root, sizeof(root));
	sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=x\nstep=reprobe\n");
	CHECK(run_host(check_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "NOT READY: ") && contains(out, GS_ATTEMPT_NAME) &&
	      contains(out, "an earlier activation did not finish; start no VM"), "guard: %s", out);
	free(out);
	sim_remove("state/" GS_ATTEMPT_NAME);
	sim_writef("run/" GS_ACTIVATING_NAME, "boot_id=x\n");
	CHECK(run_host(check_host, &out) == GS_EXIT_FAIL && contains(out, GS_ACTIVATING_NAME) &&
	      contains(out, "`geforce-sriov activate` is running or did not finish"), "activating: %s", out);
	free(out);
	sim_remove("run/" GS_ACTIVATING_NAME);

	/* A laptop VF whose config space reads all ones is dead: NOT READY with the power-off hint. */
	{
		static const struct fx_fn lap[] = {
			{ "0000:03:00.0", "28e0", "030000", "nvidia" },
			{ NULL, NULL, NULL, NULL }
		};
		unsigned char ones[256];

		fixture("check-dead-vf", &topo_default, lap, 0, "0/1", root, sizeof(root));
		memset(&check_opts, 0, sizeof(check_opts));
		config_of(&check_cfg, "bdfs=0000:03:00.0\n");
		sim_enable_vfs("0000:03:00.0");
		sim_writef("bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type", "942\n");
		memset(ones, 0xff, sizeof(ones));
		{
			char p[GS_PATH_MAX];

			gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:03:00.4/config", sim.root);
			write_bin(p, ones, sizeof(ones));
		}
		CHECK(run_host(check_host, &out) == GS_EXIT_FAIL &&
		      contains(out, "its config space reads ffffffff at 0x08, or cannot be read; the GPU may have "
				    "been powered off"), "dead laptop VF: %s", out);
		free(out);
	}
}

static void test_daemon_ready(void)
{
	char root[GS_PATH_MAX], *out;

	/* Ready: types and VF power are not part of the admission predicate. */
	ready_host("ready-daemon", "0", root, sizeof(root));
	for (unsigned int i = 4; i <= 7; i++) {
		char rel[128];

		snprintf(rel, sizeof(rel), "bus/pci/devices/0000:03:00.%u/power/control", i);
		sim_writef(rel, "auto\n");
		snprintf(rel, sizeof(rel), "bus/pci/devices/0000:03:00.%u/power/runtime_status", i);
		sim_writef(rel, "suspended\n");
	}
	CHECK(run_host(daemon_host, &out) == GS_EXIT_OK && out[0] == '\0', "ready without published types: %d %s",
	      failures, out);
	free(out);

	/* activate's own marker must not reject admission; a guard without it must. */
	{
		char lock[GS_PATH_MAX];
		int fd;

		/* The running activation holds the lock; this process stands in for it. */
		gs_pathf(lock, sizeof(lock), "%s/lock", sim.run);
		fd = open(lock, O_RDWR | O_CREAT, 0600);
		if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB))
			die(lock);
		sim_writef("run/" GS_ACTIVATING_NAME, "boot_id=x\n");
		CHECK(run_host(daemon_host, &out) == GS_EXIT_OK && out[0] == '\0', "activating marker ignored: %s", out);
		free(out);
		sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=x\n");
		CHECK(run_host(daemon_host, &out) == GS_EXIT_OK && out[0] == '\0',
		      "the running activation's own guard is ignored while its marker is present: %s", out);
		free(out);
		/* The activation was killed: its marker is still there, the lock is not held any more. */
		close(fd);
		CHECK(run_host(daemon_host, &out) == GS_EXIT_REFUSED && contains(out, "the activation lock"),
		      "a marker left by a killed activation does not hide the guard: %s", out);
		free(out);
	}
	sim_remove("run/" GS_ACTIVATING_NAME);
	CHECK(run_host(daemon_host, &out) == GS_EXIT_REFUSED && contains(out, "the activation lock"), "guard alone: %s",
	      out);
	free(out);
	sim_remove("state/" GS_ATTEMPT_NAME);

	/* Not ready: a missing PF, a wrong VF count, a VF off nvidia, an unassigned BAR. */
	sim_remove("bus/pci/devices/0000:03:00.0");
	CHECK(run_host(daemon_host, &out) == GS_EXIT_REFUSED && contains(out, "0000:03:00.0 is missing"),
	      "missing PF: %s", out);
	free(out);
	ready_host("ready-daemon-2", "0", root, sizeof(root));
	sim_writef("bus/pci/devices/0000:03:00.0/sriov_numvfs", "2\n");
	CHECK(run_host(daemon_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "0000:03:00.0 has 2 of 4 VFs enabled"), "wrong count: %s", out);
	free(out);
	ready_host("ready-daemon-3", "0", root, sizeof(root));
	sim_remove("bus/pci/devices/0000:03:00.6/driver");
	CHECK(run_host(daemon_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "VF 0000:03:00.6 of 0000:03:00.0 is bound to nothing, not nvidia"), "VF unbound: %s", out);
	free(out);
	ready_host("ready-daemon-4", "0", root, sizeof(root));
	{
		/* BAR0 unassigned: rewrite line 0 only, VF BAR rows stay assigned. */
		char p[GS_PATH_MAX];
		char lines[13][128];
		char text[13 * 128];
		size_t off = 0;

		gs_pathf(p, sizeof(p), "%s/devices/%s/0000:03:00.0/resource", sim.root, topo_default.bridge_path);
		snprintf(lines[0], sizeof(lines[0]), "0x0 0x0 0x0\n");
		for (unsigned int i = 1; i <= 12; i++)
			snprintf(lines[i], sizeof(lines[i]), "0x%016llx 0x%016llx 0x000000000014220c\n",
				 0x387fc0000000ull + (i - 1) * 0x1000000ull,
				 0x387fc0000000ull + (i - 1) * 0x1000000ull + 0xffffffull);
		for (unsigned int i = 0; i <= 12; i++) {
			size_t n = strlen(lines[i]);

			memcpy(text + off, lines[i], n);
			off += n;
		}
		write_bin(p, text, off);
	}
	CHECK(run_host(daemon_host, &out) == GS_EXIT_REFUSED && contains(out, "BAR0 or a VF BAR is unassigned"),
	      "unassigned BAR: %s", out);
	free(out);
}

/* ---- activation --------------------------------------------------------- */

/*
 * A host activation can bring up: the livepatch is already loaded with the configured parameters
 * (the early boot unit's work), NVIDIA's driver is bound, the card hides SR-IOV (so it needs the
 * empty-bus resize) and, when display, the display stack is loaded and the PF has a bound
 * simple-framebuffer. The default action is --boot with short waits.
 */
static void set_section(const char *name);

static void act_host(const char *name, const struct fx_topo *k, const struct fx_fn *fns, int boot_vga, const char *sriov,
		     char *root, size_t len)
{
	char p[GS_PATH_MAX], rel[256];

	fixture(name, k, fns, boot_vga, sriov, root, len);
	/* Name the fixture in the progress line: a stall inside this group names the case. */
	set_section(name);
	set_bridge_windows(root, k, 18, 1440);
	set_root_windows(root, 64, 8192);
	sim_writef("proc/driver/nvidia/version",
		   "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.159.01  Release Build\n"
		   "GCC version:  gcc version 12.2.0 (Debian 12.2.0-14)\n");
	sim_writef("proc/driver/nvidia/params", "ResmanDebugLevel: 4294967295\nEnableGpuFirmware: 0\nRegistryDwords: \"\"\n");
	snprintf(rel, sizeof(rel), "proc/driver/nvidia/gpus/%s/unbindLock", k->pf);
	sim_writef(rel, "0\n");
	sim_writef("module/geforce_sriov/parameters/ari", "Y\n");
	sim_writef("module/geforce_sriov/parameters/msix", "Y\n");
	sim_writef("module/geforce_sriov/parameters/vfs", "4\n");
	{
		char b[GS_BDF_STR_SIZE + 8];

		snprintf(b, sizeof(b), "%s\n", k->pf);
		sim_writef("module/geforce_sriov/parameters/bdfs", b);
	}
	sim_writef("kernel/livepatch/geforce_sriov/enabled", "1\n");
	sim_writef("kernel/livepatch/geforce_sriov/transition", "0\n");
	sim_writef("module/geforce_sriov/refcnt", "0\n");
	sim_module("nvidia");
	if (boot_vga) {
		sim_module("nvidia_modeset");
		sim_module("nvidia_drm");
		add_firmware_fb(root, k, "simple-framebuffer.0", "simple-framebuffer");
		gs_pathf(p, sizeof(p), "%s/class/graphics/fb0", root);
		gs_mkdir_p(p, 0755);
		gs_pathf(p, sizeof(p), "%s/class/graphics/fb0/device", root);
		{
			char target[GS_PATH_MAX];

			gs_pathf(target, sizeof(target), "../../../devices/%s/%s/simple-framebuffer.0", k->bridge_path, k->pf);
			link_to(target, p);
		}
		add_iomem_fw(root, k, 1);
		gs_copy(sim.display_pf, sizeof(sim.display_pf), k->pf);
	}
	config_of(&act_cfg, "");
	act_cfg.nbdfs = 1;
	gs_addr_parse(k->pf, &act_cfg.bdfs[0]);
	memset(&act_opts, 0, sizeof(act_opts));
	act_opts.boot = 1;
	act_opts.boot_wait_ms = 1;
	act_opts.settle_ms = 1;
	act_opts.types_wait_ms = 100;
}

/* The lines the empty-bus resize prints before a successful enable. */
static void check_rescan_out(const char *out)
{
	CHECK(contains(out, "livepatch geforce_sriov already loaded"), "livepatch reused: %s", out);
	CHECK(contains(out, "0000:03:00.0: bridge 0000:00:02.0 power/control=on runtime_status=active (held awake)"),
	      "bridge readback: %s", out);
	CHECK(contains(out, "0000:03:00.0: remove 0000:03:00.1") && contains(out, "0000:03:00.0: remove 0000:03:00.0"),
	      "companions then PF removed: %s", out);
	CHECK(contains(out, "0000:03:00.0: rescan ") && contains(out, "MSI-X") == 0 &&
	      contains(out, "sriov_totalvfs=4 ari_enabled=1 msix_linked=1, VF BARs assigned"),
	      "SR-IOV, ARI and MSI-X re-presented: %s", out);
	CHECK(contains(out, "4 VFs enabled and bound to nvidia"), "VF enable: %s", out);
	CHECK(contains(out, "activation finished"), "finished: %s", out);
}

static struct gs_livepatch_opts lp_opts;

static int livepatch_host(void)
{
	struct gs_host h = sim_host();
	char config[GS_PATH_MAX];

	gs_pathf(config, sizeof(config), "%s/livepatch.conf", sim.root);
	lp_opts.host = &h;
	lp_opts.config_path = config;
	return gs_cmd_livepatch(&lp_opts);
}

static void test_livepatch_guard(void)
{
	char root[GS_PATH_MAX], *out, *log;

	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	act_host("livepatch-guard", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	sim_writef("proc/sys/kernel/random/boot_id", "11111111-1111-1111-1111-111111111111\n");
	sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=11111111-1111-1111-1111-111111111111\n");
	sim_remove("module/geforce_sriov");
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "Nothing done"),
	      "same boot blocks livepatch: %s", out);
	free(out);
	CHECK(!sim_text("sim.log"), "same boot causes no module load");
	sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=22222222-2222-2222-2222-222222222222\n");
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && contains(out, "the lock remains set"),
	      "previous boot permits livepatch: %s", out);
	free(out);
	log = sim_text("sim.log");
	CHECK(contains(log, "R modprobe geforce_sriov") && !contains(log, "sriov-manage") &&
	      !contains(log, "/rescan") && sim_exists("state/" GS_ATTEMPT_NAME),
	      "only livepatch loaded and guard retained: %s", log);
	free(log);
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "nothing done"),
	      "previous boot still blocks activation: %s", out);
	free(out);
	sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=unknown\n");
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_COLD_CYCLE, "unknown ID fails closed: %s", out);
	free(out);
}

/* --load-driver: nvidia only after the livepatch is present and enabled, and only on request. */
static void test_livepatch_load_driver(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* The livepatch modprobe comes first, then nvidia, in one run. */
	fixture("livepatch-order", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	sim_writef("kernel/livepatch/geforce_sriov/enabled", "1\n");
	sim_writef("kernel/livepatch/geforce_sriov/transition", "0\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK &&
	      contains(out, "nvidia loaded after the livepatch\n") && !contains(out, "before udev"),
	      "livepatch then driver: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "R modprobe geforce_sriov") >= 0 && pos_of(log, "R modprobe nvidia") > 0 &&
	      pos_of(log, "R modprobe nvidia") > pos_of(log, "R modprobe geforce_sriov") &&
	      sim_exists("module/nvidia"), "the driver loads only after the livepatch: %s", log);
	free(log);
	free(out);

	/* The livepatch load fails: no driver load follows. */
	fixture("livepatch-refuse", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	sim.run_fail = "modprobe geforce_sriov";
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "modprobe geforce_sriov exited with 1"), "livepatch refused: %s", out);
	sim.run_fail = NULL;
	log = sim_text("sim.log");
	CHECK(contains(log, "R modprobe geforce_sriov") && !contains(log, "R modprobe nvidia") &&
	      !sim_exists("module/nvidia"), "a refused livepatch loads no driver: %s", log);
	free(log);
	free(out);

	/* Without --load-driver the command still loads the livepatch only. */
	fixture("livepatch-noflag", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	sim_writef("kernel/livepatch/geforce_sriov/enabled", "1\n");
	sim_writef("kernel/livepatch/geforce_sriov/transition", "0\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK, "livepatch without the flag: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "R modprobe nvidia") && !sim_exists("module/nvidia"),
	      "no driver without the flag: %s", log);
	free(log);
	free(out);

	/* nvidia is already resident: it is never modprobed again. */
	act_host("livepatch-bound", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK &&
	      contains(out, "nvidia is already loaded; not loading it again"), "bound driver: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "R modprobe nvidia"), "an already loaded driver is not loaded again: %s", log);
	free(log);
	free(out);

	/* The escape hatch: skip means no livepatch and no driver, and the run says so. */
	act_host("livepatch-skip", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	sim_writef("proc/cmdline", "BOOT_IMAGE=/vmlinuz-6.8.0 geforce_sriov.skip\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && contains(out, "no NVIDIA driver") &&
	      contains(out, "modprobe nvidia"), "skip names the manual driver step: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "R modprobe"), "skip loads no module at all: %s", log ? log : "");
	free(log);
	free(out);

	/* The boot unit asks for that mode exactly once. */
	{
		char p[GS_PATH_MAX], *unit;

		gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-livepatch.service.in", srcdir);
		unit = slurp(p);
		CHECK(unit && count_of(unit, "ExecStart=@BINDIR@/geforce-sriov livepatch --boot\n") == 1 &&
		      !contains(unit, "--load-driver"),
		      "the early unit loads the livepatch only: %s", unit ? unit : "(missing)");
		free(unit);
	}
}

/* ---- NVIDIA GPUs that are not selected: nvidia loads after udev's rules, status lists them ---- */

static const struct fx_fn gpu_other04[] = {
	{ "0000:04:00.0", "2d04", "030000", NULL },
	{ "0000:04:00.1", "22bc", "040300", NULL },
	{ NULL, NULL, NULL, NULL }
};
static const struct fx_fn gpu_other04_vfio[] = {
	{ "0000:04:00.0", "2d04", "030000", "vfio-pci" },
	{ NULL, NULL, NULL, NULL }
};
static const struct fx_fn gpu_other04_nvidia[] = {
	{ "0000:04:00.0", "2d04", "030000", "nvidia" },
	{ NULL, NULL, NULL, NULL }
};
static const struct fx_fn gpu_3070_05[] = {
	{ "0000:05:00.0", "2484", "030000", "nvidia" },
	{ NULL, NULL, NULL, NULL }
};
static const struct fx_fn gpu_ampere_vfio[] = {
	{ "0000:03:00.0", "2206", "030000", "vfio-pci" },
	{ NULL, NULL, NULL, NULL }
};

static void set_override(const char *root, const struct fx_topo *k, const char *bdf, const char *value)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/driver_override", root, k->bridge_path, bdf);
	writef(p, "%s\n", value);
}

/* A boot step host: the selected Ampere card at 03:00.0 and an unselected card at 04:00.0, nvidia not loaded. */
static void unsel_host(const char *name, const struct fx_fn *other, const char *override, char *root, size_t len)
{
	fixture(name, &topo_default, gpu_ampere, 0, NULL, root, len);
	make_card(root, &topo_display, other, 0, NULL);
	if (override)
		set_override(root, &topo_display, "0000:04:00.0", override);
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	sim_writef("kernel/livepatch/geforce_sriov/enabled", "1\n");
	sim_writef("kernel/livepatch/geforce_sriov/transition", "0\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
}

/*
 * An activation host: the selected Ampere card at 03:00.0 and an unselected card at 04:00.0. nvidia is not loaded,
 * so activation loads it and the cards bind to it as they do at a boot.
 */
static void unsel_act(const char *name, const struct fx_fn *other, const char *override, char *root, size_t len)
{
	act_host(name, &topo_default, gpu_ampere, 0, NULL, root, len);
	make_card(root, &topo_display, other, 0, NULL);
	if (override)
		set_override(root, &topo_display, "0000:04:00.0", override);
	sim_remove("module/nvidia");
	sim.gpu_info = 1;
}

static struct gs_diag_opts unsel_status_opts;
static int unsel_status_host(void)
{
	return gs_cmd_status(&unsel_status_opts);
}

static int unsel_status(const char *root, char **out)
{
	char cfg[GS_PATH_MAX];

	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	memset(&unsel_status_opts, 0, sizeof(unsel_status_opts));
	unsel_status_opts.sysroot = root;
	unsel_status_opts.config_path = cfg;
	return run_host(unsel_status_host, out);
}

static void test_unselected(void)
{
	char root[GS_PATH_MAX], p[GS_PATH_MAX], *out, *log, *unit;
	static const struct fx_topo topo_05 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };
	int rc, rc_without;

	/* Units: the early unit loads the livepatch only; the activate unit loads the driver first, before the display manager. */
	gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-livepatch.service.in", srcdir);
	unit = slurp(p);
	CHECK(unit && count_of(unit, "ExecStart=") == 1 && count_of(unit, "ExecStart=@BINDIR@/geforce-sriov livepatch --boot\n") == 1 &&
	      !contains(unit, "load-driver") && contains(unit, "Before=systemd-udevd.service"),
	      "early unit: livepatch only, still before udev: %s", unit ? unit : "(missing)");
	free(unit);
	gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-activate.service.in", srcdir);
	unit = slurp(p);
	CHECK(unit && count_of(unit, "ExecStartPre=-@BINDIR@/geforce-sriov livepatch --boot --load-driver\n") == 1 &&
	      count_of(unit, "ExecStartPre=") == 1 && count_of(unit, "\nNotifyAccess=exec\n") == 1 &&
	      pos_of(unit, "ExecStartPre=") > 0 && pos_of(unit, "ExecStartPre=") < pos_of(unit, "\nExecStart=@BINDIR@") &&
	      contains(unit, "\nBefore=display-manager.service ") && pos_of(unit, "\n[Unit]") < pos_of(unit, "\nBefore=display-manager.service") &&
	      pos_of(unit, "\nBefore=display-manager.service") < pos_of(unit, "\n[Service]"),
	      "activate unit: pre-step before ExecStart, same unit as Before=display-manager: %s", unit ? unit : "(missing)");
	free(unit);

	/* Order: udevadm settle, then modprobe nvidia; the selected card is bound; no unselected card is touched. */
	unsel_host("unsel-order", gpu_other04, NULL, root, sizeof(root));
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && contains(out, "udevadm settle --timeout=30\n") &&
	      contains(out, "nvidia loaded after the livepatch\n") && !contains(out, "udev has not finished"),
	      "settle then load: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "R udevadm settle --timeout=30") >= 0 &&
	      pos_of(log, "R modprobe nvidia") > pos_of(log, "R udevadm settle --timeout=30") &&
	      pos_of(log, "R modprobe nvidia") > pos_of(log, "R modprobe geforce_sriov"), "the order in the log: %s", log);
	free(log);
	/* A second wait follows the load; the boot step says nothing about the unselected card and opens none. */
	CHECK(count_of(out, "udevadm settle --timeout=30\n") == 2 &&
	      pos_of(out, "nvidia loaded after the livepatch\n") < pos_of_last(out, "udevadm settle --timeout=30\n") &&
	      !contains(out, "0000:04:00.0 (10de:2d04)") && !contains(out, "NVIDIA GPU not selected"),
	      "settle after the load, no unselected card test: %s", out);
	free(out);
	/* No driver rule and none of the user's: nvidia takes the card, as in 0.4.0. */
	CHECK(sim_exists("bus/pci/devices/0000:04:00.0/driver"), "an unselected card without a rule is bound by nvidia");

	/* udev not idle after 30 s: the load goes on and the line says what may be missing. */
	unsel_host("unsel-settle-fail", gpu_other04, NULL, root, sizeof(root));
	sim.settle_fail = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK &&
	      contains(out, "udev has not finished its queue after 30 s; a driver_override rule of yours may not have applied yet\n") &&
	      contains(out, "nvidia loaded after the livepatch\n"), "settle failure continues: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "R modprobe nvidia") > pos_of(log, "R udevadm settle"), "still loaded after the wait: %s", log);
	free(log);
	free(out);
	sim.settle_fail = 0;
	/* No udevadm: the line names that, not a wait that did not happen. */
	unsel_host("unsel-no-udevadm", gpu_other04, NULL, root, sizeof(root));
	sim.settle_fail = 2;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK &&
	      contains(out, "udevadm was not found, so udev's queue was not waited for; a driver_override rule of yours may not have applied yet\n") &&
	      !contains(out, "udev has not finished") && contains(out, "nvidia loaded after the livepatch\n"),
	      "udevadm missing: %s", out);
	free(out);
	sim.settle_fail = 0;

	/* driver_override=vfio-pci, no driver: nvidia does not bind it; the selected card is bound. */
	unsel_host("unsel-override", gpu_other04, "vfio-pci", root, sizeof(root));
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK, "override: %s", out);
	CHECK(!sim_exists("bus/pci/devices/0000:04:00.0/driver") && sim_exists("bus/pci/devices/0000:03:00.0/driver") &&
	      !sim_exists("proc/driver/nvidia/gpus/0000:04:00.0/unbindLock"),
	      "the overridden card is not offered to nvidia, the selected one is bound");
	free(out);
	/* driver_override=nvidia: the user asked for nvidia. */
	unsel_host("unsel-override-nvidia", gpu_other04, "nvidia", root, sizeof(root));
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && sim_exists("bus/pci/devices/0000:04:00.0/driver"),
	      "override=nvidia is bound: %s", out);
	free(out);

	/* L2: a card NVIDIA's driver cannot start is released; every other case is left alone. */
	{
		static const struct fx_fn gpu_3070_04[] = {
			{ "0000:04:00.0", "2484", "030000", NULL },
			{ NULL, NULL, NULL, NULL }
		};
		static const struct fx_fn gpu_other04_vf[] = {
			{ "0000:05:00.2", "2206", "030200", "nvidia" },
			{ NULL, NULL, NULL, NULL }
		};
		static const struct fx_fn gpu_3d04[] = {
			{ "0000:04:00.0", "2d04", "030200", NULL },
			{ NULL, NULL, NULL, NULL }
		};
		static const struct fx_fn gpu_other05[] = {
			{ "0000:05:00.0", "2d04", "030000", NULL },
			{ NULL, NULL, NULL, NULL }
		};
		static const struct fx_topo topo_05_l2 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };
		char *ov, node[GS_PATH_MAX];

		/* Refused (open gives EIO): override, unbind, stale node removed, one line, exit 0. */
		unsel_act("unsel-l2-refused", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      count_of(out, "0000:04:00.0 (10de:2d04): ") == 1 &&
		      contains(out, "0000:04:00.0 (10de:2d04): NVIDIA's driver cannot start this GPU (open /dev/nvidia4: Input/output error). Released: no driver binds it now (driver_override=geforce-sriov-released)\n"),
		      "refused card is released: %s", out);
		ov = sim_text("bus/pci/devices/0000:04:00.0/driver_override");
		CHECK(ov && contains(ov, GS_UNSELECTED_RELEASED), "the override is set: %s", ov ? ov : "(none)");
		free(ov);
		gs_pathf(node, sizeof(node), "%s/dev/nvidia4", root);
		CHECK(!sim_exists("bus/pci/devices/0000:04:00.0/driver") && !sim_exists("dev/nvidia4"),
		      "unbound, stale node gone");
		log = sim_text("sim.log");
		CHECK(count_of(log, "O /dev/nvidia4\n") == 1 && contains(log, "R nvidia-modprobe -c 4\n") &&
		      pos_of(log, "gpus/0000:04:00.0/unbindLock 1") > pos_of(log, "O /dev/nvidia4\n") &&
		      pos_of(log, "W bus/pci/devices/0000:04:00.0/driver_override geforce-sriov-released") >
		      pos_of(log, "gpus/0000:04:00.0/unbindLock 1") &&
		      pos_of(log, "W bus/pci/drivers/nvidia/unbind 0000:04:00.0") >
		      pos_of(log, "W bus/pci/devices/0000:04:00.0/driver_override geforce-sriov-released"),
		      "order of the release: %s", log);
		CHECK(pos_of(log, "R modprobe nvidia\n") >= 0 && pos_of(log, "O /dev/nvidia4\n") > pos_of(log, "R modprobe nvidia\n") &&
		      pos_of(log, "R modprobe nvidia_vgpu_vfio\n") > pos_of(log, "O /dev/nvidia4\n") && !contains(log, "R rmmod nvidia\n"),
		      "modprobe nvidia < the test open < modprobe nvidia_vgpu_vfio, no rmmod nvidia: %s", log);
		free(log);
		free(out);
		/*
		 * A second activation in the same boot has nothing to do (every VF is configured), so it opens no node
		 * and the released card stays without a driver.
		 */
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "there is nothing to do") &&
		      !contains(out, "NVIDIA's driver cannot start this GPU"), "second activation: %s", out);
		log = sim_text("sim.log");
		CHECK(count_of(log, "O /dev/nvidia4\n") == 1 && count_of(log, "driver_override geforce-sriov-released") == 1 &&
		      !sim_exists("bus/pci/devices/0000:04:00.0/driver"),
		      "the released card is not opened again and stays driverless: %s", log);
		free(log);
		free(out);

		/* A card the driver starts stays bound; nothing is written for it. */
		unsel_act("unsel-l2-works", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): NVIDIA's driver started this GPU (open /dev/nvidia4 worked); left on nvidia\n"),
		      "working card: %s", out);
		log = sim_text("sim.log");
		CHECK(sim_exists("bus/pci/devices/0000:04:00.0/driver") && !contains(log, "0000:04:00.0/driver_override") &&
		      !contains(log, "gpus/0000:04:00.0/unbindLock") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0"),
		      "working card untouched: %s", log);
		CHECK(sim_exists("dev/nvidia4"), "a working unselected card keeps /dev/nvidia4 that the test made");
		free(log);
		free(out);

		/*
		 * An activation that stops after the test (nvidia_vgpu_vfio does not load) leaves its lock and the card's
		 * marker. Once the lock is cleared and activation runs again in the same boot, the card is not opened again;
		 * and when the marker is there and the run reaches the step, the step skips the card.
		 */
		unsel_act("unsel-l2-rerun", gpu_other04, "", root, sizeof(root));
		sim.run_fail = "modprobe nvidia_vgpu_vfio";
		CHECK(run_host(activate_host, &out) != GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): NVIDIA's driver started this GPU (open /dev/nvidia4 worked); left on nvidia\n"),
		      "the first run stops after the test: %s", out);
		free(out);
		sim.run_fail = NULL;
		sim_remove("state/" GS_ATTEMPT_NAME);
		sim_remove("run/" GS_ACTIVATING_NAME);
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK && sim_exists("run/unselected-tested-0000:04:00.0"),
		      "the second run finishes, the marker stays: %s", out);
		log = sim_text("sim.log");
		CHECK(count_of(log, "O /dev/nvidia4\n") == 1 && sim_exists("bus/pci/devices/0000:04:00.0/driver"),
		      "one open of the card in the boot: %s", log);
		free(log);
		free(out);

		/* A card whose marker is there is skipped, even though the driver would refuse it. */
		unsel_act("unsel-l2-marked", gpu_other04, "", root, sizeof(root));
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		sim_writef("run/unselected-tested-0000:04:00.0", "\n");
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): not tested again: it was tested earlier in this boot; left as it is\n") &&
		      sim_exists("bus/pci/devices/0000:04:00.0/driver"), "a marked card is not tested: %s", out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "O /dev/nvidia4") && !contains(log, "0000:04:00.0/driver_override"),
		      "a marked card is not opened or written: %s", log ? log : "");
		free(log);
		free(out);

		/* The boot display GPU is not tested at all, even when the driver would refuse it. */
		unsel_act("unsel-l2-bootvga", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		gs_pathf(node, sizeof(node), "%s/devices/%s/0000:04:00.0/boot_vga", root, topo_display.bridge_path);
		writef(node, "1\n");
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "NVIDIA GPU not selected, 0000:04:00.0 (10de:2d04): not tested; it is the boot display GPU. Left on nvidia\n"),
		      "boot display card: %s", out);
		log = sim_text("sim.log");
		CHECK(sim_exists("bus/pci/devices/0000:04:00.0/driver") && !contains(log, "O /dev/nvidia4") &&
		      !contains(log, "R nvidia-modprobe -c 4\n") && !contains(log, "0000:04:00.0/driver_override"),
		      "boot display card untouched: %s", log ? log : "");
		free(log);
		free(out);

		/* The user's override (nvidia) and the user's driver (vfio-pci): never opened, never written. */
		unsel_act("unsel-l2-useroverride", gpu_other04, "nvidia", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK && sim_exists("bus/pci/devices/0000:04:00.0/driver"),
		      "user override: %s", out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "O /dev/nvidia4") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0") &&
		      !contains(log, "0000:04:00.0/driver_override"), "user override untouched: %s", log ? log : "");
		free(log);
		free(out);
		unsel_act("unsel-l2-uservfio", gpu_other04_vfio, NULL, root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "user driver: %s", out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "O /dev/nvidia4") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0") &&
		      !contains(log, "0000:04:00.0/driver_override"), "user driver untouched: %s", log ? log : "");
		free(log);
		free(out);

		/* A card in the device table: bound by nvidia and left alone, never opened. */
		unsel_act("unsel-l2-table", gpu_3070_04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "NVIDIA GPU not selected, 0000:04:00.0: 10de:2484 GeForce RTX 3070 driver nvidia; Bound by NVIDIA's driver and not activated."),
		      "table card: %s", out);
		log = sim_text("sim.log");
		CHECK(sim_exists("bus/pci/devices/0000:04:00.0/driver") && !contains(log, "O /dev/nvidia4") &&
		      !contains(log, "drivers/nvidia/unbind 0000:04:00.0") && !contains(log, "0000:04:00.0/driver_override"),
		      "table card untouched: %s", log ? log : "");
		free(log);
		free(out);

		/* unbindLock refused: left bound, one line, the lock written back to 0, no override. */
		unsel_act("unsel-l2-lock", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		sim.lock_denied = 1;
		/* The selected card's unbindLock is denied as well, so the activation stops there, after the step. */
		CHECK(run_host(activate_host, &out) != GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): NVIDIA's driver cannot start this GPU (open /dev/nvidia4: Input/output error), but it did not grant unbindLock, so a program holds the GPU; left on nvidia\n"),
		      "lock refused: %s", out);
		log = sim_text("sim.log");
		CHECK(sim_exists("bus/pci/devices/0000:04:00.0/driver") && sim_exists("dev/nvidia4") &&
		      contains(log, "gpus/0000:04:00.0/unbindLock 1") && contains(log, "gpus/0000:04:00.0/unbindLock 0") &&
		      !contains(log, "0000:04:00.0/driver_override") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0"),
		      "lock refused: nothing changed: %s", log ? log : "");
		free(log);
		free(out);

		/* The open does not finish: left bound, one line, nothing written. */
		unsel_act("unsel-l2-timeout", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -ETIMEDOUT;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): cannot test: open /dev/nvidia4 did not finish within 30 s; left on nvidia\n"),
		      "open timeout: %s", out);
		log = sim_text("sim.log");
		CHECK(sim_exists("bus/pci/devices/0000:04:00.0/driver") && !contains(log, "gpus/0000:04:00.0/unbindLock") &&
		      !contains(log, "0000:04:00.0/driver_override") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0"),
		      "open timeout: nothing written: %s", log ? log : "");
		free(log);
		free(out);

		/* Another error from the open: not the driver's refusal, so no release. */
		unsel_act("unsel-l2-eacces", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EACCES;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): cannot test: open /dev/nvidia4: Permission denied; left on nvidia\n") &&
		      sim_exists("bus/pci/devices/0000:04:00.0/driver"), "other open error: %s", out);
		free(out);

		/* No nvidia-modprobe: the node cannot be made, so the card cannot be tested. */
		unsel_act("unsel-l2-nomodprobe", gpu_other04, "", root, sizeof(root));
		sim.gpu_info = 1;
		sim.no_nvidia_modprobe = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): cannot test: nvidia-modprobe -c 4 did not make /dev/nvidia4; left on nvidia\n") &&
		      sim_exists("bus/pci/devices/0000:04:00.0/driver"), "no nvidia-modprobe: %s", out);
		free(out);

		/* VFs on the host: no card is tested, because an unbind puts their power management back to auto. */
		unsel_act("unsel-l2-vfs", gpu_other04, "", root, sizeof(root));
		make_card(root, &topo_05_l2, gpu_other04_vf, 0, NULL);
		gs_pathf(node, sizeof(node), "%s/devices/%s/0000:05:00.2/physfn", root, topo_05_l2.bridge_path);
		writef(node, "\n");
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		/* Activation itself refuses before any test: another GPU has a VF. */
		CHECK(run_host(activate_host, &out) != GS_EXIT_OK &&
		      contains(out, "VF 0000:05:00.2 of physfn: that GPU is not selected"), "VFs exist: %s", out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "O /dev/nvidia4") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0"), "VFs exist: nothing opened: %s", log ? log : "");
		free(log);
		free(out);

		/* nvidia_vgpu_vfio loaded: a late unbind is not asked for, so no card is tested. */
		unsel_act("unsel-l2-vgpuvfio", gpu_other04, "", root, sizeof(root));
		sim_writef("module/nvidia_vgpu_vfio/refcnt", "0\n");
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): not tested: nvidia_vgpu_vfio is loaded; left on nvidia\n") &&
		      sim_exists("bus/pci/devices/0000:04:00.0/driver"), "vgpu module loaded: %s", out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "O /dev/nvidia4") && !contains(log, "drivers/nvidia/unbind 0000:04:00.0"), "vgpu module loaded: nothing opened: %s", log ? log : "");
		free(log);
		free(out);

		/* A 3D-class card has no boot_vga file: it is not the boot display, so it is tested and released. */
		unsel_act("unsel-l2-3d", gpu_3d04, "", root, sizeof(root));
		sim_remove("bus/pci/devices/0000:04:00.0/boot_vga");
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): NVIDIA's driver cannot start this GPU (open /dev/nvidia4: Input/output error). Released:") &&
		      !sim_exists("bus/pci/devices/0000:04:00.0/driver"), "3D-class card without boot_vga: %s", out);
		free(out);

		/* An unbindLock write that does not finish: the step tests no further card, with one line for it. */
		unsel_act("unsel-l2-stuck", gpu_other04, "", root, sizeof(root));
		make_card(root, &topo_05_l2, gpu_other05, 0, NULL);
		set_override(root, &topo_05_l2, "0000:05:00.0", "");
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia4";
		sim.open_err = -EIO;
		sim.write_timeout = "/unbindLock";
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:04:00.0 (10de:2d04): NVIDIA's driver cannot start this GPU (open /dev/nvidia4: Input/output error), but asking it for unbindLock did not finish within 10 s; left as it is\n") &&
		      contains(out, "0000:05:00.0 (10de:2d04): not tested: a write to NVIDIA's driver for an earlier card did not finish; left on nvidia\n"),
		      "a stuck write stops the step: %s", out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "O /dev/nvidia5"), "no open of the second card: %s", log ? log : "");
		free(log);
		free(out);
	}

	/* A card bound to vfio-pci stays there. */
	unsel_host("unsel-vfio", gpu_other04_vfio, NULL, root, sizeof(root));
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK, "vfio-pci bound: %s", out);
	{
		char target[GS_PATH_MAX];
		ssize_t n;

		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:04:00.0/driver", root);
		n = readlink(p, target, sizeof(target) - 1);
		CHECK(n > 0 && (target[n] = '\0', ends_with(target, "/vfio-pci")), "the card stays on vfio-pci");
	}
	free(out);

	/* nvidia already loaded (an early unit of an older install, or modules-load): not loaded again, no wait. */
	act_host("unsel-loaded", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0\nvfs=4\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && contains(out, "nvidia is already loaded; not loading it again"),
	      "loaded: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "R modprobe nvidia") && !contains(log, "udevadm"), "no second load: %s", log ? log : "");
	free(log);
	free(out);

	/* A selected card that vfio-pci took (same ID as a passthrough card): activation's refusal names the driver. */
	act_host("unsel-samepair", &topo_default, gpu_ampere_vfio, 0, NULL, root, sizeof(root));
	CHECK(run_host(activate_host, &out) != GS_EXIT_OK &&
	      contains(out, "0000:03:00.0: still bound to vfio-pci after ") &&
	      contains(out, "0000:03:00.0 is bound to vfio-pci, not nvidia"), "selected card on vfio-pci: %s", out);
	free(out);

	/* Console order: the pre-step loads nvidia first; phase C then unloads it, hands the framebuffer over, loads it again. */
	act_host("unsel-console", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim_remove("module/nvidia_drm");
	sim_remove("module/nvidia_modeset");
	sim_remove("module/nvidia");
	sim_writef("livepatch.conf", "bdfs=0000:04:00.0\nvfs=4\n");
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	lp_opts.load_driver = 1;
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && contains(out, "nvidia loaded after the livepatch"),
	      "pre-step on the display host: %s", out);
	free(out);
	sim_core_unused();
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "firmware framebuffer simple-framebuffer.0 released"),
	      "activation after the pre-step: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "R modprobe nvidia\n") >= 0 && pos_of(log, "R rmmod nvidia\n") > pos_of(log, "R modprobe nvidia\n") &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") > pos_of(log, "R rmmod nvidia\n") &&
	      pos_of(log, "nvidia_drm modeset=1") > pos_of(log, "simple-framebuffer.0/driver/unbind"),
	      "pre-step load, unload, handoff, display load: %s", log);
	{
		const char *l = log, *second = NULL, *first = strstr(l, "R modprobe nvidia\n");

		if (first)
			second = strstr(first + 1, "R modprobe nvidia\n");
		CHECK(second && second - log > pos_of(log, "simple-framebuffer.0/driver/unbind") &&
		      second - log < pos_of(log, "nvidia_drm modeset=1"), "the second nvidia load is after the handoff, before nvidia_drm: %s", log);
	}
	free(log);
	free(out);

	/*
	 * With the console handoff: nvidia is unloaded for it and loaded again, and the unselected card that the reload
	 * bound is tested after that load, before nvidia_vgpu_vfio, with no unload of nvidia after it.
	 */
	{
		static const struct fx_fn gpu_other05h[] = {
			{ "0000:05:00.0", "2d04", "030000", NULL },
			{ NULL, NULL, NULL, NULL }
		};
		static const struct fx_topo topo_05h = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };
		long ld, rm, op, vf;

		act_host("unsel-l2-console", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
		make_card(root, &topo_05h, gpu_other05h, 0, NULL);
		set_override(root, &topo_05h, "0000:05:00.0", "");
		sim_core_unused();
		sim.gpu_info = 1;
		sim.open_fail = "/dev/nvidia5";
		sim.open_err = -EIO;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
		      contains(out, "0000:05:00.0 (10de:2d04): NVIDIA's driver cannot start this GPU (open /dev/nvidia5: Input/output error). Released:"),
		      "handoff host: the refused card is released: %s", out);
		log = sim_text("sim.log");
		rm = pos_of(log, "R rmmod nvidia\n");
		ld = pos_of(log, "R modprobe nvidia\n");
		op = pos_of(log, "O /dev/nvidia5\n");
		vf = pos_of(log, "R modprobe nvidia_vgpu_vfio\n");
		CHECK(rm >= 0 && ld > rm && op > ld && vf > op && count_of(log, "O /dev/nvidia5\n") == 1 &&
		      pos_of(log, "R rmmod nvidia\n") == pos_of_last(log, "R rmmod nvidia\n"),
		      "rmmod nvidia < modprobe nvidia < the test open < modprobe nvidia_vgpu_vfio, no rmmod after it: %s", log);
		free(log);
		free(out);
	}

	/* status: each meaning, and the boot display mark. */
	fixture("unsel-status", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	rc_without = unsel_status(root, &out);
	CHECK(!contains(out, "NVIDIA GPUs that are not selected"), "no unselected card, no section: %s", out);
	free(out);
	make_card(root, &topo_display, gpu_other04, 1, NULL);
	make_card(root, &topo_05, gpu_3070_05, 0, NULL);
	rc = unsel_status(root, &out);
	CHECK(rc == rc_without, "the section does not change the exit status (%d, %d)", rc, rc_without);
	CHECK(contains(out, "\nNVIDIA GPUs that are not selected:\n0000:04:00.0: 10de:2d04 (not in device table) driver none, boot display\n"
			    "  No driver is bound. geforce-sriov does not touch it.\n") &&
	      contains(out, "0000:05:00.0: 10de:2484 GeForce RTX 3070 driver nvidia\n"
			    "  Bound by NVIDIA's driver and not activated. `sudo geforce-sriov install --device 0000:05:00.0` selects it.\n") &&
	      pos_of(out, "NVIDIA GPUs that are not selected") > pos_of(out, "0000:03:00.0: 10de:2206"),
	      "none and nvidia: %s", out);
	free(out);
	set_override(root, &topo_display, "0000:04:00.0", "vfio-pci");
	unsel_status(root, &out);
	CHECK(contains(out, "0000:04:00.0: 10de:2d04 (not in device table) driver none, driver_override=vfio-pci, boot display\n"
			    "  No driver is bound, and driver_override=vfio-pci lets only a driver of that name bind it. geforce-sriov does not touch it.\n"),
	      "override and no driver: %s", out);
	free(out);
	fixture("unsel-status2", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_other04_vfio, 0, NULL);
	set_override(root, &topo_display, "0000:04:00.0", "vfio-pci");
	unsel_status(root, &out);
	CHECK(contains(out, "0000:04:00.0: 10de:2d04 (not in device table) driver vfio-pci, driver_override=vfio-pci\n"
			    "  Kept by vfio-pci (your configuration). geforce-sriov does not touch it.\n"), "vfio-pci: %s", out);
	free(out);
	/* A released card: status says who released it and how to undo it. */
	fixture("unsel-status-released", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_other04, 0, NULL);
	set_override(root, &topo_display, "0000:04:00.0", GS_UNSELECTED_RELEASED);
	unsel_status(root, &out);
	CHECK(contains(out, "0000:04:00.0: 10de:2d04 (not in device table) driver none, driver_override=geforce-sriov-released\n"
			    "  NVIDIA's driver could not start this GPU, so geforce-sriov released it. No driver binds it.\n"
			    "  To ask NVIDIA's driver to bind it again until the next boot, run: `echo > /sys/bus/pci/devices/0000:04:00.0/driver_override && echo 0000:04:00.0 > /sys/bus/pci/drivers_probe`\n"
			    "  A card that driver still cannot start stays without a driver. With VFs on the host, run `geforce-sriov check` afterwards. If it says NOT READY, stop the VMs and run `geforce-sriov activate`.\n"
			    "  To keep it on NVIDIA's driver at every boot, write `nvidia` to its driver_override with a udev rule.\n"),
	      "released card in status: %s", out);
	free(out);
	/* sysfs prints (null) for an unset driver_override: that is no override, for nvidia's bind and for status. */
	unsel_host("unsel-null", gpu_other04, "(null)", root, sizeof(root));
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && sim_exists("bus/pci/devices/0000:04:00.0/driver"),
	      "(null) is bound by nvidia: %s", out);
	free(out);
	unsel_status(root, &out);
	CHECK(contains(out, "0000:04:00.0: 10de:2d04 (not in device table) driver nvidia\n"
			    "  Bound by NVIDIA's driver and not activated.\n") && !contains(out, "driver_override=") &&
	      !contains(out, "0000:04:00.1"), "(null) is no override, and the audio function is not listed: %s", out);
	free(out);
	/* The audio function of an unselected card never appears. */
	unsel_host("unsel-audio", gpu_other04, NULL, root, sizeof(root));
	unsel_status(root, &out);
	CHECK(contains(out, "0000:04:00.0: 10de:2d04") && !contains(out, "0000:04:00.1"), "audio function not listed: %s", out);
	free(out);
	fixture("unsel-status3", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_other04_nvidia, 0, NULL);
	unsel_status(root, &out);
	CHECK(contains(out, "0000:04:00.0: 10de:2d04 (not in device table) driver nvidia\n"
			    "  Bound by NVIDIA's driver and not activated.\n"), "nvidia, not in the table: %s", out);
	free(out);

	/* check lists selected cards only: identical with and without an unselected card. */
	{
		char *without;
		int rc_check;

		ready_host("unsel-check", "525", root, sizeof(root));
		rc_check = run_host(check_host, &without);
		make_card(root, &topo_display, gpu_other04, 0, NULL);
		rc = run_host(check_host, &out);
		CHECK(rc == rc_check && strcmp(out, without) == 0, "check unchanged:\n%s\n--\n%s", without, out);
		free(without);
		free(out);
	}
}

/* A datagram socket standing in for systemd's $NOTIFY_SOCKET: a path under tmp, or "@name" (abstract). */
static int notify_bind(const char *name)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	size_t n = strlen(name);
	socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	if (fd < 0 || n >= sizeof(sa.sun_path))
		die("notify socket");
	memcpy(sa.sun_path, name, n);
	if (name[0] == '@') {
		sa.sun_path[0] = '\0';
		len--;
	}
	if (bind(fd, (struct sockaddr *)&sa, len))
		die("bind notify socket");
	return fd;
}

/* Every datagram waiting, each followed by ';'. */
static void notify_drain(int fd, char *out, size_t len)
{
	char m[256];
	ssize_t n;

	out[0] = '\0';
	while ((n = recv(fd, m, sizeof(m) - 1, 0)) > 0) {
		m[n] = '\0';
		snprintf(out + strlen(out), len - strlen(out), "%s;", m);
	}
}

static void test_notify(void)
{
	char path[GS_PATH_MAX], got[16384], root[GS_PATH_MAX], *out, *unit;
	int fd;

	/* The wire message: the step's own limit plus the margin, in microseconds. */
	unsetenv("NOTIFY_SOCKET");
	gs_pathf(path, sizeof(path), "%s/notify.sock", tmp);
	unlink(path);
	fd = notify_bind(path);
	gs_notify_extend(5000);
	notify_drain(fd, got, sizeof(got));
	CHECK(got[0] == '\0', "without $NOTIFY_SOCKET nothing is sent: %s", got);
	setenv("NOTIFY_SOCKET", path, 1);
	gs_notify_extend(5000);
	gs_notify_extend(60000);
	gs_notify_extend(0);
	notify_drain(fd, got, sizeof(got));
	CHECK(strcmp(got, "EXTEND_TIMEOUT_USEC=35000000;EXTEND_TIMEOUT_USEC=90000000;EXTEND_TIMEOUT_USEC=30000000;") == 0,
	      "path socket: %s", got);
	/* A name that is neither a path nor an abstract name is ignored, as sd_notify does. */
	setenv("NOTIFY_SOCKET", "notify.sock", 1);
	gs_notify_extend(5000);
	setenv("NOTIFY_SOCKET", "", 1);
	gs_notify_extend(5000);
	notify_drain(fd, got, sizeof(got));
	CHECK(got[0] == '\0', "a relative or empty name sends nothing: %s", got);
	close(fd);
	setenv("NOTIFY_SOCKET", path, 1);
	gs_notify_extend(5000); /* nobody listens: no error, no hang */
	unlink(path);

	snprintf(got, sizeof(got), "@gs-notify-test-%d", (int)getpid());
	fd = notify_bind(got);
	setenv("NOTIFY_SOCKET", got, 1);
	gs_notify_extend(10000);
	notify_drain(fd, got, sizeof(got));
	CHECK(strcmp(got, "EXTEND_TIMEOUT_USEC=40000000;") == 0, "abstract socket: %s", got);
	close(fd);

	/* An activation sends one message per waiting step, each well formed and none shorter than the margin. */
	gs_pathf(path, sizeof(path), "%s/notify-act.sock", tmp);
	unlink(path);
	fd = notify_bind(path);
	setenv("NOTIFY_SOCKET", path, 1);
	act_host("act-notify", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	set_bridge_windows(root, &topo_default, 18, 128);
	set_root_windows(root, 64, 8192);
	sim.grow_windows = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "activate: %s", out);
	free(out);
	unsetenv("NOTIFY_SOCKET");
	notify_drain(fd, got, sizeof(got));
	close(fd);
	unlink(path);
	{
		unsigned int n = 0;
		const char *p = got;

		for (; *p; n++) {
			unsigned long long usec;
			int used = 0;

			CHECK(sscanf(p, "EXTEND_TIMEOUT_USEC=%llu;%n", &usec, &used) == 1 && used > 0 &&
			      usec >= GS_NOTIFY_MARGIN_MS * 1000ull, "a well-formed extension at: %.60s", p);
			if (used <= 0)
				break;
			p += used;
		}
		CHECK(n >= 10 && contains(got, "EXTEND_TIMEOUT_USEC=40000000;"),
		      "the activation extended the timeout before its waiting steps (%u messages): %.200s", n, got);
	}

	/* The unit: a modest base timeout and the permission to extend it from the main process. */
	gs_pathf(path, sizeof(path), "%s/systemd/geforce-sriov-activate.service.in", srcdir);
	unit = slurp(path);
	CHECK(unit && count_of(unit, "\nType=oneshot\n") == 1 && count_of(unit, "\nNotifyAccess=exec\n") == 1 &&
	      count_of(unit, "\nTimeoutStartSec=@START_TIMEOUT@\n") == 1 && count_of(unit, "TimeoutStartSec=") == 1 &&
	      !contains(unit, "TimeoutSec="), "the activation unit extends its own timeout: %s", unit ? unit : "(missing)");
	free(unit);
}

/* GS_MAX_PFS cards are accepted by the config, one more is not. */
static void test_many_cards(void)
{
	char text[GS_BDF_LIST_STR_SIZE + 64], err[256];
	struct gs_config c;
	size_t len = 0;

	len += (size_t)snprintf(text + len, sizeof(text) - len, "bdfs=");
	for (unsigned int i = 0; i < GS_MAX_PFS; i++)
		len += (size_t)snprintf(text + len, sizeof(text) - len, "%s0000:%02x:00.0", i ? "," : "", i + 1);
	len += (size_t)snprintf(text + len, sizeof(text) - len, "\n");
	CHECK(gs_config_parse(text, len, &c, err, sizeof(err)) == 0 && c.nbdfs == GS_MAX_PFS &&
	      strcmp(c.bdfs[GS_MAX_PFS - 1].str, "0000:20:00.0") == 0, "%u cards: %s", GS_MAX_PFS, err);
	len--;
	len += (size_t)snprintf(text + len, sizeof(text) - len, ",0000:%02x:00.0\n", GS_MAX_PFS + 1);
	CHECK(gs_config_parse(text, len, &c, err, sizeof(err)) != 0 && contains(err, "more than 32 devices"),
	      "one card more: %s", err);
	/* Every buffer that holds the card list or one entry per card is sized from GS_MAX_PFS. */
	{
		struct gs_module_state m;
		struct gs_dev_nodes dn;

		CHECK(sizeof(m.bdfs) >= GS_BDF_LIST_STR_SIZE, "the loaded module's bdfs fits the longest list");
		CHECK(sizeof("bdfs=") - 1 + GS_BDF_LIST_STR_SIZE > GS_BDF_LIST_STR_SIZE, "modprobe option has room for its prefix");
		CHECK(GS_DEV_NODES_MAX >= GS_MAX_PFS + 8, "device nodes: every GPU plus the seven others");
		CHECK(sizeof(dn.core) >= 15 + 7 * GS_DEV_NODES_MAX, "the nvidia-modprobe -c list fits %u nodes", GS_DEV_NODES_MAX);
	}
}

/*
 * The selection texts at the most cards the module takes: the command that names all of them, in the
 * verdict, in the display forms and in the line status prints, is whole (the 32nd address is
 * in it and the text ends as it should), not cut at a fixed buffer.
 */
static void test_selection_many(void)
{
	char root[GS_PATH_MAX], bdfs[GS_BDF_LIST_STR_SIZE + 16] = "bdfs=", last[64], cmd[GS_SEL_CMD_MAX];
	char tail[96], msg[GS_SEL_MSG_MAX], line[GS_SEL_LINE_MAX], forms[GS_SEL_FORMS_MAX], args[GS_SEL_ARGS_MAX];
	struct gs_config c;
	struct gs_host h;

	for (unsigned int i = 0; i < GS_MAX_PFS; i++)
		snprintf(bdfs + strlen(bdfs), sizeof(bdfs) - strlen(bdfs), "%s0000:%02x:00.0", i ? "," : "", i + 1);
	strcat(bdfs, "\n");
	snprintf(last, sizeof(last), "--device 0000:%02x:00.0", GS_MAX_PFS);
	snprintf(tail, sizeof(tail), "0000:%02x:00.0` to record them", GS_MAX_PFS);
	act_host("sel-many", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	config_of(&c, bdfs);
	CHECK(c.nbdfs == GS_MAX_PFS, "%u cards in the config", c.nbdfs);
	h = sim_host();
	gs_selection_command(c.bdfs, c.nbdfs, 1, NULL, cmd, sizeof(cmd));
	CHECK(strlen(cmd) < sizeof(cmd) - 1 && strlen(cmd) < GS_SEL_ARGS_MAX && contains(cmd, last) &&
	      contains(cmd, " --disable-d3cold") && !strstr(cmd, "--disable-d3cold --disable-d3cold"),
	      "the command names all %u cards and the option: %s", GS_MAX_PFS, cmd);
	/* The record cannot be read: the verdict names the whole command. */
	sim_writef("state/" GS_SELECTION_NAME, "not a record\n");
	CHECK(gs_selection_verdict(&h, &c, NULL, msg, sizeof(msg)) == 1 && contains(msg, last) &&
	      contains(msg, "--device 0000:01:00.0") && strstr(msg, last)[strlen(last)] == '\n',
	      "the verdict has the whole command with the %uth address: %s", GS_MAX_PFS, msg);
	/* No record of some card: the line says the whole command and ends with its sentence. */
	sim_remove("state/" GS_SELECTION_NAME);
	CHECK(gs_selection_unrecorded(sim.state, &c, line, sizeof(line)) == 1 && contains(line, last) &&
	      contains(line, tail), "the unrecorded line has the whole command: %s", line);
	/* The two display forms each repeat the options. */
	gs_selection_command(c.bdfs, c.nbdfs, 0, NULL, args, sizeof(args));
	CHECK(gs_display_forms(&h, args + strlen("sudo geforce-sriov install "), forms, sizeof(forms)) == 1 &&
	      count_of(forms, last) == 2 && contains(forms, "--no-display\n") && contains(forms, "--nvkms-source PATH\n") &&
	      contains(forms, "sudo ./geforce-sriov install --device"),
	      "both forms carry all cards: %s", forms);
}

/*
 * The start timeout is an upper bound built from activation's own limits (include/gs_lifecycle.h).
 * This fails when a limit or the number of waiting calls in src/activate.c changes without the sum
 * following: update the limits and counts in gs_lifecycle.h, then the expected numbers here.
 */
static void test_start_timeout(void)
{
	static const struct { const char *name; unsigned int value, expect; } limits[] = {
		{ "POLL_MS", POLL_MS, 50 }, { "LIVEPATCH_WAIT_MS", LIVEPATCH_WAIT_MS, 10000 },
		{ "VF_BIND_WAIT_MS", VF_BIND_WAIT_MS, 30000 }, { "MGR_ACTIVE_WAIT_MS", MGR_ACTIVE_WAIT_MS, 30000 },
		{ "MGR_SETTLE_MS", MGR_SETTLE_MS, 2000 }, { "PF_REBIND_WAIT_MS", PF_REBIND_WAIT_MS, 10000 },
		{ "VF_ACTIVE_WAIT_MS", VF_ACTIVE_WAIT_MS, 5000 }, { "VF_TYPES_WAIT_MS", VF_TYPES_WAIT_MS, 15000 },
		{ "REMOVE_WAIT_MS", REMOVE_WAIT_MS, 5000 }, { "RETURN_WAIT_MS", RETURN_WAIT_MS, 15000 },
		{ "SRIOV_ATTR_WAIT_MS", SRIOV_ATTR_WAIT_MS, 5000 }, { "FN_RETURN_WAIT_MS", FN_RETURN_WAIT_MS, 5000 },
		{ "BRIDGE_WAKE_MS", BRIDGE_WAKE_MS, 5000 }, { "PF_WAKE_MS", PF_WAKE_MS, 5000 },
		{ "DM_ACTIVE_WAIT_MS", DM_ACTIVE_WAIT_MS, 10000 }, { "SYSFS_DEADLINE_MS", SYSFS_DEADLINE_MS, 10000 },
		{ "RELEASE_WAIT_MS", RELEASE_WAIT_MS, 5000 }, { "GS_BOOT_BIND_WAIT_MS", GS_BOOT_BIND_WAIT_MS, 30000 },
		{ "GS_MODULE_DEADLINE_MS", GS_MODULE_DEADLINE_MS, 60000 }, { "GS_CAPTURE_TIMEOUT_MS", GS_CAPTURE_TIMEOUT_MS, 5000 },
		{ "GS_SLOT_FNS", GS_SLOT_FNS, 8 }, { "GS_ACT_CARD_WRITES", GS_ACT_CARD_WRITES, 43 },
		{ "GS_ACT_MODULE_RUNS", GS_ACT_MODULE_RUNS, 29 }, { "GS_ACTIVATE_CARD_MS", GS_ACTIVATE_CARD_MS, 690000 },
		{ "GS_ACTIVATE_VF_MS", GS_ACTIVATE_VF_MS, 55000 }, { "GS_ACTIVATE_PRESTEP_MS", GS_ACTIVATE_PRESTEP_MS, 460000 },
		{ "GS_UDEV_SETTLE_MS", GS_UDEV_SETTLE_MS, 35000 }, { "GS_ACTIVATE_BASE_MS", GS_ACTIVATE_BASE_MS, 2262000 },
		{ "GS_UNSELECTED_OPEN_MS", GS_UNSELECTED_OPEN_MS, 30000 }, { "GS_UNSELECTED_BUDGET_MS", GS_UNSELECTED_BUDGET_MS, 120000 },
		{ "GS_UNSELECTED_CARD_MS", GS_UNSELECTED_CARD_MS, 140000 },
	};
	/* How often src/activate.c calls what the sum counts: a new waiting call needs a new term. */
	static const struct { const char *token; int expect; } sites[] = {
		{ "sysfs_write(h,", 23 }, { "wait_exists(", 8 }, { "wait_attr_eq(", 3 }, { "gs_host_run(", 9 },
		{ "gs_module_load(", 6 }, { "gs_module_unload(", 6 }, { "daemon_start(", 2 }, { "daemon_restart(", 3 },
		{ "ensure_livepatch(", 3 }, { "gs_sleep_ms(", 13 }, { "h->capture(", 1 },
	};
	static const struct { unsigned int cards, vfs, seconds; } rows[] = {
		{ 0, 4, 2489 }, { 1, 4, 3490 }, { 8, 4, 10497 }, { 32, 4, 34521 }, { 1, 16, 4216 },
	};
	char path[GS_PATH_MAX], *src;

	for (unsigned int i = 0; i < sizeof(limits) / sizeof(limits[0]); i++)
		CHECK(limits[i].value == limits[i].expect, "%s is %u; the start timeout was built for %u", limits[i].name,
		      limits[i].value, limits[i].expect);
	gs_pathf(path, sizeof(path), "%s/src/activate.c", srcdir);
	src = slurp(path);
	CHECK(src != NULL, "src/activate.c is readable");
	for (unsigned int i = 0; src && i < sizeof(sites) / sizeof(sites[0]); i++)
		CHECK(count_of(src, sites[i].token) == sites[i].expect,
		      "activate.c has %d uses of %s; the start timeout was built for %d", count_of(src, sites[i].token),
		      sites[i].token, sites[i].expect);
	free(src);
	for (unsigned int r = 0; r < sizeof(rows) / sizeof(rows[0]); r++)
		CHECK(gs_activate_start_timeout_s(rows[r].cards, rows[r].vfs) == rows[r].seconds,
		      "%u cards with %u VFs: %u s, got %u", rows[r].cards, rows[r].vfs, rows[r].seconds,
		      gs_activate_start_timeout_s(rows[r].cards, rows[r].vfs));
	/* install writes it for 1, 8 and 32 cards (the default VF count). */
	for (unsigned int r = 1; r < sizeof(rows) / sizeof(rows[0]); r++) {
		unsigned int n = rows[r].cards;
		char stage[GS_PATH_MAX], p[GS_PATH_MAX], addr[GS_MAX_PFS][24], want[64], *out, *unit;
		const char *args[8 + 2 * GS_MAX_PFS + 1];
		unsigned int k = 0;

		if (rows[r].vfs != GS_DEFAULT_VFS)
			continue;
		snprintf(stage, sizeof(stage), "%s/stage-timeout-%u", tmp, n);
		args[k++] = "install";
		args[k++] = "--destdir";
		args[k++] = stage;
		args[k++] = "--source";
		args[k++] = srcdir;
		args[k++] = "--prefix";
		args[k++] = "/usr";
		for (unsigned int i = 0; i < n; i++) {
			snprintf(addr[i], sizeof(addr[i]), "0000:%02x:00.0", i + 1);
			args[k++] = "--device";
			args[k++] = addr[i];
		}
		args[k] = NULL;
		CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK, "staged install of %u cards: %s", n, out);
		free(out);
		gs_pathf(p, sizeof(p), "%s/etc/systemd/system/geforce-sriov-activate.service", stage);
		unit = slurp(p);
		snprintf(want, sizeof(want), "\nTimeoutStartSec=%u\n", rows[r].seconds);
		CHECK(unit && count_of(unit, want) == 1 && !contains(unit, "@START_TIMEOUT@"),
		      "%u cards write %s: %s", n, want, unit ? unit : "(missing)");
		free(unit);
	}
}

/*
 * The polling loops end on elapsed time: a `systemctl is-active` that takes 1 s per call and never
 * says active ends the manager wait after MGR_ACTIVE_WAIT_MS (30 s) plus one call, not after 600
 * iterations of 1 s.
 */
static void test_poll_elapsed(void)
{
	char root[GS_PATH_MAX], *out;
	long t0, took;

	act_host("act-poll-elapsed", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.mgr_stays_down = 1;
	sim.is_active_delay_ms = 1000;
	t0 = now_ms();
	CHECK(run_host(activate_host, &out) != GS_EXIT_OK, "the manager never becomes active: %s", out);
	took = now_ms() - t0;
	CHECK(contains(out, "nvidia-vgpu-mgr.service is not active 30000 ms after its restart"),
	      "the wait names its limit: %s", out);
	CHECK(took >= 30000 && took < 45000, "the wait ended on elapsed time, after %ld ms", took);
	free(out);
}

static void test_activate_rescan(void)
{
	char root[GS_PATH_MAX], *out, *log;

	act_host("act-rescan", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	/* A window too small now, but inside the room above the bridge: the rescan grows it. */
	set_bridge_windows(root, &topo_default, 18, 128);
	set_root_windows(root, 64, 8192);
	sim.grow_windows = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "activate: %s", out);
	log = sim_text("sim.log");
	check_rescan_out(out);
	CHECK(contains(out, "VF resources source=the bridge grows on rescan; the room above it suffices count=4 "
			   "nonprefetch need=1048576 window=18874368 prefetch need=1207959552 window=134217728"),
	      "resource line: %s", out);
	/* Every mandatory order. */
	CHECK(pos_of(log, "W bus/pci/devices/0000:03:00.0/power/control on") > 0 &&
	      pos_of(log, "W devices/pci0000:00/0000:00:02.0/power/control on") <
		      pos_of(log, "W bus/pci/devices/0000:03:00.0/power/control on"),
	      "the bridge is held awake before the PF: %s", log);
	CHECK(pos_of(log, "bus/pci/devices/0000:03:00.1/driver/unbind 0000:03:00.1") > 0 &&
	      pos_of(log, "bus/pci/devices/0000:03:00.1/driver/unbind 0000:03:00.1") <
		      pos_of(log, "bus/pci/devices/0000:03:00.1/remove 1") &&
	      pos_of(log, "bus/pci/devices/0000:03:00.1/remove 1") <
		      pos_of(log, "bus/pci/devices/0000:03:00.0/remove 1"),
	      "audio unbound, removed, then the PF: %s", log);
	CHECK(count_of(log, "/rescan 1") == 1 && pos_of(log, "/rescan 1") > pos_of(log, "0000:03:00.0/remove 1"),
	      "exactly one rescan, after the empty bus: %s", log);
	CHECK(!contains(log, "sriov_numvfs"), "never writes sriov_numvfs: %s", log);
	CHECK(!contains(log, "-f") && !contains(log, "rmmod -f"), "never forces an unload: %s", log);
	CHECK(pos_of(log, "R /usr/lib/nvidia/sriov-manage -e 0000:03:00.0") > 0 &&
	      count_of(log, "sriov-manage -e 0000:03:00.0") == 1, "sriov-manage once per card: %s", log);
	CHECK(pos_of(log, "R systemctl restart nvidia-vgpud.service") > 0 &&
	      pos_of(log, "R systemctl restart nvidia-vgpud.service") <
		      pos_of(log, "R systemctl restart nvidia-vgpu-mgr.service"),
	      "vgpud then the manager: %s", log);
	CHECK(pos_of(log, "R systemctl restart nvidia-vgpu-mgr.service") <
		      pos_of(log, "W bus/pci/drivers/nvidia/unbind 0000:03:00.0"),
	      "the PF is re-probed after the daemons: %s", log);
	CHECK(pos_of(log, "W bus/pci/drivers/nvidia/unbind 0000:03:00.0") <
		      pos_of(log, "W bus/pci/drivers/nvidia/bind 0000:03:00.0"),
	      "unbind then bind: %s", log);
	CHECK(contains(out, "0000:03:00.0: re-probed, bound to nvidia again"), "re-probe reported: %s", out);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_ACTIVATING_NAME),
	      "guard and activating marker cleared");
	free(log);
	free(out);
}

/* SR-IOV already presented with its VF BARs assigned: enable only, no removal or rescan. */
static void test_activate_enable(void)
{
	char root[GS_PATH_MAX], *out, *log;

	act_host("act-enable", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_writef("bus/pci/devices/0000:03:00.0/power/control", "on\n");
	sim_writef("bus/pci/devices/0000:03:00.0/power/runtime_status", "active\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "enable: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "/remove") && !contains(log, "/rescan"), "no removal and no rescan: %s", log);
	CHECK(count_of(log, "sriov-manage -e 0000:03:00.0") == 1 && contains(out, "4 VFs enabled and bound to nvidia"),
	      "VFs enabled once: %s", out);
	CHECK(!contains(out, "0000:03:00.0: was runtime_status="), "no wake when the PF is already active: %s", out);
	free(log);
	free(out);
}

/* The one-shot vgpud may end inactive; the manager must become active. */
static void test_activate_oneshot(void)
{
	char root[GS_PATH_MAX], *out, *log;

	act_host("act-oneshot", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.vgpud_active = 0; /* is-active says inactive, as a finished oneshot does */
	{
		int rc = run_host(activate_host, &out);

		CHECK(rc == GS_EXIT_OK && contains(out, "activation finished"),
		      "oneshot vgpud counts as success: rc=%d %s", rc, out);
	}
	log = sim_text("sim.log");
	CHECK(contains(log, "R systemctl restart nvidia-vgpud.service") &&
	      !contains(log, "R systemctl is-active --quiet nvidia-vgpud"), "vgpud restarted, never polled: %s", log);
	CHECK(contains(log, "R systemctl restart nvidia-vgpu-mgr.service") &&
	      contains(log, "R systemctl is-active --quiet nvidia-vgpu-mgr.service"),
	      "the manager is restarted and then polled: %s", log);
	free(log);
	free(out);
}

/* MSI-X hidden after the rescan: stop before any VF is enabled, no retry, guard retained. */
static void test_activate_msix(void)
{
	char root[GS_PATH_MAX], *out, *log;

	act_host("act-msix", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.msix_hidden_after_rescan = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL, "msix hidden: %d\n%s", 0, out);
	log = sim_text("sim.log");
	CHECK(contains(out, "came back with MSI-X hidden after the rescan; no VF is enabled and the rescan is not retried"),
	      "msix message: %s", out);
	CHECK(!contains(log, "sriov-manage") && count_of(log, "/rescan 1") == 1 &&
	      !contains(log, "modprobe geforce_sriov"), "no VF enabled, one rescan, no livepatch reload: %s", log);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME) && sim_exists("run/" GS_ACTIVATING_NAME),
	      "guard and activating marker retained");
	CHECK(contains(out, "activation stopped after it had started to change the GPUs or NVIDIA's modules") &&
	      contains(out, "The activation lock stays set.") && contains(out, "Recovery: do not rescan again"), "stop message: %s", out);
	free(log);
	free(out);

	/* SR-IOV not re-presented after the rescan. */
	act_host("act-nosriov", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.no_sriov_after_rescan = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "came back without sriov_totalvfs, so SR-IOV is not presented"), "no SR-IOV: %s", out);
	free(out);

	/* VF BAR rows unassigned after the rescan. */
	act_host("act-nobars", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.zero_vf_bars_after_rescan = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "no VF BAR was assigned after the rescan, so enabling VFs would fail") &&
	      contains(out, "Above 4G decoding"), "no VF BARs: %s", out);
	free(out);
}

/* The empty-bus requirement and bounded operations. */
static void test_activate_bounded(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* Another device appears on the bus between the preflight and the emptiness check: no rescan. */
	act_host("act-neighbour", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.neighbour_on_bus = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "is still on bus 0000:03"),
	      "non-empty bus: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "/rescan"), "the rescan is not issued: %s", log);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME), "guard retained");
	free(log);
	free(out);

	/* A remove that completes but leaves the device listed: no rescan, uncertain state. */
	act_host("act-keepremoved", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.keep_removed = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "is still listed 5000 ms after its remove; do not rescan"), "listed after remove: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "/rescan"), "no rescan after an incomplete remove: %s", log);
	free(log);
	free(out);

	/* A write whose read-back says something else: refused before anything is removed. */
	act_host("act-lie", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.write_lie = "power/control";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "power/control reads 'auto' after writing on; nothing was removed"), "readback: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "/remove") && !contains(log, "/rescan") && !sim_exists("state/" GS_ATTEMPT_NAME),
	      "nothing removed, guard cleared: %s", log);
	free(log);
	free(out);

	/* A write that times out: exit 5, one attempt, guard and activating marker retained. */
	act_host("act-wtimeout", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.write_timeout = "power/control";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "did not finish within 10000 ms; whether the bridge is awake is unknown"), "write timeout: %s",
	      out);
	log = sim_text("sim.log");
	CHECK(count_of(log, "power/control on") == 1 && !contains(log, "/remove"), "one attempt, no removal: %s", log);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME) && sim_exists("run/" GS_ACTIVATING_NAME),
	      "guard and activating marker retained after an uncertain write");
	/* Regression: the retained guard stops every later run before it writes anything. */
	free(log);
	free(out);
	{
		char lp[GS_PATH_MAX];

		gs_pathf(lp, sizeof(lp), "%s/sim.log", sim.root);
		unlink(lp);
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "activation lock") &&
	      contains(out, "nothing done"), "a second run with the retained guard: %s", out);
	CHECK(!sim_text("sim.log"), "the retained guard stops the second run before any write");
	free(out);

	/* Stale display modules on a plain host: unloaded drm then modeset, never with any VF present. */
	act_host("act-unload-order", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_module("nvidia_drm");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "stale modules: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "R rmmod nvidia_drm") > 0 &&
	      pos_of(log, "R rmmod nvidia_drm") < pos_of(log, "R rmmod nvidia_modeset") &&
	      pos_of(log, "R rmmod nvidia_modeset") < pos_of(log, "sriov-manage -e"),
	      "nvidia_drm then nvidia_modeset, before any VF is enabled: %s", log);
	free(log);
	free(out);

	/* An unload that times out: exit 5, one attempt, guard retained. */
	act_host("act-utimeout", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_module("nvidia_drm");
	sim.run_timeout = "rmmod nvidia_drm";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "rmmod nvidia_drm did not finish within 60000 ms; whether it unloaded is unknown") &&
	      contains(out, "An operation did not finish in time, so the state of the GPUs is uncertain. The activation lock stays set.") && contains(out, "Recovery: do not rescan again"), "unload timeout: %s", out);
	log = sim_text("sim.log");
	CHECK(count_of(log, "R rmmod nvidia_drm") == 1 && !contains(log, "R rmmod nvidia_modeset") &&
	      !contains(log, "sriov-manage"), "one unload attempt, no second module unload: %s", log);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME), "guard retained after an uncertain unload");
	free(log);
	free(out);
}

/* The global zero-VF rule for each unload, and the NVIDIA lock. */
static void test_activate_gates(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* Another NVIDIA PF has VFs: stale display modules must not be unloaded. */
	act_host("act-anyvf", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_module("nvidia_drm");
	{
		static const struct fx_fn spare[] = { { "0000:05:00.0", "2206", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };
		static const struct fx_topo t5 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };

		make_card(root, &t5, spare, 0, "4/4");
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "has sriov_numvfs=4; NVIDIA's display modules are unloaded only while no affected GPU has VFs") &&
	      contains(out, "check reports NOT READY"), "any VFs: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "rmmod nvidia_drm"), "no display unload: %s", log);
	free(log);
	free(out);

	/* nvidia_uvm loaded with a VF anywhere: refuse before any change. */
	act_host("act-uvm", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	sim_module("nvidia_uvm");
	{
		static const struct fx_fn spare[] = { { "0000:05:00.0", "2206", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };
		static const struct fx_topo t5 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };

		make_card(root, &t5, spare, 0, "4/4");
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "nvidia_uvm is loaded and 0000:05:00.0 has sriov_numvfs=4; UVM is unloaded only while no "
			    "VF exists and no user holds it"), "uvm with VFs: %s", out);
	CHECK(!sim_text("sim.log"), "uvm with VFs: nothing written");
	free(out);

	/* A CUDA client holding the UVM node refuses too. */
	act_host("act-cuda", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_module("nvidia_uvm");
	{
		char fd[GS_PATH_MAX], link[GS_PATH_MAX];

		gs_pathf(fd, sizeof(fd), "%s/proc/777/fd", sim.root);
		gs_mkdir_p(fd, 0755);
		gs_pathf(link, sizeof(link), "%s/3", fd);
		link_to("/dev/nvidia-uvm", link);
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "process 777 holds /dev/nvidia-uvm. Stop it, then run activate again"), "cuda client: %s", out);
	free(out);

	/* A program holds a GPU that has to be removed: it is not removed, and nothing else changed. */
	act_host("act-remove-lock", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.lock_denied = 1;
	{
		char fd[GS_PATH_MAX], link[GS_PATH_MAX];

		gs_pathf(fd, sizeof(fd), "%s/proc/777/fd", sim.root);
		gs_mkdir_p(fd, 0755);
		gs_pathf(link, sizeof(link), "%s/3", fd);
		link_to("/dev/nvidia0", link);
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "NVIDIA's driver did not grant unbindLock") && contains(out, "it was not removed") &&
	      contains(out, "process 777 holds /dev/nvidia0"), "a held GPU is not removed: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "/remove") && !contains(log, "/rescan") && contains(log, "unbindLock 1") &&
	      contains(log, "unbindLock 0"), "no removal, and the lock is given back: %s", log);
	CHECK(contains(log, "snd_hda_intel/bind 0000:03:00.1"), "the audio function is bound again: %s", log);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME), "no guard after a refusal that removed nothing");
	free(log);
	free(out);

	/* NVIDIA's unbindLock not granted at the re-probe: the PF is not unbound, no guard. */
	act_host("act-lock", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim.lock_denied = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "NVIDIA's driver did not grant unbindLock") && contains(out, "A client still holds the GPU"),
	      "lock denied: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "W bus/pci/drivers/nvidia/unbind") && !contains(log, "nvidia/bind"),
	      "nothing unbound: %s", log);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME), "guard cleared after a known-state refusal");
	CHECK(!contains(out, "The activation lock stays set") && contains(out, "so the activation\nlock was removed"),
	      "the closing message matches the guard's state: %s", out);
	CHECK(contains(log, "unbindLock 1") && contains(log, "unbindLock 0"), "lock released: %s", log);
	free(log);
	free(out);
}

/* Display consent, the compositor that is not the manager's, the handoff and the dry run. */
static void test_activate_display(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* The whole display transaction: framebuffer handoff, display unload, reload with the allowlist. */
	act_host("act-display", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "display activate: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(out, "firmware framebuffer simple-framebuffer.0 released"), "handoff: %s", out);
	CHECK(contains(log, "bus/platform/devices/simple-framebuffer.0/driver/unbind simple-framebuffer.0") &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") < pos_of(log, "bus/pci/devices/0000:04:00.0/remove 1"),
	      "the firmware framebuffer is released before the removal: %s", log);
	CHECK(pos_of(log, "R rmmod nvidia_drm") > 0 &&
	      pos_of(log, "R rmmod nvidia_drm") < pos_of(log, "R rmmod nvidia_modeset") &&
	      pos_of(log, "R rmmod nvidia_modeset") < pos_of(log, "sriov-manage -e 0000:04:00.0"),
	      "drm then modeset, before any VF is enabled: %s", log);
	CHECK(contains(log, "R modprobe --ignore-install nvidia_drm modeset=1 fbdev=1 gpu_allow=0x400") &&
	      pos_of(log, "W bus/pci/drivers/nvidia/bind 0000:04:00.0") <
		      pos_of_last(log, "R modprobe --ignore-install nvidia_drm modeset=1 fbdev=1 gpu_allow=0x400"),
	      "the display stack returns after the re-probe with the GPU allowlist: %s", log);
	CHECK(count_of(log, "R rmmod nvidia_drm") == 2 && count_of(log, "R modprobe --ignore-install nvidia_drm") == 2,
	      "the initial load and the host-wide unload/reload around sriov-manage: %s", log);
	CHECK(!contains(log, "systemctl stop display-manager"), "an inactive manager is not stopped: %s", log);
	free(log);
	free(out);

	/* A running display manager without consent refuses and names the commands. */
	act_host("act-dm", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim.manager_active = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "display-manager.service holds a selected GPU") &&
	      contains(out, "does not stop graphical sessions") &&
	      contains(out, "systemctl stop display-manager.service") && contains(out, "systemctl start display-manager.service"),
	      "no consent: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "W ") && !sim_exists("state/" GS_ATTEMPT_NAME), "nothing written, no guard: %s", log);
	free(log);
	free(out);

	/* Consent given, but a process still holds a node after the stop: fail without touching the GPU. */
	act_host("act-client", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim.manager_active = 1;
	act_opts.stop_display_manager = 1;
	{
		char fd[GS_PATH_MAX], link[GS_PATH_MAX];

		gs_pathf(fd, sizeof(fd), "%s/proc/8080/fd", sim.root);
		gs_mkdir_p(fd, 0755);
		gs_pathf(link, sizeof(link), "%s/3", fd);
		link_to("/dev/nvidia0", link);
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "was stopped, but a process still holds a selected GPU's device node") &&
	      contains(out, "Nothing else changed. Restore the display"), "client after stop: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(log, "R systemctl stop display-manager.service") &&
	      contains(log, "R systemctl start display-manager.service") && !contains(log, "W "),
	      "manager stopped and started again, nothing written: %s", log);
	free(log);
	free(out);

	/* Consent given and the manager stopped, then the guard cannot be written: the manager is started again. */
	act_host("act-dm-guard", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim.manager_active = 1;
	act_opts.stop_display_manager = 1;
	sim.on_run = "systemctl stop display-manager";
	sim.on_run_path = "state/" GS_ATTEMPT_NAME;
	sim.on_mkdir = "1";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "cannot write the activation lock") &&
	      !contains(out, "the activation lock stays set"), "guard write fails after the stop: %s", out);
	log = sim_text("sim.log");
	CHECK(log && contains(log, "R systemctl stop display-manager.service") &&
	      contains(log, "R systemctl start display-manager.service") && !contains(log, "W ") && sim.manager_active,
	      "the manager is started again and nothing else is written: %s", log ? log : "");
	{
		char guard[GS_PATH_MAX];
		struct stat st;

		/* What blocked the write is a directory the fixture put there; no guard file exists. */
		gs_pathf(guard, sizeof(guard), "%s/state/" GS_ATTEMPT_NAME, sim.root);
		CHECK(lstat(guard, &st) == 0 && S_ISDIR(st.st_mode), "no guard file was written");
	}
	free(log);
	free(out);

	/* A genuinely unreadable plan refuses before any change, distinct from a readable one. */
	act_host("act-unknown-plan", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	{
		char drm[GS_PATH_MAX];

		gs_pathf(drm, sizeof(drm), "%s/class/drm", sim.root);
		rm_rf(drm);
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "the display clients cannot all be inspected") &&
	      contains(out, "cannot list"), "unknown plan: %s", out);
	free(out);

	/* Dry run on a display host names the display steps without writing. */
	act_host("act-dry-display", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	act_opts.dry_run = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "dry run: every check passed; activation would run, in this order:") &&
	      contains(out, "unload nvidia_drm, then nvidia_modeset") &&
	      contains(out, "load nvidia_modeset, then nvidia_drm modeset=1 fbdev=1 gpu_allow=0x400") &&
	      !contains(out, "gpu_allow=<display GPU IDs>"),
	      "display dry run: %s", out);
	/* A garbage VF count in the card (an uninitialised field) shows up here as extra type lines. */
	{
		int lines = 0;
		const char *q = out;

		while ((q = strstr(q, "nvidia/current_vgpu_type")) != NULL) {
			lines++;
			q++;
		}
		CHECK(lines == 4, "display dry run lists one type line per configured VF: %d\n%s", lines, out);
	}
	free(out);

	/* Dry run on a plain host: every check passes, nothing is written, the plan names each step. */
	act_host("act-dry", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	act_opts.dry_run = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "dry run: every check passed; activation would run, in this order:") &&
	      contains(out, "remove and rescan the empty bus (bridge held awake, one rescan)") &&
	      contains(out, "systemctl restart nvidia-vgpud.service") &&
	      contains(out, "echo 525 > nvidia/current_vgpu_type"), "dry run: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "W ") && !sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_ACTIVATING_NAME),
	      "dry run writes nothing: %s", log);
	free(log);
	free(out);

	/* No display work on a non-display card. */
	act_host("act-plain", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && !contains(out, "nvidia_drm") &&
	      !contains(out, "display allowlist"), "plain host: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "nvidia_drm") && !contains(log, "nvidia_modeset"), "no display modules touched: %s", log);
	free(log);
	free(out);

	/*
	 * Cold display host: the firmware framebuffer is still the only user (no nvidia, no display
	 * modules loaded). The release must happen before the driver binds and before nvidia_drm
	 * loads; that is the order the lifecycle owner fixed.
	 */
	fixture("act-cold-display", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	set_bridge_windows(root, &topo_display, 18, 1440);
	set_root_windows(root, 64, 8192);
	sim_writef("module/geforce_sriov/refcnt", "0\n");
	sim_writef("module/geforce_sriov/parameters/ari", "Y\n");
	sim_writef("module/geforce_sriov/parameters/msix", "Y\n");
	sim_writef("module/geforce_sriov/parameters/vfs", "4\n");
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:04:00.0\n");
	sim_writef("kernel/livepatch/geforce_sriov/enabled", "1\n");
	sim_writef("kernel/livepatch/geforce_sriov/transition", "0\n");
	add_firmware_fb(root, &topo_display, "simple-framebuffer.0", "simple-framebuffer");
	{
		char p[GS_PATH_MAX], target[GS_PATH_MAX];

		gs_pathf(p, sizeof(p), "%s/class/graphics/fb0", root);
		gs_mkdir_p(p, 0755);
		gs_pathf(p, sizeof(p), "%s/class/graphics/fb0/device", root);
		gs_pathf(target, sizeof(target), "../../../devices/%s/%s/simple-framebuffer.0",
			 topo_display.bridge_path, topo_display.pf);
		link_to(target, p);
	}
	add_iomem_fw(root, &topo_display, 1);
	gs_copy(sim.display_pf, sizeof(sim.display_pf), topo_display.pf);
	config_of(&act_cfg, "bdfs=0000:04:00.0\n");
	memset(&act_opts, 0, sizeof(act_opts));
	act_opts.boot = 1;
	act_opts.boot_wait_ms = 1;
	act_opts.settle_ms = 1;
	act_opts.types_wait_ms = 100;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "firmware framebuffer simple-framebuffer.0 released") &&
	      contains(out, "activation finished"), "cold display: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "simple-framebuffer.0/driver/unbind") > 0 &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") < pos_of(log, "R modprobe nvidia\n") &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") < pos_of(log, "R modprobe --ignore-install nvidia_drm") &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") < pos_of(log, "bus/pci/devices/0000:04:00.0/remove 1"),
	      "the firmware framebuffer is released before the driver and DRM load, and before the remove: %s", log);
	CHECK(contains(log, "R modprobe nvidia_modeset") && contains(log, "R modprobe --ignore-install nvidia_drm modeset=1 fbdev=1 gpu_allow=0x400"),
	      "the display stack loads after the release: %s", log);
	free(log);
	free(out);
}

/* Two cards on two bridges: per-bus rescan, one enable each, display=boot limits KMS to the boot GPU. */
static void test_activate_mixed(void)
{
	char root[GS_PATH_MAX], *out, *log;
	static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };

	/* 0000:03:00.0 is plain, 0000:04:00.0 is the boot display; one host-wide core serves both. */
	act_host("act-mixed", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_display, 1, NULL);
	set_bridge_windows(root, &topo_display, 18, 1440);
	sim_module("nvidia_modeset");
	sim_module("nvidia_drm");
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:03:00.0,0000:04:00.0\n");
	add_firmware_fb(root, &t4, "simple-framebuffer.0", "simple-framebuffer");
	add_iomem_fw(root, &t4, 1);
	gs_copy(sim.display_pf, sizeof(sim.display_pf), "0000:04:00.0");
	act_cfg.nbdfs = 2;
	gs_addr_parse("0000:03:00.0", &act_cfg.bdfs[0]);
	gs_addr_parse("0000:04:00.0", &act_cfg.bdfs[1]);
	act_cfg.display = GS_DISPLAY_BOOT;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "two cards: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(log, "devices/pci0000:00/0000:00:02.0/pci_bus/0000:03/rescan") &&
	      contains(log, "devices/pci0000:00/0000:00:03.0/pci_bus/0000:04/rescan") &&
	      count_of(log, "/rescan 1") == 2, "one rescan on each card's own bus: %s", log);
	CHECK(count_of(log, "sriov-manage -e") == 2 &&
	      contains(log, "sriov-manage -e 0000:03:00.0") && contains(log, "sriov-manage -e 0000:04:00.0"),
	      "one enable per card: %s", log);
	CHECK(count_of(log, "nvidia/unbind 0000:03:00.0") == 1 && count_of(log, "nvidia/unbind 0000:04:00.0") == 1,
	      "each PF is re-probed once: %s", log);
	CHECK(contains(log, "R modprobe --ignore-install nvidia_drm modeset=1 fbdev=1 gpu_allow=0x400") &&
	      !contains(log, "gpu_allow=0x300") && !contains(log, "0x300,0x400"),
	      "the allowlist names only the boot display GPU: %s", log);
	CHECK(count_of(log, "R rmmod nvidia_drm") == 2 && count_of(log, "R modprobe --ignore-install nvidia_drm") == 2,
	      "the initial load and the host-wide unload/reload around sriov-manage: %s", log);
	CHECK(count_of(log, "R systemctl restart nvidia-vgpud.service") == 1,
	      "the host-wide daemons restart once: %s", log);
	CHECK(contains(out, "OK 0000:03:00.4 (VF of 0000:03:00.0)") &&
	      contains(out, "OK 0000:04:00.4 (VF of 0000:04:00.0)"), "both cards' VFs ready: %s", out);
	free(log);
	free(out);
}

/*
 * display=on puts KMS on every selected card, the boot display GPU and any other. With display=boot a selection
 * without any boot display GPU loads no display stack at all rather than an empty allowlist (which would mean
 * every GPU).
 */
static struct gs_build_src kms_src;
static void build_for(struct gs_build_src *src, const char *dir, int display);

static void test_activate_display_on(void)
{
	char root[GS_PATH_MAX], *out, *log;
	static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };

	/* 0000:03:00.0 is plain, 0000:04:00.0 is the boot display; display=on asks for both. */
	act_host("act-on-mixed", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_display, 1, NULL);
	set_bridge_windows(root, &topo_display, 18, 1440);
	sim_module("nvidia_modeset");
	sim_module("nvidia_drm");
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:03:00.0,0000:04:00.0\n");
	add_firmware_fb(root, &t4, "simple-framebuffer.0", "simple-framebuffer");
	add_iomem_fw(root, &t4, 1);
	gs_copy(sim.display_pf, sizeof(sim.display_pf), "0000:04:00.0");
	act_cfg.nbdfs = 2;
	gs_addr_parse("0000:03:00.0", &act_cfg.bdfs[0]);
	gs_addr_parse("0000:04:00.0", &act_cfg.bdfs[1]);
	act_cfg.display = GS_DISPLAY_ON;
	/* A card that is not the boot display has KMS only when the build record says the display modules are in. */
	build_for(&kms_src, root, 1);
	writef(kms_src.record, "kernel=6.8.0\nmodule_build_id=deadbeef01020304\ndisplay=1\nhost_cuda=0\n");
	act_opts.build = &kms_src;

	/* The dry run names the allowlist with both cards, and writes nothing. */
	act_opts.dry_run = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "gpu_allow=0x300,0x400") && !contains(out, "is limited") &&
	      !contains(out, "gpu_allow=<display GPU IDs>"), "display=on dry run: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "W ") && !sim_exists("state/" GS_ATTEMPT_NAME), "display=on dry run writes nothing: %s", log);
	free(log);
	free(out);

	/* The run loads nvidia_drm with both cards in gpu_allow and prints no limit notice. */
	act_opts.dry_run = 0;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && !contains(out, "is limited"), "display=on two cards: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(log, "R modprobe --ignore-install nvidia_drm modeset=1 fbdev=1 gpu_allow=0x300,0x400"),
	      "display=on names every selected card in gpu_allow: %s", log);
	CHECK(count_of(log, "R modprobe --ignore-install nvidia_drm") == 2,
	      "the display stack loads around the VF enable: %s", log);
	free(log);
	free(out);

	/* display=boot and no selected card boots the display (0 and unknown): no stack, and the plain path still runs. */
	act_host("act-on-noboot", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_display, 0, NULL);
	set_bridge_windows(root, &topo_display, 18, 1440);
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:03:00.0,0000:04:00.0\n");
	sim_remove("bus/pci/devices/0000:04:00.0/boot_vga");
	act_cfg.nbdfs = 2;
	gs_addr_parse("0000:03:00.0", &act_cfg.bdfs[0]);
	gs_addr_parse("0000:04:00.0", &act_cfg.bdfs[1]);
	act_cfg.display = GS_DISPLAY_BOOT;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"),
	      "display=boot without a boot display: %s", out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "nvidia_drm") && !contains(log, "nvidia_modeset") &&
	      !sim_exists("module/nvidia_drm") && !sim_exists("module/nvidia_modeset"),
	      "no display stack without a boot display: %s", log);
	free(log);
	free(out);
}

/*
 * A host that a boot activation already finished: four VFs enabled and registered, each with type
 * ("0": none). holder names the client that would deny unbindLock (see sim.lock_holder).
 */
static void rerun_host(const char *name, const char *type, const char *holder, char *root, size_t len)
{
	char rel[128];

	act_host(name, &topo_default, gpu_ampere, 0, "0/4", root, len);
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_writef("bus/pci/devices/0000:03:00.0/power/control", "on\n");
	sim_writef("bus/pci/devices/0000:03:00.0/power/runtime_status", "active\n");
	sim_enable_vfs("0000:03:00.0");
	for (unsigned int i = 4; i <= 7; i++) {
		snprintf(rel, sizeof(rel), "bus/pci/devices/0000:03:00.%u/nvidia/current_vgpu_type", i);
		sim_writef(rel, type);
	}
	sim_module("nvidia_vgpu_vfio");
	sim_module("nvidia_uvm");
	sim_writef("module/nvidia_uvm/parameters/uvm_disable_hmm", "Y\n");
	/* An active host has the device nodes of its modules: an earlier activation made them. */
	sim_writef("dev/nvidiactl", "");
	sim_writef("dev/nvidia-uvm", "");
	sim_writef("dev/nvidia-uvm-tools", "");
	act_cfg.host_cuda = GS_SW_ON;
	sim.mgr_active = 1;
	sim.lock_holder = holder;
	act_opts.boot = 0;
}

/* A laptop GPU is touched only with the D3cold consent in the config. */
static void test_activate_d3cold_consent(void)
{
	static const struct fx_fn lap[] = {
		{ "0000:03:00.0", "28e0", "030000", "nvidia" },
		{ NULL, NULL, NULL, NULL }
	};
	char root[GS_PATH_MAX], *out;

	act_host("act-laptop", &topo_default, lap, 0, NULL, root, sizeof(root));
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "refused, nothing changed: no D3cold consent in the config for laptop GPU 0000:03:00.0") &&
	      contains(out, "geforce-sriov install --device 0000:03:00.0 --disable-d3cold"), "laptop GPU without consent: %s", out);
	CHECK(!sim_text("sim.log") && !sim_exists("state/" GS_ATTEMPT_NAME), "nothing was written");
	free(out);
	act_cfg.d3cold_off = 1;
	run_host(activate_host, &out);
	CHECK(!contains(out, "no D3cold consent"), "with d3cold=off the consent check passes: %s", out);
	free(out);
}

/* The console card needs a removal while another selected card has VFs: nothing is unloaded. */
static void test_activate_handoff_vfs(void)
{
	char root[GS_PATH_MAX], *out;
	static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };

	act_host("act-handoff-vfs", &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_enable_vfs("0000:03:00.0");
	make_card(root, &topo_display, gpu_display, 1, NULL);
	set_bridge_windows(root, &topo_display, 18, 1440);
	sim_module("nvidia_modeset");
	sim_module("nvidia_drm");
	sim_writef("module/nvidia/refcnt", "0\n");
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:03:00.0,0000:04:00.0\n");
	add_firmware_fb(root, &t4, "simple-framebuffer.0", "simple-framebuffer");
	add_iomem_fw(root, &t4, 1);
	gs_copy(sim.display_pf, sizeof(sim.display_pf), "0000:04:00.0");
	act_cfg.nbdfs = 2;
	gs_addr_parse("0000:03:00.0", &act_cfg.bdfs[0]);
	gs_addr_parse("0000:04:00.0", &act_cfg.bdfs[1]);
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "refused, nothing changed: 0000:03:00.0 has sriov_numvfs=4") &&
	      contains(out, "0000:04:00.0 carries the host console"), "console card with VFs elsewhere: %s", out);
	CHECK(!sim_text("sim.log") && sim_exists("module/nvidia_drm") && sim_exists("module/nvidia") &&
	      !sim_exists("state/" GS_ATTEMPT_NAME), "nothing was unloaded or written");
	free(out);
}

/* One card whose VFs activation has to enable: SR-IOV presented, no VF yet. */
static void enable_host(const char *name, char *root, size_t len)
{
	act_host(name, &topo_default, gpu_ampere, 0, "0/4", root, len);
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim.on_run = sim.on_run_path = sim.on_run_write = NULL;
}

/* Gates and stops that end an activation early: each refuses or fails, and none reports success. */
static void test_activate_stops(void)
{
	char root[GS_PATH_MAX], p[GS_PATH_MAX], *out, *log, *text;
	struct gs_build_src src;

	/* The loaded livepatch was loaded for other GPUs, or for another VF count: refused before any write. */
	enable_host("act-lp-bdfs", root, sizeof(root));
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:05:00.0\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED && contains(out, "bdfs=0000:05:00.0, not the configured") &&
	      !sim_exists("sim.log"), "livepatch loaded for other GPUs: %s", out);
	free(out);
	enable_host("act-lp-vfs", root, sizeof(root));
	sim_writef("module/geforce_sriov/parameters/vfs", "8\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED && contains(out, "vfs=8 bdfs=0000:03:00.0, not the configured") &&
	      !sim_exists("sim.log"), "livepatch loaded for another VF count: %s", out);
	free(out);

	/* A build record exists and the module is not that build (a kernel update put the stock module back). */
	enable_host("act-stock-module", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/driver-build", root);
	writef(p, "kernel=6.8.0\n");
	memset(&src, 0, sizeof(src));
	src.record = p;
	src.release = "6.8.0";
	src.module = "/nonexistent/nvidia.ko";
	act_opts.build = &src;
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "refused, nothing changed: nvidia.ko for 6.8.0 is not") &&
	      contains(out, "build it again for this kernel (`sudo geforce-sriov rebuild`)") && !sim_exists("sim.log"),
	      "module is not the recorded build: %s", out);
	free(out);
	/* No record at all: a warning, and activation goes on. */
	enable_host("act-no-record", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/no-driver-build", root);
	act_opts.build = &src;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "warning: nvidia.ko for 6.8.0 is not") &&
	      contains(out, "activation finished"), "no build record warns and goes on: %s", out);
	free(out);

	/* Another NVIDIA GPU, not selected, has VFs: the daemon restart would re-register it too. */
	enable_host("act-foreign-vf", root, sizeof(root));
	make_card(root, &topo_display, gpu_display, 0, "0/4");
	set_bridge_windows(root, &topo_display, 18, 1440);
	set_vf_bars(root, &topo_display, "0000:04:00.0", 1);
	sim_enable_vfs("0000:04:00.0");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED && contains(out, "of 0000:04:00.0: that GPU is not selected") &&
	      !sim_exists("sim.log"), "VFs on a GPU that is not selected: %s", out);
	free(out);

	/* A VF that sriov-manage left without the nvidia driver: no daemon is restarted. */
	enable_host("act-vf-unbound", root, sizeof(root));
	sim.on_run = "sriov-manage -e";
	sim.on_run_path = "bus/pci/devices/0000:03:00.4/driver";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && !contains(out, "activation finished"), "a VF without a driver: %s", out);
	log = sim_text("sim.log");
	CHECK(log && !contains(log, "R systemctl restart nvidia-vgpud.service"), "no daemon restart after an unbound VF: %s", log);
	free(log);
	free(out);

	/* nvidia-vgpud does not restart: nvidia-vgpu-mgr is not started and no GPU is re-probed. */
	enable_host("act-vgpud-fail", root, sizeof(root));
	sim.run_fail = "systemctl restart nvidia-vgpud.service";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "no GPU was re-probed") &&
	      !contains(out, "activation finished"), "vgpud restart fails: %s", out);
	log = sim_text("sim.log");
	CHECK(log && !contains(log, "nvidia-vgpu-mgr.service"), "the manager is left alone after vgpud failed: %s", log);
	free(log);
	free(out);
	sim.run_fail = NULL;

	/* nvidia-vgpu-mgr does not start: no GPU is re-probed. */
	enable_host("act-mgr-fail", root, sizeof(root));
	sim.run_fail = "nvidia-vgpu-mgr.service";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "no GPU was re-probed") &&
	      !contains(out, "activation finished"), "manager start fails: %s", out);
	free(out);
	sim.run_fail = NULL;

	/* A VM took a vGPU between the checks and the vgpud restart: the manager is not restarted. */
	enable_host("act-vm-late", root, sizeof(root));
	sim.on_run = "systemctl restart nvidia-vgpud.service";
	sim.on_run_path = "bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type";
	sim.on_run_write = "525\n";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "VF 0000:03:00.4 has vGPU type 525 since the checks; nvidia-vgpud was restarted, nvidia-vgpu-mgr was not"),
	      "a VM started after the checks: %s", out);
	free(out);

	/* Host CUDA with VFs to enable: loads by name are blocked for the window, and the block is released. */
	enable_host("act-enable-block", root, sizeof(root));
	act_cfg.host_cuda = GS_SW_ON;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "nvidia_drm and nvidia_uvm autoloading is blocked while the VFs are enabled") &&
	      !contains(out, "cannot block module autoloading"), "the enable window is blocked: %s", out);
	text = sim_text("sim.block");
	CHECK(text && contains(text, "install nvidia_drm /bin/false") && contains(text, "install nvidia_uvm /bin/false"),
	      "the block is in place while sriov-manage runs: %s", text ? text : "(no file)");
	free(text);
	gs_pathf(p, sizeof(p), "%s/../modprobe.d/zz-geforce-sriov-enable.conf", sim.run);
	text = slurp(p);
	CHECK(text && contains(text, "# released by geforce-sriov") && !contains(text, "/bin/false"),
	      "the block is released afterwards: %s", text ? text : "(no file)");
	free(text);
	free(out);

	/* The last check decides the exit status: a VF that lost its driver after it was configured is a failure. */
	enable_host("act-final-check", root, sizeof(root));
	act_cfg.host_cuda = GS_SW_ON;
	sim.on_run = "nvidia_uvm";
	sim.on_run_path = "bus/pci/devices/0000:03:00.4/driver";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "activation finished") &&
	      contains(out, "NOT READY 0000:03:00.4"), "the final check sets the exit status: %s", out);
	free(out);
	sim.on_run = sim.on_run_path = sim.on_run_write = NULL;
}

/*
 * The device nodes of the loaded modules exist when activation ends. Reported on hardware: a
 * container that is given /dev/nvidia-modeset did not start after a boot, because the module was
 * loaded and nothing had made its node.
 */
static void test_activate_nodes(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* A display build: the modeset node, the control node, the GPU's node and the capability nodes. */
	act_host("nodes-display", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim_writef("proc/driver/nvidia/gpus/0000:04:00.0/information",
		   "Model: \t\t NVIDIA GeForce RTX 3080\nBus Location: \t 0000:04:00.0\nDevice Minor: \t 0\nGPU Excluded:\t No\n");
	sim_writef("proc/driver/nvidia/capabilities/mig/config", "DeviceFileMinor: 1\nDeviceFileMode: 256\nDeviceFileModify: 1\n");
	sim_writef("proc/driver/nvidia/capabilities/mig/monitor", "DeviceFileMinor: 2\nDeviceFileMode: 256\nDeviceFileModify: 1\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "nvidia-modprobe -c 0 -c 255 -m -f ") &&
	      contains(out, "/driver/nvidia/capabilities/mig/config -f ") && !contains(out, "warning: these device nodes"),
	      "display: the nodes are made: %s", out);
	CHECK(sim_exists("dev/nvidia-modeset") && sim_exists("dev/nvidiactl") && sim_exists("dev/nvidia0") &&
	      sim_exists("dev/nvidia-caps/nvidia-cap1") && sim_exists("dev/nvidia-caps/nvidia-cap2"),
	      "display: every node of the loaded modules exists");
	CHECK(!sim_exists("dev/nvidia-uvm") && !contains(out, "nvidia-modprobe -u"), "display: no UVM node without nvidia_uvm: %s", out);
	CHECK(pos_of(out, "nvidia-modprobe -c 0") < pos_of(out, "activation finished"), "display: before activation ends: %s", out);
	free(out);

	/* Host CUDA: the two UVM nodes, and no modeset node without the module. */
	enable_host("nodes-cuda", root, sizeof(root));
	act_cfg.host_cuda = GS_SW_ON;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "nvidia-modprobe -c 255\n") &&
	      contains(out, "nvidia-modprobe -u -c 0\n"), "host CUDA: the nodes are made: %s", out);
	log = sim_text("sim.log");
	CHECK(sim_exists("dev/nvidia-uvm") && sim_exists("dev/nvidia-uvm-tools") && sim_exists("dev/nvidiactl") &&
	      !sim_exists("dev/nvidia-modeset"), "host CUDA: the UVM nodes exist, the modeset node does not");
	CHECK(pos_of(log, "R modprobe nvidia_uvm") < pos_of(log, "R nvidia-modprobe -u -c 0"), "host CUDA: after the module is loaded: %s", log);
	free(log);
	free(out);
	/* A second run finds every node and runs nothing. */
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "there is nothing to do") &&
	      !contains(out, "nvidia-modprobe"), "host CUDA, again: nothing is run: %s", out);
	free(out);
	/* A node that went away is made again by a run that has nothing else to do. */
	sim_remove("dev/nvidia-uvm-tools");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "there is nothing to do") &&
	      contains(out, "nvidia-modprobe -u -c 0\n") && !contains(out, "nvidia-modprobe -c") &&
	      sim_exists("dev/nvidia-uvm-tools"), "host CUDA, a node gone: it is made again: %s", out);
	free(out);

	/* nvidia-modprobe is not installed: a warning with the commands, and activation succeeds. */
	enable_host("nodes-noprogram", root, sizeof(root));
	act_cfg.host_cuda = GS_SW_ON;
	sim.no_nvidia_modprobe = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished") &&
	      contains(out, "warning: these device nodes of NVIDIA's loaded modules do not exist: /dev/nvidiactl, /dev/nvidia-uvm, /dev/nvidia-uvm-tools.") &&
	      contains(out, "nvidia-modprobe, the program of NVIDIA's host driver package that creates them, was not found. To create them, run as root:\n"
			    "  nvidia-modprobe -c 255\n  nvidia-modprobe -u -c 0\n"),
	      "no nvidia-modprobe: a warning, not a failure: %s", out);
	free(out);
	sim.no_nvidia_modprobe = 0;

	/* It fails for one group of nodes: the others are made, and the warning names what is left. */
	enable_host("nodes-fail", root, sizeof(root));
	act_cfg.host_cuda = GS_SW_ON;
	sim.run_fail = "nvidia-modprobe -u";
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && sim_exists("dev/nvidiactl") &&
	      contains(out, "do not exist: /dev/nvidia-uvm, /dev/nvidia-uvm-tools.") &&
	      contains(out, "`nvidia-modprobe -u -c 0` exited with 1. To create them, run as root:\n  nvidia-modprobe -u -c 0\n") &&
	      !contains(out, "  nvidia-modprobe -c 255"), "nvidia-modprobe fails: %s", out);
	free(out);
	sim.run_fail = NULL;

	/* The dry run names the step. */
	enable_host("nodes-dry", root, sizeof(root));
	act_opts.dry_run = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "  nvidia-modprobe, for each device node of a loaded NVIDIA module that does not exist\n") &&
	      !sim_exists("dev/nvidiactl"), "dry run: %s", out);
	free(out);
	act_opts.dry_run = 0;
}

/* One card whose VFs activation has to enable. */
static void one_card_host(const char *name, const struct fx_fn *fns, char *root, size_t len)
{
	act_host(name, &topo_default, fns, 0, "0/4", root, len);
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_writef("bus/pci/devices/0000:03:00.0/power/control", "on\n");
	sim_writef("bus/pci/devices/0000:03:00.0/power/runtime_status", "active\n");
}

/*
 * The Ada slot hint names VFs whose fixed slot ends near the top of the card. A card whose VFs read placement_id
 * Not Applicable (the module carries the Ada placement change) has no slots and gets no line.
 */
static void test_slot_hint(void)
{
	char root[GS_PATH_MAX], xml[GS_PATH_MAX], *out;

	for (int fixed = 0; fixed < 2; fixed++) {
		char name[32];

		snprintf(name, sizeof(name), "slot-hint-%d", fixed);
		one_card_host(name, gpu_ada, root, sizeof(root));
		sim_placement_ids = fixed;
		sim_writef("nvidia-smi.out", "00000000:03:00.0, 10000\n");
		gs_pathf(xml, sizeof(xml), "%s/vgpuConfig.xml", root);
		writef(xml, "<vgpuTypes><vgpuType id=\"942\" name=\"RTX6000-Ada-2Q\"><profileSize>0x80000000</profileSize></vgpuType></vgpuTypes>\n");
		act_opts.vgpu_config = xml;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished") &&
		      contains(out, "0000:03:00.0: with type 942 the VF 0000:03:00.7 may not start: its fixed range of video memory ends near the top of the card's 10000 MiB. "
				    "Put VMs on the lower VFs, or choose a smaller type.\n") == fixed,
		      "slot hint with %s: %s", fixed ? "fixed slots" : "the Ada placement change", out);
		free(out);
		sim_placement_ids = 0;
	}
}

/* fb_slots= was removed in 0.4.0: a line left in a config is read and dropped, and the config is never written with it. */
static void test_fb_slots_ignored(void)
{
	struct gs_config c;
	char err[256], line[768];

	gs_config_defaults(&c);
	CHECK(gs_config_parse("fb_slots=off\n", 13, &c, err, sizeof(err)) == 0 && c.fb_slots_ignored, "fb_slots=off is read and ignored: %s", err);
	CHECK(gs_config_parse("fb_slots=on\nari=on\n", 19, &c, err, sizeof(err)) == 0 && c.fb_slots_ignored, "fb_slots=on is read and ignored: %s", err);
	CHECK(gs_config_render(&c, line, sizeof(line)) > 0 && !contains(line, "fb_slots"), "the config is written without it");
	gs_config_defaults(&c);
	CHECK(gs_config_parse("ari=on\n", 7, &c, err, sizeof(err)) == 0 && !c.fb_slots_ignored, "no key, no note");
}

/* The removed fb_placement key, host_desktop, which Ada card is selected, and the VM video memory line of status and check. */
static void test_ada_placement(void)
{
#define FP_PARSE(text) gs_config_parse(text, sizeof(text) - 1, &c, err, sizeof(err))
	struct gs_config c, back;
	char line[768], text[4096], err[256], sys[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX];

	/* fb_placement was never released: every value of it is an unknown key, and the message is the existing one. */
	CHECK(FP_PARSE("fb_placement=auto\n") != 0 && strcmp(err, "line 1: unknown key 'fb_placement'") == 0, "fb_placement=auto: %s", err);
	CHECK(FP_PARSE("fb_placement=top\n") != 0 && strcmp(err, "line 1: unknown key 'fb_placement'") == 0, "fb_placement=top: %s", err);
	CHECK(FP_PARSE("fb_placement=default\n") != 0 && strcmp(err, "line 1: unknown key 'fb_placement'") == 0, "fb_placement=default: %s", err);
	CHECK(FP_PARSE("ari=on\nfb_placement=auto\n") != 0 && strcmp(err, "line 2: unknown key 'fb_placement'") == 0, "fb_placement after another key: %s", err);
	/* The written config has no placement line, and what it writes reads back. */
	gs_config_defaults(&c);
	CHECK(gs_config_render(&c, text, sizeof(text)) > 0 && !contains(text, "fb_placement") && !contains(text, "placement"),
	      "the rendered config has no placement line: %s", text);
	CHECK(gs_config_parse(text, strlen(text), &back, err, sizeof(err)) == 0, "the written config reads back: %s", err);
	/* host_desktop: auto, on or off; auto, which is what an absent key means, is written back as auto. */
	CHECK(FP_PARSE("host_desktop=auto\n") == 0 && c.host_desktop == GS_DESKTOP_AUTO, "host_desktop=auto");
	CHECK(FP_PARSE("host_desktop=on\n") == 0 && c.host_desktop == GS_DESKTOP_ON, "host_desktop=on");
	CHECK(FP_PARSE("host_desktop=off\n") == 0 && c.host_desktop == GS_DESKTOP_OFF, "host_desktop=off");
	CHECK(FP_PARSE("host_desktop=maybe\n") != 0 && contains(err, "line 1: host_desktop must be auto, on or off"), "host_desktop=maybe: %s", err);
	CHECK(FP_PARSE("host_desktop=on\nhost_desktop=off\n") != 0, "host_desktop twice: %s", err);
	gs_config_defaults(&c);
	CHECK(c.host_desktop == GS_DESKTOP_AUTO, "host_desktop: auto by default, also for a config of 0.3.0 that has no such line");
	CHECK(gs_config_render(&c, text, sizeof(text)) > 0 && contains(text, "\nhost_cuda=off\nhost_desktop=auto\n"), "auto is written after host_cuda: %s", text);
	c.host_desktop = GS_DESKTOP_OFF;
	CHECK(gs_config_render(&c, text, sizeof(text)) > 0 && contains(text, "\nhost_desktop=off\n"), "off is written: %s", text);
	CHECK(gs_config_parse(text, strlen(text), &back, err, sizeof(err)) == 0 && back.host_desktop == GS_DESKTOP_OFF, "and read back: %s", err);

	/* Which selection asks for the Ada placement change: one selected GPU that is Ada. */
	{
		struct gs_addr ampere[1], ada[1], mixed[2], none[1];

		gs_pathf(sys, sizeof(sys), "%s/fp-sys", tmp);
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:01:00.0/device", sys);
		writef(p, "0x2206\n");
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:02:00.0/device", sys);
		writef(p, "0x2684\n");
		CHECK(gs_parse_addr_list("bdfs", "0000:01:00.0", ampere, &(unsigned int){ 0 }, err, sizeof(err)) == 0 &&
		      gs_parse_addr_list("bdfs", "0000:02:00.0", ada, &(unsigned int){ 0 }, err, sizeof(err)) == 0 &&
		      gs_parse_addr_list("bdfs", "0000:01:00.0,0000:02:00.0", mixed, &(unsigned int){ 0 }, err, sizeof(err)) == 0 &&
		      gs_parse_addr_list("bdfs", "0000:09:00.0", none, &(unsigned int){ 0 }, err, sizeof(err)) == 0, "fixture addresses: %s", err);
		CHECK(!gs_fb_ada_selected(sys, ampere, 1), "Ampere only: no Ada card");
		CHECK(gs_fb_ada_selected(sys, ada, 1), "Ada only: an Ada card");
		CHECK(gs_fb_ada_selected(sys, mixed, 2), "Ampere and Ada: an Ada card");
		CHECK(!gs_fb_ada_selected(sys, none, 1) && !gs_fb_ada_selected(sys, ampere, 0), "a GPU that is absent or no GPU selected: no Ada card");
	}

	/* The VM video memory line, from a fixture sysfs: a card 0000:02:00.0 with two VFs. */
	{
		static const char *const vfs[2] = { "0000:02:00.4", "0000:02:00.5" };
		struct gs_addr pf;
		const char *set[3][2] = { { "Not Applicable", "Not Applicable" }, { "0", "2" }, { "Not Applicable", "4" } };

		CHECK(gs_addr_parse("0000:02:00.0", &pf) == 0, "pf address");
		for (unsigned int i = 0; i < 2; i++) {
			gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/placement_id", sys, vfs[i]);
			writef(p, "%s\n", set[0][i]);
			gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, vfs[i]);
			link_to("../0000:02:00.0", p);
			gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:02:00.0/virtfn%u", sys, i);
			snprintf(q, sizeof(q), "../%s", vfs[i]);
			link_to(q, p);
		}
		CHECK(gs_ada_placement_line(sys, &pf, 2, line, sizeof(line)) == 1 &&
		      strcmp(line, "VM video memory: taken from free memory when a VM starts (placement_id: Not Applicable)") == 0,
		      "every VF Not Applicable: %s", line);
		for (unsigned int k = 1; k < 3; k++) {
			for (unsigned int i = 0; i < 2; i++) {
				gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/placement_id", sys, vfs[i]);
				writef(p, "%s\n", set[k][i]);
			}
			CHECK(gs_ada_placement_line(sys, &pf, 2, line, sizeof(line)) == 0 &&
			      contains(line, k == 1 ? "VM video memory: fixed slots (placement_id 0, 2): the loaded nvidia.ko does not carry the Ada placement change. "
						      "Run sudo geforce-sriov install, then power the machine off and on" :
						      "VM video memory: fixed slots (placement_id 4): the loaded nvidia.ko does not carry the Ada placement change. "
						      "Run sudo geforce-sriov install, then power the machine off and on"),
			      "a VF reads a number (%u): %s", k, line);
		}
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/placement_id", sys, vfs[1]);
		unlink(p);
		CHECK(gs_ada_placement_line(sys, &pf, 2, line, sizeof(line)) == 0 &&
		      strcmp(line, "VM video memory: placement_id could not be read") == 0, "a file that cannot be read: %s", line);
		writef(p, "n/a\n");
		CHECK(gs_ada_placement_line(sys, &pf, 2, line, sizeof(line)) == 0 &&
		      strcmp(line, "VM video memory: placement_id could not be read") == 0, "a value that is neither: %s", line);
	}
#undef FP_PARSE
}

/*
 * udev's module loading when a GPU comes back from the rescan, and what does and does not stop it.
 * Each case is a defect this project met on hardware: nvidia_drm loaded behind activation's back.
 */
static void test_activate_udev(void)
{
	char root[GS_PATH_MAX], stage[GS_PATH_MAX], p[GS_PATH_MAX], *out, *log, *alias;
	const char *sel[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr",
			      "--device", "0000:03:00.0", "--no-display", NULL };

	/* No alias override: udev loads nvidia_drm for the rescanned GPU. It is unloaded before a VF is enabled. */
	act_host("act-udev", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.udev = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "udev autoload: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "U udev loads nvidia_drm for 0000:03:00.0") > 0 &&
	      pos_of(log, "U udev loads nvidia_drm for 0000:03:00.0") < pos_of(log, "R rmmod nvidia_drm") &&
	      pos_of(log, "R rmmod nvidia_drm") < pos_of(log, "R rmmod nvidia_modeset") &&
	      pos_of(log, "R rmmod nvidia_modeset") < pos_of(log, "sriov-manage -e") && !sim_exists("module/nvidia_drm"),
	      "the autoloaded display modules are gone before any VF is enabled: %s", log);
	free(log);
	free(out);

	/* A blacklist line and an install rule do not stop udev. */
	act_host("act-udev-blacklist", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.udev = 1;
	sim_writef("etc/modprobe.d/local.conf", "blacklist nvidia_drm\ninstall nvidia_drm /bin/false\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK, "udev with a blacklist: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(log, "U udev loads nvidia_drm for 0000:03:00.0"), "blacklist and install do not stop udev: %s", log);
	free(log);
	free(out);

	/* The alias overrides that install writes do: the file is taken from a staged install as it is. */
	snprintf(stage, sizeof(stage), "%s/stage-udev", tmp);
	CHECK(run_cli(&out, NULL, sel) == GS_EXIT_OK, "staged install for the alias file: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/etc/modprobe.d/geforce-sriov-autoload.conf", stage);
	alias = slurp(p);
	CHECK(alias != NULL, "install wrote %s", p);
	act_host("act-udev-alias", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.udev = 1;
	sim_writef("etc/modprobe.d/geforce-sriov-autoload.conf", alias ? alias : "");
	free(alias);
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "udev with the alias file: %s", out);
	log = sim_text("sim.log");
	CHECK(log && !contains(log, "U udev loads") && !contains(log, "rmmod nvidia_drm"),
	      "install's alias file keeps udev from loading nvidia_drm: %s", log ? log : "");
	free(log);
	free(out);

	/* On the boot display GPU the autoloaded nvidia_drm has no options; the console still gets them. */
	act_host("act-udev-display", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim.udev = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "udev on the display GPU: %s", out);
	log = sim_text("sim.log");
	{
		char *modeset = sim_text("module/nvidia_drm/parameters/modeset");
		char *allow = sim_text("module/nvidia_drm/parameters/gpu_allow");

		CHECK(contains(log, "U udev loads nvidia_drm for 0000:04:00.0") && modeset && modeset[0] == 'Y' && allow &&
		      strncmp(allow, "0x400", 5) == 0, "nvidia_drm ends up loaded with modeset and the allowlist: modeset=%s allow=%s",
		      modeset ? modeset : "(none)", allow ? allow : "(none)");
		free(modeset);
		free(allow);
	}
	free(log);
	free(out);

	/* A display module that something still uses cannot be unloaded: activation stops before any VF is enabled. */
	enable_host("act-drm-in-use", root, sizeof(root));
	sim_module("nvidia_drm");
	sim_writef("module/nvidia_drm/refcnt", "1\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "rmmod nvidia_drm exited with 1 and the module is still loaded"),
	      "a display module in use: %s", out);
	log = sim_text("sim.log");
	CHECK(log && !contains(log, "sriov-manage") && !contains(log, "rmmod nvidia_modeset"), "nothing after the failed unload: %s",
	      log ? log : "");
	free(log);
	free(out);
}

/*
 * The display fixtures once more with no program holding nvidia: after its display modules are gone the
 * driver itself is unused, and the console handoff unloads it and loads it again. Whether a program
 * holds the driver on the boot path is not established, so both states have to work.
 */
static void test_activate_core_unused(void)
{
	char root[GS_PATH_MAX], *out, *log;
	static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };

	/* One boot display GPU. */
	act_host("act-display-core0", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	sim_core_unused();
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished") &&
	      contains(out, "firmware framebuffer simple-framebuffer.0 released"), "display GPU, driver unused: %s", out);
	log = sim_text("sim.log");
	CHECK(pos_of(log, "R rmmod nvidia_modeset") > 0 && pos_of(log, "R rmmod nvidia\n") > pos_of(log, "R rmmod nvidia_modeset") &&
	      pos_of(log, "R modprobe nvidia\n") > pos_of(log, "R rmmod nvidia\n") &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") > pos_of(log, "R rmmod nvidia\n") &&
	      pos_of(log, "simple-framebuffer.0/driver/unbind") < pos_of(log, "R modprobe nvidia\n"),
	      "nvidia is unloaded after its display modules, and loaded again after the framebuffer is released: %s", log);
	CHECK(sim_exists("module/nvidia") && sim_exists("module/nvidia_drm") && sim_exists("bus/pci/devices/0000:04:00.0/driver") &&
	      contains(out, "OK 0000:04:00.4 (VF of 0000:04:00.0)"), "the driver, the console and the VFs are up: %s", out);
	free(log);
	free(out);

	/* A plain GPU and the boot display GPU on one host. */
	act_host("act-mixed-core0", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_display, 1, NULL);
	set_bridge_windows(root, &topo_display, 18, 1440);
	sim_module("nvidia_drm");
	sim_core_unused();
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:03:00.0,0000:04:00.0\n");
	add_firmware_fb(root, &t4, "simple-framebuffer.0", "simple-framebuffer");
	add_iomem_fw(root, &t4, 1);
	gs_copy(sim.display_pf, sizeof(sim.display_pf), "0000:04:00.0");
	act_cfg.nbdfs = 2;
	gs_addr_parse("0000:03:00.0", &act_cfg.bdfs[0]);
	gs_addr_parse("0000:04:00.0", &act_cfg.bdfs[1]);
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "two cards, driver unused: %s", out);
	log = sim_text("sim.log");
	CHECK(count_of(log, "R rmmod nvidia\n") == 1 && count_of(log, "R modprobe nvidia\n") == 1 && count_of(log, "/rescan 1") == 2 &&
	      count_of(log, "sriov-manage -e") == 2, "one unload and one load of the driver, one rescan and one enable per card: %s", log);
	CHECK(contains(out, "OK 0000:03:00.4 (VF of 0000:03:00.0)") && contains(out, "OK 0000:04:00.4 (VF of 0000:04:00.0)"),
	      "both cards' VFs ready: %s", out);
	free(log);
	free(out);
}

/* A module load whose command fails: not loaded is a failure; loaded all the same is uncertain. */
static void test_activate_load_outcomes(void)
{
	char root[GS_PATH_MAX], *out, *log;

	enable_host("act-vfio-absent", root, sizeof(root));
	sim.run_fail = "modprobe nvidia_vgpu_vfio";
	CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "modprobe nvidia_vgpu_vfio exited with 1;") &&
	      !contains(out, "activation finished"), "the module is not loaded: %s", out);
	log = sim_text("sim.log");
	CHECK(log && !contains(log, "systemctl restart nvidia-vgpud.service") && !sim_exists("module/nvidia_vgpu_vfio"),
	      "no daemon is restarted without the module: %s", log ? log : "");
	free(log);
	free(out);

	enable_host("act-vfio-noisy", root, sizeof(root));
	sim.run_noisy = "modprobe nvidia_vgpu_vfio";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "but the module is loaded") &&
	      !contains(out, "activation finished"), "the command fails and the module is loaded: %s", out);
	log = sim_text("sim.log");
	CHECK(log && !contains(log, "systemctl restart nvidia-vgpud.service") && sim_exists("module/nvidia_vgpu_vfio") &&
	      sim_exists("state/" GS_ATTEMPT_NAME), "nothing goes on from an unknown load, and the guard stays: %s", log ? log : "");
	free(log);
	free(out);
}

/* What the VF enable needs from the host, and that activation asks before it changes anything. */
static void test_enable_prereq(void)
{
	char root[GS_PATH_MAX], p[GS_PATH_MAX], why[512] = "", *out;
	struct gs_config cfg;

	gs_config_defaults(&cfg);
	gs_pathf(p, sizeof(p), "%s/prereq/sriov-manage", tmp);
	gs_copy(cfg.sriov_manage, sizeof(cfg.sriov_manage), p);
	CHECK(gs_enable_prereq(&cfg, why, sizeof(why)) && contains(why, "install NVIDIA's vGPU host driver"),
	      "sriov-manage missing: %s", why);
	writef(p, "#!/bin/sh\n");
	chmod(p, 0644);
	CHECK(gs_enable_prereq(&cfg, why, sizeof(why)) && contains(why, "is not an executable file"), "not executable: %s", why);
	chmod(p, 0775);
	CHECK(gs_enable_prereq(&cfg, why, sizeof(why)) && contains(why, "is not root-owned or is group/world-writable"),
	      "not root's, or writable by others: %s", why);

	/* A host without sriov-manage is refused before the guard or any write. */
	act_host("act-prereq", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim.prereq_missing = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED && contains(out, "refused, nothing changed:") &&
	      contains(out, "install NVIDIA's vGPU host driver"), "activation without sriov-manage: %s", out);
	CHECK(!sim_exists("sim.log") && !sim_exists("state/" GS_ATTEMPT_NAME), "nothing was written and no guard was set");
	free(out);
	sim.prereq_missing = 0;
}

/* How often the simulated host's log holds text. */
static unsigned int log_count(const char *log, const char *text)
{
	unsigned int n = 0;

	for (const char *at = log; at && (at = strstr(at, text)); at += strlen(text))
		n++;
	return n;
}

static void sim_no_injection(void)
{
	sim.write_timeout = sim.write_timeout_value = sim.on_run = sim.on_write = sim.on_run_path = NULL;
	sim.on_run_write = sim.on_mkdir = NULL;
	sim.write_timeout_after = 0;
}

/* A bounded write that does not finish is uncertain state wherever it happens: exit 5, the guard set, no second try. */
static void test_activate_write_timeouts(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* The short path of a rerun: the vGPU type write of the first VF does not finish. */
	rerun_host("act-finish-ttimeout", "0\n", "mgr", root, sizeof(root));
	sim_no_injection();
	sim.write_timeout = "nvidia/current_vgpu_type";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "writing the vGPU type did not finish within") && contains(out, "The activation lock is set.") &&
	      !contains(out, "activation finished"), "type write timeout on the short path: %s", out);
	log = sim_text("sim.log");
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME) && sim_exists("run/" GS_ACTIVATING_NAME) &&
	      log_count(log, "nvidia/current_vgpu_type 525") == 1, "guard and record kept, one write only: %s", log ? log : "");
	free(log);
	free(out);

	/* The same for the power write of a VF. */
	rerun_host("act-finish-ptimeout", "0\n", "mgr", root, sizeof(root));
	sim_no_injection();
	sim_writef("bus/pci/devices/0000:03:00.4/power/control", "auto\n");
	sim.write_timeout = "0000:03:00.4/power/control";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "writing on to power/control did not finish within") && contains(out, "The activation lock is set."),
	      "power write timeout on the short path: %s", out);
	log = sim_text("sim.log");
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME) && sim_exists("run/" GS_ACTIVATING_NAME) &&
	      log_count(log, "0000:03:00.4/power/control on") == 1 && !contains(log, "0000:03:00.5/"),
	      "guard and record kept, one write, no later VF: %s", log ? log : "");
	free(log);
	free(out);

	/* The bind of a re-probe does not finish, although the driver link is already there. */
	enable_host("act-bind-timeout", root, sizeof(root));
	sim_no_injection();
	sim.write_timeout = "drivers/nvidia/bind";
	sim.write_timeout_after = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "the bind did not finish within") &&
	      !contains(out, "re-probed, bound to nvidia again") && !contains(out, "activation finished"),
	      "bind timeout with the driver link visible: %s", out);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME) && sim_exists("bus/pci/devices/0000:03:00.0/driver"),
	      "the guard is kept although the PF shows its driver");
	free(out);

	/* The rollback of a wake: giving the PF's power/control back does not finish. One try, exit 5, guard kept. */
	act_host("act-unwake-timeout", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_no_injection();
	sim.lock_denied = 1;
	sim.write_timeout = "0000:03:00.0/power/control";
	sim.write_timeout_value = "auto";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "restoring power/control=auto did not finish within") && contains(out, "the activation lock stays set"),
	      "rollback write timeout: %s", out);
	log = sim_text("sim.log");
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME) && log_count(log, "0000:03:00.0/power/control auto") == 1 &&
	      !contains(log, "/remove 1"), "guard kept, the restore is tried once, nothing removed: %s", log ? log : "");
	free(log);
	free(out);

	/* The guard cannot be updated right before a removal: NVIDIA's lock is not taken and nothing is removed. */
	act_host("act-guard-fail", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_no_injection();
	sim.on_write = "0000:03:00.1/driver/unbind";
	sim.on_run_path = "state/" GS_ATTEMPT_NAME;
	sim.on_mkdir = "1";
	CHECK(run_host(activate_host, &out) != GS_EXIT_OK && contains(out, "cannot update the activation lock") &&
	      contains(out, "nothing removed"), "guard update fails before a removal: %s", out);
	log = sim_text("sim.log");
	{
		char *lock = sim_text("proc/driver/nvidia/gpus/0000:03:00.0/unbindLock");

		CHECK(log && !contains(log, "unbindLock 1") && !contains(log, "/remove 1") && lock && lock[0] == '0' &&
		      contains(log, "drivers/snd_hda_intel/bind 0000:03:00.1"),
		      "the lock is not held, nothing is removed, the audio function is bound again: %s", log ? log : "");
		free(lock);
	}
	free(log);
	free(out);
	sim_no_injection();
}

/* What the driver step's result line tells install: whether the run changed a module or the plugin. */
static void test_driver_result(void)
{
	static const struct {
		const char *line;
		enum gs_driver_result want;
	} c[] = {
		{ "install_result=unchanged", GS_DRIVER_UNCHANGED },
		{ "install_result=changed", GS_DRIVER_CHANGED },
		{ "install_result=something-new", GS_DRIVER_CHANGED },
		/* A message is not the result, whatever it says. */
		{ "geforce-sriov-driver: already prepared for 6.8.0 (module abc); nothing to do", GS_DRIVER_CHANGED },
		{ "geforce-sriov-driver: NVIDIA's module and plugin are prepared", GS_DRIVER_CHANGED },
	};
	char script[GS_PATH_MAX], log[GS_PATH_MAX], why[256];
	const char *argv[] = { script, NULL };

	gs_pathf(script, sizeof(script), "%s/driver-result.sh", tmp);
	gs_pathf(log, sizeof(log), "%s/driver-result.log", tmp);
	for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		enum gs_driver_result got = (enum gs_driver_result)-1;

		writef(script, "#!/bin/sh\nprintf '%%s\\n' \"%s\"\n", c[i].line);
		chmod(script, 0755);
		CHECK(gs_driver_step(argv, log, why, sizeof(why), &got) == 0 && got == c[i].want, "driver result for: %s (got %d)",
		      c[i].line, (int)got);
	}
	/* A step that fails reports a change: nothing may be concluded from it. */
	writef(script, "#!/bin/sh\necho 'install_result=unchanged'\nexit 3\n");
	{
		enum gs_driver_result got = GS_DRIVER_UNCHANGED;

		CHECK(gs_driver_step(argv, log, why, sizeof(why), &got) == 3 && got == GS_DRIVER_CHANGED,
		      "a failing step reports a change (got %d)", (int)got);
	}
}

/* The read-only commands and the root check, through the built CLI. */
static void test_cli_smoke(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out;
	const char *devices[] = { "devices", NULL }, *uninstall[] = { "uninstall", NULL };
	const char *status[] = { "status", "--sysfs-root", root, "--config", cfg, NULL };

	CHECK(run_cli(&out, NULL, devices) == GS_EXIT_OK && contains(out, "10de:2204 Ampere  GA102  GeForce RTX 3090") &&
	      contains(out, "10de:2684 Ada     AD102  GeForce RTX 4090"), "devices lists the table: %s", out);
	free(out);
	CHECK(run_cli(&out, NULL, uninstall) == GS_EXIT_REFUSED && contains(out, "uninstall must run as root"),
	      "uninstall needs root: %s", out);
	free(out);
	fixture("smoke", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	CHECK(run_cli(&out, NULL, status) == GS_EXIT_FAIL && contains(out, "0 of 1 selected GPU(s) have every VF ready.") &&
	      contains(out, "next: ") && contains(out, "\nhost desktop: requested=auto\n"), "status on a host that is not activated: %s", out);
	free(out);
	{
		const char *gone[] = { "doctor", NULL };
		const char *dry_stage[] = { "install", "--dry-run", "--destdir", root, NULL };

		CHECK(run_cli(&out, NULL, gone) == GS_EXIT_USAGE && contains(out, "unknown command 'doctor'"),
		      "doctor is not a command: %s", out);
		free(out);
		CHECK(run_cli(&out, NULL, dry_stage) == GS_EXIT_USAGE && contains(out, "--dry-run checks this host"),
		      "install --dry-run takes no --destdir: %s", out);
	}
	free(out);
}

/* Exit 5 always leaves the guard set: a module load that did not finish is uncertain state. */
static void test_activate_exit5_guard(void)
{
	char root[GS_PATH_MAX], *out;

	/* The livepatch load times out before anything else was changed. */
	act_host("act-lp-timeout", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_remove("kernel/livepatch/geforce_sriov");
	sim_remove("module/geforce_sriov");
	sim_remove("module/nvidia");
	sim.run_timeout = "modprobe geforce_sriov";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE, "a livepatch load that times out is exit 5: %s", out);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME), "and the guard is still set");
	free(out);

	/* The display module load times out on the console card. */
	act_host("act-drm-timeout", &topo_default, gpu_ampere, 1, "0/4", root, sizeof(root));
	set_vf_bars(root, &topo_default, "0000:03:00.0", 1);
	sim_remove("module/nvidia_drm");
	sim.run_timeout = "nvidia_drm modeset=1";
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE &&
	      contains(out, "did not finish within"), "a display module load that times out is exit 5: %s", out);
	CHECK(sim_exists("state/" GS_ATTEMPT_NAME), "and the guard is still set");
	free(out);
}

/* activate on a host that is already active: nothing is restarted, re-probed or unloaded. */
static void test_activate_rerun(void)
{
	char root[GS_PATH_MAX], *out, *log;

	/* Every VF configured: no write, no command, no marker. */
	rerun_host("act-rerun-ready", "525\n", "nvidia_uvm", root, sizeof(root));
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "Every selected VF is already configured, so there is nothing to do.") &&
	      contains(out, "OK 0000:03:00.7 (VF of 0000:03:00.0)"), "rerun on a ready host: %s", out);
	log = sim_text("sim.log");
	CHECK(!log, "a ready host is not written to and no command runs: %s", log ? log : "");
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_ACTIVATING_NAME), "no guard and no record");
	free(log);
	free(out);
	act_opts.dry_run = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "Every selected VF is already configured, so there is nothing to do.") &&
	      !sim_text("sim.log"), "dry run on a ready host: %s", out);
	free(out);

	/* The types are gone (a daemon restart): only the types are written again. */
	rerun_host("act-rerun-untyped", "0\n", "mgr", root, sizeof(root));
	act_opts.dry_run = 1;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK &&
	      contains(out, "0000:03:00.0: VF 0: echo 525 > nvidia/current_vgpu_type") &&
	      !contains(out, "systemctl restart") && !contains(out, "unbindLock") && !sim_text("sim.log"),
	      "dry run with the types missing: %s", out);
	free(out);
	act_opts.dry_run = 0;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished") &&
	      contains(out, "0000:03:00.4: echo 525 > nvidia/current_vgpu_type") &&
	      contains(out, "OK 0000:03:00.7 (VF of 0000:03:00.0)"), "rerun with the types missing: %s", out);
	log = sim_text("sim.log");
	CHECK(count_of(log, "/nvidia/current_vgpu_type 525") == 4, "each type is written once: %s", log);
	CHECK(!contains(log, "R systemctl") && !contains(log, "R modprobe") && !contains(log, "unbindLock") &&
	      !contains(log, "/unbind") && !contains(log, "sriov-manage"),
	      "no daemon restart, no module change and no re-probe: %s", log);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_ACTIVATING_NAME),
	      "no guard, and the record is gone after the run");
	free(log);
	free(out);

	/*
	 * An earlier run that stopped half-way left its record: the short path does not apply, whoever
	 * holds the GPU. The full path then fails at the re-probe as it did on hardware.
	 */
	{
		static const char *const holders[] = { "mgr", "nvidia_uvm" };

		for (size_t k = 0; k < sizeof(holders) / sizeof(holders[0]); k++) {
			rerun_host("act-rerun-unfinished", "0\n", holders[k], root, sizeof(root));
			sim_remove("sim.log");
			sim_writef("run/" GS_ACTIVATING_NAME, "boot_id=unknown\npfs=0000:03:00.0\n");
			CHECK(run_host(activate_host, &out) == GS_EXIT_FAIL && contains(out, "did not grant unbindLock") &&
			      !contains(out, "already configured"), "an unfinished run takes the full path (%s): %s",
			      holders[k], out);
			log = sim_text("sim.log");
			CHECK(contains(log, "R systemctl restart nvidia-vgpu-mgr.service"),
			      "the full path restarts the daemons (%s): %s", holders[k], log);
			free(log);
			free(out);
		}
	}
}

/* Preflight refusals that change nothing. */
static void test_activate_refusals(void)
{
	char root[GS_PATH_MAX], *out;

	/* A guard from an unfinished attempt stops before anything. */
	act_host("act-guard", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=x\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_COLD_CYCLE && contains(out, "activation lock") &&
	      contains(out, "Recovery: do not rescan again"), "guard: %s", out);
	CHECK(!sim_text("sim.log"), "guard: nothing written");
	free(out);

	/* A card not in the device table. */
	act_host("act-unlisted", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("devices/pci0000:00/0000:00:02.0/0000:03:00.0/device", "0xdead\n");
	{
		int rc = run_host(activate_host, &out);

		CHECK(rc == GS_EXIT_REFUSED && contains(out, "0000:03:00.0 (10de:dead) is not in the device table") &&
		      contains(out, "refused, nothing changed"), "unlisted: rc=%d %s", rc, out);
		CHECK(!sim_text("sim.log"), "unlisted: nothing written");
	}
	free(out);

	/* Bound to a driver that is not nvidia. */
	act_host("act-driver", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_remove("bus/pci/devices/0000:03:00.0/driver");
	{
		char p[GS_PATH_MAX];

		gs_pathf(p, sizeof(p), "%s/devices/pci0000:00/0000:00:02.0/0000:03:00.0/driver", sim.root);
		link_to("../../../../bus/pci/drivers/xhci_hcd", p);
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "0000:03:00.0 is bound to xhci_hcd, not nvidia"), "driver: %s", out);
	free(out);

	/* More VFs active than the configured count. */
	act_host("act-manyvfs", &topo_default, gpu_ampere, 0, "5/4", root, sizeof(root));
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "0000:03:00.0 has 5 VFs of 4, more than the configured 4; disable them with NVIDIA's own "
			    "tool (`sriov-manage -d 0000:03:00.0`), then run activate again"), "too many VFs: %s", out);
	free(out);

	/* The livepatch is loaded with other parameters. */
	act_host("act-lp", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_writef("module/geforce_sriov/parameters/msix", "N\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "is loaded with ari=Y msix=N vfs=4 bdfs=0000:03:00.0, not the configured ari=Y msix=Y vfs=4") &&
	      contains(out, "power the machine off and on to change them"), "livepatch mismatch: %s", out);
	free(out);

	/* No livepatch and NVIDIA already loaded: the one reboot that fixes it. */
	act_host("act-nolp", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_remove("module/geforce_sriov");
	sim_remove("kernel/livepatch/geforce_sriov");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "the livepatch geforce_sriov is not loaded and NVIDIA's driver is") &&
	      contains(out, "power the machine off and on, and activation then runs with it in place"), "no livepatch: %s",
	      out);
	free(out);

	/* No livepatch support in the kernel at all. */
	act_host("act-nolpsupport", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_remove("module/geforce_sriov");
	sim_remove("module/nvidia");
	sim_remove("kernel/livepatch");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "the kernel has no livepatch support (CONFIG_LIVEPATCH)"), "no livepatch support: %s", out);
	free(out);

	/* A VF BAR state that cannot be read refuses before a removal. */
	act_host("act-nobars-state", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	sim_remove("devices/pci0000:00/0000:00:02.0/0000:03:00.0/resource");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "its VF BAR state is unreadable, so neither a removal nor an enable can be judged"),
	      "unreadable resources: %s", out);
	free(out);

	/* A vGPU on a VF stops the daemon restart: the VMs must go first. */
	act_host("act-hostvgpu", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	{
		static const struct fx_fn spare[] = { { "0000:05:00.0", "2206", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };
		static const struct fx_topo t5 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };

		make_card(root, &t5, spare, 0, "4/4");
		sim_enable_vfs("0000:05:00.0");
		sim_writef("bus/pci/devices/0000:05:00.4/nvidia/current_vgpu_type", "525\n");
	}
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "each VF below has a vGPU type set, and restarting NVIDIA's vGPU daemons would clear it") &&
	      contains(out, "VF 0000:05:00.4 has vGPU type 525") &&
	      contains(out, "echo 0 > /sys/bus/pci/devices/$f/nvidia/current_vgpu_type"), "host vGPU: %s", out);
	free(out);
}

/* ---- activate --clear-lock -------------------------------------------- */

/*
 * gs_cmd_activate's clear-lock branch uses the live paths (GS_STATE_DIR, gs_host_live) and
 * requires root, so it can only be exercised hermetically in a private mount namespace: tmpfs
 * over /run and /var/lib, uid 0 inside a user namespace mapped to this user.
 */
#define SKIP_ENV(what, ...) do { \
		skips++; \
		if (getenv("GS_TEST_REQUIRE_NS")) \
			failures++; \
		fprintf(stderr, "SKIP (environment: " what ") %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} while (0)

static int run_in_ns(const char *script, char **out)
{
	const char *argv[] = { "unshare", "-Urm", "/bin/sh", "-c", script, NULL };
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/* mount a private /run and /var/lib; then the case's own commands. */
#define NS_PREFIX \
	"mount -t tmpfs none /run || exit 90; " \
	"mount -t tmpfs none /var/lib || exit 91; " \
	"mkdir -p /run/geforce-sriov /var/lib/geforce-sriov || exit 92; "
#define GUARD "/var/lib/geforce-sriov/" GS_ATTEMPT_NAME

static void test_clear_guard(void)
{
	char script[4096], *out, *probe;

	/*
	 * Probe the exact capability the cases need (a private user+mount namespace whose tmpfs
	 * mounts succeed). Ubuntu restricts unprivileged user namespaces, so this is an environment
	 * skip there, not a test failure: the four cases below stay real wherever the probe passes.
	 */
	int ns = run_in_ns(NS_PREFIX "true", &probe);

	/* The helper reports a process status in every environment (0 here, 1/127 where restricted). */
	CHECK(ns == 0 || ns == 1 || ns == 127, "the namespace probe answers with a status: %d", ns);
	if (ns != 0) {
		SKIP_ENV("unprivileged user namespaces are unavailable; run as root or with "
			 "kernel.apparmor_restrict_unprivileged_userns=0 to cover this",
			 "%s", probe ? probe : "unshare failed");
		free(probe);
		return;
	}
	free(probe);

	/* No guard: idempotent, exit 0, nothing created. */
	snprintf(script, sizeof(script), NS_PREFIX "'%s' activate --clear-lock; echo RC=$?", cli);
	CHECK(run_in_ns(script, &out) == 0 &&
	      contains(out, "activation lock " GUARD " is not set") && contains(out, "RC=0"),
	      "no guard is not an error: %s", out);
	CHECK(!contains(out, "modprobe") && !contains(out, "sriov-manage") && !contains(out, "rescan"),
	      "no module or GPU operation: %s", out);
	free(out);

	/* A guard from another boot: printed, removed, nothing else touched; a plain activate passes it. */
	snprintf(script, sizeof(script),
		 NS_PREFIX
		 "printf 'boot_id=00000000-0000-0000-0000-000000000000\\nstep=activate\\n' > " GUARD "; "
		 "'%s' activate --clear-lock; rc=$?; echo CLEAR-RC=$rc; "
		 "if [ -e " GUARD " ]; then echo STILL-THERE; else echo GUARD-GONE; fi; "
		 "ls -A /var/lib/geforce-sriov | sed 's/^/LEFT:/'; "
		 "GEFORCE_SRIOV_CONFIG=/var/lib/no-config '%s' activate; echo ACT-RC=$?; "
		 "exit $rc",
		 cli, cli);
	CHECK(run_in_ns(script, &out) == 0 &&
	      contains(out, "activation lock " GUARD ":") && contains(out, "boot_id=00000000-0000-0000-0000-000000000000") &&
	      contains(out, "activation lock cleared. A changed or unavailable boot ID cannot establish that the machine was "
			    "powered off and on.") && contains(out, "CLEAR-RC=0") && contains(out, "GUARD-GONE") &&
	      !contains(out, "LEFT:") && contains(out, "ACT-RC=3") && !contains(out, "is set; nothing done"),
	      "guard cleared, nothing else touched, a later activate passes the guard: %s", out);
	CHECK(!contains(out, "modprobe") && !contains(out, "sriov-manage") && !contains(out, "rescan") &&
	      !contains(out, "W "), "no module or GPU operation: %s", out);
	free(out);

	/* A guard written in this boot cannot be cleared: exit 5, the guard stays, no other change. */
	snprintf(script, sizeof(script),
		 NS_PREFIX
		 "boot=$(cat /proc/sys/kernel/random/boot_id); printf 'boot_id=%%s\\n' \"$boot\" > " GUARD "; "
		 "'%s' activate --clear-lock; rc=$?; echo CLEAR-RC=$rc; "
		 "if [ -e " GUARD " ]; then echo STILL-THERE; fi; exit $rc",
		 cli);
	CHECK(run_in_ns(script, &out) == 5 &&
	      contains(out, "the unfinished attempt is from this boot, so the activation lock stays.") &&
	      contains(out, "Recovery: do not rescan again or restart in a loop.") &&
	      contains(out, "CLEAR-RC=5") && contains(out, "STILL-THERE") && !contains(out, "modprobe") &&
	      !contains(out, "sriov-manage"), "a guard from this boot stays: %s", out);
	free(out);

	/* The guard cannot be removed: a read-only state filesystem gives exit 1 and keeps it. */
	snprintf(script, sizeof(script),
		 NS_PREFIX
		 "printf 'boot_id=x\\n' > " GUARD "; "
		 "mount --bind /var/lib/geforce-sriov /var/lib/geforce-sriov && "
		 "mount -o remount,ro,bind /var/lib/geforce-sriov || exit 93; "
		 "'%s' activate --clear-lock; rc=$?; echo CLEAR-RC=$rc; "
		 "if [ -e " GUARD " ]; then echo STILL-THERE; fi; exit $rc",
		 cli);
	CHECK(run_in_ns(script, &out) == 1 &&
	      contains(out, "geforce-sriov: cannot remove " GUARD) && contains(out, "CLEAR-RC=1") &&
	      contains(out, "STILL-THERE") && !contains(out, "modprobe") && !contains(out, "sriov-manage"),
	      "an unremovable guard fails loudly: %s", out);
	free(out);

	/* uninstall of this version stops on another version's DKMS registration, and names the way out. */
	snprintf(script, sizeof(script),
		 NS_PREFIX "mkdir -p /var/lib/dkms/" GS_DKMS_NAME "/0.3.0; '%s' uninstall; echo RC=$?; "
		 "if [ -d /var/lib/dkms/" GS_DKMS_NAME "/0.3.0 ]; then echo DKMS-STILL-THERE; fi", cli);
	CHECK(run_in_ns(script, &out) == 0 && contains(out, "RC=3") && contains(out, "DKMS-STILL-THERE") &&
	      contains(out, "another version of geforce-sriov than " GS_VERSION " is installed") &&
	      contains(out, "dkms remove " GS_DKMS_NAME "/0.3.0 --all"),
	      "uninstall stops on another installed version and names the manual way out: %s", out);
	free(out);
}

static void test_version_policy(void)
{
	char line[512];

	CHECK(gs_version_confirmed(GS_VERSION_HOST, "580.159.01") && gs_version_confirmed(GS_VERSION_COMPANION, "580.159.03"),
	      "the tested versions");
	CHECK(!gs_version_confirmed(GS_VERSION_HOST, "580.159.03") &&
	      !gs_version_confirmed(GS_VERSION_COMPANION, "595.91.04") &&
	      !gs_version_confirmed(GS_VERSION_HOST, NULL) && !gs_version_confirmed(GS_VERSION_HOST, "580.159.1"),
	      "other versions are not confirmed");
	CHECK(gs_driver_version_line("580.159.01", line, sizeof(line)) == 0 &&
	      strcmp(line, "NVIDIA's driver 580.159.01 is confirmed to work.") == 0, "confirmed line: %s", line);
	CHECK(gs_driver_version_line("595.91.04", line, sizeof(line)) == 1 &&
	      strcmp(line, "NVIDIA's driver 595.91.04 is not guaranteed to work; confirmed versions: 580.159.01.") == 0,
	      "unsupported line: %s", line);
	gs_version_format(GS_VERSION_MSG_PROMPT, "595.91.04", line, sizeof(line));
	CHECK(strcmp(line, "NVIDIA's driver 595.91.04 is not guaranteed to work. Confirmed versions: 580.159.01. Continue? [y/N]") == 0,
	      "prompt: %s", line);
	gs_version_format(GS_VERSION_MSG_SETUP_REFUSAL, "595.91.04", line, sizeof(line));
	CHECK(strcmp(line, "setup: NVIDIA's driver 595.91.04 is not guaranteed to work. Confirmed versions: 580.159.01. Nothing changed. To continue without questions, pass --allow-unsupported-driver.") == 0,
	      "setup refusal: %s", line);
	gs_version_format(GS_VERSION_MSG_INSTALL_NOTICE, "595.91.04", line, sizeof(line));
	CHECK(strcmp(line, "geforce-sriov: NVIDIA's driver 595.91.04 is not guaranteed to work. Confirmed versions: 580.159.01. Continuing uses best effort; input and safety checks still apply.") == 0,
	      "install notice: %s", line);
	gs_version_format(GS_VERSION_MSG_BEST_EFFORT, "595.91.04", line, sizeof(line));
	CHECK(strcmp(line, "Continuing uses best effort; input and safety checks still apply. Default is no.") == 0,
	      "best effort: %s", line);
	gs_version_format(GS_VERSION_MSG_OPEN_REFUSAL, NULL, line, sizeof(line));
	CHECK(contains(line, "NVIDIA's open flavour cannot be used by this build") &&
	      contains(line, "--allow-unsupported-driver does not override this check"), "open refusal: %s", line);

	/* The built-in rows are the blocks of the table the driver step reads. */
	{
		size_t count;
		const struct gs_confirmed_version *rows = gs_confirmed_versions(&count);
		char path[GS_PATH_MAX], want[256], *table;

		gs_pathf(path, sizeof(path), "%s/scripts/gs-driver-versions.tbl", srcdir);
		table = slurp(path);

		CHECK(count >= 1 && table, "the table of confirmed versions is read: %zu rows", count);
		for (size_t i = 0; table && i < count; i++) {
			snprintf(want, sizeof(want), "\nhost=%s\n", rows[i].host);
			CHECK(contains(table, want), "row %zu is a block of the table: %s", i, rows[i].host);
			snprintf(want, sizeof(want), "\npatched_plugin=%s\n", rows[i].patched_plugin ? rows[i].patched_plugin : "?");
			CHECK(contains(table, want), "row %zu has the table's patched plugin digest", i);
			snprintf(want, sizeof(want), "\npackage=%s\n", rows[i].package ? rows[i].package : "?");
			CHECK(contains(table, want), "row %zu has the table's package digest", i);
		}
		free(table);
		CHECK(strcmp(gs_default_host_version(), rows[0].host) == 0, "the first row is the default");
	}

	/*
	 * A second confirmed version is a second row: it is confirmed, every message lists both, and
	 * its files are judged by its own digests. The first row is the table's own.
	 */
	{
		struct gs_confirmed_version two[2];
		size_t count;
		char why[1024];
		const char *copy = "/usr/lib/libnvidia-vgpu.so.595.91.04.geforce-sriov";

		two[0] = gs_confirmed_versions(&count)[0];
		two[1] = (struct gs_confirmed_version){
			.host = "595.91.04", .second = "595.91.05",
			.guest_ampere = "Linux 595.91.05, Windows 596.10", .guest_ada = "Linux 595.91.05, Windows 596.10",
			.stock_plugin = "1111111111111111111111111111111111111111111111111111111111111111",
			.mixed_plugin = "2222222222222222222222222222222222222222222222222222222222222222",
			.patched_plugin = "3333333333333333333333333333333333333333333333333333333333333333",
		};
		CHECK(gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", copy, two[1].patched_plugin, why, sizeof(why)) == -1 &&
		      contains(why, "not the patched plugin copy of NVIDIA's 580.159.01"),
		      "a digest of no row is not accepted: %s", why);
		gs_confirmed_versions_set(two, 2);
		CHECK(gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", copy, two[1].patched_plugin, why, sizeof(why)) == 0 &&
		      gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", copy, two[0].patched_plugin, why, sizeof(why)) == 0,
		      "the patched plugin copy of each row is accepted");
		CHECK(gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", copy, two[1].stock_plugin, why, sizeof(why)) == -1 &&
		      contains(why, "is NVIDIA's unchanged 595.91.04 plugin") && contains(why, two[1].patched_plugin),
		      "the second row's stock plugin is named with its version and its patched digest: %s", why);
		CHECK(gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", copy, two[1].mixed_plugin, why, sizeof(why)) == -1 &&
		      contains(why, "is an earlier copy") && contains(why, two[1].patched_plugin),
		      "the second row's earlier copy: %s", why);
		CHECK(gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", copy,
					"4444444444444444444444444444444444444444444444444444444444444444", why, sizeof(why)) == -1 &&
		      contains(why, "not the patched plugin copy of NVIDIA's 595.91.04") && contains(why, two[1].patched_plugin),
		      "another digest is judged by the version the file name states: %s", why);
		CHECK(gs_plugin_verdict("/usr/lib/libnvidia-vgpu.so", "/usr/lib/libnvidia-vgpu.so.580.159.01", two[0].stock_plugin,
					why, sizeof(why)) == -1 &&
		      contains(why, "is NVIDIA's unchanged 580.159.01 plugin") && contains(why, two[0].patched_plugin),
		      "the first row's stock plugin: %s", why);
		CHECK(gs_version_confirmed(GS_VERSION_HOST, "595.91.04") && gs_version_confirmed(GS_VERSION_HOST, "580.159.01") &&
		      gs_version_confirmed(GS_VERSION_COMPANION, "595.91.05") && !gs_version_confirmed(GS_VERSION_HOST, "600.1.2"),
		      "both rows are confirmed, another version is not");
		CHECK(gs_driver_version_line("595.91.04", line, sizeof(line)) == 0 &&
		      strcmp(line, "NVIDIA's driver 595.91.04 is confirmed to work.") == 0, "the second row's line: %s", line);
		gs_version_format(GS_VERSION_MSG_SETUP_REFUSAL, "600.1.2", line, sizeof(line));
		CHECK(contains(line, "NVIDIA's driver 600.1.2 is not guaranteed to work. Confirmed versions: 580.159.01, 595.91.04."),
		      "a refusal lists both: %s", line);
		gs_confirmed_versions_set(NULL, 0);
		CHECK(!gs_version_confirmed(GS_VERSION_HOST, "595.91.04"), "the built-in table is back");
	}
}

/* A driver-build record: fields are lines "key=value"; only the ones a test names are written. */
static void build_record(const char *path, const char *const lines[])
{
	FILE *f = fopen(path, "w");

	if (!f)
		die(path);
	for (size_t i = 0; lines[i]; i++)
		fprintf(f, "%s\n", lines[i]);
	fclose(f);
}

static void test_version_record(void)
{
	char record[GS_PATH_MAX], text[512];
	struct gs_build b;

	gs_pathf(record, sizeof(record), "%s/driver-build", tmp);
	{
		static const char *const lines[] = { "kernel=6.8.0", "driver_version=580.159.01",
						     "driver_version_confirmed=1", "display=1", "host_cuda=0", NULL };

		build_record(record, lines);
	}
	CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT &&
	      b.record_read && b.driver_version_confirmed == 1 && b.display == 1 && b.host_cuda == 0 &&
	      strcmp(b.driver_version, "580.159.01") == 0, "confirmed record");
	{
		static const char *const lines[] = { "kernel=6.8.0", "driver_version=595.91.04",
						     "driver_version_confirmed=0", NULL };

		build_record(record, lines);
	}
	CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT && b.driver_version_confirmed == 0 &&
	      b.display == -1 && b.host_cuda == -1, "unconfirmed record, features not recorded");
	{
		static const char *const lines[] = { "kernel=6.8.0", "driver_version=595.91.04", NULL };

		build_record(record, lines);
	}
	CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT && b.driver_version_confirmed == -1,
	      "an old record without the field is not assumed confirmed");
	gs_version_format(gs_version_confirmed(GS_VERSION_HOST, b.driver_version) ? GS_VERSION_MSG_CONFIRMED :
										GS_VERSION_MSG_STATUS,
			  b.driver_version, text, sizeof(text));
	CHECK(contains(text, "595.91.04 is not guaranteed to work"), "recomputed policy: %s", text);
	{
		static const char *const lines[] = { "kernel=6.8.0", NULL };

		build_record(record, lines);
	}
	CHECK(gs_driver_build_read(record, "6.9.0", NULL, &b) == GS_BUILD_ABSENT && b.record_read &&
	      !b.driver_version[0], "a record for another kernel names nothing");
	/* The record of a kernel the driver step built for earlier is kept beside the published one. */
	{
		static const char *const lines[] = { "kernel=6.9.0", "module_build_id=abc", "driver_version=580.159.01",
						     "driver_version_confirmed=1", "display=1", "host_cuda=1", NULL };
		char other[GS_PATH_MAX];

		gs_pathf(other, sizeof(other), "%s/" GS_DRIVER_BUILDS_NAME, tmp);
		gs_mkdir_p(other, 0755);
		gs_pathf(other, sizeof(other), "%s/" GS_DRIVER_BUILDS_NAME "/6.9.0", tmp);
		build_record(other, lines);
		CHECK(gs_driver_build_read(record, "6.9.0", NULL, &b) == GS_BUILD_PRESENT && b.record_read &&
		      strcmp(b.rec_kernel, "6.9.0") == 0 && strcmp(b.rec_id, "abc") == 0 && b.display == 1 && b.host_cuda == 1 &&
		      strcmp(b.record, other) == 0, "the record kept for another kernel is read: %s", b.record);
		CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT && strcmp(b.record, record) == 0,
		      "the published record still answers for its own kernel");
		CHECK(gs_driver_build_read(record, "6.7.0", NULL, &b) == GS_BUILD_ABSENT && strcmp(b.record, record) == 0,
		      "a kernel with no record of its own has none");
		unlink(other);
	}
	gs_pathf(record, sizeof(record), "%s/no-record", tmp);
	unlink(record);
	CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_ABSENT && !b.record_read,
	      "no record at all");
}

/*
 * gs_setup on a fixture host: the steps are logged by setup_run and never run. install writes the
 * config the test names (a staged install would, in the real binary); install --dry-run/activate/check return
 * setup_rc.
 */
static char setup_log[GS_PATH_MAX];
static const char *setup_write_config;

/*
 * rebuild makes sure that the livepatch module is installed for the kernel it builds NVIDIA's module
 * for. DKMS is the fixture here: what `dkms status` prints, and whether `dkms install` succeeds.
 */
static const char *dkms_status_text;
static int dkms_status_rc, dkms_install_rc, dkms_managed;
static const char *dkms_release, *dkms_named;
static int dkms_fake_capture(const char *const argv[], char *out, size_t len)
{
	printf("C");
	for (unsigned int i = 0; argv[i]; i++)
		printf(" %s", argv[i]);
	printf("\n");
	snprintf(out, len, "%s", dkms_status_text);
	return dkms_status_rc;
}
static int dkms_fake_run(const char *const argv[])
{
	printf("R");
	for (unsigned int i = 0; argv[i]; i++)
		printf(" %s", argv[i]);
	printf("\n");
	return dkms_install_rc;
}
static int dkms_ensure_host(void)
{
	return gs_dkms_ensure(dkms_fake_run, dkms_fake_capture, dkms_managed, dkms_release, dkms_named);
}
static const char *dkms_other_root, *dkms_other_state;
static int dkms_other_host(void)
{
	gs_dkms_other_kernels(dkms_fake_run, dkms_other_root, dkms_other_state);
	return 0;
}
/* One more installed kernel that NVIDIA's module was built for. */
static void writef_kernel(const char *root, const char *state, const char *release)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/driver-builds/%s", state, release);
	writef(p, "kernel=%s\n", release);
	gs_pathf(p, sizeof(p), "%s/lib/modules/%s/kernel", root, release);
	gs_mkdir_p(p, 0755);
}
static void test_dkms_ensure(void)
{
	char root[GS_PATH_MAX], state[GS_PATH_MAX], p[GS_PATH_MAX], list[256], *out;
	static const char install_cmd[] = "R dkms install -m geforce-sriov -v " GS_VERSION " -k 7.0.14-12-pve\n";

	dkms_managed = 1;
	dkms_release = "7.0.14-12-pve";
	dkms_named = "7.0.14-12-pve";
	dkms_status_rc = dkms_install_rc = 0;

	/* A kernel that was installed before geforce-sriov: DKMS has the module for the other kernel only. It is built. */
	dkms_status_text = "";
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_OK &&
	      contains(out, "C dkms status -m geforce-sriov -v " GS_VERSION " -k 7.0.14-12-pve\n") && contains(out, install_cmd) &&
	      contains(out, "The livepatch module geforce_sriov is not installed for 7.0.14-12-pve: dkms install -m geforce-sriov -v " GS_VERSION " -k 7.0.14-12-pve\n") &&
	      contains(out, "The livepatch module geforce_sriov is now installed for 7.0.14-12-pve.\n"), "missing for the kernel: installed: %s", out);
	free(out);
	/* Built but not installed, and a kernel whose name only begins like the one asked for, do not count. */
	dkms_status_text = "geforce-sriov/" GS_VERSION ", 7.0.14-12-pve, x86_64: built\n"
			   "geforce-sriov/" GS_VERSION ", 7.0.14-12-pve-extra, x86_64: installed\n";
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_OK && contains(out, install_cmd), "built only, or another kernel: installed: %s", out);
	free(out);
	/* A status that cannot be read is no reason to skip the install, which DKMS repeats without harm. */
	dkms_status_text = "geforce-sriov/" GS_VERSION ", 7.0.14-12-pve, x86_64: installed\n";
	dkms_status_rc = 1;
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_OK && contains(out, install_cmd), "status fails: installed all the same: %s", out);
	free(out);
	dkms_status_rc = 0;

	/* Present: nothing is run. Both layouts of `dkms status`. */
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_OK && !contains(out, "R dkms") &&
	      contains(out, "The livepatch module geforce_sriov is installed for 7.0.14-12-pve.\n"), "present: untouched: %s", out);
	free(out);
	dkms_status_text = "geforce-sriov, " GS_VERSION ", 7.0.14-16-pve, x86_64: installed\n"
			   "geforce-sriov, " GS_VERSION ", 7.0.14-12-pve, x86_64: installed (original_module exists)\n";
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_OK && !contains(out, "R dkms"), "present, the older layout: untouched: %s", out);
	free(out);

	/* DKMS fails (no headers for that kernel, for example): refused, with the command that failed and the one to repeat. */
	dkms_status_text = "geforce-sriov/" GS_VERSION ", 7.0.14-16-pve, x86_64: installed\n";
	dkms_install_rc = 1;
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_FAIL &&
	      contains(out, "`dkms install -m geforce-sriov -v " GS_VERSION " -k 7.0.14-12-pve` failed (DKMS's output is above)") &&
	      contains(out, "geforce_sriov is not installed for 7.0.14-12-pve, and that kernel would boot with no VFs") &&
	      contains(out, "The usual cause is that the headers of that kernel are not installed") &&
	      contains(out, "run `sudo geforce-sriov rebuild --kernel 7.0.14-12-pve` again"), "dkms fails: %s", out);
	free(out);
	dkms_named = NULL;
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_FAIL && contains(out, "run `sudo geforce-sriov rebuild` again"),
	      "dkms fails for the running kernel: the command has no --kernel: %s", out);
	free(out);
	dkms_install_rc = 0;

	/* A host where DKMS does not hold the module (a packaged install): a warning, nothing is run, rebuild goes on. */
	dkms_managed = 0;
	CHECK(run_host(dkms_ensure_host, &out) == GS_EXIT_OK && !contains(out, "C dkms") && !contains(out, "R dkms") &&
	      contains(out, "warning: DKMS does not hold geforce-sriov/" GS_VERSION " on this host"), "not managed by DKMS: %s", out);
	free(out);

	/* The host checks: the kernels NVIDIA's module was built for that are installed and have no livepatch module. */
	gs_pathf(root, sizeof(root), "%s/dkms-root", tmp);
	gs_pathf(state, sizeof(state), "%s/var/lib/geforce-sriov", root);
	gs_pathf(p, sizeof(p), "%s/driver-builds", state);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/driver-builds/7.0.14-12-pve", state);
	writef(p, "kernel=a\n");
	gs_pathf(p, sizeof(p), "%s/driver-builds/7.0.14-16-pve", state);
	writef(p, "kernel=b\n");
	gs_pathf(p, sizeof(p), "%s/driver-builds/6.8.0-removed", state);
	writef(p, "kernel=c\n");
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.14-12-pve/kernel", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.14-16-pve/kernel", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.14-16-pve/updates/dkms", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.14-16-pve/updates/dkms/geforce_sriov.ko.zst", root);
	writef(p, "module\n");
	CHECK(gs_kernels_without_livepatch(root, state, list, sizeof(list)) == 1 && strcmp(list, "7.0.14-12-pve") == 0,
	      "the kernel without a livepatch module is named, a kernel that is gone is not: %s", list);
	CHECK(gs_module_present_for(root, "7.0.14-16-pve") && !gs_module_present_for(root, "7.0.14-12-pve"), "the module of each kernel");

	/*
	 * install: its DKMS step removes this version for every kernel and builds it again for the running
	 * one. The other installed kernels with a build of NVIDIA's module get the livepatch module back;
	 * one that is gone, and the running one, which has it, are left alone.
	 */
	dkms_install_rc = 0;
	dkms_other_root = root;
	dkms_other_state = state;
	writef_kernel(root, state, "7.0.14-9-pve");
	CHECK(run_host(dkms_other_host, &out) == 0 &&
	      contains(out, "R dkms install -m geforce-sriov -v " GS_VERSION " -k 7.0.14-12-pve\n") &&
	      contains(out, "R dkms install -m geforce-sriov -v " GS_VERSION " -k 7.0.14-9-pve\n") &&
	      !contains(out, "-k 7.0.14-16-pve") && !contains(out, "-k 6.8.0-removed") &&
	      contains(out, "NVIDIA's module is also built for 7.0.14-12-pve, so the livepatch module geforce_sriov is installed for it too") &&
	      !contains(out, "warning"), "install: every other kernel with a build keeps the livepatch module: %s", out);
	free(out);
	/* The published record names the kernel of the latest build, which need not be the running one. */
	gs_pathf(p, sizeof(p), "%s/driver-build", state);
	writef(p, "kernel=7.0.14-20-pve\nmodule_sha256=0\n");
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.14-20-pve/kernel", root);
	gs_mkdir_p(p, 0755);
	CHECK(gs_kernels_without_livepatch(root, state, list, sizeof(list)) == 3 && contains(list, "7.0.14-20-pve") &&
	      contains(list, "7.0.14-12-pve") && contains(list, "7.0.14-9-pve"), "the kernel of the published record counts too: %s", list);
	dkms_install_rc = 1;
	CHECK(run_host(dkms_other_host, &out) == 0 &&
	      contains(out, "warning: `dkms install -m geforce-sriov -v " GS_VERSION " -k 7.0.14-12-pve` failed") &&
	      contains(out, "`sudo geforce-sriov rebuild --kernel 7.0.14-12-pve`") &&
	      contains(out, "-k 7.0.14-9-pve"), "install: a kernel DKMS cannot build for is a warning, and the next one is still tried: %s", out);
	free(out);
	dkms_install_rc = 0;
}

/*
 * NVIDIA's CUDA libraries for host CUDA. The driver step leaves them in a staging directory with an
 * index; install puts them in a directory of their own with the links the loader follows and the
 * file that names the directory to it, all in the install manifest. Here the staging directory is
 * made by hand, and ldconfig is a fixture: what it is run with, and what `ldconfig -p` answers.
 */
static char cu_root[GS_PATH_MAX], cu_manifest[GS_PATH_MAX], cu_dir[GS_PATH_MAX], cu_conf[GS_PATH_MAX], cu_stage[GS_PATH_MAX];
static char cu_loader[3 * GS_PATH_MAX];
static int cu_want;
static int cu_run(const char *const argv[])
{
	printf("R");
	for (unsigned int i = 0; argv[i]; i++)
		printf(" %s", argv[i]);
	printf("\n");
	fflush(stdout);
	return strcmp(argv[0], "rm") == 0 ? gs_run(argv) : 0;
}
static int cu_capture(const char *const argv[], char *out, size_t len)
{
	(void)argv;
	snprintf(out, len, "%s", cu_loader);
	return 0;
}
static const struct gs_nvlibs_io cu_io = { cu_run, cu_capture };
static int cu_apply(void)
{
	const struct gs_nvdest d = { cu_dir, cu_conf, cu_stage, NULL };

	return gs_nvlibs_apply(&gs_nvset_cuda, cu_manifest, &d, cu_want, &cu_io);
}
static int cu_uninstall(void)
{
	return gs_uninstall_files(cu_manifest, cu_root, NULL);
}
/* What the driver step leaves: three libraries of one version, each with its digest and the name the loader asks for. */
static void cu_stage_make(const char *version)
{
	static const char *const libs[][2] = { { "libcuda", "libcuda.so.1" }, { "libnvidia-ptxjitcompiler", "libnvidia-ptxjitcompiler.so.1" },
					       { "libnvidia-nvvm", "libnvidia-nvvm.so.4" } };
	char p[GS_PATH_MAX], index[4096] = "", sha[65];
	size_t at;

	gs_mkdir_p(cu_stage, 0755);
	at = (size_t)snprintf(index, sizeof(index), "version=%s\n", version);
	for (unsigned int i = 0; i < 3; i++) {
		gs_pathf(p, sizeof(p), "%s/%s.so.%s", cu_stage, libs[i][0], version);
		writef(p, "%s of %s\n", libs[i][0], version);
		gs_sha256_file(p, sha);
		at += (size_t)snprintf(index + at, sizeof(index) - at, "lib=%s.so.%s %s %s\n", libs[i][0], version, sha, libs[i][1]);
	}
	gs_pathf(p, sizeof(p), "%s/index", cu_stage);
	writef(p, "%s", index);
}
static int cu_link_is(const char *name, const char *to)
{
	char p[GS_PATH_MAX], have[128];
	ssize_t n;

	gs_pathf(p, sizeof(p), "%s/%s", cu_dir, name);
	n = readlink(p, have, sizeof(have) - 1);
	return n > 0 && (have[n] = '\0', strcmp(have, to) == 0);
}
static int cu_has(const char *name)
{
	char p[GS_PATH_MAX];

	return !gs_pathf(p, sizeof(p), "%s/%s", cu_dir, name) && gs_exists(p);
}
static void test_cuda_libs(void)
{
	char p[GS_PATH_MAX], foreign[GS_PATH_MAX], line[2 * GS_PATH_MAX], dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX];
	char want_line[2 * GS_PATH_MAX], *out, *text;

	gs_pathf(cu_root, sizeof(cu_root), "%s/cuda-libs", tmp);
	gs_pathf(cu_manifest, sizeof(cu_manifest), "%s/state/installed-files", cu_root);
	gs_pathf(cu_dir, sizeof(cu_dir), "%s/usr/lib/geforce-sriov/cuda", cu_root);
	gs_pathf(cu_conf, sizeof(cu_conf), "%s/etc/ld.so.conf.d/geforce-sriov-cuda.conf", cu_root);
	gs_pathf(cu_stage, sizeof(cu_stage), "%s/state/host-cuda", cu_root);
	gs_pathf(p, sizeof(p), "%s/state", cu_root);
	gs_mkdir_p(p, 0755);
	/* A libcuda of another package in the system's library directory: it is never touched. */
	gs_pathf(foreign, sizeof(foreign), "%s/usr/lib/x86_64-linux-gnu/libcuda.so.1", cu_root);
	gs_pathf(p, sizeof(p), "%s/usr/lib/x86_64-linux-gnu", cu_root);
	gs_mkdir_p(p, 0755);
	writef(foreign, "another libcuda\n");
	snprintf(cu_loader, sizeof(cu_loader), "\tlibcuda.so.1 (libc6,x86-64) => %s/libcuda.so.1\n", cu_dir);

	/* Host CUDA is on and nothing was handed over: one warning, nothing installed. */
	cu_want = 1;
	CHECK(run_host(cu_apply, &out) == 0 && !cu_has("libcuda.so.1") &&
	      contains(out, "warning: host CUDA is on, but NVIDIA's CUDA library (libcuda) is not installed, so CUDA\n"
			    "programs on the host cannot start.") &&
	      contains(out, "Run `sudo ./geforce-sriov install --host-cuda --companion-file PATH` in the source folder with that package."),
	      "nothing staged, nothing installed: %s", out);
	free(out);
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 1, NULL, line, sizeof(line)) == -1 && contains(line, "not installed, so CUDA programs on the host cannot start"),
	      "status says so: %s", line);
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 0, NULL, line, sizeof(line)) == 0 && !line[0], "and nothing when host CUDA is off");

	/* An index that does not fit its files is not installed from. */
	cu_stage_make("580.159.03");
	gs_pathf(p, sizeof(p), "%s/libcuda.so.580.159.03", cu_stage);
	writef(p, "not the file the index names\n");
	CHECK(run_host(cu_apply, &out) == 0 && !cu_has("libcuda.so.580.159.03") &&
	      contains(out, "does not hold what the driver step recorded there; NVIDIA's CUDA libraries\nare not installed from it."),
	      "a staging directory that was changed: %s", out);
	free(out);

	/* Install: the libraries, the links, the loader's file, ldconfig; the staging directory is used up. */
	cu_stage_make("580.159.03");
	CHECK(run_host(cu_apply, &out) == 0 && cu_has("libcuda.so.580.159.03") && cu_has("libnvidia-nvvm.so.580.159.03") &&
	      cu_has("libnvidia-ptxjitcompiler.so.580.159.03"), "the libraries are installed: %s", out);
	CHECK(cu_link_is("libcuda.so.1", "libcuda.so.580.159.03") && cu_link_is("libcuda.so", "libcuda.so.1") &&
	      cu_link_is("libnvidia-nvvm.so.4", "libnvidia-nvvm.so.580.159.03") &&
	      cu_link_is("libnvidia-ptxjitcompiler.so.1", "libnvidia-ptxjitcompiler.so.580.159.03"), "the links the loader follows: %s", out);
	text = slurp(cu_conf);
	snprintf(want_line, sizeof(want_line), "%s\n", cu_dir);
	CHECK(text && strcmp(text, want_line) == 0, "the loader's file names the directory: %s", text ? text : "(none)");
	free(text);
	snprintf(want_line, sizeof(want_line), "host CUDA: NVIDIA's CUDA libraries 580.159.03 are installed in %s\n", cu_dir);
	CHECK(contains(out, want_line) && contains(out, "R ldconfig\n") && !gs_exists(cu_stage) && !contains(out, "warning"),
	      "said, ldconfig is run, the staging directory is gone: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/libcuda.so.580.159.03", cu_dir);
	{
		struct stat st;

		CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0755, "a library is executable, as NVIDIA's installer leaves it");
	}
	text = slurp(cu_manifest);
	CHECK(text && contains(text, "l libcuda.so.580.159.03 ") && contains(text, "/cuda/libcuda.so.1\n") && contains(text, cu_conf),
	      "the manifest lists the files, the links and the loader's file: %s", text ? text : "(none)");
	free(text);
	CHECK(gs_nvlibs_state(&gs_nvset_cuda, cu_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == 1 &&
	      strcmp(dir, cu_dir) == 0 && strcmp(version, "580.159.03") == 0, "the manifest says what is installed: %s %s", dir, version);
	snprintf(want_line, sizeof(want_line), "libcuda 580.159.03 is installed in %s", cu_dir);
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 1, cu_capture, line, sizeof(line)) == 1 && strcmp(line, want_line) == 0, "status and doctor: %s", line);

	/* Again, with nothing handed over: nothing is written and ldconfig is not run. */
	CHECK(run_host(cu_apply, &out) == 0 && !contains(out, "R ") && !contains(out, "installed") && !contains(out, "warning"),
	      "a rerun changes nothing: %s", out);
	free(out);
	/* Again, with the same libraries handed over: every file is as it was. */
	cu_stage_make("580.159.03");
	CHECK(run_host(cu_apply, &out) == 0 && contains(out, "unchanged ") && !contains(out, "R ldconfig") && !contains(out, "linked ") &&
	      !gs_exists(cu_stage), "the same libraries again: %s", out);
	free(out);

	/* The loader finds another libcuda first: said, and that file is left alone. */
	snprintf(cu_loader, sizeof(cu_loader), "\tlibcuda.so.1 (libc6,x86-64) => %s\n\tlibcuda.so.1 (libc6,x86-64) => %s/libcuda.so.1\n", foreign, cu_dir);
	CHECK(run_host(cu_apply, &out) == 0 && contains(out, "warning: programs on this host load another CUDA library first: ") &&
	      contains(out, foreign) && contains(out, "geforce-sriov did\nnot install it and leaves it as it is."), "another libcuda comes first: %s", out);
	free(out);
	text = slurp(foreign);
	CHECK(text && strcmp(text, "another libcuda\n") == 0, "the other libcuda is untouched");
	free(text);
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 1, cu_capture, line, sizeof(line)) == -1 && contains(line, "but programs load another one first: ") &&
	      contains(line, foreign), "doctor says so: %s", line);
	/* A 32-bit entry before it is not the one a 64-bit program gets. */
	snprintf(cu_loader, sizeof(cu_loader), "\tlibcuda.so.1 (libc6) => /usr/lib32/libcuda.so.1\n\tlibcuda.so.1 (libc6,x86-64) => %s/libcuda.so.1\n", cu_dir);
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 1, cu_capture, line, sizeof(line)) == 1, "a 32-bit libcuda does not count: %s", line);

	/* A second package of another version: its libraries replace the installed ones. */
	cu_stage_make("580.159.05");
	CHECK(run_host(cu_apply, &out) == 0 && cu_has("libcuda.so.580.159.05") && !cu_has("libcuda.so.580.159.03") &&
	      !cu_has("libnvidia-nvvm.so.580.159.03") && cu_link_is("libcuda.so.1", "libcuda.so.580.159.05") &&
	      contains(out, "libcuda.so.580.159.03: it belongs to another version of NVIDIA's CUDA libraries") && contains(out, "R ldconfig\n"),
	      "another version replaces the installed one: %s", out);
	free(out);

	/* A library that was changed since install: incomplete, and said. */
	gs_pathf(p, sizeof(p), "%s/libnvidia-nvvm.so.580.159.05", cu_dir);
	writef(p, "changed\n");
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 1, NULL, line, sizeof(line)) == -1 && contains(line, "incomplete: ") && contains(line, p),
	      "a changed library: %s", line);
	CHECK(run_host(cu_apply, &out) == 0 && contains(out, "warning: NVIDIA's CUDA libraries are incomplete: ") && contains(out, p),
	      "install says so too: %s", out);
	free(out);

	/* Host CUDA off: what this package installed goes, what was changed stays, the other libcuda stays. */
	cu_want = 0;
	CHECK(gs_nvlibs_line(&gs_nvset_cuda, cu_manifest, 0, NULL, line, sizeof(line)) == 2 && contains(line, "although host_cuda=off"), "off, still installed: %s", line);
	CHECK(run_host(cu_apply, &out) == 0 && !cu_has("libcuda.so.580.159.05") && !cu_has("libcuda.so.1") && !cu_has("libcuda.so") &&
	      !gs_exists(cu_conf) && gs_exists(p) && gs_exists(foreign) && contains(out, ": host CUDA is off\n") &&
	      contains(out, "(changed since install)") && contains(out, "R ldconfig\n"), "host CUDA off removes them: %s", out);
	free(out);
	unlink(p);
	CHECK(run_host(cu_apply, &out) == 0 && !gs_exists(cu_dir) && !contains(out, "R ldconfig"), "off again: the directory goes, nothing else: %s", out);
	free(out);

	/* Uninstall removes exactly what was installed: the files while unchanged, the links while they are the ones made. */
	cu_want = 1;
	snprintf(cu_loader, sizeof(cu_loader), "\tlibcuda.so.1 (libc6,x86-64) => %s/libcuda.so.1\n", cu_dir);
	cu_stage_make("580.159.03");
	CHECK(run_host(cu_apply, &out) == 0 && cu_has("libcuda.so.1"), "installed again: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/libcuda.so", cu_dir);
	unlink(p);
	CHECK(symlink("somewhere-else", p) == 0, "a link that the user changed");
	CHECK(run_host(cu_uninstall, &out) == GS_EXIT_OK && !cu_has("libcuda.so.580.159.03") && !cu_has("libcuda.so.1") &&
	      !cu_has("libnvidia-nvvm.so.4") && !gs_exists(cu_conf) && gs_exists(p) && gs_exists(foreign) &&
	      contains(out, "libcuda.so (changed since install)"), "uninstall: %s", out);
	free(out);
	text = slurp(cu_manifest);
	CHECK(text && contains(text, "/cuda/libcuda.so\n") && !contains(text, "libcuda.so.580.159.03 /"), "the manifest keeps the link that stayed: %s",
	      text ? text : "(none)");
	free(text);
}

/* The paths of the desktop fixtures are far shorter than the buffers; GCC cannot know that. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
/*
 * NVIDIA's graphics libraries for the host desktop: the CUDA mechanism with the X modules, the small
 * files of /etc and /usr/share, the GBM backend link in libgbm's directory and the output-class file.
 * A tree of a Debian host is made by hand; its files must come out of every run as they went in.
 */
static char dt_root[GS_PATH_MAX], dt_manifest[GS_PATH_MAX], dt_dir[GS_PATH_MAX], dt_conf[GS_PATH_MAX], dt_stage[GS_PATH_MAX];
static char dt_loader[3 * GS_PATH_MAX];
static int dt_want;
static const char *dt_accept;     /* the version the installed nvidia.ko accepts; NULL: not known */
static const char *dt_xconf_text; /* NVIDIA's output-class file in the stage; NULL: a usual one */
static int dt_capture(const char *const argv[], char *out, size_t len)
{
	(void)argv;
	snprintf(out, len, "%s", dt_loader);
	return 0;
}
static const struct gs_nvlibs_io dt_io = { cu_run, dt_capture };
static int dt_apply(void)
{
	const struct gs_nvdest d = { dt_dir, dt_conf, dt_stage, dt_accept };

	return gs_nvlibs_apply(&gs_nvset_desktop, dt_manifest, &d, dt_want, &dt_io);
}
static int dt_uninstall(void)
{
	return gs_uninstall_files(dt_manifest, dt_root, NULL);
}
static char dt_sha[65];
/* One file of the staging directory; its digest is in dt_sha. */
static const char *dt_new(const char *name, const char *content)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", dt_stage, name);
	writef(p, "%s", content);
	gs_sha256_file(p, dt_sha);
	return dt_sha;
}
/* What the driver step leaves for the desktop set: libraries, X modules, small files, the GBM backend, one index. */
static void dt_stage_make(const char *v)
{
	/* Library, and the soname the loader asks for (none: the file name is its own soname). */
	static const char *const libs[][2] = {
		{ "libEGL_nvidia", "libEGL_nvidia.so.0" }, { "libGLX_nvidia", "libGLX_nvidia.so.0" },
		{ "libnvidia-glcore", NULL }, { "libnvidia-allocator", "libnvidia-allocator.so.1" },
	};
	const char *const rm[] = { "rm", "-rf", "--", dt_stage, NULL };
	char *index = calloc(1, 8192), name[96], link[96], p[GS_PATH_MAX];
	size_t at = 0;

	gs_run(rm);
	gs_mkdir_p(dt_stage, 0755);
	at += (size_t)snprintf(index + at, 8192 - at, "version=%s\n", v);
	for (unsigned int i = 0; i < 4; i++) {
		snprintf(name, sizeof(name), "%s.so.%s", libs[i][0], v);
		snprintf(link, sizeof(link), "%s", libs[i][1] ? libs[i][1] : name);
		at += (size_t)snprintf(index + at, 8192 - at, "lib=%s %s %s\n", name, dt_new(name, name), link);
	}
	at += (size_t)snprintf(index + at, 8192 - at, "lib=libnvidia-egl-gbm.so.1.1.3 %s libnvidia-egl-gbm.so.1\n",
			       dt_new("libnvidia-egl-gbm.so.1.1.3", "egl gbm\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "xmod=drivers/nvidia_drv.so %s\n", dt_new("nvidia_drv.so", "x driver\n"));
	snprintf(name, sizeof(name), "libglxserver_nvidia.so.%s", v);
	at += (size_t)snprintf(index + at, 8192 - at, "xmod=extensions/%s %s libglxserver_nvidia.so\n", name, dt_new(name, "glx module\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "etc=egl_vendor 10_nvidia.json %s\n", dt_new("10_nvidia.json", "{ \"library_path\" : \"libEGL_nvidia.so.0\" }\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "etc=egl_platform 15_nvidia_gbm.json %s\n", dt_new("15_nvidia_gbm.json", "gbm platform\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "etc=egl_platform 10_nvidia_wayland.json %s\n", dt_new("10_nvidia_wayland.json", "wayland platform\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "etc=vulkan_icd nvidia_icd.json %s\n", dt_new("nvidia_icd.json", "{ \"library_path\": \"libGLX_nvidia.so.0\" }\n"));
	snprintf(name, sizeof(name), "nvidia-application-profiles-%s-rc", v);
	at += (size_t)snprintf(index + at, 8192 - at, "etc=profile %s %s\n", name, dt_new(name, "profiles\n"));
	snprintf(name, sizeof(name), "nvidia-application-profiles-%s-key-documentation", v);
	at += (size_t)snprintf(index + at, 8192 - at, "etc=profile %s %s\n", name, dt_new(name, "profile keys\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "etc=xconf nvidia-drm-outputclass.conf %s\n",
			       dt_new("nvidia-drm-outputclass.conf", dt_xconf_text ? dt_xconf_text : "# NVIDIA's own words.\n\nSection \"OutputClass\"\n"
				      "    Identifier     \"nvidia\"\n    MatchDriver    \"nvidia-drm\"\n    Driver         \"nvidia\"\nEndSection\n"));
	at += (size_t)snprintf(index + at, 8192 - at, "gbm=libnvidia-allocator.so.1\n");
	gs_pathf(p, sizeof(p), "%s/index", dt_stage);
	writef(p, "%s", index);
	free(index);
}
static int dt_has(const char *rel)
{
	char p[GS_PATH_MAX];
	struct stat st;

	return !gs_pathf(p, sizeof(p), "%s/%s", dt_root, rel) && lstat(p, &st) == 0;
}
static int dt_link_is(const char *rel, const char *to)
{
	char p[GS_PATH_MAX], have[GS_PATH_MAX];
	ssize_t n;

	gs_pathf(p, sizeof(p), "%s/%s", dt_root, rel);
	n = readlink(p, have, sizeof(have) - 1);
	return n > 0 && (have[n] = '\0', strcmp(have, to) == 0);
}
/* Every file, directory and link below dir with its digest or target, in name order, except skip. */
static void dt_dump(const char *dir, const char *skip, char *out, size_t cap)
{
	struct dirent **names;
	int n = scandir(dir, &names, NULL, alphasort);

	for (int i = 0; i < n; i++) {
		char p[GS_PATH_MAX], sha[65], to[GS_PATH_MAX];
		struct stat st;
		size_t at = strlen(out);
		ssize_t k;

		if (strcmp(names[i]->d_name, ".") && strcmp(names[i]->d_name, "..") &&
		    !gs_pathf(p, sizeof(p), "%s/%s", dir, names[i]->d_name) && !(skip && strcmp(p, skip) == 0) && lstat(p, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				snprintf(out + at, cap - at, "d %s\n", p);
				dt_dump(p, skip, out, cap);
			} else if (S_ISLNK(st.st_mode)) {
				k = readlink(p, to, sizeof(to) - 1);
				to[k > 0 ? k : 0] = '\0';
				snprintf(out + at, cap - at, "l %s -> %s\n", p, to);
			} else {
				gs_sha256_file(p, sha);
				snprintf(out + at, cap - at, "f %s %s\n", p, sha);
			}
		}
		free(names[i]);
	}
	free(names);
}
static void test_host_desktop(void)
{
	static const char v[] = "580.159.03";
	const struct gs_nvgl_paths saved = gs_nvgl;
	char p[GS_PATH_MAX], q[GS_PATH_MAX], state[GS_PATH_MAX], line[2 * GS_PATH_MAX], dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX];
	char *before = calloc(1, 1 << 17), *after = calloc(1, 1 << 17), *out, *text, *deb_before = calloc(1, 1 << 16), *deb_after = calloc(1, 1 << 16);
	char libdir[GS_PATH_MAX], gbm[GS_PATH_MAX], xconf[GS_PATH_MAX], foreign[GS_PATH_MAX];
	struct stat st;

	gs_pathf(dt_root, sizeof(dt_root), "%s/desktop", tmp);
	gs_pathf(state, sizeof(state), "%s/state", dt_root);
	gs_mkdir_p(state, 0755);
	gs_pathf(dt_manifest, sizeof(dt_manifest), "%s/installed-files", state);
	gs_pathf(dt_dir, sizeof(dt_dir), "%s/usr/local/lib/geforce-sriov/gl", dt_root);
	gs_pathf(dt_conf, sizeof(dt_conf), "%s/etc/ld.so.conf.d/geforce-sriov-gl.conf", dt_root);
	gs_pathf(dt_stage, sizeof(dt_stage), "%s/host-desktop", state);
	gs_pathf(libdir, sizeof(libdir), "%s/usr/lib/x86_64-linux-gnu", dt_root);
	gs_pathf(gbm, sizeof(gbm), "%s/gbm/nvidia-drm_gbm.so", libdir);
	gs_pathf(xconf, sizeof(xconf), "%s/etc/X11/xorg.conf.d/geforce-sriov-nvidia-drm-outputclass.conf", dt_root);
	snprintf(gs_nvgl.etc_vendor, sizeof(gs_nvgl.etc_vendor), "%s/etc/glvnd/egl_vendor.d", dt_root);
	snprintf(gs_nvgl.etc_platform, sizeof(gs_nvgl.etc_platform), "%s/etc/egl/egl_external_platform.d", dt_root);
	snprintf(gs_nvgl.etc_vulkan, sizeof(gs_nvgl.etc_vulkan), "%s/etc/vulkan/icd.d", dt_root);
	snprintf(gs_nvgl.share_nvidia, sizeof(gs_nvgl.share_nvidia), "%s/usr/share/nvidia", dt_root);
	snprintf(gs_nvgl.xconf, sizeof(gs_nvgl.xconf), "%s/etc/X11/xorg.conf.d", dt_root);
	snprintf(gs_nvgl.share_vendor, sizeof(gs_nvgl.share_vendor), "%s/usr/share/glvnd/egl_vendor.d", dt_root);
	snprintf(gs_nvgl.share_platform, sizeof(gs_nvgl.share_platform), "%s/usr/share/egl/egl_external_platform.d", dt_root);
	snprintf(gs_nvgl.xorg_drivers, sizeof(gs_nvgl.xorg_drivers), "%s/usr/lib/xorg/modules/drivers", dt_root);
	/* The files a Debian host has: Mesa's libglvnd, libgbm with its backend directory, Mesa's EGL vendor file, an X configuration directory. */
	gs_pathf(p, sizeof(p), "%s/libEGL.so.1", libdir);
	writef(p, "libglvnd's libEGL\n");
	gs_pathf(p, sizeof(p), "%s/libgbm.so.1", libdir);
	writef(p, "libgbm\n");
	gs_pathf(p, sizeof(p), "%s/gbm/dri_gbm.so", libdir);
	writef(p, "mesa's gbm backend\n");
	gs_pathf(p, sizeof(p), "%s/usr/share/glvnd/egl_vendor.d/50_mesa.json", dt_root);
	writef(p, "mesa\n");
	gs_pathf(p, sizeof(p), "%s/usr/share/X11/xorg.conf.d/10-quirks.conf", dt_root);
	writef(p, "quirks\n");
	gs_pathf(p, sizeof(p), "%s/usr/lib/xorg/modules/drivers/modesetting_drv.so", dt_root);
	writef(p, "modesetting\n");
	gs_pathf(p, sizeof(p), "%s/etc/ld.so.conf.d/x86_64-linux-gnu.conf", dt_root);
	writef(p, "/usr/lib/x86_64-linux-gnu\n");
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n", libdir);
	gs_pathf(p, sizeof(p), "%s/usr", dt_root);
	dt_dump(dt_root, state, before, 1 << 17);
	dt_dump(p, NULL, deb_before, 1 << 16);

	/* Host desktop is on and nothing was handed over: one warning, nothing installed. */
	dt_want = GS_NVLIBS_ON;
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local/lib/geforce-sriov/gl") &&
	      contains(out, "warning: host_desktop=on, but NVIDIA's graphics libraries are not installed, so the desktop\nis drawn by the CPU.") &&
	      contains(out, "Run `sudo ./geforce-sriov install --host-desktop --companion-file PATH` in the source folder with that package."),
	      "nothing staged, nothing installed: %s", out);
	free(out);
	/* With auto it is a line, not a warning. */
	dt_want = GS_NVLIBS_AUTO;
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "host desktop: not installed (host_desktop=auto): NVIDIA's graphics libraries were not taken") &&
	      !contains(out, "warning"), "auto says it on stdout: %s", out);
	free(out);
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, NULL, line, sizeof(line)) == -1 && contains(line, "the desktop is drawn by the CPU"),
	      "status says so: %s", line);
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 0, NULL, line, sizeof(line)) == 0 && !line[0], "and nothing when it is off");

	/* An index that does not fit its files is not installed from. */
	dt_want = GS_NVLIBS_ON;
	dt_stage_make(v);
	gs_pathf(p, sizeof(p), "%s/nvidia_drv.so", dt_stage);
	writef(p, "not the file the index names\n");
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local/lib/geforce-sriov/gl") &&
	      contains(out, "does not hold what the driver step recorded there; NVIDIA's graphics libraries\nare not installed from it."),
	      "a staging directory that was changed: %s", out);
	free(out);

	/* A file of someone else in a place the set writes to is kept, named, and nothing is installed. */
	dt_stage_make(v);
	gs_pathf(foreign, sizeof(foreign), "%s/etc/glvnd/egl_vendor.d/10_nvidia.json", dt_root);
	writef(foreign, "somebody else's\n");
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "warning: NVIDIA's graphics libraries are not installed: ") && contains(out, foreign) &&
	      contains(out, "exists and was not installed by\ngeforce-sriov, and is kept as it is.") && !dt_has("usr/local/lib/geforce-sriov/gl") &&
	      !contains(out, "R ldconfig") && gs_exists(dt_stage), "a foreign file: %s", out);
	free(out);
	text = slurp(foreign);
	CHECK(text && strcmp(text, "somebody else's\n") == 0, "the foreign file is as it was");
	free(text);
	unlink(foreign);
	gs_pathf(p, sizeof(p), "%s/etc/glvnd/egl_vendor.d", dt_root);
	rmdir(p);
	gs_pathf(p, sizeof(p), "%s/etc/glvnd", dt_root);
	rmdir(p);
	/* So is a link of someone else where the GBM backend goes. */
	writef(gbm, "a backend of someone else\n");
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, gbm) && contains(out, "is kept as it is.") && !dt_has("usr/local/lib/geforce-sriov/gl"),
	      "a foreign GBM backend: %s", out);
	free(out);
	unlink(gbm);
	/* Another NVIDIA graphics driver on the host: the two would be mixed. */
	gs_pathf(foreign, sizeof(foreign), "%s/usr/share/glvnd/egl_vendor.d/10_nvidia.json", dt_root);
	writef(foreign, "another driver\n");
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "belongs to another NVIDIA graphics\ndriver, and the two would be mixed.") &&
	      contains(out, foreign) && !dt_has("usr/local/lib/geforce-sriov/gl"), "another NVIDIA graphics driver: %s", out);
	free(out);
	unlink(foreign);
	gs_pathf(foreign, sizeof(foreign), "%s/usr/lib/xorg/modules/drivers/nvidia_drv.so", dt_root);
	writef(foreign, "another X driver\n");
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "belongs to another NVIDIA graphics") && contains(out, foreign) &&
	      !dt_has("usr/local/lib/geforce-sriov/gl"), "another NVIDIA X driver: %s", out);
	free(out);
	unlink(foreign);
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n\tlibEGL_nvidia.so.0 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/libEGL_nvidia.so.0\n", libdir);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "belongs to another NVIDIA graphics") && contains(out, "/usr/lib/x86_64-linux-gnu/libEGL_nvidia.so.0") &&
	      !dt_has("usr/local/lib/geforce-sriov/gl"), "another libEGL_nvidia the loader finds: %s", out);
	free(out);
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n", libdir);

	/* Install: libraries, X modules, small files, the backend link, the loader's file, ldconfig, then the output-class file. */
	CHECK(run_host(dt_apply, &out) == 0 && !contains(out, "warning"), "installs: %s", out);
	CHECK(dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.580.159.03") && dt_has("usr/local/lib/geforce-sriov/gl/libGLX_nvidia.so.580.159.03") &&
	      dt_has("usr/local/lib/geforce-sriov/gl/libnvidia-glcore.so.580.159.03") && dt_has("usr/local/lib/geforce-sriov/gl/libnvidia-allocator.so.580.159.03") &&
	      dt_has("usr/local/lib/geforce-sriov/gl/libnvidia-egl-gbm.so.1.1.3"), "the libraries are in the package's directory: %s", out);
	CHECK(dt_link_is("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0", "libEGL_nvidia.so.580.159.03") &&
	      dt_link_is("usr/local/lib/geforce-sriov/gl/libGLX_nvidia.so.0", "libGLX_nvidia.so.580.159.03") &&
	      dt_link_is("usr/local/lib/geforce-sriov/gl/libnvidia-allocator.so.1", "libnvidia-allocator.so.580.159.03") &&
	      dt_link_is("usr/local/lib/geforce-sriov/gl/libnvidia-egl-gbm.so.1", "libnvidia-egl-gbm.so.1.1.3") &&
	      !dt_has("usr/local/lib/geforce-sriov/gl/libnvidia-glcore.so.0"), "the soname links, and none for a library that is its own soname");
	CHECK(dt_has("usr/local/lib/geforce-sriov/xorg/drivers/nvidia_drv.so") && dt_has("usr/local/lib/geforce-sriov/xorg/extensions/libglxserver_nvidia.so.580.159.03") &&
	      dt_link_is("usr/local/lib/geforce-sriov/xorg/extensions/libglxserver_nvidia.so", "libglxserver_nvidia.so.580.159.03"), "the X modules");
	CHECK(dt_has("etc/X11/xorg.conf.d/geforce-sriov-nvidia-drm-outputclass.conf") && !dt_has("usr/share/X11/xorg.conf.d/geforce-sriov-nvidia-drm-outputclass.conf"),
	      "the output-class file is in /etc/X11/xorg.conf.d, not in the X server package's /usr/share directory");
	CHECK(dt_has("etc/glvnd/egl_vendor.d/10_nvidia.json") && dt_has("etc/egl/egl_external_platform.d/15_nvidia_gbm.json") &&
	      dt_has("etc/egl/egl_external_platform.d/10_nvidia_wayland.json") && dt_has("etc/vulkan/icd.d/nvidia_icd.json") &&
	      dt_has("usr/share/nvidia/nvidia-application-profiles-580.159.03-rc") && dt_has("usr/share/nvidia/nvidia-application-profiles-580.159.03-key-documentation"),
	      "the small files");
	snprintf(q, sizeof(q), "%s/usr/local/lib/geforce-sriov/gl/libnvidia-allocator.so.1", dt_root);
	CHECK(dt_link_is("usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so", q), "the GBM backend link names the allocator in the package's directory");
	text = slurp(dt_conf);
	snprintf(q, sizeof(q), "%s\n", dt_dir);
	CHECK(text && strcmp(text, q) == 0, "the loader's file names the directory: %s", text ? text : "(none)");
	free(text);
	gs_pathf(p, sizeof(p), "%s/usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.580.159.03", dt_root);
	CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0755, "a library is executable, as NVIDIA's installer leaves it");
	gs_pathf(p, sizeof(p), "%s/usr/share/nvidia/nvidia-application-profiles-580.159.03-rc", dt_root);
	CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0444, "the profile is read-only");
	/* The output-class file: NVIDIA's own, with the module path and ProbeAllGpus=false inside its OutputClass section: no BusID, no Device section. */
	text = slurp(xconf);
	snprintf(q, sizeof(q), "    ModulePath     \"%s/usr/local/lib/geforce-sriov/xorg\"\nEndSection\n", dt_root);
	CHECK(text && strncmp(text, "# NVIDIA's nvidia-drm-outputclass.conf, with the directory geforce-sriov keeps NVIDIA's X modules in.\n# NVIDIA's own words.\n", 120) == 0 &&
	      contains(text, "Section \"OutputClass\"\n    Identifier     \"nvidia\"\n    MatchDriver    \"nvidia-drm\"\n    Driver         \"nvidia\"\n") &&
	      contains(text, q) && !contains(text, "BusID") && !contains(text, "Device") &&
	      contains(text, "    Option         \"ProbeAllGpus\" \"false\"\n    ModulePath") && strstr(text, "ProbeAllGpus") < strstr(text, "EndSection") &&
	      strstr(text, "ProbeAllGpus") > strstr(text, "Section \"OutputClass\"") && !strstr(strstr(text, "ProbeAllGpus") + 1, "ProbeAllGpus") &&
	      !contains(text, "ServerFlags") && contains(text, "# geforce-sriov: X probes only the boot display card") &&
	      !contains(text, "opens only the GPU"), "the output-class file: %s", text ? text : "(none)");
	free(text);
	{
		const char *ld = strstr(out, "R ldconfig\n"), *xc = strstr(out, xconf), *drv = strstr(out, "xorg/drivers/nvidia_drv.so");
		const char *js = strstr(out, "egl_vendor.d/10_nvidia.json"), *gl = strstr(out, gbm), *pl = strstr(out, "15_nvidia_gbm.json");

		/*
		 * The order that keeps the X server alive (X on NVIDIA's EGL without NVIDIA's X driver dies): the X driver, then the
		 * output-class file, and only then, after the loader's cache has the libraries, what makes NVIDIA's EGL reachable.
		 */
		CHECK(drv && xc && js && gl && pl && ld, "every step of the order is in the output: %s", out);
		CHECK(drv < xc, "the X driver is in before the output-class file: %s", out);
		CHECK(xc < js && xc < gl && xc < pl, "the output-class file is in before the EGL vendor file, the platform file and the GBM link: %s", out);
		CHECK(xc < ld && ld < js && ld < gl, "ldconfig runs before NVIDIA's EGL is made reachable: %s", out);
		CHECK(contains(out, "host desktop: NVIDIA's graphics libraries 580.159.03 are installed in ") && !gs_exists(dt_stage),
		      "said, and the staging directory is used up: %s", out);
	}
	free(out);
	/* No file of a Debian package changed, and the only entry in a directory of theirs is the backend link. */
	gs_pathf(p, sizeof(p), "%s/usr", dt_root);
	dt_dump(p, NULL, deb_after, 1 << 16);
	CHECK(strstr(deb_after, "gbm/nvidia-drm_gbm.so") != NULL, "the backend link is in libgbm's directory");
	gs_pathf(p, sizeof(p), "%s/libEGL.so.1", libdir);
	text = slurp(p);
	CHECK(text && strcmp(text, "libglvnd's libEGL\n") == 0, "libglvnd's libEGL is as it was");
	free(text);
	gs_pathf(p, sizeof(p), "%s/gbm/dri_gbm.so", libdir);
	text = slurp(p);
	CHECK(text && strcmp(text, "mesa's gbm backend\n") == 0, "Mesa's GBM backend is as it was");
	free(text);
	gs_pathf(p, sizeof(p), "%s/usr/share/glvnd/egl_vendor.d/50_mesa.json", dt_root);
	text = slurp(p);
	CHECK(text && strcmp(text, "mesa\n") == 0 && !dt_has("usr/share/glvnd/egl_vendor.d/10_nvidia.json") &&
	      !dt_has("usr/lib/x86_64-linux-gnu/libEGL_nvidia.so.0") && !dt_has("usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0"),
	      "Mesa's EGL vendor file is as it was and nothing of NVIDIA's is in the distribution's directories");
	free(text);
	text = slurp(dt_manifest);
	CHECK(text && contains(text, "f ") && contains(text, xconf) && contains(text, dt_conf) && contains(text, "/gbm/nvidia-drm_gbm.so\n") &&
	      contains(text, "/usr/local/lib/geforce-sriov/xorg/drivers/nvidia_drv.so\n"), "the manifest lists every file and link: %s", text ? text : "(none)");
	free(text);
	CHECK(gs_nvlibs_state(&gs_nvset_desktop, dt_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == 1 &&
	      strcmp(dir, dt_dir) == 0 && strcmp(version, v) == 0, "the manifest says what is installed: %s %s", dir, version);
	CHECK(gs_nvlibs_state(&gs_nvset_cuda, dt_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == 0,
	      "and the CUDA set sees none of it");
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n\tlibEGL_nvidia.so.0 (libc6,x86-64) => %s/libEGL_nvidia.so.0\n"
		 "\tlibGLX_nvidia.so.0 (libc6,x86-64) => %s/libGLX_nvidia.so.0\n", libdir, dt_dir, dt_dir);
	snprintf(q, sizeof(q), "NVIDIA's graphics libraries 580.159.03 are installed in %s", dt_dir);
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, dt_capture, line, sizeof(line)) == 1 && strcmp(line, q) == 0, "status and doctor: %s", line);
	/* libgbm looks elsewhere: the link would not be found. */
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/other/libgbm.so.1\n\tlibEGL_nvidia.so.0 (libc6,x86-64) => %s/libEGL_nvidia.so.0\n", libdir, dt_dir);
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, dt_capture, line, sizeof(line)) == -1 && contains(line, "libgbm looks for its backends in "), "libgbm elsewhere: %s", line);
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n\tlibEGL_nvidia.so.0 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/libEGL_nvidia.so.0\n", libdir);
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, dt_capture, line, sizeof(line)) == -1 && contains(line, "but programs load another one first: "), "another libEGL_nvidia first: %s", line);
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n", libdir);

	/* Again, with nothing handed over: nothing is written and ldconfig is not run. */
	CHECK(run_host(dt_apply, &out) == 0 && !contains(out, "R ") && !contains(out, "installed") && !contains(out, "warning"), "a rerun changes nothing: %s", out);
	free(out);
	/* Again, with the same libraries handed over: every file is as it was. */
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "unchanged ") && !contains(out, "R ldconfig") && !contains(out, "linked ") && !gs_exists(dt_stage),
	      "the same libraries again: %s", out);
	free(out);

	/* A second package of another version: its libraries replace the installed ones, small files included. */
	dt_stage_make("580.159.05");
	CHECK(run_host(dt_apply, &out) == 0 && dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.580.159.05") &&
	      !dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.580.159.03") && !dt_has("usr/local/lib/geforce-sriov/xorg/extensions/libglxserver_nvidia.so.580.159.03") &&
	      !dt_has("usr/share/nvidia/nvidia-application-profiles-580.159.03-rc") && dt_has("usr/share/nvidia/nvidia-application-profiles-580.159.05-rc") &&
	      dt_link_is("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0", "libEGL_nvidia.so.580.159.05") &&
	      contains(out, "libEGL_nvidia.so.580.159.03: it belongs to another version of NVIDIA's graphics libraries") && contains(out, "R ldconfig\n"),
	      "another version replaces the installed one: %s", out);
	{
		const char *xc = strstr(out, xconf), *js = strstr(out, "egl_vendor.d/10_nvidia.json"), *ld = strstr(out, "R ldconfig\n");
		const char *gnew = strstr(out, "installed "), *gold = strstr(out, "libglxserver_nvidia.so.580.159.03: it belongs");
		char rm_xc[GS_PATH_MAX + 16], rm_drv[GS_PATH_MAX + 16];

		snprintf(rm_xc, sizeof(rm_xc), "removed %s", xconf);
		snprintf(rm_drv, sizeof(rm_drv), "removed %s/usr/local/lib/geforce-sriov/xorg/drivers/nvidia_drv.so", dt_root);
		/* An upgrade: X's two halves are replaced in place, never taken out, and NVIDIA's EGL file is not written before X is on NVIDIA's driver. */
		CHECK(!contains(out, rm_xc) && !contains(out, rm_drv), "an upgrade never removes the output-class file or the X driver: %s", out);
		CHECK(xc && js && ld && xc < js && ld < js, "an upgrade keeps the EGL vendor file after the output-class file and ldconfig: %s", out);
		gnew = strstr(out, "installed ") ? strstr(out, "libglxserver_nvidia.so.580.159.05") : NULL;
		CHECK(gnew && gold && gnew < gold, "the new GLX module is in before the old one is swept: %s", out);
	}
	free(out);

	/* A library that was changed since install: incomplete, and said; with the set off it is kept and named, the rest goes. */
	gs_pathf(p, sizeof(p), "%s/usr/local/lib/geforce-sriov/gl/libnvidia-glcore.so.580.159.05", dt_root);
	writef(p, "changed\n");
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, NULL, line, sizeof(line)) == -1 && contains(line, "incomplete: ") && contains(line, p), "a changed library: %s", line);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "warning: NVIDIA's graphics libraries are incomplete: ") && contains(out, p), "install says so too: %s", out);
	free(out);
	dt_want = 0;
	CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 0, NULL, line, sizeof(line)) == 2 && contains(line, "although host_desktop=off"), "off, still installed: %s", line);
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0") && !dt_has("usr/local/lib/geforce-sriov/xorg") &&
	      !dt_has("etc/glvnd") && !dt_has("etc/egl") && !dt_has("etc/vulkan") && !dt_has("usr/share/nvidia") && !gs_exists(xconf) && !gs_exists(dt_conf) &&
	      !dt_has("usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so") && gs_exists(p) && contains(out, "kept ") && contains(out, "(changed since install)") &&
	      contains(out, ": host desktop is off\n") && contains(out, "R ldconfig\n"), "off removes what is unchanged and keeps what was changed: %s", out);
	{
		const char *xc = strstr(out, xconf), *js = strstr(out, "egl_vendor.d/10_nvidia.json: host desktop is off");
		const char *gl = strstr(out, "nvidia-drm_gbm.so: host desktop is off"), *lib = strstr(out, "libEGL_nvidia.so.580.159.05: host desktop is off");
		const char *drv = strstr(out, "xorg/drivers/nvidia_drv.so: host desktop is off");

		CHECK(xc && js && gl && lib && drv, "every removal is said: %s", out);
		CHECK(js < xc && gl < xc && lib < xc && drv < xc, "the output-class file goes last, after NVIDIA's EGL files, the GBM link and the X driver: %s", out);
	}
	free(out);
	unlink(p);
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local") && !contains(out, "R ldconfig"), "off again: the directories go, nothing else: %s", out);
	free(out);

	/* M1: libgbm without a backend directory (Mesa before 24.3): the X11 desktop installs, only the GBM link is left out, and it is said. */
	{
		char gdir[GS_PATH_MAX];
		const char *rm_gbm[] = { "rm", "-rf", "--", gdir, NULL };

		gs_pathf(gdir, sizeof(gdir), "%s/gbm", libdir);
		gs_run(rm_gbm);
		dt_want = GS_NVLIBS_ON;
		dt_stage_make(v);
		CHECK(run_host(dt_apply, &out) == 0 && !contains(out, "warning") && contains(out, "host desktop: the GBM backend link is not made: libgbm's backend directory ") &&
		      contains(out, "X11 desktops are not affected") && dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0") &&
		      dt_has("usr/local/lib/geforce-sriov/xorg/drivers/nvidia_drv.so") && dt_has("etc/glvnd/egl_vendor.d/10_nvidia.json") &&
		      dt_has("etc/X11/xorg.conf.d/geforce-sriov-nvidia-drm-outputclass.conf") && gs_exists(dt_conf) && !dt_has("usr/lib/x86_64-linux-gnu/gbm") &&
		      contains(out, "host desktop: NVIDIA's graphics libraries 580.159.03 are installed in "),
		      "libgbm without its directory: the set installs, one line says the link is left out: %s", out);
		free(out);
		CHECK(gs_nvlibs_state(&gs_nvset_desktop, dt_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == 1 &&
		      gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, NULL, line, sizeof(line)) == 1, "and the set is installed, not partial: %s", line);
		dt_want = 0;
		CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local") && !dt_has("etc/X11") && !dt_has("etc/glvnd") && !dt_has("usr/lib/x86_64-linux-gnu/gbm"),
		      "and off takes it away, with the directories made for it: %s", out);
		free(out);
		gs_pathf(p, sizeof(p), "%s/gbm/dri_gbm.so", libdir);
		writef(p, "mesa's gbm backend\n");
	}

	/* M2: an install that fails half-way is taken back completely, and nothing reports a partial set as installed. */
	CHECK(gs_mkdir_p(dt_conf, 0755) == 0, "the loader's file cannot be written: a directory is in its place");
	dt_want = GS_NVLIBS_ON;
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "warning: NVIDIA's graphics libraries could not be installed in ") &&
	      contains(out, "What this run wrote\nwas taken out again; the desktop is as it was without them.") && !contains(out, "host_desktop=on, but") &&
	      !dt_has("usr/local") && !dt_has("etc/X11") && !dt_has("etc/glvnd") && !dt_has("etc/egl") && !dt_has("etc/vulkan") && !dt_has("usr/share/nvidia") &&
	      !dt_has("usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so") && gs_exists(dt_stage),
	      "a failed install is rolled back, the stage stays for the next run: %s", out);
	free(out);
	CHECK(gs_nvlibs_state(&gs_nvset_desktop, dt_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == 0 &&
	      gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, NULL, line, sizeof(line)) == -1 && contains(line, "not installed"),
	      "status and doctor say it is not installed: %s", line);
	rmdir(dt_conf);
	after[0] = '\0';
	dt_dump(dt_root, state, after, 1 << 17);
	CHECK(strcmp(before, after) == 0, "the host is as before the failed install:\n%s\n--- against ---\n%s", before, after);
	CHECK(run_host(dt_apply, &out) == 0 && !contains(out, "warning") && dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0"), "the same command installs once the cause is gone: %s", out);
	free(out);
	/* A manifest that lists the libraries but not the output-class file, the EGL vendor file or the loader's file is not a complete set. */
	text = slurp(dt_manifest);
	CHECK(text != NULL, "the manifest of the complete set");
	{
		static const char *const parts[] = { "geforce-sriov-nvidia-drm-outputclass.conf", "10_nvidia.json", "geforce-sriov-gl.conf" };
		char *whole = text ? strdup(text) : NULL, *copy;

		for (unsigned int k = 0; k < 3 && whole; k++) {
			FILE *f;
			char *l, *save = NULL;

			copy = strdup(whole);
			f = fopen(dt_manifest, "w");
			for (l = strtok_r(copy, "\n", &save); f && l; l = strtok_r(NULL, "\n", &save))
				if (!strstr(l, parts[k]))
					fprintf(f, "%s\n", l);
			if (f)
				fclose(f);
			free(copy);
			CHECK(gs_nvlibs_state(&gs_nvset_desktop, dt_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == -1 && contains(bad, parts[k]),
			      "a manifest without %s is not a complete set: %s", parts[k], bad);
			CHECK(gs_nvlibs_line(&gs_nvset_desktop, dt_manifest, 1, NULL, line, sizeof(line)) == -1 && contains(line, "incomplete: "), "and the line says so: %s", line);
		}
		if (whole) {
			FILE *f = fopen(dt_manifest, "w");

			if (f) {
				fputs(whole, f);
				fclose(f);
			}
		}
		free(whole);
	}
	free(text);
	dt_want = 0;
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local"), "off again: %s", out);
	free(out);

	/* M2, late: the failure comes after the EGL vendor file is in place (the platform directory's parent is a file). The rollback takes
	 * NVIDIA's EGL file out before the output-class file. */
	gs_pathf(p, sizeof(p), "%s/etc/egl", dt_root);
	writef(p, "not a directory\n");
	dt_want = GS_NVLIBS_ON;
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "warning: NVIDIA's graphics libraries could not be installed in "), "a late failure: %s", out);
	{
		const char *js = strstr(out, "egl_vendor.d/10_nvidia.json: the install of NVIDIA's graphics libraries failed"), *xc;

		CHECK(contains(out, "installed ") && js, "the vendor file was written before the failure, and taken out: %s", out);
		xc = strstr(out, "outputclass.conf: the install of NVIDIA's graphics libraries failed");
		CHECK(js && xc && js < xc, "the rollback removes the EGL vendor file before the output-class file: %s", out);
	}
	free(out);
	CHECK(gs_nvlibs_state(&gs_nvset_desktop, dt_manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad)) == 0 &&
	      !dt_has("usr/local") && !dt_has("etc/X11") && !dt_has("etc/glvnd"), "and the set is not installed");
	unlink(p);
	dt_want = 0;
	CHECK(run_host(dt_apply, &out) == 0, "off: %s", out);
	free(out);

	/* S2: NVIDIA's output-class file that cannot be used is found before the first write. A keyword with blanks and a CR line end is usable. */
	dt_want = GS_NVLIBS_ON;
	dt_xconf_text = "Section \"OutputClass\"\n    Identifier \"nvidia\"\n";
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "has no `EndSection` line") && contains(out, "Nothing was written.") && !contains(out, "installed ") &&
	      !contains(out, "R ldconfig") && !dt_has("usr/local") && !dt_has("etc/X11") && !dt_has("etc/glvnd") && gs_exists(dt_stage),
	      "an unusable output-class file refuses the set before anything is written: %s", out);
	free(out);
	dt_xconf_text = "Section \"OutputClass\"\r\n    Identifier \"nvidia\"\r\n   EndSection \r\n";
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && !contains(out, "warning") && gs_exists(xconf), "an indented keyword with a CR is usable: %s", out);
	free(out);
	text = slurp(xconf);
	snprintf(q, sizeof(q), "    ModulePath     \"%s/usr/local/lib/geforce-sriov/xorg\"\n   EndSection", dt_root);
	CHECK(text && contains(text, q), "the module path is put before that keyword: %s", text ? text : "(none)");
	free(text);
	dt_xconf_text = NULL;
	dt_want = 0;
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local"), "off again: %s", out);
	free(out);

	/* S3: a stage of another version than the build accepts is refused with one line, and kept. */
	dt_want = GS_NVLIBS_ON;
	dt_accept = "580.159.05";
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && contains(out, "the staged ones are version 580.159.03 and the installed\nnvidia.ko accepts version 580.159.05.") &&
	      !contains(out, "installed /") && !dt_has("usr/local") && !dt_has("etc/X11") && gs_exists(dt_stage), "another version than the build accepts: %s", out);
	free(out);
	dt_accept = "580.159.03";
	CHECK(run_host(dt_apply, &out) == 0 && !contains(out, "warning") && dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0"), "the version the build accepts installs: %s", out);
	free(out);
	dt_accept = NULL;
	dt_want = 0;
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local"), "off again: %s", out);
	free(out);

	/* Without libgbm on the host there is no backend link and the rest installs. */
	dt_want = GS_NVLIBS_AUTO;
	dt_loader[0] = '\0';
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so") && dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0") && !contains(out, "warning"),
	      "no libgbm: %s", out);
	free(out);
	dt_want = 0;
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("usr/local"), "and off again: %s", out);
	free(out);

	/* The distribution's own platform file of that name: the package's copy is not written, and says so. */
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n", libdir);
	gs_pathf(foreign, sizeof(foreign), "%s/usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json", dt_root);
	writef(foreign, "libnvidia-egl-wayland1's\n");
	dt_want = GS_NVLIBS_ON;
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && !dt_has("etc/egl/egl_external_platform.d/10_nvidia_wayland.json") &&
	      dt_has("etc/egl/egl_external_platform.d/15_nvidia_gbm.json") && contains(out, "host desktop: 10_nvidia_wayland.json is not installed: the distribution has ") &&
	      !contains(out, "warning"), "a platform file the distribution has: %s", out);
	free(out);

	/* Uninstall removes exactly what was installed: unchanged files and links go, one that was changed stays; the output-class file last. */
	gs_pathf(p, sizeof(p), "%s/usr/local/lib/geforce-sriov/gl/libnvidia-egl-gbm.so.1", dt_root);
	unlink(p);
	CHECK(symlink("somewhere-else", p) == 0, "a link that the user changed");
	CHECK(run_host(dt_uninstall, &out) == GS_EXIT_OK && !dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0") && !dt_has("usr/local/lib/geforce-sriov/xorg") &&
	      !gs_exists(xconf) && !gs_exists(dt_conf) && !dt_has("usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so") && gs_exists(p) &&
	      contains(out, "libnvidia-egl-gbm.so.1 (changed since install)"), "uninstall: %s", out);
	{
		const char *xc = strstr(out, "geforce-sriov-nvidia-drm-outputclass.conf"), *js = strstr(out, "egl_vendor.d/10_nvidia.json");
		const char *gl = strstr(out, "nvidia-drm_gbm.so"), *lib = strstr(out, "libEGL_nvidia.so.580.159.03");

		CHECK(xc && js && gl && lib, "every removal is said: %s", out);
		CHECK(js < xc && gl < xc && lib < xc, "the output-class file is removed last, after NVIDIA's EGL files, the GBM link and the libraries: %s", out);
	}
	free(out);
	text = slurp(dt_manifest);
	CHECK(text && contains(text, "/gl/libnvidia-egl-gbm.so.1\n") && !contains(text, "libEGL_nvidia.so.580.159.03 /"), "the manifest keeps the link that stayed: %s", text ? text : "(none)");
	free(text);
	unlink(p);
	unlink(foreign);
	/* What the changed link kept is the user's now: it goes with its directories before the host is compared. */
	gs_pathf(p, sizeof(p), "%s/usr/local", dt_root);
	{
		const char *rm_local[] = { "rm", "-rf", "--", p, NULL };

		gs_run(rm_local);
	}
	gs_pathf(p, sizeof(p), "%s/usr/share/egl/egl_external_platform.d", dt_root);
	rmdir(p);
	gs_pathf(p, sizeof(p), "%s/usr/share/egl", dt_root);
	rmdir(p);

	/* A clean cycle puts everything back exactly as it was: the same files, links and directories. */
	{
		const char *rm_all[] = { "rm", "-rf", "--", state, NULL };

		gs_run(rm_all);
	}
	snprintf(dt_loader, sizeof(dt_loader), "\tlibgbm.so.1 (libc6,x86-64) => %s/libgbm.so.1\n", libdir);
	gs_pathf(p, sizeof(p), "%s/usr", dt_root);
	after[0] = '\0';
	dt_dump(dt_root, state, after, 1 << 17);
	CHECK(strcmp(before, after) == 0, "the earlier runs left nothing behind:\n%s\n--- against ---\n%s", before, after);
	gs_mkdir_p(state, 0755);
	dt_want = GS_NVLIBS_ON;
	dt_stage_make(v);
	CHECK(run_host(dt_apply, &out) == 0 && dt_has("usr/local/lib/geforce-sriov/gl/libEGL_nvidia.so.0"), "installed again: %s", out);
	free(out);
	CHECK(run_host(dt_uninstall, &out) == GS_EXIT_OK, "uninstall: %s", out);
	free(out);
	after[0] = '\0';
	dt_dump(dt_root, state, after, 1 << 17);
	CHECK(strcmp(before, after) == 0, "install and uninstall leave the host as it was:\n%s\n--- against ---\n%s", before, after);
	deb_after[0] = '\0';
	gs_pathf(p, sizeof(p), "%s/usr", dt_root);
	dt_dump(p, NULL, deb_after, 1 << 16);
	CHECK(strcmp(deb_before, deb_after) == 0, "nothing of the distribution's changed");
	/* The display server, from a fake /proc: NVIDIA's libraries mapped, not mapped, a map that cannot be read, none. */
	{
		char pr[GS_PATH_MAX], who[256];

		gs_pathf(pr, sizeof(pr), "%s/proc", dt_root);
		CHECK(gs_desktop_servers(pr, who, sizeof(who)) == -1, "no /proc: no display server");
		gs_pathf(p, sizeof(p), "%s/100/comm", pr);
		writef(p, "Xorg\n");
		gs_pathf(p, sizeof(p), "%s/100/maps", pr);
		CHECK(symlink("maps", p) == 0, "a map that cannot be opened (a link loop)");
		CHECK(gs_desktop_servers(pr, who, sizeof(who)) == -2 && contains(who, "the X server (pid 100)"), "unreadable map: cannot be told: %s", who);
		unlink(p);
		writef(p, "7f00 r-xp /usr/lib/xorg/modules/drivers/modesetting_drv.so\n");
		CHECK(gs_desktop_servers(pr, who, sizeof(who)) == 0, "readable, without NVIDIA's libraries: software");
		writef(p, "7f00 r-xp /usr/local/lib/geforce-sriov/xorg/drivers/nvidia_drv.so\n");
		CHECK(gs_desktop_servers(pr, who, sizeof(who)) == 1, "readable, with the X driver: NVIDIA");
		{
			const char *loaded = "the running nvidia module (build-id aa) is not the installed one (bb); power the machine off and on to load it";

			CHECK(gs_desktop_draw_line(1, who, 1, loaded, line, sizeof(line)) == 1 && contains(line, "draws with NVIDIA's driver") &&
			      contains(line, "used after the next power cycle") && !contains(line, "in software"), "stale module, NVIDIA renderer: %s", line);
			CHECK(gs_desktop_draw_line(-1, who, 1, loaded, line, sizeof(line)) == 1 && contains(line, "used after the next power cycle") &&
			      !contains(line, "in software"), "stale module, no server: %s", line);
			CHECK(gs_desktop_draw_line(0, who, 1, loaded, line, sizeof(line)) == 3 && contains(line, "draws in software"), "stale module, software renderer: %s", line);
			CHECK(gs_desktop_draw_line(1, who, 0, loaded, line, sizeof(line)) == 0, "current module, NVIDIA renderer: nothing to say");
		}
	}
	gs_nvgl = saved;
	free(before);
	free(after);
	free(deb_before);
	free(deb_after);
}

#pragma GCC diagnostic pop

/* The build sentence names the one command that builds again for this kernel, whatever the features. */
static void test_build_line(void)
{
	struct gs_build b;
	char line[3 * GS_PATH_MAX];

	memset(&b, 0, sizeof(b));
	b.state = GS_BUILD_PRESENT;
	b.record_read = 1;
	gs_copy(b.release, sizeof(b.release), "6.8.0");
	gs_copy(b.record, sizeof(b.record), "/var/lib/geforce-sriov/driver-build");
	b.display = 1;
	b.host_cuda = 1;
	CHECK(gs_build_line(&b, line, sizeof(line)) == 1 &&
	      contains(line, "build it again for this kernel (`sudo geforce-sriov rebuild`)") &&
	      !contains(line, "--companion") && !contains(line, "geforce-sriov-driver"),
	      "a display and host CUDA record needs no option: %s", line);
	b.display = b.host_cuda = 0;
	CHECK(gs_build_line(&b, line, sizeof(line)) == 1 && contains(line, "(`sudo geforce-sriov rebuild`)"),
	      "a plain record: %s", line);
	b.display = b.host_cuda = -1;
	b.state = GS_BUILD_ABSENT;
	CHECK(gs_build_line(&b, line, sizeof(line)) == 1 && contains(line, "(`sudo geforce-sriov rebuild`)"),
	      "an old record without feature lines: %s", line);
}

static int setup_run(const char *const argv[])
{
	char line[1024], p[GS_PATH_MAX];

	joined(argv, line, sizeof(line));
	gs_pathf(p, sizeof(p), "%s/setup.log", sim.root);
	{
		FILE *f = fopen(p, "a");

		if (!f)
			die(p);
		fprintf(f, "R %s\n", line);
		fclose(f);
	}
	if (strcmp(argv[1], "install") == 0 && (!argv[2] || strcmp(argv[2], "--dry-run") != 0) && setup_write_config) {
		char text[128];

		snprintf(text, sizeof(text), "bdfs=0000:03:00.0\n");
		writef(setup_write_config, "%s", text);
	}
	return 0;
}

static int run_setup(const char *root, const char *answers, const struct gs_install_opts *o, int driver_present,
		     const char *config_path, const char *destdir, char **out)
{
	struct gs_host h;
	struct gs_setup_host s;
	int in[2] = { -1, -1 }, pipefd[2];
	pid_t pid;

	memset(&h, 0, sizeof(h));
	h.sys = root;
	h.proc = root;
	h.state_dir = root;
	h.run_dir = root;
	h.root = root;
	h.run = setup_run;
	memset(&s, 0, sizeof(s));
	s.h = &h;
	s.in = -1;
	s.driver_present = driver_present;
	s.config = config_path;
	if (answers) {
		if (pipe(in) || write(in[1], answers, strlen(answers)) != (ssize_t)strlen(answers))
			die("pipe");
		close(in[1]);
		s.in = in[0];
	}
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		if (destdir)
			setenv("DESTDIR", destdir, 1);
		else
			unsetenv("DESTDIR");
		rc = gs_setup(&s, o);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	if (answers)
		close(in[0]);
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

static void setup_opts(struct gs_install_opts *o, const char *driver)
{
	memset(o, 0, sizeof(*o));
	o->ov.ari = o->ov.msix = GS_SW_UNSET;
	o->driver_file = driver;
	o->accept_nvidia_license = 1; /* so the version question, not the licence one, comes first */
	o->ndevices = 1;
}

static void test_version_setup(void)
{
	char root[GS_PATH_MAX], pkg[GS_PATH_MAX], cfg[GS_PATH_MAX], answer[64], *out, *log;
	struct gs_install_opts o;

	snprintf(root, sizeof(root), "%s/ver-setup", tmp);
	make_card(root, &topo_default, gpu_ampere, 0, NULL);
	sim_reset(root);
	sim_writef("proc/driver/nvidia/version",
		   "NVRM version: NVIDIA UNIX x86_64 Kernel Module  595.91.04  Release Build\n");
	gs_pathf(pkg, sizeof(pkg), "%s/NVIDIA-Linux-x86_64-595.91.04-vgpu-kvm.run", root);
	writef(pkg, "#!/bin/sh\nexit 0\n");
	gs_pathf(cfg, sizeof(cfg), "%s/no-config", root);
	gs_pathf(setup_log, sizeof(setup_log), "%s/setup.log", root);
	setup_write_config = cfg;
	setup_opts(&o, pkg);
	gs_addr_parse("0000:03:00.0", &o.devices[0]);

	/* No prompt: refused with the setup sentence, before any step runs. */
	CHECK(run_setup(root, NULL, &o, 0, cfg, NULL, &out) == GS_EXIT_REFUSED &&
	      contains(out, "setup: NVIDIA's driver 595.91.04 is not guaranteed to work. Confirmed versions: 580.159.01. "
			    "Nothing changed. To continue without questions, pass --allow-unsupported-driver."),
	      "no-prompt refusal: %s", out);
	CHECK(!slurp(setup_log), "no-prompt refusal runs no step");
	free(out);

	/* The same run with that version in the table: no question, no refusal, and setup goes on to its steps. */
	{
		struct gs_confirmed_version two[2];
		size_t count;
		char *steps;

		/* The table's own row, and a synthetic second one. */
		two[0] = gs_confirmed_versions(&count)[0];
		two[1] = (struct gs_confirmed_version){
			.host = "595.91.04", .second = "595.91.05",
			.guest_ampere = "Linux 595.91.05, Windows 596.10", .guest_ada = "Linux 595.91.05, Windows 596.10",
		};
		gs_confirmed_versions_set(two, 2);
		unlink(cfg);
		unlink(setup_log);
		CHECK(run_setup(root, NULL, &o, 0, cfg, NULL, &out) == GS_EXIT_OK &&
		      contains(out, "NVIDIA's driver 595.91.04 is confirmed to work.") && !contains(out, "595.91.04 is not guaranteed") &&
		      contains(out, "(with host driver 595.91.04)"), "a second confirmed row is honoured by setup: %s", out);
		steps = slurp(setup_log);
		CHECK(contains(steps, " install "), "and setup runs its steps: %s", steps ? steps : "");
		CHECK(contains(steps, "--from-setup"), "setup marks the install it runs, so that install prints no closing "
		      "instructions of its own: %s", steps ? steps : "");
		free(steps);
		free(out);
		gs_confirmed_versions_set(NULL, 0);
		unlink(cfg);
		unlink(setup_log);
	}

	/* End of input is a no, naming the flag that answers it. */
	unlink(setup_log);
	CHECK(run_setup(root, "", &o, 0, cfg, NULL, &out) == GS_EXIT_REFUSED &&
	      contains(out, "Continue? [y/N]") && contains(out, "setup: end of input. Nothing changed. Use "
							    "--allow-unsupported-driver instead."),
	      "EOF at the version question: %s", out);
	log = slurp(setup_log);
	CHECK(!log, "EOF runs no step: %s", log ? log : "");
	free(log);
	free(out);

	/* A yes continues: install --dry-run, install, activate, check, in that order. */
	unlink(setup_log);
	unlink(cfg);
	CHECK(run_setup(root, "\ny\n", &o, 0, cfg, NULL, &out) == GS_EXIT_OK && contains(out, "so CUDA programs can run on the host? [y/N]") && !contains(out, "--host-cuda"), "host CUDA left at no, then yes: %s", out);
	log = slurp(setup_log);
	CHECK(contains(log, " install --dry-run ") && contains(log, " install ") &&
	      contains(log, " activate --boot") && contains(log, " check"),
	      "steps after yes: %s", log ? log : "");
	CHECK(contains(out, "  Continuing uses best effort; input and safety checks still apply. Default is no."),
	      "best-effort notice after yes: %s", out);
	free(log);
	free(out);

	/* --allow-unsupported-driver answers it without a question. */
	unlink(setup_log);
	o.allow_unsupported_driver = 1;
	CHECK(run_setup(root, NULL, &o, 0, cfg, NULL, &out) == GS_EXIT_OK &&
	      contains(out, "Continuing uses best effort; input and safety checks still apply. Default is no.") &&
	      !contains(out, "Continue? [y/N]"), "allow flag: %s", out);
	free(out);
	o.allow_unsupported_driver = 0;

	/* A confirmed version gets no warning and no question. */
	{
		char okpkg[GS_PATH_MAX], okcfg[GS_PATH_MAX];

		gs_pathf(okpkg, sizeof(okpkg), "%s/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run", root);
		writef(okpkg, "#!/bin/sh\nexit 0\n");
		gs_pathf(okcfg, sizeof(okcfg), "%s/ok-config", root);
		unlink(okcfg);
		setup_write_config = okcfg;
		setup_opts(&o, okpkg);
		gs_addr_parse("0000:03:00.0", &o.devices[0]);
		answer[0] = '\0';
		CHECK(run_setup(root, NULL, &o, 0, okcfg, NULL, &out) == GS_EXIT_OK &&
		      contains(out, "NVIDIA's driver 580.159.01 is confirmed to work.") &&
		      !contains(out, "580.159.01 is not guaranteed to work") && !contains(out, "Continue?"), "confirmed setup: %s",
		      out);
		free(out);
	}
	(void)answer;
}

/* ---- the loaded display patch (idx 39) --------------------------------- */

/* An ELF64 core image whose one PT_LOAD segment maps vaddr to file offset 0x1000. */
static void make_kcore(const char *path, unsigned long long vaddr, const unsigned char *bytes, size_t len)
{
	Elf64_Ehdr eh;
	Elf64_Phdr ph;
	unsigned char page[0x2000];
	FILE *f;

	memset(&eh, 0, sizeof(eh));
	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_phoff = sizeof(eh);
	eh.e_phentsize = sizeof(ph);
	eh.e_phnum = 1;
	memset(&ph, 0, sizeof(ph));
	ph.p_type = PT_LOAD;
	ph.p_offset = 0x2000;
	ph.p_vaddr = vaddr;
	ph.p_filesz = 0x1000;
	ph.p_memsz = 0x1000;
	memset(page, 0, sizeof(page));
	memcpy(page, &eh, sizeof(eh));
	memcpy(page + sizeof(eh), &ph, sizeof(ph));
	f = fopen(path, "wb");
	if (!f)
		die(path);
	if (fwrite(page, 1, sizeof(page), f) != sizeof(page))
		die(path);
	{
		unsigned char payload[0x1000];

		memset(payload, 0, sizeof(payload));
		if (len > sizeof(payload) - 0x4f)
			len = sizeof(payload) - 0x4f;
		memcpy(payload + 0x4f, bytes, len);
		if (fwrite(payload, 1, sizeof(payload), f) != sizeof(payload))
			die(path);
	}
	fclose(f);
}

/* A /proc fixture for the verifier; the patch bytes are at record offset 0x4f of vaddr. */
static void patch_proc(const char *root, const char *kallsyms, const char *modules, const unsigned char *bytes,
		       size_t len)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/proc", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/proc/kallsyms", root);
	writef(p, "%s", kallsyms);
	gs_pathf(p, sizeof(p), "%s/proc/modules", root);
	writef(p, "%s", modules ? modules : "nvidia 123 0 - Live 0x0000000000000000\n");
	gs_pathf(p, sizeof(p), "%s/proc/kcore", root);
	make_kcore(p, 0xffffffff81000000ull, bytes, len);
}

static void patch_case(const char *name, const char *record_extra, const char *kallsyms, const char *modules,
		       int have_kcore, const unsigned char *bytes, size_t len, enum gs_loaded_patch want,
		       const char *why_part)
{
	char root[GS_PATH_MAX], record[GS_PATH_MAX], p[GS_PATH_MAX], text[1024], proc[GS_PATH_MAX];
	struct gs_build b;
	struct gs_loaded_patch_result r;

	snprintf(root, sizeof(root), "%s/%s", tmp, name);
	gs_mkdir_p(root, 0755);
	gs_pathf(record, sizeof(record), "%s/driver-build", root);
	if (record_extra)
		snprintf(text, sizeof(text),
			 "kernel=6.8.0\nidx39_function=_nv048804rm\nidx39_function_offset=79\n%s", record_extra);
	else
		snprintf(text, sizeof(text), "kernel=6.8.0\nidx39_function=_nv048804rm\nidx39_function_offset=79\n");
	writef(record, "%s", text);
	patch_proc(root, kallsyms, modules, bytes, len);
	if (!have_kcore) {
		gs_pathf(p, sizeof(p), "%s/proc/kcore", root);
		unlink(p);
	}
	CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT && b.record_read, "%s: record", name);
	gs_pathf(proc, sizeof(proc), "%s/proc", root);
	memset(&r, 0, sizeof(r));
	CHECK(gs_loaded_patch_verify(proc, &b, &r) == 0, "%s: verifier runs", name);
	CHECK(r.state == want, "%s: state=%d want=%d why=%s", name, r.state, want, r.why);
	if (why_part)
		CHECK(contains(r.why, why_part), "%s: why=%s", name, r.why);
	if (want == GS_LOADED_MATCH)
		CHECK(r.found_len == 5 && r.found[0] == 0x31 && r.offset == 0x4f, "%s: bytes %zu", name, r.found_len);
}

static void test_loaded_patch(void)
{
	static const unsigned char patched[] = { 0x31, 0xc0, 0x90, 0x90, 0x90 };
	static const unsigned char unpatched[] = { 0xe8, 0x00, 0x00, 0x00, 0x00 };
	static const unsigned char other[] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	static const char symbols[] = "ffffffff81000000 T _nv048804rm\nffffffff82000000 T other\n";

	patch_case("patch-match", NULL, symbols, NULL, 1, patched, sizeof(patched), GS_LOADED_MATCH, NULL);
	patch_case("patch-unpatched", NULL, symbols, NULL, 1, unpatched, sizeof(unpatched), GS_LOADED_MISMATCH,
		   "holds the unpatched call instruction");
	patch_case("patch-other", NULL, symbols, NULL, 1, other, sizeof(other), GS_LOADED_MISMATCH,
		   "holds aa bb cc dd ee, not the patched instruction");
	patch_case("patch-override", NULL, "ffffffff81000000 T _nv048804rm\n",
		   "nvidia 123 0 - Live 0x0000000000000000\n"
		   "vgx_override 16384 0 - Live 0x0000000000000000\n",
		   1, patched, sizeof(patched), GS_LOADED_MISMATCH, "vgx_override is loaded");
	patch_case("patch-nosym", NULL, "ffffffff82000000 T other\n", NULL, 1, patched, sizeof(patched),
		   GS_LOADED_UNKNOWN, "is not in kallsyms");
	patch_case("patch-zero", NULL, "0000000000000000 T _nv048804rm\n", NULL, 1, patched, sizeof(patched),
		   GS_LOADED_UNKNOWN, "kptr_restrict");
	patch_case("patch-nokcore", NULL, symbols, NULL, 0, patched, sizeof(patched), GS_LOADED_UNKNOWN,
		   "cannot be opened");

	/* A record written by the driver step stores the offset in decimal: 79 is 0x4f, not 0x79. */
	{
		char root[GS_PATH_MAX], record[GS_PATH_MAX], proc[GS_PATH_MAX];
		struct gs_build b;
		struct gs_loaded_patch_result r;

		snprintf(root, sizeof(root), "%s/patch-decimal", tmp);
		gs_mkdir_p(root, 0755);
		gs_pathf(record, sizeof(record), "%s/driver-build", root);
		writef(record, "kernel=6.8.0\nidx39_function=_nv048804rm\nidx39_function_offset=79\n");
		patch_proc(root, symbols, NULL, patched, sizeof(patched));
		CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT, "decimal record reads");
		gs_pathf(proc, sizeof(proc), "%s/proc", root);
		memset(&r, 0, sizeof(r));
		CHECK(gs_loaded_patch_verify(proc, &b, &r) == 0 && r.state == GS_LOADED_MATCH && r.offset == 0x4f,
		      "decimal offset 79 must read as 0x4f: state=%d offset=0x%llx why=%s", r.state,
		      (unsigned long long)r.offset, r.why);
	}

	/* A record with no idx39 lines at all: the fields that locate the patch are missing. */
	{
		char root[GS_PATH_MAX], record[GS_PATH_MAX], proc[GS_PATH_MAX];
		struct gs_build b;
		struct gs_loaded_patch_result r;

		snprintf(root, sizeof(root), "%s/patch-norecord", tmp);
		gs_mkdir_p(root, 0755);
		gs_pathf(record, sizeof(record), "%s/driver-build", root);
		writef(record, "kernel=6.8.0\nidx39_function_offset=79\n");
		patch_proc(root, symbols, NULL, patched, sizeof(patched));
		CHECK(gs_driver_build_read(record, "6.8.0", NULL, &b) == GS_BUILD_PRESENT, "record without function");
		gs_pathf(proc, sizeof(proc), "%s/proc", root);
		gs_loaded_patch_verify(proc, &b, &r);
		CHECK(r.state == GS_LOADED_UNKNOWN &&
		      contains(r.why, "has no idx39_function and idx39_function_offset lines"),
		      "missing function line: %s", r.why);
	}
}

/* ---- migration of 0.2.x artifacts (staged) ----------------------------- */

/* A manifest line for an existing file: "f <sha256> <absolute path>". */
static void manifest_add_file(const char *stage, const char *rel)
{
	char full[GS_PATH_MAX], manifest[GS_PATH_MAX], sha[65];
	FILE *f;

	gs_pathf(full, sizeof(full), "%s%s", stage, rel);
	if (gs_sha256_file(full, sha))
		die(full);
	gs_pathf(manifest, sizeof(manifest), "%s" GS_MANIFEST, stage);
	f = fopen(manifest, "a");
	if (!f)
		die(manifest);
	fprintf(f, "f %s %s\n", sha, rel);
	fclose(f);
}

static void stage_install(char **out, const char *stage)
{
	const char *args[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr", NULL };

	CHECK(run_cli(out, NULL, args) == GS_EXIT_OK, "staged install: %s", *out);
}

/*
 * The installed copy, as `install` lays it out: the binary beside geforce-sriov-driver and
 * geforce-sriov-patch, with no build tree. `install` from there names rebuild, and rebuild reaches the
 * driver step. Neither looks for the build tree's driver/geforce-sriov-patch.
 */
static void test_rebuild(void)
{
	char stage[GS_PATH_MAX], installed[GS_PATH_MAX], p[GS_PATH_MAX], script[GS_PATH_MAX + 1024], *out, *probe;
	const char *built = cli;
	const char *again[] = { "install", NULL };
	const char *nonroot[] = { "rebuild", NULL };
	const char *badopt[] = { "rebuild", "--kernel", "a/b", NULL };
	int ns;

	gs_pathf(stage, sizeof(stage), "%s/stage-rebuild", tmp);
	stage_install(&out, stage);
	free(out);
	gs_pathf(installed, sizeof(installed), "%s/usr/bin/geforce-sriov", stage);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov-patch", stage);
	CHECK(gs_exists(installed) && gs_exists(p), "the staged layout has the binary and the patch tool side by side");
	gs_pathf(p, sizeof(p), "%s/usr/bin/driver", stage);
	CHECK(!gs_exists(p), "and no build tree");

	cli = installed;
	CHECK(run_cli(&out, NULL, again) == GS_EXIT_REFUSED && contains(out, "is the installed copy") &&
	      contains(out, "After a kernel update, run `sudo geforce-sriov rebuild`") && !contains(out, "run make first"),
	      "install from the installed copy names rebuild: %s", out);
	free(out);
	CHECK(run_cli(&out, NULL, badopt) == GS_EXIT_USAGE, "rebuild --kernel takes a release: %s", out);
	free(out);
	if (geteuid() != 0) {
		CHECK(run_cli(&out, NULL, nonroot) == GS_EXIT_REFUSED && contains(out, "rebuild must run as root"),
		      "rebuild as a user: %s", out);
		free(out);
	}
	cli = built;
	CHECK(run_cli(&out, NULL, badopt) == GS_EXIT_USAGE, "the built binary parses rebuild too: %s", out);
	free(out);

	/* As root in a private namespace: the installed rebuild runs the driver step, which refuses for a kernel that has no headers. */
	ns = run_in_ns(NS_PREFIX "true", &probe);
	if (ns != 0) {
		SKIP_ENV("unprivileged user namespaces are unavailable; run as root or with "
			 "kernel.apparmor_restrict_unprivileged_userns=0 to cover this",
			 "%s", probe ? probe : "unshare failed");
		free(probe);
	} else {
		free(probe);
		snprintf(script, sizeof(script),
			 NS_PREFIX "GEFORCE_SRIOV_CONFIG=/var/lib/no-config '%s' rebuild --kernel gs-no-such-kernel; echo RC=$?; "
			 "ls /var/lib/geforce-sriov | sed 's/^/STATE:/'", installed);
		CHECK(run_in_ns(script, &out) == 0 && contains(out, "RC=3") && !contains(out, "run make first") &&
		      contains(out, "/usr/bin/geforce-sriov-driver --retained --kernel gs-no-such-kernel") &&
		      !contains(out, "--fb-placement") && !contains(out, "--ada-selected") && !contains(out, "placement") &&
		      contains(out, "geforce-sriov-driver: refused: ") &&
		      contains(out, "NVIDIA's module and plugin for gs-no-such-kernel are as they were") &&
		      contains(out, "run `sudo geforce-sriov rebuild --kernel gs-no-such-kernel` again") && !contains(out, "STATE:driver-build") &&
		      !contains(out, "STATE:inputs"),
		      "the installed rebuild reaches the driver step and changes nothing: %s", out);
		free(out);
	}

	/* The index of kept inputs, as the host checks and setup read it. */
	gs_pathf(p, sizeof(p), "%s/kept/state/inputs", tmp);
	gs_mkdir_p(p, 0755);
	CHECK(!gs_kept_input(p, "companion"), "no index: nothing is kept");
	gs_pathf(script, sizeof(script), "%s/index", p);
	writef(script, "nvidia_source=/usr/src/nvidia-580.159.01\ncompanion=/x/companion/a.run\ncompanion_kind=file\nnvkms=\n");
	CHECK(gs_kept_input(p, "companion") && !gs_kept_input(p, "nvkms") && !gs_kept_input(p, "companion_k"),
	      "the index names the second package and no public source");

	/* uninstall removes the kept inputs with the rest, and the state directory once it is empty. */
	gs_pathf(script, sizeof(script), "%s/companion", p);
	gs_mkdir_p(script, 0755);
	gs_pathf(script, sizeof(script), "%s/companion/a.run", p);
	writef(script, "kept package\n");
	gs_pathf(script, sizeof(script), "%s/kept/state/installed-files", tmp);
	writef(script, "%s", "");
	gs_pathf(installed, sizeof(installed), "%s/kept/modprobe.d", tmp);
	gs_mkdir_p(installed, 0755);
	fflush(stdout);
	CHECK(gs_uninstall_files(script, installed, NULL) == GS_EXIT_OK && !gs_exists(p), "uninstall removes the kept inputs");
	gs_pathf(p, sizeof(p), "%s/kept/state", tmp);
	CHECK(!gs_exists(p), "and the state directory after them");
}

/* ---- a first install, as a newcomer sees it ---------------------------- */

static struct gs_diag_opts doctor_opts;
static int doctor_host(void)
{
	return gs_hostcheck(&doctor_opts, GS_HOSTCHECK_POST);
}

static struct gs_diag_opts hostcheck_opts;
static enum gs_hostcheck_mode hostcheck_mode;
static int hostcheck_host(void)
{
	return gs_hostcheck(&hostcheck_opts, hostcheck_mode);
}

/* gs_hostcheck on a fixture root, as install --dry-run (PRE) or status (POST) runs it; extra: the options install took. */
static int run_hostcheck(char **out, enum gs_hostcheck_mode mode, const char *root, const char *cfg,
			 const struct gs_diag_opts *extra)
{
	memset(&hostcheck_opts, 0, sizeof(hostcheck_opts));
	if (extra)
		hostcheck_opts = *extra;
	hostcheck_opts.sysroot = root;
	hostcheck_opts.config_path = cfg;
	hostcheck_mode = mode;
	return run_host(hostcheck_host, out);
}

static struct gs_config next_cfg;
static int next_from_setup;
static int install_next_host(void)
{
	gs_install_next(&next_cfg, next_from_setup);
	return 0;
}

static const char *un_manifest, *un_modprobe;
static int uninstall_files_host(void)
{
	return gs_uninstall_files(un_manifest, un_modprobe, NULL);
}

/*
 * NVIDIA's package is a script that root runs. It is run from a copy that only root can change,
 * and the copy is what is hashed: whoever owns the file or its directory cannot put another script
 * there between the check and the run.
 */
static char pkg_file[GS_PATH_MAX], pkg_base[GS_PATH_MAX], pkg_root[GS_PATH_MAX];

static int package_host(void)
{
	char dir[GS_PATH_MAX];
	int changed;

	return gs_nvidia_package(pkg_file, pkg_base, pkg_root, 0, dir, sizeof(dir), &changed);
}

/* 1 when dir holds an entry whose name starts with prefix. */
static int dir_has_prefix(const char *dir, const char *prefix)
{
	struct dirent *de;
	DIR *d = opendir(dir);
	int found = 0;

	while (d && (de = readdir(d)))
		found |= strncmp(de->d_name, prefix, strlen(prefix)) == 0;
	if (d)
		closedir(d);
	return found;
}

static void test_package_copy(void)
{
	char p[GS_PATH_MAX], ran[GS_PATH_MAX], state[GS_PATH_MAX], *out, *text;
	static const char body[] = "#!/bin/sh\nprintf '%%s\\n' \"$0\" >>%s\n"
				   "[ \"$1\" != --extract-only ] || mkdir -p \"$3/kernel\"\nexit 0\n";

	gs_pathf(p, sizeof(p), "%s/pkgcopy/given", tmp);
	gs_mkdir_p(p, 0755);
	gs_pathf(state, sizeof(state), "%s/pkgcopy/state", tmp);
	gs_pathf(pkg_base, sizeof(pkg_base), "%s/nvidia", state);
	gs_pathf(pkg_root, sizeof(pkg_root), "%s/pkgcopy/root", tmp);
	gs_pathf(ran, sizeof(ran), "%s/pkgcopy/ran", tmp);
	gs_pathf(p, sizeof(p), "%s/usr/lib/x86_64-linux-gnu", pkg_root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.999.0.1", pkg_root);
	writef(p, "%s", "library\n");

	/* A package named as a confirmed version with another digest: not copied into place, not run. */
	gs_pathf(pkg_file, sizeof(pkg_file), "%s/pkgcopy/given/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run", tmp);
	writef(pkg_file, body, ran);
	CHECK(run_host(package_host, &out) == GS_EXIT_REFUSED && contains(out, "It was not run."), "pinned name, other digest: %s", out);
	CHECK(!gs_exists(ran) && !gs_exists(state), "nothing ran and nothing is left behind");
	free(out);

	/* A package without a recorded digest: the copy is what runs, and it is removed afterwards. */
	gs_pathf(pkg_file, sizeof(pkg_file), "%s/pkgcopy/given/NVIDIA-Linux-x86_64-999.0.1-vgpu-kvm.run", tmp);
	writef(pkg_file, body, ran);
	CHECK(run_host(package_host, &out) == GS_EXIT_OK &&
	      contains(out, "What is run below is a copy of that file that only root can change: ") &&
	      contains(out, "/pkgcopy/state/.package/"), "the copy is named: %s", out);
	text = slurp(ran);
	CHECK(text && contains(text, "/pkgcopy/state/.package/") && contains(text, "/NVIDIA-Linux-x86_64-999.0.1-vgpu-kvm.run\n") &&
	      !contains(text, "/pkgcopy/given/"), "only the copy was run: %s", text ? text : "(not run)");
	free(text);
	gs_pathf(p, sizeof(p), "%s/999.0.1/kernel", pkg_base);
	CHECK(gs_exists(p) && !dir_has_prefix(state, ".package"), "the extraction is in place and the copy is gone");
	free(out);

	/* The file given is a link to the package: the link is resolved once, and the copy is still what runs. */
	gs_pathf(p, sizeof(p), "%s/pkgcopy/given/link.run", tmp);
	if (symlink(pkg_file, p))
		die(p);
	gs_copy(pkg_file, sizeof(pkg_file), p);
	unlink(ran);
	gs_pathf(p, sizeof(p), "%s/999.0.1", pkg_base);
	rm_rf(p);
	/* A run that was killed left its copy behind: the next run removes it first. */
	gs_pathf(p, sizeof(p), "%s/.package/left-behind.run", state);
	writef(p, "%s", "left\n");
	CHECK(run_host(package_host, &out) == GS_EXIT_OK, "a link to the package: %s", out);
	text = slurp(ran);
	CHECK(text && contains(text, "/pkgcopy/state/.package/") && !contains(text, "/pkgcopy/given/"),
	      "a link to the package: only the copy was run: %s", text ? text : "(not run)");
	CHECK(!dir_has_prefix(state, ".package"), "what an earlier run left behind is gone too");
	free(text);
	free(out);
}

static void test_first_install(void)
{
	char sysroot[GS_PATH_MAX], state[GS_PATH_MAX], record[GS_PATH_MAX], manifest[GS_PATH_MAX], units[GS_PATH_MAX];
	char p[GS_PATH_MAX], *out;
	struct gs_build_src src;

	/*
	 * The checks after install, as status runs them, on a host that has not been installed yet: what install is about to put
	 * in place is noted, not warned about, and nothing is said about a build record that does not exist.
	 */
	fixture("doctor-fresh", &topo_display, gpu_display, 1, NULL, sysroot, sizeof(sysroot));
	gs_pathf(state, sizeof(state), "%s/doctor-state", tmp);
	gs_mkdir_p(state, 0755);
	gs_pathf(record, sizeof(record), "%s/driver-build", state);
	gs_pathf(manifest, sizeof(manifest), "%s/" GS_MANIFEST_NAME, state);
	gs_pathf(units, sizeof(units), "%s/doctor-units", tmp);
	gs_mkdir_p(units, 0755);
	memset(&src, 0, sizeof(src));
	src.record = record;
	src.release = "6.8.0";
	src.module = "/nonexistent/nvidia.ko";
	memset(&doctor_opts, 0, sizeof(doctor_opts));
	doctor_opts.sysroot = sysroot;
	gs_pathf(p, sizeof(p), "%s/no-config", state);
	doctor_opts.config_path = p;
	doctor_opts.build = &src;
	doctor_opts.unit_dir = units;
	doctor_opts.ndevices = 1;
	CHECK(gs_addr_parse("0000:04:00.0", &doctor_opts.devices[0]) == 0, "the fixture's device parses");
	run_host(doctor_host, &out);
	CHECK(contains(out, "[INFO] no vgpud drop-in yet; install writes nvidia-vgpud.service.d/") &&
	      !contains(out, "no vgpud drop-in found"), "no install yet: the drop-in is noted, not warned about: %s", out);
	CHECK(contains(out, "[INFO] NVIDIA's module has not been built on this host yet; install builds it"),
	      "no install yet: the build is noted: %s", out);
	CHECK(!contains(out, "the build record does not") && !contains(out, "build record: no confirmation line") &&
	      !contains(out, "to record it"), "no install yet: nothing is said about a record that does not exist: %s", out);
	if (!gs_unit_file_present(GS_UNIT_ACTIVATE))
		CHECK(contains(out, "[INFO] " GS_UNIT_ACTIVATE " is not installed yet; install installs it") &&
		      !contains(out, "nothing brings the GPUs up at boot"), "no install yet: the unit is noted: %s", out);
	free(out);

	/* The same host after an install (its manifest is there): what is missing is a warning, as before. */
	writef(manifest, "%s", "");
	run_host(doctor_host, &out);
	CHECK(contains(out, "[WARN] no vgpud drop-in found: vgpud may start before the VFs are configured") &&
	      !contains(out, "no vgpud drop-in yet"), "installed host: a missing drop-in is a warning: %s", out);
	CHECK(contains(out, "[WARN] nvidia.ko for 6.8.0 is not known to be the build geforce-sriov installed"),
	      "installed host: a missing build is a warning: %s", out);
	if (!gs_unit_file_present(GS_UNIT_ACTIVATE))
		CHECK(contains(out, "[WARN] " GS_UNIT_ACTIVATE " is not installed: nothing brings the GPUs up at boot"),
		      "installed host: a missing unit is a warning: %s", out);
	free(out);

	/* And with a record that names no features, the record lines are back. */
	writef(record, "kernel=6.8.0\n");
	run_host(doctor_host, &out);
	CHECK(contains(out, "[WARN] display is requested and the build record does not say whether the display stack is installed"),
	      "installed host with an old record: the display line stays: %s", out);
	free(out);

	/* install alone: one closing block, which does not send the user to setup. */
	gs_config_defaults(&next_cfg);
	next_from_setup = 0;
	run_host(install_next_host, &out);
	CHECK(contains(out, "Next: power the machine off and on.") && !contains(out, "geforce-sriov doctor") &&
	      contains(out, "After the boot, `geforce-sriov status` shows each GPU and its next step.") &&
	      !contains(out, "geforce-sriov setup"), "install alone: its own instructions, no word of setup: %s", out);
	free(out);
	/* install under setup: no instructions; setup's closing lines are the only ones. */
	next_from_setup = 1;
	run_host(install_next_host, &out);
	CHECK(contains(out, "are enabled for the next boot, not started.") && contains(out, "geforce_sriov.skip") &&
	      !contains(out, "Next:") && !contains(out, "After the boot") && !contains(out, "setup"),
	      "install under setup: no closing instructions: %s", out);
	free(out);

	/* uninstall says what it leaves: NVIDIA's package as --driver-file extracted it. */
	gs_pathf(state, sizeof(state), "%s/un-state", tmp);
	gs_pathf(p, sizeof(p), "%s/nvidia/580.159.01/kernel", state);
	gs_mkdir_p(p, 0755);
	gs_pathf(manifest, sizeof(manifest), "%s/" GS_MANIFEST_NAME, state);
	writef(manifest, "%s", "");
	gs_pathf(units, sizeof(units), "%s/un-modprobe.d", tmp);
	gs_mkdir_p(units, 0755);
	un_manifest = manifest;
	un_modprobe = units;
	gs_pathf(p, sizeof(p), "kept %s/nvidia/: NVIDIA's package as --driver-file extracted it, and the only thing left in %s.",
		 state, state);
	gs_pathf(record, sizeof(record), "`rm -r %s` removes both", state);
	CHECK(run_host(uninstall_files_host, &out) == GS_EXIT_OK && contains(out, p) && contains(out, record),
	      "uninstall names the extraction it leaves, alone in the state directory: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/nvidia/580.159.01", state);
	CHECK(gs_exists(p), "and leaves it");
	/* With something else left beside it (the activation lock), the command names the folder only. */
	writef(manifest, "%s", "");
	gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, state);
	writef(p, "boot_id=x\n");
	gs_pathf(p, sizeof(p), "delete it with `rm -r %s/nvidia` when you do not need it.", state);
	CHECK(run_host(uninstall_files_host, &out) == GS_EXIT_OK && contains(out, p) && !contains(out, "the only thing left"),
	      "uninstall names the extraction beside other kept files: %s", out);
	free(out);
	/* No extraction: no such line. */
	gs_pathf(p, sizeof(p), "%s/nvidia", state);
	rm_rf(p);
	writef(manifest, "%s", "");
	CHECK(run_host(uninstall_files_host, &out) == GS_EXIT_OK && !contains(out, "--driver-file extracted"),
	      "no extraction, no line about it: %s", out);
	free(out);
	/* The version stamp goes with a finished uninstall only: a failed step (a restore, a unit, dkms remove) keeps it. */
	gs_pathf(p, sizeof(p), "%s/" GS_FORMAT_NAME, state);
	writef(p, GS_VERSION "\n");
	gs_format_release(state, GS_EXIT_FAIL);
	CHECK(gs_exists(p), "a failed uninstall keeps the version stamp, so a re-install of this version is not refused");
	gs_format_release(state, GS_EXIT_OK);
	CHECK(!gs_exists(p), "a finished uninstall removes the version stamp");
}

/* A staged host of an older install: the files an uninstall of any version removes, without the stamp of this one. */
static void older_stage(const char *name, char *stage, size_t len, int record, int dkms, int program, int link)
{
	char p[GS_PATH_MAX], wants[GS_PATH_MAX];

	snprintf(stage, len, "%s/%s", tmp, name);
	if (record) {
		char sha[65], rec[256];

		/* A record whose module is still installed with the recorded digest: a restore would have work to do. */
		gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.0/updates/dkms/nvidia.ko", stage);
		writef(p, "patched module\n");
		gs_sha256_buf("patched module\n", 15, sha);
		snprintf(rec, sizeof(rec), "kernel=7.0.0\nmodule_sha256=%s\nmodule_build_id=00\nstock_sha256=00\n", sha);
		gs_pathf(p, sizeof(p), "%s" GS_DRIVER_BUILD, stage);
		writef(p, "%s", rec);
	}
	if (dkms) {
		gs_pathf(p, sizeof(p), "%s/var/lib/dkms/" GS_DKMS_NAME "/0.3.0", stage);
		gs_mkdir_p(p, 0755);
	}
	if (program) {
		gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
		writef(p, "#!/bin/sh\n");
		gs_pathf(p, sizeof(p), "%s/var/lib/geforce-sriov", stage);
		gs_mkdir_p(p, 0755);
		manifest_add_file(stage, "/usr/bin/geforce-sriov");
	}
	if (link) {
		gs_pathf(p, sizeof(p), "%s" GS_UNIT_WANTS_LIVEPATCH, stage);
		gs_pathf(wants, sizeof(wants), "%s/etc/systemd/system/sysinit.target.wants", stage);
		gs_mkdir_p(wants, 0755);
		link_to("../" GS_UNIT_LIVEPATCH, p);
	}
}

/* install over a host that another version installed stops before any write; the same version, and a host the uninstall emptied, do not. */
static void test_older_install(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], wants[GS_PATH_MAX], *out, *text;
	const char *args[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr", NULL };
	static const struct { const char *name; int record, dkms, program, link; const char *what; } signs[] = {
		{ "old-all", 1, 1, 1, 1, "a host as 0.3.0 left it" },
		{ "old-disabled", 1, 1, 1, 0, "units disabled by hand: the record, DKMS and the program remain" },
		{ "old-record", 1, 0, 0, 0, "the build record alone" },
		{ "old-dkms", 0, 1, 0, 0, "the DKMS registration alone" },
		{ "old-program", 0, 0, 1, 0, "the installed program alone" },
		{ "old-link", 0, 0, 0, 1, "the enabled boot unit alone" },
	};

	for (size_t i = 0; i < sizeof(signs) / sizeof(signs[0]); i++) {
		older_stage(signs[i].name, stage, sizeof(stage), signs[i].record, signs[i].dkms, signs[i].program, signs[i].link);
		CHECK(run_cli(&out, NULL, args) == GS_EXIT_REFUSED &&
		      contains(out, "refused, nothing changed: another version of geforce-sriov than " GS_VERSION " is installed") &&
		      contains(out, "Run the installed version's `sudo geforce-sriov uninstall`") &&
		      contains(out, "power the machine off and on, then run this install again"),
		      "%s: refused with the one instruction: %s", signs[i].what, out);
		free(out);
		gs_pathf(p, sizeof(p), "%s" GS_FORMAT_PATH, stage);
		CHECK(!gs_exists(p), "%s: no stamp written", signs[i].what);
		gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov-patch", stage);
		CHECK(!gs_exists(p), "%s: nothing installed", signs[i].what);
		gs_pathf(p, sizeof(p), "%s/usr/share/geforce-sriov/config.example", stage);
		CHECK(!gs_exists(p), "%s: no file written", signs[i].what);
	}
	/* The refusal says what the old uninstall removes and prints the installed settings worth noting. */
	older_stage("old-settings", stage, sizeof(stage), 1, 1, 1, 0);
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov", stage);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s" GS_CONFIG_PATH, stage);
	writef(p, "bdfs=0000:04:00.0,0000:03:00.0\ndisplay=off\nhost_cuda=on\nhost_desktop=off\nvfs=8\ntype_ada=900\nfb_placement=top\nfb_slots=4\nsome_future_key=1\n");
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_REFUSED && contains(out, "uninstall removes /etc/geforce-sriov (your card selection and settings") &&
	      contains(out, "  bdfs=0000:04:00.0,0000:03:00.0\n  display=off\n  host_cuda=on\n  host_desktop=off\n  vfs=8\n  type_ada=900\n") &&
	      !contains(out, "fb_placement"), "the refusal names the settings that the uninstall removes, and not the removed key: %s", out);
	free(out);
	/* setup stops the same way, before any step runs. */
	{
		struct gs_install_opts so;
		char cfg[GS_PATH_MAX];

		older_stage("old-setup", stage, sizeof(stage), 1, 1, 1, 0);
		gs_pathf(cfg, sizeof(cfg), "%s/no-config", stage);
		gs_pathf(setup_log, sizeof(setup_log), "%s/setup.log", stage);
		setup_write_config = cfg;
		setup_opts(&so, NULL);
		CHECK(run_setup(stage, NULL, &so, 0, cfg, stage, &out) == GS_EXIT_REFUSED &&
		      contains(out, "refused, nothing changed: another version of geforce-sriov than " GS_VERSION " is installed") &&
		      contains(out, "power the machine off and on, then run this install again"), "setup: refused: %s", out);
		CHECK(!slurp(setup_log), "setup: no step ran");
		free(out);
		/* Without DESTDIR the host's own root is read: a record under it refuses the same way. */
		CHECK(run_setup(stage, NULL, &so, 0, cfg, NULL, &out) == GS_EXIT_REFUSED &&
		      contains(out, "refused, nothing changed: another version of geforce-sriov"), "setup, live root: refused: %s", out);
		free(out);
	}
	/*
	 * A record that outlives a finished uninstall (the kernel was purged, or NVIDIA's module was replaced) is no sign:
	 * its restore had nothing to do. install goes through.
	 */
	older_stage("record-purged", stage, sizeof(stage), 1, 0, 0, 0);
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.0/updates/dkms/nvidia.ko", stage);
	unlink(p);
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "staged below"),
	      "a record of a purged kernel does not stop the install: %s", out);
	free(out);
	older_stage("record-replaced", stage, sizeof(stage), 1, 0, 0, 0);
	gs_pathf(p, sizeof(p), "%s/lib/modules/7.0.0/updates/dkms/nvidia.ko", stage);
	writef(p, "NVIDIA's own module, reinstalled\n");
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "staged below"),
	      "a record of a module that was replaced does not stop the install: %s", out);
	free(out);
	older_stage("record-archived", stage, sizeof(stage), 0, 0, 0, 0);
	gs_pathf(p, sizeof(p), "%s/var/lib/geforce-sriov/driver-builds/6.8.0-old", stage);
	writef(p, "kernel=6.8.0-old\nmodule_sha256=00\nmodule_build_id=00\nstock_sha256=00\n");
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "staged below"),
	      "an archived record of a kernel that is gone does not stop the install: %s", out);
	free(out);

	/* The way out is printed when the installed program is gone, and not when it is there. */
	older_stage("old-gone", stage, sizeof(stage), 1, 1, 0, 1);
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_REFUSED && contains(out, "If that program is gone") &&
	      contains(out, "dkms remove " GS_DKMS_NAME "/0.3.0 --all") && contains(out, "systemctl disable " GS_UNIT_LIVEPATCH) &&
	      contains(out, "--restore --kernel 7.0.0"), "the manual way out: %s", out);
	free(out);
	older_stage("old-here", stage, sizeof(stage), 1, 1, 1, 1);
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_REFUSED && !contains(out, "If that program is gone"),
	      "no manual lines while the installed program is there: %s", out);
	free(out);

	/* A long --prefix with every sign does not overrun the evidence buffer. */
	{
		char longp[GS_PATH_MAX], rel[1200] = "", bin[GS_PATH_MAX];
		const char *largs[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", rel, NULL };

		for (int k = 0; k < 40; k++)
			strcat(rel, "/abcdefghijklmnopqrstuvwxyz");
		older_stage("old-long", stage, sizeof(stage), 1, 1, 0, 1);
		gs_pathf(bin, sizeof(bin), "%s%s/bin", stage, rel);
		gs_mkdir_p(bin, 0755);
		gs_pathf(longp, sizeof(longp), "%s/geforce-sriov", bin);
		writef(longp, "#!/bin/sh\n");
		manifest_add_file(stage, longp + strlen(stage));
		CHECK(run_cli(&out, NULL, largs) == GS_EXIT_REFUSED && contains(out, "refused, nothing changed"),
		      "a long prefix with every sign is refused without overrunning: %s", out);
		free(out);
	}

	/* The 0.2.x unit's enablement link is a sign as well. */
	snprintf(stage, sizeof(stage), "%s/old-prepare", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/sysinit.target.wants", stage);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/sysinit.target.wants/geforce-sriov-prepare.service", stage);
	link_to("../geforce-sriov-prepare.service", p);
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_REFUSED && contains(out, "geforce-sriov-prepare.service"),
	      "the 0.2.x boot unit: %s", out);
	free(out);

	/* The stamp names another version: refused, and the version is named. */
	older_stage("other-version", stage, sizeof(stage), 1, 1, 1, 0);
	gs_pathf(p, sizeof(p), "%s" GS_FORMAT_PATH, stage);
	writef(p, "0.0.1\n");
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_REFUSED && contains(out, "version 0.0.1: "), "another version's stamp: %s", out);
	free(out);

	/*
	 * What an uninstall of any version leaves on purpose is not a sign: the config (with the removed fb_slots
	 * line), the manifest listing it and an edited unit, the activation lock, NVIDIA's extracted package, and
	 * a stale /run marker. The install goes through and writes the stamp.
	 */
	snprintf(stage, sizeof(stage), "%s/after-uninstall", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	writef(p, "bdfs=0000:03:00.0\ndisplay=auto\nfb_slots=off\n");
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/" GS_UNIT_ACTIVATE, stage);
	writef(p, "[Unit]\nDescription=edited by the user\n");
	gs_pathf(p, sizeof(p), "%s" GS_ATTEMPT_MARKER, stage);
	writef(p, "boot_id=x\n");
	gs_pathf(p, sizeof(p), "%s" GS_STATE_DIR "/nvidia/x", stage);
	writef(p, "x\n");
	manifest_add_file(stage, "/etc/geforce-sriov/config");
	manifest_add_file(stage, "/etc/systemd/system/" GS_UNIT_ACTIVATE);
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "staged below") &&
	      contains(out, "note: fb_slots was removed in 0.4.0 and is ignored; delete the line from " GS_CONFIG_PATH),
	      "after an uninstall that kept its config: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s" GS_FORMAT_PATH, stage);
	text = slurp(p);
	CHECK(text && strcmp(text, GS_VERSION "\n") == 0, "the stamp names this version: %s", text ? text : "(none)");
	free(text);

	/* An edited program that the uninstall kept on purpose (still in the kept manifest) is no sign. */
	older_stage("kept-program", stage, sizeof(stage), 0, 0, 1, 0);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	writef(p, "#!/bin/sh\n# edited by the user\n");
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "staged below"),
	      "a kept, edited program does not stop the install: %s", out);
	free(out);

	/* The same version again: nothing stops it, whatever it left (record, DKMS registration, program, enabled unit). */
	gs_pathf(p, sizeof(p), "%s/var/lib/dkms/" GS_DKMS_NAME "/" GS_VERSION, stage);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s" GS_DRIVER_BUILD, stage);
	writef(p, "kernel=7.0.0\nmodule_sha256=00\nmodule_build_id=00\nstock_sha256=00\n");
	gs_pathf(p, sizeof(p), "%s" GS_UNIT_WANTS_LIVEPATCH, stage);
	gs_pathf(wants, sizeof(wants), "%s/etc/systemd/system/sysinit.target.wants", stage);
	gs_mkdir_p(wants, 0755);
	link_to("../" GS_UNIT_LIVEPATCH, p);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	CHECK(gs_exists(p), "the program of the first install is there");
	CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "staged below") && !contains(out, "refused"),
	      "install again over the same version: %s", out);
	free(out);

	/* A plain staged install writes below DESTDIR only, and the manifest and the stamp are there. */
	snprintf(stage, sizeof(stage), "%s/stage-plain", tmp);
	stage_install(&out, stage);
	CHECK(contains(out, "staged below"), "a staged install stops at the staging boundary: %s", out);
	gs_pathf(p, sizeof(p), "%s/var/lib/geforce-sriov/installed-files", stage);
	CHECK(gs_exists(p), "the manifest is written below DESTDIR");
	gs_pathf(p, sizeof(p), "%s" GS_FORMAT_PATH, stage);
	CHECK(gs_exists(p), "the stamp is written below DESTDIR");
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	CHECK(gs_exists(p), "the binary is staged below DESTDIR");
	free(out);
}

/* ---- the kernel hook ---------------------------------------------------- */

static struct gs_diag_opts hook_status_opts;
static int hook_status_host(void)
{
	return gs_cmd_status(&hook_status_opts);
}

/*
 * install puts the hook where the distribution runs its kernel hooks (the image's and the headers'),
 * only on a host that has those directories; uninstall removes it through the manifest; status
 * reports a kernel whose automatic build failed, with the command that fixes it.
 */
static void test_kernel_hook(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], manifest[GS_PATH_MAX], modprobe[GS_PATH_MAX], sha[65];
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], marks[GS_PATH_MAX], *out, *text;
	static const char *const dirs[] = { "etc/kernel/postinst.d", "etc/kernel/header_postinst.d" };
	struct stat st;
	FILE *f;

	/* A Debian-style host: both directories exist, so both get the hook. */
	snprintf(stage, sizeof(stage), "%s/stage-hook", tmp);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/%s", stage, dirs[i]);
		gs_mkdir_p(p, 0755);
	}
	gs_pathf(p, sizeof(p), "%s/etc/apt/apt.conf.d", stage);
	gs_mkdir_p(p, 0755);
	stage_install(&out, stage);
	CHECK(contains(out, "kernel hook: a kernel update builds NVIDIA's module for the new kernel itself") &&
	      !contains(out, "this host has no /etc/kernel/postinst.d"), "install says the hook is in place: %s", out);
	CHECK(contains(out, "headers that apt installs after their kernel make the missing build too"), "install says the apt snippet is in place: %s", out);
	free(out);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/%s/geforce-sriov", stage, dirs[i]);
		text = slurp(p);
		CHECK(text && !stat(p, &st) && (st.st_mode & 0777) == 0755, "the hook in %s is executable", dirs[i]);
		CHECK(text && contains(text, "cli=/usr/bin/geforce-sriov\n") && contains(text, "state=/var/lib/geforce-sriov\n") &&
		      contains(text, "rebuild --kernel \"$kernel\"") && !contains(text, "@BINDIR@") && !contains(text, "@STATE@") &&
		      !contains(text, "@VERSION@"), "the hook in %s is rendered for the install", dirs[i]);
		free(text);
	}
	gs_pathf(p, sizeof(p), "%s" GS_MANIFEST, stage);
	text = slurp(p);
	CHECK(text && contains(text, " /etc/kernel/postinst.d/geforce-sriov\n") &&
	      contains(text, " /etc/kernel/header_postinst.d/geforce-sriov\n"), "the manifest owns both hooks: %s", text ? text : "");
	CHECK(text && contains(text, " /etc/apt/apt.conf.d/99geforce-sriov\n"), "the manifest owns the apt snippet: %s", text ? text : "");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/apt/apt.conf.d/99geforce-sriov", stage);
	text = slurp(p);
	CHECK(text && !stat(p, &st) && (st.st_mode & 0777) == 0644 && contains(text, "DPkg::Post-Invoke") &&
	      contains(text, "/etc/kernel/postinst.d/geforce-sriov --retry") && contains(text, "|| true"),
	      "the apt snippet runs the hook with --retry after dpkg and never fails apt: %s", text ? text : "");
	free(text);

	/* uninstall removes what the manifest lists: here a manifest of the staged copies, so that only they go. */
	gs_pathf(manifest, sizeof(manifest), "%s/hook-manifest", tmp);
	gs_pathf(modprobe, sizeof(modprobe), "%s/hook-modprobe.d", tmp);
	gs_mkdir_p(modprobe, 0755);
	f = fopen(manifest, "w");
	if (!f)
		die(manifest);
	for (size_t i = 0; i < 3; i++) {
		gs_pathf(p, sizeof(p), "%s/%s", stage, i < 2 ? dirs[i] : "etc/apt/apt.conf.d/99geforce-sriov");
		if (i < 2)
			strncat(p, "/geforce-sriov", sizeof(p) - strlen(p) - 1);
		if (gs_sha256_file(p, sha))
			die(p);
		fprintf(f, "f %s %s\n", sha, p);
	}
	fclose(f);
	fflush(stdout);
	CHECK(gs_uninstall_files(manifest, modprobe, NULL) == GS_EXIT_OK, "uninstall of the hooks succeeds");
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/%s/geforce-sriov", stage, dirs[i]);
		CHECK(!gs_exists(p), "uninstall removed the hook in %s", dirs[i]);
		gs_pathf(p, sizeof(p), "%s/%s", stage, dirs[i]);
		CHECK(gs_exists(p), "and left the distribution's directory %s", dirs[i]);
	}
	gs_pathf(p, sizeof(p), "%s/etc/apt/apt.conf.d/99geforce-sriov", stage);
	CHECK(!gs_exists(p), "uninstall removed the apt snippet");
	gs_pathf(p, sizeof(p), "%s/etc/apt/apt.conf.d", stage);
	CHECK(gs_exists(p), "and left apt's directory");

	/* A host without those directories gets no hook, and is told what to run instead. */
	snprintf(stage, sizeof(stage), "%s/stage-nohook", tmp);
	stage_install(&out, stage);
	CHECK(contains(out, "kernel hook: this host has no /etc/kernel/postinst.d, so after a kernel update run "
			    "`sudo geforce-sriov rebuild` for the new kernel") &&
	      !contains(out, "builds NVIDIA's module for the new kernel itself"), "no hook directory: said: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/etc/kernel", stage);
	CHECK(!gs_exists(p), "and nothing is created there");

	/* --driver=skip leaves no build record: the hook is installed and does nothing, and install says so. */
	snprintf(stage, sizeof(stage), "%s/stage-hook-skip", tmp);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/%s", stage, dirs[i]);
		gs_mkdir_p(p, 0755);
	}
	{
		const char *args[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr", "--driver=skip", NULL };

		CHECK(run_cli(&out, NULL, args) == GS_EXIT_OK && contains(out, "kernel hook: --driver=skip leaves no build record, so the hook does nothing") &&
		      !contains(out, "builds NVIDIA's module for the new kernel itself"), "driver=skip: the hook is not announced as building: %s", out);
		free(out);
	}

	/* status: a kernel whose automatic build failed is named with the command that fixes it. */
	fixture("hook-status", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	gs_pathf(marks, sizeof(marks), "%s/kernel-hook-failed", root);
	memset(&hook_status_opts, 0, sizeof(hook_status_opts));
	hook_status_opts.sysroot = root;
	hook_status_opts.config_path = cfg;
	hook_status_opts.kernel_hook_dir = marks;
	run_host(hook_status_host, &out);
	CHECK(!contains(out, "kernel hook:"), "no failure reported: nothing is said: %s", out);
	free(out);
	gs_pathf(q, sizeof(q), "%s/7.0.99-1-pve", marks);
	writef(q, "exit 3 at 2026-11-12 10:00:00; see /var/lib/geforce-sriov/kernel-hook-logs/7.0.99-1-pve.log\n");
	run_host(hook_status_host, &out);
	CHECK(contains(out, "kernel hook: the automatic build for kernel 7.0.99-1-pve failed (exit 3 at 2026-11-12 10:00:00; see "
			    "/var/lib/geforce-sriov/kernel-hook-logs/7.0.99-1-pve.log); run `sudo geforce-sriov rebuild --kernel 7.0.99-1-pve`"),
	      "a failed automatic build is reported with the fix: %s", out);
	free(out);
}

/*
 * An earlier build of this version installed the sources of the guest framebuffer cap under driver-patches/guest-fb/. A reinstall
 * removes the copies the manifest still names while they are unchanged, and the directory with them; a changed copy stays, listed.
 */
static void test_stale_cap_files(void)
{
	static const char *const names[] = { "gs-guest-fb.h", "guest-fb.patch" };
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], man[GS_PATH_MAX], sha[65], *out, *text;

	for (int mode = 0; mode < 3; mode++) {
		int edited = mode == 1, deleted = mode == 2;
		FILE *f;

		snprintf(stage, sizeof(stage), "%s/stage-stale-cap-%d", tmp, mode);
		stage_install(&out, stage);
		free(out);
		gs_pathf(man, sizeof(man), "%s" GS_MANIFEST, stage);
		gs_pathf(p, sizeof(p), "%s/usr/bin/driver-patches/guest-fb", stage);
		gs_mkdir_p(p, 0755);
		f = fopen(man, "a");
		if (!f)
			die(man);
		fprintf(f, "d - /usr/bin/driver-patches/guest-fb\n");
		for (size_t k = 0; k < 2; k++) {
			gs_pathf(p, sizeof(p), "%s/usr/bin/driver-patches/guest-fb/%s", stage, names[k]);
			writef(p, "a file of an earlier build\n");
			if (gs_sha256_file(p, sha))
				die(p);
			fprintf(f, "f %s /usr/bin/driver-patches/guest-fb/%s\n", sha, names[k]);
		}
		fclose(f);
		gs_pathf(p, sizeof(p), "%s/usr/bin/driver-patches/guest-fb/guest-fb.patch", stage);
		if (edited)
			writef(p, "edited by hand\n");
		if (deleted && unlink(p))
			die(p);
		stage_install(&out, stage);
		text = slurp(man);
		gs_pathf(p, sizeof(p), "%s/usr/bin/driver-patches/guest-fb/gs-guest-fb.h", stage);
		CHECK(!gs_exists(p) && contains(out, "removed obsolete"), "stale cap source %d: the unchanged copy is removed: %s", edited, out);
		gs_pathf(p, sizeof(p), "%s/usr/bin/driver-patches/guest-fb/guest-fb.patch", stage);
		CHECK(gs_exists(p) == edited, "stale cap source %d: the copy is %s", edited, edited ? "kept when it was changed" : "removed");
		CHECK(text && !contains(text, "gs-guest-fb.h") && contains(text, "guest-fb.patch") == edited &&
		      contains(text, "/usr/bin/driver-patches/guest-fb\n") == edited,
		      "stale cap source %d: the manifest keeps only what is still there: %s", edited, text ? text : "");
		free(text);
		if (deleted)
			CHECK(!contains(out, "guest-fb.patch") && !contains(out, "failed"),
			      "stale cap source, copy deleted by the user: only the manifest line goes, with no complaint: %s", out);
		free(out);
	}
}

/* `rebuild` asks for a power cycle only for the running kernel; a kernel that is not running has its build ready for the boot that uses it. */
static void test_rebuild_done_text(void)
{
	char text[512];

	gs_rebuild_done_text("7.0.14-16-pve", "7.0.14-16-pve", text, sizeof(text));
	CHECK(contains(text, "for 7.0.14-16-pve are now the build") && contains(text, "Next: power the machine off and on. A restart is not enough"),
	      "the running kernel: the power cycle is asked: %s", text);
	gs_rebuild_done_text("7.0.14-12-pve", "7.0.14-16-pve", text, sizeof(text));
	CHECK(contains(text, "for 7.0.14-12-pve are now the build") && contains(text, "it is used when the machine boots that kernel") &&
	      contains(text, "The running kernel (7.0.14-16-pve) is not changed") && !contains(text, "power the machine off and on"),
	      "another kernel: ready for its boot, no power cycle: %s", text);
	gs_rebuild_done_text("7.0.14-12-pve", "", text, sizeof(text));
	CHECK(contains(text, "The running kernel (unknown) is not changed") && !contains(text, "power the machine off and on"),
	      "the running kernel unknown: no power cycle: %s", text);
}

/*
 * Hosts whose kernel packages run no postinst.d hook: kernel-install (Fedora, openSUSE, any systemd host) gets a plugin, pacman (Arch) a
 * hook, each only where its directory exists; both call the hook script, installed beside the program. A host with postinst.d keeps its
 * hook and gets no plugin. uninstall removes them through the manifest.
 */
static void test_kernel_install_pacman(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], man[GS_PATH_MAX], modprobe[GS_PATH_MAX], sha[65], *out, *text;
	static const char *const files[] = { "/etc/kernel/install.d/90-geforce-sriov.install", "/usr/bin/geforce-sriov-kernel-hook",
					     "/etc/pacman.d/hooks/90-geforce-sriov.hook" };
	struct stat st;
	FILE *f;

	/* A host with kernel-install and no postinst.d. */
	snprintf(stage, sizeof(stage), "%s/stage-kinstall", tmp);
	gs_pathf(p, sizeof(p), "%s/usr/lib/kernel/install.d", stage);
	gs_mkdir_p(p, 0755);
	stage_install(&out, stage);
	CHECK(contains(out, "kernel-install plugin /etc/kernel/install.d/90-geforce-sriov.install") &&
	      !contains(out, "this host has no /etc/kernel/postinst.d") && !contains(out, "pacman builds"),
	      "kernel-install host: install says the plugin is in place: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s%s", stage, files[0]);
	text = slurp(p);
	CHECK(text && !stat(p, &st) && (st.st_mode & 0777) == 0755 && contains(text, "hook=/usr/bin/geforce-sriov-kernel-hook\n") &&
	      contains(text, "[ \"${1:-}\" = add ] || exit 0") && !contains(text, "@BINDIR@") && !contains(text, "@VERSION@"),
	      "the plugin is executable, runs the hook for add only and is rendered: %s", text ? text : "");
	free(text);
	gs_pathf(p, sizeof(p), "%s%s", stage, files[1]);
	text = slurp(p);
	CHECK(text && !stat(p, &st) && (st.st_mode & 0777) == 0755 && contains(text, "cli=/usr/bin/geforce-sriov\n") && !contains(text, "@BINDIR@"),
	      "the hook script is installed beside the program, rendered: %s", text ? text : "");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/pacman.d", stage);
	CHECK(!gs_exists(p), "and no pacman hook or directory is made");
	gs_pathf(p, sizeof(p), "%s" GS_MANIFEST, stage);
	text = slurp(p);
	CHECK(text && contains(text, " /etc/kernel/install.d/90-geforce-sriov.install\n") && contains(text, " /usr/bin/geforce-sriov-kernel-hook\n"),
	      "the manifest owns the plugin and the hook script: %s", text ? text : "");
	free(text);

	/* A host with pacman. */
	snprintf(stage, sizeof(stage), "%s/stage-pacman", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/pacman.d", stage);
	gs_mkdir_p(p, 0755);
	stage_install(&out, stage);
	CHECK(contains(out, "pacman builds NVIDIA's module for every installed kernel that has headers and no build yet (/etc/pacman.d/hooks/90-geforce-sriov.hook)") &&
	      !contains(out, "kernel-install plugin"), "pacman host: install says the hook is in place: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s%s", stage, files[2]);
	text = slurp(p);
	CHECK(text && !stat(p, &st) && (st.st_mode & 0777) == 0644 && contains(text, "\nWhen = PostTransaction\n") &&
	      contains(text, "\nExec = /usr/bin/geforce-sriov-kernel-hook --retry\n") && contains(text, "\nTarget = usr/lib/modules/*/build/include/\n") &&
	      contains(text, "\nTarget = usr/lib/modules/*/modules.order\n") && !contains(text, "@BINDIR@"),
	      "the pacman hook runs the hook script after the transaction on kernel and header paths: %s", text ? text : "");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/kernel/install.d", stage);
	CHECK(!gs_exists(p), "and no kernel-install plugin is made");

	/* postinst.d wins: a host that has it keeps its hook and gets no plugin. */
	snprintf(stage, sizeof(stage), "%s/stage-kinstall-apt", tmp);
	gs_pathf(p, sizeof(p), "%s/usr/lib/kernel/install.d", stage);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/etc/kernel/postinst.d", stage);
	gs_mkdir_p(p, 0755);
	stage_install(&out, stage);
	free(out);
	gs_pathf(p, sizeof(p), "%s/etc/kernel/install.d", stage);
	CHECK(!gs_exists(p), "a host with postinst.d gets no kernel-install plugin");

	/* uninstall removes the files the manifest lists (a manifest of the staged copies, so that only they go). */
	snprintf(stage, sizeof(stage), "%s/stage-kinstall", tmp);
	gs_pathf(man, sizeof(man), "%s/ki-manifest", tmp);
	gs_pathf(modprobe, sizeof(modprobe), "%s/ki-modprobe.d", tmp);
	gs_mkdir_p(modprobe, 0755);
	f = fopen(man, "w");
	if (!f)
		die(man);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s%s", stage, files[i]);
		if (gs_sha256_file(p, sha))
			die(p);
		fprintf(f, "f %s %s\n", sha, p);
	}
	snprintf(stage, sizeof(stage), "%s/stage-pacman", tmp);
	gs_pathf(p, sizeof(p), "%s%s", stage, files[2]);
	if (gs_sha256_file(p, sha))
		die(p);
	fprintf(f, "f %s %s\n", sha, p);
	fclose(f);
	fflush(stdout);
	CHECK(gs_uninstall_files(man, modprobe, NULL) == GS_EXIT_OK, "uninstall of the plugin, the hook script and the pacman hook succeeds");
	snprintf(stage, sizeof(stage), "%s/stage-kinstall", tmp);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s%s", stage, files[i]);
		CHECK(!gs_exists(p), "uninstall removed %s", files[i]);
	}
	snprintf(stage, sizeof(stage), "%s/stage-pacman", tmp);
	gs_pathf(p, sizeof(p), "%s%s", stage, files[2]);
	CHECK(!gs_exists(p), "uninstall removed %s", files[2]);
}

/*
 * Guest autostart is ordered after activation and never blocked by it. The four triggers that used to stop
 * every guest on the host: activation failed, one VF not ready, no GPU selected, a geforce_sriov.skip boot.
 * systemd is not run here. What decides each case is in the files install writes: the drop-in must order
 * and must not require, its check must not fail the unit, and the activation unit must be bounded and
 * must name both autostart units in Before=.
 */
static void test_vm_autostart(void)
{
	static const char *const units[] = { "libvirt-guests.service", "pve-guests.service" };
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out, *text;
	const char *check[] = { "check", "--sysfs-root", root, "--config", cfg, NULL };

	snprintf(stage, sizeof(stage), "%s/stage-vm-autostart", tmp);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/usr/lib/systemd/system/%s", stage, units[i]);
		writef(p, "[Service]\nExecStart=/bin/true\n");
	}
	stage_install(&out, stage);
	free(out);
	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/etc/systemd/system/%s.d/geforce-sriov.conf", stage, units[i]);
		text = slurp(p);
		/* Activation failed, or one VF is not ready: ordering stays, no dependency exists to fail. */
		CHECK(text && contains(text, "\nAfter=geforce-sriov-activate.service\n"), "%s: ordered after activation: %s",
		      units[i], text ? text : "(missing)");
		CHECK(text && !contains(text, "\nRequires=") && !contains(text, "\nBindsTo=") &&
		      !contains(text, "\nWants=") && !contains(text, "\nPartOf=") && !contains(text, "\nRequisite="),
		      "%s: no dependency on activation: %s", units[i], text ? text : "(missing)");
		/* Not ready, or no GPU selected: check exits non-zero, and the leading '-' ignores that. */
		CHECK(text && contains(text, "\nExecStartPre=-/usr/bin/geforce-sriov check\n") &&
		      !contains(text, "\nExecStartPre=/"), "%s: the check cannot fail the unit: %s", units[i],
		      text ? text : "(missing)");
		free(text);
	}
	/* A skip boot or a host without config skips the activation unit; After= on a skipped unit orders nothing. */
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/" GS_UNIT_ACTIVATE, stage);
	text = slurp(p);
	CHECK(text && contains(text, "\nBefore=display-manager.service libvirt-guests.service pve-guests.service\n") &&
	      contains(text, "\nConditionKernelCommandLine=!geforce_sriov.skip\n") &&
	      contains(text, "\nConditionPathExists=/etc/geforce-sriov/config\n"),
	      "the activation unit orders before autostart and is skipped on a skip boot: %s", text ? text : "(missing)");
	/* Autostart can wait for activation only as long as the unit may run. */
	CHECK(text && contains(text, "\nTimeoutStartSec=") && !contains(text, "TimeoutStartSec=0") &&
	      !contains(text, "TimeoutStartSec=infinity") && !contains(text, "@START_TIMEOUT@"),
	      "the activation unit has a finite start timeout: %s", text ? text : "(missing)");
	free(text);
	/* No GPU selected: check refuses, which the drop-in tolerates. */
	fixture("vm-autostart-nogpu", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "identity=on\n");
	CHECK(run_cli(&out, NULL, check) != 0, "check with no GPU selected is not ready: %s", out);
	free(out);
}

/* ---- manifest removal rules ------------------------------------------- */

/* install --dry-run and status name the display state activation produces: display=on puts KMS on a card that is not the boot display. */
static void test_display_report(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out;
	const char *status[] = { "status", "--sysfs-root", root, "--config", cfg, NULL };

	fixture("disp-report", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\ndisplay=on\n");
	run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, NULL);
	CHECK(contains(out, "display: boot_vga=0, requested=on, effective=on\n") && !contains(out, "on (forced)"),
	      "install --dry-run, display=on on a non-boot card: %s", out);
	free(out);
	run_cli(&out, NULL, status);
	CHECK(contains(out, "display: requested=on, effective for the selection: on"),
	      "status, display=on on a non-boot card: %s", out);
	free(out);

	/* The boot display GPU keeps its line. */
	fixture("disp-report-boot", &topo_default, gpu_ampere, 1, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\ndisplay=on\n");
	run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, NULL);
	CHECK(contains(out, "display: boot_vga=1, requested=on, effective=on (the boot display)"),
	      "install --dry-run, display=on on the boot display: %s", out);
	/* The display build's inputs are named before install runs, and no longer once they are given. */
	CHECK(contains(out, "[FAIL] 0000:03:00.0 is the boot display GPU, so install builds the modules for the host console") &&
	      contains(out, "--companion-file PATH (or --companion-source DIR): the Linux driver package") &&
	      contains(out, "--nvkms-source PATH: NVIDIA's public kernel module source"), "the checks name the display inputs: %s", out);
	free(out);
	{
		struct gs_diag_opts given = { .companion_file = cfg, .nvkms_source = root };
		struct gs_diag_opts half = { .companion_file = cfg };
		struct gs_diag_opts none = { .display_set = 1, .display = GS_DISPLAY_OFF };
		struct gs_diag_opts cuda = { .display_set = 1, .display = GS_DISPLAY_OFF, .host_cuda_set = 1, .host_cuda = 1 };
		struct gs_diag_opts skip = { .skip_driver = 1 };
		struct gs_diag_opts gone = { .companion_file = "/nonexistent/p.run", .nvkms_source = root };


		run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &given);
		CHECK(!contains(out, "that\n       build needs two more inputs"), "inputs given: %s", out);
		free(out);
		run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &half);
		CHECK(contains(out, "--nvkms-source PATH: NVIDIA's public kernel module source") &&
		      !contains(out, "--companion-file PATH (or"), "only the missing input is named: %s", out);
		free(out);
		run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &none);
		CHECK(!contains(out, "build needs two more inputs") && !contains(out, "host CUDA is selected"),
		      "--no-display needs no input: %s", out);
		free(out);
		run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &cuda);
		CHECK(contains(out, "[FAIL] host CUDA is selected, and its build needs the Linux driver package"),
		      "host CUDA names its input: %s", out);
		free(out);
		run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &skip);
		CHECK(!contains(out, "build needs two more inputs"), "--driver=skip builds nothing and needs no input: %s", out);
		free(out);
		run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &gone);
		CHECK(contains(out, "[FAIL] --companion-file /nonexistent/p.run does not exist"), "a missing path: %s", out);
	}
	free(out);
	run_cli(&out, NULL, status);
	CHECK(contains(out, "display: requested=on, effective for the boot display GPU: on"),
	      "status, display=on with the boot display: %s", out);
	free(out);
}

/* ---- the KMS table of gs_display_kms() ------------------------------------------------------------ */

static const struct fx_fn gpu_ada_display[] = {
	{ "0000:04:00.0", "2783", "030000", "nvidia" },
	{ "0000:04:00.1", "22bc", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL }
};

static struct gs_build_src kms_src;

/* A card for the pure rule: a table entry of the device id, and the boot role. */
static void kms_card_of(struct gs_card *c, unsigned int device, int boot_vga)
{
	memset(c, 0, sizeof(*c));
	c->present = 1;
	c->device = device;
	c->entry = device ? gs_device_lookup(device) : NULL;
	c->boot_vga = boot_vga;
}
static void build_for(struct gs_build_src *src, const char *dir, int display);


/* 1 when the log shows unbindLock taken for bdf before that card's removal: the lock of the removal, not of the re-probe. */
static int kms_removal_lock(const char *log, const char *bdf)
{
	char lock[96], rm[96];
	long l, r;

	snprintf(lock, sizeof(lock), "gpus/%s/unbindLock 1", bdf);
	snprintf(rm, sizeof(rm), "devices/%s/remove 1", bdf);
	l = pos_of(log, lock);
	r = pos_of(log, rm);
	return l >= 0 && r >= 0 && l < r;
}

/*
 * Two selected cards, 0000:03:00.0 (fns03, not the boot display) and the Ampere boot display 0000:04:00.0,
 * activated with display=mode (lab 1) or display=boot (lab 0) and, when extra is not NULL, a build record with
 * display=display_rec and the extra lines. Returns the exit status; the output and the simulator log go to out and log.
 */
static int kms_two(const char *name, const struct fx_fn *fns03, enum gs_display_mode mode, int lab, const char *extra,
		   int display_rec, char **out, char **log)
{
	char root[GS_PATH_MAX], nm[96];
	static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };
	int rc;

	snprintf(nm, sizeof(nm), "%s-%d", name, lab);
	act_host(nm, &topo_default, fns03, 0, NULL, root, sizeof(root));
	make_card(root, &topo_display, gpu_display, 1, NULL);
	set_bridge_windows(root, &topo_display, 18, 1440);
	sim_module("nvidia_modeset");
	sim_module("nvidia_drm");
	sim_writef("module/geforce_sriov/parameters/bdfs", "0000:03:00.0,0000:04:00.0\n");
	add_firmware_fb(root, &t4, "simple-framebuffer.0", "simple-framebuffer");
	add_iomem_fw(root, &t4, 1);
	gs_copy(sim.display_pf, sizeof(sim.display_pf), "0000:04:00.0");
	act_cfg.nbdfs = 2;
	gs_addr_parse("0000:03:00.0", &act_cfg.bdfs[0]);
	gs_addr_parse("0000:04:00.0", &act_cfg.bdfs[1]);
	act_cfg.display = lab ? mode : GS_DISPLAY_BOOT;
	if (extra || lab) {
		build_for(&kms_src, root, display_rec);
		writef(kms_src.record, "kernel=6.8.0\nmodule_build_id=deadbeef01020304\ndisplay=%d\nhost_cuda=0\n%s", display_rec,
		       extra ? extra : "");
		act_opts.build = &kms_src;
	}
	rc = run_host(activate_host, out);
	*log = sim_text("sim.log");
	return rc;
}

/* Which GPUs get KMS is one table, by generation and boot role; the console follows the boot display alone. */
static void test_kms_table(void)
{
	struct gs_config on, off, boot_only;
	struct gs_card c;
	struct gs_kms_build ok = { 1, 1 }, noplace = { 0, 1 }, nodisp = { 1, 0 }, unknown = { -1, -1 };
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out, *log;

	config_of(&on, "");
	config_of(&off, "display=off\n");
	config_of(&boot_only, "display=boot\n");
	CHECK(on.display == GS_DISPLAY_AUTO && off.display == GS_DISPLAY_OFF && boot_only.display == GS_DISPLAY_BOOT,
	      "the configs for the rule");

	/* Shipped table: every selected card has KMS. An Ada card that is not the boot display needs the Ada placement build. */
	for (int lab = 1; lab < 2; lab++) {
		for (unsigned int g = 0; g < 2; g++) {
			unsigned int dev = g ? 0x2783 : 0x2206;

			kms_card_of(&c, dev, 1);
			CHECK(gs_display_kms(&on, &c, NULL) == 1 && gs_display_kms(&on, &c, &ok) == 1 &&
			      gs_display_kms(&on, &c, &noplace) == 1 && gs_display_kms(&on, &c, &nodisp) == 1 &&
			      gs_display_kms(&off, &c, NULL) == 0 && gs_display_console(&on, &c) == 1 &&
			      gs_display_console(&off, &c) == 0, "boot display %04x, lab %d: KMS on whatever the record says", dev, lab);
			for (int boot = 0; boot > -2; boot--) {
				int want = 1;

				kms_card_of(&c, dev, boot);
				CHECK(gs_display_console(&on, &c) == 0, "%04x boot_vga %d: no console", dev, boot);
				CHECK(gs_display_kms(&on, &c, NULL) == want && gs_display_kms(&off, &c, NULL) == 0 &&
				      gs_display_kms(&boot_only, &c, NULL) == 0 && gs_display_kms(&boot_only, &c, &ok) == 0,
				      "%04x boot_vga %d, lab %d: KMS %d, none with display=boot", dev, boot, lab, want);
				CHECK(gs_display_kms(&on, &c, &ok) == want, "%04x non-boot, lab %d, record complete", dev, lab);
				CHECK(gs_display_kms(&on, &c, &nodisp) == 0, "%04x non-boot: a build without display modules has none", dev);
				CHECK(gs_display_kms(&on, &c, &unknown) == 0, "%04x non-boot: an unknown build record gives no KMS", dev);
				/* Only the Ada pair is held to the placement change. */
				CHECK(gs_display_kms(&on, &c, &noplace) == (g ? 0 : want),
				      "%04x non-boot, lab %d, no Ada placement in the build: %d", dev, lab, gs_display_kms(&on, &c, &noplace));
			}
		}
		/* A card outside the device table is judged by its boot role alone. */
		kms_card_of(&c, 0, 1);
		CHECK(gs_display_kms(&on, &c, NULL) == 1, "unlisted boot card");
		kms_card_of(&c, 0, 0);
		CHECK(gs_display_kms(&on, &c, NULL) == 0, "unlisted non-boot card");
	}
	kms_card_of(&c, 0x2206, 1);
	CHECK(gs_display_kms(&boot_only, &c, NULL) == 1 && gs_display_console(&boot_only, &c) == 1, "display=boot: the boot display keeps KMS");

	/* Two cards, boot_vga 1 and 0. lab 0: display=boot, KMS on the boot card only. lab 1: both cards in gpu_allow. */
	for (int lab = 0; lab < 2; lab++) {
		CHECK(kms_two("kms-two", gpu_ampere, GS_DISPLAY_AUTO, lab, NULL, 1, &out, &log) == GS_EXIT_OK &&
		      contains(out, "activation finished"), "two cards, lab %d: %s", lab, out);
		CHECK(contains(log, lab ? "gpu_allow=0x300,0x400" : "gpu_allow=0x400") && (lab || !contains(log, "0x300")),
		      "the allowlist, lab %d: %s", lab, log);
		CHECK(count_of(out, "firmware framebuffer simple-framebuffer.0 released") == 1,
		      "the firmware framebuffer is released once, for the boot card, lab %d: %s", lab, out);
		CHECK(count_of(log, "R modprobe --ignore-install nvidia_drm modeset=1 fbdev=1 gpu_allow=") == 2 &&
		      count_of(log, lab ? "nvidia_drm modeset=1 fbdev=1 gpu_allow=0x300,0x400" :
					  "nvidia_drm modeset=1 fbdev=1 gpu_allow=0x400") == 2,
		      "the display stack comes back after the re-probe with the same allowlist, lab %d: %s", lab, log);
		/* The non-boot card takes unbindLock when it has no KMS; with KMS nvidia_drm is its client and the lock is skipped. */
		CHECK(kms_removal_lock(log, "0000:03:00.0") == !lab && kms_removal_lock(log, "0000:04:00.0") == 0,
		      "unbindLock of the non-boot card, lab %d: %s", lab, log);
		CHECK(contains(out, "is limited") == 0, "display=auto prints no limit notice, lab %d", lab);
		free(log);
		free(out);
	}
	/* display=on prints no limit notice either. */
	for (int lab = 0; lab < 2; lab++) {
		CHECK(kms_two("kms-two-on", gpu_ampere, GS_DISPLAY_ON, lab, NULL, 1, &out, &log) == GS_EXIT_OK &&
		      !contains(out, "is limited"), "display=on, lab %d: %s", lab, out);
		free(log);
		free(out);
	}

	/* An Ada card that is not the boot display needs a build with the Ada placement. */
	CHECK(kms_two("kms-ada-nb", gpu_ada, GS_DISPLAY_AUTO, 1, "ada_placement=1\n", 1, &out, &log) == GS_EXIT_OK &&
	      contains(log, "gpu_allow=0x300,0x400") && count_of(out, "simple-framebuffer.0 released") == 1, "Ada non-boot, placement build: %s", out);
	CHECK(kms_removal_lock(log, "0000:03:00.0") == 0, "Ada non-boot KMS card: no unbindLock: %s", log);
	free(log);
	free(out);
	CHECK(kms_two("kms-ada-nb0", gpu_ada, GS_DISPLAY_AUTO, 1, "ada_placement=0\n", 1, &out, &log) == GS_EXIT_OK &&
	      contains(log, "gpu_allow=0x400") && !contains(log, "0x300"), "Ada non-boot, fixed-slot build: %s", out);
	CHECK(kms_removal_lock(log, "0000:03:00.0") == 1, "Ada non-boot without KMS takes the lock: %s", log);
	free(log);
	free(out);
	CHECK(kms_two("kms-ada-nbold", gpu_ada, GS_DISPLAY_AUTO, 1, "", 1, &out, &log) == GS_EXIT_OK &&
	      contains(log, "gpu_allow=0x400") && !contains(log, "0x300"), "Ada non-boot, record from before the key: %s", out);
	free(log);
	free(out);
	/* An Ada boot display keeps KMS whatever the record says: that is what 0.4.0 does. */
	act_host("kms-ada-boot", &topo_display, gpu_ada_display, 1, NULL, root, sizeof(root));
	build_for(&kms_src, root, 1);
	writef(kms_src.record, "kernel=6.8.0\nmodule_build_id=deadbeef01020304\ndisplay=1\nhost_cuda=0\nada_placement=0\n");
	act_opts.build = &kms_src;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "firmware framebuffer simple-framebuffer.0 released"),
	      "Ada boot display, fixed-slot record: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(log, "gpu_allow=0x400"), "Ada boot display keeps KMS: %s", log);
	free(log);
	free(out);

	/* Only a non-boot card selected, and a build without display modules: no refusal, no display load, no release. */
	for (int lab = 0; lab < 2; lab++) {
		act_host(lab ? "kms-nb-only-1" : "kms-nb-only-0", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
		build_for(&kms_src, root, 0);
		act_opts.build = &kms_src;
		act_cfg.display = lab ? GS_DISPLAY_AUTO : GS_DISPLAY_BOOT;
		CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished") &&
		      !contains(out, "refused") && !contains(out, "simple-framebuffer.0 released"),
		      "non-boot card only, no display modules, lab %d: %s", lab, out);
		log = sim_text("sim.log");
		CHECK(!contains(log, "nvidia_drm") && !contains(log, "nvidia_modeset") && !contains(log, "unbind simple") &&
		      kms_removal_lock(log, "0000:03:00.0") == 1,
		      "no display load, and the lock is taken, lab %d: %s", lab, log);
		free(log);
		free(out);
	}
	/* Entry on and the build has the modules: the non-boot card alone gets KMS, and no console is released. */
	act_host("kms-nb-only-on", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	act_cfg.display = GS_DISPLAY_AUTO;
	sim_module("nvidia_modeset");
	sim_module("nvidia_drm");
	build_for(&kms_src, root, 1);
	act_opts.build = &kms_src;
	gs_copy(sim.display_pf, sizeof(sim.display_pf), "0000:03:00.0");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && !contains(out, "simple-framebuffer.0 released"),
	      "non-boot card only, display modules built, entry on: %s", out);
	log = sim_text("sim.log");
	CHECK(contains(log, "nvidia_drm modeset=1 fbdev=1 gpu_allow=0x300") && !contains(log, "0x300,") &&
	      kms_removal_lock(log, "0000:03:00.0") == 0, "KMS on the lone non-boot card: %s", log);
	free(log);
	free(out);

	/* A wrong live allowlist with VFs present is refused, not reloaded. */
	CHECK(kms_two("kms-wrong-allow", gpu_ampere, GS_DISPLAY_AUTO, 1, NULL, 1, &out, &log) == GS_EXIT_OK,
	      "the run that makes the VFs: %s", out);
	free(log);
	free(out);
	sim_writef("module/nvidia_drm/parameters/gpu_allow", "0x400\n");
	act_opts.boot = 0;
	CHECK(run_host(activate_host, &out) != GS_EXIT_OK &&
	      contains(out, "gpu_allow=0x400, not modeset=1 fbdev=1 gpu_allow=0x300,0x400"),
	      "a wrong live allowlist with VFs: %s", out);
	log = sim_text("sim.log");
	CHECK(count_of(log, "R rmmod nvidia_drm") == 2 && count_of(log, "R modprobe --ignore-install nvidia_drm") == 2,
	      "nvidia_drm is not unloaded or reloaded under the VFs: %s", log);
	free(log);
	free(out);

	/* The build decision and the reports follow the rule. */
	{
		struct gs_config e;
		char why[256];
		const char *txt[2] = { "bdfs=0000:03:00.0\n", "bdfs=0000:03:00.0\ndisplay=on\n" };
		const char *boot_txt = "bdfs=0000:03:00.0\ndisplay=boot\n";

		fixture("kms-eff-boot", &topo_default, gpu_ampere, 1, NULL, root, sizeof(root));
		config_of(&e, "bdfs=0000:03:00.0\n");
		CHECK(gs_display_effective(root, &e, 0, why, sizeof(why)) == 1 && contains(why, "automatic for selected boot-display GPU"),
		      "boot card, no inputs: %s", why);
		config_of(&e, "bdfs=0000:03:00.0\ndisplay=off\n");
		CHECK(gs_display_effective(root, &e, 1, why, sizeof(why)) == 0 && contains(why, "not enabled"), "display=off: %s", why);
		fixture("kms-eff-nb", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
		for (int lab = 0; lab < 2; lab++)
			for (int m = 0; m < 2; m++)
				for (int inputs = 0; inputs < 2; inputs++) {
					int want = lab && inputs;

					config_of(&e, lab ? txt[m] : boot_txt);
					CHECK(gs_display_effective(root, &e, inputs, why, sizeof(why)) == want,
					      "non-boot only, lab %d, display=%s, inputs %d: %s", lab, m ? "on" : "auto", inputs, why);
				}
		/* Inputs count as given only when both the guest package and the module source are. */
		CHECK(gs_display_inputs(NULL, "a", NULL, "b") == 1 && gs_display_inputs(NULL, NULL, "a", "b") == 1 &&
		      gs_display_inputs(NULL, "a", NULL, NULL) == 0 && gs_display_inputs(NULL, NULL, NULL, "b") == 0 &&
		      gs_display_inputs(NULL, NULL, NULL, NULL) == 0, "inputs given");
	}
	/* install --dry-run and status say the same. */
	fixture("kms-report", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\ndisplay=boot\n");
	run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, NULL);
	CHECK(contains(out, "display: boot_vga=0, requested=boot, effective=off\n"), "dry run, display=boot: %s", out);
	free(out);
	memset(&hook_status_opts, 0, sizeof(hook_status_opts));
	hook_status_opts.sysroot = root;
	hook_status_opts.config_path = cfg;
	run_host(hook_status_host, &out);
	CHECK(contains(out, "display: requested=boot, effective for the selection: off\n"), "status, display=boot: %s", out);
	free(out);
	writef(cfg, "bdfs=0000:03:00.0\ndisplay=on\n");
	run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, NULL);
	CHECK(contains(out, "display: boot_vga=0, requested=on, effective=on\n"), "dry run, display=on: %s", out);
	free(out);
	run_host(hook_status_host, &out);
	CHECK(contains(out, "display: requested=on, effective for the selection: on"), "status, display=on: %s", out);
	free(out);
}

/*
 * The build and report lines for every cell of display (auto, on, off) x the card's boot role x the build inputs
 * (given or not) x the table entry (off, on): setup's summary, install --dry-run's card line and status.
 */
static void test_kms_reports(void)
{
	static const char *const mode[] = { "auto", "on", "off" };
	static const enum gs_display_mode mode_v[] = { GS_DISPLAY_AUTO, GS_DISPLAY_ON, GS_DISPLAY_OFF };

	for (int boot = 0; boot < 2; boot++) {
		char nocfg[GS_PATH_MAX];
		char root[GS_PATH_MAX], cfg[GS_PATH_MAX];

		fixture(boot ? "kms-rep-boot" : "kms-rep-nb", &topo_default, gpu_ampere, boot, NULL, root, sizeof(root));
		gs_pathf(cfg, sizeof(cfg), "%s/config", root);
		gs_pathf(setup_log, sizeof(setup_log), "%s/setup.log", root);
		setup_write_config = NULL;
		for (int m = 0; m < 3; m++)
			for (int inputs = 0; inputs < 2; inputs++)
				for (int lab = 0; lab < 2; lab++) {
					int kms = m != 2 && (boot || lab);
					int build = m != 2 && (boot || (lab && inputs));
					char want[256], *out;
					struct gs_diag_opts extra = { 0 };
					struct gs_install_opts o;

					const char *cm = lab || m == 2 ? mode[m] : "boot"; /* lab 0: display=boot instead of auto and on */

					writef(cfg, "bdfs=0000:03:00.0\ndisplay=%s\n", cm);

					/* install --dry-run: the card's line follows the KMS rule alone. */
					if (inputs)
						extra.companion_file = cfg, extra.nvkms_source = root;
					run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &extra);
					snprintf(want, sizeof(want), "display: boot_vga=%d, requested=%s, effective=%s\n", boot, cm,
						 kms ? (boot ? "on (the boot display)" : "on") : "off");
					CHECK(contains(out, want), "dry run, boot %d, display=%s, inputs %d, lab %d: %s", boot, mode[m], inputs,
					      lab, out);
					free(out);

					/* status. */
					memset(&hook_status_opts, 0, sizeof(hook_status_opts));
					hook_status_opts.sysroot = root;
					hook_status_opts.config_path = cfg;
					run_host(hook_status_host, &out);
					snprintf(want, sizeof(want), "display: requested=%s, effective for %s: %s\n", cm,
						 boot ? "the boot display GPU" : "the selection", kms ? "on" : "off");
					CHECK(contains(out, want), "status, boot %d, display=%s, lab %d: %s", boot, mode[m], lab, out);
					free(out);

					/* setup on a host with a config: the build decision names why display=on builds nothing. */
					setup_opts(&o, NULL);
					gs_addr_parse("0000:03:00.0", &o.devices[0]);
					if (inputs)
						o.companion_file = cfg, o.nvkms_source = root;
					run_setup(root, NULL, &o, 1, cfg, NULL, &out);
					CHECK(contains(out, "Display: display=on is set, but the display modules are not built.") ==
					      (m == 1 && lab && !build), "setup with a config, boot %d, display=%s, inputs %d, lab %d: %s", boot,
					      mode[m], inputs, lab, out);
					free(out);

					/* setup on a first install: the summary says whether install builds the display modules. */
					if (lab) {
						gs_pathf(nocfg, sizeof(nocfg), "%s/no-config", root);
						o.display_set = 1;
						o.display = mode_v[m];
						run_setup(root, "none\n", &o, 1, nocfg, NULL, &out);
						snprintf(want, sizeof(want), "  display:      %s\n", build ? "enabled" : "not enabled");
						CHECK(contains(out, want), "setup summary, boot %d, display=%s, inputs %d, lab %d: %s", boot,
						      mode[m], inputs, lab, out);
						free(out);
					}
				}
	}
}

/*
 * The two ways to run the host checks: before install (every line, a summary, the banner) and after it, inside
 * status (INFO, WARN and FAIL only; a FAIL is the exit code). doctor itself is not a command.
 */
static void test_hostcheck_modes(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], p[GS_PATH_MAX], *out;
	const char *status[] = { "status", "--sysfs-root", root, "--config", cfg, NULL };
	struct gs_diag_opts d3 = { .disable_d3cold = 1 };

	fixture("hostcheck-modes", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");

	CHECK(run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, NULL) == GS_EXIT_OK && contains(out, "checks before install (sysfs root ") &&
	      contains(out, "[OK  ] kernel livepatch support present") && contains(out, "\n0 failure(s), "),
	      "before install: banner, OK lines and the summary: %s", out);
	free(out);
	CHECK(run_hostcheck(&out, GS_HOSTCHECK_POST, root, cfg, NULL) == GS_EXIT_OK && !contains(out, "[OK  ]") &&
	      !contains(out, "failure(s)") && !contains(out, "checks before install"),
	      "after install: no OK lines, no summary, no banner: %s", out);
	free(out);
	/* --disable-d3cold needs a laptop GPU among the GPUs judged. */
	CHECK(run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, &d3) == GS_EXIT_USAGE, "--disable-d3cold without a laptop GPU: %s", out);
	free(out);

	/* A FAIL after install is the exit code of status; the lines come under their own heading. */
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch", root);
	CHECK(rmdir(p) == 0, "the fixture's livepatch directory goes");
	CHECK(run_hostcheck(&out, GS_HOSTCHECK_POST, root, cfg, NULL) == GS_EXIT_REFUSED &&
	      contains(out, "Other host checks (OK lines are not shown):\n[FAIL] kernel: no "), "after install: a FAIL: %s", out);
	free(out);
	CHECK(run_hostcheck(&out, GS_HOSTCHECK_PRE, root, cfg, NULL) == GS_EXIT_REFUSED && contains(out, "\n1 failure(s), "),
	      "before install: the same FAIL is exit 3: %s", out);
	free(out);
	CHECK(run_cli(&out, NULL, status) == GS_EXIT_FAIL && contains(out, "Other host checks (OK lines are not shown):\n[FAIL] kernel: no ") &&
	      contains(out, "next: ") && !contains(out, "doctor"), "status carries the FAIL and names no doctor: %s", out);
	free(out);
}

/* The alias overrides follow the selection: written with a GPU selected, removed without one. */
static void test_autoload_scope(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], *out, *text;
	const char *sel[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr",
			      "--device", "0000:03:00.0", "--no-display", NULL };

	/* A selected GPU gets the file whatever the display mode: nvidia_vgpu_vfio carries the aliases too. */
	snprintf(stage, sizeof(stage), "%s/stage-autoload-sel", tmp);
	CHECK(run_cli(&out, NULL, sel) == GS_EXIT_OK, "staged install with a GPU: %s", out);
	/* The command printed for the target host builds what was chosen: --no-display is not --display. */
	CHECK(contains(out, "geforce-sriov-driver --source") && !contains(out, " --display") && !contains(out, " --host-cuda") &&
	      contains(out, "Host desktop: not installed (host_desktop=auto): it needs the display build, and the display build is off here.\n"),
	      "the staged hint follows the effective features: %s", out);
	free(out);
	{
		const char *both[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr",
				       "--display", "--host-cuda", NULL };

		CHECK(run_cli(&out, NULL, both) == GS_EXIT_OK &&
		      contains(out, "/usr/src/nvidia-580.159.01 --display --host-cuda ") &&
		      contains(out, "Host desktop: enabled (host_desktop=auto); NVIDIA's graphics libraries come from the Linux driver package.\n"),
		      "the staged hint names both features, and the host desktop is on with the display build: %s", out);
		free(out);
	}
	{
		const char *off[] = { "install", "--destdir", stage, "--source", srcdir, "--prefix", "/usr",
				      "--display", "--no-host-desktop", NULL };

		CHECK(run_cli(&out, NULL, off) == GS_EXIT_OK && contains(out, "Host desktop: disabled (host_desktop=off).\n") &&
		      !contains(out, "Host desktop: enabled"), "--no-host-desktop: %s", out);
		free(out);
		gs_pathf(p, sizeof(p), "%s" GS_CONFIG_PATH, stage);
		text = slurp(p);
		CHECK(text && contains(text, "\nhost_desktop=off\n"), "the config keeps it: %s", text ? text : "(absent)");
		free(text);
	}
	gs_pathf(p, sizeof(p), "%s" GS_AUTOLOAD_PATH, stage);
	text = slurp(p);
	CHECK(text && count_of(text, "alias pci:v000010DEd*") == 3 && count_of(text, " off\n") == 3,
	      "three alias overrides with a GPU selected: %s", text ? text : "(absent)");
	free(text);

	/* No GPU selected: the units stay disabled, so nothing may stop udev from loading the driver. */
	snprintf(stage, sizeof(stage), "%s/stage-autoload-none", tmp);
	stage_install(&out, stage);
	free(out);
	gs_pathf(p, sizeof(p), "%s" GS_AUTOLOAD_PATH, stage);
	CHECK(!gs_exists(p), "no alias overrides without a selected GPU");

	/* An unchanged copy an earlier install wrote is removed; the manifest forgets it. */
	writef(p, "alias pci:v000010DEd*sv*sd*bc03sc00i00* off\n");
	manifest_add_file(stage, GS_AUTOLOAD_PATH);
	stage_install(&out, stage);
	CHECK(!gs_exists(p) && contains(out, "it is not needed while no GPU is selected"), "the earlier copy is removed: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s" GS_MANIFEST, stage);
	text = slurp(p);
	CHECK(text && !contains(text, GS_AUTOLOAD_PATH), "the manifest no longer lists it: %s", text ? text : "");
	free(text);
}

/* The alias file keeps udev from loading nvidia; the warning follows the unit whose first step loads it. */
static void test_unselected_units(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], units[GS_PATH_MAX], mod[GS_PATH_MAX], p[GS_PATH_MAX], *out;
	struct gs_diag_opts extra = { 0 };

	fixture("unsel-units", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	gs_pathf(units, sizeof(units), "%s/units", root);
	gs_pathf(mod, sizeof(mod), "%s/modprobe.d", root);
	gs_pathf(p, sizeof(p), "%s/multi-user.target.wants", units);
	gs_mkdir_p(p, 0755);
	gs_mkdir_p(mod, 0755);
	gs_pathf(p, sizeof(p), "%s/geforce-sriov-autoload.conf", mod);
	writef(p, "alias pci:v000010DEd*sv*sd*bc03sc00i00* off\n");
	extra.unit_dir = units;
	extra.modprobe_dir = mod;
	run_hostcheck(&out, GS_HOSTCHECK_POST, root, cfg, &extra);
	CHECK(contains(out, "geforce-sriov-activate.service, whose first step loads it at boot instead, is not enabled"),
	      "activate unit not enabled: warned: %s", out);
	free(out);
	/* The early unit's link alone does not stand in for it. */
	gs_pathf(p, sizeof(p), "%s/sysinit.target.wants", units);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/sysinit.target.wants/" GS_UNIT_LIVEPATCH, units);
	link_to("../" GS_UNIT_LIVEPATCH, p);
	run_hostcheck(&out, GS_HOSTCHECK_POST, root, cfg, &extra);
	CHECK(contains(out, "whose first step loads it at boot instead, is not enabled"), "livepatch link alone: still warned: %s", out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/multi-user.target.wants/" GS_UNIT_ACTIVATE, units);
	link_to("../" GS_UNIT_ACTIVATE, p);
	run_hostcheck(&out, GS_HOSTCHECK_POST, root, cfg, &extra);
	CHECK(!contains(out, "stops udev from loading NVIDIA's driver"), "activate unit enabled: no warning: %s", out);
	free(out);
}

static void test_manifest_remove(void)
{
	char dir[GS_PATH_MAX], path[GS_PATH_MAX], link[GS_PATH_MAX], sha[65];
	int r;

	snprintf(dir, sizeof(dir), "%s/manifest", tmp);
	gs_mkdir_p(dir, 0755);
	gs_pathf(path, sizeof(path), "%s/file", dir);
	writef(path, "content\n");
	gs_sha256_file(path, sha);
	CHECK(gs_uninstall_file(path, sha) == 1 && !gs_exists(path), "unchanged file removed");
	writef(path, "changed\n");
	CHECK(gs_uninstall_file(path, sha) == 0 && gs_exists(path), "a changed file is kept");
	CHECK(gs_uninstall_file(path, sha) == 0 && gs_exists(path), "still kept on a second look");
	unlink(path);
	gs_pathf(link, sizeof(link), "%s/link", dir);
	link_to(path, link);
	writef(path, "content\n");
	r = gs_uninstall_file(link, sha);
	CHECK(r <= 0 && gs_exists(path), "a link is never followed for removal: %d", r);
}

/* ---- progress reporting and the watchdog -------------------------------- */

static char cur_section[96];
static size_t cur_section_len;

/* SIGALRM: name the section that stalled and exit; write(2) only, so the handler is safe. */
static void watchdog(int sig)
{
	static const char prefix[] = "test_lifecycle: WATCHDOG: section ";
	static const char suffix[] = " did not finish within 120 s\n";

	(void)sig;
	/* The result is deliberately ignored, but both compilers mark write() warn_unused_result. */
	if (write(2, prefix, sizeof(prefix) - 1) < 0) {
	}
	if (write(2, cur_section, cur_section_len) < 0) {
	}
	if (write(2, suffix, sizeof(suffix) - 1) < 0) {
	}
	_exit(3);
}

/* Print the section name before it runs, line-buffered and flushed: a killed CI log ends here. */
static void set_section(const char *name)
{
	sigset_t block, saved;

	sigemptyset(&block);
	sigaddset(&block, SIGALRM);
	sigprocmask(SIG_BLOCK, &block, &saved);
	snprintf(cur_section, sizeof(cur_section), "%s", name);
	cur_section_len = strlen(cur_section);
	printf("test_lifecycle: section %s\n", cur_section);
	fflush(stdout);
	sigprocmask(SIG_SETMASK, &saved, NULL);
}

static const char *trace_section(void)
{
	return cur_section;
}

/* "test_addr" -> "addr": the section line names the group, not its C symbol. */
static const char *section_name(const char *symbol)
{
	return strncmp(symbol, "test_", 5) == 0 ? symbol + 5 : symbol;
}

/* ---- the selection record: the card at a selected address against the card that was selected ---- */

static const struct fx_fn gpu_ada04[] = {
	{ "0000:04:00.0", "2783", "030000", "nvidia" },
	{ "0000:04:00.1", "22bc", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL }
};
static const struct fx_topo topo_third = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };
static const struct fx_fn gpu_ada05[] = {
	{ "0000:05:00.0", "2783", "030000", "nvidia" },
	{ "0000:05:00.1", "22bc", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL }
};

/* What install records for the two cards of the fixture: an Ampere 3080 at 03:00.0, an Ada 4070 SUPER at 04:00.0. */
#define REC_TWO "0000:03:00.0 10de:2206 10de:146d\n0000:04:00.0 10de:2783 10de:146d\n"

/* Rewrite what a card says it is: its device ID and the subsystem IDs. NULL leaves one as it was. */
static void set_card_ids(const char *root, const struct fx_topo *k, const char *bdf, const char *device, const char *subvendor,
			 const char *subdevice)
{
	const char *name[3] = { "device", "subsystem_vendor", "subsystem_device" };
	const char *value[3] = { device, subvendor, subdevice };
	char p[GS_PATH_MAX];

	for (unsigned int i = 0; i < 3; i++) {
		if (!value[i])
			continue;
		gs_pathf(p, sizeof(p), "%s/devices/%s/%s/%s", root, k->bridge_path, bdf, name[i]);
		writef(p, "0x%s\n", value[i]);
	}
}

/* A host with the Ampere card at 03:00.0, the Ada card at 04:00.0 (when with04) or 05:00.0 (when with05), and the record of both. */
static void ident_host(const char *name, int with04, int with05, char *root, size_t len)
{
	act_host(name, &topo_default, gpu_ampere, 0, "0/4", root, len);
	if (with04)
		make_card(root, &topo_display, gpu_ada04, 0, "0/4");
	if (with05)
		make_card(root, &topo_third, gpu_ada05, 0, "0/4");
	config_of(&act_cfg, "bdfs=0000:03:00.0,0000:04:00.0\n");
	check_cfg = act_cfg;
	memset(&check_opts, 0, sizeof(check_opts));
	memset(&lp_opts, 0, sizeof(lp_opts));
	lp_opts.boot = 1;
	sim_writef("livepatch.conf", "bdfs=0000:03:00.0,0000:04:00.0\nvfs=4\n");
	sim_remove("module/geforce_sriov");
	sim_writef("state/" GS_SELECTION_NAME, REC_TWO);
}

/* The four commands that look before they act refuse, and nothing was written or run. */
static void ident_refused(const char *what, const char *const must[])
{
	static int (*const hosts[])(void) = { activate_host, livepatch_host, check_host, daemon_host };
	static const char *const names[] = { "activate", "livepatch", "check", "check --daemon-ready" };

	for (unsigned int i = 0; i < 4; i++) {
		char *out;

		CHECK(run_host(hosts[i], &out) == GS_EXIT_REFUSED && contains(out, "refused, nothing changed: ") &&
		      !sim_exists("sim.log"), "%s, %s: refused and nothing run: %s", what, names[i], out);
		for (unsigned int k = 0; must[k]; k++)
			CHECK(contains(out, must[k]), "%s, %s: names '%s': %s", what, names[i], must[k], out);
		free(out);
	}
}

static void test_selection_identity(void)
{
	char root[GS_PATH_MAX], text[4096], *out, *log, line[1024];
	struct gs_ident ids[GS_CFG_MAX_BDFS], back[GS_CFG_MAX_BDFS];
	struct gs_addr list[GS_CFG_MAX_BDFS];
	unsigned int n, nback = 0;
	char err[256];
	int len;

	/* What install records: read from the cards, rendered, read back; an address that was recorded stays unless named again. */
	ident_host("ident-roundtrip", 1, 0, root, sizeof(root));
	set_card_ids(root, &topo_display, "0000:04:00.0", NULL, "1043", "88e2");
	CHECK(gs_parse_addr_list("bdfs", "0000:03:00.0,0000:04:00.0", list, &n, err, sizeof(err)) == 0 && n == 2, "two addresses");
	CHECK(gs_selection_collect(root, list, n, NULL, 0, 1, ids) == 2 && ids[0].device == 0x2206 && ids[1].device == 0x2783 &&
	      ids[1].subvendor == 0x1043 && ids[1].subdevice == 0x88e2, "the identities are read from the cards");
	len = gs_selection_render(ids, 2, text, sizeof(text));
	CHECK(len > 0 && contains(text, "0000:04:00.0 10de:2783 1043:88e2\n"), "rendered: %s", text);
	sim_writef("state/" GS_SELECTION_NAME, text);
	CHECK(gs_selection_read(sim.state, back, &nback) == 0 && nback == 2 && back[1].subdevice == 0x88e2 &&
	      strcmp(back[0].pf.str, "0000:03:00.0") == 0, "read back");
	set_card_ids(root, &topo_display, "0000:04:00.0", "2782", NULL, NULL);
	CHECK(gs_selection_collect(root, list, n, back, nback, 0, ids) == 2 && ids[1].device == 0x2783,
	      "without refresh the recorded line stays, whatever is there now");
	CHECK(gs_selection_collect(root, list, n, back, nback, 1, ids) == 2 && ids[1].device == 0x2782,
	      "with refresh the card there now is recorded");
	set_card_ids(root, &topo_display, "0000:04:00.0", "1234", NULL, NULL);
	CHECK(gs_selection_collect(root, list, n, NULL, 0, 1, ids) == 1, "a card that is not in the device table is not recorded");

	/* A selected card that is the one that was selected: nothing is refused, whatever else is on the host. */
	ident_host("ident-same", 1, 0, root, sizeof(root));
	CHECK(run_host(livepatch_host, &out) == GS_EXIT_OK && contains(out, "loaded for"), "unchanged cards: %s", out);
	free(out);
	log = sim_text("sim.log");
	CHECK(contains(log, "R modprobe geforce_sriov"), "the livepatch loads for the cards that were selected: %s", log);
	free(log);

	/* Two selected cards of different kinds trade places: both are refused, the command names each card where it is now. */
	ident_host("ident-swap", 1, 0, root, sizeof(root));
	set_card_ids(root, &topo_default, "0000:03:00.0", "2783", NULL, NULL);
	set_card_ids(root, &topo_display, "0000:04:00.0", "2206", NULL, NULL);
	{
		static const char *const must[] = { "0000:03:00.0 is not the GPU that was selected", "0000:04:00.0 is not the GPU that was selected",
						    "the selected GPU is at 0000:04:00.0 now", "the selected GPU is at 0000:03:00.0 now",
						    "Nothing was written to any GPU",
						    "sudo geforce-sriov install --device 0000:03:00.0 --device 0000:04:00.0\n",
						    "power the machine off and on", NULL };

		ident_refused("swap", must);
	}
	{
		struct gs_host h = sim_host();

		CHECK(gs_selection_verdict(&h, &act_cfg, NULL, text, sizeof(text)) == 1 &&
		      contains(text, "selected:  GeForce RTX 3080 10de:2206 (subsystem 10de:146d)") &&
		      contains(text, "there now: GeForce RTX 4070 SUPER 10de:2783 (subsystem 10de:146d)"), "the cards are named: %s", text);
		act_opts.ndevices = 1;
		gs_addr_parse("0000:03:00.0", &act_opts.devices[0]);
		CHECK(gs_selection_verdict(&h, &act_cfg, &act_opts, text, sizeof(text)) == 1 &&
		      !contains(text, "0000:04:00.0 is not"), "check --device judges only the card it names: %s", text);
		act_opts.ndevices = 0;
	}

	/* A selected card replaced by a card of another kind the tool supports: refused; the command selects the card that is there. */
	ident_host("ident-replaced", 1, 0, root, sizeof(root));
	set_card_ids(root, &topo_display, "0000:04:00.0", "2782", NULL, NULL);
	{
		static const char *const must[] = { "0000:04:00.0 is not the GPU that was selected", "there now: GeForce RTX 4070 Ti 10de:2782",
						    "sudo geforce-sriov install --device 0000:03:00.0 --device 0000:04:00.0\n", NULL };

		ident_refused("replaced", must);
	}
	/* The same chip on another board (the subsystem IDs differ) is another card too. */
	ident_host("ident-board", 1, 0, root, sizeof(root));
	set_card_ids(root, &topo_display, "0000:04:00.0", NULL, "1043", "88e2");
	{
		static const char *const must[] = { "there now: GeForce RTX 4070 SUPER 10de:2783 (subsystem 1043:88e2)", NULL };

		ident_refused("another board", must);
	}
	/* Replaced by a card the tool does not support: no address is named for it. */
	ident_host("ident-unsupported", 1, 0, root, sizeof(root));
	set_card_ids(root, &topo_display, "0000:04:00.0", "1234", NULL, NULL);
	{
		static const char *const must[] = { "there now: 10de:1234", "sudo geforce-sriov install --device 0000:03:00.0\n", NULL };

		ident_refused("unsupported replacement", must);
	}

	/* A selected card at a new address: found there and named. The card at the old address is gone. */
	ident_host("ident-moved", 0, 1, root, sizeof(root));
	{
		static const char *const must[] = { "0000:04:00.0 is not the GPU that was selected", "there now: nothing",
						    "the selected GPU is at 0000:05:00.0 now (the PCI addresses changed)",
						    "sudo geforce-sriov install --device 0000:03:00.0 --device 0000:05:00.0\n", NULL };

		ident_refused("moved", must);
	}

	/* A selected card is missing and the other still matches: nothing is activated, as for a missing card before. */
	ident_host("ident-missing", 0, 0, root, sizeof(root));
	{
		static const char *const must[] = { "0000:04:00.0 is not the GPU that was selected", "there now: nothing",
						    "sudo geforce-sriov install --device 0000:03:00.0\n", NULL };

		ident_refused("missing", must);
	}


	/* A config from 0.3.0 has no record: every command goes on as before, and status says so once. */
	enable_host("ident-unrecorded", root, sizeof(root));
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "no record: activation as before: %s", out);
	free(out);
	{
		struct gs_config c;

		config_of(&c, "bdfs=0000:03:00.0\n");
		CHECK(gs_selection_unrecorded(sim.state, &c, line, sizeof(line)) == 1 &&
		      contains(line, "the card identities are not recorded yet") &&
		      contains(line, "run `sudo geforce-sriov install --device 0000:03:00.0` to record them"), "said once: %s", line);
		sim_writef("state/" GS_SELECTION_NAME, "0000:03:00.0 10de:2206 10de:146d\n");
		CHECK(gs_selection_unrecorded(sim.state, &c, line, sizeof(line)) == 0, "recorded: nothing to say");
		config_of(&c, "bdfs=0000:03:00.0,0000:04:00.0\n");
		CHECK(gs_selection_unrecorded(sim.state, &c, line, sizeof(line)) == 1, "one address without a line is not recorded yet");
	}
	/* An address with no line in a record that exists is not judged: that card is activated as before. */
	enable_host("ident-partial", root, sizeof(root));
	sim_writef("state/" GS_SELECTION_NAME, "0000:09:00.0 10de:2783 10de:146d\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "a card with no line: %s", out);
	free(out);
	/* The record, with its card, is what lets activation go on. */
	enable_host("ident-recorded", root, sizeof(root));
	sim_writef("state/" GS_SELECTION_NAME, "0000:03:00.0 10de:2206 10de:146d\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished"), "the card that was selected: %s", out);
	free(out);
	/* A record that cannot be read: the cards cannot be told apart, so nothing is touched. */
	enable_host("ident-corrupt", root, sizeof(root));
	sim_writef("state/" GS_SELECTION_NAME, "0000:03:00.0 10de:2206\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED && contains(out, "cannot be read as the record of the selected GPUs") &&
	      contains(out, "sudo geforce-sriov install --device 0000:03:00.0\n") && !sim_exists("sim.log"), "corrupt record: %s", out);
	free(out);
}

/* A GNU build-id note in a minimal ELF64 relocatable file, as a kernel module has one. */
static void make_ko(const char *path, const unsigned char *id, size_t idlen)
{
	Elf64_Ehdr eh;
	Elf64_Shdr sh[2];
	unsigned char buf[512], note[64];
	unsigned int hdr[3] = { 4, (unsigned int)idlen, 3 };
	size_t nlen = 16 + ((idlen + 3) & ~(size_t)3);

	memset(&eh, 0, sizeof(eh));
	memset(sh, 0, sizeof(sh));
	memset(note, 0, sizeof(note));
	memcpy(note, hdr, sizeof(hdr));
	memcpy(note + 12, "GNU", 4);
	memcpy(note + 16, id, idlen);
	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_ident[EI_VERSION] = EV_CURRENT;
	eh.e_type = ET_REL;
	eh.e_machine = EM_X86_64;
	eh.e_version = EV_CURRENT;
	eh.e_ehsize = sizeof(eh);
	eh.e_shentsize = sizeof(Elf64_Shdr);
	eh.e_shnum = 2;
	eh.e_shoff = sizeof(eh) + nlen;
	sh[1].sh_type = SHT_NOTE;
	sh[1].sh_offset = sizeof(eh);
	sh[1].sh_size = nlen;
	memcpy(buf, &eh, sizeof(eh));
	memcpy(buf + sizeof(eh), note, nlen);
	memcpy(buf + sizeof(eh) + nlen, sh, sizeof(sh));
	write_bin(path, buf, sizeof(eh) + nlen + sizeof(sh));
}

/* A build record for kernel 6.8.0 whose module is the one at ko, with the given display flag. */
static void build_for(struct gs_build_src *src, const char *dir, int display)
{
	static const unsigned char id[] = { 0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4 };
	static char record[GS_PATH_MAX], ko[GS_PATH_MAX];
	char text[256];

	gs_pathf(record, sizeof(record), "%s/driver-build", dir);
	gs_pathf(ko, sizeof(ko), "%s/nvidia.ko", dir);
	make_ko(ko, id, sizeof(id));
	snprintf(text, sizeof(text), "kernel=6.8.0\nmodule_build_id=deadbeef01020304\ndisplay=%d\nhost_cuda=0\n", display);
	writef(record, "%s", text);
	memset(src, 0, sizeof(*src));
	src->record = record;
	src->release = "6.8.0";
	src->module = ko;
}

/* A failure report of the kernel hook for a kernel whose installed module is the recorded build is not shown: a later install made it. */
static void test_hook_report_cleared(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], marks[GS_PATH_MAX], q[GS_PATH_MAX], *out;
	struct gs_build_src src;

	fixture("hook-cleared", &topo_default, gpu_ampere, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	gs_pathf(marks, sizeof(marks), "%s/kernel-hook-failed", root);
	build_for(&src, root, 0);
	memset(&hook_status_opts, 0, sizeof(hook_status_opts));
	hook_status_opts.sysroot = root;
	hook_status_opts.config_path = cfg;
	hook_status_opts.kernel_hook_dir = marks;
	hook_status_opts.build = &src;
	gs_mkdir_p(marks, 0755);
	gs_pathf(q, sizeof(q), "%s/6.8.0", marks);
	writef(q, "exit 1 at 2026-11-12 10:00:00; see x\n");
	run_host(hook_status_host, &out);
	CHECK(!contains(out, "kernel hook:"), "the kernel's module is the recorded build: no failure is reported: %s", out);
	free(out);
	gs_pathf(q, sizeof(q), "%s/7.0.99-1-pve", marks);
	writef(q, "exit 1 at 2026-11-12 10:00:00; see x\n");
	run_host(hook_status_host, &out);
	CHECK(contains(out, "kernel hook: the automatic build for kernel 7.0.99-1-pve failed") && !contains(out, "for kernel 6.8.0"),
	      "a kernel with no build is still reported: %s", out);
	free(out);
}

/*
 * The boot display GPU is selected and the installed build has no display modules: the firmware framebuffer
 * is not released for modules that are not there, so nothing is touched and the command that fixes it is named.
 */
static void test_activate_nodisplay_build(void)
{
	char root[GS_PATH_MAX], *out, *log;
	struct gs_build_src src;

	act_host("nodisplay-build", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	build_for(&src, root, 0);
	act_opts.build = &src;
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED &&
	      contains(out, "refused, nothing changed: 0000:04:00.0 is the boot display GPU, and the installed build of NVIDIA's module has no display") &&
	      contains(out, "sudo geforce-sriov install --no-display\n") &&
	      contains(out, "sudo ./geforce-sriov install --companion-file PATH --nvkms-source PATH") &&
	      !contains(out, "released") && !sim_exists("sim.log") && !sim_exists("state/" GS_ATTEMPT_NAME),
	      "no display modules, no inputs kept: %s", out);
	free(out);
	/* The inputs for the display modules were kept: rebuild is the command. */
	sim_writef("state/" GS_INPUTS_NAME "/index", "nvidia_source=/usr/src/nvidia-580.159.01\ncompanion=/x/c.run\nnvkms=/x/nvkms\n");
	CHECK(run_host(activate_host, &out) == GS_EXIT_REFUSED && contains(out, "To build them, run:\n  sudo geforce-sriov rebuild\n") &&
	      !contains(out, "--no-display") && !sim_exists("sim.log"), "inputs kept: %s", out);
	free(out);
	/* display=off: there is no console to hand over, so a build without display modules is right. */
	act_cfg.display = GS_DISPLAY_OFF;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "activation finished") &&
	      !contains(out, "boot display GPU, and the installed build"), "display=off: %s", out);
	free(out);

	/* A build with the display modules, or a host with no record of the build: the handoff goes on. */
	act_host("display-build", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	build_for(&src, root, 1);
	act_opts.build = &src;
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "firmware framebuffer simple-framebuffer.0 released"),
	      "a display build: %s", out);
	free(out);
	log = sim_text("sim.log");
	CHECK(contains(log, "simple-framebuffer.0/driver/unbind"), "the handoff ran: %s", log);
	free(log);
	act_host("display-norecord", &topo_display, gpu_display, 1, NULL, root, sizeof(root));
	CHECK(run_host(activate_host, &out) == GS_EXIT_OK && contains(out, "firmware framebuffer simple-framebuffer.0 released"),
	      "no build record: %s", out);
	free(out);
}

/* ---- install from the installed copy: a change of the selection ---------------------------------- */

static struct gs_install_opts sel_opts;
static struct gs_select_env sel_env;
static struct gs_build_src sel_build;
static char sel_dest[GS_PATH_MAX], sel_driver[GS_PATH_MAX], sel_args[GS_PATH_MAX], sel_drvlog[GS_PATH_MAX];

static int select_host(void)
{
	return gs_install_select(&sel_opts, &sel_env);
}

/*
 * A host with the Ampere card at 03:00.0 and the Ada card at 04:00.0 (the boot display when ada_boot), an
 * install of this tool that selected 03:00.0, and a driver step that records what it was asked.
 */
static void select_fixture(const char *name, int ada_boot, int display_record, const char *config)
{
	static char root[GS_PATH_MAX]; /* sel_env keeps a pointer to it */
	char p[GS_PATH_MAX], sha[65];

	act_host(name, &topo_default, gpu_ampere, 0, "0/4", root, sizeof(root));
	make_card(root, &topo_display, gpu_ada04, ada_boot, "0/4");
	gs_pathf(sel_dest, sizeof(sel_dest), "%s/dest", root);
	gs_pathf(p, sizeof(p), "%s" GS_CONFIG_PATH, sel_dest);
	{
		struct gs_config c;
		char text[4096];

		config_of(&c, config);
		if (gs_config_render(&c, text, sizeof(text)) < 0)
			die("render");
		writef(p, "%s", text);
		chmod(p, 0644);
	}
	if (gs_sha256_file(p, sha))
		die(p);
	gs_pathf(p, sizeof(p), "%s" GS_MANIFEST, sel_dest);
	writef(p, "f %s " GS_CONFIG_PATH "\n", sha);
	gs_pathf(sel_driver, sizeof(sel_driver), "%s/fake-driver", root);
	gs_pathf(sel_args, sizeof(sel_args), "%s/driver-args", root);
	gs_pathf(sel_drvlog, sizeof(sel_drvlog), "%s/driver.log", root);
	writef(sel_driver, "#!/bin/sh\necho \"$@\" >>%s\nexit 0\n", sel_args);
	chmod(sel_driver, 0755);
	if (display_record >= 0)
		build_for(&sel_build, root, display_record);
	memset(&sel_opts, 0, sizeof(sel_opts));
	sel_opts.ov.ari = sel_opts.ov.msix = GS_SW_UNSET;
	sel_env = (struct gs_select_env){ root, sel_dest, display_record >= 0 ? &sel_build : NULL, sel_driver, sel_drvlog, "6.8.0", NULL };
}

static char *sel_file(const char *rel)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s%s", sel_dest, rel);
	return slurp(p);
}

static void test_install_select(void)
{
	static const char cfg[] = "bdfs=0000:03:00.0\nidentity=on\nidentity_force_host=on\n";
	char *out, *text, p[GS_PATH_MAX];


	/* The selection changes: checked, written with the cards' identities, no file copied, the power cycle named. */
	select_fixture("select-device", 0, -1, cfg);
	sel_opts.devices[0] = (struct gs_addr){ 0 };
	gs_addr_parse("0000:04:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_OK && contains(out, "installed " ) && contains(out, "recorded the card at 0000:04:00.0: 10de:2783") &&
	      contains(out, "power the machine off and on") && contains(out, "no file was copied"), "device change: %s", out);
	free(out);
	text = sel_file(GS_CONFIG_PATH);
	CHECK(contains(text, "bdfs=0000:04:00.0") && !contains(text, "0000:03:00.0"), "the config names the new card: %s", text);
	free(text);
	text = sel_file(GS_SELECTION);
	CHECK(contains(text, "0000:04:00.0 10de:2783 10de:146d\n") && !contains(text, "0000:03:00.0"), "the record names it: %s", text);
	free(text);
	text = sel_file(GS_MANIFEST);
	CHECK(contains(text, " " GS_SELECTION "\n") && contains(text, " " GS_CONFIG_PATH "\n"), "the manifest lists the record: %s", text);
	free(text);
	gs_pathf(p, sizeof(p), "%s/usr", sel_dest);
	CHECK(!gs_exists(p) && !gs_exists(sel_args), "nothing was copied and the driver step did not run");
	/* The same command again changes nothing. */
	CHECK(run_host(select_host, &out) == GS_EXIT_OK && contains(out, "Nothing changed"), "again: %s", out);
	free(out);

	/* The Ada placement change: a selection that needs another build than the installed one runs the driver step, with
	 * --ada-selected when an Ada card is selected. A record without ada_placement is a build without the change. */
	for (int k = 0; k < 4; k++) {
		static const struct { const char *from, *to, *record; int runs, ada; } t[] = {
			{ "0000:03:00.0", "0000:04:00.0", "", 1, 1 },
			{ "0000:03:00.0", "0000:04:00.0", "ada_placement=1\n", 0, 1 },
			{ "0000:04:00.0", "0000:03:00.0", "ada_placement=1\n", 1, 0 },
			{ "0000:04:00.0", "0000:03:00.0", "ada_placement=0\n", 0, 0 },
		};
		char conf[256], name[64], *rec;

		snprintf(conf, sizeof(conf), "bdfs=%s\nidentity=on\nidentity_force_host=on\n", t[k].from);
		snprintf(name, sizeof(name), "select-ada-placement-%d", k);
		select_fixture(name, 0, 0, conf);
		rec = slurp(sel_build.record);
		writef(sel_build.record, "%s%s", rec ? rec : "", t[k].record);
		free(rec);
		sel_opts.devices[0] = (struct gs_addr){ 0 };
		gs_addr_parse(t[k].to, &sel_opts.devices[0]);
		sel_opts.ndevices = 1;
		CHECK(run_host(select_host, &out) == GS_EXIT_OK, "ada placement case %d: %s", k, out);
		free(out);
		CHECK(gs_exists(sel_args) == t[k].runs, "ada placement case %d: the driver step %s", k, t[k].runs ? "runs" : "does not run");
		if (t[k].runs) {
			text = slurp(sel_args);
			CHECK(contains(text, "--retained --kernel 6.8.0") && contains(text, "--ada-selected") == t[k].ada,
			      "ada placement case %d: --ada-selected is %s: %s", k, t[k].ada ? "passed" : "not passed", text ? text : "(none)");
			free(text);
		}
	}

	/* The activate unit installed earlier gets the start timeout of the new card count; an edited unit is kept. */
	for (int edited = 0; edited < 2; edited++) {
		char unit[GS_PATH_MAX], sha[65], want[64];
		char *t;

		select_fixture(edited ? "select-unit-edited" : "select-unit", 0, -1, cfg);
		gs_pathf(unit, sizeof(unit), "%s%s/" GS_UNIT_ACTIVATE, sel_dest, GS_UNIT_DIR);
		gs_mkdir_p(strrchr(unit, '/') ? (gs_pathf(p, sizeof(p), "%s%s", sel_dest, GS_UNIT_DIR), p) : p, 0755);
		writef(unit, "[Service]\nTimeoutStartSec=%u\nStandardOutput=journal\n", gs_activate_start_timeout_s(1, GS_DEFAULT_VFS));
		if (gs_sha256_file(unit, sha))
			die(unit);
		gs_pathf(p, sizeof(p), "%s" GS_MANIFEST, sel_dest);
		t = slurp(p);
		writef(p, "%sf %s %s/" GS_UNIT_ACTIVATE "\n", t, sha, GS_UNIT_DIR);
		free(t);
		if (edited)
			writef(unit, "[Service]\nTimeoutStartSec=77\n");
		sel_opts.devices[0] = (struct gs_addr){ 0 };
		sel_opts.devices[1] = (struct gs_addr){ 0 };
		gs_addr_parse("0000:03:00.0", &sel_opts.devices[0]);
		gs_addr_parse("0000:04:00.0", &sel_opts.devices[1]);
		sel_opts.ndevices = 2;
		CHECK(run_host(select_host, &out) == GS_EXIT_OK, "two cards: %s", out);
		free(out);
		t = slurp(unit);
		snprintf(want, sizeof(want), "TimeoutStartSec=%u\n", edited ? 77u : gs_activate_start_timeout_s(2, GS_DEFAULT_VFS));
		CHECK(t && contains(t, want) && contains(t, "StandardOutput=journal\n") == !edited,
		      "%s unit: the start timeout of two cards, the rest of the unit as it was: %s", edited ? "edited" : "installed", t ? t : "(missing)");
		free(t);
	}

	/* The record alone (the cards were selected before the record existed): no power cycle. */
	select_fixture("select-record", 0, -1, cfg);
	sel_opts.devices[0] = (struct gs_addr){ 0 };
	gs_addr_parse("0000:03:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_OK && contains(out, "only the record of the cards changed") &&
	      contains(out, "power the machine off and on if an earlier change still waits for it"), "record only: %s", out);
	free(out);

	/* An address that cannot be selected stops before anything is written. */
	select_fixture("select-refused", 0, -1, cfg);
	gs_addr_parse("0000:09:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_REFUSED && contains(out, "refused --device: 0000:09:00.0: no such PCI function"), "%s", out);
	free(out);
	text = sel_file(GS_CONFIG_PATH);
	gs_pathf(p, sizeof(p), "%s" GS_SELECTION, sel_dest);
	CHECK(contains(text, "bdfs=0000:03:00.0") && !gs_exists(p), "config and record untouched: %s", text);
	free(text);
	/* --disable-d3cold needs a laptop GPU among the selected. */
	select_fixture("select-d3cold", 0, -1, cfg);
	sel_opts.disable_d3cold = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_USAGE, "d3cold without a laptop GPU: %s", out);
	free(out);
	/* A host with no selection yet needs the source folder: it enables the boot units. */
	select_fixture("select-none", 0, -1, "ari=on\n");
	gs_addr_parse("0000:03:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_REFUSED && contains(out, "no GPU is selected yet"), "%s", out);
	free(out);
	/* A config this tool did not write is left alone. */
	select_fixture("select-foreign", 0, -1, cfg);
	gs_pathf(p, sizeof(p), "%s" GS_MANIFEST, sel_dest);
	writef(p, "%s", "");
	gs_addr_parse("0000:04:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_REFUSED && contains(out, "was not written by geforce-sriov"), "%s", out);
	free(out);

	/* The boot display joins the selection and the installed build has no display modules: with no kept inputs, the two forms. */
	select_fixture("select-display-refused", 1, 0, cfg);
	gs_addr_parse("0000:04:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_REFUSED && contains(out, "no earlier install kept them") &&
	      contains(out, "sudo geforce-sriov install --device 0000:04:00.0 --no-display\n") &&
	      contains(out, "sudo ./geforce-sriov install --device 0000:04:00.0 --companion-file PATH --nvkms-source PATH") &&
	      !gs_exists(sel_args), "display, nothing kept: %s", out);
	free(out);
	text = sel_file(GS_CONFIG_PATH);
	CHECK(contains(text, "bdfs=0000:03:00.0"), "the config is as it was: %s", text);
	free(text);
	/* With the inputs kept the driver step runs, as rebuild runs it, before the config is written. */
	select_fixture("select-display", 1, 0, cfg);
	gs_pathf(p, sizeof(p), "%s" GS_INPUTS_DIR "/index", sel_dest);
	writef(p, "companion=/x/c.run\nnvkms=/x/nvkms\n");
	gs_addr_parse("0000:04:00.0", &sel_opts.devices[0]);
	sel_opts.ndevices = 1;
	CHECK(run_host(select_host, &out) == GS_EXIT_OK && contains(out, "Display: automatic for selected boot-display GPU 0000:04:00.0") &&
	      contains(out, "== NVIDIA's files:") && contains(out, "NVIDIA's modules for 6.8.0 were rebuilt and installed (nvidia, nvidia-modeset, nvidia-drm)") &&
	      contains(out, "The power cycle loads them") && !contains(out, "no file was copied"), "display, inputs kept: %s", out);
	free(out);
	text = slurp(sel_args);
	CHECK(contains(text, "--retained --kernel 6.8.0 --display --no-host-cuda --ada-selected"), "the driver step builds the display modules, for an Ada card: %s", text ? text : "(none)");
	free(text);
	text = sel_file(GS_CONFIG_PATH);
	CHECK(contains(text, "bdfs=0000:04:00.0"), "then the config is written: %s", text);
	free(text);
	/* Leaving the display out of a build that has it: the driver step without it, and display=off in the config. */
	select_fixture("select-nodisplay", 0, 1, cfg);
	sel_opts.display_set = 1;
	sel_opts.display = GS_DISPLAY_OFF;
	CHECK(run_host(select_host, &out) == GS_EXIT_OK && contains(out, "Display: not enabled.") &&
	      contains(out, "NVIDIA's modules for 6.8.0 were rebuilt and installed (nvidia)") && !contains(out, "no file was copied"), "--no-display: %s", out);
	free(out);
	text = slurp(sel_args);
	CHECK(contains(text, "--kernel 6.8.0 --no-display --no-host-cuda"), "the driver step leaves the display modules out: %s", text ? text : "(none)");
	free(text);
	text = sel_file(GS_CONFIG_PATH);
	CHECK(contains(text, "display=off") && contains(text, "bdfs=0000:03:00.0"), "display=off, the selection kept: %s", text);
	free(text);
	/* A driver step that refuses leaves the config alone. */
	select_fixture("select-driver-fails", 0, 1, cfg);
	writef(sel_driver, "#!/bin/sh\necho 'geforce-sriov-driver: refused: no headers' >&2\nexit 3\n");
	sel_opts.display_set = 1;
	sel_opts.display = GS_DISPLAY_OFF;
	CHECK(run_host(select_host, &out) == GS_EXIT_REFUSED && contains(out, "NVIDIA's module and plugin are as they were, and the config is not written"), "%s", out);
	free(out);
	text = sel_file(GS_CONFIG_PATH);
	CHECK(!contains(text, "display=off"), "the config is as it was: %s", text);
	free(text);

	/* The command line: only selection options are taken from the installed copy; everything else keeps the refusal. */
	{
		char stage[GS_PATH_MAX], installed[GS_PATH_MAX];
		const char *built = cli;
		static const char *const refused[][6] = {
			{ "install", NULL }, { "install", "--vfs", "8", NULL }, { "install", "--host-cuda", NULL },
			{ "install", "--device", "0000:03:00.0", "--ari", "off", NULL }, { "install", "--driver", "skip", NULL },
			{ "install", "--device", "0000:03:00.0", "--prefix", "/usr", NULL },
		};

		gs_pathf(stage, sizeof(stage), "%s/stage-select", tmp);
		stage_install(&out, stage);
		free(out);
		gs_pathf(installed, sizeof(installed), "%s/usr/bin/geforce-sriov", stage);
		gs_pathf(p, sizeof(p), "%s/var/lib/geforce-sriov/" GS_SELECTION_NAME, stage);
		CHECK(!gs_exists(p), "a staged install records no card: it reads none");
		cli = installed;
		for (unsigned int i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
			CHECK(run_cli(&out, NULL, refused[i]) == GS_EXIT_REFUSED && contains(out, "is the installed copy") &&
			      contains(out, "sudo geforce-sriov install --device BDF"), "refused: install %s %s: %s",
			      refused[i][1] ? refused[i][1] : "", refused[i][2] ? refused[i][2] : "", out);
			free(out);
		}
		if (geteuid() != 0) {
			static const char *const taken[][5] = {
				{ "install", "--device", "0000:03:00.0", NULL }, { "install", "--disable-d3cold", NULL },
				{ "install", "--no-display", NULL }, { "install", "--display", "--device", "0000:03:00.0", NULL },
			};

			for (unsigned int i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
				CHECK(run_cli(&out, NULL, taken[i]) == GS_EXIT_REFUSED && contains(out, "a live install must run as root") &&
				      !contains(out, "is the installed copy"), "taken: install %s: %s", taken[i][1], out);
				free(out);
			}
		}
		cli = built;

	}
}

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : NULL;

/* "test_addr" -> "addr": the section line names the group, not its C symbol. */
#define RUN(fn) do { if (!only || strstr(#fn, only)) { set_section(section_name(#fn)); fn(); } } while (0)
	/* Line-buffered stdout: a CI cancellation must not lose the last section line. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGALRM, watchdog);
	alarm(120);
	/* Several tests run the CLI against the live system and rely on it refusing an unprivileged caller. */
	if (geteuid() == 0 && !getenv("GS_TEST_ALLOW_ROOT")) {
		fprintf(stderr, "test_lifecycle: refusing to run as root, where some tests could change this host; "
			"set GS_TEST_ALLOW_ROOT=1 to run anyway\n");
		return 2;
	}
	cli = getenv("GS_CLI");
	srcdir = getenv("GS_SRC");
	if (!cli || !srcdir) {
		fprintf(stderr, "set GS_CLI and GS_SRC (make test does)\n");
		return 2;
	}
	gs_test_template(tmp, sizeof(tmp), "gs-lifecycle-test.XXXXXX");
	if (!mkdtemp(tmp))
		die("mkdtemp");

	RUN(test_selection_identity);
	RUN(test_activate_nodisplay_build);
	RUN(test_install_select);
	RUN(test_addr);
	RUN(test_config);
	RUN(test_many_cards);
	RUN(test_switches);
	RUN(test_write_atomic);
	RUN(test_sha256);
	RUN(test_write_attr);
	RUN(test_live_deadline);
	RUN(test_resource_fit);
	RUN(test_display_plan);
	RUN(test_display_inspect);
	RUN(test_check);
	RUN(test_daemon_ready);
	RUN(test_livepatch_guard);
	RUN(test_livepatch_load_driver);
	RUN(test_unselected);
	RUN(test_activate_rescan);
	RUN(test_notify);
	RUN(test_hook_report_cleared);
	RUN(test_selection_many);
	RUN(test_start_timeout);
	RUN(test_poll_elapsed);
	RUN(test_activate_enable);
	RUN(test_activate_oneshot);
	RUN(test_activate_msix);
	RUN(test_activate_bounded);
	RUN(test_activate_gates);
	RUN(test_activate_display);
	RUN(test_activate_mixed);
	RUN(test_activate_display_on);
	RUN(test_kms_table);
	RUN(test_kms_reports);
	RUN(test_activate_d3cold_consent);
	RUN(test_activate_handoff_vfs);
	RUN(test_activate_stops);
	RUN(test_cli_smoke);
	RUN(test_driver_result);
	RUN(test_activate_write_timeouts);
	RUN(test_enable_prereq);
	RUN(test_activate_udev);
	RUN(test_activate_core_unused);
	RUN(test_activate_load_outcomes);
	RUN(test_activate_exit5_guard);
	RUN(test_activate_rerun);
	RUN(test_activate_refusals);
	RUN(test_clear_guard);
	RUN(test_rebuild);
	RUN(test_cuda_libs);
	RUN(test_host_desktop);
	RUN(test_dkms_ensure);
	RUN(test_first_install);
	RUN(test_package_copy);
	RUN(test_activate_nodes);
	RUN(test_fb_slots_ignored);
	RUN(test_slot_hint);
	RUN(test_ada_placement);
	RUN(test_version_policy);
	RUN(test_version_record);
	RUN(test_build_line);
	RUN(test_version_setup);
	RUN(test_loaded_patch);
	RUN(test_older_install);
	RUN(test_kernel_hook);
	RUN(test_stale_cap_files);
	RUN(test_kernel_install_pacman);
	RUN(test_rebuild_done_text);
	RUN(test_vm_autostart);
	RUN(test_autoload_scope);
	RUN(test_display_report);
	RUN(test_hostcheck_modes);
	RUN(test_unselected_units);
	RUN(test_manifest_remove);

	printf("test_lifecycle: %d checks, %d failures, %d skipped\n", checks, failures, skips);
	/* A passing run leaves nothing behind; a failing one keeps its fixtures, and so does GS_TEST_KEEP. */
	if (!failures && !getenv("GS_TEST_KEEP"))
		rm_rf(tmp);
	return failures ? 1 : 0;
}

