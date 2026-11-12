/* SPDX-License-Identifier: MIT */
/*
 * Synthetic module ABI fixtures. The kernel module loader reads export name
 * offsets through .rela__ksymtab and CRCs through __kcrctab data; the
 * __crc_<sym> symbol value is only a section offset, so fixtures give it a
 * bogus value on purpose to catch a checker that compares it.
 */
#define _POSIX_C_SOURCE 200809L
#include "modcheck.h"
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)
#define CHECK_STR(hay, needle) do { checks++; if (!(hay) || !strstr((hay), (needle))) { failures++; fprintf(stderr, "FAIL line %d: missing [%s] in: %s\n", __LINE__, (needle), (hay) ? (hay) : "(null)"); } } while (0)

#define SECTIONS 12
static const char shnames[] =
	"\0.shstrtab\0.strtab\0.symtab\0.modinfo\0__versions\0__ksymtab\0__ksymtab_gpl\0__kcrctab"
	"\0__kcrctab_gpl\0__ksymtab_strings\0.rela__ksymtab\0.rela__ksymtab_gpl\0__version_ext_names"
	"\0__version_ext_crcs\0";
static const char symbols_strings[] = "\0__kstrtab_example\0__crc_example\0";
static unsigned char image[4096];

struct spec {
	int core;
	const char *name, *vm, *license, *depends;
	unsigned crc;            /* stored CRC data for "example" */
	unsigned layout_crc;     /* module_layout import CRC */
	int gpl;                 /* core: export through __ksymtab_gpl */
	int no_export;           /* core: no exported symbol at all */
	int ext;                 /* module: __version_ext_* instead of __versions */
	int bogus_crc_symbol;    /* core: __crc_example st_value is not the CRC */
	int bad_versions_size;   /* module: __versions size not a multiple of 64 */
	int bad_ext_count;       /* module: ext names/CRCs disagree */
	int ext_pad;             /* module: trailing NUL after the last ext name */
	int no_modinfo;
};

static unsigned sh_name(const char *want)
{
	const char *p = shnames + 1;

	while ((size_t)(p - shnames) < sizeof(shnames)) {
		if (!strcmp(p, want))
			return (unsigned)(p - shnames);
		p += strlen(p) + 1;
	}
	return 0;
}

