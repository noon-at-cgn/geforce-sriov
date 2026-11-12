// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gs_lifecycle.h"
#include "gs_internal.h"

/* ---- prerequisites and NVIDIA's package --------------------------------- */

int gs_dep_present(enum gs_dep dep, const char *release)
{
	char p[GS_PATH_MAX];

	switch (dep) {
	case GS_DEP_HEADERS:
		return release && release[0] && !gs_pathf(p, sizeof(p), "/lib/modules/%s/build/Makefile", release) &&
		       gs_exists(p);
	case GS_DEP_DKMS:
		return gs_find_program("dkms", p, sizeof(p)) == 0;
	case GS_DEP_PCIUTILS:
		return gs_find_program("lspci", p, sizeof(p)) == 0 && gs_find_program("setpci", p, sizeof(p)) == 0;
	default:
		return gs_find_program("cc", p, sizeof(p)) == 0 && gs_find_program("make", p, sizeof(p)) == 0;
	}
}

/* Live install, before anything is written: a missing prerequisite refuses. */
int gs_deps_step(void)
{
	char rel[128] = "";

	gs_read_attr(GS_LIVE_PROC "/sys/kernel/osrelease", rel, sizeof(rel));
	for (unsigned int d = 0; d < GS_DEP_COUNT; d++) {
		if (gs_dep_present((enum gs_dep)d, rel))
			continue;
		fprintf(stderr, "geforce-sriov: refused, nothing changed: a prerequisite is missing; `geforce-sriov doctor`\n"
			"names it. " GS_PREREQ_MSG "\n");
		return GS_EXIT_REFUSED;
	}
	return GS_EXIT_OK;
}

int gs_nvidia_package_version(const char *name, char *out, size_t len)
{
	const char *base = strrchr(name, '/'), *s;

	for (s = base ? base + 1 : name; *s; s++) {
		const char *p = s + 1;
		unsigned int parts = 0;
		size_t n;

		if (*s != '-')
			continue;
		for (;;) {
			const char *digits = p;

			while (*p >= '0' && *p <= '9')
				p++;
			if (p == digits)
				break;
			parts++;
			if (*p != '.' || !(p[1] >= '0' && p[1] <= '9'))
				break;
			p++;
		}
		if (parts < 2 || !(*p == '-' || *p == '.' || *p == '\0'))
			continue;
		n = (size_t)(p - (s + 1));
		if (n >= len)
			return -1;
		memcpy(out, s + 1, n);
		out[n] = '\0';
		return 0;
	}
	return -1;
}

#define PKG_MARKER ".geforce-sriov-sha256"

static int userspace_installed(const char *root, const char *ver);

/* The version a package that names none is taken to be: a confirmed one whose userspace is on root, else the first. */
static const char *assumed_version(const char *root)
{
	size_t count;
	const struct gs_confirmed_version *rows = gs_confirmed_versions(&count);

	for (size_t i = 0; i < count; i++)
		if (userspace_installed(root, rows[i].host))
			return rows[i].host;
	return gs_default_host_version();
}

/* NVIDIA's vGPU userspace of version ver below root: libnvidia-vgpu.so.<ver> where the driver step looks for it. */
static int userspace_installed(const char *root, const char *ver)
{
	static const char *const dirs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" };
	char p[GS_PATH_MAX];
	struct stat st;

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s%s/libnvidia-vgpu.so.%s", root, dirs[i], ver) && lstat(p, &st) == 0 &&
		    S_ISREG(st.st_mode))
			return 1;
	return 0;
}

static const char *const lib_dirs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" };

/*
 * argv[0] from root's system directories (live: ""), run with a fixed environment: its exit status,
 * or -1 when it is not there or cannot run. out receives the first len - 1 bytes it printed.
 */
