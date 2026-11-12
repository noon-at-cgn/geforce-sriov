#!/bin/sh
# SPDX-License-Identifier: MIT
# Independent black-box checks for scripts/geforce-sriov-driver.sh against the
# v0.3.0 integration brief. Synthetic host, companion and public source trees
# and stub build tools are used; no NVIDIA file, no root, no network, no sysfs
# and no module is loaded.
#
# What this fixture covers: argument and refusal behaviour, --dry-run mutating
# nothing, refusal before any installed-file change, the plain transaction
# (install, backup, record, plugin link, modprobe file), rollback after
# installation starts, restore of a plain install, restore of the modules a
# feature tier added (reverse order, ownership by digest), foreign-file
# preservation, and the host-CUDA ABI refusal path.
#
# What it cannot prove: real ELF internals (symbol correspondence, idx 39
# relocation handling), a real kernel build, NVIDIA's recorded digests, the
# confirmed-input short path (it needs the recorded package digests), or any
# runtime behaviour.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH='' cd -- "$here/../.." && pwd)
driver=${GS_DRIVER_UNDER_TEST:-$here/../../scripts/geforce-sriov-driver.sh}
version_lib=$repo/scripts/gs-driver-version.sh
kernel=6.1.0-gs-brief
plugin_version=580.159.01
passed=0
failed=0

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/gs-driver-brief.XXXXXX")
trap 'rm -rf "$ROOT"' EXIT HUP INT TERM
REAL_SHA=$(command -v sha256sum)

check() {
    description=$1
    shift
    if "$@"; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        printf 'FAIL: %s\n' "$description" >&2
    fi
}

contains() {
    case $1 in
        *"$2"*) return 0 ;;
        *) return 1 ;;
    esac
}

exists() { [ -e "$1" ] || [ -L "$1" ]; }
snapshot() { ( cd "$1" && find . -mindepth 1 \( -type f -o -type l \) -print 2>/dev/null | LC_ALL=C sort ); }

# A .text image of the size the plain gate reads: zeros, with the eight recorded
# offsets set when PATCHED is 1.
fx_text() {
    path=$1
    patched=$2
    dd if=/dev/zero of="$path" bs=1 count=0 seek=$((0xd89671)) 2>/dev/null
    [ "$patched" -eq 0 ] && return 0
    for at in 0x14a7c4 0x14a7c5 0x14a7c6 0x14a7c7 0xa16708 0xa16709 0xd8966f 0xd89670; do
        printf 'P' | dd of="$path" bs=1 seek=$((at)) conv=notrunc 2>/dev/null
    done
}

# Every host path is under FX. The driver is copied with its sourced libraries
# next to it, which is the installed layout it locates.
fx_init() {
    FX=$ROOT/$1
    rm -rf "$FX"
    mkdir -p "$FX/bin/driver-patches" "$FX/tmp" \
        "$FX/root/lib/modules/$kernel/build" "$FX/root/lib/modules/$kernel/updates/dkms" \
        "$FX/root/etc/modprobe.d" "$FX/root/usr/lib/x86_64-linux-gnu" \
        "$FX/src/nvidia" "$FX/src/common/inc" \
        "$FX/companion/kernel/common/inc" "$FX/companion/kernel/nvidia-uvm" \
        "$FX/nvkms/src/nvidia-modeset" "$FX/built" "$FX/text"
    cp "$driver" "$FX/bin/geforce-sriov-driver"
    for library in "$repo"/scripts/gs-driver-*; do
        [ -f "$library" ] || continue
        cp "$library" "$FX/bin/"
    done
    # The installed payload carries the source patches; the host-CUDA route only
    # requires them to be present.
    printf 'fixture\n' >"$FX/bin/driver-patches/gpu-allow.patch"
    printf 'fixture\n' >"$FX/bin/driver-patches/allocation-threshold.patch"

    # Host source tree.
    printf -- '-DNV_VERSION_STRING="580.159.01"\n' >"$FX/src/Kbuild"
    printf 'VGX_KVM_BUILD=1\n' >"$FX/src/conftest.sh"
    printf 'obj-m += nvidia.o\n' >"$FX/src/nvidia/nvidia-sources.Kbuild"
    printf '#define SHARED 1\n' >"$FX/src/common/inc/shared.h"
    # Companion source tree: one second-input-only header, a shared one equal to
    # the host copy, and the UVM component.
    printf -- '-DNV_VERSION_STRING="580.159.03"\n' >"$FX/companion/kernel/Kbuild"
    printf 'GRID_BUILD=1\n' >"$FX/companion/kernel/conftest.sh"
    printf '#define SHARED 1\n' >"$FX/companion/kernel/common/inc/shared.h"
    printf '#define SECOND_ONLY 1\n' >"$FX/companion/kernel/common/inc/second-only.h"
    printf 'obj-m += nvidia-uvm.o\n' >"$FX/companion/kernel/nvidia-uvm/nvidia-uvm.Kbuild"
    # Public NVKMS source with the mode 0 marker the staging library requires.
    printf 'NVIDIA_VERSION = 580.159.03\n' >"$FX/nvkms/version.mk"
    printf 'all:\n\t@true\n' >"$FX/nvkms/src/nvidia-modeset/Makefile"
    printf '/* stock sysmem push buffer, NVKMS_PB_MODE 0 */\n' >"$FX/nvkms/src/nvidia-modeset/nvkms-rm.c"
    # Kernel header tree the script and the ABI gate read.
    printf '\n' >"$FX/root/lib/modules/$kernel/build/Makefile"
    printf '0xdeadbeef\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\t\n' \
        >"$FX/root/lib/modules/$kernel/build/Module.symvers"

    # Stub tools. They are the only way a build is simulated; nothing is compiled.
    # The sha256sum stub can answer the recorded identities for marker files, so
    # the known-input path is reachable without an NVIDIA artifact.
    printf 'unknown\n' >"$FX/sha-mode"
    cat >"$FX/bin/sha256sum" <<EOF
#!/bin/sh
mode=\$(cat "\$FXDIR/sha-mode" 2>/dev/null || true)
[ "\$mode" = known ] || exec "$REAL_SHA" "\$@"
key=\$(sed -n '1p' "\$1" 2>/dev/null || true)
case \$key in
    stock-object) d=4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449 ;;
    gate-object) d=ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701 ;;
    vfcount-object) d=f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12 ;;
    patched-object) d=946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29 ;;
    patched39-object) d=83ce12abba0441812aa135d4f099636513d709090c3da8999cda9243bd3b67ff ;;
    stock-plugin) d=7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7 ;;
    mixed-plugin) d=7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6 ;;
    patched-plugin-fixture) d=aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c ;;
    *) exec "$REAL_SHA" "\$@" ;;
