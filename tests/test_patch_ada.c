/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ada.h"
#include "elf_reader.h"
#include "patch.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
/* The shipped rows are admitted only by digests of measured vendor files, which
 * cannot be reproduced synthetically. Block 1 checks the invariants of the
 * shipped table (shape, width, lowercase-hex digests, no digest repeated on the
 * same side of two rows, exactly two rows chained from another row's output,
 * every row's output digest pinned to its input digest, and the five measured
 * rows with their sites, widths of 1 to 16 bytes and byte pairs). Block 2 builds
 * its own two-byte row, a four-byte row chained after it and a fifteen-byte row
 * over a 64-byte buffer and drives
 * through them every accepting and refusing path of gs_ada_state, gs_ada_apply
 * and gs_ada_revert, and every mode of gs_ada_find_in, plus the argument and
 * unknown-digest refusals of gs_ada_find over the shipped table. Block 3 takes the
 * shipped plugin-mixed and plugin-div4 rows (sites, widths, bytes) with the digests
 * of a synthetic file that carries their original bytes at their file offsets, and
 * runs the chain stock -> mixed -> div4 and back, with its refusals.
 * Not covered here: gs_ada_find accepting a shipped row, and applying a shipped
 * row to its real file, both of which need the measured vendor files; placement_locators()
 * reads the vendor object only to check the ada-placement locator, when it is present. Block 4 checks the admission wrapper:
 * a digest match reports admission 0 and the row, a matching digest with other
 * site bytes names the revision, and an unknown digest refuses with the exact
 * anchor message. Block 5 runs the built patcher binary when it exists and
 * checks the usage exit 2, the refusal exit 3 with its anchor message and no
 * output file, and the I/O exit 1. Block 6 builds synthetic ET_DYN plugins that
 * carry the documented instruction shapes and drives both structural locators:
 * the measured shape must reproduce the recorded row bytes, shifted offsets and
 * virtual addresses must still be found, and every missing, duplicated or
 * unmeasured anchor must refuse with its row name. The real vendor digests stay
 * pinned in the shipped table (Block 1); a synthetic file cannot carry them. */
static int failures, checks;
/* Always evaluated, unlike assert(), so the checks also run under -DNDEBUG. */
#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)
enum { N = 64, SITE = 32, SITE4 = 40, SITE15 = 44 };
static const struct gs_ada_revision sentinel;
static char err[256];

/* Exactly 64 lowercase hex characters; stops at the first non-hex byte, so it
 * never reads past the terminator of a shorter string. */
static int hex64(const char *s)
{
	size_t i;

	if (!s)
		return 0;
	for (i = 0; i < 64; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return 0;
	return s[64] == '\0';
}

/* Clears the message buffer before a call so a stale message cannot pass. */
static char *fresh(void)
{
	err[0] = '\0';
	return err;
}

/* Refused, and the message names the expected reason. */
static int refused(int rc, const char *why)
{
	return rc < 0 && strstr(err, why) != NULL;
}

static int same(const unsigned char *a, const unsigned char *b)
{
	return memcmp(a, b, N) == 0;
}

/* gs_ada_find_in over rows selects rows[want] in state `state`. */
static int finds(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, int mode, size_t want, int state)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return gs_ada_find_in(rows, count, data, N, mode, &out, &st, fresh(), sizeof(err)) == 0 && out == &rows[want] && st == state;
}

/* gs_ada_find_in over rows refuses for the expected reason and writes no result. */
static int misses(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, int mode, const char *why)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return refused(gs_ada_find_in(rows, count, data, N, mode, &out, &st, fresh(), sizeof(err)), why) && out == &sentinel && st == 42;
}

static void table_invariants(void)
{
	static const struct {
		const char *input_sha, *output_sha;
		size_t site;
		unsigned char width, old_bytes[16], new_bytes[16];
		int measured;
	} measured[] = {
		{"4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449", "ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b", 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}, 0},
		{"ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701", "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12", 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}, 1},
		{"f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12", "946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29", 0x126644, 4, {0x0f, 0xb7, 0x45, 0x06}, {0x31, 0xc0, 0xb0, 0x04}, 1},
		{"7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7", "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6", 0x8fc17, 15,
		 {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
		 {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00}, 0},
		{"7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6", "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c", 0xa5f28, 6,
		 {0x8b, 0xbb, 0xd0, 0x29, 0x00, 0x00}, {0xbf, 0x04, 0x00, 0x00, 0x00, 0x90}, 1},
		{"946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29", "20eaebcac4f5220d21f64bcd5145143abe8f806a25cb9e249088a1b98df7d157", 0xd3a86, 1,
		 {0x01}, {0x00}, 1},
	};
	size_t count = gs_ada_revision_count(), nmeasured = sizeof(measured) / sizeof(measured[0]), i, j, dupes = 0, links = 0, confirmed = 0;

	CHECK(count == nmeasured, "expected %zu revisions, got %zu", nmeasured, count);
	CHECK(gs_ada_revision_at(count) == NULL, "index equal to the count must be NULL");
	CHECK(gs_ada_revision_at(SIZE_MAX) == NULL, "index SIZE_MAX must be NULL");

	for (i = 0; i < count; i++) {
		const struct gs_ada_revision *x = gs_ada_revision_at(i);

		CHECK(x != NULL, "row %zu below the count is NULL", i);
		if (!x)
			continue;
		CHECK(x->name && x->artifact && x->version && x->input_sha && x->output_sha, "row %zu has a NULL field", i);
		CHECK(hex64(x->input_sha), "row %zu input_sha is not 64 lowercase hex characters", i);
		CHECK(hex64(x->output_sha), "row %zu output_sha is not 64 lowercase hex characters", i);
		if (x->input_sha && x->output_sha)
			CHECK(strcmp(x->input_sha, x->output_sha) != 0, "row %zu input_sha equals its output_sha", i);
		CHECK(x->width >= 1 && x->width <= sizeof(x->old_bytes), "row %zu width %u is outside 1 to 16", i, (unsigned)x->width);
		CHECK(x->width >= 1 && x->width <= sizeof(x->old_bytes) && memcmp(x->old_bytes, x->new_bytes, x->width) != 0,
		      "row %zu old_bytes equal its new_bytes over its width", i);
		CHECK(x->site != 0, "row %zu site is 0", i);
		CHECK(x->measured == 0 || x->measured == 1, "row %zu measured flag is %d, not 0 or 1", i, x->measured);
		confirmed += (size_t)(x->measured != 0);
		/* Every row must be listed in measured[], so a new unlisted row or a
		 * wrong input/output pairing fails rather than being skipped. */
		for (j = 0; j < nmeasured; j++)
			if (x->input_sha && !strcmp(x->input_sha, measured[j].input_sha))
				break;
		CHECK(j < nmeasured && x->output_sha && !strcmp(x->output_sha, measured[j].output_sha),
		      "row %zu %s", i, j < nmeasured ? "output_sha is not the recorded output for its input digest" : "input_sha has no recorded output digest");
	}

	/* Same-row pairs are covered above; here every pair from different rows.
	 * No two rows share an input or an output, so each direction selects at
	 * most one row. An output may equal another row's input only where the
	 * measured stack chains revisions: module-gate's stack output into vf-count,
	 * plugin-mixed's output into plugin-div4, and the plain chain's output into
	 * ada-placement. */
	for (i = 0; i < count; i++)
		for (j = 0; j < count; j++) {
			const struct gs_ada_revision *a = gs_ada_revision_at(i), *b = gs_ada_revision_at(j);

			if (i == j || !a || !b || !a->input_sha || !a->output_sha || !b->input_sha || !b->output_sha)
				continue;
			if (j > i && !strcmp(a->input_sha, b->input_sha))
				dupes++;
			if (j > i && !strcmp(a->output_sha, b->output_sha))
				dupes++;
			if (!strcmp(a->output_sha, b->input_sha))
				links++;
		}
	CHECK(dupes == 0, "%zu digest pairs repeat on the same side across rows", dupes);
	CHECK(links == 3, "%zu rows take another row's output as input, expected exactly 3", links);
	/* The stock gate-only row and plugin-mixed are intermediate states, not the tested stack; the
	 * three rows that reproduce a tested file and the placement row (measured on hardware,
	 * B9, 2026-11-13) are confirmed. */
	CHECK(confirmed == 4, "%zu rows are marked measured, expected exactly 4", confirmed);

	for (i = 0; i < nmeasured; i++) {
		const struct gs_ada_revision *pick = NULL;
		size_t hits = 0;

		for (j = 0; j < count; j++) {
			const struct gs_ada_revision *x = gs_ada_revision_at(j);

			if (x && x->input_sha && !strcmp(x->input_sha, measured[i].input_sha)) {
				pick = x;
				hits++;
			}
		}
		CHECK(hits == 1, "measured row %.12s present %zu times, expected once", measured[i].input_sha, hits);
		if (hits != 1)
			continue;
		CHECK(pick->site == measured[i].site, "measured row %.12s site is not 0x%zx", measured[i].input_sha, measured[i].site);
		CHECK(pick->measured == measured[i].measured, "measured row %.12s measured flag is not %d", measured[i].input_sha, measured[i].measured);
		CHECK(pick->width == measured[i].width && !memcmp(pick->old_bytes, measured[i].old_bytes, measured[i].width) &&
		      !memcmp(pick->new_bytes, measured[i].new_bytes, measured[i].width),
		      "measured row %.12s is not width %u with the recorded old and new bytes over that width",
		      measured[i].input_sha, (unsigned)measured[i].width);
	}
}