static int root_capture(const char *root, const char *const argv[], char *out, size_t len)
{
	static const char *const dirs[] = { "/usr/sbin", "/usr/bin", "/sbin", "/bin" };
	static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL };
	char prog[GS_PATH_MAX], rest[512];
	size_t off = 0, i;
	int fd[2], status;
	ssize_t n;
	pid_t pid;

	out[0] = '\0';
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!gs_pathf(prog, sizeof(prog), "%s%s/%s", root, dirs[i], argv[0]) && access(prog, X_OK) == 0)
			break;
	if (i == sizeof(dirs) / sizeof(dirs[0]) || pipe(fd))
		return -1;
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);

		dup2(fd[1], 1);
		if (null >= 0)
			dup2(null, 2);
		close(fd[0]);
		close(fd[1]);
		execve(prog, (char *const *)argv, envp);
		_exit(127);
	}
	close(fd[1]);
	/* Read to the end, so a long listing never blocks the child; what does not fit is dropped. */
	for (;;) {
		if (off + 1 < len)
			n = read(fd[0], out + off, len - 1 - off);
		else
			n = read(fd[0], rest, sizeof(rest));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (off + 1 < len)
			off += (size_t)n;
	}
	close(fd[0]);
	out[off] = '\0';
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Any libnvidia-vgpu.so* in root's library directories. */
static int vgpu_library_present(const char *root)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	int found = 0;
	DIR *d;

	for (size_t i = 0; !found && i < sizeof(lib_dirs) / sizeof(lib_dirs[0]); i++) {
		if (gs_pathf(p, sizeof(p), "%s%s", root, lib_dirs[i]) || !(d = opendir(p)))
			continue;
		while (!found && (de = readdir(d)))
			found = strncmp(de->d_name, "libnvidia-vgpu.so", strlen("libnvidia-vgpu.so")) == 0;
		closedir(d);
	}
	return found;
}

/* Appends name to the ", "-separated list; *count counts every name, listed or not. */
static void name_add(char *list, size_t len, const char *name, unsigned int *count)
{
	size_t n = strlen(list);

	if (*count < 3)
		snprintf(list + n, len - n, "%s%s", n ? ", " : "", name);
	(*count)++;
}

/*
 * Another NVIDIA driver on root, which NVIDIA's vGPU installer would collide with: the nvidia module
 * answers `modinfo -F version nvidia` while no libnvidia-vgpu.so* exists, or while distribution
 * packages own the driver. GS_EXIT_REFUSED with what was found and the command that removes it.
 */
static int foreign_driver(const char *root)
{
	const char *const modinfo[] = { "modinfo", "-F", "version", "nvidia", NULL };
	const char *const dpkg[] = { "dpkg-query", "-W", "-f=${Package} ${Status}\\n", "nvidia*", NULL };
	const char *const rpm[] = { "rpm", "-qa", "nvidia*", NULL };
	static const char owned[] = " ok installed";
	char ver[64], out[8192], pkgs[256] = "", why[512], *line, *save = NULL;
	const char *remove = NULL;
	unsigned int npkgs = 0;
	int lib;

	if (root_capture(root, modinfo, ver, sizeof(ver)) != 0)
		return 0;
	ver[strcspn(ver, "\n")] = '\0';
	if (!ver[0])
		return 0;
	/* dpkg-query exits 1 when nothing matches; its lines are "<package> <want> ok <state>". */
	if (root_capture(root, dpkg, out, sizeof(out)) >= 0)
		for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			size_t n = strlen(line);
			char *sp = strchr(line, ' ');

			if (!sp || n < sizeof(owned) - 1 || strcmp(line + n - (sizeof(owned) - 1), owned) != 0)
				continue;
			*sp = '\0';
			name_add(pkgs, sizeof(pkgs), line, &npkgs);
			remove = "apt-get remove --purge 'nvidia-*'";
		}
	if (!remove && root_capture(root, rpm, out, sizeof(out)) == 0)
		for (save = NULL, line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			name_add(pkgs, sizeof(pkgs), line, &npkgs);
			remove = "dnf remove 'nvidia*'";
		}
	lib = vgpu_library_present(root);
	if (lib && !remove)
		return 0;
	why[0] = '\0';
	if (!lib)
		snprintf(why, sizeof(why), "no libnvidia-vgpu.so* is in %s, %s or %s", lib_dirs[0], lib_dirs[1], lib_dirs[2]);
	if (remove) {
		size_t n = strlen(why);
		char more[32] = "";

		if (npkgs > 3)
			snprintf(more, sizeof(more), " and %u more", npkgs - 3);
		snprintf(why + n, sizeof(why) - n, "%sdistribution packages own it (%s%s)", n ? "; " : "", pkgs, more);
	}
	fprintf(stderr, "geforce-sriov: refused, nothing changed: NVIDIA's driver %s is installed (modinfo -F version nvidia)\n"
		"and is not the vGPU host driver: %s. NVIDIA's vGPU installer would overwrite its module and\n"
		"libraries%s.\n"
		"Remove it with `%s`%s, then rerun.\n",
		ver, why, remove ? ", and the package manager would put them back" : "",
		remove ? remove : "nvidia-uninstall",
		remove ? "" : " if NVIDIA's .run installer put it there, or with the package manager that installed it");
	return GS_EXIT_REFUSED;
}

