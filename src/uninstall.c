// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gs_lifecycle.h"
#include "gs_internal.h"

/* ---- uninstall ------------------------------------------------------------ */

/*
 * Active VFs on a GPU the module acts on. Its bdfs parameter is the authoritative list
 * (the config only adds to it). When that list cannot be read or parsed, safety cannot be
 * proven and the caller refuses. Other SR-IOV devices (network cards) never count.
 */
static int any_active_vfs(char *which, size_t len)
{
	struct gs_addr list[2 * GS_CFG_MAX_BDFS];
	unsigned int n = 0, k;
	struct gs_config cfg;
	char param[256], err[512];

	if (gs_read_attr(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME "/parameters/bdfs", param, sizeof(param)) ||
	    gs_parse_addr_list("bdfs", param, list, &n, err, sizeof(err))) {
		snprintf(which, len, "the loaded livepatch module's bdfs parameter cannot be read or parsed, so active VFs on its GPUs cannot be ruled out");
		return 1;
	}
	if (gs_config_load(gs_config_path(), &cfg, err, sizeof(err)) == 0)
		for (k = 0; k < cfg.nbdfs && n < 2 * GS_CFG_MAX_BDFS; k++)
			list[n++] = cfg.bdfs[k];
	for (k = 0; k < n; k++) {
		char p[GS_PATH_MAX];
		unsigned int v = 0;

		gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/bus/pci/devices/%s/sriov_numvfs", list[k].str);
		if (gs_read_uint(p, &v) == 0 && v > 0) {
			snprintf(which, len, "%s has %u VFs", list[k].str, v);
			return 1;
		}
	}
	return 0;
}

/* activation writes this before its first change and removes it only when every VF reads back ready. */
static int interrupted_activation(void)
{
	return gs_exists(GS_RUN_DIR "/" GS_ACTIVATING_NAME);
}

static int livepatch_unload(void)
{
	char p[GS_PATH_MAX], why[256];
	/* rmmod needs no modules index, so it works whatever state DKMS is in. */
	const char *argv[] = { "rmmod", GS_MODULE_NAME, NULL };

	if (!gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME)) {
		printf("%s is not loaded\n", GS_MODULE_NAME);
		return 0;
	}
	if (any_active_vfs(why, sizeof(why))) {
		fprintf(stderr, "geforce-sriov: refused --unload: %s; disable the VFs, or power the machine off and on instead\n", why);
		return GS_EXIT_REFUSED;
	}
	if (interrupted_activation()) {
		fprintf(stderr, "geforce-sriov: refused --unload: an activation was interrupted, so the card state is unknown; power the machine off and on instead\n");
		return GS_EXIT_REFUSED;
	}
	gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/kernel/livepatch/%s/enabled", GS_MODULE_NAME);
	if (gs_exists(p) && gs_write_attr(p, "0")) {
		fprintf(stderr, "geforce-sriov: disabling livepatch %s failed\n", GS_MODULE_NAME);
		return GS_EXIT_FAIL;
	}
	gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/kernel/livepatch/%s", GS_MODULE_NAME);
	for (unsigned int ms = 0; gs_exists(p); ms += 100) {
		char tp[GS_PATH_MAX];
		unsigned int t = 1, e = 1;

		gs_pathf(tp, sizeof(tp), "%s/transition", p);
		if (gs_read_uint(tp, &t) == 0 && t == 0) {
			gs_pathf(tp, sizeof(tp), "%s/enabled", p);
			if (gs_read_uint(tp, &e) == 0 && e == 0)
				break;
		}
		if (ms >= 15000) {
			fprintf(stderr, "geforce-sriov: livepatch %s did not finish unpatching; module left loaded\n",
				GS_MODULE_NAME);
			return GS_EXIT_FAIL;
		}
		gs_sleep_ms(100);
	}
	if (gs_run(argv) != 0) {
		fprintf(stderr, "geforce-sriov: rmmod %s failed\n", GS_MODULE_NAME);
		return GS_EXIT_FAIL;
	}
	printf("%s unloaded\n", GS_MODULE_NAME);
	return 0;
}

/*
 * Uninstall's removal of one manifest file: removed only while install still owns it (gs_manifest_owns()),
 * so a link put in its place is kept even when it points at identical content. 1 removed; 0 kept
 * (saying why) or absent; -errno when the unlink failed.
 */