static void synthetic_row(void)
{
	static const unsigned char old4[4] = {0x0f, 0xb7, 0x45, 0x06}, new4[4] = {0x31, 0xc0, 0xb0, 0x04};
	static const unsigned char old15[15] = {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
				   new15[15] = {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00};
	static const int modes[] = {GS_ADA_ANY, GS_ADA_FORWARD, GS_ADA_BACKWARD};
	unsigned char orig[N], rev[N], chained[N], fifteen[N], buf[N];
	char in_sha[65], out_sha[65], next_sha[65], fif_sha[65], bad_in[65], bad_out[65];
	const struct gs_ada_revision *out = &sentinel;
	struct gs_ada_revision mine, next, long15, bad, skew, end, far, edge, wide[2], pair[2];
	size_t i;
	int st = 42;

	for (i = 0; i < N; i++)
		orig[i] = (unsigned char)(i * 37u + 0x5au);
	orig[SITE] = 0x11;
	orig[SITE + 1] = 0x22;
	memcpy(orig + SITE4, old4, sizeof(old4));
	memcpy(orig + SITE15, old15, sizeof(old15));
	memcpy(rev, orig, N);
	rev[SITE] = 0x33;
	rev[SITE + 1] = 0x44;
	memcpy(chained, rev, N);
	memcpy(chained + SITE4, new4, sizeof(new4));
	memcpy(fifteen, orig, N);
	memcpy(fifteen + SITE15, new15, sizeof(new15));
	gs_sha256(orig, N, SIZE_MAX, in_sha);
	gs_sha256(rev, N, SIZE_MAX, out_sha);
	gs_sha256(chained, N, SIZE_MAX, next_sha);
	gs_sha256(fifteen, N, SIZE_MAX, fif_sha);
	mine = (struct gs_ada_revision){
		.name = "synthetic", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = in_sha, .output_sha = out_sha, .site = SITE, .width = 2,
		.old_bytes = {0x11, 0x22}, .new_bytes = {0x33, 0x44},
	};
	/* Chained after mine: its input digest is mine's output digest. */
	next = (struct gs_ada_revision){
		.name = "synthetic-4", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = out_sha, .output_sha = next_sha, .site = SITE4, .width = 4,
		.old_bytes = {0x0f, 0xb7, 0x45, 0x06}, .new_bytes = {0x31, 0xc0, 0xb0, 0x04},
	};
	/* Fifteen bytes over the original, the width of the measured plugin row. */
	long15 = (struct gs_ada_revision){
		.name = "synthetic-15", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = in_sha, .output_sha = fif_sha, .site = SITE15, .width = 15,
		.old_bytes = {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
		.new_bytes = {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00},
	};

	/* 1. Classification of the two measured states. */
	CHECK(gs_ada_state(&mine, orig, N, fresh(), sizeof(err)) == 0, "original buffer is not state 0: %s", err);
	CHECK(gs_ada_state(&mine, rev, N, fresh(), sizeof(err)) == 1, "revised buffer is not state 1: %s", err);
	memcpy(buf, orig, N);
	buf[0] ^= 0x01;
	CHECK(refused(gs_ada_state(&mine, buf, N, fresh(), sizeof(err)), "neither"), "state accepted a buffer changed away from the site: %s", err);

	/* 2. Apply on the original. */
	memcpy(buf, orig, N);
	CHECK(gs_ada_apply(&mine, buf, N, fresh(), sizeof(err)) == 0, "apply on the original failed: %s", err);
	CHECK(buf[SITE] == 0x33 && buf[SITE + 1] == 0x44, "apply did not write 33 44 at the site");
	CHECK(gs_ada_state(&mine, buf, N, fresh(), sizeof(err)) == 1, "applied buffer is not state 1: %s", err);

	/* 3. Revert completes the round trip. */
	CHECK(gs_ada_revert(&mine, buf, N, fresh(), sizeof(err)) == 0, "revert of the applied buffer failed: %s", err);
	CHECK(same(buf, orig), "apply then revert is not byte-identical to the original");

	/* 4. Apply on an already revised buffer. */
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_apply(&mine, buf, N, fresh(), sizeof(err)), "already patched"), "apply on a revised buffer not refused as already revised: %s", err);
	CHECK(same(buf, rev), "refused apply changed the revised buffer");

	/* 5. Revert on an unrevised buffer. */
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_revert(&mine, buf, N, fresh(), sizeof(err)), "not patched"), "revert on an unrevised buffer not refused as not revised: %s", err);
	CHECK(same(buf, orig), "refused revert changed the original buffer");

	/* 6. Result digest mismatch restores the two bytes, in both directions. */
	memcpy(bad_out, out_sha, sizeof(bad_out));
	bad_out[0] = bad_out[0] == '0' ? '1' : '0';
	bad = mine;
	bad.output_sha = bad_out;
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&bad, buf, N, fresh(), sizeof(err)), "restored"), "apply with a wrong output digest not refused with restore: %s", err);
	CHECK(same(buf, orig), "apply with a wrong output digest did not restore the original bytes");
	memcpy(bad_in, in_sha, sizeof(bad_in));
	bad_in[0] = bad_in[0] == '0' ? '1' : '0';
	bad = mine;
	bad.input_sha = bad_in;
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_revert(&bad, buf, N, fresh(), sizeof(err)), "restored"), "revert with a wrong input digest not refused with restore: %s", err);
	CHECK(same(buf, rev), "revert with a wrong input digest did not restore the revised bytes");

	/* 7. Digest matches but the bytes at the site do not. */
	skew = mine;
	skew.old_bytes[1] = 0x23;
	CHECK(refused(gs_ada_state(&skew, orig, N, fresh(), sizeof(err)), "recorded original bytes"), "state accepted an input digest with other site bytes: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&skew, buf, N, fresh(), sizeof(err)), "recorded original bytes"), "apply accepted an input digest with other site bytes: %s", err);
	CHECK(same(buf, orig), "apply refused for site bytes changed the buffer");
	CHECK(misses(&skew, 1, orig, GS_ADA_FORWARD, "recorded original bytes"), "find_in accepted an input digest with other site bytes: %s", err);
	skew = mine;
	skew.new_bytes[1] = 0x45;
	CHECK(refused(gs_ada_state(&skew, rev, N, fresh(), sizeof(err)), "recorded patched bytes"), "state accepted an output digest with other site bytes: %s", err);

	/* 8. Size bound, with digests that match the buffer so only size can refuse:
	 * a width-2 row at site n-1 and a width-4 row at site n-3 are each one byte short. */
	end = mine;
	end.site = N - 1;
	end.old_bytes[0] = orig[N - 1];
	end.old_bytes[1] = 0;
	CHECK(refused(gs_ada_state(&end, orig, N, fresh(), sizeof(err)), "smaller"), "state at site n-1 not refused for size: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&end, buf, N, fresh(), sizeof(err)), "smaller"), "apply at site n-1 not refused for size: %s", err);
	CHECK(same(buf, orig), "apply refused for size changed the buffer");
	CHECK(refused(gs_ada_revert(&end, buf, N, fresh(), sizeof(err)), "smaller"), "revert at site n-1 not refused for size: %s", err);
	CHECK(same(buf, orig), "revert refused for size changed the buffer");
	CHECK(misses(&end, 1, orig, GS_ADA_FORWARD, "smaller"), "find_in at site n-1 not refused for size: %s", err);
	end = mine;
	end.site = N - 3;
	end.width = 4;
	memcpy(end.old_bytes, orig + N - 3, 3);
	end.old_bytes[3] = 0;
	CHECK(refused(gs_ada_state(&end, orig, N, fresh(), sizeof(err)), "smaller"), "width-4 state at site n-3 not refused for size: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&end, buf, N, fresh(), sizeof(err)), "smaller"), "width-4 apply at site n-3 not refused for size: %s", err);
	CHECK(same(buf, orig), "width-4 apply refused for size changed the buffer");
	CHECK(refused(gs_ada_revert(&end, buf, N, fresh(), sizeof(err)), "smaller"), "width-4 revert at site n-3 not refused for size: %s", err);
	CHECK(same(buf, orig), "width-4 revert refused for size changed the buffer");
	far = mine;
	far.site = SIZE_MAX;
	CHECK(refused(gs_ada_state(&far, orig, N, fresh(), sizeof(err)), "smaller"), "state at site SIZE_MAX not refused for size: %s", err);
	edge = mine;
	edge.site = N - 2;
	edge.old_bytes[0] = orig[N - 2];
	edge.old_bytes[1] = orig[N - 1];
	CHECK(gs_ada_state(&edge, orig, N, fresh(), sizeof(err)) == 0, "state at site n-2 with a matching digest not accepted: %s", err);
	edge.site = N - 4;
	edge.width = 4;
	memcpy(edge.old_bytes, orig + N - 4, 4);
	CHECK(gs_ada_state(&edge, orig, N, fresh(), sizeof(err)) == 0, "width-4 state at site n-4 with a matching digest not accepted: %s", err);

	/* 9. Width outside 1 to 16 is refused before digest or site bytes are read. */
	wide[0] = mine;
	wide[0].width = 0;
	wide[1] = mine;
	wide[1].width = 17;
	for (i = 0; i < 2; i++) {
		CHECK(refused(gs_ada_state(&wide[i], orig, N, fresh(), sizeof(err)), "1 to 16"), "state accepted width %u: %s", (unsigned)wide[i].width, err);
		memcpy(buf, orig, N);
		CHECK(refused(gs_ada_apply(&wide[i], buf, N, fresh(), sizeof(err)), "1 to 16") && same(buf, orig),
		      "apply with width %u not refused for width, or changed the buffer: %s", (unsigned)wide[i].width, err);
		memcpy(buf, rev, N);
		CHECK(refused(gs_ada_revert(&wide[i], buf, N, fresh(), sizeof(err)), "1 to 16") && same(buf, rev),
		      "revert with width %u not refused for width, or changed the buffer: %s", (unsigned)wide[i].width, err);
	}

	/* 10. Four-byte round trip over the revised buffer, and a four-byte restore. */
	CHECK(gs_ada_state(&next, rev, N, fresh(), sizeof(err)) == 0, "revised buffer is not state 0 of the four-byte row: %s", err);
	memcpy(buf, rev, N);
	CHECK(gs_ada_apply(&next, buf, N, fresh(), sizeof(err)) == 0, "four-byte apply failed: %s", err);
	CHECK(!memcmp(buf + SITE4, new4, sizeof(new4)), "four-byte apply did not write 31 c0 b0 04 at the site");
	CHECK(gs_ada_state(&next, buf, N, fresh(), sizeof(err)) == 1, "four-byte applied buffer is not state 1: %s", err);
	CHECK(gs_ada_revert(&next, buf, N, fresh(), sizeof(err)) == 0, "four-byte revert failed: %s", err);
	CHECK(same(buf, rev), "four-byte apply then revert is not byte-identical to the start");
	memcpy(bad_out, next_sha, sizeof(bad_out));
	bad_out[0] = bad_out[0] == '0' ? '1' : '0';
	bad = next;
	bad.output_sha = bad_out;
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_apply(&bad, buf, N, fresh(), sizeof(err)), "restored") && same(buf, rev),
	      "four-byte apply with a wrong output digest did not restore all four bytes: %s", err);

	/* 11. Fifteen-byte round trip over the original, and a fifteen-byte restore. */
	CHECK(gs_ada_state(&long15, orig, N, fresh(), sizeof(err)) == 0, "original buffer is not state 0 of the fifteen-byte row: %s", err);
	memcpy(buf, orig, N);
	CHECK(gs_ada_apply(&long15, buf, N, fresh(), sizeof(err)) == 0, "fifteen-byte apply failed: %s", err);
	CHECK(!memcmp(buf + SITE15, new15, sizeof(new15)), "fifteen-byte apply did not write all 15 new bytes at the site");
	CHECK(gs_ada_state(&long15, buf, N, fresh(), sizeof(err)) == 1, "fifteen-byte applied buffer is not state 1: %s", err);
	CHECK(gs_ada_revert(&long15, buf, N, fresh(), sizeof(err)) == 0, "fifteen-byte revert failed: %s", err);
	CHECK(same(buf, orig), "fifteen-byte apply then revert is not byte-identical to the start");
	memcpy(bad_out, fif_sha, sizeof(bad_out));
	bad_out[0] = bad_out[0] == '0' ? '1' : '0';
	bad = long15;
	bad.output_sha = bad_out;
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&bad, buf, N, fresh(), sizeof(err)), "restored") && same(buf, orig),
	      "fifteen-byte apply with a wrong output digest did not restore all fifteen bytes: %s", err);

	/* 12. Chained rows: rev's digest is mine's output and next's input, so the
	 * mode decides. This digest was once refused as ambiguous. */
	pair[0] = mine;
	pair[1] = next;
	CHECK(finds(pair, 2, rev, GS_ADA_FORWARD, 1, 0), "forward on the chain digest did not select the later row in state 0: %s", err);
	CHECK(finds(pair, 2, rev, GS_ADA_BACKWARD, 0, 1), "backward on the chain digest did not select the earlier row in state 1: %s", err);
	CHECK(finds(pair, 2, rev, GS_ADA_ANY, 1, 0), "any on the chain digest did not prefer the input match: %s", err);
	CHECK(finds(pair, 2, chained, GS_ADA_ANY, 1, 1), "any on an output-only digest did not fall back to the output match: %s", err);
	CHECK(misses(pair, 2, chained, GS_ADA_FORWARD, "already at output"), "forward on an output-only digest not refused: %s", err);
	CHECK(misses(pair, 2, orig, GS_ADA_BACKWARD, "not at output"), "backward on an input-only digest not refused: %s", err);
	pair[1] = mine;
	CHECK(misses(pair, 2, orig, GS_ADA_FORWARD, "more than one"), "two rows with one input digest not refused as ambiguous: %s", err);
	CHECK(misses(pair, 2, orig, 3, "unknown lookup direction"), "mode 3 not refused: %s", err);

	/* 13. Missing arguments. */
	CHECK(gs_ada_state(NULL, orig, N, fresh(), sizeof(err)) < 0 && err[0], "state without a revision not refused with a message");
	CHECK(gs_ada_apply(NULL, buf, N, fresh(), sizeof(err)) < 0 && err[0], "apply without a revision not refused with a message");
	CHECK(gs_ada_revert(NULL, buf, N, fresh(), sizeof(err)) < 0 && err[0], "revert without a revision not refused with a message");
	CHECK(gs_ada_state(&mine, NULL, 5, fresh(), sizeof(err)) < 0 && err[0], "state with NULL data and size 5 not refused with a message");
	CHECK(gs_ada_find(orig, N, GS_ADA_ANY, NULL, &st, fresh(), sizeof(err)) < 0 && err[0] && st == 42,
	      "find without a result pointer not refused cleanly");
	CHECK(gs_ada_find(orig, N, GS_ADA_ANY, &out, NULL, fresh(), sizeof(err)) < 0 && err[0] && out == &sentinel,
	      "find without a state pointer not refused cleanly");
	CHECK(misses(NULL, 1, orig, GS_ADA_ANY, "missing"), "find_in with a NULL table and count 1 not refused: %s", err);

	/* 14. The synthetic digests are not in the shipped table, in any mode. */
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		CHECK(refused(gs_ada_find(orig, N, modes[i], &out, &st, fresh(), sizeof(err)), "no supported byte patch"),
		      "find in mode %d did not refuse an unknown digest: %s", modes[i], err);
		CHECK(out == &sentinel && st == 42, "refused find in mode %d wrote a result", modes[i]);
	}
}

