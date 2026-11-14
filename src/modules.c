// SPDX-License-Identifier: MIT
/*
 * NVIDIA's kernel modules: which are loaded, loading and unloading one with a deadline, and the
 * order in which the modules that sit on top of nvidia are unloaded.
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

/*
 * The modules that use nvidia, in the order they are unloaded: each one before what it depends on.
 * The first GS_NVIDIA_DISPLAY_MODULES are the display stack.
 */
const char *const gs_nvidia_clients[GS_NVIDIA_CLIENT_MODULES] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm" };

/* One host process through the deadline callback, or the synchronous one when there is none. */
int gs_host_run(const struct gs_host *h, const char *const argv[], unsigned int ms)
{
	gs_notify_extend(ms);
	if (h->run_deadline)
		return h->run_deadline(h, argv, ms);
	return h->run ? h->run(argv) : -ENOSYS;
}

/*
 * Wait for udev's event queue to empty (`udevadm settle`, at most GS_UDEV_SETTLE_S seconds). When it
 * did not (a timeout, an error, or no udevadm), the line says so and the caller goes on; returns non-zero then.
 */
int gs_udev_settle(const struct gs_host *h)
{
	const char *const settle[] = { "udevadm", "settle", "--timeout=" GS_UDEV_SETTLE_S, NULL };
	int r;

	printf("udevadm settle --timeout=" GS_UDEV_SETTLE_S "\n");
	r = gs_host_run(h, settle, GS_UDEV_SETTLE_MS);
	if (r == -ENOENT)
		printf("udevadm was not found, so udev's queue was not waited for; a driver_override rule of yours may not have applied yet\n");
	else if (r)
		printf("udev has not finished its queue after " GS_UDEV_SETTLE_S " s; a driver_override rule of yours may not have applied yet\n");
	return r;
}

/* 1 when h->sys/module/name exists. */
int gs_module_loaded(const struct gs_host *h, const char *name)
{
	char p[GS_PATH_MAX];

	return !gs_pathf(p, sizeof(p), "%s/module/%s", h->sys, name) && gs_exists(p);
}

/* 1 when the node name (as on the host, "/dev/...") does not exist under the host's root; it is then noted in n. */
static int node_missing(const struct gs_host *h, struct gs_dev_nodes *n, const char *name)
{
	char p[GS_PATH_MAX];
	struct stat st;

	if (gs_pathf(p, sizeof(p), "%s%s", h->root ? h->root : "", name) || lstat(p, &st) == 0)
		return 0;
	if (n->nmissing < GS_DEV_NODES_MAX)
		gs_copy(n->missing[n->nmissing++], sizeof(n->missing[0]), name);
	return 1;
}

/* The unsigned number after "key:" on a line of the text at path; -1 when there is none. */
long gs_proc_field(const char *path, const char *key)
{
	char *data, *line, *save = NULL;
	size_t len, klen = strlen(key);
	long v = -1;

	if (gs_read_whole(path, &data, &len, 1 << 16))
		return -1;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *end;

		if (strncmp(line, key, klen) != 0 || line[klen] != ':')
			continue;
		line += klen + 1;
		while (*line == ' ' || *line == '\t')
			line++;
		v = strtol(line, &end, 10);
		if (end == line || v < 0)
			v = -1;
		break;
	}
	free(data);
	return v;
}

static int by_minor(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;

	return x < y ? -1 : x > y;
}

void gs_dev_nodes_scan(const struct gs_host *h, struct gs_dev_nodes *n)
{
	static const char *const caps[] = { "mig/config", "mig/monitor" };
	unsigned int minors[GS_DEV_NODES_MAX], nminors = 0, ncaps = 0, a = 0;
	char p[GS_PATH_MAX], name[48];
	size_t off;
	int modeset = 0;

	memset(n, 0, sizeof(*n));
	if (!gs_module_loaded(h, "nvidia"))
		return;
	if (node_missing(h, n, "/dev/nvidiactl"))
		minors[nminors++] = GS_NVIDIA_CTL_MINOR;
	if (!gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus", h->proc)) {
		struct dirent *de;
		DIR *d = opendir(p);

		while (d && (de = readdir(d))) {
			long minor;

			if (de->d_name[0] == '.')
				continue;
			/* NVIDIA's programs make a node for each GPU they list, and they do not list a VF. */
			if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", h->sys, de->d_name) || gs_exists(p))
				continue;
			if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/information", h->proc, de->d_name))
				continue;
			minor = gs_proc_field(p, "Device Minor");
			if (minor < 0 || minor >= (long)GS_NVIDIA_CTL_MINOR)
				continue;
			snprintf(name, sizeof(name), "/dev/nvidia%ld", minor);
			if (node_missing(h, n, name) && nminors < GS_DEV_NODES_MAX)
				minors[nminors++] = (unsigned int)minor;
		}
		if (d)
			closedir(d);
	}
	/* The order of the directory is not the order of the command. */
	qsort(minors, nminors, sizeof(minors[0]), by_minor);
	if (gs_module_loaded(h, "nvidia_modeset"))
		modeset = node_missing(h, n, "/dev/nvidia-modeset");
	n->core_argv[a++] = "nvidia-modprobe";
	off = (size_t)snprintf(n->core, sizeof(n->core), "nvidia-modprobe");
	for (unsigned int i = 0; i < nminors; i++) {
		snprintf(n->minors[i], sizeof(n->minors[0]), "%u", minors[i]);
		n->core_argv[a++] = "-c";
		n->core_argv[a++] = n->minors[i];
		off += (size_t)snprintf(n->core + off, sizeof(n->core) - off, " -c %s", n->minors[i]);
	}
	if (modeset) {
		n->core_argv[a++] = "-m";
		off += (size_t)snprintf(n->core + off, sizeof(n->core) - off, " -m");
	}
	for (unsigned int i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		long minor;

		char *path = n->caps[ncaps];

		if (gs_pathf(path, sizeof(n->caps[0]), "%s/driver/nvidia/capabilities/%s", h->proc, caps[i]))
			continue;
		minor = gs_proc_field(path, "DeviceFileMinor");
		if (minor < 0)
			continue;
		snprintf(name, sizeof(name), "/dev/nvidia-caps/nvidia-cap%ld", minor);
		if (!node_missing(h, n, name) || off + strlen(path) + 5 >= sizeof(n->core))
			continue;
		n->core_argv[a++] = "-f";
		n->core_argv[a++] = path;
		off += (size_t)snprintf(n->core + off, sizeof(n->core) - off, " -f %s", path);
		ncaps++;
	}
	n->core_argv[a] = NULL;
	if (a == 1)
		n->core[0] = '\0';
	if (gs_module_loaded(h, "nvidia_uvm") &&
	    (node_missing(h, n, "/dev/nvidia-uvm") | node_missing(h, n, "/dev/nvidia-uvm-tools"))) {
		static const char *const uvm[] = { "nvidia-modprobe", "-u", "-c", "0", NULL };

		memcpy(n->uvm_argv, uvm, sizeof(uvm));
		gs_copy(n->uvm, sizeof(n->uvm), "nvidia-modprobe -u -c 0");
	}
}