int gs_uninstall_file(const char *path, const char *want)
{
	switch (gs_manifest_owns(path, want)) {
	case OWN_ABSENT:
		return 0;
	case OWN_UNREADABLE:
		printf("kept %s (unreadable)\n", path);
		return 0;
	case OWN_CHANGED:
		printf("kept %s (changed since install)\n", path);
		return 0;
	case OWN_YES:
		break;
	}
	if (unlink(path) < 0) {
		int e = errno;

		fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", path, strerror(e));
		return -e;
	}
	printf("removed %s\n", path);
	return 1;
}

/* driver --restore: 0, or its exit status with in why its reason, or its exit status when it gave none. */
static int run_restore(const char *driver, char *why, size_t len)
{
	const char *const argv[] = { driver, "--restore", NULL };
	int rc = gs_driver_step(argv, NULL, why, len, NULL);

	if (rc < 0)
		snprintf(why, len, "%.900s --restore could not run", driver);
	else if (rc && !why[0])
		snprintf(why, len, "%.900s --restore exited with %d", driver, rc);
	return rc;
}

/* The nvidia.ko the driver step restores for kernel release: modinfo's, else the DKMS path. */
static void restore_target(const char *release, char *module, size_t len)
{
	if (gs_nvidia_module_path(release, module, len))
		gs_pathf(module, len, "/lib/modules/%s/updates/dkms/nvidia.ko", release);
}

/*
 * After a restore: what is left under state/stock. A backup of nvidia.ko that is still there was not
 * put back, for another kernel or because the installed module was not this package's; the command
 * that puts it back is printed. A directory without it holds backups of other modules that were kept,
 * which the restore named when it kept them. A restore that put everything back leaves nothing here.
 */
static void other_kernels(const char *state)
{
	char stock[GS_PATH_MAX], p[GS_PATH_MAX], module[GS_PATH_MAX], core[GS_PATH_MAX];
	struct dirent **names;
	struct stat st;
	int n, i, any = 0;

	if (gs_pathf(stock, sizeof(stock), "%s/stock", state))
		return;
	n = scandir(stock, &names, NULL, alphasort);
	for (i = 0; i < n; i++) {
		const char *k = names[i]->d_name;

		if (k[0] != '.' && !gs_pathf(p, sizeof(p), "%s/%s", stock, k) && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			if (gs_pathf(core, sizeof(core), "%s/nvidia.ko", p) || !gs_exists(core)) {
				printf("kept %s: it holds the backups of modules that were not put back (see the lines above)\n", p);
				free(names[i]);
				continue;
			}
			if (!any++)
				printf("A backup of NVIDIA's module was not put back for these kernels; to put one back, as root:\n");
			restore_target(k, module, sizeof(module));
			printf("restore %s: cp %s %s && depmod -a %s\n", k, core, module, k);
		}
		free(names[i]);
	}
	if (n >= 0)
		free(names);
}

/*
 * The hold of nvidia in modprobe_dir, which is the user's file and stays. A hold file install wrote and
 * uninstall kept (changed since install) is still listed in m, and its "kept" line already names it.
 */
static void hold_left(const char *modprobe_dir, struct manifest *m)
{
	char files[2 * GS_PATH_MAX + 8];

	if (!gs_nvidia_hold(modprobe_dir, files, sizeof(files)) && !gs_manifest_find(m, 'f', files))
		printf("%s still holds nvidia back at boot; remove it if nothing else needs it\n", files);
}

/*
 * NVIDIA's package as --driver-file extracted it stays in state/nvidia: a reinstall uses it again, and
 * NVIDIA's own installation came from it. Say that it is there and how to remove it.
 */
