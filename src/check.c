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

/* ---- check: the Ada and Ampere CPU-RM host stack ------------------------------------------ */

int gs_parse_vgpu_type(const char *value, unsigned int *out)
{
	unsigned long v;
	char *end;

	if (!value || value[0] < '1' || value[0] > '9')
		return -1;
	errno = 0;
	v = strtoul(value, &end, 10);
	if (errno || *end || v > GS_VGPU_TYPE_MAX)
		return -1;
	*out = (unsigned int)v;
	return 0;
}

static int digest_is(const char *sha, const char *recorded)
{
	return recorded && strcmp(sha, recorded) == 0;
}

/*
 * The row a plugin is judged by: the one that records its digest; else the confirmed version its
 * resolved file name states (libnvidia-vgpu.so.<version>...); else the first row.
 */
static const struct gs_confirmed_version *plugin_row(const char *real, const char *sha)
{
	size_t count;
	const struct gs_confirmed_version *rows = gs_confirmed_versions(&count);
	const char *name = strrchr(real, '/');

	for (size_t i = 0; i < count; i++)
		if (digest_is(sha, rows[i].patched_plugin) || digest_is(sha, rows[i].mixed_plugin) ||
		    digest_is(sha, rows[i].stock_plugin))
			return &rows[i];
	name = name ? name + 1 : real;
	for (size_t i = 0; i < count; i++)
		if (strstr(name, rows[i].host))
			return &rows[i];
	return &rows[0];
}

/* The verdict half of check_plugin, on a digest already taken; prints nothing. */
int gs_plugin_verdict(const char *path, const char *real, const char *sha, char *why, size_t len)
{
	const struct gs_confirmed_version *row = plugin_row(real, sha);
	const char *patched = row->patched_plugin ? row->patched_plugin : "not recorded";

	if (digest_is(sha, row->patched_plugin))
		return 0;
	if (digest_is(sha, row->mixed_plugin))
		return snprintf(why, len, "%.300s (%.300s) is an earlier copy without the Ada framebuffer change. Run\n"
				"`sudo geforce-sriov install` again to replace it with the patched plugin copy, SHA-256 %s"
				" (docs/driver-patch.md#byte-patches)", path, real, patched), -1;
	if (strcmp(sha, ADA_PLUGIN_H5_SHA) == 0)
		return snprintf(why, len, "%.300s (%.300s) is the earlier Ada-only copy; replace it with the patched plugin copy,\n"
				"SHA-256 %s (docs/driver-patch.md#byte-patches)", path, real, patched), -1;
	if (digest_is(sha, row->stock_plugin))
		return snprintf(why, len, "%.300s is NVIDIA's unchanged %s plugin (%.300s); Ada and Ampere need its patched plugin copy,\n"
				"SHA-256 %s (docs/driver-patch.md#byte-patches)", path, row->host, real, patched), -1;
	return snprintf(why, len, "%.300s (%.300s) has SHA-256 %s: not the patched plugin copy of NVIDIA's %s\n"
			"plugin, SHA-256 %s", path, real, sha, row->host, patched), -1;
}

/*
 * The plugin nvidia-vgpu-mgr loads must be the patched plugin copy of a confirmed version: the
 * verdict comes from gs_plugin_verdict() on the SHA-256 of the resolved copy.
 */

/* ---- check ------------------------------------------------------------------------------- */

