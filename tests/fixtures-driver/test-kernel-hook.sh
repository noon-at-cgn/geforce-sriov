#!/bin/sh
# SPDX-License-Identifier: MIT
# The kernel hook (scripts/geforce-sriov-kernel-hook.sh): what the kernel package's postinst runs
# for a new kernel. It is rendered the way install renders it, against a stub geforce-sriov, in a
# temporary tree; nothing outside it is touched.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
template=$here/../../scripts/geforce-sriov-kernel-hook.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gs-kernel-hook.XXXXXX")
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
contains() { case $1 in *"$2"*) return 0 ;; *) return 1 ;; esac; }
not_contains() { case $1 in *"$2"*) return 1 ;; *) return 0 ;; esac; }

bin=$tmp/bin
state=$tmp/state
stubs=$tmp/stubs
mkdir -p "$bin" "$state" "$stubs"
# The headers directory and the wait between tries are the fixture's own, so that nothing outside the tree is read and no test sleeps.
sed -e "s|@BINDIR@|$bin|g" -e "s|@STATE@|$state|g" -e "s|@VERSION@|0.4.1|g" \
	-e "s|/boot/vmlinuz-|$tmp/boot/vmlinuz-|g" -e "s|/lib/modules/|$tmp/modules/|g" -e "s|sleep 10|sleep 0|" "$template" >"$tmp/hook"
for k in 7.0.99-1-pve 7.0.99-2-pve; do
	mkdir -p "$tmp/modules/$k/build"
	: >"$tmp/modules/$k/build/Makefile"
done
chmod 755 "$tmp/hook"
check "every placeholder is replaced" test -z "$(grep '@[A-Z]' "$tmp/hook" || true)"
check "the hook is valid shell" sh -n "$tmp/hook"

# The stub: records its arguments; as the driver step does, a successful rebuild leaves the record
# of the kernel; the exit code is what the file mode holds.
cat >"$bin/geforce-sriov" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >>"$tmp/calls"
echo "stub rebuild output for \$3"
if [ -s "$tmp/seq" ]; then
	rc=\$(head -n 1 "$tmp/seq")
	sed -i 1d "$tmp/seq"
else
	rc=\$(cat "$tmp/mode")
fi
if [ "\$rc" -eq 0 ]; then
	mkdir -p "$state/driver-builds"
	echo "kernel=\$3" >"$state/driver-builds/\$3"
fi
exit "\$rc"
EOF
chmod 755 "$bin/geforce-sriov"
# logger: the tag and the lines it was given.
cat >"$stubs/logger" <<EOF
#!/bin/sh
printf 'logger %s\n' "\$*" >>"$tmp/logger"
EOF
chmod 755 "$stubs/logger"
# modinfo -k RELEASE -n nvidia finds NVIDIA's own module for a kernel only when the fixture made it; dkms autoinstall -k RELEASE makes it
# unless the fixture says it cannot.
mkdir -p "$tmp/stock"
for k in 7.0.99-1-pve 7.0.99-2-pve 7.0.99-3-pve; do : >"$tmp/stock/$k"; done
cat >"$stubs/modinfo" <<EOF
#!/bin/sh
[ -e "$tmp/stock/\$2" ] || exit 1
case "\$*" in *vermagic*) if [ -e "$tmp/vermagic-wrong/\$2" ]; then echo "6.0.0-other SMP mod_unload"; else echo "\$2 SMP mod_unload"; fi ;; esac
EOF
cat >"$stubs/dkms" <<EOF
#!/bin/sh
printf 'dkms %s\n' "\$*" >>"$tmp/dkmscalls"
[ ! -f "$tmp/dkms-fails" ] || exit 1
: >"$tmp/stock/\$3"
EOF
chmod 755 "$stubs/modinfo" "$stubs/dkms"
# The running kernel is the second of the fixture's kernels.
printf '#!/bin/sh\necho 7.0.99-2-pve\n' >"$stubs/uname"
chmod 755 "$stubs/uname"

run() {
	rc=0
	env -i PATH="$stubs:/usr/sbin:/usr/bin:/sbin:/bin" sh "$tmp/hook" "$@" >"$tmp/out" 2>&1 </dev/null || rc=$?
	out=$(cat "$tmp/out")
}

