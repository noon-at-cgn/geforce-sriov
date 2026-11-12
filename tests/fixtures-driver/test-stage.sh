#!/bin/sh
# Synthetic source trees exercise staging and exact patch anchors, not NVIDIA builds.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$here/../..
# shellcheck disable=SC1091
. "$root/scripts/gs-driver-stage.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
host=$tmp/host companion=$tmp/companion public=$tmp/public
mkdir -p "$host/common/inc" "$host/nvidia" "$companion/common/inc" \
    "$companion/nvidia-uvm" "$companion/nvidia-drm" "$companion/nvidia-modeset" \
    "$public/src/nvidia-modeset"
printf 'host kbuild\n' > "$host/Kbuild"
printf 'host conftest\n' > "$host/conftest.sh"
printf 'host sources\n' > "$host/nvidia/nvidia-sources.Kbuild"
printf 'prepatched object\n' > "$host/nvidia/nv-kernel.o_binary"
printf 'shared\n' > "$host/common/inc/shared.h"
printf 'shared\n' > "$companion/common/inc/shared.h"
printf 'new\n' > "$companion/common/inc/new.h"
printf 'uvm\n' > "$companion/nvidia-uvm/source"
printf 'drm kbuild\n' > "$companion/nvidia-drm/nvidia-drm.Kbuild"
printf 'wrapper\n' > "$companion/nvidia-modeset/wrapper"
printf 'old blob\n' > "$companion/nvidia-modeset/nv-modeset-kernel.o_binary"
printf 'NVIDIA_VERSION = 580.159.03\n' > "$public/version.mk"
cat > "$public/src/nvidia-modeset/Makefile" <<'EOF'
all:
	mkdir -p _out/Linux_$(shell uname -m)
	printf 'public core\n' > _out/Linux_$(shell uname -m)/nv-modeset-kernel.o
EOF

gs_stage_tree "$host" "$companion" "$tmp/cuda" 0 1 580.159.01 '' ''
cmp "$host/Kbuild" "$tmp/cuda/Kbuild"
cmp "$host/conftest.sh" "$tmp/cuda/conftest.sh"
cmp "$host/nvidia/nvidia-sources.Kbuild" "$tmp/cuda/nvidia/nvidia-sources.Kbuild"
cmp "$host/nvidia/nv-kernel.o_binary" "$tmp/cuda/nvidia/nv-kernel.o_binary"
cmp "$companion/common/inc/new.h" "$tmp/cuda/common/inc/new.h"
[ -f "$tmp/cuda/nvidia-uvm/source" ] && [ ! -e "$tmp/cuda/nvidia-drm" ]
gs_stage_tree "$host" "$companion" "$tmp/display" 1 1 580.159.01 580.159.03 "$public"
[ "$(cat "$tmp/display/nvidia-modeset/nv-modeset-kernel.o_binary")" = 'public core' ]
[ -f "$tmp/display/nvidia-uvm/source" ] && [ -f "$tmp/display/nvidia-drm/nvidia-drm.Kbuild" ]
grep -F 'NVIDIA_DRM_VERSION_STRING := 580.159.03' "$tmp/display/nvidia-drm/nvidia-drm.Kbuild" >/dev/null
gs_stage_manifest 1 1 580.159.01 580.159.03 > "$tmp/manifest"
grep -Fx 'NV_KERNEL_MODULES=nvidia nvidia-modeset nvidia-drm nvidia-uvm' "$tmp/manifest" >/dev/null
grep -Fx 'module=nvidia stamp=580.159.01 define=NV_VERSION_STRING' "$tmp/manifest" >/dev/null
if grep -q 'nvidia-vgpu-vfio' "$tmp/manifest"; then exit 1; fi
gs_stage_manifest 0 0 580.159.01 '' > "$tmp/plain"
grep -Fx 'NV_KERNEL_MODULES=nvidia' "$tmp/plain" >/dev/null
[ "$(grep -c '^module=' "$tmp/plain")" -eq 1 ]
grep -Fx 'module=nvidia-uvm stamp=580.159.01 define=NV_VERSION_STRING' "$tmp/manifest" >/dev/null
grep -Fx 'module=nvidia-modeset stamp=580.159.01 define=NV_VERSION_STRING' "$tmp/manifest" >/dev/null
grep -Fx 'module=nvidia-drm stamp=580.159.03 define=NV_VERSION_STRING' "$tmp/manifest" >/dev/null
grep -Fx 'component=nvkms-core stamp=580.159.03 define=NVIDIA_VERSION' "$tmp/manifest" >/dev/null
printf 'different\n' > "$companion/common/inc/shared.h"
rc=0; gs_stage_tree "$host" "$companion" "$tmp/bad" 0 1 580.159.01 '' '' 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/bad" ]
grep -F 'inc/shared.h' "$tmp/error" >/dev/null
printf 'shared\n' > "$companion/common/inc/shared.h"

