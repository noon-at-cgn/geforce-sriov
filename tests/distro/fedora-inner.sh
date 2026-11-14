#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs inside a Fedora container (tests/distro/smoke.sh fedora): the kernel-install plugin of geforce-sriov, rendered the way
# `install` renders it, against a stub `geforce-sriov` that logs its arguments. A dummy kernel directory stands for an installed
# kernel; `kernel-install add` and `remove` are the real systemd tool. What is proved: the plugin runs for `add` with the right
# kernel release, `remove` runs nothing, and a failing build does not change the exit status of kernel-install.
# The repository is mounted at /src.
set -u
passed=0
failed=0
check() {
	desc=$1
	shift
	if "$@"; then
		passed=$((passed + 1))
		printf 'PASS: %s\n' "$desc"
	else
		failed=$((failed + 1))
		printf 'FAIL: %s\n' "$desc"
	fi
}
contains() { case $1 in *"$2"*) return 0 ;; *) return 1 ;; esac; }
calls() { cat /var/log/gs-calls 2>/dev/null || :; }

dnf -y install systemd-udev >/tmp/dnf.log 2>&1 || { cat /tmp/dnf.log; echo 'FAIL: dnf install systemd-udev'; exit 1; }
command -v kernel-install >/dev/null 2>&1 || { echo 'FAIL: kernel-install is not installed'; exit 1; }
[ -s /etc/machine-id ] || systemd-machine-id-setup >/dev/null 2>&1 || cat /proc/sys/kernel/random/uuid | tr -d - >/etc/machine-id

S=/opt/gs
state=/var/lib/geforce-sriov
K=6.99.1-1.fc42.x86_64
mkdir -p "$S/bin" "$state" /etc/kernel/install.d /usr/local/bin "/lib/modules/$K/build" /boot
: >/var/log/gs-calls
render() { sed -e "s|@BINDIR@|$S/bin|g" -e "s|@STATE@|$state|g" -e "s|@VERSION@|0.4.1|g" "$1" >"$2"; }
render /src/scripts/geforce-sriov-kernel-hook.sh "$S/bin/geforce-sriov-kernel-hook"
chmod 755 "$S/bin/geforce-sriov-kernel-hook"
cat >"$S/bin/geforce-sriov" <<'EOF'
#!/bin/sh
echo "geforce-sriov $*" >>/var/log/gs-calls
[ "$1 $2" = "rebuild --kernel" ] || exit 0
[ ! -f /run/gs-fail ] || exit 3
mkdir -p /var/lib/geforce-sriov/driver-builds
echo "kernel=$3" >"/var/lib/geforce-sriov/driver-builds/$3"
EOF
chmod 755 "$S/bin/geforce-sriov"
printf '#!/bin/sh\nexit 0\n' >/usr/local/bin/modinfo
chmod 755 /usr/local/bin/modinfo
echo "kernel=6.0.0-running" >"$state/driver-build"
: >"/lib/modules/$K/build/Makefile"
: >"/lib/modules/$K/vmlinuz"
cp "/lib/modules/$K/vmlinuz" "/boot/vmlinuz-$K"
: >"/lib/modules/$K/modules.dep"
export KERNEL_INSTALL_MACHINE_ID
KERNEL_INSTALL_MACHINE_ID=$(cat /etc/machine-id)

# The result of kernel-install without our plugin: the other plugins of this container may complain about a missing boot setup.
kernel-install add "$K" "/lib/modules/$K/vmlinuz" >/tmp/baseline.out 2>&1
base_rc=$?
check "without the plugin: no build is run" test "$(calls | grep -c '^geforce-sriov rebuild' || :)" -eq 0

render /src/scripts/geforce-sriov-kernel-install.install /etc/kernel/install.d/90-geforce-sriov.install
chmod 755 /etc/kernel/install.d/90-geforce-sriov.install
: >/var/log/gs-calls
kernel-install --verbose add "$K" "/lib/modules/$K/vmlinuz" >/tmp/add.out 2>&1
rc=$?
check "add: kernel-install ends as it did without the plugin" test "$rc" -eq "$base_rc"
check "add: the build ran for that kernel release, once" test "$(calls)" = "geforce-sriov rebuild --kernel $K"
check "add: the real kernel-install ($(kernel-install --version 2>&1 | head -n 1)) ran the plugin" contains "$(cat /tmp/add.out)" "90-geforce-sriov.install"
check "add: the record of that kernel exists" test -f "$state/driver-builds/$K"

: >/var/log/gs-calls
kernel-install remove "$K" >/tmp/remove.out 2>&1
check "remove: nothing is run" test "$(wc -c </var/log/gs-calls)" -eq 0

: >/run/gs-fail
rm -f "$state/driver-builds/$K"
: >/var/log/gs-calls
kernel-install add "$K" "/lib/modules/$K/vmlinuz" >/tmp/addfail.out 2>&1
rc=$?
check "a failing build: kernel-install ends as it did without the plugin" test "$rc" -eq "$base_rc"
check "a failing build: the build was tried" contains "$(calls)" "geforce-sriov rebuild --kernel $K"
check "a failing build: the failure is recorded for status" test -f "$state/kernel-hook-failed/$K"
rm -f /run/gs-fail

printf 'distro smoke fedora: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
