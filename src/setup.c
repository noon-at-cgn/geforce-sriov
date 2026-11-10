// SPDX-License-Identifier: MIT
/*
 * geforce-sriov: setup, the guided path from a clean host to a ready VF.
 *
 * The user runs one command. It reads the host's state and runs the next steps of a first install
 * (doctor, install) or of a prepared boot (doctor, check, prepare when needed, activate, check),
 * each as a child process of this binary, and ends by naming the guest drivers of each selected
 * card. On a boot whose console release the config grants, it asks before it runs the commands of
 * gs_release_steps. Attaching a VF to a VM stays the user's step.
 *
 * Before a first install it asks for what no flag supplied (NVIDIA's package, the licence, the
 * GPUs, and, for a boot display GPU granted a console release, whether install creates the hold of
 * nvidia) when standard input is a terminal or --interactive is given, and prints the flags that
 * repeat the run without questions. Without a terminal it asks nothing.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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
#define STEP_MAX_ARGS (2 + 4 * GS_CFG_MAX_BDFS + 16)

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

/* The --device and --boot-display-release of a first install, for the install step. */
static unsigned int push_selection(const struct gs_install_opts *o, const char *argv[], unsigned int n)
{
	unsigned int i;

	for (i = 0; i < o->ndevices; i++)
		n = push_opt(argv, n, "--device", o->devices[i].str);
	for (i = 0; i < o->nrelease; i++)
		n = push_opt(argv, n, "--boot-display-release", o->release[i].str);
	return n;
}

/*
 * doctor with the configuration this process read: children get a fixed environment, so a config
 * named by the environment would otherwise be lost between steps. o: a first install's options,
 * whose --device selection and --boot-display-release grants doctor judges as install will use
 * them; NULL on an installed host.
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
		n = push_selection(o, argv, n);
	/* The hold and the D3cold consent the user chose (flag or yes) are judged as install will write them. */
	if (o && o->hold_nvidia)
		argv[n++] = "--hold-nvidia";
	if (o && o->disable_d3cold)
		argv[n++] = "--disable-d3cold";
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
	switch (generation) {
	case GS_AMPERE:
		printf("    Linux 580.159.03, Windows 582.53\n");
		break;
	case GS_ADA:
		printf("    Linux 535.230.02 (vGPU 16.9), Windows 539.19 (16.9)\n");
		break;
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
				printf("  %s\n    cannot read the card; Ampere 580.159.03, Ada 535.230.02\n",
				       cfg->bdfs[i].str);
				continue;
			}
			printf("  %s %s\n", gs_generation_name(generation), name);
			guest_drivers(generation);
		}
	}
	printf("  Every guest driver needs RMSetVgpuGspPluginOffloadMode=0.\n"
	       "  Use these versions; other versions are not supported.\n"
	       "  Setting and steps: docs/installation.md#guests.\n");
	printf("  Attaching a VF to a VM is your step: docs/installation.md#create-a-vm.\n");
}

/* A configured card that prepare has not recorded for this boot. */
static int needs_prepare(const char *run_dir, const struct gs_config *cfg)
{
	char boot[64];
	unsigned int i;

	if (gs_boot_id(boot, sizeof(boot)))
		return 1;
	for (i = 0; i < cfg->nbdfs; i++) {
		struct gs_prepared rec;

		gs_prepared_read(run_dir, &cfg->bdfs[i], &rec);
		if (!rec.present || strcmp(rec.boot_id, boot) != 0)
			return 1;
	}
	return 0;
}

/* ---- questions before a first install ------------------------------------ */

/* A card install can select; boot is its boot display mark (selected only with a console release). */
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
 * The NVIDIA physical functions in sys that install accepts with a console-release grant, so the boot
 * display is among them: the first max by address. *more counts the eligible ones left out.
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

/* No --driver-file, --nvidia-source or --driver=skip, and no NVIDIA tree or userspace on the host. */
static int needs_driver(const struct gs_setup_host *s, const struct gs_install_opts *a)
{
	return !a->driver_file && !a->nvidia_source && !a->skip_driver && !s->driver_present;
}

