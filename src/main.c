// SPDX-License-Identifier: MIT
/* geforce-sriov: command-line entry point. */
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

#include "gs_lifecycle.h"

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: geforce-sriov COMMAND [OPTIONS]\n"
		"  devices | status\n"
		"  check [--device BDF]... [--type N] [--daemon-ready]\n"
		"  activate [--type N] [--dry-run] [--boot] [--clear-lock]\n"
		"           [--stop-display-manager]\n"
		"  livepatch --boot [--load-driver] [--config FILE]\n"
		"      Load the livepatch (the early boot unit): no removal, no rescan.\n"
		"      --load-driver  wait for udev, then load nvidia (the first step of the activate unit)\n"
		"  setup [--device BDF]... [--vfs N] [--interactive | --no-prompt]\n"
		"        [--allow-unsupported-driver] [feature and package options]\n"
		"  install [--device BDF]... [--vfs N] [--prefix PATH] [--destdir PATH]\n"
		"          [--source DIR] [--ari=on|off] [--msix=on|off] [feature and package options]\n"
		"  install --dry-run [--device BDF]... [feature and package options]\n"
		"      Check this host for that install and change nothing (setup runs the same checks first).\n"
		"  rebuild [--kernel RELEASE]\n"
		"      Build NVIDIA's modules again for a kernel, from the inputs install kept. A kernel update runs it by itself.\n"
		"  uninstall [--unload]\n"
		"Feature and package options for install and setup:\n"
		"  --display | --no-display      select display build\n"
		"  --host-cuda | --no-host-cuda  select host CUDA\n"
		"  --host-desktop | --no-host-desktop  select NVIDIA's graphics libraries for the host desktop\n"
		"  --companion-file PATH | --companion-source DIR\n"
		"  --nvkms-source PATH           public NVKMS source directory or tarball\n"
		"  --driver-file PATH | --nvidia-source DIR | --driver=skip\n"
		"  --accept-nvidia-license --disable-d3cold\n"
		"  --allow-unsupported-driver    setup consent; ignored by install\n"
		"Exit status: 0 ok, 1 failed, 2 usage, 3 refused, 4 locked, 5 activation lock.\n");
}

static int bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int bad(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "geforce-sriov: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\nTry 'geforce-sriov --help'.\n");
	return GS_EXIT_USAGE;
}

/* "--vfs", "--num-vfs=8", "--max_vfs" ...: an option asking for a VF count. */
static int is_vf_count_opt(const char *a)
{
	size_t n = strcspn(a, "=");
	char name[32];

	if (strncmp(a, "--", 2) != 0 || n >= sizeof(name))
		return 0;
	memcpy(name, a, n);
	name[n] = '\0';
	return strstr(name, "vf") || strstr(name, "VF");
}

/* a is not an option of this command; say where the VF count is set when that was the intent. */
static int unknown_opt(const char *a, int install)
{
	if (!is_vf_count_opt(a))
		return bad("unknown option '%s'", a);
	if (install)
		return bad("%s: the VF count option is --vfs N (1 up to the card's hardware default, default %u)", a,
			   GS_DEFAULT_VFS);
	return bad("%s: set the VF count with `geforce-sriov install --vfs N` (vfs=N in the config). It takes effect\n"
		   "at the next boot", a);
}

/* "--name=value" or "--name value"; returns the value or NULL when absent. */
static const char *opt_value(int argc, char **argv, int *i, const char *name)
{
	size_t n = strlen(name);
	const char *a = argv[*i];

	if (strncmp(a, name, n) != 0)
		return NULL;
	if (a[n] == '=')
		return a + n + 1;
	if (a[n] != '\0')
		return NULL;
	if (*i + 1 >= argc)
		return "";
	return argv[++*i];
}

static int parse_device(const char *opt, const char *v, struct gs_addr *list, unsigned int *n)
{
	struct gs_addr a;
	unsigned int i;

	if (gs_addr_parse(v, &a))
		return bad("%s '%s' is not a full PCI address dddd:bb:ss.f as /sys/bus/pci/devices names it (lowercase\n"
			   "hex; a domain longer than 4 digits has no leading zero; at most %x)", opt, v, GS_PCI_DOMAIN_MAX);
	for (i = 0; i < *n; i++)
		if (strcmp(list[i].str, a.str) == 0)
			return bad("%s %s given twice", opt, v);
	if (*n == GS_CFG_MAX_BDFS)
		return bad("at most %u %s options (%s)", GS_CFG_MAX_BDFS, opt, v);
	list[(*n)++] = a;
	return 0;
}