/* When NVIDIA's installer is still needed for the package file, no other NVIDIA driver may be on root. */
int gs_package_conflict(const char *file, const char *root, int accept_license)
{
	char ver[80];
	int have_ver = gs_nvidia_package_version(file, ver, sizeof(ver)) == 0;

	if (userspace_installed(root, have_ver ? ver : assumed_version(root)) && (have_ver || !accept_license))
		return 0;
	return foreign_driver(root);
}

/*
 * A copy of the regular file at path that only root can change: the file is opened once, without
 * following a link, and what that descriptor reads is written to the new file dst. 0 or -errno.
 */
static int private_copy(const char *path, const char *dst)
{
	static char buf[1 << 16];
	struct stat st;
	int in = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), out, r = 0;
	ssize_t n;

	if (in < 0)
		return -errno;
	if (fstat(in, &st) || !S_ISREG(st.st_mode)) {
		close(in);
		return -EINVAL;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 0) {
		r = -errno;
		close(in);
		return r;
	}
	while (!r && (n = read(in, buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno != EINTR)
				r = -errno;
			continue;
		}
		for (ssize_t off = 0, w; off < n; off += w)
			if ((w = write(out, buf + off, (size_t)(n - off))) < 0) {
				if (errno == EINTR) {
					w = 0;
					continue;
				}
				r = -errno;
				break;
			}
	}
	close(in);
	if (close(out) && !r)
		r = -errno;
	return r;
}

static int package_steps(const char *real, const char *run, const char *sha, const char *base, const char *root,
			 int accept_license, char *dir, size_t len, int *changed);

/*
 * The package is a shell script that root runs, so the file that is checked must be the file that
 * is run. It is copied once into a directory only root can enter; the copy is hashed, and only the
 * copy is run. Whoever owns the file or its directory cannot change it in between.
 */
