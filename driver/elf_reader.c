// SPDX-License-Identifier: MIT
/* Bounds-checked read-only ELF64 LE x86_64 ET_REL view; see elf_reader.h. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "elf_reader.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "elf_reader copies ELF64LSB structures directly and needs a little-endian host"
#endif

static int fail(char *err, size_t errlen, int rc, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static int fail(char *err, size_t errlen, int rc, const char *fmt, ...)
{
	va_list ap;

	if (err && errlen) {
		va_start(ap, fmt);
		vsnprintf(err, errlen, fmt, ap);
		va_end(ap);
	}
	return rc;
}

/* [off, off + len) lies inside a buffer of size bytes, without overflow. */
static int in_bounds(size_t size, uint64_t off, uint64_t len)
{
	return off <= size && len <= size - off;
}

static void read_shdr(const struct gs_elf *e, size_t index, Elf64_Shdr *shdr)
{
	memcpy(shdr, e->data + e->ehdr.e_shoff + index * sizeof(*shdr), sizeof(*shdr));
}

/* String at off in a validated SHT_STRTAB (its last byte is NUL). */
static const char *str_at(const struct gs_elf *e, size_t strsec, uint64_t off)
{
	Elf64_Shdr sh;

	read_shdr(e, strsec, &sh);
	if (off >= sh.sh_size)
		return NULL;
	return (const char *)e->data + sh.sh_offset + off;
}

static int check_strtab(const struct gs_elf *e, size_t index, const Elf64_Shdr *sh, char *err, size_t errlen)
{
	if (sh->sh_type != SHT_STRTAB)
		return fail(err, errlen, -1, "section %zu is not a string table", index);
	if (sh->sh_size == 0 || e->data[sh->sh_offset + sh->sh_size - 1] != '\0')
		return fail(err, errlen, -1, "string table section %zu is empty or not NUL-terminated", index);
	return 0;
}

static int check_sections(struct gs_elf *e, int shared, char *err, size_t errlen)
{
	Elf64_Shdr sh;
	size_t i;

	for (i = 0; i < e->shnum; i++) {
		read_shdr(e, i, &sh);
		if (i == 0) {
			if (sh.sh_type != SHT_NULL)
				return fail(err, errlen, -1, "section 0 is not SHT_NULL");
			continue;
		}
		if (sh.sh_type != SHT_NOBITS && sh.sh_type != SHT_NULL &&
		    !in_bounds(e->size, sh.sh_offset, sh.sh_size))
			return fail(err, errlen, -1, "section %zu data [0x%llx, +0x%llx) exceeds file size 0x%zx", i,
				    (unsigned long long)sh.sh_offset, (unsigned long long)sh.sh_size, e->size);
		if (sh.sh_link >= e->shnum)
			return fail(err, errlen, -1, "section %zu sh_link %u out of range", i, sh.sh_link);
		if (sh.sh_type == SHT_REL)
			return fail(err, errlen, -1, "section %zu is SHT_REL; x86_64 objects use SHT_RELA only", i);
		if (sh.sh_type == SHT_SYMTAB) {
			if (e->symtab)
				return fail(err, errlen, -1, "more than one SHT_SYMTAB (sections %zu and %zu)",
					    e->symtab, i);
			if (sh.sh_entsize != sizeof(Elf64_Sym) || sh.sh_size % sizeof(Elf64_Sym) || sh.sh_size == 0)
				return fail(err, errlen, -1, "symbol table section %zu has bad entsize/size", i);
			e->symtab = i;
			e->symstr = sh.sh_link;
			e->nsyms = sh.sh_size / sizeof(Elf64_Sym);
			if (sh.sh_info > e->nsyms)
				return fail(err, errlen, -1, "symbol table sh_info %u exceeds %zu entries", sh.sh_info,
					    e->nsyms);
		}
	}

	if (e->symtab) {
		read_shdr(e, e->symstr, &sh);
		if (e->symstr == 0 || check_strtab(e, e->symstr, &sh, err, errlen))
			return fail(err, errlen, -1, "symbol table %zu links to invalid string table %zu", e->symtab,
				    e->symstr);
	}

	for (i = 1; i < e->shnum; i++) {
		read_shdr(e, i, &sh);
		if (sh.sh_type == SHT_RELA) {
			/* A shared object's dynamic relocation tables target no section
			 * (sh_info 0) and reference .dynsym, not the symbol table. */
			if (!shared && (!e->symtab || sh.sh_link != e->symtab))
				return fail(err, errlen, -1, "relocation section %zu sh_link %u is not the symbol table",
					    i, sh.sh_link);
			if (sh.sh_entsize != sizeof(Elf64_Rela) || sh.sh_size % sizeof(Elf64_Rela))
				return fail(err, errlen, -1, "relocation section %zu has bad entsize/size", i);
			if (sh.sh_info >= e->shnum || (!shared && sh.sh_info == 0))
				return fail(err, errlen, -1, "relocation section %zu targets bad section %u", i,
					    sh.sh_info);
		} else if (sh.sh_type == SHT_SYMTAB_SHNDX) {
			if (!e->symtab || sh.sh_link != e->symtab)
				return fail(err, errlen, -1, "SHT_SYMTAB_SHNDX section %zu does not link the symbol table",
					    i);
			if (e->symshndx)
				return fail(err, errlen, -1, "more than one SHT_SYMTAB_SHNDX section");
			if (sh.sh_entsize != sizeof(Elf32_Word) || sh.sh_size != e->nsyms * sizeof(Elf32_Word))
				return fail(err, errlen, -1, "SHT_SYMTAB_SHNDX section %zu size does not match symbols", i);
			e->symshndx = i;
		}
	}
	return 0;
}

