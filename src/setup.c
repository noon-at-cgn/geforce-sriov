// SPDX-License-Identifier: MIT
/*
 * geforce-sriov: setup, the guided path from a clean host to a ready VF.
 *
 * The user runs one command. It reads the host's state and runs the next steps of a first install
 * (doctor, install) or of an existing install (doctor, install or migration when the state needs it,
 * the single activation transaction, check), each as a child process of this binary, and ends by
 * naming the guest drivers of each selected card. Attaching a VF to a VM stays the user's step.
 *
 * Before a first install it asks for what no flag supplied (NVIDIA's package, the licence, the GPUs,
 * and the D3cold consent of a laptop GPU) when standard input is a terminal or --interactive is
 * given, and prints the flags that repeat the run without questions. Without a terminal it asks
 * nothing. The version question is asked once, before any installation or runtime change; an
 * unsupported version needs a yes, and --no-prompt needs --allow-unsupported-driver. Stopping a
 * display manager needs a yes of its own, and --no-prompt never gives it.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "gs_lifecycle.h"

/* Resolved path of this binary, so every step runs the same file. */
static int self_path(char *out, size_t len)
{
	char buf[GS_PATH_MAX];

	if (!realpath("/proc/self/exe", buf)) {
		fprintf(stderr, "setup: cannot resolve this binary: %s\n", strerror(errno));
		return -1;
	}
	gs_copy(out, len, buf);
	return 0;
}

/* One step of this binary, run as a child; its exit status, or GS_EXIT_FAIL when it cannot run. */
#define STEP_MAX_ARGS (2 + 6 * GS_CFG_MAX_BDFS + 32)

static int step(const struct gs_host *h, const char *self, const char *cmd, const char *const extra[])
{
	const char *argv[STEP_MAX_ARGS];
	unsigned int n = 0, k;
	int r;

	argv[n++] = self;
	argv[n++] = cmd;
	for (k = 0; extra && extra[k] && n < STEP_MAX_ARGS - 1; k++)
		argv[n++] = extra[k];
	argv[n] = NULL;
	printf("\n== %s\n", cmd);
	fflush(stdout);
	r = h->run(argv);
	if (r < 0) {
		fprintf(stderr, "setup: cannot run %s: %s\n", cmd, strerror(-r));
		return GS_EXIT_FAIL;
	}
	return r;
}

/* Appends "--name" and value to argv; returns the new count. */
static unsigned int push_opt(const char *argv[], unsigned int n, const char *name, const char *value)
{
	argv[n++] = name;
	argv[n++] = value;
	return n;
}

/* The --device of a first install, for the doctor and install steps. */
static unsigned int push_devices(const struct gs_install_opts *o, const char *argv[], unsigned int n)
{
	for (unsigned int i = 0; i < o->ndevices; i++)
		n = push_opt(argv, n, "--device", o->devices[i].str);
	return n;
}

/*
 * doctor with the configuration this process read: children get a fixed environment, so a config
 * named by the environment would otherwise be lost between steps. o: this run's options, whose
 * selection, features and build inputs doctor judges as install will use them.
 */
static int step_doctor(const struct gs_host *h, const char *self, const struct gs_install_opts *o)
{
	const char *cfg = getenv(GS_CONFIG_ENV);
	const char *argv[STEP_MAX_ARGS];
	unsigned int n = 0;

	if (cfg && cfg[0])
		n = push_opt(argv, n, "--config", cfg);
	if (o && o->nvidia_source)
		n = push_opt(argv, n, "--nvidia-source", o->nvidia_source);
	if (o && o->driver_file)
		n = push_opt(argv, n, "--driver-file", o->driver_file);
	if (o)
		n = push_devices(o, argv, n);
	/* The D3cold consent the user chose (flag or yes) is judged as install will write it. */
	if (o && o->disable_d3cold)
		argv[n++] = "--disable-d3cold";
	/* The features and their build inputs, so doctor names a missing one before install starts. */
	if (o && o->display_set)
		argv[n++] = o->display == GS_DISPLAY_ON ? "--display" : "--no-display";
	if (o && o->host_cuda_set)
		argv[n++] = o->host_cuda ? "--host-cuda" : "--no-host-cuda";
	if (o && o->companion_file)
		n = push_opt(argv, n, "--companion-file", o->companion_file);
	if (o && o->companion_source)
		n = push_opt(argv, n, "--companion-source", o->companion_source);
	if (o && o->nvkms_source)
		n = push_opt(argv, n, "--nvkms-source", o->nvkms_source);
	/* --driver=skip builds nothing, so doctor asks for no build input. */
	if (o && o->skip_driver)
		argv[n++] = "--driver=skip";
	argv[n] = NULL;
	return step(h, self, "doctor", argv);
}

/* The generation of the card behind pf, from its sysfs device ID; -1 when it cannot be read. */
static int card_generation(const char *sys, const struct gs_addr *pf, unsigned int *generation, const char **name)
{
	char path[GS_PATH_MAX];
	const struct gs_device *d;
	unsigned int device;

	if (gs_pathf(path, sizeof(path), "%s/bus/pci/devices/%s/device", sys, pf->str))
		return -1;
	if (gs_read_hex(path, &device))
		return -1;
	d = gs_device_lookup(device);
	if (!d)
		return -1;
	*generation = d->generation;
	*name = d->name;
	return 0;
}

/* The guest drivers of one generation. */
static void guest_drivers(unsigned int generation)
{
	size_t count;
	const struct gs_confirmed_version *rows = gs_confirmed_versions(&count);

	for (size_t k = 0; k < count; k++) {
		const char *guests = generation == GS_AMPERE ? rows[k].guest_ampere :
				     generation == GS_ADA ? rows[k].guest_ada : NULL;

		if (!guests)
			continue;
		/* With one confirmed host driver the line needs no qualifier. */
		if (count > 1)
			printf("    %s (with host driver %s)\n", guests, rows[k].host);
		else
			printf("    %s\n", guests);
	}
}

/*
 * Ends every phase: what the guests need, per selected card, and where this package stops.
 * before_install has no selection yet, so both generations are named.
 */
