/* SPDX-License-Identifier: MIT */
/*
 * geforce-sriov host CLI: configuration, sysfs inspection and lifecycle commands.
 *
 * Read-only commands (devices, status) accept a diagnostic sysfs root.
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
/* The link `systemctl enable` creates for it (WantedBy=multi-user.target); its first step loads nvidia. */
#define GS_UNIT_WANTS_ACTIVATE GS_UNIT_DIR "/multi-user.target.wants/" GS_UNIT_ACTIVATE
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
 * blacklist/install rules, so the alias itself is mapped away; the activate unit's first step
 * loads nvidia (livepatch --boot --load-driver, after udev's coldplug) and activation loads nvidia_drm. */
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
/*
 * The kernel hook's report: one file per kernel release whose automatic build failed, holding one
 * line with the cause; a successful `rebuild` for that release removes it. Each run's log is
 * GS_KERNEL_HOOK_LOGS/<release>.log, so one apt run for two kernels keeps both.
 */
#define GS_KERNEL_HOOK_FAILED GS_STATE_DIR "/kernel-hook-failed"
#define GS_KERNEL_HOOK_LOGS GS_STATE_DIR "/kernel-hook-logs"
/* Written by install when it replaces nvidia.ko: kernel=, module_sha256=, module_build_id= lines. */
#define GS_DRIVER_BUILD_NAME "driver-build"
#define GS_DRIVER_BUILD GS_STATE_DIR "/" GS_DRIVER_BUILD_NAME
/* Beside it: the record of each other kernel the driver step built for, one file named after the kernel. */
#define GS_DRIVER_BUILDS_NAME "driver-builds"
#define GS_NVIDIA_MODPROBE "/etc/modprobe.d/geforce-sriov-nvidia.conf"
/* Host CUDA: where the driver step puts NVIDIA's CUDA libraries for install, and the loader's file for their directory. */
#define GS_CUDA_STAGE GS_STATE_DIR "/host-cuda"
#define GS_CUDA_CONF "/etc/ld.so.conf.d/geforce-sriov-cuda.conf"
/* Host desktop: the same for NVIDIA's graphics libraries (the library directory is <prefix>/lib/geforce-sriov/gl). */
#define GS_GL_STAGE GS_STATE_DIR "/host-desktop"
#define GS_GL_CONF "/etc/ld.so.conf.d/geforce-sriov-gl.conf"
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

/* auto and on: KMS on every selected card the build allows; boot: KMS on the boot display GPU only; off: no display modules. */
enum gs_display_mode { GS_DISPLAY_AUTO, GS_DISPLAY_ON, GS_DISPLAY_OFF, GS_DISPLAY_BOOT };
/* host_desktop=: NVIDIA's graphics libraries for the host desktop. auto (what an absent key means) is on when the display build is. */
enum gs_host_desktop { GS_DESKTOP_AUTO, GS_DESKTOP_ON, GS_DESKTOP_OFF };
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
	int fb_slots_ignored;        /* the config had the removed fb_slots= line; it is read and dropped */
	int host_cuda;
	enum gs_host_desktop host_desktop;
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

/* An NVIDIA GPU function the configuration does not list (src/unselected.c). */
struct gs_unselected {
	struct gs_addr pf;
	unsigned int device;
	const struct gs_device *entry; /* table entry, or NULL */
	char driver[64];               /* "" when unbound */
	char override[64];             /* driver_override, "" when unset */
	int boot_vga;                  /* -1 unknown */
};
#define GS_UNSELECTED_MAX 32
/*
 * Every NVIDIA physical function of class 0300, 0302 or 0680 (the classes NVIDIA's driver matches)
 * whose address is not in sel, in address order, at most max. Reads sysroot only; returns how many.
 */
unsigned int gs_unselected_scan(const char *sysroot, const struct gs_addr *sel, unsigned int nsel,
				struct gs_unselected *out, unsigned int max);
/* One entry as status prints it (two lines), or as one journal line. */
void gs_unselected_print(const struct gs_unselected *u, int one_line);
/* The GPU's /proc node field "key: N" of the text at path (e.g. "Device Minor"); -1 when there is none. */
long gs_proc_field(const char *path, const char *key);
/* Reads sysroot (e.g. "/sys") only. Returns 0; card->present tells whether the PF exists. */
int gs_card_inspect(const char *sysroot, const struct gs_addr *pf, struct gs_card *card);
/*
 * What the installed build record says that the KMS rule needs: ada_placement and display are 1 or 0
 * as recorded, -1 when no record is read. A NULL gs_kms_build means nothing is judged.
 */
struct gs_kms_build {
	int ada_placement;
	int display;
};
struct gs_build_src;
/* The record src names (NULL src: the live record is not read: all -1). */
void gs_kms_build_read(const struct gs_build_src *src, struct gs_kms_build *kb);
/* 1 when activation hands the firmware console of this card to the display modules: display is not off and it is the boot display GPU. */
int gs_display_console(const struct gs_config *cfg, const struct gs_card *c);
/* 1 when activation puts KMS on this card: see the table in pci.c. kb may be NULL. */
int gs_display_kms(const struct gs_config *cfg, const struct gs_card *c, const struct gs_kms_build *kb);
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

/* Install-time selection check: the PF may legitimately be bound to nvidia or to no driver. */
int gs_check_selectable(const struct gs_card *card, char *reason, size_t len);

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
	char client_accept[32];       /* the version of the libraries nvidia.ko accepts besides its own (client_accept=); empty: none */
};

