#!/bin/sh
# SPDX-License-Identifier: MIT
# Stage and verify the driver before changing installed files.
set -eu
# Every file and directory this script makes is root's and nobody else's to write, whatever the
# umask of the caller.
umask 022

src=
kernel=
root=
dry=0
restore=0
retained=0
kernel_given=0
display=0
host_cuda=0
# --ada-selected: a selected GPU is an Ada card (the CLI passes it). The build then carries the ada-placement
# row, so that an Ada VM takes one contiguous free piece of video memory when it starts. The value is recorded
# with the build as ada_placement=1 or 0; a build for another value is made again.
ada_selected=0
driver_file=
driver_work=
driver_package_sha=none

companion_file=
companion_source=
nvkms_source=
source_given=0
# --cuda-stage DIR: with --host-cuda, NVIDIA's CUDA libraries are copied from the Linux driver package
# into DIR, for the CLI to install. Without it they are not looked for.
cuda_stage=
# --desktop-stage DIR: with --display, NVIDIA's graphics libraries are copied from the Linux driver package
# into DIR, for the CLI to install (host desktop). Without it they are not looked for.
desktop_stage=
# The version of the Linux driver package's programs that nvidia.ko accepts besides its own (client_accept):
# set by a display build whose Linux driver package is not of the host's version, and recorded.
client_accept=
# The Linux driver package's version when the patch above did not match this source: recorded, so that the build is not repeated for it.
client_accept_unmatched=
while [ "$#" -gt 0 ]; do
    case $1 in
        --dry-run) dry=1 ;;
        --restore) restore=1 ;;
        # --retained: an input that is not given is the one an earlier successful run kept.
        --retained) retained=1 ;;
        --display) display=1 ;;
        --no-display) display=0 ;;
        --host-cuda) host_cuda=1 ;;
        --no-host-cuda) host_cuda=0 ;;
        --ada-selected) ada_selected=1 ;;
        # --root DIR: every fixed path below DIR instead of / (the tests' fixture hosts).
        --source|--kernel|--root|--driver-file|--companion-file|--companion-source|--nvkms-source|--cuda-stage|--desktop-stage)
            [ "$#" -ge 2 ] || { printf '%s requires a value\n' "$1" >&2; exit 2; }
            case $1 in
                --source) src=$2; source_given=1 ;;
                --kernel) kernel=$2; kernel_given=1 ;;
                --root) root=$2 ;;
                --driver-file) driver_file=$2 ;;
                --companion-file) companion_file=$2 ;;
                --companion-source) companion_source=$2 ;;
                --nvkms-source) nvkms_source=$2 ;;
                --cuda-stage) cuda_stage=$2 ;;
                --desktop-stage) desktop_stage=$2 ;;
            esac
            shift ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done
[ -z "$companion_file" ] || [ -z "$companion_source" ] || {
    printf '%s\n' '--companion-file and --companion-source are mutually exclusive' >&2
    exit 2
}
# A path becomes part of a line in a record and of a message: none may hold a control character.
case $cuda_stage in ''|/*) ;; *) printf 'invalid --cuda-stage: %s\n' "$cuda_stage" >&2; exit 2 ;; esac
case $desktop_stage in ''|/*) ;; *) printf 'invalid --desktop-stage: %s\n' "$desktop_stage" >&2; exit 2 ;; esac
for given in "$src" "$root" "$driver_file" "$companion_file" "$companion_source" "$nvkms_source" "$cuda_stage" "$desktop_stage"; do
    [ "$(printf '%s' "$given" | LC_ALL=C tr -d '\000-\037\177')" = "$given" ] ||
        { printf '%s\n' 'a path given to this script holds a control character, such as a line break; rename the file or directory' >&2; exit 2; }
done
[ -n "$kernel" ] || kernel=$(uname -r)
case $kernel in ''|.|..|*/*) printf 'invalid kernel: %s\n' "$kernel" >&2; exit 2;; esac
case $root in ''|/*) ;; *) printf 'invalid root: %s\n' "$root" >&2; exit 2;; esac
state=$root/var/lib/geforce-sriov
build=$state/build/$kernel
module=
headers=$root/lib/modules/$kernel/build
config=$root/etc/modprobe.d/geforce-sriov-nvidia.conf
# The published record names one kernel: the one of the latest build. The record of every other
# kernel this script built for is kept under $records, one file per kernel. $record is the record
# this run reads, the one of its own kernel wherever it is; a commit writes $published. Nothing is
# moved before a commit is certain, so a run that refuses leaves every file as it was.
published=$state/driver-build
record=$published
records=$state/driver-builds
archived=
backup=$state/stock/$kernel/nvidia.ko
# The journal of a commit: the record it will publish, plus what is needed to finish or undo it.
journal=$published.gs-new
# What a successful run keeps so that the same build can be made again for another kernel: a copy
# of the Linux driver package and of the public source, and the index that names them and the host tree.
inputs=$state/inputs
inputs_index=$inputs/index
given_companion_file=$companion_file
given_companion_source=$companion_source
given_nvkms=$nvkms_source
kept_companion=0
kept_nvkms=0
# What this run changed, for the line `install_result=` that the CLI reads.
recovered=0
mode_fixed=0

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


# ---- the commit and its journal ------------------------------------------------
#
# A commit replaces several files: each module, the plugin copy and its link, the modprobe file and
# the record. A power loss or a SIGKILL can stop it between any two of them. So that the next run
# can tell what happened, the commit is journaled:
#
#   1. Everything it needs is put under $state first and synced: the built modules ($build), the
#      patched plugin, and a copy of each file as it is before this commit ($state/txn). Those
#      copies are this commit's own. The stock backups under $state/stock are a different thing:
#      they hold NVIDIA's files for --restore and are not what an upgrade replaces.
#   2. The journal is written: the record to be published, plus journal_* lines that name those
#      files. From here on the commit is recoverable.
#   3. commit_files installs what the journal names. Each file is written under a temporary name,
#      synced and renamed, so an installed file is always complete, old or new.
#   4. The record is published last, and the journal is removed.
#
# A run that finds a journal first finishes that commit from the staged files (recover_commit). When
# a staged file is gone it puts back the files as they were before the commit, from the copies in
# $state/txn; the earlier record was never touched, so it describes that state again. A file that
# is neither what the commit found nor what it built was changed by someone else since: then, and
# when a needed copy is gone, nothing is changed, the journal is kept and the run refuses.
jv() { gs_record_get "$journal" "$1"; }
# The directory entry of a rename is made durable.
sync_dir() { sync "$1" 2>/dev/null || sync; }
# A complete file at DEST: written beside it, synced, renamed.
# With a digest, the copy must have it before it takes the place of DEST.
put_file() {
    run cp -p "$1" "$2.gs-new" || return 1
    if [ -n "${3:-}" ] && [ "$(sha "$2.gs-new")" != "$3" ]; then
        rm -f "$2.gs-new"
        printf 'geforce-sriov-driver: %s is not the file that was built: it does not have SHA-256 %s\n' "$1" "$3" >&2
        return 1
    fi
    run sync "$2.gs-new" && run mv -f "$2.gs-new" "$2" && sync_dir "${2%/*}"
}
# The fixtures cut a commit off at a named point, as a power loss would. Only with --root, which
# the installed CLI never passes, and it runs this script with a fixed environment.
fault() {
    [ -n "$root" ] && [ "${GS_DRIVER_FAULT:-}" = "$1" ] || return 0
    kill -KILL $$
}
in_place() { [ -f "$1" ] && [ ! -L "$1" ] && [ "$(sha "$1")" = "$2" ]; }
journal_modules() { gs_record_modules "$journal"; }
# Where the journal's modules were built. A journal of the first form of this commit names no
# directory; that form always built into the kernel's build directory.
journal_build() {
    j_dir=$(jv journal_build)
    [ -n "$j_dir" ] || j_dir=$state/build/$(jv kernel)
    printf '%s' "$j_dir"
}

# What a destination holds, against the journal: `new` (what the commit built), `before` (what the
# commit found, which may be nothing), or `foreign`. A journal without the before lines knows only new.
dest_state() {
    if in_place "$1" "$2"; then
        printf new
    elif [ "$3" = present ] && in_place "$1" "$4"; then
        printf before
    elif [ "$3" = absent ] && [ ! -e "$1" ] && [ ! -L "$1" ]; then
        printf before
    else
        printf foreign
    fi
}
module_state() {
    dest_state "$(jv "module_$1_path")" "$(jv "module_$1_sha256")" "$(jv "journal_before_$1")" "$(jv "journal_before_$1_sha256")"
}
plugin_state() {
    dest_state "$(jv journal_plugin_copy)" "$(jv plugin_sha256)" "$(jv journal_plugin_previous)" "$(jv journal_plugin_previous_sha256)"
}

# archive_published KERNEL: before a record for KERNEL replaces the published one, a published
# record that names another kernel is copied under that kernel's name.
archive_published() {
    other=
    [ ! -f "$published" ] || [ -L "$published" ] || other=$(gs_record_get "$published" kernel)
    case $other in ''|.|..|*/*|"$1") return 0 ;; esac
    mkdir -p "$records" && cp -p "$published" "$records/$other.gs-new" && sync "$records/$other.gs-new" &&
        mv -f "$records/$other.gs-new" "$records/$other" && sync_dir "$records" ||
        fail "could not keep the build record of $other under $records"
    archived=$records/$other
}

# Installs every file the journal names that is not in place yet, then publishes the record.
# The same steps finish a new commit and one that was cut off. Everything that needs a staged file
# comes first; the modules of the earlier build that this one drops go last, so that once they are
# touched the commit can always be finished and never has to be undone.
commit_files() {
    j_kernel=$(jv kernel)
    j_build=$(journal_build)
    first=1
    for flat in $(journal_modules); do
        disk=$(printf '%s' "$flat" | tr '_' '-')
        path=$(jv "module_${flat}_path")
        if in_place "$path" "$(jv "module_${flat}_sha256")"; then
            continue
        fi
        module_changed=1
        put_file "$j_build/$disk.ko" "$path" "$(jv "module_${flat}_sha256")" || fail "could not install $path"
        [ "$first" -eq 0 ] || fault after-first-module
        first=0
        if [ "$flat" = nvidia ]; then fault after-core; else fault after-companion; fi
    done
    fault before-plugin
    j_copy=$(jv journal_plugin_copy)
    if [ -n "$j_copy" ]; then
        j_link=$(jv journal_plugin_link)
        if ! in_place "$j_copy" "$(jv plugin_sha256)"; then
            run rm -f "$j_copy.new"
            run cp -p "$(jv journal_plugin_staged)" "$j_copy.new"
            # Executable, like NVIDIA's own copies of the library.
            run chmod 0755 "$j_copy.new"
            run sync "$j_copy.new"
            run mv -f "$j_copy.new" "$j_copy"
        fi
        run chmod 0755 "$j_copy"
        run ln -sfn "${j_copy##*/}" "$j_link"
        sync_dir "${j_copy%/*}"
        fault after-plugin
        if [ "$(jv journal_config)" = write ]; then
            run rm -f "$config"
            show printf '%s\n' 'options nvidia NVreg_EnableGpuFirmware=0'
            printf '%s\n' 'options nvidia NVreg_EnableGpuFirmware=0' >"$config"
            sync "$config"
        fi
    fi
    # Modules of the earlier build that this one no longer has.
    for name in $(jv journal_retire); do
        path=$(jv "journal_retire_${name}_path")
        want=$(jv "journal_retire_${name}_sha256")
        disk=${path##*/}
        if [ ! -e "$path" ] && [ ! -L "$path" ]; then
            run rm -f "$state/stock/$j_kernel/$disk"
            continue
        fi
        if [ -z "$want" ] || [ "$(sha "$path")" != "$want" ]; then
            printf 'geforce-sriov-driver: kept %s: it is not the module this script installed\n' "$path"
            continue
        fi
        if [ "$(jv "journal_retire_${name}_previous")" = present ] && [ -f "$state/stock/$j_kernel/$disk" ] &&
            [ "$(sha "$state/stock/$j_kernel/$disk")" = "$(jv "journal_retire_${name}_backup_sha256")" ]; then
            put_file "$state/stock/$j_kernel/$disk" "$path" || fail "could not put back $path"
        else
            run rm -f "$path"
        fi
        fault after-retire
        run rm -f "$state/stock/$j_kernel/$disk"
    done
    run depmod -a "$j_kernel"
    fault before-record
    # Only now is the record published: every file it names is in place. A build for a kernel that is
    # not running leaves the published record alone when that names the running kernel: it is kept
    # under that kernel's own name. Otherwise a published record of another kernel is first kept under
    # that kernel's name; the copy of this kernel's, if there is one, is older than what is published now.
    landed=$published
    if [ "$(jv journal_land)" = kept ]; then
        landed=$records/$j_kernel
        mkdir -p "$records" || fail "could not create $records"
    else
        archive_published "$j_kernel"
    fi
    sed '/^journal_/d' "$journal" >"$landed.gs-tmp" && sync "$landed.gs-tmp" && mv -f "$landed.gs-tmp" "$landed" ||
        fail "could not write $landed"
    archived=
    [ "$landed" != "$published" ] || rm -f "$records/$j_kernel"
    sync_dir "${landed%/*}"
    [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
    sync_dir "$state"
    rm -f "$journal" "$state/plugin-copy.gs-new"
    rm -rf "$state/txn"
    sync_dir "$state"
}

# Why an interrupted commit can be neither finished nor undone; nothing was changed.
stuck() {
    refuse "an earlier run was interrupted while it installed its files, and it can be neither completed nor undone: $1. Nothing was changed, and $journal is kept. Check that file, or reinstall NVIDIA's vGPU host driver so that NVIDIA's own files are back; then delete $journal and run install again"
}

# Puts every file back as it was before an interrupted commit, from that commit's own copies. The
# caller has checked that no destination is foreign. Every check comes before the first change.
rollback_commit() {
    j_kernel=$(jv kernel)
    j_txn=$(jv journal_txn)
    [ -n "$j_txn" ] || stuck "the journal names no copies of the files that run replaced"
    plan=
    for flat in $(journal_modules); do
        disk=$(printf '%s' "$flat" | tr '_' '-')
        [ "$(module_state "$flat")" = new ] || continue
        if [ "$(jv "journal_before_${flat}")" = present ]; then
            in_place "$j_txn/$disk.ko" "$(jv "journal_before_${flat}_sha256")" ||
                stuck "the copy $j_txn/$disk.ko of a replaced module is missing or is not the file the journal names"
            plan="$plan put:$flat"
        else
            plan="$plan del:$flat"
        fi
    done
    j_copy=$(jv journal_plugin_copy)
    plugin_plan=none
    if [ -n "$j_copy" ] && [ "$(plugin_state)" = new ]; then
        if [ "$(jv journal_plugin_previous)" != present ]; then
            plugin_plan=del
        elif [ "$(jv journal_plugin_previous_sha256)" != "$(jv plugin_sha256)" ]; then
            in_place "$j_txn/plugin-copy" "$(jv journal_plugin_previous_sha256)" ||
                stuck "the copy $j_txn/plugin-copy of the replaced plugin copy is missing or is not the file the journal names"
            plugin_plan=put
        fi
    fi
    for step in $plan; do
        flat=${step#*:}
        disk=$(printf '%s' "$flat" | tr '_' '-')
        path=$(jv "module_${flat}_path")
        case $step in
            put:*) put_file "$j_txn/$disk.ko" "$path" || fail "could not put back $path" ;;
            del:*) run rm -f "$path" ;;
        esac
    done
    if [ -n "$j_copy" ]; then
        j_link=$(jv journal_plugin_link)
        case $plugin_plan in
            put) put_file "$j_txn/plugin-copy" "$j_copy" || fail "could not put back $j_copy" ;;
            del) run rm -f "$j_copy" ;;
        esac
        link_previous=$(jv journal_plugin_link_previous)
        if [ "$link_previous" = absent ] || [ -z "$link_previous" ]; then
            run rm -f "$j_link"
        else
            run ln -sfn "$link_previous" "$j_link"
        fi
        if [ "$(jv journal_config)" = write ] && [ "$(jv journal_config_previous)" = absent ] && [ -f "$config" ] &&
            [ ! -L "$config" ] && [ "$(cat "$config")" = 'options nvidia NVreg_EnableGpuFirmware=0' ]; then
            run rm -f "$config"
        fi
    fi
    run depmod -a "$j_kernel"
    rm -f "$journal" "$state/plugin-copy.gs-new"
    rm -rf "$state/txn"
    sync_dir "$state"
}