esac
printf '%s  %s\n' "\$d" "\$1"
EOF
    cat >"$FX/bin/modinfo" <<'EOF'
#!/bin/sh
kernel=
name=
field=
path=
while [ $# -gt 0 ]; do
    case $1 in
        -k) kernel=$2; shift 2 ;;
        -n) name=$2; shift 2 ;;
        -F) field=$2; shift 2 ;;
        --) shift ;;
        *) path=$1; shift ;;
    esac
done
if [ -n "$name" ]; then
    case $name in
        nvidia) printf '%s/root/lib/modules/%s/updates/dkms/nvidia.ko\n' "$FXDIR" "$kernel" ;;
        *) printf '%s/root/lib/modules/%s/updates/dkms/%s.ko\n' "$FXDIR" "$kernel" "$name" ;;
    esac
    exit 0
fi
target=${path:-nvidia}
# A companion this fixture marks incompatible with the built core.
case $target in
    *nvidia-uvm*) printf 'foreign SMP mod_unload\n'; exit 0 ;;
esac
case $field in
    vermagic) printf '%s SMP mod_unload\n' "$kernel" ;;
    license) printf 'NVIDIA\n' ;;
    version) if [ -f "$target" ]; then sed -n 's/^version=//p' "$target"; else printf '%s\n' "$kernel"; fi ;;
    depends) if [ -f "$target" ]; then sed -n 's/^depends=//p' "$target"; else printf 'nvidia\n'; fi ;;
esac
exit 0
EOF
    cat >"$FX/bin/depmod" <<'EOF'
#!/bin/sh
# Fail once after installation starts when the fixture asks for a rollback.
if [ -f "$FXDIR/fail-depmod" ]; then
    rm -f "$FXDIR/fail-depmod"
    exit 1
fi
exit 0
EOF
    cat >"$FX/bin/objcopy" <<'EOF'
#!/bin/sh
eval "input=\${$(($# - 1))}"
eval "output=\${$#}"
marker=$(sed -n '1s/^text=//p' "$input" 2>/dev/null || true)
if [ -n "$marker" ] && [ -f "$FXDIR/text/$marker.text" ]; then
    cp "$FXDIR/text/$marker.text" "$output"
    exit 0
fi
if [ -f "$input.text" ]; then
    cp "$input.text" "$output"
    exit 0
fi
printf 'objcopy stub: no text image for %s\n' "$input" >&2
exit 1
EOF
    cat >"$FX/bin/readelf" <<'EOF'
#!/bin/sh
# Only the build-id note is needed outside the real ELF checks.
if [ "$1" = -n ] && [ "$#" -ge 2 ]; then
    marker=$(sed -n 's/^id=//p' "$2" 2>/dev/null || true)
    printf '    Build ID: %s\n' "${marker:-00112233445566778899aabbccddeeff00112233}"
    exit 0
fi
exit 0
EOF
    cat >"$FX/bin/make" <<'EOF'
#!/bin/sh
set -eu
fx=$FXDIR
tree=
modules=
while [ $# -gt 0 ]; do
    case $1 in
        -C) tree=$2; shift 2 ;;
        NV_KERNEL_MODULES=*) modules=${1#*=}; shift ;;
        *) shift ;;
    esac
done
[ -n "$tree" ] || { printf 'make stub: no -C\n' >&2; exit 1; }
# The public NVKMS Makefile has no Kbuild beside it; the merged tree does.
if [ -f "$tree/Makefile" ] && [ ! -f "$tree/Kbuild" ]; then
    mkdir -p "$tree/_out/Linux_$(uname -m)"
    printf 'public core\n' >"$tree/_out/Linux_$(uname -m)/nv-modeset-kernel.o"
    exit 0
