#!/bin/sh
# SPDX-License-Identifier: MIT
# Offline tests for scripts/geforce-sriov-driver.sh. Every host path is under a
# temporary directory, and the tools that would act on the host are replaced by
# stubs. Two fixture kinds are used:
#
#   marker  text marker files stand in for the vendor object and the modules,
#           with a sha256sum stub that answers the recorded digests. It exercises
#           the plain route, the version policy and the transaction, not ELF.
#   elf     real ELF objects and modules built with the host compiler. It
#           exercises the source staging, the ABI gate, symbol correspondence
#           and the finished-module idx 39 check.
#
# Nothing here proves a driver works on hardware; no module is loaded and no
# device is touched.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
driver=$here/../../scripts/geforce-sriov-driver.sh
kernel=6.1.0-gs-fixture
kdir=$kernel
passed=0
failed=0

say() { printf '%s\n' "$*"; }
check() {
	description=$1
	shift
	if "$@"; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		printf 'FAIL: %s\n' "$description" >&2
		[ -z "${out:-}" ] || printf '%s\n' "$out" >&2
	fi
}
contains() { case $1 in *"$2"*) return 0 ;; *) return 1 ;; esac; }
not_contains() { case $1 in *"$2"*) return 1 ;; *) return 0 ;; esac; }
exists() { [ -e "$1" ] || [ -L "$1" ]; }

# --- fixture construction ----------------------------------------------------

fx_stub() {
	path=$1
	shift
	{
		printf '#!/bin/sh\n'
		printf '%s\n' "$*"
	} >"$path"
	chmod 0755 "$path"
}

# A .text image of the size the plain gate needs, with its eight bytes set when patched.
fx_plain_text() {
	path=$1
	patched=$2
	truncate -s 0xd89671 "$path" 2>/dev/null || {
		# truncate(1) is not POSIX; fall back to dd.
		dd if=/dev/zero of="$path" bs=1 count=0 seek=$((0xd89671)) status=none
	}
	[ "$patched" -eq 0 ] && return 0
	for at in 0x14a7c4 0x14a7c5 0x14a7c6 0x14a7c7 0xa16708 0xa16709 0xd8966f 0xd89670; do
		printf '\220' | dd of="$path" bs=1 seek=$((at)) conv=notrunc status=none
	done
}

fx_init() {
	FX=$1
	MODE=$2
	rm -rf "$FX"
	mkdir -p "$FX/bin" "$FX/tmp" "$FX/root" "$FX/companion/kernel/common/inc" \
		"$FX/src/common/inc" "$FX/src/nvidia" "$FX/modules" "$FX/objects" "$FX/patchdir"
	REAL_SHA=$(command -v sha256sum)
	REAL_PATCH=$here/../../driver/geforce-sriov-patch
	REAL_MODINFO=$(command -v modinfo || true)
	export FXDIR="$FX" FXMODE="$MODE"
	export TMPDIR="$FX/tmp"
	printf 'unknown\n' >"$FX/sha-mode"

	fx_stub "$FX/bin/depmod" 'exit 0'
	fx_stub "$FX/bin/make" "exec sh '$here/make-stub.sh' \"\$@\""
	# The patcher follows the documented key=value contract and records each
	# revision under the name of its output, which is what the driver passes.
	cat >"$FX/bin/geforce-sriov-patch" <<'EOF'
#!/bin/sh
set -eu
fx=${FXDIR:?}
mode=${FXMODE:?}
sha() { sha256sum "$1" | cut -d' ' -f1; }
case $1 in
	abi-check)
		# Marker files are not modules; the ELF-mode cases run the real check.
		[ "$mode" = elf ] || exit 0
		exec "$REAL_PATCH" "$@" ;;
	apply) shift ;;
	ada) shift; [ "$1" = apply ] || exit 2; shift ;;
	display)
		shift
		case $1 in
			inspect) shift; printf 'function=%s\nfunction_offset=%s\nfile_offset=0\nrelocation_section=.rela.text\nrelocation_offset=1\n' "$(cat "$fx/idx39-function" 2>/dev/null || echo _nv045867rm)" "$(cat "$fx/idx39-offset" 2>/dev/null || echo 0)"; exit 0 ;;
			apply) shift ;;
			*) exit 2 ;;
		esac ;;
	*) exit 2 ;;
esac
input=$1 output=$2
key=$(cat "$input" 2>/dev/null || true)
case $key in
	REFUSE:*) printf 'geforce-sriov-patch: anchor fixture: the recorded context was not found\n' >&2; exit 3 ;;
esac
base=${output##*/}
base=${base%.out}
case $base in
	scheduler|module-gate|vf-count|plugin-mixed|plugin-div4|display-idx39) stage=$base ;;
	scheduler) stage=scheduler ;;
	gate) stage=module-gate ;;
	patched) stage=vf-count ;;
	patched39) stage=display-idx39 ;;
	patched-plugin.mixed) stage=plugin-mixed ;;
	patched-plugin) stage=plugin-div4 ;;
	*) stage=scheduler ;;
esac
case $base in
	patched|patched39)
		if [ "$mode" = elf ] && [ -f "$fx/objects/final-object" ]; then
			cp "$fx/objects/final-object" "$output"
		else
			case $key in
				gate-object) printf 'patched-object\n' >"$output" ;;
				patched-object) printf 'patched39-object\n' >"$output" ;;
				*) cp "$input" "$output" ;;
			esac
		fi ;;
	*)
		# A fixture may supply the object after each revision, as a real patcher produces it.
		if [ "$mode" = elf ] && [ "$base" = scheduler ] && [ -f "$fx/objects/stage1-object" ]; then
			cp "$fx/objects/stage1-object" "$output"
		elif [ "$mode" = elf ] && [ "$base" = gate ] && [ -f "$fx/objects/stage2-object" ]; then
			cp "$fx/objects/stage2-object" "$output"
		else
			case $key in
				stock-object) printf 'scheduler-object\n' >"$output" ;;
				scheduler-object) printf 'gate-object\n' >"$output" ;;
				stock-plugin) printf 'mixed-plugin\n' >"$output" ;;
				mixed-plugin) printf 'patched-plugin\n' >"$output" ;;
				*) cp "$input" "$output" ;;
			esac
		fi ;;
esac
in_sha=$(sha "$input"); out_sha=$(sha "$output")
printf 'revision=%s\nadmission=anchor\nstate=unconfirmed:unknown\ninput_sha256=%s\noutput_sha256=%s\n' \
	"$stage" "$in_sha" "$out_sha"
# Where the revision was found, when the fixture says: "stage function function_offset file_offset".
if [ -f "$fx/stage-functions" ]; then
	awk -v want="$stage" '$1 == want { printf "function=%s\nfunction_offset=%s\nfile_offset=%s\n", $2, $3, $4 }' \
		"$fx/stage-functions"
fi
EOF
	chmod 0755 "$FX/bin/geforce-sriov-patch"

	if [ "$MODE" = marker ]; then
		cat >"$FX/bin/sha256sum" <<EOF
#!/bin/sh
mode=\$(cat "$FX/sha-mode")
key=\$(cat "\$1" 2>/dev/null | tr -d '\\n' || true)
d=
case \$1 in
	*/host.run) [ "\$mode" = known ] && d=8fb252d5eafbb5740c9f623c881334119c532c1960834712956eb5e304e7ec41 ;;
esac
if [ -z "\$d" ] && [ "\$mode" = known ]; then
	case \$key in
		stock-object) d=4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449 ;;
		scheduler-object) d=ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701 ;;
		gate-object) d=f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12 ;;
		patched-object) d=946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29 ;;
		stock-plugin) d=7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7 ;;
		mixed-plugin) d=7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6 ;;
		patched-plugin) d=aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c ;;
	esac
fi
if [ -z "\$d" ]; then exec "$REAL_SHA" "\$@"; fi
printf '%s  %s\\n' "\$d" "\$1"
EOF
		chmod 0755 "$FX/bin/sha256sum"
		cat >"$FX/bin/modinfo" <<'STUB'
#!/bin/sh
# -k answers for the fixture kernel; a file path goes to the real tool.
if [ "$1" = -k ]; then
	kernel=$2
	shift 2
	case $1 in
		-n) if [ "$2" = nvidia ] && [ -f "$FXDIR/root/lib/modules/$kernel/updates/dkms/nvidia.ko" ]; then
			printf '%s/root/lib/modules/%s/updates/dkms/nvidia.ko\n' "$FXDIR" "$kernel"
		fi
		exit 0 ;;
		-F) case $2 in
			vermagic) printf '%s SMP mod_unload\n' "$kernel" ;;
			license) printf 'NVIDIA\n' ;;
			esac
			exit 0 ;;
	esac
	exit 0
fi
exec "$REAL_MODINFO" "$@"
STUB
		chmod 0755 "$FX/bin/modinfo"
		fx_stub "$FX/bin/objcopy" "cp \"$FX/\$(sed -n 's/^text=//p' \"\$5\").text\" \"\$6\""
		fx_stub "$FX/bin/readelf" "printf '    Build ID: %s\\n' \"\$(sed -n 's/^id=//p' \"\$2\")\""
		fx_stub "$FX/bin/make" "cp '$FX/built.ko' \"\$2/nvidia.ko\"; cp '$FX/built.ko' \"\$2/nvidia-vgpu-vfio.ko\""
	fi
	if [ "$MODE" = elf ]; then
		cat >"$FX/bin/modinfo" <<'STUB'
#!/bin/sh
# -k answers for the fixture kernel; a file path goes to the real tool.
if [ "$1" = -k ]; then
	kernel=$2
	shift 2
	case $1 in
		-n) if [ "$2" = nvidia ]; then
			printf '%s/root/lib/modules/%s/updates/dkms/nvidia.ko\n' "$FXDIR" "$kernel"
		fi
		exit 0 ;;
		-F) case $2 in
			vermagic) printf '%s SMP mod_unload\n' "$kernel" ;;
			license) printf 'NVIDIA\n' ;;
			esac
			exit 0 ;;
	esac
	exit 0
fi
exec "$REAL_MODINFO" "$@"
STUB
		chmod 0755 "$FX/bin/modinfo"
	fi

	# Host source tree.
	printf '%s\n' '-DNV_VERSION_STRING=\"580.159.01\"' >"$FX/src/Kbuild"
	printf '%s\n' 'VGX_KVM_BUILD=1' >"$FX/src/conftest.sh"
	printf '%s\n' 'obj-m += nvidia.o' >"$FX/src/nvidia/nvidia-sources.Kbuild"
	printf '#define SHARED 1\n' >"$FX/src/common/inc/shared.h"
	# Companion source tree.
	printf '%s\n' '-DNV_VERSION_STRING=\"580.159.03\"' >"$FX/companion/kernel/Kbuild"
	printf '%s\n' 'GRID_BUILD=1' >"$FX/companion/kernel/conftest.sh"
	printf '#define SHARED 1\n' >"$FX/companion/kernel/common/inc/shared.h"
	printf '#define SECOND_ONLY 1\n' >"$FX/companion/kernel/common/inc/second-only.h"
	mkdir -p "$FX/companion/kernel/nvidia-uvm" "$FX/companion/kernel/nvidia-modeset" "$FX/companion/kernel/nvidia-drm"
	printf 'obj-m += nvidia-uvm.o\n' >"$FX/companion/kernel/nvidia-uvm/nvidia-uvm.Kbuild"
	printf 'obj-m += nvidia-modeset.o\n' >"$FX/companion/kernel/nvidia-modeset/nvidia-modeset.Kbuild"
	printf '#include "nvidia-drm-priv.h"\n#include "nvidia-drm-drv.h"\nint fixture_drm;\n' \
		>"$FX/companion/kernel/nvidia-drm/nvidia-drm-drv.c"
	printf 'obj-m += nvidia-drm.o\n' >"$FX/companion/kernel/nvidia-drm/nvidia-drm.Kbuild"
	# Public source: version.mk, the NVKMS Makefile and a mode 0 source tree.
	printf 'NVIDIA_VERSION = 580.159.03\n' >"$FX/nvkms-version.mk"
	mkdir -p "$FX/nvkms/src/nvidia-modeset"
	cp "$FX/nvkms-version.mk" "$FX/nvkms/version.mk"
	printf 'all:\n\t@true\n' >"$FX/nvkms/src/nvidia-modeset/Makefile"
	printf '/* stock sysmem push buffer */\n' >"$FX/nvkms/src/nvidia-modeset/nvkms-rm.c"
	# Fixture source patches, unique in the fixture files.
	cat >"$FX/patchdir/gpu-allow.patch" <<'EOF'
--- a/nvidia-drm/nvidia-drm-drv.c
+++ b/nvidia-drm/nvidia-drm-drv.c
@@ -1,2 +1,3 @@
 #include "nvidia-drm-priv.h"
 #include "nvidia-drm-drv.h"
+/* fixture: selected GPU IDs */
EOF
	cat >"$FX/patchdir/allocation-threshold.patch" <<'EOF'
--- a/nvidia/os-interface.c
+++ b/nvidia/os-interface.c
@@ -1,2 +1,2 @@
 /* fixture allocation */
-#define KMALLOC_LIMIT 8192
+#define KMALLOC_LIMIT 131072
EOF
	printf '/* fixture allocation */\n#define KMALLOC_LIMIT 8192\n' >"$FX/src/nvidia/os-interface.c"
	# Header tree the script and the ABI gate read.
	mkdir -p "$FX/root/lib/modules/$kdir/build"
	printf '\n' >"$FX/root/lib/modules/$kdir/build/Makefile"
	printf '0xdeadbeef\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\t\n' \
		>"$FX/root/lib/modules/$kdir/build/Module.symvers"
	printf 'NVIDIA\n' >"$FX/license"
	cat >"$FX/host.run" <<'RUN'
#!/bin/sh
# Fixture package: --extract-only copies the prepared kernel tree.
set -eu
[ "$1" = --extract-only ] || exit 2
mkdir NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm
cp -a "$FXDIR/src/." NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm/
mkdir -p NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm/kernel
cp -a "$FXDIR/src/." NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm/kernel/
RUN
	chmod 0755 "$FX/host.run"
}

# Marker-mode files: the vendor object, the installed module, the built module,
# the plugin and the two .text images.
fx_marker_files() {
	printf 'stock-object\n' >"$FX/src/nvidia/nv-kernel.o_binary"
	mkdir -p "$FX/root/lib/modules/$kdir/updates/dkms" "$FX/root/usr/lib/x86_64-linux-gnu"
	printf 'text=BBBB\nid=installed-id\n' >"$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko"
	printf 'stock-plugin\n' >"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01"
	printf 'text=AAAA\nid=built-id\n' >"$FX/built.ko"
	fx_plain_text "$FX/AAAA.text" 1
	fx_plain_text "$FX/BBBB.text" 0
}

