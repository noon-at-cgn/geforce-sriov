/* SPDX-License-Identifier: MIT */
/* Table of measured revisions of 1 to 16 bytes. Each row acts only on the exact
 * file whose whole-file SHA-256 was measured before or after the revision. */
#include "ada.h"
#include "elf_reader.h"
#include "insn_find.h"
#include "patch.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct gs_ada_revision revisions[] = {
	{"module-gate",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state is the stock object; output state is the gate-only object, not the supported NVIDIA host driver setup",
	 "580.159.01",
	 "4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449",
	 "ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b",
	 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}, 0, "rm_is_supported_device", 0x1cf},
	{"module-gate",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state is the object with the scheduler patch; output state is the supported NVIDIA host driver setup",
	 "580.159.01",
	 "ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701",
	 "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12",
	 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}, 1, "rm_is_supported_device", 0x1cf},
	{"vf-count",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state carries the probe-gate patch and the scheduler patch; output state reports 4 total VFs",
	 "580.159.01",
	 "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12",
	 "946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29",
	 0x126644, 4, {0x0f, 0xb7, 0x45, 0x06}, {0x31, 0xc0, 0xb0, 0x04}, 1, "_nv039075rm", 0x44},
	{"plugin-mixed",
	 "NVIDIA vGPU plugin shared library (580.159.01); input state is the stock plugin; output state is the input of plugin-div4, not the installed copy",
	 "580.159.01",
	 "7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7",
	 "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6",
	 0x8fc17, 15,
	 {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
	 {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00}, 0, NULL, 0},
	{"plugin-div4",
	 "NVIDIA vGPU plugin shared library (580.159.01); input state carries plugin-mixed; output state divides the Ada host framebuffer by 4 VFs and is the patched plugin copy for Ada and Ampere hosts",
	 "580.159.01",
	 "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6",
	 "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c",
	 0xa5f28, 6, {0x8b, 0xbb, 0xd0, 0x29, 0x00, 0x00}, {0xbf, 0x04, 0x00, 0x00, 0x00, 0x90}, 1, NULL, 0},
	{"ada-placement",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state is the final object of the plain chain; output state stores 0 instead of 1 as homogeneousPlacementSupported (the immediate of `movb $0x1,0x134c3a(%rsi,%rax,1)` becomes `movb $0x0,...` in _nv042355rm), so an Ada VM takes one contiguous free piece of video memory when it starts; applied only when an Ada card is selected",
	 "580.159.01",
	 "946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29",
	 "20eaebcac4f5220d21f64bcd5145143abe8f806a25cb9e249088a1b98df7d157",
	 0xd3a86, 1, {0x01}, {0x00}, 1, "_nv042355rm", 0x7f6}
};
#define REVISION_COUNT (sizeof(revisions) / sizeof(revisions[0]))

static int fail(char *e, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *e, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (e && n) {
		va_start(ap, fmt);
		vsnprintf(e, n, fmt, ap);
		va_end(ap);
	}
	return -1;
}

/* Width has already been admitted by admit_size. */
static int bytes_equal(const struct gs_ada_revision *r, const unsigned char *data, const unsigned char *want)
{
	return !memcmp(data + r->site, want, r->width);
}

/* Size has already been admitted; hash is the whole-file digest of data. */
static int classify(const struct gs_ada_revision *r, const unsigned char *data, const char *hash, char *err, size_t errlen)
{
	if (!strcmp(hash, r->input_sha)) {
		if (!bytes_equal(r, data, r->old_bytes))
			return fail(err, errlen, "input SHA-256 matches but the bytes at the patch site are not the recorded original bytes");
		return 0;
	}
	if (!strcmp(hash, r->output_sha)) {
		if (!bytes_equal(r, data, r->new_bytes))
			return fail(err, errlen, "output SHA-256 matches but the bytes at the patch site are not the recorded patched bytes");
		return 1;
	}
	return fail(err, errlen, "SHA-256 matches neither the recorded input nor the recorded output of this byte patch");
}

static int admit_size(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r || (!data && size))
		return fail(err, errlen, "missing byte patch or data");
	if (r->width == 0 || r->width > sizeof(r->old_bytes))
		return fail(err, errlen, "patch width is outside 1 to 16 bytes");
	if (r->site > size || size - r->site < r->width)
		return fail(err, errlen, "file is smaller than the patch site");
	return 0;
}

size_t gs_ada_revision_count(void)
{
	return REVISION_COUNT;
}

const struct gs_ada_revision *gs_ada_revision_at(size_t index)
{
	return index < REVISION_COUNT ? &revisions[index] : NULL;
}

int gs_ada_state(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen)
{
	char hash[65];

	if (admit_size(r, data, size, err, errlen))
		return -1;
	gs_sha256(data, size, SIZE_MAX, hash);
	return classify(r, data, hash, err, errlen);
}

/* Moves the buffer from state `from` to the other state, restoring the
 * previous width bytes if the resulting whole-file digest is not `expect`. */
static int rewrite(const struct gs_ada_revision *r, unsigned char *data, size_t size, int from,
		   const unsigned char *to, const char *expect, char *err, size_t errlen)
{
	unsigned char prev[sizeof(r->old_bytes)];
	char hash[65];
	int st = gs_ada_state(r, data, size, err, errlen);

	if (st < 0)
		return -1;
	if (st != from)
		return fail(err, errlen, from == 0 ? "apply requires the unpatched input; this file is already patched"
						   : "revert requires the patched output; this file is not patched");
	memcpy(prev, data + r->site, r->width);
	memcpy(data + r->site, to, r->width);
	gs_sha256(data, size, SIZE_MAX, hash);
	if (strcmp(hash, expect)) {
		memcpy(data + r->site, prev, r->width);
		return fail(err, errlen, "resulting SHA-256 does not match the recorded digest; buffer restored");
	}
	return 0;
}

int gs_ada_apply(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r)
		return fail(err, errlen, "missing byte patch");
	return rewrite(r, data, size, 0, r->new_bytes, r->output_sha, err, errlen);
}

int gs_ada_revert(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r)
		return fail(err, errlen, "missing byte patch");
	return rewrite(r, data, size, 1, r->old_bytes, r->input_sha, err, errlen);
}

/* Returns the row whose input_sha (state 0) or output_sha (state 1) equals hash, or NULL;
 * *many is set when more than one row matches on that side. */
static const struct gs_ada_revision *match(const struct gs_ada_revision *rows, size_t count,
					   const char *hash, int state, int *many)
{
	const struct gs_ada_revision *pick = NULL;
	size_t i;

	*many = 0;
	for (i = 0; i < count; i++) {
		if (strcmp(hash, state ? rows[i].output_sha : rows[i].input_sha))
			continue;
		if (pick)
			*many = 1;
		pick = &rows[i];
	}
	return pick;
}

