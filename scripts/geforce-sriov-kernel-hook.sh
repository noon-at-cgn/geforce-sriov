#!/bin/sh
# SPDX-License-Identifier: MIT
# geforce-sriov @VERSION@: kernel hook. A new kernel gets NVIDIA's module built the way geforce-sriov
# makes it, so that it boots with the VFs without `rebuild` run by hand.
#
# It is installed by `geforce-sriov install` into /etc/kernel/postinst.d (run by the kernel image package) and
# /etc/kernel/header_postinst.d (run by the headers package, which can come later), and removed by `geforce-sriov uninstall`.
# Both directories are run in name order and DKMS's own hook is called "dkms", so this one runs after DKMS has built its stock
# modules for the kernel. On a systemd kernel-install host (no postinst.d) a plugin calls it, and on an Arch host a pacman hook calls it with --retry.
#
# $1 is the kernel release. It runs `geforce-sriov rebuild --kernel $1`, which needs no answer, logs
# to the journal (tag geforce-sriov-kernel-hook) and to @STATE@/kernel-hook-logs/$1.log, and never fails
# the package installation: a failure leaves a line in @STATE@/kernel-hook-failed/$1, which
# `geforce-sriov status` shows, and the next boot's activation refuses the kernel without the build.
# The command that fixes it is `sudo geforce-sriov rebuild --kernel $1`.
#
# Run as `geforce-sriov-kernel-hook --retry` (by apt, from /etc/apt/apt.conf.d/99geforce-sriov, after dpkg), it builds for
# each installed kernel that has headers and no build yet: the headers can arrive after the image, and the headers package
# does not run a kernel hook on every distribution (Proxmox).

kernel=${1:-}
cli=@BINDIR@/geforce-sriov
state=@STATE@
failed=$state/kernel-hook-failed
logs=$state/kernel-hook-logs

# NVIDIA's own module of kernel $1 exists and was built for that kernel: the driver step compares the build string (vermagic) too.
stock_module() {
    [ "$(modinfo -k "$1" -F vermagic nvidia 2>/dev/null | awk '{print $1}')" = "$1" ]
}
case $kernel in ''|.*|*/*) exit 0 ;; esac

# `--retry` is run by apt after dpkg (/etc/apt/apt.conf.d/99geforce-sriov): the headers of a kernel can be installed after its
# image, and not every distribution runs header_postinst.d for them. Every installed kernel (it has an image in /boot) that has
# its headers and no build of ours gets the build (also a kernel whose build still carries the guest framebuffer cap of an earlier
# build). A kernel reported as missing headers or as missing NVIDIA's own module is tried again; a build that failed for another
# reason is not: `status` names it and the command that fixes it. When there is nothing to build this returns at once, and it
# never fails apt.
if [ "$kernel" = --retry ]; then
    [ -x "$cli" ] && [ -f "$state/driver-build" ] || exit 0
    current=$(sed -n 's/^kernel=//p' "$state/driver-build" 2>/dev/null)
    for dir in /lib/modules/*/; do
        k=${dir%/}
        k=${k##*/}
        case $k in ''|'*'|.*) continue ;; esac
        [ -f "/lib/modules/$k/build/Makefile" ] || continue
        # An image is in /boot (Debian, Fedora) or in the kernel's own directory (Fedora, Arch).
        [ -e "/boot/vmlinuz-$k" ] || [ -e "/lib/modules/$k/vmlinuz" ] || continue
        # The published record names the kernel of the latest build (any kernel, not the running one): that is its build record, and
        # every other kernel's is $state/driver-builds/KERNEL. A kernel that has its build is left alone while NVIDIA's module for it
        # is still installed for that kernel (a headers reinstall or a removed module needs the build again), unless that build still
        # carries the guest framebuffer cap of an earlier build.
        own=
        if [ "$k" = "$current" ]; then
            own=$state/driver-build
        elif [ -e "$state/driver-builds/$k" ]; then
            own=$state/driver-builds/$k
        fi
        if [ -n "$own" ] && ! grep -q '^guest_fb=1$' "$own" 2>/dev/null && stock_module "$k"; then
            continue
        fi
        if [ -f "$failed/$k" ] && ! grep -q -e '^the headers of kernel ' -e "^NVIDIA's own module for kernel " "$failed/$k" 2>/dev/null; then
            continue
        fi
        # DKMS was asked for NVIDIA's own module of this kernel and could not build it: ask again only when the module exists now or the
        # headers changed since (their ctime is later than the report: a reinstalled package keeps the mtimes of its files), so that no
        # apt run pays for a DKMS build that fails every time.
        if [ -f "$failed/$k" ] && grep -q "^NVIDIA's own module for kernel " "$failed/$k" 2>/dev/null; then
            stock_module "$k" ||
                [ "$(stat -L -c %Z "/lib/modules/$k/build/Makefile" 2>/dev/null || echo 0)" -gt "$(stat -c %Z "$failed/$k" 2>/dev/null || echo 0)" ] || continue
        fi
        GS_HOOK_RETRY=1 sh "$0" "$k" || :
    done
    exit 0
