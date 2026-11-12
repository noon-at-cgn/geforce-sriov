// SPDX-License-Identifier: MIT
/* PCI addresses and /etc/geforce-sriov/config (format shared with hook/gs_hook.c). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_lifecycle.h"

int gs_addr_parse(const char *s, struct gs_addr *out)
{
	struct gs_bdf b;
	size_t n = s ? strlen(s) : 0;

	if (n == 0 || n >= GS_BDF_STR_SIZE || !gs_parse_bdf(s, &b))
		return -1;
	out->domain = b.domain;
	out->bus = b.bus;
	out->slot = b.devfn >> 3;
	out->func = b.devfn & 7;
	memcpy(out->str, s, n + 1);
	return 0;
}

int gs_parse_switch(const char *value, int *out)
{
	if (strcmp(value, "on") == 0)
		*out = GS_SW_ON;
	else if (strcmp(value, "off") == 0)
		*out = GS_SW_OFF;
	else
		return -1;
	return 0;
}

int gs_parse_addr_list(const char *key, const char *value, struct gs_addr *out, unsigned int *n, char *err,
		       size_t errlen)
{
	const char *p = value;
	unsigned int count = 0, i;

	for (;;) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		char item[GS_BDF_STR_SIZE + 1];
		struct gs_addr b;

		if (len == 0 || len >= sizeof(item)) {
			snprintf(err, errlen, "%s: empty or malformed entry", key);
			return -1;
		}
		memcpy(item, p, len);
		item[len] = '\0';
		if (gs_addr_parse(item, &b)) {
			snprintf(err, errlen, "%s: '%s' is not a full PCI address dddd:bb:ss.f as in /sys/bus/pci/devices (lowercase hex; a domain over 4 digits has no leading zero; at most %x)",
				 key, item, GS_PCI_DOMAIN_MAX);
			return -1;
		}
		if (b.func != 0) {
			snprintf(err, errlen, "%s: %s is not function 0; select the GPU's physical function", key, item);
			return -1;
		}
		for (i = 0; i < count; i++) {
			if (strcmp(out[i].str, b.str) == 0) {
				snprintf(err, errlen, "%s: %s listed twice", key, item);
				return -1;
			}
		}
		if (count == GS_CFG_MAX_BDFS) {
			snprintf(err, errlen, "%s: more than %u devices", key, GS_CFG_MAX_BDFS);
			return -1;
		}
		out[count++] = b;
		if (!comma)
			break;
		p = comma + 1;
	}
	*n = count;
	return 0;
}

void gs_addr_list_str(const struct gs_addr *list, unsigned int n, char *buf, size_t len)
{
	size_t off = 0;

	if (len)
		buf[0] = '\0';
	for (unsigned int i = 0; i < n && off < len; i++) {
		int k = snprintf(buf + off, len - off, "%s%s", i ? "," : "", list[i].str);

		if (k < 0)
			break;
		off += (size_t)k;
	}
}


void gs_config_defaults(struct gs_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->ari = GS_SW_ON;
	cfg->msix = GS_SW_ON;
	cfg->identity = GS_SW_OFF;
	cfg->identity_force_host = GS_SW_OFF;
	snprintf(cfg->sriov_manage, sizeof(cfg->sriov_manage), "%s", GS_SRIOV_MANAGE_DEFAULT);
}

unsigned int gs_config_vfs(const struct gs_config *cfg)
{
	return cfg->vfs ? cfg->vfs : GS_DEFAULT_VFS;
}

unsigned int gs_config_vgpu_type(const struct gs_config *cfg, unsigned int generation)
{
	unsigned int set = generation == GS_ADA ? cfg->type_ada : cfg->type_ampere;

	return set ? set : gs_vgpu_type_default(generation);
}

int gs_parse_vfs(const char *value, unsigned int *out)
{
	unsigned int v = 0;
	size_t i;

	if (!value[0] || value[0] == '0' || strlen(value) > 2)
		return -1;
	for (i = 0; value[i]; i++) {
		if (value[i] < '0' || value[i] > '9')
			return -1;
		v = v * 10 + (unsigned int)(value[i] - '0');
	}
	if (v < 1 || v > GS_VFS_MAX)
		return -1;
	*out = v;
	return 0;
}

int gs_vfs_check(const struct gs_device *e, const struct gs_config *cfg, char *why, size_t len)
{
	unsigned int ceiling = gs_vf_ceiling(e, e->totalvfs);
	unsigned int vfs = gs_config_vfs(cfg);

	if (cfg->vfs && cfg->vfs > ceiling) {
		if (ceiling == e->totalvfs)
			snprintf(why, len, "vfs=%u is above the hardware default of 10de:%04x, %u VFs", cfg->vfs,
				 e->device, e->totalvfs);
		else
			snprintf(why, len, "vfs=%u is above the %u VFs geforce-sriov presents on 10de:%04x (hardware default %u)",
				 cfg->vfs, ceiling, e->device, e->totalvfs);
		return -1;
	}
	if ((vfs < ceiling ? vfs : ceiling) > GS_DEFAULT_VFS) {
		snprintf(why, len, "vfs=%u is above the default of %u: ARI decoding in the GPU does not work, so above %u VFs the livepatch module geforce_sriov refuses to load when the card's VF layout check fails",
			 vfs, GS_DEFAULT_VFS, GS_DEFAULT_VFS);
		return 1;
	}
	return 0;
}

void gs_vfs_describe(const struct gs_device *e, const struct gs_config *cfg, char *buf, size_t len)
{
	char how[32];

	if (cfg->vfs)
		snprintf(how, sizeof(how), "vfs=%u", cfg->vfs);
	else
		snprintf(how, sizeof(how), "default");
	snprintf(buf, len, "hardware default %u, geforce-sriov presents %u (%s)", e->totalvfs,
		 gs_vf_limit(e, e->totalvfs, gs_config_vfs(cfg)), how);
}

static int valid_key(const char *k, size_t n)
{
	size_t i;

	if (n == 0)
		return 0;
	for (i = 0; i < n; i++) {
		char c = k[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return 0;
	}
	return 1;
}

enum { K_BDFS, K_DISPLAY, K_HOST_CUDA, K_ARI, K_MSIX, K_VFS, K_IDENTITY, K_FORCE_HOST, K_SRIOV_MANAGE,
       K_TYPE_ADA, K_TYPE_AMPERE, K_D3COLD, K_FB_SLOTS, K_COUNT };
static const char *const keys[K_COUNT] = {
	"bdfs", "display", "host_cuda", "ari", "msix", "vfs", "identity", "identity_force_host",
	"sriov_manage", "type_ada", "type_ampere", "d3cold", "fb_slots",
};

int gs_config_parse(const char *text, size_t len, struct gs_config *cfg, char *err, size_t errlen)
{
	int seen[K_COUNT] = { 0 };
	size_t pos = 0;
	unsigned int lineno = 0;

	gs_config_defaults(cfg);
	if (len > GS_CONFIG_MAX_BYTES) {
		snprintf(err, errlen, "larger than %d bytes", GS_CONFIG_MAX_BYTES);
		return -1;
	}
	if (memchr(text, '\0', len)) {
		snprintf(err, errlen, "contains a NUL byte");
		return -1;
	}
	while (pos < len) {
		const char *line = text + pos;
		const char *nl = memchr(line, '\n', len - pos);
		size_t n = nl ? (size_t)(nl - line) : len - pos;
		const char *eq;
		char value[GS_PATH_MAX];
		size_t klen, vlen, i;
		int k;

		pos += n + (nl ? 1 : 0);
		lineno++;
		if (n == 0 || line[0] == '#')
			continue;
		for (i = 0; i < n; i++) {
			if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\v' ||
			    line[i] == '\f') {
				snprintf(err, errlen, "line %u: whitespace is not allowed", lineno);
				return -1;
			}
		}
		eq = memchr(line, '=', n);
		if (!eq) {
			snprintf(err, errlen, "line %u: expected key=value", lineno);
			return -1;
		}
		klen = (size_t)(eq - line);
		vlen = n - klen - 1;
		if (!valid_key(line, klen)) {
			snprintf(err, errlen, "line %u: key must match [a-z0-9_]+", lineno);
			return -1;
		}
		if (klen == sizeof("boot_display_release") - 1 &&
		    memcmp(line, "boot_display_release", klen) == 0) {
			snprintf(err, errlen, "line %u: %.*s: boot_display_release is obsolete; delete this line; use display=auto|on|off",
				 lineno, (int)n, line);
			return -1;
		}
		if (vlen == 0 || vlen >= sizeof(value)) {
			snprintf(err, errlen, "line %u: empty or overlong value", lineno);
			return -1;
		}
		memcpy(value, eq + 1, vlen);
		value[vlen] = '\0';
		for (k = 0; k < K_COUNT; k++) {
			if (strlen(keys[k]) == klen && memcmp(keys[k], line, klen) == 0)
				break;
		}
		if (k == K_COUNT) {
			/* Switches geforce-sriov does not have: the line is deleted, not converted. */
			if ((klen == 16 && memcmp(line, "experimental_vfs", klen) == 0) ||
			    (klen == 7 && memcmp(line, "twin_id", klen) == 0))
				snprintf(err, errlen, "line %u: unknown key '%.*s'; delete this line", lineno, (int)klen, line);
			else if (memmem(line, klen, "vf", 2))
				snprintf(err, errlen, "line %u: unknown key '%.*s'; the VF count key is vfs (vfs=1..%u, default %u)",
					 lineno, (int)klen, line, GS_VFS_MAX, GS_DEFAULT_VFS);
			else
				snprintf(err, errlen, "line %u: unknown key '%.*s'", lineno, (int)klen, line);
			return -1;
		}
		if (seen[k]++) {
			snprintf(err, errlen, "line %u: duplicate key '%s'", lineno, keys[k]);
			return -1;
		}
		switch (k) {
		case K_BDFS: {
			char sub[512];

			if (gs_parse_addr_list(keys[k], value, cfg->bdfs, &cfg->nbdfs, sub, sizeof(sub))) {
				snprintf(err, errlen, "line %u: %s", lineno, sub);
				return -1;
			}
			break;
		}
		case K_DISPLAY:
			if (strcmp(value, "auto") == 0)
				cfg->display = GS_DISPLAY_AUTO;
			else if (strcmp(value, "on") == 0)
				cfg->display = GS_DISPLAY_ON;
			else if (strcmp(value, "off") == 0)
				cfg->display = GS_DISPLAY_OFF;
			else {
				snprintf(err, errlen, "line %u: display must be auto, on or off", lineno);
				return -1;
			}
			break;
		case K_FB_SLOTS:
			if (strcmp(value, "auto") == 0)
				cfg->fb_slots = GS_FB_SLOTS_AUTO;
			else if (strcmp(value, "on") == 0)
				cfg->fb_slots = GS_FB_SLOTS_ON;
			else if (strcmp(value, "off") == 0)
				cfg->fb_slots = GS_FB_SLOTS_OFF;
			else {
				snprintf(err, errlen, "line %u: fb_slots must be auto, on or off", lineno);
				return -1;
			}
			break;
		case K_ARI:
		case K_MSIX:
		case K_HOST_CUDA:
		case K_IDENTITY:
		case K_FORCE_HOST: {
			int *dst = k == K_ARI ? &cfg->ari : k == K_MSIX ? &cfg->msix :
				   k == K_HOST_CUDA ? &cfg->host_cuda :
				   k == K_IDENTITY ? &cfg->identity : &cfg->identity_force_host;

			if (gs_parse_switch(value, dst)) {
				snprintf(err, errlen, "line %u: %s must be on or off", lineno, keys[k]);
				return -1;
			}
			break;
		}
		case K_VFS:
			if (gs_parse_vfs(value, &cfg->vfs)) {
				snprintf(err, errlen, "line %u: vfs must be a number of VFs from 1 to %u", lineno, GS_VFS_MAX);
				return -1;
			}
			break;
		case K_SRIOV_MANAGE:
			if (gs_validate_abs_path(value)) {
				snprintf(err, errlen, "line %u: sriov_manage must be a normalized absolute path",
					 lineno);
				return -1;
			}
			snprintf(cfg->sriov_manage, sizeof(cfg->sriov_manage), "%s", value);
			break;
		case K_TYPE_ADA:
		case K_TYPE_AMPERE:
			if (gs_parse_vgpu_type(value, k == K_TYPE_ADA ? &cfg->type_ada : &cfg->type_ampere)) {
				snprintf(err, errlen, "line %u: %s must be a vGPU type ID from 1 to %u, as nvidia/creatable_vgpu_types lists it",
					 lineno, keys[k], GS_VGPU_TYPE_MAX);
				return -1;
			}
			break;
		case K_D3COLD:
			/* Consent only: off is the one value, and no key means no consent. */
			if (strcmp(value, "off") != 0) {
				snprintf(err, errlen, "line %u: d3cold must be off (leave the key out for no consent)", lineno);
				return -1;
			}
			cfg->d3cold_off = 1;
			break;
		}
	}
	cfg->present = 1;
	return 0;
}

