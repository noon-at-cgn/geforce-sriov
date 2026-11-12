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

/* ---- loaded display patch (procfs, read-only) ----------------------------------------- */

/* The 580.159.01 display patch: the call instruction and its replacement (P0-PATCH-MECHANICS, driver/display.c). */
static const unsigned char display_before[] = { 0xe8, 0x00, 0x00, 0x00, 0x00 };
static const unsigned char display_after[] = { 0x31, 0xc0, 0x90, 0x90, 0x90 };
#define DISPLAY_PATCH_LEN (sizeof(display_after))

/*
 * The record's driver_version_confirmed=0 describes the build, not the version: it is also written
 * for the expected driver version when the combination of features or input packages has no recorded
 * test. This is the sentence for that case; another version gets the "not guaranteed to work" sentence.
 */
void gs_combination_line(const struct gs_build *b, char *line, size_t len)
{
	snprintf(line, len, "NVIDIA's driver %.32s is confirmed to work, but the installed build combines features or "
		 "input packages that are not confirmed to work together (display=%s, host_cuda=%s)", b->driver_version,
		 b->display < 0 ? "not recorded" : b->display ? "on" : "off",
		 b->host_cuda < 0 ? "not recorded" : b->host_cuda ? "on" : "off");
}

/* n bytes at off of the open file into buf: 0, or -1 on a short read. */
static int read_file_at(int fd, unsigned long long off, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = pread(fd, (char *)buf + done, n - done, (off_t)(off + done));

		if (r <= 0)
			return -1;
		done += (size_t)r;
	}
	return 0;
}

/* One record line's value into out: 1 when the record has key, else 0 with out "". Reads b->record. */
int gs_record_value(const char *record, const char *key, char *out, size_t len)
{
	char *data, *line, *save = NULL;
	size_t dlen, klen = strlen(key);
	int found = 0;

	out[0] = '\0';
	if (gs_read_whole(record, &data, &dlen, 1 << 16))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save))
		if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
			gs_copy(out, len, line + klen + 1);
			found = 1;
		}
	free(data);
	return found;
}

/*
 * The text address of function in procroot/kallsyms: 0 with *addr set, -1 when it is not listed (or
 * kptr_restrict hides its address, which prints as 0). kallsyms is a few megabytes; the cap is generous.
 */
static int kallsyms_addr(const char *procroot, const char *function, unsigned long long *addr, char *why, size_t len)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t dlen, flen = strlen(function);
	int found = 0;

	*addr = 0;
	if (gs_pathf(p, sizeof(p), "%s/kallsyms", procroot) || gs_read_whole(p, &data, &dlen, 32u << 20)) {
		snprintf(why, len, "%s/kallsyms cannot be read", procroot);
		return -1;
	}
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
		char *sp = strchr(line, ' '), *name;
		unsigned long long a = 0, ndig = 0;

		if (!sp)
			continue;
		/* The address field: hex digits, then the type letter. */
		for (char *q = line; q < sp; q++)
			if (*q >= '0' && *q <= '9')
				a = (a << 4) | (unsigned long long)(*q - '0'), ndig++;
			else if (*q >= 'a' && *q <= 'f')
				a = (a << 4) | (unsigned long long)(*q - 'a' + 10), ndig++;
			else if (*q >= 'A' && *q <= 'F')
				a = (a << 4) | (unsigned long long)(*q - 'A' + 10), ndig++;
			else
				break;
		if (!ndig || sp[1] == '\0' || sp[2] != ' ')
			continue;
		name = sp + 3;
		if (strncmp(name, function, flen) == 0 && (name[flen] == '\0' || name[flen] == '\t')) {
			if (a == 0) {
				snprintf(why, len, "%s is listed with address 0: kptr_restrict hides it; run as root", function);
				free(data);
				return -1;
			}
			*addr = a;
			found = 1;
		}
	}
	free(data);
	if (!found)
		snprintf(why, len, "%s is not in kallsyms: the module that holds it is not loaded", function);
	return found ? 0 : -1;
}

/* The file offset of virtual address addr in the ELF image fd (a kcore): 0 with *off set, -1 with why. */
static int kcore_file_offset(int fd, unsigned long long addr, unsigned long long *off, char *why, size_t len)
{
	Elf64_Ehdr eh;
	Elf64_Phdr ph;
	unsigned int i;
	int n = 0;

	*off = 0;
	if (read_file_at(fd, 0, &eh, sizeof(eh)) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_phentsize != sizeof(ph)) {
		snprintf(why, len, "its ELF header is not a 64-bit core image");
		return -1;
	}
	for (i = 0; i < eh.e_phnum && i < 512; i++) {
		if (read_file_at(fd, (unsigned long long)eh.e_phoff + (unsigned long long)i * sizeof(ph), &ph, sizeof(ph)))
			break;
		if (ph.p_type != PT_LOAD || addr < ph.p_vaddr || addr - ph.p_vaddr >= ph.p_memsz)
			continue;
		if (addr - ph.p_vaddr >= ph.p_filesz) {
			snprintf(why, len, "the address is in a segment that has no file bytes");
			return -1;
		}
		*off = ph.p_offset + (addr - ph.p_vaddr);
		n = 1;
		break;
	}
	if (!n)
		snprintf(why, len, "no core segment contains the address");
	return n ? 0 : -1;
}