int gs_nvidia_package(const char *file, const char *base, const char *root, int accept_license, char *dir, size_t len,
		      int *changed)
{
	char real[PATH_MAX], sha[65], parent[GS_PATH_MAX], scratch[GS_PATH_MAX], copy[GS_PATH_MAX];
	const char *rm_argv[] = { "rm", "-rf", "--", scratch, NULL };
	const char *slash = strrchr(base, '/'), *name;
	struct statvfs vfs;
	struct stat st;
	int r, made;

	*changed = 0;
	if (!realpath(file, real) || stat(real, &st) || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: --driver-file %s is not a regular file\n", file);
		return GS_EXIT_REFUSED;
	}
	name = strrchr(real, '/') + 1;
	/* Beside the place of the extraction, on the same file system: a package is hundreds of megabytes. */
	if (!slash || slash == base || gs_pathf(parent, sizeof(parent), "%.*s", (int)(slash - base), base) ||
	    gs_pathf(scratch, sizeof(scratch), "%s/" GS_PACKAGE_COPY_NAME, parent)) {
		fprintf(stderr, "geforce-sriov: %s: not a usable directory for NVIDIA's package\n", base);
		return GS_EXIT_FAIL;
	}
	made = !gs_exists(parent);
	if ((r = gs_mkdir_p(parent, 0755))) {
		fprintf(stderr, "geforce-sriov: cannot create %s: %s\n", parent, strerror(-r));
		return GS_EXIT_FAIL;
	}
	/*
	 * The directory has one name, in a directory only root can write to. A run that was killed leaves
	 * it behind; the next run starts by removing it, and so does uninstall.
	 */
	if (gs_exists(scratch) && gs_run(rm_argv) != 0) {
		fprintf(stderr, "geforce-sriov: cannot remove %s, which an earlier run left behind\n", scratch);
		return GS_EXIT_FAIL;
	}
	/* Room for the copy is checked before anything is written; the extraction needs more, and says so itself. */
	if (statvfs(parent, &vfs) == 0 &&
	    (unsigned long long)vfs.f_bavail * vfs.f_frsize < (unsigned long long)st.st_size) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: not enough space in %s: %llu MB needed for a copy of\n"
			"%s, %llu MB free. Free some space, then run the same command again.\n", parent,
			((unsigned long long)st.st_size + (1 << 20) - 1) >> 20, real,
			((unsigned long long)vfs.f_bavail * vfs.f_frsize) >> 20);
		if (made)
			rmdir(parent);
		return GS_EXIT_REFUSED;
	}
	if (mkdir(scratch, 0700)) {
		fprintf(stderr, "geforce-sriov: cannot create %s for a copy of %s: %s\n", scratch, real, strerror(errno));
		if (made)
			rmdir(parent);
		return GS_EXIT_FAIL;
	}
	if (gs_pathf(copy, sizeof(copy), "%s/%s", scratch, name))
		r = -ENAMETOOLONG;
	else if (!(r = private_copy(real, copy)))
		r = gs_sha256_file(copy, sha);
	if (r) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: cannot copy %s to %s: %s\n", real, scratch,
			strerror(-r));
		r = GS_EXIT_REFUSED;
	} else {
		r = package_steps(real, copy, sha, base, root, accept_license, dir, len, changed);
	}
	unlink(copy);
	rmdir(scratch);
	/* A run that wrote nothing leaves no directory behind. */
	if (made)
		rmdir(parent);
	return r;
}