int gs_config_load(const char *path, struct gs_config *cfg, char *err, size_t errlen)
{
	char *data;
	size_t len;
	int r = gs_read_whole(path, &data, &len, GS_CONFIG_MAX_BYTES);

	if (r == -ENOENT) {
		gs_config_defaults(cfg);
		return 0;
	}
	if (r) {
		snprintf(err, errlen, "%s: %s", path,
			 r == -EFBIG ? "larger than 65536 bytes" :
			 r == -EINVAL ? "not a regular file" : strerror(-r));
		return -1;
	}
	if (gs_config_parse(data, len, cfg, err, errlen)) {
		char sub[512];

		snprintf(sub, sizeof(sub), "%s", err);
		snprintf(err, errlen, "%s: %s", path, sub);
		free(data);
		return -1;
	}
	free(data);
	return 0;
}

const char *gs_config_path(void)
{
	const char *env = getenv(GS_CONFIG_ENV);

	return env && env[0] ? env : GS_CONFIG_PATH;
}

int gs_config_render(const struct gs_config *cfg, char *buf, size_t len)
{
	size_t off = 0;
	unsigned int i;
	int n;

#define EMIT(...) do { \
		n = snprintf(buf + off, len - off, __VA_ARGS__); \
		if (n < 0 || (size_t)n >= len - off) \
			return -1; \
		off += (size_t)n; \
	} while (0)

	EMIT("# geforce-sriov configuration: key=value, no spaces; see docs.\n");
	EMIT("# bdfs: the GPUs (physical function .0) that the boot units and activate act on.\n");
	if (cfg->nbdfs) {
		EMIT("bdfs=");
		for (i = 0; i < cfg->nbdfs; i++)
			EMIT("%s%s", i ? "," : "", cfg->bdfs[i].str);
		EMIT("\n");
	} else {
		EMIT("# no device selected: the boot units do nothing\n");
	}
	if (cfg->d3cold_off)
		EMIT("# d3cold=off: consent to keep a laptop GPU powered when idle (activate sets its\n"
		     "# d3cold_allowed to 0); `install --disable-d3cold` writes it.\n"
		     "d3cold=off\n");
	EMIT("ari=%s\n", cfg->ari == GS_SW_OFF ? "off" : "on");
	EMIT("display=%s\n", cfg->display == GS_DISPLAY_ON ? "on" :
	     cfg->display == GS_DISPLAY_OFF ? "off" : "auto");
	EMIT("host_cuda=%s\n", cfg->host_cuda == GS_SW_ON ? "on" : "off");
	/* Written only when it was set: auto is what an absent key means. */
	if (cfg->fb_slots != GS_FB_SLOTS_AUTO)
		EMIT("fb_slots=%s\n", cfg->fb_slots == GS_FB_SLOTS_ON ? "on" : "off");
	EMIT("msix=%s\n", cfg->msix == GS_SW_OFF ? "off" : "on");
	if (cfg->vfs)
		EMIT("vfs=%u\n", cfg->vfs);
	EMIT("identity=%s\n", cfg->identity == GS_SW_ON ? "on" : "off");
	EMIT("identity_force_host=%s\n", cfg->identity_force_host == GS_SW_ON ? "on" : "off");
	if (strcmp(cfg->sriov_manage, GS_SRIOV_MANAGE_DEFAULT) != 0)
		EMIT("sriov_manage=%s\n", cfg->sriov_manage);
	EMIT("# type_ada, type_ampere: the vGPU type ID activate sets on each VF of an Ada or Ampere card\n"
	     "# and check expects (default %u on Ada, %u on Ampere).\n", GS_VGPU_TYPE_ADA, GS_VGPU_TYPE_AMPERE);
	if (cfg->type_ada)
		EMIT("type_ada=%u\n", cfg->type_ada);
	else
		EMIT("#type_ada=%u\n", GS_VGPU_TYPE_ADA);
	if (cfg->type_ampere)
		EMIT("type_ampere=%u\n", cfg->type_ampere);
	else
		EMIT("#type_ampere=%u\n", GS_VGPU_TYPE_AMPERE);