enum gs_version_package { GS_VERSION_HOST, GS_VERSION_COMPANION };
int gs_version_confirmed(enum gs_version_package package, const char *version);
/*
 * One confirmed host driver version and what belongs to it: the version of the Linux driver package,
 * the guest drivers to use on each generation, and the recorded digests the CLI judges files by.
 * The rows are the blocks of scripts/gs-driver-versions.tbl, the one place that says which
 * versions are confirmed and holds their data; the build makes src/gs_versions.inc from it, and
 * the driver step reads the same file. A new version is a new block. A message that names the
 * confirmed versions lists every row. A digest a row does not record is NULL.
 */
struct gs_confirmed_version {
	const char *host;           /* NVIDIA's vGPU host driver */
	const char *second;         /* the Linux driver package for display and host CUDA builds */
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
 * The sentence check and activate print for b: 0 when the module is the driver step's build,
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
	/* The feature selection and build inputs install would get, judged before it runs. */
	int display_set, host_cuda_set, host_cuda;
	int host_desktop_set, host_desktop; /* --host-desktop / --no-host-desktop */
	int skip_driver;           /* --driver=skip: install builds nothing, so no build input is needed */
	enum gs_display_mode display;
	const char *companion_file, *companion_source, *nvkms_source;
	const char *config_path;   /* NULL: default rules (see gs_diag_config) */
	struct gs_addr devices[GS_CFG_MAX_BDFS];
	unsigned int ndevices;
	int disable_d3cold;       /* install writes d3cold=off (needs a laptop GPU among those judged) */
	const char *nvidia_source; /* NVIDIA's kernel tree to check; NULL: gs_nvidia_source_default() */
	const char *driver_file;   /* install extracts NVIDIA's tree from this package */
	const struct gs_build_src *build; /* NULL: the live host's, skipped on a fixture root */
	const char *modprobe_dir; /* modprobe.d to read; NULL: /etc/modprobe.d, skipped on a fixture root */
	const char *plugin;       /* libnvidia-vgpu.so to check; NULL: NVIDIA's plugin link, skipped on a fixture root */
	const char *unit_dir;     /* unit directory whose vGPU daemon drop-ins to read; NULL: systemd's unit
				   * directories, skipped on a fixture root */
	const char *run_dir;      /* status: where GS_ACTIVATING_NAME is; NULL: GS_RUN_DIR, skipped on a fixture root */
	const char *kernel_hook_dir; /* status: where the kernel hook's failure files are; NULL: GS_KERNEL_HOOK_FAILED, skipped on a fixture root */
};
int gs_cmd_devices(void);
/*
 * The host checks (the old doctor), one function with two callers. GS_HOSTCHECK_PRE: what install would meet on
 * this host, printed with every [OK|INFO|WARN|FAIL] line and the failure count; `install --dry-run` and setup
 * run it. GS_HOSTCHECK_POST: the state of an installed host, printed only as INFO, WARN and FAIL lines, for
 * status to follow its own lines with. Returns GS_EXIT_REFUSED when a line is a FAIL, GS_EXIT_USAGE for
 * --disable-d3cold without a laptop GPU, else GS_EXIT_OK.
 */