static int ask_driver(const struct gs_setup_host *s, struct gs_install_opts *a)
{
	static char real[PATH_MAX];
	char path[PATH_MAX];
	struct stat st;

	for (;;) {
		if (ask(s, "No NVIDIA driver found. Path to NVIDIA's vGPU host driver package (.run)?",
			"an absolute path, e.g. /path/to/NVIDIA-Linux-x86_64-" GS_NVIDIA_VERSION "-vgpu-kvm.run",
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
		printf("  = no --accept-nvidia-license: install extracts the package and stops before the driver step.\n"
		       "    Install NVIDIA's vGPU userspace with NVIDIA's installer yourself, then run setup again.\n");
	return 0;
}

/* "all", "N" or "N,M,...": 0 with chosen[] set; -1 for anything else. all never picks a boot display. */
static int parse_choice(const char *ans, const struct pick *list, unsigned int n, int *chosen)
{
	const char *p = ans;
	unsigned int i, count = 0;

	memset(chosen, 0, n * sizeof(chosen[0]));
	if (strcasecmp(ans, "all") == 0) {
		for (i = 0; i < n; i++)
			if (!list[i].boot)
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

/*
 * Yes or no to a console release, asked as question with hint, the one explanation of what the answer means;
 * an empty answer is no. 1, 0, or -1 at the end of input (flag: as for ask()).
 */
static int ask_release(const struct gs_setup_host *s, const char *question, const char *hint, const char *flag)
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

/* The console-release question for one boot display card: 1 yes, 0 no, -1 at the end of input. */
static int ask_console_grant(const struct gs_setup_host *s, const struct gs_addr *pf)
{
	char question[128];

	snprintf(question, sizeof(question), "Allow releasing the console of %s after the next boot? [y/N]", pf->str);
	return ask_release(s, question,
			   "After every boot, running setup releases the console on this GPU.\n"
			   "  The monitor may go blank or freeze on the last frame. You will have no\n"
			   "  local text console until the next boot.\n"
			   "  Set up and test SSH or a serial console BEFORE you power cycle.\n"
			   "  Without that access, you cannot see the result or fix a problem.\n"
			   "  Yes records the grant; nothing is released now.\n"
			   "  Default is no.",
			   "--boot-display-release BDF");
}

static int ask_devices(const struct gs_setup_host *s, struct gs_install_opts *a, const struct pick *list, unsigned int n,
		       unsigned int more)
{
	int chosen[GS_CFG_MAX_BDFS], boot = 0, left = 0, r;
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
			"all leaves out the boot display; select it by typing its number", "--device BDF", ans, sizeof(ans)))
			return -1;
		if (parse_choice(ans, list, n, chosen) == 0)
			break;
		printf("  answer all, or numbers from 1 to %u\n", n);
	}
	/* A boot display card stays selected only when its console release is confirmed; the release itself comes after the boot. */
	for (i = 0; i < n; i++) {
		if (!chosen[i] || !list[i].boot)
			continue;
		if ((r = ask_console_grant(s, &list[i].pf)) < 0)
			return -1;
		if (!r) {
			chosen[i] = 0;
			printf("  %s is left out\n", list[i].pf.str);
		}
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
	for (i = 0; i < n; i++)
		if (chosen[i] && list[i].boot) {
			a->release[a->nrelease++] = list[i].pf;
			printf(" --boot-display-release %s", list[i].pf.str);
			boot = 1;
		}
	printf("\n");
	/* The question explained the grant, so install leaves its note out. */
	if (boot)
		a->release_note_shown = 1;
	return 0;
}

/* The only eligible card is the boot display: it is selected when its console release is confirmed. */
static int ask_boot_only(const struct gs_setup_host *s, struct gs_install_opts *a, const struct pick *card)
{
	int r;

	printf("\nThe only NVIDIA GPU here, %s, is the boot display.\n", card->pf.str);
	if ((r = ask_console_grant(s, &card->pf)) < 0)
		return -1;
	if (!r) {
		fprintf(stderr, "setup: no GPU left to select. Nothing changed.\n");
		return -1;
	}
	a->devices[a->ndevices++] = card->pf;
	a->release[a->nrelease++] = card->pf;
	printf("  = --device %s --boot-display-release %s\n", card->pf.str, card->pf.str);
	/* The question explained the grant, so install leaves its note out. */
	a->release_note_shown = 1;
	return 0;
}

/* A file in the modprobe.d directory setup reads already holds nvidia back at boot. */
static int hold_present(const struct gs_setup_host *s)
{
	char files[2 * GS_PATH_MAX + 8];

	return !gs_nvidia_hold(s->modprobe_dir ? s->modprobe_dir : "/etc/modprobe.d", files, sizeof(files));
}

/*
 * After a console-release grant, with no hold in place: the notice, then whether install creates the hold.
 * Yes sets --hold-nvidia. 0, or -1 at the end of input.
 */
static int ask_hold(const struct gs_setup_host *s, struct gs_install_opts *a)
{
	int r;

	printf("\n");
	gs_hold_notice(stdout, 0);
	r = ask_release(s, "Create " GS_NVIDIA_HOLD " now? [y/N]",
			"Yes: install writes the file with its other files.\n"
			"  No: after install, setup shows how to create it by hand.\n"
			"  Default is no.", "--hold-nvidia");
	if (r < 0)
		return -1;
	a->hold_nvidia = r;
	printf("  = %s\n", r ? "--hold-nvidia" : "no --hold-nvidia");
	return 0;
}

/*
 * The D3cold consent for the laptop GPUs among the selection sel (nsel GPUs, with the console-release grants rel),
 * unless the config already has it (have): *flag is --disable-d3cold, given, or set by a yes to the question that
 * follows the notice (asked when s has input). 0 to go on; GS_EXIT_USAGE for the flag without a laptop GPU;
 * GS_EXIT_REFUSED, nothing changed, on a no, without a question, or at the end of input. Each says why.
 */
static int ask_d3cold(const struct gs_setup_host *s, const struct gs_addr *sel, unsigned int nsel,
		      const struct gs_addr *rel, unsigned int nrel, int have, int *flag)
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
		r = ask_release(s, question,
				"Yes: save d3cold=off in the config. prepare keeps this GPU powered each time it runs.\n"
				"  No: prepare refuses this GPU. Default is no.", NULL);
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
	gs_d3cold_why(why, sizeof(why), laptops, n, sel, nsel, rel, nrel, "");
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

/* The command line that repeats this run without questions. */
static void flag_set(const struct gs_install_opts *a, char *buf, size_t len)
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
	for (i = 0; i < a->nrelease; i++) {
		flag_add(buf, len, "--boot-display-release");
		flag_add(buf, len, a->release[i].str);
	}
	if (a->hold_nvidia)
		flag_add(buf, len, "--hold-nvidia");
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
	char flags[2 * PATH_MAX + 1024];
	unsigned int i;

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
	flag_set(a, flags, sizeof(flags));
	printf("  the same run without questions:\n    %s\n", flags);
}

/*
 * No --device and exactly one eligible card that is not the boot display: that card. The boot
 * display is selected only by naming it, with its console release.
 */
static unsigned int plain_cards(const struct pick *cards, unsigned int ncards, const struct gs_addr **one)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < ncards; i++)
		if (!cards[i].boot && !n++)
			*one = &cards[i].pf;
	return n;
}

