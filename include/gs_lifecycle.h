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
#define GS_UNIT_PREPARE "geforce-sriov-prepare.service"
#define GS_UNIT_DIR "/etc/systemd/system"
/* The link `systemctl enable` creates for the boot unit (WantedBy=sysinit.target). */
#define GS_UNIT_WANTS GS_UNIT_DIR "/sysinit.target.wants/" GS_UNIT_PREPARE
#define GS_UNIT_ACTIVATE "geforce-sriov-activate.service"
/* The name of the package's drop-in in nvidia-vgpud.service.d, which preloads the identity hook. */
#define GS_VGPUD_DROPIN_NAME "geforce-sriov.conf"

#define GS_CONFIG_PATH "/etc/geforce-sriov/config"
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
#define GS_NVIDIA_VERSION "580.159.01"
#define GS_NVIDIA_SRC "/usr/src/nvidia-" GS_NVIDIA_VERSION
#define GS_NVIDIA_MODPROBE "/etc/modprobe.d/geforce-sriov-nvidia.conf"
/* The hold that keeps nvidia from loading at boot on a host with a console-release grant; install --hold-nvidia writes it. */
#define GS_NVIDIA_HOLD "/etc/modprobe.d/nvidia-manual-load.conf"
#define GS_NVIDIA_HOLD_TEXT "blacklist nvidia\ninstall nvidia /bin/false\n"
#define GS_RUN_DIR "/run/geforce-sriov"
#define GS_PREPARED_NAME "prepared"
#define GS_PREPARED_DIR GS_RUN_DIR "/" GS_PREPARED_NAME
/* This boot's deferral by the boot unit; nvidia-vgpud's drop-in asserts that it is absent. */
#define GS_DEFERRED_NAME "deferred"
#define GS_DEFERRED GS_RUN_DIR "/" GS_DEFERRED_NAME
#define GS_LOCK_PATH GS_RUN_DIR "/lock"
#define GS_LIVE_SYSFS "/sys"
#define GS_LIVE_PROC "/proc"
/* The only firmware framebuffer kind that `prepare --release-console` releases. */
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
	/* Boot display PFs whose console `prepare --release-console` may release; a subset of bdfs. */
	struct gs_addr boot_display_release[GS_CFG_MAX_BDFS];
	unsigned int nrelease;
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
/* 1 when boot_display_release lists pf. */
int gs_config_grants_release(const struct gs_config *cfg, const char *pf);
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

enum gs_verdict { GS_PLAN_READY, GS_PLAN_REFUSE };

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

#define GS_PLAN_MAX_UNBIND 8
struct gs_plan {
	enum gs_verdict verdict;
	char reason[768];
	struct gs_addr unbind[GS_PLAN_MAX_UNBIND]; /* audio functions of this card to unbind */
	unsigned int nunbind;
	char release[64];         /* firmware framebuffer to unbind first; "" when none is bound */
	char release_driver[64];  /* its driver */
};

/*
 * Pure decision over an inspected card: may `prepare` remove and rescan it? display is NULL
 * unless the config grants a console release for the card; a boot display PF is refused without it.
 */
void gs_plan_prepare(const struct gs_card *card, const struct gs_display *display, struct gs_plan *plan);
/*
 * Install-time selection check (the PF may legitimately be bound while installing). A boot display
 * PF is accepted only with release_granted.
 */
int gs_check_selectable(const struct gs_card *card, int release_granted, char *reason, size_t len);

/* GS_RUN_DIR/prepared/<pf>, written by prepare for each card it prepared this boot. */
struct gs_prepared {
	int present;
	char boot_id[64];
	char console_release[64]; /* firmware framebuffer released for this card, "none", or "" */
	int msix_linked;          /* MSI-X linked when prepare finished: 1, 0, or -1 unknown (no key, as 0.2.0 wrote) */
};
int gs_prepared_read(const char *run_dir, const struct gs_addr *pf, struct gs_prepared *rec);
/*
 * May activate act on this card: prepared in the boot boot_id names, and, for a boot display PF or
 * one whose console was released, released this boot with the config's grant? 0 or -1 with why.
 */