static void closing(const char *sys, const struct gs_config *cfg, int before_install)
{
	unsigned int i;

	printf("\nGuest drivers:\n");
	if (before_install || !cfg || !cfg->nbdfs) {
		printf("  Ampere\n");
		guest_drivers(GS_AMPERE);
		printf("  Ada\n");
		guest_drivers(GS_ADA);
	} else {
		for (i = 0; i < cfg->nbdfs; i++) {
			unsigned int generation;
			const char *name;

			if (card_generation(sys, &cfg->bdfs[i], &generation, &name)) {
				printf("  %s\n    cannot read the card; for an Ampere card:\n", cfg->bdfs[i].str);
				guest_drivers(GS_AMPERE);
				printf("    for an Ada card:\n");
				guest_drivers(GS_ADA);
				continue;
			}
			printf("  %s %s\n", gs_generation_name(generation), name);
			guest_drivers(generation);
		}
	}
	printf("  Every guest driver needs RMSetVgpuGspPluginOffloadMode=0.\n"
	       "  Use these versions; other versions are not guaranteed to work.\n"
	       "  Setting and steps: docs/installation.md#guests.\n");
	printf("  Attaching a VF to a VM is your step: docs/installation.md#create-a-vm.\n");
}

/* ---- questions before a first install ------------------------------------ */

/* A card install can select; boot is its boot display mark. */
struct pick {
	struct gs_addr pf;
	const struct gs_device *e;
	int boot;
};

static int pick_cmp(const void *x, const void *y)
{
	return strcmp(((const struct pick *)x)->pf.str, ((const struct pick *)y)->pf.str);
}

/*
 * The NVIDIA physical functions in sys that install accepts: the first max by address. A boot
 * display card is selectable; activation handles its console as part of its own transaction.
 * *more counts the eligible ones left out.
 */
static unsigned int eligible_cards(const char *sys, struct pick *list, unsigned int max, unsigned int *more)
{
	char p[GS_PATH_MAX], why[256];
	unsigned int n = 0, cap = 0, vendor;
	struct pick *all = NULL;
	struct dirent *de;
	DIR *d;

	*more = 0;
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sys) || !(d = opendir(p)))
		return 0;
	while ((de = readdir(d))) {
		struct gs_addr a;
		struct gs_card c;

		if (gs_addr_parse(de->d_name, &a) || a.func != 0 ||
		    gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", sys, a.str) || gs_read_hex(p, &vendor) ||
		    vendor != GS_VENDOR_NVIDIA)
			continue;
		gs_card_inspect(sys, &a, &c);
		if (gs_check_selectable(&c, 1, why, sizeof(why)))
			continue;
		/* All of them first: which ones are listed must not depend on the directory's order. */
		if (n == cap) {
			struct pick *grown = realloc(all, (cap ? 2 * cap : 16) * sizeof(*all));

			if (!grown)
				break;
			all = grown;
			cap = cap ? 2 * cap : 16;
		}
		all[n].pf = a;
		all[n].e = c.entry;
		all[n].boot = c.boot_vga == 1;
		n++;
	}
	closedir(d);
	if (n)
		qsort(all, n, sizeof(all[0]), pick_cmp);
	if (n > max) {
		*more = n - max;
		n = max;
	}
	if (n)
		memcpy(list, all, n * sizeof(list[0]));
	free(all);
	return n;
}

/* One answer line from fd, without surrounding blanks; -1 at the end of input. */
static int read_answer(int fd, char *buf, size_t len)
{
	size_t n = 0, start = 0;
	ssize_t r;
	char c;

	for (;;) {
		r = read(fd, &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r != 1 || c == '\n')
			break;
		if (n + 1 < len)
			buf[n++] = c;
	}
	if (r != 1 && n == 0)
		return -1;
	while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';
	while (buf[start] == ' ' || buf[start] == '\t')
		start++;
	memmove(buf, buf + start, n - start + 1);
	return 0;
}

/* One question and its hint; -1 at the end of input, with what to give instead when flag is not NULL. */
static int ask(const struct gs_setup_host *s, const char *question, const char *hint, const char *flag, char *buf,
	       size_t len)
{
	printf("\n%s\n  %s\n> ", question, hint);
	fflush(stdout);
	if (read_answer(s->in, buf, len) == 0) {
		/* Piped answers are not echoed by a terminal: show each one, so the transcript reads as asked. */
		if (!isatty(s->in))
			printf("%s\n", buf);
		return 0;
	}
	printf("\n");
	if (flag)
		fprintf(stderr, "setup: end of input. Nothing changed. Use %s instead.\n", flag);
	return -1;
}

/* 1 yes, 0 no, -1 end of input. */
static int ask_yes_no(const struct gs_setup_host *s, const char *question, const char *hint, const char *flag)
{
	char a[64];

	for (;;) {
		if (ask(s, question, hint, flag, a, sizeof(a)))
			return -1;
		if (strcasecmp(a, "y") == 0 || strcasecmp(a, "yes") == 0)
			return 1;
		if (strcasecmp(a, "n") == 0 || strcasecmp(a, "no") == 0)
			return 0;
		printf("  answer yes or no\n");
	}
}

/* 1 yes, 0 no or an empty answer, -1 at the end of input. */
static int ask_default_no(const struct gs_setup_host *s, const char *question, const char *hint, const char *flag)
{
	char a[64];

	for (;;) {
		if (ask(s, question, hint, flag, a, sizeof(a)))
			return -1;
		if (!a[0] || strcasecmp(a, "n") == 0 || strcasecmp(a, "no") == 0)
			return 0;
		if (strcasecmp(a, "y") == 0 || strcasecmp(a, "yes") == 0)
			return 1;
		printf("  answer yes or no\n");
	}
}

/* No --driver-file, --nvidia-source or --driver=skip, and no NVIDIA tree or userspace on the host. */
static int needs_driver(const struct gs_setup_host *s, const struct gs_install_opts *a)
{
	return !a->driver_file && !a->nvidia_source && !a->skip_driver && !s->driver_present;
}

static int ask_driver(const struct gs_setup_host *s, struct gs_install_opts *a)
{
	static char real[PATH_MAX];
	char path[PATH_MAX], example[160];
	struct stat st;

	snprintf(example, sizeof(example), "an absolute path, e.g. /path/to/NVIDIA-Linux-x86_64-%s-vgpu-kvm.run",
		 gs_default_host_version());
	for (;;) {
		if (ask(s, "No NVIDIA driver found. Path to NVIDIA's vGPU host driver package (.run)?", example,
			"--driver-file PATH", path, sizeof(path)))
			return -1;
		if (!path[0])
			printf("  give the absolute path of the .run file\n");
		else if (path[0] != '/')
			printf("  %s is not an absolute path; start it with /\n", path);
		else if (!realpath(path, real) || stat(real, &st) || !S_ISREG(st.st_mode))
			printf("  %s is not an existing file\n", path);
		else
			break;
	}
	a->driver_file = real;
	printf("  = --driver-file %s\n", real);
	return 0;
}

