// SPDX-License-Identifier: MIT
/*
 * NVIDIA's libraries from the Linux driver package: for host CUDA and for the host desktop.
 *
 * NVIDIA's vGPU host package has no CUDA or graphics libraries; the Linux driver package, which nvidia-uvm
 * and the display modules are built from, has them. The driver step copies the libraries from that
 * package into a staging directory with an index (stage_cuda and stage_desktop in
 * scripts/geforce-sriov-driver.sh), and install puts them in place from there. There are two sets,
 * one mechanism; a set is a constant structure (struct gs_nvset) and the paths below.
 *
 * Host CUDA:
 *   <prefix>/lib/geforce-sriov/cuda/<library>.so.<version>   each library, as NVIDIA names it
 *   <prefix>/lib/geforce-sriov/cuda/<soname>                 the link the loader follows, as NVIDIA's
 *                                                            installer makes it, and libcuda.so
 *   /etc/ld.so.conf.d/geforce-sriov-cuda.conf                names that directory to the loader
 *
 * Host desktop (the graphics libraries a consumer driver installs, without NVIDIA's copy of libglvnd):
 *   <prefix>/lib/geforce-sriov/gl/...                        the libraries, their soname links
 *   <prefix>/lib/geforce-sriov/xorg/{drivers,extensions}/    the X driver and its GLX module
 *   /etc/ld.so.conf.d/geforce-sriov-gl.conf                  names the library directory to the loader
 *   /etc/glvnd/egl_vendor.d, /etc/egl/egl_external_platform.d, /etc/vulkan/icd.d, /usr/share/nvidia
 *                                                            the small files that say where the libraries are
 *   <libgbm's directory>/gbm/nvidia-drm_gbm.so               the GBM backend link, when libgbm has that directory
 *   /etc/X11/xorg.conf.d/geforce-sriov-nvidia-drm-outputclass.conf
 *                                                            NVIDIA's output-class file with the module path
 *
 * The order is the one that is safe for the X server: X was measured to die when it draws through NVIDIA's
 * EGL and GBM without being on NVIDIA's X driver. So the X modules and the output-class file go in before
 * the EGL vendor file, the platform files and the GBM link, and come out after them: at every point
 * either both halves of X's side are there, or NVIDIA's EGL is not reachable.
 *
 * Directories of their own, not the system's library directory: a library that another package or the
 * user put there is then never replaced, uninstall has nothing to put back, and what this package
 * installed is exactly what its manifest lists. The loader searches the directories of ld.so.conf.d in
 * the order of the file names, so these come before the distribution's; whether the loader really
 * finds these libraries first is asked of `ldconfig -p`, and said when it does not. The one entry
 * inside a directory that a Debian package owns is the GBM backend link, which libgbm looks for only
 * there; no file of a Debian package is replaced.
 *
 * Nothing here fails an install: a set that cannot be installed is one warning, and the host is then as
 * it was before.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gs_internal.h"

#define LIB_MAX 32
#define XMOD_MAX 4
#define ETC_MAX 16
#define PLAN_MAX 96

#define XCONF_NAME GS_XCONF_NAME
#define GBM_NAME "nvidia-drm_gbm.so"

struct gs_nvset {
	const char *anchor;      /* the first library of the set: its file name starts with this */
	const char *dirmark;     /* the directory name the anchor sits in */
	const char *loader[2];   /* what `ldconfig -p` lists for the set; the first 64-bit entry is the one programs get */
	int desktop;             /* the set has X modules, small files in /etc and the GBM link */
	const char *conf;        /* the loader's file: part of a complete desktop set; NULL: not checked */
};

const struct gs_nvset gs_nvset_cuda = { "libcuda.so.", "/cuda/", { "libcuda.so.1 (", NULL }, 0, NULL };
const struct gs_nvset gs_nvset_desktop = { "libEGL_nvidia.so.", "/gl/", { "libEGL_nvidia.so.0 (", "libGLX_nvidia.so.0 (" }, 1, GS_GL_CONF };

struct gs_nvgl_paths gs_nvgl = {
	.etc_vendor = "/etc/glvnd/egl_vendor.d",
	.etc_platform = "/etc/egl/egl_external_platform.d",
	.etc_vulkan = "/etc/vulkan/icd.d",
	.share_nvidia = "/usr/share/nvidia",
	.xconf = "/etc/X11/xorg.conf.d",
	.share_vendor = "/usr/share/glvnd/egl_vendor.d",
	.share_platform = "/usr/share/egl/egl_external_platform.d",
	.xorg_drivers = "/usr/lib/xorg/modules/drivers",
};

struct nv_lib {
	char file[96], sha[65], soname[96];
};

struct nv_xmod {
	char sub[16], file[96], sha[65], link[96];
};

struct nv_etc {
	char kind[16], file[96], sha[65];
};

struct nv_stage {
	char version[32];
	struct nv_lib lib[LIB_MAX];
	unsigned int n;
	struct nv_xmod xmod[XMOD_MAX];
	unsigned int nx;
	struct nv_etc etc[ETC_MAX];
	unsigned int ne;
	char gbm[96];
};

#define NAME_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"

/* A file name as it may appear in an index: no directory, no blank, nothing a path could hide in. */
static int name_ok(const char *s)
{
	size_t n = strlen(s);

	return n > 0 && n < 64 && s[0] != '.' && strspn(s, NAME_CHARS) == n;
}

static int lib_name_ok(const char *s)
{
	size_t n = strlen(s);

	if (n < 7 || n >= 64 || strncmp(s, "lib", 3) != 0 || !strstr(s, ".so"))
		return 0;
	return strspn(s, NAME_CHARS) == n;
}

static int suffix(const char *s, const char *end)
{
	size_t n = strlen(s), k = strlen(end);

	return n > k && strcmp(s + n - k, end) == 0;
}

/* The directory a kind of small file goes to, or NULL for a kind that does not exist. */
static const char *etc_dir(const char *kind)
{
	if (strcmp(kind, "egl_vendor") == 0)
		return gs_nvgl.etc_vendor;
	if (strcmp(kind, "egl_platform") == 0)
		return gs_nvgl.etc_platform;
	if (strcmp(kind, "vulkan_icd") == 0)
		return gs_nvgl.etc_vulkan;
	if (strcmp(kind, "profile") == 0)
		return gs_nvgl.share_nvidia;
	if (strcmp(kind, "xconf") == 0)
		return gs_nvgl.xconf;
	return NULL;
}

static int etc_name_ok(const char *kind, const char *file)
{
	if (!name_ok(file))
		return 0;
	if (strcmp(kind, "profile") == 0)
		return strncmp(file, "nvidia-application-profiles-", 28) == 0;
	if (strcmp(kind, "xconf") == 0)
		return strcmp(file, "nvidia-drm-outputclass.conf") == 0;
	return suffix(file, ".json");
}

