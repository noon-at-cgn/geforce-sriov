// SPDX-License-Identifier: MIT
/* File, sysfs and process helpers for the geforce-sriov CLI. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gs_lifecycle.h"

int gs_pathf(char *out, size_t len, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(out, len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= len) {
		if (len)
			out[0] = '\0';
		return -ENAMETOOLONG;
	}
	return 0;
}

void gs_copy(char *dst, size_t len, const char *src)
{
	size_t n;

	if (len == 0)
		return;
	n = strnlen(src, len - 1);
	memcpy(dst, src, n);
	dst[n] = '\0';
}

int gs_exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

/* Read a small attribute; trailing whitespace removed. 0 or -errno. */
int gs_read_attr(const char *path, char *buf, size_t len)
{
	int fd;
	ssize_t n;

	if (len == 0)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		n = read(fd, buf, len - 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	close(fd);
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
		buf[--n] = '\0';
	return 0;
}

int gs_read_hex(const char *path, unsigned int *out)
{
	char buf[32], *end;
	unsigned long v;
	int r = gs_read_attr(path, buf, sizeof(buf));

	if (r)
		return r;
	if (strncmp(buf, "0x", 2) != 0 || buf[2] == '\0')
		return -EINVAL;
	errno = 0;
	v = strtoul(buf + 2, &end, 16);
	if (errno || *end || v > 0xffffffffUL)
		return -EINVAL;
	*out = (unsigned int)v;
	return 0;
}

int gs_read_uint(const char *path, unsigned int *out)
{
	char buf[32], *end;
	unsigned long v;
	int r = gs_read_attr(path, buf, sizeof(buf));

	if (r)
		return r;
	if (buf[0] < '0' || buf[0] > '9')
		return -EINVAL;
	errno = 0;
	v = strtoul(buf, &end, 10);
	if (errno || *end || v > 0xffffffffUL)
		return -EINVAL;
	*out = (unsigned int)v;
	return 0;
}

/*
 * In-place write of a sysfs attribute: never creates or truncates, never follows a link at the last
 * component, and writes only a regular file (sysfs attributes are). The open never blocks: a FIFO
 * without a reader fails there (-ENXIO); anything else that opens but is not a regular file is -EINVAL.
 */
int gs_write_attr(const char *path, const char *value)
{
	size_t len = strlen(value);
	struct stat st;
	ssize_t n;
	int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return -EINVAL;
	}
	do {
		n = write(fd, value, len);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	if (close(fd) < 0)
		return -errno;
	return (size_t)n == len ? 0 : -EIO;
}

int gs_fsync_dir(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int r = 0;

	if (fd < 0)
		return -errno;
	if (fsync(fd) < 0)
		r = -errno;
	close(fd);
	return r;
}

static int dirname_of(const char *path, char *out, size_t len)
{
	const char *slash = strrchr(path, '/');

	if (!slash)
		return gs_pathf(out, len, ".");
	if (slash == path)
		return gs_pathf(out, len, "/");
	if ((size_t)(slash - path) >= len)
		return -ENAMETOOLONG;
	memcpy(out, path, (size_t)(slash - path));
	out[slash - path] = '\0';
	return 0;
}

/*
 * Write a new file next to path, fsync it, rename over path, fsync the directory. The temporary name is
 * unique (mkostemp), so a file left by an earlier run that lost power never blocks the write.
 */
int gs_write_file_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
	char tmp[GS_PATH_MAX], dir[GS_PATH_MAX];
	const char *p = data;
	size_t off = 0;
	int fd, r;

	if (gs_pathf(tmp, sizeof(tmp), "%s.gs-tmp.XXXXXX", path) || dirname_of(path, dir, sizeof(dir)))
		return -ENAMETOOLONG;
	fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0)
		return -errno;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			r = -errno;
			goto fail;
		}
		off += (size_t)n;
	}
	if (fchmod(fd, mode) < 0 || fsync(fd) < 0) {
		r = -errno;
		goto fail;
	}
	if (close(fd) < 0) {
		r = -errno;
		unlink(tmp);
		return r;
	}
	if (rename(tmp, path) < 0) {
		r = -errno;
		unlink(tmp);
		return r;
	}
	return gs_fsync_dir(dir);
fail:
	close(fd);
	unlink(tmp);
	return r;
}