int gs_ada_find_in(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, size_t size,
		   int mode, const struct gs_ada_revision **out, int *state, char *err, size_t errlen)
{
	const struct gs_ada_revision *pick = NULL;
	char hash[65];
	int st = 0, many = 0;

	if (!out || !state || (!data && size) || (!rows && count))
		return fail(err, errlen, "missing data, table or result pointers");
	if (mode != GS_ADA_ANY && mode != GS_ADA_FORWARD && mode != GS_ADA_BACKWARD)
		return fail(err, errlen, "unknown lookup direction");
	gs_sha256(data, size, SIZE_MAX, hash);
	if (mode != GS_ADA_BACKWARD)
		pick = match(rows, count, hash, 0, &many);
	if (!pick && mode != GS_ADA_FORWARD) {
		pick = match(rows, count, hash, 1, &many);
		st = 1;
	}
	if (!pick) {
		if (mode == GS_ADA_FORWARD && match(rows, count, hash, 1, &many))
			return fail(err, errlen, "already at output state and no supported byte patch takes this file as input; refusing to repatch or create output");
		if (mode == GS_ADA_BACKWARD && match(rows, count, hash, 0, &many))
			return fail(err, errlen, "not at output state of any supported byte patch; refusing to revert or create output");
		return fail(err, errlen, "no supported byte patch matches this file's SHA-256");
	}
	if (many)
		return fail(err, errlen, "SHA-256 matches more than one supported byte patch in this direction; refusing ambiguous selection");
	if (admit_size(pick, data, size, err, errlen))
		return -1;
	if (!bytes_equal(pick, data, st ? pick->new_bytes : pick->old_bytes))
		return fail(err, errlen, st ? "output SHA-256 matches but the bytes at the patch site are not the recorded patched bytes"
					    : "input SHA-256 matches but the bytes at the patch site are not the recorded original bytes");
	*out = pick;
	*state = st;
	return 0;
}

int gs_ada_find(const unsigned char *data, size_t size, int mode,
		const struct gs_ada_revision **out, int *state, char *err, size_t errlen)
{
	return gs_ada_find_in(revisions, REVISION_COUNT, data, size, mode, out, state, err, errlen);
}

/* ------------------------------------------------------------ plugin locators
 *
 * Structural locators for the two libnvidia-vgpu.so rows, following the measured
 * shapes in RE-PLUGIN-LOCATORS.md. They use file bytes only: no symbol names, no
 * fixed offsets and no relocation tables. A .text address is sh_addr + index and
 * is used only to resolve RIP-relative and branch targets; the reported site is
 * always a file offset. Each locator must find exactly one site and re-derive
 * its operands from the matched code, or refuse. */

struct plugin_site {
	size_t site;              /* file offset of the first changed byte */
	int state;                /* 0 = stock, 1 = already patched */
	unsigned width;           /* bytes the edit writes */
	unsigned divisor;         /* plugin-div4: divisor installed by this edit */
	const char *function;     /* containing symbol, NULL when the row has no symbol anchor */
	size_t function_offset;   /* site relative to that symbol */
	unsigned char old_bytes[16];
	unsigned char new_bytes[16];
};

/* ABSENT: the row has no locator for this input. NOTFOUND: the row's window is not in .text. It
 * refuses a forward step when no row is located or a later row is; it never refuses the rows
 * before it in the chain, since another build may hold other code there. */
enum { GS_ADA_ABSENT = 1, GS_ADA_NOTFOUND = 2 };

static int32_t get_i32(const unsigned char *p)
{
	uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;

	return (int32_t)v;
}

static void put_i32(unsigned char *p, int32_t v)
{
	uint32_t u = (uint32_t)v;

	p[0] = (unsigned char)u;
	p[1] = (unsigned char)(u >> 8);
	p[2] = (unsigned char)(u >> 16);
	p[3] = (unsigned char)(u >> 24);
}

static size_t count_bytes(const unsigned char *p, size_t n, const char *needle, size_t len)
{
	size_t i, hits = 0;

	if (len == 0 || len > n)
		return 0;
	for (i = 0; i + len <= n; i++)
		if (!memcmp(p + i, needle, len))
			hits++;
	return hits;
}

static size_t find_bytes(const unsigned char *p, size_t n, const char *needle, size_t len)
{
	size_t i;

	for (i = 0; i + len <= n; i++)
		if (!memcmp(p + i, needle, len))
			return i;
	return n;
}

static int text_section(const struct gs_elf *e, const unsigned char **bytes, size_t *size, uint64_t *addr,
			char *err, size_t errlen)
{
	Elf64_Shdr sh;
	size_t index;

	if (gs_elf_section_by_name(e, ".text", &index, &sh, NULL, 0) || sh.sh_type != SHT_PROGBITS ||
	    !(sh.sh_flags & SHF_EXECINSTR) || sh.sh_addr == 0 || sh.sh_size < 0x20)
		return fail(err, errlen, "no mapped .text section (fully section-stripped object is unsupported)");
	*bytes = e->data + sh.sh_offset;
	*size = sh.sh_size;
	*addr = sh.sh_addr;
	return 0;
}

/* `cmp r64,r64` (REX.W, 39/3b, mod=11): returns the rm register or -1. */
static int cmp_rm(const unsigned char *p, size_t n, size_t at)
{
	if (at + 3 > n)
		return -1;
	if (p[at] != 0x48 && p[at] != 0x49 && p[at] != 0x4c && p[at] != 0x4d)
		return -1;
	if (p[at + 1] != 0x39 && p[at + 1] != 0x3b)
		return -1;
	if ((p[at + 2] & 0xc0) != 0xc0)
		return -1;
	return p[at + 2] & 7;
}