static void select_single(struct gs_install_opts *a, const struct pick *cards, unsigned int ncards)
{
	const struct gs_addr *one = NULL;

	if (!a->ndevices && plain_cards(cards, ncards, &one) == 1) {
		a->devices[a->ndevices++] = *one;
		printf("The one eligible GPU, %s, is selected: --device %s\n", one->str, one->str);
	}
}

/* --hold-nvidia on a first install that grants no console release: GS_EXIT_USAGE, with why printed; else 0. */
static int hold_without_grant(const struct gs_install_opts *a)
{
	if (!a->hold_nvidia || a->nrelease)
		return 0;
	fprintf(stderr, "geforce-sriov: --hold-nvidia requires a selected boot display GPU.\n"
		"Select it with --boot-display-release BDF. Nothing changed.\n"
		"Try 'geforce-sriov --help'.\n");
	return GS_EXIT_USAGE;
}

/*
 * Each question no flag answered, in this order: NVIDIA's package and licence, the GPUs with their console
 * release, the hold, then the D3cold consent of a laptop GPU, which needs the final selection. GS_EXIT_REFUSED
 * when the input ends first or the consent is refused, GS_EXIT_USAGE as hold_without_grant or ask_d3cold.
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
	if (!a->ndevices && ncards == 1 && cards[0].boot && ask_boot_only(s, a, &cards[0]))
		return GS_EXIT_REFUSED;
	if ((r = hold_without_grant(a)))
		return r;
	if (a->nrelease && !a->hold_nvidia && !hold_present(s) && ask_hold(s, a))
		return GS_EXIT_REFUSED;
	select_single(a, cards, ncards);
	if ((r = ask_d3cold(s, a->devices, a->ndevices, a->release, a->nrelease, 0, &a->disable_d3cold)))
		return r;
	summary(s, a, ncards);
	return GS_EXIT_OK;
}

/* Without questions: the flags that would let the run continue, when doctor stopped it. */
static void missing_flags(const struct gs_setup_host *s, const struct gs_install_opts *o, unsigned int ncards)
{
	char need[128] = "";

	if (needs_driver(s, o))
		strcat(need, " --driver-file PATH");
	if (o->driver_file && !o->accept_nvidia_license)
		strcat(need, " --accept-nvidia-license");
	if (!o->ndevices && ncards > 1)
		strcat(need, " --device BDF");
	if (need[0])
		printf("To continue, add:%s\n"
		       "(or run setup on a terminal, or with --interactive, to be asked)\n", need);
}

