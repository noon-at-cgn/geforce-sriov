#!/bin/sh
# SPDX-License-Identifier: MIT
# Stage and verify the driver before changing installed files.
set -eu

stock_object=4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449
patched_object=946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29
stock_plugin=7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7
# The stock plugin after the patcher's row plugin-mixed: the input of plugin-div4. A copy that has only
# this patch was installed by earlier versions; it is replaced, never accepted as the installed copy.
mixed_plugin=7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6
# The patched plugin copy: plugin-mixed and then plugin-div4. The only copy this step installs.
patched_plugin=aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c
src=/usr/src/nvidia-580.159.01
kernel=
root=
dry=0
restore=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --dry-run) dry=1 ;;
        --restore) restore=1 ;;
        # --root DIR: every fixed path below DIR instead of / (the tests' fixture hosts).
        --source|--kernel|--root)
            [ "$#" -ge 2 ] || { printf '%s requires a value\n' "$1" >&2; exit 2; }
            case $1 in --source) src=$2 ;; --kernel) kernel=$2 ;; *) root=$2 ;; esac
            shift ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$kernel" ] || kernel=$(uname -r)
case $kernel in ''|.|..|*/*) printf 'invalid kernel: %s\n' "$kernel" >&2; exit 2;; esac
case $root in ''|/*) ;; *) printf 'invalid root: %s\n' "$root" >&2; exit 2;; esac
state=$root/var/lib/geforce-sriov
build=$state/build/$kernel
module=
headers=$root/lib/modules/$kernel/build
config=$root/etc/modprobe.d/geforce-sriov-nvidia.conf
record=$state/driver-build
backup=$state/stock/$kernel/nvidia.ko

refuse() { printf 'geforce-sriov-driver: refused: %s\n' "$*" >&2; exit 3; }
fail() { printf 'geforce-sriov-driver: failed: %s\n' "$*" >&2; exit 1; }

# Commands are logged as shell-quoted arguments so paths with spaces remain legible.
quote() {
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}
show() {
    printf '+'
    for arg do printf ' '; quote "$arg"; done
    printf '\n'
}
run() { show "$@"; "$@"; }
sha() { sha256sum "$1" | cut -d ' ' -f 1; }

# The patched plugin copy is executable, like NVIDIA's own copies; one from an earlier install may not be.
copy_mode() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ "$(stat -c %a "$1")" != 755 ] || return 0
    chmod 0755 "$1" || fail "could not set mode 0755 on $1"
    printf 'geforce-sriov-driver: set mode 0755 on %s\n' "$1"
}

# --restore: undo this script's changes for the kernel. The stock module comes back only while the
# installed module is still the one this script installed, from a backup whose digest the record
# holds; the build directory, the plugin link and the modprobe file are restored whenever they are
# this script's own. Runs before any build check, so it works on a host without headers. Every
# check comes before the first change; it exits 0 only when each step succeeded. Then the backup
# it put back goes, and so do the build and backup directories once they are empty; after a
# failure every backup stays.
if [ "$restore" -eq 1 ]; then
    command -v modinfo >/dev/null 2>&1 || refuse 'modinfo not found: cannot locate the installed module'
    module=$(modinfo -k "$kernel" -n nvidia 2>/dev/null || true)
    put_back=0
    if [ -z "$module" ]; then
        printf 'geforce-sriov-driver: no nvidia module installed for %s; nothing to restore\n' "$kernel"
    elif [ ! -f "$record" ]; then
        printf 'geforce-sriov-driver: %s absent: this script did not prepare %s; nothing to restore\n' "$record" "$kernel"
    else
        rec_kernel=$(sed -n 's/^kernel=//p' "$record")
        rec_hash=$(sed -n 's/^module_sha256=//p' "$record")
        rec_stock=$(sed -n 's/^stock_sha256=//p' "$record")
        installed=$(sha "$module") || fail "could not hash $module"
        if [ "$rec_kernel" != "$kernel" ] || [ "$rec_hash" != "$installed" ]; then
            printf 'geforce-sriov-driver: kept %s: it is not the module this script installed\n' "$module"
        else
            [ -f "$backup" ] || refuse "backup missing: $backup"
            [ -n "$rec_stock" ] || refuse "$record records no stock_sha256, so the backup $backup cannot be verified"
            backup_hash=$(sha "$backup") || fail "could not hash $backup"
            [ "$backup_hash" = "$rec_stock" ] || refuse "backup $backup has SHA-256 $backup_hash; $record records $rec_stock"
            put_back=1
        fi
    fi
    # The plugin the install put in place, and the link it repointed, are named deterministically.
    plugdir=
    for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
        if [ -f "$dir/libnvidia-vgpu.so.580.159.01" ]; then plugdir=$dir; break; fi
    done
    relink=0
    foreign=
    if [ -n "$plugdir" ] && [ -L "$plugdir/libnvidia-vgpu.so" ]; then
        target=$(readlink "$plugdir/libnvidia-vgpu.so")
        if [ "$target" = libnvidia-vgpu.so.580.159.01.geforce-sriov ]; then
            relink=1
        # A link to NVIDIA's own plugin needs nothing; any other target is someone else's and stays.
        elif [ "$(readlink -f "$plugdir/libnvidia-vgpu.so" || true)" != "$(readlink -f "$plugdir/libnvidia-vgpu.so.580.159.01")" ]; then
            foreign=$target
        fi
    fi
    own_config=0
    if [ -f "$config" ] && [ ! -L "$config" ] && [ "$(cat "$config")" = 'options nvidia NVreg_EnableGpuFirmware=0' ]; then
        own_config=1
    fi
    if [ "$dry" -eq 1 ]; then
        show rm -rf "$build"
        if [ "$put_back" -eq 1 ]; then
            show cp -p "$backup" "$module"
            show depmod -a "$kernel"
            show rm -f "$record"
        fi
        [ "$relink" -eq 0 ] || show ln -sfn libnvidia-vgpu.so.580.159.01 "$plugdir/libnvidia-vgpu.so"
        [ -z "$plugdir" ] || show rm -f "$plugdir/libnvidia-vgpu.so.580.159.01.geforce-sriov"
        [ "$own_config" -eq 0 ] || show rm -f "$config"
        [ "$put_back" -eq 0 ] || show rm -rf "${backup%/*}"
        show rmdir "$state/build" "$state/stock"
        exit 0
    fi
    status=0
    if [ -e "$build" ] || [ -L "$build" ]; then
        if rm -rf "$build"; then
            printf 'geforce-sriov-driver: removed %s\n' "$build"
        else
            printf 'geforce-sriov-driver: failed: could not remove %s\n' "$build" >&2
            status=1
        fi
    fi
    if [ "$put_back" -eq 1 ]; then
        if cp -p "$backup" "$module" && depmod -a "$kernel"; then
            rm -f "$record"
            printf 'geforce-sriov-driver: restored the stock module at %s\n' "$module"
        else
            printf 'geforce-sriov-driver: failed: could not restore %s from %s\n' "$module" "$backup" >&2
            status=1
        fi
    fi
    if [ "$relink" -eq 1 ]; then
        if ln -sfn libnvidia-vgpu.so.580.159.01 "$plugdir/libnvidia-vgpu.so"; then
            printf 'geforce-sriov-driver: pointed %s back at the stock plugin libnvidia-vgpu.so.580.159.01\n' "$plugdir/libnvidia-vgpu.so"
        else
            printf 'geforce-sriov-driver: failed: could not point %s/libnvidia-vgpu.so back at the stock plugin\n' "$plugdir" >&2
            status=1
        fi
    elif [ -n "$foreign" ]; then
        printf 'geforce-sriov-driver: left %s: it points to %s, which this package did not install\n' "$plugdir/libnvidia-vgpu.so" "$foreign"
    fi
    if [ -n "$plugdir" ] && [ -e "$plugdir/libnvidia-vgpu.so.580.159.01.geforce-sriov" ] &&
        [ "$(readlink "$plugdir/libnvidia-vgpu.so" || true)" != libnvidia-vgpu.so.580.159.01.geforce-sriov ]; then
        if rm -f "$plugdir/libnvidia-vgpu.so.580.159.01.geforce-sriov"; then
            printf 'geforce-sriov-driver: removed %s\n' "$plugdir/libnvidia-vgpu.so.580.159.01.geforce-sriov"
        else
            printf 'geforce-sriov-driver: failed: could not remove %s\n' "$plugdir/libnvidia-vgpu.so.580.159.01.geforce-sriov" >&2
            status=1
        fi
    fi
    if [ "$own_config" -eq 1 ]; then
        if rm -f "$config"; then
            printf 'geforce-sriov-driver: removed %s\n' "$config"
        else
            status=1
        fi
    fi
    if [ "$status" -eq 0 ]; then
        if [ "$put_back" -eq 1 ]; then
            if rm -rf "${backup%/*}"; then
                printf 'geforce-sriov-driver: removed %s\n' "${backup%/*}"
            else
                printf 'geforce-sriov-driver: failed: could not remove %s\n' "${backup%/*}" >&2
                status=1
            fi
        fi
        for dir in "$state/build" "$state/stock"; do
            if [ -d "$dir" ] && rmdir "$dir" 2>/dev/null; then
                printf 'geforce-sriov-driver: removed %s\n' "$dir"
            fi
        done
    fi
    exit "$status"
fi

# Dry-run is a plan only: it runs no build, patcher, or filesystem mutation.
if [ "$dry" -eq 1 ] && [ "$restore" -eq 0 ]; then
    # The installed module as the real run finds it below, or a placeholder when modinfo does not know it.
    module=$(modinfo -k "$kernel" -n nvidia 2>/dev/null || true)
    [ -n "$module" ] || module="<installed nvidia.ko for $kernel>"
    cat <<EOF
+ sha256sum '$src/nvidia/nv-kernel.o_binary' # require $stock_object
+ test -f '$headers/Makefile'
+ test -f '$module'
+ modinfo -k '$kernel' -F license nvidia # refuse NVIDIA's open flavour
+ test -x '${0%/*}/geforce-sriov-patch' || command -v geforce-sriov-patch
+ sha256sum '<plugin directory>/libnvidia-vgpu.so.580.159.01' # require the stock digest, that of plugin-mixed's output or that of the patched plugin copy
+ check $root/etc/modprobe.d/*.conf for NVreg_EnableGpuFirmware # another file that sets 0 is kept; any other value refuses
+ mktemp -d '\${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX'
+ cp -a '$src' '<temporary directory>/tree'
+ geforce-sriov-patch apply '<temporary directory>/tree/nvidia/nv-kernel.o_binary' '<temporary directory>/scheduler'
+ geforce-sriov-patch ada apply '<temporary directory>/scheduler' '<temporary directory>/gate'
+ geforce-sriov-patch ada apply '<temporary directory>/gate' '<temporary directory>/patched'
+ sha256sum '<temporary directory>/patched' # require $patched_object
+ cp '<temporary directory>/patched' '<temporary directory>/tree/nvidia/nv-kernel.o_binary'
+ make -C '<temporary directory>/tree' NV_EXCLUDE_BUILD_MODULES= KERNEL_UNAME='$kernel' SYSSRC='$headers' modules
+ objcopy -O binary -j .text '$module' '<temporary directory>/stock.text' # refuse if unreadable
+ objcopy -O binary -j .text '<temporary directory>/tree/nvidia.ko' '<temporary directory>/built.text' # refuse if unreadable
+ cmp -l '<temporary directory>/stock.text' '<temporary directory>/built.text' # identical: with config and the patched plugin copy in place (a copy with plugin-mixed only is replaced) and the backup '$backup' differing at exactly the eight offsets, write '$record' for this module and exit 0, else refuse; otherwise require exactly the eight .text offsets 0x14a7c4-0x14a7c7, 0xa16708, 0xa16709, 0xd8966f, 0xd89670
+ geforce-sriov-patch ada apply '<stock plugin>' '<temporary directory>/patched-plugin.mixed' # plugin-mixed, unless the plugin already carries it
+ sha256sum '<temporary directory>/patched-plugin.mixed' # require $mixed_plugin
+ geforce-sriov-patch ada apply '<temporary directory>/patched-plugin.mixed' '<temporary directory>/patched-plugin' # plugin-div4, unless the plugin already carries it
+ sha256sum '<temporary directory>/patched-plugin' # require $patched_plugin
+ mkdir -p '$state/build' '$state/stock/$kernel'
+ rm -rf '$build' # a leftover of an earlier run
+ mv '<temporary directory>/tree' '$build'
+ cp -p '$module' '$backup' # require the module's SHA-256
+ cp -p '$build/nvidia.ko' '$module'
+ depmod -a '$kernel'
+ readelf -n '$module'
+ write '$record' # kernel, module_sha256, module_build_id, stock_sha256
+ install the patched plugin copy (mode 0755) and update '<plugin directory>/libnvidia-vgpu.so' symlink
+ write '$config' # options nvidia NVreg_EnableGpuFirmware=0, unless another file already sets it to 0
EOF
    exit 0
fi

# Install passes its --nvidia-source DIR here as --source DIR.
nvidia_source_hint='give the kernel source tree (/usr/src/nvidia-<version>) or an extracted package with install --nvidia-source DIR (this script: --source DIR)'
[ -d "$src" ] || refuse "source tree missing: $src; $nvidia_source_hint"
# DIR may be the kernel source tree (/usr/src/nvidia-<version>) or the root of an extracted .run
# package, whose tree is in kernel/. Accept both rather than making the user know which they have.
if [ ! -f "$src/nvidia/nv-kernel.o_binary" ] && [ -f "$src/kernel/nvidia/nv-kernel.o_binary" ]; then
    src=$src/kernel
fi
[ -f "$src/nvidia/nv-kernel.o_binary" ] || refuse "vendor object missing: $src/nvidia/nv-kernel.o_binary; $nvidia_source_hint"
# The patcher is installed next to this script; install runs it with a fixed PATH that need not
# hold the install prefix, so look beside the script first, then on PATH.
patcher=${0%/*}/geforce-sriov-patch
if [ ! -f "$patcher" ] || [ ! -x "$patcher" ]; then
    patcher=$(command -v geforce-sriov-patch 2>/dev/null) ||
        refuse "geforce-sriov-patch is not installed next to $0 or on PATH"
fi
for tool in sha256sum cut make cmp awk readelf objcopy cp mv mkdir mktemp ln rm chmod stat depmod sed wc; do
    command -v "$tool" >/dev/null 2>&1 || refuse "required command missing: $tool"
done
actual=$(sha "$src/nvidia/nv-kernel.o_binary") || fail 'could not hash vendor object'
[ "$actual" = "$stock_object" ] || refuse "vendor object SHA-256 is $actual; expected $stock_object"
[ -f "$headers/Makefile" ] || refuse "kernel headers missing: $headers/Makefile"
# The installed module's path depends on the distribution and on DKMS, so ask the kernel's own tool.
module=$(modinfo -k "$kernel" -n nvidia 2>/dev/null || true)
[ -n "$module" ] && [ "$(modinfo -k "$kernel" -F vermagic nvidia 2>/dev/null | awk '{print $1}')" = "$kernel" ] || module=
[ -n "$module" ] || refuse "installed nvidia module not found for $kernel (modinfo -k $kernel -n nvidia)"
[ -f "$module" ] && [ ! -L "$module" ] || refuse "stock module missing or not a regular file: $module"
# NVIDIA's open flavour carries this licence; this stack builds the proprietary one from nv-kernel.o_binary.
case $(modinfo -k "$kernel" -F license nvidia 2>/dev/null || true) in
    *'Dual MIT/GPL'*)
        refuse "$module is NVIDIA's open flavour (licence Dual MIT/GPL). This stack needs the proprietary one. Remove NVIDIA's driver (for example with nvidia-uninstall), install the vGPU host package again with --kernel-module-type=proprietary, then run install again" ;;
esac
# The stock plugin: the one regular libnvidia-vgpu.so.580.159.01 in the library directories, with the stock
# digest, or a digest this step's patches lead to. Sets plugin, plugin_hash, and copy and link beside it.
find_plugin() {
    plugin=
    for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
        candidate=$dir/libnvidia-vgpu.so.580.159.01
        if [ -f "$candidate" ] && [ ! -L "$candidate" ]; then
            [ -z "$plugin" ] || refuse 'the stock plugin is in more than one library directory'
            plugin=$candidate
        fi
    done
    [ -n "$plugin" ] || refuse 'libnvidia-vgpu.so.580.159.01 not found'
    plugin_hash=$(sha "$plugin") || fail 'could not hash plugin'
    case $plugin_hash in
        "$stock_plugin"|"$mixed_plugin"|"$patched_plugin") ;;
        *) refuse "plugin SHA-256 is $plugin_hash; expected $stock_plugin, $mixed_plugin or $patched_plugin" ;;
    esac
    copy=${plugin%/*}/libnvidia-vgpu.so.580.159.01.geforce-sriov
    link=${plugin%/*}/libnvidia-vgpu.so
}
# Writes the patched plugin copy to $1 from $plugin: the patcher's rows plugin-mixed and then plugin-div4,
# each one the plugin does not carry yet, and each result checked against its recorded digest.
make_copy() {
    from=$plugin
    if [ "$plugin_hash" = "$stock_plugin" ]; then
        run "$patcher" ada apply "$plugin" "$1.mixed"
        actual=$(sha "$1.mixed") || fail 'could not hash the plugin after plugin-mixed'
        [ "$actual" = "$mixed_plugin" ] || refuse "plugin SHA-256 after plugin-mixed is $actual; expected $mixed_plugin"
        from=$1.mixed
    fi
    if [ "$plugin_hash" = "$patched_plugin" ]; then
        run cp -p "$plugin" "$1"
    else
        run "$patcher" ada apply "$from" "$1"
    fi
    actual=$(sha "$1") || fail 'could not hash the patched plugin copy'
    [ "$actual" = "$patched_plugin" ] || refuse "patched plugin copy SHA-256 is $actual; expected $patched_plugin"
}
# Puts the patched plugin copy $1 in place of an installed copy that is not it (an earlier install's copy
# with plugin-mixed only), through a new name in the same directory, and points the link at it.
replace_copy() {
    [ ! -d "$copy" ] && [ ! -d "$copy.new" ] && [ ! -d "$link" ] || refuse 'an installation target is a directory'
    run rm -f "$copy.new"
    run cp -p "$1" "$copy.new"
    run chmod 0755 "$copy.new"
    run mv -f "$copy.new" "$copy"
    run ln -sfn "${copy##*/}" "$link"
    printf 'geforce-sriov-driver: installed the patched plugin copy %s (SHA-256 %s)\n' "$copy" "$patched_plugin"
}
# A rerun on a host that is already prepared for this kernel is a no-op, not a refusal, once the patched
# plugin copy is in place; any other copy is replaced with it.
if [ -f "$record" ] && [ ! -L "$record" ]; then
    rec_kernel=$(sed -n 's/^kernel=//p' "$record")
    rec_hash=$(sed -n 's/^module_sha256=//p' "$record")
    if [ "$rec_kernel" = "$kernel" ] && [ "$rec_hash" = "$(sha "$module")" ]; then
        patched=0
        for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
            candidate=$dir/libnvidia-vgpu.so.580.159.01.geforce-sriov
            if [ -f "$candidate" ] && [ ! -L "$candidate" ] && [ "$(sha "$candidate")" = "$patched_plugin" ]; then
                patched=1
            fi
        done
        if [ "$patched" -eq 1 ]; then
            printf 'geforce-sriov-driver: already prepared for %s (module %s); nothing to do\n' "$kernel" "$rec_hash"
            for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
                copy_mode "$dir/libnvidia-vgpu.so.580.159.01.geforce-sriov"
            done
            exit 0
        fi
        printf 'geforce-sriov-driver: already prepared for %s (module %s); replacing the plugin copy, which is not the patched one\n' "$kernel" "$rec_hash"
        find_plugin
        work=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") || fail 'cannot create temporary directory'
        trap 'rm -rf "$work"' 0
        trap 'exit 1' HUP INT TERM
        make_copy "$work/patched-plugin"
        replace_copy "$work/patched-plugin"
        exit 0
    fi