# An existing destination, a component the host tree already has, and a
# tarball whose top directory carries version.mk are handled explicitly.
rc=0; gs_stage_tree "$host" "$companion" "$tmp/cuda" 0 1 580.159.01 '' '' 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && grep -F 'destination exists' "$tmp/error" >/dev/null
mkdir -p "$tmp/host2"; cp -a "$host/." "$tmp/host2/"; mkdir "$tmp/host2/nvidia-drm"
rc=0; gs_stage_tree "$tmp/host2" "$companion" "$tmp/conflict" 1 0 580.159.01 580.159.03 "$public" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/conflict" ] && grep -F 'nvidia-drm' "$tmp/error" >/dev/null
cp -a "$public" "$tmp/pubmode"; mkdir -p "$tmp/pubmode/src/nvidia-modeset/src"
printf '#define NVKMS_PB_MODE 2\n' > "$tmp/pubmode/src/nvidia-modeset/src/nvkms-rm.c"
rc=0; gs_stage_tree "$host" "$companion" "$tmp/variant" 1 0 580.159.01 580.159.03 "$tmp/pubmode" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/variant" ] && grep -F 'NVKMS_PB_MODE' "$tmp/error" >/dev/null
printf '#define NVKMS_PB_MODE 0\n' > "$tmp/pubmode/src/nvidia-modeset/src/nvkms-rm.c"
gs_stage_tree "$host" "$companion" "$tmp/mode0" 1 0 580.159.01 580.159.03 "$tmp/pubmode"
tar -C "$tmp" -cf "$tmp/public.tar" public
gs_stage_tree "$host" "$companion" "$tmp/tarred" 1 0 580.159.01 580.159.03 "$tmp/public.tar"
[ "$(cat "$tmp/tarred/nvidia-modeset/nv-modeset-kernel.o_binary")" = 'public core' ]
rc=0; gs_stage_tree "$host" "$companion" "$tmp/wrongver" 1 0 580.159.01 580.159.04 "$public" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/wrongver" ] && grep -F 'NVIDIA_VERSION' "$tmp/error" >/dev/null
printf 'NVIDIA_DRM_VERSION_STRING := 580.159.03\n' >> "$companion/nvidia-drm/nvidia-drm.Kbuild"
rc=0; gs_stage_tree "$host" "$companion" "$tmp/stamped" 1 0 580.159.01 580.159.03 "$public" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/stamped" ] && grep -F 'already carries a DRM version stamp' "$tmp/error" >/dev/null
python3 - "$companion/nvidia-drm/nvidia-drm.Kbuild" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().splitlines(keepends=True)
open(p, 'w').write(''.join(l for l in lines if not l.startswith('NVIDIA_DRM_VERSION_STRING')))
PY
python3 - "$tmp/public.tar" "$tmp/evil.tar" <<'PY'
import sys, tarfile
with tarfile.open(sys.argv[1]) as src, tarfile.open(sys.argv[2], 'w') as dst:
    for m in src.getmembers(): dst.addfile(m, src.extractfile(m) if m.isfile() else None)
    bad = tarfile.TarInfo('../escape'); bad.size = 1
    dst.addfile(bad, __import__('io').BytesIO(b'x'))
PY
rc=0; gs_stage_tree "$host" "$companion" "$tmp/escape" 1 0 580.159.01 580.159.03 "$tmp/evil.tar" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/escape" ] && grep -F 'unsafe archive member' "$tmp/error" >/dev/null

# Construct neutral fake input from each exact old hunk; no vendor files used.
python3 - "$root/driver-patches" "$tmp/patch-tree" <<'PY'
from pathlib import Path
import sys
patches, tree = map(Path, sys.argv[1:])
for patch in patches.glob('*.patch'):
    lines = patch.read_text().splitlines(keepends=True)
    current = None
    for line in lines:
        if line.startswith('--- a/'):
            current = tree / line[6:].strip()
            current.parent.mkdir(parents=True, exist_ok=True)
            current.write_text('')
        elif line.startswith('@@ '):
            with current.open('a') as out: out.write('unique hunk separator\n')
        elif current and line[:1] in (' ', '-') and not line.startswith('--- '):
            with current.open('a') as out: out.write(line[1:])
