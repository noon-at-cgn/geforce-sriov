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

int gs_dkms_step(int (*run)(const char *const argv[]), int registered, const char *scratch)
{
	const char *const add_argv[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const build_argv[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const inst_argv[] = { "dkms", "install", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const disable_argv[] = { "systemctl", "disable", GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE, NULL };
	const char *const rm_argv[] = { "dkms", "remove", DKMS_ID, "--all", NULL };
	int disabled;

	if (registered) {
		char tree[GS_PATH_MAX];
		const char *const try_add[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "--dkmstree", tree, NULL };
		const char *const try_build[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "--dkmstree", tree, NULL };
		const char *const clean_argv[] = { "rm", "-rf", "--", tree, NULL };
		int built;

		if (gs_pathf(tree, sizeof(tree), "%s/geforce-sriov-dkms.XXXXXX", scratch) || !mkdtemp(tree)) {
			fprintf(stderr, "geforce-sriov: cannot create a scratch DKMS tree in %s; the installed %s module is unchanged\n",
				scratch, DKMS_ID);
			return GS_EXIT_FAIL;
		}
		printf("\n== %s is registered: a trial build in %s first; the installed livepatch module stays until it succeeds\n",
		       DKMS_ID, tree);
		fflush(stdout);
		built = run(try_add) == 0 && run(try_build) == 0;
		if (run(clean_argv) != 0)
			fprintf(stderr, "geforce-sriov: removing the scratch tree %s failed; remove it yourself\n", tree);
		if (!built) {
			fprintf(stderr, "geforce-sriov: %s does not build for the running kernel (DKMS's output is above). Nothing\n"
				"was removed: the livepatch module installed earlier stays registered and loads at the next boot, and the boot\n"
				"units are unchanged. Fix the build, then run `geforce-sriov install` again.\n", DKMS_ID);
			return GS_EXIT_FAIL;
		}
		if (run(rm_argv) != 0) {
			fprintf(stderr, "geforce-sriov: dkms remove %s --all failed (DKMS's output is above), so the livepatch module installed\n"
				"earlier may be gone. Fix the cause, then run `geforce-sriov install` again.\n", DKMS_ID);
			return GS_EXIT_FAIL;
		}
	}
	if (run(add_argv) == 0 && run(build_argv) == 0 && run(inst_argv) == 0)
		return GS_EXIT_OK;
	if (!registered) {
		fprintf(stderr, "geforce-sriov: DKMS could not add/build/install %s for the running kernel; files are installed, `geforce-sriov uninstall` removes them\n",
			DKMS_ID);
		return GS_EXIT_FAIL;
	}
	/* The module installed earlier is gone: the next boot must not try to load it. */
	disabled = run(disable_argv) == 0;
	fprintf(stderr, "geforce-sriov: DKMS could not add, build or install %s for the running kernel although the trial\n"
		"build succeeded, and `dkms remove %s --all` had already removed the livepatch module installed earlier.\n"
		"%s\nFix the cause in DKMS's output above, then run `geforce-sriov install` again%s.\n",
		DKMS_ID, DKMS_ID,
		disabled ? "So that the next boot does not try to load it, " GS_UNIT_LIVEPATCH " and " GS_UNIT_ACTIVATE
			   " are disabled."
			 : "Disabling " GS_UNIT_LIVEPATCH " and " GS_UNIT_ACTIVATE " failed: the next boot's activation fails to "
			   "load the livepatch module unless you run `systemctl disable " GS_UNIT_LIVEPATCH " " GS_UNIT_ACTIVATE "`.",
		disabled ? "; it enables them when GPUs are selected" : "");
	return GS_EXIT_FAIL;
}

/* Does status, the output of `dkms status`, say that a module is installed for release? Both layouts name the kernel between ", " and ",". */
static int dkms_installed_for(const char *status, const char *release)
{
	const size_t n = strlen(release);

	for (const char *line = status; *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line)) {
		size_t len = strcspn(line, "\n");
		const char *state = memchr(line, ':', len);

		if (!state || strncmp(state, ": installed", 11) != 0)
			continue;
		for (const char *at = line; at + n + 3 <= state; at++)
			if (strncmp(at, ", ", 2) == 0 && strncmp(at + 2, release, n) == 0 && at[2 + n] == ',')
				return 1;
	}
	return 0;
}

int gs_dkms_ensure(int (*run)(const char *const argv[]), int (*capture)(const char *const argv[], char *out, size_t len),
		   int managed, const char *release, const char *named)
{
	const char *const status_argv[] = { "dkms", "status", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "-k", release, NULL };
	const char *const inst_argv[] = { "dkms", "install", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "-k", release, NULL };
	char status[2048] = "";

	if (!managed) {
		fprintf(stderr, "geforce-sriov: warning: DKMS does not hold %s on this host, so rebuild cannot make sure that the livepatch\n"
			"module %s is installed for %s. A kernel without it boots with no VFs. Install it for that kernel\n"
			"yourself, or run `sudo ./geforce-sriov install` in the source folder while that kernel runs.\n",
			DKMS_ID, GS_MODULE_NAME, release);
		return GS_EXIT_OK;
	}
	if (capture(status_argv, status, sizeof(status)) == 0 && dkms_installed_for(status, release)) {
		printf("The livepatch module %s is installed for %s.\n", GS_MODULE_NAME, release);
		return GS_EXIT_OK;
	}
	printf("The livepatch module %s is not installed for %s: dkms install -m %s -v %s -k %s\n", GS_MODULE_NAME, release,
	       GS_DKMS_NAME, GS_VERSION, release);
	fflush(stdout);
	if (run(inst_argv) != 0) {
		fprintf(stderr, "geforce-sriov: `dkms install -m %s -v %s -k %s` failed (DKMS's output is above), so the livepatch module\n"
			"%s is not installed for %s, and that kernel would boot with no VFs. NVIDIA's module and plugin for it\n"
			"are as they were. The usual cause is that the headers of that kernel are not installed. Fix the cause,\n"
			"then run `sudo geforce-sriov rebuild%s%s` again.\n",
			GS_DKMS_NAME, GS_VERSION, release, GS_MODULE_NAME, release, named ? " --kernel " : "", named ? named : "");
		return GS_EXIT_FAIL;
	}
	printf("The livepatch module %s is now installed for %s.\n", GS_MODULE_NAME, release);
	return GS_EXIT_OK;
}

void gs_dkms_other_kernels(int (*run)(const char *const argv[]), const char *root, const char *state_dir)
{
	char list[2048], *k, *save = NULL;

	if (!gs_kernels_without_livepatch(root, state_dir, list, sizeof(list)))
		return;
	for (k = strtok_r(list, ", ", &save); k; k = strtok_r(NULL, ", ", &save)) {
		const char *const inst_argv[] = { "dkms", "install", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "-k", k, NULL };

		printf("NVIDIA's module is also built for %s, so the livepatch module %s is installed for it too: dkms install -m %s -v %s -k %s\n",
		       k, GS_MODULE_NAME, GS_DKMS_NAME, GS_VERSION, k);
		fflush(stdout);
		if (run(inst_argv) != 0)
			fprintf(stderr, "geforce-sriov: warning: `dkms install -m %s -v %s -k %s` failed (DKMS's output is above), so the livepatch\n"
				"module %s is not installed for %s, and that kernel would boot with no VFs. Before you boot it, run\n"
				"`sudo geforce-sriov rebuild --kernel %s`.\n", GS_DKMS_NAME, GS_VERSION, k, GS_MODULE_NAME, k, k);
	}
}