int gs_check_enable(const struct gs_card *c, const struct gs_config *cfg, const struct gs_prepared *rec,
		    const char *boot_id, char *why, size_t len);

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
};
/* The record file, the kernel release and the nvidia.ko for it ("" or NULL: none found). Reads files only. */
enum gs_build_state gs_driver_build_read(const char *record, const char *release, const char *module,
					 struct gs_build *out);
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
	const char *config_path;   /* NULL: default rules (see gs_cmd_doctor) */
	struct gs_addr devices[GS_CFG_MAX_BDFS];
	unsigned int ndevices;
	/* doctor --boot-display-release, each also a --device: judged as granted, as install writes it */
	struct gs_addr release[GS_CFG_MAX_BDFS];
	unsigned int nrelease;
	int hold_nvidia;          /* doctor --hold-nvidia: install writes the hold (needs a console release, planned or in the config) */
	int disable_d3cold;       /* doctor --disable-d3cold: install writes d3cold=off (needs a laptop GPU among those judged) */
	const char *nvidia_source; /* doctor: NVIDIA's kernel tree to check; NULL: GS_NVIDIA_SRC */
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
 */
struct gs_host {
	const char *sys;       /* sysfs root */
	const char *proc;      /* procfs root */
	const char *state_dir; /* the guard GS_ATTEMPT_NAME */
	const char *run_dir;   /* this boot: GS_PREPARED_NAME records, GS_DEFERRED_NAME */
	int (*write_attr)(const char *path, const char *value);
	int (*run)(const char *const argv[]);
	/* argv run with its standard output kept, as gs_run_capture; NULL: nothing is run (tests) */
	int (*capture)(const char *const argv[], char *out, size_t len);
};
extern const struct gs_host gs_host_live;

/* Mutating commands: live system only. */
struct gs_overrides {
	int ari, msix; /* GS_SW_* */
};
struct gs_prepare_opts {
	int boot; /* run by the boot unit: with boot_display_release set, defer and change nothing */
	struct gs_addr release[GS_CFG_MAX_BDFS]; /* --release-console: must equal boot_display_release */
	unsigned int nrelease;
};
int gs_cmd_prepare(const struct gs_overrides *ov, const struct gs_prepare_opts *po, int clear_guard);
/* prepare after the root check, lock and config load (cfg with the overrides applied). */
int gs_prepare(const struct gs_host *h, struct gs_config *cfg, const struct gs_prepare_opts *po);

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
};
#define GS_BOOT_BIND_WAIT_MS 30000u
/* --type and type_ada=/type_ampere= value: decimal 1..GS_VGPU_TYPE_MAX without a leading zero. 0 or -1. */
int gs_parse_vgpu_type(const char *value, unsigned int *out);
/*
 * Verdict on the plugin's SHA-256 sha: 0 for the patched plugin copy, else -1 with why naming
 * path and real (its resolved path). Pure, so tests can feed it the real digests, which no test file has.
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
	unsigned int ndevices;
	struct gs_addr release[GS_CFG_MAX_BDFS]; /* --boot-display-release, each also a --device */
	unsigned int nrelease;
	struct gs_overrides ov;
	unsigned int vfs;          /* --vfs N; 0: not given */
	int skip_driver;           /* --driver=skip: no NVIDIA module or plugin work */
	const char *nvidia_source; /* --nvidia-source DIR: NVIDIA's extracted package; NULL: GS_NVIDIA_SRC */
	const char *driver_file;   /* --driver-file PATH: NVIDIA's .run package (absolute, resolved) */
	int accept_nvidia_license; /* --accept-nvidia-license: run NVIDIA's installer from driver_file */
	int release_note_shown;    /* --release-note-shown: setup explained the console-release grant at its question */
	int hold_nvidia;           /* --hold-nvidia: write GS_NVIDIA_HOLD (needs a console-release grant) */
	int hold_note_shown;       /* --hold-note-shown: setup prints the hold notice itself after install */
	int disable_d3cold;        /* --disable-d3cold: write d3cold=off (needs a laptop GPU selected) */
};
int gs_cmd_install(const struct gs_install_opts *opts);
/*
 * The framed notice that nvidia must not load at boot while a boot display GPU is granted a console
 * release: what the release does, the file GS_NVIDIA_HOLD and its two lines, and how to revert it.
 * by_hand: the file is not created for the user, so it is reverted by deleting it, not by uninstall.
 * In red when gs_color(f).
 */
void gs_hold_notice(FILE *f, int by_hand);
/* gs_hold_notice in red (ANSI escapes before and after) when color, else plain. */
void gs_hold_notice_color(FILE *f, int by_hand, int color);
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
 * The end of a live install that enabled the boot units: the units, the console-release hold and
 * commands when cfg grants a release (the file that holds nvidia back when the modprobe.d directory
 * modprobe_dir has one, else gs_hold_notice unless hold_shown: setup prints it after install), then
 * the next step (doctor, the power cycle, what to run after it).
 */
