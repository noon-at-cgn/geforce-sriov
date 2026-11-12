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

#define DKMS_OTHER_MAX 8
#define DKMS_VER_LEN 64

/* Does a line of status before line name the same GS_DKMS_NAME version as v (n bytes, ended by ',' or ':')? */
static int dkms_seen_before(const char *status, const char *line, const char *v, size_t n)
{
	const size_t nl = strlen(GS_DKMS_NAME);

	for (const char *l = status; l && l < line; l = strchr(l, '\n')) {
		const char *w;

		l += *l == '\n';
		if (l >= line || strncmp(l, GS_DKMS_NAME, nl) != 0)
			continue;
		w = l + nl;
		if (*w == '/')
			w++;
		else if (w[0] == ',' && w[1] == ' ')
			w += 2;
		else
			continue;
		if (strncmp(w, v, n) == 0 && strcspn(w, ",:\n") == n)
			return 1;
	}
	return 0;
}

/*
 * The versions of GS_DKMS_NAME other than GS_VERSION in status, the output of `dkms status -m`: lines
 * "geforce-sriov/0.2.0, <kernel>, <arch>: installed", "geforce-sriov/0.2.0: added" (DKMS 3) or
 * "geforce-sriov, 0.2.0, <kernel>, <arch>: installed" (DKMS 2), each version once, at most max. A version
 * past max or as long as DKMS_VER_LEN is not taken: one warning names it with the command that removes it
 * by hand. A name with a character outside ok is not echoed: one warning says to look in `dkms status`.
 */