static int under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return strncmp(path, dir, n) == 0 && path[n] == '/' && !strchr(path + n + 1, '/');
}

static const char *base_of(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

/* The X modules' directory of the desktop set: beside the library directory, which ends in /gl. */
static void xdir_of(const char *dir, char *out, size_t len)
{
	size_t n = strlen(dir);

	out[0] = '\0';
	if (n > 3 && strcmp(dir + n - 3, "/gl") == 0)
		snprintf(out, len, "%.*s/xorg", (int)(n - 3), dir);
}

/* The index the driver step wrote into stage: 0, -ENOENT without one, -EINVAL for one that is not usable. */
static int stage_read(const struct gs_nvset *set, const char *stage, struct nv_stage *st)
{
	char p[GS_PATH_MAX], have[65], *data, *line, *save = NULL, *slash;
	size_t len;
	int r;

	memset(st, 0, sizeof(*st));
	if (gs_pathf(p, sizeof(p), "%s/index", stage))
		return -ENAMETOOLONG;
	if ((r = gs_read_whole(p, &data, &len, 1 << 16)))
		return r == -ENOENT ? -ENOENT : -EINVAL;
	r = 0;
	for (line = strtok_r(data, "\n", &save); line && !r; line = strtok_r(NULL, "\n", &save)) {
		const char *file = NULL, *sha = NULL;

		if (strncmp(line, "version=", 8) == 0) {
			gs_copy(st->version, sizeof(st->version), line + 8);
			continue;
		}
		if (strncmp(line, "lib=", 4) == 0) {
			struct nv_lib *l = &st->lib[st->n];

			if (st->n == LIB_MAX || sscanf(line + 4, "%95s %64s %95s", l->file, l->sha, l->soname) != 3 ||
			    !lib_name_ok(l->file) || !lib_name_ok(l->soname)) {
				r = -EINVAL;
				break;
			}
			file = l->file, sha = l->sha;
			st->n++;
		} else if (set->desktop && strncmp(line, "xmod=", 5) == 0) {
			struct nv_xmod *m = &st->xmod[st->nx];
			char what[96];
			int got = sscanf(line + 5, "%95s %64s %95s", what, m->sha, m->link);

			if (st->nx == XMOD_MAX || (got != 2 && got != 3) || !(slash = strchr(what, '/')) || slash - what >= (long)sizeof(m->sub)) {
				r = -EINVAL;
				break;
			}
			*slash = '\0';
			gs_copy(m->sub, sizeof(m->sub), what);
			gs_copy(m->file, sizeof(m->file), slash + 1);
			if (got == 2)
				m->link[0] = '\0';
			if ((strcmp(m->sub, "drivers") != 0 && strcmp(m->sub, "extensions") != 0) || !name_ok(m->file) ||
			    !strstr(m->file, ".so") || (m->link[0] && !name_ok(m->link))) {
				r = -EINVAL;
				break;
			}
			file = m->file, sha = m->sha;
			st->nx++;
		} else if (set->desktop && strncmp(line, "etc=", 4) == 0) {
			struct nv_etc *e = &st->etc[st->ne];

			if (st->ne == ETC_MAX || sscanf(line + 4, "%15s %95s %64s", e->kind, e->file, e->sha) != 3 || !etc_dir(e->kind) ||
			    !etc_name_ok(e->kind, e->file)) {
				r = -EINVAL;
				break;
			}
			file = e->file, sha = e->sha;
			st->ne++;
		} else if (set->desktop && strncmp(line, "gbm=", 4) == 0) {
			if (!lib_name_ok(line + 4) || strlen(line + 4) >= sizeof(st->gbm)) {
				r = -EINVAL;
				break;
			}
			gs_copy(st->gbm, sizeof(st->gbm), line + 4);
			continue;
		} else {
			continue;
		}
		if (strlen(sha) != 64 || gs_pathf(p, sizeof(p), "%s/%s", stage, file) || gs_sha256_file(p, have) || strcmp(have, sha) != 0)
			r = -EINVAL;
	}
	free(data);
	if (!r && (!st->n || strncmp(st->lib[0].file, set->anchor, strlen(set->anchor)) != 0 ||
		   strspn(st->version, "0123456789.") != strlen(st->version) || !st->version[0]))
		r = -EINVAL;
	if (!r && st->gbm[0]) {
		r = -EINVAL;
		for (unsigned int i = 0; i < st->n; i++)
			if (strcmp(st->gbm, st->lib[i].soname) == 0)
				r = 0;
	}
	return r;
}

/* Is the entry one of the set's? The desktop set owns entries in a few directories no other install writes to. */
static int owned(const struct gs_nvset *set, const char *dir, const char *xdir, const struct entry *e)
{
	char sub[GS_PATH_MAX];

	if (e->type != 'f' && e->type != 'l')
		return 0;
	if (under(e->path, dir))
		return 1;
	if (!set->desktop || !xdir[0])
		return 0;
	if ((!gs_pathf(sub, sizeof(sub), "%s/drivers", xdir) && under(e->path, sub)) ||
	    (!gs_pathf(sub, sizeof(sub), "%s/extensions", xdir) && under(e->path, sub)))
		return 1;
	if (under(e->path, gs_nvgl.etc_vendor) || under(e->path, gs_nvgl.etc_platform) || under(e->path, gs_nvgl.etc_vulkan) ||
	    under(e->path, gs_nvgl.share_nvidia))
		return 1;
	if (e->type == 'f' && under(e->path, gs_nvgl.xconf) && strcmp(base_of(e->path), XCONF_NAME) == 0)
		return 1;
	return e->type == 'l' && strcmp(base_of(e->path), GBM_NAME) == 0;
}

static int entry_intact(const struct entry *e, const char *full)
{
	char to[GS_LINK_MAX];
	ssize_t n;

	if (e->type == 'f')
		return gs_manifest_owns(full, e->sha) == OWN_YES;
	n = readlink(full, to, sizeof(to) - 1);
	return n > 0 && (to[n] = '\0', strcmp(to, e->sha) == 0);
}

/*
 * What the manifest m lists of the set: the directory and the version of its anchor library in dir and
 * version, and whether every listed file and link is still as it was installed.
 * 1 complete, 0 nothing is listed, -1 something is missing or was changed (its path in bad).
 */
static int nv_state(struct manifest *m, const struct gs_nvset *set, const char *destdir, char *dir, size_t dlen,
		    char *version, size_t vlen, char *bad, size_t blen)
{
	char full[GS_PATH_MAX], xdir[GS_PATH_MAX];
	size_t alen = strlen(set->anchor);
	int complete = 1;

	dir[0] = version[0] = bad[0] = '\0';
	for (unsigned int i = 0; i < m->n && !dir[0]; i++) {
		const char *base = strrchr(m->e[i].path, '/');

		if (m->e[i].type == 'f' && base && strncmp(base + 1, set->anchor, alen) == 0 && strstr(m->e[i].path, set->dirmark)) {
			gs_pathf(dir, dlen, "%.*s", (int)(base - m->e[i].path), m->e[i].path);
			gs_copy(version, vlen, base + 1 + alen);
		}
	}
	if (!dir[0])
		return 0;
	xdir_of(dir, xdir, sizeof(xdir));
	for (unsigned int i = 0; i < m->n; i++) {
		if (!owned(set, dir, xdir, &m->e[i]) || gs_pathf(full, sizeof(full), "%s%s", destdir, m->e[i].path))
			continue;
		if (!entry_intact(&m->e[i], full) && complete) {
			complete = 0;
			gs_copy(bad, blen, full);
		}
	}
	/* The desktop set is complete only with the output-class file, an EGL vendor file and the loader's file: a set that stopped half-way is not one. */
	if (set->desktop && complete) {
		int have_x = 0, have_vendor = 0, have_conf = !set->conf;

		for (unsigned int i = 0; i < m->n; i++) {
			const struct entry *e = &m->e[i];

			if (e->type != 'f')
				continue;
			have_x |= under(e->path, gs_nvgl.xconf) && strcmp(base_of(e->path), XCONF_NAME) == 0;
			have_vendor |= under(e->path, gs_nvgl.etc_vendor);
			have_conf |= set->conf && strcmp(base_of(e->path), base_of(set->conf)) == 0;
		}
		if (!have_x)
			gs_pathf(bad, blen, "%s%s/%s", destdir, gs_nvgl.xconf, XCONF_NAME);
		else if (!have_vendor)
			gs_pathf(bad, blen, "%s%s/10_nvidia.json", destdir, gs_nvgl.etc_vendor);
		else if (!have_conf)
			gs_pathf(bad, blen, "%s%s", destdir, set->conf);
		complete = have_x && have_vendor && have_conf;
	}
	return complete ? 1 : -1;
}

int gs_nvlibs_state(const struct gs_nvset *set, const char *manifest, char *dir, size_t dlen, char *version, size_t vlen,
		    char *bad, size_t blen)
{
	struct manifest *m = calloc(1, sizeof(*m));
	int r;

	dir[0] = version[0] = bad[0] = '\0';
	if (!m)
		return 0;
	r = gs_manifest_load(manifest, m) ? 0 : nv_state(m, set, "", dir, dlen, version, vlen, bad, blen);
	free(m);
	return r;
}

/* The first 64-bit entry of `ldconfig -p` that starts with name, in out; its path in found. 1 when there is one. */
static int ldconfig_entry(const char *list, const char *name, char *found, size_t len)
{
	char *copy = strdup(list), *line, *save = NULL;
	int r = 0;

	if (!copy)
		return 0;
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		const char *at = line + strspn(line, " \t"), *to = strstr(line, "=> ");

		/* The first entry of this machine's own kind is the one the loader takes. */
		if (strncmp(at, name, strlen(name)) != 0 || !to || !strstr(at, "x86-64"))
			continue;
		gs_copy(found, len, to + 3);
		r = 1;
		break;
	}
	free(copy);
	return r;
}