void gs_install_next(const char *bindir, const struct gs_config *cfg, const char *modprobe_dir, int hold_shown);
int gs_cmd_uninstall(int unload);
/* setup's questions: AUTO asks when standard input is a terminal, ON always (--interactive), OFF never (--no-prompt). */
enum gs_prompt { GS_PROMPT_AUTO, GS_PROMPT_ON, GS_PROMPT_OFF };
/*
 * setup: from the host's state, the next steps of a first install (doctor, install) or of a
 * prepared boot (doctor, check, prepare when needed, activate, check), each run as a child
 * process of this binary; ends with the guest drivers of each selected card. o carries --device,
 * --boot-display-release, --vfs, --driver=skip, --nvidia-source, --driver-file,
 * --accept-nvidia-license and --hold-nvidia, which only a first install takes (an installed host notes
 * and ignores them); on an installed host --device must name exactly the configured GPUs. A first
 * install that grants a console release, with no hold of nvidia in modprobe.d, asks after
 * gs_hold_notice whether install creates GS_NVIDIA_HOLD (--hold-nvidia); without a yes or that option,
 * it prints gs_hold_notice for a file created by hand after install. When the config grants
 * a console release, check is not ready and NVIDIA's driver is not loaded, it runs the commands of
 * gs_release_steps after a yes; without a question it refuses with them (GS_EXIT_REFUSED). With NVIDIA's
 * driver loaded it refuses before prepare (GS_EXIT_REFUSED): the console card cannot be released that boot.
 * When every GPU was prepared this boot (the console released) but NVIDIA's driver is not loaded, it runs
 * the commands after prepare (modprobe, activate, check) without asking again.
 * When check is not ready and the loaded nvidia module is not the installed build, it refuses before
 * prepare and activate (GS_EXIT_REFUSED).
 * A selected laptop GPU without D3cold consent (d3cold=off in the config, or --disable-d3cold): after the
 * other questions, gs_d3cold_notice and the question whether install writes d3cold=off. Yes passes
 * --disable-d3cold to doctor and install (on an installed host, `install --driver=skip --disable-d3cold`
 * ends the run); no, or no question, refuses before any step (GS_EXIT_REFUSED). --disable-d3cold without
 * a selected laptop GPU is GS_EXIT_USAGE.
 */
int gs_cmd_setup(const struct gs_install_opts *o, enum gs_prompt prompt);
/*
 * setup with the host it reads (a fixture in tests): h->sys lists the cards, h->run runs each step.
 * in is the descriptor the answers come from, -1 to ask nothing; driver_present: NVIDIA's kernel tree
 * or vGPU userspace is on the host; config: the config file of an installed host, NULL for
 * gs_config_path() (the steps read GS_CONFIG_PATH, so another path set in the environment refuses);
 * build: the driver build sources (NULL: the live host's, skipped on a fixture host); modprobe_dir:
 * where a hold of nvidia is looked for (NULL: /etc/modprobe.d).
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
 * (other kernels) are named with the command that puts each back. A hold of nvidia in the modprobe.d
 * directory modprobe_dir is named either way, unless the manifest lists it (install --hold-nvidia wrote
 * it, and it is removed with the other files or kept with them). GS_EXIT_OK or GS_EXIT_FAIL.
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
 * the last 40 lines of log and its path.
 */
int gs_driver_step(const char *const argv[], const char *log, char *why, size_t len);
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

/* What leads gs_release_steps wherever it lists what to run after each boot. */
#define GS_RELEASE_STEPS_LEAD "After each boot, as root, in this order:"
/*
 * The commands that bring the GPUs up after a boot on a host whose config grants a console release,
 * one per line after indent: prepare with the config's --release-console consent, modprobe
 * --ignore-install nvidia, activate, check. or_setup adds the line naming setup, which runs them.
 */
void gs_release_steps(FILE *f, const char *indent, const struct gs_config *cfg, int or_setup);

/* The consent text that doctor and prepare show for a console release; install says instead that it only records the grant. */
#define GS_CONSOLE_RELEASE_NOTE \
	"Console release: the Linux console on that GPU stays detached until the next boot, and the monitor\n" \
	"may keep the last frame or lose signal. Logs continue in the journal and over SSH. See\n" \
	"docs/installation.md#what-the-opt-in-changes."

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

#endif