# ELF-mode files: a stock object, its patched counterpart, and the modules.
fx_elf_files() {
	layout=${1:-display}
	cc=${CC:-gcc}
	ld=${LD:-ld}
	objcopy=${OBJCOPY:-objcopy}
	command -v "$cc" >/dev/null 2>&1 || return 1
	command -v "$ld" >/dev/null 2>&1 || return 1
	cat >"$FX/objects/a.c" <<'EOF'
__attribute__((noinline)) int f1(int x) { return x + 1; }
__attribute__((noinline)) int f2(int x) { return x * 3; }
__attribute__((noinline)) int _nv045867rm(void) { return 0; }
EOF
	"$cc" -O1 -c -fno-asynchronous-unwind-tables -fno-stack-protector \
		"$FX/objects/a.c" -o "$FX/objects/compiled-object"
	if [ "$layout" = plain ]; then
		# The plain gate compares whole .text images at its eight recorded offsets.
		fx_plain_text "$FX/objects/stock.text" 0
		fx_plain_text "$FX/objects/patched.text" 1
	elif [ "$layout" = shifted ]; then
		# Another host version: each revision changes bytes wherever this build put its function.
		# The object after each revision is kept, and the table says where the patcher found it.
		"$objcopy" -O binary -j .text "$FX/objects/compiled-object" "$FX/objects/stock.text"
		python3 - "$FX/objects" "$FX/stage-functions" <<'PYSHIFT' || return 1
import re, subprocess, sys
objects, table = sys.argv[1], sys.argv[2]
path = objects + '/compiled-object'
syms = {}
for line in subprocess.run(['readelf', '-sW', path], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 8 and p[0].rstrip(':').isdigit() and p[6] not in ('UND', 'ABS'):
        syms[p[7]] = int(p[1], 16)
text_off = None
for line in subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', line)
    if m and m.group(2) == '.text':
        text_off = int(m.group(5), 16)
sites = [('scheduler', 'f1', 0), ('module-gate', 'f2', 1), ('vf-count', '_nv045867rm', 0)]
if text_off is None or any(name not in syms for _, name, _ in sites):
    sys.exit(1)
text = bytearray(open(objects + '/stock.text', 'rb').read())
rows = []
for number, (stage, name, inside) in enumerate(sites, 1):
    at = syms[name] + inside
    text[at:at + 2] = b'\x90\x90'
    open(objects + ('/patched.text' if number == 3 else '/stage%d.text' % number), 'wb').write(text)
    rows.append('%s %s 0x%x 0x%x' % (stage, name, inside, text_off + at))
open(table, 'w').write('\n'.join(rows) + '\n')
PYSHIFT
		for number in 1 2; do
			"$objcopy" --update-section .text="$FX/objects/stage$number.text" \
				"$FX/objects/compiled-object" "$FX/objects/stage$number-object"
		done
	else
		# The display gate derives its offsets from the object's own bytes. The idx 39 bytes
		# are planted at the function's real .text offset: the compiler decides where the
		# function lands, so a fixed offset breaks under another compiler.
		"$objcopy" -O binary -j .text "$FX/objects/compiled-object" "$FX/objects/stock.text"
		cp "$FX/objects/stock.text" "$FX/objects/patched.text"
		symrel=$(python3 - "$FX/objects/compiled-object" <<'PYSYM'
import re, subprocess, sys
path = sys.argv[1]
syms = subprocess.run(['readelf', '-sW', path], capture_output=True, text=True).stdout
value = ndx = None
for line in syms.splitlines():
    p = line.split()
    if len(p) >= 8 and p[7] == '_nv045867rm' and p[6] not in ('UND', 'ABS'):
        value, ndx = int(p[1], 16), p[6]
        break
if value is None:
    sys.exit(1)
secs = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
base = None
for line in secs.splitlines():
    m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', line)
    if m and m.group(2) == '.text':
        base = int(m.group(4), 16)
        break
if base is None:
    sys.exit(1)
print(value - base)
PYSYM
		) || return 1
		[ -n "$symrel" ] || return 1
		printf '\231' | dd of="$FX/objects/patched.text" bs=1 seek=1 conv=notrunc status=none
		printf '\061\300\220\220\220' | dd of="$FX/objects/patched.text" bs=1 seek="$symrel" conv=notrunc status=none
	fi
	"$objcopy" --update-section .text="$FX/objects/stock.text" \
		"$FX/objects/compiled-object" "$FX/objects/stock-object"
	"$objcopy" --update-section .text="$FX/objects/patched.text" \
		"$FX/objects/compiled-object" "$FX/objects/final-object"
	python3 - "$FX/objects/buildid.bin" <<'PY'
import struct, sys
name = b'GNU\x00'
desc = bytes.fromhex('00112233445566778899aabbccddeeff00112233')
open(sys.argv[1], 'wb').write(struct.pack('<III', len(name), len(desc), 3) + name + desc)
PY
	python3 - "$FX/objects/core.modinfo" "$kernel" <<'PYMOD'
import sys
fields = ['vermagic=%s SMP mod_unload' % sys.argv[2], 'version=580.159.01',
          'depends=nvidia', 'license=NVIDIA']
open(sys.argv[1], 'wb').write(b'\0'.join(f.encode() for f in fields) + b'\0')
PYMOD
	python3 - "$FX/objects/core.versions" <<'PY'
import struct
name = 'module_layout'.encode()
open(__import__('sys').argv[1], 'wb').write(struct.pack('<Q', 0xdeadbeef) + name + b'\x00' * (56 - len(name)))
PY
	"$ld" -r "$FX/objects/stock-object" -o "$FX/objects/stock.ko"
	"$ld" -r "$FX/objects/final-object" -o "$FX/objects/built.ko"
	for module in stock built; do
		"$objcopy" --add-section ".modinfo=$FX/objects/core.modinfo" \
			--add-section ".note.gnu.build-id=$FX/objects/buildid.bin" \
			--add-section "__versions=$FX/objects/core.versions" \
			"$FX/objects/$module.ko" "$FX/objects/$module.core.ko"
	done
	mkdir -p "$FX/root/lib/modules/$kdir/updates/dkms" "$FX/root/usr/lib/x86_64-linux-gnu"
	cp "$FX/objects/stock.core.ko" "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko"
	cp "$FX/objects/stock-object" "$FX/src/nvidia/nv-kernel.o_binary"
	cp "$FX/objects/built.core.ko" "$FX/modules/nvidia.ko"
	cp "$FX/objects/stock.core.ko" "$FX/modules/nvidia-vgpu-vfio.ko"
	printf 'stock-plugin\n' >"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01"
	printf '_nv045867rm\n' >"$FX/idx39-function"
	printf '0\n' >"$FX/idx39-offset"
	fx_elf_companion nvidia-uvm 580.159.01 'depends=nvidia' ''
	fx_elf_companion nvidia-modeset 580.159.01 'depends=nvidia' ''
	fx_elf_companion nvidia-drm 580.159.03 'depends=nvidia,nvidia-modeset' ''
	printf 'nv-modeset-kernel 580.159.03\n' >"$FX/modules/nv-modeset-kernel.o"
	return 0
}

# A companion module with its own version metadata and, optionally, one required CRC.
fx_elf_companion() {
	name=$1
	version=$2
	depends=$3
	crc_symbol=$4
	modinfo=$FX/objects/$name.modinfo
	python3 - "$modinfo" "$kernel" "$version" "$depends" <<'PYMOD'
import sys
fields = ['vermagic=%s SMP mod_unload' % sys.argv[2], 'version=' + sys.argv[3],
          sys.argv[4], 'license=NVIDIA']
if sys.argv[1].endswith('nvidia-modeset.modinfo'):
    fields.append('description=NVKMS core 580.159.03')
open(sys.argv[1], 'wb').write(b'\0'.join(f.encode() for f in fields) + b'\0')
PYMOD
	python3 - "$FX/objects/$name.versions" "$crc_symbol" <<'PY'
import struct, sys
entries = [(0xdeadbeef, 'module_layout')]
if sys.argv[2]:
    entries.append((0x12345678, sys.argv[2]))
data = b''
for crc, name in entries:
    raw = name.encode()
    data += struct.pack('<Q', crc) + raw + b'\x00' * (56 - len(raw))
open(sys.argv[1], 'wb').write(data)
PY
	objcopy --add-section ".modinfo=$modinfo" \
		--add-section ".note.gnu.build-id=$FX/objects/buildid.bin" \
		--add-section "__versions=$FX/objects/$name.versions" \
		"$FX/objects/stock.ko" "$FX/modules/$name.ko"
}

# Run and keep the exit status in rc (POSIX sh has no way to return and keep $?).
fx_exec() {
	FX_SHA=$1
	shift
	printf '%s\n' "$FX_SHA" >"$FX/sha-mode"
	rc=0
	source_args="--source $FX/src"
	case " $* " in
		*" --driver-file "*|*" --retained "*) source_args= ;;
	esac
	# FX_EVERY_KERNEL=1: no --kernel, as uninstall runs --restore.
	kernel_args="--kernel $kernel"
	[ -z "${FX_EVERY_KERNEL:-}" ] || kernel_args=
	(
		cd "$FX"
		# shellcheck disable=SC2086 # the fields are built above, never split from data
		env -i PATH="$FX/bin:/usr/sbin:/usr/bin:/sbin:/bin" TMPDIR="$FX/tmp" \
			FXDIR="$FX" FXMODE="$MODE" HOME="$FX" \
			REAL_MODINFO="$REAL_MODINFO" REAL_SHA="$REAL_SHA" REAL_PATCH="$REAL_PATCH" \
			GS_DRIVER_PATCH_DIR="$FX/patchdir" GS_DRIVER_FAULT="${FX_FAULT:-}" \
			sh "$driver" --root "$FX/root" $kernel_args $source_args "$@" \
			>"$FX/out" 2>&1
	) || rc=$?
	out=$(cat "$FX/out")
}

record_field() { sed -n "s/^$1=//p" "$FX/root/var/lib/geforce-sriov/driver-build"; }
# Give one field of the build record another value, as an earlier run could have left it.
record_set() {
	sed "s|^$1=.*|$1=$2|" "$FX/root/var/lib/geforce-sriov/driver-build" >"$FX/record.edit" &&
		cp "$FX/record.edit" "$FX/root/var/lib/geforce-sriov/driver-build"
}

# --- the owner of the Ada VFs' framebuffer slots (stage B of the build) -------

# An ELF fixture host with the owner's files beside the source patches, and the module that the
# build with the owner leaves. $1: the object the module's closed part is linked from; $2: the
# owner's entry points that module defines; $3: a kernel symbol it imports.
fx_owner_files() {
	mkdir -p "$FX/patchdir/fb-owner"
	printf '/* fixture owner */\n' >"$FX/patchdir/fb-owner/gs-fb-owner.c"
	printf '/* fixture owner header */\n' >"$FX/patchdir/fb-owner/gs-fb-owner.h"
	printf 'int open_layer;\n' >"$FX/src/nvidia/nv.c"
	printf 'int pci_layer;\n' >"$FX/src/nvidia/nv-pci.c"
	printf 'previous\n' >"$FX/src/nvidia/os-interface.o"
	cat >"$FX/patchdir/fb-owner/fb-owner.patch" <<'PATCH'
--- a/nvidia/nv.c
+++ b/nvidia/nv.c
@@ -1 +1,2 @@
 int open_layer;
+int gs_fb_called;
--- a/nvidia/nvidia-sources.Kbuild
+++ b/nvidia/nvidia-sources.Kbuild
@@ -1 +1,2 @@
 obj-m += nvidia.o
+NVIDIA_SOURCES += nvidia/gs-fb-owner.c
PATCH
	{
		printf 'extern int %s(int);\n' "$3"
		printf 'const char gs_fb_mark[] = "geforce-sriov-fb-abi=1;";\n'
		for entry in $2; do
			printf '__attribute__((noinline)) int %s(int x) { return %s(x) + 1; }\n' "$entry" "$3"
		done
	} >"$FX/objects/owner.c"
	"${CC:-gcc}" -O1 -c -fno-asynchronous-unwind-tables -fno-stack-protector "$FX/objects/owner.c" -o "$FX/objects/owner.o"
	"${LD:-ld}" -r "$FX/objects/$1" "$FX/objects/owner.o" -o "$FX/objects/owner.ko"
	objcopy --add-section ".modinfo=$FX/objects/core.modinfo" \
		--add-section ".note.gnu.build-id=$FX/objects/buildid.bin" \
		--add-section "__versions=$FX/objects/core.versions" \
		"$FX/objects/owner.ko" "$FX/modules/nvidia-owner.ko"
}
owner_entries='gs_fb_init gs_fb_exit gs_fb_ioctl_pre gs_fb_ioctl_post gs_fb_file_closed gs_fb_gpu_remove'
installed_core() { printf '%s\n' "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko"; }
kept_tree() { printf '%s\n' "$FX/root/var/lib/geforce-sriov/build/$kernel"; }
# A build with the owner that is not accepted: $1 names the case, $2 is what the record must say.
# The verified module is installed, the tree is as it was verified, and the run succeeds.
owner_falls_back() {
	fx_exec unknown
	check "$1: the run succeeds" test "$rc" -eq 0
	check "$1: a warning says the module is installed without the owner" contains "$out" \
		'warning: nvidia.ko is installed without the owner of the Ada framebuffer slots: '
	check "$1: the reason" contains "$(record_field fb_owner_reason)" "$2"
	check "$1: the record says so" test "$(record_field fb_owner)/$(record_field fb_owner_wanted)" = 0/1
	check "$1: the verified module is the one installed" test \
		"$(sha256sum "$(installed_core)" | cut -d' ' -f1)" = "$(sha256sum "$FX/modules/nvidia.ko" | cut -d' ' -f1)"
	check "$1: the tree that is kept is the verified one" test \
		"$(cat "$(kept_tree)/nvidia/nv.c")/$(test -e "$(kept_tree)/nvidia/gs-fb-owner.c" || echo gone)" = 'int open_layer;/gone'
}
case_owner() {
	fx_init "$FXROOT/owner" elf
	if ! fx_elf_files plain; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_owner_files final-object "$owner_entries" capable
	fx_exec unknown
	check "owner: installs" test "$rc" -eq 0
	check "owner: the files are not the recorded ones, on a build that is not confirmed: taken as they are, and said" contains "$out" \
		'the framebuffer owner is not recorded for driver 580.159.01; it is built as it is'
	check "owner: the closed functions were compared" contains "$out" 'functions of the closed object that the modules name are the same bytes in both'
	check "owner: said" contains "$out" 'nvidia.ko carries the owner of the Ada framebuffer slots (anchor)'
	check "owner: two builds" test "$(printf '%s\n' "$out" | grep -c "^+ 'make' ")" -eq 2
	check "owner: the record" test \
		"$(record_field fb_owner)/$(record_field fb_owner_wanted)/$(record_field fb_owner_admission)/$(record_field fb_owner_abi)" = 1/1/anchor/1
	check "owner: the record names the files" test "$(record_field fb_owner_patch_sha256)" = \
		"$(sha256sum "$FX/patchdir/fb-owner/fb-owner.patch" | cut -d' ' -f1)"
	check "owner: the record keeps the digest of the verified text" test "$(record_field base_text_sha256 | wc -c)" -eq 65
	check "owner: the module with the owner is the one installed" grep -aqF 'geforce-sriov-fb-abi=1;' "$(installed_core)"
	check "owner: the tree that is kept has the owner" test -f "$(kept_tree)/nvidia/gs-fb-owner.c" -a -f "$(kept_tree)/common/inc/gs-fb-owner.h"
	fx_exec unknown
	check "owner, again: nothing to do" contains "$out" 'already prepared for'
	# A newer package whose owner changed: the installed module was built with other files, so it
	# is built again, once. The same when the record says the owner's interface is another.
	printf '/* a later revision */\n' >>"$FX/patchdir/fb-owner/gs-fb-owner.c"
	fx_exec unknown
	check "owner changed: built again" contains "$out" \
		'the owner of the Ada framebuffer slots in this package is not the one the installed nvidia.ko was built with; the modules are built again'
	check "owner changed: not the shortcut" not_contains "$out" 'already prepared for'
	check "owner changed: the module carries the owner" contains "$out" 'nvidia.ko carries the owner of the Ada framebuffer slots (anchor)'
	check "owner changed: the record names the new file" test "$(record_field fb_owner_c_sha256)" = \
		"$(sha256sum "$FX/patchdir/fb-owner/gs-fb-owner.c" | cut -d' ' -f1)"
	fx_exec unknown
	check "owner changed, again: nothing to do" contains "$out" 'already prepared for'
	record_set fb_owner_abi 0
	fx_exec unknown
	check "another interface in the record: built again" not_contains "$out" 'already prepared for'
	check "another interface in the record: the record is current" test "$(record_field fb_owner_abi)" = 1
	fx_exec unknown
	check "another interface in the record, again: nothing to do" contains "$out" 'already prepared for'
	# Without the owner on request: built again, and said once without a warning.
	fx_exec unknown --no-fb-owner
	check "owner off: built again" not_contains "$out" 'already prepared for'
	check "owner off: said, and no warning" test "$(printf '%s\n' "$out" | grep -c 'without the owner of the Ada framebuffer slots')/$(printf '%s\n' "$out" | grep -c 'warning: nvidia.ko')" = 1/0
	check "owner off: the record" test "$(record_field fb_owner)/$(record_field fb_owner_wanted)" = 0/0
	check "owner off: the module without the owner is installed" test \
		"$(sha256sum "$(installed_core)" | cut -d' ' -f1)" = "$(sha256sum "$FX/modules/nvidia.ko" | cut -d' ' -f1)"
	fx_exec unknown --no-fb-owner
	check "owner off, again: nothing to do" contains "$out" 'already prepared for'
	fx_exec unknown
	check "owner wanted again: built again" contains "$out" 'nvidia.ko carries the owner'
	# A build that carries test code (the hook proof) is replaced, whatever else its record says.
	printf 'test_only=kofb-hook-proof\n' >>"$FX/root/var/lib/geforce-sriov/driver-build"
	fx_exec unknown
	check "test build: replaced" contains "$out" 'the installed build carries test code (kofb-hook-proof); the modules are built again'
	check "test build: the new record has no such line" test -z "$(record_field test_only)"

	# Each way the build with the owner can be wrong ends with the verified module installed.
	fx_init "$FXROOT/owner-entry" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "${owner_entries% gs_fb_gpu_remove}" capable
	owner_falls_back "owner without an entry point" 'the module built with it does not define gs_fb_gpu_remove'
	fx_exec unknown
	check "owner without an entry point, again: not built again for the same request" contains "$out" 'already prepared for'
	check "owner without an entry point: the record names the files that were tried" test "$(record_field fb_owner_c_sha256)" = \
		"$(sha256sum "$FX/patchdir/fb-owner/gs-fb-owner.c" | cut -d' ' -f1)"
	# The package now has an owner that may be accepted: the build is tried again.
	fx_owner_files final-object "$owner_entries" capable
	printf '/* a later revision */\n' >>"$FX/patchdir/fb-owner/gs-fb-owner.c"
	fx_exec unknown
	check "an owner that was refused, and a newer one: built again, with the owner" contains "$out" 'nvidia.ko carries the owner'

	fx_init "$FXROOT/owner-closed" elf
	fx_elf_files plain || return 0
	# The same object with one byte of its first function changed.
	python3 - "$FX/objects/patched.text" "$FX/objects/changed.text" <<'PY'
import sys
text = bytearray(open(sys.argv[1], 'rb').read())
text[0] ^= 0xff
open(sys.argv[2], 'wb').write(text)
PY
	objcopy --update-section .text="$FX/objects/changed.text" "$FX/objects/compiled-object" "$FX/objects/changed-object"
	fx_owner_files changed-object "$owner_entries" capable
	owner_falls_back "closed code changed" "NVIDIA's closed code is not the same in the module with it"

	fx_init "$FXROOT/owner-import" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "$owner_entries" klp_enable_patch
	owner_falls_back "an import that is not on the list" 'imports klp_enable_patch, which is not on the list recorded for it'

	fx_init "$FXROOT/owner-object" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "$owner_entries" capable
	: >"$FX/owner-extra-object"
	owner_falls_back "another object changed" 'changed an object it has no business with: ./nvidia/os-interface.o'

	fx_init "$FXROOT/owner-compile" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "$owner_entries" capable
	: >"$FX/owner-build-fails"
	owner_falls_back "the owner does not compile" 'it did not compile'

	# The same on a display build: the companion modules and the symbol table that the failed build
	# left half written are the verified ones again, so the ABI check passes and they are installed.
	fx_init "$FXROOT/owner-compile-display" elf
	fx_elf_files display || return 0
	fx_owner_files final-object "$owner_entries" capable
	: >"$FX/owner-build-fails"
	fx_exec unknown --display --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "the owner does not compile, display build: the run succeeds" test "$rc" -eq 0
	check "the owner does not compile, display build: the reason" contains "$(record_field fb_owner_reason)" 'it did not compile'
	check "the owner does not compile, display build: the verified core is installed" test \
		"$(sha256sum "$(installed_core)" | cut -d' ' -f1)" = "$(sha256sum "$FX/modules/nvidia.ko" | cut -d' ' -f1)"
	for name in nvidia-modeset nvidia-drm; do
		check "the owner does not compile, display build: the verified $name is installed" test \
			"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/$name.ko" | cut -d' ' -f1)" = "$(sha256sum "$FX/modules/$name.ko" | cut -d' ' -f1)"
	done
	check "the owner does not compile, display build: no half written symbol table is kept" not_contains \
		"$(cat "$(kept_tree)/Module.symvers" 2>/dev/null)" 'half written'

	# Without python3 the owner cannot be added or checked: the verified module, and the reason.
	fx_init "$FXROOT/owner-python" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "$owner_entries" capable
	printf '#!/bin/sh\nexit 127\n' >"$FX/bin/python3"
	chmod +x "$FX/bin/python3"
	owner_falls_back "no python3" 'python3 is not installed'
	check "no python3: one build only" test "$(printf '%s\n' "$out" | grep -c "^+ 'make' ")" -eq 1

	# The ABI check refuses the module that was built with the owner: the verified one is checked and installed.
	fx_init "$FXROOT/owner-abi" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "$owner_entries" capable
	python3 - "$FX/objects/owner.modinfo" <<'PY'
