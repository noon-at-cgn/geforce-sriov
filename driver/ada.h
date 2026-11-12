/* SPDX-License-Identifier: MIT */
#ifndef GS_ADA_H
#define GS_ADA_H
#include <stddef.h>
/* One measured revision of 1 to 16 bytes in a user-supplied file, admitted only by
 * the exact SHA-256 of the whole file before (input_sha) or after (output_sha). */
struct gs_ada_revision {
	const char *name;          /* short stable identifier, e.g. "module-gate" */
	const char *artifact;      /* what kind of file this row applies to */
	const char *version;       /* version string of the measured build */
	const char *input_sha;     /* 64 lowercase hex, expected digest of the input file */
	const char *output_sha;    /* 64 lowercase hex, expected digest after the revision */
	size_t site;               /* byte offset of the first changed byte */
	unsigned char width;       /* number of bytes replaced, 1 to 16 */
	unsigned char old_bytes[16]; /* read only up to width */
	unsigned char new_bytes[16]; /* read only up to width */
	int measured;              /* exact revision and input/output were tested */
	const char *function;      /* containing symbol for site, NULL when the row has none */
	size_t function_offset;    /* site relative to that symbol */
};

size_t gs_ada_revision_count(void);
/* Returns NULL when index is out of range. */
const struct gs_ada_revision *gs_ada_revision_at(size_t index);

/* Returns 0 when data matches input_sha and the width bytes at site equal old_bytes;
 * 1 when data matches output_sha and the width bytes at site equal new_bytes;
 * -1 (refused, message in err) for anything else, including a width outside 1 to 16
 * or a size smaller than site+width. */
int gs_ada_state(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen);

/* Requires state 0. Writes new_bytes at site, then re-verifies output_sha over the whole buffer. */
int gs_ada_apply(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen);

/* Requires state 1. Writes old_bytes at site, then re-verifies input_sha over the whole buffer. */
int gs_ada_revert(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen);

/* Lookup direction for gs_ada_find. Consecutive rows may chain: one row's output_sha can be
 * the next row's input_sha, so the direction decides which row a digest selects. */
enum { GS_ADA_ANY = 0, GS_ADA_FORWARD = 1, GS_ADA_BACKWARD = 2 };

/* Selects the table row whose digest equals that of data, by mode:
 * GS_ADA_FORWARD the row whose input_sha matches (*state 0), to be applied;
 * GS_ADA_BACKWARD the row whose output_sha matches (*state 1), to be reverted;
 * GS_ADA_ANY an input_sha match if one exists, else an output_sha match.
 * Then requires the width bytes at site to equal old_bytes (state 0) or new_bytes (state 1).
 * Refuses when no row matches in that direction, when more than one does, when the file is
 * smaller than site+width, or when the digest matches but the site bytes do not. */
int gs_ada_find(const unsigned char *data, size_t size, int mode,
		const struct gs_ada_revision **out, int *state, char *err, size_t errlen);

/* Applies exactly the rules of gs_ada_find, over a caller-supplied table of count rows
 * instead of the shipped one, so the lookup can be exercised against constructed rows.
 * Also refuses rows == NULL with count > 0. gs_ada_find is this over the shipped table. */
int gs_ada_find_in(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, size_t size,
		   int mode, const struct gs_ada_revision **out, int *state, char *err, size_t errlen);

/* One admitted edit: the table row plus the located site and byte pair. */
struct gs_ada_found {
	const struct gs_ada_revision *row;
	int state;         /* 0 = row input state, 1 = row output state */
	int admission;     /* GS_ADA_DIGEST or GS_ADA_ANCHOR */
	size_t site;       /* file offset of the first changed byte */
	unsigned width;    /* bytes written at site, 1 to 16 */
	unsigned divisor;  /* plugin-div4: divisor installed by this edit */
	unsigned char old_bytes[16]; /* read only up to width */
	unsigned char new_bytes[16]; /* read only up to width */
	const char *function;        /* containing symbol, NULL when unknown */
	size_t function_offset;      /* site relative to that symbol */
};
enum { GS_ADA_DIGEST = 0, GS_ADA_ANCHOR = 1 };

/* Admits one edit for this file. The digest path is gs_ada_find_in unchanged; when no
 * digest matches, each row's structural locator is tried and the first applicable row in
 * table order is used (the table is an ordered chain). A structural admission is always
 * unconfirmed. Refusals name the anchor: "anchor ada.<revision>: <what was not found>". */
int gs_ada_admit(const unsigned char *, size_t, int, struct gs_ada_found *, char *, size_t);
/* Same rules over a caller-supplied table, so refusals can be exercised synthetically. */
int gs_ada_admit_in(const struct gs_ada_revision *, size_t, const unsigned char *, size_t, int,
		    struct gs_ada_found *, char *, size_t);

/* Applies (revert 0) or reverts (revert 1) an admitted edit in place and updates the
 * admitted state. A digest admission re-verifies the recorded digest afterwards and
 * restores the bytes on mismatch. A structural revert is refused: the replaced stock
 * bytes are not recoverable from the patched file. */
int gs_ada_edit(struct gs_ada_found *, unsigned char *, size_t, int, char *, size_t);
#endif