int gs_check_vfs(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	const char *sys = h->sys;
	struct gs_vgpu_card *cards;
	unsigned int n = 0, i, j;
	int r, ready = 1;

	cards = calloc(GS_CFG_MAX_BDFS, sizeof(*cards));
	if (!cards)
		return GS_EXIT_FAIL;
	if ((r = gs_select_cards(sys, cfg, o, cards, &n))) {
		free(cards);
		return r;
	}
	/* An unfinished activate or re-probe: the VF state read now may be about to change. */
	for (i = 0; i < 2; i++) {
		char p[GS_PATH_MAX];
		struct stat st;

		if (gs_pathf(p, sizeof(p), "%s/%s", i ? h->run_dir : h->state_dir, i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME)) {
			printf("NOT READY: the %s path is too long\n", i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME);
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (lstat(p, &st) == 0) {
			printf("NOT READY: %s exists: %s did not finish; start no VM\n", p,
			       i ? "`geforce-sriov activate` is running or" : "an earlier activation");
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (errno != ENOENT) {
			printf("NOT READY: cannot tell whether %s exists: %s\n", p, strerror(errno));
			free(cards);
			return GS_EXIT_FAIL;
		}
	}
	/* The driver version and the driver step's build are reported, never judged: the VF lines below decide the exit code. */
	{
		char ver[32], line[BUILD_LINE_MAX], loaded[256];
		struct gs_build b;

		gs_nvidia_version(h->proc, ver, sizeof(ver));
		if (ver[0]) {
			gs_driver_version_line(ver, line, sizeof(line));
			printf("%s\n", line);
		}
		if (gs_build_identity(o->build, gs_is_live_root(sys), &b)) {
			gs_build_line(&b, line, sizeof(line));
			if (gs_loaded_line(sys, &b, loaded, sizeof(loaded)))
				printf("%s; %s\n", line, loaded);
			else
				printf("%s\n", line);
		}
	}
	for (i = 0; i < n; i++) {
		struct gs_vgpu_card *k = &cards[i];
		char why[512], want[16];
		struct gs_card c;

		snprintf(want, sizeof(want), "%u", k->type);
		gs_card_inspect(sys, &k->pf, &c);
		if (strcmp(c.driver, "nvidia") != 0) {
			printf("NOT READY %s: bound to %s, not nvidia\n", k->pf.str, c.driver[0] ? c.driver : "nothing");
			ready = 0;
			continue;
		}
		if (c.numvfs < 1) {
			printf("NOT READY %s: no VF is enabled (`geforce-sriov activate` enables them)\n", k->pf.str);
			ready = 0;
			continue;
		}
		if (gs_vf_list(sys, &k->pf, c.numvfs, k->vf, why, sizeof(why))) {
			printf("NOT READY %s\n", why);
			ready = 0;
			continue;
		}
		/* A laptop GPU powered off since its VFs were configured (warnings only; the VF lines decide). */
		{
			char lost[2][256];
			unsigned int nlost = gs_laptop_lost(sys, &c, lost);

			for (j = 0; j < nlost; j++)
				printf("WARNING %s\n", lost[j]);
		}
		for (j = 0; j < c.numvfs; j++) {
			char drv[64], ctl[32], st[32], type[32], dir[GS_PATH_MAX];
			unsigned int id08 = 0xffffffffu;
			int ok, dead = 0;

			gs_dev_driver(sys, k->vf[j].str, drv, sizeof(drv));
			gs_read_dev_attr(sys, k->vf[j].str, "power/control", ctl, sizeof(ctl));
			gs_read_dev_attr(sys, k->vf[j].str, "power/runtime_status", st, sizeof(st));
			ok = gs_read_dev_attr(sys, k->vf[j].str, "nvidia/current_vgpu_type", type, sizeof(type)) == 0 &&
			     strcmp(drv, "nvidia") == 0 && strcmp(ctl, "on") == 0 && strcmp(st, "active") == 0 &&
			     strcmp(type, want) == 0;
			/*
			 * A laptop VF's liveness: its revision/class dword (offset 0x08). A VF's vendor and device IDs read
			 * ffff by design, so they are no test; a VF that does not answer reads ffffffff here too.
			 */
			if (c.entry && c.entry->laptop &&
			    (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sys, k->vf[j].str) ||
			     gs_config_dword(dir, 0x08, &id08) || id08 == 0xffffffffu))
				ok = 0, dead = 1;
			printf("%s %s (VF of %s): driver=%s type=%s expected=%s power/control=%s runtime_status=%s\n",
			       ok ? "OK" : "NOT READY", k->vf[j].str, k->pf.str, drv[0] ? drv : "none", type[0] ? type : "unreadable",
			       want, ctl[0] ? ctl : "unreadable", st[0] ? st : "unreadable");
			if (dead)
				printf("NOT READY %s: its config space reads ffffffff at 0x08, or cannot be read; the GPU may have "
				       "been powered off\n", k->vf[j].str);
			if (!ok)
				ready = 0;
		}
		/*
		 * Ada: whether the VFs' framebuffer slots are held. Told, never judged: without a held slot a VM
		 * starts as it did before there was an owner, so this line changes neither READY nor the exit code.
		 */
		if (k->generation == GS_ADA) {
			char slots[768], ver[32] = "";
			struct gs_build b;
			int have = gs_build_identity(o->build, gs_is_live_root(sys), &b);

			gs_nvidia_version(h->proc, ver, sizeof(ver));
			gs_fb_line(h->proc, cfg, have ? b.record : NULL, k->pf.str, ver, slots, sizeof(slots));
			printf("INFO %s: framebuffer slots: %s\n", k->pf.str, slots);
		}
	}
	/* An activate or re-probe that started during the reads: what was read above may be changing. */
	for (i = 0; i < 2; i++) {
		char p[GS_PATH_MAX];
		struct stat st;

		if (gs_pathf(p, sizeof(p), "%s/%s", i ? h->run_dir : h->state_dir, i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME)) {
			printf("NOT READY: the %s path is too long\n", i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME);
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (lstat(p, &st) == 0) {
			printf("NOT READY: %s appeared during the check (%s); the readings above do not count; start no VM\n",
			       p, i ? "`geforce-sriov activate` started" : "an activation started");
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (errno != ENOENT) {
			printf("NOT READY: cannot tell whether %s exists: %s; the readings above do not count\n", p,
			       strerror(errno));
			free(cards);
			return GS_EXIT_FAIL;
		}
	}
	free(cards);
	if (!ready)
		fprintf(stderr, "geforce-sriov: not ready: start no VM on a VF marked NOT READY. After a PF re-probe or a restart of\n"
			"nvidia-vgpu-mgr, stop the VMs that use a vGPU and run `geforce-sriov activate` again\n");
	return ready ? GS_EXIT_OK : GS_EXIT_FAIL;
}

int gs_cmd_check(const struct gs_activate_opts *o)
{
	const char *path = gs_config_path();
	struct gs_config cfg;
	char err[512];

	if (gs_config_load(path, &cfg, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: config: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	if (cfg.nbdfs == 0) {
		fprintf(stderr, "geforce-sriov: no device selected (bdfs= in %s)\n", path);
		return GS_EXIT_REFUSED;
	}
	/* vgpud's ExecStartPre: the read-only admission predicate, without the per-VF report. */
	if (o->daemon_ready)
		return gs_daemon_ready(&gs_host_live, &cfg, o);
	return gs_check_vfs(&gs_host_live, &cfg, o);
}