import sys
fields = ['vermagic=other SMP mod_unload', 'version=580.159.01', 'license=NVIDIA']
open(sys.argv[1], 'wb').write(b''.join(f.encode() + b'\0' for f in fields))
PY
	objcopy --update-section ".modinfo=$FX/objects/owner.modinfo" "$FX/modules/nvidia-owner.ko" "$FX/modules/nvidia-owner.ko.new"
	mv "$FX/modules/nvidia-owner.ko.new" "$FX/modules/nvidia-owner.ko"
	owner_falls_back "the ABI check refuses the module with the owner" 'the ABI check refused the modules built with it'
	check "the ABI check refuses the module with the owner: checked twice" test "$(printf '%s\n' "$out" | grep -c "abi-check")" -ge 2

	fx_init "$FXROOT/owner-hunk" elf
	fx_elf_files plain || return 0
	fx_owner_files final-object "$owner_entries" capable
	printf 'int another_open_layer;\n' >"$FX/src/nvidia/nv.c"
	fx_exec unknown
	check "a hunk that does not match: the run succeeds" test "$rc" -eq 0
	check "a hunk that does not match: the reason" contains "$(record_field fb_owner_reason)" \
		'its hunks do not match the open layer of driver 580.159.01 exactly once'
	check "a hunk that does not match: the verified module is installed" test \
		"$(sha256sum "$(installed_core)" | cut -d' ' -f1)" = "$(sha256sum "$FX/modules/nvidia.ko" | cut -d' ' -f1)"

	# A confirmed build takes only the files that are recorded for its version.
	fx_init "$FXROOT/owner-confirmed" marker
	fx_marker_files
	mkdir -p "$FX/patchdir/fb-owner"
	printf 'other\n' >"$FX/patchdir/fb-owner/gs-fb-owner.c"
	printf 'int open_layer;\n' >"$FX/src/nvidia/nv.c"
	printf 'int pci_layer;\n' >"$FX/src/nvidia/nv-pci.c"
	cp "$here/../../driver-patches/fb-owner/gs-fb-owner.h" "$here/../../driver-patches/fb-owner/fb-owner.patch" "$FX/patchdir/fb-owner/"
	fx_exec known --driver-file "$FX/host.run"
	check "confirmed build, other files: the run succeeds" test "$rc" -eq 0
	check "confirmed build, other files: not applied" test "$(record_field fb_owner)/$(record_field fb_owner_reason)" = \
		'0/its source files are not the ones recorded for driver 580.159.01'
	check "confirmed build, other files: still confirmed" test "$(record_field confirmation)" = confirmed-inputs
	# The recorded files are admitted by their digests; the fixture tree has no open layer for them to match.
	cp "$here/../../driver-patches/fb-owner/gs-fb-owner.c" "$FX/patchdir/fb-owner/"
	fx_exec known --driver-file "$FX/host.run" --no-fb-owner
	fx_exec known --driver-file "$FX/host.run"
	check "confirmed build, the recorded files: admitted, and the hunks decide" test "$(record_field fb_owner_reason)" = \
		'its hunks do not match the open layer of driver 580.159.01 exactly once'
}

# --- marker-mode cases -------------------------------------------------------

case_plain() {
	fx_init "$FXROOT/plain" marker
	fx_marker_files
	fx_exec known --driver-file "$FX/host.run"
	check "plain: installs with the recorded digests" test "$rc" -eq 0
	check "plain: completion line" contains "$out" 'NVIDIA'"'"'s module and plugin are prepared'
	check "plain: record says confirmed inputs" test "$(record_field confirmation)" = confirmed-inputs
	check "plain: record names the host version" test "$(record_field driver_version)" = 580.159.01
	check "plain: record flags" test "$(record_field display)/$(record_field host_cuda)" = 0/0
	check "plain: patched plugin copy installed" exists \
		"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov"
	check "plain: modprobe file written" test -f "$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf"
	check "plain: build tree kept" exists "$FX/root/var/lib/geforce-sriov/build/$kernel/nvidia.ko"
	check "plain: unrelated module not installed" test ! -e \
		"$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
	# A rerun is a no-op.
	fx_exec known --driver-file "$FX/host.run"
	check "plain rerun: no-op" contains "$out" 'already prepared for'
	check "plain rerun: the result is unchanged" contains "$out" 'install_result=unchanged'
	# A record written under an earlier version policy is classified again; nothing is rebuilt.
	record_set confirmation unconfirmed
	record_set evidence_profile none
	record_set driver_version_confirmed 0
	before=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")
	fx_exec known --driver-file "$FX/host.run"
	check "plain reclassify: exit 0" test "$rc" -eq 0
	check "plain reclassify: the result is record-only" contains "$out" 'install_result=record-only'
	check "plain reclassify: the record is confirmed again" test \
		"$(record_field confirmation)/$(record_field evidence_profile)/$(record_field driver_version_confirmed)" = \
		confirmed-inputs/host-580.159.01-plain/1
	check "plain reclassify: the module is untouched" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")" = "$before"
	check "plain reclassify: no temporary record is left" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build.gs-tmp"
	fx_exec known --driver-file "$FX/host.run"
	check "plain reclassify: the next run changes nothing" contains "$out" 'install_result=unchanged'
	# A plugin copy with the wrong mode is fixed, and that is a change.
	chmod 0644 "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov"
	fx_exec known --driver-file "$FX/host.run"
	check "plain rerun: a mode fix is a change" contains "$out" 'install_result=changed'
	check "plain rerun: the mode is fixed" test \
		"$(stat -c %a "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov")" = 755
	# Foreign change is kept by --restore; an owned module is restored.
	module=$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko
	printf 'foreign\n' >"$module"
	fx_exec known --restore
	check "plain restore: foreign module kept" contains "$out" 'it is not the module this script installed'
	check "plain restore: foreign module untouched" test "$(cat "$module")" = foreign
	check "plain restore: the backup stays for a later attempt" test -f \
		"$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia.ko"
	check "plain restore: the record stays with the kept module" test -f \
		"$FX/root/var/lib/geforce-sriov/driver-build"
	# A foreign file at the plugin copy's name is not removed.
	pcopy=$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov
	printf 'foreign plugin\n' >"$pcopy"
	fx_exec known --restore
	check "plain restore: foreign plugin copy kept" contains "$out" "left $pcopy"
	check "plain restore: foreign plugin copy untouched" test "$(cat "$pcopy")" = 'foreign plugin'
}

case_unknown_anchors() {
	fx_init "$FXROOT/unknown" marker
	fx_marker_files
	fx_exec unknown
	check "unknown inputs with all anchors: proceeds" test "$rc" -eq 0
	check "unknown inputs: notice printed once" test "$(printf '%s\n' "$out" | grep -c '^Best effort:')" -eq 1
	check "unknown inputs: record is unconfirmed" test "$(record_field confirmation)" = unconfirmed
	check "unknown inputs: admission recorded as anchor" test "$(record_field patch_scheduler_admission)" = anchor
	check "unknown inputs: state recorded" test "$(record_field patch_scheduler_state)" = 'unconfirmed:unknown'
	check "unknown inputs: confirmed flag is 0" test "$(record_field driver_version_confirmed)" = 0
	# A rerun must not promote the local output to confirmed.
	fx_exec unknown
	check "unknown inputs: rerun keeps the classification" test "$(record_field confirmation)" = unconfirmed
	check "unknown inputs: rerun keeps confirmed=0" test "$(record_field driver_version_confirmed)" = 0
}

case_identity_refused() {
	fx_init "$FXROOT/identity" marker
	fx_marker_files
	: >"$FX/src/Kbuild"
	fx_exec known --driver-file "$FX/host.run"
	check "unreadable host identity: refused" test "$rc" -eq 3
	check "unreadable host identity: named" contains "$out" 'cannot determine the host driver version'
	check "unreadable host identity: nothing installed" test ! -e \
		"$FX/root/var/lib/geforce-sriov/driver-build"
	printf '%s\n' '-DNV_VERSION_STRING=\"580.159.01\"' '-DNV_VERSION_STRING=\"580.159.03\"' >"$FX/src/Kbuild"
	fx_exec known --driver-file "$FX/host.run"
	check "contradictory host identity: refused" test "$rc" -eq 3
	check "contradictory host identity: nothing installed" test ! -e \
		"$FX/root/var/lib/geforce-sriov/driver-build"
}

case_unknown_refused() {
	fx_init "$FXROOT/refused" marker
	fx_marker_files
	printf 'REFUSE: no anchor\n' >"$FX/src/nvidia/nv-kernel.o_binary"
	fx_exec unknown
	check "missing anchor: refused with exit 3" test "$rc" -eq 3
	check "missing anchor: nothing installed" test ! -e \
		"$FX/root/var/lib/geforce-sriov/driver-build"
	check "missing anchor: no modprobe file" test ! -e "$FX/root/etc/modprobe.d/geforce-sriov-nvidia.conf"
	check "missing anchor: names the failed anchor" contains "$out" 'anchor fixture'
}

# A package is a shell script and extracting it runs it: a file that claims the recorded version
# and is not the recorded file is refused before it runs.
case_package_digest() {
	fx_init "$FXROOT/pkgdigest" marker
	fx_marker_files
	bad=$FX/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run
	printf '#!/bin/sh\n: >"%s/ran"\nexit 0\n' "$FX" >"$bad"
	fx_exec unknown --driver-file "$bad"
	check "package digest: a wrong file under the recorded name is refused" test "$rc" -eq 3
	check "package digest: the refusal says it was not run" contains "$out" 'It was not run'
	check "package digest: and it was not run" test ! -e "$FX/ran"
	check "package digest: nothing installed" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
}


