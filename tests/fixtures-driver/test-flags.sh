#!/bin/sh
# SPDX-License-Identifier: MIT
# Argument contract and fail-closed checks for scripts/geforce-sriov-driver.sh.
# Every run is offline, under a temporary root, and mutates nothing there.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
driver=$here/../../scripts/geforce-sriov-driver.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gs-driver-flags.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/root"
passed=0
failed=0
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
contains() { case $1 in *"$2"*) return 0 ;; *) return 1 ;; esac; }

run() {
	rc=0
	env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin sh "$driver" --root "$tmp/root" --kernel fixture "$@" \
		>"$tmp/out" 2>&1 || rc=$?
	out=$(cat "$tmp/out")
}

run --companion-file a --companion-source b
check "conflicting companion inputs are a usage error" test "$rc" -eq 2
run --source "$tmp/root" --driver-file "$tmp/package.run"
check "two host inputs are a usage error" test "$rc" -eq 2
run --no-such-option
check "an unknown option is a usage error" test "$rc" -eq 2
run --display
check "display without sources refuses" test "$rc" -eq 3
run --host-cuda
check "host CUDA without a companion refuses" test "$rc" -eq 3
run --dry-run --display --companion-file a --nvkms-source b
check "dry run of a display request succeeds" test "$rc" -eq 0
check "dry run prints the feature flags" contains "$out" 'features: display=1 host_cuda=0'
check "dry run prints the inputs" contains "$out" 'inputs: driver_file=none companion_file=a'
check "dry run prints the classification" contains "$out" 'version classification:'
check "dry run lists the patch plan" contains "$out" '+ geforce-sriov-patch apply'
check "dry run lists the idx 39 step" contains "$out" 'display apply'
check "dry run leaves the root untouched" test -z "$(find "$tmp/root" -mindepth 1 -print -quit)"
run --restore --dry-run
check "restore dry run succeeds without an installation" test "$rc" -eq 0
check "restore dry run says there is nothing to restore" contains "$out" 'nothing to restore'

printf 'driver argument fixtures: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
