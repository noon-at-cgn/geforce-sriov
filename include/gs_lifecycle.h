/* SPDX-License-Identifier: MIT */
/*
 * geforce-sriov host CLI: configuration, sysfs inspection and lifecycle commands.
 *
 * Read-only commands (devices, doctor, status) accept a diagnostic sysfs root.
 * Every mutating function in this interface takes no root argument: it acts on
 * the live /sys only, so a fixture root can never reach a mutation path.
 */
#ifndef GS_LIFECYCLE_H
#define GS_LIFECYCLE_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#include "gs_devices.h"

/* Exit codes of the geforce-sriov binary. */
#define GS_EXIT_OK 0
#define GS_EXIT_FAIL 1         /* a step failed; the message names it */
#define GS_EXIT_USAGE 2        /* bad command line */
#define GS_EXIT_REFUSED 3      /* a precondition does not hold; nothing was changed */
#define GS_EXIT_LOCKED 4       /* another geforce-sriov process holds the lock */
#define GS_EXIT_COLD_CYCLE 5   /* guard set: a full power-off is required before anything else */

#define GS_MODULE_NAME "geforce_sriov"
#define GS_DKMS_NAME "geforce-sriov"
#define GS_UNIT_DIR "/etc/systemd/system"
/* The early unit: loads the livepatch before udev; it removes, rescans and defers nothing. */
#define GS_UNIT_LIVEPATCH "geforce-sriov-livepatch.service"
/* The link `systemctl enable` creates for the early unit (WantedBy=sysinit.target). */
#define GS_UNIT_WANTS_LIVEPATCH GS_UNIT_DIR "/sysinit.target.wants/" GS_UNIT_LIVEPATCH
/* The one activation transaction, ordered before the display manager and the VM autostarts. */
#define GS_UNIT_ACTIVATE "geforce-sriov-activate.service"
/* The package's drop-in for a supported VM autostart unit. */
#define GS_VM_DROPIN_NAME "geforce-sriov.conf"
/* The name of the package's drop-in in nvidia-vgpud.service.d, which preloads the identity hook. */
#define GS_VGPUD_DROPIN_NAME "geforce-sriov.conf"

#define GS_CONFIG_PATH "/etc/geforce-sriov/config"
/* Package-owned modprobe option so any autoload of UVM keeps HMM off (host CUDA builds). */
#define GS_UVM_OPTION_PATH "/etc/modprobe.d/geforce-sriov-uvm.conf"
/* Package-owned options so any autoload of nvidia_drm starts with KMS, fbdev and the allowlist. */
#define GS_DRM_OPTION_PATH "/etc/modprobe.d/geforce-sriov-display.conf"
/* Package-owned alias overrides: udev binds nvidia_drm to the PCI class alias and ignores
 * blacklist/install rules, so the alias itself is mapped away; the early unit loads nvidia
 * (--load-driver) and activation loads nvidia_drm. */
#define GS_AUTOLOAD_PATH "/etc/modprobe.d/geforce-sriov-autoload.conf"
#define GS_CONFIG_ENV "GEFORCE_SRIOV_CONFIG"
#define GS_CONFIG_MAX_BYTES 65536
#define GS_STATE_DIR "/var/lib/geforce-sriov"
#define GS_ATTEMPT_NAME "prepare-attempt"
#define GS_ATTEMPT_MARKER GS_STATE_DIR "/" GS_ATTEMPT_NAME
#define GS_MANIFEST_NAME "installed-files"
#define GS_MANIFEST GS_STATE_DIR "/" GS_MANIFEST_NAME
/* The driver step's whole output during install, rewritten by each run (mode 0600). */
#define GS_DRIVER_LOG GS_STATE_DIR "/driver-step.log"
/* Written by install when it replaces nvidia.ko: kernel=, module_sha256=, module_build_id= lines. */
#define GS_DRIVER_BUILD_NAME "driver-build"
#define GS_DRIVER_BUILD GS_STATE_DIR "/" GS_DRIVER_BUILD_NAME
/* Beside it: the record of each other kernel the driver step built for, one file named after the kernel. */
#define GS_DRIVER_BUILDS_NAME "driver-builds"
#define GS_NVIDIA_MODPROBE "/etc/modprobe.d/geforce-sriov-nvidia.conf"
/* Host CUDA: where the driver step puts NVIDIA's CUDA libraries for install, and the loader's file for their directory. */
#define GS_CUDA_STAGE GS_STATE_DIR "/host-cuda"
#define GS_CUDA_CONF "/etc/ld.so.conf.d/geforce-sriov-cuda.conf"
#define GS_RUN_DIR "/run/geforce-sriov"
#define GS_LOCK_PATH GS_RUN_DIR "/lock"
#define GS_LIVE_SYSFS "/sys"
#define GS_LIVE_PROC "/proc"
/* The only firmware framebuffer kind the display handoff releases. */
#define GS_SIMPLEFB_PREFIX "simple-framebuffer."
#define GS_SRIOV_MANAGE_DEFAULT "/usr/lib/nvidia/sriov-manage"
#define GS_DEFAULT_PREFIX "/usr/local"

#define GS_CFG_MAX_BDFS GS_MAX_PFS
#define GS_PATH_MAX 4096

/* Canonical PCI address as sysfs names it: dddd:bb:ss.f, lowercase hex, 4 to 8 domain digits. */
struct gs_addr {
	unsigned int domain;
	unsigned int bus;
	unsigned int slot;
	unsigned int func;
	char str[GS_BDF_STR_SIZE];
};

/* 0 when s is exactly a canonical lowercase address; -1 otherwise. */
int gs_addr_parse(const char *s, struct gs_addr *out);

/* Tri-state switch values for configuration overrides. */
#define GS_SW_UNSET (-1)
#define GS_SW_OFF 0
#define GS_SW_ON 1

enum gs_display_mode { GS_DISPLAY_AUTO, GS_DISPLAY_ON, GS_DISPLAY_OFF };
/* fb_slots=: hold the framebuffer slots of Ada VFs. auto and on mean the same today; off is the way out. */
enum gs_fb_slots { GS_FB_SLOTS_AUTO, GS_FB_SLOTS_ON, GS_FB_SLOTS_OFF };

/*
 * /etc/geforce-sriov/config, shared with the identity hook. Syntax: at most
 * 65536 bytes; lines split on '\n' (the last may lack one); empty lines and
 * lines whose first byte is '#' are ignored; any other line is key=value with
 * key [a-z0-9_]+, a non-empty value and no whitespace or '\r' anywhere; a
 * duplicate or unknown key makes the whole file invalid.
 */
struct gs_config {
	struct gs_addr bdfs[GS_CFG_MAX_BDFS];
	unsigned int nbdfs;
	enum gs_display_mode display;
	enum gs_fb_slots fb_slots;
	int host_cuda;
	int ari;                 /* default on */
	int msix;                /* default on */
	unsigned int vfs;        /* VFs per card, 1..GS_VFS_MAX; 0: not set, GS_DEFAULT_VFS applies */
	int identity;            /* hook only, default off */
	int identity_force_host; /* hook only, default off */
	unsigned int type_ada;    /* vGPU type activate sets on each VF of an Ada card; 0: GS_VGPU_TYPE_ADA */
	unsigned int type_ampere; /* the same for an Ampere card; 0: GS_VGPU_TYPE_AMPERE */
	char sriov_manage[GS_PATH_MAX];
	/* d3cold=off: consent to keep a laptop GPU powered (prepare's D3cold guards); 0 without the key */
	int d3cold_off;
	int present;             /* the file existed */
};