/* real is the file the user named, for the messages; run is the private copy, with the digest sha. */
static int package_steps(const char *real, const char *run, const char *sha, const char *base, const char *root,
			 int accept_license, char *dir, size_t len, int *changed)
{
	char tag[80], ver[80], tmp[GS_PATH_MAX], p[GS_PATH_MAX], line[80];
	const struct gs_confirmed_version *rows, *named = NULL;
	const char *want;
	size_t count;
	struct stat st;
	int r, have_ver, installed, extracted = 0, recorded = 0;

	have_ver = gs_nvidia_package_version(real, ver, sizeof(ver)) == 0;
	if (have_ver)
		gs_copy(tag, sizeof(tag), ver);
	else
		snprintf(tag, sizeof(tag), "%.16s", sha);
	if (gs_pathf(dir, len, "%s/%s", base, tag) || gs_pathf(tmp, sizeof(tmp), "%s.partial", dir)) {
		fprintf(stderr, "geforce-sriov: %s/%s: path too long\n", base, tag);
		return GS_EXIT_FAIL;
	}
	printf("\n== NVIDIA's package %s (SHA-256 %s)\n"
	       "What is run below is a copy of that file that only root can change: %s\n", real, sha, run);
	/*
	 * The package is a shell script: extracting it runs it as root. A file with a recorded digest is
	 * a recorded package. A file that is named as a version with a recorded digest must be that
	 * file; any other file is run after a notice.
	 */
	rows = gs_confirmed_versions(&count);
	for (size_t i = 0; i < count; i++) {
		if (!rows[i].package)
			continue;
		if (strcmp(sha, rows[i].package) == 0)
			recorded = 1;
		else if (!named && have_ver && strcmp(ver, rows[i].host) == 0)
			named = &rows[i];
	}
	if (!recorded && named) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is named as NVIDIA's %s package, but its SHA-256\n"
			"is not %s, the recorded digest of that package. It was not run.\n"
			"Download it again from NVIDIA, or rename the file if it is a different package.\n",
			real, named->host, named->package);
		return GS_EXIT_REFUSED;
	}
	if (!recorded)
		printf("Notice: no digest is recorded for this package. It is a shell script and is run to extract it,\n"
		       "so use only a package you trust.\n");
	/* Before anything is written. */
	if ((r = gs_package_conflict(real, root, accept_license)))
		return r;
	if (gs_exists(dir)) {
		char *have = NULL;
		size_t hlen;
		int same = !gs_pathf(p, sizeof(p), "%s/" PKG_MARKER, dir) && gs_read_whole(p, &have, &hlen, 128) == 0 &&
			   strncmp(have, sha, 64) == 0;

		free(have);
		if (!same) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s holds another extraction; remove it, then rerun\n",
				dir);
			return GS_EXIT_REFUSED;
		}
		printf("reusing %s, extracted from this file earlier\n", dir);
	} else {
		const char *rm_argv[] = { "rm", "-rf", "--", tmp, NULL };
		const char *ex_argv[] = { "sh", run, "--extract-only", "--target", tmp, NULL };

		if ((r = gs_mkdir_p(base, 0755))) {
			fprintf(stderr, "geforce-sriov: cannot create %s: %s\n", base, strerror(-r));
			return GS_EXIT_FAIL;
		}
		if (gs_exists(tmp) && gs_run(rm_argv) != 0) {
			fprintf(stderr, "geforce-sriov: cannot remove the unfinished extraction %s\n", tmp);
			return GS_EXIT_FAIL;
		}
		printf("sh %s --extract-only --target %s\n", run, tmp);
		fflush(stdout);
		r = gs_run(ex_argv);
		if (r != 0) {
			fprintf(stderr, "geforce-sriov: extracting %s failed (exit %d); give NVIDIA's vGPU host .run package\n",
				real, r);
			return GS_EXIT_FAIL;
		}
		if (gs_pathf(p, sizeof(p), "%s/kernel", tmp) || stat(p, &st) || !S_ISDIR(st.st_mode)) {
			fprintf(stderr, "geforce-sriov: refused: %s has no kernel/ directory, so it is not NVIDIA's Linux driver package\n",
				real);
			return GS_EXIT_FAIL; /* the extraction below base was written */
		}
		snprintf(line, sizeof(line), "%s\n", sha);
		if (gs_pathf(p, sizeof(p), "%s/" PKG_MARKER, tmp) ||
		    (r = gs_write_file_atomic(p, line, strlen(line), 0644)) || rename(tmp, dir)) {
			fprintf(stderr, "geforce-sriov: cannot finish the extraction in %s\n", dir);
			return GS_EXIT_FAIL;
		}
		printf("extracted to %s\n", dir);
		extracted = *changed = 1;
	}
	/* The driver step needs the userspace of the version it patches; an unnamed package is taken to be that one. */
	want = have_ver ? ver : assumed_version(root);
	installed = userspace_installed(root, want);
	if (installed && (have_ver || !accept_license)) {
		printf("NVIDIA's vGPU userspace %s is already installed; NVIDIA's installer not run\n", want);
		return GS_EXIT_OK;
	}
	if (!accept_license) {
		fprintf(stderr, "geforce-sriov: refused: extracted only. Building NVIDIA's module needs NVIDIA's vGPU userspace\n"
			"(nvidia-vgpud, nvidia-vgpu-mgr, libnvidia-vgpu.so and sriov-manage), which is not installed.\n"
			"Run NVIDIA's installer yourself (`sh %s --dkms --skip-module-load --kernel-module-type=proprietary`), or rerun with\n"
			"--accept-nvidia-license to have install run it. Nothing else changed.\n", real);
		return extracted ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
	}
	if (!gs_pathf(p, sizeof(p), "%s/sys/module/nouveau", root) && gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: refused: nouveau is loaded, and NVIDIA's installer cannot run while it is.\n"
			"Put `blacklist nouveau` and `options nouveau modeset=0` in /etc/modprobe.d/blacklist-nouveau.conf,\n"
			"rebuild the initramfs (`update-initramfs -u`, or `dracut -f`), power the machine off and on, then rerun.\n"
			"Nothing else changed.\n");
		return extracted ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
	}
	{
		/* --skip-module-load: nothing may load nvidia before the early unit loads the livepatch.
		 * --kernel-module-type=proprietary: the installer otherwise selects the type from the attached GPUs. */
		const char *inst_argv[] = { "sh", run, "--silent", "--dkms", "--skip-module-load",
					    "--kernel-module-type=proprietary", NULL };

		gs_pathf(p, sizeof(p), "%s/LICENSE", dir);
		printf("NVIDIA's licence: --accept-nvidia-license accepts the licence agreement of this package for you\n"
		       "(%s). NVIDIA's installer runs with --silent and asks nothing.\n"
		       "sh %s --silent --dkms --skip-module-load --kernel-module-type=proprietary\n",
		       gs_exists(p) ? p : "NVIDIA's installer shows it when run without --silent", run);
		fflush(stdout);
		*changed = 1;
		r = gs_run(inst_argv);
		if (r != 0) {
			fprintf(stderr, "geforce-sriov: NVIDIA's installer exited with %d; see /var/log/nvidia-installer.log.\n"
				"geforce-sriov's own files are not installed\n", r);
			return GS_EXIT_FAIL;
		}
	}
	return GS_EXIT_OK;
}

