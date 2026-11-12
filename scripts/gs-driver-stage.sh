#!/bin/sh
# SPDX-License-Identifier: MIT
# Sourced library; no commands run on import. All functions return 0 on success,
# 3 on refusal or staging failure. The caller owns the private destination parent.
#
# gs_stage_tree HOST_KERNEL COMPANION_KERNEL DEST DISPLAY CUDA H P NVKMS_SOURCE
# Copy HOST_KERNEL to a new DEST, import requested companion components and
# unique common files, and, for DISPLAY=1, build pristine NVKMS from the supplied
# public source directory or tarball. H and P are already authenticated versions.
# DISPLAY and CUDA are 0 or 1; neither set is the plain build and refuses here.
# A component the host tree already contains, a shared common file that differs,
# an existing DEST and an unsafe archive all refuse before DEST is created.
# idx 39 must already have been applied to the host object by the caller.
#
# gs_stage_manifest DISPLAY CUDA H P prints the staged module list, then one
# stamp and define record per module; nvkms-core is an object inside
# nvidia-modeset, not an additional module. nvidia-vgpu-vfio is never built:
# NVIDIA's installed copy stays, since it imports no symbol from nvidia.ko.
#
# gs_apply_source_patch TREE PATCH applies every exact, unique unified-diff hunk
# to regular files under TREE. A hunk that matches zero or several places refuses
# and names the file and hunk; no fuzzy offsets and no partial change remain.
# Requires python3. Changes are made only inside TREE.
#
# gs_copy_tree SRC DEST copies the directory SRC to a new DEST for a build that root runs: the copy
# belongs to the user of this run and nobody else can write to it. It holds regular files,
# directories and symbolic links that stay inside it, and nothing else: no link that leads out of
# it, no FIFO, socket or device, and no file with the set-user-ID or set-group-ID bit, which a copy
# made by root would turn into a root-owned one. So what is built is what was copied and nothing
# else. SRC may itself be a link to a directory. A copy that is refused is removed.
# gs_tree_closed DIR [NAME] is the check alone, for a tree that was copied earlier; NAME is how
# messages call DIR. gs_tree_digest DIR prints one SHA-256 over the names, modes, link targets and
# contents of DIR, the same for a copy made by gs_copy_tree. None of them needs python3.