# A second confirmed version is one more block of the table and nothing else. The driver step and
# its libraries are copied beside a table that has a synthetic block, the way an installation holds
# them. The block's digests are the real ones of the marker files, so the unchanged sha256sum
# answers them. A build of that version is confirmed with its own profile and is compared at its
# own offsets; the same tree against the unchanged table is another version, built best effort.
synth_text() {
	image=$1
	dd if=/dev/zero of="$image" bs=1 count=64 status=none
	[ "$2" -eq 0 ] && return 0
	shift 2
	for at do
		printf '\220' | dd of="$image" bs=1 seek="$((at))" conv=notrunc status=none
	done
}
real_digest() { printf '%s\n' "$1" | "$REAL_SHA" | cut -d' ' -f1; }
# The fixture host of the synthetic version 999.1.2; the offsets are those of the built module.
fx_second_version() {
	fx_init "$FXROOT/$1" marker
	shift
	fx_marker_files
	printf '%s\n' '-DNV_VERSION_STRING=\"999.1.2\"' >"$FX/src/Kbuild"
	mv "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01" \
		"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.999.1.2"
	synth_text "$FX/BBBB.text" 0
	synth_text "$FX/AAAA.text" 1 "$@"
	mkdir "$FX/synth"
	cp "$here/../../scripts/geforce-sriov-driver.sh" "$here"/../../scripts/gs-driver-* "$FX/synth/"
	{
		printf '\nhost=999.1.2\n'
		printf 'stock_object=%s\nscheduler_object=%s\ngate_object=%s\npatched_object=%s\n' \
			"$(real_digest stock-object)" "$(real_digest scheduler-object)" \
			"$(real_digest gate-object)" "$(real_digest patched-object)"
		printf 'stock_plugin=%s\nmixed_plugin=%s\npatched_plugin=%s\n' \
			"$(real_digest stock-plugin)" "$(real_digest mixed-plugin)" "$(real_digest patched-plugin)"
		printf 'plain_offsets=0x10 0x11 0x12 0x13 0x20 0x21 0x30 0x31\n'
		printf 'profile_plain=host-999.1.2-plain\n'
	} >>"$FX/synth/gs-driver-versions.tbl"
}
case_second_version() {
	shipped=$driver
	pcopy_dir=usr/lib/x86_64-linux-gnu

	fx_second_version second-version 0x10 0x11 0x12 0x13 0x20 0x21 0x30 0x31
	driver=$FX/synth/geforce-sriov-driver.sh
	fx_exec unknown --dry-run
	check "second version, plan: its own stock digest" contains "$out" "require $(real_digest stock-object), or admit"
	check "second version, plan: its own offsets" contains "$out" \
		'require exactly the eight .text offsets 0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x30, 0x31'
	check "second version, plan: its own plugin" contains "$out" "/libnvidia-vgpu.so.999.1.2'"
	check "second version, plan: classified from its digests" contains "$out" 'version classification: confirmed-inputs'
	fx_exec unknown
	check "second version: the build installs" test "$rc" -eq 0
	check "second version: confirmed inputs" test "$(record_field confirmation)" = confirmed-inputs
	check "second version: its own profile" test "$(record_field evidence_profile)" = host-999.1.2-plain
	check "second version: the record names it" test \
		"$(record_field driver_version)/$(record_field driver_version_confirmed)" = 999.1.2/1
	check "second version: no best-effort notice" not_contains "$out" 'Best effort:'
	check "second version: its plugin copy is installed" test \
		"$(real_digest patched-plugin)" = "$("$REAL_SHA" "$FX/root/$pcopy_dir/libnvidia-vgpu.so.999.1.2.geforce-sriov" | cut -d' ' -f1)"
	check "second version: the link points at that copy" test \
		"$(readlink "$FX/root/$pcopy_dir/libnvidia-vgpu.so")" = libnvidia-vgpu.so.999.1.2.geforce-sriov
	fx_exec unknown
	check "second version, rerun: nothing to do" contains "$out" 'install_result=unchanged'
	fx_exec unknown --restore
	check "second version, restore: exit 0" test "$rc" -eq 0
	check "second version, restore: the link is back at its stock plugin" test \
		"$(readlink "$FX/root/$pcopy_dir/libnvidia-vgpu.so")" = libnvidia-vgpu.so.999.1.2
	check "second version, restore: the copy is gone" test ! -e \
		"$FX/root/$pcopy_dir/libnvidia-vgpu.so.999.1.2.geforce-sriov"
	check "second version, restore: the stock module is back" test \
		"$(cat "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")" = "$(printf 'text=BBBB\nid=installed-id\n')"

	# A module that differs at the offsets of 580.159.01 is not this version's build.
	fx_second_version second-version-offsets 0x14 0x15 0x16 0x17 0x20 0x21 0x30 0x31
	driver=$FX/synth/geforce-sriov-driver.sh
	fx_exec unknown
	check "second version, other offsets: refused" test "$rc" -eq 3
	check "second version, other offsets: names the first one" contains "$out" \
		'module .text differences are not the eight expected offsets; first unexpected offset: 0x14'
	check "second version, other offsets: no record" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"

	# An object of that version with another digest: admitted by anchors, compared at the block's
	# offsets, not confirmed, and the notice lists both confirmed versions.
	fx_second_version second-version-anchors 0x10 0x11 0x12 0x13 0x20 0x21 0x30 0x31
	driver=$FX/synth/geforce-sriov-driver.sh
	printf 'another-object\n' >"$FX/src/nvidia/nv-kernel.o_binary"
	fx_exec unknown
	check "second version, another object: the build installs" test "$rc" -eq 0
	check "second version, another object: not confirmed" test \
		"$(record_field confirmation)/$(record_field evidence_profile)" = unconfirmed/none
	check "second version, another object: the notice lists both versions" contains "$out" \
		'Best effort: driver 999.1.2 is not guaranteed to work. Confirmed versions: 580.159.01, 999.1.2.'

	# The table as shipped has no block for 999.1.2: it is another version. Its revisions are
	# admitted by anchors, and with no offsets recorded or reported the module cannot be compared.
	fx_second_version second-version-unlisted 0x10 0x11 0x12 0x13 0x20 0x21 0x30 0x31
	driver=$shipped
	fx_exec unknown --dry-run
	check "unlisted version, plan: not confirmed" contains "$out" 'version classification: unconfirmed'
	fx_exec unknown
	check "unlisted version: refused" test "$rc" -eq 3
	check "unlisted version: the notice names it and the confirmed version" contains "$out" \
		'Best effort: driver 999.1.2 is not guaranteed to work. Confirmed versions: 580.159.01. Required patch anchors matched'
	check "unlisted version: says why the module cannot be compared" contains "$out" \
		'no .text offsets are recorded for driver 999.1.2, and the patch tool reported none for this build'
	check "unlisted version: nothing installed" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
	check "unlisted version: the module is untouched" test \
		"$(cat "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")" = "$(printf 'text=BBBB\nid=installed-id\n')"
	driver=$shipped
}

# A clean commit leaves no journal and no staged file behind, and an older journal is still read.
case_interrupted_commit() {
	fx_init "$FXROOT/interrupted" marker
	fx_marker_files
	fx_exec known --driver-file "$FX/host.run"
	check "commit: the install succeeds" test "$rc" -eq 0
	rec=$FX/root/var/lib/geforce-sriov/driver-build
	check "commit: no journal after a clean run" test ! -e "$rec.gs-new"
	check "commit: no staged plugin after a clean run" test ! -e "$FX/root/var/lib/geforce-sriov/plugin-copy.gs-new"
	check "commit: no copy of a replaced file after a clean run" test ! -e "$FX/root/var/lib/geforce-sriov/txn"
	check "commit: no temporary module name is left" test ! -e \
		"$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko.gs-new"
	check "commit: the module is renamed into place" contains "$out" 'nvidia.ko.gs-new'
	check "commit: the published record has no journal line" test -z "$(sed -n '/^journal_/p' "$rec")"
	# A journal in the first form of this commit, which had no journal lines: its module is in place,
	# so its record is published.
	mv "$rec" "$rec.gs-new"
	fx_exec known --restore
	check "commit: an older journal is published" contains "$out" 'its record is now in place'
	check "commit: restore then returns the stock module" contains "$out" 'restored the stock module'
	# A leftover that names no module is not a journal.
	mkdir -p "${rec%/*}"
	printf 'kernel=%s\nmodule_sha256=0000\n' "$kernel" >"$rec.gs-new"
	fx_exec known --restore
	check "commit: a leftover that names no module is removed" test ! -e "$rec.gs-new"
	check "commit: and it is not taken for the record" test ! -e "$rec"
}

# --- ELF-mode cases ----------------------------------------------------------

case_cuda() {
	fx_init "$FXROOT/cuda" elf
	if ! fx_elf_files plain; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	stock_core=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown --host-cuda --companion-source "$FX/companion"
	check "host CUDA: installs" test "$rc" -eq 0
	check "host CUDA: uvm installed" exists "$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
	check "host CUDA: record flag" test "$(record_field host_cuda)" = 1
	check "host CUDA: uvm digest recorded" test -n "$(record_field module_nvidia_uvm_sha256)"
	check "host CUDA: uvm stamp is the host version" test "$(record_field module_nvidia_uvm_stamp)" = 580.159.01
	check "host CUDA: second-input-only header imported" exists \
		"$FX/root/var/lib/geforce-sriov/build/$kernel/common/inc/second-only.h"
	check "host CUDA: classification is unconfirmed" test "$(record_field confirmation)" = unconfirmed
	# A later plain request drops UVM; the core is unchanged by that request.
	fx_exec unknown
	check "retire: the plain build keeps the same core" test "$rc" -eq 0
	check "retire: uvm removed" test ! -e "$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
	check "retire: uvm backup removed" test ! -e "$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia-uvm.ko"
	check "retire: record no longer names uvm" test -z "$(record_field module_nvidia_uvm_path)"
	check "retire: record feature flags" test "$(record_field host_cuda)/$(record_field display)" = 0/0
	# The stock backup survived the feature change, so restore returns the original module.
	fx_exec unknown --restore
	check "retire: restore returns the stock core" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$stock_core"
	# A fresh host with a module whose vermagic differs.
	fx_init "$FXROOT/cuda-vermagic" elf
	fx_elf_files plain || return 0
	uvm=$FX/modules/nvidia-uvm.ko
	python3 - "$FX/objects/uvm.modinfo" <<'PYMOD'
import sys
fields = ['vermagic=other SMP mod_unload', 'version=580.159.01', 'depends=nvidia']
open(sys.argv[1], 'wb').write(b'\0'.join(f.encode() for f in fields) + b'\0')
PYMOD
	objcopy --update-section ".modinfo=$FX/objects/uvm.modinfo" \
		--update-section "__versions=$FX/objects/nvidia-uvm.versions" "$uvm" "$uvm.new"
	mv "$uvm.new" "$uvm"
	fx_exec unknown --host-cuda --companion-source "$FX/companion"
	check "ABI gate, wrong vermagic: refused" test "$rc" -eq 3
	check "ABI gate, wrong vermagic: names vermagic" contains "$out" 'vermagic'
	check "ABI gate, wrong vermagic: uvm not installed" test ! -e \
		"$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
}

# NVIDIA's CUDA libraries for host CUDA: with --cuda-stage the driver step copies them from the
# second package into a directory of their own, with an index, for the CLI to install.
fx_cuda_libs() {
	# The package as extracted: the libraries beside its kernel directory.
	for lib in libcuda:libcuda.so.1 libnvidia-ptxjitcompiler:libnvidia-ptxjitcompiler.so.1 libnvidia-nvvm:libnvidia-nvvm.so.4 \
		libnvidia-ml:libnvidia-ml.so.1; do
		printf 'int fixture_%s(void) { return %s; }\n' "$(printf '%s' "${lib%%:*}" | tr -c 'a-z\n' '_')" "$1" >"$FX/objects/lib.c"
		"${CC:-gcc}" -shared -fPIC -Wl,-soname,"${lib#*:}" -o "$FX/companion/${lib%%:*}.so.$2" "$FX/objects/lib.c"
	done
	# One library has a version of its own in its name, as in NVIDIA's package.
	"${CC:-gcc}" -shared -fPIC -Wl,-soname,libnvidia-nvvm70.so.4 -o "$FX/companion/libnvidia-nvvm70.so.4" "$FX/objects/lib.c"
	ln -sf "libcuda.so.$2" "$FX/companion/libcuda.so.1"
}
case_cuda_libs() {
	fx_init "$FXROOT/cuda-libs" elf
	if ! fx_elf_files plain; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	stage=$FX/root/var/lib/geforce-sriov/host-cuda
	fx_cuda_libs 1 580.159.03
	# Without --cuda-stage nothing is looked for.
	fx_exec unknown --host-cuda --companion-source "$FX/companion"
	check "CUDA libraries, not asked for: installs" test "$rc" -eq 0
	check "CUDA libraries, not asked for: none is staged" test ! -e "$stage"
	fx_exec unknown
	# After a build.
	fx_exec unknown --host-cuda --companion-source "$FX/companion" --cuda-stage "$stage"
	check "CUDA libraries: installs" test "$rc" -eq 0
	check "CUDA libraries: said" contains "$out" \
		"NVIDIA's CUDA libraries 580.159.03 are ready to be installed: libcuda.so.580.159.03 libnvidia-ptxjitcompiler.so.580.159.03 libnvidia-nvvm.so.580.159.03 libnvidia-nvvm70.so.4"
	check "CUDA libraries: the ones the package lacks are named" contains "$out" \
		'warning: the second package does not have libnvidia-gpucomp libcudadebugger; a CUDA program that needs it may fail'
	check "CUDA libraries: the index names the version" test "$(sed -n 's/^version=//p' "$stage/index")" = 580.159.03
	check "CUDA libraries: libcuda with its digest and the name the loader asks for" test \
		"$(sed -n 's/^lib=libcuda\.so\.580\.159\.03 //p' "$stage/index")" = \
		"$(sha256sum "$FX/companion/libcuda.so.580.159.03" | cut -d' ' -f1) libcuda.so.1"
	check "CUDA libraries: the NVVM library by the name it states" contains "$(cat "$stage/index")" ' libnvidia-nvvm.so.4'
	check "CUDA libraries: four libraries" test "$(grep -c '^lib=' "$stage/index")" -eq 4
	check "CUDA libraries: the one with a version of its own, under its own name" contains "$(cat "$stage/index")" \
		' libnvidia-nvvm70.so.4'
	check "CUDA libraries: the copy is the file" cmp -s "$FX/companion/libcuda.so.580.159.03" "$stage/libcuda.so.580.159.03"
	check "CUDA libraries: no link is copied" test ! -e "$stage/libcuda.so.1"
	check "CUDA libraries: the library of nvidia-smi is not taken" test ! -e "$stage/libnvidia-ml.so.580.159.03"
	# The build is installed: the kept second package is opened for the libraries alone.
	rm -rf "$stage"
	fx_exec unknown --retained --host-cuda --cuda-stage "$stage"
	check "CUDA libraries, build installed: nothing is built" contains "$out" 'already prepared for'
	check "CUDA libraries, build installed: taken from the kept package" test \
		"$(sed -n 's/^lib=libcuda\.so\.580\.159\.03 //p' "$stage/index")" = \
		"$(sha256sum "$FX/companion/libcuda.so.580.159.03" | cut -d' ' -f1) libcuda.so.1"
	check "CUDA libraries, build installed: no build" not_contains "$out" "'make'"
	# A second package of another version, given again: its libraries replace the staged ones.
	rm -f "$FX"/companion/lib*.so.*
	fx_cuda_libs 2 580.159.05
	fx_exec unknown --host-cuda --companion-source "$FX/companion" --cuda-stage "$stage"
	check "CUDA libraries, another version: staged" test "$(sed -n 's/^version=//p' "$stage/index")" = 580.159.05
	check "CUDA libraries, another version: the earlier ones are gone" test ! -e "$stage/libcuda.so.580.159.03"
	# A second package without libcuda (its kernel directory alone): one warning, and the run succeeds.
	rm -rf "$stage"
	rm -f "$FX"/companion/lib*.so.*
	fx_exec unknown --host-cuda --companion-source "$FX/companion" --cuda-stage "$stage"
	check "CUDA libraries, a package without them: the run succeeds" test "$rc" -eq 0
	check "CUDA libraries, a package without them: one warning" contains "$out" \
		"warning: NVIDIA's CUDA libraries were not taken from the second package: it has no libcuda.so.<version>"
	check "CUDA libraries, a package without them: nothing is staged" test ! -e "$stage" -a ! -e "$stage.gs-new"
	# Host CUDA off: nothing is staged whatever the option says.
	fx_cuda_libs 1 580.159.03
	fx_exec unknown --cuda-stage "$stage"
	check "CUDA libraries, host CUDA off: nothing is staged" test ! -e "$stage"

	# The second package as a file: the kept copy is checked and extracted for the libraries alone.
	fx_init "$FXROOT/cuda-libs-file" elf
	fx_elf_files plain || return 0
	stage=$FX/root/var/lib/geforce-sriov/host-cuda
	fx_cuda_libs 1 580.159.03
	cat >"$FX/second.run" <<'RUN'
#!/bin/sh
# Fixture package: --extract-only copies the prepared second package.
set -eu
printf '%s\n' "$0" >>"$FXDIR/ran-from"
[ "$1" = --extract-only ] || exit 2
mkdir second-package
cp -a "$FXDIR/companion/." second-package/
RUN
	chmod 0755 "$FX/second.run"
	fx_exec unknown --host-cuda --companion-file "$FX/second.run"
	check "CUDA libraries, a package file: the build installs" test "$rc" -eq 0
	rm -f "$FX/second.run" "$FX/ran-from"
	fx_exec unknown --retained --host-cuda --cuda-stage "$stage"
	check "CUDA libraries, a kept package file: nothing is built" contains "$out" 'already prepared for'
	check "CUDA libraries, a kept package file: staged" test "$(grep -c '^lib=' "$stage/index")" -eq 4
	check "CUDA libraries, a kept package file: only a copy of the package was run" test -s "$FX/ran-from" -a \
		-z "$(grep -v "^$FX/tmp/geforce-sriov-driver\.[A-Za-z0-9]*/second\.run\$" "$FX/ran-from")"
	check "CUDA libraries, a kept package file: no scratch directory is left" test \
		-z "$(find "$FX/tmp" -maxdepth 1 -name 'geforce-sriov-driver.*' -print)"
	# A kept package that was changed since is not run.
	rm -rf "$stage" "$FX/ran-from"
	printf '\n: >"%s/ran"\n' "$FX" >>"$(kept_dir)/companion/second.run"
	fx_exec unknown --retained --host-cuda --cuda-stage "$stage"
	check "CUDA libraries, a kept package that was changed: the run succeeds" test "$rc" -eq 0
	check "CUDA libraries, a kept package that was changed: not run" test ! -e "$FX/ran" -a ! -e "$FX/ran-from"
	check "CUDA libraries, a kept package that was changed: one warning" contains "$out" \
		"warning: NVIDIA's CUDA libraries were not taken from the second package: none was given, and none that an earlier install kept can be used"
	check "CUDA libraries, a kept package that was changed: nothing is staged" test ! -e "$stage"
}

