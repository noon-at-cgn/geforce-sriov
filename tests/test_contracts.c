// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gs_lifecycle.h"

int gs_cli_main(int argc, char **argv);
#define main gs_cli_main
#include "../src/main.c"
#undef main

static struct gs_install_opts captured_install;
static struct gs_activate_opts captured_activate;
static struct gs_livepatch_opts captured_livepatch;
int gs_cmd_devices(void) { return 0; }
int gs_cmd_doctor(const struct gs_diag_opts *o) { (void)o; return 0; }
int gs_cmd_status(const struct gs_diag_opts *o) { (void)o; return 0; }
int gs_cmd_install(const struct gs_install_opts *o) { captured_install = *o; return 0; }
int gs_cmd_setup(const struct gs_install_opts *o, enum gs_prompt p)
{
	(void)p;
	captured_install = *o;
	return 0;
}
int gs_cmd_activate(const struct gs_activate_opts *o) { captured_activate = *o; return 0; }
int gs_cmd_check(const struct gs_activate_opts *o) { captured_activate = *o; return 0; }
int gs_cmd_livepatch(const struct gs_livepatch_opts *o) { captured_livepatch = *o; return 0; }
int gs_cmd_uninstall(int unload) { (void)unload; return 0; }
static const char *captured_release;
static int rebuilds;
int gs_cmd_rebuild(const char *release) { captured_release = release; rebuilds++; return 0; }

static void test_cli(void)
{
	char *setup[] = { "gs", "setup", "--display", "--host-cuda", "--companion-source",
			  "/source", "--nvkms-source", "/nvkms", "--allow-unsupported-driver" };
	char *activate[] = { "gs", "activate", "--clear-lock" };
	char *activate_old[] = { "gs", "activate", "--clear-guard" };
	char *activate_both[] = { "gs", "activate", "--clear-lock", "--dry-run" };
	char *check[] = { "gs", "check", "--daemon-ready" };
	char *conflict[] = { "gs", "setup", "--display", "--no-display" };
	char *removed[] = { "gs", "prepare" };
	char *oldopt[] = { "gs", "install", "--hold-nvidia" };
	char *livepatch[] = { "gs", "livepatch", "--boot", "--config", "/cfg" };
	char *livepatch_driver[] = { "gs", "livepatch", "--boot", "--load-driver", "--config", "/cfg" };
	char *livepatch_plain[] = { "gs", "livepatch" };
	char *from_setup[] = { "gs", "install", "--from-setup" };
	char *plain_install[] = { "gs", "install" };
	char *rebuild[] = { "gs", "rebuild" };
	char *rebuild_kernel[] = { "gs", "rebuild", "--kernel", "6.8.12-4-pve" };
	char *rebuild_path[] = { "gs", "rebuild", "--kernel", "../6.8" };
	char *rebuild_input[] = { "gs", "rebuild", "--companion-file", "/x" };
	assert(gs_cli_main(9, setup) == 0);
	assert(captured_install.display_set && captured_install.display == GS_DISPLAY_ON);
	assert(captured_install.host_cuda_set && captured_install.host_cuda);
	assert(!strcmp(captured_install.companion_source, "/source"));
	assert(!strcmp(captured_install.nvkms_source, "/nvkms") && captured_install.allow_unsupported_driver);
	assert(gs_cli_main(3, activate) == 0 && captured_activate.clear_guard);
	/* The option's earlier name is refused with the new one, and nothing is cleared. */
	captured_activate.clear_guard = 0;
	assert(gs_cli_main(3, activate_old) == GS_EXIT_USAGE && !captured_activate.clear_guard);
	assert(gs_cli_main(4, activate_both) == GS_EXIT_USAGE);
	assert(gs_cli_main(3, check) == 0 && captured_activate.daemon_ready);
	assert(gs_cli_main(4, conflict) == GS_EXIT_USAGE);
	assert(gs_cli_main(2, removed) == GS_EXIT_USAGE);
	assert(gs_cli_main(3, oldopt) == GS_EXIT_USAGE);
	assert(gs_cli_main(5, livepatch) == 0 && captured_livepatch.boot &&
	       !strcmp(captured_livepatch.config_path, "/cfg"));
	assert(!captured_livepatch.load_driver);
	assert(gs_cli_main(6, livepatch_driver) == 0 && captured_livepatch.boot && captured_livepatch.load_driver &&
	       !strcmp(captured_livepatch.config_path, "/cfg"));
	assert(gs_cli_main(2, livepatch_plain) == GS_EXIT_USAGE);
	/* setup's mark on the install it runs. */
	assert(gs_cli_main(3, from_setup) == 0 && captured_install.from_setup);
	assert(gs_cli_main(2, plain_install) == 0 && !captured_install.from_setup);
	/* rebuild takes no input: the running kernel, or the one named. */
	assert(gs_cli_main(2, rebuild) == 0 && rebuilds == 1 && !captured_release);
	assert(gs_cli_main(4, rebuild_kernel) == 0 && rebuilds == 2 && !strcmp(captured_release, "6.8.12-4-pve"));
	assert(gs_cli_main(4, rebuild_path) == GS_EXIT_USAGE && gs_cli_main(4, rebuild_input) == GS_EXIT_USAGE && rebuilds == 2);
}

