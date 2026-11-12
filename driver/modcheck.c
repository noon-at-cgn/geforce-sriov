/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "modcheck.h"
#include "elf_reader.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct gs_image {
	const char *path;
	unsigned char *data;
	size_t size;
	struct gs_elf elf;
	const char *vermagic, *license, *depends, *name;
};
struct gs_export {
	char *name, *provider;
	uint32_t crc;
	int gpl, core;
};
struct gs_table { struct gs_export *v; size_t n; };
struct gs_import { char *name; uint32_t crc; };
struct gs_imports { struct gs_import *v; size_t n; };

static int gs_error(const char *fmt, ...)
{
	va_list ap;

	fputs("abi-check: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	return 3;
}

static int gs_load(struct gs_image *im, const char *path)
{
	char err[256];
	struct stat st;
	int fd;

	memset(im, 0, sizeof(*im));
	im->path = path;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "abi-check: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "abi-check: %s: not a nonempty regular file\n", path);
		close(fd);
		return 1;
	}
	im->size = (size_t)st.st_size;
	im->data = mmap(NULL, im->size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (im->data == MAP_FAILED) {
		im->data = NULL;
		fprintf(stderr, "abi-check: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (gs_elf_open(im->data, im->size, &im->elf, err, sizeof(err)))
		return gs_error("%s: %s", path, err);
	return 0;
}

static void gs_unload(struct gs_image *im)
{
	if (im->data)
		munmap(im->data, im->size);
	im->data = NULL;
}

/* 0 found, 1 absent, 3 malformed. */
static int gs_section(const struct gs_image *im, const char *name, size_t *idx,
		      const unsigned char **p, size_t *n)
{
	char err[256];
	size_t i;
	int r = gs_elf_section_by_name(&im->elf, name, &i, NULL, err, sizeof(err));

	if (r == GS_ELF_NOT_FOUND)
		return 1;
	if (r)
		return gs_error("%s: %s", im->path, err);
	if (gs_elf_section_data(&im->elf, i, p, n, err, sizeof(err)))
		return gs_error("%s: %s", im->path, err);
	*idx = i;
	return 0;
}

/* Kbuild appends a trailing space when the flag list ends with one (the kernel's own modules do
 * too), so compare the meaningful text without touching the mapped image, which is read-only. */
static size_t vermagic_len(const char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		n--;
	return n;
}

static int vermagic_equal(const char *a, const char *b)
{
	size_t na = vermagic_len(a), nb = vermagic_len(b);

	return na == nb && memcmp(a, b, na) == 0;
}

static int gs_info(struct gs_image *im)
{
	const unsigned char *p;
	size_t n, idx, off = 0;
	int r = gs_section(im, ".modinfo", &idx, &p, &n);

	if (r)
		return gs_error("%s: missing or malformed .modinfo", im->path);
	while (off < n) {
		const char *s = (const char *)p + off;
		const char *end = memchr(s, 0, n - off);
		const char **dest = NULL;

		if (!end)
			return gs_error("%s: unterminated .modinfo entry", im->path);
		if (!strncmp(s, "vermagic=", 9))
			dest = &im->vermagic;
		else if (!strncmp(s, "license=", 8))
			dest = &im->license;
		else if (!strncmp(s, "depends=", 8))
			dest = &im->depends;
		else if (!strncmp(s, "name=", 5))
			dest = &im->name;
		if (dest) {
			const char *value = strchr(s, '=') + 1;
			if (*dest && strcmp(*dest, value))
				return gs_error("%s: conflicting .modinfo fields", im->path);
			*dest = value;
		}
		off += (size_t)(end - s) + 1;
	}
	if (!im->vermagic || !*im->vermagic)
		return gs_error("%s: .modinfo has no vermagic", im->path);
	if (!im->license || !*im->license)
		return gs_error("%s: .modinfo has no license", im->path);
	return 0;
}

static int gs_add_export(struct gs_table *t, const char *name, const char *provider, uint32_t crc,
			 int gpl, int core)
{
	struct gs_export *v;

	for (size_t i = 0; i < t->n; i++) {
		if (strcmp(t->v[i].name, name))
			continue;
		if (t->v[i].crc == crc && t->v[i].gpl == gpl && t->v[i].core == core &&
		    !strcmp(t->v[i].provider, provider))
			return 0;
		return gs_error("symbol %s is exported twice with conflicting values", name);
	}
	if (t->n == SIZE_MAX / sizeof(*v) || !(v = realloc(t->v, (t->n + 1) * sizeof(*v))))
		return 1;
	t->v = v;
	v += t->n;
	v->name = strdup(name);
	v->provider = strdup(provider);
	if (!v->name || !v->provider) {
		free(v->name);
		free(v->provider);
		return 1;
	}
	v->crc = crc;
	v->gpl = gpl;
	v->core = core;
	t->n++;
	return 0;
}

static void gs_free_table(struct gs_table *t)
{
	for (size_t i = 0; i < t->n; i++) {
		free(t->v[i].name);
		free(t->v[i].provider);
	}
	free(t->v);
	t->v = NULL;
	t->n = 0;
}

/*
 * Export entries are 12 bytes (value, name, namespace offsets). The name
 * offset is entry + 4 and is a PC-relative relocation to the name string.
 * The CRC lives in the matching __kcrctab entry as data; the __crc_<sym>
 * symbol value is an offset into its section and is never the CRC.
 */
static int gs_core_exports(const struct gs_image *im, struct gs_table *t, const char *provider)
{
	for (int gpl = 0; gpl < 2; gpl++) {
		const char *kn = gpl ? "__ksymtab_gpl" : "__ksymtab";
		const char *cn = gpl ? "__kcrctab_gpl" : "__kcrctab";
		const unsigned char *kp = NULL, *cp = NULL, *sp;
		size_t ki = 0, ci = 0, kl = 0, cl = 0, sl, entries;
		unsigned char *seen = NULL;
		struct gs_elf_rela_iter it;
		struct gs_elf_rela rel;
		char err[256];
		int kr, cr, r;

		kr = gs_section(im, kn, &ki, &kp, &kl);
		cr = gs_section(im, cn, &ci, &cp, &cl);
		if (kr == 1 && cr == 1)
			continue;
		if (kr || cr)
			return gs_error("%s: %s present without %s (core not built with MODVERSIONS?)",
					im->path, kr ? cn : kn, kr ? kn : cn);
		if (kl % 12 || cl != (kl / 12) * 4)
			return gs_error("%s: inconsistent %s/%s sizes", im->path, kn, cn);
		entries = kl / 12;
		if (entries && !(seen = calloc(entries, 1)))
			return 1;
		gs_elf_rela_begin(&it, ki);
		while ((r = gs_elf_rela_next(&im->elf, &it, &rel, err, sizeof(err))) > 0) {
			struct gs_elf_sym sym;
			size_t entry, pos;

			if (rel.offset % 12 != 4)
				continue;
			entry = (size_t)(rel.offset / 12);
			if (rel.type != R_X86_64_PC32 || !rel.sym || seen[entry])
				goto bad;
			if (gs_elf_symbol(&im->elf, rel.sym, &sym, err, sizeof(err)) || !sym.shndx)
				goto bad;
			if (gs_elf_section_data(&im->elf, sym.shndx, &sp, &sl, err, sizeof(err)))
				goto bad;
			if (rel.addend < 0 || sym.value > sl || (uint64_t)rel.addend >= sl - sym.value)
				goto bad;
			pos = (size_t)sym.value + (size_t)rel.addend;
			if (!sp[pos] || !memchr(sp + pos, 0, sl - pos))
				goto bad;
			{
				uint32_t crc;

				memcpy(&crc, cp + entry * 4, 4);
				r = gs_add_export(t, (const char *)sp + pos, provider, crc, gpl, 1);
			}
			if (r) {
				free(seen);
				return r;
			}
			seen[entry] = 1;
			continue;
bad:
			free(seen);
			return gs_error("%s: malformed relocation %zu in %s", im->path, it.index - 1, kn);
		}
		if (r < 0) {
			free(seen);
			return gs_error("%s: %s", im->path, err);
		}
		for (size_t i = 0; i < entries; i++) {
			if (!seen[i]) {
				free(seen);
				return gs_error("%s: %s entry %zu has no name relocation", im->path, kn, i);
			}
		}
		free(seen);
	}
	return 0;
}

/* CRC SYMBOL MODULE EXPORT_TYPE [NAMESPACE]; module may be vmlinux. */
static int gs_symvers(const char *path, struct gs_table *t)
{
	char *line = NULL;
	size_t cap = 0, nr = 0;
	ssize_t n;
	FILE *f;
	int rc = 0;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "abi-check: %s: %s\n", path, strerror(errno));
		return 1;
	}
	while ((n = getline(&line, &cap, f)) >= 0) {
		char crc[32], name[512], provider[512], type[64], ns[512], extra[2];
		unsigned long value;
		char *end, *p = line;
		int fields;

		nr++;
		if (memchr(line, 0, (size_t)n)) {
			rc = gs_error("%s:%zu: embedded NUL", path, nr);
			break;
		}
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\n' || *p == '\r' || !*p)
			continue;
		fields = sscanf(p, "%31s %511s %511s %63s %511s %1s", crc, name, provider, type, ns,
				extra);
		if (fields < 4 || fields > 5) {
			rc = gs_error("%s:%zu: malformed Module.symvers line", path, nr);
			break;
		}
		errno = 0;
		value = strtoul(crc, &end, 16);
		if (errno || *end || value > UINT32_MAX ||
		    (strcmp(type, "EXPORT_SYMBOL") && strcmp(type, "EXPORT_SYMBOL_GPL"))) {
			rc = gs_error("%s:%zu: malformed Module.symvers entry", path, nr);
			break;
		}
		for (size_t i = 0; i < t->n; i++) {
			if (!t->v[i].core || strcmp(t->v[i].name, name))
				continue;
			/* The built core is the provider that will be loaded. */
			if (t->v[i].crc != (uint32_t)value) {
				rc = gs_error("%s:%zu: %s CRC disagrees with the core export", path, nr,
					      name);
				break;
			}
			goto next;
		}
		if (rc)
			break;
		rc = gs_add_export(t, name, provider, (uint32_t)value,
				   !strcmp(type, "EXPORT_SYMBOL_GPL"), 0);
		if (rc)
			break;
next:
		;
	}
	if (ferror(f) && !rc)
		rc = 1;
	free(line);
	if (fclose(f) && !rc)
		rc = 1;
	return rc;
}

static int gs_import_add(struct gs_imports *a, const char *name, uint32_t crc)
{
	struct gs_import *v;

	for (size_t i = 0; i < a->n; i++) {
		if (strcmp(a->v[i].name, name))
			continue;
		if (a->v[i].crc == crc)
			return 0;
		return gs_error("import %s carries two different CRCs", name);
	}
	if (a->n == SIZE_MAX / sizeof(*v) || !(v = realloc(a->v, (a->n + 1) * sizeof(*v))))
		return 1;
	a->v = v;
	a->v[a->n].name = strdup(name);
	if (!a->v[a->n].name)
		return 1;
	a->v[a->n].crc = crc;
	a->n++;
	return 0;
}

/* Legacy __versions (64-byte entries) and Linux 7 __version_ext_names/__version_ext_crcs. */
static int gs_imports_read(const struct gs_image *im, struct gs_imports *a)
{
	const unsigned char *p, *names, *crcs;
	size_t idx, n, ni, nn, ci, cn, count = 0, off;
	int r, nr, cr;

	r = gs_section(im, "__versions", &idx, &p, &n);
	if (r < 0 || r == 3)
		return r;
	if (!r) {
		if (n % 64)
			return gs_error("%s: __versions size is not a multiple of 64", im->path);
		for (size_t i = 0; i < n; i += 64) {
			uint64_t crc;
			const char *name = (const char *)p + i + 8;

			memcpy(&crc, p + i, 8);
			if (crc > UINT32_MAX || !*name || !memchr(name, 0, 56))
				return gs_error("%s: malformed __versions entry", im->path);
			if ((r = gs_import_add(a, name, (uint32_t)crc)))
				return r;
		}
	}
	nr = gs_section(im, "__version_ext_names", &ni, &names, &nn);
	cr = gs_section(im, "__version_ext_crcs", &ci, &crcs, &cn);
	if (nr < 0 || nr == 3 || cr < 0 || cr == 3)
		return 3;
	if (nr == 1 && cr == 1) {
		if (!a->n)
			return gs_error("%s: no module version data", im->path);
		return 0;
	}
	if (nr || cr || cn % 4)
		return gs_error("%s: malformed extended version sections", im->path);
	if ((nn && !cn) || (!nn && cn))
		return gs_error("%s: extended version names/CRCs disagree", im->path);
	/*
	 * modpost emits one name per CRC as adjacent C string literals; the
	 * compiler appends one terminating NUL, so the names section holds
	 * crc_count names followed by NUL padding. Consume exactly crc_count
	 * names and refuse anything but NUL after them.
	 */
	count = cn / 4;
	off = 0;
	for (size_t i = 0; i < count; i++) {
		const char *s;
		const char *end;
		uint32_t crc;

		if (off >= nn)
			return gs_error("%s: extended version names end after %zu of %zu entries", im->path,
					i, count);
		s = (const char *)names + off;
		end = memchr(s, 0, nn - off);
		if (!end || end == s)
			return gs_error("%s: malformed extended version name %zu", im->path, i);
		memcpy(&crc, crcs + i * 4, 4);
		if ((r = gs_import_add(a, s, crc)))
			return r;
		off = (size_t)(end - (const char *)names) + 1;
	}
	for (; off < nn; off++) {
		if (names[off])
			return gs_error("%s: unexpected bytes after extended version names", im->path);
	}
	if (!a->n)
		return gs_error("%s: no module version data", im->path);
	return 0;
}

static int gs_gpl(const char *s)
{
	static const char *const licenses[] = {
		"GPL", "GPL v2", "GPL and additional rights", "Dual BSD/GPL", "Dual MIT/GPL",
		"Dual MPL/GPL",
	};

	for (size_t i = 0; i < sizeof(licenses) / sizeof(licenses[0]); i++) {
		if (!strcmp(s, licenses[i]))
			return 1;
	}
	return 0;
}

static int gs_dependency(const char *list, const char *provider)
{
	const char *base = strrchr(provider, '/');
	size_t len;

	base = base ? base + 1 : provider;
	len = strlen(base);
	if (len > 3 && !strcmp(base + len - 3, ".ko"))
		len -= 3;
	if (!strcmp(base, "vmlinux") || !strcmp(base, "kernel"))
		return 1;
	if (!list)
		return 0;
	while (*list) {
		const char *end = strchr(list, ',');
		size_t n = end ? (size_t)(end - list) : strlen(list);
		size_t i;

		for (i = 0; i < n && i < len; i++) {
			if ((list[i] == '-' ? '_' : list[i]) != (base[i] == '-' ? '_' : base[i]))
				break;
		}
		if (i == len && n == len)
			return 1;
		if (!end)
			break;
		list = end + 1;
	}
	return 0;
}

static const char *gs_basename(const char *path)
{
	const char *base = strrchr(path, '/');

	return base ? base + 1 : path;
}

/* vermagic_only: the kernel stores no CRCs (built without module versioning), so only the vermagic is judged. */
static int gs_check_module(const char *path, const struct gs_image *core, const struct gs_table *t,
			   const char *expected, int vermagic_only)
{
	struct gs_image im;
	struct gs_imports a = { 0 };
	size_t fromcore = 0, kernel = 0, unresolved = 0, mismatched = 0;
	char *details = NULL;
	size_t details_size = 0;
	FILE *out = NULL;
	const char *name;
	int rc, bad = 0, layout = 0;

	rc = gs_load(&im, path);
	if (rc)
		return rc;
	if ((rc = gs_info(&im)) || (!vermagic_only && (rc = gs_imports_read(&im, &a))))
		goto done;
	out = open_memstream(&details, &details_size);
	if (!out) {
		rc = 1;
		goto done;
	}
	name = im.name ? im.name : gs_basename(path);
	if (!vermagic_equal(im.vermagic, expected)) {
		fprintf(out, "failure=vermagic module=%s expected=%s actual=%s\n", name, expected,
			im.vermagic);
		bad = 1;
	}
	for (size_t i = 0; i < a.n; i++) {
		const struct gs_import *v = &a.v[i];
		const struct gs_export *x = NULL;

		if (!strcmp(v->name, "module_layout"))
			layout = 1;
		for (size_t j = 0; j < t->n; j++) {
			if (!strcmp(t->v[j].name, v->name)) {
				x = &t->v[j];
				break;
			}
		}
		if (!x) {
			unresolved++;
			bad = 1;
			fprintf(out, "failure=unresolved module=%s symbol=%s\n", name, v->name);
			continue;
		}
		if (x->core)
			fromcore++;
		else
			kernel++;
		if (x->crc != v->crc) {
			mismatched++;
			bad = 1;
			fprintf(out, "failure=crc_mismatch module=%s symbol=%s provider=%s expected=%08" PRIx32
				     " actual=%08" PRIx32 "\n",
				name, v->name, x->provider, x->crc, v->crc);
		}
		if (x->gpl && !gs_gpl(im.license)) {
			bad = 1;
			fprintf(out, "failure=gpl_only module=%s symbol=%s provider=%s module_license=%s"
				     " provider_license=%s\n",
				name, v->name, x->provider, im.license,
				x->core ? core->license : "kernel GPL export");
		}
		if (!gs_dependency(im.depends, x->provider)) {
			bad = 1;
			fprintf(out, "failure=dependency module=%s symbol=%s provider=%s depends=%s\n", name,
				v->name, x->provider,
				!im.depends ? "<missing>" : *im.depends ? im.depends : "<empty>");
		}
	}
	if (!layout && !vermagic_only) {
		bad = 1;
		fprintf(out, "failure=missing_module_layout module=%s\n", name);
	}
	if (fclose(out)) {
		out = NULL;
		rc = 1;
		goto done;
	}
	out = NULL;
	printf("module=%s vermagic=%s imports=%zu provided_by_core=%zu provided_by_kernel=%zu"
	       " unresolved=%zu crc_mismatch=%zu\n",
	       name, vermagic_equal(im.vermagic, expected) ? "ok" : "MISMATCH", a.n, fromcore, kernel,
	       unresolved, mismatched);
	if (details_size)
		fputs(details, stdout);
	rc = bad ? 3 : 0;
done:
	if (out)
		fclose(out);
	free(details);
	for (size_t i = 0; i < a.n; i++)
		free(a.v[i].name);
	free(a.v);
	gs_unload(&im);
	return rc;
}

static void gs_usage(FILE *f)
{
	fputs("Usage: geforce-sriov-patch abi-check --core CORE.ko --module MOD.ko"
	      " [--module MOD2.ko ...] [--kernel-symvers Module.symvers] [--vermagic STRING]"
	      " [--vermagic-only]\n",
	      f);
}

int gs_modcheck(int argc, char **argv)
{
	const char *core_path = NULL, *symvers = NULL, *vermagic = NULL, *core_name;
	const char **modules;
	size_t count = 0;
	struct gs_image core = { 0 };
	struct gs_table t = { 0 };
	int rc = 0, r, vermagic_only = 0;

	modules = calloc((size_t)argc + 1, sizeof(*modules));
	if (!modules)
		return 1;
	for (int i = 0; i < argc; i++) {
		const char *opt = argv[i];

		if (!strcmp(opt, "--vermagic-only")) {
			vermagic_only = 1;
			continue;
		}
		i++;
		if (i >= argc || !*argv[i]) {
			rc = 2;
			goto done;
		}
		if (!strcmp(opt, "--core") && !core_path)
			core_path = argv[i];
		else if (!strcmp(opt, "--module"))
			modules[count++] = argv[i];
		else if (!strcmp(opt, "--kernel-symvers") && !symvers)
			symvers = argv[i];
		else if (!strcmp(opt, "--vermagic") && !vermagic)
			vermagic = argv[i];
		else {
			rc = 2;
			goto done;
		}
	}
	if (!core_path || !count || !*core_path) {
		rc = 2;
		goto done;
	}
	if ((rc = gs_load(&core, core_path)) || (rc = gs_info(&core)))
		goto done;
	core_name = core.name ? core.name : gs_basename(core_path);
	if (!vermagic_only && (rc = gs_core_exports(&core, &t, core_name)))
		goto done;
	if (!vermagic_only && symvers && (rc = gs_symvers(symvers, &t)))
		goto done;
	for (size_t i = 0; i < count; i++) {
		r = gs_check_module(modules[i], &core, &t, vermagic ? vermagic : core.vermagic, vermagic_only);
		if (r == 1 || (r == 3 && !rc))
			rc = r;
		if (r == 1)
			break;
	}
done:
	gs_unload(&core);
	gs_free_table(&t);
	free(modules);
	if (rc == 2)
		gs_usage(stderr);
	return rc;
}
