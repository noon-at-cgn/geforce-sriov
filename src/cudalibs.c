// SPDX-License-Identifier: MIT
/*
 * NVIDIA's CUDA libraries for host CUDA.
 *
 * A CUDA program on the host needs NVIDIA's CUDA library (libcuda). NVIDIA's vGPU host package has
 * none; the second package, which nvidia-uvm is built from, has it. The driver step copies the
 * libraries from that package into a staging directory with an index (stage_cuda in
 * scripts/geforce-sriov-driver.sh), and install puts them in place from there:
 *
 *   <prefix>/lib/geforce-sriov/cuda/<library>.so.<version>   each library, as NVIDIA names it
 *   <prefix>/lib/geforce-sriov/cuda/<soname>                 the link the loader follows, as NVIDIA's
 *                                                            installer makes it, and libcuda.so
 *   /etc/ld.so.conf.d/geforce-sriov-cuda.conf                names that directory to the loader
 *
 * A directory of their own, not the system's library directory: a libcuda that another package or
 * the user put there is then never replaced, uninstall has nothing to put back, and what this
 * package installed is exactly what its manifest lists. The loader searches the directories of
 * ld.so.conf.d in the order of the file names, so this one comes before the distribution's; whether
 * the loader really finds this libcuda first is asked of `ldconfig -p`, and said when it does not.
 *
 * Nothing here fails an install: a library that cannot be installed is one warning, and host CUDA
 * is then as it was before these libraries were installed at all.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gs_internal.h"

#define CUDA_MAX 8

struct cuda_lib {
	char file[96], sha[65], soname[96];
};

struct cuda_set {
	char version[32];
	struct cuda_lib lib[CUDA_MAX];
	unsigned int n;
};

/* A library's file name as it may appear in an index: no directory, no blank, nothing a path could hide in. */
static int lib_name_ok(const char *s)
{
	size_t n = strlen(s);

	if (n < 7 || n >= 64 || strncmp(s, "lib", 3) != 0 || !strstr(s, ".so"))
		return 0;
	return strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-") == n;
}

/* The index the driver step wrote into stage: 0, -ENOENT without one, -EINVAL for one that is not usable. */
static int stage_read(const char *stage, struct cuda_set *set)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t len;
	int r;

	memset(set, 0, sizeof(*set));
	if (gs_pathf(p, sizeof(p), "%s/index", stage))
		return -ENAMETOOLONG;
	if ((r = gs_read_whole(p, &data, &len, 1 << 16)))
		return r == -ENOENT ? -ENOENT : -EINVAL;
	r = 0;
	for (line = strtok_r(data, "\n", &save); line && !r; line = strtok_r(NULL, "\n", &save)) {
		struct cuda_lib *l = &set->lib[set->n];
		char have[65];

		if (strncmp(line, "version=", 8) == 0) {
			gs_copy(set->version, sizeof(set->version), line + 8);
			continue;
		}
		if (strncmp(line, "lib=", 4) != 0)
			continue;
		if (set->n == CUDA_MAX || sscanf(line + 4, "%95s %64s %95s", l->file, l->sha, l->soname) != 3 ||
		    !lib_name_ok(l->file) || !lib_name_ok(l->soname) || strlen(l->sha) != 64 ||
		    gs_pathf(p, sizeof(p), "%s/%s", stage, l->file) || gs_sha256_file(p, have) || strcmp(have, l->sha) != 0)
			r = -EINVAL;
		else
			set->n++;
	}
	free(data);
	if (!r && (!set->n || strncmp(set->lib[0].file, "libcuda.so.", 11) != 0 ||
		   strspn(set->version, "0123456789.") != strlen(set->version) || !set->version[0]))
		r = -EINVAL;
	return r;
}

static int under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return strncmp(path, dir, n) == 0 && path[n] == '/' && !strchr(path + n + 1, '/');
}

/*
 * What the manifest m lists of the libraries: the directory and the version of libcuda in dir and
 * version, and whether every listed file and link is still as it was installed.
 * 1 complete, 0 nothing is listed, -1 something is missing or was changed (its path in bad).
 */