# A display build is installed, and the next request has no display: host CUDA alone, or plain.
# The display build's own .text differs from NVIDIA's at more than the eight offsets (the idx 39
# change and the allocation threshold), so the new build is compared with NVIDIA's module, which is
# the recorded backup, and not with the installed module.
fx_display_installed() {
	fx_init "$FXROOT/$1" elf
	fx_elf_files plain || return 1
	stock_core=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown
	[ "$rc" -eq 0 ] || return 1
	# The installed core becomes one whose .text carries four more changed bytes, and the record
	# names it as a display build, as a display install leaves both.
	installed=$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko
	cp "$FX/objects/patched.text" "$FX/objects/display.text"
	for at in 0x14870 0x14871 0x148aa 0x148ab; do
		printf '\220' | dd of="$FX/objects/display.text" bs=1 seek="$((at))" conv=notrunc status=none
	done
	objcopy --update-section .text="$FX/objects/display.text" "$installed" "$installed.new"
	mv "$installed.new" "$installed"
	display_core=$(sha256sum "$installed" | cut -d' ' -f1)
	record_set module_sha256 "$display_core"
	record_set module_nvidia_sha256 "$display_core"
	record_set display 1
}
case_after_display() {
	if ! fx_display_installed after-display-cuda; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_exec unknown --no-display --host-cuda --companion-source "$FX/companion"
	check "host CUDA after display: installs" test "$rc" -eq 0
	check "host CUDA after display: compared with NVIDIA's module" contains "$out" \
		"'.text' '$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia.ko'"
	check "host CUDA after display: the record flags" test "$(record_field display)/$(record_field host_cuda)" = 0/1
	check "host CUDA after display: the core is the new build" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$(record_field module_sha256)"
	check "host CUDA after display: uvm is installed" exists "$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
	check "host CUDA after display: the backup is still NVIDIA's module" test \
		"$(sha256sum "$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia.ko" | cut -d' ' -f1)/$(record_field stock_sha256)" = \
		"$stock_core/$stock_core"
	fx_exec unknown --restore
	check "host CUDA after display: restore returns NVIDIA's module" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$stock_core"

	fx_display_installed after-display-plain || return 0
	fx_exec unknown --no-display --no-host-cuda
	check "plain after display: installs" test "$rc" -eq 0
	check "plain after display: the record flags" test "$(record_field display)/$(record_field host_cuda)" = 0/0
	check "plain after display: the backup is still NVIDIA's module" test \
		"$(sha256sum "$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia.ko" | cut -d' ' -f1)" = "$stock_core"

	# The check is not weakened: a build that differs from NVIDIA's module anywhere else is refused.
	fx_display_installed after-display-extra || return 0
	cp "$FX/objects/patched.text" "$FX/objects/extra.text"
	printf '\220' | dd of="$FX/objects/extra.text" bs=1 seek="$((0x14870))" conv=notrunc status=none
	objcopy --update-section .text="$FX/objects/extra.text" "$FX/modules/nvidia.ko" "$FX/modules/nvidia.ko.new"
	mv "$FX/modules/nvidia.ko.new" "$FX/modules/nvidia.ko"
	fx_exec unknown --no-display --host-cuda --companion-source "$FX/companion"
	check "host CUDA after display, another difference: refused" test "$rc" -eq 3
	check "host CUDA after display, another difference: names it" contains "$out" \
		'module .text differences are not the eight expected offsets; first unexpected offset: 0x14870'
	check "host CUDA after display, another difference: the installed core stays" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$display_core"

	# A build whose .text is NVIDIA's own, byte for byte, carries none of the revisions. Equal to
	# the backup is not "already installed": it is refused, and nothing is recorded as this build.
	fx_display_installed after-display-stock || return 0
	record_before=$(cat "$FX/root/var/lib/geforce-sriov/driver-build")
	objcopy --update-section .text="$FX/objects/stock.text" "$FX/modules/nvidia.ko" "$FX/modules/nvidia.ko.new"
	mv "$FX/modules/nvidia.ko.new" "$FX/modules/nvidia.ko"
	fx_exec unknown --no-display --host-cuda --companion-source "$FX/companion"
	check "host CUDA after display, a build without the revisions: refused" test "$rc" -eq 3
	check "host CUDA after display, a build without the revisions: says so" contains "$out" \
		'module .text differences are not the eight expected offsets; first unexpected offset: none (expected offset missing)'
	check "host CUDA after display, a build without the revisions: the installed core stays" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$display_core"
	check "host CUDA after display, a build without the revisions: the record stays" test \
		"$(cat "$FX/root/var/lib/geforce-sriov/driver-build")" = "$record_before"
}

case_cuda_crc_and_provider() {
	fx_init "$FXROOT/cuda-crc" elf
	if ! fx_elf_files plain; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	# module_layout with a CRC the kernel's Module.symvers does not record.
	python3 - "$FX/objects/nvidia-uvm.versions" <<'PY'
import struct
name = b'module_layout'
data = struct.pack('<Q', 0x11111111) + name + b'\x00' * (56 - len(name))
open(__import__('sys').argv[1], 'wb').write(data)
PY
	objcopy --update-section ".modinfo=$FX/objects/nvidia-uvm.modinfo" \
		--update-section "__versions=$FX/objects/nvidia-uvm.versions" "$FX/modules/nvidia-uvm.ko" "$FX/modules/nvidia-uvm.ko.new"
	mv "$FX/modules/nvidia-uvm.ko.new" "$FX/modules/nvidia-uvm.ko"
	fx_exec unknown --host-cuda --companion-source "$FX/companion"
	check "ABI gate, wrong CRC: refused" test "$rc" -eq 3
	check "ABI gate, wrong CRC: reports the mismatch" contains "$out" 'failure=crc_mismatch'
	# A symbol no provider exports.
	python3 - "$FX/objects/nvidia-uvm.versions" <<'PY'
import struct, sys
name = b'nv_absent'
data = struct.pack('<Q', 0x12345678) + name + b'\x00' * (56 - len(name))
open(sys.argv[1], 'wb').write(data)
PY
	objcopy --update-section "__versions=$FX/objects/nvidia-uvm.versions" "$FX/modules/nvidia-uvm.ko" "$FX/modules/nvidia-uvm.ko.new"
	mv "$FX/modules/nvidia-uvm.ko.new" "$FX/modules/nvidia-uvm.ko"
	fx_exec unknown --host-cuda --companion-source "$FX/companion"
	check "ABI gate, missing provider: refused" test "$rc" -eq 3
	check "ABI gate, missing provider: names the symbol" contains "$out" 'nv_absent'
	check "ABI gate refusals: nothing installed" test ! -e \
		"$FX/root/var/lib/geforce-sriov/driver-build"
}

# The gate runs for the plain build too, and the expected vermagic is the target kernel's own.
case_plain_gate() {
	fx_init "$FXROOT/plain-gate" elf
	if ! fx_elf_files plain; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_exec unknown
	check "plain gate: a matching build installs" test "$rc" -eq 0
	check "plain gate: the check ran" contains "$out" 'abi-check'
	check "plain gate: no kernel module, so the installed nvidia module is named" contains "$out" \
		'(vermagic) comes from the installed nvidia module'
	# A module of the kernel itself says the kernel is another build: the built core must not go in.
	fx_init "$FXROOT/plain-gate-kernel" elf
	fx_elf_files plain || return 0
	mkdir -p "$FX/root/lib/modules/$kdir/kernel/drivers"
	python3 - "$FX/objects/intree.modinfo" <<'PYMOD'
import sys
fields = ['vermagic=other-kernel SMP mod_unload', 'license=GPL']
open(sys.argv[1], 'wb').write(b'\0'.join(f.encode() for f in fields) + b'\0')
PYMOD
	objcopy --add-section ".modinfo=$FX/objects/intree.modinfo" "$FX/objects/stock.ko" \
		"$FX/root/lib/modules/$kdir/kernel/drivers/fixture.ko"
	before=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown
	check "plain gate, kernel vermagic differs: refused" test "$rc" -eq 3
	check "plain gate, kernel vermagic differs: names vermagic" contains "$out" 'failure=vermagic'
	check "plain gate, kernel vermagic differs: expected value is the kernel's" contains "$out" 'expected=other-kernel'
	check "plain gate, kernel vermagic differs: the installed module is untouched" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$before"
	check "plain gate, kernel vermagic differs: no record" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
}

# Another host version on the plain path: not guaranteed to work, tried best effort. The built module is
# compared at the offsets the patcher reported for that build, as exactly as the recorded eight.
# The ELF fixture host as a host of 570.1.2, a version with no block in the table.
fx_other_version() {
	printf '%s\n' '-DNV_VERSION_STRING=\"570.1.2\"' >"$FX/src/Kbuild"
	mv "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01" \
		"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.570.1.2"
}
case_plain_other_version() {
	fx_init "$FXROOT/other-version" elf
	if ! fx_elf_files shifted; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_other_version
	fx_exec unknown
	check "other version, plain: the build installs" test "$rc" -eq 0
	check "other version, plain: the notice names it and the confirmed version" contains "$out" \
		'Best effort: driver 570.1.2 is not guaranteed to work. Confirmed versions: 580.159.01. Required patch anchors matched'
	check "other version, plain: one notice" test "$(printf '%s\n' "$out" | grep -c '^Best effort:')" -eq 1
	check "other version, plain: compared at the reported offsets" contains "$out" \
		'at the offsets the patch tool reported for this build: 0x'
	check "other version, plain: recorded as not confirmed" test "$(record_field confirmation)" = unconfirmed
	check "other version, plain: the record names it" test \
		"$(record_field driver_version)/$(record_field driver_version_confirmed)/$(record_field evidence_profile)" = \
		570.1.2/0/none
	check "other version, plain: every revision was admitted by its anchors" test \
		"$(record_field patch_scheduler_admission)/$(record_field patch_vf_count_admission)/$(record_field patch_plugin_div4_admission)" = \
		anchor/anchor/anchor
	check "other version, plain: the plugin copy carries its version" exists \
		"$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.570.1.2.geforce-sriov"
	check "other version, plain: the module was replaced" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = \
		"$(sha256sum "$FX/modules/nvidia.ko" | cut -d' ' -f1)"
	# One more changed byte anywhere else: refused before any installed file changes.
	fx_init "$FXROOT/other-version-extra" elf
	fx_elf_files shifted || return 0
	fx_other_version
	cp "$FX/objects/patched.text" "$FX/objects/extra.text"
	printf '\220' | dd of="$FX/objects/extra.text" bs=1 seek=3 conv=notrunc status=none
	objcopy --update-section .text="$FX/objects/extra.text" "$FX/modules/nvidia.ko" "$FX/modules/nvidia.ko.new"
	mv "$FX/modules/nvidia.ko.new" "$FX/modules/nvidia.ko"
	before=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown
	check "other version, extra change: refused" test "$rc" -eq 3
	check "other version, extra change: names the offset" contains "$out" \
		'are not the offsets the patch tool reported for this build; first unexpected offset: 0x3'
	check "other version, extra change: the installed module is untouched" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$before"
	check "other version, extra change: no record" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
}

case_display() {
	fx_init "$FXROOT/display" elf
	if ! fx_elf_files display; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_exec unknown --display --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "display: installs" test "$rc" -eq 0
	for name in nvidia-modeset nvidia-drm; do
		check "display: $name installed" exists "$FX/root/lib/modules/$kdir/updates/dkms/$name.ko"
		check "display: $name recorded" test -n "$(record_field "module_$(printf '%s' "$name" | tr '-' '_')_sha256")"
	done
	check "display: record flag" test "$(record_field display)" = 1
	check "display: modeset wrapper stamp is the host version" test \
		"$(record_field module_nvidia_modeset_stamp)" = 580.159.01
	check "display: DRM stamp is the public version" test "$(record_field module_nvidia_drm_stamp)" = 580.159.03
	check "display: idx 39 function recorded" test -n "$(record_field idx39_function)"
	check "display: idx 39 offset recorded" test -n "$(record_field idx39_function_offset)"
	check "display: gpu-selection patch applied" contains \
		"$(cat "$FX/root/var/lib/geforce-sriov/build/$kernel/nvidia-drm/nvidia-drm-drv.c")" 'fixture: selected GPU IDs'
	check "display: allocation-threshold patch applied" contains \
		"$(cat "$FX/root/var/lib/geforce-sriov/build/$kernel/nvidia/os-interface.c")" 'KMALLOC_LIMIT 131072'
	check "display: uvm not built" test ! -e \
		"$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
	# A foreign change to a companion module is preserved by --restore.
	drm=$FX/root/lib/modules/$kdir/updates/dkms/nvidia-drm.ko
	cp "$drm" "$FX/objects/drm.installed"
	printf 'foreign\n' >"$drm"
	fx_exec unknown --restore
	check "display restore: foreign companion kept" contains "$out" "kept $drm"
	check "display restore: foreign companion untouched" test "$(cat "$drm")" = foreign
	check "display restore: owned modules removed" test ! -e \
		"$FX/root/lib/modules/$kdir/updates/dkms/nvidia-modeset.ko"
	check "display restore: core restored" test "$(cat "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")" != ''
}

# A display and host CUDA build made before the version policy had a profile for that combination:
# its record says unconfirmed. The rerun finds the build already installed and applies the current
# policy to the record. The fixture objects are synthetic, so the record is given the input digests
# and the digest admissions that a build from NVIDIA's files has.
case_reclassify() {
	fx_init "$FXROOT/reclassify" elf
	if ! fx_elf_files display; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "reclassify: the display and host CUDA build installs" test "$rc" -eq 0
	check "reclassify: the synthetic build is unconfirmed" test "$(record_field confirmation)" = unconfirmed
	for stage in scheduler module_gate vf_count plugin_mixed plugin_div4 display_idx39; do
		record_set "patch_${stage}_admission" digest
	done
	record_set host_sha256 none
	record_set second_sha256 8de9c10cfda77a89a8056acec476c13549f8f6dc7c7f9675da38dc9a73208a81
	record_set public_sha256 none
	record_set driver_public_version 580.159.03
	record_set confirmation unconfirmed
	record_set evidence_profile none
	record_set driver_version_confirmed 0
	before=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "reclassify: exit 0" test "$rc" -eq 0
	check "reclassify: already prepared, nothing is built" contains "$out" 'already prepared for'
	check "reclassify: the change is reported" contains "$out" \
		'the build record now reads confirmation=confirmed-inputs, evidence_profile=display-cuda-580.159.01-idx39; it read confirmation=unconfirmed, evidence_profile=none'
	check "reclassify: the record is confirmed for display with host CUDA" test \
		"$(record_field confirmation)/$(record_field evidence_profile)/$(record_field driver_version_confirmed)" = \
		confirmed-inputs/display-cuda-580.159.01-idx39/1
	check "reclassify: the other fields stay" test "$(record_field display)/$(record_field host_cuda)" = 1/1
	check "reclassify: the module is untouched" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko")" = "$before"
	# A revision that was admitted by its anchors keeps the build unconfirmed.
	record_set patch_display_idx39_admission anchor
	record_set confirmation unconfirmed
	record_set evidence_profile none
	record_set driver_version_confirmed 0
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "reclassify: an anchor admission stays unconfirmed" test \
		"$(record_field confirmation)/$(record_field evidence_profile)" = unconfirmed/none
	check "reclassify: and no change is reported" not_contains "$out" 'the build record now reads'
}