static int ask_license(const struct gs_setup_host *s, struct gs_install_opts *a)
{
	int r = ask_yes_no(s, "Accept NVIDIA's licence and run NVIDIA's installer from this package? [y/n]",
			   "yes runs `sh PACKAGE --silent --dkms --skip-module-load --kernel-module-type=proprietary`; no only extracts it",
			   "--accept-nvidia-license");

	if (r < 0)
		return -1;
	a->accept_nvidia_license = r;
	if (r)
		printf("  = --accept-nvidia-license\n");
	else
		printf("  = no --accept-nvidia-license: install extracts the package and stops before it builds NVIDIA's module.\n"
		       "    Install NVIDIA's vGPU userspace with NVIDIA's installer yourself, then run setup again.\n");
	return 0;
}

/* "all", "N" or "N,M,...": 0 with chosen[] set; -1 for anything else. */
static int parse_choice(const char *ans, unsigned int n, int *chosen)
{
	const char *p = ans;
	unsigned int i, count = 0;

	memset(chosen, 0, n * sizeof(chosen[0]));
	if (strcasecmp(ans, "all") == 0) {
		for (i = 0; i < n; i++)
			chosen[i] = 1, count++;
		return count ? 0 : -1;
	}
	for (;;) {
		unsigned long v;
		char *end;

		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			return -1;
		errno = 0;
		v = strtoul(p, &end, 10);
		if (errno || v < 1 || v > n || chosen[v - 1])
			return -1;
		chosen[v - 1] = 1;
		for (p = end; *p == ' '; p++)
			;
		if (!*p)
			return 0;
		if (*p++ != ',')
			return -1;
	}
}

static int ask_devices(const struct gs_setup_host *s, struct gs_install_opts *a, const struct pick *list, unsigned int n,
		       unsigned int more)
{
	int chosen[GS_CFG_MAX_BDFS], left = 0;
	char ans[256];
	unsigned int i;

	printf("\nGPUs install can select:\n");
	for (i = 0; i < n; i++)
		printf("  %u  %s  %s (%s)%s\n", i + 1, list[i].pf.str, list[i].e->name,
		       gs_generation_name(list[i].e->generation), list[i].boot ? "  boot display" : "");
	if (more)
		printf("  %u more not listed (at most %u); name them with --device BDF\n", more, GS_CFG_MAX_BDFS);
	for (;;) {
		if (ask(s, "Which GPUs? all, a number, or numbers separated by commas",
			"all selects every GPU listed; a boot display GPU needs no separate consent", "--device BDF", ans,
			sizeof(ans)))
			return -1;
		if (parse_choice(ans, n, chosen) == 0)
			break;
		printf("  answer all, or numbers from 1 to %u\n", n);
	}
	for (i = 0; i < n; i++)
		left += chosen[i];
	if (!left) {
		fprintf(stderr, "setup: no GPU left to select. Nothing changed.\n");
		return -1;
	}
	printf("  =");
	for (i = 0; i < n; i++)
		if (chosen[i]) {
			a->devices[a->ndevices++] = list[i].pf;
			printf(" --device %s", list[i].pf.str);
		}
	printf("\n");
	return 0;
}

/*
 * The D3cold consent for the laptop GPUs among the selection sel (nsel GPUs), unless the config
 * already has it (have): *flag is --disable-d3cold, given, or set by a yes to the question that
 * follows the notice (asked when s has input). 0 to go on; GS_EXIT_USAGE for the flag without a
 * laptop GPU; GS_EXIT_REFUSED, nothing changed, on a no, without a question, or at the end of
 * input. Each says why.
 */
static int ask_d3cold(const struct gs_setup_host *s, const struct gs_addr *sel, unsigned int nsel, int have, int *flag)
{
	struct gs_addr laptops[GS_CFG_MAX_BDFS];
	unsigned int n = gs_laptop_gpus(s->h->sys, sel, nsel, laptops);
	char list[GS_CFG_MAX_BDFS * (GS_BDF_STR_SIZE + 2)] = "", question[sizeof(list) + 80], why[GS_D3COLD_WHY_SIZE];
	int r;

	if (*flag && !n)
		return gs_d3cold_usage();
	if (!n || *flag || have)
		return 0;
	printf("\n");
	gs_d3cold_notice(stdout);
	if (s->in >= 0) {
		for (unsigned int i = 0; i < n; i++) {
			if (i)
				strcat(list, ", ");
			strcat(list, laptops[i].str);
		}
		snprintf(question, sizeof(question), "Turn off D3cold for %s while geforce-sriov manages it? [y/N]", list);
		r = ask_default_no(s, question,
				   "Yes: save d3cold=off in the config. activate keeps this GPU powered each time it\n"
				   "  brings the GPUs up.\n"
				   "  No: activation refuses this GPU. Default is no.", NULL);
		if (r < 0) {
			fprintf(stderr, "setup: end of input. Nothing changed. Use --disable-d3cold for a non-interactive run.\n");
			return GS_EXIT_REFUSED;
		}
		printf("  = %s\n", r ? "--disable-d3cold" : "no --disable-d3cold");
		if (r) {
			*flag = 1;
			return 0;
		}
		/* The notice follows a no too, then why setup stops. */
		printf("\n");
		gs_d3cold_notice(stdout);
	}
	fflush(stdout);
	gs_d3cold_why(why, sizeof(why), laptops, n, sel, nsel, NULL, 0, "");
	fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", why);
	return GS_EXIT_REFUSED;
}