# ---- NVIDIA's CUDA libraries, for host CUDA ----------------------------------
#
# A CUDA program on the host needs NVIDIA's CUDA library, libcuda. NVIDIA's vGPU host package has
# none; the Linux driver package, which nvidia-uvm is built from, has it. With --host-cuda and
# --cuda-stage DIR the libraries are copied into DIR, with an index of their digests and of the
# names the loader knows them by, and the CLI installs them from there. The set is what NVIDIA's
# installer installs for CUDA (the CUDA_LIB lines of the package's .manifest, and the library they
# load by its full name): libcuda itself, the compilers it loads when a program gives it PTX or NVVM
# IR (libnvidia-ptxjitcompiler, libnvidia-nvvm, libnvidia-nvvm70), libnvidia-gpucomp, and the
# debugger's library. Most carry the package's version in their name; libnvidia-nvvm70 does not.
# Nothing else is taken: a library that the host package installs in its own version (libnvidia-ml,
# which nvidia-smi uses and which checks the driver's version) must not get a second copy.
# Nothing here is a reason to refuse a build: a set that cannot be taken is one warning.
cuda_names='libcuda libnvidia-ptxjitcompiler libnvidia-nvvm libnvidia-nvvm70 libnvidia-gpucomp libcudadebugger'
# stage_cuda DIR: DIR is the Linux driver package as extracted, or its kernel directory. $cuda_why says why not.
stage_cuda() {
    cuda_why=
    c_src=
    for c_try in "$1" "$1/.."; do
        for c_lib in "$c_try"/libcuda.so.[0-9]*; do
            if [ -f "$c_lib" ] && [ ! -L "$c_lib" ]; then
                c_src=$c_try
                c_version=${c_lib##*/libcuda.so.}
                break 2
            fi
        done
    done
    [ -n "$c_src" ] || { cuda_why="it has no libcuda.so.<version> ($1)"; return 1; }
    case $c_version in ''|*[!0-9.]*) cuda_why="its libcuda has no version in its name (libcuda.so.$c_version)"; return 1 ;; esac
    rm -rf "$cuda_stage.gs-new"
    mkdir -p "$cuda_stage.gs-new" && chmod 0755 "$cuda_stage.gs-new" ||
        { cuda_why="$cuda_stage.gs-new could not be made"; return 1; }
    c_list=
    c_sonames=
    c_absent=
    printf 'version=%s\n' "$c_version" >"$cuda_stage.gs-new/index"
    for c_name in $cuda_names; do
        # By the package's version, or else the one file of that name with a version of its own.
        c_file=$c_name.so.$c_version
        if [ ! -f "$c_src/$c_file" ] || [ -L "$c_src/$c_file" ]; then
            c_file=
            for c_lib in "$c_src/$c_name".so.[0-9]*; do
                [ -f "$c_lib" ] && [ ! -L "$c_lib" ] || continue
                [ -z "$c_file" ] || { c_file=; break; }
                c_file=${c_lib##*/}
            done
        fi
        [ -n "$c_file" ] || { c_absent="$c_absent $c_name"; continue; }
        cp -- "$c_src/$c_file" "$cuda_stage.gs-new/$c_file" && chmod 0755 "$cuda_stage.gs-new/$c_file" &&
            c_sha=$(sha "$cuda_stage.gs-new/$c_file") ||
            { rm -rf "$cuda_stage.gs-new"; cuda_why="$c_file could not be copied to $cuda_stage"; return 1; }
        # The name programs ask the loader for. A file that states none is asked for by its own name.
        c_soname=$(readelf -d -- "$cuda_stage.gs-new/$c_file" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(lib[A-Za-z0-9_.+-]*\)\]$/\1/p' | sed -n '1p')
        [ -n "$c_soname" ] || c_soname=$c_file
        printf 'lib=%s %s %s\n' "$c_file" "$c_sha" "$c_soname" >>"$cuda_stage.gs-new/index"
        c_list="$c_list $c_file"
        c_sonames="$c_sonames $c_soname"
    done
    # What a library of the set links against from NVIDIA must be in the set too.
    for c_file in $c_list; do
        for c_need in $(readelf -d -- "$cuda_stage.gs-new/$c_file" 2>/dev/null |
            sed -n 's/.*(NEEDED).*\[\(lib\(nvidia-\|cuda\)[A-Za-z0-9_.+-]*\)\]$/\1/p'); do
            case " $c_sonames " in
                *" $c_need "*) ;;
                *) printf 'geforce-sriov-driver: warning: %s links against %s, which is not one of the CUDA libraries taken from the Linux driver package; a CUDA program may fail to start\n' "$c_file" "$c_need" >&2 ;;
            esac
        done
    done
    rm -rf "$cuda_stage" && mv "$cuda_stage.gs-new" "$cuda_stage" ||
        { rm -rf "$cuda_stage.gs-new"; cuda_why="$cuda_stage could not be written"; return 1; }
    printf "geforce-sriov-driver: NVIDIA's CUDA libraries %s are ready to be installed:%s\n" "$c_version" "$c_list"
    [ -z "$c_absent" ] ||
        printf 'geforce-sriov-driver: warning: the Linux driver package does not have%s; a CUDA program that needs it may fail\n' "$c_absent" >&2
}
cuda_not_staged() {
    printf "geforce-sriov-driver: warning: NVIDIA's CUDA libraries were not taken from the Linux driver package: %s\n" "$cuda_why" >&2
}
# After a build: the Linux driver package is extracted, or was given as a directory.
stage_cuda_built() {
    [ -n "$cuda_stage" ] && [ "$host_cuda" -eq 1 ] || return 0
    stage_cuda "$1" || cuda_not_staged
}

# ---- NVIDIA's graphics libraries, for the host desktop -----------------------
#
# With --display and --desktop-stage DIR the graphics libraries of a consumer driver are copied from
# the Linux driver package into DIR, with an index (version=, lib=, xmod=, etc=, gbm=; read by src/nvlibs.c),
# and the CLI installs them from there into geforce-sriov's own directories. The set is what NVIDIA's
# installer installs for GLX, EGL, Vulkan and GBM, without its copies of libglvnd (the distribution
# has them): the libraries and the two X modules, the JSON files that name them, NVIDIA's application
# profiles and its output-class file. A set that is not complete is not taken at all: half of a
# desktop driver is worse than none. Refused too when the libraries are not of the version the NVKMS
# core was stamped with: NVKMS compares its clients' versions exactly, and X would not start.
# Nothing here is a reason to refuse a build: a set that cannot be taken is one warning.
desktop_libs='libEGL_nvidia libGLX_nvidia libnvidia-glcore libnvidia-tls libnvidia-glsi libnvidia-glvkspirv libnvidia-gpucomp libGLESv2_nvidia libGLESv1_CM_nvidia libnvidia-eglcore libnvidia-allocator'
desktop_own_libs='libnvidia-egl-gbm libnvidia-egl-wayland libnvidia-egl-xcb libnvidia-egl-xlib'
desktop_etc='egl_vendor:10_nvidia.json egl_platform:15_nvidia_gbm.json egl_platform:10_nvidia_wayland.json egl_platform:20_nvidia_xcb.json egl_platform:20_nvidia_xlib.json vulkan_icd:nvidia_icd.json xconf:nvidia-drm-outputclass.conf'
# desk_take FILE: copy a file of the package into the staging directory; its digest in d_sha.
desk_take() {
    cp -- "$d_src/$1" "$d_new/$1" && chmod "$2" "$d_new/$1" && d_sha=$(sha "$d_new/$1") ||
        { rm -rf "$d_new"; desktop_why="$1 could not be copied to $desktop_stage"; return 1; }
}
# stage_desktop DIR VERSION: DIR is the Linux driver package as extracted, or its kernel directory; VERSION is
# the version the NVKMS core is stamped with. $desktop_why says why not.
stage_desktop() {
    desktop_why=
    d_src=
    for d_try in "$1" "$1/.."; do
        for d_lib in "$d_try"/libEGL_nvidia.so.[0-9]*; do
            if [ -f "$d_lib" ] && [ ! -L "$d_lib" ]; then
                d_src=$d_try
                d_version=${d_lib##*/libEGL_nvidia.so.}
                break 2
            fi
        done
    done
    [ -n "$d_src" ] || { desktop_why="it has no libEGL_nvidia.so.<version> ($1)"; return 1; }
    case $d_version in ''|*[!0-9.]*) desktop_why="its libEGL_nvidia has no version in its name (libEGL_nvidia.so.$d_version)"; return 1 ;; esac
    [ "$d_version" = "$2" ] ||
        { desktop_why="its graphics libraries are version $d_version and NVIDIA's NVKMS core is stamped $2, which compares the version of its clients exactly, so NVIDIA's X driver could not start"; return 1; }
    d_new=$desktop_stage.gs-new
    rm -rf "$d_new"
    mkdir -p "$d_new" && chmod 0755 "$d_new" || { desktop_why="$d_new could not be made"; return 1; }
    printf 'version=%s\n' "$d_version" >"$d_new/index"
    d_list=
    d_sonames=
    d_gbm=
    for d_name in $desktop_libs $desktop_own_libs; do
        d_file=$d_name.so.$d_version
        case " $desktop_own_libs " in
            *" $d_name "*)
                # A library with a version of its own: the one file of that name.
                d_file=
                for d_lib in "$d_src/$d_name".so.[0-9]*; do
                    [ -f "$d_lib" ] && [ ! -L "$d_lib" ] || continue
                    [ -z "$d_file" ] || { d_file=; break; }
                    d_file=${d_lib##*/}
                done ;;
        esac
        { [ -n "$d_file" ] && [ -f "$d_src/$d_file" ] && [ ! -L "$d_src/$d_file" ]; } ||
            { rm -rf "$d_new"; desktop_why="the Linux driver package has no $d_name"; return 1; }
        desk_take "$d_file" 0755 || return 1
        d_soname=$(readelf -d -- "$d_new/$d_file" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(lib[A-Za-z0-9_.+-]*\)\]$/\1/p' | sed -n '1p')
        [ -n "$d_soname" ] || d_soname=$d_file
        printf 'lib=%s %s %s\n' "$d_file" "$d_sha" "$d_soname" >>"$d_new/index"
        d_list="$d_list $d_file"
        d_sonames="$d_sonames $d_soname"
        [ "$d_name" != libnvidia-allocator ] || d_gbm=$d_soname
    done
    # The X driver and its GLX module.
    desk_take nvidia_drv.so 0755 || return 1
    printf 'xmod=drivers/nvidia_drv.so %s\n' "$d_sha" >>"$d_new/index"
    desk_take "libglxserver_nvidia.so.$d_version" 0755 || return 1
    printf 'xmod=extensions/libglxserver_nvidia.so.%s %s libglxserver_nvidia.so\n' "$d_version" "$d_sha" >>"$d_new/index"
    # The small files, and NVIDIA's application profiles, which carry the version in their name.
    for d_item in $desktop_etc "profile:nvidia-application-profiles-$d_version-rc" "profile:nvidia-application-profiles-$d_version-key-documentation"; do
        d_name=${d_item#*:}
        { [ -f "$d_src/$d_name" ] && [ ! -L "$d_src/$d_name" ]; } || { rm -rf "$d_new"; desktop_why="the Linux driver package has no $d_name"; return 1; }
        desk_take "$d_name" 0644 || return 1
        printf 'etc=%s %s %s\n' "${d_item%%:*}" "$d_name" "$d_sha" >>"$d_new/index"
    done
    [ -z "$d_gbm" ] || printf 'gbm=%s\n' "$d_gbm" >>"$d_new/index"
    # What a library of the set links against from NVIDIA must be in the set too.
    for d_file in $d_list; do
        for d_need in $(readelf -d -- "$d_new/$d_file" 2>/dev/null |
            sed -n 's/.*(NEEDED).*\[\(lib\(nvidia-\|GLX_nvidia\|EGL_nvidia\)[A-Za-z0-9_.+-]*\)\]$/\1/p'); do
            case " $d_sonames " in
                *" $d_need "*) ;;
                *) printf 'geforce-sriov-driver: warning: %s links against %s, which is not one of the graphics libraries taken from the Linux driver package; a program that draws may fail to start\n' "$d_file" "$d_need" >&2 ;;
            esac
        done
    done
    rm -rf "$desktop_stage" && mv "$d_new" "$desktop_stage" ||
        { rm -rf "$d_new"; desktop_why="$desktop_stage could not be written"; return 1; }
    printf "geforce-sriov-driver: NVIDIA's graphics libraries %s are ready to be installed:%s\n" "$d_version" "$d_list"
}
desktop_not_staged() {
    printf "geforce-sriov-driver: warning: NVIDIA's graphics libraries were not taken from the Linux driver package: %s\n" "$desktop_why" >&2
    # A stage left by an earlier run is of another package: the CLI must not install it.
    [ -z "$desktop_stage" ] || rm -rf "$desktop_stage" "$desktop_stage.gs-new"
}
# After a build: the Linux driver package is extracted, or was given as a directory.
stage_desktop_built() {
    [ -n "$desktop_stage" ] && [ "$display" -eq 1 ] || return 0
    stage_desktop "$1" "$public_version" || desktop_not_staged
}
# What the Linux driver package is asked for: its CUDA libraries (--host-cuda --cuda-stage) and its graphics
# libraries (--display --desktop-stage). libs_not_taken REASON says why neither could be taken.
libs_wanted() {
    { [ -n "$cuda_stage" ] && [ "$host_cuda" -eq 1 ]; } || { [ -n "$desktop_stage" ] && [ "$display" -eq 1 ]; }
}
libs_not_taken() {
    if [ -n "$cuda_stage" ] && [ "$host_cuda" -eq 1 ]; then
        cuda_why=$1
        cuda_not_staged
    fi
    if [ -n "$desktop_stage" ] && [ "$display" -eq 1 ]; then
        desktop_why=$1
        desktop_not_staged
    fi
}
stage_wanted() {
    if [ -n "$cuda_stage" ] && [ "$host_cuda" -eq 1 ]; then
        stage_cuda "$1" || cuda_not_staged
    fi
    if [ -n "$desktop_stage" ] && [ "$display" -eq 1 ]; then
        stage_desktop "$1" "$desktop_nvkms" || desktop_not_staged
    fi
}
# For a build that is installed already, which reads no input: the Linux driver package that was given, or
# the one an earlier install kept, is opened for the libraries alone. The NVKMS core of the installed
# build was stamped with the public source's version, which its record has.
stage_libs_prepared() {
    libs_wanted || return 0
    desktop_nvkms=$(gs_record_get "$record" driver_public_version)
    c_file=$companion_file
    c_tree=$companion_source
    if [ -z "$c_file$c_tree" ]; then
        # The same checks as for a build; what would refuse a build is only the reason here.
        c_kept=$(use_kept companion "the Linux driver package" '--companion-file PATH (or --companion-source DIR)' >/dev/null 2>&1 &&
            printf '%s:%s' "$kept_kind" "$kept_path") ||
            { libs_not_taken 'none was given, and none that an earlier install kept can be used; pass --companion-file PATH (or --companion-source DIR)'; return 0; }
        case $c_kept in
            file:*) c_file=${c_kept#file:} ;;
            *) c_tree=${c_kept#*:} ;;
        esac
    fi
    if [ -n "$c_tree" ]; then
        if [ -d "$c_tree" ]; then
            stage_wanted "$c_tree"
        else
            libs_not_taken "$c_tree is not a directory"
        fi
        return 0
    fi
    # A package is a shell script: only a copy that nobody else can write to is checked and run.
    c_work=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") ||
        { libs_not_taken 'no temporary directory could be made'; return 0; }
    c_copy=$c_work/${c_file##*/}
    c_why=
    if ! { [ -f "$c_file" ] && cp -P --no-preserve=ownership -- "$c_file" "$c_copy" && [ -f "$c_copy" ] && [ ! -L "$c_copy" ] &&
        chmod ug-s,go-w "$c_copy"; }; then
        c_why="$c_file could not be copied"
    elif ! gs_package_check second "$c_copy" "$c_file"; then
        c_why="$c_file was not run; the line above says why"
    elif ! { mkdir "$c_work/x" && ( cd "$c_work/x" && sh "$c_copy" --extract-only ) >"$c_work/log" 2>&1; }; then
        c_why="$c_file could not be extracted with --extract-only"
    else
        c_found=
        for c_dir in "$c_work"/x/*/; do
            [ ! -d "$c_dir" ] || c_found=${c_dir%/}
        done
        if [ -z "$c_found" ]; then
            c_why="the extraction of $c_file produced no directory"
        else
            stage_wanted "$c_found"
        fi
    fi
    rm -rf "$c_work"
    [ -z "$c_why" ] || libs_not_taken "$c_why"
}

# A journal left by a run that was cut off: finish that commit, or undo it.
recover_commit() {
    [ -f "$journal" ] && [ ! -L "$journal" ] || return 0
    if [ "$dry" -eq 1 ]; then
        printf 'geforce-sriov-driver: an earlier run was interrupted while it installed its files (%s); a run without --dry-run completes or undoes that installation first\n' "$journal"
        return 0
    fi
    # Not a journal this script can act on: it names no module. Such a file is only a leftover.
    if [ -z "$(journal_modules)" ]; then
        rm -f "$journal"
        return 0
    fi
    j_build=$(journal_build)
    j_kernel=$(jv kernel)
    forward=1
    retired=0
    # No destination may hold anything but what the commit found there or what it built: a file
    # that was changed since is someone else's and is not overwritten.
    for flat in $(journal_modules); do
        disk=$(printf '%s' "$flat" | tr '_' '-')
        case $(module_state "$flat") in
            new) ;;
            before) in_place "$j_build/$disk.ko" "$(jv "module_${flat}_sha256")" || forward=0 ;;
            *) stuck "$(jv "module_${flat}_path") is neither the module that run found there nor the one it built, so it was changed since" ;;
        esac
    done
    j_copy=$(jv journal_plugin_copy)
    if [ -n "$j_copy" ]; then
        case $(plugin_state) in
            new) ;;
            before) in_place "$(jv journal_plugin_staged)" "$(jv plugin_sha256)" || forward=0 ;;
            *) stuck "$j_copy is neither the plugin copy that run found there nor the one it made, so it was changed since" ;;
        esac
    fi
    for name in $(jv journal_retire); do
        path=$(jv "journal_retire_${name}_path")
        disk=${path##*/}
        if in_place "$path" "$(jv "journal_retire_${name}_sha256")"; then
            :
        elif [ ! -e "$path" ] && [ ! -L "$path" ]; then
            retired=1
        elif in_place "$path" "$(jv "journal_retire_${name}_backup_sha256")"; then
            retired=1
        else
            stuck "$path is neither the module that run was about to drop nor what dropping it leaves, so it was changed since"
        fi
    done
    # The dropped modules are touched last, when nothing staged is needed any more.
    [ "$forward" -eq 1 ] || [ "$retired" -eq 0 ] ||
        stuck "a module of the earlier build was already dropped, and a file that run had built is missing"
    recovered=1
    if [ "$forward" -eq 1 ]; then
        printf 'geforce-sriov-driver: an earlier run was interrupted while it installed its files. Completing that installation from the files it had built; nothing is built again\n'
        commit_files
        printf 'geforce-sriov-driver: the interrupted installation is complete and its record is now in place\n'
    else
        printf 'geforce-sriov-driver: an earlier run was interrupted while it installed its files, and the files it had built are no longer complete. Putting back the files as they were before that run\n'
        rollback_commit
        printf 'geforce-sriov-driver: every file is as it was before that run, and the earlier record describes the installation again\n'
    fi
}

# The patched plugin copy is executable, like NVIDIA's own copies; one from an earlier install may not be.
copy_mode() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ "$(stat -c %a "$1")" != 755 ] || return 0
    chmod 0755 "$1" || fail "could not set mode 0755 on $1"
    mode_fixed=1
    printf 'geforce-sriov-driver: set mode 0755 on %s\n' "$1"
}

