#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs inside an Arch Linux container (tests/distro/smoke.sh arch): the pacman hook of geforce-sriov, rendered the way `install`
# renders it, against a stub `geforce-sriov` that logs its arguments. Fake kernel packages are built by hand and installed with
# `pacman -U`. What is proved: the hook fires with the right kernel release, a transaction that touches neither kernel nor
# headers paths does not run it, a failing build never fails the transaction, and headers that arrive after the image build.
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
rebuilds() { calls | grep -c '^geforce-sriov rebuild' || :; }

S=/opt/gs
state=/var/lib/geforce-sriov
mkdir -p "$S/bin" "$state" /etc/pacman.d/hooks /usr/local/bin /tmp/build
: >/var/log/gs-calls
render() { sed -e "s|@BINDIR@|$S/bin|g" -e "s|@STATE@|$state|g" -e "s|@VERSION@|0.4.1|g" "$1" >"$2"; }
render /src/scripts/geforce-sriov-kernel-hook.sh "$S/bin/geforce-sriov-kernel-hook"
chmod 755 "$S/bin/geforce-sriov-kernel-hook"
render /src/scripts/geforce-sriov-pacman.hook /etc/pacman.d/hooks/90-geforce-sriov.hook
cat >"$S/bin/geforce-sriov" <<'EOF'
#!/bin/sh
echo "geforce-sriov $*" >>/var/log/gs-calls
[ "$1 $2" = "rebuild --kernel" ] || exit 0
[ ! -f /run/gs-fail ] || exit 3
mkdir -p /var/lib/geforce-sriov/driver-builds
echo "kernel=$3" >"/var/lib/geforce-sriov/driver-builds/$3"
EOF
chmod 755 "$S/bin/geforce-sriov"
# NVIDIA's own module for a kernel: found by modinfo (the fixture does not need DKMS here).
printf '#!/bin/sh\nexit 0\n' >/usr/local/bin/modinfo
printf '#!/bin/sh\necho "dkms $*" >>/var/log/gs-calls\n' >/usr/local/bin/dkms
chmod 755 /usr/local/bin/modinfo /usr/local/bin/dkms
echo "kernel=6.0.0-running" >"$state/driver-build"

# A package with the files given as "path" arguments (parents are made; each file holds its own path).
mkpkg() {
	name=$1
	shift
	d=/tmp/build/$name
	rm -rf "$d"
	mkdir -p "$d"
	for f in "$@"; do
		mkdir -p "$d/$(dirname "$f")"
		printf '%s\n' "$f" >"$d/$f"
	done
	cat >"$d/.PKGINFO" <<EOF
pkgname = $name
pkgbase = $name
pkgver = 1-1
pkgdesc = a package of the geforce-sriov smoke test
url = https://example.invalid
builddate = 1700000000
packager = smoke test
size = 1024
arch = any
license = MIT
EOF
	(cd "$d" && tar -czf "/tmp/$name.pkg.tar.gz" .PKGINFO usr)
}
install_pkg() { pacman -U --noconfirm --noscriptlet "/tmp/$1.pkg.tar.gz" >"/tmp/pacman.out" 2>&1; }

K=6.99.1-arch1-1
mkpkg fakelinux "usr/lib/modules/$K/vmlinuz" "usr/lib/modules/$K/modules.order" "usr/lib/modules/$K/build/Makefile" "usr/lib/modules/$K/build/include/gs.h"
install_pkg fakelinux
rc=$?
check "a kernel with its headers: the transaction succeeds" test "$rc" -eq 0
check "a kernel with its headers: the hook ran for that kernel release" contains "$(calls)" "geforce-sriov rebuild --kernel $K"
check "a kernel with its headers: pacman itself ran the hook after the transaction" contains "$(cat /tmp/pacman.out)" "Building NVIDIA's module for geforce-sriov"
check "a kernel with its headers: one build" test "$(rebuilds)" -eq 1
check "a kernel with its headers: the running kernel's record was not touched" test "$(cat "$state/driver-build")" = "kernel=6.0.0-running"

: >/var/log/gs-calls
mkpkg fakeother "usr/share/fakeother/readme"
install_pkg fakeother
rc=$?
check "an unrelated package: the transaction succeeds" test "$rc" -eq 0
check "an unrelated package: the hook does not run" test "$(wc -c </var/log/gs-calls)" -eq 0
check "an unrelated package: nothing is said about geforce-sriov" test -z "$(grep -i 'geforce' /tmp/pacman.out || :)"

# The kernel is installed again with nothing missing: the hook runs (the paths match) and builds nothing more.
: >/var/log/gs-calls
install_pkg fakelinux
rc=$?
check "the same kernel again: the transaction succeeds and builds nothing" test "$rc" -eq 0 -a "$(rebuilds)" -eq 0

# A failing build never fails the transaction, and the failure is kept for status.
K2=6.99.2-arch1-1
: >/run/gs-fail
mkpkg fakelinux2 "usr/lib/modules/$K2/vmlinuz" "usr/lib/modules/$K2/modules.order" "usr/lib/modules/$K2/build/Makefile" "usr/lib/modules/$K2/build/include/gs.h"
: >/var/log/gs-calls
install_pkg fakelinux2
rc=$?
check "a failing build: the pacman transaction still succeeds" test "$rc" -eq 0
check "a failing build: the package is installed" pacman -Q fakelinux2
check "a failing build: the failure is recorded for status" test -f "$state/kernel-hook-failed/$K2"
rm -f /run/gs-fail
: >/var/log/gs-calls
install_pkg fakeother
check "a failure of another reason is not tried again by an unrelated transaction" test "$(rebuilds)" -eq 0

# Headers that arrive after the image: the image alone builds nothing, the headers package starts the build.
K3=6.99.3-arch1-1
mkpkg fakelinux3 "usr/lib/modules/$K3/vmlinuz" "usr/lib/modules/$K3/modules.order"
: >/var/log/gs-calls
install_pkg fakelinux3
rc=$?
check "an image without headers: the transaction succeeds and builds nothing" test "$rc" -eq 0 -a "$(rebuilds)" -eq 0
check "an image without headers: nothing is reported as failed" test ! -e "$state/kernel-hook-failed/$K3"
mkpkg fakeheaders3 "usr/lib/modules/$K3/build/Makefile" "usr/lib/modules/$K3/build/include/gs.h"
install_pkg fakeheaders3
rc=$?
check "the headers arrive later: the transaction succeeds" test "$rc" -eq 0
check "the headers arrive later: the build is made for that kernel" contains "$(calls)" "geforce-sriov rebuild --kernel $K3"

printf 'distro smoke arch: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