# A power loss at each point of the commit. The process is killed there, so nothing is rolled back
# by the run itself. The next run finishes the installation from the files the first run had built.
loss_run() {
	FX_FAULT=$1
	fx_exec unknown --display --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	FX_FAULT=
}
case_power_loss() {
	for point in after-core after-companion before-plugin after-plugin before-record; do
		fx_init "$FXROOT/loss-$point" elf
		if ! fx_elf_files display; then
			say 'SKIP: no C compiler or linker for the ELF fixtures'
			return 0
		fi
		rec=$FX/root/var/lib/geforce-sriov/driver-build
		mods=$FX/root/lib/modules/$kdir/updates/dkms
		loss_run "$point"
		check "power loss $point: the run is cut off" test "$rc" -ne 0
		check "power loss $point: the journal stays" test -f "$rec.gs-new"
		check "power loss $point: no record is published" test ! -e "$rec"
		loss_run ''
		check "power loss $point: the next run succeeds" test "$rc" -eq 0
		check "power loss $point: it completes the installation" contains "$out" 'Completing that installation'
		check "power loss $point: the journal is gone" test ! -e "$rec.gs-new"
		check "power loss $point: the staged plugin is gone" test ! -e "$FX/root/var/lib/geforce-sriov/plugin-copy.gs-new"
		for name in nvidia nvidia-modeset nvidia-drm; do
			flat=$(printf '%s' "$name" | tr '-' '_')
			check "power loss $point: $name is the module the record names" test \
				"$(sha256sum "$mods/$name.ko" | cut -d' ' -f1)" = "$(record_field "module_${flat}_sha256")"
		done
		pcopy=$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov
		check "power loss $point: the plugin copy is the one the record names" test \
			"$(sha256sum "$pcopy" | cut -d' ' -f1)" = "$(record_field plugin_sha256)"
		check "power loss $point: the plugin link points at the copy" test \
			"$(readlink "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so")" = "${pcopy##*/}"
		check "power loss $point: the published record has no journal line" test -z "$(sed -n '/^journal_/p' "$rec")"
	done

	# A staged module is gone as well: the commit cannot be finished, so what it replaced is put back.
	fx_init "$FXROOT/loss-undo" elf
	fx_elf_files display || return 0
	rec=$FX/root/var/lib/geforce-sriov/driver-build
	mods=$FX/root/lib/modules/$kdir/updates/dkms
	stock=$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)
	loss_run after-first-module
	check "power loss, undo: the core was replaced" test "$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)" != "$stock"
	rm -f "$FX/root/var/lib/geforce-sriov/build/$kernel"/*.ko
	fx_exec unknown --restore
	check "power loss, undo: the run succeeds" test "$rc" -eq 0
	check "power loss, undo: it says what it does" contains "$out" 'Putting back the files as they were before that run'
	check "power loss, undo: the stock core is back" test "$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)" = "$stock"
	check "power loss, undo: the journal is gone" test ! -e "$rec.gs-new"
	check "power loss, undo: no record is published" test ! -e "$rec"
	loss_run ''
	check "power loss, undo: a new install then succeeds" test "$rc" -eq 0

	# Neither the staged modules nor the copy of the replaced core is there: nothing is changed and the journal is kept.
	fx_init "$FXROOT/loss-stuck" elf
	fx_elf_files display || return 0
	rec=$FX/root/var/lib/geforce-sriov/driver-build
	mods=$FX/root/lib/modules/$kdir/updates/dkms
	loss_run after-first-module
	new=$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)
	rm -f "$FX/root/var/lib/geforce-sriov/build/$kernel"/*.ko "$FX/root/var/lib/geforce-sriov/txn/nvidia.ko"
	fx_exec unknown --restore
	check "power loss, stuck: refused" test "$rc" -eq 3
	check "power loss, stuck: it says why" contains "$out" 'can be neither completed nor undone'
	check "power loss, stuck: the journal is kept" test -f "$rec.gs-new"
	check "power loss, stuck: the core is untouched" test "$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)" = "$new"

	# An upgrade of an installed build, display only to display with host CUDA, cut off after its first
	# module, with the staged files gone. The files of the earlier build come back, not NVIDIA's, and
	# the earlier record, which was never touched, describes the installation again.
	fx_init "$FXROOT/loss-upgrade" elf
	fx_elf_files display || return 0
	rec=$FX/root/var/lib/geforce-sriov/driver-build
	mods=$FX/root/lib/modules/$kdir/updates/dkms
	pcopy=$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov
	loss_run ''
	check "power loss, upgrade: the display build installs" test "$rc" -eq 0
	before=$(cd "$mods" && sha256sum nvidia.ko nvidia-modeset.ko nvidia-drm.ko)
	before_rec=$(sha256sum "$rec" | cut -d' ' -f1)
	before_plugin=$(sha256sum "$pcopy" | cut -d' ' -f1)
	stock_modeset=$(sha256sum "$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia-modeset.ko" 2>/dev/null | cut -d' ' -f1)
	# The next build's companion modules differ from the installed ones, as a rebuild's do.
	printf 'rebuilt\n' >>"$FX/modules/nvidia-modeset.ko"
	printf 'rebuilt\n' >>"$FX/modules/nvidia-drm.ko"
	FX_FAULT='after-first-module'
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	FX_FAULT=
	check "power loss, upgrade: the run is cut off" test "$rc" -ne 0
	check "power loss, upgrade: a journal is left" test -f "$rec.gs-new"
	check "power loss, upgrade: a module of the new build is in place" test \
		"$(cd "$mods" && sha256sum nvidia.ko nvidia-modeset.ko nvidia-drm.ko)" != "$before"
	check "power loss, upgrade: the earlier record is still the record" test "$(sha256sum "$rec" | cut -d' ' -f1)" = "$before_rec"
	rm -f "$FX/root/var/lib/geforce-sriov/build/$kernel"/*.ko
	# The next run asks for the earlier build again: it undoes the upgrade and then finds that build installed.
	loss_run ''
	check "power loss, upgrade: the next run succeeds" test "$rc" -eq 0
	check "power loss, upgrade: it says what it does" contains "$out" 'Putting back the files as they were before that run'
	check "power loss, upgrade: the modules are those of the earlier build" test \
		"$(cd "$mods" && sha256sum nvidia.ko nvidia-modeset.ko nvidia-drm.ko)" = "$before"
	check "power loss, upgrade: the companion is not NVIDIA's stock file" test \
		"$(sha256sum "$mods/nvidia-modeset.ko" | cut -d' ' -f1)" != "$stock_modeset"
	check "power loss, upgrade: host CUDA's module is not left behind" test ! -e "$mods/nvidia-uvm.ko"
	check "power loss, upgrade: the record is the earlier record" test "$(sha256sum "$rec" | cut -d' ' -f1)" = "$before_rec"
	check "power loss, upgrade: the plugin copy is the earlier one" test "$(sha256sum "$pcopy" | cut -d' ' -f1)" = "$before_plugin"
	check "power loss, upgrade: the journal is gone" test ! -e "$rec.gs-new"
	check "power loss, upgrade: the copies are gone" test ! -e "$FX/root/var/lib/geforce-sriov/txn"

	# A module was changed by someone else after the interruption: it is not overwritten.
	fx_init "$FXROOT/loss-foreign" elf
	fx_elf_files display || return 0
	rec=$FX/root/var/lib/geforce-sriov/driver-build
	mods=$FX/root/lib/modules/$kdir/updates/dkms
	loss_run after-first-module
	printf 'foreign\n' >"$mods/nvidia-drm.ko"
	fx_exec unknown --restore
	check "power loss, foreign module: refused" test "$rc" -eq 3
	check "power loss, foreign module: it says the file was changed" contains "$out" 'so it was changed since'
	check "power loss, foreign module: the file is untouched" test "$(cat "$mods/nvidia-drm.ko")" = foreign
	check "power loss, foreign module: the journal is kept" test -f "$rec.gs-new"

	# The same for the plugin copy.
	fx_init "$FXROOT/loss-foreign-plugin" elf
	fx_elf_files display || return 0
	rec=$FX/root/var/lib/geforce-sriov/driver-build
	pcopy=$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov
	loss_run before-plugin
	printf 'foreign plugin\n' >"$pcopy"
	fx_exec unknown --restore
	check "power loss, foreign plugin: refused" test "$rc" -eq 3
	check "power loss, foreign plugin: it says the file was changed" contains "$out" 'so it was changed since'
	check "power loss, foreign plugin: the file is untouched" test "$(cat "$pcopy")" = 'foreign plugin'
	check "power loss, foreign plugin: the journal is kept" test -f "$rec.gs-new"
}

# --restore puts back a companion module that was there before the install, and then its backup is
# gone like the core's: nothing is left that a later message could offer to restore again.
case_restore_companion() {
	fx_init "$FXROOT/restore-companion" elf
	if ! fx_elf_files display; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	mods=$FX/root/lib/modules/$kdir/updates/dkms
	stock_core=$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)
	printf 'drm of the package\n' >"$mods/nvidia-drm.ko"
	fx_exec unknown --display --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "restore companion: installs" test "$rc" -eq 0
	check "restore companion: the earlier module is backed up" test -f "$FX/root/var/lib/geforce-sriov/stock/$kernel/nvidia-drm.ko"
	fx_exec unknown --restore
	check "restore companion: the restore succeeds" test "$rc" -eq 0
	check "restore companion: the earlier module is back" test "$(cat "$mods/nvidia-drm.ko")" = 'drm of the package'
	check "restore companion: the stock core is back" test "$(sha256sum "$mods/nvidia.ko" | cut -d' ' -f1)" = "$stock_core"
	check "restore companion: no backup is left" test ! -e "$FX/root/var/lib/geforce-sriov/stock"
	check "restore companion: no record is left" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
}

case_rollback() {
	fx_init "$FXROOT/rollback" elf
	if ! fx_elf_files display; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	cat >"$FX/bin/depmod" <<'EOF'
#!/bin/sh
exit 1
EOF
	chmod 0755 "$FX/bin/depmod"
	before=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown --host-cuda --companion-source "$FX/companion"
	check "rollback: the run fails" test "$rc" -ne 0
	after=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	check "rollback: the core is restored" test "$before" = "$after"
	check "rollback: no uvm left behind" test ! -e \
		"$FX/root/lib/modules/$kdir/updates/dkms/nvidia-uvm.ko"
	check "rollback: no record left behind" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
}

# --- a kernel update: the same build again, from the inputs the install kept ---

first_kernel=$kernel
next_kernel=6.2.0-gs-next
# The fixture host after a kernel update: the new kernel's headers and, as DKMS leaves them,
# NVIDIA's own modules for it. The fixture files are made again for the kernel named in $kernel.
fx_next_kernel() {
	kernel=$next_kernel
	kdir=$next_kernel
	mkdir -p "$FX/root/lib/modules/$kdir/build"
	cp "$FX/root/lib/modules/$first_kernel/build/Makefile" "$FX/root/lib/modules/$first_kernel/build/Module.symvers" \
		"$FX/root/lib/modules/$kdir/build/"
	if [ "$MODE" = elf ]; then
		fx_elf_files "$1"
	else
		fx_marker_files
	fi
}
fx_first_kernel() {
	kernel=$first_kernel
	kdir=$first_kernel
}
kept_dir() { printf '%s' "$FX/root/var/lib/geforce-sriov/inputs"; }
kept_field() { sed -n "s/^$1=//p" "$(kept_dir)/index"; }

case_kernel_update() {
	umask_before=$(umask)
	umask 022
	# Directories as inputs: a display and host CUDA build for the first kernel.
	fx_init "$FXROOT/update-trees" elf
	if ! fx_elf_files display; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "kernel update: the first build installs" test "$rc" -eq 0
	check "kernel update: the second package is kept" test -f "$(kept_dir)/companion/companion/kernel/Kbuild"
	check "kernel update: the public source is kept" test -f "$(kept_dir)/nvkms/nvkms/version.mk"
	check "kernel update: the host tree is kept as a copy" test \
		"$(kept_field nvidia_source)/$(kept_field nvidia_source_kind)/$(kept_field nvidia_source_origin)" = \
		"$(kept_dir)/nvidia_source/kernel/directory/$FX/src"
	check "kernel update: the copy holds the tree" test -f "$(kept_dir)/nvidia_source/kernel/nvidia/nv-kernel.o_binary"
	check "kernel update: the index names what was kept" test \
		"$(kept_field companion_kind)/$(kept_field companion_version)/$(kept_field nvkms_kind)/$(kept_field nvkms_version)" = \
		directory/580.159.03/directory/580.159.03
	check "kernel update: nothing kept is writable by others" test -z "$(find "$(kept_dir)" -perm /022 ! -type l)"
	first_record=$(cat "$FX/root/var/lib/geforce-sriov/driver-build")
	# install again without any input option, as `make install` runs it: the index is written
	# again for the host tree, and what it says about the kept inputs stays.
	index_before=$(sort "$(kept_dir)/index")
	fx_exec unknown --retained --source "$FX/src" --display --host-cuda
	check "install again without inputs: exit 0" test "$rc" -eq 0
	check "install again without inputs: nothing is built" contains "$out" 'already prepared for'
	check "install again without inputs: the index holds the same lines" test "$(sort "$(kept_dir)/index")" = "$index_before"
	check "install again without inputs: it still names both inputs" test \
		"$(kept_field companion)/$(kept_field companion_version)/$(kept_field nvkms)/$(kept_field nvkms_version)" = \
		"$(kept_dir)/companion/companion/580.159.03/$(kept_dir)/nvkms/nvkms/580.159.03"
	check "install again without inputs: the kept copies are there" test -f "$(kept_dir)/companion/companion/kernel/Kbuild" -a \
		-f "$(kept_dir)/nvkms/nvkms/version.mk"
	# An installation made before inputs were kept: a rerun with the inputs keeps them and builds nothing.
	rm -rf "$(kept_dir)"
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "kernel update, rerun: nothing is built" contains "$out" 'already prepared for'
	check "kernel update, rerun: the inputs are kept" contains "$out" "kept NVIDIA's second package for a later build"
	check "kernel update, rerun: the index is back" test "$(kept_field nvkms_version)" = 580.159.03
	# An input that is not the one the installed build was made from is not kept.
	rm -rf "$(kept_dir)"
	cp -a "$FX/nvkms" "$FX/nvkms-other"
	printf 'NVIDIA_VERSION = 580.159.04\n' >"$FX/nvkms-other/version.mk"
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms-other"
	check "kernel update, another source: it is not kept" contains "$out" \
		"was not kept: it is not NVIDIA's public kernel module source that the installed build was made from"
	check "kernel update, another source: the index does not name it" test -z "$(kept_field nvkms)"
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"

	# A host installed before inputs were kept, upgraded: the build is installed already, so a run
	# without any input needs none, changes nothing and does not refuse.
	rm -rf "$(kept_dir)"
	before=$(cd "$FX/root/lib/modules/$kdir/updates/dkms" && sha256sum nvidia.ko nvidia-modeset.ko nvidia-drm.ko nvidia-uvm.ko)
	fx_exec unknown --retained --source "$FX/src" --display --host-cuda
	check "upgrade without inputs: exit 0" test "$rc" -eq 0
	check "upgrade without inputs: nothing is built" contains "$out" 'already prepared for'
	check "upgrade without inputs: no refusal" not_contains "$out" 'refused:'
	check "upgrade without inputs: the modules are untouched" test \
		"$(cd "$FX/root/lib/modules/$kdir/updates/dkms" && sha256sum nvidia.ko nvidia-modeset.ko nvidia-drm.ko nvidia-uvm.ko)" = "$before"
	check "upgrade without inputs: the record is unchanged" test \
		"$(cat "$FX/root/var/lib/geforce-sriov/driver-build")" = "$first_record"
	check "upgrade without inputs: no input is claimed as kept" test -z "$(kept_field companion)$(kept_field nvkms)"
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"

	# The kernel update. The user's own copies are gone; no input is given.
	rm -rf "$FX/companion" "$FX/nvkms"
	fx_next_kernel display
	stock=$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	# A kept input that is gone refuses, says what to do, and changes nothing.
	mv "$(kept_dir)/nvkms" "$FX/nvkms.aside"
	fx_exec unknown --retained --display --host-cuda
	check "kernel update, kept source gone: refused" test "$rc" -eq 3
	check "kernel update, kept source gone: says so" contains "$out" \
		"NVIDIA's public kernel module source that an earlier install kept is missing: $(kept_dir)/nvkms/nvkms."
	check "kernel update, kept source gone: names the next step" contains "$out" \
		'Run `geforce-sriov install` in the source folder with --nvkms-source PATH; that keeps it again'
	check "kernel update, kept source gone: the record is the first kernel's" test \
		"$(cat "$FX/root/var/lib/geforce-sriov/driver-build")" = "$first_record"
	check "kernel update, kept source gone: the new kernel's module is untouched" test \
		"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$stock"
	mv "$FX/nvkms.aside" "$(kept_dir)/nvkms"
	# A kept tree that declares another version is not the one that was kept.
	printf '%s\n' '-DNV_VERSION_STRING=\"580.159.05\"' >"$(kept_dir)/companion/companion/kernel/Kbuild"
	fx_exec unknown --retained --display --host-cuda
	check "kernel update, kept package changed: refused" test "$rc" -eq 3
	check "kernel update, kept package changed: says so" contains "$out" \
		'declares version 580.159.05, and the one that was kept declared 580.159.03'
	printf '%s\n' '-DNV_VERSION_STRING=\"580.159.03\"' >"$(kept_dir)/companion/companion/kernel/Kbuild"
	# One command, no input.
	fx_exec unknown --retained --display --host-cuda
	check "kernel update: the build for the new kernel installs" test "$rc" -eq 0
	check "kernel update: the record names the new kernel" test "$(record_field kernel)" = "$next_kernel"
	check "kernel update: the same features" test "$(record_field display)/$(record_field host_cuda)" = 1/1
	check "kernel update: the same input versions" test \
		"$(record_field driver_version)/$(record_field driver_second_version)/$(record_field driver_public_version)" = \
		580.159.01/580.159.03/580.159.03
	for name in nvidia nvidia-modeset nvidia-drm nvidia-uvm; do
		check "kernel update: $name is installed for the new kernel" test \
			"$(sha256sum "$FX/root/lib/modules/$kdir/updates/dkms/$name.ko" | cut -d' ' -f1)" = \
			"$(record_field "module_$(printf '%s' "$name" | tr '-' '_')_sha256")"
	done
	check "kernel update: NVIDIA's module for the new kernel is backed up" test \
		"$(sha256sum "$FX/root/var/lib/geforce-sriov/stock/$kdir/nvidia.ko" | cut -d' ' -f1)" = "$stock"
	check "kernel update: the first kernel's backup stays" test -f "$FX/root/var/lib/geforce-sriov/stock/$first_kernel/nvidia.ko"
	check "kernel update: the kept inputs stay" test -f "$(kept_dir)/nvkms/nvkms/version.mk"
	fx_exec unknown --retained --display --host-cuda
	check "kernel update, rerun: nothing is built again" contains "$out" "already prepared for $next_kernel"
	fx_first_kernel

	# Files as inputs: the package and a source archive, each kept with its digest.
	fx_init "$FXROOT/update-files" elf
	fx_elf_files display || return 0
	cat >"$FX/second.run" <<'RUN'
#!/bin/sh
# Fixture package: --extract-only copies the prepared second package.
set -eu
printf '%s\n' "$0" >>"$FXDIR/ran-from"
[ "$1" = --extract-only ] || exit 2
mkdir second-package
cp -a "$FXDIR/companion/." second-package/
RUN
	chmod 0755 "$FX/second.run"
	tar -C "$FX" -cf "$FX/nvkms.tar" nvkms
	fx_exec unknown --display --host-cuda --companion-file "$FX/second.run" --nvkms-source "$FX/nvkms.tar"
	check "kernel update, files: the first build installs" test "$rc" -eq 0
	check "kernel update, files: only a copy of the package was run" test -s "$FX/ran-from" -a \
		-z "$(grep -v "^$FX/tmp/geforce-sriov-driver\.[A-Za-z0-9]*/in/companion/second\.run\$" "$FX/ran-from")"
	second_sha=$(record_field second_sha256)
	check "kernel update, files: the package is kept with its digest" test \
		"$(kept_field companion_kind)/$(kept_field companion_sha256)" = "file/$(sha256sum "$FX/second.run" | cut -d' ' -f1)"
	check "kernel update, files: the record holds the same digest" test "$second_sha" = "$(kept_field companion_sha256)"
	check "kernel update, files: the archive is kept with its digest" test \
		"$(kept_field nvkms_kind)/$(kept_field nvkms_sha256)" = "file/$(sha256sum "$FX/nvkms.tar" | cut -d' ' -f1)"
	first_record=$(cat "$FX/root/var/lib/geforce-sriov/driver-build")
	fx_next_kernel display
	rm -f "$FX/second.run" "$FX/nvkms.tar"
	# A kept file that was changed is not run and not used.
	cp -p "$(kept_dir)/companion/second.run" "$FX/second.aside"
	printf '\n: >"%s/ran"\n' "$FX" >>"$(kept_dir)/companion/second.run"
	fx_exec unknown --retained --display --host-cuda
	check "kernel update, kept file changed: refused" test "$rc" -eq 3
	check "kernel update, kept file changed: says so" contains "$out" \
		"NVIDIA's second package that an earlier install kept was changed since: $(kept_dir)/companion/second.run has SHA-256"
	check "kernel update, kept file changed: names the digest that was kept" contains "$out" \
		"and the file that was kept had $second_sha"
	check "kernel update, kept file changed: names the next step" contains "$out" \
		'Run `geforce-sriov install` in the source folder with --companion-file PATH (or --companion-source DIR)'
	check "kernel update, kept file changed: it was not run" test ! -e "$FX/ran"
	check "kernel update, kept file changed: the record is the first kernel's" test \
		"$(cat "$FX/root/var/lib/geforce-sriov/driver-build")" = "$first_record"
	cp -p "$FX/second.aside" "$(kept_dir)/companion/second.run"
	fx_exec unknown --retained --display --host-cuda
	check "kernel update, files: the build for the new kernel installs" test "$rc" -eq 0
	check "kernel update, files: the record names the new kernel" test "$(record_field kernel)" = "$next_kernel"
	check "kernel update, files: the same package digest and classification" test \
		"$(record_field second_sha256)/$(record_field confirmation)/$(record_field evidence_profile)" = \
		"$second_sha/$(printf '%s\n' "$first_record" | sed -n 's/^confirmation=//p')/$(printf '%s\n' "$first_record" | sed -n 's/^evidence_profile=//p')"
	fx_first_kernel

	# Nothing kept: a feature build without its inputs says what is missing.
	fx_init "$FXROOT/update-none" elf
	fx_elf_files display || return 0
	fx_exec unknown --retained --source "$FX/src" --display
	check "kernel update, nothing kept: refused" test "$rc" -eq 3
	check "kernel update, nothing kept: says so" contains "$out" \
		"this build needs NVIDIA's second package, and no earlier install kept one"
	check "kernel update, nothing kept: no record" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"

	# The plain build keeps a copy of the host tree and needs nothing else.
	fx_init "$FXROOT/update-plain" marker
	fx_marker_files
	fx_exec known
	check "kernel update, plain: the first build installs" test "$rc" -eq 0
	check "kernel update, plain: the host tree is kept as a copy" test \
		"$(kept_field nvidia_source)" = "$(kept_dir)/nvidia_source/kernel"
	# The confirmed build, installed, with nothing kept, as install runs the step: nothing changed.
	rm -rf "$(kept_dir)"
	fx_exec known --retained --source "$FX/src"
	check "upgrade without inputs, plain: exit 0" test "$rc" -eq 0
	check "upgrade without inputs, plain: nothing changed" contains "$out" 'install_result=unchanged'
	# A new kernel for which DKMS has not built NVIDIA's module: the refusal says how to build it.
	kernel=6.3.0-gs-unbuilt
	mkdir -p "$FX/root/lib/modules/$kernel/build"
	cp "$FX/root/lib/modules/$first_kernel/build/Makefile" "$FX/root/lib/modules/$kernel/build/"
	fx_exec known --retained
	check "kernel update, no module yet: refused" test "$rc" -eq 3
	check "kernel update, no module yet: says so" contains "$out" \
		"NVIDIA's own module for 6.3.0-gs-unbuilt is not built yet"
	check "kernel update, no module yet: names the commands" contains "$out" \
		'build NVIDIA'"'"'s module with `dkms install nvidia/580.159.01 -k 6.3.0-gs-unbuilt` (or `dkms autoinstall -k 6.3.0-gs-unbuilt`), then run `sudo geforce-sriov rebuild --kernel 6.3.0-gs-unbuilt`'
	check "kernel update, no module yet: the record is the first kernel's" test "$(record_field kernel)" = "$first_kernel"
	kernel=$first_kernel
	fx_next_kernel plain
	fx_exec known --retained
	check "kernel update, plain: the build for the new kernel installs" test "$rc" -eq 0
	check "kernel update, plain: the record names the new kernel" test \
		"$(record_field kernel)/$(record_field confirmation)/$(record_field evidence_profile)" = \
		"$next_kernel/confirmed-inputs/host-580.159.01-plain"
	fx_first_kernel

	# Root builds from its own copy of a tree, never from a place. A tree that others can write to
	# is copied, and nobody else can write to the copy. A link that stays inside the tree is kept.
	fx_init "$FXROOT/update-copy" marker
	fx_marker_files
	chmod go+w "$FX/src/Kbuild"
	ln -s Kbuild "$FX/src/inside-link"
	fx_exec known
	check "kept tree: the first build installs" test "$rc" -eq 0
	check "kept tree: says so" contains "$out" "kept NVIDIA's kernel tree for a later build: $(kept_dir)/nvidia_source/kernel"
	check "kept tree: nobody else can write to the copy" test -z "$(find "$(kept_dir)/nvidia_source" -perm /022 ! -type l)"
	check "kept tree: a link inside the tree is kept" test "$(readlink "$(kept_dir)/nvidia_source/kernel/inside-link")" = Kbuild
	# The place belongs to someone else afterwards, who puts a link to another tree there. The
	# build for the next kernel is made from the copy, so it is the confirmed build again.
	fx_next_kernel plain
	mkdir "$FX/other"
	cp -a "$FX/src/." "$FX/other/"
	printf 'another-object\n' >"$FX/other/nvidia/nv-kernel.o_binary"
	mv "$FX/src" "$FX/src.real"
	ln -s "$FX/other" "$FX/src"
	fx_exec known --retained
	check "place replaced by a link: the build for the new kernel installs" test "$rc" -eq 0
	check "place replaced by a link: it is built from the copy" test \
		"$(record_field kernel)/$(record_field confirmation)" = "$next_kernel/confirmed-inputs"
	check "place replaced by a link: the other tree is not read" not_contains "$out" "$FX/other"
	rm "$FX/src"
	mv "$FX/src.real" "$FX/src"
	fx_first_kernel

	# An index of an earlier version names the place of the tree. A place is not built from: not
	# one that is a link now, and not one below a directory that another user owns.
	fx_init "$FXROOT/update-place" marker
	fx_marker_files
	fx_exec known
	check "kept by place: the first build installs" test "$rc" -eq 0
	record_before=$(cat "$FX/root/var/lib/geforce-sriov/driver-build")
	mv "$FX/src" "$FX/src.real"
	ln -s "$FX/src.real" "$FX/src"
	rm -rf "$(kept_dir)/nvidia_source"
	printf 'nvidia_source=%s\n' "$FX/src" >"$(kept_dir)/index"
	fx_next_kernel plain
	fx_exec known --retained
	check "kept by place, a link now: refused, with no tree of its own to build from" test "$rc" -eq 3
	check "kept by place, a link now: names where it looked" contains "$out" \
		"source tree missing: $FX/root/usr/src/nvidia-580.159.01"
	check "kept by place, a link now: the place is not used" contains "$out" \
		"an earlier install kept only the place of the kernel tree it used, $FX/src. A place is not built from again without being named"
	check "kept by place, a link now: it names the option" contains "$out" '`geforce-sriov install --nvidia-source DIR` names a tree and keeps a copy of it'
	check "kept by place, a link now: nothing is built from it" not_contains "$out" "'$FX/src"
	check "kept by place, a link now: the record is as it was" test \
		"$(cat "$FX/root/var/lib/geforce-sriov/driver-build")" = "$record_before"
	rm "$FX/src"
	mkdir -p "$FX/home/user"
	chmod 0777 "$FX/home/user"
	mv "$FX/src.real" "$FX/home/user/kernel"
	printf 'nvidia_source=%s\n' "$FX/home/user/kernel" >"$(kept_dir)/index"
	fx_exec known --retained
	check "kept by place, below a directory of another user: the place is not used" contains "$out" \
		"an earlier install kept only the place of the kernel tree it used, $FX/home/user/kernel."
	check "kept by place, below a directory of another user: nothing is built from it" not_contains "$out" "'$FX/home/user/kernel"
	check "kept by place, below a directory of another user: the record is as it was" test \
		"$(cat "$FX/root/var/lib/geforce-sriov/driver-build")" = "$record_before"
	# Naming the tree is the way out: install copies it and keeps the copy.
	fx_first_kernel
	mv "$FX/home/user/kernel" "$FX/src"
	fx_exec known
	check "kept by place, named again: the copy is kept" test \
		"$(kept_field nvidia_source)/$(kept_field nvidia_source_kind)" = "$(kept_dir)/nvidia_source/kernel/directory"

	# A link that leads out of a tree makes the bytes behind it someone else's: such a tree is
	# not built from, whether it is given or was kept by an earlier version.
	fx_init "$FXROOT/update-link" marker
	fx_marker_files
	printf 'int x;\n' >"$FX/outside.c"
	ln -s "$FX/outside.c" "$FX/src/nvidia/outside.c"
	fx_exec known
	check "given tree with a link out: refused" test "$rc" -eq 3
	check "given tree with a link out: names the link" contains "$out" \
		"$FX/src/nvidia/outside.c is a symbolic link to $FX/outside.c, which is outside $FX/src"
	check "given tree with a link out: says what happens" contains "$out" \
		"$FX/src is not built from; the line above says why. NVIDIA's files are as they were"
	check "given tree with a link out: nothing installed" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
	rm "$FX/src/nvidia/outside.c"
	fx_exec known
	check "link removed: the build installs" test "$rc" -eq 0
	ln -s ../../../../../../outside.c "$(kept_dir)/nvidia_source/kernel/nvidia/outside.c"
	fx_next_kernel plain
	fx_exec known --retained
	check "kept tree with a link out: refused" test "$rc" -eq 3
	check "kept tree with a link out: names the link" contains "$out" \
		"$(kept_dir)/nvidia_source/kernel/nvidia/outside.c is a symbolic link to ../../../../../../outside.c, which is outside $(kept_dir)/nvidia_source/kernel"
	check "kept tree with a link out: says how to keep it again" contains "$out" \
		'the kernel tree that an earlier install kept is not built from; the line above says why. NVIDIA'"'"'s files are as they were. Run `geforce-sriov install --nvidia-source DIR`; that keeps the tree again'
	check "kept tree with a link out: no build for the new kernel" test "$(record_field kernel)" = "$first_kernel"
	fx_first_kernel

	# A file with the set-user-ID bit would become root's set-user-ID file in a copy that root makes,
	# in a directory everyone can reach. The copies carry no such bit, and hold nothing but regular
	# files, directories and links.
	fx_init "$FXROOT/update-special" marker
	fx_marker_files
	chmod u+s "$FX/src/Kbuild"
	mkdir "$FX/src/group"
	chmod g+s "$FX/src/group"
	fx_exec known
	check "set-user-ID in the tree: the build installs" test "$rc" -eq 0
	check "set-user-ID in the tree: the copy is kept" test -f "$(kept_dir)/nvidia_source/kernel/Kbuild"
	check "set-user-ID in the tree: no copy carries the bits" test -z "$(find "$FX/root/var/lib/geforce-sriov" -perm /6000)"
	# A tree that an earlier build kept with such a file is not built from.
	chmod u+s "$(kept_dir)/nvidia_source/kernel/Kbuild"
	fx_next_kernel plain
	fx_exec known --retained
	check "kept tree with a set-user-ID file: refused" test "$rc" -eq 3
	check "kept tree with a set-user-ID file: names it" contains "$out" \
		"$(kept_dir)/nvidia_source/kernel/Kbuild has the set-user-ID or set-group-ID bit. A tree that is built as root holds no such file"
	check "kept tree with a set-user-ID file: no build for the new kernel" test "$(record_field kernel)" = "$first_kernel"
	chmod u-s "$(kept_dir)/nvidia_source/kernel/Kbuild"
	mkfifo "$FX/src/nvidia/pipe"
	fx_exec known
	check "FIFO in the tree: refused" test "$rc" -eq 3
	check "FIFO in the tree: names it" contains "$out" \
		"$FX/src/nvidia/pipe is not a regular file, a directory or a symbolic link. A tree that is built as root holds nothing else: remove it"
	check "FIFO in the tree: no build for the new kernel" test "$(record_field kernel)" = "$first_kernel"
	check "FIFO in the tree: nothing left in the scratch directory" test -z "$(ls -A "$FX/tmp")"
	rm "$FX/src/nvidia/pipe"
	fx_first_kernel

	# What is kept is what the installed build was made from: the record holds the digest of each
	# input, and a run that builds nothing keeps a tree only when its copy has that digest.
	fx_init "$FXROOT/update-bound" marker
	fx_marker_files
	fx_exec known
	check "bound to the build: the first build installs" test "$rc" -eq 0
	check "bound to the build: the record holds the digest of the tree" test "$(record_field input_source_sha256 | wc -c)" -eq 65
	rm -rf "$(kept_dir)"
	printf 'int extra;\n' >"$FX/src/nvidia/extra.c"
	fx_exec known
	check "tree changed since the build: nothing is built" contains "$out" 'already prepared for'
	check "tree changed since the build: it is not kept" contains "$out" \
		"$FX/src was not kept: it is not NVIDIA's kernel tree that the installed build was made from"
	check "tree changed since the build: no copy, no index line" test ! -e "$(kept_dir)/nvidia_source" -a -z "$(kept_field nvidia_source 2>/dev/null)"
	rm "$FX/src/nvidia/extra.c"
	fx_exec known
	check "tree as it was built: it is kept" test "$(kept_field nvidia_source)" = "$(kept_dir)/nvidia_source/kernel"
	# A record of an earlier build holds no such digest: nothing is kept until the next build.
	rm -rf "$(kept_dir)"
	sed -i '/^input_/d' "$FX/root/var/lib/geforce-sriov/driver-build"
	fx_exec known
	check "record without the digest: says so" contains "$out" \
		"$FX/src was not kept: the record of the installed build does not hold the digest of NVIDIA's kernel tree it was made from. The next build keeps it"
	check "record without the digest: no copy" test ! -e "$(kept_dir)/nvidia_source"

	# A package is a script that root runs. The file that is checked is the file that is run: a
	# copy in this run's own directory, never the file at the place that was given.
	fx_init "$FXROOT/update-run" marker
	fx_marker_files
	sed -i 's|^set -eu$|set -eu; printf "%s\\n" "$0" >>"$FXDIR/ran"|' "$FX/host.run"
	# The caller's umask allows everything: what the step writes is still root's alone to write.
	umask 000
	fx_exec known --driver-file "$FX/host.run"
	umask 022
	check "host package: the build installs" test "$rc" -eq 0
	check "lax umask: nobody else can write to anything in the state directory" test -z \
		"$(find "$FX/root/var/lib/geforce-sriov" -perm /022 ! -type l)"
	check "host package: it was run" test -s "$FX/ran"
	check "host package: only the copy was run" test -z "$(grep -v "^$FX/tmp/geforce-sriov-driver\.[A-Za-z0-9]*/package/host\.run\$" "$FX/ran")"
	check "host package: the copy is gone afterwards" test -z "$(find "$FX/tmp" -name host.run)"
	# Too little room for a copy: the run refuses before its first change and leaves nothing behind.
	fx_init "$FXROOT/update-space" marker
	fx_marker_files
	cat >"$FX/bin/df" <<'STUB'