fi
[ -n "$modules" ] || modules='nvidia nvidia-vgpu-vfio'
for module in $modules; do
    cp "$fx/built/$module.ko" "$tree/$module.ko"
    if [ -f "$fx/text/$module.text" ]; then
        cp "$fx/text/$module.text" "$tree/$module.ko.text"
    else
        cp "$fx/text/default.text" "$tree/$module.ko.text"
    fi
done
exit 0
EOF
    # The patcher follows the documented CLI and key=value report. Its revision
    # id is the stage name the driver wrote into the output path.
    cat >"$FX/bin/geforce-sriov-patch" <<'EOF'
#!/bin/sh
set -eu
fx=${FXDIR:?}
sha() { sha256sum "$1" | cut -d' ' -f1; }
command=$1
shift
case $command in
    apply) stage=scheduler ;;
    ada)
        [ "$1" = apply ] || exit 2
        shift
        stage=
        ;;
    display)
        shift
        case $1 in
            inspect)
                printf 'admission=anchor\nstate=confirmed\nrevision=display-idx39\n'
                printf 'function=%s\nfunction_offset=%s\nfile_offset=0x49bf2f\n' \
                    "$(cat "$fx/idx39-function" 2>/dev/null || printf '_nv045867rm')" \
                    "$(cat "$fx/idx39-offset" 2>/dev/null || printf '0x4f')"
                printf 'relocation_section=.rela.text\nrelocation_offset=0x1\n'
                exit 0
                ;;
            apply) shift ;;
            *) exit 2 ;;
        esac
        stage= ;;
    abi-check)
        if [ -f "$fx/abi-fail" ]; then
            printf 'abi-check: vermagic of nvidia-uvm.ko is foreign SMP; the core has %s SMP\n' "${FX_KERNEL:-the target kernel}" >&2
            exit 3
        fi
        printf 'abi-check: every module agrees with the core\n'
        exit 0
        ;;
    *) exit 2 ;;
esac
input=$1
output=$2
if [ -z "${stage:-}" ]; then
    base=${output##*/}
    base=${base%.out}
    case $base in
        scheduler) stage=scheduler ;;
        gate|module-gate) stage=module-gate ;;
        patched|vf-count) stage=vf-count ;;
        patched39|display-idx39) stage=display-idx39 ;;
        patched-plugin.mixed|plugin-mixed) stage=plugin-mixed ;;
        patched-plugin|plugin-div4) stage=plugin-div4 ;;
        *) stage=scheduler ;;
    esac
fi
if [ -f "$fx/refuse-stage" ] && [ "$(cat "$fx/refuse-stage")" = "$stage" ]; then
    printf 'geforce-sriov-patch: anchor %s.context: the recorded context was not found\n' "$stage" >&2
    exit 3
fi
if [ "$(sed -n '1p' "$input" 2>/dev/null || true)" = 'REFUSE' ]; then
    printf 'geforce-sriov-patch: anchor %s.context: refused by the fixture\n' "$stage" >&2
    exit 3
fi
case $stage in
    scheduler) printf 'gate-object\n' >"$output" ;;
    module-gate) printf 'vfcount-object\n' >"$output" ;;
    vf-count) printf 'patched-object\n' >"$output" ;;
    display-idx39) printf 'patched39-object\n' >"$output" ;;
    plugin-mixed) printf 'mixed-plugin\n' >"$output" ;;
    plugin-div4) printf 'patched-plugin-fixture\n' >"$output" ;;
    *) cp "$input" "$output" ;;