static int plugin_placement(const struct gs_elf *e, struct plugin_site *out, char *err, size_t errlen)
{
	static const char full[] = "vmiop_log: (0x%x): placement_fb_length 0x%llx is not equal to allocation_size 0x%llx";
	static const char tail[] = "is not equal to allocation_size 0x%llx";
	static const char desc_or[] = "\x83\x48\x10\x02";
	const unsigned char *text, *rodata;
	Elf64_Shdr rsh;
	size_t tn, rn, ridx, i, j, k, refs = 0, blocks = 0, ncand = 0, distinct = 0;
	uint64_t taddr, raddr, log_va, log_lea = 0, target = 0;
	uint64_t cand[16], cand_target[16];
	int cand_state[16], cand_src[16];
	int32_t cand_desc[16];
	int state = 0, src = 0;
	int32_t desc = 0;
	size_t off;

	if (gs_elf_section_by_name(e, ".rodata", &ridx, &rsh, NULL, 0) || rsh.sh_type != SHT_PROGBITS ||
	    rsh.sh_addr == 0 || rsh.sh_size == 0)
		return fail(err, errlen, "no .rodata section");
	rodata = e->data + rsh.sh_offset;
	rn = rsh.sh_size;
	raddr = rsh.sh_addr;
	if (count_bytes(rodata, rn, full, sizeof(full) - 1) == 1) {
		log_va = raddr + find_bytes(rodata, rn, full, sizeof(full) - 1);
	} else {
		size_t hits = count_bytes(rodata, rn, tail, sizeof(tail) - 1);

		if (hits != 1)
			return fail(err, errlen, "%zu copies of the 'is not equal to allocation_size' log string (want 1)",
				    hits);
		log_va = raddr + find_bytes(rodata, rn, tail, sizeof(tail) - 1);
	}

	if (text_section(e, &text, &tn, &taddr, err, errlen))
		return -1;
	for (i = 0; i + 7 <= tn; i++) {
		int64_t ref;

		if (text[i] != 0x48 || text[i + 1] != 0x8d || text[i + 2] != 0x35)
			continue;
		ref = (int64_t)(taddr + i) + 7 + get_i32(text + i + 3);
		if ((uint64_t)ref == log_va) {
			log_lea = taddr + i;
			refs++;
		}
	}
	if (refs != 1)
		return fail(err, errlen, "%zu lea rsi refs to the log string (want 1)", refs);

	for (i = 0; i + 0x20 <= tn; i++) {
		if (text[i] != 0x48 || text[i + 1] != 0x8b || text[i + 2] != 0x83)
			continue;
		for (k = i + 3; k + sizeof(desc_or) - 1 <= i + 0x18; k++)
			if (!memcmp(text + k, desc_or, sizeof(desc_or) - 1))
				break;
		if (k + sizeof(desc_or) - 1 > i + 0x18)
			continue;
		blocks++;
		target = taddr + i;
		desc = get_i32(text + i + 3);
		for (j = 0; j + 6 <= tn; j++) {
			size_t len;
			int64_t branch;

			if (text[j] == 0x0f && text[j + 1] >= 0x80 && text[j + 1] <= 0x8f) {
				len = 6;
				branch = (int64_t)(taddr + j) + 6 + get_i32(text + j + 2);
			} else if (text[j] == 0xe9) {
				len = 5;
				branch = (int64_t)(taddr + j) + 5 + get_i32(text + j + 1);
			} else if (text[j] >= 0x70 && text[j] <= 0x7f) {
				len = 2;
				branch = (int64_t)(taddr + j) + 2 + (int8_t)text[j + 1];
			} else {
				continue;
			}
			if ((uint64_t)branch != target)
				continue;
			/* The error block that logs the string is the fall-through. */
			if (!((uint64_t)(taddr + j) + len <= log_lea && log_lea <= (uint64_t)(taddr + j) + len + 0x40))
				continue;
			if (len == 6 && text[j + 1] == 0x84 && j >= 3 && cmp_rm(text, tn, j - 3) >= 0) {
				if (ncand >= sizeof(cand) / sizeof(cand[0]))
					return fail(err, errlen, "too many candidate sites (want 1)");
				cand[ncand] = taddr + j - 3;
				cand_target[ncand] = target;
				cand_state[ncand] = 0;
				cand_src[ncand] = cmp_rm(text, tn, j - 3);
				cand_desc[ncand] = desc;
				ncand++;
			} else if (len == 5 && j >= 11 && !memcmp(text + j - 11, "\x48\x8b\x93", 3) &&
				   !memcmp(text + j - 4, "\x48\x89\x4a\x08", 4)) {
				if (ncand >= sizeof(cand) / sizeof(cand[0]))
					return fail(err, errlen, "too many candidate sites (want 1)");
				cand[ncand] = taddr + j - 11;
				cand_target[ncand] = target;
				cand_state[ncand] = 1;
				cand_src[ncand] = 1;
				cand_desc[ncand] = get_i32(text + j - 8);
				ncand++;
			}
		}
	}
	if (blocks == 0)
		return fail(err, errlen, "no memdesc update block (mov rax,[rbx+D]; or dword [rax+0x10],2)");
	if (blocks > 8)
		return fail(err, errlen, "%zu memdesc update blocks (implausible)", blocks);
	if (ncand == 0)
		return fail(err, errlen, "the branch that skips the 'is not equal to allocation_size' block was not found");
	for (i = 0; i < ncand; i++) {
		for (j = 0; j < i; j++)
			if (cand[j] == cand[i])
				break;
		if (j == i)
			distinct++;
	}
	if (distinct != 1)
		return fail(err, errlen, "%zu candidate sites (want 1)", distinct);
	state = cand_state[0];
	src = cand_src[0];
	desc = cand_desc[0];
	target = cand_target[0];

	if (desc != 0x2b58 && desc != 0x2b60 && desc != 0x2b68)
		return fail(err, errlen, "memdesc pointer offset 0x%x is not a measured value", (unsigned)desc);
	if (src != 1)
		return fail(err, errlen, "compared source register is not rcx (rm=%d)", src);
	off = (size_t)(cand[0] - taddr);
	if (off + 16 > tn)
		return fail(err, errlen, "site is truncated (want 16 bytes)");
	if (state == 0) {
		if (cmp_rm(text, tn, off) < 0 || text[off + 3] != 0x0f || text[off + 4] != 0x84)
			return fail(err, errlen, "stock site is not 'cmp r64,r64; je rel32'");
	} else if (memcmp(text + off, "\x48\x8b\x93", 3) != 0) {
		return fail(err, errlen, "patched site is not 'mov rdx,[rbx+D]...'");
	}

	memcpy(out->new_bytes, "\x48\x8b\x93", 3);
	put_i32(out->new_bytes + 3, desc);
	memcpy(out->new_bytes + 7, "\x48\x89\x4a\x08\xe9", 5);
	if (target < cand[0] + 16 ? cand[0] + 16 - target > 0x80000000ull : target - (cand[0] + 16) > 0x7fffffffull)
		return fail(err, errlen, "jump target out of rel32 range");
	put_i32(out->new_bytes + 12, (int32_t)(target - (cand[0] + 16)));
	memcpy(out->old_bytes, text + off, 15);
	if (text[off + 15] != out->new_bytes[15])
		return fail(err, errlen, "byte at site+15 is 0x%02x, the rewrite needs 0x%02x", text[off + 15],
			    out->new_bytes[15]);
	out->site = (size_t)(text - e->data) + off;
	out->state = state;
	out->width = 16;
	out->divisor = 0;
	return 0;
}