static char *ldconfig_list(int (*capture)(const char *const argv[], char *out, size_t len))
{
	const char *const argv[] = { "ldconfig", "-p", NULL };
	char *out = malloc(1 << 20);

	if (!out)
		return NULL;
	out[0] = '\0';
	if (capture(argv, out, 1 << 20) != 0) {
		free(out);
		return NULL;
	}
	return out;
}

int gs_nvlibs_loader(const struct gs_nvset *set, int (*capture)(const char *const argv[], char *out, size_t len), const char *dir,
		     char *found, size_t len)
{
	char *out = ldconfig_list(capture), one[GS_PATH_MAX];
	int r = -1;

	found[0] = '\0';
	if (!out)
		return -1;
	for (unsigned int i = 0; i < 2 && set->loader[i]; i++) {
		if (!ldconfig_entry(out, set->loader[i], one, sizeof(one)))
			continue;
		if (!under(one, dir)) {
			gs_copy(found, len, one);
			r = 0;
			break;
		}
		r = 1;
	}
	free(out);
	return r;
}

/* libgbm's backend directory from `ldconfig -p`: out, or "" when libgbm is not known. */
static void gbm_dir(int (*capture)(const char *const argv[], char *out, size_t len), char *out, size_t len)
{
	char *list = capture ? ldconfig_list(capture) : NULL, one[GS_PATH_MAX], *slash;

	out[0] = '\0';
	if (!list)
		return;
	if (ldconfig_entry(list, "libgbm.so.1 (", one, sizeof(one)) && (slash = strrchr(one, '/'))) {
		*slash = '\0';
		gs_pathf(out, len, "%s/gbm", one);
	}
	free(list);
}