static int open_common(const unsigned char *data, size_t size, uint16_t type, const char *kind,
		       struct gs_elf *out, char *err, size_t errlen)
{
	struct gs_elf e = { .data = data, .size = size };
	Elf64_Ehdr *h = &e.ehdr;
	Elf64_Shdr sh0, sh;

	if (!data || size < sizeof(*h))
		return fail(err, errlen, -1, "file is %zu bytes, shorter than an ELF64 header", size);
	memcpy(h, data, sizeof(*h));
	if (memcmp(h->e_ident, ELFMAG, SELFMAG) != 0)
		return fail(err, errlen, -1, "not an ELF file");
	if (h->e_ident[EI_CLASS] != ELFCLASS64)
		return fail(err, errlen, -1, "not ELF64");
	if (h->e_ident[EI_DATA] != ELFDATA2LSB)
		return fail(err, errlen, -1, "not little-endian ELF");
	if (h->e_ident[EI_VERSION] != EV_CURRENT || h->e_version != EV_CURRENT)
		return fail(err, errlen, -1, "unsupported ELF version");
	if (h->e_type != type)
		return fail(err, errlen, -1, "not a %s (e_type %u)", kind, h->e_type);
	if (h->e_machine != EM_X86_64)
		return fail(err, errlen, -1, "not x86_64 (e_machine %u)", h->e_machine);
	if (h->e_ehsize != sizeof(*h) || h->e_shentsize != sizeof(Elf64_Shdr))
		return fail(err, errlen, -1, "unexpected ELF header or section header entry size");
	if (h->e_shoff == 0 || !in_bounds(size, h->e_shoff, sizeof(sh0)))
		return fail(err, errlen, -1, "section header table offset 0x%llx out of range",
			    (unsigned long long)h->e_shoff);

	/* gABI extended numbering: real counts live in section 0 when the header fields overflow. */
	memcpy(&sh0, data + h->e_shoff, sizeof(sh0));
	e.shnum = h->e_shnum ? h->e_shnum : sh0.sh_size;
	if (h->e_shnum == 0 && sh0.sh_size > (size - h->e_shoff) / sizeof(sh0))
		return fail(err, errlen, -1, "extended section count 0x%llx exceeds file",
			    (unsigned long long)sh0.sh_size);
	if (e.shnum < 2)
		return fail(err, errlen, -1, "object has no sections");
	if (e.shnum > (size - h->e_shoff) / sizeof(sh0))
		return fail(err, errlen, -1, "section header table (%zu entries) truncated", e.shnum);
	if (h->e_shstrndx == SHN_XINDEX)
		e.shstrndx = sh0.sh_link;
	else if (h->e_shstrndx == SHN_UNDEF || h->e_shstrndx >= SHN_LORESERVE)
		return fail(err, errlen, -1, "object has no section name string table");
	else
		e.shstrndx = h->e_shstrndx;
	if (e.shstrndx == 0 || e.shstrndx >= e.shnum)
		return fail(err, errlen, -1, "section name string table index %zu out of range", e.shstrndx);

	if (check_sections(&e, type == ET_DYN, err, errlen))
		return -1;
	read_shdr(&e, e.shstrndx, &sh);
	if (check_strtab(&e, e.shstrndx, &sh, err, errlen))
		return -1;
	for (size_t i = 1; i < e.shnum; i++) {
		Elf64_Shdr s;

		read_shdr(&e, i, &s);
		if (s.sh_name >= sh.sh_size)
			return fail(err, errlen, -1, "section %zu name offset out of range", i);
	}
	*out = e;
	return 0;
}

int gs_elf_open(const unsigned char *data, size_t size, struct gs_elf *out, char *err, size_t errlen)
{
	return open_common(data, size, ET_REL, "relocatable object", out, err, errlen);
}