/* Appends one argument to buf, quoted for a POSIX shell when it needs it. */
static void flag_add(char *buf, size_t len, const char *arg)
{
	static const char safe[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./=:,+@%";
	size_t n = strlen(buf);

	if (arg[0] && arg[strspn(arg, safe)] == '\0') {
		snprintf(buf + n, len - n, " %s", arg);
		return;
	}
	snprintf(buf + n, len - n, " '");
	for (; *arg; arg++) {
		n = strlen(buf);
		if (*arg == '\'')
			snprintf(buf + n, len - n, "'\\''");
		else
			snprintf(buf + n, len - n, "%c", *arg);
	}
	n = strlen(buf);
	snprintf(buf + n, len - n, "'");
}

/* 1 when install would build the display modules for this selection, with the reason in why (gs_display_effective). */
static int display_build(const struct gs_setup_host *s, const struct gs_install_opts *a, char *why, size_t len)
{
	struct gs_config cfg;
	unsigned int i;

	gs_config_defaults(&cfg);
	for (i = 0; i < a->ndevices; i++)
		cfg.bdfs[i] = a->devices[i];
	cfg.nbdfs = a->ndevices;
	cfg.display = a->display_set ? a->display : GS_DISPLAY_AUTO;
	return gs_display_effective(s->h->sys, &cfg, why, len);
}

/*
 * An existing absolute path, asked until one is given: 1 with its real path in out (PATH_MAX),
 * 0 for the answer `none` when that is allowed, -1 at the end of input.
 */
static int ask_path(const struct gs_setup_host *s, const char *question, const char *hint, int allow_none, char *out)
{
	char path[PATH_MAX];

	for (;;) {
		if (ask(s, question, hint, NULL, path, sizeof(path)))
			return -1;
		if (allow_none && strcasecmp(path, "none") == 0)
			return 0;
		if (!path[0])
			printf("  give an absolute path%s\n", allow_none ? ", or none" : "");
		else if (path[0] != '/')
			printf("  %s is not an absolute path; start it with /\n", path);
		else if (!realpath(path, out))
			printf("  %s does not exist\n", path);
		else
			return 1;
	}
}

/* 1 when an earlier install on this host kept the input name; a fixture host has none. */
static int input_kept(const struct gs_setup_host *s, const char *name)
{
	return strcmp(s->h->sys, GS_LIVE_SYSFS) == 0 && gs_kept_input(GS_INPUTS_DIR, name);
}

/* The row of the host version the selected input names when that is a confirmed one, else the first row. */
static const struct gs_confirmed_version *input_row(const struct gs_install_opts *a)
{
	size_t count;
	const struct gs_confirmed_version *rows = gs_confirmed_versions(&count);
	char version[64];

	if (!gs_install_version_named(a, version, sizeof(version)))
		for (size_t i = 0; i < count; i++)
			if (strcmp(rows[i].host, version) == 0)
				return &rows[i];
	return &rows[0];
}

/*
 * The optional features, asked after the GPUs are known: host CUDA (default no), then the inputs
 * the display and host CUDA builds need. The end of input leaves a question unanswered, and doctor
 * then names what is missing.
 */
static void ask_features(const struct gs_setup_host *s, struct gs_install_opts *a)
{
	static char companion[PATH_MAX], nvkms[PATH_MAX];
	const struct gs_confirmed_version *row = input_row(a);
	/* A row may record no second package; its release is then the one NVIDIA pairs with the host driver. */
	const char *second = row->second ? row->second : "of the matching release";
	char why[256], hint[320];
	struct stat st;
	int display, cuda, r;

	if (a->skip_driver)
		return;
	if (!a->host_cuda_set) {
		r = ask_default_no(s, "Build NVIDIA's CUDA module for the host too, so CUDA programs can run on the host? [y/N]",
				   "host CUDA is optional and needs NVIDIA's second package; the default is no", NULL);
		if (r < 0)
			return;
		if (r) {
			a->host_cuda_set = 1;
			a->host_cuda = 1;
			printf("  = --host-cuda\n");
		}
	}
	display = display_build(s, a, why, sizeof(why));
	cuda = a->host_cuda_set && a->host_cuda;
	/* An input an earlier install kept is used again, so it is not asked for. */
	if ((display || cuda) && !a->companion_file && !a->companion_source && !input_kept(s, "companion")) {
		snprintf(hint, sizeof(hint),
			 "the Linux guest driver package (.run) from NVIDIA's vGPU download, or NVIDIA's GeForce driver package (.run),\n"
			 "  version %s; an extracted directory works too. Answer none to leave these modules out", second);
		r = ask_path(s, display ? "A selected GPU is the boot display, so the modules for the host console are built.\n"
					  "Path to NVIDIA's second package?"
					: "Host CUDA needs NVIDIA's second package. Path to it?",
			     hint, 1, companion);
		if (r < 0)
			return;
		if (!r) {
			if (display) {
				a->display_set = 1;
				a->display = GS_DISPLAY_OFF;
				printf("  = --no-display\n");
			}
			if (cuda) {
				a->host_cuda = 0;
				printf("  = --no-host-cuda\n");
			}
			return;
		}
		if (stat(companion, &st) == 0 && S_ISDIR(st.st_mode)) {
			a->companion_source = companion;
			printf("  = --companion-source %s\n", companion);
		} else {
			a->companion_file = companion;
			printf("  = --companion-file %s\n", companion);
		}
	}
	if (display && !a->nvkms_source && !input_kept(s, "nvkms")) {
		snprintf(hint, sizeof(hint), "NVIDIA's open-gpu-kernel-modules source, version %s, as a directory or a tarball",
			 row->public ? row->public : second);
		if (ask_path(s, "Path to NVIDIA's public kernel module source?", hint, 0, nvkms) <= 0)
			return;
		a->nvkms_source = nvkms;
		printf("  = --nvkms-source %s\n", nvkms);
	}
}

/* The command line that repeats this run without questions. */
static void flag_set(const struct gs_install_opts *a, int allow_unsupported, char *buf, size_t len)
{
	char opt[64];
	unsigned int i;

	gs_copy(buf, len, "geforce-sriov setup --no-prompt");
	if (a->driver_file) {
		flag_add(buf, len, "--driver-file");
		flag_add(buf, len, a->driver_file);
	}
	if (a->accept_nvidia_license)
		flag_add(buf, len, "--accept-nvidia-license");
	for (i = 0; i < a->ndevices; i++) {
		flag_add(buf, len, "--device");
		flag_add(buf, len, a->devices[i].str);
	}
	if (a->display_set)
		flag_add(buf, len, a->display == GS_DISPLAY_ON ? "--display" : "--no-display");
	if (a->host_cuda_set)
		flag_add(buf, len, a->host_cuda ? "--host-cuda" : "--no-host-cuda");
	if (a->companion_file) {
		flag_add(buf, len, "--companion-file");
		flag_add(buf, len, a->companion_file);
	}
	if (a->companion_source) {
		flag_add(buf, len, "--companion-source");
		flag_add(buf, len, a->companion_source);
	}
	if (a->nvkms_source) {
		flag_add(buf, len, "--nvkms-source");
		flag_add(buf, len, a->nvkms_source);
	}
	if (allow_unsupported)
		flag_add(buf, len, "--allow-unsupported-driver");
	if (a->disable_d3cold)
		flag_add(buf, len, "--disable-d3cold");
	if (a->vfs) {
		snprintf(opt, sizeof(opt), "--vfs=%u", a->vfs);
		flag_add(buf, len, opt);
	}
	if (a->skip_driver)
		flag_add(buf, len, "--driver=skip");
	if (a->nvidia_source) {
		flag_add(buf, len, "--nvidia-source");
		flag_add(buf, len, a->nvidia_source);
	}
}

/* What a first install runs with, before anything runs. */
static void summary(const struct gs_setup_host *s, const struct gs_install_opts *a, unsigned int ncards)
{
	char flags[4 * PATH_MAX + 1024], version[64], why[256];
	unsigned int i;
	int allow = a->allow_unsupported_driver, display;

	printf("\nSetup runs with:\n  driver:       ");
	if (a->driver_file)
		printf("%s (%s)\n", a->driver_file,
		       a->accept_nvidia_license ? "NVIDIA's installer runs" : "extract only");
	else if (a->skip_driver)
		printf("NVIDIA's files left untouched (--driver=skip)\n");
	else if (a->nvidia_source)
		printf("NVIDIA's kernel tree at %s\n", a->nvidia_source);
	else
		printf("%s\n", s->driver_present ? "the NVIDIA driver found on this host" : "none (doctor names what is missing)");
	printf("  GPUs:         ");
	for (i = 0; i < a->ndevices; i++)
		printf("%s%s%s", i ? ", " : "", a->devices[i].str, i + 1 == a->ndevices ? "\n" : "");
	if (!a->ndevices)
		printf("%s\n", ncards ? "none selected" : "none eligible (doctor names why)");
	if (!gs_install_version_named(a, version, sizeof(version)) && !gs_version_confirmed(GS_VERSION_HOST, version))
		allow = 1;
	/* The display and host-CUDA features are recorded by install; display follows the boot display card. */
	display = display_build(s, a, why, sizeof(why));
	printf("  display:      %s\n", display ? "enabled" : "not enabled");
	/* --display that builds nothing says why, so the summary does not contradict the flag. */
	if (!display && a->display_set && a->display == GS_DISPLAY_ON)
		printf("    %s\n", why);
	printf("  host CUDA:    %s\n", a->host_cuda_set && a->host_cuda ? "enabled" : "disabled");
	flag_set(a, allow, flags, sizeof(flags));
	printf("  the same run without questions:\n    %s\n", flags);
}

/* No --device and exactly one eligible card: that card. */
static void select_single(struct gs_install_opts *a, const struct pick *cards, unsigned int ncards, unsigned int more)
{
	if (!a->ndevices && ncards == 1 && !more) {
		a->devices[a->ndevices++] = cards[0].pf;
		printf("The one eligible GPU, %s, is selected: --device %s\n", cards[0].pf.str, cards[0].pf.str);
	}
}

/*
 * Each question no flag answered, in this order: NVIDIA's package and licence, the GPUs, then the
 * D3cold consent of a laptop GPU, which needs the final selection. GS_EXIT_REFUSED when the input
 * ends first or the consent is refused, GS_EXIT_USAGE as ask_d3cold.
 */
static int ask_missing(const struct gs_setup_host *s, struct gs_install_opts *a, const struct pick *cards,
		       unsigned int ncards, unsigned int more)
{
	int r;

	if (needs_driver(s, a) && ask_driver(s, a))
		return GS_EXIT_REFUSED;
	if (a->driver_file && !a->accept_nvidia_license && ask_license(s, a))
		return GS_EXIT_REFUSED;
	if (!a->ndevices && ncards > 1 && ask_devices(s, a, cards, ncards, more))
		return GS_EXIT_REFUSED;
	select_single(a, cards, ncards, more);
	if ((r = ask_d3cold(s, a->devices, a->ndevices, 0, &a->disable_d3cold)))
		return r;
	ask_features(s, a);
	summary(s, a, ncards + more);
	return GS_EXIT_OK;
}

/* Without questions: the flags that would let the run continue, when doctor stopped it. */
static void missing_flags(const struct gs_setup_host *s, const struct gs_install_opts *o, unsigned int ncards)
{
	char need[160] = "", why[256];
	int display = !o->skip_driver && display_build(s, o, why, sizeof(why));
	int cuda = !o->skip_driver && o->host_cuda_set && o->host_cuda;

	if (needs_driver(s, o))
		strcat(need, " --driver-file PATH");
	if (o->driver_file && !o->accept_nvidia_license)
		strcat(need, " --accept-nvidia-license");
	if (!o->ndevices && ncards > 1)
		strcat(need, " --device BDF");
	if ((display || cuda) && !o->companion_file && !o->companion_source && !input_kept(s, "companion"))
		strcat(need, " --companion-file PATH");
	if (display && !o->nvkms_source && !input_kept(s, "nvkms"))
		strcat(need, " --nvkms-source PATH");
	if (need[0])
		printf("To continue, add:%s\n"
		       "(or run setup on a terminal, or with --interactive, to be asked)\n", need);
}

/* ---- setup ------------------------------------------------------------------- */

/* --device on an installed host: exactly the configured GPUs, or none. 0, or GS_EXIT_REFUSED with why printed. */
static int same_devices(const struct gs_install_opts *o, const struct gs_config *cfg, const char *path)
{
	char bdfs[GS_BDF_LIST_STR_SIZE];
	unsigned int i, k;

	gs_addr_list_str(cfg->bdfs, cfg->nbdfs, bdfs, sizeof(bdfs));
	for (i = 0; i < o->ndevices; i++) {
		for (k = 0; k < cfg->nbdfs && strcmp(cfg->bdfs[k].str, o->devices[i].str) != 0; k++)
			;
		if (k == cfg->nbdfs) {
			fprintf(stderr, "setup: --device %s is not a configured GPU (bdfs=%s in %s). On an installed host\n"
				"setup acts on every configured GPU: name exactly those with --device, or leave it out.\n",
				o->devices[i].str, bdfs, path);
			return GS_EXIT_REFUSED;
		}
	}
	if (o->ndevices && o->ndevices != cfg->nbdfs) {
		fprintf(stderr, "setup: --device names %u of the %u configured GPUs (bdfs=%s in %s). On an installed host\n"
			"setup acts on every configured GPU: name exactly those with --device, or leave it out.\n",
			o->ndevices, cfg->nbdfs, bdfs, path);
		return GS_EXIT_REFUSED;
	}
	return 0;
}

/* NVIDIA's driver is loaded: its version file exists, as activation reads it. */
static int nvidia_loaded(const struct gs_host *h)
{
	char p[GS_PATH_MAX];

	return !gs_pathf(p, sizeof(p), "%s/driver/nvidia/version", h->proc) && gs_exists(p);
}

/* "\nStop here: ...", and code, for the first step that failed. */
static int stopped(const char *what, int code)
{
	printf("\nStopped: %s exited with %d.\n", what, code);
	return code;
}

/*
 * The version this host installs or runs: the one the selected input's name states, else the one
 * the installed build record names. -1 when neither is knowable.
 */
static int setup_version(const struct gs_install_opts *o, char *out, size_t len)
{
	struct gs_build b;
	struct utsname u;
	char module[GS_PATH_MAX] = "";
	const char *release;

	if ((o->driver_file || o->nvidia_source) && !gs_install_version_named(o, out, len))
		return 0;
	release = uname(&u) == 0 ? u.release : "";
	if (gs_nvidia_module_path(release, module, sizeof(module)))
		module[0] = '\0';
	if (gs_driver_build_read(GS_DRIVER_BUILD, release, module, &b) == GS_BUILD_ABSENT || !b.driver_version[0])
		return -1;
	gs_copy(out, len, b.driver_version);
	return 0;
}

/*
 * The version question, asked once before any installation or runtime change: an unsupported
 * version needs a yes, an empty answer or the end of input is no, and --no-prompt refuses unless
 * --allow-unsupported-driver was given. NVIDIA's open flavour is refused first, and the flag never
 * overrides that. 0 to go on, or GS_EXIT_REFUSED with the reason printed.
 */
static int version_consent(const struct gs_setup_host *s, const struct gs_install_opts *o)
{
	char version[64], line[512], prompt[768], hint[768];
	int r;

	if (setup_version(o, version, sizeof(version)))
		return GS_EXIT_OK;
	if (gs_nvidia_open_flavour(o)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: NVIDIA's open flavour cannot be used by this build. "
			"--allow-unsupported-driver does not override this check.\n");
		return GS_EXIT_REFUSED;
	}
	if (gs_version_confirmed(GS_VERSION_HOST, version)) {
		gs_version_format(GS_VERSION_MSG_CONFIRMED, version, line, sizeof(line));
		printf("%s\n", line);
		return GS_EXIT_OK;
	}
	gs_version_format(GS_VERSION_MSG_SETUP_REFUSAL, version, line, sizeof(line));
	if (o->allow_unsupported_driver) {
		gs_version_format(GS_VERSION_MSG_BEST_EFFORT, version, line, sizeof(line));
		printf("%s\n", line);
		return GS_EXIT_OK;
	}
	if (s->in < 0) {
		fprintf(stderr, "%s\n", line);
		return GS_EXIT_REFUSED;
	}
	gs_version_format(GS_VERSION_MSG_PROMPT, version, prompt, sizeof(prompt));
	gs_version_format(GS_VERSION_MSG_BEST_EFFORT, version, hint, sizeof(hint));
	r = ask_default_no(s, prompt, hint, "--allow-unsupported-driver");
	if (r < 0)
		return GS_EXIT_REFUSED;
	if (!r) {
		fprintf(stderr, "%s\n", line);
		return GS_EXIT_REFUSED;
	}
	gs_version_format(GS_VERSION_MSG_BEST_EFFORT, version, line, sizeof(line));
	printf("  %s\n", line);
	return GS_EXIT_OK;
}