/* --driver-file PATH: an existing regular file, resolved once so every step reads the same one. */
static int parse_driver_file(const char *v, const char **out)
{
	static char real[PATH_MAX];
	struct stat st;

	if (*out)
		return bad("%s given twice", "--driver-file");
	if (!v[0] || !realpath(v, real) || stat(real, &st) || !S_ISREG(st.st_mode))
		return bad("--driver-file '%s' is not a regular file; give NVIDIA's vGPU host .run package", v);
	*out = real;
	return 0;
}

static int parse_package_opt(int argc, char **argv, int *i, struct gs_install_opts *o, int *matched)
{
	const char *a = argv[*i], *v;
	const char **path = NULL;

	*matched = 1;
	if ((v = opt_value(argc, argv, i, "--driver-file")))
		return parse_driver_file(v, &o->driver_file);
	if ((v = opt_value(argc, argv, i, "--companion-file")))
		path = &o->companion_file;
	else if ((v = opt_value(argc, argv, i, "--companion-source")))
		path = &o->companion_source;
	else if ((v = opt_value(argc, argv, i, "--nvkms-source")))
		path = &o->nvkms_source;
	if (path) {
		if (*path)
			return bad("%s given twice", a);
		if (gs_validate_abs_path(v))
			return bad("%s requires a normalized absolute path", a);
		*path = v;
		return 0;
	}
	if (!strcmp(a, "--display") || !strcmp(a, "--no-display")) {
		if (o->display_set)
			return bad("--display and --no-display may be given only once");
		o->display_set = 1;
		o->display = !strcmp(a, "--display") ? GS_DISPLAY_ON : GS_DISPLAY_OFF;
	} else if (!strcmp(a, "--host-cuda") || !strcmp(a, "--no-host-cuda")) {
		if (o->host_cuda_set)
			return bad("--host-cuda and --no-host-cuda may be given only once");
		o->host_cuda_set = 1;
		o->host_cuda = !strcmp(a, "--host-cuda");
	} else if (!strcmp(a, "--host-desktop") || !strcmp(a, "--no-host-desktop")) {
		if (o->host_desktop_set)
			return bad("--host-desktop and --no-host-desktop may be given only once");
		o->host_desktop_set = 1;
		o->host_desktop = !strcmp(a, "--host-desktop");
	} else if (!strcmp(a, "--allow-unsupported-driver")) {
		o->allow_unsupported_driver = 1;
	} else if (!strcmp(a, "--accept-nvidia-license")) {
		o->accept_nvidia_license = 1;
	} else if (!strcmp(a, "--disable-d3cold")) {
		o->disable_d3cold = 1;
	} else {
		*matched = 0;
	}
	return 0;
}


/* What install and setup check once every option is read. */
static int check_install_opts(const struct gs_install_opts *o)
{
	if (o->companion_file && o->companion_source)
		return bad("--companion-file and --companion-source both name the Linux driver package; give one");
	if (o->driver_file && o->nvidia_source)
		return bad("--driver-file and --nvidia-source both name NVIDIA's kernel tree; give one");
	if (o->driver_file && o->skip_driver)
		return bad("--driver-file and --driver=skip cannot be combined");
	if (o->accept_nvidia_license && !o->driver_file)
		return bad("--accept-nvidia-license needs --driver-file PATH");
	return 0;
}

static int parse_switch_opt(int argc, char **argv, int *i, struct gs_overrides *ov, int *matched)
{
	static const char *const names[] = { "--ari", "--msix" };
	int *dst[] = { &ov->ari, &ov->msix };

	*matched = 0;
	for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		const char *v = opt_value(argc, argv, i, names[k]);

		if (!v)
			continue;
		*matched = 1;
		if (gs_parse_switch(v, dst[k]))
			return bad("%s takes on or off", names[k]);
		return 0;
	}
	return 0;
}

