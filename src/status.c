// SPDX-License-Identifier: MIT
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
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gs_hook.h"
#include "gs_lifecycle.h"
#include "gs_internal.h"

/* ---- status ------------------------------------------------------------- */

/* One VF as check defines readiness: on nvidia, power/control on, runtime active, the expected vGPU type. */
static int vf_ready(const char *sys, const struct gs_addr *vf, unsigned int type)
{
	char drv[64], ctl[32], st[32], want[16], got[16];

	gs_dev_driver(sys, vf->str, drv, sizeof(drv));
	gs_read_dev_attr(sys, vf->str, "power/control", ctl, sizeof(ctl));
	gs_read_dev_attr(sys, vf->str, "power/runtime_status", st, sizeof(st));
	gs_read_dev_attr(sys, vf->str, "nvidia/current_vgpu_type", got, sizeof(got));
	snprintf(want, sizeof(want), "%u", type);
	return strcmp(drv, "nvidia") == 0 && strcmp(ctl, "on") == 0 && strcmp(st, "active") == 0 &&
	       strcmp(got, want) == 0;
}

/* Each enabled VF of the card as the vGPU stack has it; returns how many are ready. */
static unsigned int print_vfs(const char *sysroot, const struct gs_card *c, unsigned int type)
{
	struct gs_addr vfs[GS_VFS_MAX];
	char why[512];
	unsigned int i, ready = 0, n = 0;

	if (gs_vf_list(sysroot, &c->pf, c->numvfs, vfs, why, sizeof(why))) {
		printf("  VFs: %s\n", why);
		return 0;
	}
	n = c->numvfs < GS_VFS_MAX ? c->numvfs : GS_VFS_MAX;
	for (i = 0; i < n; i++) {
		char drv[64], ctl[32], st[32], got[32];
		int ok = vf_ready(sysroot, &vfs[i], type);

		gs_dev_driver(sysroot, vfs[i].str, drv, sizeof(drv));
		gs_read_dev_attr(sysroot, vfs[i].str, "power/control", ctl, sizeof(ctl));
		gs_read_dev_attr(sysroot, vfs[i].str, "power/runtime_status", st, sizeof(st));
		gs_read_dev_attr(sysroot, vfs[i].str, "nvidia/current_vgpu_type", got, sizeof(got));
		printf("  %s %s: driver=%s type=%s expected=%u power/control=%s runtime_status=%s\n",
		       ok ? "VF ready    " : "VF NOT READY", vfs[i].str, drv[0] ? drv : "none",
		       got[0] ? got : "unreadable", type, ctl[0] ? ctl : "unreadable", st[0] ? st : "unreadable");
		if (ok)
			ready++;
	}
	return ready;
}

/* A card that does not present SR-IOV: why, and what to do next. */
static void status_next(const struct gs_card *c, const struct gs_config *cfg, const char *cfg_path,
			const struct gs_module_state *m, int live)
{
	if (!c->entry) {
		printf("  not in the device table: geforce-sriov does not act on it\n");
		return;
	}
	if (!cfg_path || !gs_addr_listed(cfg->bdfs, cfg->nbdfs, c->pf.str)) {
		printf("  not selected. Next: `geforce-sriov doctor --device %s`, then\n"
		       "  `sudo geforce-sriov install --device %s` (one --device per GPU)\n",
		       c->pf.str, c->pf.str);
		return;
	}
	if (!live) {
		printf("  activation presents SR-IOV on the live host; a fixture root shows the recorded files only\n");
		return;
	}
	printf("  expected until activation runs: %s selects it, and %s presents SR-IOV when it runs.\n"
	       "  Next: `sudo geforce-sriov activate`\n", cfg_path, GS_UNIT_ACTIVATE);
	if (gs_module_selects(m, &c->pf) && !m->lp_ok)
		printf("  note: %s is loaded and its livepatch is not enabled; see `journalctl -b -u %s` and\n"
		       "  `geforce-sriov doctor`\n", GS_MODULE_NAME, GS_UNIT_ACTIVATE);
}