# The libraries and the source patches sit next to this script as installed; in
# the source tree the patches are one directory above scripts/.
libdir=${0%/*}
# The version policy, the record reader and the table of confirmed versions with their recorded
# digests. The library has no side effects.
# shellcheck source=gs-driver-version.sh
. "$libdir/gs-driver-version.sh"
# The copy of a tree that root builds from, and the staging of the display and host CUDA modules.
# shellcheck source=gs-driver-stage.sh
. "$libdir/gs-driver-stage.sh"
gs_versions_load "$libdir/gs-driver-versions.tbl" ||
    refuse "the table of confirmed driver versions, $libdir/gs-driver-versions.tbl, is missing or cannot be used; the line above says why"
# Without --source: the kernel tree of a confirmed version, the first one that is there.
if [ "$source_given" -eq 0 ]; then
    for version in $gs_confirmed_hosts; do
        [ -n "$src" ] || src=$root/usr/src/nvidia-$version
        if [ -d "$root/usr/src/nvidia-$version" ]; then
            src=$root/usr/src/nvidia-$version
            break
        fi
    done
fi
find_patch_dir() {
    patch_dir=
    for candidate in "${GS_DRIVER_PATCH_DIR:-}" "$libdir/driver-patches" \
        "$libdir/../share/geforce-sriov/driver-patches" "$libdir/../driver-patches"; do
        [ -n "$candidate" ] || continue
        if [ -f "$candidate/gpu-allow.patch" ] && [ -f "$candidate/allocation-threshold.patch" ]; then
            patch_dir=$candidate
            return 0
        fi
    done
    return 1
}

# ---- the inputs kept for a later build ---------------------------------------
#
# A kernel update needs the same build again, for the new kernel, and root runs it without being
# told where the inputs are. A place cannot be trusted for that: whoever owns a directory above it
# can put another tree there afterwards. So a copy of every input is kept under $inputs, owned by
# root, not writable by anyone else, and with no symbolic link that leads out of it: the host tree,
# the Linux driver package and the public source. A kept file is used again only while it still has the
# digest the index holds; a kept tree is identified by the version it declares. A run with
# --retained takes every input it is not given from there. What is kept after a build is the copy
# the build was made from.
index_get() {
    [ ! -f "$inputs_index" ] || [ -L "$inputs_index" ] || gs_record_get "$inputs_index" "$1"
}
# Replaces the lines of one input in the index, through a new file and a rename.
index_put() {
    index_name=$1
    shift
    {
        [ ! -f "$inputs_index" ] || sed "/^$index_name\\(_[a-z0-9]*\\)\\{0,1\\}=/d" "$inputs_index"
        [ "$#" -eq 0 ] || printf '%s\n' "$@"
    } >"$inputs_index.gs-new" && gs_record_emit <"$inputs_index.gs-new" >/dev/null 2>&1 &&
        chmod 0644 "$inputs_index.gs-new" && sync "$inputs_index.gs-new" && mv -f "$inputs_index.gs-new" "$inputs_index" ||
        { rm -f "$inputs_index.gs-new"; return 1; }
}
# use_kept NAME WHAT OPTION: sets kept_path and kept_kind to the kept input NAME, or refuses and
# says how to give it again.
use_kept() {
    again="NVIDIA's files are as they were. Run \`geforce-sriov install\` in the source folder with $3; that keeps it again"
    kept_path=$(index_get "$1")
    kept_kind=$(index_get "$1_kind")
    [ -n "$kept_path" ] ||
        refuse "this build needs $2, and no earlier install kept one ($inputs_index names none). $again"
    case $kept_kind in
        file)
            [ -f "$kept_path" ] && [ ! -L "$kept_path" ] ||
                refuse "$2 that an earlier install kept is missing: $kept_path. $again"
            kept_now=$(sha "$kept_path") || fail "could not hash $kept_path"
            [ "$kept_now" = "$(index_get "$1_sha256")" ] ||
                refuse "$2 that an earlier install kept was changed since: $kept_path has SHA-256 $kept_now, and the file that was kept had $(index_get "$1_sha256"). $again"
            [ ! -u "$kept_path" ] && [ ! -g "$kept_path" ] ||
                refuse "$2 that an earlier install kept, $kept_path, has the set-user-ID or set-group-ID bit and is not used. $again" ;;
        directory)
            [ -d "$kept_path" ] && [ ! -L "$kept_path" ] ||
                refuse "$2 that an earlier install kept is missing: $kept_path. $again"
            gs_tree_closed "$kept_path" ||
                refuse "$2 that an earlier install kept is not built from; the line above says why. $again" ;;
        *) refuse "$inputs_index does not say whether $kept_path is a file or a directory. $again" ;;
    esac
}
# The host tree of a run with --retained that names none: the copy an earlier run kept. An index
# written before copies were kept names the place of the tree instead; that place is not used.
src_kept=0
if [ "$retained" -eq 1 ] && [ "$restore" -eq 0 ] && [ "$source_given" -eq 0 ] && [ -z "$driver_file" ]; then
    kept_src=$(index_get nvidia_source)
    case $(index_get nvidia_source_kind):$kept_src in
        :) ;;
        directory:"$inputs"/nvidia_source/*)
            if [ -d "$kept_src" ] && [ ! -L "$kept_src" ]; then
                src=$kept_src
                src_kept=1
            fi ;;
        *)
            printf 'geforce-sriov-driver: an earlier install kept only the place of the kernel tree it used, %s. A place is not built from again without being named, so this run looks for %s. `geforce-sriov install --nvidia-source DIR` names a tree and keeps a copy of it\n' \
                "$kept_src" "$src" ;;
    esac
fi
# The kept Linux driver package and public source, for a run with --retained that was not given them.
# Called only once a build is certain: a run that finds its build installed needs no input.
use_kept_inputs() {
    [ "$retained" -eq 1 ] || return 0
    if { [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; } && [ -z "$companion_file" ] && [ -z "$companion_source" ]; then
        use_kept companion "the Linux driver package" '--companion-file PATH (or --companion-source DIR)'
        if [ "$kept_kind" = file ]; then companion_file=$kept_path; else companion_source=$kept_path; fi
        kept_companion=1
    fi
    if [ "$display" -eq 1 ] && [ -z "$nvkms_source" ]; then
        use_kept nvkms "NVIDIA's public kernel module source" '--nvkms-source PATH'
        nvkms_source=$kept_path
        kept_nvkms=1
    fi
}
# The version an input declares, read in a scratch directory of its own; nothing when it has none.
input_version() {
    v_dir=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") || return 0
    gs_detect_identity "$1" "$2" "$v_dir" 2>/dev/null | sed -n 's/^version=//p'
    rm -rf "$v_dir"
}
# has_space DIR PATH: whether the file system of DIR has room for a copy of PATH. Without it,
# $space_why says how much is needed and how much is free. A size that cannot be read is not judged.
has_space() {
    space_need=$(du -sk -- "$(readlink -f -- "$2")" 2>/dev/null | cut -f 1) || space_need=
    space_free=$(df -Pk -- "$1" 2>/dev/null | awk 'NR == 2 { print $4 }') || space_free=
    case $space_need:$space_free in :*|*:|*[!0-9:]*) return 0 ;; esac
    [ "$space_free" -lt "$space_need" ] || return 0
    space_why="not enough space in $1: $(( (space_need + 1023) / 1024 )) MB needed for a copy of $2, $(( space_free / 1024 )) MB free"
    return 1
}
# The same before a build: a copy that cannot be made refuses before the first change.
need_space() {
    has_space "$1" "$2" || refuse "$space_why. NVIDIA's files are as they were. Free some space, then run the same command again"
}
# keep_input NAME WHAT PATH VERSION BUILT [ORIGIN [BASE]]: copies one input under $inputs and names
# it in the index. PATH is what is copied; ORIGIN is the path the user gave, when PATH is this
# run's copy of it. BASE names the kept copy; without it the copy has the name of ORIGIN.
keep_input() {
    k_name=$1 k_what=$2 k_from=${3%/} k_version=$4 k_given=${6:-$3} k_base=${7:-}
    k_given=${k_given%/}
    case $k_from in /*) ;; *) k_from=$PWD/$k_from ;; esac
    case $k_given in /*) ;; *) k_given=$PWD/$k_given ;; esac
    [ -n "$k_base" ] || k_base=${k_given##*/}
    # The place becomes a line of the index; made absolute, it may hold what the arguments did not.
    [ "$(printf '%s' "$k_given" | LC_ALL=C tr -d '\000-\037\177')" = "$k_given" ] || return 1
    # What is kept must be what the installed build was made from: the record holds its digest.
    case $k_name in
        nvidia_source) k_bound=$(gs_record_get "$record" input_source_sha256) ;;
        companion) k_bound=$(gs_record_get "$record" input_second_sha256) ;;
        *) k_bound=$(gs_record_get "$record" input_public_sha256) ;;
    esac
    case $k_bound in
        ''|none)
            printf 'geforce-sriov-driver: %s was not kept: the record of the installed build does not hold the digest of %s it was made from. The next build keeps it\n' "$k_given" "$k_what"
            return 0 ;;
    esac
    # The name becomes part of a line of the index.
    case $k_base in ''|.|..|*[!A-Za-z0-9._+-]*) return 1 ;; esac
    k_have=$(index_get "$k_name")
    if [ -d "$k_from" ]; then
        k_kind=directory
        k_sha=none
        # A tree kept from the same place for the same version is copied again only after a build.
        if [ "$5" -eq 0 ] && [ "$(index_get "${k_name}_origin")" = "$k_given" ] &&
            [ "$(index_get "${k_name}_version")" = "$k_version" ] && [ -d "$k_have" ]; then
            return 0
        fi
    elif [ -f "$k_from" ]; then
        k_kind='file'
        k_sha=$(sha "$k_from") || return 1
        if [ "$k_sha" != "$k_bound" ]; then
            printf 'geforce-sriov-driver: %s was not kept: it is not %s that the installed build was made from\n' "$k_given" "$k_what"
            return 0
        fi
        if [ "$(index_get "${k_name}_sha256")" = "$k_sha" ] && [ -f "$k_have" ] && [ ! -L "$k_have" ] &&
            [ "$(sha "$k_have")" = "$k_sha" ]; then
            return 0
        fi
    else
        return 1
    fi
    k_real=$(readlink -f "$k_from") || return 1
    k_dest=$inputs/$k_name
    if ! has_space "$inputs" "$k_real"; then
        printf 'geforce-sriov-driver: %s\n' "$space_why" >&2
        return 1
    fi
    rm -rf "$k_dest.gs-new" && mkdir -p "$k_dest.gs-new" &&
        if [ "$k_kind" = directory ]; then
            gs_copy_tree "$k_real" "$k_dest.gs-new/$k_base" || { rm -rf "$k_dest.gs-new"; return 1; }
            # The copy, not the place it came from, is what is compared with the build.
            if [ "$(gs_tree_digest "$k_dest.gs-new/$k_base")" != "$k_bound" ]; then
                rm -rf "$k_dest.gs-new"
                printf 'geforce-sriov-driver: %s was not kept: it is not %s that the installed build was made from\n' "$k_given" "$k_what"
                return 0
            fi
        else
            cp -P --no-preserve=ownership "$k_real" "$k_dest.gs-new/$k_base" && chmod ug-s,go-w "$k_dest.gs-new/$k_base" &&
                [ -f "$k_dest.gs-new/$k_base" ] && [ ! -L "$k_dest.gs-new/$k_base" ] &&
                [ "$(sha "$k_dest.gs-new/$k_base")" = "$k_sha" ]
        fi &&
        sync -f "$k_dest.gs-new" && rm -rf "$k_dest" && mv "$k_dest.gs-new" "$k_dest" ||
        { rm -rf "$k_dest.gs-new"; return 1; }
    sync_dir "$inputs"
    index_put "$k_name" "$k_name=$k_dest/$k_base" "${k_name}_kind=$k_kind" "${k_name}_sha256=$k_sha" \
        "${k_name}_version=$k_version" "${k_name}_origin=$k_given" || return 1
    printf 'geforce-sriov-driver: kept %s for a later build: %s\n' "$k_what" "$k_dest/$k_base"
}
# keep_checked NAME WHAT PATH VERSION WANT BUILT [ORIGIN [BASE]]: keeps an input that is the one the
# installed build was made from, which WANT, the version in the build record, says.
keep_checked() {
    [ -n "$3" ] || return 0
    case $5 in ''|none) return 0 ;; esac
    if [ "$4" != "$5" ]; then
        printf 'geforce-sriov-driver: %s was not kept: it is not %s that the installed build was made from\n' "${7:-$3}" "$2"
        return 0
    fi
    keep_input "$1" "$2" "$3" "$4" "$6" "${7:-$3}" "${8:-}" ||
        printf 'geforce-sriov-driver: warning: %s could not be kept under %s; a build for another kernel then needs it passed to install again\n' \
            "${7:-$3}" "$inputs" >&2
}
# keep_inputs BUILT: after a run that left the installed build in place, 1 when it built it. The
# inputs this run was given are kept; those it took from $inputs are there already. A run that
# built kept its own copies, the ones the build was made from. A run that built nothing copies what
# it was given, after checking it against the record of the installed build.
keep_inputs() {
    [ -f "$record" ] && [ ! -L "$record" ] || return 0
    # Root's files, whatever the umask of this run.
    mkdir -p "$inputs" && chmod 0755 "$inputs" || return 1
    # The tree a --driver-file run extracted is not kept: the CLI keeps its own extraction and names it.
    if [ "$src_kept" -eq 0 ] && [ -z "$driver_file" ]; then
        keep_checked nvidia_source "NVIDIA's kernel tree" "$src" "$host_version" \
            "$(gs_record_get "$record" driver_version)" "$1" "$src_shown" kernel
    fi
    k_second=${given_companion_file:-$given_companion_source}
    if [ -n "$k_second" ]; then
        k_want=$(gs_record_get "$record" driver_second_version)
        if [ "$1" -eq 1 ]; then
            k_is=$second_version
        elif [ -n "$given_companion_file" ]; then
            # Running a package to read its version is the build's business: the digest decides.
            k_is=
            [ "$(sha "$k_second")" != "$(gs_record_get "$record" second_sha256)" ] || k_is=$k_want
        else
            k_is=$(input_version second "$k_second")
        fi
        # A package is the one the build was made from only with the digest the record holds.
        if [ "$1" -eq 1 ] && [ -n "$given_companion_file" ] &&
            [ "$(sha "${taken_companion:-$k_second}")" != "$(gs_record_get "$record" second_sha256)" ]; then
            k_is=
        fi
        keep_checked companion "the Linux driver package" "${taken_companion:-$k_second}" "$k_is" "$k_want" "$1" "$k_second"
    fi
    if [ -n "$given_nvkms" ]; then
        k_want=$(gs_record_get "$record" driver_public_version)
        if [ "$1" -eq 1 ]; then k_is=$public_version; else k_is=$(input_version public "$given_nvkms"); fi
        keep_checked nvkms "NVIDIA's public kernel module source" "${taken_nvkms:-$given_nvkms}" "$k_is" "$k_want" "$1" "$given_nvkms"
    fi
}