enum gs_hostcheck_mode { GS_HOSTCHECK_PRE, GS_HOSTCHECK_POST };
int gs_hostcheck(const struct gs_diag_opts *opts, enum gs_hostcheck_mode mode);
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
/* 1 when one of the n addresses of sel is an Ada card by its device attribute below sys. */
int gs_fb_ada_selected(const char *sys, const struct gs_addr *sel, unsigned int n);
/*
 * The line status and check print for an Ada card with numvfs VFs enabled, read from the placement_id
 * file of each VF below sys (no nvidia-smi, which can hang). The line starts with "VM video memory:".
 * Every VF reads "Not Applicable": a VM takes free memory when it starts. A VF reads a number: fixed
 * slots, the loaded nvidia.ko does not carry the Ada placement change. A file that cannot be read, or
 * reads anything else: that is said. Told, never judged. Returns 1 for the first state, 0 for the others.
 */
int gs_ada_placement_line(const char *sys, const struct gs_addr *pf, unsigned int numvfs, char *line, size_t len);

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
	/* Open path (read and write) once, as a program would, and close it: 0, -errno, or -ETIMEDOUT after ms. NULL: -ENOSYS. */
	int (*open_node)(const struct gs_host *, const char *path, unsigned int ms);
};
extern const struct gs_host gs_host_live;
/*
 * Activation's work on the unselected GPUs, one journal line for each. A GPU that NVIDIA's driver bound
 * and cannot start is released: its node is opened once, and when that fails with EIO the card gets
 * driver_override GS_UNSELECTED_RELEASED and is unbound. Only a GPU outside the device table, bound to
 * nvidia, without a driver_override, that is not the boot display GPU, is tested. Nothing else is touched,
 * and the step never changes the exit status.
 */
#define GS_UNSELECTED_RELEASED "geforce-sriov-released"
#define GS_UNSELECTED_OPEN_MS 30000u
/* The step stops testing after this long; one card's tests run at most GS_UNSELECTED_CARD_MS more. */
#define GS_UNSELECTED_BUDGET_MS 120000u
#define GS_UNSELECTED_CARD_MS (GS_MODULE_DEADLINE_MS + GS_UNSELECTED_OPEN_MS + 5 * SYSFS_DEADLINE_MS)
void gs_unselected_release(const struct gs_host *h, const struct gs_addr *sel, unsigned int nsel);
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
 * Why activate refuses the laptop GPUs laptops (n of them) while the config has no d3cold=off: the text after
 * "refused, nothing changed: ", ending with the install command that keeps the selection sel.
 * Each line after the first starts with indent. buf takes GS_D3COLD_WHY_SIZE bytes.
 */
#define GS_D3COLD_WHY_SIZE 1536
void gs_d3cold_why(char *buf, size_t len, const struct gs_addr *laptops, unsigned int n, const struct gs_addr *sel,
		   unsigned int nsel, const char *indent);

/*
 * Ada and Ampere CPU-RM host stack (docs/driver-patch.md#revisions). activate: enable the VFs of a
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
 * ("not supported"), with the sentence check and activate print in line. */
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
	int host_desktop, host_desktop_set; /* --host-desktop is on, --no-host-desktop off */
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
	int dry_run;               /* install --dry-run: run the checks before install on this host, change nothing */
};
int gs_cmd_install(const struct gs_install_opts *opts);
/*
 * install from the installed copy with only selection options (--device, --disable-d3cold, --display,
 * --no-display): checks each address, runs the driver step as rebuild does when the display need no longer
 * matches the installed build, then writes the config and the selection record. It copies no files.
 * env is the host it acts on: tests pass a fixture; the real one is the live system.
 */
struct gs_select_env {
	const char *sys;       /* sysfs root */
	const char *destdir;   /* prefix of the config, the manifest and the state directory; "" for the live host */
	const struct gs_build_src *build; /* the installed build's record, as gs_build_identity takes it */
	const char *driver;    /* geforce-sriov-driver */
	const char *driver_log;
	const char *release;   /* the kernel the driver step builds for; NULL: the running one */
	int (*before_driver)(const char *release); /* run before the driver step; NULL: nothing */
};
int gs_install_select(const struct gs_install_opts *opts, const struct gs_select_env *env);
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
 * and the next step (the power cycle the first install needs, then status).
 */