static int plugin_div(const struct gs_elf *e, struct plugin_site *out, char *err, size_t errlen)
{
	static const unsigned char prefix[9] = {0x48, 0x89, 0xf8, 0x49, 0xf7, 0xf1, 0x48, 0x89, 0xf8};
	static const unsigned char suffix[15] = {0x48, 0x29, 0xd0, 0x31, 0xd2, 0x48, 0x89, 0x83,
						 0x70, 0x05, 0x00, 0x00, 0x48, 0xf7, 0xf7};
	static const unsigned char type_info[5] = {0xba, 0x03, 0x01, 0x81, 0xa0};
	const unsigned char *text;
	size_t tn, i, hits = 0, site_off = 0;
	uint64_t taddr;
	int state = 0, seen = 0;
	int32_t disp = 0;
	uint32_t imm = 0;

	if (text_section(e, &text, &tn, &taddr, err, errlen))
		return -1;
	for (i = 0; i + 30 <= tn; i++) {
		if (memcmp(text + i, prefix, sizeof(prefix)) != 0)
			continue;
		if (memcmp(text + i + 15, suffix, sizeof(suffix)) != 0)
			continue;
		if (text[i + 9] == 0x8b && text[i + 10] == 0xbb) {
			state = 0;
			disp = get_i32(text + i + 11);
		} else if (text[i + 9] == 0xbf && text[i + 14] == 0x90) {
			state = 1;
			imm = (uint32_t)text[i + 10] | (uint32_t)text[i + 11] << 8 | (uint32_t)text[i + 12] << 16 |
			      (uint32_t)text[i + 13] << 24;
		} else {
			continue;
		}
		site_off = i + 9;
		hits++;
	}
	if (hits == 0)
		return fail(err, errlen, "the host-framebuffer division sequence (mov rax,rdi; div r9; mov rax,rdi; "
					 "mov edi,<div>; ...; mov [rbx+0x570],rax; div rdi) was not found");
	if (hits > 1)
		return fail(err, errlen, "%zu division sequences (want 1)", hits);
	if (state == 0 && disp != 0x29c8 && disp != 0x29d0)
		return fail(err, errlen, "maxVgpus field offset 0x%x is not a measured value", (unsigned)disp);
	if (state == 1 && imm != 2 && imm != 4 && imm != 8)
		return fail(err, errlen, "installed divisor %u is not a measured value", imm);
	for (i = site_off > 0x1000 ? site_off - 0x1000 : 0; i + sizeof(type_info) <= site_off; i++)
		if (!memcmp(text + i, type_info, sizeof(type_info))) {
			seen = 1;
			break;
		}
	if (!seen)
		return fail(err, errlen, "0xa0810103 (GET_VGPU_TYPE_INFO) is not within 0x1000 bytes before the site");
	out->site = (size_t)(text - e->data) + site_off;
	out->state = state;
	out->width = 6;
	out->divisor = state ? imm : 4;
	memcpy(out->old_bytes, text + site_off, 6);
	if (state)
		memcpy(out->new_bytes, out->old_bytes, 6);
	else
		memcpy(out->new_bytes, "\xbf\x04\x00\x00\x00\x90", 6);
	return 0;
}

/* ----------------------------------------------------------- object locators
 *
 * Structural locators for the two host-object rows, following the measured shapes
 * in RE-GATE-VF-LOCATORS.md. Both need the relocatable object's symbol table and
 * .rela.text: the gate's call target is a relocation name, and the VF reader's
 * capability reads must be shown to be real calls. All sites are reported as file
 * offsets; .text addresses are sh_addr + index (0 in an ET_REL object). */

#define GATE_STRING "vGPU host system is not supported by"

struct obj_ctx {
	const struct gs_elf *e;
	const unsigned char *text;
	size_t tn;
	size_t toff;
	size_t text_index;
};

/* Innermost STT_FUNC in .text containing off: 1 found, 0 none, -1 malformed. */
static int containing(const struct obj_ctx *c, uint64_t off, struct gs_elf_sym *out)
{
	struct gs_elf_sym s, best;
	size_t i;
	int found = 0;

	for (i = 1; i < c->e->nsyms; i++) {
		if (gs_elf_symbol(c->e, i, &s, NULL, 0))
			return -1;
		if (s.type != STT_FUNC || s.shndx != c->text_index || s.size == 0)
			continue;
		if (s.value <= off && off < s.value + s.size && (!found || s.value > best.value)) {
			best = s;
			found = 1;
		}
	}
	if (found)
		*out = best;
	return found;
}

static int find_named(const struct obj_ctx *c, const char *name, struct gs_elf_sym *out)
{
	struct gs_elf_sym s;
	size_t i;

	for (i = 1; i < c->e->nsyms; i++) {
		if (gs_elf_symbol(c->e, i, &s, NULL, 0))
			return -1;
		if (s.type == STT_FUNC && s.shndx == c->text_index && !strcmp(s.name, name)) {
			*out = s;
			return 1;
		}
	}
	return 0;
}

static int64_t sign8(unsigned char b)
{
	return b > 0x7f ? (int64_t)b - 0x100 : (int64_t)b;
}

/* True when the relocation names a string in .rodata that contains needle. */
static int reloc_string_is(const struct obj_ctx *c, const struct gs_elf_rela *rel, const char *needle)
{
	Elf64_Shdr ro;
	struct gs_elf_sym s;
	size_t ro_index, i;
	uint64_t off;
	const unsigned char *p;

	if (gs_elf_section_by_name(c->e, ".rodata", &ro_index, &ro, NULL, 0) || ro.sh_type != SHT_PROGBITS)
		return 0;
	if (rel->sym == 0 || gs_elf_symbol(c->e, rel->sym, &s, NULL, 0) || s.shndx != ro_index)
		return 0;
	off = (uint64_t)((int64_t)(s.type == STT_SECTION ? 0 : s.value) + rel->addend);
	if (off >= ro.sh_size)
		return 0;
	p = c->e->data + ro.sh_offset + off;
	for (i = 0; off + i < ro.sh_size; i++)
		if (p[i] == 0)
			break;
	if (off + i >= ro.sh_size)
		return 0;
	return strstr((const char *)p, needle) != NULL;
}

/* PC32 relocation offsets in [lo, hi): 0 ok, -1 malformed or more than cap. */
static int reloc_offsets(const struct obj_ctx *c, uint64_t lo, uint64_t hi, uint64_t *out, size_t cap, size_t *count)
{
	struct gs_elf_rela_iter it;
	struct gs_elf_rela rel;
	size_t n = 0;
	int rc;

	gs_elf_rela_begin(&it, c->text_index);
	while ((rc = gs_elf_rela_next(c->e, &it, &rel, NULL, 0)) > 0) {
		if (rel.type != R_X86_64_PC32 || rel.offset < lo || rel.offset >= hi)
			continue;
		if (n >= cap)
			return -1;
		out[n++] = rel.offset;
	}
	if (rc < 0)
		return -1;
	*count = n;
	return 0;
}