#undef EMIT
	return (int)off;
}

/*
 * The confirmed versions: the blocks of scripts/gs-driver-versions.tbl, as the build writes them.
 * A release that confirms another host driver adds a block there.
 */
static const struct gs_confirmed_version confirmed_builtin[] = {
#include "gs_versions.inc"
};
static const struct gs_confirmed_version *confirmed_rows = confirmed_builtin;
static size_t confirmed_count = sizeof(confirmed_builtin) / sizeof(confirmed_builtin[0]);

const struct gs_confirmed_version *gs_confirmed_versions(size_t *count)
{
	*count = confirmed_count;
	return confirmed_rows;
}

void gs_confirmed_versions_set(const struct gs_confirmed_version *rows, size_t count)
{
	confirmed_rows = rows ? rows : confirmed_builtin;
	confirmed_count = rows ? count : sizeof(confirmed_builtin) / sizeof(confirmed_builtin[0]);
}

const char *gs_default_host_version(void)
{
	return confirmed_rows[0].host;
}

const char *gs_nvidia_source_default(void)
{
	static char path[GS_PATH_MAX];

	for (size_t i = 0; i < confirmed_count; i++)
		if (!gs_pathf(path, sizeof(path), "/usr/src/nvidia-%s", confirmed_rows[i].host) && gs_exists(path))
			return path;
	gs_pathf(path, sizeof(path), "/usr/src/nvidia-%s", gs_default_host_version());
	return path;
}

