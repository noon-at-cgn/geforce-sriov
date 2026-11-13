/* SPDX-License-Identifier: MIT */
/*
 * gs_insn_find_unique() on relocatable objects built here in memory: a small
 * hand-assembled .text, a few function symbols and one or two relocations.
 */
#include "elf_reader.h"
#include "insn_find.h"
#include <elf.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define TEXT_LEN 48
#define IMG_MAX 2048

struct fn {
	const char *name;
	unsigned off, size;
};

struct rel {
	unsigned off;
	unsigned sym; /* 0: ext_a, 1: ext_b (undefined globals) */
};

/* Section order: 0 null, 1 .text, 2 .rela.text, 3 .symtab, 4 .strtab, 5 .shstrtab. */
static const char shstr[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab";
#define SH_TEXT 1
#define SH_RELA 7
#define SH_SYMTAB 18
#define SH_STRTAB 26
#define SH_SHSTR 34

#define NEXT 2

static size_t build(unsigned char *img, const unsigned char *text, const struct fn *fns, size_t nfn,
		    const struct rel *rels, size_t nrel)
{
	static const char *ext[NEXT] = { "ext_a", "ext_b" };
	char strtab[256] = { 0 };
	size_t stlen = 1, i, off = sizeof(Elf64_Ehdr);
	Elf64_Sym syms[16] = { { 0 } };
	Elf64_Rela relas[4];
	Elf64_Shdr sh[6] = { { 0 } };
	Elf64_Ehdr eh = { 0 };
	size_t nsym = 1 + nfn + NEXT;
	size_t text_off, rela_off, sym_off, str_off, shs_off, sht_off;

	memset(img, 0, IMG_MAX);
	for (i = 0; i < nfn; i++) {
		syms[1 + i].st_name = stlen;
		syms[1 + i].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
		syms[1 + i].st_shndx = 1;
		syms[1 + i].st_value = fns[i].off;
		syms[1 + i].st_size = fns[i].size;
		strcpy(strtab + stlen, fns[i].name);
		stlen += strlen(fns[i].name) + 1;
	}
	for (i = 0; i < NEXT; i++) {
		syms[1 + nfn + i].st_name = stlen;
		syms[1 + nfn + i].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
		strcpy(strtab + stlen, ext[i]);
		stlen += strlen(ext[i]) + 1;
	}
	for (i = 0; i < nrel; i++) {
		relas[i].r_offset = rels[i].off;
		relas[i].r_info = ELF64_R_INFO(1 + nfn + rels[i].sym, R_X86_64_PLT32);
		relas[i].r_addend = -4;
	}

	text_off = off;
	memcpy(img + off, text, TEXT_LEN);
	off += TEXT_LEN;
	rela_off = off;
	memcpy(img + off, relas, nrel * sizeof(relas[0]));
	off += nrel * sizeof(relas[0]);
	sym_off = off;
	memcpy(img + off, syms, nsym * sizeof(syms[0]));
	off += nsym * sizeof(syms[0]);
	str_off = off;
	memcpy(img + off, strtab, stlen);
	off += stlen;
	shs_off = off;
	memcpy(img + off, shstr, sizeof(shstr));
	off += sizeof(shstr);
	off = (off + 7) & ~(size_t)7;
	sht_off = off;

	sh[1] = (Elf64_Shdr){ .sh_name = SH_TEXT, .sh_type = SHT_PROGBITS, .sh_flags = SHF_ALLOC | SHF_EXECINSTR,
			      .sh_offset = text_off, .sh_size = TEXT_LEN, .sh_addralign = 16 };
	sh[2] = (Elf64_Shdr){ .sh_name = SH_RELA, .sh_type = SHT_RELA, .sh_link = 3, .sh_info = 1,
			      .sh_offset = rela_off, .sh_size = nrel * sizeof(Elf64_Rela),
			      .sh_entsize = sizeof(Elf64_Rela), .sh_addralign = 8 };
	sh[3] = (Elf64_Shdr){ .sh_name = SH_SYMTAB, .sh_type = SHT_SYMTAB, .sh_link = 4, .sh_info = 1,
			      .sh_offset = sym_off, .sh_size = nsym * sizeof(Elf64_Sym),
			      .sh_entsize = sizeof(Elf64_Sym), .sh_addralign = 8 };
	sh[4] = (Elf64_Shdr){ .sh_name = SH_STRTAB, .sh_type = SHT_STRTAB, .sh_offset = str_off, .sh_size = stlen,
			      .sh_addralign = 1 };
	sh[5] = (Elf64_Shdr){ .sh_name = SH_SHSTR, .sh_type = SHT_STRTAB, .sh_offset = shs_off,
			      .sh_size = sizeof(shstr), .sh_addralign = 1 };
	memcpy(img + sht_off, sh, sizeof(sh));

	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_ident[EI_VERSION] = EV_CURRENT;
	eh.e_type = ET_REL;
	eh.e_machine = EM_X86_64;
	eh.e_version = EV_CURRENT;
	eh.e_shoff = sht_off;
	eh.e_ehsize = sizeof(eh);
	eh.e_shentsize = sizeof(Elf64_Shdr);
	eh.e_shnum = 6;
	eh.e_shstrndx = 5;
	memcpy(img, &eh, sizeof(eh));
	return sht_off + sizeof(sh);
}

/* Pattern: mov eax, imm32 with a wildcard immediate; mask leaves the immediate free. */
static const unsigned char PAT[5] = { 0xb8, 0x00, 0x00, 0x00, 0x00 };
static const unsigned char MASK[5] = { 0xff, 0x00, 0x00, 0x00, 0x00 };
static const unsigned char INS[5] = { 0xb8, 0x11, 0x22, 0x33, 0x44 };

/* Three functions of 16 bytes at 0, 16 and 32, filled with int3. */
static const struct fn FUNCS[3] = { { "fa", 0, 16 }, { "fb", 16, 16 }, { "fc", 32, 16 } };

static void fresh_text(unsigned char *t)
{
	memset(t, 0xcc, TEXT_LEN);
}

static int open_obj(unsigned char *img, const unsigned char *text, const struct rel *rels, size_t nrel,
		    struct gs_elf *e)
{
	char err[256];
	size_t n = build(img, text, FUNCS, 3, rels, nrel);
	int rc = gs_elf_open(img, n, e, err, sizeof(err));

	if (rc)
		fprintf(stderr, "synthetic object does not open: %s\n", err);
	return rc;
}

static int find(const struct gs_elf *e, enum gs_insn_scope_kind kind, const char *sym, struct gs_insn_match *m,
		char *err, size_t errlen)
{
	struct gs_insn_scope sc = { kind, sym };

	return gs_insn_find_unique(e, PAT, MASK, sizeof(PAT), &sc, m, err, errlen);
}

int main(void)
{
	unsigned char img[IMG_MAX], text[TEXT_LEN];
	struct gs_elf e;
	struct gs_insn_match m;
	char err[256];
	const struct rel rb[1] = { { 20, 0 } }; /* inside fb, against ext_a */
	const struct rel rbc[2] = { { 20, 0 }, { 36, 1 } };
	size_t text_file = 0;
	int rc;

	/* One match, in fb at +3; the wildcard immediate is ignored by the mask. */
	fresh_text(text);
	memcpy(text + 16 + 3, INS, 5);
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	{
		Elf64_Shdr sh;

		CHECK(gs_elf_section(&e, 1, &sh, NULL, NULL, 0) == GS_ELF_OK);
		text_file = sh.sh_offset;
	}
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fb", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_FOUND);
	CHECK(m.count == 1 && strcmp(m.func_name, "fb") == 0 && m.func_start == 16 && m.match_offset == 3);
	CHECK(m.file_offset == text_file + 19);
	CHECK(memcmp(img + m.file_offset, INS, sizeof(INS)) == 0);

	/* The same bytes are outside the scope of fa and fc: no match there. */
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fa", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_NONE && m.count == 0);
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fc", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_NONE && m.count == 0);

	/* A mask byte that is not a wildcard must match: a different opcode does not. */
	fresh_text(text);
	text[16 + 3] = 0xb9;
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fb", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_NONE);

	/* Zero matches in an object without the pattern at all. */
	fresh_text(text);
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	CHECK(find(&e, GS_INSN_SCOPE_SYMBOL, "fb", &m, err, sizeof(err)) == GS_INSN_NONE && m.count == 0);

	/* Two matches in one function. */
	fresh_text(text);
	memcpy(text + 16 + 0, INS, 5);
	memcpy(text + 16 + 9, INS, 5);
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fb", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_MULTIPLE && m.count == 2);

	/* Two matches in different functions: each symbol scope finds exactly its function. */
	fresh_text(text);
	memcpy(text + 16 + 2, INS, 5);
	memcpy(text + 32 + 6, INS, 5);
	CHECK(open_obj(img, text, rbc, 2, &e) == 0);
	rc = gs_insn_find_unique(&e, PAT, MASK, sizeof(PAT), &(struct gs_insn_scope){ GS_INSN_SCOPE_SYMBOL, "fb" },
				 &m, err, sizeof(err));
	CHECK(rc == GS_INSN_FOUND && m.match_offset == 2);
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fc", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_FOUND && strcmp(m.func_name, "fc") == 0 && m.func_start == 32 && m.match_offset == 6);
	CHECK(m.file_offset == text_file + 38);

	/* A scope symbol that does not exist is an error, not "no match". */
	rc = find(&e, GS_INSN_SCOPE_SYMBOL, "no_such_symbol", &m, err, sizeof(err));
	CHECK(rc == GS_INSN_ERR && strstr(err, "no_such_symbol"));

	/* The same bytes at another offset are still found, and the reported offsets follow. */
	for (unsigned shift = 0; shift + 5 <= 16; shift += 4) {
		fresh_text(text);
		memcpy(text + 16 + shift, INS, 5);
		CHECK(open_obj(img, text, rb, 1, &e) == 0);
		rc = find(&e, GS_INSN_SCOPE_SYMBOL, "fb", &m, err, sizeof(err));
		CHECK(rc == GS_INSN_FOUND && m.match_offset == shift && m.file_offset == text_file + 16 + shift);
	}

	/* A match must lie wholly inside the function: one straddling fb and fc is not a match. */
	fresh_text(text);
	memcpy(text + 16 + 13, INS, 5);
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	CHECK(find(&e, GS_INSN_SCOPE_SYMBOL, "fb", &m, err, sizeof(err)) == GS_INSN_NONE);

	/* An empty pattern is refused. */
	{
		struct gs_insn_scope sc = { GS_INSN_SCOPE_SYMBOL, "fb" };

		CHECK(gs_insn_find_unique(&e, PAT, MASK, 0, &sc, &m, err, sizeof(err)) == GS_INSN_ERR);
	}

	/* Whole-.text scope: a match straddling two functions is found, named after the function it starts in. */
	rc = find(&e, GS_INSN_SCOPE_TEXT, NULL, &m, err, sizeof(err));
	CHECK(rc == GS_INSN_FOUND && m.count == 1 && strcmp(m.func_name, "fb") == 0 && m.func_start == 16 &&
	      m.match_offset == 13 && m.file_offset == text_file + 29);
	/* One in each of two functions: two matches. */
	fresh_text(text);
	memcpy(text + 2, INS, 5);
	memcpy(text + 32 + 6, INS, 5);
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	rc = find(&e, GS_INSN_SCOPE_TEXT, NULL, &m, err, sizeof(err));
	CHECK(rc == GS_INSN_MULTIPLE && m.count == 2);
	/* No match at all. */
	fresh_text(text);
	CHECK(open_obj(img, text, rb, 1, &e) == 0);
	rc = find(&e, GS_INSN_SCOPE_TEXT, NULL, &m, err, sizeof(err));
	CHECK(rc == GS_INSN_NONE && m.count == 0);
	/* A match in bytes that no function symbol covers: found, with no function name. */
	fresh_text(text);
	memcpy(text + 40, INS, 5);
	{
		size_t n = build(img, text, FUNCS, 2, rb, 1);

		CHECK(gs_elf_open(img, n, &e, err, sizeof(err)) == 0);
		rc = find(&e, GS_INSN_SCOPE_TEXT, NULL, &m, err, sizeof(err));
		CHECK(rc == GS_INSN_FOUND && m.func_name == NULL && m.match_offset == 40 && m.file_offset == text_file + 40);
	}

	printf("test_insn_find: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