void gs_config_defaults(struct gs_config *cfg);
int gs_config_parse(const char *text, size_t len, struct gs_config *cfg, char *err, size_t errlen);
/* Missing file: defaults with present=0 and return 0. Invalid: -1 and err. */
int gs_config_load(const char *path, struct gs_config *cfg, char *err, size_t errlen);
/* Canonical text of cfg; returns length or -1 when buf is too small. */
int gs_config_render(const struct gs_config *cfg, char *buf, size_t len);
/* $GEFORCE_SRIOV_CONFIG when set and non-empty, else GS_CONFIG_PATH. */
const char *gs_config_path(void);
/* Comma-separated list of 1..8 canonical function-0 addresses without duplicates; errors name key. */
int gs_parse_addr_list(const char *key, const char *value, struct gs_addr *out, unsigned int *n, char *err,
		       size_t errlen);
int gs_parse_switch(const char *value, int *out);
/* "a,b,c" of n addresses into buf (room for GS_BDF_LIST_STR_SIZE). */
void gs_addr_list_str(const struct gs_addr *list, unsigned int n, char *buf, size_t len);
/* The VF count the module is loaded with: vfs, or GS_DEFAULT_VFS when not set. */
unsigned int gs_config_vfs(const struct gs_config *cfg);
/* vfs=N value: decimal 1..GS_VFS_MAX without a leading zero. 0 or -1. */
int gs_parse_vfs(const char *value, unsigned int *out);

/* One PCI function of the inspected card or its bus. */
struct gs_fn {
	struct gs_addr bdf;
	unsigned int vendor;
	unsigned int device;
	unsigned int class_code;  /* 24-bit class */
	int is_vf;
	char driver[64];          /* "" when unbound */
};

#define GS_MAX_BUS_FNS 32

/* Everything read about one selected physical function and its bus. */
struct gs_card {
	struct gs_addr pf;
	int present;
	unsigned int vendor, device, subvendor, subdevice, class_code;
	int is_vf;
	const struct gs_device *entry; /* table entry, or NULL */
	char driver[64];
	int boot_vga;             /* -1 unknown */
	int has_sriov;            /* sriov_totalvfs exists */
	unsigned int totalvfs, numvfs;
	int ari_enabled;          /* -1 unknown */
	int msix_linked;          /* MSI-X in the config space's capability list; -1 config unreadable or short */
	char iommu_group[32];
	/* From <pf>/resource: 1 assigned, 0 all-zero (unassigned), -1 unknown or absent. */
	int bar0_assigned;        /* line 0: BAR0 */
	int vf_bars_assigned;     /* lines 7-12: the SR-IOV VF BARs (CONFIG_PCI_IOV) */
	int bridge_is_pci;        /* the parent is a PCI bridge function */
	struct gs_addr bridge;
	char bridge_dir[GS_PATH_MAX];
	char rescan_path[GS_PATH_MAX];
	int rescan_present;
	char bridge_pm[16];       /* power/control */
	char bridge_rt[32];       /* power/runtime_status */
	int bridge_ari_fwd_supported; /* DevCap2 bit 5; -1 unknown */
	struct gs_fn fns[GS_MAX_BUS_FNS];   /* card functions other than the PF (same slot) */
	unsigned int nfns;
	struct gs_fn others[GS_MAX_BUS_FNS]; /* functions on the bus outside the card's slot */
	unsigned int nothers;
	unsigned int nvfs_on_bus;
};

/* Reads sysroot (e.g. "/sys") only. Returns 0; card->present tells whether the PF exists. */
int gs_card_inspect(const char *sysroot, const struct gs_addr *pf, struct gs_card *card);
/* 1 when activation puts the host console (KMS) on this card: display is not off and it is the boot display GPU. */
int gs_display_kms(const struct gs_config *cfg, const struct gs_card *c);
/*
 * The configured VF count against one listed card: -1 with why when vfs is set above what the card
 * offers (its hardware TotalVFs, gs_vf_ceiling()); 1 with why when the card would be given more than
 * GS_DEFAULT_VFS VFs, with which the module refuses to load if the card's VF layout check fails; 0
 * otherwise.
 */
int gs_vfs_check(const struct gs_device *e, const struct gs_config *cfg, char *why, size_t len);
/* "hardware default 16, geforce-sriov presents 4 (default)" for a listed card. */
void gs_vfs_describe(const struct gs_device *e, const struct gs_config *cfg, char *buf, size_t len);
/* SR-IOV presented with sriov_totalvfs 1..gs_config_vfs(cfg)? 0, or -1 with why. */
int gs_check_presented(const struct gs_card *c, const struct gs_config *cfg, char *why, size_t len);

/* The most companion functions one card may have unbound in one transaction (activation). */
#define GS_PLAN_MAX_UNBIND 8

/* Install-time selection check: the PF may legitimately be bound to nvidia or to no driver.
 * release_granted is kept for source compatibility and ignored (console releases were removed). */
int gs_check_selectable(const struct gs_card *card, int release_granted, char *reason, size_t len);

/*
 * Console users of a boot display PF, read by gs_display_inspect(). The only firmware framebuffer
 * it accepts is the kernel's simple-framebuffer.N platform device whose parent is this PF.
 */
enum gs_fw_state { GS_FW_NONE, GS_FW_RELEASED, GS_FW_BOUND };
struct gs_display {
	char fw[64];              /* the PF's own simple-framebuffer.N, "" when it has none */
	char fw_dir[GS_PATH_MAX]; /* its resolved sysfs directory */
	char fw_driver[64];       /* its driver; "" when unbound (released) */
	unsigned int nconsumers;  /* fb and DRM devices of that framebuffer */
	int proc_checked;         /* /proc/iomem and /proc/consoles were read */
	char refuse[384];         /* why releasing the console is not safe; "" when it is */
};

/*
 * Reads sysroot and, when procroot is not NULL, procroot/iomem and procroot/consoles. Any user
 * of the card's memory or console other than its own firmware framebuffer, an unreadable source,
 * or a firmware framebuffer that cannot be attributed to this PF sets refuse.
 */
int gs_display_inspect(const char *sysroot, const char *procroot, const struct gs_card *c, struct gs_display *d);
static inline enum gs_fw_state gs_display_fw_state(const struct gs_display *d)
{
	return !d->fw[0] ? GS_FW_NONE : d->fw_driver[0] ? GS_FW_BOUND : GS_FW_RELEASED;
}
/*
 * /proc/iomem text: the only resource inside a BAR of pf may be one BOOTFB, and only when the PF
 * has its firmware framebuffer; while that is released, nothing may remain inside BOOTFB.
 * 0 or -1 with why.
 */