int gs_validate_abs_path(const char *p) { return !p || p[0] != '/'; }
int gs_parse_vgpu_type(const char *value, unsigned int *out)
{
	(void)value;
	(void)out;
	return -1;
}

int main(void)
{
	struct gs_config c, roundtrip;
	struct gs_vf_bar_size bar;
	char err[1024], text[8192];
	const char *input = "display=on\nhost_cuda=on\n";
	gs_config_defaults(&c);
	test_cli();
	assert(c.display == GS_DISPLAY_AUTO && c.host_cuda == GS_SW_OFF);
	assert(gs_config_parse(input, strlen(input), &c, err, sizeof(err)) == 0);
	assert(c.display == GS_DISPLAY_ON && c.host_cuda == GS_SW_ON);
	assert(gs_config_render(&c, text, sizeof(text)) > 0);
	assert(gs_config_parse(text, strlen(text), &roundtrip, err, sizeof(err)) == 0);
	assert(roundtrip.display == c.display && roundtrip.host_cuda == c.host_cuda);
	input = "display=auto\ndisplay=off\n";
	assert(gs_config_parse(input, strlen(input), &c, err, sizeof(err)) < 0 && strstr(err, "line 2: duplicate"));
	input = "host_cuda=auto\n";
	assert(gs_config_parse(input, strlen(input), &c, err, sizeof(err)) < 0 && strstr(err, "host_cuda must be on or off"));
	input = "display=no\n";
	assert(gs_config_parse(input, strlen(input), &c, err, sizeof(err)) < 0 && strstr(err, "display must be"));
	input = "# old\nboot_display_release=0000:01:00.0\n";
	assert(gs_config_parse(input, strlen(input), &c, err, sizeof(err)) < 0 && strstr(err, "line 2: boot_display_release=0000:01:00.0") && strstr(err, "display=auto|on|off"));
	assert(gs_version_confirmed(GS_VERSION_HOST, "580.159.01"));
	assert(!gs_version_confirmed(GS_VERSION_HOST, "580.159.03"));
	assert(gs_version_confirmed(GS_VERSION_COMPANION, "580.159.03"));
	assert(gs_version_format(GS_VERSION_MSG_PROMPT, "1", text, sizeof(text)) > 0 && strstr(text, "[y/N]"));
	assert(gs_vf_bar_size_lookup(GS_AMPERE, 0, &bar) && bar.bytes == 262144 && !bar.prefetchable);
	assert(gs_vf_bar_size_lookup(GS_ADA, 1, &bar) && bar.bytes == 268435456 && bar.prefetchable);
	assert(gs_vf_bar_size_lookup(GS_ADA, 3, &bar) && bar.bytes == 33554432);
	assert(!gs_vf_bar_size_lookup(99, 0, &bar));
	assert(!gs_vf_bar_size_lookup(GS_ADA, 2, &bar));
	puts("contract tests passed");
	return 0;
}