# No driver step has run here (install --driver=skip): nothing to build, nothing reported.
echo 0 >"$tmp/mode"
run 7.0.99-1-pve
check "no build record: the hook succeeds" test "$rc" -eq 0
check "no build record: rebuild is not run" test ! -e "$tmp/calls"
check "no build record: nothing is reported" test ! -e "$state/kernel-hook-failed"

# A new kernel after a successful install: the same build `rebuild --kernel` does.
echo "kernel=7.0.14-16-pve" >"$state/driver-build"
run 7.0.99-1-pve
check "new kernel: the hook succeeds" test "$rc" -eq 0
check "new kernel: rebuild --kernel RELEASE is the command run" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-1-pve"
check "new kernel: rebuild was given the release (the stub writes the record; the real driver step is covered by test-driver.sh)" test -f "$state/driver-builds/7.0.99-1-pve"
check "new kernel: no failure is reported" test ! -e "$state/kernel-hook-failed/7.0.99-1-pve"
check "new kernel: the log holds rebuild's output" contains "$(cat "$state/kernel-hook-logs/7.0.99-1-pve.log")" "stub rebuild output for 7.0.99-1-pve"
check "new kernel: the journal gets it under the hook's tag" contains "$(cat "$tmp/logger")" "geforce-sriov-kernel-hook"
check "new kernel: the package output says what happened" contains "$out" "NVIDIA's module for kernel 7.0.99-1-pve is built"
check "a kernel that is not running: the build is used when the machine boots it, no power cycle is asked" contains "$out" "it is used when the machine boots that kernel" 
check "a kernel that is not running: no power cycle is asked" not_contains "$out" "power cycle"

# A failed build never fails the kernel package; it is reported, with the one command that fixes it.
rm -f "$tmp/calls" "$state/driver-builds/7.0.99-1-pve"
echo 3 >"$tmp/mode"
run 7.0.99-1-pve
check "failed build: the hook still succeeds" test "$rc" -eq 0
check "failed build: the failure is recorded for that kernel" contains "$(cat "$state/kernel-hook-failed/7.0.99-1-pve")" "exit 3"
check "failed build: no build record" test ! -e "$state/driver-builds/7.0.99-1-pve"
check "failed build: the fix is named" contains "$out" "sudo geforce-sriov rebuild --kernel 7.0.99-1-pve"
check "failed build: the log says why" contains "$(cat "$state/kernel-hook-logs/7.0.99-1-pve.log")" "stub rebuild output"

# The headers can arrive after the image: the hook runs again and builds (`rebuild` clears the report).
echo 0 >"$tmp/mode"
run 7.0.99-1-pve
check "second run: the record exists" test -f "$state/driver-builds/7.0.99-1-pve"

# One apt run can configure two kernels: each keeps its own log.
run 7.0.99-2-pve
check "two kernels: both logs are kept" test -f "$state/kernel-hook-logs/7.0.99-1-pve.log" -a -f "$state/kernel-hook-logs/7.0.99-2-pve.log"
check "the running kernel: the first boot into it needs a power cycle" contains "$out" "the first boot into it needs a power cycle"

# The headers are not there: nothing is built, and the report says what to install.
rm -f "$tmp/calls" "$state/kernel-hook-failed/7.0.99-3-pve"
echo 0 >"$tmp/mode"
run 7.0.99-3-pve
check "no headers: the hook succeeds" test "$rc" -eq 0
check "no headers: rebuild is not run" test ! -e "$tmp/calls"
check "no headers: status gets the cause" contains "$(cat "$state/kernel-hook-failed/7.0.99-3-pve")" "the headers of kernel 7.0.99-3-pve are not installed; install them, then run the command below"
check "no headers: the package output says it" contains "$out" "install them, then run: sudo geforce-sriov rebuild --kernel 7.0.99-3-pve"
# The headers arrive later: the hook builds and the report is gone.
mkdir -p "$tmp/modules/7.0.99-3-pve/build"
: >"$tmp/modules/7.0.99-3-pve/build/Makefile"
run 7.0.99-3-pve
check "headers arrive: the report is gone" test ! -e "$state/kernel-hook-failed/7.0.99-3-pve"