/* A synthetic file the size of the plugin's sites: filler bytes, with the original bytes of
 * the two shipped plugin rows at their file offsets. No vendor bytes beyond those sites. */
enum { P = 0xa6000 };
static unsigned char pstock[P], pmixed[P], pfinal[P], pbuf[P];

static const struct gs_ada_revision *shipped(const char *name)
{
	size_t i;

	for (i = 0; i < gs_ada_revision_count(); i++)
		if (!strcmp(gs_ada_revision_at(i)->name, name))
			return gs_ada_revision_at(i);
	return NULL;
}

/* gs_ada_find_in over the two plugin rows selects rows[want] in state `state`. */
static int pfinds(const struct gs_ada_revision *rows, const unsigned char *data, int mode, size_t want, int state)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return gs_ada_find_in(rows, 2, data, P, mode, &out, &st, fresh(), sizeof(err)) == 0 && out == &rows[want] && st == state;
}

/* gs_ada_find_in over the two plugin rows refuses for the expected reason and writes no result. */
static int pmisses(const struct gs_ada_revision *rows, const unsigned char *data, int mode, const char *why)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return refused(gs_ada_find_in(rows, 2, data, P, mode, &out, &st, fresh(), sizeof(err)), why) && out == &sentinel && st == 42;
}

static void plugin_chain(void)
{
	static const unsigned char div4_new[6] = {0xbf, 0x04, 0x00, 0x00, 0x00, 0x90};
	const struct gs_ada_revision *mixed = shipped("plugin-mixed"), *div4 = shipped("plugin-div4");
	char stock_sha[65], mixed_sha[65], final_sha[65], got[65], skew_sha[65];
	struct gs_ada_revision rows[2], skew;
	size_t i;

	CHECK(mixed && div4, "shipped table lacks plugin-mixed or plugin-div4");
	if (!mixed || !div4)
		return;
	CHECK(!strcmp(mixed->output_sha, div4->input_sha), "plugin-div4 does not take plugin-mixed's output as its input");
	CHECK(div4 == mixed + 1, "plugin-div4 is not the row after plugin-mixed");
	for (i = 0; i < P; i++)
		pstock[i] = (unsigned char)(i * 131u + 7u);
	memcpy(pstock + mixed->site, mixed->old_bytes, mixed->width);
	memcpy(pstock + div4->site, div4->old_bytes, div4->width);
	memcpy(pmixed, pstock, P);
	memcpy(pmixed + mixed->site, mixed->new_bytes, mixed->width);
	memcpy(pfinal, pmixed, P);
	memcpy(pfinal + div4->site, div4->new_bytes, div4->width);
	gs_sha256(pstock, P, SIZE_MAX, stock_sha);
	gs_sha256(pmixed, P, SIZE_MAX, mixed_sha);
	gs_sha256(pfinal, P, SIZE_MAX, final_sha);
	/* The shipped rows with this file's three digests in place of the vendor file's. */
	rows[0] = *mixed;
	rows[0].input_sha = stock_sha;
	rows[0].output_sha = mixed_sha;
	rows[1] = *div4;
	rows[1].input_sha = mixed_sha;
	rows[1].output_sha = final_sha;

	/* Forward: stock -> mixed -> div4, each step reproducing its recorded digest. */
	memcpy(pbuf, pstock, P);
	CHECK(pfinds(rows, pbuf, GS_ADA_FORWARD, 0, 0), "forward on the stock file did not select plugin-mixed at input: %s", err);
	CHECK(gs_ada_apply(&rows[0], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pmixed, P),
	      "plugin-mixed apply on the stock file did not give the mixed file: %s", err);
	gs_sha256(pbuf, P, SIZE_MAX, got);
	CHECK(!strcmp(got, mixed_sha), "after plugin-mixed the digest is %s, not the mixed digest", got);
	CHECK(pfinds(rows, pbuf, GS_ADA_FORWARD, 1, 0), "forward on the mixed file did not select plugin-div4 at input: %s", err);
	CHECK(pfinds(rows, pbuf, GS_ADA_ANY, 1, 0), "inspect on the mixed file did not prefer plugin-div4 at input: %s", err);
	CHECK(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pfinal, P),
	      "plugin-div4 apply on the mixed file did not give the final file: %s", err);
	gs_sha256(pbuf, P, SIZE_MAX, got);
	CHECK(!strcmp(got, final_sha), "after plugin-div4 the digest is %s, not the final digest", got);
	CHECK(!memcmp(pbuf + 0xa5f28, div4_new, sizeof(div4_new)) && !memcmp(pbuf + 0x8fc17, mixed->new_bytes, 15),
	      "the final file does not carry both revisions at 0x8fc17 and 0xa5f28");
	CHECK(pfinds(rows, pbuf, GS_ADA_ANY, 1, 1), "inspect on the final file did not select plugin-div4 at output: %s", err);
	CHECK(pmisses(rows, pbuf, GS_ADA_FORWARD, "already at output"), "apply on the final file not refused: %s", err);

	/* Backward: div4 -> mixed -> stock, byte-identical to each earlier step. */
	CHECK(pfinds(rows, pbuf, GS_ADA_BACKWARD, 1, 1), "revert on the final file did not select plugin-div4 at output: %s", err);
	CHECK(gs_ada_revert(&rows[1], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pmixed, P),
	      "plugin-div4 revert did not give back the mixed file: %s", err);
	CHECK(pfinds(rows, pbuf, GS_ADA_BACKWARD, 0, 1), "revert on the mixed file did not select plugin-mixed at output: %s", err);
	CHECK(gs_ada_revert(&rows[0], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pstock, P),
	      "plugin-mixed revert did not give back the stock file: %s", err);
	CHECK(pmisses(rows, pbuf, GS_ADA_BACKWARD, "not at output"), "revert on the stock file not refused: %s", err);

	/* Refusals of plugin-div4: an input that is not the mixed file, and wrong bytes at its site. */
	memcpy(pbuf, pstock, P);
	CHECK(refused(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)), "neither") && !memcmp(pbuf, pstock, P),
	      "plugin-div4 apply on the stock file not refused for its digest, or changed it: %s", err);
	memcpy(pbuf, pmixed, P);
	pbuf[0] ^= 0x01;
	CHECK(refused(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)), "neither"),
	      "plugin-div4 apply on a changed mixed file not refused for its digest: %s", err);
	CHECK(pmisses(rows, pbuf, GS_ADA_FORWARD, "no supported byte patch"), "forward on a changed mixed file not refused: %s", err);
	memcpy(pbuf, pfinal, P);
	CHECK(refused(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)), "already patched") && !memcmp(pbuf, pfinal, P),
	      "plugin-div4 apply on the final file not refused as already revised: %s", err);
	memcpy(pbuf, pmixed, P);
	CHECK(refused(gs_ada_revert(&rows[1], pbuf, P, fresh(), sizeof(err)), "not patched") && !memcmp(pbuf, pmixed, P),
	      "plugin-div4 revert on the mixed file not refused as not revised: %s", err);
	/* A file whose digest the row names but whose site holds other bytes. */
	pbuf[div4->site + 1] ^= 0x01;
	gs_sha256(pbuf, P, SIZE_MAX, skew_sha);
	skew = rows[1];
	skew.input_sha = skew_sha;
	CHECK(refused(gs_ada_apply(&skew, pbuf, P, fresh(), sizeof(err)), "recorded original bytes"),
	      "plugin-div4 apply with other bytes at 0xa5f28 not refused: %s", err);
	pbuf[div4->site + 1] ^= 0x01;
	CHECK(!memcmp(pbuf, pmixed, P), "refused plugin-div4 apply with other bytes at 0xa5f28 changed the file");
	/* A wrong recorded output digest restores all six bytes. */
	skew = rows[1];
	memcpy(got, final_sha, sizeof(got));
	got[0] = got[0] == '0' ? '1' : '0';
	skew.output_sha = got;
	memcpy(pbuf, pmixed, P);
	CHECK(refused(gs_ada_apply(&skew, pbuf, P, fresh(), sizeof(err)), "restored") && !memcmp(pbuf, pmixed, P),
	      "plugin-div4 apply with a wrong output digest did not restore the six bytes: %s", err);
}

/* ------------------------------------------------------- ada-placement row */

/* Synthetic images of the row: the object image of the locator tests with the window planted. */
enum { PL_IMG = 0x1000 };
static unsigned char plvf[PL_IMG], plwork[PL_IMG];

/* Admission: a digest match reports the digest path and the selected row; a digest
 * match with other site bytes names the revision; an unknown digest refuses with the
 * reserved anchor message and writes no result. */