/*
 * --driver-file on a live install: NVIDIA's package extracted (and installed, with the licence
 * accepted), then its vGPU daemon units and sriov-manage put in place where systemd and the config
 * do not find them yet. dir receives the extracted root; x->written is set once anything was written.
 */
int gs_nvidia_files(struct ctx *x, const struct gs_install_opts *o, const struct gs_config *cfg, char *dir,
			size_t len)
{
	static const char *const units[] = { "nvidia-vgpud.service", "nvidia-vgpu-mgr.service" };
	static const char *const unit_dirs[] = { "systemd", "init-scripts/systemd" };
	char src[GS_PATH_MAX], dst[GS_PATH_MAX];
	int changed, copied = 0;
	int r = gs_nvidia_package(o->driver_file, GS_NVIDIA_PKG_DIR, "", o->accept_nvidia_license, dir, len, &changed);

	x->written |= changed;
	if (r)
		return r;
	for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
		size_t k;

		if (gs_unit_file_present(units[u]))
			continue;
		for (k = 0; k < sizeof(unit_dirs) / sizeof(unit_dirs[0]); k++)
			if (!gs_pathf(src, sizeof(src), "%s/%s/%s", dir, unit_dirs[k], units[u]) && gs_exists(src))
				break;
		if (k == sizeof(unit_dirs) / sizeof(unit_dirs[0])) {
			printf("warning: %s is not installed and not in %s/systemd/; `activate` needs it\n", units[u], dir);
			continue;
		}
		gs_pathf(dst, sizeof(dst), "%s/%s", UNIT_DIR, units[u]);
		if ((r = gs_install_copy(x, src, dst, 0644))) {
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", dst, strerror(-r));
			return GS_EXIT_FAIL;
		}
		copied = 1;
	}
	if (copied) {
		const char *reload_argv[] = { "systemctl", "daemon-reload", NULL };

		if (gs_run(reload_argv) != 0) {
			fprintf(stderr, "geforce-sriov: systemctl daemon-reload failed\n");
			return GS_EXIT_FAIL;
		}
	}
	if (!gs_exists(cfg->sriov_manage) && !gs_exists(GS_SRIOV_MANAGE_DEFAULT) &&
	    !gs_pathf(src, sizeof(src), "%s/sriov-manage", dir) && gs_exists(src) &&
	    (r = gs_install_copy(x, src, GS_SRIOV_MANAGE_DEFAULT, 0755))) {
		fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", GS_SRIOV_MANAGE_DEFAULT, strerror(-r));
		return GS_EXIT_FAIL;
	}
	return GS_EXIT_OK;
}

int gs_is_kernel_source(const char *name)
{
	size_t n = strlen(name);

	if (name[0] == '.')
		return 0;
	if (strcmp(name, "Kbuild") == 0 || strcmp(name, "Makefile") == 0 || strcmp(name, "dkms.conf") == 0)
		return 1;
	if (strstr(name, ".mod."))
		return 0;
	return n > 2 && name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'h');
}

#define DRIVER_TAG "geforce-sriov-driver: "
#define DRIVER_LOG_TAIL 40