# Exit 4 (another geforce-sriov process holds the lock) is tried again, six tries in all.
rm -f "$tmp/calls"
printf '4\n4\n0\n' >"$tmp/seq"
run 7.0.99-2-pve
check "lock held twice: the third try builds" test "$(wc -l <"$tmp/calls")" -eq 3 -a ! -e "$state/kernel-hook-failed/7.0.99-2-pve"
rm -f "$tmp/calls"
echo 4 >"$tmp/mode"
run 7.0.99-2-pve
check "lock held throughout: six tries, then the failure is reported" test "$(wc -l <"$tmp/calls")" -eq 6
check "lock held throughout: the report names exit 4" contains "$(cat "$state/kernel-hook-failed/7.0.99-2-pve")" "exit 4"
echo 0 >"$tmp/mode"

# `--retry`, as apt runs it after dpkg (/etc/apt/apt.conf.d/99geforce-sriov): a kernel with an image and headers and no build gets one,
# whichever package came first. Nothing else is touched, and apt never fails.
mkdir -p "$tmp/boot"
for k in 7.0.99-1-pve 7.0.99-2-pve 7.0.99-3-pve; do : >"$tmp/boot/vmlinuz-$k"; done
newkernel() { # $1 release, $2 headers (1/0), $3 image (1/0), $4 nostock: NVIDIA's own module is not built
	[ "$2" -eq 0 ] || { mkdir -p "$tmp/modules/$1/build"; : >"$tmp/modules/$1/build/Makefile"; }
	[ "$3" -eq 0 ] || : >"$tmp/boot/vmlinuz-$1"
	[ "$2" -eq 1 ] || mkdir -p "$tmp/modules/$1"
	[ "${4:-}" = nostock ] || : >"$tmp/stock/$1"
}
rm -f "$tmp/calls"
run --retry
check "retry, every kernel with headers has its build: the hook succeeds and builds nothing" test "$rc" -eq 0 -a ! -e "$tmp/calls"
check "retry, nothing to do: it says nothing" test -z "$out"
newkernel 7.0.99-4-pve 1 1
run --retry
check "retry, headers came after the image: the build is made" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-4-pve" -a -f "$state/driver-builds/7.0.99-4-pve"
rm -f "$tmp/calls"
run --retry
check "retry, again: nothing is built a second time" test ! -e "$tmp/calls"
newkernel 7.0.99-5-pve 1 0
newkernel 7.0.99-6-pve 0 1
run --retry
check "retry, no image or no headers: nothing is built and nothing is reported" test ! -e "$tmp/calls" -a ! -e "$state/kernel-hook-failed/7.0.99-5-pve" -a ! -e "$state/kernel-hook-failed/7.0.99-6-pve"
newkernel 7.0.99-7-pve 1 1
mkdir -p "$state/kernel-hook-failed"
printf 'exit 3 at 2026-11-12 10:00:00; see somewhere\n' >"$state/kernel-hook-failed/7.0.99-7-pve"
run --retry
check "retry, an earlier build failed for another reason: it is left to the user" test ! -e "$tmp/calls" -a -f "$state/kernel-hook-failed/7.0.99-7-pve"
newkernel 7.0.99-8-pve 1 1
printf 'the headers of kernel 7.0.99-8-pve are not installed; install them, then run the command below\n' >"$state/kernel-hook-failed/7.0.99-8-pve"
run --retry
check "retry, the report was missing headers and they are there now: built, report gone" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-8-pve" -a ! -e "$state/kernel-hook-failed/7.0.99-8-pve"
rm -f "$tmp/calls"
newkernel 7.0.99-9-pve 1 1
echo 4 >"$tmp/mode"
run --retry
check "retry, the lock is held: one try, no wait, no failure report, apt is not failed" test "$rc" -eq 0 -a "$(wc -l <"$tmp/calls")" -eq 1 -a ! -e "$state/kernel-hook-failed/7.0.99-9-pve"
check "retry, the lock is held: one line says so" contains "$out" "another geforce-sriov process is running; the next apt run tries again"
rm -f "$tmp/calls"
echo 3 >"$tmp/mode"
run --retry
check "retry, the build fails: apt is not failed and the failure is reported" test "$rc" -eq 0 -a -f "$state/kernel-hook-failed/7.0.99-9-pve"
echo 0 >"$tmp/mode"
rm -f "$tmp/calls"
run --retry
check "retry, that failure is not tried at every apt run" test ! -e "$tmp/calls"
mv "$state/driver-build" "$state/driver-build.away"
newkernel 7.0.99-10-pve 1 1
run --retry
check "retry, no build record (install --driver=skip, or uninstalled): nothing is built" test "$rc" -eq 0 -a ! -e "$tmp/calls"
mv "$state/driver-build.away" "$state/driver-build"
: >"$state/driver-builds/7.0.99-10-pve"