/* The closing lines of an install that a boot has to pick up. from_setup: setup gives the instructions itself. */
void gs_install_next(const struct gs_config *cfg, int from_setup);
int gs_cmd_uninstall(int unload);
/* setup's questions: AUTO asks when standard input is a terminal, ON always (--interactive), OFF never (--no-prompt). */
enum gs_prompt { GS_PROMPT_AUTO, GS_PROMPT_ON, GS_PROMPT_OFF };
/*
 * setup: from the host's state, the next steps of a first install (install --dry-run, install, then activation
 * when it is safe) or of an installed host (install --dry-run, check, activate, check), each run as a child
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

/* Prerequisites of a live install; install refuses, and `install --dry-run` names them, with GS_PREREQ_MSG. */
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
 * another kernel: a copy of the Linux driver package and of the public source, and index, which names
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
/* The closing text of `rebuild` for the kernel `release` while `running` is the running kernel (a kernel that is not running needs no power cycle). */
void gs_rebuild_done_text(const char *release, const char *running, char *out, size_t len);
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
 * changed, read from its line `install_result=unchanged|changed`; without that line,
 * or with another value, it is GS_DRIVER_CHANGED. No message text is parsed.
 */
enum gs_driver_result {
	GS_DRIVER_CHANGED,     /* a module or the plugin copy was built or replaced, or the step did not say */
	GS_DRIVER_UNCHANGED,   /* the build was already installed and nothing was written */
};
int gs_driver_step(const char *const argv[], const char *log, char *why, size_t len, enum gs_driver_result *result);
/*
 * install's DKMS step for GS_DKMS_NAME/GS_VERSION, each command run by run (gs_run; tests record them).
 * registered: DKMS holds this version already (a re-install). The new tree is then first built in a scratch
 * DKMS tree below the directory scratch (`dkms --dkmstree`), which leaves the registered module and its
 * installed copies alone; only when that succeeds is it removed (`dkms remove --all`) and added, built and
 * installed for the running kernel again. When that fails after the removal, the boot units are disabled.
 * Another version of GS_DKMS_NAME is not handled: install refuses while one is installed
 * (gs_other_install). 0, or GS_EXIT_FAIL with what was removed and the way back printed.
 */
int gs_dkms_step(int (*run)(const char *const argv[]), int registered, const char *scratch);
/*
 * rebuild: the livepatch module of this version must be installed for the kernel NVIDIA's module is
 * built for. DKMS builds for a kernel only when that kernel's package is installed, so a kernel
 * that was on the host before geforce-sriov has none. managed: dkms is installed and holds this
 * version. Asks `dkms status` (capture) and, when the module is not installed for release, runs
 * `dkms install -k release` (run). GS_EXIT_OK, or GS_EXIT_FAIL when that install failed; named is
 * the kernel the user named, for the command the message repeats.
 */
/* ---- NVIDIA's libraries from the Linux driver package: host CUDA and host desktop (src/nvlibs.c) ---- */

/* How the commands that install them are run: NULL members on a staged install, fixtures in the tests. */
struct gs_nvlibs_io {
	int (*run)(const char *const argv[]);
	int (*capture)(const char *const argv[], char *out, size_t len);
};
/* The two sets; what a set is lives in nvlibs.c. */
struct gs_nvset;
extern const struct gs_nvset gs_nvset_cuda, gs_nvset_desktop;
/*
 * Where a set goes: its library directory, the loader's file naming it, and the driver step's staging directory.
 * accept: the version of the programs the installed nvidia.ko accepts (its own, or the client_accept of the
 * build); the desktop set is not installed from a stage of another version. NULL or "": not known, not checked.
 */