static void admission(void)
{
	static const unsigned char old2[2] = {0x11, 0x22}, new2[2] = {0x33, 0x44};
	unsigned char orig[N], rev[N], buf[N];
	char in_sha[65], out_sha[65];
	struct gs_ada_revision mine, skew;
	struct gs_ada_found f;
	size_t i;

	for (i = 0; i < N; i++)
		orig[i] = (unsigned char)(i * 37u + 0x5au);
	memcpy(orig + SITE, old2, sizeof(old2));
	memcpy(rev, orig, N);
	memcpy(rev + SITE, new2, sizeof(new2));
	gs_sha256(orig, N, SIZE_MAX, in_sha);
	gs_sha256(rev, N, SIZE_MAX, out_sha);
	mine = (struct gs_ada_revision){
		.name = "synthetic", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = in_sha, .output_sha = out_sha, .site = SITE, .width = 2,
		.old_bytes = {0x11, 0x22}, .new_bytes = {0x33, 0x44}, .measured = 1,
	};

	/* Digest admission reports the path, the row and the located bytes. */
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(&mine, 1, orig, N, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0 &&
	      f.row == &mine && f.state == 0 && f.admission == GS_ADA_DIGEST && f.site == SITE && f.width == 2 &&
	      !memcmp(f.new_bytes, new2, 2), "digest admission did not select the row at input state: %s", err);
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(&mine, 1, rev, N, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 &&
	      f.row == &mine && f.state == 1 && f.admission == GS_ADA_DIGEST,
	      "digest admission did not select the row at output state: %s", err);

	/* The admitted edit moves the buffer in both directions. */
	memcpy(buf, rev, N);
	CHECK(gs_ada_edit(&f, buf, N, 1, fresh(), sizeof(err)) == 0 && !memcmp(buf, orig, N),
	      "digest revert did not restore the original: %s", err);
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(&mine, 1, orig, N, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0, "re-admit: %s", err);
	CHECK(gs_ada_edit(&f, buf, N, 0, fresh(), sizeof(err)) == 0 && !memcmp(buf, rev, N),
	      "digest apply did not write the revision: %s", err);
	CHECK(refused(gs_ada_edit(&f, buf, N, 0, fresh(), sizeof(err)), "already patched"),
	      "a second apply was not refused: %s", err);

	/* A matching digest with other bytes at the site names the revision. */
	skew = mine;
	skew.old_bytes[0] = 0x99;
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit_in(&skew, 1, orig, N, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.synthetic: input SHA-256 matches but the bytes at the patch site are not the recorded original bytes"),
	      "admission accepted a digest with other site bytes: %s", err);
	CHECK(f.row == NULL, "refused admission wrote a result");

	/* An already revised or unrevised file names the revision, not the unknown anchor. */
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit_in(&mine, 1, rev, N, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.synthetic: already at output state"),
	      "admission of an applied file not refused as already at output: %s", err);
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit_in(&mine, 1, orig, N, GS_ADA_BACKWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.synthetic: not at output state"),
	      "admission of a stock file for revert not refused as not at output: %s", err);

	/* An unknown digest refuses with the reserved structural-fallback message. */
	memcpy(buf, orig, N);
	buf[0] ^= 0x01;
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit_in(&mine, 1, buf, N, GS_ADA_ANY, &f, fresh(), sizeof(err)),
		      "anchor ada.unknown: no recipe matches this input and no structural locator is implemented for it"),
	      "admission of an unknown digest did not refuse with the anchor message: %s", err);
	CHECK(f.row == NULL, "refused admission of an unknown digest wrote a result");
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit(buf, N, GS_ADA_ANY, &f, fresh(), sizeof(err)),
		      "anchor ada.unknown: no recipe matches this input and no structural locator is implemented for it"),
	      "shipped admission of an unknown digest did not refuse with the anchor message: %s", err);
	CHECK(refused(gs_ada_admit_in(NULL, 1, orig, N, GS_ADA_ANY, &f, fresh(), sizeof(err)), "missing data"),
	      "admission without a table not refused: %s", err);

	/* A structural revert is refused: the replaced bytes are not recoverable. */
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(&mine, 1, rev, N, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0, "admit patched: %s", err);
	f.admission = GS_ADA_ANCHOR;
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_edit(&f, buf, N, 1, fresh(), sizeof(err)), "structural revert") && !memcmp(buf, rev, N),
	      "structural revert not refused, or changed the buffer: %s", err);
}

/* ---------------------------------------------------------------- plugin sites */

enum { IMG = 0x2000, TEXT_SIZE = 0x400 };

/* Synthetic ET_DYN shared object: .text at a chosen file offset and virtual
 * address, .rodata holding the log string, and one .rela.dyn entry with
 * sh_info 0. No vendor bytes beyond the instruction shapes documented in
 * RE-PLUGIN-LOCATORS.md. */
struct plugin_fixture {
	size_t text_off;
	uint64_t text_addr, rodata_addr;
	int measured_shape, place_patched, div_patched;
	unsigned divisor;
	int32_t desc;
	int32_t stock_disp;
	unsigned char src_rm, error_high;
	int with_string, with_type_info, duplicate_div, duplicate_lea;
};
static struct plugin_fixture pf;
static unsigned char img[IMG];
static size_t img_len, img_site, img_div;

static void put32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void plugin_defaults(void)
{
	pf = (struct plugin_fixture){
		.text_off = 0x200, .text_addr = 0x1000, .rodata_addr = 0x2000,
		.measured_shape = 1, .desc = 0x2b60, .stock_disp = 0x29d0, .src_rm = 1,
		.with_string = 1, .with_type_info = 1,
	};
}