int gs_cmd_status(const struct gs_diag_opts *o)
{
	const char *sysroot = o->sysroot ? o->sysroot : GS_LIVE_SYSFS;
	int live = gs_is_live_root(sysroot), all_ok = 1, stale = 0, build_bad = 0;
	unsigned int ready = 0;
	struct gs_addr devs[GS_CFG_MAX_BDFS];
	struct gs_module_state m;
	struct gs_config cfg;
	const char *cfg_path, *source;
	char err[512], act[GS_PATH_MAX], ver[64] = "";
	unsigned int n, i;
	struct gs_build b;
	int have_b;

	if (gs_diag_config(o, &cfg, &cfg_path, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: config: %s\n", err);
		gs_config_defaults(&cfg);
		all_ok = 0;
	}
	have_b = gs_build_identity(o->build, live, &b);
	if (live)
		gs_nvidia_version(GS_LIVE_PROC, ver, sizeof(ver));
	/* The driver: the running version and its classification, then the installed build and its features. */
	if (ver[0]) {
		char line[256];

		gs_version_format(gs_version_confirmed(GS_VERSION_HOST, ver) ? GS_VERSION_MSG_CONFIRMED :
				  GS_VERSION_MSG_STATUS, ver, line, sizeof(line));
		printf("driver: %s\n", line);
	} else if (live) {
		printf("driver: NVIDIA's driver is not loaded\n");
	}
	if (have_b) {
		char loaded[256], msg[256];

		if (b.state == GS_BUILD_MATCHES)
			printf("build: nvidia.ko for %.100s is the build geforce-sriov installed\n", b.release);
		else if (b.state == GS_BUILD_PRESENT)
			printf("build: nvidia.ko for %.100s is not the build geforce-sriov installed (build-id %s, recorded %s); "
			       "run `sudo geforce-sriov rebuild`\n", b.release, b.found_id[0] ? b.found_id : "unreadable",
			       b.rec_id[0] ? b.rec_id : "none");
		else
			printf("build: no build record for %.100s; run `sudo geforce-sriov rebuild`\n", b.release);
		if (gs_loaded_line(sysroot, &b, loaded, sizeof(loaded))) {
			printf("build: %s\n", loaded);
			stale = 1;
			all_ok = 0;
		}
		if (b.display == 1) {
			struct gs_loaded_patch_result lp;

			if (!live) {
				printf("loaded patch: not checked on a diagnostic fixture root\n");
			} else {
				gs_loaded_patch_verify(GS_LIVE_PROC, &b, &lp);
				printf("loaded patch: %s%s%s\n", lp.state == GS_LOADED_MATCH ? "MATCH" :
				       lp.state == GS_LOADED_MISMATCH ? "MISMATCH" : "UNKNOWN",
				       lp.why[0] ? ": " : "", lp.why);
				if (lp.state != GS_LOADED_MATCH)
					all_ok = 0;
			}
		}
		if (b.display >= 0 || b.host_cuda >= 0)
			printf("build: installed features display=%s host_cuda=%s\n",
			       b.display < 0 ? "not recorded" : b.display ? "on" : "off",
			       b.host_cuda < 0 ? "not recorded" : b.host_cuda ? "on" : "off");
		if (b.driver_version[0]) {
			if (ver[0] && strcmp(ver, b.driver_version))
				printf("build: the record names NVIDIA's driver %s; the running driver is %s\n",
				       b.driver_version, ver);
			if (b.driver_version_confirmed == 0 && gs_version_confirmed(GS_VERSION_HOST, b.driver_version)) {
				gs_combination_line(&b, msg, sizeof(msg));
				printf("build: %s\n", msg);
			} else if (b.driver_version_confirmed == 0) {
				gs_version_format(GS_VERSION_MSG_STATUS, b.driver_version, msg, sizeof(msg));
				printf("build: %s (the installed build record)\n", msg);
			} else if (b.driver_version_confirmed < 0)
				printf("build: the record does not record the driver's classification\n");
		}
		{
			char conf[64];

			if (gs_record_value(b.record, "confirmation", conf, sizeof(conf)))
				printf("build: confirmation=%s\n", conf);
			else
				printf("build: no confirmation line (not recorded)\n");
		}
	}
	gs_read_module_state(sysroot, &m);
	printf("module %s: %s", GS_MODULE_NAME, m.loaded ? "loaded" : "not loaded");
	if (m.loaded)
		printf(" (ari=%s msix=%s vfs=%s bdfs=%s)", m.ari, m.msix, m.vfs[0] ? m.vfs : "(none)",
		       m.bdfs[0] ? m.bdfs : "(none)");
	putchar('\n');
	if (m.loaded)
		printf("livepatch: %s\n", !m.lp_present ? "directory missing" : m.lp_ok ? "enabled, transition complete" :
		       "not enabled or still in transition");
	else
		printf("livepatch: not loaded (activate loads it)\n");
	if (live) {
		printf("activation lock: %s\n", gs_exists(GS_ATTEMPT_MARKER) ?
		       "SET - an activation attempt did not finish (see `geforce-sriov doctor`)" : "clear");
		if (gs_exists(GS_ATTEMPT_MARKER))
			all_ok = 0;
	}
	/*
	 * The loaded module lacks what the installed build has: the power cycle is the next step.
	 */
	if (stale)
		all_ok = 0;
	if (gs_activating_left(o, live, act, sizeof(act))) {
		printf("activate: %s exists (activate is running or did not finish)\n", act);
		all_ok = 0;
	}
	n = gs_selected_devices(o, &cfg, sysroot, devs, &source);
	if (n == 0) {
		printf("no NVIDIA display function selected or found (%s)\n"
		       "next: select a GPU with `sudo geforce-sriov install --device BDF`; `geforce-sriov devices` lists the supported IDs\n",
		       source);
		return GS_EXIT_FAIL;
	}
	/* The display and host-CUDA requests against what the installed build provides. */
	{
		int boot_display = 0, want_display, console = 0;

		for (i = 0; i < n; i++) {
			struct gs_card c;

			gs_card_inspect(sysroot, &devs[i], &c);
			boot_display |= c.present && c.boot_vga == 1;
			console |= c.present && gs_display_kms(&cfg, &c);
		}
		/* want_display is what the build must provide; console is what activation sets up. */
		want_display = cfg.display != GS_DISPLAY_OFF && boot_display;
		printf("display: requested=%s, effective for %s: %s\n", gs_display_mode_name(cfg.display),
		       boot_display ? "the boot display GPU" : "the selection",
		       console ? "on" : cfg.display == GS_DISPLAY_ON ?
		       "off (no selected GPU is the boot display, so no console is set up)" : "off");
		printf("host CUDA: requested=%s%s\n", gs_sw_name(cfg.host_cuda),
		       have_b && b.host_cuda >= 0 ? (b.host_cuda ? "; installed" : "; not in the installed build") :
		       have_b ? "; not recorded in the build" : "");
		/* Host CUDA needs NVIDIA's CUDA library too. It has no part in whether the VFs are ready. */
		if (live) {
			char cuda[2 * GS_PATH_MAX];

			if (gs_cuda_libs_line(GS_MANIFEST, cfg.host_cuda == GS_SW_ON, NULL, cuda, sizeof(cuda)))
				printf("host CUDA libraries: %s\n", cuda);
		}
		if (have_b && b.display >= 0 && want_display != b.display) {
			printf("  %s\n", want_display ? "display is requested and the installed build has none" :
			       "the installed build has the display stack and display is not requested");
			all_ok = 0;
			build_bad = 1;
		}
		if (have_b && b.display < 0 && want_display) {
			printf("  display is requested and the build record does not say whether the display stack is installed\n");
			all_ok = 0;
			build_bad = 1;
		}
		if (have_b && b.host_cuda >= 0 && cfg.host_cuda != b.host_cuda) {
			printf("  %s\n", cfg.host_cuda ? "host CUDA is requested and the installed build has no nvidia-uvm" :
			       "the installed build carries nvidia-uvm and host CUDA is not requested");
			all_ok = 0;
			build_bad = 1;
		}
		if (!have_b && (want_display || cfg.host_cuda)) {
			printf("  no build record, so the requested features cannot be confirmed; run `sudo geforce-sriov install`\n");
			all_ok = 0;
			build_bad = 1;
		}
	}
	for (i = 0; i < n; i++) {
		struct gs_card c;
		char p[GS_PATH_MAX], first[256];
		unsigned int nothers, type = 0, vfready = 0;
		int with_bridge;

		gs_card_inspect(sysroot, &devs[i], &c);
		printf("\n%s", devs[i].str);
		if (!c.present) {
			printf(": absent\n  check the address: `geforce-sriov doctor` lists every NVIDIA GPU it finds\n");
			all_ok = 0;
			continue;
		}
		printf(": 10de:%04x %s driver %s\n", c.device, c.entry ? c.entry->name : "(not in device table)",
		       c.driver[0] ? c.driver : "none");
		if (c.entry) {
			char vfs[128];

			type = gs_config_vgpu_type(&cfg, c.entry->generation);
			gs_vfs_describe(c.entry, &cfg, vfs, sizeof(vfs));
			printf("  VFs configured: %s\n", vfs);
		}
		printf("  display/KMS: boot_vga=%d, %s\n", c.boot_vga,
		       !gs_pathf(p, sizeof(p), "%s/module/nvidia_drm", sysroot) && gs_exists(p) ? "nvidia_drm loaded" :
		       "no nvidia_drm");
		if (gs_iommu_peers(sysroot, &c, &with_bridge, first, sizeof(first), &nothers) == 0 && nothers)
			printf("  warning: IOMMU group %s also holds %u other device(s), first %s: VFs may share it (see `geforce-sriov doctor`)\n",
			       c.iommu_group, nothers, first);
		if (!c.has_sriov) {
			printf("  SR-IOV: not presented\n");
			status_next(&c, &cfg, cfg_path, &m, live);
			all_ok = 0;
			continue;
		}
		if (gs_check_presented(&c, &cfg, err, sizeof(err))) {
			printf("  SR-IOV: %s\n", err);
			all_ok = 0;
			continue;
		}
		printf("  SR-IOV: presented, sriov_totalvfs=%u ari_enabled=%d\n", c.totalvfs, c.ari_enabled);
		printf("  VF resources: %s\n", c.vf_bars_assigned == 0 ? "none assigned" :
		       c.vf_bars_assigned > 0 ? "assigned" : "unknown (resource file unreadable)");
		if (c.vf_bars_assigned == 0 || c.bar0_assigned == 0) {
			printf("  %s\n", c.bar0_assigned == 0 ? "BAR0 is not assigned: the card is unusable this boot; see "
			       "`geforce-sriov doctor`" : "no VF BAR is assigned: enabling VFs would fail; see "
			       "`geforce-sriov doctor`");
			all_ok = 0;
		}
		printf("  VFs enabled: %u\n", c.numvfs);
		if (c.numvfs == 0) {
			all_ok = 0;
			printf("  VF configuration: no VF to check until activation enables them\n");
			continue;
		}
		vfready = print_vfs(sysroot, &c, type);
		printf("  VF configuration: %u of %u ready (driver nvidia, type %u, power/control on, runtime active)\n",
		       vfready, c.numvfs, type);
		/* Ada: whether the VFs' framebuffer slots are held. Not held is how it was before, so it is no failure. */
		if (live && c.entry && c.entry->generation == GS_ADA) {
			char slots[768];

			gs_fb_line(GS_LIVE_PROC, &cfg, have_b ? b.record : NULL, c.pf.str, ver, slots, sizeof(slots));
			printf("  framebuffer slots: %s\n", slots);
		}
		if (vfready < c.numvfs)
			all_ok = 0;
		else
			ready++;
	}
	printf("\n");
	if (stale)
		printf("next: power the machine off and on (the running nvidia module is not the installed one)\n");
	else if (live && gs_exists(GS_ATTEMPT_MARKER))
		printf("next: after a cold power cycle, run `geforce-sriov doctor`, then if safe `geforce-sriov activate --clear-lock` and `geforce-sriov activate`\n");
	else if (gs_activating_left(o, live, act, sizeof(act)))
		printf("next: stop every VM that uses a vGPU, then run `geforce-sriov activate`\n");
	else if (ready < n)
		printf("next: `sudo geforce-sriov activate`; each card above names what is missing\n");
	else if (build_bad)
		printf("next: every selected VF is ready, but the installed build does not match the config (see the lines above).\n"
		       "      Run `sudo geforce-sriov install`, then power the machine off and on\n");
	else if (!all_ok)
		printf("next: every selected VF is ready, but a line above reports a problem; `geforce-sriov doctor` gives the details\n");
	else
		printf("next: every selected VF is ready. Nothing to bring up.\n");
	printf("%u of %u selected GPU(s) have every VF ready.\n", ready, n);
	return all_ok ? GS_EXIT_OK : GS_EXIT_FAIL;
}