# NVIDIA's own module for the kernel is not built (Proxmox's headers package does not run DKMS's hook): DKMS is asked first, and the
# build follows when it made the module. When it did not, the kernel is reported and tried again at the next apt run.
newkernel 7.0.99-11-pve 1 1 nostock
rm -f "$tmp/calls" "$tmp/dkmscalls"
run --retry
check "retry, no stock module: dkms autoinstall -k RELEASE is asked, then the build is made" test "$(cat "$tmp/dkmscalls")" = "dkms autoinstall -k 7.0.99-11-pve" -a "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-11-pve"
check "retry, no stock module: the output says DKMS is asked" contains "$out" "asking DKMS to build it"
newkernel 7.0.99-12-pve 1 1 nostock
: >"$tmp/dkms-fails"
rm -f "$tmp/calls" "$tmp/dkmscalls"
run --retry
check "retry, DKMS cannot build it: the build is not run, apt is not failed" test "$rc" -eq 0 -a ! -e "$tmp/calls"
check "retry, DKMS cannot build it: the report names the module" contains "$(cat "$state/kernel-hook-failed/7.0.99-12-pve")" "NVIDIA's own module for kernel 7.0.99-12-pve is not built yet"
rm -f "$tmp/dkmscalls"
run --retry
check "retry, that report is not tried again while nothing changed (no DKMS build at every apt run)" test ! -e "$tmp/dkmscalls" -a ! -e "$tmp/calls"
sleep 1
# A reinstalled headers package keeps the mtimes of its files: the mtime is old, only the ctime moved.
touch -d '2001-01-01 00:00:00' "$tmp/modules/7.0.99-12-pve/build/Makefile"
run --retry
check "retry, only the headers' ctime moved since (their mtime is old): DKMS is asked again" test "$(cat "$tmp/dkmscalls")" = "dkms autoinstall -k 7.0.99-12-pve"
rm -f "$tmp/dkmscalls"
: >"$tmp/stock/7.0.99-12-pve"
rm -f "$tmp/dkms-fails" "$tmp/calls"
run --retry
check "retry, the module exists now (built by other means): no DKMS run, the build is made and the report is gone" test ! -e "$tmp/dkmscalls" -a "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-12-pve" -a ! -e "$state/kernel-hook-failed/7.0.99-12-pve"