/* First `e8 rel32` whose displacement carries a PC32 relocation in [pos, pos+limit). */
static size_t call_after(const struct obj_ctx *c, size_t pos, size_t limit)
{
	uint64_t offsets[0x40];
	size_t n = 0, i, k;
	uint64_t hi = pos + limit;

	if (hi > c->tn)
		hi = c->tn;
	if (reloc_offsets(c, pos + 1, hi + 1, offsets, sizeof(offsets) / sizeof(offsets[0]), &n))
		return SIZE_MAX;
	for (i = pos; i < hi && i + 5 <= c->tn; i++)
		if (c->text[i] == 0xe8)
			for (k = 0; k < n; k++)
				if (offsets[k] == i + 1)
					return i;
	return SIZE_MAX;
}

/* First occurrence of needle in [from, to). */
static size_t find_in(const struct obj_ctx *c, size_t from, size_t to, const unsigned char *needle, size_t len)
{
	size_t i;

	if (to > c->tn)
		to = c->tn;
	for (i = from; i + len <= to; i++)
		if (!memcmp(c->text + i, needle, len))
			return i;
	return SIZE_MAX;
}

static int obj_open(const unsigned char *data, size_t size, struct gs_elf *e, struct obj_ctx *c, char *err, size_t errlen)
{
	Elf64_Shdr sh;
	size_t index;

	if (gs_elf_open(data, size, e, err, errlen))
		return -1;
	if (gs_elf_section_by_name(e, ".text", &index, &sh, NULL, 0) || sh.sh_type != SHT_PROGBITS ||
	    !(sh.sh_flags & SHF_EXECINSTR) || sh.sh_size < 0x20)
		return fail(err, errlen, "text: .text section missing or not executable");
	c->e = e;
	c->text = e->data + sh.sh_offset;
	c->tn = sh.sh_size;
	c->toff = sh.sh_offset;
	c->text_index = index;
	if (!e->symtab)
		return fail(err, errlen, "symtab: .symtab/.strtab missing; the locator needs symbol and relocation names");
	if (gs_elf_section_by_name(e, ".rela.text", NULL, NULL, NULL, 0))
		return fail(err, errlen, "relocations: .rela.text missing or empty; instruction relocations are required");
	return 0;
}

static int target_seen(const char **targets, size_t n, const char *name)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!strcmp(targets[i], name))
			return 1;
	return 0;
}

static int locate_gate(const struct obj_ctx *c, struct plugin_site *out, char *err, size_t errlen)
{
	struct gs_elf_sym scope, owner;
	struct gs_elf_rela_iter it;
	struct gs_elf_rela rel;
	const char *targets[64];
	size_t ntargets = 0, calls[256], nscoped = 0, all_calls = 0, refs[64], nrefs = 0;
	size_t i, k, at, site = 0, nsites = 0;
	uint64_t lo = 0, hi = c->tn;
	int rc, scoped = find_named(c, "rm_is_supported_device", &scope), state = 0;

	if (scoped < 0)
		return fail(err, errlen, "symtab: malformed symbol table");
	if (scoped) {
		lo = scope.value;
		hi = scope.value + scope.size;
	}

	/* Every relocation that reaches os_is_vgx_hyper: the direct symbol and the
	 * 9-byte `endbr64; jmp` thunks that are called instead of it. */
	gs_elf_rela_begin(&it, c->text_index);
	while ((rc = gs_elf_rela_next(c->e, &it, &rel, NULL, 0)) > 0) {
		if (rel.type != R_X86_64_PC32 || strcmp(rel.sym_name, "os_is_vgx_hyper") != 0 || rel.offset == 0)
			continue;
		if (!target_seen(targets, ntargets, "os_is_vgx_hyper")) {
			if (ntargets >= sizeof(targets) / sizeof(targets[0]))
				return fail(err, errlen, "vgx-thunk: too many distinct call targets");
			targets[ntargets++] = "os_is_vgx_hyper";
		}
		if (containing(c, rel.offset - 1, &owner) != 1)
			continue;
		if (owner.size != 9 || rel.offset != owner.value + 5 ||
		    memcmp(c->text + owner.value, "\xf3\x0f\x1e\xfa\xe9", 5) != 0)
			continue;
		if (!target_seen(targets, ntargets, owner.name)) {
			if (ntargets >= sizeof(targets) / sizeof(targets[0]))
				return fail(err, errlen, "vgx-thunk: too many distinct call targets");
			targets[ntargets++] = owner.name;
		}
	}
	if (rc < 0)
		return fail(err, errlen, "relocations: malformed relocation table");
	if (ntargets == 0)
		return fail(err, errlen, "vgx-thunk: no function reaches os_is_vgx_hyper (neither a 9-byte "
					 "endbr64;jmp thunk nor a direct call)");

	/* Call sites of those targets inside the scope. */
	gs_elf_rela_begin(&it, c->text_index);
	while ((rc = gs_elf_rela_next(c->e, &it, &rel, NULL, 0)) > 0) {
		if (rel.type != R_X86_64_PC32 || rel.offset == 0)
			continue;
		for (k = 0; k < ntargets; k++)
			if (!strcmp(targets[k], rel.sym_name))
				break;
		if (k == ntargets)
			continue;
		at = (size_t)rel.offset - 1;
		if (at + 9 > c->tn || c->text[at] != 0xe8)
			continue;
		all_calls++;
		if (at < lo || at >= hi)
			continue;
		if (nscoped >= sizeof(calls) / sizeof(calls[0]))
			return fail(err, errlen, "vgx-call: too many call sites in scope");
		calls[nscoped++] = at;
	}
	if (rc < 0)
		return fail(err, errlen, "relocations: malformed relocation table");
	if (nscoped == 0)
		return fail(err, errlen, "vgx-call: %s does not call os_is_vgx_hyper (%zu call sites exist "
					 "elsewhere in .text)", scoped ? scope.name : ".text", all_calls);

	for (i = 0; i < nscoped; i++)
		if (calls[i] + 9 <= c->tn && c->text[calls[i] + 5] == 0x84 && c->text[calls[i] + 6] == 0xc0)
			break;
	if (i == nscoped)
		return fail(err, errlen, "test-al: the call to os_is_vgx_hyper at .text 0x%zx is not followed "
					 "by `84 c0` (test al,al)", calls[0] + 5);
	for (i = 0; i < nscoped; i++)
		if (calls[i] + 9 <= c->tn && c->text[calls[i] + 5] == 0x84 && c->text[calls[i] + 6] == 0xc0 &&
		    (c->text[calls[i] + 7] == 0x75 ||
		     (c->text[calls[i] + 7] == 0x90 && c->text[calls[i] + 8] == 0x90)))
			break;
	if (i == nscoped)
		return fail(err, errlen, "branch: after `test al,al` expected `75 xx` (jne rel8) or its 2-byte "
					 "NOP form");

	/* The refusal block: R_X86_64_32S references to the rodata sentence. */
	gs_elf_rela_begin(&it, c->text_index);
	while ((rc = gs_elf_rela_next(c->e, &it, &rel, NULL, 0)) > 0) {
		if (rel.type != R_X86_64_32S || !reloc_string_is(c, &rel, GATE_STRING))
			continue;
		if (nrefs >= sizeof(refs) / sizeof(refs[0]))
			return fail(err, errlen, "reject-block: too many references to the rejection string");
		refs[nrefs++] = (size_t)rel.offset;
	}
	if (rc < 0)
		return fail(err, errlen, "relocations: malformed relocation table");

	for (i = 0; i < nscoped; i++) {
		int ok = 0;

		if (calls[i] + 9 > c->tn || c->text[calls[i] + 5] != 0x84 || c->text[calls[i] + 6] != 0xc0)
			continue;
		if (c->text[calls[i] + 7] == 0x75) {
			int64_t tgt = (int64_t)calls[i] + 9 + sign8(c->text[calls[i] + 8]);

			if (tgt <= (int64_t)(calls[i] + 9) || tgt > (int64_t)hi)
				continue;
			for (k = 0; k < nrefs; k++)
				if ((int64_t)refs[k] >= tgt && (int64_t)refs[k] <= tgt + 0x60)
					ok = 1;
		} else if (c->text[calls[i] + 8] == 0x90) {
			/* The branch is NOPed: the refusal block must still be the only
			 * reference to the sentence in the scope, and lie after the site. */
			size_t in_scope = 0, only = SIZE_MAX;

			for (k = 0; k < nrefs; k++)
				if (refs[k] >= lo && refs[k] < hi) {
					in_scope++;
					only = refs[k];
				}
			ok = in_scope == 1 && only > calls[i];
		}
		if (!ok)
			continue;
		if (nsites == 0) {
			site = calls[i];
			state = c->text[site + 7] == 0x75 ? 0 : 1;
		}
		nsites++;
	}
	if (nsites == 0)
		return fail(err, errlen, "reject-block: the branch at .text 0x%zx does not reach an "
					 "R_X86_64_32S reference to the rodata string '%s' within 0x60 bytes",
			    calls[0] + 7, GATE_STRING);
	if (nsites > 1)
		return fail(err, errlen, "unique: %zu gate candidates in %s (want 1)", nsites,
			    scoped ? scope.name : ".text");

	out->site = c->toff + site + 7;
	out->state = state;
	out->width = 2;
	out->divisor = 0;
	memcpy(out->old_bytes, c->text + site + 7, 2);
	memcpy(out->new_bytes, "\x90\x90", 2);
	if (containing(c, site, &owner) == 1) {
		out->function = owner.name;
		out->function_offset = site + 7 - owner.value;
	} else if (scoped) {
		out->function = scope.name;
		out->function_offset = site + 7 - scope.value;
	} else {
		out->function = NULL;
		out->function_offset = 0;
	}
	return 0;
}