int gs_iomem_check(const char *iomem, const char *pf, enum gs_fw_state fw, char *why, size_t len);
/* /proc/consoles text: -1 with why when an early EFI framebuffer console (efifb) is registered. */
int gs_consoles_check(const char *consoles, char *why, size_t len);

/*
 * The driver step's record (GS_DRIVER_BUILD) against the nvidia.ko installed for a kernel release.
 * ABSENT: no record names this release; PRESENT: a record for it names another module (or the module's
 * GNU build-id cannot be read); MATCHES: the module's build-id is the recorded module_build_id.
 */
enum gs_build_state { GS_BUILD_ABSENT, GS_BUILD_PRESENT, GS_BUILD_MATCHES };
struct gs_build {
	enum gs_build_state state;
	int record_read;         /* the record file was read */
	char record[GS_PATH_MAX], release[128];
	char module[GS_PATH_MAX]; /* the nvidia.ko read; "" when none was found */
	char found_id[129];      /* its GNU build-id; "" when unreadable */
	char rec_kernel[128], rec_sha256[65], rec_id[129]; /* the recorded values */
	char driver_version[128];
	int driver_version_confirmed; /* -1 when not recorded */
	int display, host_cuda;       /* -1 when not recorded */
};

enum gs_version_package { GS_VERSION_HOST, GS_VERSION_COMPANION };
int gs_version_confirmed(enum gs_version_package package, const char *version);
/*
 * One confirmed host driver version and what belongs to it: the version of the second package,
 * the guest drivers to use on each generation, and the recorded digests the CLI judges files by.
 * The rows are the blocks of scripts/gs-driver-versions.tbl, the one place that says which
 * versions are confirmed and holds their data; the build makes src/gs_versions.inc from it, and
 * the driver step reads the same file. A new version is a new block. A message that names the
 * confirmed versions lists every row. A digest a row does not record is NULL.
 */
struct gs_confirmed_version {
	const char *host;           /* NVIDIA's vGPU host driver */
	const char *second;         /* the second package for display and host CUDA builds */
	const char *public;         /* NVIDIA's public kernel module source for display builds */
	const char *guest_ampere;   /* guest drivers for an Ampere card, as printed */
	const char *guest_ada;      /* the same for an Ada card */
	const char *package;        /* SHA-256 of NVIDIA's vGPU host package (.run) */
	const char *stock_plugin;   /* SHA-256 of NVIDIA's unchanged plugin (docs/ada.md) */
	const char *mixed_plugin;   /* ... of the earlier copy with the first plugin patch only: `install` replaces it */
	const char *patched_plugin; /* ... of the patched plugin copy, the only one activate accepts */
};
const struct gs_confirmed_version *gs_confirmed_versions(size_t *count);
/* The first confirmed host version: the one a hint names when it needs a single version. */
const char *gs_default_host_version(void);
/*
 * NVIDIA's kernel tree when none is named: /usr/src/nvidia-<version> of the first confirmed
 * version whose tree is there, else of the first confirmed version.
 */
const char *gs_nvidia_source_default(void);
/* Replace the table (tests); NULL puts the built-in one back. The rows must outlive their use. */
void gs_confirmed_versions_set(const struct gs_confirmed_version *rows, size_t count);
/* The confirmed host versions as "A" or "A, B". */
void gs_confirmed_list(char *buf, size_t len);
enum gs_version_message {
	GS_VERSION_MSG_CONFIRMED, GS_VERSION_MSG_PROMPT, GS_VERSION_MSG_BEST_EFFORT,
	GS_VERSION_MSG_SETUP_REFUSAL, GS_VERSION_MSG_INSTALL_NOTICE,
	GS_VERSION_MSG_DOCTOR_WARNING, GS_VERSION_MSG_STATUS, GS_VERSION_MSG_OPEN_REFUSAL
};
int gs_version_format(enum gs_version_message message, const char *version, char *buf, size_t len);

enum gs_resource_fit_state { GS_RESOURCE_FIT_UNKNOWN, GS_RESOURCE_FIT_OK, GS_RESOURCE_FIT_INSUFFICIENT };
struct gs_resource_fit {
	enum gs_resource_fit_state state;
	unsigned int vfs;
	unsigned long long nonprefetch_need, prefetch_need;
	unsigned long long nonprefetch_window, prefetch_window;
	const char *source;
	/* 1 when only the growth room is unknown (a root port under the host bridge) and the
	 * rescan itself decides; the removal is still verified afterwards. */
	int room_unknown;
};
/* The record file, the kernel release and the nvidia.ko for it ("" or NULL: none found). Reads files only. */
enum gs_build_state gs_driver_build_read(const char *record, const char *release, const char *module,
					 struct gs_build *out);
/*
 * The sentence doctor, check and activate print for b: 0 when the module is the driver step's build,
 * 1 otherwise, with the command that rebuilds it and the features the record names.
 */
int gs_build_line(const struct gs_build *b, char *line, size_t len);
/* The nvidia.ko a release would load, found as the driver step finds it. 0, or -1 with out "". */
int gs_nvidia_module_path(const char *release, char *out, size_t len);
/*
 * Whether the *.conf files of the modprobe.d directory dir hold nvidia back at boot (a `blacklist nvidia`
 * line and an `install nvidia /bin/false` line): 0 with files naming the file, or both files when the
 * lines are in different ones ("a and b"); -1 when they do not.
 */
int gs_nvidia_hold(const char *dir, char *files, size_t len);
/* Fixture sources for the driver build check (tests); a NULL field is the live host's. */
struct gs_build_src {
	const char *record;  /* NULL: GS_DRIVER_BUILD */
	const char *release; /* NULL: the running kernel's */
	const char *module;  /* NULL: `modinfo -k <release> -n nvidia`, else /lib/modules/<release>/updates/dkms/nvidia.ko */
};
/*
 * 1 when the installed nvidia.ko is the driver step's build and the nvidia module loaded under sysroot
 * (its build-id note) is another build; 0 otherwise. src NULL: the live host's, skipped on a fixture root.
 */
int gs_loaded_not_installed(const char *sysroot, const struct gs_build_src *src);

/* Read-only commands. sysroot NULL means the live /sys. */
struct gs_diag_opts {
	const char *sysroot;
	/* doctor: the feature selection and build inputs install would get, judged before it runs. */
	int display_set, host_cuda_set, host_cuda;
	int skip_driver;           /* --driver=skip: install builds nothing, so no build input is needed */
	enum gs_display_mode display;
	const char *companion_file, *companion_source, *nvkms_source;
	const char *config_path;   /* NULL: default rules (see gs_cmd_doctor) */
	struct gs_addr devices[GS_CFG_MAX_BDFS];
	unsigned int ndevices;
	int disable_d3cold;       /* doctor --disable-d3cold: install writes d3cold=off (needs a laptop GPU among those judged) */
	const char *nvidia_source; /* doctor: NVIDIA's kernel tree to check; NULL: gs_nvidia_source_default() */
	const char *driver_file;   /* doctor --driver-file: install extracts NVIDIA's tree from this package */
	const struct gs_build_src *build; /* doctor, status: NULL: the live host's, skipped on a fixture root */
	const char *modprobe_dir; /* doctor: modprobe.d to read; NULL: /etc/modprobe.d, skipped on a fixture root */
	const char *plugin;       /* doctor: libnvidia-vgpu.so to check; NULL: NVIDIA's plugin link, skipped on a fixture root */
	const char *unit_dir;     /* doctor: unit directory whose vGPU daemon drop-ins to read; NULL: systemd's unit
				   * directories, skipped on a fixture root */
	const char *run_dir;      /* doctor, status: where GS_ACTIVATING_NAME is; NULL: GS_RUN_DIR, skipped on a fixture root */
};
int gs_cmd_devices(void);
int gs_cmd_doctor(const struct gs_diag_opts *opts);
int gs_cmd_status(const struct gs_diag_opts *opts);