# --restore: undo this script's changes for the kernel. The stock module comes back only while the
# installed module is still the one this script installed, from a backup whose digest the record
# holds; the build directory, the plugin link and the modprobe file are restored whenever they are
# this script's own. Added modules are removed only while they still match the recorded digest.
# Runs before any build check, so it works on a host without headers. Every check comes before the
# first change; it exits 0 only when each step succeeded. Then the backups it put back go, and so
# do the build and backup directories once they are empty; after a failure every backup stays.
recover_commit
# --restore without --kernel undoes every kernel this script built for: the one the published
# record names and each one with a record of its own, one run each. A host with no record at all
# gets the run for the running kernel, which also tidies up the plugin link and the modprobe file.
if [ "$restore" -eq 1 ] && [ "$kernel_given" -eq 0 ]; then
    all=
    [ ! -f "$published" ] || [ -L "$published" ] || all=$(gs_record_get "$published" kernel)
    for file in "$records"/*; do
        [ -f "$file" ] && [ ! -L "$file" ] || continue
        case " $all " in *" ${file##*/} "*) ;; *) all="$all ${file##*/}" ;; esac
    done
    [ -n "$all" ] || all=$kernel
    status=0
    for one in $all; do
        case $one in ''|.|..|*/*) continue ;; esac
        set -- --restore --kernel "$one"
        [ -z "$root" ] || set -- "$@" --root "$root"
        [ "$dry" -eq 0 ] || set -- "$@" --dry-run
        rc=0
        sh "$0" "$@" || rc=$?
        [ "$rc" -eq 0 ] || [ "$status" -ne 0 ] || status=$rc
    done
    [ "$dry" -eq 1 ] || [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
    exit "$status"
fi
# The record this run reads is the one of its kernel: the published record when it names this
# kernel, which is then the authority whatever is kept under the kernel's name; else the one kept
# under the kernel's name. Without either, the published record of another kernel, or none, is what
# the checks below see, and they treat this kernel as not built.
if [ -f "$published" ] && [ ! -L "$published" ] && [ "$(gs_record_get "$published" kernel)" = "$kernel" ]; then
    record=$published
elif [ -f "$records/$kernel" ] && [ ! -L "$records/$kernel" ]; then
    record=$records/$kernel
fi
if [ "$restore" -eq 1 ]; then
    command -v modinfo >/dev/null 2>&1 || refuse 'modinfo not found: cannot locate the installed module'
    module=$(modinfo -k "$kernel" -n nvidia 2>/dev/null || true)
    put_back=0
    # A record whose module is gone or is no longer the one this script installed has nothing to restore: it goes with a clean restore.
    drop_record=0
    core_kept=0
    stale=0
    if [ -z "$module" ]; then
        printf 'geforce-sriov-driver: no nvidia module installed for %s; nothing to restore\n' "$kernel"
        drop_record=1
    elif [ ! -f "$record" ]; then
        printf 'geforce-sriov-driver: %s absent: this script did not prepare %s; nothing to restore\n' "$record" "$kernel"
    else
        rec_kernel=$(gs_record_get "$record" kernel)
        rec_hash=$(gs_record_get "$record" module_sha256)
        rec_stock=$(gs_record_get "$record" stock_sha256)
        installed=$(sha "$module") || fail "could not hash $module"
        if [ "$rec_kernel" != "$kernel" ]; then
            printf 'geforce-sriov-driver: kept %s: it is not the module this script installed\n' "$module"
            drop_record=1
        elif [ "$rec_hash" != "$installed" ]; then
            # Another module than this script's build. When it has another build-id, or is the module the
            # backup holds, it is NVIDIA's (installed again) or someone else's: nothing of this script's is
            # installed, so there is nothing to put back and the old backup and record are stale. The same
            # build-id with other bytes is this script's build, changed since (for example signed): it stays
            # and the restore fails. When the build-id cannot be read the module stays and the restore fails.
            rec_id=$(gs_record_get "$record" module_build_id)
            installed_id=$(readelf -n "$module" 2>/dev/null | awk '$1 == "Build" && $2 == "ID:" {print $3; exit}' || true)
            if [ "$installed" = "$rec_stock" ] || { [ -n "$rec_id" ] && [ -n "$installed_id" ] && [ "$installed_id" != "$rec_id" ]; }; then
                printf 'geforce-sriov-driver: %s is not the module this script installed; it is another build, so nothing is put back and the backup of the earlier stock module and the build record are removed\n' "$module"
                stale=1
            else
                printf 'geforce-sriov-driver: kept %s: it is not the module this script installed\n' "$module"
                core_kept=1
            fi
        else
            [ -f "$backup" ] || refuse "backup missing: $backup"
            [ -n "$rec_stock" ] || refuse "$record records no stock_sha256, so the backup $backup cannot be verified"
            backup_hash=$(sha "$backup") || fail "could not hash $backup"
            [ "$backup_hash" = "$rec_stock" ] || refuse "backup $backup has SHA-256 $backup_hash; $record records $rec_stock"
            put_back=1
        fi
    fi
    # Every other module this script installed, as its record names it.
    extra=
    if [ -f "$record" ] && [ ! -L "$record" ]; then
        extra=$(gs_record_modules "$record")
    fi
    restorable=0
    path=''
    want=''
    previous=''
    saved=''
    action=''
    disk=''
    for name in $extra; do
        [ "$name" = nvidia ] && continue
        # The record flattens '-' to '_' in field names; the file keeps its own name.
        disk=$(printf '%s' "$name" | tr '_' '-')
        path=$(gs_record_get "$record" "module_${name}_path")
        want=$(gs_record_get "$record" "module_${name}_sha256")
        previous=$(gs_record_get "$record" "module_${name}_previous")
        saved=$(gs_record_get "$record" "module_${name}_backup_sha256")
        eval "restore_${name}=keep"
        eval "restore_path_${name}=\$path"
        eval "restore_disk_${name}=\$disk"
        [ -n "$path" ] || continue
        if [ ! -e "$path" ] && [ ! -L "$path" ]; then
            printf 'geforce-sriov-driver: %s is already absent\n' "$path"
            eval "restore_${name}=done"
        elif [ -z "$want" ] || [ "$(sha "$path")" != "$want" ]; then
            printf 'geforce-sriov-driver: kept %s: it is not the module this script installed\n' "$path"
            if [ "$stale" -eq 1 ]; then
                eval "restore_${name}=stale"
            else
                eval "restore_${name}=failed"
            fi
        elif [ "$previous" = present ]; then
            if [ -f "$state/stock/$kernel/$disk.ko" ] &&
                [ "$(sha "$state/stock/$kernel/$disk.ko")" = "$saved" ]; then
                eval "restore_${name}=putback"
                restorable=$((restorable + 1))
            else
                printf 'geforce-sriov-driver: kept %s: its backup is missing or does not match the record\n' "$path" >&2
                eval "restore_${name}=failed"
            fi
        else
            eval "restore_${name}=remove"
            restorable=$((restorable + 1))
        fi
    done
    # Modules added last are removed first.
    reversed=
    for name in $extra; do
        [ "$name" = nvidia ] && continue
        reversed="$name $reversed"
    done
    # The plugin the install put in place, and the link it repointed, are named deterministically
    # after the driver version: the one the record names, else a confirmed one.
    plugdir=
    plugin_name=
    rec_version=
    [ ! -f "$record" ] || rec_version=$(gs_record_get "$record" driver_version)
    case $rec_version in *[!0-9.]*) rec_version= ;; esac
    for version in $rec_version $gs_confirmed_hosts; do
        for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
            if [ -f "$dir/libnvidia-vgpu.so.$version" ]; then
                plugdir=$dir
                plugin_name=libnvidia-vgpu.so.$version
                break 2
            fi
        done
    done
    # The digests of that version, when it is a confirmed one.
    gs_version_row "${plugin_name#libnvidia-vgpu.so.}" || :
    relink=0
    foreign=
    if [ -n "$plugdir" ] && [ -L "$plugdir/libnvidia-vgpu.so" ]; then
        target=$(readlink "$plugdir/libnvidia-vgpu.so")
        if [ "$target" = "$plugin_name.geforce-sriov" ]; then
            relink=1
        # A link to NVIDIA's own plugin needs nothing; any other target is someone else's and stays.
        elif [ "$(readlink -f "$plugdir/libnvidia-vgpu.so" || true)" != "$(readlink -f "$plugdir/$plugin_name")" ]; then
            foreign=$target
        fi
    fi
    own_config=0
    if [ -f "$config" ] && [ ! -L "$config" ] && [ "$(cat "$config")" = 'options nvidia NVreg_EnableGpuFirmware=0' ]; then
        own_config=1
    fi
    # The plugin copy is removed only while it is the copy this script installed: the recorded digest
    # on a record, or the digest of the patched copy this step's revisions produce.
    copy_owned=0
    copy_path=$plugdir/$plugin_name.geforce-sriov
    if [ -n "$plugdir" ] && [ -f "$copy_path" ] && [ ! -L "$copy_path" ]; then
        rec_plugin=
        [ ! -f "$record" ] || rec_plugin=$(gs_record_get "$record" plugin_sha256)
        copy_digest_now=$(sha "$copy_path") || fail "could not hash $copy_path"
        if [ -n "$rec_plugin" ] && [ "$copy_digest_now" = "$rec_plugin" ]; then
            copy_owned=1
        elif [ -n "$gs_patched_plugin" ] && [ "$copy_digest_now" = "$gs_patched_plugin" ]; then
            copy_owned=1
        fi
    fi
    if [ "$dry" -eq 1 ]; then
        show rm -rf "$build"
        if [ "$put_back" -eq 1 ]; then
            show cp -p "$backup" "$module"
            show depmod -a "$kernel"
            show rm -f "$record"
        fi
        for name in $reversed; do
            eval "path=\$restore_path_$name"
            eval "disk=\$restore_disk_$name"
            eval "action=\$restore_$name"
            case $action in
                putback) show cp -p "$state/stock/$kernel/$disk.ko" "$path"
                    show rm -f "$state/stock/$kernel/$disk.ko" ;;
                remove) show rm -f "$path" ;;
            esac
        done
        [ "$relink" -eq 0 ] || show ln -sfn "$plugin_name" "$plugdir/libnvidia-vgpu.so"
        [ -z "$plugdir" ] || show rm -f "$copy_path"
        [ "$own_config" -eq 0 ] || show rm -f "$config"
        [ "$put_back" -eq 0 ] || show rm -rf "${backup%/*}"
        [ "$drop_record" -eq 0 ] || [ -z "$record" ] || show rm -f "$record"
        [ "$stale" -eq 0 ] || show rm -f "$record" "$backup"
        show rmdir "$state/build" "$state/stock"
        exit 0
    fi
    status=0
    if [ "$core_kept" -eq 1 ]; then
        printf 'geforce-sriov-driver: failed: %s was kept: it is not the module this script installed\n' "$module" >&2
        status=1
    fi
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
            rm -f "$record" "$records/$kernel" "$backup"
            [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
            printf 'geforce-sriov-driver: restored the stock module at %s\n' "$module"
        else
            printf 'geforce-sriov-driver: failed: could not restore %s from %s\n' "$module" "$backup" >&2
            status=1
        fi
    fi
    if [ "$stale" -eq 1 ]; then
        rm -f "$record" "$records/$kernel" "$backup"
        [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
        printf 'geforce-sriov-driver: removed the build record and the backup of the earlier stock module for %s\n' "$kernel"
    fi
    for name in $reversed; do
        eval "path=\$restore_path_$name"
        eval "disk=\$restore_disk_$name"
        eval "action=\$restore_$name"
        case $action in
            putback)
                if cp -p "$state/stock/$kernel/$disk.ko" "$path"; then
                    # The backup has done its work, like the core's: a copy left here would keep the
                    # directory, and uninstall would then offer a restore that already happened.
                    rm -f "$state/stock/$kernel/$disk.ko"
                    printf 'geforce-sriov-driver: restored %s at %s\n' "$name" "$path"
                    restorable=$((restorable - 1))
                else
                    printf 'geforce-sriov-driver: failed: could not restore %s\n' "$path" >&2
                    status=1
                fi ;;
            remove)
                if rm -f "$path"; then
                    printf 'geforce-sriov-driver: removed %s\n' "$path"
                    restorable=$((restorable - 1))
                else
                    printf 'geforce-sriov-driver: failed: could not remove %s\n' "$path" >&2
                    status=1
                fi ;;
            stale)
                # NVIDIA's package replaced this module too: the old backup of it is stale.
                rm -f "$state/stock/$kernel/$disk.ko"
                printf 'geforce-sriov-driver: removed the stale backup of %s\n' "$name" ;;
            failed) status=1 ;;
        esac
    done
    if [ "$relink" -eq 1 ]; then
        if ln -sfn "$plugin_name" "$plugdir/libnvidia-vgpu.so"; then
            printf 'geforce-sriov-driver: pointed %s back at the stock plugin %s\n' "$plugdir/libnvidia-vgpu.so" "$plugin_name"
        else
            printf 'geforce-sriov-driver: failed: could not point %s/libnvidia-vgpu.so back at the stock plugin\n' "$plugdir" >&2
            status=1
        fi
    elif [ -n "$foreign" ]; then
        printf 'geforce-sriov-driver: left %s: it points to %s, which this package did not install\n' "$plugdir/libnvidia-vgpu.so" "$foreign"
    fi
    if [ -n "$plugdir" ] && [ -e "$copy_path" ] &&
        [ "$(readlink "$plugdir/libnvidia-vgpu.so" || true)" != "$plugin_name.geforce-sriov" ]; then
        if [ "$copy_owned" -eq 0 ]; then
            printf 'geforce-sriov-driver: left %s: it is not the plugin copy this script installed\n' "$copy_path"
        elif rm -f "$copy_path"; then
            printf 'geforce-sriov-driver: removed %s\n' "$copy_path"
        else
            printf 'geforce-sriov-driver: failed: could not remove %s\n' "$copy_path" >&2
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
    if [ "$status" -eq 0 ] && [ "$drop_record" -eq 1 ] && [ -n "$record" ]; then
        rm -f "$record" "$records/$kernel"
        [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ]; then
        # The directory goes only once it is empty: a kept foreign core or a kept module still
        # needs its verified backup for a later attempt.
        if [ -d "${backup%/*}" ] && rmdir "${backup%/*}" 2>/dev/null; then
            printf 'geforce-sriov-driver: removed %s\n' "${backup%/*}"
        fi
        for dir in "$state/build" "$state/stock"; do
            if [ -d "$dir" ] && rmdir "$dir" 2>/dev/null; then
                printf 'geforce-sriov-driver: removed %s\n' "$dir"
            fi
        done
    fi
    exit "$status"
fi

# Dry-run is a plan only: it runs no build, patcher or filesystem mutation. It names the recorded
# digests of the version the source tree declares. A tree that cannot be read, and one of a version
# with no recorded digests, is planned with those of the first confirmed version.
if [ "$dry" -eq 1 ] && [ "$restore" -eq 0 ]; then
    plan_version=$(gs_source_version "$src" 2>/dev/null) || plan_version=
    gs_version_row "$plan_version" || gs_version_row "${gs_confirmed_hosts%% *}" ||
        refuse 'the table of confirmed driver versions has no usable first block'
    plugin_name=libnvidia-vgpu.so.${plan_version:-$gs_host_version}
    plain_offsets_list=$(printf '%s' "$gs_plain_offsets" | sed 's/ /, /g')
    # What the build's module is compared at, and which object the build goes on with: the plain
    # chain's last object, or with an Ada card selected the one after the ada-placement row.
    plan_final=patched
    plan_require=$gs_patched_object
    plan_eight='the eight expected offsets'
    if [ "$ada_selected" -eq 1 ]; then
        plan_final=placed
        [ -z "$gs_placement_object" ] || plan_require=$gs_placement_object
        if [ -n "$gs_placement_offsets" ]; then
            plain_offsets_list="$plain_offsets_list, and the placement offset $(printf '%s' "$gs_placement_offsets" | sed 's/ /, /g')"
            plan_eight='the eight expected offsets and the placement offset'
        fi
    fi
    # The installed module as the real run finds it below, or a placeholder when modinfo does not know it.
    module=$(modinfo -k "$kernel" -n nvidia 2>/dev/null || true)
    [ -n "$module" ] || module="<installed nvidia.ko for $kernel>"
    cat <<EOF
+ sha256sum '$src/nvidia/nv-kernel.o_binary' # require $gs_stock_object, or admit every byte patch by anchor before any change
+ test -f '$headers/Makefile'
+ test -f '$module'
+ modinfo -k '$kernel' -F license nvidia # refuse NVIDIA's open flavour
+ test -x '${0%/*}/geforce-sriov-patch' || command -v geforce-sriov-patch
+ sha256sum '<plugin directory>/$plugin_name' # require the stock digest or that of the patched plugin copy, or admit every plugin byte patch by anchor
+ check $root/etc/modprobe.d/*.conf for NVreg_EnableGpuFirmware # another file that sets 0 is kept; any other value refuses
EOF
    # The classification term is a plan statement from digests this dry run can read, never a
    # claim that a build happened. The object and plugin inputs are the identity; the host
    # package file is informational and absent for an extracted tree.
    plain_class=unconfirmed
    host_ok=1
    if [ -n "$driver_file" ] && [ "$(sha "$driver_file")" != "$gs_host_package" ]; then
        host_ok=0
    fi
    if [ "$display" -eq 0 ] && [ "$host_cuda" -eq 0 ] && [ "$host_ok" -eq 1 ] &&
        [ -f "$src/nvidia/nv-kernel.o_binary" ] &&
        [ "$(sha "$src/nvidia/nv-kernel.o_binary")" = "$gs_stock_object" ]; then
        for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
            candidate=$dir/$plugin_name
            if [ -f "$candidate" ] && [ "$(sha "$candidate")" = "$gs_stock_plugin" ]; then
                plain_class='confirmed-inputs'
            fi
        done
    fi
    if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ] || [ -n "$driver_file" ]; then
        printf 'features: display=%s host_cuda=%s\n' "$display" "$host_cuda"
        printf 'inputs: driver_file=%s companion_file=%s companion_source=%s nvkms_source=%s\n' \
            "${driver_file:-none}" "${companion_file:-none}" "${companion_source:-none}" "${nvkms_source:-none}"
        printf 'version classification: %s\n' "$plain_class"
        printf '%s\n' '+ detect host, Linux-driver-package and public-source versions from package metadata'
        printf '%s\n' '+ preflight every required byte patch; a failed anchor refuses with no installed-file change'
        if [ "$display" -eq 1 ]; then
            printf '%s\n' "+ gs_stage_tree '$src' '<companion kernel>' '<temporary directory>/tree' 1 '$host_cuda' '<H>' '<P>' '$nvkms_source'"
            printf '%s\n' "+ apply driver-patches/gpu-allow.patch and driver-patches/allocation-threshold.patch to the staged tree"
            printf '%s\n' "+ when the Linux driver package is not of the host's version: apply driver-patches/client-accept.patch, so that nvidia.ko accepts that version"
        else
            printf '%s\n' "+ gs_stage_tree '$src' '<companion kernel>' '<temporary directory>/tree' 0 1 '<H>' '<P>' none"
        fi
        printf '%s\n' "+ geforce-sriov-patch apply '<temporary directory>/object' '<temporary directory>/scheduler'"
        printf '%s\n' "+ geforce-sriov-patch ada apply '<temporary directory>/scheduler' '<temporary directory>/gate'"
        printf '%s\n' "+ geforce-sriov-patch ada apply '<temporary directory>/gate' '<temporary directory>/patched'"
        [ "$ada_selected" -eq 0 ] || printf '%s\n' \
            "+ geforce-sriov-patch ada apply '<temporary directory>/patched' '<temporary directory>/placed' # ada-placement, with an Ada card selected"
        [ "$display" -eq 0 ] || printf '%s\n' "+ geforce-sriov-patch display apply '<temporary directory>/$plan_final' '<temporary directory>/patched39'"
        printf '%s\n' "+ cp '<temporary directory>/$plan_final' '<temporary directory>/tree/nvidia/nv-kernel.o_binary'"
        printf '%s\n' "+ make -C '<temporary directory>/tree' NV_EXCLUDE_BUILD_MODULES= KERNEL_UNAME='$kernel' SYSSRC='$headers' 'NV_KERNEL_MODULES=<staged list>' modules"
        if [ "$display" -eq 1 ]; then
            printf '+ object chain digest %.8s…, then the idx 39 bytes and relocations at the recorded function offset\n' \
                "$([ "$ada_selected" -eq 1 ] && printf '%s' "$gs_placement_display_object" || printf '%s' "$gs_display_object")"
        else
            printf '%s\n' "+ cmp -l '$module' .text with the built .text at exactly $plan_eight"
        fi
        printf '%s\n' '+ ABI gate on every built module: the vermagic of the target kernel, all recorded CRCs against the built core and the kernel, nvidia dependency'
        printf '%s\n' '+ stage every module, then back up and replace each: nvidia and the requested companion modules'
        printf '%s\n' "+ write '$published' with the driver version, classification, input digests and one stamp and digest per module"
        exit 0
    fi
    printf 'version classification: %s\n' "$plain_class"
    cat <<EOF
+ mktemp -d '\${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX'
+ cp -a '$src' '<temporary directory>/tree'
+ geforce-sriov-patch apply '<temporary directory>/tree/nvidia/nv-kernel.o_binary' '<temporary directory>/scheduler'
+ geforce-sriov-patch ada apply '<temporary directory>/scheduler' '<temporary directory>/gate'
EOF
    printf '%s\n' "+ geforce-sriov-patch ada apply '<temporary directory>/gate' '<temporary directory>/patched'"
    if [ "$ada_selected" -eq 1 ]; then
        printf '%s\n' "+ geforce-sriov-patch ada apply '<temporary directory>/patched' '<temporary directory>/placed' # ada-placement, with an Ada card selected"
    fi
    cat <<EOF
+ sha256sum '<temporary directory>/$plan_final' # require $plan_require
+ cp '<temporary directory>/$plan_final' '<temporary directory>/tree/nvidia/nv-kernel.o_binary'
+ make -C '<temporary directory>/tree' NV_EXCLUDE_BUILD_MODULES= KERNEL_UNAME='$kernel' SYSSRC='$headers' modules
+ objcopy -O binary -j .text '$module' '<temporary directory>/stock.text' # refuse if unreadable
+ objcopy -O binary -j .text '<temporary directory>/tree/nvidia.ko' '<temporary directory>/built.text' # refuse if unreadable
+ cmp -l '<temporary directory>/stock.text' '<temporary directory>/built.text' # identical: with config and the patched plugin copy in place and the backup '$backup' differing at exactly the eight offsets, write '$published' for this module and exit 0, else refuse; otherwise require exactly the eight .text offsets $plain_offsets_list
+ geforce-sriov-patch abi-check --core '<temporary directory>/tree/nvidia.ko' --module '<temporary directory>/tree/nvidia.ko' # the vermagic of the target kernel, and its CRCs when the kernel uses module versioning
+ geforce-sriov-patch ada apply '<stock plugin>' '<temporary directory>/patched-plugin.mixed' # plugin-mixed, unless the plugin already carries it
+ sha256sum '<temporary directory>/patched-plugin.mixed' # require $gs_mixed_plugin
+ geforce-sriov-patch ada apply '<temporary directory>/patched-plugin.mixed' '<temporary directory>/patched-plugin' # plugin-div4, unless the plugin already carries it
+ sha256sum '<temporary directory>/patched-plugin' # require $gs_patched_plugin
+ mkdir -p '$state/build' '$state/stock/$kernel'
+ rm -rf '$build' # a leftover of an earlier run
+ mv '<temporary directory>/tree' '$build'
+ cp -p '$module' '$backup' # require the module's SHA-256
+ write the journal '$journal' # the record to publish, and what finishes or undoes this commit after a power loss
+ cp -p '$build/nvidia.ko' '$module.gs-new' && mv -f '$module.gs-new' '$module' # the installed file is always a complete module
+ depmod -a '$kernel'
+ readelf -n '$module'
+ install the patched plugin copy (mode 0755) and update '<plugin directory>/libnvidia-vgpu.so' symlink
+ write '$config' # options nvidia NVreg_EnableGpuFirmware=0, unless another file already sets it to 0
+ write '$published' last # kernel, module_sha256, module_build_id, stock_sha256, driver version and classification
EOF
    exit 0
fi

# Install passes its --nvidia-source DIR here as --source DIR.
nvidia_source_hint='give the kernel source tree (/usr/src/nvidia-<version>) or an extracted package with install --nvidia-source DIR (this script: --source DIR)'
# --driver-file is a host .run package: extract it, never run its installer. The
# extracted tree is what --source would have named.
if [ -n "$driver_file" ]; then
    [ "$source_given" -eq 0 ] || { printf '%s\n' '--driver-file and --source name the same input; pass one' >&2; exit 2; }
    [ -f "$driver_file" ] || refuse "host package missing: $driver_file"
    driver_work=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") || fail 'cannot create temporary directory'
    trap 'rm -rf "$driver_work"' 0
    trap 'exit 1' HUP INT TERM
    # Extracting a .run package runs it. A file that claims to be the recorded package must be it,
    # and the file that is checked must be the file that is run: a copy that only root can change.
    driver_copy=$driver_work/package/${driver_file##*/}
    need_space "${driver_work%/*}" "$driver_file"
    mkdir "$driver_work/package" "$driver_work/host" &&
        cp -P --no-preserve=ownership -- "$(readlink -f -- "$driver_file")" "$driver_copy" &&
        [ -f "$driver_copy" ] && [ ! -L "$driver_copy" ] && chmod ug-s,go-w "$driver_copy" ||
        refuse "could not copy the host package $driver_file to $driver_work"
    gs_package_check host "$driver_copy" "$driver_file" ||
        refuse "the host package $driver_file was not run; the line above says why"
    # The package digest is the host input identity the version policy records.
    driver_package_sha=$(sha "$driver_copy") || driver_package_sha=none
    ( cd "$driver_work/host" && sh "$driver_copy" --extract-only ) >"$driver_work/host.log" 2>&1 ||
        { rm -rf "$driver_work"; refuse "could not extract $driver_file (see its own output with --extract-only)"; }
    extracted=
    for candidate in "$driver_work"/host/*/; do
        [ -d "$candidate" ] || continue
        [ -z "$extracted" ] || { rm -rf "$driver_work"; refuse "package extraction produced more than one directory: $driver_file"; }
        extracted=${candidate%/}
    done
    [ -n "$extracted" ] || { rm -rf "$driver_work"; refuse "package extraction produced no directory: $driver_file"; }
    src=$extracted
    if [ ! -f "$src/nvidia/nv-kernel.o_binary" ] && [ -f "$src/kernel/nvidia/nv-kernel.o_binary" ]; then
        src=$src/kernel
    fi
fi
[ -d "$src" ] || refuse "source tree missing: $src; $nvidia_source_hint"
# DIR may be the kernel source tree (/usr/src/nvidia-<version>) or the root of an extracted .run
# package, whose tree is in kernel/. Accept both rather than making the user know which they have.
if [ ! -f "$src/nvidia/nv-kernel.o_binary" ] && [ -f "$src/kernel/nvidia/nv-kernel.o_binary" ]; then
    src=$src/kernel
fi
[ -f "$src/nvidia/nv-kernel.o_binary" ] || refuse "vendor object missing: $src/nvidia/nv-kernel.o_binary; $nvidia_source_hint"
# The tree as the user knows it; once a build is certain $src is this run's own copy of it.
src_shown=$src
# The version the tree declares selects the block of recorded digests, and names NVIDIA's plugin. A
# version with no block has no recorded digest: every revision is then admitted by its anchors.
if ! host_version=$(gs_source_version "$src" 2>&1); then
    refuse "cannot determine the host driver version from $src: $(printf '%s\n' "$host_version" | sed -n '$p')"
fi
gs_version_row "$host_version" || :
plugin_name=libnvidia-vgpu.so.$host_version
# The patcher is installed next to this script; install runs it with a fixed PATH that need not
# hold the install prefix, so look beside the script first, then on PATH.
patcher=${0%/*}/geforce-sriov-patch
if [ ! -f "$patcher" ] || [ ! -x "$patcher" ]; then
    patcher=$(command -v geforce-sriov-patch 2>/dev/null) ||
        refuse "geforce-sriov-patch is not installed next to $0 or on PATH"
fi
for tool in sha256sum cut make cmp awk readelf objcopy cp mv mkdir mktemp ln rm chmod stat depmod sed wc sort tr tail find modinfo sync; do
    command -v "$tool" >/dev/null 2>&1 || refuse "required command missing: $tool"
done
# The staging library that builds the display and host CUDA modules is written in python3.
if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
    command -v python3 >/dev/null 2>&1 ||
        refuse 'required command missing: python3 (it is needed to build the display and host CUDA modules)'
fi
actual=$(sha "$src/nvidia/nv-kernel.o_binary") || fail 'could not hash vendor object'
known_object=0
[ "$actual" = "$gs_stock_object" ] && known_object=1
# With an Ada card selected the build carries one more byte patch, ada-placement. For a driver the table has no
# recorded object for it is located by its code window, as the other rows are, and applied as best effort.
# When its site cannot be found the step stops: there is no fallback to fixed slots, and the
# message names the driver version to use instead.
# The version to recommend: the highest confirmed one whose block records the placement patch.
recommended_driver() {
    for rec_candidate in $gs_confirmed_hosts; do
        ( gs_version_row "$rec_candidate" && [ -n "$gs_placement_object" ] && [ -n "$gs_placement_offsets" ] &&
            printf '%s\n' "$rec_candidate" ) || :
    done | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -n 1
}
placement_refuse() {
    rec_driver=$(recommended_driver)
    if [ -n "$rec_driver" ]; then
        rec_use="Install NVIDIA's vGPU host driver $rec_driver, the recommended confirmed version, and run this again"
    else
        rec_use='No confirmed driver version records these patches'
    fi
    refuse "the VMs of an Ada card cannot be placed from free video memory with driver $host_version: $1. $rec_use; nothing was installed"
}
[ -f "$headers/Makefile" ] || refuse "the headers of kernel $kernel are not installed: $headers/Makefile is missing. Install them (for example proxmox-headers-$kernel or linux-headers-$kernel). If NVIDIA's own module for that kernel is not built either, build it with \`dkms install nvidia/$host_version -k $kernel\`. Then run \`sudo geforce-sriov rebuild --kernel $kernel\`"
# The installed module's path depends on the distribution and on DKMS, so ask the kernel's own tool.
module=$(modinfo -k "$kernel" -n nvidia 2>/dev/null || true)
[ -n "$module" ] && [ "$(modinfo -k "$kernel" -F vermagic nvidia 2>/dev/null | awk '{print $1}')" = "$kernel" ] || module=
# DKMS builds NVIDIA's module for a new kernel when that kernel's headers are there; without them
# there is no module yet, and nothing to compare this build with or to back up.
[ -n "$module" ] || refuse "NVIDIA's own module for $kernel is not built yet (modinfo -k $kernel -n nvidia finds none). Install the headers of that kernel, build NVIDIA's module with \`dkms install nvidia/$host_version -k $kernel\` (or \`dkms autoinstall -k $kernel\`), then run \`sudo geforce-sriov rebuild --kernel $kernel\`"
[ -f "$module" ] && [ ! -L "$module" ] || refuse "stock module missing or not a regular file: $module"
# NVIDIA's open flavour carries this licence; this stack builds the proprietary one from nv-kernel.o_binary.
case $(modinfo -k "$kernel" -F license nvidia 2>/dev/null || true) in
    *'Dual MIT/GPL'*)
        refuse "$module is NVIDIA's open flavour (licence Dual MIT/GPL). The NVIDIA host driver setup needs the proprietary one. Remove NVIDIA's driver (for example with nvidia-uninstall), install the vGPU host package again with --kernel-module-type=proprietary, then run install again" ;;
esac
# The stock plugin: the one regular libnvidia-vgpu.so.<version> in the library directories. A digest
# this step's patches lead to is accepted; any other digest must pass the anchor preflight, never be
# refused merely because it is unfamiliar.
find_plugin() {
    plugin=
    for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
        candidate=$dir/$plugin_name
        if [ -f "$candidate" ] && [ ! -L "$candidate" ]; then
            [ -z "$plugin" ] || refuse 'the stock plugin is in more than one library directory'
            plugin=$candidate
        fi
    done
    [ -n "$plugin" ] || refuse "$plugin_name not found"
    plugin_hash=$(sha "$plugin") || fail 'could not hash plugin'
    known_plugin=0
    case $plugin_hash in
        "$gs_stock_plugin"|"$gs_patched_plugin") known_plugin=1 ;;
    esac
    copy=$plugin.geforce-sriov
    link=${plugin%/*}/libnvidia-vgpu.so
}
# Writes the patched plugin copy to $1 from $plugin: the patcher's rows plugin-mixed and then plugin-div4,
# each one the plugin does not carry yet. A plugin that is neither the recorded patched copy nor the recorded
# plugin-mixed output (the stock plugin, or the plugin of a version with no recorded digests) takes both rows,
# as the preflight applied them. A known input also checks each recorded digest.
make_copy() {
    from=$plugin
    if [ "$plugin_hash" != "$gs_patched_plugin" ] && [ "$plugin_hash" != "$gs_mixed_plugin" ]; then
        run "$patcher" ada apply "$plugin" "$1.mixed"
        if [ "$known_plugin" -eq 1 ]; then
            actual=$(sha "$1.mixed") || fail 'could not hash the plugin after plugin-mixed'
            [ "$actual" = "$gs_mixed_plugin" ] || refuse "plugin SHA-256 after plugin-mixed is $actual; expected $gs_mixed_plugin"
        fi
        from=$1.mixed
    fi
    if [ "$plugin_hash" = "$gs_patched_plugin" ]; then
        run cp -p "$plugin" "$1"
    else
        run "$patcher" ada apply "$from" "$1"
    fi
    actual=$(sha "$1") || fail 'could not hash the patched plugin copy'
    if [ "$known_plugin" -eq 1 ]; then
        [ "$actual" = "$gs_patched_plugin" ] || refuse "patched plugin copy SHA-256 is $actual; expected $gs_patched_plugin"
    fi
}
# Puts the patched plugin copy $1 in place of an installed copy that is not it, through a new name in
# the same directory, and points the link at it.
replace_copy() {
    [ ! -d "$copy" ] && [ ! -d "$copy.new" ] && [ ! -d "$link" ] || refuse 'an installation target is a directory'
    run rm -f "$copy.new"
    run cp -p "$1" "$copy.new"
    run chmod 0755 "$copy.new"
    run mv -f "$copy.new" "$copy"
    run ln -sfn "${copy##*/}" "$link"
    printf 'geforce-sriov-driver: installed the patched plugin copy %s (SHA-256 %s)\n' "$copy" "$copy_digest"
}