# NVIDIA's module is there but was built for another kernel (its vermagic names a different release): the build does not start, the
# report is the retryable one (the stock module is "not built yet"), and a later run with a matching vermagic builds.
newkernel 7.0.99-14-pve 1 1
mkdir -p "$tmp/vermagic-wrong"
: >"$tmp/vermagic-wrong/7.0.99-14-pve"
: >"$tmp/dkms-fails"
rm -f "$tmp/calls" "$tmp/dkmscalls"
run 7.0.99-14-pve
check "vermagic of another kernel: the hook succeeds and builds nothing" test "$rc" -eq 0 -a ! -e "$tmp/calls"
check "vermagic of another kernel: DKMS is asked for the module" test "$(cat "$tmp/dkmscalls")" = "dkms autoinstall -k 7.0.99-14-pve"
check "vermagic of another kernel: the retryable report is written, not a build failure" contains "$(cat "$state/kernel-hook-failed/7.0.99-14-pve")" "NVIDIA's own module for kernel 7.0.99-14-pve is not built yet"
check "vermagic of another kernel: no build failure is recorded" not_contains "$(cat "$state/kernel-hook-failed/7.0.99-14-pve")" "exit "
check "vermagic of another kernel: no build record" test ! -e "$state/driver-builds/7.0.99-14-pve"
rm -f "$tmp/dkmscalls"
run --retry
check "vermagic of another kernel, apt retry gate: the report is retried only when something changed, so nothing runs" test ! -e "$tmp/calls" -a ! -e "$tmp/dkmscalls"
# The retry gate itself: the report is old and the headers changed, so DKMS is asked again, and still no build while the vermagic is wrong.
sleep 1
touch -d '2001-01-01 00:00:00' "$tmp/modules/7.0.99-14-pve/build/Makefile"
run --retry
check "vermagic of another kernel, apt retry gate: the module does not count, DKMS is asked and nothing is built" test "$(cat "$tmp/dkmscalls")" = "dkms autoinstall -k 7.0.99-14-pve" -a ! -e "$tmp/calls" -a -f "$state/kernel-hook-failed/7.0.99-14-pve"
# The module is rebuilt for the kernel: the vermagic matches, the retry builds and the report is gone.
rm -f "$tmp/vermagic-wrong/7.0.99-14-pve" "$tmp/dkms-fails" "$tmp/dkmscalls"
run --retry
check "vermagic matches now: apt's retry builds, no DKMS run, report gone" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-14-pve" -a ! -e "$tmp/dkmscalls" -a ! -e "$state/kernel-hook-failed/7.0.99-14-pve" -a -f "$state/driver-builds/7.0.99-14-pve"
# The direct run: a wrong vermagic is also not built, and DKMS making a matching module lets the build go ahead.
newkernel 7.0.99-15-pve 1 1
: >"$tmp/vermagic-wrong/7.0.99-15-pve"
cat >"$stubs/dkms" <<EOF
#!/bin/sh
printf 'dkms %s\n' "\$*" >>"$tmp/dkmscalls"
rm -f "$tmp/vermagic-wrong/\$3"
: >"$tmp/stock/\$3"
EOF
rm -f "$tmp/calls" "$tmp/dkmscalls"
run 7.0.99-15-pve
check "vermagic of another kernel, DKMS rebuilds it for the kernel: the build is made" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-15-pve" -a ! -e "$state/kernel-hook-failed/7.0.99-15-pve"

# The published record names the kernel of the latest build, which a hook build for a kernel that is not running republishes: that kernel
# has no copy under driver-builds, and the published record is its build. It is left alone while its module is installed, and built
# again when the module is gone (a headers reinstall after the module was removed).
newkernel 7.0.99-16-pve 1 1
printf 'kernel=7.0.99-16-pve\n' >"$state/driver-build"
rm -f "$tmp/calls" "$tmp/dkmscalls" "$state/driver-builds/7.0.99-16-pve"
run --retry
check "published record names a kernel that is not running, its module is there: nothing is built" test ! -e "$tmp/calls" -a ! -e "$tmp/dkmscalls"
rm -f "$tmp/stock/7.0.99-16-pve"
sleep 1
touch -d '2001-01-01 00:00:00' "$tmp/modules/7.0.99-16-pve/build/Makefile"
run --retry
check "published record names that kernel, its NVIDIA module is gone: DKMS is asked and the build is made again" test "$(cat "$tmp/dkmscalls")" = "dkms autoinstall -k 7.0.99-16-pve" -a "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-16-pve"
printf 'kernel=7.0.99-2-pve\nguest_fb=1\n' >"$state/driver-build"
rm -f "$tmp/calls" "$state/kernel-hook-failed/7.0.99-2-pve"
run --retry
check "published record of the running kernel, built with the cap: the build is made again" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-2-pve"
echo "kernel=7.0.14-16-pve" >"$state/driver-build"

# A kernel built by an earlier build of this version that carried the guest framebuffer cap is built again, once.
printf 'kernel=7.0.99-4-pve\nguest_fb=1\n' >"$state/driver-builds/7.0.99-4-pve"
rm -f "$tmp/calls"
run --retry
check "retry, a record with guest_fb=1: the build is made again" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-4-pve"
rm -f "$tmp/calls"
run --retry
check "retry, the new record has no cap: nothing more is built" test ! -e "$tmp/calls"