/*
 * Where prepare reads and writes: gs_host_live, or a fixture tree with a simulated kernel behind
 * write_attr and run (tests).
 * Deadline callbacks execute one operation, returning -ETIMEDOUT when its
 * completion is uncertain. Fixtures should model the same single operation
 * and may inject a timeout; NULL falls back to the synchronous callback.
 * State records use state_dir and run_dir in the fixture filesystem.
 *
 * The same code runs on every host. What differs between the real system and a fixture is data in
 * this structure, never a test of which one it is: root prefixes the host's other files, build
 * says where the build record is, and enable_prereq vets the programs the real runner will execute.
 */
/*
 * The framebuffer slots of Ada VFs (src/fbslot.c). The owner inside nvidia.ko holds them and is
 * told what to hold through GS_FB_PROC below the procfs root; its presence is how the CLI knows
 * that the loaded module carries the owner.
 */
#define GS_FB_PROC "driver/nvidia/geforce-sriov-fb"
/* The alignment of the vGPU plugin's request, in 580.159.01: 32 MiB. */
#define GS_FB_ALIGN 0x2000000ull
#define GS_FB_NO_PLACEMENT 0xffffu
#define GS_VFS_MAX_QUERY 64
/* What NVIDIA's driver answers for one VF: its status, and then the placement and the range. */
struct gs_fb_answer {
	unsigned int status, placement;
	unsigned long long offset, length;
};
struct gs_fb_query {
	unsigned long long ram_size, segment; /* the GPU's memory, and the size its ranges are multiples of */
	struct gs_fb_answer vf[GS_VFS_MAX_QUERY];
};
struct gs_fb_range {
	unsigned long long offset, size;
};
/*
 * The slots the answers for nvf VFs name, into out (room for nvf); *n may be 0, as for an Ampere
 * card. -1 with why when one answer does not hold together: then no slot of that GPU is held.
 */
int gs_fb_validate(const struct gs_fb_query *q, unsigned int nvf, struct gs_fb_range *out, unsigned int *n, char *why,
		   size_t len);
enum gs_fb_slot_kind { GS_FB_HELD, GS_FB_GIVEN_UP, GS_FB_FAILED };
struct gs_fb_slot_state {
	char bdf[16];
	unsigned long long offset, size;
	enum gs_fb_slot_kind state;
	unsigned int dups, answered, status;
	char why[160];
};
#define GS_FB_STATE_MAX 128
struct gs_fb_state {
	int present, abi;
	unsigned int n;
	struct gs_fb_slot_state slot[GS_FB_STATE_MAX];
};
/* What the owner in the loaded nvidia.ko holds: 0, -ENOENT when that module has no owner, or another negative errno. */
int gs_fb_state_read(const char *procroot, struct gs_fb_state *st);
struct gs_vgpu_card;
struct gs_config;
struct gs_host;
/* activate, before anything is unloaded or re-probed: the owner lets go of everything. Never fails the caller. */
void gs_fb_release(const struct gs_host *h);
/* activate, once the VFs have their vGPU types: ask, check and hold the slots of each Ada card. Never fails the caller. */
void gs_fb_hold(const struct gs_host *h, const struct gs_config *cfg, const struct gs_vgpu_card *vc, unsigned int n);
/*
 * The sentence status, doctor and check print about the slots of the Ada GPU pf. record is the
 * path of the build record or NULL; ver the running driver version or "". 1: every known slot is
 * held; 0: off by the config; -1: not held, or not all; -2: the owner could not be read.
 */
int gs_fb_line(const char *procroot, const struct gs_config *cfg, const char *record, const char *pf, const char *ver,
	       char *line, size_t len);

struct gs_host {
	const char *sys;       /* sysfs root */
	const char *proc;      /* procfs root */
	const char *state_dir; /* the guard GS_ATTEMPT_NAME */
	const char *run_dir;   /* this boot: GS_ACTIVATING_NAME while an activation runs */
	const char *root;      /* prefix of /etc/systemd and /run/systemd; "" or NULL: the real root */
	/* The build record and module to judge when the caller names none; NULL: none is known, nothing is judged. */
	const struct gs_build_src *build;
	/* The programs the VF enable runs are safe to run as root: 0, or -1 with why. NULL: nothing to vet. */
	int (*enable_prereq)(const struct gs_config *cfg, char *why, size_t len);
	/* The real system: the caller must be root and hold the lock, and the config must be root's. */
	int system;
	int (*write_attr)(const char *path, const char *value);
	int (*run)(const char *const argv[]);
	/* argv run with its standard output kept, as gs_run_capture; NULL: nothing is run (tests) */
	int (*capture)(const char *const argv[], char *out, size_t len);
	int (*write_attr_deadline)(const struct gs_host *, const char *, const char *, unsigned int);
	int (*run_deadline)(const struct gs_host *, const char *const [], unsigned int);
	/*
	 * Where NVIDIA's driver puts the framebuffer of each VF of a card (src/fbslot.c): 0, or -1 with
	 * why. NULL: the driver itself is asked, through its device files below root.
	 */
	int (*fb_query)(const struct gs_host *, const struct gs_vgpu_card *, struct gs_fb_query *, char *why, size_t len);
};
extern const struct gs_host gs_host_live;
/* The live host's build record and module: every field NULL, so each takes its default. */
extern const struct gs_build_src gs_build_live;
/* gs_host_live's enable_prereq: sriov-manage is a root-owned executable, and lspci and setpci exist for it. */
int gs_enable_prereq(const struct gs_config *cfg, char *why, size_t len);

/* Mutating commands: live system only. */
struct gs_overrides {
	int ari, msix; /* GS_SW_* */
};

/*
 * Laptop GPUs and D3cold (docs/laptops.md#d3cold). gs_laptop_gpus: the laptop rows (gs_device.laptop) among
 * the n addresses of list, by each one's device attribute below sys (readable while the card is off), into
 * out; returns how many. gs_d3cold_usage: --disable-d3cold without a laptop GPU selected, said on standard
 * error; returns GS_EXIT_USAGE.
 */
unsigned int gs_laptop_gpus(const char *sys, const struct gs_addr *list, unsigned int n, struct gs_addr *out);
int gs_d3cold_usage(void);
/*
 * Why prepare refuses the laptop GPUs laptops (n of them) while the config has no d3cold=off: the text after
 * "refused, nothing changed: ", ending with the install command that keeps the selection sel and its grants rel.
 * Each line after the first starts with indent. buf takes GS_D3COLD_WHY_SIZE bytes.
 */