int gs_mkdir_p(const char *path, mode_t mode)
{
	char buf[GS_PATH_MAX];
	size_t i;

	if (gs_pathf(buf, sizeof(buf), "%s", path))
		return -ENAMETOOLONG;
	for (i = 1; buf[i]; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, mode) < 0 && errno != EEXIST)
			return -errno;
		buf[i] = '/';
	}
	if (mkdir(buf, mode) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

/*
 * Whole regular file up to max bytes; *data is NUL-terminated and must be freed.
 * Reads until EOF rather than trusting st_size: procfs files report 0.
 */
int gs_read_whole(const char *path, char **data, size_t *len, size_t max)
{
	struct stat st;
	char *buf;
	size_t off = 0, cap;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -EINVAL;
	}
	if ((unsigned long long)st.st_size > max) {
		close(fd);
		return -EFBIG;
	}
	cap = st.st_size > 0 ? (size_t)st.st_size + 1 : (max < 4096 ? max + 1 : 4096);
	buf = malloc(cap);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	for (;;) {
		ssize_t n;

		if (off + 1 == cap) {
			char *nb;

			if (off == max) {
				char extra;

				do {
					n = read(fd, &extra, 1);
				} while (n < 0 && errno == EINTR);
				if (n == 0)
					break;
				free(buf);
				close(fd);
				return n > 0 ? -EFBIG : -EIO;
			}
			cap = cap - 1 > max / 2 ? max + 1 : 2 * (cap - 1) + 1;
			nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(fd);
				return -ENOMEM;
			}
			buf = nb;
		}
		n = read(fd, buf + off, cap - 1 - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			n = -errno;
			free(buf);
			close(fd);
			return (int)n;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	close(fd);
	buf[off] = '\0';
	*data = buf;
	*len = off;
	return 0;
}

static const char *const system_path[] = {
	"/usr/sbin", "/usr/bin", "/sbin", "/bin", NULL,
};

int gs_find_program(const char *name, char *out, size_t len)
{
	size_t i;

	if (name[0] == '/') {
		if (access(name, X_OK) != 0)
			return -errno;
		return gs_pathf(out, len, "%s", name);
	}
	if (strchr(name, '/'))
		return -EINVAL;
	for (i = 0; system_path[i]; i++) {
		if (gs_pathf(out, len, "%s/%s", system_path[i], name))
			continue;
		if (access(out, X_OK) == 0)
			return 0;
	}
	out[0] = '\0';
	return -ENOENT;
}

/* The fixed environment of every program the CLI runs. */
static char *const run_envp[] = {
	"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL,
};

/*
 * Run argv without a shell. argv[0] is an absolute path or a bare program name
 * looked up in the fixed system PATH. Returns the exit status, 128+signal, or
 * -errno when the program cannot be started.
 */
int gs_run(const char *const argv[])
{
	char prog[GS_PATH_MAX];
	pid_t pid;
	int status, r;

	r = gs_find_program(argv[0], prog, sizeof(prog));
	if (r)
		return r;
	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return -errno;
	if (pid == 0) {
		execve(prog, (char *const *)argv, run_envp);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -errno;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -ECHILD;
}

/* Milliseconds left until GS_CAPTURE_TIMEOUT_MS after t0; 0 once it has passed. */
static int capture_left(const struct timespec *t0)
{
	struct timespec now;
	long ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (long)GS_CAPTURE_TIMEOUT_MS - ((long)(now.tv_sec - t0->tv_sec) * 1000L +
					    (now.tv_nsec - t0->tv_nsec) / 1000000L);
	return ms > 0 ? (int)ms : 0;
}

int gs_run_capture(const char *const argv[], char *out, size_t len)
{
	char prog[GS_PATH_MAX], rest[512];
	struct timespec t0;
	size_t off = 0;
	int fd[2], status, left;
	ssize_t n;
	pid_t pid, done = 0;

	out[0] = '\0';
	if (gs_find_program(argv[0], prog, sizeof(prog)) || pipe(fd))
		return -1;
	fflush(NULL);
	clock_gettime(CLOCK_MONOTONIC, &t0);
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
		execve(prog, (char *const *)argv, run_envp);
		_exit(127);
	}
	close(fd[1]);
	/* Read to the end, so a long listing never blocks the child; what does not fit is dropped. */
	while ((left = capture_left(&t0)) > 0) {
		struct pollfd p = { .fd = fd[0], .events = POLLIN };
		int r = poll(&p, 1, left);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		n = off + 1 < len ? read(fd[0], out + off, len - 1 - off) : read(fd[0], rest, sizeof(rest));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (off + 1 < len)
			off += (size_t)n;
	}
	close(fd[0]);
	out[off] = '\0';
	while (!(done = waitpid(pid, &status, WNOHANG)) && capture_left(&t0) > 0)
		gs_sleep_ms(20);
	if (done != pid) {
		/* Past the deadline: kill it, and never block on a child stuck in the kernel. */
		kill(pid, SIGKILL);
		for (unsigned int i = 0; i < 25 && waitpid(pid, &status, WNOHANG) == 0; i++)
			gs_sleep_ms(20);
		out[0] = '\0';
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void gs_sleep_ms(unsigned int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

int gs_boot_id(char *buf, size_t len)
{
	return gs_read_attr("/proc/sys/kernel/random/boot_id", buf, len);
}

/* Absolute, no empty, "." or ".." components, no trailing slash (except "/"). */
int gs_validate_abs_path(const char *p)
{
	const char *c;

	if (!p || p[0] != '/' || strlen(p) >= GS_PATH_MAX - 256)
		return -1;
	if (strcmp(p, "/") == 0)
		return 0;
	for (c = p; *c; ) {
		const char *next;
		size_t n;

		if (*c != '/')
			return -1;
		c++;
		next = strchr(c, '/');
		n = next ? (size_t)(next - c) : strlen(c);
		if (n == 0 || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
			return -1;
		for (size_t i = 0; i < n; i++) {
			unsigned char ch = (unsigned char)c[i];

			if (ch < 0x21 || ch == 0x7f)
				return -1;
		}
		c += n;
	}
	return 0;
}

/* Exclusive non-blocking lock held until exit. 0, or GS_EXIT_LOCKED / GS_EXIT_FAIL. */
int gs_lock(void)
{
	int fd;

	if ((fd = gs_mkdir_p(GS_RUN_DIR, 0755))) {
		fprintf(stderr, "geforce-sriov: cannot create %s: %s\n", GS_RUN_DIR, strerror(-fd));
		return GS_EXIT_FAIL;
	}
	fd = open(GS_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		fprintf(stderr, "geforce-sriov: cannot open %s: %s\n", GS_LOCK_PATH, strerror(errno));
		return GS_EXIT_FAIL;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "geforce-sriov: another geforce-sriov process holds %s\n", GS_LOCK_PATH);
		close(fd);
		return GS_EXIT_LOCKED;
	}
	return 0; /* fd intentionally kept open: the lock lives as long as the process */
}

/* ---- bounded operations (activation only) -------------------------------- */

/*
 * One host operation with a deadline, run in a child so that an operation stuck in the kernel
 * cannot hold the caller. A timeout returns -ETIMEDOUT: the operation may still take effect, so
 * activation treats it as uncertain device state (exit 5, guard retained, no second attempt).
 * These are the struct gs_host deadline callbacks; a fixture host supplies its own to model
 * the same single operation and to inject a timeout.
 */
static long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * Wait for pid until deadline, then kill it and give up. group: pid leads its own process group
 * and the whole group is killed, so the programs a command started do not outlive the timeout.
 * Returns 1 when it was reaped into st, 0 on timeout, and -1 when it cannot be waited for (the
 * kernel reaped it already, as it does while SIGCHLD is ignored). In that last case the child is
 * gone and nothing is signalled: its PID may belong to another process by now.
 */
static int reap_bounded(pid_t pid, int *st, long deadline, int group)
{
	for (;;) {
		pid_t r = waitpid(pid, st, WNOHANG);

		if (r == pid)
			return 1;
		if (r < 0 && errno != EINTR)
			return -1;
		if (mono_ms() >= deadline) {
			kill(group ? -pid : pid, SIGKILL);
			/* A child stuck in the kernel cannot be reaped; never block on it. */
			for (unsigned int i = 0; i < 25 && waitpid(pid, st, WNOHANG) == 0; i++)
				gs_sleep_ms(20);
			return 0;
		}
		gs_sleep_ms(20);
	}
}

static int write_attr_deadline_cb(const struct gs_host *h, const char *path, const char *value, unsigned int ms)
{
	long deadline = mono_ms() + (long)(ms ? ms : 1);
	int fd[2], st, result = -EIO;
	pid_t pid;
	ssize_t n;

	if (!h->write_attr)
		return -ENOSYS;
	if (pipe(fd))
		return -errno;
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -errno;
	}
	if (pid == 0) {
		int r;

		close(fd[0]);
		r = h->write_attr(path, value);
		n = write(fd[1], &r, sizeof(r));
		(void)n;
		close(fd[1]);
		_exit(0);
	}
	close(fd[1]);
	/*
	 * Wait for the result no longer than the deadline. A plain read here would block for as long
	 * as the child's write is stuck in the kernel, because the child keeps the pipe open.
	 */
	for (;;) {
		struct pollfd pfd = { .fd = fd[0], .events = POLLIN };
		long left = deadline - mono_ms();
		int ready = poll(&pfd, 1, left > 0 ? (int)left : 0);

		if (ready > 0)
			break;
		if (ready < 0 && errno == EINTR)
			continue;
		close(fd[0]);
		reap_bounded(pid, &st, 0, 0);
		return -ETIMEDOUT;
	}
	while ((n = read(fd[0], &result, sizeof(result))) < 0 && errno == EINTR)
		;
	close(fd[0]);
	/*
	 * The child sends its result after the write returned, so a complete result is the write's
	 * outcome even when the child can no longer be waited for. Without one, the outcome is unknown.
	 */
	if (n != (ssize_t)sizeof(result) || reap_bounded(pid, &st, deadline, 0) == 0)
		return -ETIMEDOUT;
	return result;
}

/*
 * The command runs in a process group of its own, and the live host's program is executed by the
 * child itself instead of through a second fork. A timeout then kills the program and everything
 * it started: a helper such as sriov-manage must not keep moving a GPU between drivers after
 * activation has reported the timeout and gone on to its cleanup.
 */
static int run_deadline_cb(const struct gs_host *h, const char *const argv[], unsigned int ms)
{
	long deadline = mono_ms() + (long)(ms ? ms : 1);
	char prog[GS_PATH_MAX] = "";
	int st, r;
	pid_t pid;

	if (!h->run)
		return -ENOSYS;
	if (h->run == gs_run && (r = gs_find_program(argv[0], prog, sizeof(prog))))
		return r;
	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return -errno;
	if (pid == 0) {
		/* Without its own group a timeout could not kill what the command starts: do not run it. */
		if (setpgid(0, 0) && getpgrp() != getpid())
			_exit(126);
		if (prog[0]) {
			execve(prog, (char *const *)argv, run_envp);
			_exit(127);
		}
		_exit(h->run(argv) & 0xff);
	}
	/* Both sides set the group, so it exists whichever of them runs first. */
	setpgid(pid, pid);
	r = reap_bounded(pid, &st, deadline, 1);
	if (r == 0)
		return -ETIMEDOUT;
	/* The command ended and its status is lost: a failure, not a timeout. */
	if (r < 0)
		return -ECHILD;
	if (WIFEXITED(st))
		return WEXITSTATUS(st);
	if (WIFSIGNALED(st))
		return 128 + WTERMSIG(st);
	return -ECHILD;
}

/*
 * One open of a device node in a child, so that an open stuck in the kernel cannot hold the caller. The
 * child's exit status carries errno of a failed open (0: it opened). A child that does not end in time is
 * killed and not waited for beyond reap_bounded's grace.
 */
static int open_node_cb(const struct gs_host *h, const char *path, unsigned int ms)
{
	long deadline = mono_ms() + (long)(ms ? ms : 1);
	int st, r;
	pid_t pid;

	(void)h;
	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return -errno;
	if (pid == 0) {
		int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);

		if (fd < 0)
			_exit(errno > 0 && errno < 256 ? errno : ECHILD);
		close(fd);
		_exit(0);
	}
	r = reap_bounded(pid, &st, deadline, 0);
	if (r == 0)
		return -ETIMEDOUT;
	/* The child ended and its status is lost, or it did not exit: not a verdict on the open. */
	if (r < 0 || !WIFEXITED(st))
		return -ECHILD;
	return WEXITSTATUS(st) ? -WEXITSTATUS(st) : 0;
}

const struct gs_host gs_host_live = {
	.sys = GS_LIVE_SYSFS,
	.proc = GS_LIVE_PROC,
	.state_dir = GS_STATE_DIR,
	.run_dir = GS_RUN_DIR,
	.write_attr = gs_write_attr,
	.run = gs_run,
	.capture = gs_run_capture,
	.write_attr_deadline = write_attr_deadline_cb,
	.run_deadline = run_deadline_cb,
	.open_node = open_node_cb,
	.root = "",
	.build = &gs_build_live,
	.enable_prereq = gs_enable_prereq,
	.system = 1,
};