/* --type of activate and check; --device too when check, --dry-run and --boot when activate. */
static int parse_vgpu_opts(int argc, char **argv, struct gs_activate_opts *o, int activate)
{
	int i, r;

	memset(o, 0, sizeof(*o));
	for (i = 2; i < argc; i++) {
		const char *v;

		if (activate && strncmp(argv[i], "--device", 8) == 0 && (argv[i][8] == '\0' || argv[i][8] == '=')) {
			return bad("activate acts on every selected GPU (bdfs= in the config) and takes no --device");
		} else if (!activate && (v = opt_value(argc, argv, &i, "--device"))) {
			if ((r = parse_device("--device", v, o->devices, &o->ndevices)))
				return r;
			if (o->devices[o->ndevices - 1].func != 0)
				return bad("--device %s: name the physical function .0", v);
		} else if ((v = opt_value(argc, argv, &i, "--type"))) {
			if (o->type)
				return bad("%s given twice", "--type");
			if (gs_parse_vgpu_type(v, &o->type))
				return bad("--type '%s': give a vGPU type ID, 1 to %u, as nvidia/creatable_vgpu_types lists it", v,
					   GS_VGPU_TYPE_MAX);
		} else if (activate && strcmp(argv[i], "--dry-run") == 0) {
			o->dry_run = 1;
		} else if (activate && strcmp(argv[i], "--boot") == 0) {
			o->boot = 1;
		} else if (activate && strcmp(argv[i], "--clear-lock") == 0) {
			o->clear_guard = 1;
		} else if (activate && strcmp(argv[i], "--stop-display-manager") == 0) {
			o->stop_display_manager = 1;
		} else if (!activate && strcmp(argv[i], "--daemon-ready") == 0) {
			o->daemon_ready = 1;
		} else {
			return unknown_opt(argv[i], 0);
		}
	}
	if (o->clear_guard && argc != 3)
		return bad("--clear-lock takes no other option");
	return 0;
}

/* livepatch --boot [--load-driver] [--config FILE]: loads the module, then optionally nvidia. */
static int parse_livepatch_opts(int argc, char **argv, struct gs_livepatch_opts *o)
{
	struct stat st;
	int i;

	memset(o, 0, sizeof(*o));
	for (i = 2; i < argc; i++) {
		const char *v;

		if (strcmp(argv[i], "--boot") == 0) {
			o->boot = 1;
		} else if (strcmp(argv[i], "--load-driver") == 0) {
			o->load_driver = 1;
		} else if ((v = opt_value(argc, argv, &i, "--config"))) {
			if (v[0] == '\0')
				return bad("%s needs a file", "--config");
			o->config_path = v;
		} else if ((v = opt_value(argc, argv, &i, "--sysfs-root"))) {
			if (gs_validate_abs_path(v) || stat(v, &st) || !S_ISDIR(st.st_mode))
				return bad("--sysfs-root '%s' must be an existing normalized absolute directory", v);
			o->sysroot = v;
		} else {
			return unknown_opt(argv[i], 0);
		}
	}
	if (!o->boot)
		return bad("livepatch needs --boot: it is the early boot unit's mode and changes nothing else");
	return 0;
}

static int status_main(int argc, char **argv)
{
	struct gs_diag_opts o;
	struct stat st;
	int i, r;

	memset(&o, 0, sizeof(o));
	for (i = 2; i < argc; i++) {
		const char *v;

		if ((v = opt_value(argc, argv, &i, "--device"))) {
			if ((r = parse_device("--device", v, o.devices, &o.ndevices)))
				return r;
		} else if ((v = opt_value(argc, argv, &i, "--sysfs-root"))) {
			if (gs_validate_abs_path(v) || stat(v, &st) || !S_ISDIR(st.st_mode))
				return bad("--sysfs-root '%s' must be an existing normalized absolute directory", v);
			o.sysroot = v;
		} else if ((v = opt_value(argc, argv, &i, "--config"))) {
			if (v[0] == '\0')
				return bad("%s needs a file", "--config");
			o.config_path = v;
		} else {
			return unknown_opt(argv[i], 0);
		}
	}
	return gs_cmd_status(&o);
}