static void build_plugin(void)
{
	/* Local to this builder so they cannot be confused with the 64-byte row sites. */
	enum { SITE = 0x40, TARGET = 0x100, DIV = 0x200 };
	static const char names[] = "\0.text\0.rodata\0.shstrtab\0.rela.dyn\0";
	static const char logstr[] = "vmiop_log: (0x%x): placement_fb_length 0x%llx is not equal to allocation_size 0x%llx";
	Elf64_Ehdr eh = { 0 };
	Elf64_Shdr sh[5] = { { 0 } };
	const size_t roff = 0x1000, soff = 0x1200, reloff = 0x1300, shoff = 0x1400;
	unsigned char *t = img + pf.text_off;
	const size_t lea = SITE + 0x1e;
	size_t target;

	memset(img, 0, sizeof(img));
	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_ident[EI_VERSION] = EV_CURRENT;
	eh.e_type = ET_DYN;
	eh.e_machine = EM_X86_64;
	eh.e_version = EV_CURRENT;
	eh.e_ehsize = sizeof(eh);
	eh.e_shoff = shoff;
	eh.e_shentsize = sizeof(sh[0]);
	eh.e_shnum = 5;
	eh.e_shstrndx = 3;
	sh[1].sh_name = 1;
	sh[1].sh_type = SHT_PROGBITS;
	sh[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
	sh[1].sh_addr = pf.text_addr;
	sh[1].sh_offset = pf.text_off;
	sh[1].sh_size = TEXT_SIZE;
	sh[1].sh_addralign = 16;
	sh[2].sh_name = 7;
	sh[2].sh_type = SHT_PROGBITS;
	sh[2].sh_flags = SHF_ALLOC;
	sh[2].sh_addr = pf.rodata_addr;
	sh[2].sh_offset = roff;
	sh[2].sh_size = 0x100;
	sh[2].sh_addralign = 1;
	sh[3].sh_name = 15;
	sh[3].sh_type = SHT_STRTAB;
	sh[3].sh_offset = soff;
	sh[3].sh_size = sizeof(names);
	sh[4].sh_name = 25;
	sh[4].sh_type = SHT_RELA;
	sh[4].sh_offset = reloff;
	sh[4].sh_size = sizeof(Elf64_Rela);
	sh[4].sh_entsize = sizeof(Elf64_Rela);
	sh[4].sh_link = 1;
	sh[4].sh_info = 0;
	sh[4].sh_addralign = 8;

	if (pf.with_string)
		memcpy(img + roff, logstr, sizeof(logstr));
	if (pf.measured_shape) {
		/* The exact 15 site bytes and 16th byte of the recorded 580.159.01 row. */
		memcpy(t + SITE, "\x4c\x39\xc1\x0f\x84\x08\x03\x00\x00\x48\x8d\x05\x91\x78\x2d\x00", 16);
		target = SITE + 9 + 0x308;
	} else {
		t[SITE] = 0x4c;
		t[SITE + 1] = 0x39;
		t[SITE + 2] = (unsigned char)(0xc0 | pf.src_rm);
		t[SITE + 3] = 0x0f;
		t[SITE + 4] = 0x84;
		target = TARGET;
		put32le(t + SITE + 5, (uint32_t)(int32_t)((int32_t)target - (int32_t)(SITE + 9)));
		memcpy(t + SITE + 9, "\x48\x8d\x05\x00\x00\x00", 6);
		t[SITE + 15] = pf.error_high;
	}
	t[lea] = 0x48;
	t[lea + 1] = 0x8d;
	t[lea + 2] = 0x35;
	put32le(t + lea + 3, (uint32_t)(int32_t)((int64_t)pf.rodata_addr - (int64_t)(pf.text_addr + lea + 7)));
	if (pf.duplicate_lea) {
		const size_t d2 = lea + 0x28;

		t[d2] = 0x48;
		t[d2 + 1] = 0x8d;
		t[d2 + 2] = 0x35;
		put32le(t + d2 + 3, (uint32_t)(int32_t)((int64_t)pf.rodata_addr - (int64_t)(pf.text_addr + d2 + 7)));
	}
	memcpy(t + target, "\x48\x8b\x83", 3);
	put32le(t + target + 3, (uint32_t)pf.desc);
	memcpy(t + target + 7, "\x83\x48\x10\x02", 4);
	if (pf.with_type_info)
		memcpy(t + DIV - 0x40, "\xba\x03\x01\x81\xa0", 5);
	memcpy(t + DIV - 9, "\x48\x89\xf8\x49\xf7\xf1\x48\x89\xf8", 9);
	if (pf.div_patched) {
		t[DIV] = 0xbf;
		put32le(t + DIV + 1, pf.divisor);
		t[DIV + 5] = 0x90;
	} else {
		t[DIV] = 0x8b;
		t[DIV + 1] = 0xbb;
		put32le(t + DIV + 2, (uint32_t)pf.stock_disp);
	}
	memcpy(t + DIV + 6, "\x48\x29\xd0\x31\xd2\x48\x89\x83\x70\x05\x00\x00\x48\xf7\xf7", 15);
	if (pf.duplicate_div)
		memcpy(t + DIV + 0x80 - 9, t + DIV - 9, 30);
	if (pf.place_patched) {
		t[SITE] = 0x48;
		t[SITE + 1] = 0x8b;
		t[SITE + 2] = 0x93;
		put32le(t + SITE + 3, (uint32_t)pf.desc);
		memcpy(t + SITE + 7, "\x48\x89\x4a\x08\xe9", 5);
		put32le(t + SITE + 12, (uint32_t)(int32_t)((int64_t)target - (int64_t)(SITE + 16)));
	}
	memcpy(img, &eh, sizeof(eh));
	memcpy(img + shoff, sh, sizeof(sh));
	memcpy(img + soff, names, sizeof(names));
	img_len = 0x1600;
	img_site = pf.text_off + SITE;
	img_div = pf.text_off + DIV;
}

static void plugin_locators(void)
{
	const struct gs_ada_revision *mixed = shipped("plugin-mixed"), *div4 = shipped("plugin-div4");
	struct gs_ada_revision one[1];
	struct gs_ada_found f;
	unsigned char copy[IMG];
	char mixed_sha[65], final_sha[65];

	CHECK(mixed && div4, "shipped table lacks plugin-mixed or plugin-div4");
	if (!mixed || !div4)
		return;

	/* Stock plugin with the measured site shape: the located bytes are the row's. */
	plugin_defaults();
	build_plugin();
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0, "stock plugin not admitted: %s", err);
	CHECK(f.row == mixed && f.admission == GS_ADA_ANCHOR && f.state == 0 && f.site == img_site && f.width == 16,
	      "stock plugin admission is not the placement row at the located site");
	CHECK(!memcmp(f.old_bytes, mixed->old_bytes, 15), "located old bytes differ from the recorded row");
	CHECK(!memcmp(f.new_bytes, mixed->new_bytes, 15) && f.new_bytes[15] == 0,
	      "located rewrite differs from the recorded row");
	memcpy(copy, img, img_len);
	CHECK(gs_ada_edit(&f, copy, img_len, 0, fresh(), sizeof(err)) == 0, "placement apply failed: %s", err);
	CHECK(!memcmp(copy + img_site, f.new_bytes, 16), "placement apply did not write the located rewrite");
	CHECK(!memcmp(copy + img_site + 16, img + img_site + 16, 0x40), "placement apply changed bytes after the site");
	gs_sha256(copy, img_len, SIZE_MAX, mixed_sha);

	/* The mixed file is plugin-div4's input. */
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(copy, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0, "mixed plugin not admitted: %s", err);
	CHECK(f.row == div4 && f.admission == GS_ADA_ANCHOR && f.state == 0 && f.site == img_div && f.divisor == 4,
	      "mixed plugin admission is not the div row at the located site");
	CHECK(!memcmp(f.old_bytes, div4->old_bytes, 6) && !memcmp(f.new_bytes, div4->new_bytes, 6),
	      "located div bytes differ from the recorded row");
	CHECK(gs_ada_edit(&f, copy, img_len, 0, fresh(), sizeof(err)) == 0, "div apply failed: %s", err);
	CHECK(!memcmp(copy + img_div, div4->new_bytes, 6), "div apply did not write the recorded bytes");
	gs_sha256(copy, img_len, SIZE_MAX, final_sha);
	CHECK(strcmp(mixed_sha, final_sha) != 0, "the two chain steps produced the same file");

	/* The final file is at output state for both rows. */
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit(copy, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-mixed: already at output state"), "final plugin not refused: %s", err);
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(copy, img_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.row == div4 && f.state == 1 &&
	      f.divisor == 4, "final plugin inspect did not report plugin-div4 at output state: %s", err);
	CHECK(refused(gs_ada_admit(copy, img_len, GS_ADA_BACKWARD, &f, fresh(), sizeof(err)), "structural revert"),
	      "structural revert not refused: %s", err);

	/* Shifted file offset and virtual addresses: the same anchors still resolve. */
	plugin_defaults();
	pf.text_off = 0x600;
	pf.text_addr = 0x9000;
	pf.rodata_addr = 0x20000;
	build_plugin();
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0 && f.site == img_site &&
	      f.site == 0x640, "shifted plugin not found at its file offset: %s", err);
	CHECK(!memcmp(f.new_bytes, mixed->new_bytes, 15), "shifted plugin rewrite differs from the recorded row");
	memcpy(copy, img, img_len);
	CHECK(gs_ada_edit(&f, copy, img_len, 0, fresh(), sizeof(err)) == 0 && !memcmp(copy + 0x640, f.new_bytes, 16),
	      "shifted plugin apply did not write at the shifted file offset: %s", err);

	/* Already-patched placement is recognised, and apply refuses it. */
	plugin_defaults();
	pf.place_patched = 1;
	build_plugin();
	one[0] = *mixed;
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(one, 1, img, img_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.state == 1 &&
	      f.admission == GS_ADA_ANCHOR, "patched placement not recognised: %s", err);
	CHECK(refused(gs_ada_admit_in(one, 1, img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "already at output state"), "patched placement not refused for apply: %s", err);

	/* Patched div: the divisor is re-derived, and only 4 is the recorded value. */
	plugin_defaults();
	pf.div_patched = 1;
	pf.divisor = 4;
	build_plugin();
	one[0] = *div4;
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(one, 1, img, img_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.state == 1 &&
	      f.divisor == 4, "patched div4 not recognised: %s", err);
	CHECK(refused(gs_ada_admit_in(one, 1, img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "already at output state"), "patched div4 not refused for apply: %s", err);
	plugin_defaults();
	pf.div_patched = 1;
	pf.divisor = 2;
	build_plugin();
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(one, 1, img, img_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.divisor == 2,
	      "patched div2 not recognised: %s", err);
	CHECK(refused(gs_ada_admit_in(one, 1, img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "installed divisor 2 is not the recorded 4"), "patched div2 not refused with its divisor: %s", err);
	/* With the full table the placement row is still stock, but a foreign divisor blocks apply. */
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-div4: installed divisor 2 is not the recorded 4"),
	      "full-table apply on a div2 plugin not refused: %s", err);

	/* Anchor refusals, each naming the row. */
	plugin_defaults();
	pf.measured_shape = 0;
	pf.desc = 0x2b70;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-mixed: memdesc pointer offset 0x2b70 is not a measured value"),
	      "unmeasured memdesc offset not refused: %s", err);
	plugin_defaults();
	pf.measured_shape = 0;
	pf.src_rm = 6;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-mixed: compared source register is not rcx (rm=6)"),
	      "wrong compared register not refused: %s", err);
	plugin_defaults();
	pf.with_string = 0;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-mixed: 0 copies of the 'is not equal to allocation_size' log string (want 1)"),
	      "missing log string not refused: %s", err);
	plugin_defaults();
	pf.duplicate_lea = 1;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-mixed: 2 lea rsi refs to the log string (want 1)"),
	      "duplicate log reference not refused: %s", err);
	plugin_defaults();
	pf.duplicate_div = 1;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-div4: 2 division sequences (want 1)"),
	      "duplicate division sequence not refused: %s", err);
	plugin_defaults();
	pf.with_type_info = 0;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "0xa0810103 (GET_VGPU_TYPE_INFO) is not within 0x1000 bytes before the site"),
	      "missing type-info command not refused: %s", err);
	plugin_defaults();
	pf.stock_disp = 0x29d8;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-div4: maxVgpus field offset 0x29d8 is not a measured value"),
	      "unmeasured maxVgpus offset not refused: %s", err);
	plugin_defaults();
	pf.div_patched = 1;
	pf.divisor = 3;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-div4: installed divisor 3 is not a measured value"),
	      "unmeasured installed divisor not refused: %s", err);
	plugin_defaults();
	pf.measured_shape = 0;
	pf.error_high = 0x7f;
	build_plugin();
	CHECK(refused(gs_ada_admit(img, img_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.plugin-mixed: byte at site+15 is 0x7f, the rewrite needs 0x00"),
	      "site+15 mismatch not refused: %s", err);

	/* Container: the shared-object open accepts ET_DYN with sh_info 0; ET_REL does not. */
	plugin_defaults();
	build_plugin();
	{
		struct gs_elf e;

		CHECK(gs_elf_open_shared(img, img_len, &e, fresh(), sizeof(err)) == 0, "shared-object open failed: %s", err);
		CHECK(gs_elf_open(img, img_len, &e, fresh(), sizeof(err)) < 0, "relocatable open accepted an ET_DYN");
		img[16] = ET_REL;
		CHECK(refused(gs_elf_open(img, img_len, &e, fresh(), sizeof(err)), "relocation section 4"),
		      "ET_REL with a dynamic relocation table not refused: %s", err);
		img[16] = ET_DYN;
	}
	CHECK(refused(gs_ada_admit(img, 0x100, GS_ADA_FORWARD, &f, fresh(), sizeof(err)), "anchor ada.plugin-mixed:"),
	      "truncated shared object not refused with the row name: %s", err);
}

/* ------------------------------------------------------------- object fixtures */

enum { OBJ_IMG = 0x2000, OBJ_TEXT = 0x400, OBJ_TEXT_OFF = 0x200 };

/* Synthetic ET_REL RM object: .text, .rela.text, .rodata, .symtab/.strtab. It
 * carries only the instruction shapes documented in RE-GATE-VF-LOCATORS.md. */
struct obj_fixture {
	size_t text_off;
	size_t shift;
	int patched_gate, patched_vf;
	int no_string, dup_gate, bad_hdr, bad_store, bad_load, no_thunk, bad_branch, rename_scope;
};
static struct obj_fixture of;
static unsigned char objimg[OBJ_IMG];
static size_t obj_len, obj_gate_site, obj_vf_site, obj_text_off;

static void obj_defaults(void)
{
	memset(&of, 0, sizeof(of));
	of.text_off = OBJ_TEXT_OFF;
}

static void build_obj(void)
{
	static const char shnames[] = "\0.text\0.rela.text\0.rodata\0.symtab\0.strtab\0.shstrtab\0";
	static const char strtab_tmpl[] = "\0.text\0.rodata\0os_is_vgx_hyper\0_nv000001rm\0rm_is_supported_device\0"
					  "_nv039075rm\0_nv000002rm\0";
	static const char sentence[] = "NVRM: installed in this vGPU host system is not supported by proprietary nvidia.ko.";
	static const unsigned char bars[5][5] = {
		{ 0xbe, 0xf0, 0x0b, 0x00, 0x00 }, { 0xbe, 0xf4, 0x0b, 0x00, 0x00 }, { 0xbe, 0xf8, 0x0b, 0x00, 0x00 },
		{ 0xbe, 0xfc, 0x0b, 0x00, 0x00 }, { 0xbe, 0x00, 0x0c, 0x00, 0x00 },
	};
	Elf64_Ehdr eh = { 0 };
	Elf64_Shdr sh[7] = { { 0 } };
	Elf64_Sym sy[8] = { { 0 } };
	Elf64_Rela re[10] = { { 0 } };
	char strtab[sizeof(strtab_tmpl)];
	const size_t sym_off = 0xb00, str_off = 0xc00, ro_off = 0xa00, rela_off = 0x800, sh_off = 0xe00;
	const size_t S = of.shift;
	unsigned char *t = objimg + of.text_off;
	size_t nrel = 0, i;

	memset(objimg, 0, sizeof(objimg));
	memcpy(strtab, strtab_tmpl, sizeof(strtab));
	if (of.rename_scope) {
		char *p = memmem(strtab, sizeof(strtab), "rm_is_supported_device", 22);

		if (p)
			p[strlen("rm_is_supported_device") - 1] = 'X';
	}

	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_ident[EI_VERSION] = EV_CURRENT;
	eh.e_type = ET_REL;
	eh.e_machine = EM_X86_64;
	eh.e_version = EV_CURRENT;
	eh.e_ehsize = sizeof(eh);
	eh.e_shoff = sh_off;
	eh.e_shentsize = sizeof(sh[0]);
	eh.e_shnum = 7;
	eh.e_shstrndx = 6;

	sh[1].sh_name = 1;
	sh[1].sh_type = SHT_PROGBITS;
	sh[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
	sh[1].sh_offset = of.text_off;
	sh[1].sh_size = OBJ_TEXT;
	sh[1].sh_addralign = 16;
	sh[2].sh_name = 7;
	sh[2].sh_type = SHT_RELA;
	sh[2].sh_offset = rela_off;
	sh[2].sh_entsize = sizeof(Elf64_Rela);
	sh[2].sh_link = 4;
	sh[2].sh_info = 1;
	sh[2].sh_addralign = 8;
	sh[3].sh_name = 18;
	sh[3].sh_type = SHT_PROGBITS;
	sh[3].sh_flags = SHF_ALLOC;
	sh[3].sh_offset = ro_off;
	sh[3].sh_size = 0x100;
	sh[3].sh_addralign = 1;
	sh[4].sh_name = 26;
	sh[4].sh_type = SHT_SYMTAB;
	sh[4].sh_offset = sym_off;
	sh[4].sh_size = sizeof(sy);
	sh[4].sh_entsize = sizeof(sy[0]);
	sh[4].sh_link = 5;
	sh[4].sh_info = 1;
	sh[4].sh_addralign = 8;
	sh[5].sh_name = 34;
	sh[5].sh_type = SHT_STRTAB;
	sh[5].sh_offset = str_off;
	sh[5].sh_size = sizeof(strtab);
	sh[5].sh_addralign = 1;
	sh[6].sh_name = 42;
	sh[6].sh_type = SHT_STRTAB;
	sh[6].sh_offset = 0xd00;
	sh[6].sh_size = sizeof(shnames);
	sh[6].sh_addralign = 1;

	sy[1].st_name = 1;
	sy[1].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
	sy[1].st_shndx = 1;
	sy[2].st_name = 7;
	sy[2].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
	sy[2].st_shndx = 3;
	sy[3].st_name = 15;
	sy[3].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	sy[3].st_shndx = 1;
	sy[3].st_value = 0x300 + S;
	sy[3].st_size = 0x10;
	sy[4].st_name = 31;
	sy[4].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	sy[4].st_shndx = 1;
	sy[4].st_value = 0x320 + S;
	sy[4].st_size = 9;
	sy[5].st_name = 43;
	sy[5].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	sy[5].st_shndx = 1;
	sy[5].st_value = 0x40 + S;
	sy[5].st_size = 0x120;
	sy[6].st_name = 66;
	sy[6].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	sy[6].st_shndx = 1;
	sy[6].st_value = 0x200 + S;
	sy[6].st_size = 0x100;
	sy[7].st_name = 78;
	sy[7].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	sy[7].st_shndx = 1;
	sy[7].st_value = 0x380 + S;
	sy[7].st_size = 0x10;

	/* Probe gate: call the thunk, test al,al, branch into the refusal block. */
	t[0x40 + S] = 0xe8;
	if (of.patched_gate) {
		t[0x47 + S] = 0x90;
		t[0x48 + S] = 0x90;
	} else {
		t[0x47 + S] = of.bad_branch ? 0x74 : 0x75;
		t[0x48 + S] = 0x7f;
	}
	t[0x45 + S] = 0x84;
	t[0x46 + S] = 0xc0;
	t[0xc8 + S] = 0x44;
	t[0xc9 + S] = 0x89;
	t[0xca + S] = 0xff;
	t[0xcb + S] = 0x48;
	t[0xcc + S] = 0x8d;
	t[0xcd + S] = 0x05;
	if (of.dup_gate) {
		t[0x80 + S] = 0xe8;
		t[0x85 + S] = 0x84;
		t[0x86 + S] = 0xc0;
		t[0x87 + S] = 0x75;
		t[0x88 + S] = 0x3f;
	}

	/* VF count: HDR3 read, call, TotalVFs load, HDR5 read, store, BAR reads. */
	memcpy(t + 0x200 + S, "\xbe\xd8\x0b\x00\x00", 5);
	t[0x205 + S] = 0xe8;
	if (of.bad_load)
		memcpy(t + 0x20a + S, "\x0f\xb7\x45\x04", 4);
	else if (of.patched_vf)
		memcpy(t + 0x20a + S, "\x31\xc0\xb0\x04", 4);
	else
		memcpy(t + 0x20a + S, "\x0f\xb7\x45\x06", 4);
	memcpy(t + 0x20e + S, "\xbe\xe0\x0b\x00\x00", 5);
	if (of.bad_store)
		memcpy(t + 0x213 + S, "\x89\xc3\x90\x90\x90\x90", 6);
	else
		memcpy(t + 0x213 + S, "\x89\x83\x60\x41\x00\x00", 6);
	for (i = 0; i < 5; i++) {
		memcpy(t + 0x230 + S + i * 0x10, bars[i], 5);
		if (of.bad_hdr && i == 3)
			memcpy(t + 0x230 + S + i * 0x10, bars[2], 5);
		t[0x235 + S + i * 0x10] = 0xe8;
	}

	t[0x300 + S] = 0xc3;
	t[0x320 + S] = 0xf3;
	t[0x321 + S] = 0x0f;
	t[0x322 + S] = 0x1e;
	t[0x323 + S] = 0xfa;
	t[0x324 + S] = 0xe9;
	t[0x380 + S] = 0xc3;

	if (!of.no_string)
		memcpy(objimg + ro_off, sentence, sizeof(sentence));

	/* relocations */
	if (!of.no_thunk) {
		re[nrel].r_offset = 0x41 + S;
		re[nrel].r_info = ELF64_R_INFO(4, R_X86_64_PC32);
		re[nrel].r_addend = -4;
		nrel++;
		re[nrel].r_offset = 0x325 + S;
		re[nrel].r_info = ELF64_R_INFO(3, R_X86_64_PC32);
		re[nrel].r_addend = -4;
		nrel++;
	}
	re[nrel].r_offset = 0x206 + S;
	re[nrel].r_info = ELF64_R_INFO(7, R_X86_64_PC32);
	re[nrel].r_addend = -4;
	nrel++;
	for (i = 0; i < 5; i++) {
		re[nrel].r_offset = 0x236 + S + i * 0x10;
		re[nrel].r_info = ELF64_R_INFO(7, R_X86_64_PC32);
		re[nrel].r_addend = -4;
		nrel++;
	}
	if (of.dup_gate) {
		re[nrel].r_offset = 0x81 + S;
		re[nrel].r_info = ELF64_R_INFO(4, R_X86_64_PC32);
		re[nrel].r_addend = -4;
		nrel++;
	}
	re[nrel].r_offset = 0xce + S;
	re[nrel].r_info = ELF64_R_INFO(2, R_X86_64_32S);
	re[nrel].r_addend = 0;
	nrel++;
	sh[2].sh_size = nrel * sizeof(Elf64_Rela);

	memcpy(objimg, &eh, sizeof(eh));
	memcpy(objimg + sh_off, sh, sizeof(sh));
	memcpy(objimg + sym_off, sy, sizeof(sy));
	memcpy(objimg + str_off, strtab, sizeof(strtab));
	memcpy(objimg + 0xd00, shnames, sizeof(shnames));
	memcpy(objimg + rela_off, re, nrel * sizeof(re[0]));
	obj_len = 0x1000;
	obj_text_off = of.text_off;
	obj_gate_site = of.text_off + 0x47 + S;
	obj_vf_site = of.text_off + 0x20a + S;
}

static void object_locators(void)
{
	const struct gs_ada_revision *gate = shipped("module-gate"), *vf = shipped("vf-count");
	struct gs_ada_revision one[1];
	struct gs_ada_found f;
	unsigned char copy[OBJ_IMG];

	CHECK(gate && vf, "shipped table lacks module-gate or vf-count");
	if (!gate || !vf)
		return;

	/* Stock object: the gate is the first pending revision. */
	obj_defaults();
	build_obj();
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0, "stock object not admitted: %s", err);
	CHECK(f.row == gate && f.admission == GS_ADA_ANCHOR && f.state == 0 && f.site == obj_gate_site && f.width == 2,
	      "stock object admission is not the gate row at the located site");
	CHECK(f.function && !strcmp(f.function, "rm_is_supported_device") && f.function_offset == 0x7,
	      "gate admission did not report the containing function and offset");
	CHECK(!memcmp(f.old_bytes, "\x75\x7f", 2) && !memcmp(f.new_bytes, "\x90\x90", 2),
	      "located gate bytes differ from the recorded row");
	memcpy(copy, objimg, obj_len);
	CHECK(gs_ada_edit(&f, copy, obj_len, 0, fresh(), sizeof(err)) == 0 && !memcmp(copy + obj_gate_site, "\x90\x90", 2),
	      "gate apply did not write the two NOPs: %s", err);

	/* The gated object is vf-count's input. */
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(copy, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0, "gated object not admitted: %s", err);
	CHECK(f.row == vf && f.state == 0 && f.site == obj_vf_site && f.width == 4,
	      "gated object admission is not the vf row at the located site");
	CHECK(f.function && !strcmp(f.function, "_nv039075rm") && f.function_offset == 0xa,
	      "vf admission did not report the containing function and offset");
	CHECK(!memcmp(f.new_bytes, "\x31\xc0\xb0\x04", 4), "located vf bytes differ from the recorded row");
	CHECK(gs_ada_edit(&f, copy, obj_len, 0, fresh(), sizeof(err)) == 0 && !memcmp(copy + obj_vf_site, "\x31\xc0\xb0\x04", 4),
	      "vf apply did not write the four bytes: %s", err);

	/* Both revisions applied: nothing left, and inspect reports the last row. */
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit(copy, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.ada-placement: window:"), "patched object without a placement window not refused: %s", err);
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(copy, obj_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.row == vf && f.state == 1,
	      "patched object inspect did not report vf-count at output state: %s", err);
	CHECK(refused(gs_ada_admit(copy, obj_len, GS_ADA_BACKWARD, &f, fresh(), sizeof(err)), "structural revert"),
	      "structural revert not refused: %s", err);

	/* Already-patched recognition for each row on its own. */
	obj_defaults();
	of.patched_gate = 1;
	build_obj();
	one[0] = *gate;
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(one, 1, objimg, obj_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.state == 1 &&
	      f.admission == GS_ADA_ANCHOR, "patched gate not recognised: %s", err);
	CHECK(refused(gs_ada_admit_in(one, 1, objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "already at output state"), "patched gate not refused for apply: %s", err);
	obj_defaults();
	of.patched_vf = 1;
	build_obj();
	one[0] = *vf;
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit_in(one, 1, objimg, obj_len, GS_ADA_ANY, &f, fresh(), sizeof(err)) == 0 && f.state == 1,
	      "patched vf not recognised: %s", err);

	/* Shifted .text file offset and shifted code inside .text. */
	obj_defaults();
	of.text_off = 0x300;
	of.shift = 0x80;
	build_obj();
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0 && f.site == obj_gate_site &&
	      f.site == 0x3c7 && f.function_offset == 0x7,
	      "shifted object not found at its file offset: %s", err);

	/* The name-independent fallback: renaming the scope symbol still finds one site. */
	obj_defaults();
	of.rename_scope = 1;
	build_obj();
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0 && f.site == obj_gate_site &&
	      f.function && !strcmp(f.function, "rm_is_supported_devicX"),
	      "text-scan fallback did not find the gate: %s", err);

	/* Refusals, each naming the row and the stage. */
	obj_defaults();
	of.dup_gate = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.module-gate: unique: 2 gate candidates"), "duplicate gate not refused: %s", err);
	obj_defaults();
	of.no_string = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.module-gate: reject-block:"), "missing reject string not refused: %s", err);
	obj_defaults();
	of.no_thunk = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.module-gate: vgx-thunk:"), "missing vgx target not refused: %s", err);
	obj_defaults();
	of.bad_branch = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.module-gate: branch:"), "changed branch not refused: %s", err);
	obj_defaults();
	of.bad_hdr = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.vf-count: bar-window:"), "changed HDR sequence not refused: %s", err);
	obj_defaults();
	of.bad_store = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.vf-count: store:"), "changed store shape not refused: %s", err);
	obj_defaults();
	of.bad_load = 1;
	build_obj();
	CHECK(refused(gs_ada_admit(objimg, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)),
		      "anchor ada.vf-count: totalvfs-load:"), "wrong TotalVFs load not refused: %s", err);

	/* A shared object is not these rows' input: the object rows are absent. */
	obj_defaults();
	build_obj();
	one[0] = *shipped("plugin-mixed");
	memset(&f, 0, sizeof(f));
	CHECK(refused(gs_ada_admit_in(one, 1, objimg, obj_len, GS_ADA_ANY, &f, fresh(), sizeof(err)),
		      "anchor ada.unknown: no recipe matches this input"), "an object was treated as a plugin: %s", err);
}

