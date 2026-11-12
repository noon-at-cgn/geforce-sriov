#!/bin/sh
# SPDX-License-Identifier: MIT
# The owner of the Ada VFs' framebuffer slots, as far as it can be checked without a kernel:
#   - fb-owner.patch only adds lines, applies exactly once to a file that has its anchors, and
#     applies exactly once to nothing else;
#   - the three files are the ones the table of confirmed versions records;
#   - gs-fb-owner.c calls nothing of the kernel that is not on the list below (plain exports only:
#     a module with NVIDIA's licence cannot use a GPL-only one), and every kernel symbol the table
#     allows the module to import newly is on that list;
#   - gs_elf_closed_same tells a module whose closed functions are unchanged from one where a
#     function differs, on real ELF objects.
# What the code does is tests/fbowner/test_fbowner.c; what it does on a GPU is seen on hardware.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$here/../..
dir=$root/driver-patches/fb-owner
# shellcheck source=../../scripts/gs-driver-stage.sh
. "$root/scripts/gs-driver-stage.sh"
# shellcheck source=../../scripts/gs-driver-elf.sh
. "$root/scripts/gs-driver-elf.sh"
# shellcheck source=../../scripts/gs-driver-version.sh
. "$root/scripts/gs-driver-version.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gs-fb-owner.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
die() { printf 'test-fb-owner: %s\n' "$*" >&2; exit 1; }

for f in gs-fb-owner.c gs-fb-owner.h fb-owner.patch; do
    [ -f "$dir/$f" ] || die "missing: $dir/$f"
done

# ---- the patch ---------------------------------------------------------------------------------
if sed -n '/^--- a\//,$p' "$dir/fb-owner.patch" | grep -v '^--- ' | grep -q '^-'; then
    die 'fb-owner.patch removes a line; it may only add calls'
fi
added=$(sed -n '/^--- a\//,$p' "$dir/fb-owner.patch" | sed -n 's/^+//p' | grep -v '^++ ' | grep -v '^[[:space:]]*$' |
    grep -v 'gs_fb_\|gs-fb-owner\|^ *[{}]$\|^ *status = 0;$\|^ *break;$\|^ *\/\* geforce-sriov: ' || :)
[ -z "$added" ] || die "fb-owner.patch adds a line that is not a call into the owner: $added"
python3 - "$dir/fb-owner.patch" "$tmp/tree" <<'PY'
from pathlib import Path
import sys
patch, tree = Path(sys.argv[1]), Path(sys.argv[2])
current = None
for line in patch.read_text().splitlines(keepends=True):
    if line.startswith('--- a/'):
        current = tree / line[6:].strip()
        current.parent.mkdir(parents=True, exist_ok=True)
        current.write_text('')
    elif line.startswith('@@ '):
        with current.open('a') as out: out.write('unique hunk separator\n')
    elif current and line[:1] in (' ', '-') and not line.startswith('--- '):
        with current.open('a') as out: out.write(line[1:])
PY
gs_apply_source_patch "$tmp/tree" "$dir/fb-owner.patch"
for call in 'gs_fb_init();' 'gs_fb_exit();' 'if (gs_fb_ioctl_pre(nvlfp, arg_cmd, arg_copy, arg_size, &gs_fb_seen))' \
    'gs_fb_ioctl_post(nvlfp, arg_cmd, arg_copy, arg_size, rmStatus, gs_fb_seen);' 'gs_fb_file_closed(nvlfp);' 'NvU32 gs_fb_seen = 0;'; do
    grep -qF -- "$call" "$tmp/tree/nvidia/nv.c" || die "after the patch nv.c lacks: $call"
done
[ "$(grep -cF 'gs_fb_gpu_remove(nv->gpu_id);' "$tmp/tree/nvidia/nv-pci.c")" -eq 2 ] ||
    die 'after the patch nv-pci.c lacks a removal call: one before the removal, one in its wait for the users to go'
grep -qxF 'NVIDIA_SOURCES += nvidia/gs-fb-owner.c' "$tmp/tree/nvidia/nvidia-sources.Kbuild" || die 'the source list lacks the owner'
# The manager's request is looked at before the RM sees it, and the record is kept after.
awk '/gs_fb_ioctl_pre\(/ { pre = NR } /rmStatus = rm_ioctl\(/ { rm = NR } /gs_fb_ioctl_post\(/ { post = NR }
     END { exit !(pre && rm && post && pre < rm && rm < post) }' "$tmp/tree/nvidia/nv.c" || die 'the calls around rm_ioctl() are out of order'
rc=0; gs_apply_source_patch "$tmp/tree" "$dir/fb-owner.patch" 2>/dev/null || rc=$?
[ "$rc" -eq 3 ] || die 'a second application of the patch was not refused'

