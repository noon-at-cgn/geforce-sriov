#!/bin/sh
# SPDX-License-Identifier: MIT
# Smoke tests of the kernel-install plugin (Fedora) and the pacman hook (Arch) in Docker containers.
#   tests/distro/smoke.sh arch|fedora|all [LOGDIR]
# They need Docker and, for the container's own packages (systemd-udev on Fedora), network. They are not part of `make check`.
# Logs go to LOGDIR (default: tests/distro/logs).
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$here/../..
which=${1:-all}
logdir=${2:-$here/logs}
mkdir -p "$logdir"
status=0
run() { # $1 name, $2 image
	printf '== %s (%s)\n' "$1" "$2"
	if docker run --rm -v "$root:/src:ro" "$2" sh "/src/tests/distro/$1-inner.sh" >"$logdir/$1.log" 2>&1; then
		tail -n 1 "$logdir/$1.log"
	else
		grep -E '^FAIL|distro smoke' "$logdir/$1.log" || tail -n 20 "$logdir/$1.log"
		status=1
	fi
}
case $which in
arch) run arch "${ARCH_IMAGE:-archlinux:latest}" ;;
fedora) run fedora "${FEDORA_IMAGE:-fedora:latest}" ;;
all)
	run arch "${ARCH_IMAGE:-archlinux:latest}"
	run fedora "${FEDORA_IMAGE:-fedora:latest}"
	;;
*) echo 'usage: smoke.sh arch|fedora|all [LOGDIR]' >&2; exit 2 ;;
esac
exit "$status"