#!/bin/sh
# Free space as the fixture says: FXDIR/df-free for every place, FXDIR/df-free-inputs for the kept inputs.
for last do :; done
free=
[ ! -f "$FXDIR/df-free" ] || free=$(cat "$FXDIR/df-free")
case $last in */inputs) [ ! -f "$FXDIR/df-free-inputs" ] || free=$(cat "$FXDIR/df-free-inputs") ;; esac
[ -n "$free" ] || exec /usr/bin/df "$@"
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\nfixture 1000000 0 %s 0%% /\n' "$free"
STUB
	chmod 0755 "$FX/bin/df"
	printf '0\n' >"$FX/df-free"
	fx_exec known
	check "no room for the copy of the tree: refused" test "$rc" -eq 3
	check "no room for the copy of the tree: says how much" contains "$out" \
		"not enough space in $FX/tmp: 1 MB needed for a copy of $FX/src, 0 MB free. NVIDIA's files are as they were. Free some space, then run the same command again"
	check "no room for the copy of the tree: nothing installed" test ! -e "$FX/root/var/lib/geforce-sriov/driver-build"
	check "no room for the copy of the tree: nothing left in the scratch directory" test -z "$(ls -A "$FX/tmp")"
	fx_exec known --driver-file "$FX/host.run"
	check "no room for the copy of the package: refused" test "$rc" -eq 3
	check "no room for the copy of the package: says so" contains "$out" \
		"not enough space in $FX/tmp: 1 MB needed for a copy of $FX/host.run, 0 MB free"
	check "no room for the copy of the package: nothing left in the scratch directory" test -z "$(ls -A "$FX/tmp")"
	# Room for the build and none for what is kept: the build installs, and the run says what was not kept.
	rm "$FX/df-free"
	printf '0\n' >"$FX/df-free-inputs"
	fx_exec known
	check "no room for the kept copy: the build installs" test "$rc" -eq 0
	check "no room for the kept copy: says how much" contains "$out" \
		"not enough space in $(kept_dir): 1 MB needed for a copy of"
	check "no room for the kept copy: says what follows" contains "$out" \
		"warning: $FX/src could not be kept under $(kept_dir); a build for another kernel then needs it passed to install again"
	check "no room for the kept copy: nothing partial is left" test -z "$(find "$(kept_dir)" -name '*.gs-new')" -a \
		-z "$(kept_field nvidia_source)"
	rm "$FX/df-free-inputs"
	fx_exec known
	check "room again: the tree is kept" test "$(kept_field nvidia_source)" = "$(kept_dir)/nvidia_source/kernel"

	# A path with a line break would add a line to the index.
	fx_exec known --source "$FX/src