void gs_dev_nodes_list(const struct gs_dev_nodes *n, char *buf, size_t len)
{
	size_t off = 0;

	buf[0] = '\0';
	for (unsigned int i = 0; i < n->nmissing && off < len; i++)
		off += (size_t)snprintf(buf + off, len - off, "%s%s", i ? ", " : "", n->missing[i]);
}

/*
 * modprobe name [opts...] with a deadline. A load stuck in the kernel is uncertain device state:
 * the caller reports exit 5 and keeps the guard. force adds --ignore-install: the display load
 * must run even while the block file of the enable window is in place.
 */
int gs_module_load(const struct gs_host *h, int force, const char *name, const char *const *opts, unsigned int nopts,
		   char *why, size_t len)
{
	const char *argv[5 + GS_CFG_MAX_BDFS];
	unsigned int n = 0;
	int r;

	argv[n++] = "modprobe";
	if (force)
		argv[n++] = "--ignore-install";
	argv[n++] = name;
	for (unsigned int i = 0; i < nopts && n < sizeof(argv) / sizeof(argv[0]) - 1; i++)
		argv[n++] = opts[i];
	argv[n] = NULL;
	printf("modprobe %s%s", force ? "--ignore-install " : "", name);
	for (unsigned int i = 0; i < nopts; i++)
		printf(" %s", opts[i]);
	putchar('\n');
	r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		snprintf(why, len, "modprobe %s did not finish within %u ms; whether it loaded is unknown", name,
			 GS_MODULE_DEADLINE_MS);
		return -ETIMEDOUT;
	}
	/*
	 * modprobe exits with 0 for a module that is already loaded, so a failure with the module
	 * present is not "already there". Something went wrong during or after the insertion, or
	 * another loader put the module in meanwhile, and then not with these options. The module is
	 * neither absent nor known to be what was asked for: the caller treats that as uncertain.
	 */
	if (r != 0 && gs_module_loaded(h, name)) {
		snprintf(why, len, "modprobe %s exited with %d, but the module is loaded; whether this command loaded it, and with "
			 "the options given, is unknown (`journalctl -k -b`)", name, r);
		return -ETIMEDOUT;
	}
	if (r != 0 || !gs_module_loaded(h, name)) {
		snprintf(why, len, "modprobe %s exited with %d%s; `journalctl -k -b` says why", name, r,
			 r ? "" : " but the module is not loaded");
		return -1;
	}
	return 0;
}

/*
 * Unload one module with `rmmod name`, bounded, never forced; 0 or -1 with why. rmmod removes
 * exactly the module it is given. `modprobe -r` also removes the modules it depends on once they
 * are unused, so unloading nvidia_drm could take nvidia with it and unbind every GPU.
 */
int gs_module_unload(const struct gs_host *h, const char *name, char *why, size_t len)
{
	const char *argv[] = { "rmmod", name, NULL };
	int r;

	if (!gs_module_loaded(h, name))
		return 0;
	printf("rmmod %s\n", name);
	r = gs_host_run(h, argv, GS_MODULE_DEADLINE_MS);
	if (r == -ETIMEDOUT) {
		snprintf(why, len, "rmmod %s did not finish within %u ms; whether it unloaded is unknown", name,
			 GS_MODULE_DEADLINE_MS);
		return -ETIMEDOUT;
	}
	if (r != 0 || gs_module_loaded(h, name)) {
		snprintf(why, len, "rmmod %s exited with %d%s", name, r,
			 gs_module_loaded(h, name) ? " and the module is still loaded" : "");
		return -1;
	}
	return 0;
}

/* 1 when a boolean module parameter reads true; sysfs prints Y, and 1 is accepted as well. */
int gs_param_bool(const char *v)
{
	return !strcmp(v, "Y") || !strcmp(v, "1");
}