/*
 * The selected cards already bound to nvidia (the driver is installed and running): prepare cannot
 * act on them before the next boot. One note for all of them.
 */
static void bound_note(const char *sys, const struct gs_install_opts *a)
{
	char list[GS_CFG_MAX_BDFS * (GS_BDF_STR_SIZE + 2)] = "";
	unsigned int n = 0;

	for (unsigned int i = 0; i < a->ndevices; i++) {
		struct gs_card c;

		gs_card_inspect(sys, &a->devices[i], &c);
		if (strcmp(c.driver, "nvidia") != 0)
			continue;
		if (n++)
			strcat(list, ", ");
		strcat(list, c.pf.str);
	}
	if (n == 1)
		printf("NVIDIA's driver is bound to %s, so prepare cannot act on it before the next boot. Setup\n"
		       "continues; the GPU is prepared after you power the machine off and on.\n", list);
	else if (n > 1)
		printf("NVIDIA's driver is bound to %s, so prepare cannot act on them before the next boot. Setup\n"
		       "continues; the GPUs are prepared after you power the machine off and on.\n", list);
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

/* Options only a first install takes: on an installed host, one note each, and the run goes on without them. */
static void ignore_first_install_opts(const struct gs_install_opts *o)
{
	const struct {
		int given;
		const char *name;
	} opts[] = {
		{ o->vfs != 0, "--vfs" },
		{ o->nrelease != 0, "--boot-display-release" },
		{ o->nvidia_source != NULL, "--nvidia-source" },
		{ o->skip_driver, "--driver=skip" },
		{ o->driver_file != NULL, "--driver-file" },
		{ o->hold_nvidia, "--hold-nvidia" },
	};

	for (size_t k = 0; k < sizeof(opts) / sizeof(opts[0]); k++)
		if (opts[k].given)
			printf("install already ran, so %s is ignored: it applies only to a first install\n", opts[k].name);
}

/* NVIDIA's driver is loaded: its version file exists, as activate reads it. */
static int nvidia_loaded(const struct gs_host *h)
{
	char p[GS_PATH_MAX];

	return !gs_pathf(p, sizeof(p), "%s/driver/nvidia/version", h->proc) && gs_exists(p);
}

/* "\nStop here: ...", and code, for the first of the console-release commands that failed. */
static int stopped(const char *what, int code)
{
	printf("\nStopped: %s exited with %d.\n", what, code);
	return code;
}

/*
 * After the console release: NVIDIA's driver loaded past its hold, activate, check. The first that fails
 * ends the run with its exit status.
 */
static int bring_up(const struct gs_host *h, const char *self, const struct gs_config *cfg)
{
	static const char *const modprobe[] = { "modprobe", "--ignore-install", "nvidia", NULL };
	int r;

	printf("\n== modprobe --ignore-install nvidia\n");
	fflush(stdout);
	if ((r = h->run(modprobe))) {
		if (r < 0) {
			fprintf(stderr, "setup: cannot run modprobe: %s\n", strerror(-r));
			r = GS_EXIT_FAIL;
		}
		return stopped("modprobe --ignore-install nvidia", r);
	}
	if ((r = step(h, self, "activate", NULL)))
		return stopped("activate", r);
	r = step(h, self, "check", NULL);
	closing(h->sys, cfg, 0);
	return r;
}

/*
 * A boot whose console release the config grants, not done yet, with NVIDIA's driver not loaded: the
 * commands of gs_release_steps, in order, after a yes; the first that fails ends the run with its exit
 * status. Without a yes nothing changes: no is exit 0, no question (or no answer) GS_EXIT_REFUSED.
 */
static int release_now(const struct gs_setup_host *s, const char *self, const struct gs_config *cfg)
{
	const struct gs_host *h = s->h;
	char bdfs[GS_BDF_LIST_STR_SIZE], question[GS_BDF_LIST_STR_SIZE + 64], hint[GS_BDF_LIST_STR_SIZE + 320];
	char consent[GS_CFG_MAX_BDFS][GS_BDF_STR_SIZE + 20];
	const char *prep[GS_CFG_MAX_BDFS + 1];
	unsigned int i;
	int r = -1;

	gs_addr_list_str(cfg->boot_display_release, cfg->nrelease, bdfs, sizeof(bdfs));
	snprintf(question, sizeof(question), "Release the console of %s and bring the GPUs up now? [y/N]", bdfs);
	snprintf(hint, sizeof(hint),
		 "If you continue, the monitor on %s may go blank or freeze on\n"
		 "  the last frame. You will have no local text console until the next boot.\n"
		 "  Run this over SSH or a serial console so you can see the result.\n"
		 "  Default is no.", bdfs);
	if (s->in >= 0)
		r = ask_release(s, question, hint, NULL);
	if (r < 0) {
		fprintf(stderr, "setup: releasing the console needs a yes. Run setup on a terminal, or run these commands:\n");
		gs_release_steps(stderr, "  ", cfg, 0);
		return GS_EXIT_REFUSED;
	}
	if (!r) {
		printf("Nothing changed. To bring the GPUs up yourself, as root, in this order:\n");
		gs_release_steps(stdout, "  ", cfg, 0);
		return GS_EXIT_OK;
	}
	for (i = 0; i < cfg->nrelease; i++) {
		snprintf(consent[i], sizeof(consent[i]), "--release-console=%s", cfg->boot_display_release[i].str);
		prep[i] = consent[i];
	}
	prep[i] = NULL;
	if ((r = step(h, self, "prepare", prep)))
		return stopped("prepare", r);
	return bring_up(h, self, cfg);
}

int gs_setup(const struct gs_setup_host *s, const struct gs_install_opts *o)
{
	const struct gs_host *h = s->h;
	const char *path = s->config ? s->config : gs_config_path();
	char self[GS_PATH_MAX], err[256], vfsbuf[16] = "";
	struct gs_config cfg;
	int r;

	if (self_path(self, sizeof(self)))
		return GS_EXIT_FAIL;

	printf("geforce-sriov %s setup\n", GS_VERSION);

	if (!gs_exists(path)) {
		const char *destdir = getenv("DESTDIR");
		int staged = destdir && destdir[0];
		struct gs_install_opts a = *o;
		struct pick cards[GS_CFG_MAX_BDFS];
		unsigned int ncards = 0, more = 0;

		printf("This host is not set up.\n");
		/* Without questions, only the options can grant the console release --hold-nvidia needs. */
		if ((staged || s->in < 0) && (r = hold_without_grant(&a)))
			return r;
		/* A staged install describes another host: nothing here to ask about. */
		if (!staged) {
			const struct gs_addr *one = NULL;

			ncards = eligible_cards(h->sys, cards, GS_CFG_MAX_BDFS, &more);
			/*
			 * Without input the boot display is selected only by naming it; with input, ask_missing asks for its
			 * console release. With cards left out of the list (more), the boot display is not the only card.
			 */
			if (s->in < 0 && !a.ndevices && ncards && !more && !plain_cards(cards, ncards, &one)) {
				fprintf(stderr, "setup: the only NVIDIA GPU here is the boot display, which setup does not select by itself.\n"
					"To use it, give --device BDF --boot-display-release BDF, or add another GPU. Nothing changed.\n");
				return GS_EXIT_REFUSED;
			}
			if (s->in >= 0 && (r = ask_missing(s, &a, cards, ncards, more)))
				return r;
			if (s->in < 0)
				select_single(&a, cards, ncards);
			if (s->in < 0 && (r = ask_d3cold(s, a.devices, a.ndevices, a.release, a.nrelease, 0, &a.disable_d3cold)))
				return r;
			/* No yes and no --hold-nvidia: the notice for a hold created by hand follows install. */
			if (a.nrelease && !a.hold_nvidia && !hold_present(s))
				a.hold_note_shown = 1;
			bound_note(h->sys, &a);
		}
		{
			/* A staged install's GPUs are on another host: doctor does not judge its D3cold consent here. */
			struct gs_install_opts d = a;

			if (staged)
				d.disable_d3cold = 0;
			r = step_doctor(h, self, &d);
		}
		if (r != GS_EXIT_OK && !staged) {
			printf("\nStopped: fix what doctor names above, then run setup again. Nothing changed.\n");
			if (s->in < 0)
				missing_flags(s, &a, ncards + more);
			return r;
		}
		if (r != GS_EXIT_OK)
			printf("\nStaging anyway: a staged install changes nothing here. Fix doctor's failures on the\n"
			       "target host before running the steps below.\n");
		if (!staged && !a.ndevices && ncards + more > 1) {
			fprintf(stderr, "setup: %u GPUs are eligible. Give --device BDF for each GPU to use, or run setup on a\n"
				"terminal or with --interactive to be asked. Nothing changed.\n",
				ncards + more);
			return GS_EXIT_REFUSED;
		}
		{
			const char *argv[STEP_MAX_ARGS];
			unsigned int n = 0;

			if (staged)
				n = push_opt(argv, n, "--destdir", destdir);
			n = push_selection(&a, argv, n);
			if (a.vfs) {
				snprintf(vfsbuf, sizeof(vfsbuf), "%u", a.vfs);
				n = push_opt(argv, n, "--vfs", vfsbuf);
			}
			if (a.skip_driver)
				argv[n++] = "--driver=skip";
			if (a.nvidia_source)
				n = push_opt(argv, n, "--nvidia-source", a.nvidia_source);
			if (a.driver_file)
				n = push_opt(argv, n, "--driver-file", a.driver_file);
			if (a.accept_nvidia_license)
				argv[n++] = "--accept-nvidia-license";
			if (a.release_note_shown)
				argv[n++] = "--release-note-shown";
			if (a.hold_nvidia)
				argv[n++] = "--hold-nvidia";
			if (a.hold_note_shown)
				argv[n++] = "--hold-note-shown";
			if (a.disable_d3cold)
				argv[n++] = "--disable-d3cold";
			argv[n] = NULL;
			r = step(h, self, "install", argv);
		}
		if (r != GS_EXIT_OK)
			return r;
		if (a.hold_note_shown) {
			printf("\n");
			gs_hold_notice(stdout, 1);
		}
		/* install's closing text names the power cycle and the step after it. */
		if (staged)
			printf("\nThis host is unchanged and needs no power cycle. After the target host's first boot, run\n"
			       "`geforce-sriov setup` there.\n");
		closing(h->sys, NULL, 1);
		return GS_EXIT_OK;
	}

	if (!s->config && getenv(GS_CONFIG_ENV) && getenv(GS_CONFIG_ENV)[0] &&
	    strcmp(getenv(GS_CONFIG_ENV), GS_CONFIG_PATH) != 0) {
		fprintf(stderr,
			"setup: %s is set, but the steps it runs read %s; unset it or point it at %s\n",
			GS_CONFIG_ENV, GS_CONFIG_PATH, GS_CONFIG_PATH);
		return GS_EXIT_REFUSED;
	}

	if (gs_config_load(path, &cfg, err, sizeof(err))) {
		fprintf(stderr, "setup: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	if (!cfg.nbdfs) {
		fprintf(stderr,
			"setup: %s selects no GPU. Run `geforce-sriov install --device BDF` (`geforce-sriov devices` lists the table).\n",
			path);
		return GS_EXIT_REFUSED;
	}
	if ((r = same_devices(o, &cfg, path)))
		return r;
	ignore_first_install_opts(o);
	/*
	 * A selected laptop GPU without D3cold consent: prepare would refuse it, so the consent comes first. Given
	 * (flag or yes), install records it in the config and keeps the rest; its closing text names the next step.
	 */
	{
		int consent = o->disable_d3cold;

		if ((r = ask_d3cold(s, cfg.bdfs, cfg.nbdfs, cfg.boot_display_release, cfg.nrelease, cfg.d3cold_off,
				    &consent)))
			return r;
		if (consent && !cfg.d3cold_off) {
			/* NVIDIA's files were handled by the first install (or by hand): only the config changes. */
			static const char *const consent_argv[] = { "--driver=skip", "--disable-d3cold", NULL };

			return step(h, self, "install", consent_argv);
		}
	}

	r = step_doctor(h, self, NULL);
	if (r != GS_EXIT_OK) {
		printf("\nStopped: fix what doctor names above, then run setup again. Nothing changed.\n");
		return r;
	}

	/*
	 * The activate unit normally brings the VFs up during the boot, so check first: when every
	 * VF is already ready, running the bring-up again would refuse on the vGPU types it finds.
	 * Anything but ready or not ready (a refusal: config, selection, a missing GPU) stops the run.
	 */
	r = step(h, self, "check", NULL);
	if (r == GS_EXIT_OK) {
		printf("\nEvery VF is ready. Nothing to bring up.\n");
		closing(h->sys, &cfg, 0);
		return GS_EXIT_OK;
	}
	if (r != GS_EXIT_FAIL) {
		printf("\nStopped: check refused (exit %d). Nothing changed.\n", r);
		return r;
	}

	if (cfg.nrelease && needs_prepare(h->run_dir, &cfg)) {
		if (!nvidia_loaded(h))
			return release_now(s, self, &cfg);
		fprintf(stderr, "setup: NVIDIA's driver is loaded, so the console GPU cannot be released in this boot. Create "
			GS_NVIDIA_HOLD " with these two lines:\n"
			"  blacklist nvidia\n"
			"  install nvidia /bin/false\n"
			"Power the machine off and on, then run setup again.\n"
			"See docs/installation.md#boot-display-card.\n");
		return GS_EXIT_REFUSED;
	}
	/* Released this boot, but NVIDIA's driver is not loaded (an earlier run stopped at modprobe): carry on from there. */
	if (cfg.nrelease && !nvidia_loaded(h)) {
		printf("\nThe console was released this boot and NVIDIA's driver is not loaded: loading it.\n");
		return bring_up(h, self, &cfg);
	}

	/* activate would bring the VFs up on the loaded module, which lacks what the installed build has. */
	if (gs_loaded_not_installed(h->sys, s->build)) {
		fprintf(stderr, "setup: the running nvidia module is not the installed one. Power the machine off and on, "
			"then run setup again.\n");
		return GS_EXIT_REFUSED;
	}

	if (needs_prepare(h->run_dir, &cfg)) {
		static const char *const boot[] = { "--boot", NULL };

		r = step(h, self, "prepare", boot);
		if (r != GS_EXIT_OK)
			return r;
	}

	{
		static const char *const boot[] = { "--boot", NULL };

		r = step(h, self, "activate", boot);
		if (r != GS_EXIT_OK)
			return r;
	}

	r = step(h, self, "check", NULL);
	closing(h->sys, &cfg, 0);
	return r;
}

/* NVIDIA's kernel tree where doctor looks for it, or its vGPU userspace where install looks. */
static int driver_present(const struct gs_install_opts *o)
{
	static const char *const libs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" };
	const char *src = o->nvidia_source ? o->nvidia_source : GS_NVIDIA_SRC;
	char p[GS_PATH_MAX];

	if (!gs_pathf(p, sizeof(p), "%s/nvidia/nv-kernel.o_binary", src) && gs_exists(p))
		return 1;
	if (!gs_pathf(p, sizeof(p), "%s/kernel/nvidia/nv-kernel.o_binary", src) && gs_exists(p))
		return 1;
	for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s/libnvidia-vgpu.so." GS_NVIDIA_VERSION, libs[i]) && gs_exists(p))
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
