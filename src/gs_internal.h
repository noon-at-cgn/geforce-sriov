/* SPDX-License-Identifier: MIT */
/*
 * Shared by the files of the CLI and not part of gs_lifecycle.h: what was local to one file
 * while its users sat in that file.
 */
#ifndef GS_INTERNAL_H
#define GS_INTERNAL_H

#include "gs_lifecycle.h"

/* ---- from lifecycle.c ---- */

/* systemd's unit search path, most significant first. */
static const char *const gs_unit_dirs[] = { "/etc/systemd/system", "/run/systemd/system", "/usr/local/lib/systemd/system",
					 "/usr/lib/systemd/system", "/lib/systemd/system" };

/*
 * The earlier Ada-only (H5) plugin copy of 580.159.01: refused, the patched plugin copy replaces it.
 * It is a file an earlier release of this package made, not a revision of any confirmed chain, so
 * it is not a field of the version table. The other plugin digests are in the rows of that table.
 */
#define ADA_PLUGIN_H5_SHA "097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37"

/* What a refusal of a GPU that does not come back from a power-off suggests. */
#define WAKE_REMEDY \
	"Switch the laptop to its discrete GPU (dGPU or discrete mode in the firmware setup), or stop whatever powers " \
	"the GPU off (for example bbswitch), then run activate again"

/* Room for build_line: the module, record and release paths, both build-ids and the sentence. */
#define BUILD_LINE_MAX (3 * GS_PATH_MAX)

const char *gs_sw_name(int v);
const char *gs_display_mode_name(enum gs_display_mode m);
int gs_pf_runtime_status(const char *sysroot, const struct gs_addr *pf, char *buf, size_t len);
int gs_is_live_root(const char *sysroot);
int gs_diag_config(const struct gs_diag_opts *o, struct gs_config *cfg, const char **used, char *err,
		       size_t errlen);
unsigned int gs_selected_devices(const struct gs_diag_opts *o, const struct gs_config *cfg,
				     const char *sysroot, struct gs_addr *out, const char **source);
int gs_same_selection(const struct gs_addr *a, unsigned int na, const struct gs_addr *b, unsigned int nb);
int gs_module_selects(const struct gs_module_state *m, const struct gs_addr *pf);
const struct gs_device *gs_pf_entry(const char *sysroot, const struct gs_addr *pf, unsigned int *device);
int gs_config_dword(const char *dir, unsigned int off, unsigned int *out);
int gs_config_gate(const char *dir, unsigned int device, char *why, size_t len);
int gs_pf_bridge_dir(const char *sysroot, const struct gs_addr *pf, char *out, size_t len);
unsigned int gs_laptop_lost(const char *sysroot, const struct gs_card *c, char out[2][256]);
int gs_iommu_peers(const char *sysroot, const struct gs_card *c, int *with_bridge, char *first, size_t len,
		       unsigned int *nothers);
int gs_config_file_safe(const char *path, char *why, size_t len);
int gs_installed_module_present(void);
int gs_nvidia_autoload(char *where, size_t len);
void gs_combination_line(const struct gs_build *b, char *line, size_t len);
int gs_record_value(const char *record, const char *key, char *out, size_t len);
int gs_activating_left(const struct gs_diag_opts *o, int live, char *p, size_t len);

/* ---- from install.c ---- */

#define MAX_ENTRIES 192

#define DKMS_ID GS_DKMS_NAME "/" GS_VERSION

#define UNIT_DIR GS_UNIT_DIR

struct entry {
	char type;            /* 'f' file, 'd' directory created by install, 'l' symbolic link */
	char sha[65];         /* of a file; "-" for a directory; the target of a link */
	char path[GS_PATH_MAX];
};

struct manifest {
	struct entry e[MAX_ENTRIES];
	unsigned int n;
};

/*
 * Does install still own the file at path, recorded with hash want? Only while it is the regular file
 * with that hash: gs_sha256_file never follows a link, so a link put in its place is not owned.
 */
enum owned { OWN_ABSENT, OWN_UNREADABLE, OWN_CHANGED, OWN_YES };

struct ctx {
	const char *destdir; /* "" for live */
	struct manifest m;
	int written;         /* this run created a file or directory */
};

static const char *const kernel_required[] = { "geforce_sriov.c", "Kbuild", "Makefile", "dkms.conf", NULL };

struct entry *gs_manifest_find(struct manifest *m, char type, const char *path);
int gs_manifest_load(const char *file, struct manifest *m);
int gs_manifest_save(const char *file, const struct manifest *m);
enum owned gs_manifest_owns(const char *path, const char *want);
int gs_install_copy(struct ctx *x, const char *src, const char *target, mode_t mode);
int gs_install_put(struct ctx *x, const char *target, const void *data, size_t len, mode_t mode);
/* A symbolic link at target that names to, recorded as 'l'. One that is there and is not this package's refuses. */
int gs_install_link(struct ctx *x, const char *target, const char *to);
void gs_manifest_drop(struct manifest *m, struct entry *e);
/* NVIDIA's CUDA libraries (src/cudalibs.c): what was written or removed; never an error of the install. */
int gs_cuda_libs_sync(struct ctx *x, const char *dir, const char *conf, const char *stage, int want, const struct gs_cuda_io *io);
int gs_deps_step(void);
int gs_package_conflict(const char *file, const char *root, int accept_license);
int gs_nvidia_files(struct ctx *x, const struct gs_install_opts *o, const struct gs_config *cfg, char *dir,
			size_t len);
int gs_is_kernel_source(const char *name);

#endif