fi
stock_hash=$(sha "$module") || fail "could not hash $module"
find_plugin
# Another modprobe file may set the option already. Set to 0, the value this stack needs, it is kept and
# this script writes no file of its own; any other value refuses. Every `options nvidia` line counts,
# with the option among any other tokens on it.
kept=
for conf in "$root"/etc/modprobe.d/*.conf; do
    [ -f "$conf" ] && [ "$conf" != "$config" ] || continue
    verdict=$(awk '$1 == "options" && $2 == "nvidia" {
            for (i = 3; i <= NF; i++) if ($i ~ /^NVreg_EnableGpuFirmware(=|$)/) {
                v = $i; sub(/^[^=]*=?/, "", v)
                if (v != "0") { print "other " v; other = 1; exit }
                zero = 1
            }
        }
        END { if (zero && !other) print "zero" }' "$conf") || refuse "could not read $conf"
    case $verdict in
        zero) [ -n "$kept" ] || kept=$conf ;;
        other*) refuse "$conf sets NVreg_EnableGpuFirmware=${verdict#other }; this stack needs 0" ;;
    esac
done

work=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") || fail 'cannot create temporary directory'
committing=0
module_changed=0
# Backup targets and their existence without following symlinks.
save() {
    if [ -e "$2" ] || [ -L "$2" ]; then
        cp -a "$2" "$work/save-$1"
        : >"$work/had-$1"
    fi
}
restore() {
    if [ -f "$work/had-$1" ]; then
        rm -f "$2"
        cp -a "$work/save-$1" "$2" || printf 'geforce-sriov-driver: could not restore %s\n' "$2" >&2
    else
        rm -f "$2" || printf 'geforce-sriov-driver: could not remove %s\n' "$2" >&2
    fi
}
cleanup() {
    status=$?
    trap - 0 HUP INT TERM
    if [ "$status" -ne 0 ] && [ "$committing" -eq 1 ]; then
        set +e
        restore config "$config"
        restore link "$link"
        restore copy "$copy"
        restore record "$record"
        restore module "$module"
        restore backup "$backup"
        rm -rf "$build"
        if [ "$module_changed" -eq 1 ]; then depmod -a "$kernel" || printf 'geforce-sriov-driver: rollback depmod failed for %s\n' "$kernel" >&2; fi
    fi
    rm -rf "$work"
    if [ "$status" -ne 0 ] && [ "$status" -ne 3 ]; then status=1; fi
    exit "$status"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

run cp -a "$src" "$work/tree"
run "$patcher" apply "$work/tree/nvidia/nv-kernel.o_binary" "$work/scheduler"
run "$patcher" ada apply "$work/scheduler" "$work/gate"
run "$patcher" ada apply "$work/gate" "$work/patched"
actual=$(sha "$work/patched") || fail 'could not hash patched object'
[ "$actual" = "$patched_object" ] || refuse "patched object SHA-256 is $actual; expected $patched_object"
run cp "$work/patched" "$work/tree/nvidia/nv-kernel.o_binary"
run make -C "$work/tree" NV_EXCLUDE_BUILD_MODULES= "KERNEL_UNAME=$kernel" "SYSSRC=$headers" modules
built=$work/tree/nvidia.ko
[ -f "$built" ] || fail "built module missing: $built"
# A rebuild differs outside .text by design (build-id, banner, DWARF), so compare only the
# executable section of each module. cmp reports one-based decimal positions; the expected
# offsets are zero-based and relative to the start of .text.
show objcopy -O binary -j .text "$module" "$work/stock.text"
objcopy -O binary -j .text "$module" "$work/stock.text" || refuse "could not read .text from $module"
show objcopy -O binary -j .text "$built" "$work/built.text"
objcopy -O binary -j .text "$built" "$work/built.text" || refuse "could not read .text from $built"
[ -s "$work/stock.text" ] || refuse "module has no .text section: $module"
[ -s "$work/built.text" ] || refuse "module has no .text section: $built"
stock_size=$(wc -c <"$work/stock.text")
built_size=$(wc -c <"$work/built.text")
if [ "$stock_size" -ne "$built_size" ]; then
    if [ "$stock_size" -lt "$built_size" ]; then size=$stock_size; else size=$built_size; fi
    refuse "module .text sizes differ; first unexpected offset: $(printf '0x%x' "$size")"
fi
show cmp -l "$work/stock.text" "$work/built.text"
cmp_status=0
cmp -l "$work/stock.text" "$work/built.text" >"$work/diff" 2>"$work/cmp-error" || cmp_status=$?
[ "$cmp_status" -le 1 ] || refuse "could not compare .text sections: $(cat "$work/cmp-error")"
printf '%s\n' 0x14a7c4 0x14a7c5 0x14a7c6 0x14a7c7 0xa16708 0xa16709 0xd8966f 0xd89670 >"$work/expected"
# Identical .text means the installed module is already this build. The record names this kernel
# and module only when the check above matched, so here it is missing or names another kernel or
# module (one record serves every kernel): with the rest of the installation in place, record this
# module for this kernel, taking stock_sha256 from this kernel's backup once its .text differs from
# this build at exactly the eight offsets. The installed plugin copy must be the patched one, or an
# earlier install's copy with plugin-mixed only, which is replaced. Otherwise refuse, because
# continuing would back up this patched module over the stock backup.
if [ ! -s "$work/diff" ]; then
    incomplete="the installed module $module is already this build, but $record, $config or $copy is missing or wrong. Reinstall the stock module first, or install with --driver=skip to keep this module as it is"
    copy_hash=
    if [ -f "$copy" ] && [ ! -L "$copy" ]; then
        copy_hash=$(sha "$copy") || fail "could not hash $copy"
    fi
    case $copy_hash in "$patched_plugin"|"$mixed_plugin") ;; *) refuse "$incomplete" ;; esac
    { [ -f "$config" ] || [ -n "$kept" ]; } &&
        [ -f "$backup" ] && [ ! -L "$backup" ] && [ ! -L "$record" ] && [ ! -d "$record" ] || refuse "$incomplete"
    objcopy -O binary -j .text "$backup" "$work/backup.text" 2>/dev/null || refuse "$incomplete"
    { cmp -l "$work/backup.text" "$work/built.text" 2>/dev/null || true; } | awk '{printf "0x%x\n", $1 - 1}' >"$work/backup-offsets"
    cmp -s "$work/expected" "$work/backup-offsets" || refuse "$incomplete"
    backup_hash=$(sha "$backup") || fail "could not hash $backup"
    installed_id=$(readelf -n "$module" | awk '$1 == "Build" && $2 == "ID:" {print $3; exit}')
    [ -n "$installed_id" ] || refuse "installed module has no GNU build-id: $module"
    [ "$copy_hash" = "$patched_plugin" ] || make_copy "$work/patched-plugin"
    printf '%s\n' "kernel=$kernel" "module_sha256=$stock_hash" "module_build_id=$installed_id" "stock_sha256=$backup_hash" >"$record.tmp" &&
        mv -f "$record.tmp" "$record" || { rm -f "$record.tmp"; fail "could not write $record"; }
    printf 'geforce-sriov-driver: recorded the installed module for %s\n' "$kernel"
    if [ "$copy_hash" = "$patched_plugin" ]; then
        copy_mode "$copy"
    else
        replace_copy "$work/patched-plugin"
    fi
    exit 0
fi
awk '{printf "0x%x\n", $1 - 1}' "$work/diff" >"$work/offsets"
if ! cmp -s "$work/expected" "$work/offsets"; then
    unexpected=$(awk 'NR==FNR {expected[$0]=1; next} !($0 in expected) {print; exit}' "$work/expected" "$work/offsets")
    [ -n "$unexpected" ] || unexpected='none (expected offset missing)'
    refuse "module .text differences are not the eight expected offsets; first unexpected offset: $unexpected"
fi
make_copy "$work/patched-plugin"
show readelf -n "$built"
build_id=$(readelf -n "$built" | awk '$1 == "Build" && $2 == "ID:" {print $3; exit}')
[ -n "$build_id" ] || refuse "built module has no GNU build-id: $built"
module_hash=$(sha "$built") || fail 'could not hash built module'
[ ! -d "$copy" ] && [ ! -d "$link" ] && [ ! -d "$record" ] && [ ! -d "$backup" ] && [ ! -d "$config" ] || refuse 'an installation target is a directory'
save module "$module"
save backup "$backup"
save record "$record"
save copy "$copy"
save link "$link"
save config "$config"
committing=1
run mkdir -p "$state/build" "$state/stock/$kernel" "${config%/*}"
run rm -rf "$build"
run mv "$work/tree" "$build"
run rm -f "$backup"
run cp -p "$module" "$backup"
backup_hash=$(sha "$backup") || fail "could not hash $backup"
[ "$backup_hash" = "$stock_hash" ] || fail "the backup $backup has SHA-256 $backup_hash; $module has $stock_hash"
module_changed=1
run cp -p "$build/nvidia.ko" "$module"
run depmod -a "$kernel"
run rm -f "$record"
show printf '%s\n' "kernel=$kernel" "module_sha256=$module_hash" "module_build_id=$build_id" "stock_sha256=$stock_hash"
printf '%s\n' "kernel=$kernel" "module_sha256=$module_hash" "module_build_id=$build_id" "stock_sha256=$stock_hash" >"$record"
run rm -f "$copy"
run cp -p "$work/patched-plugin" "$copy"
# Executable, like NVIDIA's own copies of the library.
run chmod 0755 "$copy"
run ln -sfn "${copy##*/}" "$link"
copy_hash=$(sha "$copy") || fail "could not hash $copy"
printf 'geforce-sriov-driver: patched plugin copy installed: %s (sha256 %s)\n' "$copy" "$(printf '%s\n' "$copy_hash" | cut -c 1-16)"
if [ -n "$kept" ]; then
    printf 'geforce-sriov-driver: kept %s: it already sets NVreg_EnableGpuFirmware=0\n' "$kept"
else
    run rm -f "$config"
    show printf '%s\n' 'options nvidia NVreg_EnableGpuFirmware=0'
    printf '%s\n' 'options nvidia NVreg_EnableGpuFirmware=0' >"$config"
fi
printf '%s\n' 'geforce-sriov-driver: NVIDIA'\''s module and plugin are prepared'