# The module set this request builds and installs.
wanted_modules() {
    set -- nvidia
    [ "$display" -eq 0 ] || set -- "$@" nvidia-modeset nvidia-drm
    [ "$host_cuda" -eq 0 ] || set -- "$@" nvidia-uvm
    printf '%s\n' "$*"
}
wanted=$(wanted_modules)
# A module's installed path: the kernel's own answer when it already exists, otherwise the directory of
# the nvidia module, which is where NVIDIA's package puts its companions.
module_dest() {
    found=$(modinfo -k "$kernel" -n "$1" 2>/dev/null || true)
    if [ -n "$found" ] && [ -f "$found" ]; then
        printf '%s\n' "$found"
    else
        printf '%s/%s.ko\n' "${module%/*}" "$1"
    fi
}

# The version of the Linux driver package's programs that nvidia.ko has to accept besides its own: the Linux driver
# package's, in a display build whose Linux driver package (the record names it) is not of the host's version.
wants_accept() {
    [ "$display" -eq 1 ] || return 0
    w_second=$(gs_record_get "$record" driver_second_version)
    case $w_second in
        ''|none|"$host_version") ;;
        "$(gs_record_get "$record" client_accept_unmatched)") ;;
        *) printf '%s' "$w_second" ;;
    esac
}

# A rerun on a host that is already prepared for this kernel and this module set is a no-op, not a
# refusal, once the patched plugin copy is in place; any other copy is replaced with it. A feature
# request whose set differs from the record never takes this path, so --host-cuda on an already
# patched core still builds and installs UVM.
if [ -f "$record" ] && [ ! -L "$record" ]; then
    rec_kernel=$(gs_record_get "$record" kernel)
    rec_hash=$(gs_record_get "$record" module_sha256)
    rec_display=$(gs_record_get "$record" display)
    rec_cuda=$(gs_record_get "$record" host_cuda)
    rec_set=$(gs_record_modules "$record" | tr '_' '-' | sort | tr '\n' ' ')
    want_set=$(printf '%s\n' "$wanted" | tr ' ' '\n' | sort | tr '\n' ' ')
    [ -n "$rec_set" ] || rec_set='nvidia '
    # Every module the record names must still be the one it installed; a changed companion is a
    # request to build again, not a reason to report the old installation as current.
    rec_intact=1
    recorded_names=$(gs_record_modules "$record")
    for recorded in $recorded_names; do
        recorded_path=$(gs_record_get "$record" "module_${recorded}_path")
        recorded_sha=$(gs_record_get "$record" "module_${recorded}_sha256")
        if [ -z "$recorded_sha" ] || [ ! -f "$recorded_path" ] ||
            [ "$(sha "$recorded_path")" != "$recorded_sha" ]; then
            rec_intact=0
            break
        fi
    done
    # The installed build was made for another ada_placement than the one asked for: it is built again.
    rec_placement=$(gs_record_get "$record" ada_placement)
    # An nvidia.ko of an earlier build of this version carries the guest framebuffer cap, which this package no longer builds.
    # The record of that build names it (guest_fb=1); the module is built again so that the cap and its /proc file are gone.
    if [ "$(gs_record_get "$record" guest_fb)" = 1 ]; then
        printf '%s\n' 'geforce-sriov-driver: the installed nvidia.ko was built with a guest framebuffer cap, which this package does not build; the modules are built again'
    elif [ "$rec_placement" != "$ada_selected" ]; then
        printf 'geforce-sriov-driver: the installed nvidia.ko was built with ada_placement=%s and ada_placement=%s is asked for; the modules are built again\n' "${rec_placement:-absent}" "$ada_selected"
    elif [ "$(gs_record_get "$record" client_accept)" != "$(wants_accept)" ]; then
        printf 'geforce-sriov-driver: the installed nvidia.ko does not accept the programs of the Linux driver package (client_accept=%s is recorded, %s is wanted); the modules are built again\n' \
            "$(gs_record_get "$record" client_accept)" "$(wants_accept)"
    elif [ "$rec_intact" -eq 1 ] && [ "$rec_kernel" = "$kernel" ] && [ "$rec_hash" = "$(sha "$module")" ] &&
        [ "${rec_display:-0}" = "$display" ] && [ "${rec_cuda:-0}" = "$host_cuda" ] &&
        [ "$rec_set" = "$want_set" ]; then
        # An earlier build for another kernel published its record over this running kernel's, which was
        # then only kept under $records: the running kernel's is the published one again.
        if [ "$record" != "$published" ] && [ "$kernel" = "$(uname -r)" ] && [ ! -L "$published" ]; then
            archive_published "$kernel"
            cp -p "$record" "$published.gs-tmp" && sync "$published.gs-tmp" && mv -f "$published.gs-tmp" "$published" ||
                fail "could not write $published"
            archived=
            rm -f "$record"
            sync_dir "$state"
            [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
            record=$published
            recovered=1
            printf 'geforce-sriov-driver: the build record of %s is the published one again\n' "$kernel"
        fi
        patched=0
        for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
            candidate=$dir/$plugin_name.geforce-sriov
            if [ -f "$candidate" ] && [ ! -L "$candidate" ] && [ "$(sha "$candidate")" = "$gs_patched_plugin" ]; then
                patched=1
            fi
        done
        gs_version_row "$host_version" || :
        if [ "$patched" -eq 1 ]; then
            printf 'geforce-sriov-driver: already prepared for %s (module %s); nothing to do\n' "$kernel" "$rec_hash"
            for dir in "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64" "$root/usr/lib"; do
                copy_mode "$dir/$plugin_name.geforce-sriov"
            done
            stage_libs_prepared
            keep_inputs 0 || printf 'geforce-sriov-driver: warning: the build inputs could not be kept under %s\n' "$inputs" >&2
            # The one line the CLI reads to learn what this run changed (driver_line() in install.c).
            # It is not a message: the wording of every other line is free. No such line means changed.
            if [ "$mode_fixed" -eq 1 ] || [ "$recovered" -eq 1 ]; then
                printf 'install_result=changed\n'
            else
                printf 'install_result=unchanged\n'
            fi
            exit 0
        fi
        printf 'geforce-sriov-driver: already prepared for %s (module %s); replacing the plugin copy, which is not the patched one\n' "$kernel" "$rec_hash"
        find_plugin
        work=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") || fail 'cannot create temporary directory'
        trap 'rm -rf "$work"; [ -z "$driver_work" ] || rm -rf "$driver_work"' 0
        trap 'exit 1' HUP INT TERM
        make_copy "$work/patched-plugin"
        copy_digest=$(sha "$work/patched-plugin") || fail 'could not hash the patched plugin copy'
        replace_copy "$work/patched-plugin"
        stage_libs_prepared
        keep_inputs 0 || printf 'geforce-sriov-driver: warning: the build inputs could not be kept under %s\n' "$inputs" >&2
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
        other*) refuse "$conf sets NVreg_EnableGpuFirmware=${verdict#other }; the NVIDIA host driver setup needs 0" ;;
    esac