static int cuda_state(struct manifest *m, const char *destdir, char *dir, size_t dlen, char *version, size_t vlen,
		      char *bad, size_t blen)
{
	char full[GS_PATH_MAX], to[96];
	int complete = 1;

	dir[0] = version[0] = bad[0] = '\0';
	for (unsigned int i = 0; i < m->n && !dir[0]; i++) {
		const char *base = strrchr(m->e[i].path, '/');

		if (m->e[i].type == 'f' && base && strncmp(base + 1, "libcuda.so.", 11) == 0 && strstr(m->e[i].path, "/cuda/")) {
			gs_pathf(dir, dlen, "%.*s", (int)(base - m->e[i].path), m->e[i].path);
			gs_copy(version, vlen, base + 12);
		}
	}
	if (!dir[0])
		return 0;
	for (unsigned int i = 0; i < m->n; i++) {
		ssize_t n;
		int ok;

		if ((m->e[i].type != 'f' && m->e[i].type != 'l') || !under(m->e[i].path, dir) ||
		    gs_pathf(full, sizeof(full), "%s%s", destdir, m->e[i].path))
			continue;
		if (m->e[i].type == 'f') {
			ok = gs_manifest_owns(full, m->e[i].sha) == OWN_YES;
		} else {
			n = readlink(full, to, sizeof(to) - 1);
			ok = n > 0 && (to[n] = '\0', strcmp(to, m->e[i].sha) == 0);
		}
		if (!ok && complete) {
			complete = 0;
			gs_copy(bad, blen, full);
		}
	}
	return complete ? 1 : -1;
}

int gs_cuda_libs_state(const char *manifest, char *dir, size_t dlen, char *version, size_t vlen, char *bad, size_t blen)
{
	struct manifest *m = calloc(1, sizeof(*m));
	int r;

	dir[0] = version[0] = bad[0] = '\0';
	if (!m)
		return 0;
	r = gs_manifest_load(manifest, m) ? 0 : cuda_state(m, "", dir, dlen, version, vlen, bad, blen);
	free(m);
	return r;
}

int gs_cuda_loader(int (*capture)(const char *const argv[], char *out, size_t len), const char *dir, char *found, size_t len)
{
	const char *const argv[] = { "ldconfig", "-p", NULL };
	char *out = malloc(1 << 20), *line, *save = NULL;
	int r = -1;

	found[0] = '\0';
	if (!out)
		return -1;
	out[0] = '\0';
	if (capture(argv, out, 1 << 20) == 0) {
		for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			const char *name = line + strspn(line, " \t"), *to = strstr(line, "=> ");

			/* The first entry of this machine's own kind is the one the loader takes. */
			if (strncmp(name, "libcuda.so.1 (", 14) != 0 || !to || !strstr(name, "x86-64"))
				continue;
			gs_copy(found, len, to + 3);
			r = under(found, dir);
			break;
		}
	}
	free(out);
	return r;
}

/* Takes one entry of this package out of place while it is unchanged. 1 when the manifest no longer lists it, 0 when it is kept. */
static int cuda_drop(struct ctx *x, struct entry *e, const char *why)
{
	char full[GS_PATH_MAX], to[96];
	ssize_t n;
	int mine;

	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, e->path))
		return 0;
	if (!gs_exists(full)) {
		gs_manifest_drop(&x->m, e);
		return 1;
	}
	if (e->type == 'f') {
		mine = gs_manifest_owns(full, e->sha) == OWN_YES;
	} else {
		n = readlink(full, to, sizeof(to) - 1);
		mine = n > 0 && (to[n] = '\0', strcmp(to, e->sha) == 0);
	}
	if (!mine) {
		printf("kept %s (changed since install)\n", full);
		return 0;
	}
	if (unlink(full)) {
		fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", full, strerror(errno));
		return 0;
	}
	printf("removed %s: %s\n", full, why);
	gs_manifest_drop(&x->m, e);
	x->written = 1;
	return 1;
}