static unsigned int dkms_other_versions(const char *status, char ver[][DKMS_VER_LEN], unsigned int max)
{
	static const char ok[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._+~-";
	const size_t nl = strlen(GS_DKMS_NAME);
	unsigned int n = 0;
	int bad_named = 0;

	for (const char *line = status; line; line = strchr(line, '\n')) {
		const char *v;
		size_t len;
		unsigned int i = 0;

		line += *line == '\n';
		if (strncmp(line, GS_DKMS_NAME, nl) != 0)
			continue;
		v = line + nl;
		if (*v == '/')
			v++;
		else if (v[0] == ',' && v[1] == ' ')
			v += 2;
		else
			continue;
		len = strspn(v, ok);
		if (len == strlen(GS_VERSION) && strncmp(v, GS_VERSION, len) == 0 && (v[len] == ',' || v[len] == ':'))
			continue;
		while (i < n && !(strlen(ver[i]) == len && strncmp(ver[i], v, len) == 0))
			i++;
		if (i < n && (v[len] == ',' || v[len] == ':'))
			continue;
		if (len == 0 || len >= DKMS_VER_LEN || (v[len] != ',' && v[len] != ':') || n == max) {
			/* One line per version (DKMS lists it once per kernel); a name install does not accept is not echoed. */
			if (len == 0 || (v[len] != ',' && v[len] != ':')) {
				if (!bad_named++)
					fprintf(stderr, "geforce-sriov: warning: DKMS also holds a %s version whose name install does "
						"not accept, so install does not remove it; find it with `dkms status -m %s` and remove "
						"it with `dkms remove %s/<version> --all`\n", GS_DKMS_NAME, GS_DKMS_NAME, GS_DKMS_NAME);
			} else if (!dkms_seen_before(status, line, v, len)) {
				fprintf(stderr, "geforce-sriov: warning: DKMS also holds %s version %.*s, which install does not "
					"remove; remove it yourself: dkms remove %s/%.*s --all\n", GS_DKMS_NAME, (int)len, v,
					GS_DKMS_NAME, (int)len, v);
			}
			continue;
		}
		memcpy(ver[n], v, len);
		ver[n++][len] = '\0';
	}
	return n;
}

#define OLD_TREE_DEPTH 8

/*
 * 0 when everything below the directory path (manifest path; root + path on disk) is a directory or a
 * file m records that install still owns (gs_manifest_owns()); otherwise -1 with in why the first other entry.
 */
static int old_tree_owned(struct manifest *m, const char *root, const char *path, unsigned int depth, char *why,
			  size_t len)
{
	char full[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;
	int r = 0;

	if (gs_pathf(full, sizeof(full), "%s%s", root, path) || !(d = opendir(full)))
		return snprintf(why, len, "%s%s cannot be read", root, path), -1;
	while (!r && (de = readdir(d))) {
		char sub[GS_PATH_MAX], subfull[GS_PATH_MAX];
		const struct entry *e;
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (gs_pathf(sub, sizeof(sub), "%s/%s", path, de->d_name) ||
		    gs_pathf(subfull, sizeof(subfull), "%s%s", root, sub) || lstat(subfull, &st)) {
			snprintf(why, len, "an entry of %s cannot be read", full);
			r = -1;
		} else if (S_ISDIR(st.st_mode)) {
			if (depth < OLD_TREE_DEPTH) {
				r = old_tree_owned(m, root, sub, depth + 1, why, len);
			} else {
				snprintf(why, len, "%s is nested too deep", subfull);
				r = -1;
			}
		} else if (!(e = gs_manifest_find(m, 'f', sub))) {
			snprintf(why, len, "%s was not installed by geforce-sriov", subfull);
			r = -1;
		} else if (gs_manifest_owns(subfull, e->sha) != OWN_YES) {
			snprintf(why, len, "%s changed since install", subfull);
			r = -1;
		}
	}
	closedir(d);
	return r;
}

/* Remove the directory full and everything below it, which old_tree_owned() accepted. 0 or -errno. */
static int old_tree_remove(const char *full, unsigned int depth)
{
	struct dirent *de;
	DIR *d = opendir(full);
	int r = 0;

	if (!d)
		return -errno;
	while (!r && (de = readdir(d))) {
		char sub[GS_PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (gs_pathf(sub, sizeof(sub), "%s/%s", full, de->d_name))
			r = -ENAMETOOLONG;
		else if (lstat(sub, &st))
			r = -errno;
		else if (S_ISDIR(st.st_mode))
			r = depth >= OLD_TREE_DEPTH ? -ELOOP : old_tree_remove(sub, depth + 1);
		else if (unlink(sub))
			r = -errno;
	}
	closedir(d);
	if (!r && rmdir(full))
		r = -errno;
	return r;
}

/*
 * After the DKMS step installed this version: the source tree /usr/src/GS_DKMS_NAME-<v> (below root) of
 * each version v it removed from DKMS goes too, when it is a real directory (not a link) that holds only
 * directories and files the install manifest at manifest records and install still owns, as uninstall
 * decides (gs_manifest_owns()). Its manifest entries that are gone are dropped. One line per removed tree;
 * any tree that stays is a warning, never a failure.
 */
static void dkms_old_trees(const char *root, const char *manifest, char ver[][DKMS_VER_LEN], unsigned int n)
{
	struct manifest *m;
	unsigned int kept = 0;
	int r, dropped = 0;

	if (!n)
		return;
	m = calloc(1, sizeof(*m));
	if (!m || gs_manifest_load(manifest, m)) {
		fprintf(stderr, "geforce-sriov: warning: %s cannot be read, so the source trees of the removed versions stay in "
			"/usr/src\n", manifest);
		free(m);
		return;
	}
	for (unsigned int i = 0; i < n; i++) {
		char path[GS_PATH_MAX], full[GS_PATH_MAX], why[GS_PATH_MAX + 64];
		struct stat st;
		size_t len;

		if (gs_pathf(path, sizeof(path), "/usr/src/%s-%s", GS_DKMS_NAME, ver[i]) ||
		    gs_pathf(full, sizeof(full), "%s%s", root, path))
			continue;
		len = strlen(path);
		if (lstat(full, &st) != 0) {
			/* Already gone: only its manifest entries go. */
		} else if (!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "geforce-sriov: warning: kept %s: it is %s, not the directory install wrote\n", full,
				S_ISLNK(st.st_mode) ? "a symbolic link" : "not a directory");
		} else if (old_tree_owned(m, root, path, 0, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: warning: kept %s: %s\n", full, why);
		} else if ((r = old_tree_remove(full, 0))) {
			fprintf(stderr, "geforce-sriov: warning: removing %s failed: %s; remove it yourself\n", full,
				strerror(-r));
		} else {
			printf("removed %s, the source of %s/%s, which DKMS no longer holds\n", full, GS_DKMS_NAME, ver[i]);
		}
		/* Its entries that are gone leave the manifest, so uninstall does not look for them. */
		for (unsigned int j = 0; j < m->n; j++) {
			char p[GS_PATH_MAX];
			int below = strncmp(m->e[j].path, path, len) == 0 && (m->e[j].path[len] == '/' || !m->e[j].path[len]);

			if (below && !gs_pathf(p, sizeof(p), "%s%s", root, m->e[j].path) && !gs_exists(p))
				m->e[j].type = 0, dropped = 1;
		}
	}
	if (dropped) {
		for (unsigned int j = 0; j < m->n; j++)
			if (m->e[j].type)
				m->e[kept++] = m->e[j];
		m->n = kept;
		if ((r = gs_manifest_save(manifest, m)))
			fprintf(stderr, "geforce-sriov: warning: writing %s failed: %s\n", manifest, strerror(-r));
	}
	free(m);
}

int gs_dkms_step(int (*run)(const char *const argv[]), int registered, const char *status, const char *scratch,
		 const char *root, const char *manifest)
{
	const char *const add_argv[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const build_argv[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const inst_argv[] = { "dkms", "install", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
	const char *const disable_argv[] = { "systemctl", "disable", GS_UNIT_LIVEPATCH, GS_UNIT_ACTIVATE, NULL };
	char other[DKMS_OTHER_MAX][DKMS_VER_LEN];
	/* What install removes from DKMS before it adds this version: the other versions, then this one. */
	char ids[DKMS_OTHER_MAX + 1][DKMS_VER_LEN + sizeof(GS_DKMS_NAME) + 1];
	unsigned int nother = dkms_other_versions(status, other, DKMS_OTHER_MAX), nids = 0;
	char removed[1024] = "";
	int disabled;

	for (unsigned int i = 0; i < nother; i++)
		gs_pathf(ids[nids++], sizeof(ids[0]), "%s/%s", GS_DKMS_NAME, other[i]);
	if (registered)
		gs_pathf(ids[nids++], sizeof(ids[0]), "%s", DKMS_ID);
	if (nids) {
		char tree[GS_PATH_MAX];
		const char *const try_add[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "--dkmstree", tree, NULL };
		const char *const try_build[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, "--dkmstree", tree, NULL };
		const char *const clean_argv[] = { "rm", "-rf", "--", tree, NULL };
		int built;

		if (gs_pathf(tree, sizeof(tree), "%s/geforce-sriov-dkms.XXXXXX", scratch) || !mkdtemp(tree)) {
			fprintf(stderr, "geforce-sriov: cannot create a scratch DKMS tree in %s; the installed %s module is unchanged\n",
				scratch, ids[0]);
			return GS_EXIT_FAIL;
		}
		if (!nother)
			printf("\n== %s is registered: a trial build in %s first; the installed livepatch module stays until it succeeds\n",
			       DKMS_ID, tree);
		else
			printf("\n== %s%s registered: a trial build of %s in %s first; the installed livepatch module stays until it\n"
			       "succeeds, then install replaces it\n",
			       ids[0], nids > 1 ? " and others are" : " is", DKMS_ID, tree);
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
		for (unsigned int i = 0; i < nids; i++) {
			const char *const rm_argv[] = { "dkms", "remove", ids[i], "--all", NULL };
			size_t at = strlen(removed);

			if (run(rm_argv) != 0) {
				fprintf(stderr, "geforce-sriov: dkms remove %s --all failed (DKMS's output is above), so the livepatch module installed\n"
					"earlier may be gone. Fix the cause, then run `geforce-sriov install` again.\n", ids[i]);
				return GS_EXIT_FAIL;
			}
			gs_pathf(removed + at, sizeof(removed) - at, "%s`dkms remove %s --all`", i ? " and " : "", ids[i]);
		}
	}
	if (run(add_argv) == 0 && run(build_argv) == 0 && run(inst_argv) == 0) {
		dkms_old_trees(root, manifest, other, nother);
		return GS_EXIT_OK;
	}
	if (!nids) {
		fprintf(stderr, "geforce-sriov: DKMS could not add/build/install %s for the running kernel; files are installed, `geforce-sriov uninstall` removes them\n",
			DKMS_ID);
		return GS_EXIT_FAIL;
	}
	/* The module installed earlier is gone: the next boot must not try to load it. */
	disabled = run(disable_argv) == 0;
	fprintf(stderr, "geforce-sriov: DKMS could not add, build or install %s for the running kernel although the trial\n"
		"build succeeded, and %s had already removed the livepatch module installed earlier.\n"
		"%s\nFix the cause in DKMS's output above, then run `geforce-sriov install` again%s.\n",
		DKMS_ID, removed,
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
