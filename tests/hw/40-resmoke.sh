#!/bin/bash
# Step 40: short re-smoke of release candidate rc2 over the installed rc1 (same version 0.4.0: install over it is allowed).
# Two phases, one boot between them:
#   install: unpack the rc2 archive, check its sha256, install it over the installed build, ask for the cold boot.
#   boot:    after that boot: version, units, check/status, placement line, confirmation=confirmed, 4+4 VFs,
#            VM134 + VM144 with guest CUDA, desktop on NVIDIA, no stop condition. Every VM is stopped again at the end.
# Needs the rc2 archive and its SHA256SUMS on pve in $RC2DIR (default /root/release-rc2; copy from
# /home/nathan/geforce-sriov-win/v040/release-rc2/).
# Run: /root/geforce-sriov-win/gpu-run.sh 3000 "0.4.0 rc2: install" bash /root/geforce-sriov-win/v040/final/40-resmoke.sh install
#      (cold boot via request-pve-reboot, then)
#      /root/geforce-sriov-win/gpu-run.sh 1500 "0.4.0 rc2: re-smoke" bash /root/geforce-sriov-win/v040/final/40-resmoke.sh boot
. "$(dirname "$0")/common.sh"
PHASE=${1:-}
RC2DIR=${RC2DIR:-/root/release-rc2}
RC2SRC=/root/rel040rc2/geforce-sriov-0.4.0
case $PHASE in install|boot) ;; *) echo "usage: $0 install|boot" >&2; exit 2 ;; esac
begin_unique 40-resmoke-$PHASE

if [ $PHASE = install ]; then
	check "rc2 archive matches its SHA256SUMS" '(cd $RC2DIR && sha256sum -c SHA256SUMS)'
	check "no VM running" 'vms_stopped'
	check "installed build is 0.4.0 (rc1)" 'geforce-sriov --version | grep -q "geforce-sriov $EXPECT_VER"'
	snap rc1-before
	[ $DRY = 1 ] || { rm -rf /root/rel040rc2; mkdir -p /root/rel040rc2 && tar xzf $RC2DIR/geforce-sriov-0.4.0.tar.gz -C /root/rel040rc2; }
	run build 0 make -C $RC2SRC
	run rc2version 0 $RC2SRC/geforce-sriov --version
	has rc2version "geforce-sriov $EXPECT_VER"
	run install 0 sh -c "cd $RC2SRC && ./geforce-sriov install --driver-file $DRV --companion-file $GRID --nvkms-source $OGKM --accept-nvidia-license --device $ADA --device $AMP --host-desktop"
	hasnt install 'refused'
	check "format stamp, record (top), selection (2 lines), units enabled" '[ "$(cat $STATE/format)" = "$EXPECT_VER" ] && [ "$(rec fb_placement)" = top ] && [ "$(grep -vc "^#" $STATE/selection)" = 2 ] && systemctl is-enabled --quiet geforce-sriov-activate && systemctl is-enabled --quiet geforce-sriov-livepatch'
	check "installed geforce-sriov is byte-identical to the rc2 build" 'cmp -s /usr/local/bin/geforce-sriov $RC2SRC/geforce-sriov'
	snap rc2-installed-not-booted
	statehash rc2-installed
	stopcheck
	nextboot "first boot of rc2 (run: 40-resmoke.sh boot)"
	finish
	exit $?
fi

# ---- phase boot ----
check "this boot differs from the boot that asked for the power cycle" '[ "$(cat $E2E/LAST-REQUEST-BOOT 2>/dev/null)" != "$(bootid)" ]'
run version 0 geforce-sriov --version
has version "geforce-sriov $EXPECT_VER"
check "livepatch and activate units active (exited), lightdm active" 'systemctl is-active --quiet geforce-sriov-livepatch && systemctl is-active --quiet geforce-sriov-activate && systemctl is-active --quiet lightdm'
run check 0 geforce-sriov check
run status 0 geforce-sriov status
has status 'host programs. video memory: placed from the top of the card down'
hasnt status 'framebuffer slots'
check "record: confirmation=confirmed-inputs, driver_version_confirmed=1 (top + display chain recorded; rc2 writes confirmed-inputs, not the bare word)" '[ "$(rec confirmation)" = confirmed-inputs ] && [ "$(rec driver_version_confirmed)" = 1 ]'
has status 'build: confirmation=confirmed-inputs$'
check "4 VFs per card; Ada VFs type 942, Ampere VFs type 525" 'for i in 4 5 6 7; do [ "$(cat /sys/bus/pci/devices/${ADA%.0}.$i/nvidia/current_vgpu_type)" = 942 ] && [ "$(cat /sys/bus/pci/devices/${AMP%.0}.$i/nvidia/current_vgpu_type)" = 525 ] || exit 1; done; [ "$(ls -d /sys/bus/pci/devices/$ADA/virtfn* | wc -l)" = 4 ] && [ "$(ls -d /sys/bus/pci/devices/$AMP/virtfn* | wc -l)" = 4 ]'
check "VM134 (Ada first VF) and VM144 (Ampere first VF) start and run guest CUDA" '{ up 134 >/dev/null && up 144 >/dev/null && cuda 134 && cuda 144; }'
run vmcheck - sh -c 'qm status 134; qm status 144; nvidia-smi --query-gpu=index,pci.bus_id,memory.used --format=csv'
check "desktop on NVIDIA: lightdm active, X draws with NVIDIA's driver" 'systemctl is-active --quiet lightdm && grep -q "draws with NVIDIA" <(geforce-sriov status)'
[ $DRY = 1 ] || { down 134; down 144; }
check "all test VMs stopped, none of the legacy VMs started" 'vms_stopped'
run final-status 0 geforce-sriov status
snap rc2-booted
statehash rc2-booted
check "journalctl -k -b: no stop condition (Wi-Fi mac80211 WARNING excluded)" 'journalctl -k -b --no-pager | grep -E "$STOP_RE" | grep -vE "r8169|mac80211|ieee80211|iwl_mld" | wc -l | grep -q "^0$"'
stopcheck "$(date -d "-$(cut -d. -f1 /proc/uptime) seconds" +"%Y-%m-%d %H:%M:%S")"
finish