fi
log=$logs/$kernel.log
[ -x "$cli" ] || exit 0
# No driver step has run here (install --driver=skip): there is nothing to build again.
[ -f "$state/driver-build" ] || exit 0

say() {
    printf 'geforce-sriov: %s\n' "$*" >&2
    logger -t geforce-sriov-kernel-hook -- "$*" >/dev/null 2>&1 || true
}

# Without the headers of the kernel nothing can be built, and the headers package (which can come
# later) runs this hook again: say what is missing instead of a build that fails.
if [ ! -f "/lib/modules/$kernel/build/Makefile" ]; then
    mkdir -p "$failed" 2>/dev/null
    printf 'the headers of kernel %s are not installed; install them, then run the command below\n' "$kernel" >"$failed/$kernel" 2>/dev/null
    say "the headers of kernel $kernel are not installed, so NVIDIA's module is not built for it yet; install them, then run: sudo geforce-sriov rebuild --kernel $kernel"
    exit 0
fi
# NVIDIA's own module for the kernel is what the build starts from. DKMS builds it for a new kernel from its own headers hook,
# which not every distribution runs (Proxmox's headers package does not), so ask DKMS here; when it is still missing the kernel
# is reported as such, and the next apt run tries again.
limit=
if command -v timeout >/dev/null 2>&1; then
    limit="timeout 3600"
fi
# The module counts only when it was built for this kernel: the driver step compares the build string (vermagic) too.
if ! stock_module "$kernel"; then
    if command -v dkms >/dev/null 2>&1; then
        say "NVIDIA's own module for kernel $kernel is not built yet; asking DKMS to build it (dkms autoinstall -k $kernel)"
        mkdir -p "$logs" 2>/dev/null
        $limit dkms autoinstall -k "$kernel" >"$logs/$kernel.dkms.log" 2>&1 </dev/null || :
    fi
    if ! stock_module "$kernel"; then
        mkdir -p "$failed" 2>/dev/null
        printf 'NVIDIA'\''s own module for kernel %s is not built yet; build it with `dkms autoinstall -k %s`, then run the command below\n' "$kernel" "$kernel" >"$failed/$kernel" 2>/dev/null
        say "NVIDIA's own module for kernel $kernel is not built yet, so geforce-sriov's module is not built for it; build it with DKMS, then run: sudo geforce-sriov rebuild --kernel $kernel"
        exit 0
    fi
fi
say "building NVIDIA's module for kernel $kernel (geforce-sriov rebuild --kernel $kernel); log: $log"
mkdir -p "$logs" 2>/dev/null
# Exit 4 is another geforce-sriov process holding the lock (an activation after boot, an install): wait
# for it, six tries ten seconds apart, before calling it a failure.
tries=0
max=5
[ -z "${GS_HOOK_RETRY:-}" ] || max=0
while :; do
    $limit "$cli" rebuild --kernel "$kernel" >"$log" 2>&1 </dev/null
    rc=$?
    [ "$rc" -eq 4 ] && [ "$tries" -lt "$max" ] || break
    tries=$((tries + 1))
    sleep 10
done
# A run that apt started does not wait for another geforce-sriov process and does not call that a failure: the next apt run tries again.
if [ "$rc" -eq 4 ] && [ -n "${GS_HOOK_RETRY:-}" ]; then
    say "another geforce-sriov process is running; the next apt run tries again"
    exit 0
fi
logger -t geforce-sriov-kernel-hook -f "$log" >/dev/null 2>&1 || true
if [ "$rc" -eq 0 ]; then
    rm -f "$failed/$kernel" 2>/dev/null
    if [ "$kernel" = "$(uname -r 2>/dev/null)" ]; then
        say "NVIDIA's module for kernel $kernel is built; the first boot into it needs a power cycle (details: $log)"
    else
        say "NVIDIA's module for kernel $kernel is built; it is used when the machine boots that kernel (details: $log)"
    fi
else
    mkdir -p "$failed" 2>/dev/null
    printf 'exit %s at %s; see %s\n' "$rc" "$(date '+%Y-%m-%d %H:%M:%S')" "$log" >"$failed/$kernel" 2>/dev/null
    say "building NVIDIA's module for kernel $kernel failed (exit $rc); the kernel install goes on. See $log, then run: sudo geforce-sriov rebuild --kernel $kernel"
fi
exit 0