/* Takes one entry of this package out of place while it is unchanged. 1 when the manifest no longer lists it, 0 when it is kept. */
static int nv_drop(struct ctx *x, struct entry *e, const char *why)
{
	char full[GS_PATH_MAX];

	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, e->path))
		return 0;
	if (!gs_exists(full)) {
		gs_manifest_drop(&x->m, e);
		return 1;
	}
	if (!entry_intact(e, full)) {
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

/* What one run writes: the destination of every file and link of the set, in the order they are written. */
struct plan_item {
	char kind;                 /* 'F' a copy of src from the staging directory, 'L' a link to `to`, 'X' the output-class file */
	int phase;                 /* 0 libraries and X modules, 1 the output-class file, 2 what makes NVIDIA's EGL reachable */
	int soft;                  /* an item that cannot be written is one warning; the set is installed without it */
	mode_t mode;
	char src[96];
	char to[GS_LINK_MAX];
	char path[GS_PATH_MAX];
};

struct plan {
	struct plan_item *i;
	unsigned int n;
};

static int plan_add(struct plan *p, char kind, mode_t mode, const char *src, const char *to, const char *dir, const char *name)
{
	struct plan_item *it;

	if (p->n == PLAN_MAX)
		return -ENOSPC;
	it = &p->i[p->n];
	memset(it, 0, sizeof(*it));
	it->kind = kind;
	it->mode = mode;
	if (src)
		gs_copy(it->src, sizeof(it->src), src);
	if (to)
		gs_copy(it->to, sizeof(it->to), to);
	if (gs_pathf(it->path, sizeof(it->path), "%s/%s", dir, name))
		return -ENAMETOOLONG;
	p->n++;
	return 0;
}

/* Puts the item just added in a phase of the write order. */
static void plan_phase(struct plan *p, int phase, int soft)
{
	p->i[p->n - 1].phase = phase;
	p->i[p->n - 1].soft = soft;
}

static int in_plan(const struct plan *p, const char *path)
{
	for (unsigned int i = 0; p && i < p->n; i++)
		if (strcmp(p->i[i].path, path) == 0)
			return 1;
	return 0;
}

static int plan_build(struct ctx *x, const struct gs_nvset *set, const struct nv_stage *st, const char *dir, const char *xdir,
		      const char *gbmdir, struct plan *p)
{
	char to[GS_PATH_MAX], sub[GS_PATH_MAX], probe[GS_PATH_MAX];
	int r = 0;

	for (unsigned int i = 0; i < st->n && !r; i++) {
		const struct nv_lib *l = &st->lib[i];

		/* Executable, as NVIDIA's installer leaves its libraries. */
		r = plan_add(p, 'F', 0755, l->file, NULL, dir, l->file);
		if (!r && strcmp(l->soname, l->file) != 0)
			r = plan_add(p, 'L', 0, NULL, l->file, dir, l->soname);
		/* The name without a version, which NVIDIA's installer makes for libcuda and some programs open. */
		if (!r && !set->desktop && i == 0)
			r = plan_add(p, 'L', 0, NULL, strcmp(l->soname, l->file) ? l->soname : l->file, dir, "libcuda.so");
	}
	for (unsigned int i = 0; i < st->nx && !r; i++) {
		const struct nv_xmod *m = &st->xmod[i];

		if (gs_pathf(sub, sizeof(sub), "%s/%s", xdir, m->sub)) {
			r = -ENAMETOOLONG;
			break;
		}
		r = plan_add(p, 'F', 0755, m->file, NULL, sub, m->file);
		if (!r && m->link[0] && strcmp(m->link, m->file) != 0)
			r = plan_add(p, 'L', 0, NULL, m->file, sub, m->link);
	}
	for (unsigned int i = 0; i < st->ne && !r; i++) {
		const struct nv_etc *e = &st->etc[i];

		if (strcmp(e->kind, "xconf") == 0)
			continue;
		if (strcmp(e->kind, "egl_platform") == 0 &&
		    (gs_pathf(probe, sizeof(probe), "%s%s/%s", x->destdir, gs_nvgl.share_platform, e->file) || gs_exists(probe))) {
			printf("host desktop: %s is not installed: the distribution has %s/%s\n", e->file, gs_nvgl.share_platform, e->file);
			continue;
		}
		r = plan_add(p, 'F', strcmp(e->kind, "profile") == 0 ? 0444 : 0644, e->file, NULL, etc_dir(e->kind), e->file);
		if (!r) {
			int profile = strcmp(e->kind, "profile") == 0;

			/* The libraries read the profiles only in /usr/share/nvidia, and draw without them: they may fail. The EGL files come last. */
			plan_phase(p, strcmp(e->kind, "egl_vendor") == 0 || strcmp(e->kind, "egl_platform") == 0 ? 2 : 0, profile);
		}
	}
	/* The output-class file is in before the EGL vendor file and the GBM link: where NVIDIA's EGL is reachable, X is on NVIDIA's driver. */
	for (unsigned int i = 0; i < st->ne && !r; i++)
		if (strcmp(st->etc[i].kind, "xconf") == 0) {
			r = plan_add(p, 'X', 0644, st->etc[i].file, NULL, gs_nvgl.xconf, XCONF_NAME);
			if (!r)
				plan_phase(p, 1, 0);
		}
	if (!r && st->gbm[0] && gbmdir[0]) {
		char gfull[GS_PATH_MAX];
		struct stat gst;

		if (gs_pathf(gfull, sizeof(gfull), "%s%s", x->destdir, gbmdir) == 0 && stat(gfull, &gst) == 0 && S_ISDIR(gst.st_mode)) {
			if (!gs_pathf(to, sizeof(to), "%s/%s", dir, st->gbm)) {
				r = plan_add(p, 'L', 0, NULL, to, gbmdir, GBM_NAME);
				if (!r)
					plan_phase(p, 2, 1);
			}
		} else {
			printf("host desktop: the GBM backend link is not made: libgbm's backend directory %s does not exist on this host.\n"
			       "X11 desktops are not affected; Wayland compositors do not use NVIDIA's GBM backend.\n", gbmdir);
		}
	}
	return r;
}

/* An existing file or link at a target that no earlier install of this package made: 1, named. */
static int plan_foreign(struct ctx *x, const struct plan *p, char *first, size_t len)
{
	char full[GS_PATH_MAX], have[GS_LINK_MAX];
	int found = 0;

	for (unsigned int i = 0; i < p->n; i++) {
		const struct plan_item *it = &p->i[i];
		struct entry *e;
		ssize_t n;

		if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, it->path) || (!gs_exists(full) && readlink(full, have, 1) < 0))
			continue;
		if (it->kind == 'L') {
			e = gs_manifest_find(&x->m, 'l', it->path);
			n = readlink(full, have, sizeof(have) - 1);
			if (n > 0)
				have[n] = '\0';
			if (n > 0 && (strcmp(have, it->to) == 0 || (e && strcmp(have, e->sha) == 0)))
				continue;
		} else if (gs_manifest_find(&x->m, 'f', it->path)) {
			continue;
		}
		if (!found)
			gs_copy(first, len, full);
		found++;
	}
	return found;
}

/*
 * Another NVIDIA graphics driver on the host: its files and these would be mixed. 1 and the first file
 * found, 0 when there is none.
 */
static int desktop_foreign(struct ctx *x, const char *dir, const struct gs_nvlibs_io *io, char *found, size_t len)
{
	char probe[GS_PATH_MAX];

	if (!gs_pathf(probe, sizeof(probe), "%s%s/10_nvidia.json", x->destdir, gs_nvgl.share_vendor) && gs_exists(probe)) {
		gs_copy(found, len, probe);
		return 1;
	}
	if (!gs_pathf(probe, sizeof(probe), "%s%s/nvidia_drv.so", x->destdir, gs_nvgl.xorg_drivers) && gs_exists(probe)) {
		gs_copy(found, len, probe);
		return 1;
	}
	return io && io->capture && gs_nvlibs_loader(&gs_nvset_desktop, io->capture, dir, found, len) == 0;
}

/*
 * The output-class file: NVIDIA's own, with the directory of the X modules added and one line saying so, and
 * ProbeAllGpus=false: NVIDIA's X driver otherwise probes every NVIDIA GPU, also the ones reserved for VMs, and allocates
 * about 1 MiB on each (measured 2026-11-12). NVIDIA documents the option for the Screen and Device sections; xorg.conf(5)
 * applies an OutputClass's Option entries to every matching device, so it is written here, where no BusID is pinned.
 * Made before anything is written, so that a file that cannot be used refuses the whole set (-EINVAL).
 * The keyword that ends the section is found with leading blanks and a carriage return allowed.
 */