struct gs_nvdest {
	const char *dir, *conf, *stage, *accept;
};
/* The small files of the desktop set, in the directories NVIDIA's installer uses; the fixtures point them into a temporary tree. */
struct gs_nvgl_paths {
	char etc_vendor[GS_PATH_MAX], etc_platform[GS_PATH_MAX], etc_vulkan[GS_PATH_MAX], share_nvidia[GS_PATH_MAX], xconf[GS_PATH_MAX];
	/* Where another NVIDIA graphics driver would have put its files: install does not mix with them. */
	char share_vendor[GS_PATH_MAX], share_platform[GS_PATH_MAX], xorg_drivers[GS_PATH_MAX];
};
extern struct gs_nvgl_paths gs_nvgl;
/* want: 0 the set is off, GS_NVLIBS_ON it is asked for, GS_NVLIBS_AUTO it is on when its inputs are there. */
#define GS_NVLIBS_ON 1
#define GS_NVLIBS_AUTO 2
/*
 * Brings the set in line with want, on the install manifest at path manifest: installs it from the
 * staging directory when the driver step left one there, removes it when want is 0. 0, or a negative
 * errno when the manifest cannot be read or written.
 */
int gs_nvlibs_apply(const struct gs_nvset *set, const char *manifest, const struct gs_nvdest *d, int want,
		    const struct gs_nvlibs_io *io);
/*
 * What the manifest lists of a set: 1 complete, 0 nothing, -1 a file is missing or changed (bad).
 * dir and version are those of its first library (libcuda, libEGL_nvidia).
 */
int gs_nvlibs_state(const struct gs_nvset *set, const char *manifest, char *dir, size_t dlen, char *version, size_t vlen,
		    char *bad, size_t blen);
/* Which library the loader takes, from `ldconfig -p`: 1 the one in dir, 0 another (found), -1 not known. */
int gs_nvlibs_loader(const struct gs_nvset *set, int (*capture)(const char *const argv[], char *out, size_t len), const char *dir,
		     char *found, size_t len);
/*
 * The sentence status and the host checks print after "host CUDA libraries: " or "host desktop libraries: ". 1 in
 * place, -1 not (a warning), 2 installed although the feature is off, 0 nothing to say. capture NULL:
 * the loader is not asked.
 */
int gs_nvlibs_line(const struct gs_nvset *set, const char *manifest, int want,
		   int (*capture)(const char *const argv[], char *out, size_t len), char *line, size_t len);
/* "auto", "on" or "off". */
const char *gs_host_desktop_name(enum gs_host_desktop m);
/*
 * The display server of the host, from the processes below proc (an X server or a compositor), and whether its
 * memory map holds NVIDIA's graphics libraries: 1 it draws with NVIDIA's driver, 0 it does not, -1 no display
 * server is running, -2 one is running and its memory map cannot be read (another user's: only root can tell).
 */
int gs_desktop_servers(const char *proc, char *line, size_t len);
/*
 * The host desktop verdict of the host checks from gs_desktop_servers' answer sv for server, and whether the running nvidia module
 * is stale (loaded: the sentence that says so). Returns 0 nothing to say, 1 INFO, 2 WARN, 3 FAIL, with line set.
 */
int gs_desktop_draw_line(int sv, const char *server, int stale, const char *loaded, char *line, size_t len);

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
	char ari[8], msix[8], vfs[8], bdfs[GS_BDF_LIST_STR_SIZE];
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

/*
 * The selection record: what install saw at each selected address, so that a card that is not the one
 * that was selected is told from it before anything is written to it. One line per card in
 * GS_STATE_DIR/selection: "address vendor:device subsystem_vendor:subsystem_device". Only install
 * writes it. An address without a line (a config edited by hand after install) is judged as before; status says so once.
 */
#define GS_SELECTION_NAME "selection"
#define GS_SELECTION GS_STATE_DIR "/" GS_SELECTION_NAME
struct gs_ident {
	struct gs_addr pf;
	unsigned int vendor, device, subvendor, subdevice;
};
/* The record in state_dir: 0; -ENOENT when there is none; -EINVAL when it cannot be parsed. */
int gs_selection_read(const char *state_dir, struct gs_ident *out, unsigned int *n);
/* The record's text: its length, or -1 when buf is too small. */
int gs_selection_render(const struct gs_ident *ids, unsigned int n, char *buf, size_t len);
/*
 * The identities to record for the n addresses: refresh 1 reads each card now; refresh 0 keeps the line
 * old already has for an address and reads only the others. A card that is absent or not in the device
 * table gets no line. Returns how many lines are in out.
 */