int gs_elf_open_shared(const unsigned char *data, size_t size, struct gs_elf *out, char *err, size_t errlen)
{
	return open_common(data, size, ET_DYN, "shared object", out, err, errlen);
}

int gs_elf_section(const struct gs_elf *e, size_t index, Elf64_Shdr *shdr, const char **name,
		   char *err, size_t errlen)
{
	Elf64_Shdr sh;

	if (index == 0 || index >= e->shnum)
		return fail(err, errlen, GS_ELF_ERR, "section index %zu out of range", index);
	read_shdr(e, index, &sh);
	if (shdr)
		*shdr = sh;
	if (name)
		*name = str_at(e, e->shstrndx, sh.sh_name);
	return GS_ELF_OK;
}

int gs_elf_section_by_name(const struct gs_elf *e, const char *name, size_t *index, Elf64_Shdr *shdr,
			   char *err, size_t errlen)
{
	size_t i, found = 0;

	for (i = 1; i < e->shnum; i++) {
		Elf64_Shdr sh;

		read_shdr(e, i, &sh);
		if (strcmp(str_at(e, e->shstrndx, sh.sh_name), name) != 0)
			continue;
		if (found)
			return fail(err, errlen, GS_ELF_AMBIGUOUS, "section %s appears more than once (%zu and %zu)",
				    name, found, i);
		found = i;
		if (shdr)
			*shdr = sh;
	}
	if (!found)
		return fail(err, errlen, GS_ELF_NOT_FOUND, "no section named %s", name);
	if (index)
		*index = found;
	return GS_ELF_OK;
}

int gs_elf_section_data(const struct gs_elf *e, size_t index, const unsigned char **bytes, size_t *len,
			char *err, size_t errlen)
{
	Elf64_Shdr sh = { 0 };

	if (gs_elf_section(e, index, &sh, NULL, err, errlen))
		return GS_ELF_ERR;
	if (sh.sh_type == SHT_NULL || sh.sh_type == SHT_NOBITS)
		return fail(err, errlen, GS_ELF_ERR, "section %zu has no file-backed contents", index);
	*bytes = e->data + sh.sh_offset;
	*len = sh.sh_size;
	return GS_ELF_OK;
}

int gs_elf_symbol(const struct gs_elf *e, size_t index, struct gs_elf_sym *out, char *err, size_t errlen)
{
	Elf64_Shdr symsh;
	Elf64_Sym s;

	if (!e->symtab)
		return fail(err, errlen, GS_ELF_ERR, "object has no symbol table");
	if (index == 0 || index >= e->nsyms)
		return fail(err, errlen, GS_ELF_ERR, "symbol index %zu out of range", index);
	read_shdr(e, e->symtab, &symsh);
	memcpy(&s, e->data + symsh.sh_offset + index * sizeof(s), sizeof(s));

	out->index = index;
	out->name = str_at(e, e->symstr, s.st_name);
	if (!out->name)
		return fail(err, errlen, GS_ELF_ERR, "symbol %zu name offset out of range", index);
	out->type = ELF64_ST_TYPE(s.st_info);
	out->bind = ELF64_ST_BIND(s.st_info);
	out->st_shndx = s.st_shndx;
	out->value = s.st_value;
	out->size = s.st_size;
	out->shndx = 0;
	if (s.st_shndx == SHN_XINDEX) {
		Elf64_Shdr xsh;
		Elf32_Word x;

		if (!e->symshndx)
			return fail(err, errlen, GS_ELF_ERR, "symbol %zu uses SHN_XINDEX without SHT_SYMTAB_SHNDX",
				    index);
		read_shdr(e, e->symshndx, &xsh);
		memcpy(&x, e->data + xsh.sh_offset + index * sizeof(x), sizeof(x));
		if (x == 0 || x >= e->shnum)
			return fail(err, errlen, GS_ELF_ERR, "symbol %zu extended section index %u out of range", index,
				    x);
		out->shndx = x;
	} else if (s.st_shndx != SHN_UNDEF && s.st_shndx < SHN_LORESERVE) {
		if (s.st_shndx >= e->shnum)
			return fail(err, errlen, GS_ELF_ERR, "symbol %zu section index %u out of range", index,
				    s.st_shndx);
		out->shndx = s.st_shndx;
	} else if (s.st_shndx != SHN_UNDEF && s.st_shndx != SHN_ABS && s.st_shndx != SHN_COMMON) {
		return fail(err, errlen, GS_ELF_ERR, "symbol %zu has unsupported reserved section index 0x%x", index,
			    s.st_shndx);
	}
	return GS_ELF_OK;
}