# The image of an Arch or Fedora kernel is in its own directory under /lib/modules, not in /boot/vmlinuz-RELEASE.
newkernel 7.0.99-13-pve 1 0
: >"$tmp/modules/7.0.99-13-pve/vmlinuz"
rm -f "$tmp/calls"
run --retry
check "retry, the image is in the kernel's own directory: the build is made" test "$(cat "$tmp/calls")" = "rebuild --kernel 7.0.99-13-pve"

# The kernel-install plugin (scripts/geforce-sriov-kernel-install.install): `add KERNEL_VERSION ENTRY_DIR KERNEL_IMAGE` runs the hook for that
# kernel; every other command does nothing; it never fails kernel-install.
plugin=$tmp/90-geforce-sriov.install
hookbin=$tmp/pluginbin
mkdir -p "$hookbin"
sed -e "s|@BINDIR@|$hookbin|g" -e "s|@VERSION@|0.4.1|g" "$here/../../scripts/geforce-sriov-kernel-install.install" >"$plugin"
chmod 755 "$plugin"
check "plugin: every placeholder is replaced" test -z "$(grep '@[A-Z]' "$plugin" || true)"
check "plugin: valid shell" sh -n "$plugin"
cat >"$hookbin/geforce-sriov-kernel-hook" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >>"$tmp/plugincalls"
[ ! -f "$tmp/plugin-fails" ] || exit 7
EOF
chmod 755 "$hookbin/geforce-sriov-kernel-hook"
rm -f "$tmp/plugincalls"
prc=0; "$plugin" add 6.99.1-1.fc42.x86_64 /boot/entry /lib/modules/6.99.1-1.fc42.x86_64/vmlinuz >"$tmp/pout" 2>&1 || prc=$?
check "plugin, add: the hook runs for that kernel and kernel-install is not failed" test "$prc" -eq 0 -a "$(cat "$tmp/plugincalls")" = "6.99.1-1.fc42.x86_64"
rm -f "$tmp/plugincalls"
prc=0; "$plugin" remove 6.99.1-1.fc42.x86_64 /boot/entry >"$tmp/pout" 2>&1 || prc=$?
check "plugin, remove: nothing is run" test "$prc" -eq 0 -a ! -e "$tmp/plugincalls"
prc=0; "$plugin" >"$tmp/pout" 2>&1 || prc=$?
check "plugin, no command: nothing is run" test "$prc" -eq 0 -a ! -e "$tmp/plugincalls"
: >"$tmp/plugin-fails"
prc=0; "$plugin" add 6.99.1-1.fc42.x86_64 >"$tmp/pout" 2>&1 || prc=$?
check "plugin, the hook fails: kernel-install is not failed" test "$prc" -eq 0
rm -f "$tmp/plugin-fails"
mv "$hookbin/geforce-sriov-kernel-hook" "$hookbin/gone"
prc=0; "$plugin" add 6.99.1-1.fc42.x86_64 >"$tmp/pout" 2>&1 || prc=$?
check "plugin, no hook installed: nothing happens" test "$prc" -eq 0

# The pacman hook (scripts/geforce-sriov-pacman.hook): after a transaction that installs or upgrades a kernel or its headers.
pmh=$here/../../scripts/geforce-sriov-pacman.hook
check "pacman hook: triggers on installs and upgrades of the kernel modules and headers, after the transaction" test \
	"$(grep -c -E '^(Operation = Install|Operation = Upgrade|Type = Path|When = PostTransaction)$' "$pmh")" -eq 4 -a \
	"$(grep -c -E '^Target = usr/lib/modules/\*/(build/include/|modules\.order)$' "$pmh")" -eq 2
check "pacman hook: runs the hook script in retry mode" test "$(grep -c '^Exec = @BINDIR@/geforce-sriov-kernel-hook --retry$' "$pmh")" -eq 1

# Arguments that are no kernel release, and a host without the installed copy.
rm -f "$tmp/calls"
for bad in '' ../x a/b .hidden; do
	run "$bad"
	check "release '$bad': ignored" test "$rc" -eq 0 -a ! -e "$tmp/calls"
done
mv "$bin/geforce-sriov" "$bin/gone"
run 7.0.99-1-pve
check "no installed copy: the hook succeeds and does nothing" test "$rc" -eq 0 -a ! -e "$tmp/calls"

printf 'kernel hook fixtures: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