#define GS_D3COLD_WHY_SIZE 1536
void gs_d3cold_why(char *buf, size_t len, const struct gs_addr *laptops, unsigned int n, const struct gs_addr *sel,
		   unsigned int nsel, const struct gs_addr *rel, unsigned int nrel, const char *indent);

/*
 * Ada and Ampere CPU-RM host stack (docs/driver-patch.md#byte-patches). activate: enable the VFs of a
 * selected PF that has none, load nvidia_vgpu_vfio, restart nvidia-vgpud and nvidia-vgpu-mgr,
 * re-probe each selected PF (driver unbind/bind, never remove/rescan), set its vGPU type on each of
 * its VFs, then power/control=on on each VF, gated on the readback, and print what check prints.
 * check: read-only gate before every VM start. Both act on every selected card; activate refuses
 * while any vGPU exists.
 */
#define GS_VGPU_TYPE_MAX 999999u
/* The vGPU type activate sets and check expects on every VF of a card unless the config or --type overrides it. */
#define GS_VGPU_TYPE_ADA 942u    /* NVIDIA RTX6000-Ada-2Q, 2048 MiB */
#define GS_VGPU_TYPE_AMPERE 525u /* NVIDIA RTXA6000-2Q, 2048 MiB */
/* The default type of a generation. */
static inline unsigned int gs_vgpu_type_default(unsigned int generation)
{
	return generation == GS_ADA ? GS_VGPU_TYPE_ADA : GS_VGPU_TYPE_AMPERE;
}
/* The type of a card of generation: type_ada or type_ampere of cfg when set, else gs_vgpu_type_default(). */
unsigned int gs_config_vgpu_type(const struct gs_config *cfg, unsigned int generation);
/* Where NVIDIA's host package installs its vGPU type list; activate reads profile sizes from it. */
#define GS_VGPU_CONFIG "/usr/share/nvidia/vgpu/vgpuConfig.xml"
struct gs_activate_opts {
	struct gs_addr devices[GS_CFG_MAX_BDFS]; /* check --device; none: every selected GPU (activate: always none) */
	unsigned int ndevices;
	unsigned int type;       /* --type N for every selected card; 0: each card's gs_config_vgpu_type() */
	int dry_run;
	int boot;                /* --boot: first wait up to GS_BOOT_BIND_WAIT_MS for each selected PF to bind to nvidia */
	unsigned int boot_wait_ms; /* --boot's wait; 0: GS_BOOT_BIND_WAIT_MS (tests use a short one) */
	unsigned int settle_ms;  /* pause after nvidia-vgpu-mgr is active, before the re-probe */
	unsigned int types_wait_ms; /* wait for a VF's vGPU types after the re-probe; 0: 15 s (tests use a short one) */
	const char *plugin;      /* libnvidia-vgpu.so whose SHA-256 must be the patched plugin copy; NULL: not checked (tests) */
	const char *vgpu_config; /* vgpuConfig.xml for the Ada slot hint; NULL: no hint (tests) */
	const struct gs_build_src *build; /* driver build check; NULL: the live host's, skipped on a fixture host */
	int clear_guard;
	int daemon_ready;
	int stop_display_manager; /* setup obtained consent to stop the display manager */
};
#define GS_BOOT_BIND_WAIT_MS 30000u
/* --type and type_ada=/type_ampere= value: decimal 1..GS_VGPU_TYPE_MAX without a leading zero. 0 or -1. */
int gs_parse_vgpu_type(const char *value, unsigned int *out);
/*
 * Verdict on the plugin's SHA-256 sha: 0 for the patched plugin copy of a confirmed version, else
 * -1 with why naming path and real (its resolved path). Pure, so tests can feed it the real digests,
 * which no test file has.
 */
int gs_plugin_verdict(const char *path, const char *real, const char *sha, char *why, size_t len);
/* 0 when ver is the version this build targets ("supported"); 1 for any other version
 * ("not supported"), with the sentence doctor, check and activate print in line. */
int gs_driver_version_line(const char *ver, char *line, size_t len);
int gs_cmd_activate(const struct gs_activate_opts *o);
/* activate after the root check, lock and config load. */
int gs_activate(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o);
int gs_cmd_check(const struct gs_activate_opts *o);
/*
 * check against h->sys with cfg (tests use a fixture host); a point-in-time read with no lock, so
 * nothing stops a re-probe or daemon restart between it and a VM start. NOT READY while h->state_dir holds the
 * guard or h->run_dir holds GS_ACTIVATING_NAME, which activate writes before its first change and
 * removes only after every VF read back on/active.
 */
#define GS_ACTIVATING_NAME "activating"
int gs_check_vfs(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o);

struct gs_install_opts {
	const char *prefix;        /* absolute; default GS_DEFAULT_PREFIX */
	const char *destdir;       /* "" or NULL: live install */
	const char *source;        /* built tree; NULL: directory of the running binary */
	struct gs_addr devices[GS_CFG_MAX_BDFS];
	enum gs_display_mode display;
	int display_set;
	int host_cuda, host_cuda_set;
	const char *companion_file, *companion_source, *nvkms_source;
	int allow_unsupported_driver;
	int from_setup;            /* --from-setup: setup runs this install and prints the closing instructions itself */
	unsigned int ndevices;
	struct gs_overrides ov;
	unsigned int vfs;          /* --vfs N; 0: not given */
	int skip_driver;           /* --driver=skip: no NVIDIA module or plugin work */
	const char *nvidia_source; /* --nvidia-source DIR: NVIDIA's extracted package; NULL: gs_nvidia_source_default() */
	const char *driver_file;   /* --driver-file PATH: NVIDIA's .run package (absolute, resolved) */
	int accept_nvidia_license; /* --accept-nvidia-license: run NVIDIA's installer from driver_file */
	int disable_d3cold;        /* --disable-d3cold: write d3cold=off (needs a laptop GPU selected) */
};
int gs_cmd_install(const struct gs_install_opts *opts);
/*
 * The framed notice before setup's D3cold question for a laptop GPU: what D3cold does, what d3cold=off keeps
 * powered and what it costs, what the user must configure, and how to revert. In red when gs_color(f);
 * gs_d3cold_notice_color: in red when color, else plain.
 */
void gs_d3cold_notice(FILE *f);
void gs_d3cold_notice_color(FILE *f, int color);
/* Whether f takes colour: a terminal, NO_COLOR unset or empty, and TERM not "dumb". */
int gs_color(FILE *f);
/*
 * The end of a live install: the early livepatch unit and the activation unit, how to enable them,
 * and the next step (doctor, then activate or the reboot the first install needs).
 */
