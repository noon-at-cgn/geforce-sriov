#!/bin/sh
# SPDX-License-Identifier: MIT
# The patch that lets the host driver accept the programs of NVIDIA's second package
# (driver-patches/client-accept.patch), checked as far as it can be without a kernel:
#   - the version goes into the patch, which then applies exactly once to the check of the version
#     string and to nothing else, with no marker left, and the module carries the version;
#   - the patched check, compiled in userspace against a model of NVIDIA's check (RmPerformVersionCheck,
#     osapi.c), lets through exactly the one version string and changes nothing else: the host's own
#     version, another build of the same major version, another major version, a longer or a shorter
#     string, a relaxed request and a request of the wrong size behave as they did without the patch,
#     and the request comes back as a strict one.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$here/../..
# shellcheck source=../../scripts/gs-driver-stage.sh
. "$root/scripts/gs-driver-stage.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gs-client-accept.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
die() { printf 'test-client-accept: %s\n' "$*" >&2; exit 1; }

patch=$root/driver-patches/client-accept.patch
[ -f "$patch" ] || die "missing: $patch"
ver=580.159.03
sed "s/@GS_CLIENT_ACCEPT@/$ver/g" "$patch" >"$tmp/patch"
if grep -q '@GS_CLIENT_ACCEPT@' "$tmp/patch"; then die 'a marker is left in the patch after the version went in'; fi
# The patch only adds lines: nothing of NVIDIA's code is removed or changed.
if sed -n '/^--- a\//,$p' "$patch" | grep -v '^--- ' | grep -q '^-'; then
    die 'client-accept.patch removes a line; it may only add'
fi

# ---- the check of the version string, as NVIDIA's source has it ----------------------------------
mkdir -p "$tmp/real/nvidia"
cat >"$tmp/exit-part.c" <<'EOF'
int fixture_before;

static void __exit nvidia_exit_module(void)
{
    nvidia_stack_t *sp = __nv_init_sp;

    nv_unregister_chrdev(NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE, 1,
        &nv_linux_control_device_cdev);
}

EOF
cat >"$tmp/case-part.c" <<'EOF'
static int fixture_ioctl(void)
{
    switch (arg_cmd)
    {
        case NV_ESC_CHECK_VERSION_STR:
        {
            NV_CTL_DEVICE_ONLY(nv);

            rmStatus = rm_perform_version_check(sp, arg_copy, arg_size);
            status = ((rmStatus == NV_OK) ? 0 : -EINVAL);
            break;
        }

        case NV_ESC_SYS_PARAMS:
        {
            nv_ioctl_sys_params_t *api = arg_copy;
        }
    }
}
EOF
cat "$tmp/exit-part.c" "$tmp/case-part.c" >"$tmp/real/nvidia/nv.c"
gs_apply_source_patch "$tmp/real" "$tmp/patch"
nv=$tmp/real/nvidia/nv.c
grep -qxF "#define GS_CLIENT_ACCEPT \"$ver\"" "$nv" || die 'the define of the version is missing'
grep -qxF 'MODULE_INFO(gs_client_accept, GS_CLIENT_ACCEPT);' "$nv" || die 'the module does not carry the version'
awk '/nv_ioctl_rm_api_version_t \*gs_v = arg_copy;/ { decl = NR }
     /NV_CTL_DEVICE_ONLY\(nv\);/ { only = NR }
     /gs_v->cmd = NV_RM_API_VERSION_CMD_RELAXED;/ { relaxed = NR }
     /rmStatus = rm_perform_version_check\(/ { check = NR }
     /gs_v->cmd = NV_RM_API_VERSION_CMD_STRICT;/ { strict = NR }
     END { exit !(decl && only && relaxed && check && strict && decl < only && only < relaxed && relaxed < check && check < strict) }' "$nv" ||
    die 'the declarations come before the first statement, and the request is relaxed before the check and strict after it'
rc=0; gs_apply_source_patch "$tmp/real" "$tmp/patch" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] || die 'a second application of the patch was not refused'
# A file that lacks the check is not patched.
mkdir -p "$tmp/other/nvidia"
printf 'int nothing;\n' >"$tmp/other/nvidia/nv.c"
rc=0; gs_apply_source_patch "$tmp/other" "$tmp/patch" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ "$(cat "$tmp/other/nvidia/nv.c")" = 'int nothing;' ] || die 'a file without the check was changed or accepted'
# The patch applied to the version of this driver's own source would not change what the host's own tools send.
case $ver in 580.159.01) die 'the fixture version is the host version' ;; esac

# ---- the patched check against a model of NVIDIA's ------------------------------------------------
cc=${CC:-cc}
if ! command -v "$cc" >/dev/null 2>&1; then
    printf 'SKIP: test-client-accept: no C compiler for the model of the check\n'
    exit 0