static int xconf_render(const char *stage, const struct plan_item *it, const char *xdir, char **outp, size_t *lenp)
{
	char p[GS_PATH_MAX], *data, *out, *line, *rest;
	static const char head[] = "# NVIDIA's nvidia-drm-outputclass.conf, with the directory geforce-sriov keeps NVIDIA's X modules in.\n";
	size_t len, cap, off;
	int r, found = 0;

	if (gs_pathf(p, sizeof(p), "%s/%s", stage, it->src))
		return -ENAMETOOLONG;
	if ((r = gs_read_whole(p, &data, &len, 1 << 16)))
		return r;
	cap = len + sizeof(head) + strlen(xdir) + 512;
	if (!(out = malloc(cap))) {
		free(data);
		return -ENOMEM;
	}
	off = (size_t)snprintf(out, cap, "%s", head);
	rest = data;
	for (line = strsep(&rest, "\n"); line; line = strsep(&rest, "\n")) {
		const char *word = line + strspn(line, " \t");

		if (strncmp(word, "EndSection", 10) == 0 && word[10 + strspn(word + 10, " \t\r")] == '\0' && !found) {
			off += (size_t)snprintf(out + off, cap - off,
						"    # geforce-sriov: X probes only the boot display card, not the other NVIDIA cards; Xorg and the window manager can still keep their device nodes open.\n"
						"    # To run X on another NVIDIA GPU too, edit or remove this file (install writes it again), or run install --no-host-desktop.\n"
						"    Option         \"ProbeAllGpus\" \"false\"\n");
			off += (size_t)snprintf(out + off, cap - off, "    ModulePath     \"%s\"\n", xdir);
			found = 1;
		}
		if (!rest && !*line)
			break;
		off += (size_t)snprintf(out + off, cap - off, "%s\n", line);
	}
	/* A last empty piece is only the end of the final newline. */
	free(data);
	if (!found) {
		free(out);
		return -EINVAL;
	}
	*outp = out;
	*lenp = off;
	return 0;
}

/* When an entry of the set goes: 0 what makes NVIDIA's EGL reachable (EGL files, the GBM link), 1 the rest, 2 the output-class file. */
static int sweep_pass(const struct entry *e)
{
	if (e->type == 'f' && under(e->path, gs_nvgl.xconf) && strcmp(base_of(e->path), XCONF_NAME) == 0)
		return 2;
	if ((e->type == 'f' && (under(e->path, gs_nvgl.etc_vendor) || under(e->path, gs_nvgl.etc_platform))) ||
	    (e->type == 'l' && strcmp(base_of(e->path), GBM_NAME) == 0))
		return 0;
	return 1;
}

/* Every entry of the set that keep does not name goes; the EGL files and the GBM link first, the output-class file last. */
static void nv_sweep(struct ctx *x, const struct gs_nvset *set, const char *dir, const struct plan *keep, const char *why)
{
	char xdir[GS_PATH_MAX];

	xdir_of(dir, xdir, sizeof(xdir));
	for (int pass = 0; pass < 3; pass++) {
		for (unsigned int i = 0; i < x->m.n;) {
			struct entry *e = &x->m.e[i];

			if (!owned(set, dir, xdir, e) || in_plan(keep, e->path) || sweep_pass(e) != pass || nv_drop(x, e, why) == 0)
				i++;
			/* An entry that was dropped is replaced by the one after it. */
		}
	}
}

/* The directories this package made for the set go once they are empty; the desktop set's parents too. */
static void nv_prune(struct ctx *x, const struct gs_nvset *set, const char *dir)
{
	char xdir[GS_PATH_MAX], drv[GS_PATH_MAX], ext[GS_PATH_MAX], full[GS_PATH_MAX];
	const char *roots[9];
	unsigned int nroots = 0;

	roots[nroots++] = dir;
	if (set->desktop) {
		xdir_of(dir, xdir, sizeof(xdir));
		gs_pathf(drv, sizeof(drv), "%s/drivers", xdir);
		gs_pathf(ext, sizeof(ext), "%s/extensions", xdir);
		roots[nroots++] = xdir;
		roots[nroots++] = drv;
		roots[nroots++] = ext;
		roots[nroots++] = gs_nvgl.etc_vendor;
		roots[nroots++] = gs_nvgl.etc_platform;
		roots[nroots++] = gs_nvgl.etc_vulkan;
		roots[nroots++] = gs_nvgl.share_nvidia;
		roots[nroots++] = gs_nvgl.xconf;
	}
	for (unsigned int i = x->m.n; i-- > 0;) {
		struct entry *e = &x->m.e[i];
		size_t n = strlen(e->path);
		int related = 0;

		if (e->type != 'd')
			continue;
		for (unsigned int k = 0; k < nroots; k++)
			related |= strcmp(roots[k], e->path) == 0 ||
				   (set->desktop && strncmp(roots[k], e->path, n) == 0 && roots[k][n] == '/');
		if (!related || gs_pathf(full, sizeof(full), "%s%s", x->destdir, e->path) || rmdir(full) != 0)
			continue;
		printf("removed %s/\n", full);
		gs_manifest_drop(&x->m, e);
	}
}

static void run_ldconfig(const struct gs_nvset *set, const struct gs_nvlibs_io *io)
{
	const char *const argv[] = { "ldconfig", NULL };

	if (io && io->run && io->run(argv) != 0) {
		if (set->desktop)
			fprintf(stderr, "geforce-sriov: warning: ldconfig failed. Run `ldconfig` as root, so that programs find NVIDIA's graphics\n"
				"libraries.\n");
		else
			fprintf(stderr, "geforce-sriov: warning: ldconfig failed. Run `ldconfig` as root, so that programs find NVIDIA's CUDA\n"
				"libraries.\n");
	}
}

/* The reason a set is removed, and what it is called in the lines about it. */
static const char *off_why(const struct gs_nvset *set)
{
	return set->desktop ? "host desktop is off" : "host CUDA is off";
}