/* 1 when procroot/modules lists a module named vgx_override. */
static int vgx_override_loaded(const char *procroot, char *p, size_t plen)
{
	char *data, *line, *save = NULL;
	size_t dlen;
	int found = 0;

	if (gs_pathf(p, plen, "%s/modules", procroot) || gs_read_whole(p, &data, &dlen, 1 << 20))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save))
		found = strncmp(line, "vgx_override ", 13) == 0;
	free(data);
	return found;
}

/*
 * The loaded display patch, read from kallsyms and kcore at the function-relative offset the build record
 * holds (idx39_function=, idx39_function_offset=). MATCH only when the replacement bytes are there;
 * UNKNOWN when the record, the symbol or the memory cannot be read, and UNKNOWN is never a pass. A loaded
 * vgx_override module is a MISMATCH: its hooks change the same region. 0 always; out->state decides.
 */
int gs_loaded_patch_verify(const char *procroot, const struct gs_build *b, struct gs_loaded_patch_result *out)
{
	char fn[128], off[32], why[256] = "", p[GS_PATH_MAX];
	unsigned long long addr = 0, foff = 0, rel;
	unsigned char buf[DISPLAY_PATCH_LEN];
	int fd, r;

	memset(out, 0, sizeof(*out));
	out->state = GS_LOADED_UNKNOWN;
	if (!b || !b->record_read) {
		snprintf(out->why, sizeof(out->why), "no build record names the loaded patch");
		return 0;
	}
	if (!gs_record_value(b->record, "idx39_function", fn, sizeof(fn)) ||
	    !gs_record_value(b->record, "idx39_function_offset", off, sizeof(off))) {
		snprintf(out->why, sizeof(out->why),
			 "the build record has no idx39_function and idx39_function_offset lines");
		return 0;
	}
	char *end = NULL;

	/*
	 * The driver step writes the offset in decimal (it converts the patcher's 0x form), and decimal
	 * is the only spelling read here, so a value such as 50 cannot mean two different offsets.
	 */
	errno = 0;
	rel = strtoull(off, &end, 10);
	if (off[0] < '0' || off[0] > '9' || errno || *end) {
		snprintf(out->why, sizeof(out->why), "idx39_function_offset=%s is not a valid offset", off);
		return 0;
	}
	gs_copy(out->function, sizeof(out->function), fn);
	out->offset = rel;
	if (vgx_override_loaded(procroot, p, sizeof(p))) {
		out->state = GS_LOADED_MISMATCH;
		snprintf(out->why, sizeof(out->why), "vgx_override is loaded (%.100s): it hooks the same region", p);
		return 0;
	}
	if (kallsyms_addr(procroot, fn, &addr, why, sizeof(why))) {
		snprintf(out->why, sizeof(out->why), "%s", why);
		return 0;
	}
	if (gs_pathf(p, sizeof(p), "%s/kcore", procroot) || (fd = open(p, O_RDONLY | O_CLOEXEC)) < 0) {
		snprintf(out->why, sizeof(out->why), "%s/kcore cannot be opened: the loaded bytes are unknown", procroot);
		return 0;
	}
	if (kcore_file_offset(fd, addr + rel, &foff, why, sizeof(why))) {
		close(fd);
		snprintf(out->why, sizeof(out->why), "%s", why);
		return 0;
	}
	r = read_file_at(fd, foff, buf, sizeof(buf));
	close(fd);
	if (r) {
		snprintf(out->why, sizeof(out->why), "the bytes at %s+0x%llx cannot be read from kcore", fn, rel);
		return 0;
	}
	{
		size_t n = sizeof(buf);

		out->found_len = n;
		memcpy(out->found, buf, n);
	}
	if (memcmp(buf, display_after, sizeof(buf)) == 0) {
		out->state = GS_LOADED_MATCH;
		return 0;
	}
	out->state = GS_LOADED_MISMATCH;
	if (memcmp(buf, display_before, sizeof(buf)) == 0)
		snprintf(out->why, sizeof(out->why), "%s+0x%llx holds the unpatched call instruction", fn, rel);
	else
		snprintf(out->why, sizeof(out->why), "%s+0x%llx holds %02x %02x %02x %02x %02x, not the patched instruction",
			 fn, rel, buf[0], buf[1], buf[2], buf[3], buf[4]);
	return 0;
}