static int locate_vf(const struct obj_ctx *c, struct plugin_site *out, char *err, size_t errlen)
{
	static const unsigned char hdr3[5] = { 0xbe, 0xd8, 0x0b, 0x00, 0x00 };
	static const unsigned char hdr5[5] = { 0xbe, 0xe0, 0x0b, 0x00, 0x00 };
	static const unsigned char bars[5][5] = {
		{ 0xbe, 0xf0, 0x0b, 0x00, 0x00 }, { 0xbe, 0xf4, 0x0b, 0x00, 0x00 }, { 0xbe, 0xf8, 0x0b, 0x00, 0x00 },
		{ 0xbe, 0xfc, 0x0b, 0x00, 0x00 }, { 0xbe, 0x00, 0x0c, 0x00, 0x00 },
	};
	static const unsigned char stock[4] = { 0x0f, 0xb7, 0x45, 0x06 };
	static const unsigned char patched[4] = { 0x31, 0xc0, 0xb0, 0x04 };
	struct gs_elf_sym owner;
	size_t p, stage_call = 0, stage_load = 0, stage_hdr5 = 0, stage_store = 0;
	size_t nsites = 0, site = 0;
	int state = 0;

	for (p = 0; p + 5 <= c->tn; p++) {
		size_t call, i, at = SIZE_MAX, p5, store, q, pos;
		int load_state = 0, bar_ok = 1;

		if (memcmp(c->text + p, hdr3, 5) != 0)
			continue;
		call = call_after(c, p + 5, 0x30);
		if (call == SIZE_MAX)
			continue;
		stage_call++;
		for (i = call + 5; i + 4 <= call + 5 + 0x20 && i + 4 <= c->tn; i++)
			if (memcmp(c->text + i, stock, 4) == 0 || memcmp(c->text + i, patched, 4) == 0) {
				at = i;
				load_state = memcmp(c->text + i, stock, 4) != 0;
				break;
			}
		if (at == SIZE_MAX)
			continue;
		stage_load++;
		p5 = find_in(c, at, at + 0x11, hdr5, 5);
		if (p5 == SIZE_MAX)
			continue;
		stage_hdr5++;
		store = SIZE_MAX;
		for (i = at + 4; i < at + 4 + 0x14 && i + 6 <= c->tn; i++)
			if (c->text[i] == 0x89 && (c->text[i + 1] & 0xc0) == 0x80 && (c->text[i + 1] & 0x38) == 0 &&
			    (c->text[i + 1] & 0x07) != 4) {
				store = i;
				break;
			}
		if (store == SIZE_MAX || store < at + 4 || store > p5 + 0x18)
			continue;
		stage_store++;
		pos = p5;
		for (i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
			q = find_in(c, pos, at + 0x181, bars[i], 5);
			if (q == SIZE_MAX || call_after(c, q + 5, 0x30) == SIZE_MAX) {
				bar_ok = 0;
				break;
			}
			pos = q;
		}
		if (!bar_ok)
			continue;
		if (nsites == 0) {
			site = at;
			state = load_state;
		}
		nsites++;
	}
	if (nsites == 0) {
		if (stage_call == 0)
			return fail(err, errlen, "hdr3-call: no call within 0x30 bytes after a `mov $0xbd8,%%esi` "
						 "HDR3 read");
		if (stage_load == 0)
			return fail(err, errlen, "totalvfs-load: no TotalVFs load `0f b7 45 06` (movzwl 0x6(%%rbp),"
						 "%%eax) or its 4-byte patched form between the HDR3 read and the "
						 "HDR5 0xbe0 read");
		if (stage_hdr5 == 0)
			return fail(err, errlen, "hdr5-read: no `mov $0xbe0,%%esi` HDR5 read within 0x10 bytes after "
						 "the TotalVFs load");
		if (stage_store == 0)
			return fail(err, errlen, "store: no 32-bit store of eax (`89 /0` with mod=10) between the "
						 "TotalVFs load and 0x18 bytes after the HDR5 read");
		return fail(err, errlen, "bar-window: the HDR9..HDR13 VF BAR read sequence was not found in order "
					 "within 0x180 bytes of the load");
	}
	if (nsites > 1)
		return fail(err, errlen, "unique: %zu VF-count candidates in .text (want 1)", nsites);

	out->site = c->toff + site;
	out->state = state;
	out->width = 4;
	out->divisor = 0;
	memcpy(out->old_bytes, c->text + site, 4);
	memcpy(out->new_bytes, "\x31\xc0\xb0\x04", 4);
	if (containing(c, site, &owner) == 1) {
		out->function = owner.name;
		out->function_offset = site - owner.value;
	} else {
		out->function = NULL;
		out->function_offset = 0;
	}
	return 0;
}

