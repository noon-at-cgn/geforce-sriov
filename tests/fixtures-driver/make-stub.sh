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
for name in $modules; do
	[ -f "$fx/modules/$name.ko" ] || {
		printf 'make-stub: no module %s\n' "$name" >&2
		exit 1
	}
	cp "$fx/modules/$name.ko" "$dir/$name.ko"
done