/* One line of the driver step's output: a refused or failed line's reason into why; filter: its own lines to the screen. */
static void driver_line(const char *line, int filter, char *why, size_t len, enum gs_driver_result *result)
{
	static const char *const tags[] = { DRIVER_TAG "refused: ", DRIVER_TAG "failed: " };
	/* The script's result line. It carries no tag, so it is logged and not shown; its messages are not parsed. */
	static const char key[] = "install_result=";

	if (result && strncmp(line, key, strlen(key)) == 0) {
		const char *v = line + strlen(key);

		*result = strcmp(v, "unchanged") == 0 ? GS_DRIVER_UNCHANGED :
			  strcmp(v, "record-only") == 0 ? GS_DRIVER_RECORD_ONLY : GS_DRIVER_CHANGED;
	}

	for (size_t k = 0; k < sizeof(tags) / sizeof(tags[0]); k++)
		if (strncmp(line, tags[k], strlen(tags[k])) == 0)
			snprintf(why, len, "%s", line + strlen(tags[k]));
	if (filter && strncmp(line, DRIVER_TAG, strlen(DRIVER_TAG)) == 0) {
		printf("%s\n", line);
		fflush(stdout);
	}
}

/* The last DRIVER_LOG_TAIL lines of the log open at fd, to standard error. */
static void log_tail(int fd)
{
	char buf[65536];
	off_t size = lseek(fd, 0, SEEK_END);
	unsigned int lines = 0;
	size_t start;
	ssize_t n;

	if (size <= 0)
		return;
	n = pread(fd, buf, sizeof(buf), size > (off_t)sizeof(buf) ? size - (off_t)sizeof(buf) : 0);
	if (n <= 0)
		return;
	/* Back from the end, past its own newline, to the start of the DRIVER_LOG_TAIL-th line. */
	for (start = (size_t)n - (buf[n - 1] == '\n'); start > 0; start--)
		if (buf[start - 1] == '\n' && ++lines == DRIVER_LOG_TAIL)
			break;
	fwrite(buf + start, 1, (size_t)n - start, stderr);
	if (buf[n - 1] != '\n')
		fputc('\n', stderr);
}

/* All of data to fd: 0, or -1 with errno set. */
static int write_all(int fd, const char *data, size_t len)
{
	while (len) {
		ssize_t n = write(fd, data, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

int gs_driver_step(const char *const argv[], const char *log, char *why, size_t len, enum gs_driver_result *result)
{
	static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL };
	char buf[4096], line[4096];
	int fd[2], status, rc, logfd = -1, log_ok = 1;
	size_t fill = 0;
	ssize_t n;
	pid_t pid;

	why[0] = '\0';
	if (result)
		*result = GS_DRIVER_CHANGED;
	if (log && ((logfd = open(log, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600)) < 0 ||
		    fchmod(logfd, 0600))) {
		fprintf(stderr, "geforce-sriov: cannot write %s: %s\n", log, strerror(errno));
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	if (pipe(fd)) {
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	if (pid == 0) {
		/* Without a log, standard output passes through and standard error is read for the reason. */
		if (logfd >= 0)
			dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);
		/* What the step writes below the state directory is root's, whatever umask this command was run with. */
		umask(022);
		execve(argv[0], (char *const *)argv, envp);
		_exit(127);
	}
	close(fd[1]);
	for (;;) {
		n = read(fd[0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (logfd < 0)
			fwrite(buf, 1, (size_t)n, stderr);
		else if (log_ok && write_all(logfd, buf, (size_t)n)) {
			fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", log, strerror(errno));
			log_ok = 0;
		}
		/* Lines longer than line keep their start, which is all the tags need. */
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != '\n') {
				if (fill < sizeof(line) - 1)
					line[fill++] = buf[i];
				continue;
			}
			line[fill] = '\0';
			driver_line(line, logfd >= 0, why, len, result);
			fill = 0;
		}
	}
	close(fd[0]);
	if (fill) {
		line[fill] = '\0';
		driver_line(line, logfd >= 0, why, len, result);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR) {
			if (logfd >= 0)
				close(logfd);
			return -1;
		}
	rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	/* Nothing is concluded from a step that failed. */
	if (rc && result)
		*result = GS_DRIVER_CHANGED;
	if (rc && logfd >= 0) {
		fprintf(stderr, "Last %d lines of the build log (full log: %s):\n", DRIVER_LOG_TAIL, log);
		log_tail(logfd);
	}
	if (logfd >= 0)
		close(logfd);
	return rc;
}