done

work=$(mktemp -d "${TMPDIR:-/tmp}/geforce-sriov-driver.XXXXXX") || fail 'cannot create temporary directory'
committing=0
module_changed=0
install_modules=
retiring=

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
        restore record "$published"
        restore krecord "$records/$kernel"
        [ -z "$archived" ] || rm -f "$archived"
        rm -f "$journal" "$journal.gs-tmp" "$state/plugin-copy.gs-new"
        rm -rf "$state/txn"
        for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
            eval "restore m-$flat \"\$dest_$flat\""
            eval "rm -f \"\$dest_$flat.gs-new\""
            eval "restore s-$flat \"\$state_dir_$flat/$name.ko\""
        done
        for name in $retiring; do
            eval "restore r-$name \"\$prev_path_$name\""
            eval "restore rs-$name \"\$state/stock/\$kernel/\$prev_disk_$name\""
        done
        restore module "$module"
        restore backup "$backup"
        rm -rf "$build"
        if [ "$module_changed" -eq 1 ]; then depmod -a "$kernel" || printf 'geforce-sriov-driver: rollback depmod failed for %s\n' "$kernel" >&2; fi
    fi
    rm -rf "$work"
    [ -z "$driver_work" ] || rm -rf "$driver_work"
    if [ "$status" -ne 0 ] && [ "$status" -ne 3 ]; then status=1; fi
    exit "$status"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