/* 1 when the build record proves the installed module carries the requested features. */
static int features_recorded(const struct gs_config *cfg, int display)
{
	struct gs_build b;
	struct utsname u;
	char module[GS_PATH_MAX] = "";
	const char *release = uname(&u) == 0 ? u.release : "";

	if (gs_nvidia_module_path(release, module, sizeof(module)))
		module[0] = '\0';
	if (gs_driver_build_read(GS_DRIVER_BUILD, release, module, &b) != GS_BUILD_MATCHES)
		return 0;
	if (b.display < 0 || b.host_cuda < 0)
		return 0;
	return b.display == display && b.host_cuda == (cfg->host_cuda == GS_SW_ON);
}

/* 1 when a 0.2.x artifact is still on the host and install has not migrated it yet. */
static int migration_pending(void)
{
	char p[GS_PATH_MAX];

	if (!gs_pathf(p, sizeof(p), "%s/%s", GS_UNIT_DIR, GS_LEGACY_UNIT_PREPARE) && gs_exists(p))
		return 1;
	if (gs_exists(GS_LEGACY_HOLD))
		return 1;
	return gs_exists(GS_LEGACY_DEFERRED);
}

/* The install step of this run, with the feature selection and the D3cold consent it should write. */
static int run_install(const struct gs_setup_host *s, const char *self, const struct gs_install_opts *o, int d3cold)
{
	const char *argv[STEP_MAX_ARGS];
	const char *destdir = getenv("DESTDIR");
	char vfs[16];
	unsigned int n = 0;

	/* Children run with a fixed environment, so a staged run must name its target explicitly. */
	if (destdir && destdir[0])
		n = push_opt(argv, n, "--destdir", destdir);
	for (unsigned int i = 0; i < o->ndevices; i++)
		n = push_opt(argv, n, "--device", o->devices[i].str);
	if (o->vfs) {
		snprintf(vfs, sizeof(vfs), "%u", o->vfs);
		n = push_opt(argv, n, "--vfs", vfs);
	}
	if (o->display_set)
		argv[n++] = o->display == GS_DISPLAY_ON ? "--display" : "--no-display";
	if (o->host_cuda_set)
		argv[n++] = o->host_cuda ? "--host-cuda" : "--no-host-cuda";
	if (o->companion_file)
		n = push_opt(argv, n, "--companion-file", o->companion_file);
	if (o->companion_source)
		n = push_opt(argv, n, "--companion-source", o->companion_source);
	if (o->nvkms_source)
		n = push_opt(argv, n, "--nvkms-source", o->nvkms_source);
	if (o->driver_file)
		n = push_opt(argv, n, "--driver-file", o->driver_file);
	if (o->accept_nvidia_license)
		argv[n++] = "--accept-nvidia-license";
	if (o->skip_driver)
		argv[n++] = "--driver=skip";
	if (o->nvidia_source)
		n = push_opt(argv, n, "--nvidia-source", o->nvidia_source);
	if (o->ov.ari != GS_SW_UNSET)
		argv[n++] = o->ov.ari == GS_SW_ON ? "--ari=on" : "--ari=off";
	if (o->ov.msix != GS_SW_UNSET)
		argv[n++] = o->ov.msix == GS_SW_ON ? "--msix=on" : "--msix=off";
	if (d3cold)
		argv[n++] = "--disable-d3cold";
	/* setup gives the closing instructions; install then prints none of its own. */
	argv[n++] = "--from-setup";
	argv[n] = NULL;
	return step(s->h, self, "install", argv);
}