/* The code window of the ada-placement row: `head` bytes of unchanged code, then the row's changed
 * bytes (its `width` bytes), then `tail` bytes of unchanged code. The head and the tail make the
 * window unique. The window is searched in all of .text, in its stock form and in its patched form. */
static const struct {
	unsigned char head[24];
	size_t head_len;
	unsigned char tail[8];
	size_t tail_len;
} placement_window = {
	/* imul $0x134f58,0x8(%rbp),%rax; mov 0x0(%rbp),%rsi; movb $imm,0x134c3a(%rsi,%rax,1); jmp */
	{ 0x48, 0x69, 0x45, 0x08, 0x58, 0x4f, 0x13, 0x00, 0x48, 0x8b, 0x75, 0x00,
	  0xc6, 0x84, 0x06, 0x3a, 0x4c, 0x13, 0x00 }, 19,
	{ 0xe9, 0xd1, 0xf8, 0xff }, 4
};

/* 0 located, -1 refused, GS_ADA_NOTFOUND when the window is not in .text, GS_ADA_ABSENT when the
 * row cannot carry a window. The whole .text is searched, so the build's function names do not
 * matter; the containing function, when a symbol holds the match, is reported as information. */
static int locate_placement(const struct gs_ada_revision *r, const struct obj_ctx *c,
			    struct plugin_site *out, char *err, size_t errlen)
{
	struct gs_insn_scope scope = { GS_INSN_SCOPE_TEXT, NULL };
	struct gs_insn_match m[2];
	unsigned char pat[2][32], mask[32];
	size_t head = placement_window.head_len;
	size_t len = head + r->width + placement_window.tail_len, k;
	int rc[2];

	if (r->width == 0 || len > sizeof(pat[0]))
		return GS_ADA_ABSENT;

	memset(mask, 0xff, sizeof(mask));
	for (k = 0; k < 2; k++) {
		memcpy(pat[k], placement_window.head, head);
		memcpy(pat[k] + head, k == 0 ? r->old_bytes : r->new_bytes, r->width);
		memcpy(pat[k] + head + r->width, placement_window.tail, placement_window.tail_len);
		rc[k] = gs_insn_find_unique(c->e, pat[k], mask, len, &scope, &m[k], err, errlen);
		if (rc[k] == GS_INSN_ERR)
			return -1;
	}
	if (rc[0] == GS_INSN_MULTIPLE || rc[1] == GS_INSN_MULTIPLE || (rc[0] == GS_INSN_FOUND && rc[1] == GS_INSN_FOUND))
		return fail(err, errlen, "unique: %zu windows of %s in .text (want 1)", m[0].count + m[1].count,
			    r->name);
	if (rc[0] == GS_INSN_NONE && rc[1] == GS_INSN_NONE) {
		fail(err, errlen, "window: the code window of %s was not found in .text", r->name);
		return GS_ADA_NOTFOUND;
	}
	k = rc[0] == GS_INSN_FOUND ? 0 : 1;
	out->site = m[k].file_offset + head;
	out->state = (int)k;
	out->width = r->width;
	out->divisor = 0;
	memcpy(out->old_bytes, r->old_bytes, r->width);
	memcpy(out->new_bytes, r->new_bytes, r->width);
	out->function = m[k].func_name;
	out->function_offset = m[k].func_name ? m[k].match_offset + head : 0;
	return 0;
}

/* One row's structural locator, when one has been established for it. */
static int locate(const struct gs_ada_revision *r, const unsigned char *data, size_t size,
		  struct plugin_site *out, char *err, size_t errlen)
{
	struct gs_elf e;
	Elf64_Ehdr h;

	if (strcmp(r->name, "module-gate") == 0 || strcmp(r->name, "vf-count") == 0 ||
	    strcmp(r->name, "ada-placement") == 0) {
		struct obj_ctx ctx;

		/* Only a relocatable object can carry these sites. */
		if (size < sizeof(h))
			return GS_ADA_ABSENT;
		memcpy(&h, data, sizeof(h));
		if (memcmp(h.e_ident, ELFMAG, SELFMAG) != 0 || h.e_ident[EI_CLASS] != ELFCLASS64 ||
		    h.e_ident[EI_DATA] != ELFDATA2LSB || h.e_type != ET_REL)
			return GS_ADA_ABSENT;
		if (obj_open(data, size, &e, &ctx, err, errlen))
			return -1;
		if (strcmp(r->name, "module-gate") == 0)
			return locate_gate(&ctx, out, err, errlen);
		if (strcmp(r->name, "vf-count") == 0)
			return locate_vf(&ctx, out, err, errlen);
		return locate_placement(r, &ctx, out, err, errlen);
	}
	if (strcmp(r->name, "plugin-mixed") != 0 && strcmp(r->name, "plugin-div4") != 0)
		return GS_ADA_ABSENT;
	/* Only a shared object can carry these sites; another container is not this row's input. */
	if (size < sizeof(h))
		return GS_ADA_ABSENT;
	memcpy(&h, data, sizeof(h));
	if (memcmp(h.e_ident, ELFMAG, SELFMAG) != 0 || h.e_ident[EI_CLASS] != ELFCLASS64 ||
	    h.e_ident[EI_DATA] != ELFDATA2LSB || h.e_type != ET_DYN)
		return GS_ADA_ABSENT;
	if (gs_elf_open_shared(data, size, &e, err, errlen))
		return -1;
	return strcmp(r->name, "plugin-mixed") == 0 ? plugin_placement(&e, out, err, errlen)
						    : plugin_div(&e, out, err, errlen);
}