unsigned int gs_selection_collect(const char *sys, const struct gs_addr *bdfs, unsigned int n,
				  const struct gs_ident *old, unsigned int nold, int refresh, struct gs_ident *out);
/*
 * Buffer sizes of the selection texts, all from GS_MAX_PFS so that a host with the most cards the
 * module takes is told its whole command: GS_SEL_ARGS_MAX holds the options of one selection
 * (" --device <BDF>" for every card, --disable-d3cold, room for one more option), GS_SEL_CMD_MAX the
 * command with them, GS_SEL_FORMS_MAX the two forms gs_display_forms prints (the options appear in each),
 * GS_SEL_LINE_MAX a status line that carries the command, GS_SEL_MSG_MAX the verdict (a block of lines for
 * every card that is not the one selected, then the command or the forms).
 */
#define GS_SEL_ARGS_MAX (GS_MAX_PFS * (sizeof(" --device ") - 1 + GS_BDF_STR_SIZE) + 64)
#define GS_SEL_CMD_MAX (GS_SEL_ARGS_MAX + 64)
#define GS_SEL_FORMS_MAX (2 * GS_SEL_ARGS_MAX + 512)
#define GS_SEL_LINE_MAX (GS_SEL_CMD_MAX + 512)
#define GS_SEL_MSG_MAX (GS_MAX_PFS * 640 + GS_SEL_FORMS_MAX + GS_SEL_CMD_MAX + 1024)
/* "sudo geforce-sriov install --device A --device B" plus --disable-d3cold, then extra. */
void gs_selection_command(const struct gs_addr *list, unsigned int n, int d3cold, const char *extra, char *out,
			  size_t len);
/*
 * Compare the card at each selected address (only those of o->devices when o names some) with its record.
 * 0 when every recorded card is the card there, or nothing is recorded; 1 with msg set (the lines after
 * "refused, nothing changed: ") when one is not, naming the command that selects the cards that are there.
 */
int gs_selection_verdict(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o,
			 char *msg, size_t len);
/* gs_selection_verdict, printed as a refusal on standard error: 0 or GS_EXIT_REFUSED. */
int gs_selection_verify(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o);
/* 1 with line set when some selected address has no recorded identity yet (status says it once). */
int gs_selection_unrecorded(const char *state_dir, const struct gs_config *cfg, char *line, size_t len);
/*
 * When the inputs for the display modules (the Linux driver package and public kernel source) were not
 * kept, the two forms of the command that give a selection a display build: 1 with text set (selection_args
 * are the --device options to repeat, or ""). 0 when they were kept, which makes the plain command enough.
 */
int gs_display_forms(const struct gs_host *h, const char *selection_args, char *text, size_t len);
/*
 * 1 when the installed build (src, as gs_build_identity reads it; sys is the sysfs root) records no display
 * modules and the selection cfg has a boot display card that needs them.
 */
int gs_display_lacking(const struct gs_build_src *src, const char *sys, const struct gs_config *cfg);
const char *gs_plugin_link(void);

/* ---- NVIDIA's kernel modules (src/modules.c) --------------------------------- */

int gs_udev_settle(const struct gs_host *h);
/* The boot step waits for udev's event queue this long (seconds, text and milliseconds) before nvidia loads. */
#define GS_UDEV_SETTLE_S "30"
#define GS_UDEV_SETTLE_MS 35000u
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
#define GS_DEV_NODES_MAX (GS_MAX_PFS + 8) /* nvidiactl, a node per GPU, modeset, two caps, two uvm */
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
/*
 * Tell systemd that the step about to run may take up to ms (plus GS_NOTIFY_MARGIN_MS): sends
 * EXTEND_TIMEOUT_USEC to $NOTIFY_SOCKET. Does nothing without the variable (every run outside the
 * activation unit) and never fails.
 */
#define GS_NOTIFY_MARGIN_MS 30000u
void gs_notify_extend(unsigned int ms);
/*
 * The limits of activation's own waits and writes (src/activate.c), kept here because the start
 * timeout install writes into geforce-sriov-activate.service is built from them.
 */