# ---- the inputs, as this run's own copies ---------------------------------------
#
# A build is certain from here on, and root runs it: make in the trees, sh on a package. Each input
# the run was given is copied once into $work, which only root can enter. The copy is root's,
# nobody else can write to it, and no symbolic link leads out of it. Everything below reads, checks,
# runs and builds the copy, and keep_inputs keeps the copy: what was checked is what runs, and
# what was built is what is kept. An input that an earlier run kept is such a copy already.
mkdir "$work/in" || fail 'cannot create the directory for the copies of the inputs'
taken_companion=
taken_nvkms=
# take_file WHAT GIVEN NAME and take_tree WHAT GIVEN NAME: $taken is the copy of GIVEN in $work/in/NAME.
take_file() {
    t_real=$(readlink -f -- "$2" 2>/dev/null) && [ -f "$t_real" ] || refuse "$1 missing: $2"
    taken=$work/in/$3/${2##*/}
    need_space "${work%/*}" "$2"
    mkdir "$work/in/$3" && cp -P --no-preserve=ownership -- "$t_real" "$taken" && [ -f "$taken" ] && [ ! -L "$taken" ] &&
        chmod ug-s,go-w "$taken" || refuse "could not copy $2 to $work. NVIDIA's files are as they were"
}
take_tree() {
    [ -d "$2" ] || refuse "$1 missing: $2"
    t_real=$(readlink -f -- "$2") || refuse "$1 missing: $2"
    taken=$work/in/$3/${t_real##*/}
    need_space "${work%/*}" "$2"
    mkdir "$work/in/$3" && gs_copy_tree "$2" "$taken" ||
        refuse "$2 is not built from; the line above says why. NVIDIA's files are as they were"
}
if [ "$src_kept" -eq 1 ]; then
    gs_tree_closed "$src" ||
        refuse "the kernel tree that an earlier install kept is not built from; the line above says why. NVIDIA's files are as they were. Run \`geforce-sriov install --nvidia-source DIR\`; that keeps the tree again"
else
    need_space "${work%/*}" "$src"
    gs_copy_tree "$src" "$work/in/kernel" ||
        refuse "$src_shown is not built from; the line above says why. NVIDIA's files are as they were"
    # What was read so far was read from the tree itself: the copy must be the same tree.
    [ "$(sha "$work/in/kernel/nvidia/nv-kernel.o_binary" 2>/dev/null)" = "$actual" ] &&
        [ "$(gs_source_version "$work/in/kernel" 2>/dev/null)" = "$host_version" ] ||
        refuse "the kernel tree $src_shown changed while this run read it. NVIDIA's files are as they were. Run install again"
    src=$work/in/kernel
fi

# ---- version policy and patch preflight -------------------------------------
GS_VERSION_NOTICE_FILE=$work/version-notice
export GS_VERSION_NOTICE_FILE
work_dir=$work/version
mkdir "$work_dir" || fail 'cannot create the version scratch directory'
second_version=none
public_version=none
identity_failed=0
detect() {
    role=$1 input=$2
    out=$(gs_detect_identity "$role" "$input" "$work_dir" 2>"$work_dir/identity.err") || return 1
    printf '%s\n' "$out"
}
host_identity=$(detect host "$src") || identity_failed=1
if [ "$identity_failed" -eq 0 ]; then
    detected=$(printf '%s\n' "$host_identity" | sed -n 's/^version=//p')
    [ -z "$detected" ] || host_version=$detected
fi
if [ "$identity_failed" -ne 0 ]; then
    reason=$(sed -n '$p' "$work_dir/identity.err" 2>/dev/null)
    refuse "cannot determine the host driver version from $src_shown: ${reason:-no version definition}"
fi
use_kept_inputs
# The Linux driver package and the public source as the user knows them, then this run's copies of them.
second_shown=${companion_file:-$companion_source}
nvkms_shown=$nvkms_source
if [ "$kept_companion" -eq 0 ]; then
    # A package that is the kept one, byte for byte, is run from the kept copy, which is root's
    # already: a second copy of some hundred megabytes would add nothing.
    same_kept=$(index_get companion)
    if [ -n "$companion_file" ] && [ -f "$companion_file" ] && [ "$(index_get companion_kind)" = file ] &&
        [ -f "$same_kept" ] && [ ! -L "$same_kept" ] &&
        [ "$(sha "$companion_file")" = "$(index_get companion_sha256)" ] &&
        [ "$(sha "$same_kept")" = "$(index_get companion_sha256)" ]; then
        companion_file=$same_kept
    elif [ -n "$companion_file" ]; then
        take_file 'Linux driver package' "$companion_file" companion
        companion_file=$taken
        taken_companion=$taken
    elif [ -n "$companion_source" ]; then
        take_tree 'Linux driver package directory' "$companion_source" companion
        companion_source=$taken
        taken_companion=$taken
    fi
fi
if [ "$display" -eq 1 ] && [ -n "$nvkms_source" ] && [ "$kept_nvkms" -eq 0 ]; then
    if [ -d "$nvkms_source" ]; then
        take_tree 'NVKMS source' "$nvkms_source" nvkms
    else
        take_file 'NVKMS source' "$nvkms_source" nvkms
    fi
    nvkms_source=$taken
    taken_nvkms=$taken
fi
if [ -n "$companion_file" ]; then
    [ -f "$companion_file" ] || refuse "Linux driver package missing: $second_shown"
    gs_package_check second "$companion_file" "$second_shown" ||
        refuse "the Linux driver package $second_shown was not run; the line above says why"
    mkdir "$work/companion" || fail 'cannot create the companion extraction directory'
    ( cd "$work/companion" && sh "$companion_file" --extract-only ) >"$work/companion.log" 2>&1 ||
        refuse "could not extract $second_shown with --extract-only"
    found=
    for candidate in "$work"/companion/*/; do
        [ -d "$candidate" ] || continue
        [ -z "$found" ] || refuse "package extraction produced more than one directory: $second_shown"
        found=${candidate%/}
    done
    [ -n "$found" ] || refuse "package extraction produced no directory: $second_shown"
    companion_source=$found
fi
if [ -n "$companion_source" ]; then
    [ -d "$companion_source" ] || refuse "Linux driver package directory missing: $second_shown"
    companion_root=$companion_source
    if [ ! -d "$companion_root/nvidia-uvm" ] && [ -d "$companion_root/kernel/nvidia-uvm" ]; then
        companion_root=$companion_root/kernel
    fi
    companion=$companion_root
    if [ "$identity_failed" -eq 0 ]; then
        second_identity=$(detect second "$companion_source") || refuse "cannot identify the Linux driver package: $second_shown"
        second_version=$(printf '%s\n' "$second_identity" | sed -n 's/^version=//p')
        [ -n "$second_version" ] || refuse "the Linux driver package declares no version: $second_shown"
        if [ "$kept_companion" -eq 1 ] && [ "$second_version" != "$(index_get companion_version)" ]; then
            refuse "the Linux driver package that an earlier install kept was changed since: $(index_get companion) declares version $second_version, and the one that was kept declared $(index_get companion_version). NVIDIA's files are as they were. Run \`geforce-sriov install\` in the source folder with --companion-file PATH (or --companion-source DIR); that keeps it again"
        fi
    else
        refuse "cannot identify the host source, so the Linux driver package cannot be classified: $src_shown"
    fi
fi
if [ "$display" -eq 1 ]; then
    [ -n "$nvkms_source" ] || refuse "--display needs --nvkms-source PATH (NVIDIA's public kernel module source). Pass it, or use --no-display"
    [ -e "$nvkms_source" ] || refuse "NVKMS source missing: $nvkms_shown"
    public_identity=$(detect public "$nvkms_source") || refuse "cannot identify the public source: $nvkms_shown"
    public_version=$(printf '%s\n' "$public_identity" | sed -n 's/^version=//p')
    [ -n "$public_version" ] || refuse "the public source declares no version: $nvkms_shown"
    if [ "$kept_nvkms" -eq 1 ] && [ "$public_version" != "$(index_get nvkms_version)" ]; then
        refuse "NVIDIA's public kernel module source that an earlier install kept was changed since: $nvkms_source declares version $public_version, and the one that was kept declared $(index_get nvkms_version). NVIDIA's files are as they were. Run \`geforce-sriov install\` in the source folder with --nvkms-source PATH; that keeps it again"
    fi
fi
if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
    [ -n "$companion_source" ] || refuse "--display and --host-cuda need the Linux driver package: pass --companion-file PATH or --companion-source DIR, or use --no-display and --no-host-cuda"
fi
# A package file contributes its pinned digest; an extracted directory contributes none. The public
# source digest was never pinned, so a display build records none and identifies P by version only.
second_sha=none
public_sha=none
[ -z "$companion_file" ] || second_sha=$(gs_sha256 "$companion_file") || second_sha=none
# The digest of each input as this build uses it, a file's own or that of a whole tree. What a run
# keeps for a later build must have it, so a kept input is always one a build was made from.
input_source_sha=$(gs_tree_digest "$src") || fail "could not hash the kernel tree $src_shown"
input_second_sha=none
input_public_sha=none
if [ -n "$companion_file" ]; then
    input_second_sha=$second_sha
elif [ -n "$companion_source" ]; then
    input_second_sha=$(gs_tree_digest "$companion_source") || fail "could not hash $second_shown"
fi
if [ "$display" -eq 1 ]; then
    if [ -d "$nvkms_source" ]; then
        input_public_sha=$(gs_tree_digest "$nvkms_source") || fail "could not hash $nvkms_shown"
    else
        input_public_sha=$(gs_sha256 "$nvkms_source") || fail "could not hash $nvkms_shown"
    fi
fi

# Preflight every revision when a digest this step records does not match the input, and for the
# display build, whose idx 39 revision has no recorded input/output pair to compare.
preflight_state=none
object_chain="$actual"
plugin_chain="$plugin_hash"
if [ "$known_object" -eq 1 ] && [ "$known_plugin" -eq 1 ] && [ "$plugin_hash" = "$gs_stock_plugin" ]; then
    object_chain=$gs_object_chain
    plugin_chain=$gs_plugin_chain
fi
if [ "$known_object" -eq 0 ] || [ "$known_plugin" -eq 0 ] || [ "$plugin_hash" != "$gs_stock_plugin" ] || [ "$display" -eq 1 ] ||
    { [ "$ada_selected" -eq 1 ] && [ -z "$gs_placement_offsets" ]; }; then
    for tool in modprobe modinfo od tr grep; do
        command -v "$tool" >/dev/null 2>&1 || refuse "required command missing: $tool"
    done
    preflight=$work/preflight
    preflight_ok=1
    gs_preflight "$patcher" "$src/nvidia/nv-kernel.o_binary" "$plugin" "$display" "$work_dir" "$host_version" "$ada_selected" >"$preflight" 2>"$work/preflight.err" ||
        preflight_ok=0
    cat "$work/preflight.err" >&2
    if [ "$preflight_ok" -eq 0 ]; then
        # An Ada build whose placement patch does not find its window is not built another way.
        if [ "$ada_selected" -eq 1 ] &&
            awk '/^preflight ada-placement: / && !/skipped after/ { f = 1 } /^anchor ada-placement/ { f = 1 } END { exit !f }' "$work/preflight.err"; then
            if grep -E '^preflight ada-placement: ' "$work/preflight.err" | grep -q 'unique:'; then
                placement_refuse 'the code window of the placement change was found more than once in the driver'
            else
                placement_refuse 'the code window of the placement change was not found in the driver'
            fi
        fi
        refuse 'a required patch anchor did not match; nothing was installed'
    fi
    preflight_state=$(sed -n 's/^preflight_state=//p' "$preflight" | tail -n 1)
    case $preflight_state in
        confirmed|unconfirmed) ;;
        *) refuse 'the patch preflight produced no usable state; nothing was installed' ;;
    esac
    object_chain=$actual
    plugin_chain=$plugin_hash
    while IFS= read -r line; do
        case $line in
            stage=*) stage=${line#stage=} ;;
            output_sha256=*)
                case $stage in
                    scheduler|module-gate|vf-count|display-idx39) object_chain="$object_chain,${line#output_sha256=}" ;;
                    plugin-mixed|plugin-div4) plugin_chain="$plugin_chain,${line#output_sha256=}" ;;
                esac ;;
        esac
    done <"$preflight"
fi
host_sha=$driver_package_sha
classification=$(gs_classify "$host_sha" "$second_sha" "$public_sha" "$public_version" "$object_chain" \
    "$plugin_chain" "$display" "$host_cuda" "$preflight_state" "$ada_selected") || refuse 'the version classification failed'
confirmation=$(printf '%s\n' "$classification" | sed -n 's/^confirmation=//p')
profile=$(printf '%s\n' "$classification" | sed -n 's/^evidence_profile=//p')
# A build can be unconfirmed for two different reasons, and the notice names the one that applies.
# When a revision was admitted by its anchors, the driver is not one of the recorded versions. When
# every revision matched its recorded digest, the driver is the expected one and only this
# combination of features and input packages has no recorded test.
if [ "$confirmation" != confirmed-inputs ]; then
    if [ "$preflight_state" = unconfirmed ]; then
        gs_version_notice "$host_version" || refuse 'could not print the version notice'
    else
        printf 'geforce-sriov-driver: the driver files match their recorded digests, but this combination (display=%s, host_cuda=%s, with these input packages) is not confirmed to work. The build continues and every check still applies.\n' \
            "$display" "$host_cuda" >&2
    fi
fi
record_inputs=$(gs_version_fields "$host_version" "$second_version" "$public_version" \
    "$confirmation" "$profile" "$display" "$host_cuda" "$host_sha" "$second_sha" "$public_sha") ||
    refuse 'could not compose the version fields'
# The admission and state of each revision, for the build record.
patch_field() {
    stage=$1 key=$2 fallback=$3
    value=$(awk -v want="stage=$stage" -v key="$key" '
        $0 == want { in_stage = 1; next }
        /^stage=/ { in_stage = 0 }
        in_stage && index($0, key "=") == 1 { sub(/^[^=]*=/, ""); print; exit }' "$work/preflight" 2>/dev/null)
    [ -n "$value" ] || value=$fallback
    printf 'patch_%s_%s=%s\n' "$(printf '%s' "$stage" | tr '-' '_')" "$key" "$value"
}

# One value of a revision's block in the preflight result; empty when the block has no such line.
stage_value() {
    awk -v want="stage=$1" -v key="$2" '
        $0 == want { in_stage = 1; next }
        /^stage=/ { in_stage = 0 }
        in_stage && index($0, key "=") == 1 { sub(/^[^=]*=/, ""); print; exit }' "$work/preflight" 2>/dev/null
}
# The .text offsets at which this build's module differs from the unpatched one, one per line in
# ascending order. For each object revision the bytes it changed are placed in the built module
# through the function, and the offset inside it, that the patch tool reported for that revision.
# Returns 1 when the tool reported no function for a revision, or when a function is not a unique
# symbol of the built module's .text; the caller then keeps the recorded offsets of the version.
reported_offsets() {
    [ -f "$work/preflight" ] || return 1
    # shellcheck disable=SC1091 # the library is installed beside the script
    . "$libdir/gs-driver-elf.sh"
    : >"$work/expected.raw"
    before=$work/object
    for pair in scheduler:scheduler module-gate:gate vf-count:patched $placement_pairs; do
        stage=${pair%%:*}
        after=$work/${pair##*:}
        fn=$(stage_value "$stage" function)
        fo=$(stage_value "$stage" function_offset)
        at=$(stage_value "$stage" file_offset)
        [ -n "$fn" ] || return 1
        for number in "$fo" "$at"; do
            case $number in
                0[xX][0-9a-fA-F]*|[0-9]*) ;;
                *) return 1 ;;
            esac
        done
        row=$(gs_elf_symbol "$built" "$fn" 2>/dev/null) || return 1
        value=$(printf '%s\n' "$row" | awk '{print $1}')
        ndx=$(printf '%s\n' "$row" | awk '{print $3}')
        [ "$(gs_elf_sections "$built" | awk -v want="$ndx" '$1 == want { print $2; exit }')" = .text ] || return 1
        base=$((0x$value + fo - at))
        { cmp -l "$before" "$after" 2>/dev/null || true; } |
            awk -v base="$base" '{ print $1 - 1 + base }' >>"$work/expected.raw"
        before=$after
    done
    [ -s "$work/expected.raw" ] || return 1
    sort -n -u "$work/expected.raw" | awk '{ printf "0x%x\n", $1 }'
}

# ---- staged build ------------------------------------------------------------
# An Ada card adds the ada-placement row after the plain chain; without one the row is left out.
# The object the build goes on with is the plain chain's last (patched), or the one after the row (placed).
placement_pairs=
[ "$ada_selected" -eq 0 ] || placement_pairs='ada-placement:placed'
run cp -a "$src" "$work/tree"
run cp "$src/nvidia/nv-kernel.o_binary" "$work/object"
run "$patcher" apply "$work/object" "$work/scheduler"
run "$patcher" ada apply "$work/scheduler" "$work/gate"
run "$patcher" ada apply "$work/gate" "$work/patched"
obj_final=$work/patched
want_object=$gs_patched_object
if [ "$ada_selected" -eq 1 ]; then
    run "$patcher" ada apply "$work/patched" "$work/placed" || placement_refuse 'the placement change did not apply to the driver'
    obj_final=$work/placed
    # The block records it for a confirmed version; any other driver has no digest to compare with.
    want_object=$gs_placement_object
fi
actual=$(sha "$obj_final") || fail 'could not hash patched object'
if [ "$known_object" -eq 1 ] && [ "$display" -eq 0 ] && [ -n "$want_object" ]; then
    [ "$actual" = "$want_object" ] || refuse "patched object SHA-256 is $actual; expected $want_object"
fi
idx39_function=
idx39_offset=
if [ "$display" -eq 1 ]; then
    run "$patcher" display apply "$obj_final" "$work/patched39"
    display_out=$(run "$patcher" display inspect "$work/patched39" 2>/dev/null || true)
    idx39_function=$(printf '%s\n' "$display_out" | sed -n 's/^function=//p' | tail -n 1)
    idx39_offset=$(printf '%s\n' "$display_out" | sed -n 's/^function_offset=//p' | tail -n 1)
    [ -n "$idx39_function" ] && [ -n "$idx39_offset" ] ||
        refuse 'the idx 39 patch reported no located function offset'
    case $idx39_offset in
        0x*|0X*) idx39_offset=$((idx39_offset)) ;;
        *[!0-9]*) refuse "the idx 39 patch reported an invalid function offset: $idx39_offset" ;;
    esac
    actual=$(sha "$work/patched39") || fail 'could not hash the idx 39 object'
    cp -p "$work/patched39" "$obj_final" || fail 'could not keep the idx 39 object'
fi
if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
    # shellcheck disable=SC1091 # the library is installed beside the script
    . "$libdir/gs-driver-elf.sh"
    find_patch_dir || refuse "the source patches are not installed next to $0 (driver-patches/gpu-allow.patch and allocation-threshold.patch)"
    rm -rf "$work/tree"
    # The staging library accepts a version in digits and colons only, so an absent public version
    # is passed as an empty field rather than the literal "none" that the record uses.
    stage_public=$public_version
    [ "$display" -eq 1 ] || stage_public=
    stage_nvkms=$nvkms_source
    [ "$display" -eq 1 ] || stage_nvkms=
    gs_stage_tree "$src" "$companion" "$work/tree" "$display" "$host_cuda" \
        "$host_version" "$stage_public" "$stage_nvkms" ||
        refuse 'the source staging step refused; nothing was installed'
    run cp "$obj_final" "$work/tree/nvidia/nv-kernel.o_binary"
    if [ "$display" -eq 1 ]; then
        gs_apply_source_patch "$work/tree" "$patch_dir/gpu-allow.patch" ||
            refuse 'the DRM GPU-selection patch did not match uniquely; nothing was installed'
        gs_apply_source_patch "$work/tree" "$patch_dir/allocation-threshold.patch" ||
            refuse 'the allocation-threshold patch did not match uniquely; nothing was installed'
        # The programs of the Linux driver package are of another version than this driver: nvidia.ko accepts that one version (NV_ESC_CHECK_VERSION_STR).
        case $second_version in
            none|"$host_version") ;;
            ''|*[!0-9.]*) refuse "the Linux driver package's version is not usable: $second_version" ;;
            *)
                [ -f "$patch_dir/client-accept.patch" ] ||
                    refuse "the source patches are not installed next to $0 (driver-patches/client-accept.patch)"
                sed "s/@GS_CLIENT_ACCEPT@/$second_version/g" "$patch_dir/client-accept.patch" >"$work/client-accept.patch" ||
                    refuse 'the client-accept patch could not be prepared; nothing was installed'
                # A source this patch does not fit is built without it: the display build of 0.3.0 does not need it, and
                # host_desktop=off must not lose the build. The graphics libraries are then not installed (their
                # version is not the one nvidia.ko accepts).
                if gs_apply_source_patch "$work/tree" "$work/client-accept.patch"; then
                    client_accept=$second_version
                    printf 'geforce-sriov-driver: nvidia.ko accepts programs of version %s besides its own\n' "$client_accept"
                else
                    client_accept_unmatched=$second_version
                    printf "geforce-sriov-driver: warning: the client-accept patch does not match this driver's source, so nvidia.ko is built without it and does not accept programs of version %s; NVIDIA's graphics libraries are not installed\n" "$second_version" >&2
                fi ;;
        esac
    fi
    run make -C "$work/tree" NV_EXCLUDE_BUILD_MODULES= "KERNEL_UNAME=$kernel" "SYSSRC=$headers" \
        "NV_KERNEL_MODULES=$wanted" modules
else
    run cp "$obj_final" "$work/tree/nvidia/nv-kernel.o_binary"
    run make -C "$work/tree" NV_EXCLUDE_BUILD_MODULES= "KERNEL_UNAME=$kernel" "SYSSRC=$headers" modules
fi
built=$work/tree/nvidia.ko
[ -f "$built" ] || fail "built module missing: $built"

if [ "$display" -eq 1 ]; then
    # The object chain is the identity. A confirmed-inputs build must be exactly the recorded
    # chain; an anchor-admitted build of another version is accepted as best effort, recorded
    # as unconfirmed, and its own digest is kept. A whole-module comparison cannot be used for
    # this tier: a reviewed source patch (the allocation threshold) changes .text on purpose.
    patched_sha=$(sha256sum "$obj_final" | cut -d' ' -f1)
    if [ "$confirmation" = confirmed-inputs ]; then
        want_display=$gs_display_object
        [ "$ada_selected" -eq 0 ] || want_display=$gs_placement_display_object
        [ "$patched_sha" = "$want_display" ] ||
            refuse "the display object is $patched_sha, not the recorded chain endpoint; nothing was installed"
    elif [ "$(patch_field display-idx39 admission anchor)" = 'patch_display_idx39_admission=anchor' ]; then
        printf 'geforce-sriov-driver: display object %s admitted by anchors, not confirmed\n' "$patched_sha"
    else
        printf 'geforce-sriov-driver: display object %s matches its recorded digest\n' "$patched_sha"
    fi
    gs_idx39_finished "$built" "$idx39_function" "$idx39_offset" ||
        refuse 'the finished module does not carry the idx 39 change; nothing was installed'
else
    # A rebuild differs outside .text by design (build-id, banner, DWARF), so compare only the
    # executable section of each module. cmp reports one-based decimal positions; the expected
    # offsets are zero-based and relative to the start of .text.
    # The build is compared with NVIDIA's own module. That is the installed one, unless the
    # installed one is the build the record names for this kernel: then it is this script's own,
    # with whatever its feature set changed (a display build carries the idx 39 change and the
    # allocation threshold), and NVIDIA's module is the backup whose digest the record holds.
    reference=$module
    if [ -f "$record" ] && [ ! -L "$record" ] && [ "$(gs_record_get "$record" kernel)" = "$kernel" ] &&
        [ "$(gs_record_get "$record" module_sha256)" = "$stock_hash" ] &&
        [ -f "$backup" ] && [ ! -L "$backup" ] &&
        [ "$(sha "$backup")" = "$(gs_record_get "$record" stock_sha256)" ]; then
        reference=$backup
    fi
    show objcopy -O binary -j .text "$reference" "$work/stock.text"
    objcopy -O binary -j .text "$reference" "$work/stock.text" || refuse "could not read .text from $reference"
    show objcopy -O binary -j .text "$built" "$work/built.text"
    objcopy -O binary -j .text "$built" "$work/built.text" || refuse "could not read .text from $built"
    [ -s "$work/stock.text" ] || refuse "module has no .text section: $reference"
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
    # No difference from the installed module means the installed module already is this build.
    # No difference from the backup of NVIDIA's module means the build carries none of the
    # revisions: that is never "already installed", and the comparison below refuses it.
    already=0
    [ "$reference" != "$module" ] || [ -s "$work/diff" ] || already=1
    # Where the built module may differ from the installed one. For the recorded object these are
    # the eight offsets measured on its version. Another host version is not guaranteed to work, but
    # it may be tried: when its revisions were admitted by their anchors, the comparison uses the
    # offsets the patch tool reported for that build, and it is just as exact.
    expected_what='the eight expected offsets'
    expected_list=$gs_plain_offsets
    if [ "$ada_selected" -eq 1 ] && [ -n "$gs_placement_offsets" ]; then
        expected_list="$gs_plain_offsets $gs_placement_offsets"
        expected_what='the eight expected offsets and the one that the Ada placement change adds'
    fi
    for at in $expected_list; do
        printf '%s\n' "$((at))"
    done | sort -n | awk '{ printf "0x%x\n", $1 }' >"$work/expected"
    if { [ "$known_object" -eq 0 ] || { [ "$ada_selected" -eq 1 ] && [ -z "$gs_placement_offsets" ]; }; } && reported_offsets >"$work/expected.reported"; then
        cp "$work/expected.reported" "$work/expected"
        expected_what='the offsets the patch tool reported for this build'
        printf 'geforce-sriov-driver: the built module is compared with the installed one at %s: %s\n' \
            "$expected_what" "$(tr '\n' ' ' <"$work/expected")"
    elif [ -z "$gs_plain_offsets" ]; then
        refuse "no .text offsets are recorded for driver $host_version, and the patch tool reported none for this build, so the built module cannot be compared with the installed one; nothing was installed"
    fi
    # Identical .text means the installed module is already this build. The record names this kernel
    # and module only when the check above matched, so here it is missing or names another kernel or
    # module (one record serves every kernel): with the rest of the installation in place, record this
    # module for this kernel, taking stock_sha256 from this kernel's backup once its .text differs from
    # this build at exactly the eight offsets. The installed plugin copy must be the patched one.
    # Otherwise refuse, because continuing would back up this patched module over the stock backup.
    if [ "$already" -eq 1 ] && [ -f "$record" ] && [ ! -L "$record" ] &&
        [ "$(gs_record_get "$record" kernel)" = "$kernel" ] &&
        [ "$(gs_record_get "$record" module_sha256)" = "$stock_hash" ]; then
        # The record already names this kernel and this installed module. The feature set or a
        # companion module changed, so the transaction below still runs: it rewrites the record
        # with the new composition and retires a module this request no longer builds.
        :
    elif [ "$already" -eq 1 ]; then
        incomplete="the installed module $module is already this build, but $published, $config or $copy is missing or wrong. Reinstall the stock module first, or install with --driver=skip to keep this module as it is"
        # Recording the installed module is all the plain route needs here. A feature request also
        # has companion modules to build and install, and this record cannot authorise that.
        if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
            refuse "$incomplete"
        fi
        copy_hash=
        if [ -f "$copy" ] && [ ! -L "$copy" ]; then
            copy_hash=$(sha "$copy") || fail "could not hash $copy"
        fi
        case $copy_hash in "$gs_patched_plugin") ;; *) refuse "$incomplete" ;; esac
        { [ -f "$config" ] || [ -n "$kept" ]; } &&
            [ -f "$backup" ] && [ ! -L "$backup" ] && [ ! -L "$published" ] && [ ! -d "$published" ] || refuse "$incomplete"
        objcopy -O binary -j .text "$backup" "$work/backup.text" 2>/dev/null || refuse "$incomplete"
        { cmp -l "$work/backup.text" "$work/built.text" 2>/dev/null || true; } | awk '{printf "0x%x\n", $1 - 1}' >"$work/backup-offsets"
        cmp -s "$work/expected" "$work/backup-offsets" || refuse "$incomplete"
        backup_hash=$(sha "$backup") || fail "could not hash $backup"
        installed_id=$(readelf -n "$module" | awk '$1 == "Build" && $2 == "ID:" {print $3; exit}')
        [ -n "$installed_id" ] || refuse "installed module has no GNU build-id: $module"
        [ "$copy_hash" = "$gs_patched_plugin" ] || make_copy "$work/patched-plugin"
        archive_published "$kernel"
        if ! {
                printf '%s\n' "kernel=$kernel" "module_sha256=$stock_hash" "module_build_id=$installed_id" "stock_sha256=$backup_hash" "display=0" "host_cuda=0" \
                    "ada_placement=$ada_selected"
            } >"$published.tmp" ||
            ! mv -f "$published.tmp" "$published"; then
            rm -f "$published.tmp"
            fail "could not write $published"
        fi
        rm -f "$records/$kernel"
        [ ! -d "$records" ] || rmdir "$records" 2>/dev/null || :
        record=$published
        printf 'geforce-sriov-driver: recorded the installed module for %s\n' "$kernel"
        if [ "$copy_hash" = "$gs_patched_plugin" ]; then
            copy_mode "$copy"
        else
            copy_digest=$(sha "$work/patched-plugin") || fail 'could not hash the patched plugin copy'
            replace_copy "$work/patched-plugin"
        fi
        keep_inputs 0 || printf 'geforce-sriov-driver: warning: the build inputs could not be kept under %s\n' "$inputs" >&2
        exit 0
    else
        awk '{printf "0x%x\n", $1 - 1}' "$work/diff" >"$work/offsets"
        if ! cmp -s "$work/expected" "$work/offsets"; then
            unexpected=$(awk 'NR==FNR {expected[$0]=1; next} !($0 in expected) {print; exit}' "$work/expected" "$work/offsets")
            [ -n "$unexpected" ] || unexpected='none (expected offset missing)'
            refuse "module .text differences are not $expected_what; first unexpected offset: $unexpected"
        fi
    fi
fi

printf 'geforce-sriov-driver: the build record names ada_placement=%s\n' "$ada_selected"

# Every built module must match the kernel it was built for and the core it will load against,
# whatever the feature set. The patch tool's abi-check compares the vermagic, the stored CRCs, the
# core's own exports and the kernel header tree's Module.symvers; the built core is checked as a
# module too.
# The expected vermagic comes from the target kernel itself, read from one of its own modules. The
# installed nvidia module is used only when that kernel has none, because a stale or wrong nvidia
# module would otherwise define what is expected of its own replacement.
kernel_magic=
intree=$(find "$root/lib/modules/$kernel/kernel" -type f -name '*.ko*' -print -quit 2>/dev/null || true)
[ -z "$intree" ] || kernel_magic=$(modinfo -F vermagic "$intree" 2>/dev/null | sed 's/[[:space:]]*$//')
if [ -z "$kernel_magic" ]; then
    kernel_magic=$(modinfo -k "$kernel" -F vermagic nvidia 2>/dev/null | sed 's/[[:space:]]*$//')
    [ -n "$kernel_magic" ] ||
        refuse "cannot read the build string (vermagic) of kernel $kernel from one of its own modules or from the installed nvidia module"
    printf 'geforce-sriov-driver: kernel %s has no module of its own under %s; the expected kernel build string (vermagic) comes from the installed nvidia module\n' \
        "$kernel" "$root/lib/modules/$kernel/kernel"
fi
set -- --core "$built" --module "$built"
for name in $wanted; do
    [ "$name" = nvidia ] && continue
    [ -f "$work/tree/$name.ko" ] || fail "built module missing: $work/tree/$name.ko"
    set -- "$@" --module "$work/tree/$name.ko"
done
# A kernel built without module versioning stores no CRCs, so for the plain build only the
# vermagic can be compared there. A display or host CUDA build keeps the full check, as before.
case " $kernel_magic " in
    *' modversions '*) versioned=1 ;;
    *) versioned=0 ;;
esac
if [ "$versioned" -eq 1 ] || [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
    # Two providers: the kernel (its header tree's Module.symvers) and the sibling modules of
    # this same build (the tree's own Module.symvers, where nvidia-modeset exports the
    # nvKmsKapi* symbols nvidia-drm imports).
    symvers=$work/symvers.all
    compose_symvers() {
        {
            [ -f "$headers/Module.symvers" ] && cat "$headers/Module.symvers"
            [ -f "$work/tree/Module.symvers" ] && cat "$work/tree/Module.symvers"
            :
        } >"$symvers" 2>/dev/null || refuse "cannot compose the symbol tables under $work"
        [ -s "$symvers" ] || refuse "no symbol table is readable for the ABI check"
    }
    compose_symvers
    set -- "$@" --kernel-symvers "$symvers" --vermagic "$kernel_magic"
else
    compose_symvers() { :; }
    set -- "$@" --vermagic "$kernel_magic" --vermagic-only
fi
show "$patcher" abi-check "$@"
"$patcher" abi-check "$@" || refuse 'the ABI check refused a built module; nothing was installed'

if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
    manifest=$work/manifest
    gs_stage_manifest "$display" "$host_cuda" "$host_version" "$public_version" >"$manifest" ||
        refuse 'the staged module list could not be composed'
    for name in $wanted; do
        [ "$name" = nvidia ] && continue
        stamp=$(awk -v want="$name" '
            $1 ~ /^module=/ || $1 ~ /^component=/ {
                n = $1; sub(/^[^=]*=/, "", n)
                if (n == want) {
                    for (i = 2; i <= NF; i++) if ($i ~ /^stamp=/) { sub(/^stamp=/, "", $i); print $i }
                }
            }' "$manifest")
        [ -n "$stamp" ] || refuse "the staged manifest names no stamp for $name"
        version=$(modinfo -F version -- "$work/tree/$name.ko" 2>/dev/null | sed 's/[[:space:]]*$//')
        [ "$version" = "$stamp" ] ||
            refuse "version of $name is '$version'; the staged manifest records '$stamp'"
    done
    if [ "$display" -eq 1 ]; then
        LC_ALL=C grep -aq "$public_version" "$work/tree/nvidia-modeset.ko" ||
            refuse "the built nvidia-modeset carries no $public_version NVKMS core"
    fi
fi

make_copy "$work/patched-plugin"
show readelf -n "$built"
build_id=$(readelf -n "$built" | awk '$1 == "Build" && $2 == "ID:" {print $3; exit}')
[ -n "$build_id" ] || refuse "built module has no GNU build-id: $built"
module_hash=$(sha "$built") || fail 'could not hash built module'
copy_digest=$(sha "$work/patched-plugin") || fail 'could not hash the patched plugin copy'

# Every destination and every backup is checked before the first change.
install_modules=$wanted
dest=''
state_dir=''
for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
    eval "dest_$flat=\$(module_dest \"\$name\")"
    eval "state_dir_$flat=\$state/stock/\$kernel"
    eval "dest=\$dest_$flat"
    [ ! -d "$dest" ] && [ ! -L "$dest" ] ||
        refuse "an installation target is a directory or a symlink: $dest"
done
[ ! -d "$copy" ] && [ ! -d "$link" ] && [ ! -d "$published" ] && [ ! -d "$backup" ] && [ ! -d "$config" ] || refuse 'an installation target is a directory'

# A module the previous record installed and this request does not build is retired under the same
# ownership rule: it is removed or restored only while it still matches that record.
retiring=
prev_names=
path=''
want=''
previous=''
saved=''
if [ -f "$record" ] && [ ! -L "$record" ]; then
    prev_kernel=$(gs_record_get "$record" kernel)
    if [ "$prev_kernel" = "$kernel" ]; then
        prev_names=$(gs_record_modules "$record")
        install_flat=$(printf '%s' "$install_modules" | tr '-' '_')
        for name in $prev_names; do
            case " $install_flat " in
                *" $name "*) continue ;;
            esac
            eval "prev_path_$name=\$(gs_record_get \"\$record\" module_${name}_path)"
            eval "prev_sha_$name=\$(gs_record_get \"\$record\" module_${name}_sha256)"
            eval "prev_previous_$name=\$(gs_record_get \"\$record\" module_${name}_previous)"
            eval "prev_backup_$name=\$(gs_record_get \"\$record\" module_${name}_backup_sha256)"
            retiring="$retiring $name"
        done
    fi
fi

# A record that already names this kernel holds the true stock backup. When the installed module
# is already this build (a feature change), copying it back into the backup would replace stock
# with our own output; keep the verified backup instead.
record_for_kernel=0
if [ -f "$record" ] && [ ! -L "$record" ] &&
    [ "$(gs_record_get "$record" kernel)" = "$kernel" ]; then
    record_for_kernel=1
fi
# What becomes of each backup, decided before the first change. NVIDIA's own module is copied.
# A module this script built is never copied as if it were NVIDIA's: it is recognised by the build
# kept for this kernel under $build, whatever the record says or whether there is one. Then the
# backup that is there stays, or the module is one this script added and has no backup.
for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
    eval "dest=\$dest_$flat"
    eval "state_dir=\$state_dir_$flat"
    plan=copy
    recorded_previous=
    if [ "$record_for_kernel" -eq 1 ]; then
        recorded_previous=$(gs_record_get "$record" "module_${flat}_previous")
        recorded_backup=$(gs_record_get "$record" "module_${flat}_backup_sha256")
        recorded_built=$(gs_record_get "$record" "module_${flat}_sha256")
        [ -n "$recorded_built" ] || [ "$name" != nvidia ] || recorded_built=$(gs_record_get "$record" module_sha256)
        # The backup stays only while the installed module is still this script's build: the recorded
        # digest, or for the core the recorded build-id with other bytes (a copy signed for Secure Boot).
        # Any other module there (NVIDIA's package was installed again) is NVIDIA's own, and is what an
        # uninstall must put back, so it is copied over the old backup.
        ours=0
        if [ -n "$recorded_built" ] && [ -f "$dest" ] && [ ! -L "$dest" ]; then
            if [ "$(sha "$dest")" = "$recorded_built" ]; then
                ours=1
            elif [ "$name" = nvidia ]; then
                recorded_id=$(gs_record_get "$record" module_build_id)
                dest_id=$(readelf -n "$dest" 2>/dev/null | awk '$1 == "Build" && $2 == "ID:" {print $3; exit}' || true)
                [ -z "$recorded_id" ] || [ "$dest_id" != "$recorded_id" ] || ours=1
            fi
        fi
        if [ "$recorded_previous" = present ] && [ -n "$recorded_backup" ] &&
            [ -f "$state_dir/$name.ko" ] &&
            [ "$(sha "$state_dir/$name.ko")" = "$recorded_backup" ] && [ "$ours" -eq 1 ]; then
            plan=keep
        fi
    fi
    if [ "$plan" = copy ] && [ -f "$dest" ] && [ ! -L "$dest" ] && [ -f "$build/$name.ko" ] &&
        [ "$(sha "$dest")" = "$(sha "$build/$name.ko")" ]; then
        if [ "$recorded_previous" = absent ]; then
            plan=none
        elif [ -f "$state_dir/$name.ko" ] && [ ! -L "$state_dir/$name.ko" ] &&
            [ "$(sha "$state_dir/$name.ko")" != "$(sha "$dest")" ]; then
            plan=keep
        elif [ "$name" = nvidia ]; then
            refuse "$dest is a module geforce-sriov built for $kernel earlier, and $state_dir holds no backup of NVIDIA's own module, so there is nothing to put back at an uninstall. Put NVIDIA's module back first (\`dkms install --force nvidia/$host_version -k $kernel\`), then run this again"
        else
            plan=none
        fi
    fi
    eval "backup_plan_$flat=\$plan"
done

save module "$module"
save backup "$backup"
save record "$published"
save krecord "$records/$kernel"
save copy "$copy"
save link "$link"
save config "$config"
for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
    eval "save m-$flat \"\$dest_$flat\""
    eval "save s-$flat \"\$state_dir_$flat/$name.ko\""
done
for name in $retiring; do
    eval "prev_disk_$name=\${prev_path_$name##*/}"
    eval "save r-$name \"\$prev_path_$name\""
    eval "save rs-$name \"\$state/stock/\$kernel/\$prev_disk_$name\""
done
# The digest of each module as it was built, taken while the tree is still in this run's own
# directory: the journal names these, and a module is installed only with the digest it names.
for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
    eval "new_sha_$flat=\$(sha \"\$work/tree/$name.ko\")" || fail "could not hash the built $name"
done
committing=1
run mkdir -p "$state/build" "$state/stock/$kernel" "${config%/*}"
run rm -rf "$build"
run mv "$work/tree" "$build"

for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
    eval "dest=\$dest_$flat"
    eval "state_dir=\$state_dir_$flat"
    eval "plan=\$backup_plan_$flat"
    previous=absent
    saved=none
    case $plan in
        keep)
            saved=$(sha "$state_dir/$name.ko") || fail "could not hash the backup $state_dir/$name.ko"
            previous=present ;;
        none) run rm -f "$state_dir/$name.ko" ;;
        *)
            run rm -f "$state_dir/$name.ko"
            if [ -f "$dest" ] && [ ! -L "$dest" ]; then
                run cp -p "$dest" "$state_dir/$name.ko"
                saved=$(sha "$state_dir/$name.ko") || fail "could not hash the backup of $dest"
                previous=present
            fi ;;
    esac
    eval "prev_state_$flat=\$previous"
    eval "prev_sha_value_$flat=\$saved"
