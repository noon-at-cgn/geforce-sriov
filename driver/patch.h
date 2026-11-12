/* SPDX-License-Identifier: MIT */
#ifndef GS_PATCH_H
#define GS_PATCH_H
#include <stddef.h>
struct gs_patch_info { size_t offset; size_t function_offset; int patched; const char *version; const char *function; int measured; int digest; };
/* The structural locator is also used for unconfirmed anchor admission. */
int gs_patch_structure(const unsigned char *, size_t, const char *, struct gs_patch_info *, char *, size_t);
int gs_patch_inspect(const unsigned char *, size_t, struct gs_patch_info *, char *, size_t);
int gs_patch_revert_check(const unsigned char *, size_t, const unsigned char *, size_t, char *, size_t);
/* Exact byte relationship only; does not admit either object for output. */
int gs_patch_exact_pair(const unsigned char *, size_t, const unsigned char *, size_t, size_t, char *, size_t);
void gs_sha256(const unsigned char *, size_t, size_t, char [65]);
#endif