/* Mutating commands never take a diagnostic root or config: name what they act on instead. */
static int reject_diag_opts(int argc, char **argv)
{
	const char *env = getenv("DESTDIR");
	int install = strcmp(argv[1], "install") == 0, stage = install && env && env[0];
	int i;

	for (i = 2; i < argc && install; i++)
		if ((strncmp(argv[i], "--destdir=", 10) == 0 && argv[i][10]) ||
		    (strcmp(argv[i], "--destdir") == 0 && i + 1 < argc && argv[i + 1][0]))
			stage = 1;
	for (i = 2; i < argc; i++) {
		if (strncmp(argv[i], "--sysfs-root", 12) != 0 && strncmp(argv[i], "--config", 8) != 0)
			continue;
		if (stage)
			return bad("%s is for status only. Staging writes the config from --device and the switches\n"
				   "and checks no device; run `geforce-sriov install --dry-run` on the target host", argv[i]);
		return bad("%s is for status only; this command acts on the live system and its config",
			   argv[i]);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd;
	int i, r, matched;

	/*
	 * The CLI waits for its own children. A caller that ignores SIGCHLD passes that on through exec,
	 * and the kernel would then reap each child before the CLI could read its exit status.
	 */
	signal(SIGCHLD, SIG_DFL);

	if (argc < 2) {
		usage(stderr);
		return GS_EXIT_USAGE;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(stdout);
			return GS_EXIT_OK;
		}
	}
	cmd = argv[1];
	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
		printf("geforce-sriov %s\n", GS_VERSION);
		return GS_EXIT_OK;
	}
	if (strcmp(cmd, "help") == 0) {
		usage(stdout);
		return GS_EXIT_OK;
	}
	if (strcmp(cmd, "devices") == 0) {
		if (argc > 2)
			return unknown_opt(argv[2], 0);
		return gs_cmd_devices();
	}
	if (strcmp(cmd, "status") == 0)
		return status_main(argc, argv);
	if (strcmp(cmd, "check") == 0) {
		struct gs_activate_opts o;

		if ((r = parse_vgpu_opts(argc, argv, &o, 0)))
			return r;
		return gs_cmd_check(&o);
	}
	if (strcmp(cmd, "livepatch") == 0) {
		struct gs_livepatch_opts o;

		if ((r = parse_livepatch_opts(argc, argv, &o)))
			return r;
		return gs_cmd_livepatch(&o);
	}

	if ((r = reject_diag_opts(argc, argv)))
		return r;

	if (strcmp(cmd, "install") == 0) {
		struct gs_install_opts o;
		const char *env = getenv("DESTDIR");

		memset(&o, 0, sizeof(o));
		o.ov.ari = o.ov.msix = GS_SW_UNSET;
		o.destdir = env && env[0] ? env : NULL;
		for (i = 2; i < argc; i++) {
			const char *v;

			if ((r = parse_switch_opt(argc, argv, &i, &o.ov, &matched)))
				return r;
			if (matched)
				continue;
			if ((r = parse_package_opt(argc, argv, &i, &o, &matched)))
				return r;
			if (matched)
				continue;
			if ((v = opt_value(argc, argv, &i, "--device"))) {
				if ((r = parse_device("--device", v, o.devices, &o.ndevices)))
					return r;
				if (o.devices[o.ndevices - 1].func != 0)
					return bad("--device %s: select the physical function .0", v);
			} else if (strcmp(argv[i], "--dry-run") == 0) {
				o.dry_run = 1;
			} else if (strcmp(argv[i], "--from-setup") == 0) {
				/* setup's own mark on the install it runs; not a user option, so not in the usage text. */
				o.from_setup = 1;
			} else if ((v = opt_value(argc, argv, &i, "--prefix"))) {
				o.prefix = v;
			} else if ((v = opt_value(argc, argv, &i, "--destdir"))) {
				if (!v[0])
					return bad("%s needs a path (omit it for a live install)", "--destdir");
				o.destdir = v;
			} else if ((v = opt_value(argc, argv, &i, "--source"))) {
				if (gs_validate_abs_path(v))
					return bad("--source '%s' must be a normalized absolute path", v);
				o.source = v;
			} else if ((v = opt_value(argc, argv, &i, "--driver"))) {
				if (strcmp(v, "skip") != 0)
					return bad("--driver '%s': the only value is skip (leave NVIDIA's files untouched)", v);
				o.skip_driver = 1;
			} else if ((v = opt_value(argc, argv, &i, "--nvidia-source"))) {
				if (gs_validate_abs_path(v))
					return bad("--nvidia-source '%s' must be a normalized absolute path", v);
				o.nvidia_source = v;
			} else if ((v = opt_value(argc, argv, &i, "--vfs"))) {
				if (o.vfs)
					return bad("%s given twice", "--vfs");
				if (gs_parse_vfs(v, &o.vfs))
					return bad("--vfs '%s': give a number of VFs from 1 to %u (default %u)", v, GS_VFS_MAX,
						   GS_DEFAULT_VFS);
			} else {
				return unknown_opt(argv[i], 1);
			}
		}
		if ((r = check_install_opts(&o)))
			return r;
		if (o.dry_run && (o.destdir || o.prefix || o.source || o.vfs || o.from_setup || o.ov.ari != GS_SW_UNSET ||
				  o.ov.msix != GS_SW_UNSET))
			return bad("--dry-run checks this host for the selection, feature and package options of install; it takes no\n"
				   "--prefix, --destdir (or DESTDIR), --source, --vfs, --ari or --msix");
		if (o.allow_unsupported_driver) {
			fprintf(stderr, "geforce-sriov: --allow-unsupported-driver is ignored by install\n");
			o.allow_unsupported_driver = 0;
		}
		return gs_cmd_install(&o);
	}
	if (strcmp(cmd, "activate") == 0) {
		struct gs_activate_opts o;

		if ((r = parse_vgpu_opts(argc, argv, &o, 1)))
			return r;
		return gs_cmd_activate(&o);
	}
	if (strcmp(cmd, "setup") == 0) {
		struct gs_install_opts o;
		enum gs_prompt prompt = GS_PROMPT_AUTO;

		memset(&o, 0, sizeof(o));
		o.ov.ari = o.ov.msix = GS_SW_UNSET;
		for (i = 2; i < argc; i++) {
			const char *v;

			if ((r = parse_package_opt(argc, argv, &i, &o, &matched)))
				return r;
			if (matched)
				continue;
			if ((v = opt_value(argc, argv, &i, "--device"))) {
				if ((r = parse_device("--device", v, o.devices, &o.ndevices)))
					return r;
				if (o.devices[o.ndevices - 1].func != 0)
					return bad("--device %s: select the physical function .0", v);
			} else if ((v = opt_value(argc, argv, &i, "--vfs"))) {
				if (o.vfs)
					return bad("%s given twice", "--vfs");
				if (gs_parse_vfs(v, &o.vfs))
					return bad("--vfs '%s': give a number of VFs from 1 to %u (default %u)", v, GS_VFS_MAX,
						   GS_DEFAULT_VFS);
			} else if ((v = opt_value(argc, argv, &i, "--driver"))) {
				if (strcmp(v, "skip") != 0)
					return bad("--driver '%s': the only value is skip (leave NVIDIA's files untouched)", v);
				o.skip_driver = 1;
			} else if ((v = opt_value(argc, argv, &i, "--nvidia-source"))) {
				if (gs_validate_abs_path(v))
					return bad("--nvidia-source '%s' must be a normalized absolute path", v);
				o.nvidia_source = v;
			} else if (strcmp(argv[i], "--interactive") == 0 || strcmp(argv[i], "--no-prompt") == 0) {
				enum gs_prompt want = argv[i][2] == 'i' ? GS_PROMPT_ON : GS_PROMPT_OFF;

				if (prompt != GS_PROMPT_AUTO && prompt != want)
					return bad("--interactive asks and --no-prompt never does; give one");
				prompt = want;
			} else {
				return unknown_opt(argv[i], 1);
			}
		}
		if ((r = check_install_opts(&o)))
			return r;
		return gs_cmd_setup(&o, prompt);
	}
	if (strcmp(cmd, "rebuild") == 0) {
		const char *release = NULL;

		for (i = 2; i < argc; i++) {
			const char *v = opt_value(argc, argv, &i, "--kernel");

			if (!v)
				return unknown_opt(argv[i], 0);
			if (release)
				return bad("%s given twice", "--kernel");
			if (!v[0] || strchr(v, '/') || !strcmp(v, ".") || !strcmp(v, ".."))
				return bad("--kernel '%s': give a kernel release as /lib/modules names it", v);
			release = v;
		}
		return gs_cmd_rebuild(release);
	}
	if (strcmp(cmd, "uninstall") == 0) {
		int unload = 0;

		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--unload") == 0)
				unload = 1;
			else
				return unknown_opt(argv[i], 0);
		}
		return gs_cmd_uninstall(unload);
	}
	return bad("unknown command '%s'", cmd);
}