#define POLL_MS 50u
#define LIVEPATCH_WAIT_MS 10000u
#define VF_BIND_WAIT_MS 30000u
#define MGR_ACTIVE_WAIT_MS 30000u
#define MGR_SETTLE_MS 2000u
#define PF_REBIND_WAIT_MS 10000u
#define VF_ACTIVE_WAIT_MS 5000u
#define VF_TYPES_WAIT_MS 15000u
#define REMOVE_WAIT_MS 5000u
#define RETURN_WAIT_MS 15000u
#define SRIOV_ATTR_WAIT_MS 5000u
#define FN_RETURN_WAIT_MS 5000u
#define BRIDGE_WAKE_MS 5000u
#define PF_WAKE_MS 5000u
#define DM_ACTIVE_WAIT_MS 10000u
#define SYSFS_DEADLINE_MS 10000u
#define RELEASE_WAIT_MS 5000u

/*
 * The start timeout install writes into geforce-sriov-activate.service (TimeoutStartSec): an upper
 * bound of activation with every wait, write and process at its own limit, so that activation ends
 * through its own limits before systemd ends it, even on a systemd that ignores the extension above.
 * Normal activation takes seconds per card; the bound matters only in a real hang.
 * Scope: the polling loops end on CLOCK_MONOTONIC time, so the cost of one iteration does not extend
 * them beyond one final iteration (sysfs reads; `systemctl is-active`, which has a GS_CAPTURE_TIMEOUT_MS
 * deadline). The sum counts that final `is-active` once.
 *
 * Per card (every branch counted; the functions are in src/activate.c):
 *   sysfs writes of SYSFS_DEADLINE_MS, GS_ACT_CARD_WRITES: bridge_hold 1, wake_card 2, unwake 3,
 *     handoff_fb 1, remove_lock 2, audio unbind and rebind 2 x (GS_SLOT_FNS - 1), remove of every
 *     function of the slot GS_SLOT_FNS, of the PF 1, rescan 1, d3cold 1, laptop_hold 3, re-probe 6
 *   waits: BRIDGE_WAKE, PF_WAKE, RELEASE, REMOVE x (GS_SLOT_FNS + 1), RETURN, SRIOV_ATTR,
 *     FN_RETURN x GS_SLOT_FNS, PF_REBIND x 2, VF_BIND, the boot bind wait
 *   one sriov-manage run of GS_MODULE_DEADLINE_MS
 * Per VF: two writes (type, power), the VF_ACTIVE wait and two VF_TYPES waits.
 * Once per run: GS_ACT_MODULE_RUNS modprobe, rmmod, systemctl and nvidia-modprobe runs of
 *   GS_MODULE_DEADLINE_MS (device_nodes() runs up to 2 nvidia-modprobe commands and is called twice on a path: before
 *   the host CUDA load and at the end), the livepatch and display-manager waits, the manager wait with its last
 *   `is-active` call (GS_CAPTURE_TIMEOUT_MS), the settle pause and one nvidia-smi capture.
 * The unit's first step (`livepatch --boot --load-driver`, GS_ACTIVATE_PRESTEP_MS) is counted too: two udev waits, the
 *   livepatch wait, two module loads (the livepatch and nvidia), and the release of unselected GPUs, which activation runs
 *   after its last load of nvidia: GS_UNSELECTED_BUDGET_MS plus the longest test of one card, GS_UNSELECTED_CARD_MS
 *   (nvidia-modprobe, one open, 5 writes).
 *   systemd arms the full start timeout for each
 *   command, so the term keeps the value an upper bound of the whole start however the commands are counted.
 * tests/test_lifecycle.c (test_start_timeout) fails when a limit or a call count here no longer
 * matches src/activate.c.
 */
#define GS_ACT_CARD_WRITES (1 + 2 + 3 + 1 + 2 + 2 * (GS_SLOT_FNS - 1) + GS_SLOT_FNS + 1 + 1 + 1 + 3 + 6)
#define GS_ACT_MODULE_RUNS 29
#define GS_ACTIVATE_CARD_MS \
	(GS_ACT_CARD_WRITES * SYSFS_DEADLINE_MS + BRIDGE_WAKE_MS + PF_WAKE_MS + RELEASE_WAIT_MS + \
	 REMOVE_WAIT_MS * (GS_SLOT_FNS + 1) + RETURN_WAIT_MS + SRIOV_ATTR_WAIT_MS + FN_RETURN_WAIT_MS * GS_SLOT_FNS + \
	 PF_REBIND_WAIT_MS * 2 + VF_BIND_WAIT_MS + GS_BOOT_BIND_WAIT_MS + GS_MODULE_DEADLINE_MS)