/* Every entry in dir that keep does not name goes; the directory after them, once it is empty. */
static void cuda_sweep(struct ctx *x, const char *dir, const struct cuda_set *keep, const char *why)
{
	char full[GS_PATH_MAX];
	struct entry *e;

	for (unsigned int i = 0; i < x->m.n;) {
		const char *base;
		int wanted = 0;

		e = &x->m.e[i];
		if ((e->type != 'f' && e->type != 'l') || !under(e->path, dir)) {
			i++;
			continue;
		}
		base = strrchr(e->path, '/') + 1;
		for (unsigned int k = 0; keep && k < keep->n; k++)
			wanted |= strcmp(base, keep->lib[k].file) == 0 || strcmp(base, keep->lib[k].soname) == 0;
		wanted |= keep && e->type == 'l' && strcmp(base, "libcuda.so") == 0;
		/* An entry that was dropped is replaced by the one after it. */
		if (wanted || !cuda_drop(x, e, why))
			i++;
	}
	if (keep || gs_pathf(full, sizeof(full), "%s%s", x->destdir, dir))
		return;
	if ((e = gs_manifest_find(&x->m, 'd', dir)) && rmdir(full) == 0) {
		printf("removed %s/\n", full);
		gs_manifest_drop(&x->m, e);
	}
}

static void run_ldconfig(const struct gs_cuda_io *io)
{
	const char *const argv[] = { "ldconfig", NULL };

	if (io && io->run && io->run(argv) != 0)
		fprintf(stderr, "geforce-sriov: warning: ldconfig failed. Run `ldconfig` as root, so that programs find NVIDIA's CUDA\n"
			"libraries.\n");
}

int gs_cuda_libs_sync(struct ctx *x, const char *dir, const char *conf, const char *stage, int want, const struct gs_cuda_io *io)
{
	char src[GS_PATH_MAX], dst[GS_PATH_MAX], text[GS_PATH_MAX + 2], have_dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX];
	const char *const rm_stage[] = { "rm", "-rf", "--", stage, NULL };
	struct cuda_set set;
	struct entry *e;
	int before = x->written, r, state, staged;

	x->written = 0;
	if (!want) {
		cuda_sweep(x, dir, NULL, "host CUDA is off");
		/* A directory of an earlier prefix: its entries are this package's too. */
		if (cuda_state(&x->m, x->destdir, have_dir, sizeof(have_dir), version, sizeof(version), bad, sizeof(bad)))
			cuda_sweep(x, have_dir, NULL, "host CUDA is off");
		if ((e = gs_manifest_find(&x->m, 'f', conf)))
			(void)cuda_drop(x, e, "host CUDA is off");
		if (gs_exists(stage) && io && io->run)
			io->run(rm_stage);
		if (x->written)
			run_ldconfig(io);
		goto out;
	}
	staged = stage_read(stage, &set);
	if (staged == -EINVAL)
		fprintf(stderr, "geforce-sriov: warning: %s does not hold what the driver step recorded there; NVIDIA's CUDA libraries\n"
			"are not installed from it.\n", stage);
	if (staged == 0) {
		r = 0;
		for (unsigned int i = 0; i < set.n && !r; i++) {
			const struct cuda_lib *l = &set.lib[i];

			if (gs_pathf(src, sizeof(src), "%s/%s", stage, l->file) || gs_pathf(dst, sizeof(dst), "%s/%s", dir, l->file)) {
				r = -ENAMETOOLONG;
				break;
			}
			/* Executable, as NVIDIA's installer leaves its libraries. */
			if ((r = gs_install_copy(x, src, dst, 0755)))
				break;
			if (strcmp(l->soname, l->file) != 0 && !gs_pathf(dst, sizeof(dst), "%s/%s", dir, l->soname))
				r = gs_install_link(x, dst, l->file);
			/* The name without a version, which NVIDIA's installer makes for libcuda and some programs open. */
			if (!r && i == 0 && !gs_pathf(dst, sizeof(dst), "%s/libcuda.so", dir))
				r = gs_install_link(x, dst, strcmp(l->soname, l->file) ? l->soname : l->file);
		}
		if (!r && snprintf(text, sizeof(text), "%s\n", dir) > 0)
			r = gs_install_put(x, conf, text, strlen(text), 0644);
		if (r) {
			fprintf(stderr, "geforce-sriov: warning: NVIDIA's CUDA libraries could not be installed in %s%s: %s. CUDA programs on the\n"
				"host may not start. Fix the cause, then run the same install command again.\n", x->destdir, dir, strerror(-r));
		} else {
			/* The libraries of another version, from an earlier second package. */
			cuda_sweep(x, dir, &set, "it belongs to another version of NVIDIA's CUDA libraries");
			if (io && io->run)
				io->run(rm_stage);
			if (x->written) {
				run_ldconfig(io);
				printf("host CUDA: NVIDIA's CUDA libraries %s are installed in %s%s\n", set.version, x->destdir, dir);
			}
		}
	}
	state = cuda_state(&x->m, x->destdir, have_dir, sizeof(have_dir), version, sizeof(version), bad, sizeof(bad));
	if (x->destdir[0]) {
		if (state != 1)
			printf("host CUDA: NVIDIA's CUDA libraries are not part of a staged install. On the target host,\n"
			       "`geforce-sriov install --host-cuda --companion-file PATH` installs them.\n");
	} else if (state == 0) {
		fprintf(stderr, "geforce-sriov: warning: host CUDA is on, but NVIDIA's CUDA library (libcuda) is not installed, so CUDA\n"
			"programs on the host cannot start. It comes from NVIDIA's second driver package, and this run did not get it\n"
			"from there (%s has the reason when that package was read).\n"
			"Run `sudo ./geforce-sriov install --host-cuda --companion-file PATH` in the source folder with that package.\n",
			GS_DRIVER_LOG);
	} else if (state < 0) {
		fprintf(stderr, "geforce-sriov: warning: NVIDIA's CUDA libraries are incomplete: %s is missing or was changed since\n"
			"install. Run `sudo ./geforce-sriov install --host-cuda --companion-file PATH` in the source folder to install\n"
			"them again.\n", bad);
	} else if (io && io->capture && gs_cuda_loader(io->capture, have_dir, bad, sizeof(bad)) == 0) {
		fprintf(stderr, "geforce-sriov: warning: programs on this host load another CUDA library first: %s. geforce-sriov did\n"
			"not install it and leaves it as it is. It works only if it has the version of the loaded NVIDIA driver;\n"
			"otherwise remove it or move it away, then run `ldconfig`. The libraries of this install are in %s.\n",
			bad, have_dir);
	}