static void package_left(const char *state)
{
	char dir[GS_PATH_MAX];
	struct dirent **names;
	int n, others = 0;

	if (gs_pathf(dir, sizeof(dir), "%s/nvidia", state) || !gs_exists(dir))
		return;
	n = scandir(state, &names, NULL, alphasort);
	for (int i = 0; i < n; i++) {
		others += names[i]->d_name[0] != '.' && strcmp(names[i]->d_name, "nvidia") != 0;
		free(names[i]);
	}
	if (n >= 0)
		free(names);
	if (others)
		printf("kept %s/: NVIDIA's package as --driver-file extracted it. geforce-sriov no longer uses it;\n"
		       "delete it with `rm -r %s` when you do not need it.\n", dir, dir);
	else
		printf("kept %s/: NVIDIA's package as --driver-file extracted it, and the only thing left in %s.\n"
		       "geforce-sriov no longer uses it; `rm -r %s` removes both when you do not need it.\n", dir, state, state);
}

int gs_uninstall_files(const char *path, const char *modprobe_dir, const char *const keep[])
{
	static const char suffix[] = "/geforce-sriov-driver";
	struct manifest *m = calloc(1, sizeof(*m));
	const char *slash = strrchr(path, '/');
	char driver[GS_PATH_MAX] = "", driver_sha[65] = "", why[1024] = "", dir[GS_PATH_MAX] = "";
	unsigned int i, kept = 0, files = 0;
	int r, rc = GS_EXIT_OK, restored = 1, changed = 0;

	if (!m)
		return GS_EXIT_FAIL;
	if ((r = gs_manifest_load(path, m))) {
		fprintf(stderr, "geforce-sriov: %s is corrupt (%s); nothing removed\n", path, strerror(-r));
		free(m);
		return GS_EXIT_FAIL;
	}
	/*
	 * The driver step keeps NVIDIA's backup and its own record outside the install manifest, so ask it
	 * to put the stock module back. It restores only while the installed module is still the one it
	 * installed, and reports when there is nothing to do. The path comes from the manifest, because
	 * --prefix can put it anywhere, and the script runs as root only while it is still the file install
	 * wrote (gs_manifest_owns()). When it fails or is not run, it stays, so the user can run it again.
	 */
	for (i = 0; i < m->n && !driver[0]; i++) {
		size_t len = strlen(m->e[i].path);

		if (m->e[i].type == 'f' && len > sizeof(suffix) - 1 &&
		    strcmp(m->e[i].path + len - (sizeof(suffix) - 1), suffix) == 0) {
			gs_copy(driver, sizeof(driver), m->e[i].path);
			gs_copy(driver_sha, sizeof(driver_sha), m->e[i].sha);
		}
	}
	if (driver[0] && !gs_exists(driver)) {
		printf("kept NVIDIA's files: %s is already gone\n", driver);
	} else if (driver[0] && gs_manifest_owns(driver, driver_sha) != OWN_YES) {
		snprintf(why, sizeof(why), "%.900s changed since install, not run", driver);
		restored = 0;
		changed = 1;
	} else if (driver[0] && run_restore(driver, why, sizeof(why)) != 0) {
		restored = 0;
	}
	/* The links first: each is this package's while it still names the file it was made for. */
	for (i = 0; i < m->n; i++) {
		char to[sizeof(m->e[i].sha)];
		ssize_t n;

		if (m->e[i].type != 'l' || !gs_exists(m->e[i].path))
			continue;
		n = readlink(m->e[i].path, to, sizeof(to) - 1);
		if (n <= 0 || (to[n] = '\0', strcmp(to, m->e[i].sha) != 0))
			printf("kept %s (changed since install)\n", m->e[i].path);
		else if (unlink(m->e[i].path) == 0)
			printf("removed %s\n", m->e[i].path);
		else
			rc = GS_EXIT_FAIL;
	}
	for (i = 0; i < m->n; i++) {
		size_t k = 0;

		if (m->e[i].type != 'f' || (!restored && strcmp(m->e[i].path, driver) == 0))
			continue;
		while (keep && keep[k] && strcmp(keep[k], m->e[i].path) != 0)
			k++;
		if ((!keep || !keep[k]) && gs_uninstall_file(m->e[i].path, m->e[i].sha) < 0)
			rc = GS_EXIT_FAIL;
	}
	for (i = m->n; i-- > 0;)
		if (m->e[i].type == 'd' && rmdir(m->e[i].path) == 0)
			printf("removed %s/\n", m->e[i].path);
	/* What is still there stays in the manifest, so a later install takes it over and a later uninstall removes it. */
	for (i = 0; i < m->n; i++) {
		if (!gs_exists(m->e[i].path))
			continue;
		files += m->e[i].type == 'f' || m->e[i].type == 'l';
		m->e[kept++] = m->e[i];
	}
	m->n = kept;
	if (slash && slash != path)
		gs_pathf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
	/*
	 * The inputs the driver step kept for a later build are beside the manifest and are not installed
	 * files. They go once NVIDIA's files are back; while a restore is still owed they stay with it.
	 */
	if (dir[0]) {
		/* What an install --driver-file that was killed left behind: the copy of the package it ran. */
		char left[GS_PATH_MAX];
		const char *rm_left[] = { "rm", "-rf", "--", left, NULL };

		if (!gs_pathf(left, sizeof(left), "%s/" GS_PACKAGE_COPY_NAME, dir) && gs_exists(left) && gs_run(rm_left) == 0)
			printf("removed %s/ (left by an install that did not finish)\n", left);
	}
	if (restored && dir[0]) {
		char inputs[GS_PATH_MAX];
		const char *rm_argv[] = { "rm", "-rf", "--", inputs, NULL };

		if (!gs_pathf(inputs, sizeof(inputs), "%s/" GS_INPUTS_NAME, dir) && gs_exists(inputs)) {
			if (gs_run(rm_argv) == 0) {
				printf("removed %s/ (the build inputs kept for a later kernel)\n", inputs);
			} else {
				fprintf(stderr, "geforce-sriov: removing %s failed\n", inputs);
				rc = GS_EXIT_FAIL;
			}
		}
	}
	if (restored) {
		if (files) {
			if ((r = gs_manifest_save(path, m))) {
				fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", path, strerror(-r));
				rc = GS_EXIT_FAIL;
			} else {
				printf("kept %s: it lists the files kept above, so a later install or uninstall still owns them\n",
				       path);
			}
		} else if (unlink(path) < 0 && errno != ENOENT) {
			rc = GS_EXIT_FAIL;
		} else if (dir[0] && rmdir(dir) == 0) {
			/* The manifest's directory (GS_STATE_DIR) goes too once nothing else is in it. */
			printf("removed %s/\n", dir);
		}
		if (dir[0]) {
			other_kernels(dir);
			package_left(dir);
		}
		hold_left(modprobe_dir, m);
		free(m);
		return rc;
	}
	if ((r = gs_manifest_save(path, m)))
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", path, strerror(-r));
	{
		struct utsname u;
		char module[GS_PATH_MAX] = "";
		const char *release = uname(&u) == 0 ? u.release : "<kernel>";

		restore_target(release, module, sizeof(module));
		/* A script changed since install is not to be run as root: point at the reinstall instead. */
		if (changed)
			fprintf(stderr, "NVIDIA's module was not restored: %s. Check that file, or put the installed copy back "
				"with `geforce-sriov install`, then run uninstall again; or as root: cp ", why);
		else
			fprintf(stderr, "NVIDIA's module was not restored: %s. Fix that, then run %s --restore, or as root: cp ",
				why, driver);
		fprintf(stderr, GS_STATE_DIR "/stock/%s/nvidia.ko %s && depmod -a %s. For the plugin link and the modprobe "
			"file, see docs/driver-patch.md#reverting.\n", release, module, release);
	}
	hold_left(modprobe_dir, m);
	free(m);
	return GS_EXIT_FAIL;
}

