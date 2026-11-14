#!/bin/sh
# SPDX-License-Identifier: MIT
# Offline test of the hardware smoke scripts in this directory (common.sh, 40-resmoke.sh, smoke040.sh).
# They run on the pve host with root, ssh, qm and the GPUs; here each one runs in its dry mode
# (DRY=1, or smoke040.sh --dry-run), which prints every command and every "CHECK name :: condition"
# instead of acting. This test proves the script logic and nothing about hardware:
#   - the script parses, takes its options and exits 0 without a failure line;
#   - it starts no host tool: ssh, qm, systemctl, journalctl, nvidia-smi, ... are stubs that record
#     the call and fail, and the test fails if any call was recorded;
#   - it writes only under its own scratch directory (GS_DRY_DIR);
#   - its normalized output (times, boot id and kernel stripped) equals tests/hw/fixtures/<case>.expected,
#     so a changed command, check or phase shows up as a diff.
# After an intended change: GS_UPDATE_FIXTURES=1 sh tests/hw/test-hw-dry.sh, and review the diff.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gs-hw-dry.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
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

command -v bash >/dev/null 2>&1 || {
	echo "test-hw-dry: bash is needed to run the smoke scripts" >&2
	exit 2
}

# Host tools the scripts call; each stub records its call and fails.
stubs=$tmp/stubs
calls=$tmp/calls
mkdir "$stubs"
: >"$calls"
for tool in ssh scp qm journalctl systemctl nvidia-smi geforce-sriov lsmod modprobe dkms \
	rmmod insmod virsh reboot shutdown; do
	printf '#!/bin/sh\necho "%s $*" >>"%s"\nexit 99\n' "$tool" "$calls" >"$stubs/$tool"
	chmod 0755 "$stubs/$tool"
done

normalize() {
	sed -E -e 's/^[0-9]{2}:[0-9]{2}:[0-9]{2} //' \
		-e 's/\(boot [0-9a-f]{8}, kernel [^,]*,/(boot BOOT, kernel KERNEL,/' \
		-e 's/since [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9:]{8}/since TIME/' \
		-e "s|$1|DRYDIR|g" -e 's#(DRYDIR/[A-Za-z0-9-]*)-[0-9a-f]{8}([^0-9a-f]|$)#\1-BOOT\2#g' "$2"
}

# run_case NAME SCRIPT [ARGUMENT...]: dry run, then the checks above.
run_case() {
	name=$1
	script=$2
	shift 2
	dry=$tmp/dry-$name
	mkdir "$dry"
	: >"$calls"
	rc=0
	(cd "$dry" && PATH="$stubs:$PATH" DRY=1 GS_DRY_DIR="$dry/state" bash "$here/$script" "$@") \
		>"$tmp/$name.out" 2>"$tmp/$name.err" || rc=$?
	check "$name: exits 0" test "$rc" -eq 0
	check "$name: nothing on stderr" test ! -s "$tmp/$name.err"
	check "$name: prints no FAIL line" test -z "$(grep -E '(^| )FAIL( |$)' "$tmp/$name.out" || true)"
	check "$name: ends with 0 failure(s)" grep -Eq '=== .*: 0 failure\(s\)$' "$tmp/$name.out"
	check "$name: starts no host tool" test ! -s "$calls"
	check "$name: writes only under its own state directory" test -z "$(ls -A "$dry" | grep -vx state || true)"
	normalize "$dry/state" "$tmp/$name.out" >"$tmp/$name.norm"
	if [ -n "${GS_UPDATE_FIXTURES:-}" ]; then
		cp "$tmp/$name.norm" "$here/fixtures/$name.expected"
	fi
	check "$name: output equals fixtures/$name.expected" diff -u "$here/fixtures/$name.expected" "$tmp/$name.norm"
}

# refuses CASE EXPECTED_RC COMMAND...: a usage error exits at once, calls no tool.
refuses() {
	name=$1
	want=$2
	shift 2
	: >"$calls"
	rc=0
	(cd "$tmp" && PATH="$stubs:$PATH" DRY=1 GS_DRY_DIR="$tmp/refuse-state" "$@") >/dev/null 2>&1 || rc=$?
	check "$name: exit $want" test "$rc" -eq "$want"
	check "$name: starts no host tool" test ! -s "$calls"
}

for s in common.sh 40-resmoke.sh smoke040.sh; do
	check "$s: bash -n" bash -n "$here/$s"