int gs_ada_admit_in(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, size_t size,
		    int mode, struct gs_ada_found *out, char *err, size_t errlen)
{
	struct gs_ada_found digest = { 0 };
	struct plugin_site site = { 0 }, other_site = { 0 };
	const struct gs_ada_revision *pick = NULL, *known = NULL, *located = NULL, *refused = NULL, *other = NULL;
	const struct gs_ada_revision *missing = NULL;
	char missing_reason[256] = { 0 };
	const struct gs_ada_revision *bad_div = NULL;
	unsigned bad_divisor = 0;
	char detail[256] = { 0 }, reason[256] = { 0 }, first_reason[256] = { 0 }, hash[65];
	size_t i;
	int st = 0, rc;

	if (!out || (!data && size) || (!rows && count))
		return fail(err, errlen, "missing data, table or result pointers");
	if (mode != GS_ADA_ANY && mode != GS_ADA_FORWARD && mode != GS_ADA_BACKWARD)
		return fail(err, errlen, "unknown lookup direction");

	/* Digest path first: exactly the rules of gs_ada_find_in. */
	if (!gs_ada_find_in(rows, count, data, size, mode, &pick, &st, detail, sizeof(detail))) {
		digest.row = pick;
		digest.state = st;
		digest.admission = GS_ADA_DIGEST;
		digest.site = pick->site;
		digest.width = pick->width;
		digest.divisor = 0;
		digest.function = pick->function;
		digest.function_offset = pick->function_offset;
		memcpy(digest.old_bytes, pick->old_bytes, sizeof(digest.old_bytes));
		memcpy(digest.new_bytes, pick->new_bytes, sizeof(digest.new_bytes));
		*out = digest;
		return 0;
	}
	/* A digest that names a row is an integrity failure: never retry structurally. */
	gs_sha256(data, size, SIZE_MAX, hash);
	for (i = 0; i < count; i++)
		if (!strcmp(hash, rows[i].input_sha) || !strcmp(hash, rows[i].output_sha)) {
			known = &rows[i];
			break;
		}
	if (known)
		return fail(err, errlen, "anchor ada.%s: %s", known->name, detail);

	/* Structural path: every row's locator must be complete, and the first row in
	 * table order that is in the requested state is the one applied (the table is
	 * an ordered chain). */
	for (i = 0; i < count; i++) {
		struct plugin_site s = { 0 };

		rc = locate(&rows[i], data, size, &s, reason, sizeof(reason));
		if (rc == GS_ADA_ABSENT)
			continue;
		if (rc == GS_ADA_NOTFOUND) {
			if (!missing) {
				missing = &rows[i];
				snprintf(missing_reason, sizeof(missing_reason), "%s", reason);
			}
			continue;
		}
		if (rc) {
			if (!refused) {
				refused = &rows[i];
				snprintf(first_reason, sizeof(first_reason), "%s", reason);
			}
			continue;
		}
		if (rc == 0 && mode == GS_ADA_FORWARD && s.divisor && s.divisor != 4 && !bad_div) {
			bad_div = &rows[i];
			bad_divisor = s.divisor;
		}
		if ((mode == GS_ADA_FORWARD && s.state == 0) || (mode == GS_ADA_BACKWARD && s.state == 1) ||
		    mode == GS_ADA_ANY) {
			/* ANY prefers an input-state row, as the digest path does; among rows
			 * in the same state the first input-state row and the last output-state
			 * row are reported, which is the chain position. */
			if (!located || s.state < site.state || (s.state == 1 && site.state == 1)) {
				located = &rows[i];
				site = s;
			}
		} else if (!other) {
			other = &rows[i];
			other_site = s;
		}
	}
	if (refused)
		return fail(err, errlen, "anchor ada.%s: %s", refused->name, first_reason);
	if (bad_div)
		return fail(err, errlen, "anchor ada.%s: installed divisor %u is not the recorded 4", bad_div->name,
			    bad_divisor);
	/* A window that is missing refuses a forward step when no row is located or when the located
	 * row comes after it in the chain: the chain cannot be followed past a missing step. */
	if (missing && mode == GS_ADA_FORWARD && (!located || missing < located))
		return fail(err, errlen, "anchor ada.%s: %s", missing->name, missing_reason);
	if (located) {
		if (mode == GS_ADA_BACKWARD)
			return fail(err, errlen, "anchor ada.%s: structural revert cannot restore the replaced "
						 "bytes; revert requires the recorded digest", located->name);
		out->row = located;
		out->state = site.state;
		out->admission = GS_ADA_ANCHOR;
		out->site = site.site;
		out->width = site.width;
		out->divisor = site.divisor;
		out->function = site.function;
		out->function_offset = site.function_offset;
		memcpy(out->old_bytes, site.old_bytes, sizeof(out->old_bytes));
		memcpy(out->new_bytes, site.new_bytes, sizeof(out->new_bytes));
		return 0;
	}
	if (other) {
		if (other_site.divisor && other_site.divisor != 4)
			return fail(err, errlen, "anchor ada.%s: already at output state with divisor %u (the "
						 "recorded byte patch installs 4)", other->name, other_site.divisor);
		return fail(err, errlen, "anchor ada.%s: %s", other->name,
			    mode == GS_ADA_FORWARD ? "already at output state" : "not at output state");
	}
	if (missing)
		return fail(err, errlen, "anchor ada.%s: %s", missing->name, missing_reason);
	return fail(err, errlen, "anchor ada.unknown: no recipe matches this input and no structural locator "
				 "is implemented for it");
}

int gs_ada_admit(const unsigned char *data, size_t size, int mode, struct gs_ada_found *out, char *err,
		 size_t errlen)
{
	return gs_ada_admit_in(revisions, REVISION_COUNT, data, size, mode, out, err, errlen);
}

int gs_ada_edit(struct gs_ada_found *f, unsigned char *data, size_t size, int revert, char *err, size_t errlen)
{
	unsigned char prev[16];
	char hash[65];

	if (!f || (!data && size))
		return fail(err, errlen, "missing edit or data");
	if (f->width == 0 || f->width > sizeof(f->old_bytes))
		return fail(err, errlen, "patch width is outside 1 to 16 bytes");
	if (f->site > size || size - f->site < f->width)
		return fail(err, errlen, "file is smaller than the patch site");
	if (revert ? f->state != 1 : f->state != 0)
		return fail(err, errlen, revert ? "revert requires the patched output; this file is not patched"
						: "apply requires the unpatched input; this file is already patched");
	if (revert && f->admission == GS_ADA_ANCHOR)
		return fail(err, errlen, "structural revert cannot restore the replaced bytes; revert requires the "
					 "recorded digest");
	memcpy(prev, data + f->site, f->width);
	memcpy(data + f->site, revert ? f->old_bytes : f->new_bytes, f->width);
	if (f->admission == GS_ADA_DIGEST) {
		gs_sha256(data, size, SIZE_MAX, hash);
		if (strcmp(hash, revert ? f->row->input_sha : f->row->output_sha)) {
			memcpy(data + f->site, prev, f->width);
			return fail(err, errlen, "resulting SHA-256 does not match the recorded digest; buffer restored");
		}
	}
	f->state = revert ? 0 : 1;
	return 0;
}