static const char vendor_object[] = "/home/nathan/vgpu-work/ds580/nv-kernel.o_binary";

/* Admits `data` through a one-row table of the shipped row whose digests cannot match, so only the locator runs. */
static int placement_admit(const struct gs_ada_revision *shp, const unsigned char *data, size_t size,
			   struct gs_ada_found *f)
{
	static const char none[] = "0000000000000000000000000000000000000000000000000000000000000000";
	struct gs_ada_revision row = *shp;

	row.input_sha = none;
	row.output_sha = none;
	memset(f, 0, sizeof(*f));
	return gs_ada_admit_in(&row, 1, data, size, GS_ADA_ANY, f, fresh(), sizeof(err));
}


static int run(const char *cmd, char *out, size_t outlen);

/* ada-placement: the window is 19 bytes of code before the changed immediate and 4 after it. */
static const unsigned char placement_head[19] = { 0x48, 0x69, 0x45, 0x08, 0x58, 0x4f, 0x13, 0x00, 0x48, 0x8b,
						    0x75, 0x00, 0xc6, 0x84, 0x06, 0x3a, 0x4c, 0x13, 0x00 };
static const unsigned char placement_tail[4] = { 0xe9, 0xd1, 0xf8, 0xff };

/* The row on a synthetic object: one window found, stock and patched states, two windows refused,
 * no window not found. */