done
if [ -f "$state/stock/$kernel/nvidia.ko" ]; then
    stock_recorded=$(sha "$state/stock/$kernel/nvidia.ko") ||
        fail "could not hash $state/stock/$kernel/nvidia.ko"
else
    stock_recorded=$stock_hash
fi
# The stamp of a staged module, from the manifest the staging library printed.
module_stamp_of() {
    [ -f "$work/manifest" ] || { printf '%s\n' "$host_version"; return 0; }
    awk -v want="$1" '
        $1 ~ /^module=/ || $1 ~ /^component=/ {
            n = $1; sub(/^[^=]*=/, "", n)
            if (n == want)
                for (i = 2; i <= NF; i++)
                    if ($i ~ /^stamp=/) { sub(/^stamp=/, "", $i); print $i }
        }' "$work/manifest"
}
# What the commit installs besides the modules is kept under $state, where a later run finds it:
# the patched plugin. And every destination is copied as it is now, into this commit's own
# directory, so that the commit can be undone exactly. On an upgrade these are the modules of the
# earlier build, not NVIDIA's files, which stay in the stock backups.
run cp -p "$work/patched-plugin" "$state/plugin-copy.gs-new"
txn=$state/txn
run rm -rf "$txn"
run mkdir -p "$txn"
plugin_previous=absent
plugin_previous_sha=none
if [ -f "$copy" ] && [ ! -L "$copy" ]; then
    run cp -p "$copy" "$txn/plugin-copy"
    plugin_previous=present
    plugin_previous_sha=$(sha "$txn/plugin-copy") || fail "could not hash the copy of $copy"
fi
for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
    eval "dest=\$dest_$flat"
    before=absent
    before_sha=none
    if [ -f "$dest" ] && [ ! -L "$dest" ]; then
        run cp -p "$dest" "$txn/$name.ko"
        before=present
        before_sha=$(sha "$txn/$name.ko") || fail "could not hash the copy of $dest"
    fi
    eval "before_state_$flat=\$before"
    eval "before_sha_$flat=\$before_sha"
done
link_previous=absent
[ ! -L "$link" ] || link_previous=$(readlink "$link")
config_action='write'
[ -z "$kept" ] || config_action=kept
config_previous=absent
[ ! -f "$config" ] || config_previous=present
record_tmp=$work/record
{
    printf 'kernel=%s\n' "$kernel"
    printf 'module_sha256=%s\n' "$module_hash"
    printf 'module_build_id=%s\n' "$build_id"
    printf 'stock_sha256=%s\n' "$stock_recorded"
    printf 'ada_placement=%s\n' "$ada_selected"
    [ -z "$client_accept" ] || printf 'client_accept=%s\n' "$client_accept"
    [ -z "$client_accept_unmatched" ] || printf 'client_accept_unmatched=%s\n' "$client_accept_unmatched"
    printf '%s\n' "$record_inputs"
    printf 'input_source_sha256=%s\ninput_second_sha256=%s\ninput_public_sha256=%s\n' \
        "$input_source_sha" "$input_second_sha" "$input_public_sha"
    printf 'plugin_sha256_input=%s\n' "$plugin_hash"
    printf 'plugin_sha256=%s\n' "$copy_digest"
    if [ -f "$work/preflight" ]; then
        for stage in scheduler module-gate vf-count ada-placement plugin-mixed plugin-div4 display-idx39; do
            [ "$stage" != display-idx39 ] || [ "$display" -eq 1 ] || continue
            case $stage in ada-placement) [ "$ada_selected" -eq 1 ] || continue ;; esac
            patch_field "$stage" admission anchor
            patch_field "$stage" state 'unconfirmed:unknown'
        done
    else
        for stage in scheduler module-gate vf-count ada-placement plugin-mixed plugin-div4; do
            case $stage in ada-placement) [ "$ada_selected" -eq 1 ] || continue ;; esac
            patch_field "$stage" admission digest
            # The states the patch tool reports for this chain. plugin-mixed alone is
            # unconfirmed: it describes the intermediate plugin, which was never tested by
            # itself and is never installed; plugin-div4 completes it.
            case $stage in
                plugin-mixed) patch_field "$stage" state 'unconfirmed:unknown' ;;
                *) patch_field "$stage" state confirmed ;;
            esac
        done
    fi
    [ -z "$idx39_function" ] || printf 'idx39_function=%s\nidx39_function_offset=%s\n' "$idx39_function" "$idx39_offset"
    new_sha=''
    previous=''
    saved=''
    dest=''
    stamp=''
    for name in $install_modules; do
    flat=$(printf '%s' "$name" | tr '-' '_')
        flat=$(printf '%s' "$name" | tr '-' '_')
        stamp=$(module_stamp_of "$name")
        [ -n "$stamp" ] || stamp=$host_version
        eval "new_sha=\$new_sha_$flat"
        eval "previous=\$prev_state_$flat"
        eval "saved=\$prev_sha_value_$flat"
        eval "dest=\$dest_$flat"
        gs_record_module "$name" "$stamp" "$new_sha"
        printf 'module_%s_path=%s\nmodule_%s_previous=%s\nmodule_%s_backup_sha256=%s\n' \
            "$flat" "$dest" "$flat" "$previous" "$flat" "$saved"
    done
    # The journal's own lines; they are not part of the published record.
    printf 'journal_build=%s\njournal_txn=%s\n' "$build" "$txn"
    for name in $install_modules; do
        flat=$(printf '%s' "$name" | tr '-' '_')
        eval "before=\$before_state_$flat"
        eval "before_sha=\$before_sha_$flat"
        printf 'journal_before_%s=%s\njournal_before_%s_sha256=%s\n' "$flat" "$before" "$flat" "$before_sha"
    done
    printf 'journal_plugin_copy=%s\njournal_plugin_link=%s\njournal_plugin_staged=%s\n' \
        "$copy" "$link" "$state/plugin-copy.gs-new"
    printf 'journal_plugin_previous=%s\njournal_plugin_previous_sha256=%s\njournal_plugin_link_previous=%s\n' \
        "$plugin_previous" "$plugin_previous_sha" "$link_previous"
    printf 'journal_config=%s\njournal_config_previous=%s\n' "$config_action" "$config_previous"
    # Where the record lands is decided here, with the kernel that is running now, so that finishing an
    # interrupted commit after a reboot does the same.
    land=published
    if [ "$kernel" != "$(uname -r)" ] && [ -f "$published" ] && [ ! -L "$published" ] &&
        [ "$(gs_record_get "$published" kernel)" = "$(uname -r)" ]; then
        land=kept
    fi
    printf 'journal_land=%s\n' "$land"
    printf 'journal_retire=%s\n' "$(printf '%s' "$retiring" | sed 's/^ *//')"
    for name in $retiring; do
        eval "path=\$prev_path_$name"
        eval "want=\$prev_sha_$name"
        eval "previous=\$prev_previous_$name"
        eval "saved=\$prev_backup_$name"
        printf 'journal_retire_%s_path=%s\njournal_retire_%s_sha256=%s\njournal_retire_%s_previous=%s\njournal_retire_%s_backup_sha256=%s\n' \
            "$name" "$path" "$name" "$want" "$name" "$previous" "$name" "$saved"
    done
} >"$record_tmp" || fail "could not compose $record"
gs_record_emit <"$record_tmp" >"$journal.gs-tmp" || { rm -f "$journal.gs-tmp"; fail "could not validate $record"; }
# Everything the journal names is on disk before the journal exists, and the journal is on disk
# before the first installed file changes.
run sync -f "$state"
mv -f "$journal.gs-tmp" "$journal" || fail "could not write $journal"
sync_dir "$state"
commit_files
# The commit is complete: nothing after it undoes it. The record of this kernel is the published one now.
committing=0
record=$landed
stage_cuda_built "$companion_source"
stage_desktop_built "$companion_source"
# The extraction of the Linux driver package is not needed any more; its room may be needed for what is kept.
rm -rf "$work/companion"
keep_inputs 1 || printf 'geforce-sriov-driver: warning: the build inputs could not be kept under %s\n' "$inputs" >&2
printf 'geforce-sriov-driver: patched plugin copy installed: %s (sha256 %s)\n' "$copy" "$(printf '%s\n' "$copy_digest" | cut -c 1-16)"
if [ -n "$kept" ]; then
    printf 'geforce-sriov-driver: kept %s: it already sets NVreg_EnableGpuFirmware=0\n' "$kept"
fi
if [ "$display" -eq 1 ] || [ "$host_cuda" -eq 1 ]; then
    printf 'geforce-sriov-driver: built and installed %s (display=%s host_cuda=%s, %s)\n' \
        "$(printf '%s' "$wanted" | tr ' ' ',')" "$display" "$host_cuda" "$confirmation"
fi
printf '%s\n' 'geforce-sriov-driver: NVIDIA'\''s module and plugin are prepared'