int gs_nvlibs_sync(struct ctx *x, const struct gs_nvset *set, const struct gs_nvdest *d, int want, const struct gs_nvlibs_io *io)
{
	char have_dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX], xdir[GS_PATH_MAX], gbmdir[GS_PATH_MAX], text[GS_PATH_MAX + 2];
	const char *const rm_stage[] = { "rm", "-rf", "--", d->stage, NULL };
	struct nv_stage *st = malloc(sizeof(*st));
	struct plan plan = { 0, 0 };
	struct entry *e;
	char *xbuf = NULL;
	size_t xlen = 0;
	int before = x->written, r = 0, state, staged;

	x->written = 0;
	xdir_of(d->dir, xdir, sizeof(xdir));
	if (!st)
		return -ENOMEM;
	if (!want) {
		nv_sweep(x, set, d->dir, NULL, off_why(set));
		/* A directory of an earlier prefix: its entries are this package's too. */
		if (nv_state(&x->m, set, x->destdir, have_dir, sizeof(have_dir), version, sizeof(version), bad, sizeof(bad)))
			nv_sweep(x, set, have_dir, NULL, off_why(set));
		nv_prune(x, set, d->dir);
		if (have_dir[0] && strcmp(have_dir, d->dir) != 0)
			nv_prune(x, set, have_dir);
		if ((e = gs_manifest_find(&x->m, 'f', d->conf)))
			(void)nv_drop(x, e, off_why(set));
		if (gs_exists(d->stage) && io && io->run)
			io->run(rm_stage);
		if (x->written)
			run_ldconfig(set, io);
		goto out;
	}
	staged = stage_read(set, d->stage, st);
	if (staged == -EINVAL) {
		if (set->desktop)
			fprintf(stderr, "geforce-sriov: warning: %s does not hold what the driver step recorded there; NVIDIA's graphics libraries\n"
				"are not installed from it.\n", d->stage);
		else
			fprintf(stderr, "geforce-sriov: warning: %s does not hold what the driver step recorded there; NVIDIA's CUDA libraries\n"
				"are not installed from it.\n", d->stage);
	}
	if (staged == 0 && set->desktop && d->accept && d->accept[0] && strcmp(st->version, d->accept) != 0) {
		fprintf(stderr, "geforce-sriov: warning: NVIDIA's graphics libraries are not installed: the staged ones are version %s and the installed\n"
			"nvidia.ko accepts version %s. Run `sudo ./geforce-sriov install --companion-file PATH` with the Linux driver package of\n"
			"the version the driver was built for.\n", st->version, d->accept);
		staged = -ECANCELED;
	}
	if (staged == 0 && set->desktop) {
		char other[GS_PATH_MAX];

		if (desktop_foreign(x, d->dir, io, other, sizeof(other))) {
			fprintf(stderr, "geforce-sriov: warning: NVIDIA's graphics libraries are not installed: %s belongs to another NVIDIA graphics\n"
				"driver, and the two would be mixed. Remove that driver, or run `sudo ./geforce-sriov install --no-host-desktop`.\n", other);
			staged = -ECANCELED;
		}
	}
	if (staged == 0) {
		plan.i = calloc(PLAN_MAX, sizeof(*plan.i));
		if (!plan.i) {
			free(st);
			return -ENOMEM;
		}
		gbmdir[0] = '\0';
		if (set->desktop)
			gbm_dir(io ? io->capture : NULL, gbmdir, sizeof(gbmdir));
		r = plan_build(x, set, st, d->dir, xdir, gbmdir, &plan);
		if (!r && set->desktop && plan_foreign(x, &plan, bad, sizeof(bad))) {
			fprintf(stderr, "geforce-sriov: warning: NVIDIA's graphics libraries are not installed: %s exists and was not installed by\n"
				"geforce-sriov, and is kept as it is. Move it away, then run the same install command again.\n", bad);
			staged = -ECANCELED;
		}
	}
	if (staged == 0 && set->desktop) {
		/* NVIDIA's output-class file has to be usable before the first file is written. */
		for (unsigned int i = 0; i < plan.n && staged == 0; i++) {
			if (plan.i[i].kind != 'X')
				continue;
			if (xconf_render(d->stage, &plan.i[i], xdir, &xbuf, &xlen)) {
				fprintf(stderr, "geforce-sriov: warning: NVIDIA's graphics libraries are not installed: %s/%s has no `EndSection` line\n"
					"that geforce-sriov can add the X module path before. Nothing was written.\n", d->stage, plan.i[i].src);
				staged = -ECANCELED;
			}
		}
	}
	if (staged == 0) {
		/* Phase 0: the libraries and X modules; phase 1: the output-class file; then the loader's file and its cache; phase 2: what makes NVIDIA's EGL reachable. */
		for (int phase = 0; phase < 3 && !r; phase++) {
			if (phase == 2 && !r) {
				if (snprintf(text, sizeof(text), "%s\n", d->dir) > 0)
					r = gs_install_put(x, d->conf, text, strlen(text), 0644);
				if (!r) {
					/* The libraries of another version, from an earlier Linux driver package. */
					nv_sweep(x, set, d->dir, &plan, set->desktop ? "it belongs to another version of NVIDIA's graphics libraries" :
						 "it belongs to another version of NVIDIA's CUDA libraries");
					/* The desktop set's loader cache has the libraries before NVIDIA's EGL is made reachable. */
					if (set->desktop && x->written)
						run_ldconfig(set, io);
				}
			}
			for (unsigned int i = 0; i < plan.n && !r; i++) {
				const struct plan_item *it = &plan.i[i];
				char src[GS_PATH_MAX];
				int w = 0;

				if (it->phase != phase)
					continue;
				if (it->kind == 'F' && !gs_pathf(src, sizeof(src), "%s/%s", d->stage, it->src))
					w = gs_install_copy(x, src, it->path, it->mode);
				else if (it->kind == 'L')
					w = gs_install_link(x, it->path, it->to);
				else if (it->kind == 'X')
					w = gs_install_put(x, it->path, xbuf, xlen, it->mode);
				if (w && it->soft) {
					fprintf(stderr, "geforce-sriov: warning: %s%s could not be written: %s. The set is installed without it.\n",
						x->destdir, it->path, strerror(-w));
					w = 0;
				}
				r = w;
			}
		}
		if (r && set->desktop) {
			/* Not a set that stopped half-way: what this run wrote is taken out again, NVIDIA's EGL first and the output-class file last. */
			nv_sweep(x, set, d->dir, NULL, "the install of NVIDIA's graphics libraries failed");
			nv_prune(x, set, d->dir);
			if ((e = gs_manifest_find(&x->m, 'f', d->conf)))
				(void)nv_drop(x, e, "the install of NVIDIA's graphics libraries failed");
			if (x->written)
				run_ldconfig(set, io);
		}
		if (r) {
			if (set->desktop)
				fprintf(stderr, "geforce-sriov: warning: NVIDIA's graphics libraries could not be installed in %s%s: %s. What this run wrote\n"
					"was taken out again; the desktop is as it was without them. Fix the cause, then run the same install command again.\n",
					x->destdir, d->dir, strerror(-r));
			else
				fprintf(stderr, "geforce-sriov: warning: NVIDIA's CUDA libraries could not be installed in %s%s: %s. CUDA programs on the\n"
					"host may not start. Fix the cause, then run the same install command again.\n", x->destdir, d->dir, strerror(-r));
			if (set->desktop)
				staged = -ECANCELED;
		} else {
			if (io && io->run)
				io->run(rm_stage);
			if (!set->desktop && x->written)
				run_ldconfig(set, io);
			if (x->written) {
				if (set->desktop)
					printf("host desktop: NVIDIA's graphics libraries %s are installed in %s%s\n", st->version, x->destdir, d->dir);
				else
					printf("host CUDA: NVIDIA's CUDA libraries %s are installed in %s%s\n", st->version, x->destdir, d->dir);
			}
		}
	}
	free(xbuf);
	free(plan.i);
	state = nv_state(&x->m, set, x->destdir, have_dir, sizeof(have_dir), version, sizeof(version), bad, sizeof(bad));
	if (x->destdir[0]) {
		if (state != 1) {
			if (set->desktop)
				printf("host desktop: NVIDIA's graphics libraries are not part of a staged install. On the target host,\n"
				       "`geforce-sriov install --host-desktop --companion-file PATH` installs them.\n");
			else
				printf("host CUDA: NVIDIA's CUDA libraries are not part of a staged install. On the target host,\n"
				       "`geforce-sriov install --host-cuda --companion-file PATH` installs them.\n");
		}
	} else if (state == 0 && staged != -ECANCELED) {
		if (!set->desktop)
			fprintf(stderr, "geforce-sriov: warning: host CUDA is on, but NVIDIA's CUDA library (libcuda) is not installed, so CUDA\n"
				"programs on the host cannot start. It comes from the Linux driver package, and this run did not get it\n"
				"from there (%s has the reason when that package was read).\n"
				"Run `sudo ./geforce-sriov install --host-cuda --companion-file PATH` in the source folder with that package.\n",
				GS_DRIVER_LOG);
		else if (want == GS_NVLIBS_AUTO)
			printf("host desktop: not installed (host_desktop=auto): NVIDIA's graphics libraries were not taken from the\n"
			       "Linux driver package (%s has the reason when that package was read). Run\n"
			       "`sudo ./geforce-sriov install --companion-file PATH` in the source folder with that package to turn it on.\n",
			       GS_DRIVER_LOG);
		else
			fprintf(stderr, "geforce-sriov: warning: host_desktop=on, but NVIDIA's graphics libraries are not installed, so the desktop\n"
				"is drawn by the CPU. They come from the Linux driver package, and this run did not get them from\n"
				"there (%s has the reason when that package was read).\n"
				"Run `sudo ./geforce-sriov install --host-desktop --companion-file PATH` in the source folder with that package.\n",
				GS_DRIVER_LOG);
	} else if (state < 0) {
		if (set->desktop)
			fprintf(stderr, "geforce-sriov: warning: NVIDIA's graphics libraries are incomplete: %s is missing or was changed since\n"
				"install. Run `sudo ./geforce-sriov install --host-desktop --companion-file PATH` in the source folder to install\n"
				"them again.\n", bad);
		else
			fprintf(stderr, "geforce-sriov: warning: NVIDIA's CUDA libraries are incomplete: %s is missing or was changed since\n"
				"install. Run `sudo ./geforce-sriov install --host-cuda --companion-file PATH` in the source folder to install\n"
				"them again.\n", bad);
	} else if (state == 1 && io && io->capture && gs_nvlibs_loader(set, io->capture, have_dir, bad, sizeof(bad)) == 0) {
		if (set->desktop)
			fprintf(stderr, "geforce-sriov: warning: programs on this host load another NVIDIA graphics library first: %s. geforce-sriov did\n"
				"not install it and leaves it as it is. It works only if it has the version of the loaded NVIDIA driver;\n"
				"otherwise remove it or move it away, then run `ldconfig`. The libraries of this install are in %s.\n",
				bad, have_dir);
		else
			fprintf(stderr, "geforce-sriov: warning: programs on this host load another CUDA library first: %s. geforce-sriov did\n"
				"not install it and leaves it as it is. It works only if it has the version of the loaded NVIDIA driver;\n"
				"otherwise remove it or move it away, then run `ldconfig`. The libraries of this install are in %s.\n",
				bad, have_dir);
	}