static void placement_synthetic(void)
{
	const struct gs_ada_revision *r = shipped("ada-placement"), *vf = shipped("vf-count");
	struct gs_ada_found f;
	size_t at, dup;

	CHECK(r && vf, "shipped table lacks ada-placement or vf-count");
	if (!r || !vf)
		return;
	CHECK(!strcmp(vf->output_sha, r->input_sha), "ada-placement does not take the plain chain's final object");
	CHECK(r->site == 0xd3a86 && r->width == 1 && r->old_bytes[0] == 0x01 && r->new_bytes[0] == 0x00 &&
	      r->function && !strcmp(r->function, "_nv042355rm") && r->function_offset == 0x7f6,
	      "ada-placement is not the recorded row");

	obj_defaults();
	build_obj();
	at = obj_text_off + 0x300;
	dup = obj_text_off + 0x360;
	memcpy(plvf, objimg, obj_len);
	memcpy(plvf + at, placement_head, sizeof(placement_head));
	plvf[at + sizeof(placement_head)] = 0x01;
	memcpy(plvf + at + sizeof(placement_head) + 1, placement_tail, sizeof(placement_tail));
	CHECK(placement_admit(r, plvf, obj_len, &f) == 0 && f.admission == GS_ADA_ANCHOR && f.state == 0 &&
	      f.site == at + sizeof(placement_head) && f.width == 1 && f.old_bytes[0] == 0x01 && f.new_bytes[0] == 0x00,
	      "window of ada-placement not located at the changed byte in state 0: %s", err);
	/* The edit changes exactly that byte, a second apply is refused, and revert gives the image back. */
	memcpy(plwork, plvf, obj_len);
	CHECK(gs_ada_edit(&f, plwork, obj_len, 0, fresh(), sizeof(err)) == 0 &&
	      plwork[f.site] == 0x00 && !memcmp(plwork, plvf, f.site) &&
	      !memcmp(plwork + f.site + 1, plvf + f.site + 1, obj_len - f.site - 1), "apply did not change exactly the immediate: %s", err);
	CHECK(placement_admit(r, plwork, obj_len, &f) == 0 && f.state == 1 && f.site == at + sizeof(placement_head),
	      "patched window not located in state 1: %s", err);
	{
		static const char none[] = "0000000000000000000000000000000000000000000000000000000000000000";
		struct gs_ada_revision row = *r;

		row.input_sha = none;
		row.output_sha = none;
		CHECK(refused(gs_ada_admit_in(&row, 1, plwork, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)), "already at output state"),
		      "a second apply was not refused: %s", err);
	}

	/* Two windows (a stock one and a second one, and one stock plus one patched): refused. */
	memcpy(plwork, plvf, obj_len);
	memcpy(plwork + dup, plvf + at, sizeof(placement_head) + 1 + sizeof(placement_tail));
	CHECK(refused(placement_admit(r, plwork, obj_len, &f), "unique: 2 windows"), "two stock windows were not refused: %s", err);
	plwork[dup + sizeof(placement_head)] = 0x00;
	CHECK(refused(placement_admit(r, plwork, obj_len, &f), "unique: 2 windows"), "a stock and a patched window were not refused: %s", err);

	/* No window: not found. */
	memset(plwork + at, 0x90, sizeof(placement_head) + 1 + sizeof(placement_tail));
	memset(plwork + dup, 0x90, sizeof(placement_head) + 1 + sizeof(placement_tail));
	CHECK(refused(placement_admit(r, plwork, obj_len, &f), "not found"), "a missing window was not reported as not found: %s", err);
	/* A changed byte inside the window is no window. */
	memcpy(plwork, plvf, obj_len);
	plwork[at + 3] ^= 0x10;
	CHECK(refused(placement_admit(r, plwork, obj_len, &f), "not found"), "a window with a changed head byte was not reported as not found: %s", err);

	/* The full table, not a one-row table: the placement window does not let the row jump the chain. An object that
	 * carries the placement window and still has an earlier row to apply is taken by the earlier row (table order), and an
	 * earlier row whose locator is refused blocks the chain; neither falls through to ada-placement. */
	obj_defaults();
	build_obj();
	memcpy(plwork, objimg, obj_len);
	memcpy(plwork + at, placement_head, sizeof(placement_head));
	plwork[at + sizeof(placement_head)] = 0x01;
	memcpy(plwork + at + sizeof(placement_head) + 1, placement_tail, sizeof(placement_tail));
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(plwork, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0 && f.row && !strcmp(f.row->name, "module-gate"),
	      "a stock object with the placement window was not taken by the gate row first: %s", err);
	obj_defaults();
	of.patched_gate = 1;
	build_obj();
	memcpy(plwork, objimg, obj_len);
	memcpy(plwork + at, placement_head, sizeof(placement_head));
	plwork[at + sizeof(placement_head)] = 0x01;
	memcpy(plwork + at + sizeof(placement_head) + 1, placement_tail, sizeof(placement_tail));
	memset(&f, 0, sizeof(f));
	CHECK(gs_ada_admit(plwork, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)) == 0 && f.row == vf && f.state == 0,
	      "a gated object with the placement window was not taken by vf-count first: %s", err);
	obj_defaults();
	of.patched_gate = 1;
	of.bad_load = 1;
	build_obj();
	memcpy(plwork, objimg, obj_len);
	memcpy(plwork + at, placement_head, sizeof(placement_head));
	plwork[at + sizeof(placement_head)] = 0x01;
	memcpy(plwork + at + sizeof(placement_head) + 1, placement_tail, sizeof(placement_tail));
	CHECK(refused(gs_ada_admit(plwork, obj_len, GS_ADA_FORWARD, &f, fresh(), sizeof(err)), "anchor ada.vf-count: totalvfs-load:"),
	      "a refused vf-count locator did not block the chain before ada-placement: %s", err);
}