# ---- the files are the recorded ones -----------------------------------------------------------
gs_versions_load "$root/scripts/gs-driver-versions.tbl"
recorded=0
for version in $gs_confirmed_hosts; do
    gs_version_row "$version"
    [ -n "$gs_owner_c" ] || continue
    recorded=1
    [ "$(sha256sum "$dir/gs-fb-owner.c" | cut -d' ' -f1)" = "$gs_owner_c" ] || die "owner_c of $version is not the digest of gs-fb-owner.c"
    [ "$(sha256sum "$dir/gs-fb-owner.h" | cut -d' ' -f1)" = "$gs_owner_h" ] || die "owner_h of $version is not the digest of gs-fb-owner.h"
    [ "$(sha256sum "$dir/fb-owner.patch" | cut -d' ' -f1)" = "$gs_owner_patch" ] || die "owner_patch of $version is not the digest of fb-owner.patch"
    symbols=$gs_owner_symbols
done
[ "$recorded" -eq 1 ] || die 'no block of the table records the owner'

# ---- what the owner calls ----------------------------------------------------------------------
# Comments name other functions, so they are taken out first; macros are left as they are written.
plain='capable copy_from_user copy_to_user mutex_lock mutex_unlock sscanf snprintf printk strcmp memset memcpy
    proc_create remove_proc_entry seq_printf single_open'
open_layer='nv_kmem_cache_alloc_stack nv_kmem_cache_free_stack rm_kernel_rmapi_op nvidia_dev_get nvidia_dev_put'
calls=$("${CC:-cc}" -fpreprocessed -dD -E -P "$dir/gs-fb-owner.c" | grep -o '\b[A-Za-z_][A-Za-z0-9_]*(' | tr -d '(' | sort -u)
for call in $calls; do
    case $call in
        gs_fb_*|if|for|sizeof|defined|__attribute__|used|DEFINE_MUTEX|NV_STATE_PTR|LOCK_NV_LINUX_DEVICES|UNLOCK_NV_LINUX_DEVICES) continue ;;
        # Inline in the kernel's headers: they read a field and call nothing.
        task_pid|time_after|READ_ONCE|WRITE_ONCE) continue ;;
    esac
    case " $(printf '%s' "$plain $open_layer" | tr '\n' ' ') " in
        *" $call "*) ;;
        *) die "gs-fb-owner.c calls $call, which is not on the list of what it may call" ;;
    esac
done
# What the table lets the module import newly: nothing but what the list above stands for.
for symbol in $symbols; do
    case $symbol in
        capable|_copy_from_user|_copy_to_user|__check_object_size|mutex_lock|mutex_unlock|sscanf|snprintf|_printk|strcmp|memset|memcpy|jiffies) ;;
        proc_create|remove_proc_entry|seq_printf|seq_read|seq_lseek|single_open|single_release) ;;
        *) die "owner_symbols allows $symbol, which the owner has no call for" ;;
    esac
done

# ---- NVIDIA's closed functions, before and after -----------------------------------------------
if command -v "${CC:-cc}" >/dev/null 2>&1 && command -v ld >/dev/null 2>&1; then
    cat >"$tmp/closed.c" <<'EOF'
int closed_one(int x) { return x * 3 + 1; }
int closed_two(int x) { return closed_one(x) - 7; }
EOF
    sed 's/x \* 3/x * 5/' "$tmp/closed.c" >"$tmp/closed-changed.c"
    printf 'int open_layer(int x) { return x + 1; }\n' >"$tmp/open-a.c"
    printf 'int owner(int x) { return x ^ 85; }\nint open_layer(int x) { return owner(x) + 1; }\n' >"$tmp/open-b.c"
    for name in closed closed-changed open-a open-b; do
        "${CC:-cc}" -O1 -c -o "$tmp/$name.o" "$tmp/$name.c"
    done
    ld -r -o "$tmp/before.ko" "$tmp/open-a.o" "$tmp/closed.o"
    ld -r -o "$tmp/after.ko" "$tmp/open-b.o" "$tmp/closed.o"
    ld -r -o "$tmp/after-changed.ko" "$tmp/open-b.o" "$tmp/closed-changed.o"
    ld -r -o "$tmp/after-gone.ko" "$tmp/open-b.o"
    out=$(gs_elf_closed_same "$tmp/closed.o" "$tmp/before.ko" "$tmp/after.ko") || die "unchanged closed functions were refused: $out"
    case $out in *'the 2 functions of the closed object'*) ;; *) die "unexpected result: $out" ;; esac
    rc=0; out=$(gs_elf_closed_same "$tmp/closed.o" "$tmp/before.ko" "$tmp/after-changed.ko") || rc=$?
    [ "$rc" -eq 3 ] || die 'a changed closed function was not refused'
    case $out in *'function closed_one of the closed object differs'*) ;; *) die "unexpected refusal: $out" ;; esac
    rc=0; out=$(gs_elf_closed_same "$tmp/closed.o" "$tmp/before.ko" "$tmp/after-gone.ko") || rc=$?
    [ "$rc" -eq 3 ] || die 'a closed function that is gone was not refused'
    rc=0; out=$(gs_elf_closed_same "$tmp/closed.c" "$tmp/before.ko" "$tmp/after.ko") || rc=$?
    [ "$rc" -eq 3 ] || die 'a file that is no ELF object was not refused'
else
    printf '%s\n' 'test-fb-owner: no compiler or linker: the comparison of closed functions was not run'
fi
printf '%s\n' 'framebuffer owner fixtures passed'