companion=/tmp/x"
	check "path with a line break: usage error" test "$rc" -eq 2
	check "path with a line break: says so" contains "$out" 'a path given to this script holds a control character'
	fx_first_kernel
	umask "$umask_before"
}

# Builds for two kernels on one host: the build for the old kernel, a build for the new one, and
# the old kernel again. Each kernel keeps its own record and the backup of NVIDIA's own modules;
# a module this script built is never taken for NVIDIA's.
modules_of() { (cd "$FX/root/lib/modules/$1/updates/dkms" && sha256sum nvidia.ko nvidia-modeset.ko nvidia-drm.ko nvidia-uvm.ko); }
stock_of() { sha256sum "$FX/root/var/lib/geforce-sriov/stock/$1/nvidia.ko" | cut -d' ' -f1; }
case_two_kernels() {
	umask 022
	fx_init "$FXROOT/two-kernels" elf
	if ! fx_elf_files display; then
		say 'SKIP: no C compiler or linker for the ELF fixtures'
		return 0
	fi
	state=$FX/root/var/lib/geforce-sriov
	dkms_first=$(sha256sum "$FX/root/lib/modules/$first_kernel/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "two kernels: the first build installs" test "$rc" -eq 0
	first_record=$(cat "$state/driver-build")
	first_modules=$(modules_of "$first_kernel")
	check "two kernels: the first backup is NVIDIA's module" test "$(stock_of "$first_kernel")" = "$dkms_first"

	# The kernel update: DKMS builds NVIDIA's module for the new kernel, then rebuild --kernel.
	fx_next_kernel display
	dkms_next=$(sha256sum "$FX/root/lib/modules/$next_kernel/updates/dkms/nvidia.ko" | cut -d' ' -f1)
	fx_exec unknown --retained --display --host-cuda
	check "two kernels: the build for the new kernel installs" test "$rc" -eq 0
	check "two kernels: the published record names the new kernel" test "$(record_field kernel)" = "$next_kernel"
	next_record=$(cat "$state/driver-build")
	next_modules=$(modules_of "$next_kernel")
	check "two kernels: the new kernel's backup is NVIDIA's module" test "$(stock_of "$next_kernel")" = "$dkms_next"
	check "two kernels: the first kernel's record is kept" test \
		"$(cat "$state/driver-builds/$first_kernel")" = "$first_record"
	check "two kernels: the first kernel's modules are untouched" test "$(modules_of "$first_kernel")" = "$first_modules"

	# A third kernel with no headers: the run refuses, says what to do for that kernel, and leaves
	# the state directory exactly as it was, the records and their places included.
	state_listing() { (cd "$state" && find . ! -name driver-step.log | sort && find . -type f ! -name driver-step.log -exec sha256sum {} + | sort); }
	listing_before=$(state_listing)
	kernel=6.3.0-gs-noheaders
	fx_exec unknown --retained --display --host-cuda
	check "no headers: refused" test "$rc" -eq 3
	check "no headers: says what is missing" contains "$out" \
		"the headers of kernel 6.3.0-gs-noheaders are not installed: $FX/root/lib/modules/6.3.0-gs-noheaders/build/Makefile is missing"
	check "no headers: names the commands for that kernel" contains "$out" \
		'build it with `dkms install nvidia/580.159.01 -k 6.3.0-gs-noheaders`. Then run `sudo geforce-sriov rebuild --kernel 6.3.0-gs-noheaders`'
	check "no headers: the state directory is as it was" test "$(state_listing)" = "$listing_before"
	kernel=$next_kernel

	# The old kernel again. Its build is installed: nothing is built, and no backup is taken.
	# The fixture's built modules are the new kernel's, so a build here would not pass the ABI gate.
	fx_first_kernel
	fx_exec unknown --retained --display --host-cuda
	check "two kernels, back on the first: exit 0" test "$rc" -eq 0
	check "two kernels, back on the first: nothing is built" contains "$out" "already prepared for $first_kernel"
	check "two kernels, back on the first: no build" not_contains "$out" "'make'"
	check "two kernels, back on the first: its record is read where it is kept" test \
		"$(cat "$state/driver-builds/$first_kernel")" = "$first_record"
	check "two kernels, back on the first: the published record is still the latest build's" test \
		"$(cat "$state/driver-build")" = "$next_record"
	check "two kernels, back on the first: the modules are untouched" test \
		"$(modules_of "$first_kernel")/$(modules_of "$next_kernel")" = "$first_modules/$next_modules"
	check "two kernels, back on the first: its backup is still NVIDIA's module" test "$(stock_of "$first_kernel")" = "$dkms_first"
	check "two kernels, back on the first: the other backup is still NVIDIA's module" test "$(stock_of "$next_kernel")" = "$dkms_next"
	check "two kernels: one record per kernel, no more" test \
		"$(ls "$state/driver-builds")" = "$first_kernel"
	# A copy under the published record's own kernel name is stale: the published record decides.
	sed 's/^module_sha256=.*/module_sha256=stale/' "$state/driver-build" >"$state/driver-builds/$next_kernel"
	kernel=$next_kernel
	kdir=$next_kernel
	fx_exec unknown --retained --display --host-cuda
	check "stale copy: the published record decides" contains "$out" "already prepared for $next_kernel"
	check "stale copy: the modules are untouched" test "$(modules_of "$next_kernel")" = "$next_modules"
	rm -f "$state/driver-builds/$next_kernel"
	fx_first_kernel

	# The first kernel again, where its record no longer describes what is asked for: the record is
	# the one kept under its name, the build is made and published, the other kernel's record takes
	# its place under driver-builds, and the inputs the run was given are kept, as after any build.
	cp -a "$state" "$FX/state.before-change"
	cp -a "$FX/root/lib/modules" "$FX/modules.before-change"
	kernel=$first_kernel
	kdir=$first_kernel
	fx_elf_files display
	cp -a "$FX/modules.before-change/$first_kernel/updates/dkms/." "$FX/root/lib/modules/$first_kernel/updates/dkms/"
	sed 's/^host_cuda=1$/host_cuda=0/' "$FX/state.before-change/driver-builds/$first_kernel" >"$state/driver-builds/$first_kernel"
	rm -rf "$(kept_dir)"
	fx_exec unknown --display --host-cuda --companion-source "$FX/companion" --nvkms-source "$FX/nvkms"
	check "kept record no longer fits: the build installs" test "$rc" -eq 0
	check "kept record no longer fits: its record is published" test \
		"$(record_field kernel)/$(record_field host_cuda)" = "$first_kernel/1"
	check "kept record no longer fits: the other kernel's record is kept" test \
		"$(cat "$state/driver-builds/$next_kernel")" = "$next_record"
	check "kept record no longer fits: the old copy is gone" test ! -e "$state/driver-builds/$first_kernel"
	check "kept record no longer fits: the inputs are kept" test "$(kept_field companion_version)/$(kept_field nvkms_version)" = 580.159.03/580.159.03
	rm -rf "$state" "$FX/root/lib/modules"
	cp -a "$FX/state.before-change" "$state"
	cp -a "$FX/modules.before-change" "$FX/root/lib/modules"

	# A host where the first kernel's record was lost, as a build for another kernel left it before
	# records were kept per kernel. The installed module is recognised as this script's build by the
	# build kept for the kernel, and the backup of NVIDIA's module stays.
	cp -a "$state" "$FX/state.aside"
	cp -a "$FX/root/lib/modules" "$FX/modules.aside"
	rm -f "$state/driver-builds/$first_kernel"
	kernel=$first_kernel
	kdir=$first_kernel
	fx_elf_files display
	# fx_elf_files put NVIDIA's module back; the host under test has this script's build installed.
	cp -a "$FX/modules.aside/$first_kernel/updates/dkms/." "$FX/root/lib/modules/$first_kernel/updates/dkms/"
	fx_exec unknown --retained --display --host-cuda
	check "lost record: the build installs" test "$rc" -eq 0
	check "lost record: it is built" contains "$out" "'make'"
	check "lost record: the backup is still NVIDIA's module" test "$(stock_of "$first_kernel")" = "$dkms_first"
	check "lost record: the record says so" test "$(record_field stock_sha256)/$(record_field module_nvidia_previous)" = "$dkms_first/present"
	check "lost record: the published record is this kernel's again" test "$(record_field kernel)" = "$first_kernel"
	check "lost record: a module this script added has no backup" test \
		"$(record_field module_nvidia_uvm_previous)/$(record_field module_nvidia_uvm_backup_sha256)" = absent/none
	# And with no backup at all there is nothing to put back: refused before any change.
	rm -f "$state/driver-build" "$state/stock/$first_kernel/nvidia.ko"
	before=$(modules_of "$first_kernel")
	fx_exec unknown --retained --display --host-cuda
	check "lost record and backup: refused" test "$rc" -eq 3
	check "lost record and backup: says so" contains "$out" \
		"nvidia.ko is a module geforce-sriov built for $first_kernel earlier, and $state/stock/$first_kernel holds no backup of NVIDIA's own module"
	check "lost record and backup: names the command" contains "$out" \
		"dkms install --force nvidia/580.159.01 -k $first_kernel"
	check "lost record and backup: nothing is installed" test "$(modules_of "$first_kernel")" = "$before"
	check "lost record and backup: no backup is made" test ! -e "$state/stock/$first_kernel/nvidia.ko"
	rm -rf "$state" "$FX/root/lib/modules"
	cp -a "$FX/state.aside" "$state"
	cp -a "$FX/modules.aside" "$FX/root/lib/modules"

	# uninstall runs --restore without --kernel: NVIDIA's own modules come back for both kernels.
	FX_EVERY_KERNEL=1
	fx_exec unknown --restore
	FX_EVERY_KERNEL=
	check "two kernels, restore: exit 0" test "$rc" -eq 0
	for k in "$first_kernel" "$next_kernel"; do
		want=$dkms_first
		[ "$k" = "$first_kernel" ] || want=$dkms_next
		check "two kernels, restore: NVIDIA's module is back for $k" test \
			"$(sha256sum "$FX/root/lib/modules/$k/updates/dkms/nvidia.ko" | cut -d' ' -f1)" = "$want"
		for name in nvidia-modeset nvidia-drm nvidia-uvm; do
			check "two kernels, restore: $name is gone for $k" test ! -e "$FX/root/lib/modules/$k/updates/dkms/$name.ko"
		done
	done
	check "two kernels, restore: no record is left" test ! -e "$state/driver-build" -a ! -e "$state/driver-builds"
	check "two kernels, restore: no backup is left" test ! -e "$state/stock"
	check "two kernels, restore: the plugin link is back at NVIDIA's plugin" test \
		"$(readlink "$FX/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so")" = libnvidia-vgpu.so.580.159.01
	fx_first_kernel
}

FXROOT=$(mktemp -d "${TMPDIR:-/tmp}/gs-driver-fixtures.XXXXXX")
trap 'rm -rf "$FXROOT"' EXIT HUP INT TERM
case_plain
case_owner
case_unknown_anchors
case_identity_refused
case_unknown_refused
case_package_digest
case_second_version
case_interrupted_commit
case_cuda
case_cuda_libs
case_after_display
case_cuda_crc_and_provider
case_plain_gate
case_plain_other_version
case_display
case_reclassify
case_power_loss
case_restore_companion
case_rollback
case_kernel_update
case_two_kernels
say "driver fixtures: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