# The first symbolic link below DIR whose target is outside DIR; nothing when there is none.
gs_tree_link_out() {
    _gs_top=$(readlink -f -- "$1") || { printf '%s\n' "$1"; return 0; }
    find "$1" -type l -exec sh -c '
        top=$1
        shift
        for link do
            target=$(readlink -m -- "$link" 2>/dev/null) || target=
            case $target in
                "$top"/*) ;;
                *) printf "%s\n" "$link"; exit 0 ;;
            esac
        done' sh "$_gs_top" {} + 2>/dev/null | sed -n '1p'
}
gs_tree_closed() {
    _gs_odd=$(find "$1" ! -type f ! -type d ! -type l -print 2>/dev/null | sed -n '1p')
    if [ -n "$_gs_odd" ]; then
        printf 'stage: %s is not a regular file, a directory or a symbolic link. A tree that is built as root holds nothing else: remove it\n' \
            "${2:-$1}${_gs_odd#"$1"}" >&2
        return 3
    fi
    _gs_odd=$(find "$1" ! -type l -perm /6000 -print 2>/dev/null | sed -n '1p')
    if [ -n "$_gs_odd" ]; then
        printf 'stage: %s has the set-user-ID or set-group-ID bit. A tree that is built as root holds no such file\n' \
            "${2:-$1}${_gs_odd#"$1"}" >&2
        return 3
    fi
    _gs_link=$(gs_tree_link_out "$1")
    [ -n "$_gs_link" ] || return 0
    printf 'stage: %s is a symbolic link to %s, which is outside %s. A tree that is built as root must hold its own files: replace the link with the file it names\n' \
        "${2:-$1}${_gs_link#"$1"}" "$(readlink -- "$_gs_link" 2>/dev/null)" "${2:-$1}" >&2
    return 3
}
gs_copy_tree() {
    [ -d "$1" ] || { printf 'stage: not a directory: %s\n' "$1" >&2; return 3; }
    [ ! -e "$2" ] && [ ! -L "$2" ] || { printf 'stage: destination exists: %s\n' "$2" >&2; return 3; }
    # The special bits go first: a file that root copies is root's.
    mkdir -- "$2" && cp -a --no-preserve=ownership -- "$1/." "$2/" && chmod -R ug-s,go-w -- "$2" ||
        { rm -rf -- "$2"; printf 'stage: cannot copy %s\n' "$1" >&2; return 3; }
    gs_tree_closed "$2" "$1" || { rm -rf -- "$2"; return 3; }
}
gs_tree_digest() {
    [ -d "$1" ] || return 3
    _gs_digest=$( (cd -- "$1" && tar --format=gnu --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=ug-s,go-w -cf - .) | sha256sum) || return 3
    printf '%s\n' "${_gs_digest%% *}"
}

gs_stage_manifest() (
    [ "$#" -eq 4 ] || return 3
    case $1:$2 in 0:0|0:1|1:0|1:1) ;; *) return 3 ;; esac
    [ -n "$3" ] && { [ "$1" = 0 ] || [ -n "$4" ]; } || return 3
    modules='nvidia'
    [ "$1" = 0 ] || modules="$modules nvidia-modeset nvidia-drm"
    [ "$2" = 0 ] || modules="$modules nvidia-uvm"
    printf 'NV_KERNEL_MODULES=%s\n' "$modules"
    printf 'module=nvidia stamp=%s define=NV_VERSION_STRING\n' "$3"
    if [ "$1" = 1 ]; then
        printf 'module=nvidia-modeset stamp=%s define=NV_VERSION_STRING\n' "$3"
        printf 'component=nvkms-core stamp=%s define=NVIDIA_VERSION\n' "$4"
        printf 'module=nvidia-drm stamp=%s define=NV_VERSION_STRING\n' "$4"
    fi
    if [ "$2" = 1 ]; then
        printf 'module=nvidia-uvm stamp=%s define=NV_VERSION_STRING\n' "$3"
    fi
)

gs_apply_source_patch() (
    [ "$#" -eq 2 ] || { printf '%s\n' 'source patch: expected TREE PATCH' >&2; return 3; }
    python3 - "$1" "$2" <<'PY'
import os, re, stat, sys, tempfile
from pathlib import Path

root, patch = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
def refuse(msg):
    print('source patch: ' + msg, file=sys.stderr)
    sys.exit(3)
try:
    lines = patch.read_bytes().splitlines(keepends=True)
    edits = {}
    i = 0
    while i < len(lines) and not lines[i].startswith(b'--- '):
        i += 1
    while i < len(lines):
        if not lines[i].startswith(b'--- a/') or i + 1 >= len(lines) or not lines[i + 1].startswith(b'+++ b/'):
            refuse('invalid file header')
        name = os.fsdecode(lines[i][6:].strip())
        if lines[i + 1][6:].strip() != os.fsencode(name) or not name or any(p in ('', '.', '..') for p in name.split('/')):
            refuse('invalid target ' + name)
        target = root / name
        if target in edits or any(root.joinpath(*name.split('/')[:n]).is_symlink() for n in range(1, len(name.split('/')) + 1)):
            refuse(name + ': repeated or symlink target')
        if not target.is_file():
            refuse(name + ': missing or not a regular file')
        old = target.read_bytes().splitlines(keepends=True)
        mode = stat.S_IMODE(target.stat().st_mode)
        hunks = []
        i += 2
        while i < len(lines) and lines[i].startswith(b'@@ '):
            h = lines[i].decode('ascii', 'replace').strip()
            if not re.match(r'^@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@', h):
                refuse(name + ': invalid hunk ' + h)
            i += 1
            before, after = [], []
            while i < len(lines) and lines[i][:1] in (b' ', b'+', b'-') and not lines[i].startswith((b'--- ', b'+++ ')):
                tag, value = lines[i][:1], lines[i][1:]
                if tag != b'+': before.append(value)
                if tag != b'-': after.append(value)
                i += 1
            if not before:
                refuse(name + ': empty anchor ' + h)
            locations = [j for j in range(len(old) - len(before) + 1) if old[j:j + len(before)] == before]
            if len(locations) != 1:
                refuse(name + ': hunk ' + h + ': expected one exact match, found ' + str(len(locations)))
            pos = locations[0]
            hunks.append((pos, pos + len(before), after, h))
        if not hunks:
            refuse(name + ': no hunks')
        hunks.sort()
        if any(hunks[j][1] > hunks[j + 1][0] for j in range(len(hunks) - 1)):
            refuse(name + ': overlapping hunks')
        for start, end, replacement, _ in reversed(hunks):
            old[start:end] = replacement
        edits[target] = (b''.join(old), mode)
    if not edits:
        refuse('no file headers')
    for target, (data, mode) in edits.items():
        fd, tmp = tempfile.mkstemp(prefix='.gs-patch-', dir=target.parent)
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(data)
                os.fchmod(f.fileno(), mode)
            os.replace(tmp, target)
        finally:
            if os.path.exists(tmp): os.unlink(tmp)
except (OSError, ValueError) as e:
    refuse(str(e))
PY
)

gs_stage_tree() (
    [ "$#" -eq 8 ] || { printf '%s\n' 'stage: expected HOST COMPANION DEST DISPLAY CUDA H P NVKMS_SOURCE' >&2; return 3; }
    host=$1 companion=$2 dest=$3 display=$4 cuda=$5 host_version=$6 public_version=$7 nvkms=$8
    case $display:$cuda in 0:1|1:0|1:1) ;; *) printf '%s\n' 'stage: select display or host CUDA' >&2; return 3 ;; esac
    case $host_version:$public_version in *[!0-9.:]*) printf '%s\n' 'stage: invalid version' >&2; return 3 ;; esac
    [ -n "$host_version" ] && { [ "$display" = 0 ] || [ -n "$public_version" ]; } || return 3
    [ -d "$host/common" ] && [ -d "$companion/common" ] && [ -f "$host/Kbuild" ] &&
        [ -f "$host/conftest.sh" ] && [ -f "$host/nvidia/nvidia-sources.Kbuild" ] &&
        [ -f "$host/nvidia/nv-kernel.o_binary" ] || { printf '%s\n' 'stage: missing host or companion input' >&2; return 3; }
    [ ! -e "$dest" ] && [ ! -L "$dest" ] || { printf 'stage: destination exists: %s\n' "$dest" >&2; return 3; }
    parent=${dest%/*}; [ "$parent" != "$dest" ] || parent=.
    [ -d "$parent" ] || { printf 'stage: destination parent missing: %s\n' "$parent" >&2; return 3; }
    work=$(mktemp -d "$parent/.gs-stage.XXXXXXXX") || return 3
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    gs_copy_tree "$host" "$work/tree" || return 3
    python3 - "$host/common" "$companion/common" "$work/tree/common" <<'PY' || return 3
import os, shutil, sys
from pathlib import Path
host, extra, dst = map(Path, sys.argv[1:])
try:
    for base, dirs, files in os.walk(extra, followlinks=False):
        for name in dirs + files:
            src = Path(base) / name
            rel = src.relative_to(extra)
            old = host / rel
            out = dst / rel
            if src.is_symlink() or old.is_symlink():
                raise ValueError(str(rel) + ': symlink in common tree')
            if src.is_dir():
                if old.exists() and not old.is_dir(): raise ValueError(str(rel) + ': type differs')
                out.mkdir(parents=True, exist_ok=True)
            elif src.is_file():
                if old.exists():
                    if not old.is_file() or src.read_bytes() != old.read_bytes():
                        raise ValueError(str(rel) + ': shared common file differs')
                else:
                    out.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src, out)
            else:
                raise ValueError(str(rel) + ': unsupported common file type')
except (OSError, ValueError) as e:
    print('stage: common/' + str(e), file=sys.stderr)
    sys.exit(3)
PY
    for component in $(if [ "$display" = 1 ]; then printf '%s ' nvidia-modeset nvidia-drm; fi) $(if [ "$cuda" = 1 ]; then printf '%s' nvidia-uvm; fi); do
        [ -d "$companion/$component" ] && [ ! -e "$work/tree/$component" ] && [ ! -L "$work/tree/$component" ] || {
            printf 'stage: missing or conflicting component: %s\n' "$component" >&2; return 3;
        }
        cp -a --no-preserve=ownership "$companion/$component" "$work/tree/$component" || return 3
    done
    # The components came from another tree: the staged tree is still root's own and closed.
    chmod -R ug-s,go-w "$work/tree" || return 3
    gs_tree_closed "$work/tree" "the staged tree" || return 3
    if [ "$display" = 1 ]; then
        [ -f "$work/tree/nvidia-drm/nvidia-drm.Kbuild" ] || { printf '%s\n' 'stage: missing nvidia-drm.Kbuild' >&2; return 3; }
        if grep -q '^NVIDIA_DRM_VERSION_STRING[[:space:]]*:=' "$work/tree/nvidia-drm/nvidia-drm.Kbuild"; then
            printf '%s\n' 'stage: companion nvidia-drm.Kbuild already carries a DRM version stamp' >&2
            return 3
        fi
        # The source is always copied or extracted into staging before make.
        mkdir "$work/public" || return 3
        if [ -d "$nvkms" ]; then
            cp -a --no-preserve=ownership "$nvkms/." "$work/public/" || return 3
        elif [ -f "$nvkms" ]; then
            python3 - "$nvkms" "$work/public" <<'PY' || return 3
import inspect, pathlib, sys, tarfile
try:
    with tarfile.open(sys.argv[1], 'r:*') as tf:
        for m in tf.getmembers():
            p = pathlib.PurePosixPath(m.name)
            if p.is_absolute() or '..' in p.parts or not (m.isfile() or m.isdir()):
                raise ValueError('unsafe archive member: ' + m.name)
        if 'filter' in inspect.signature(tf.extractall).parameters:
            tf.extractall(sys.argv[2], filter='data')
        else:
            tf.extractall(sys.argv[2])
except (OSError, ValueError, tarfile.TarError) as e:
    print('stage: NVKMS archive: ' + str(e), file=sys.stderr)
    sys.exit(3)
PY
        else
            printf 'stage: NVKMS source missing: %s\n' "$nvkms" >&2; return 3
        fi
        chmod -R ug-s,go-w "$work/public" || return 3
        gs_tree_closed "$work/public" "the public source" || return 3
        public=$work/public
        if [ ! -f "$public/version.mk" ]; then
            found=0
            for candidate in "$public"/*/version.mk; do
                [ -f "$candidate" ] || continue
                found=$((found + 1))
                public=${candidate%/version.mk}
            done
            [ "$found" -eq 1 ] || { printf '%s\n' 'stage: cannot locate unique public version.mk' >&2; return 3; }
        fi
        python3 - "$public/version.mk" "$public_version" <<'PY' || return 3
import re, sys
try:
    s = open(sys.argv[1], encoding='utf-8').read()
    v = re.findall(r'^NVIDIA_VERSION\s*[:?]?=\s*([^\s#]+)', s, re.M)
    if v != [sys.argv[2]]: raise ValueError('NVIDIA_VERSION does not uniquely match ' + sys.argv[2])
except (OSError, ValueError) as e:
    print('stage: ' + str(e), file=sys.stderr)
    sys.exit(3)
PY
        [ -f "$public/src/nvidia-modeset/Makefile" ] || { printf '%s\n' 'stage: missing NVKMS Makefile' >&2; return 3; }
        # Mode 0 is the stock sysmem push buffer: refuse a source tree carrying a
        # nonzero experimental push-buffer placement switch.
        python3 - "$public/src" <<'PY' || return 3
import re, sys
from pathlib import Path
try:
    for p in Path(sys.argv[1]).rglob('*'):
        if p.suffix not in ('.c', '.h') or not p.is_file():
            continue
        for value in re.findall(r'^#define\s+NVKMS_PB_MODE\s+(\S+)', p.read_text(errors='replace'), re.M):
            if value != '0':
                raise ValueError(str(p) + ': experimental NVKMS_PB_MODE ' + value)
except (OSError, ValueError) as e:
    print('stage: ' + str(e), file=sys.stderr)
    sys.exit(3)
PY
        rm -rf "$public/src/nvidia-modeset/_out" || return 3
        make -C "$public/src/nvidia-modeset" || { printf '%s\n' 'stage: NVKMS build failed' >&2; return 3; }
        arch=$(uname -m) || return 3
        blob=$public/src/nvidia-modeset/_out/Linux_$arch/nv-modeset-kernel.o
        [ -f "$blob" ] || { printf 'stage: NVKMS object missing: %s\n' "$blob" >&2; return 3; }
        rm -f "$work/tree/nvidia-modeset/nv-modeset-kernel.o_binary" || return 3
        cp "$blob" "$work/tree/nvidia-modeset/nv-modeset-kernel.o_binary" || return 3
        cat >> "$work/tree/nvidia-drm/nvidia-drm.Kbuild" <<EOF || return 3

# DRM talks to the public NVKMS core, not the host RM version boundary.
NVIDIA_DRM_VERSION_STRING := $public_version
\$(foreach _o, \$(NVIDIA_DRM_OBJECTS), \\
 \$(eval CFLAGS_REMOVE_\$(_o) += \$\$(filter -DNV_VERSION_STRING=%,\$\$(ccflags-y))))
\$(call ASSIGN_PER_OBJ_CFLAGS, \$(NVIDIA_DRM_OBJECTS), \\
 -DNV_VERSION_STRING=\"\$(NVIDIA_DRM_VERSION_STRING)\")
EOF
    fi
    mv "$work/tree" "$dest" || return 3
)
