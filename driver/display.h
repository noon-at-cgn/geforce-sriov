/* SPDX-License-Identifier: MIT */
#ifndef GS_DISPLAY_H
#define GS_DISPLAY_H
#include <stddef.h>
#include <stdint.h>
struct gs_display_recipe {
 const char *name, *input_sha, *output_sha;
 size_t function_offset;
};
struct gs_display_info {
 const struct gs_display_recipe *recipe;
 int digest, confirmed, patched;
 size_t file_offset, function_offset, rela_section, rela_index, rela_file_offset;
 uint64_t rela_offset;
 char rela_name[128];
 char input_sha[65], output_sha[65];
};
const struct gs_display_recipe *gs_display_recipes(size_t *count);
int gs_display_inspect_in(const unsigned char *, size_t, const struct gs_display_recipe *, size_t, struct gs_display_info *, char *, size_t);
int gs_display_inspect(const unsigned char *, size_t, struct gs_display_info *, char *, size_t);
int gs_display_transform(unsigned char *, size_t, struct gs_display_info *, int, char *, size_t);
#endif