out:
	free(st);
	r = x->written;
	x->written |= before;
	return r;
}

int gs_nvlibs_apply(const struct gs_nvset *set, const char *manifest, const struct gs_nvdest *d, int want, const struct gs_nvlibs_io *io)
{
	struct ctx *x = calloc(1, sizeof(*x));
	int r;

	if (!x)
		return -ENOMEM;
	x->destdir = "";
	if ((r = gs_manifest_load(manifest, &x->m)) == 0) {
		gs_nvlibs_sync(x, set, d, want, io);
		r = gs_manifest_save(manifest, &x->m);
	}
	free(x);
	return r;
}

/* A library of the installed set that cannot load: the first soname `ldd` cannot find, "" when none. */
static void unresolved(int (*capture)(const char *const argv[], char *out, size_t len), const char *dir, const char *version, char *out,
		       size_t len)
{
	static const char *const names[] = { "libEGL_nvidia.so.", "libGLX_nvidia.so." };
	char path[GS_PATH_MAX], *text = malloc(1 << 16), *at;

	out[0] = '\0';
	if (!text)
		return;
	for (unsigned int i = 0; i < 2 && !out[0]; i++) {
		const char *argv[] = { "ldd", path, NULL };

		if (gs_pathf(path, sizeof(path), "%s/%s%s", dir, names[i], version))
			continue;
		text[0] = '\0';
		if (capture(argv, text, 1 << 16) != 0 || !(at = strstr(text, "=> not found")))
			continue;
		while (at > text && at[-1] != '\n')
			at--;
		at += strspn(at, " \t");
		gs_copy(out, len, at);
		if ((at = strchr(out, ' ')))
			*at = '\0';
		if (out[0])
			snprintf(out + strlen(out), len - strlen(out), " (of %s%s)", names[i], version);
	}
	free(text);
}