/*
 * The programs that use the selected GPUs, and the display-manager consent when one is active and
 * the display build is part of the selection: a plain build never touches a manager on another GPU,
 * so asking there would stop nothing. *consent is 1 only after a yes. 0 to go on;
 * GS_EXIT_REFUSED, with the commands, when inspection is unknown, when there is no question, or
 * when the answer is no.
 */
static int display_consent(const struct gs_setup_host *s, const struct gs_config *cfg, int *consent)
{
	struct gs_display_plan plan = { 0 };
	char question[512], hint[1024], why[128];
	const char *unit;
	int r;

	*consent = 0;
	if (gs_display_plan_build(s->h, cfg, &plan)) {
		fprintf(stderr, "setup: cannot inspect the programs that use the selected GPUs: %s\n"
			"No GPU state was changed.\n", plan.reason[0] ? plan.reason : "an unreadable source");
		return GS_EXIT_REFUSED;
	}
	if (plan.inspection_unknown) {
		fprintf(stderr, "setup: cannot tell which programs use the selected GPUs: %s\nNo GPU state was changed.\n",
			plan.reason[0] ? plan.reason : "an unreadable source");
		return GS_EXIT_REFUSED;
	}
	if (!plan.manager_active)
		return 0;
	/* Asked only when the display build is part of the selection. With no selected boot display GPU the
	 * plan above finds no manager on a selected card and has already returned. */
	if (!gs_display_effective(s->h->sys, cfg, why, sizeof(why)))
		return 0;
	unit = plan.manager_unit[0] ? plan.manager_unit : "display-manager.service";
	snprintf(question, sizeof(question), "Stop %s temporarily and bring the GPUs up now? [y/N]", unit);
	snprintf(hint, sizeof(hint),
		 "This ends graphical sessions using these GPUs. Save your work first.\n"
		 "  Setup stops %s, runs the activation, and starts it again afterwards.\n"
		 "  Default is no.", unit);
	if (s->in < 0) {
		fprintf(stderr, "setup: %s holds a selected GPU. --no-prompt does not stop graphical sessions.\n"
			"Save your work, then run as root:\n"
			"  systemctl stop %s\n"
			"  geforce-sriov activate\n"
			"  systemctl start %s\n", unit, unit, unit);
		return GS_EXIT_REFUSED;
	}
	r = ask_default_no(s, question, hint, NULL);
	if (r < 0) {
		fprintf(stderr, "setup: stopping %s needs a yes. To do it yourself, as root:\n"
			"  systemctl stop %s\n  geforce-sriov activate\n  systemctl start %s\n", unit, unit, unit);
		return GS_EXIT_REFUSED;
	}
	if (!r) {
		printf("No GPU state was changed. To bring the GPUs up yourself, as root:\n"
		       "  systemctl stop %s\n  geforce-sriov activate\n  systemctl start %s\n", unit, unit);
		return GS_EXIT_REFUSED;
	}
	*consent = 1;
	return 0;
}