static void fixture(const struct spec *s)
{
	Elf64_Ehdr eh = { 0 };
	Elf64_Shdr sh[SECTIONS] = { { 0 } };
	Elf64_Sym sy[3] = { { 0 } };
	Elf64_Rela rel = { 0 };
	enum { SHSTRTAB = 1, STRTAB, SYMTAB, MODINFO, VERSIONS, KSYMTAB, KCRCTAB, KSTRINGS, RELA, EXTNAMES, EXTCRCS };
	char modinfo[512];
	unsigned off_kstr = 1, off_crc = 1 + (unsigned)strlen("__kstrtab_example") + 1;
	size_t n = 0;
	int exports;

	memset(image, 0, sizeof(image));
	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_ident[EI_VERSION] = EV_CURRENT;
	eh.e_type = ET_REL;
	eh.e_machine = EM_X86_64;
	eh.e_version = EV_CURRENT;
	eh.e_ehsize = sizeof(eh);
	eh.e_shoff = 64;
	eh.e_shentsize = sizeof(sh[0]);
	eh.e_shnum = SECTIONS;
	eh.e_shstrndx = SHSTRTAB;

	sh[SHSTRTAB].sh_name = sh_name(".shstrtab");
	sh[SHSTRTAB].sh_type = SHT_STRTAB;
	sh[SHSTRTAB].sh_offset = 1000;
	sh[SHSTRTAB].sh_size = sizeof(shnames);
	memcpy(image + 1000, shnames, sizeof(shnames));

	sh[STRTAB].sh_name = sh_name(".strtab");
	sh[STRTAB].sh_type = SHT_STRTAB;
	sh[STRTAB].sh_offset = 1300;
	sh[STRTAB].sh_size = sizeof(symbols_strings);
	memcpy(image + 1300, symbols_strings, sizeof(symbols_strings));

	sh[SYMTAB].sh_name = sh_name(".symtab");
	sh[SYMTAB].sh_type = SHT_SYMTAB;
	sh[SYMTAB].sh_offset = 1400;
	sh[SYMTAB].sh_size = sizeof(sy);
	sh[SYMTAB].sh_entsize = sizeof(sy[0]);
	sh[SYMTAB].sh_link = STRTAB;
	sh[SYMTAB].sh_info = 3;

	n = (size_t)snprintf(modinfo, sizeof(modinfo), "vermagic=%s", s->vm) + 1;
	n += (size_t)snprintf(modinfo + n, sizeof(modinfo) - n, "license=%s", s->license) + 1;
	n += (size_t)snprintf(modinfo + n, sizeof(modinfo) - n, "depends=%s", s->depends) + 1;
	n += (size_t)snprintf(modinfo + n, sizeof(modinfo) - n, "name=%s", s->name) + 1;
	sh[MODINFO].sh_name = sh_name(".modinfo");
	sh[MODINFO].sh_type = SHT_PROGBITS;
	sh[MODINFO].sh_offset = 1600;
	sh[MODINFO].sh_size = s->no_modinfo ? 0 : n;
	sh[MODINFO].sh_addralign = 1;
	memcpy(image + 1600, modinfo, n);

	sh[VERSIONS].sh_name = sh_name("__versions");
	sh[VERSIONS].sh_type = SHT_PROGBITS;
	sh[VERSIONS].sh_offset = 2200;
	sh[VERSIONS].sh_addralign = 8;
	sh[KCRCTAB].sh_name = sh_name(s->gpl ? "__kcrctab_gpl" : "__kcrctab");
	sh[KCRCTAB].sh_type = SHT_PROGBITS;
	sh[KCRCTAB].sh_offset = 2420;
	sh[KCRCTAB].sh_addralign = 4;
	sh[KSYMTAB].sh_name = sh_name(s->gpl ? "__ksymtab_gpl" : "__ksymtab");
	sh[KSYMTAB].sh_type = SHT_PROGBITS;
	sh[KSYMTAB].sh_offset = 2400;
	sh[KSYMTAB].sh_addralign = 8;
	sh[KSTRINGS].sh_name = sh_name("__ksymtab_strings");
	sh[KSTRINGS].sh_type = SHT_PROGBITS;
	sh[KSTRINGS].sh_offset = 2440;
	sh[KSTRINGS].sh_addralign = 1;
	sh[RELA].sh_name = sh_name(s->gpl ? ".rela__ksymtab_gpl" : ".rela__ksymtab");
	sh[RELA].sh_type = SHT_RELA;
	sh[RELA].sh_offset = 2460;
	sh[RELA].sh_entsize = sizeof(rel);
	sh[RELA].sh_link = SYMTAB;
	sh[RELA].sh_info = KSYMTAB;
	sh[RELA].sh_addralign = 8;

	exports = s->core && !s->no_export;
	if (exports) {
		sh[KSYMTAB].sh_size = 12;
		sh[KCRCTAB].sh_size = 4;
		sh[KSTRINGS].sh_size = 8;
		sh[RELA].sh_size = sizeof(rel);
		memcpy(image + 2420, &s->crc, 4);
		memcpy(image + 2440, "example", 8);
		rel.r_offset = 4;
		rel.r_info = ELF64_R_INFO(1, R_X86_64_PC32);
		rel.r_addend = 0;
		memcpy(image + 2460, &rel, sizeof(rel));
	}
	sy[1].st_name = off_kstr;
	sy[1].st_shndx = KSTRINGS;
	sy[1].st_info = ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE);
	sy[2].st_name = off_crc;
	sy[2].st_shndx = KCRCTAB;
	sy[2].st_info = ELF64_ST_INFO(STB_LOCAL, STT_OBJECT);
	sy[2].st_value = s->bogus_crc_symbol ? 0xdeadbeefu : 0;
	sy[2].st_size = 4;

	if (!s->core && !s->ext) {
		uint64_t crc = s->crc;

		sh[VERSIONS].sh_size = 128;
		memcpy(image + 2200, &crc, 8);
		memcpy(image + 2208, "example", 8);
		crc = s->layout_crc;
		memcpy(image + 2264, &crc, 8);
		memcpy(image + 2272, "module_layout", 14);
	}
	if (!s->core && s->ext) {
		sh[EXTNAMES].sh_name = sh_name("__version_ext_names");
		sh[EXTNAMES].sh_type = SHT_PROGBITS;
		sh[EXTNAMES].sh_offset = 2500;
		sh[EXTNAMES].sh_size = s->ext_pad ? 23 : 22;
		memcpy(image + 2500, "example\0module_layout\0", 23);
		sh[EXTCRCS].sh_name = sh_name("__version_ext_crcs");
		sh[EXTCRCS].sh_type = SHT_PROGBITS;
		sh[EXTCRCS].sh_offset = 2600;
		sh[EXTCRCS].sh_size = s->bad_ext_count ? 4 : 8;
		memcpy(image + 2600, &s->crc, 4);
		memcpy(image + 2604, &s->layout_crc, 4);
	}
	if (!s->core && s->bad_versions_size)
		sh[VERSIONS].sh_size = 63;

	memcpy(image, &eh, sizeof(eh));
	memcpy(image + 64, sh, sizeof(sh));
	memcpy(image + 1400, sy, sizeof(sy));
}