/* The closing lines of an install that a boot has to pick up. from_setup: setup gives the instructions itself. */
void gs_install_next(const char *bindir, const struct gs_config *cfg, int from_setup);
int gs_cmd_uninstall(int unload);
/* setup's questions: AUTO asks when standard input is a terminal, ON always (--interactive), OFF never (--no-prompt). */
enum gs_prompt { GS_PROMPT_AUTO, GS_PROMPT_ON, GS_PROMPT_OFF };
/*
 * setup: from the host's state, the next steps of a first install (doctor, install, then activation
 * when it is safe) or of an installed host (doctor, check, activate, check), each run as a child
 * process of this binary; ends with the guest drivers of each selected card. o carries the selection
 * and install's feature and package options; on an installed host --device must name exactly the
 * configured GPUs. On a terminal or with --interactive it asks once per run: version consent for a
 * driver that is not confirmed to work (--allow-unsupported-driver answers it), display-manager
 * consent, which it passes to activate as --stop-display-manager, and D3cold consent for a selected
 * laptop GPU (d3cold=off in the config or --disable-d3cold answers it). Without a question an
 * unconsented step refuses before any change (GS_EXIT_REFUSED). --disable-d3cold without a selected
 * laptop GPU is GS_EXIT_USAGE.
 */
int gs_cmd_setup(const struct gs_install_opts *o, enum gs_prompt prompt);
/*
 * setup with the host it reads (a fixture in tests): h->sys lists the cards, h->run runs each step.
 * in is the descriptor the answers come from, -1 to ask nothing; driver_present: NVIDIA's kernel tree
 * or vGPU userspace is on the host; config: the config file of an installed host, NULL for
 * gs_config_path() (the steps read GS_CONFIG_PATH, so another path set in the environment refuses);
 * build: the driver build sources (NULL: the live host's, skipped on a fixture host); modprobe_dir:
 * where nvidia's modprobe.d files are read (NULL: /etc/modprobe.d).
 */
struct gs_setup_host {
	const struct gs_host *h;
	int in;
	int driver_present;
	const char *config;
	const struct gs_build_src *build;
	const char *modprobe_dir;
};
int gs_setup(const struct gs_setup_host *s, const struct gs_install_opts *o);
/* One manifest file: removed only while still the regular file with hash want (never through a
 * link). 1 removed; 0 kept or absent; -errno when the unlink failed. */
int gs_uninstall_file(const char *path, const char *want);
/*
 * uninstall's removal of what the manifest at path lists: the driver step's --restore first, then every
 * recorded file except those keep names (NULL-terminated, or NULL), the manifest and the emptied directories.
 * Files kept for any reason (changed since install, unreadable, or in keep) stay in the manifest, with
 * the directories still there, so a later install takes them over and a later uninstall removes them.
 * When --restore fails, the driver script, NVIDIA's backup and the record stay, the manifest keeps the
 * script, and the way back is printed. After a restore, the backups left in the manifest's directory
 * (other kernels) are named with the command that puts each back. GS_EXIT_OK or GS_EXIT_FAIL.
 */
int gs_uninstall_files(const char *path, const char *modprobe_dir, const char *const keep[]);

/* Prerequisites of a live install; install refuses, and doctor names them, with GS_PREREQ_MSG. */
enum gs_dep { GS_DEP_HEADERS, GS_DEP_DKMS, GS_DEP_PCIUTILS, GS_DEP_TOOLCHAIN, GS_DEP_COUNT };
#define GS_PREREQ_MSG \
	"A live install needs kernel headers for the running release, dkms, pciutils, a C compiler and make; " \
	"install them with your distribution's package manager."
/* 1 when dep is present on the live host (headers: /lib/modules/<release>/build/Makefile). */
int gs_dep_present(enum gs_dep dep, const char *release);

/* The x.y.z version in a package file name ("NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run"): 0 or -1. */
int gs_nvidia_package_version(const char *name, char *out, size_t len);
#define GS_NVIDIA_PKG_DIR GS_STATE_DIR "/nvidia"
/* Beside it, while install --driver-file runs: the copy of NVIDIA's package that is hashed and run. */
#define GS_PACKAGE_COPY_NAME ".package"
/*
 * What the driver step keeps after a successful run so that the same build can be made again for
 * another kernel: a copy of the second package and of the public source, and index, which names
 * them and NVIDIA's kernel tree (key=value lines). uninstall removes the directory.
 */
#define GS_INPUTS_NAME "inputs"
#define GS_INPUTS_DIR GS_STATE_DIR "/" GS_INPUTS_NAME
/* 1 when the index in dir names the kept input name ("companion", "nvkms"), else 0. */
int gs_kept_input(const char *dir, const char *name);
/*
 * rebuild: the driver step again, for kernel release (NULL: the running one), with the features
 * the config selects and the inputs an earlier install kept. It runs from the installed copy.
 */
int gs_cmd_rebuild(const char *release);
/*
 * NVIDIA's .run package file: extract it (`sh file --extract-only --target`) into base/<version, or
 * the first 16 hex digits of its SHA-256>/, reusing an earlier extraction of the same file. When
 * root (live: "") already has that version's libnvidia-vgpu.so, nothing is installed. Otherwise,
 * with accept_license and nouveau not loaded, run NVIDIA's installer (`sh file --silent --dkms
 * --skip-module-load --kernel-module-type=proprietary`) after the licence notice; without it, refuse:
 * the driver step needs the userspace. Writes the extracted root to dir. GS_EXIT_* with the reason
 * printed; a refusal after this call extracted the package is GS_EXIT_FAIL, because something was
 * written. *changed: 1 once it extracted the package or started NVIDIA's installer, else 0.
 */
int gs_nvidia_package(const char *file, const char *base, const char *root, int accept_license, char *dir, size_t len,
		      int *changed);
/* A unit file for name in one of systemd's system unit directories (a link counts). */
int gs_unit_file_present(const char *name);
/*
 * The driver step: argv run with a fixed environment. 0, or its exit status (-1 when it cannot run)
 * with in why the reason its last "refused: " or "failed: " line gives ("" when it gave none). log
 * NULL: its output passes through. Otherwise its standard output and error go to log (truncated,
 * mode 0600), only its own "geforce-sriov-driver:" lines reach standard output, and a failure prints
 * the last 40 lines of log and its path. result (NULL: not wanted) says what a step that succeeded
 * changed, read from its line `install_result=unchanged|record-only|changed`; without that line,
 * or with another value, it is GS_DRIVER_CHANGED. No message text is parsed.
 */
enum gs_driver_result {
	GS_DRIVER_CHANGED,     /* a module or the plugin copy was built or replaced, or the step did not say */
	GS_DRIVER_UNCHANGED,   /* the build was already installed and nothing was written */
	GS_DRIVER_RECORD_ONLY, /* the build was already installed; only the record's version fields changed */
};
int gs_driver_step(const char *const argv[], const char *log, char *why, size_t len, enum gs_driver_result *result);
/*
 * install's DKMS step for GS_DKMS_NAME/GS_VERSION, each command run by run (gs_run; tests record them).
 * registered: DKMS holds this version already. status: the output of `dkms status -m GS_DKMS_NAME`
 * (NULL or "": none), whose other versions of GS_DKMS_NAME (an upgrade) are replaced as well. When any
 * is registered, the new tree is first built in a scratch DKMS tree below the directory scratch
 * (`dkms --dkmstree`), which leaves the registered modules and their installed copies alone; only when
 * that succeeds are they removed (`dkms remove --all`, the other versions first) and the new one added,
 * built and installed for the running kernel. When that fails after the removal, the boot units are
 * disabled. After a successful install, the source tree /usr/src/GS_DKMS_NAME-<v> of each other version v
 * it removed goes too, but only when it is a real directory holding only files the install manifest at
 * manifest records and install still owns; the manifest drops the entries that are gone. Paths on disk
 * are below root ("" for the live system); a tree that stays is a warning only. 0, or GS_EXIT_FAIL with
 * what was removed and the way back printed.
 */