/*
 * The one activation transaction, then check: the first that fails ends the run with its exit
 * status. The display-manager consent travels with it, so activation stops and restarts only the
 * manager the user agreed to.
 */
static int activate_now(const struct gs_host *h, const char *self, const struct gs_config *cfg, int consent)
{
	static const char *const boot[] = { "--boot", NULL };
	static const char *const boot_consent[] = { "--boot", "--stop-display-manager", NULL };
	int r;

	r = step(h, self, "activate", consent ? boot_consent : boot);
	if (r != GS_EXIT_OK)
		return stopped("activate", r);
	r = step(h, self, "check", NULL);
	closing(h->sys, cfg, 0);
	return r;
}

/* After the installation: the display consent, the activation and check. */
static int finish(const struct gs_setup_host *s, const char *self, const struct gs_config *cfg)
{
	int consent, r;

	if ((r = display_consent(s, cfg, &consent)))
		return r;
	return activate_now(s->h, self, cfg, consent);
}

/* A host with a config: install or migrate what the request needs, then activate. */
static int installed_host(const struct gs_setup_host *s, const char *self, const char *path,
			  const struct gs_install_opts *o)
{
	const struct gs_host *h = s->h;
	const char *destdir = getenv("DESTDIR");
	struct gs_config cfg;
	char err[512], why[256];
	int r, ready = 0, need_install, consent_new, display, wrote = 0, d3cold;

	/* Once, before any installation or runtime change. */
	if ((r = version_consent(s, o)))
		return r;
	/*
	 * A staged run describes another host: install and migrate below DESTDIR and stop there.
	 * Nothing on this host is inspected, activated or restarted.
	 */
	if (destdir && destdir[0]) {
		printf("Staged run below %s: this host is not inspected and nothing here is activated.\n", destdir);
		return run_install(s, self, o, o->disable_d3cold);
	}
	if (gs_config_load(path, &cfg, err, sizeof(err))) {
		/* A 0.2.x package-owned config is migrated by install, which owns the file's history. */
		if (!strstr(err, "boot_display_release")) {
			fprintf(stderr, "setup: %s: %s\n", path, err);
			return GS_EXIT_REFUSED;
		}
		printf("The config holds a 0.2.x key; install migrates it.\n");
		if ((r = run_install(s, self, o, 0)))
			return r;
		wrote = 1;
		if (gs_config_load(path, &cfg, err, sizeof(err))) {
			fprintf(stderr, "setup: %s: %s\n", path, err);
			return GS_EXIT_REFUSED;
		}
	}
	if ((r = same_devices(o, &cfg, path)))
		return r;
	/*
	 * The D3cold consent first: activation refuses a laptop GPU without it, and install writes it.
	 * Given by flag or by a yes, it is part of this run's installation.
	 */
	d3cold = o->disable_d3cold;
	if ((r = ask_d3cold(s, cfg.bdfs, cfg.nbdfs, cfg.d3cold_off, &d3cold)))
		return r;
	consent_new = d3cold && !cfg.d3cold_off;

	r = step(h, self, "check", NULL);
	if (r == GS_EXIT_OK) {
		ready = 1;
	} else if (r != GS_EXIT_FAIL) {
		printf("\nStopped: check refused (exit %d). %s\n", r,
		       wrote ? "The install above completed; no GPU state changed." : "Nothing changed.");
		return r;
	}
	display = gs_display_effective(h->sys, &cfg, why, sizeof(why));
	if (!display && cfg.display == GS_DISPLAY_ON)
		printf("%s\n", why);
	need_install = !ready || !features_recorded(&cfg, display) || migration_pending() ||
		       (o->display_set && o->display != cfg.display) ||
		       (o->host_cuda_set && (cfg.host_cuda == GS_SW_ON) != o->host_cuda) ||
		       (o->vfs && o->vfs != cfg.vfs) ||
		       (o->ov.ari != GS_SW_UNSET && o->ov.ari != cfg.ari) ||
		       (o->ov.msix != GS_SW_UNSET && o->ov.msix != cfg.msix);
	if (!need_install) {
		/* Only the config changes: NVIDIA's files and the runtime state stay as they are. */
		if (consent_new) {
			struct gs_install_opts cfgonly = *o;

			cfgonly.skip_driver = 1;
			if ((r = run_install(s, self, &cfgonly, 1)))
				return r;
		}
		printf("\nEvery selected VF is ready. Nothing to bring up.\n");
		closing(h->sys, &cfg, 0);
		return GS_EXIT_OK;
	}
	r = step_doctor(h, self, o);
	if (r != GS_EXIT_OK) {
		printf("\nStopped: fix what doctor names above, then run setup again. %s\n",
		       wrote ? "The install above completed; no GPU state changed." : "Nothing changed.");
		return r;
	}
	if ((r = run_install(s, self, o, consent_new)))
		return r;
	if (gs_config_load(path, &cfg, err, sizeof(err))) {
		fprintf(stderr, "setup: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	/* A stale running core is not made current by installing: activation would work on the old one. */
	if (gs_loaded_not_installed(h->sys, s->build)) {
		printf("\nThe install is complete. The nvidia module that is running is not the installed build, so the GPUs were\n"
		       "not brought up.\nNext: power the machine off and on. The boot units bring the GPUs up; `sudo geforce-sriov setup`\n"
		       "then checks the result.\n");
		return GS_EXIT_OK;
	}
	return finish(s, self, &cfg);
}

/* A host with no config: the questions, doctor, install, then the first activation when it can run. */
static int first_install(const struct gs_setup_host *s, const char *self, const char *path, const struct gs_install_opts *o)
{
	const struct gs_host *h = s->h;
	const char *destdir = getenv("DESTDIR");
	int staged = destdir && destdir[0];
	struct gs_install_opts a = *o;
	struct pick cards[GS_CFG_MAX_BDFS];
	struct gs_config cfg;
	unsigned int ncards = 0, more = 0;
	char err[512];
	int r;

	printf("This host is not set up.\n");
	/* A staged install describes another host: nothing here to ask about. */
	if (!staged) {
		ncards = eligible_cards(h->sys, cards, GS_CFG_MAX_BDFS, &more);
		if (s->in >= 0) {
			if ((r = ask_missing(s, &a, cards, ncards, more)))
				return r;
		} else {
			select_single(&a, cards, ncards, more);
			if (!a.ndevices && ncards + more > 1) {
				fprintf(stderr, "setup: %u GPUs are eligible. Give --device BDF for each GPU to use, or run\n"
					"setup on a terminal or with --interactive to be asked. Nothing changed.\n",
					ncards + more);
				return GS_EXIT_REFUSED;
			}
			if (!a.ndevices && !ncards && !more)
				printf("No NVIDIA GPU was found; doctor names what is missing.\n");
			/* The D3cold consent cannot be asked here: a laptop GPU without it changes nothing. */
			if ((r = ask_d3cold(s, a.devices, a.ndevices, 0, &a.disable_d3cold)))
				return r;
		}
	}
	if ((r = version_consent(s, &a)))
		return r;
	r = step_doctor(h, self, &a);
	if (r != GS_EXIT_OK && !staged) {
		printf("\nStopped: fix what doctor names above, then run setup again. Nothing changed.\n");
		if (s->in < 0)
			missing_flags(s, &a, ncards + more);
		return r;
	}
	if (r != GS_EXIT_OK)
		printf("\nStaging anyway: a staged install changes nothing here. Fix doctor's failures on the\n"
		       "target host before running the steps below.\n");
	if ((r = run_install(s, self, &a, a.disable_d3cold)))
		return r;
	if (staged) {
		printf("\nThis host is unchanged and needs no power cycle. After the target host's first boot, run\n"
		       "`geforce-sriov setup` there.\n");
		closing(h->sys, NULL, 1);
		return GS_EXIT_OK;
	}
	if (gs_config_load(path, &cfg, err, sizeof(err))) {
		fprintf(stderr, "setup: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	/*
	 * The livepatch must load before NVIDIA's modules: with nvidia already loaded in this boot,
	 * only the early unit at the next boot can do that. Installation is done; say so.
	 */
	if (nvidia_loaded(h)) {
		printf("\nThe install is complete. NVIDIA's driver is already loaded in this boot, so the livepatch cannot load\n"
		       "before it and the GPUs were not brought up.\nNext: power the machine off and on. The boot units bring the\n"
		       "GPUs up; `sudo geforce-sriov setup` then checks the result. Later boots need no extra step.\n");
		return GS_EXIT_OK;
	}
	return finish(s, self, &cfg);
}

int gs_setup(const struct gs_setup_host *s, const struct gs_install_opts *o)
{
	const char *path = s->config ? s->config : gs_config_path();
	char self[GS_PATH_MAX];

	if (self_path(self, sizeof(self)))
		return GS_EXIT_FAIL;
	printf("geforce-sriov %s setup\n", GS_VERSION);
	if (!gs_exists(path))
		return first_install(s, self, path, o);
	if (!s->config && getenv(GS_CONFIG_ENV) && getenv(GS_CONFIG_ENV)[0] &&
	    strcmp(getenv(GS_CONFIG_ENV), GS_CONFIG_PATH) != 0) {
		fprintf(stderr,
			"setup: %s is set, but the steps it runs read %s; unset it or point it at %s\n",
			GS_CONFIG_ENV, GS_CONFIG_PATH, GS_CONFIG_PATH);
		return GS_EXIT_REFUSED;
	}
	return installed_host(s, self, path, o);
}

/* NVIDIA's kernel tree where doctor looks for it, or its vGPU userspace where install looks. */
static int driver_present(const struct gs_install_opts *o)
{
	static const char *const libs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" };
	const char *src = o->nvidia_source ? o->nvidia_source : gs_nvidia_source_default();
	const struct gs_confirmed_version *rows;
	size_t count;
	char p[GS_PATH_MAX];

	if (!gs_pathf(p, sizeof(p), "%s/nvidia/nv-kernel.o_binary", src) && gs_exists(p))
		return 1;
	if (!gs_pathf(p, sizeof(p), "%s/kernel/nvidia/nv-kernel.o_binary", src) && gs_exists(p))
		return 1;
	rows = gs_confirmed_versions(&count);
	for (size_t k = 0; k < count; k++)
		for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
			if (!gs_pathf(p, sizeof(p), "%s/libnvidia-vgpu.so.%s", libs[i], rows[k].host) && gs_exists(p))
				return 1;
	return 0;
}

int gs_cmd_setup(const struct gs_install_opts *o, enum gs_prompt prompt)
{
	struct gs_setup_host s = { &gs_host_live, -1, driver_present(o), NULL, NULL, NULL };

	if (prompt == GS_PROMPT_ON || (prompt == GS_PROMPT_AUTO && isatty(STDIN_FILENO)))
		s.in = STDIN_FILENO;
	return gs_setup(&s, o);
}
