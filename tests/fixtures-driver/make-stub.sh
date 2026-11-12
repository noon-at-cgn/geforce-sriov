#!/bin/sh
# SPDX-License-Identifier: MIT
# Fixture make: the NVKMS core build in the public source tree produces the
# recorded object; the staged-tree build copies the prepared modules. It never
# invokes a compiler or the kernel build system.
set -eu
fx=${FXDIR:?}
dir=.
modules=
while [ "$#" -gt 0 ]; do
	case $1 in
		-C) dir=$2; shift 2 ;;
		NV_KERNEL_MODULES=*) modules=${1#NV_KERNEL_MODULES=}; shift ;;
		*) shift ;;
	esac
done
case $dir in
	*/src/nvidia-modeset)
		mkdir -p "$dir/_out/Linux_$(uname -m)"
		cp "$fx/modules/nv-modeset-kernel.o" "$dir/_out/Linux_$(uname -m)/nv-modeset-kernel.o"
		exit 0
		;;
esac
[ -n "$modules" ] || modules='nvidia nvidia-vgpu-vfio'
# The second build of a run, with the owner of the framebuffer slots in the tree: its own module,
# when the fixture has one, and whatever else the fixture says that build leaves behind.
owner=0
if [ -f "$dir/nvidia/gs-fb-owner.c" ]; then
	owner=1
	if [ -f "$fx/owner-build-fails" ]; then
		# A build that stops half way leaves what it had begun to write.
		for half in "$dir"/*.ko; do
			[ ! -f "$half" ] || printf 'half built\n' >"$half"
		done
		printf 'half written\n' >"$dir/Module.symvers"
		printf 'make-stub: gs-fb-owner.c does not compile\n' >&2
		exit 2
	fi
	[ ! -f "$fx/owner-extra-object" ] || printf 'changed\n' >"$dir/nvidia/os-interface.o"
fi
for name in $modules; do
	if [ "$name" = nvidia ] && [ "$owner" -eq 1 ] && [ -f "$fx/modules/nvidia-owner.ko" ]; then
		cp "$fx/modules/nvidia-owner.ko" "$dir/nvidia.ko"
		continue
	fi
	[ -f "$fx/modules/$name.ko" ] || {
		printf 'make-stub: no module %s\n' "$name" >&2
		exit 1
	}
	cp "$fx/modules/$name.ko" "$dir/$name.ko"
done