int gs_dkms_step(int (*run)(const char *const argv[]), int registered, const char *status, const char *scratch,
		 const char *root, const char *manifest);
/*
 * rebuild: the livepatch module of this version must be installed for the kernel NVIDIA's module is
 * built for. DKMS builds for a kernel only when that kernel's package is installed, so a kernel
 * that was on the host before geforce-sriov has none. managed: dkms is installed and holds this
 * version. Asks `dkms status` (capture) and, when the module is not installed for release, runs
 * `dkms install -k release` (run). GS_EXIT_OK, or GS_EXIT_FAIL when that install failed; named is
 * the kernel the user named, for the command the message repeats.
 */
/* ---- NVIDIA's CUDA libraries for host CUDA (src/cudalibs.c) ---------------- */

/* How the commands that install them are run: NULL members on a staged install, fixtures in the tests. */
struct gs_cuda_io {
	int (*run)(const char *const argv[]);
	int (*capture)(const char *const argv[], char *out, size_t len);
};
/*
 * Brings the libraries in dir in line with want, on the install manifest at path manifest: installs
 * them from stage when the driver step left them there, removes them when want is 0. 0, or a
 * negative errno when the manifest cannot be read or written.
 */
int gs_cuda_libs_apply(const char *manifest, const char *dir, const char *conf, const char *stage, int want,
		       const struct gs_cuda_io *io);
/*
 * What the manifest lists of them: 1 complete, 0 nothing, -1 a file is missing or changed (bad).
 * dir and version are those of the installed libcuda.
 */
int gs_cuda_libs_state(const char *manifest, char *dir, size_t dlen, char *version, size_t vlen, char *bad, size_t blen);
/* Which libcuda.so.1 the loader takes, from `ldconfig -p`: 1 the one in dir, 0 another (found), -1 not known. */
int gs_cuda_loader(int (*capture)(const char *const argv[], char *out, size_t len), const char *dir, char *found, size_t len);
/*
 * The sentence status and doctor print after "host CUDA libraries: ". 1 in place, -1 not (a
 * warning), 2 installed although host CUDA is off, 0 nothing to say. capture NULL: the loader is not asked.
 */
int gs_cuda_libs_line(const char *manifest, int want, int (*capture)(const char *const argv[], char *out, size_t len),
		      char *line, size_t len);

int gs_dkms_ensure(int (*run)(const char *const argv[]), int (*capture)(const char *const argv[], char *out, size_t len),
		   int managed, const char *release, const char *named);
/*
 * install: the DKMS step takes this version out of DKMS for every kernel (`dkms remove --all`) before
 * it adds it again, and then builds for the running kernel alone. Every other installed kernel that
 * NVIDIA's module is built for (a record in state_dir/driver-builds) gets the livepatch module back
 * here; one that DKMS cannot build for is a warning that names `rebuild --kernel`, never a failure.
 */
void gs_dkms_other_kernels(int (*run)(const char *const argv[]), const char *root, const char *state_dir);
/* Is the livepatch module installed for release, under root ("" on a live host)? */
int gs_module_present_for(const char *root, const char *release);
/*
 * The installed kernels that NVIDIA's module was built for (a record in state_dir/driver-builds)
 * and that have no livepatch module: their names, comma-separated, in list; the count is returned.
 */
unsigned int gs_kernels_without_livepatch(const char *root, const char *state_dir, char *list, size_t len);

/* ---- readers shared by the commands (src/hostinfo.c) ------------------------ */

/* What may fix an unassigned BAR; activation never resizes the bridge windows above the card. */
#define BAR_REMEDY "try enabling Above 4G decoding in the firmware, or boot with pci=realloc"

/* What to do after an activation that left the guard set. */
extern const char *const gs_recovery_msg;

struct gs_module_state {
	int loaded;
	char ari[8], msix[8], vfs[8], bdfs[256];
	int lp_present;
	unsigned int lp_enabled, lp_transition;
	int lp_ok;
};

/* One selected PF, the vGPU type to set on its VFs, and the last inspection. */
struct gs_vgpu_card {
	struct gs_addr pf;
	struct gs_addr vf[GS_VFS_MAX];
	unsigned int nvf;
	int enable;        /* no VFs yet: activate enables them first */
	unsigned int generation;
	unsigned int type; /* --type, or gs_config_vgpu_type() of its generation */
};

int gs_addr_listed(const struct gs_addr *list, unsigned int n, const char *s);
int gs_cmdline_noari(const char *procroot);
void gs_nvidia_version(const char *procroot, char *out, size_t len);
int gs_note_build_id(const unsigned char *buf, size_t size, char *out, size_t len);
int gs_loaded_line(const char *sysroot, const struct gs_build *b, char *line, size_t len);
int gs_build_identity(const struct gs_build_src *src, int live, struct gs_build *b);
void gs_read_module_state(const char *sysroot, struct gs_module_state *m);
unsigned int gs_module_vfs(const struct gs_module_state *m);
int gs_read_dev_attr(const char *sys, const char *bdf, const char *name, char *buf, size_t len);
void gs_dev_driver(const char *sys, const char *bdf, char *out, size_t len);
int gs_vf_list(const char *sys, const struct gs_addr *pf, unsigned int numvfs, struct gs_addr *out, char *why,
	       size_t len);
int gs_select_cards(const char *sys, const struct gs_config *cfg, const struct gs_activate_opts *o,
		    struct gs_vgpu_card *out, unsigned int *n);
const char *gs_plugin_link(void);

/* ---- NVIDIA's kernel modules (src/modules.c) --------------------------------- */

/* How long one modprobe or rmmod may take. */
#define GS_MODULE_DEADLINE_MS 60000u
#define GS_NVIDIA_DISPLAY_MODULES 2
#define GS_NVIDIA_CLIENT_MODULES 3
extern const char *const gs_nvidia_clients[GS_NVIDIA_CLIENT_MODULES];
/* One host process through the deadline callback, or the synchronous one when there is none. */
int gs_host_run(const struct gs_host *h, const char *const argv[], unsigned int ms);
int gs_module_loaded(const struct gs_host *h, const char *name);
/*
 * The device nodes of NVIDIA's loaded modules that do not exist under h->root: /dev/nvidiactl and one
 * /dev/nvidia<minor> per GPU that is not a VF (nvidia), /dev/nvidia-modeset (nvidia_modeset),
 * /dev/nvidia-uvm and /dev/nvidia-uvm-tools (nvidia_uvm), and the two MIG capability nodes under
 * /dev/nvidia-caps that NVIDIA's programs create. core and uvm are the nvidia-modprobe commands
 * that create them, "" when nothing of theirs is missing; argv holds the same commands for
 * gs_host_run, each ended by NULL.
 */