done

# The scripts' default dry directories are absolute paths in /tmp; with GS_DRY_DIR set they must
# not be created or touched by any case.
absolute_state() { find /tmp/final-dry /tmp/smoke040-dry -printf '%p %T@\n' 2>/dev/null | sort || true; }
absolute_before=$(absolute_state)
run_case smoke040-default smoke040.sh --dry-run
run_case smoke040-default-boot-ada smoke040.sh --dry-run --placement default --boot ada
run_case smoke040-phase-boot smoke040.sh --dry-run --phase boot --display off
run_case resmoke-install 40-resmoke.sh install
run_case resmoke-boot 40-resmoke.sh boot
check "the default dry directories in /tmp were not touched" test "$absolute_before" = "$(absolute_state)"

# Each run writes its own evidence directory, named by step, phase and boot id, and never reuses one:
# a second run in the same boot (same state directory) is refused before anything else happens.
evdir=$tmp/dry-resmoke-boot/state
check "40-resmoke.sh boot: evidence directory is <step>-<phase>-<boot id>" \
	sh -c "ls -d '$evdir'/40-resmoke-boot-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] >/dev/null"
: >"$calls"
before=$(find "$evdir" -printf '%p %T@\n' | sort)
rc=0
(cd "$tmp" && PATH="$stubs:$PATH" DRY=1 GS_DRY_DIR="$evdir" bash "$here/40-resmoke.sh" boot) >"$tmp/reuse.out" 2>"$tmp/reuse.err" || rc=$?
check "40-resmoke.sh boot: second run in the same boot is refused (exit 2)" test "$rc" -eq 2
check "40-resmoke.sh boot: the refusal names the existing directory" grep -q 'already exists' "$tmp/reuse.err"
check "40-resmoke.sh boot: a refused run starts no host tool" test ! -s "$calls"
check "40-resmoke.sh boot: a refused run leaves the earlier evidence untouched" test "$before" = "$(find "$evdir" -printf '%p %T@\n' | sort)"
: >"$calls"
rc=0
(cd "$tmp" && PATH="$stubs:$PATH" DRY=1 GS_DRY_DIR="$evdir" bash "$here/40-resmoke.sh" install) >"$tmp/other-phase.out" 2>&1 || rc=$?
check "40-resmoke.sh install: another phase in the same boot gets its own directory" test "$rc" -eq 0

# Plain begin keeps the fixed name $E2E/NAME (other steps read each other's evidence by it) and may be rerun.
plain=$tmp/plain-state
: >"$calls"
for _ in 1 2; do
	(cd "$tmp" && PATH="$stubs:$PATH" DRY=1 GS_DRY_DIR="$plain" bash -c '. "$1/common.sh"; begin 99-plain; finish' sh "$here") >>"$tmp/plain.out" 2>&1 || true
done
check "begin: evidence directory is still \$E2E/NAME, with no boot id" test -d "$plain/99-plain"
check "begin: a rerun of the same step is not refused" test "$(grep -c '=== 99-plain: 0 failure' "$tmp/plain.out")" -eq 2
check "begin: starts no host tool" test ! -s "$calls"

# smoke040.sh takes DRY=1 from the environment as well as --dry-run.
: >"$calls"
(cd "$tmp" && PATH="$stubs:$PATH" DRY=1 GS_DRY_DIR="$tmp/env-state" bash "$here/smoke040.sh" --phase boot) >"$tmp/env.out" 2>&1 || true
check "smoke040.sh: DRY=1 in the environment is a dry run" grep -q 'dry=1' "$tmp/env.out"
check "smoke040.sh: DRY=1 in the environment starts no host tool" test ! -s "$calls"

refuses "smoke040.sh: unknown argument" 2 bash "$here/smoke040.sh" --dry-run --bogus
refuses "smoke040.sh: removed --slots" 2 bash "$here/smoke040.sh" --dry-run --slots 4
refuses "smoke040.sh: bad placement" 2 bash "$here/smoke040.sh" --dry-run --placement middle
refuses "smoke040.sh: bad --boot" 2 bash "$here/smoke040.sh" --dry-run --boot gpu
refuses "40-resmoke.sh: no phase" 2 bash "$here/40-resmoke.sh"
refuses "40-resmoke.sh: unknown phase" 2 bash "$here/40-resmoke.sh" later

printf 'hw dry-run fixtures: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