static void save(const char *path, size_t size)
{
	FILE *f = fopen(path, "wb");

	CHECK(f != NULL);
	if (!f)
		return;
	CHECK(fwrite(image, 1, size, f) == size);
	CHECK(!fclose(f));
}

static char *capture(int argc, char **argv, int *ret)
{
	FILE *f = tmpfile();
	int saved = dup(STDOUT_FILENO);
	char *buf;
	long len;

	CHECK(f != NULL);
	fflush(stdout);
	CHECK(dup2(fileno(f), STDOUT_FILENO) >= 0);
	*ret = gs_modcheck(argc, argv);
	fflush(stdout);
	CHECK(dup2(saved, STDOUT_FILENO) >= 0);
	close(saved);
	len = ftell(f);
	rewind(f);
	buf = calloc(1, len > 0 ? (size_t)len + 1 : 1);
	if (buf && len > 0)
		CHECK(fread(buf, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	return buf;
}

static int check(const char *core, const char *mod, const char *symvers, const char *vm, char **out)
{
	char *argv[8];
	int n = 0, rc;

	argv[n++] = "--core";
	argv[n++] = (char *)core;
	argv[n++] = "--module";
	argv[n++] = (char *)mod;
	if (symvers) {
		argv[n++] = "--kernel-symvers";
		argv[n++] = (char *)symvers;
	}
	if (vm) {
		argv[n++] = "--vermagic";
		argv[n++] = (char *)vm;
	}
	*out = capture(n, argv, &rc);
	return rc;
}

static const char vmagic[] = "test SMP";

static const struct spec core_ok = {
	.core = 1, .name = "nvidia", .vm = vmagic, .license = "NVIDIA", .depends = "",
	.crc = 0x12345678u, .bogus_crc_symbol = 1
};
static const struct spec mod_ok = {
	.name = "nvidia_uvm", .vm = vmagic, .license = "NVIDIA", .depends = "nvidia",
	.crc = 0x12345678u, .layout_crc = 0x22222222u
};

int main(void)
{
	char dir[] = "/tmp/gs-abi-XXXXXX", core[256], mod[256], mod2[256], sym[256];
	struct spec s;
	char *out;
	int rc;
	FILE *f;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(core, sizeof(core), "%s/core.ko", dir);
	snprintf(mod, sizeof(mod), "%s/mod.ko", dir);
	snprintf(mod2, sizeof(mod2), "%s/mod2.ko", dir);
	snprintf(sym, sizeof(sym), "%s/Module.symvers", dir);
	f = fopen(sym, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("0x22222222\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\t\n", f);
		CHECK(!fclose(f));
	}

	/* All imports resolve: one from the core, one from Module.symvers. */
	fixture(&core_ok); save(core, sizeof(image));
	fixture(&mod_ok); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 0);
	CHECK_STR(out, "module=nvidia_uvm vermagic=ok imports=2 provided_by_core=1 provided_by_kernel=1 unresolved=0 crc_mismatch=0");
	CHECK(!strstr(out, "failure="));
	free(out);

	/* Vermagic must match exactly; --vermagic overrides the core's string. */
	s = mod_ok; s.vm = "other SMP"; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK_STR(out, "vermagic=MISMATCH");
	CHECK_STR(out, "failure=vermagic module=nvidia_uvm expected=test SMP actual=other SMP");
	free(out);
	rc = check(core, mod, sym, "other SMP", &out);
	CHECK(rc == 0);
	free(out);

	/* Kbuild appends a trailing space when the flag list ends with one; it must still match. */
	s = mod_ok; s.vm = "test SMP "; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, "test SMP", &out);
	CHECK(rc == 0);
	CHECK_STR(out, "vermagic=ok");
	free(out);

	/* Unresolved import: the core exports nothing. */
	s = core_ok; s.no_export = 1; fixture(&s); save(core, sizeof(image));
	fixture(&mod_ok); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK_STR(out, "unresolved=1");
	CHECK_STR(out, "failure=unresolved module=nvidia_uvm symbol=example");
	free(out);

	/* CRC mismatch against the core's stored __kcrctab data. */
	fixture(&core_ok); save(core, sizeof(image));
	s = mod_ok; s.crc = 0xdeadbeefu; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK_STR(out, "crc_mismatch=1");
	CHECK_STR(out, "failure=crc_mismatch module=nvidia_uvm symbol=example provider=nvidia expected=12345678 actual=deadbeef");
	free(out);

	/* Regression: the bogus __crc_example symbol value must never be used as the CRC. */
	s = mod_ok; s.crc = 0xdeadbeefu; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK(strstr(out, "expected=12345678"));
	free(out);
	s = mod_ok; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 0);
	free(out);

	/* module_layout CRC mismatch is a CRC failure like any other. */
	fixture(&core_ok); save(core, sizeof(image));
	s = mod_ok; s.layout_crc = 0x33333333u; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK_STR(out, "failure=crc_mismatch module=nvidia_uvm symbol=module_layout provider=vmlinux expected=22222222 actual=33333333");
	free(out);

	/* Kernel provider through Module.symvers: GPL-only symbol, proprietary module. */
	f = fopen(sym, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("0x22222222\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\t\n"
		      "0x12345678\texample\tvmlinux\tEXPORT_SYMBOL_GPL\t\n", f);
		CHECK(!fclose(f));
	}
	s = core_ok; s.no_export = 1; fixture(&s); save(core, sizeof(image));
	fixture(&mod_ok); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK_STR(out, "provided_by_kernel=2");
	CHECK_STR(out, "failure=gpl_only module=nvidia_uvm symbol=example provider=vmlinux module_license=NVIDIA provider_license=kernel GPL export");
	free(out);
	s = mod_ok; s.license = "Dual MIT/GPL"; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 0);
	free(out);

	/* A dependency that the module does not declare. */
	f = fopen(sym, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("0x22222222\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\n", f);
		CHECK(!fclose(f));
	}
	fixture(&core_ok); save(core, sizeof(image));
	s = mod_ok; s.depends = ""; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	CHECK_STR(out, "failure=dependency module=nvidia_uvm symbol=example provider=nvidia depends=<empty>");
	free(out);

	/* Extended version sections replace __versions on Linux 7. */
	s = mod_ok; s.ext = 1; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 0);
	CHECK_STR(out, "imports=2 provided_by_core=1 provided_by_kernel=1");
	free(out);
	/* Real modules end the names section with the C literal's extra NUL. */
	s = mod_ok; s.ext = 1; s.ext_pad = 1; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 0);
	free(out);
	s = mod_ok; s.ext = 1; s.crc = 0xdeadbeefu; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	free(out);

	/* Malformed inputs fail closed without crashing. */
	s = mod_ok; s.bad_versions_size = 1; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	free(out);
	s = mod_ok; s.ext = 1; s.bad_ext_count = 1; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	free(out);
	s = mod_ok; s.no_modinfo = 1; fixture(&s); save(mod, sizeof(image));
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	free(out);
	fixture(&mod_ok); save(mod, 100);
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	free(out);
	save(mod, sizeof(image));

	/* Several modules: the failure wins, both summaries print. */
	fixture(&core_ok); save(core, sizeof(image));
	fixture(&mod_ok); save(mod, sizeof(image));
	s = mod_ok; s.name = "second"; s.crc = 0xdeadbeefu; fixture(&s); save(mod2, sizeof(image));
	{
		char *argv[6] = { "--core", core, "--module", mod, "--module", mod2 };

		out = capture(6, argv, &rc);
		CHECK(rc == 3);
		CHECK_STR(out, "module=nvidia_uvm vermagic=ok");
		CHECK_STR(out, "module=second vermagic=ok");
		free(out);
	}

	/* Usage errors. */
	out = capture(0, NULL, &rc);
	CHECK(rc == 2);
	free(out);
	{
		char *argv[2] = { "--module", mod };

		out = capture(2, argv, &rc);
		CHECK(rc == 2);
		free(out);
	}
	{
		char *argv[2] = { "--bogus", mod };

		out = capture(2, argv, &rc);
		CHECK(rc == 2);
		free(out);
	}
	{
		/* Without Module.symvers every kernel import is unresolved. */
		char *argv[4] = { "--core", core, "--module", mod };

		out = capture(4, argv, &rc);
		CHECK(rc == 3);
		CHECK_STR(out, "unresolved=1");
		CHECK_STR(out, "failure=unresolved module=nvidia_uvm symbol=module_layout");
		free(out);
	}

	/* Missing inputs are I/O errors, not refusals. */
	{
		char missing[300];

		snprintf(missing, sizeof(missing), "%s/none.ko", dir);
		rc = check(core, missing, sym, NULL, &out);
		CHECK(rc == 1);
		free(out);
		rc = check(missing, mod, sym, NULL, &out);
		CHECK(rc == 1);
		free(out);
	}

	/* Malformed Module.symvers is refused, not ignored. */
	f = fopen(sym, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("not a symvers line\n", f);
		CHECK(!fclose(f));
	}
	rc = check(core, mod, sym, NULL, &out);
	CHECK(rc == 3);
	free(out);

	CHECK(!unlink(core));
	CHECK(!unlink(mod));
	CHECK(!unlink(mod2));
	CHECK(!unlink(sym));
	CHECK(!rmdir(dir));
	printf("test_modcheck: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