esac
printf 'admission=anchor\nstate=unconfirmed:unknown\nrevision=%s\nrecipe=fixture\n' "$stage"
printf 'input_sha256=%s\noutput_sha256=%s\n' "$(sha "$input")" "$(sha "$output")"
printf 'function=_nv045867rm\nfunction_offset=0x4f\nfile_offset=0x49bf2f\n'
exit 0
EOF
    chmod 0755 "$FX/bin"/*
}

# The installed and built plain modules and the two .text images.
fx_plain_files() {
    printf 'stock-object\n' >"$FX/src/nvidia/nv-kernel.o_binary"
    printf 'text=installed\nid=installed-id\n' \
        >"$FX/root/lib/modules/$kernel/updates/dkms/nvidia.ko"
    printf 'stock-plugin\n' \
        >"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.$plugin_version"
    printf 'text=nvidia\nid=built-id\nversion=580.159.01\ndepends=nvidia\n' >"$FX/built/nvidia.ko"
    printf 'built-vfio\nversion=580.159.01\ndepends=nvidia\n' >"$FX/built/nvidia-vgpu-vfio.ko"
    fx_text "$FX/text/installed.text" 0
    fx_text "$FX/text/nvidia.text" 1
    printf 'vfio-text\n' >"$FX/text/nvidia-vgpu-vfio.text"
    printf 'default-text\n' >"$FX/text/default.text"
}

# Run the driver with a controlled environment; rc keeps the exit status.
fx_run() {
    rc=0
    (
        cd "$FX"
        env -i PATH="$FX/bin:/usr/sbin:/usr/bin:/sbin:/bin" TMPDIR="$FX/tmp" HOME="$FX" \
            FXDIR="$FX" FX_KERNEL="$kernel" \
            GS_DRIVER_PATCH_DIR="$FX/bin/driver-patches" \
            sh "$FX/bin/geforce-sriov-driver" --root "$FX/root" --kernel "$kernel" \
            --source "$FX/src" "$@"
    ) >"$FX/out" 2>&1 || rc=$?
    out=$(cat "$FX/out")
}

record_field() { sed -n "s/^$1=//p" "$FX/root/var/lib/geforce-sriov/driver-build"; }
module_state() { printf '%s/var/lib/geforce-sriov/stock/%s/%s.ko' "$FX/root" "$kernel" "$1"; }
installed_module() { printf '%s/root/lib/modules/%s/updates/dkms/%s.ko' "$FX" "$kernel" "$1"; }
plugin_copy() { printf '%s/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.%s.geforce-sriov' "$FX" "$plugin_version"; }

# --- cases ------------------------------------------------------------------

# --dry-run prints a plan, mutates nothing, and never runs the build.
case_dry_run() {
    fx_init dry
    fx_plain_files
    before=$(snapshot "$FX/root")
    fx_run --dry-run
    check "dry-run plain: exits 0" test "$rc" -eq 0
    check "dry-run plain: mutates nothing" test "$before" = "$(snapshot "$FX/root")"
    check "dry-run plain: names the classification" contains "$out" 'classification'
    # shellcheck disable=SC2016 # the check runs in a fresh shell with the output as $1
    check "dry-run plain: states a classification term" sh -c \
        'case $1 in *confirmed-inputs*|*unconfirmed*) exit 0 ;; *) exit 1 ;; esac' sh "$out"

    before=$(snapshot "$FX/root")
    fx_run --dry-run --host-cuda --companion-source "$FX/companion"
    check "dry-run host CUDA: exits 0" test "$rc" -eq 0
    check "dry-run host CUDA: mutates nothing" test "$before" = "$(snapshot "$FX/root")"
    check "dry-run host CUDA: names the feature" contains "$out" 'host_cuda'

    before=$(snapshot "$FX/root")
    fx_run --dry-run --display --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
    check "dry-run display: exits 0" test "$rc" -eq 0
    check "dry-run display: mutates nothing" test "$before" = "$(snapshot "$FX/root")"
    check "dry-run display: names the feature" contains "$out" 'display'

    before=$(snapshot "$FX/root")
    fx_run --dry-run --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
    check "dry-run both: exits 0" test "$rc" -eq 0
    check "dry-run both: mutates nothing" test "$before" = "$(snapshot "$FX/root")"
}

# Every refusal happens before the first installed-file change.
case_refusals() {
    fx_init refuse
    fx_plain_files
    empty=$(snapshot "$FX/root")

    fx_run --display
    check "display without a public source: refused" test "$rc" -eq 3
    fx_run --host-cuda
    check "host CUDA without a companion: refused" test "$rc" -eq 3
    fx_run --display --companion-source "$FX/companion"
    check "display without a public source: names it" contains "$out" 'nvkms'
    check "feature refusals: nothing installed" test "$empty" = "$(snapshot "$FX/root")"

    fx_run --companion-file a --companion-source b
    check "companion file and source: usage error" test "$rc" -eq 2

    printf 'options nvidia NVreg_EnableGpuFirmware=1\n' >"$FX/root/etc/modprobe.d/other.conf"
    held=$(snapshot "$FX/root")
    fx_run
    check "a foreign firmware value: refused" test "$rc" -eq 3
    check "a foreign firmware value: names the file" contains "$out" 'other.conf'
    check "a foreign firmware value: nothing installed" test "$held" = "$(snapshot "$FX/root")"
    rm -f "$FX/root/etc/modprobe.d/other.conf"

    printf 'scheduler\n' >"$FX/refuse-stage"
    held=$(snapshot "$FX/root")
    fx_run
    check "a missing patch anchor: refused" test "$rc" -eq 3
    check "a missing patch anchor: names the anchor" contains "$out" 'anchor scheduler'
    check "a missing patch anchor: nothing installed" test "$held" = "$(snapshot "$FX/root")"
    rm -f "$FX/refuse-stage"
}

# The plain build installs the built core and the companion, keeps the backup,
# writes the record and the plugin link, and never touches a second core.
case_plain() {
    fx_init plain
    fx_plain_files
    fx_run
    check "plain: exits 0" test "$rc" -eq 0
    check "plain: core installed" test "$(sed -n '1p' "$(installed_module nvidia)")" = 'text=nvidia'
    check "plain: vfio is left to NVIDIA" test ! -e "$(installed_module nvidia-vgpu-vfio)"
    check "plain: stock core backed up" test "$(sed -n '1p' "$(module_state nvidia)")" = 'text=installed'
    check "plain: record written" exists "$FX/root/var/lib/geforce-sriov/driver-build"
    check "plain: record kernel" test "$(record_field kernel)" = "$kernel"
    check "plain: record feature flags" test "$(record_field display)/$(record_field host_cuda)" = 0/0
    check "plain: record confirmation" test "$(record_field confirmation)" = unconfirmed
    check "plain: record driver version" test "$(record_field driver_version)" = 580.159.01
    check "plain: record policy" test -n "$(record_field driver_version_policy)"
    check "plain: record module stamp" test "$(record_field module_nvidia_stamp)" = 580.159.01
    check "plain: record module digest" test -n "$(record_field module_nvidia_sha256)"
    check "plain: record backup digest" test -n "$(record_field module_nvidia_backup_sha256)"
    check "plain: patched plugin copy installed" exists "$(plugin_copy)"
    check "plain: plugin link points at the copy" test \
        "$(readlink "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so" 2>/dev/null)" = \
        "libnvidia-vgpu.so.$plugin_version.geforce-sriov"
    check "plain: modprobe file written" test \
        "$(cat "$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf")" = \
        'options nvidia NVreg_EnableGpuFirmware=0'
    check "plain: no UVM installed" test ! -e "$(installed_module nvidia-uvm)"
    check "plain: no display modules installed" test ! -e "$(installed_module nvidia-drm)"
    # shellcheck disable=SC2016 # the checks run in a fresh shell with the arguments given below
    check "plain: record validates through the version library" sh -c \
        '. "$1"; gs_record_read "$2" | grep -qx record_state=recorded' sh \
        "$version_lib" "$FX/root/var/lib/geforce-sriov/driver-build"

    # The eight-offset gate refuses a build that differs anywhere else, before
    # any installed file changes.
    fx_init gate
    fx_plain_files
    printf 'X' | dd of="$FX/text/nvidia.text" bs=1 seek=16 conv=notrunc 2>/dev/null
    held=$(snapshot "$FX/root")
    fx_run
    check "unexpected .text difference: refused" test "$rc" -eq 3
    check "unexpected .text difference: names an offset" contains "$out" 'offset'
    check "unexpected .text difference: nothing installed" test "$held" = "$(snapshot "$FX/root")"
}

# A failure after installation starts puts every file back.
case_rollback() {
    fx_init rollback
    fx_plain_files
    printf '1\n' >"$FX/fail-depmod"
    held=$(snapshot "$FX/root")
    fx_run
    check "rollback: the run fails" test "$rc" -eq 1
    check "rollback: the stock core is back" test \
        "$(sed -n '1p' "$(installed_module nvidia)")" = 'text=installed'
    check "rollback: the companion is gone" test ! -e "$(installed_module nvidia-vgpu-vfio)"
    check "rollback: no record" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
    check "rollback: no build tree" test ! -e "$FX/root/var/lib/geforce-sriov/build/$kernel"
    check "rollback: no plugin copy" test ! -e "$(plugin_copy)"
    check "rollback: no modprobe file" test ! -e "$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf"
    check "rollback: the fixture root matches its pre-run shape" test "$held" = "$(snapshot "$FX/root")"
}

# Restore of a plain install, and a foreign replacement is kept.
case_restore_plain() {
    fx_init restore
    fx_plain_files
    mkdir -p "$FX/root/opt"
    printf 'foreign file\n' >"$FX/root/opt/foreign"
    fx_run
    check "restore plain: install exits 0" test "$rc" -eq 0
    fx_run --restore
    check "restore plain: exits 0" test "$rc" -eq 0
    check "restore plain: stock core is back" test \
        "$(sed -n '1p' "$(installed_module nvidia)")" = 'text=installed'
    check "restore plain: companion removed" test ! -e "$(installed_module nvidia-vgpu-vfio)"
    check "restore plain: record removed" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
    check "restore plain: build tree removed" test ! -e "$FX/root/var/lib/geforce-sriov/build/$kernel"
    check "restore plain: plugin copy removed" test ! -e "$(plugin_copy)"
    check "restore plain: plugin link back at the stock copy" test \
        "$(readlink "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so" 2>/dev/null)" = \
        "libnvidia-vgpu.so.$plugin_version"
    check "restore plain: modprobe file removed" test ! -e \
        "$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf"
    check "restore plain: a foreign file is untouched" test "$(cat "$FX/root/opt/foreign")" = 'foreign file'

    # A foreign replacement of the installed core is kept, with a failed status.
    fx_run
    printf 'foreign module\n' >"$(installed_module nvidia)"
    fx_run --restore
    check "restore plain: the foreign core is kept" test \
        "$(sed -n '1p' "$(installed_module nvidia)")" = 'foreign module'
    check "restore plain: the foreign core is reported" contains "$out" 'not the module this script installed'
    check "restore plain: the stock backup survives a foreign core" test -f \
        "$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia.ko"
    check "restore plain: the record survives a foreign core" test -f \
        "$FX/root/var/lib/geforce-sriov/driver-build"
}

# Restore removes or puts back every module a feature tier added, in reverse
# order, using the record's paths and digests only.
case_restore_added() {
    fx_init added
    state=$FX/root/var/lib/geforce-sriov
    mkdir -p "$state/stock/$kernel" "$state/build/$kernel" \
        "$FX/root/usr/lib/x86_64-linux-gnu"
    printf 'stock-core\n' >"$state/stock/$kernel/nvidia.ko"
    printf 'text=installed\nid=installed-id\n' >"$(installed_module nvidia)"
    printf 'backup-modeset\n' >"$state/stock/$kernel/nvidia-modeset.ko"
    for name in nvidia-vgpu-vfio nvidia-modeset nvidia-drm nvidia-uvm; do
        printf 'installed %s\n' "$name" >"$(installed_module "$name")"
    done
    printf 'plugin\n' >"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.$plugin_version"
    printf 'installed copy\n' >"$(plugin_copy)"
    ln -s "libnvidia-vgpu.so.$plugin_version.geforce-sriov" \
        "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so"
    printf 'options nvidia NVreg_EnableGpuFirmware=0\n' \
        >"$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf"
    mkdir -p "$FX/root/opt"
    printf 'foreign file\n' >"$FX/root/opt/foreign"
    {
        printf 'kernel=%s\n' "$kernel"
        printf 'module_sha256=%s\n' "$(sha256sum "$(installed_module nvidia)" | cut -d' ' -f1)"
        printf 'module_build_id=installed-id\n'
        printf 'stock_sha256=%s\n' "$(sha256sum "$state/stock/$kernel/nvidia.ko" | cut -d' ' -f1)"
        printf 'driver_version=580.159.01\ndriver_version_confirmed=0\n'
        printf 'driver_version_policy=0.3.0\n'
        printf 'display=1\nhost_cuda=1\nconfirmation=unconfirmed\nevidence_profile=none\n'
        printf 'plugin_sha256=%s\n' "$(sha256sum "$(plugin_copy)" | cut -d' ' -f1)"
        printf 'module_nvidia_path=%s\n' "$(installed_module nvidia)"
        printf 'module_nvidia_sha256=%s\n' "$(sha256sum "$(installed_module nvidia)" | cut -d' ' -f1)"
        printf 'module_nvidia_previous=present\n'
        printf 'module_nvidia_backup_sha256=%s\n' \
            "$(sha256sum "$state/stock/$kernel/nvidia.ko" | cut -d' ' -f1)"
        for name in nvidia-vgpu-vfio nvidia-modeset nvidia-drm nvidia-uvm; do
            flat=$(printf '%s' "$name" | tr '-' '_')
            printf 'module_%s_path=%s\n' "$flat" "$(installed_module "$name")"
            printf 'module_%s_sha256=%s\n' "$flat" \
                "$(sha256sum "$(installed_module "$name")" | cut -d' ' -f1)"
            if [ "$name" = nvidia-modeset ]; then
                printf 'module_%s_previous=present\n' "$flat"
                printf 'module_%s_backup_sha256=%s\n' "$flat" \
                    "$(sha256sum "$state/stock/$kernel/nvidia-modeset.ko" | cut -d' ' -f1)"
            else
                printf 'module_%s_previous=absent\n' "$flat"
                printf 'module_%s_backup_sha256=none\n' "$flat"
            fi
        done
        printf 'idx39_function=_nv045867rm\nidx39_function_offset=79\n'
    } >"$state/driver-build"

    fx_run --restore
    check "restore added: exits 0" test "$rc" -eq 0
    check "restore added: the stock core is back" test \
        "$(sed -n '1p' "$(installed_module nvidia)")" = 'stock-core'
    check "restore added: UVM removed" test ! -e "$(installed_module nvidia-uvm)"
    check "restore added: DRM removed" test ! -e "$(installed_module nvidia-drm)"
    check "restore added: modeset put back" test \
        "$(sed -n '1p' "$(installed_module nvidia-modeset)")" = 'backup-modeset'
    check "restore added: VFIO companion removed" test ! -e "$(installed_module nvidia-vgpu-vfio)"
    check "restore added: the record is gone" test ! -e "$state/driver-build"
    check "restore added: the build tree is gone" test ! -e "$state/build/$kernel"
    check "restore added: plugin copy removed" test ! -e "$(plugin_copy)"
    check "restore added: plugin link back at the stock copy" test \
        "$(readlink "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so" 2>/dev/null)" = \
        "libnvidia-vgpu.so.$plugin_version"
    check "restore added: modprobe file removed" test ! -e \
        "$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf"
    check "restore added: a foreign file is untouched" test "$(cat "$FX/root/opt/foreign")" = 'foreign file'
    # Reverse order: UVM was added last, so it is removed before DRM, modeset
    # and the VFIO companion. Lines name the full path.
    order=$(printf '%s\n' "$out" | awk '
        /nvidia-uvm\.ko/ && !uvm { uvm = NR }
        /nvidia-drm\.ko/ && !drm { drm = NR }
        /nvidia-modeset\.ko/ && !modeset { modeset = NR }
        /nvidia-vgpu-vfio\.ko/ && !vfio { vfio = NR }
        END { if (uvm && drm && modeset && vfio) print uvm, drm, modeset, vfio }')
    check "restore added: modules are handled in reverse order" test -n "$order"
    if [ -n "$order" ]; then
        read -r first second third fourth <<EOF
$order
EOF
        check "restore added: UVM first" test "$first" -lt "$second"
        check "restore added: DRM before modeset" test "$second" -lt "$third"
        check "restore added: modeset before VFIO" test "$third" -lt "$fourth"
    fi

    # A foreign replacement of an added module is preserved and reported.
    fx_init added-foreign
    state=$FX/root/var/lib/geforce-sriov
    mkdir -p "$state/stock/$kernel" "$FX/root/usr/lib/x86_64-linux-gnu"
    printf 'stock-core\n' >"$state/stock/$kernel/nvidia.ko"
    printf 'installed core\n' >"$(installed_module nvidia)"
    printf 'plugin\n' >"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.$plugin_version"
    printf 'installed uvm\n' >"$(installed_module nvidia-uvm)"
    printf 'foreign uvm\n' >"$(installed_module nvidia-uvm)"
    {
        printf 'kernel=%s\n' "$kernel"
        printf 'module_sha256=%s\n' "$(sha256sum "$(installed_module nvidia)" | cut -d' ' -f1)"
        printf 'stock_sha256=%s\n' "$(sha256sum "$state/stock/$kernel/nvidia.ko" | cut -d' ' -f1)"
        printf 'display=1\nhost_cuda=1\n'
        printf 'module_nvidia_path=%s\n' "$(installed_module nvidia)"
        printf 'module_nvidia_sha256=%s\n' "$(sha256sum "$(installed_module nvidia)" | cut -d' ' -f1)"
        printf 'module_nvidia_previous=present\n'
        printf 'module_nvidia_backup_sha256=%s\n' \
            "$(sha256sum "$state/stock/$kernel/nvidia.ko" | cut -d' ' -f1)"
        printf 'module_nvidia_uvm_path=%s\n' "$(installed_module nvidia-uvm)"
        printf 'module_nvidia_uvm_sha256=expected-uvm-digest\n'
        printf 'module_nvidia_uvm_previous=absent\n'
        printf 'module_nvidia_uvm_backup_sha256=none\n'
    } >"$state/driver-build"
    fx_run --restore
    check "restore added foreign: the foreign UVM is kept" test \
        "$(cat "$(installed_module nvidia-uvm)")" = 'foreign uvm'
    check "restore added foreign: it is reported" contains "$out" 'not the module this script installed'
}

# The plugin copy is named deterministically; a foreign file at that name is
# not ours and must survive a restore.
case_restore_foreign_plugin() {
    fx_init foreign-plugin
    fx_plain_files
    mkdir -p "$FX/root/opt"
    printf 'foreign file\n' >"$FX/root/opt/foreign"
    fx_run
    check "foreign plugin: the plain install exits 0" test "$rc" -eq 0
    printf 'foreign plugin\n' >"$(plugin_copy)"
    fx_run --restore
    check "foreign plugin: the foreign copy survives" test \
        "$(cat "$(plugin_copy)")" = 'foreign plugin'
    check "foreign plugin: a foreign file is untouched" test \
        "$(cat "$FX/root/opt/foreign")" = 'foreign file'
}

# An unreadable or contradictory host identity must refuse; it must not become
# an "unknown version" build.
case_identity_refusal() {
    fx_init identity
    fx_plain_files
    : >"$FX/src/Kbuild"
    held=$(snapshot "$FX/root")
    fx_run
    check "unreadable host identity: refused" test "$rc" -eq 3
    check "unreadable host identity: nothing installed" test "$held" = "$(snapshot "$FX/root")"

    fx_init identity-conflict
    fx_plain_files
    printf -- '-DNV_VERSION_STRING="580.159.01"\n-DNV_VERSION_STRING="580.159.02"\n' \
        >"$FX/src/Kbuild"
    held=$(snapshot "$FX/root")
    fx_run
    check "contradictory host identity: refused" test "$rc" -eq 3
    check "contradictory host identity: nothing installed" test "$held" = "$(snapshot "$FX/root")"
}

# A symlink at a module destination is foreign: installing through it would
# write outside the module directory.
case_foreign_symlink() {
    fx_init foreign-link
    fx_plain_files
    mkdir -p "$FX/root/opt"
    printf 'foreign target\n' >"$FX/root/opt/foreign-target"
    rm -f "$(installed_module nvidia-vgpu-vfio)"
    ln -s "$FX/root/opt/foreign-target" "$(installed_module nvidia-vgpu-vfio)"
    fx_run
    check "foreign symlink: its target is not written through" test \
        "$(cat "$FX/root/opt/foreign-target")" = 'foreign target'
}

# A --host-cuda request on an already patched core must still build and install
# UVM; the old "already this build" return must not swallow it.
case_cuda_on_patched() {
    fx_init cuda-patched
    fx_plain_files
    printf 'known\n' >"$FX/sha-mode"
    fx_run
    check "host CUDA on a patched core: the plain install exits 0" test "$rc" -eq 0
    printf 'built-uvm\nversion=580.159.01\ndepends=nvidia\n' >"$FX/built/nvidia-uvm.ko"
    printf 'uvm-text\n' >"$FX/text/nvidia-uvm.text"
    printf '1\n' >"$FX/abi-fail"
    fx_run --host-cuda --companion-source "$FX/companion"
    check "host CUDA on a patched core: the ABI refusal is reached" test "$rc" -eq 3
    check "host CUDA on a patched core: no UVM is installed" test ! -e "$(installed_module nvidia-uvm)"
    check "host CUDA on a patched core: the record keeps its classification" test \
        -n "$(record_field confirmation)"
    check "host CUDA on a patched core: the record still says plain" test \
        "$(record_field display)/$(record_field host_cuda)" = 0/0

    # With the record gone, the installed core is still the patched build. That
    # must not become a silent "already this build" no-op that skips UVM.
    rm -f "$FX/root/var/lib/geforce-sriov/driver-build"
    fx_run --host-cuda --companion-source "$FX/companion"
    check "host CUDA on a patched core without a record: the ABI refusal is reached" test "$rc" -eq 3
    check "host CUDA on a patched core without a record: no UVM is installed" test \
        ! -e "$(installed_module nvidia-uvm)"
}

# The host-CUDA ABI gate refuses before installation; the failure names the
# reason and nothing is installed.
case_cuda_abi() {
    fx_init cuda
    fx_plain_files
    printf 'text=nvidia\nid=built-id\nversion=580.159.01\ndepends=nvidia\n' >"$FX/built/nvidia.ko"
    printf 'built-uvm\nversion=580.159.01\ndepends=nvidia\n' >"$FX/built/nvidia-uvm.ko"
    printf 'uvm-text\n' >"$FX/text/nvidia-uvm.text"
    printf '1\n' >"$FX/abi-fail"
    held=$(snapshot "$FX/root")
    fx_run --host-cuda --companion-source "$FX/companion"
    check "host CUDA ABI refusal: exit 3" test "$rc" -eq 3
    # shellcheck disable=SC2016 # the check runs in a fresh shell with the output as $1
    check "host CUDA ABI refusal: names the ABI reason" sh -c \
        'case $1 in *vermagic*|*ABI*|*CRC*|*abi-check*) exit 0 ;; *) exit 1 ;; esac' sh "$out"
    check "host CUDA ABI refusal: nothing installed" test "$held" = "$(snapshot "$FX/root")"
}

# Dropping a feature retires the modules it added under the same ownership
# rule: a module whose file still matches the record is removed, and one that
# replaced an earlier file gets that backup back.
case_retire() {
    fx_init retire
    fx_plain_files
    state=$FX/root/var/lib/geforce-sriov
    mkdir -p "$state/stock/$kernel" "$state/build/$kernel"
    cp "$(installed_module nvidia)" "$state/stock/$kernel/nvidia.ko"
    printf 'previous modeset\n' >"$state/stock/$kernel/nvidia-modeset.ko"
    for name in nvidia-vgpu-vfio nvidia-modeset nvidia-drm nvidia-uvm; do
        printf 'installed %s\n' "$name" >"$(installed_module "$name")"
    done
    {
        printf 'kernel=%s\n' "$kernel"
        printf 'module_sha256=%s\n' "$(sha256sum "$(installed_module nvidia)" | cut -d' ' -f1)"
        printf 'stock_sha256=%s\n' "$(sha256sum "$state/stock/$kernel/nvidia.ko" | cut -d' ' -f1)"
        printf 'driver_version=580.159.01\ndriver_version_confirmed=0\n'
        printf 'driver_version_policy=0.3.0\ndisplay=1\nhost_cuda=1\n'
        printf 'confirmation=unconfirmed\nevidence_profile=none\n'
        printf 'module_nvidia_path=%s\n' "$(installed_module nvidia)"
        printf 'module_nvidia_sha256=%s\n' "$(sha256sum "$(installed_module nvidia)" | cut -d' ' -f1)"
        printf 'module_nvidia_previous=present\n'
        printf 'module_nvidia_backup_sha256=%s\n' \
            "$(sha256sum "$state/stock/$kernel/nvidia.ko" | cut -d' ' -f1)"
        for name in nvidia-vgpu-vfio nvidia-modeset nvidia-drm nvidia-uvm; do
            flat=$(printf '%s' "$name" | tr '-' '_')
            printf 'module_%s_path=%s\n' "$flat" "$(installed_module "$name")"
            printf 'module_%s_sha256=%s\n' "$flat" \
                "$(sha256sum "$(installed_module "$name")" | cut -d' ' -f1)"
            if [ "$name" = nvidia-modeset ]; then
                printf 'module_%s_previous=present\n' "$flat"
                printf 'module_%s_backup_sha256=%s\n' "$flat" \
                    "$(sha256sum "$state/stock/$kernel/nvidia-modeset.ko" | cut -d' ' -f1)"
            else
                printf 'module_%s_previous=absent\n' "$flat"
                printf 'module_%s_backup_sha256=none\n' "$flat"
            fi
        done
    } >"$state/driver-build"

    fx_run
    check "retire: the plain build exits 0" test "$rc" -eq 0
    check "retire: the dropped modeset is put back from its backup" test \
        "$(sed -n '1p' "$(installed_module nvidia-modeset)")" = 'previous modeset'
    check "retire: the dropped DRM is removed" test ! -e "$(installed_module nvidia-drm)"
    check "retire: the dropped UVM is removed" test ! -e "$(installed_module nvidia-uvm)"
    check "retire: the retired backup is removed" test ! -e "$state/stock/$kernel/nvidia-modeset.ko"
    check "retire: the new record has no modeset" test -z "$(record_field module_nvidia_modeset_path)"
    check "retire: the new record flags are plain" test \
        "$(record_field display)/$(record_field host_cuda)" = 0/0
}

case_dry_run
case_refusals
case_plain
case_rollback
case_restore_plain
case_restore_added
case_retire
case_restore_foreign_plugin
case_identity_refusal
case_foreign_symlink
case_cuda_on_patched
case_cuda_abi

printf 'driver brief fixtures: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
