// SPDX-License-Identifier: MIT
/*
 * Find one masked byte pattern in the functions of a relocatable object.
 *
 * The search covers the body of every function in scope. A byte b of the code
 * matches pattern byte p when (b & mask) == (p & mask). Matches may overlap;
 * each start offset counts once. The caller requires exactly one match: no
 * match and more than one match have their own return values.
 */
#ifndef GS_INSN_FIND_H
#define GS_INSN_FIND_H

#include <stddef.h>
#include <stdint.h>

#include "elf_reader.h"

enum {
	GS_INSN_ERR = -1,     /* malformed object, bad arguments or unknown scope symbol */
	GS_INSN_FOUND = 0,    /* exactly one match; *out is filled */
	GS_INSN_NONE = 1,     /* no match in scope; out->count is 0 */
	GS_INSN_MULTIPLE = 2, /* more than one match; out->count is the number, other fields unset */
};

enum gs_insn_scope_kind {
	/* The body of the defined symbol named by `symbol`. */
	GS_INSN_SCOPE_SYMBOL,
	/* The whole .text section; `symbol` is not used. A containing function is reported only as
	 * information: func_name is NULL and match_offset is the offset in .text when none contains
	 * the match. */
	GS_INSN_SCOPE_TEXT,
};

struct gs_insn_scope {
	enum gs_insn_scope_kind kind;
	const char *symbol;
};

struct gs_insn_match {
	const char *func_name;   /* containing function's symbol name, inside the ELF buffer */
	uint64_t func_start;     /* its offset within its section */
	size_t match_offset;     /* offset of the match from func_start */
	size_t file_offset;      /* offset of the match in the file */
	size_t count;            /* number of matches found in scope */
};

/*
 * pattern and mask are len bytes each (len > 0). Returns a GS_INSN_* value;
 * for GS_INSN_ERR a message is written to err when err is non-NULL and
 * errlen non-zero. *out is zeroed first; count is set for NONE, FOUND and
 * MULTIPLE.
 */
int gs_insn_find_unique(const struct gs_elf *e, const unsigned char *pattern, const unsigned char *mask,
			size_t len, const struct gs_insn_scope *scope, struct gs_insn_match *out,
			char *err, size_t errlen);

#endif