out:
	r = x->written;
	x->written |= before;
	return r;
}

int gs_cuda_libs_apply(const char *manifest, const char *dir, const char *conf, const char *stage, int want,
		       const struct gs_cuda_io *io)
{
	struct ctx *x = calloc(1, sizeof(*x));
	int r;

	if (!x)
		return -ENOMEM;
	x->destdir = "";
	if ((r = gs_manifest_load(manifest, &x->m)) == 0) {
		gs_cuda_libs_sync(x, dir, conf, stage, want, io);
		r = gs_manifest_save(manifest, &x->m);
	}
	free(x);
	return r;
}

int gs_cuda_libs_line(const char *manifest, int want, int (*capture)(const char *const argv[], char *out, size_t len),
		      char *line, size_t len)
{
	char dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX];
	int state = gs_cuda_libs_state(manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad));

	line[0] = '\0';
	if (!want) {
		if (!state)
			return 0;
		snprintf(line, len, "libcuda %s is installed in %s although host_cuda=off; the next `geforce-sriov install` removes it",
			 version, dir);
		return 2;
	}
	if (!state) {
		snprintf(line, len, "not installed, so CUDA programs on the host cannot start; run `sudo ./geforce-sriov install "
			 "--host-cuda --companion-file PATH` in the source folder with NVIDIA's second driver package");
		return -1;
	}
	if (state < 0) {
		snprintf(line, len, "incomplete: %s is missing or was changed since install; run `sudo ./geforce-sriov install "
			 "--host-cuda --companion-file PATH` in the source folder to install them again", bad);
		return -1;
	}
	if (capture && gs_cuda_loader(capture, dir, bad, sizeof(bad)) == 0) {
		snprintf(line, len, "libcuda %s is installed in %s, but programs load another one first: %s, which geforce-sriov did "
			 "not install. Remove it or move it away, then run `ldconfig`", version, dir, bad);
		return -1;
	}
	snprintf(line, len, "libcuda %s is installed in %s", version, dir);
	return 1;
}