int gs_elf_symbol_by_name(const struct gs_elf *e, const char *name, struct gs_elf_sym *out,
			  char *err, size_t errlen)
{
	struct gs_elf_sym s, any = { 0 }, func = { 0 };
	size_t i, nany = 0, nfunc = 0;

	if (!e->symtab)
		return fail(err, errlen, GS_ELF_NOT_FOUND, "object has no symbol table");
	for (i = 1; i < e->nsyms; i++) {
		if (gs_elf_symbol(e, i, &s, err, errlen))
			return GS_ELF_ERR;
		if (s.st_shndx == SHN_UNDEF || strcmp(s.name, name) != 0)
			continue;
		nany++;
		any = s;
		if (s.type == STT_FUNC) {
			nfunc++;
			func = s;
		}
	}
	if (nfunc == 1) {
		*out = func;
		return GS_ELF_OK;
	}
	if (nfunc > 1)
		return fail(err, errlen, GS_ELF_AMBIGUOUS, "%zu STT_FUNC symbols named %s", nfunc, name);
	if (nany == 1) {
		*out = any;
		return GS_ELF_OK;
	}
	if (nany > 1)
		return fail(err, errlen, GS_ELF_AMBIGUOUS, "%zu non-function symbols named %s", nany, name);
	return fail(err, errlen, GS_ELF_NOT_FOUND, "no defined symbol named %s", name);
}

int gs_elf_symbol_range(const struct gs_elf *e, const struct gs_elf_sym *sym, struct gs_elf_range *out,
			char *err, size_t errlen)
{
	Elf64_Shdr sh;

	if (sym->shndx == 0 || sym->shndx >= e->shnum)
		return fail(err, errlen, GS_ELF_ERR, "symbol %s is not defined in a section", sym->name);
	read_shdr(e, sym->shndx, &sh);
	if (sh.sh_type == SHT_NOBITS || sh.sh_type == SHT_NULL)
		return fail(err, errlen, GS_ELF_ERR, "symbol %s lives in a section without file contents", sym->name);
	if (sym->size == 0)
		return fail(err, errlen, GS_ELF_ERR, "symbol %s has zero size", sym->name);
	if (!in_bounds(sh.sh_size, sym->value, sym->size))
		return fail(err, errlen, GS_ELF_ERR, "symbol %s [0x%llx, +0x%llx) exceeds its section (0x%llx bytes)",
			    sym->name, (unsigned long long)sym->value, (unsigned long long)sym->size,
			    (unsigned long long)sh.sh_size);
	/* Section data was bounds-checked at open, so the sum cannot overflow or leave the buffer. */
	out->shndx = sym->shndx;
	out->section_offset = sym->value;
	out->file_offset = sh.sh_offset + sym->value;
	out->size = sym->size;
	out->bytes = e->data + out->file_offset;
	return GS_ELF_OK;
}

void gs_elf_rela_begin(struct gs_elf_rela_iter *it, size_t target_section)
{
	it->target = target_section;
	it->section = 1;
	it->index = 0;
}

int gs_elf_rela_next(const struct gs_elf *e, struct gs_elf_rela_iter *it, struct gs_elf_rela *out,
		     char *err, size_t errlen)
{
	Elf64_Shdr sh, target;
	Elf64_Rela r;
	struct gs_elf_sym s;

	if (it->target == 0 || it->target >= e->shnum)
		return fail(err, errlen, GS_ELF_ERR, "relocation target section %zu out of range", it->target);
	read_shdr(e, it->target, &target);
	for (; it->section < e->shnum; it->section++, it->index = 0) {
		read_shdr(e, it->section, &sh);
		if (sh.sh_type != SHT_RELA || sh.sh_info != it->target)
			continue;
		if (it->index < sh.sh_size / sizeof(r))
			break;
	}
	if (it->section >= e->shnum)
		return 0;

	memcpy(&r, e->data + sh.sh_offset + it->index * sizeof(r), sizeof(r));
	out->section = it->section;
	out->index = it->index++;
	out->offset = r.r_offset;
	out->type = ELF64_R_TYPE(r.r_info);
	out->sym = ELF64_R_SYM(r.r_info);
	out->addend = r.r_addend;
	if (r.r_offset >= target.sh_size)
		return fail(err, errlen, GS_ELF_ERR, "relocation %zu in section %zu offset 0x%llx outside target",
			    out->index, out->section, (unsigned long long)r.r_offset);
	if (out->sym == 0) {
		out->sym_type = STT_NOTYPE;
		out->sym_name = "";
		return 1;
	}
	if (gs_elf_symbol(e, out->sym, &s, err, errlen))
		return GS_ELF_ERR;
	out->sym_type = s.type;
	out->sym_name = s.name;
	if (s.type == STT_SECTION) {
		if (s.shndx == 0)
			return fail(err, errlen, GS_ELF_ERR, "section symbol %zu has no section", out->sym);
		gs_elf_section(e, s.shndx, NULL, &out->sym_name, NULL, 0);
	}
	return 1;
}