fi
# The patched case, taken out of the patched file: from its label to the break that ends it.
awk '/case NV_ESC_CHECK_VERSION_STR:/ { on = 1 } on { print } on && /^        }$/ { exit }' "$nv" >"$tmp/case.inc"
grep -q 'gs_accept' "$tmp/case.inc" || die 'the patched case was not found in the patched file'
{
    cat <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef unsigned int NvU32;
typedef int NvBool;
typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_GENERIC 1
#define NV_ERR_INVALID_ARGUMENT 2
#define EINVAL 22
#define NV_ESC_CHECK_VERSION_STR 1
#define NV_RM_API_VERSION_STRING_LENGTH 64
#define NV_RM_API_VERSION_CMD_STRICT 0
#define NV_RM_API_VERSION_CMD_RELAXED '1'
#define NV_RM_API_VERSION_CMD_QUERY '2'
#define NV_RM_API_VERSION_REPLY_RECOGNIZED 1
typedef struct { NvU32 cmd; NvU32 reply; char versionString[NV_RM_API_VERSION_STRING_LENGTH]; } nv_ioctl_rm_api_version_t;
#define NV_CTL_DEVICE_ONLY(nv) do { if (!(nv)) { status = -EINVAL; goto done; } } while (0)
EOF
    grep -F '#define GS_CLIENT_ACCEPT' "$nv"
    cat <<'EOF'
/* NVIDIA's check (RmPerformVersionCheck, osapi.c), for a driver whose own version is 580.159.01. */
static const char *kernel_version = "580.159.01";
static int rm_perform_version_check(void *sp, void *p, size_t size)
{
    nv_ioctl_rm_api_version_t *v = p;
    NvBool relaxed = 0;
    const char *a = v->versionString, *b = kernel_version;
    (void)sp;
    if (size != sizeof(*v)) return NV_ERR_INVALID_ARGUMENT;
    v->reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
    if (v->cmd == NV_RM_API_VERSION_CMD_QUERY) return NV_OK;
    if (v->cmd == NV_RM_API_VERSION_CMD_RELAXED) relaxed = 1;
    for (size_t i = 0; i < NV_RM_API_VERSION_STRING_LENGTH; i++) {
        if (a[i] != b[i]) break;
        if (a[i] == '\0') return NV_OK;
        if (relaxed && a[i] == '.') return NV_OK;
    }
    return NV_ERR_GENERIC;
}
static int ioctl_like(void *nv, void *arg_copy, size_t arg_size)
{
    int status = 0, arg_cmd = NV_ESC_CHECK_VERSION_STR;
    NV_STATUS rmStatus;
    void *sp = 0;
    switch (arg_cmd)
    {
EOF
    cat "$tmp/case.inc"
    cat <<'EOF'
    }
done:
    return status;
}
static int failures;
/* One request: its command, its version string, the size it claims; what the ioctl answered, and what came back. */
static void run(const char *what, unsigned int cmd, const char *s, size_t size, int want_status)
{
    nv_ioctl_rm_api_version_t v;
    int status;
    memset(&v, 0, sizeof(v));
    v.cmd = cmd;
    strncpy(v.versionString, s, sizeof(v.versionString));
    status = ioctl_like(&v, &v, size);
    if (status != want_status || v.cmd != cmd) {
        printf("FAIL %s: status %d (want %d), cmd %u (sent %u)\n", what, status, want_status, v.cmd, cmd);
        failures++;
    }
}
int main(void)
{
    char longer[NV_RM_API_VERSION_STRING_LENGTH + 1];
    nv_ioctl_rm_api_version_t v;
    int status;
    size_t n = sizeof(nv_ioctl_rm_api_version_t);

    run("the version of the second package", NV_RM_API_VERSION_CMD_STRICT, GS_CLIENT_ACCEPT, n, 0);
    run("the host's own version", NV_RM_API_VERSION_CMD_STRICT, "580.159.01", n, 0);
    run("another build of the same major version", NV_RM_API_VERSION_CMD_STRICT, "580.159.04", n, -EINVAL);
    run("another major version", NV_RM_API_VERSION_CMD_STRICT, "575.159.03", n, -EINVAL);
    run("the version with something after it", NV_RM_API_VERSION_CMD_STRICT, GS_CLIENT_ACCEPT "0", n, -EINVAL);
    run("the version cut short", NV_RM_API_VERSION_CMD_STRICT, "580.159.0", n, -EINVAL);
    run("an empty version", NV_RM_API_VERSION_CMD_STRICT, "", n, -EINVAL);
    run("a relaxed request, as the libraries make it with the variable set", NV_RM_API_VERSION_CMD_RELAXED, "580.159.07", n, 0);
    run("a relaxed request of another major version", NV_RM_API_VERSION_CMD_RELAXED, "575.159.03", n, -EINVAL);
    run("a query", NV_RM_API_VERSION_CMD_QUERY, "", n, 0);
    run("the right version and the wrong size", NV_RM_API_VERSION_CMD_STRICT, GS_CLIENT_ACCEPT, n - 1, -EINVAL);
    /* A string that fills the field without an end is not the version. */
    memset(longer, '5', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = '\0';
    run("a string without an end", NV_RM_API_VERSION_CMD_STRICT, longer, n, -EINVAL);
    /* The version, an end, and more after it: the string ends at the end. */
    memset(&v, 0, sizeof(v));
    memcpy(v.versionString, GS_CLIENT_ACCEPT "\0junk", sizeof(GS_CLIENT_ACCEPT) + 4);
    status = ioctl_like(&v, &v, n);
    if (status != 0 || v.cmd != NV_RM_API_VERSION_CMD_STRICT || v.reply != NV_RM_API_VERSION_REPLY_RECOGNIZED) {
        printf("FAIL the version with bytes after its end: status %d cmd %u reply %u\n", status, v.cmd, v.reply);
        failures++;
    }
    /* What a strict success looks like: the reply is set, the string is the one that was sent. */
    memset(&v, 0, sizeof(v));
    strcpy(v.versionString, GS_CLIENT_ACCEPT);
    ioctl_like(&v, &v, n);
    if (v.reply != NV_RM_API_VERSION_REPLY_RECOGNIZED || strcmp(v.versionString, GS_CLIENT_ACCEPT) != 0) {
        printf("FAIL the reply of a strict success\n");
        failures++;
    }
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
EOF
} >"$tmp/model.c"
"$cc" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-label -Wno-unused-variable -o "$tmp/model" "$tmp/model.c" ||
    die 'the patched check does not compile against the model of the kernel types'
"$tmp/model" || die 'the patched check behaves differently from NVIDIA'"'"'s for a version other than the second package'"'"'s'
printf 'test-client-accept: ok\n'