static int file_sha(const char *path, char *hex, unsigned char **keep, size_t *keep_size)
{
	FILE *fp = fopen(path, "rb");
	unsigned char *buf;
	long end;

	if (!fp)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0 || (end = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	buf = malloc((size_t)end);
	if (!buf || fread(buf, 1, (size_t)end, fp) != (size_t)end) {
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	gs_sha256(buf, (size_t)end, SIZE_MAX, hex);
	if (keep) {
		*keep = buf;
		*keep_size = (size_t)end;
	} else {
		free(buf);
	}
	return 0;
}

/* The row on the vendor object: exactly one match, the byte changes 01 -> 00, and the whole chain
 * through the patcher (stock, scheduler, gate, vf-count, ada-placement) ends at the row's output digest;
 * a second apply is refused and revert gives the plain chain's object back. Needs the object and the built patcher. */
static void placement_vendor(void)
{
	const struct gs_ada_revision *r = shipped("ada-placement"), *vf = shipped("vf-count");
	const char *bin = getenv("GS_PATCH_BIN"), *tmp = getenv("TMPDIR");
	char dir[512], cmd[2048], out[4096], hex[65];
	unsigned char *obj = NULL, *cobj = NULL;
	size_t size = 0, csize = 0;
	struct gs_ada_found f;
	size_t i, hits;

	if (!bin || !*bin)
		bin = "driver/geforce-sriov-patch";
	if (!r || !vf || access(vendor_object, R_OK) != 0 || access(bin, X_OK) != 0) {
		printf("test_patch_ada: skipped: vendor object or patcher not present; ada-placement checks on the vendor object not run\n");
		return;
	}
	if (file_sha(vendor_object, hex, &obj, &size) != 0) {
		CHECK(0, "vendor object not read");
		return;
	}
	/* The locator finds exactly one window, in the stock state, at the recorded byte. */
	CHECK(!memcmp(obj + r->site - sizeof(placement_head), placement_head, sizeof(placement_head)) &&
	      obj[r->site] == 0x01 && !memcmp(obj + r->site + 1, placement_tail, sizeof(placement_tail)),
	      "the vendor object does not carry the window of ada-placement at 0x%zx", r->site);
	CHECK(placement_admit(r, obj, size, &f) == 0 && f.state == 0 && f.site == r->site && f.width == 1 && f.function &&
	      !strcmp(f.function, r->function) && f.function_offset == r->function_offset,
	      "ada-placement not located once at its recorded byte: %s", err);
	/* Counted independently over the whole object: the 24-byte window occurs once in the object file. */
	for (i = 0, hits = 0; i + 24 <= size; i++)
		if (obj[i] == placement_head[0] && !memcmp(obj + i, placement_head, 19) && obj[i + 19] == 0x01 &&
		    !memcmp(obj + i + 20, placement_tail, 4))
			hits++;
	CHECK(hits == 1, "the window of ada-placement occurs %zu times in the object file, want 1", hits);
	{
		/* Copies of the window in the real object: moved, duplicated, stock plus patched, removed. */
		size_t w = sizeof(placement_head) + 1 + sizeof(placement_tail), at = r->site - sizeof(placement_head);
		size_t dest = at + 0x40000;
		unsigned char save[24];

		memcpy(save, obj + at, w);
		memcpy(obj + dest, save, w);
		CHECK(refused(placement_admit(r, obj, size, &f), "unique: 2 windows"), "a duplicated window in the real object was not refused: %s", err);
		obj[dest + sizeof(placement_head)] = r->new_bytes[0];
		CHECK(refused(placement_admit(r, obj, size, &f), "unique: 2 windows"), "a stock and a patched window in the real object were not refused: %s", err);
		memset(obj + at, 0x90, w);
		obj[dest + sizeof(placement_head)] = r->old_bytes[0];
		CHECK(placement_admit(r, obj, size, &f) == 0 && f.state == 0 && f.site == dest + sizeof(placement_head),
		      "a moved window in the real object was not found where it went: %s", err);
		memset(obj + dest, 0x90, w);
		CHECK(refused(placement_admit(r, obj, size, &f), "not found"), "a removed window in the real object was not reported as not found: %s", err);
		memcpy(obj + at, save, w);
		memset(obj + dest, 0, w);
	}
	free(obj);

	snprintf(dir, sizeof(dir), "%s/gs-ada-c-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(0, "mkdtemp failed");
		return;
	}
	snprintf(cmd, sizeof(cmd),
		 "set -e; P=%s; D=%s; "
		 "$P apply %s $D/s && $P ada apply $D/s $D/g && $P ada apply $D/g $D/p && "
		 "$P ada apply $D/p $D/c 2>&1",
		 bin, dir, vendor_object);
	CHECK(run(cmd, out, sizeof(out)) == 0, "the chain through ada-placement failed: %s", out);
	CHECK(strstr(out, "revision=ada-placement") != NULL && strstr(out, "admission=digest") != NULL &&
	      strstr(out, "file_offset=0xd3a86") != NULL, "the patcher did not report the row by digest at 0xd3a86: %s", out);
	snprintf(cmd, sizeof(cmd), "%s/p", dir);
	CHECK(file_sha(cmd, hex, NULL, NULL) == 0 && !strcmp(hex, vf->output_sha), "the plain chain's object is not the recorded digest");
	snprintf(cmd, sizeof(cmd), "%s/c", dir);
	CHECK(file_sha(cmd, hex, &cobj, &csize) == 0 && !strcmp(hex, r->output_sha), "the output digest %s is not the recorded %s", hex, r->output_sha);
	if (cobj) {
		CHECK(csize == size && cobj[r->site] == 0x00 && !memcmp(cobj + r->site - 7, "\xc6\x84\x06\x3a\x4c\x13\x00", 7),
		      "the output does not carry the immediate 00 at 0x%zx", r->site);
		free(cobj);
	}
	printf("test_patch_ada: ada-placement output sha256 %s\n", r->output_sha);
	snprintf(cmd, sizeof(cmd), "%s ada apply %s/c %s/c2 2>&1", bin, dir, dir);
	CHECK(run(cmd, out, sizeof(out)) == 3 && strstr(out, "already at output state") != NULL, "a second apply was not refused: %s", out);
	snprintf(cmd, sizeof(cmd), "%s/c2", dir);
	CHECK(access(cmd, F_OK) != 0, "a refused second apply created an output file");
	/* The recorded intermediate states of the chain (scheduler, gate) are admitted by digest as inputs of their own next
	 * rows; the placement row is never taken early, and the result is no hybrid. */
	snprintf(cmd, sizeof(cmd), "%s ada apply %s/s %s/x1 2>&1", bin, dir, dir);
	CHECK(run(cmd, out, sizeof(out)) == 0 && strstr(out, "revision=module-gate") != NULL && strstr(out, "ada-placement") == NULL,
	      "the scheduler-only object was not taken by its own next row: %s", out);
	snprintf(cmd, sizeof(cmd), "%s ada apply %s/g %s/x2 2>&1", bin, dir, dir);
	CHECK(run(cmd, out, sizeof(out)) == 0 && strstr(out, "revision=vf-count") != NULL && strstr(out, "ada-placement") == NULL,
	      "the gate object was not taken by vf-count: %s", out);
	snprintf(cmd, sizeof(cmd), "%s/x2", dir);
	CHECK(file_sha(cmd, hex, NULL, NULL) == 0 && !strcmp(hex, vf->output_sha), "the gate object's apply is not the plain chain's object");
	snprintf(cmd, sizeof(cmd), "%s ada revert %s/c %s/r 2>&1", bin, dir, dir);
	CHECK(run(cmd, out, sizeof(out)) == 0, "revert of ada-placement failed: %s", out);
	snprintf(cmd, sizeof(cmd), "%s/r", dir);
	CHECK(file_sha(cmd, hex, NULL, NULL) == 0 && !strcmp(hex, vf->output_sha), "revert did not give the plain chain's object back");
	/* The object differs from the plain chain's in that one byte only. */
	snprintf(cmd, sizeof(cmd), "cmp -l %s/p %s/c | wc -l", dir, dir);
	CHECK(run(cmd, out, sizeof(out)) == 0 && atoi(out) == 1, "the object does not differ in exactly one byte: %s", out);
	/* The display revision on top of it is the recorded digest. */
	snprintf(cmd, sizeof(cmd), "%s display apply %s/c %s/d 2>&1", bin, dir, dir);
	CHECK(run(cmd, out, sizeof(out)) == 0 && strstr(out, "admission=digest") != NULL &&
	      strstr(out, "output_sha256=48fd6ddd2df13a1131a98facd4a80be1d6d17877eb900f3c945dfa483347627b") != NULL,
	      "display idx39 on the ada-placement object is not the recorded digest: %s", out);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	CHECK(run(cmd, out, sizeof(out)) == 0, "removing the temporary directory failed");
}


/* A row that cannot carry a window (too wide for the window buffer) has no locator; a function name does not matter. */
static void placement_bad_rows(void)
{
	static const char none[] = "0000000000000000000000000000000000000000000000000000000000000000";
	const struct gs_ada_revision *r = shipped("ada-placement");
	struct gs_ada_revision bad;
	struct gs_ada_found f;
	int k;

	CHECK(r != NULL, "shipped table lacks ada-placement");
	if (!r)
		return;
	obj_defaults();
	build_obj();
	for (k = 0; k < 3; k++) {
		bad = *r;
		bad.input_sha = bad.output_sha = none;
		if (k == 0)
			bad.width = 16;
		else if (k == 1)
			bad.function = NULL;
		else
			bad.function = "_nv_no_such_function";
		memset(&f, 0, sizeof(f));
		CHECK(refused(gs_ada_admit_in(&bad, 1, objimg, obj_len, GS_ADA_ANY, &f, fresh(), sizeof(err)),
			      k == 0 ? "anchor ada.unknown" : "anchor ada.ada-placement: window:"),
		      "placement row %d: a width that cannot carry the window has no locator, a function name does not matter: %s", k, err);
	}
}

/* Runs cmd through a shell and returns its exit status, with combined output in out. */
static int run(const char *cmd, char *out, size_t outlen)
{
	FILE *p;
	size_t n = 0;
	int status;

	out[0] = '\0';
	p = popen(cmd, "r");
	if (!p)
		return -1;
	while (n + 1 < outlen && fgets(out + n, (int)(outlen - n), p) != NULL)
		n += strlen(out + n);
	status = pclose(p);
	return status < 0 || !WIFEXITED(status) ? -1 : WEXITSTATUS(status);
}

/* The patcher binary is exercised only when it exists; make check builds it first. */
static void cli(void)
{
	const char *bin = getenv("GS_PATCH_BIN");
	char path[] = "/tmp/gs-ada-cli-XXXXXX", ppath[] = "/tmp/gs-ada-plugin-XXXXXX";
	char opath[] = "/tmp/gs-ada-object-XXXXXX";
	char dest[sizeof(ppath) + 8], cmd[1024], out[2048];
	unsigned char data[N];
	int fd;
	size_t i;

	if (!bin || !*bin)
		bin = "driver/geforce-sriov-patch";
	if (access(bin, X_OK) != 0) {
		printf("test_patch_ada: CLI checks skipped: %s is not built\n", bin);
		return;
	}
	fd = mkstemp(path);
	CHECK(fd >= 0, "mkstemp failed");
	if (fd < 0)
		return;
	for (i = 0; i < N; i++)
		data[i] = (unsigned char)(i * 37u + 0x5au);
	CHECK(write(fd, data, N) == (ssize_t)N, "writing the synthetic input failed");
	CHECK(close(fd) == 0, "closing the synthetic input failed");
	snprintf(dest, sizeof(dest), "%s.out", path);

	snprintf(cmd, sizeof(cmd), "%s ada 2>&1", bin);
	CHECK(run(cmd, out, sizeof(out)) == 2, "ada without a subcommand did not exit 2: %s", out);
	snprintf(cmd, sizeof(cmd), "%s ada inspect %s 2>&1", bin, path);
	CHECK(run(cmd, out, sizeof(out)) == 3, "ada inspect of an unknown digest did not exit 3: %s", out);
	CHECK(strstr(out, "geforce-sriov-patch: anchor ada.unknown: no recipe matches this input and no structural locator is implemented for it") != NULL,
	      "ada inspect did not name the anchor: %s", out);
	CHECK(strstr(out, "admission=") == NULL, "ada inspect printed a report for a refused input: %s", out);
	snprintf(cmd, sizeof(cmd), "%s ada apply %s %s 2>&1", bin, path, dest);
	CHECK(run(cmd, out, sizeof(out)) == 3, "ada apply of an unknown digest did not exit 3: %s", out);
	CHECK(access(dest, F_OK) != 0, "ada apply created an output file for a refused input");
	snprintf(cmd, sizeof(cmd), "%s ada inspect %s.missing 2>&1", bin, path);
	CHECK(run(cmd, out, sizeof(out)) == 1, "ada inspect of a missing file did not exit 1: %s", out);
	CHECK(unlink(path) == 0, "unlinking the synthetic input failed");

	/* A plugin-shaped file goes through the structural path end to end. */
	plugin_defaults();
	build_plugin();
	fd = mkstemp(ppath);
	CHECK(fd >= 0, "mkstemp plugin failed");
	if (fd < 0)
		return;
	CHECK(write(fd, img, img_len) == (ssize_t)img_len, "writing the synthetic plugin failed");
	CHECK(close(fd) == 0, "closing the synthetic plugin failed");
	snprintf(cmd, sizeof(cmd), "%s ada inspect %s 2>&1", bin, ppath);
	CHECK(run(cmd, out, sizeof(out)) == 0, "ada inspect of a synthetic plugin did not exit 0: %s", out);
	CHECK(strstr(out, "admission=anchor") != NULL, "inspect did not report anchor admission: %s", out);
	CHECK(strstr(out, "state=unconfirmed:unknown") != NULL, "inspect promoted an anchor admission: %s", out);
	CHECK(strstr(out, "row 4: plugin-mixed:") != NULL, "inspect did not name the placement row: %s", out);
	CHECK(strstr(out, "revision=plugin-mixed") != NULL, "inspect did not report the revision: %s", out);
	CHECK(strstr(out, "file_offset=0x240") != NULL, "inspect did not report the located file offset: %s", out);
	snprintf(dest, sizeof(dest), "%s.out", ppath);
	snprintf(cmd, sizeof(cmd), "%s ada apply %s %s 2>&1", bin, ppath, dest);
	CHECK(run(cmd, out, sizeof(out)) == 0, "ada apply of a synthetic plugin did not exit 0: %s", out);
	CHECK(access(dest, F_OK) == 0, "ada apply did not create the plugin output file");
	CHECK(unlink(dest) == 0, "unlinking the plugin output failed");
	CHECK(unlink(ppath) == 0, "unlinking the synthetic plugin failed");

	/* An object-shaped file goes through the structural path end to end. */
	obj_defaults();
	build_obj();
	fd = mkstemp(opath);
	CHECK(fd >= 0, "mkstemp object failed");
	if (fd < 0)
		return;
	CHECK(write(fd, objimg, obj_len) == (ssize_t)obj_len, "writing the synthetic object failed");
	CHECK(close(fd) == 0, "closing the synthetic object failed");
	snprintf(cmd, sizeof(cmd), "%s ada inspect %s 2>&1", bin, opath);
	CHECK(run(cmd, out, sizeof(out)) == 0, "ada inspect of a synthetic object did not exit 0: %s", out);
	CHECK(strstr(out, "row 1: module-gate:") != NULL, "inspect did not name the gate row: %s", out);
	CHECK(strstr(out, "admission=anchor") != NULL && strstr(out, "state=unconfirmed:unknown") != NULL,
	      "inspect did not report an unconfirmed anchor admission: %s", out);
	CHECK(strstr(out, "file_offset=0x247") != NULL, "inspect did not report the located gate offset: %s", out);
	CHECK(strstr(out, "function=rm_is_supported_device") != NULL && strstr(out, "function_offset=0x7") != NULL,
	      "inspect did not report the containing function: %s", out);
	snprintf(dest, sizeof(dest), "%s.out", opath);
	snprintf(cmd, sizeof(cmd), "%s ada apply %s %s 2>&1", bin, opath, dest);
	CHECK(run(cmd, out, sizeof(out)) == 0, "ada apply of a synthetic object did not exit 0: %s", out);
	CHECK(strstr(out, "row 3: vf-count:") != NULL || strstr(out, "row 1: module-gate:") != NULL,
	      "ada apply did not report the applied row: %s", out);
	CHECK(access(dest, F_OK) == 0, "ada apply did not create the object output file");
	CHECK(unlink(dest) == 0, "unlinking the object output failed");
	CHECK(unlink(opath) == 0, "unlinking the synthetic object failed");
}

int main(void)
{
	table_invariants();
	synthetic_row();
	plugin_chain();
	admission();
	plugin_locators();
	object_locators();
	placement_synthetic();
	placement_vendor();
	placement_bad_rows();
	cli();
	printf("test_patch_ada: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