PY
cp -a "$tmp/patch-tree" "$tmp/applied"
gs_apply_source_patch "$tmp/applied" "$root/driver-patches/gpu-allow.patch"
gs_apply_source_patch "$tmp/applied" "$root/driver-patches/allocation-threshold.patch"
grep -F '#define KMALLOC_LIMIT 131072' "$tmp/applied/nvidia/os-interface.c" >/dev/null
grep -F 'gs_gpu_selected(gpu_info->gpuInfo.gpu_id)' "$tmp/applied/nvidia-drm/nvidia-drm-drv.c" >/dev/null
# A second application refuses: the anchors it needs are gone.
rc=0; gs_apply_source_patch "$tmp/applied" "$root/driver-patches/gpu-allow.patch" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && grep -F 'found 0' "$tmp/error" >/dev/null
# Unrelated leading lines shift every anchor and the patch still applies.
mkdir -p "$tmp/shifted/nvidia-drm"
{ printf 'junk one\njunk two\njunk three\n'; cat "$tmp/patch-tree/nvidia-drm/nvidia-drm-drv.c"; } > "$tmp/shifted/nvidia-drm/nvidia-drm-drv.c"
gs_apply_source_patch "$tmp/shifted" "$root/driver-patches/gpu-allow.patch"
grep -F 'gs_gpu_selected(gpu_info->gpuInfo.gpu_id)' "$tmp/shifted/nvidia-drm/nvidia-drm-drv.c" >/dev/null
mkdir "$tmp/ambig"
printf 'needle\nneedle\n' > "$tmp/ambig/a"
cat > "$tmp/ambig.patch" <<'EOF'
--- a/a
+++ b/a
@@ -1 +1 @@
-needle
+replacement
EOF
rc=0; gs_apply_source_patch "$tmp/ambig" "$tmp/ambig.patch" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ "$(cat "$tmp/ambig/a")" = "$(printf 'needle\nneedle')" ]
grep -F 'a: hunk' "$tmp/error" >/dev/null
mkdir "$tmp/missing"
printf 'other\n' > "$tmp/missing/a"
rc=0; gs_apply_source_patch "$tmp/missing" "$tmp/ambig.patch" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ "$(cat "$tmp/missing/a")" = 'other' ]
grep -F 'found 0' "$tmp/error" >/dev/null

# A symbolic link that leads out of a tree leaves the bytes behind it with someone else: a host
# tree, a component and a public source with one are not staged. A link that stays inside is.
printf 'int outside;\n' > "$tmp/outside.c"
cp -a "$host" "$tmp/host-link"
ln -s Kbuild "$tmp/host-link/inside"
gs_stage_tree "$tmp/host-link" "$companion" "$tmp/inside-ok" 0 1 580.159.01 '' ''
[ "$(readlink "$tmp/inside-ok/inside")" = Kbuild ]
ln -s "$tmp/outside.c" "$tmp/host-link/nvidia/outside.c"
rc=0; gs_stage_tree "$tmp/host-link" "$companion" "$tmp/host-out" 0 1 580.159.01 '' '' 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/host-out" ]
grep -F "$tmp/host-link/nvidia/outside.c is a symbolic link to $tmp/outside.c, which is outside $tmp/host-link" "$tmp/error" >/dev/null
cp -a "$companion" "$tmp/companion-link"
ln -s ../../outside.c "$tmp/companion-link/nvidia-uvm/outside.c"
rc=0; gs_stage_tree "$host" "$tmp/companion-link" "$tmp/component-out" 0 1 580.159.01 '' '' 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/component-out" ]
grep -F 'the staged tree/nvidia-uvm/outside.c is a symbolic link to ../../outside.c' "$tmp/error" >/dev/null
cp -a "$public" "$tmp/public-link"
ln -s "$tmp/outside.c" "$tmp/public-link/src/nvidia-modeset/outside.c"
rc=0; gs_stage_tree "$host" "$companion" "$tmp/public-out" 1 0 580.159.01 580.159.03 "$tmp/public-link" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/public-out" ]
grep -F 'the public source/src/nvidia-modeset/outside.c is a symbolic link' "$tmp/error" >/dev/null
# The copy for a build is the caller's own: nobody else can write to it, whatever the source allowed.
chmod -R go+w "$tmp/host-link"
rm "$tmp/host-link/nvidia/outside.c"
gs_copy_tree "$tmp/host-link" "$tmp/own-copy"
[ -z "$(find "$tmp/own-copy" -perm /022 ! -type l)" ]
rc=0; gs_copy_tree "$tmp/host-link" "$tmp/own-copy" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && grep -F 'destination exists' "$tmp/error" >/dev/null
# No set-user-ID or set-group-ID bit survives a copy, and a tree that carries one is not accepted.
chmod u+s "$tmp/host-link/Kbuild"
gs_copy_tree "$tmp/host-link" "$tmp/plain-copy"
[ -z "$(find "$tmp/plain-copy" -perm /6000)" ]
gs_tree_closed "$tmp/plain-copy"
rc=0; gs_tree_closed "$tmp/host-link" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && grep -F 'Kbuild has the set-user-ID or set-group-ID bit' "$tmp/error" >/dev/null
chmod u-s "$tmp/host-link/Kbuild"
# The digest of a tree is that of its copy, and changes with any file.
[ "$(gs_tree_digest "$tmp/host-link")" = "$(gs_tree_digest "$tmp/plain-copy")" ]
printf 'more\n' >>"$tmp/plain-copy/Kbuild"
[ "$(gs_tree_digest "$tmp/host-link")" != "$(gs_tree_digest "$tmp/plain-copy")" ]
# Anything but a file, a directory or a link refuses the copy, and the copy is removed.
mkfifo "$tmp/host-link/pipe"
rc=0; gs_copy_tree "$tmp/host-link" "$tmp/fifo-copy" 2>"$tmp/error" || rc=$?
[ "$rc" -eq 3 ] && [ ! -e "$tmp/fifo-copy" ]
grep -F "$tmp/host-link/pipe is not a regular file, a directory or a symbolic link" "$tmp/error" >/dev/null
printf '%s\n' 'synthetic staging fixtures passed'
