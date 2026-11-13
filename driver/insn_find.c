// SPDX-License-Identifier: MIT
/* Masked byte-pattern search over function bodies; see insn_find.h. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "insn_find.h"

static int fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err && errlen) {
		va_start(ap, fmt);
		vsnprintf(err, errlen, fmt, ap);
		va_end(ap);
	}
	return GS_INSN_ERR;
}

struct search {
	const unsigned char *pattern, *mask;
	size_t len;
	size_t count;
	struct gs_insn_match first;
};

/* Scans one function body: counts every match and keeps the first. */
static void scan_function(struct search *s, const struct gs_elf_sym *sym, const struct gs_elf_range *r)
{
	size_t off, k;

	if (r->size < s->len)
		return;
	for (off = 0; off + s->len <= r->size; off++) {
		for (k = 0; k < s->len; k++)
			if ((r->bytes[off + k] ^ s->pattern[k]) & s->mask[k])
				break;
		if (k < s->len)
			continue;
		if (s->count++ == 0) {
			s->first.func_name = sym->name;
			s->first.func_start = r->section_offset;
			s->first.match_offset = off;
			s->first.file_offset = r->file_offset + off;
		}
	}
}

static int finish(const struct search *s, struct gs_insn_match *out)
{
	if (s->count == 0) {
		out->count = 0;
		return GS_INSN_NONE;
	}
	if (s->count > 1) {
		out->count = s->count;
		return GS_INSN_MULTIPLE;
	}
	*out = s->first;
	out->count = 1;
	return GS_INSN_FOUND;
}

/* Scans all of .text; the containing function of the one match is looked up afterwards. */
static int scan_text(const struct gs_elf *e, struct search *s, char *err, size_t errlen)
{
	struct gs_elf_sym sym, best;
	Elf64_Shdr sh;
	const unsigned char *bytes;
	size_t text, n, off, k, i, first = 0;
	char msg[256];
	int found = 0;

	if (gs_elf_section_by_name(e, ".text", &text, &sh, msg, sizeof(msg)) ||
	    gs_elf_section_data(e, text, &bytes, &n, msg, sizeof(msg)))
		return fail(err, errlen, ".text: %s", msg);
	for (off = 0; n >= s->len && off <= n - s->len; off++) {
		for (k = 0; k < s->len; k++)
			if ((bytes[off + k] ^ s->pattern[k]) & s->mask[k])
				break;
		if (k < s->len)
			continue;
		if (s->count++ == 0)
			first = off;
	}
	if (s->count != 1)
		return GS_INSN_FOUND;
	s->first.file_offset = (size_t)sh.sh_offset + first;
	s->first.match_offset = first;
	/* Innermost sized function symbol of .text that contains the match, if the object has symbols. */
	for (i = 1; e->symtab && i < e->nsyms; i++) {
		if (gs_elf_symbol(e, i, &sym, msg, sizeof(msg)))
			return fail(err, errlen, "%s", msg);
		if (sym.type != STT_FUNC || sym.shndx != text || sym.size == 0)
			continue;
		if (sym.value <= first && first < sym.value + sym.size && (!found || sym.value > best.value)) {
			best = sym;
			found = 1;
		}
	}
	if (found) {
		s->first.func_name = best.name;
		s->first.func_start = best.value;
		s->first.match_offset = first - best.value;
	}
	return GS_INSN_FOUND;
}

int gs_insn_find_unique(const struct gs_elf *e, const unsigned char *pattern, const unsigned char *mask,
			size_t len, const struct gs_insn_scope *scope, struct gs_insn_match *out,
			char *err, size_t errlen)
{
	struct search s = { .pattern = pattern, .mask = mask, .len = len };
	struct gs_elf_sym sym;
	struct gs_elf_range range;
	char msg[256];

	if (!e || !pattern || !mask || !scope || !out || (scope->kind == GS_INSN_SCOPE_SYMBOL && !scope->symbol))
		return fail(err, errlen, "missing argument");
	memset(out, 0, sizeof(*out));
	if (len == 0)
		return fail(err, errlen, "empty pattern");

	switch (scope->kind) {
	case GS_INSN_SCOPE_SYMBOL:
		if (gs_elf_symbol_by_name(e, scope->symbol, &sym, msg, sizeof(msg)))
			return fail(err, errlen, "scope symbol %s: %s", scope->symbol, msg);
		if (gs_elf_symbol_range(e, &sym, &range, msg, sizeof(msg)))
			return fail(err, errlen, "%s", msg);
		scan_function(&s, &sym, &range);
		break;
	case GS_INSN_SCOPE_TEXT:
		if (scan_text(e, &s, err, errlen) != GS_INSN_FOUND)
			return GS_INSN_ERR;
		break;
	default:
		return fail(err, errlen, "unknown scope kind %d", (int)scope->kind);
	}
	return finish(&s, out);
}