int gs_nvlibs_line(const struct gs_nvset *set, const char *manifest, int want,
		   int (*capture)(const char *const argv[], char *out, size_t len), char *line, size_t len)
{
	char dir[GS_PATH_MAX], version[32], bad[GS_PATH_MAX];
	int state = gs_nvlibs_state(set, manifest, dir, sizeof(dir), version, sizeof(version), bad, sizeof(bad));

	line[0] = '\0';
	if (!want) {
		if (!state)
			return 0;
		if (set->desktop)
			snprintf(line, len, "NVIDIA's graphics libraries %s are installed in %s although host_desktop=off; the next "
				 "`geforce-sriov install` removes them", version, dir);
		else
			snprintf(line, len, "libcuda %s is installed in %s although host_cuda=off; the next `geforce-sriov install` removes it",
				 version, dir);
		return 2;
	}
	if (!state) {
		if (set->desktop)
			snprintf(line, len, "not installed, so the desktop is drawn by the CPU; run `sudo ./geforce-sriov install "
				 "--host-desktop --companion-file PATH` in the source folder with the Linux driver package");
		else
			snprintf(line, len, "not installed, so CUDA programs on the host cannot start; run `sudo ./geforce-sriov install "
				 "--host-cuda --companion-file PATH` in the source folder with the Linux driver package");
		return -1;
	}
	if (state < 0) {
		if (set->desktop)
			snprintf(line, len, "incomplete: %s is missing or was changed since install; run `sudo ./geforce-sriov install "
				 "--host-desktop --companion-file PATH` in the source folder to install them again", bad);
		else
			snprintf(line, len, "incomplete: %s is missing or was changed since install; run `sudo ./geforce-sriov install "
				 "--host-cuda --companion-file PATH` in the source folder to install them again", bad);
		return -1;
	}
	if (capture && gs_nvlibs_loader(set, capture, dir, bad, sizeof(bad)) == 0) {
		if (set->desktop)
			snprintf(line, len, "NVIDIA's graphics libraries %s are installed in %s, but programs load another one first: %s, "
				 "which geforce-sriov did not install. Remove it or move it away, then run `ldconfig`", version, dir, bad);
		else
			snprintf(line, len, "libcuda %s is installed in %s, but programs load another one first: %s, which geforce-sriov did "
				 "not install. Remove it or move it away, then run `ldconfig`", version, dir, bad);
		return -1;
	}
	if (set->desktop && capture) {
		char gdir[GS_PATH_MAX], link[GS_PATH_MAX] = "", *slash;
		struct manifest *m = calloc(1, sizeof(*m));

		gbm_dir(capture, gdir, sizeof(gdir));
		if (m && gs_manifest_load(manifest, m) == 0) {
			for (unsigned int i = 0; i < m->n; i++)
				if (m->e[i].type == 'l' && strcmp(base_of(m->e[i].path), GBM_NAME) == 0)
					gs_copy(link, sizeof(link), m->e[i].path);
		}
		free(m);
		if (link[0] && gdir[0] && (slash = strrchr(link, '/')) && (*slash = '\0', strcmp(link, gdir) != 0)) {
			snprintf(line, len, "NVIDIA's graphics libraries %s are installed in %s, but libgbm looks for its backends in %s and the "
				 "GBM backend link is in %s, so Wayland compositors do not find it; run `sudo ./geforce-sriov install "
				 "--host-desktop --companion-file PATH` again", version, dir, gdir, link);
			return -1;
		}
		unresolved(capture, dir, version, bad, sizeof(bad));
		if (bad[0]) {
			snprintf(line, len, "NVIDIA's graphics libraries %s are installed in %s, but a library cannot load: %s is not found; "
				 "install the distribution package that has it", version, dir, bad);
			return -1;
		}
	}
	if (set->desktop)
		snprintf(line, len, "NVIDIA's graphics libraries %s are installed in %s", version, dir);
	else
		snprintf(line, len, "libcuda %s is installed in %s", version, dir);
	return 1;
}

const char *gs_host_desktop_name(enum gs_host_desktop m)
{
	return m == GS_DESKTOP_ON ? "on" : m == GS_DESKTOP_OFF ? "off" : "auto";
}

/* Does the process's memory map name one of NVIDIA's graphics libraries (or X driver)? 1, 0, or -2 when the map cannot be read (another user's process). */
static int maps_have_nvidia(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[GS_PATH_MAX + 128];
	int r = 0;

	if (!f)
		return -2;
	while (!r && fgets(line, sizeof(line), f))
		r = strstr(line, "/nvidia_drv.so") || strstr(line, "/libnvidia-glcore.so") || strstr(line, "/libnvidia-eglcore.so") ||
		    strstr(line, "/libEGL_nvidia.so");
	fclose(f);
	return r;
}

/* 1 draws with NVIDIA, 0 does not, -2 cannot be told, -1 nothing: the better answer wins. */
static int server_rank(int v)
{
	return v == 1 ? 3 : v == 0 ? 2 : v == -2 ? 1 : 0;
}

int gs_desktop_servers(const char *proc, char *line, size_t len)
{
	static const struct { const char *comm, *label; } known[] = {
		{ "Xorg", "the X server" }, { "kwin_wayland", "kwin_wayland" }, { "gnome-shell", "gnome-shell" },
		{ "sway", "sway" }, { "weston", "weston" },
	};
	DIR *d = opendir(proc);
	struct dirent *de;
	int found = -1;

	line[0] = '\0';
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		char p[GS_PATH_MAX], comm[64] = "";
		FILE *f;
		unsigned int k;

		if (de->d_name[0] < '0' || de->d_name[0] > '9' || gs_pathf(p, sizeof(p), "%s/%s/comm", proc, de->d_name))
			continue;
		if (!(f = fopen(p, "r")))
			continue;
		if (fgets(comm, sizeof(comm), f))
			comm[strcspn(comm, "\n")] = '\0';
		fclose(f);
		for (k = 0; k < sizeof(known) / sizeof(known[0]) && strcmp(comm, known[k].comm) != 0; k++)
			;
		if (k == sizeof(known) / sizeof(known[0]))
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s/maps", proc, de->d_name))
			continue;
		/* A server that draws with NVIDIA's driver wins over one that does not. */
		{
			int nv = maps_have_nvidia(p);

			if (server_rank(nv) > server_rank(found)) {
				snprintf(line, len, "%s (pid %s)", known[k].label, de->d_name);
				found = nv;
			}
		}
	}
	closedir(d);
	return found;
}

/*
 * The host desktop sentence of the host checks when it is wanted: sv is gs_desktop_servers' answer for the display
 * server named in server, stale is 1 when the running nvidia module is not the installed build (loaded is the
 * sentence that says so). "Draws in software" is said only when the live renderer is seen to be software; a
 * display server that draws with NVIDIA's driver, or none that can be told, gets the power-cycle sentence.
 */
int gs_desktop_draw_line(int sv, const char *server, int stale, const char *loaded, char *line, size_t len)
{
	line[0] = '\0';
	if (stale && sv == 0) {
		snprintf(line, len, "%s draws in software, and the running nvidia module is not the installed build; the host "
			 "desktop stays in software until the next power cycle loads it (%s)", server, loaded);
		return 3;
	}
	if (stale) {
		if (sv == 1)
			snprintf(line, len, "%s draws with NVIDIA's driver; the rebuilt module is used after the next power cycle (%s)",
				 server, loaded);
		else
			snprintf(line, len, "the running nvidia module is not the installed build; the rebuilt module is used after the "
				 "next power cycle (%s)", loaded);
		return 1;
	}
	if (sv == 0) {
		snprintf(line, len, "%s runs without NVIDIA's graphics libraries although host_desktop is on; the desktop draws in "
			 "software. The X log (/var/log/Xorg.0.log) and `journalctl -k | grep 'API mismatch'` say why", server);
		return 2;
	}
	if (sv == -2) {
		snprintf(line, len, "%s is running; whether it draws with NVIDIA's driver cannot be checked without root", server);
		return 1;
	}
	return 0;
}
