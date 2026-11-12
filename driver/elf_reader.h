// SPDX-License-Identifier: MIT
/*
 * Read-only, bounds-checked view of an ELF64 little-endian x86_64 ET_REL
 * object held in a caller-owned buffer. Nothing here allocates or mutates:
 * every pointer handed out points into the caller's buffer, which must stay
 * alive and unchanged while the view is used.
 *
 * Format reference: System V gABI "Object Files" chapter and the x86-64 psABI.
 * In a relocatable file st_value is an offset into the symbol's section
 * (sh_addr is not used), so a defined symbol's file bytes start at
 * sh_offset + st_value of its section.
 *
 * Lookup functions return GS_ELF_OK, GS_ELF_NOT_FOUND, GS_ELF_AMBIGUOUS or
 * GS_ELF_ERR (malformed input); every non-OK result writes a message to err
 * when err is non-NULL and errlen is non-zero.
 */
#ifndef GS_ELF_READER_H
#define GS_ELF_READER_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

enum {
	GS_ELF_ERR = -1,
	GS_ELF_OK = 0,
	GS_ELF_NOT_FOUND = 1,
	GS_ELF_AMBIGUOUS = 2,
};

struct gs_elf {
	const unsigned char *data;
	size_t size;
	Elf64_Ehdr ehdr;
	size_t shnum;    /* resolved section count (extended numbering supported) */
	size_t shstrndx; /* resolved section-name string table index */
	size_t symtab;   /* index of the single SHT_SYMTAB, 0 if the object has none */
	size_t symstr;   /* its string table (sh_link) */
	size_t symshndx; /* SHT_SYMTAB_SHNDX for symtab, 0 if none */
	size_t nsyms;    /* entries in symtab, including entry 0 */
};

struct gs_elf_sym {
	size_t index;          /* symbol table index */
	const char *name;      /* NUL-terminated, inside the buffer */
	unsigned char type;    /* ELF64_ST_TYPE */
	unsigned char bind;    /* ELF64_ST_BIND */
	uint16_t st_shndx;     /* raw st_shndx (SHN_UNDEF, SHN_ABS, SHN_COMMON, SHN_XINDEX, ...) */
	size_t shndx;          /* real containing section, 0 if undefined/ABS/COMMON */
	uint64_t value;        /* st_value: offset within shndx for ET_REL */
	uint64_t size;         /* st_size */
};

/* File-backed bytes of a defined symbol. */
struct gs_elf_range {
	size_t shndx;          /* containing section */
	uint64_t section_offset; /* == symbol value */
	size_t file_offset;    /* sh_offset + section_offset */
	size_t size;           /* st_size, non-zero */
	const unsigned char *bytes; /* data + file_offset */
};

struct gs_elf_rela {
	size_t section;        /* SHT_RELA section holding this entry */
	size_t index;          /* entry index in that section */
	uint64_t offset;       /* r_offset: offset within the target section */
	uint32_t type;         /* ELF64_R_TYPE (R_X86_64_*) */
	size_t sym;            /* ELF64_R_SYM */
	int64_t addend;        /* r_addend */
	unsigned char sym_type; /* STT_* of the target symbol, STT_NOTYPE for sym 0 */
	const char *sym_name;  /* symbol name; section name for STT_SECTION; "" for sym 0 */
};

/* Iterates every RELA entry that applies to one target section (sh_info). */
struct gs_elf_rela_iter {
	size_t target;
	size_t section;
	size_t index;
};

/*
 * Validate headers, the section table, string tables and the link/info fields
 * of SYMTAB, SYMTAB_SHNDX and RELA sections. Returns 0 or -1.
 */
int gs_elf_open(const unsigned char *data, size_t size, struct gs_elf *out, char *err, size_t errlen);

/*
 * Same validated view for a shared object (ET_DYN). The relocatable-object
 * rules that do not hold for a linked image are relaxed: a RELA section may
 * target no section (sh_info 0) and need not link the symbol table, because
 * the dynamic tables reference .dynsym. Everything else, including every
 * bounds check, is identical.
 */
int gs_elf_open_shared(const unsigned char *data, size_t size, struct gs_elf *out, char *err, size_t errlen);

/* Section header and name by index. Returns GS_ELF_OK or GS_ELF_ERR. */
int gs_elf_section(const struct gs_elf *e, size_t index, Elf64_Shdr *shdr, const char **name,
		   char *err, size_t errlen);

/* The single section with this exact name. */
int gs_elf_section_by_name(const struct gs_elf *e, const char *name, size_t *index, Elf64_Shdr *shdr,
			   char *err, size_t errlen);

/* File-backed contents of a section; SHT_NULL/SHT_NOBITS are refused. */
int gs_elf_section_data(const struct gs_elf *e, size_t index, const unsigned char **bytes, size_t *len,
			char *err, size_t errlen);

/* Symbol by table index (0 < index < nsyms). Returns GS_ELF_OK or GS_ELF_ERR. */
int gs_elf_symbol(const struct gs_elf *e, size_t index, struct gs_elf_sym *out, char *err, size_t errlen);

/*
 * Defined symbol with this exact name. If several defined symbols share the
 * name, the single STT_FUNC among them wins; otherwise GS_ELF_AMBIGUOUS.
 * Undefined references are ignored.
 */
int gs_elf_symbol_by_name(const struct gs_elf *e, const char *name, struct gs_elf_sym *out,
			  char *err, size_t errlen);

/*
 * File-backed byte range of a symbol defined in a real, non-NOBITS section,
 * with non-zero size lying wholly inside that section.
 */
int gs_elf_symbol_range(const struct gs_elf *e, const struct gs_elf_sym *sym, struct gs_elf_range *out,
			char *err, size_t errlen);

void gs_elf_rela_begin(struct gs_elf_rela_iter *it, size_t target_section);

/* Returns 1 with *out filled, 0 at end, GS_ELF_ERR on malformed input. */
int gs_elf_rela_next(const struct gs_elf *e, struct gs_elf_rela_iter *it, struct gs_elf_rela *out,
		     char *err, size_t errlen);

#endif