#define GS_ACTIVATE_VF_MS (2 * SYSFS_DEADLINE_MS + VF_ACTIVE_WAIT_MS + 2 * VF_TYPES_WAIT_MS)
#define GS_ACTIVATE_PRESTEP_MS \
	(2 * GS_UDEV_SETTLE_MS + LIVEPATCH_WAIT_MS + 2 * GS_MODULE_DEADLINE_MS + GS_UNSELECTED_BUDGET_MS + GS_UNSELECTED_CARD_MS)
#define GS_ACTIVATE_BASE_MS \
	(GS_ACT_MODULE_RUNS * GS_MODULE_DEADLINE_MS + LIVEPATCH_WAIT_MS + DM_ACTIVE_WAIT_MS + MGR_ACTIVE_WAIT_MS + \
	 GS_CAPTURE_TIMEOUT_MS + MGR_SETTLE_MS + GS_CAPTURE_TIMEOUT_MS + GS_ACTIVATE_PRESTEP_MS)
/* 10 % margin on the sum, and the floor of the previous releases' order of magnitude. */
#define GS_ACTIVATE_START_MIN_S 900u
unsigned int gs_activate_start_timeout_s(unsigned int ncards, unsigned int nvfs);
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

/* The boot step: load the livepatch; with --load-driver (the activate unit's first step) wait for udev and
 * load nvidia; removal, rescan and deferral belong to activate. */
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
/*
 * The stamp install writes (one line: GS_VERSION) and a successful uninstall removes. A host is changed only
 * by the version that installed it: any other version is uninstalled first, then the machine is power cycled.
 */
#define GS_FORMAT_NAME "format"
#define GS_FORMAT_PATH GS_STATE_DIR "/" GS_FORMAT_NAME
/* The early unit of 0.2.x, whose enablement link is one sign of an older install. */
#define GS_LEGACY_UNIT_PREPARE "geforce-sriov-prepare.service"
/*
 * 1 when a geforce-sriov of another version than this one is installed below root ("" on a live host), with the
 * signs found in why; 0 when there is none, or when the stamp names this version. A sign is something the uninstall
 * of any version removes: a build record, the installed program at bindir, the DKMS registration of the livepatch
 * module, an enabled boot unit. Never a file an uninstall keeps on purpose (the config, manifest entries of
 * kept files, the activation lock). A build record is a sign only while the module it names is installed with
 * its recorded digest (a restore would still have something to do). fix: the commands that clear the signs by hand
 * when the installed program is gone ("" when it is there: its uninstall does that). Reads only; never writes.
 */
int gs_other_install(const char *root, const char *bindir, char *why, size_t len, char *fix, size_t fixlen);
/* The refusal install and setup print for it, to stderr. */
void gs_other_install_refusal(const char *root, const char *why, const char *fix);
/* uninstall: removes the stamp in state_dir (and the directory, when empty) only when rc is GS_EXIT_OK. */
void gs_format_release(const char *state_dir, int rc);
/* The x.y.z version the selected input names, if its name states one: 0, or -1 when it states none. */
int gs_install_version_named(const struct gs_install_opts *o, char *out, size_t len);
/*
 * 1 when the build needs the display modules, with in why the line that says why. They are built
 * for a selected boot display GPU (the console), and for another selected card that has KMS
 * (gs_display_kms()) only when inputs is 1, that is when the Linux guest driver package and the
 * module source are given or kept. sys NULL (a staged tree) reads no card: display=on then
 * stands, and the target host judges it. 0 otherwise. why takes 256 bytes.
 */
int gs_display_effective(const char *sys, const struct gs_config *cfg, int inputs, char *why, size_t len);
/*
 * 1 when both inputs of the display build are given or kept: the guest driver package (companion_file or
 * companion_source) and the module source (nvkms_source). dir is the index of kept inputs; NULL: none is kept.
 */
int gs_display_inputs(const char *dir, const char *companion_file, const char *companion_source,
		      const char *nvkms_source);
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