int gs_cmd_uninstall(int unload)
{
	static const char *const units[] = { GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE, GS_LEGACY_UNIT_PREPARE };
	static const char *const files[] = { UNIT_DIR "/" GS_UNIT_LIVEPATCH, UNIT_DIR "/" GS_UNIT_ACTIVATE,
					     UNIT_DIR "/" GS_LEGACY_UNIT_PREPARE };
	const char *keep[3] = { NULL, NULL, NULL };
	unsigned int nkeep = 0;
	struct manifest *m;
	char prog[GS_PATH_MAX];
	int r, rc = GS_EXIT_OK, cuda;

	if (geteuid() != 0) {
		fprintf(stderr, "geforce-sriov: uninstall must run as root\n");
		return GS_EXIT_REFUSED;
	}
	if ((r = gs_lock()))
		return r;
	/* The owner of the Ada framebuffer slots lets go first: it has the GPU open while it holds one. */
	gs_fb_release(&gs_host_live);
	/*
	 * Unload first: after `dkms remove` the module is gone from the modprobe index and
	 * `modprobe -r` can no longer find it. livepatch_unload() refuses with active VFs or a
	 * interrupted activation, and then nothing is removed.
	 */
	if (unload && (r = livepatch_unload())) {
		fprintf(stderr, "geforce-sriov: nothing removed\n");
		return r;
	}
	m = calloc(1, sizeof(*m));
	if (!m)
		return GS_EXIT_FAIL;
	if ((r = gs_manifest_load(GS_MANIFEST, m))) {
		fprintf(stderr, "geforce-sriov: %s is corrupt (%s); nothing removed\n", GS_MANIFEST, strerror(-r));
		free(m);
		return GS_EXIT_FAIL;
	}
	r = m->n;
	/* NVIDIA's CUDA libraries have a file that names their directory to the loader: its cache is made again below. */
	cuda = gs_manifest_find(m, 'f', GS_CUDA_CONF) != NULL;
	free(m);
	if (r == 0) {
		printf("nothing installed by geforce-sriov (%s absent)\n", GS_MANIFEST);
		return GS_EXIT_OK;
	}

	/* A unit whose disable failed keeps its file, so that uninstall run again still finds it to disable. */
	for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
		const char *argv[] = { "systemctl", "disable", units[u], NULL };

		if (gs_find_program("systemctl", prog, sizeof(prog)) || !gs_exists(files[u]) || gs_run(argv) == 0)
			continue;
		fprintf(stderr, "geforce-sriov: systemctl disable %s failed; kept %s so that uninstall run again\n"
			"disables and removes it\n", units[u], files[u]);
		keep[nkeep++] = files[u];
		rc = GS_EXIT_FAIL;
	}
	if (gs_exists("/var/lib/dkms/" GS_DKMS_NAME)) {
		const char *argv[] = { "dkms", "remove", DKMS_ID, "--all", NULL };

		if (gs_run(argv) != 0) {
			fprintf(stderr, "geforce-sriov: dkms remove %s failed\n", DKMS_ID);
			rc = GS_EXIT_FAIL;
		}
	}
	/* NVIDIA's CUDA libraries that a driver step left for an install that did not finish. */
	if (gs_exists(GS_CUDA_STAGE)) {
		const char *argv[] = { "rm", "-rf", "--", GS_CUDA_STAGE, NULL };

		if (gs_run(argv) == 0)
			printf("removed %s/\n", GS_CUDA_STAGE);
	}
	/* The driver step's log is not an installed file; it goes with the rest. */
	if (unlink(GS_DRIVER_LOG) == 0)
		printf("removed %s\n", GS_DRIVER_LOG);
	if (gs_uninstall_files(GS_MANIFEST, "/etc/modprobe.d", keep) != GS_EXIT_OK)
		rc = GS_EXIT_FAIL;
	if (cuda && !gs_exists(GS_CUDA_CONF)) {
		const char *argv[] = { "ldconfig", NULL };

		if (gs_run(argv) != 0)
			fprintf(stderr, "geforce-sriov: warning: ldconfig failed. Run `ldconfig` as root, so that the loader forgets NVIDIA's CUDA\n"
				"libraries, which are removed.\n");
	}
	/* The preparation guard is not an installed file: a reinstall must still see it. */
	if (gs_exists(GS_ATTEMPT_MARKER))
		printf("kept activation lock %s: an activation did not finish, so power the machine off and on.\n"
		       "The lock blocks activation after a reinstall until `geforce-sriov activate --clear-lock` clears it;\n"
		       "without a reinstall, delete the file after the power cycle.\n", GS_ATTEMPT_MARKER);
	if (gs_find_program("systemctl", prog, sizeof(prog)) == 0) {
		const char *argv[] = { "systemctl", "daemon-reload", NULL };

		gs_run(argv);
	}
	if (!unload && gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME))
		printf("%s stays loaded until the next boot.\n", GS_MODULE_NAME);
	if (gs_exists(GS_LIVE_SYSFS "/module/nvidia") || gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME))
		printf("Next: power the machine off and on. The loaded modules and the GPUs keep their current state until then,\n"
		       "and the next boot starts without geforce-sriov.\n");
	return rc;
}