void gs_confirmed_list(char *buf, size_t len)
{
	size_t off = 0;

	if (len)
		buf[0] = '\0';
	for (size_t i = 0; i < confirmed_count && off < len; i++) {
		int n = snprintf(buf + off, len - off, "%s%s", i ? ", " : "", confirmed_rows[i].host);

		if (n < 0 || (size_t)n >= len - off)
			break;
		off += (size_t)n;
	}
}

int gs_version_confirmed(enum gs_version_package package, const char *version)
{
	if (!version || (package != GS_VERSION_HOST && package != GS_VERSION_COMPANION))
		return 0;
	for (size_t i = 0; i < confirmed_count; i++) {
		const char *have = package == GS_VERSION_HOST ? confirmed_rows[i].host : confirmed_rows[i].second;

		/* A row may record no second package. */
		if (have && !strcmp(version, have))
			return 1;
	}
	return 0;
}

int gs_version_format(enum gs_version_message message, const char *version, char *buf, size_t len)
{
	const char *v = version ? version : "unknown";
	char list[256];
	int n;

	gs_confirmed_list(list, sizeof(list));
	switch (message) {
	case GS_VERSION_MSG_CONFIRMED:
		n = snprintf(buf, len, "NVIDIA's driver %s is confirmed to work.", v);
		break;
	case GS_VERSION_MSG_PROMPT:
		n = snprintf(buf, len, "NVIDIA's driver %s is not guaranteed to work. Confirmed versions: %s. Continue? [y/N]", v, list);
		break;
	case GS_VERSION_MSG_BEST_EFFORT:
		n = snprintf(buf, len, "Continuing uses best effort; input and safety checks still apply. Default is no.");
		break;
	case GS_VERSION_MSG_SETUP_REFUSAL:
		n = snprintf(buf, len, "setup: NVIDIA's driver %s is not guaranteed to work. Confirmed versions: %s. Nothing changed. To continue without questions, pass --allow-unsupported-driver.", v, list);
		break;
	case GS_VERSION_MSG_INSTALL_NOTICE:
		n = snprintf(buf, len, "geforce-sriov: NVIDIA's driver %s is not guaranteed to work. Confirmed versions: %s. Continuing uses best effort; input and safety checks still apply.", v, list);
		break;
	case GS_VERSION_MSG_DOCTOR_WARNING:
		n = snprintf(buf, len, "[WARN] NVIDIA's driver %s is not guaranteed to work; confirmed versions: %s.", v, list);
		break;
	case GS_VERSION_MSG_STATUS:
		n = snprintf(buf, len, "NVIDIA's driver %s is not guaranteed to work; confirmed versions: %s.", v, list);
		break;
	case GS_VERSION_MSG_OPEN_REFUSAL:
		n = snprintf(buf, len, "geforce-sriov: refused, nothing changed: NVIDIA's open flavour cannot be used by this build. --allow-unsupported-driver does not override this check.");
		break;
	default:
		return -1;
	}
	return n < 0 || (size_t)n >= len ? -1 : n;
}