#define GS_DEV_NODES_MAX 24
#define GS_NVIDIA_CTL_MINOR 255u
struct gs_dev_nodes {
	char missing[GS_DEV_NODES_MAX][48];
	unsigned int nmissing;
	char core[512], uvm[64];
	char minors[GS_DEV_NODES_MAX][8], caps[2][GS_PATH_MAX];
	const char *core_argv[2 * GS_DEV_NODES_MAX + 8], *uvm_argv[5];
};
void gs_dev_nodes_scan(const struct gs_host *h, struct gs_dev_nodes *n);
/* The missing nodes as "/dev/a, /dev/b" in buf. */
void gs_dev_nodes_list(const struct gs_dev_nodes *n, char *buf, size_t len);
/*
 * 0 loaded; -1 not loaded; -ETIMEDOUT when the outcome is unknown: the command did not finish, or it
 * failed and the module is loaded all the same. why says which.
 */
int gs_module_load(const struct gs_host *h, int force, const char *name, const char *const *opts, unsigned int nopts,
		   char *why, size_t len);
int gs_module_unload(const struct gs_host *h, const char *name, char *why, size_t len);
int gs_param_bool(const char *v);

/* Utilities shared by the command implementations. */
int gs_read_attr(const char *path, char *buf, size_t len);
int gs_read_hex(const char *path, unsigned int *out);
int gs_read_uint(const char *path, unsigned int *out);
/* Copy src into dst[len], truncating; dst is always terminated. */
void gs_copy(char *dst, size_t len, const char *src);
int gs_write_attr(const char *path, const char *value);
int gs_pathf(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int gs_exists(const char *path);
int gs_fsync_dir(const char *dir);
int gs_write_file_atomic(const char *path, const void *data, size_t len, mode_t mode);
int gs_mkdir_p(const char *path, mode_t mode);
int gs_read_whole(const char *path, char **data, size_t *len, size_t max);
/* fork + execv of argv[0] (absolute, or looked up in a fixed system PATH); no shell. */
int gs_run(const char *const argv[]);
/*
 * gs_run with its standard output in out (the first len - 1 bytes; the rest is read and dropped) and its
 * standard error discarded. A program still running GS_CAPTURE_TIMEOUT_MS after its start is killed
 * and never waited for beyond that, so a program stuck in the kernel cannot hold the caller. The exit
 * status, or -1 when it cannot run or ran out of time (out is then empty).
 */
#define GS_CAPTURE_TIMEOUT_MS 5000u
int gs_run_capture(const char *const argv[], char *out, size_t len);
int gs_find_program(const char *name, char *out, size_t len);
void gs_sleep_ms(unsigned int ms);
int gs_boot_id(char *buf, size_t len);
int gs_validate_abs_path(const char *p);
int gs_lock(void);

/* SHA-256 of a file as 64 lowercase hex characters. Strict: path must be a regular file, never a
 * symbolic link (-ELOOP); the install manifest's take-over and removal checks rely on that. */
int gs_sha256_file(const char *path, char hex[65]);
/* The same, after resolving path once with realpath() into real (the vGPU plugin is a link). */
int gs_sha256_file_resolved(const char *path, char hex[65], char real[GS_PATH_MAX]);
void gs_sha256_buf(const void *data, size_t len, char hex[65]);

/* Activation planning and daemon admission. Unknown inspection never means no clients. */
struct gs_display_plan {
	int inspection_unknown;
	int manager_active;
	char manager_unit[256];
	char reason[1024];
};
int gs_display_plan_build(const struct gs_host *, const struct gs_config *, struct gs_display_plan *);
int gs_daemon_ready(const struct gs_host *, const struct gs_config *, const struct gs_activate_opts *);
int gs_resource_fit_assess(const struct gs_host *, const struct gs_card *, unsigned int, struct gs_resource_fit *);

/* The early boot unit: load the livepatch, and with --load-driver nvidia right after it; removal,
 * rescan and deferral belong to activate. */
struct gs_livepatch_opts {
	int boot;                   /* --boot: the early boot unit's mode (the command's only mode) */
	int load_driver;            /* --load-driver: after the livepatch is verified, modprobe nvidia */
	const char *config_path;    /* --config PATH; NULL: the default rules (see gs_config_load) */
	const char *sysroot;        /* fixture sysfs root; NULL: the live host's */
	const struct gs_host *host; /* fixture host; NULL: gs_host_live */
};
int gs_cmd_livepatch(const struct gs_livepatch_opts *o);

/* ---- setup, install and the boot units (owner: V030SetupInstall) ----------------- */
/* The supported VM autostart units, NULL-terminated. Unrelated hypervisors are never touched. */
extern const char *const gs_vm_autostart_units[];
/* The 0.2.x artifacts migration removes; nothing in the current flow uses them. */
#define GS_LEGACY_UNIT_PREPARE "geforce-sriov-prepare.service"
#define GS_LEGACY_UNIT_WANTS GS_UNIT_DIR "/sysinit.target.wants/" GS_LEGACY_UNIT_PREPARE
#define GS_LEGACY_HOLD "/etc/modprobe.d/nvidia-manual-load.conf"
#define GS_LEGACY_DEFERRED GS_RUN_DIR "/deferred"
/* The x.y.z version the selected input names, if its name states one: 0, or -1 when it states none. */
int gs_install_version_named(const struct gs_install_opts *o, char *out, size_t len);
/*
 * 1 when the build needs the display modules, with in why the line that says why. They are built
 * only for a selected boot display GPU, with display=auto and with display=on alike, because the
 * host console is never put on another card. sys NULL (a staged tree) reads no card: display=on
 * then stands, and the target host judges it. 0 otherwise. why takes 256 bytes.
 */
int gs_display_effective(const char *sys, const struct gs_config *cfg, char *why, size_t len);
/* 1 when the selected package or tree names NVIDIA's open flavour, which this build cannot use. */
int gs_nvidia_open_flavour(const struct gs_install_opts *o);

/*
 * ---- V030Diag: loaded-patch verification (src/lifecycle.c) ----
 * The display patch as it is loaded, read from procroot/kallsyms and procroot/kcore at the
 * function-relative offset the build record holds (idx39_function=, idx39_function_offset=).
 * UNKNOWN is never a pass. procroot may be a fixture directory in tests.
 */
enum gs_loaded_patch { GS_LOADED_MATCH, GS_LOADED_MISMATCH, GS_LOADED_UNKNOWN };
struct gs_loaded_patch_result {
	enum gs_loaded_patch state;
	char function[128];          /* the record's idx39_function */
	unsigned long long offset;   /* its idx39_function_offset */
	unsigned char found[8];      /* the bytes read, when they could be read */
	size_t found_len;
	char why[256];               /* why it is not a MATCH */
};
int gs_loaded_patch_verify(const char *procroot, const struct gs_build *b, struct gs_loaded_patch_result *out);

#endif
