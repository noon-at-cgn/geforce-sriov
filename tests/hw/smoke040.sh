#!/bin/bash
# geforce-sriov 0.4.0 smoke test on pve after a cold boot (final run; derived from v040/smoke040.sh, sha cf60d10e).
# Run on pve:  /root/geforce-sriov-win/gpu-run.sh 5400 "0.4.0 smoke" bash /root/geforce-sriov-win/v040/final/smoke040.sh [options]
#
#   --placement top|default   what the installed build was made for (default: top = what fb_placement=auto resolves to with an Ada card)
#   --phase LIST              comma list of: boot,firstvf,display,place,managed,load,full,ampere,win135,tail (default: all that apply)
#   --dry-run                 print every command instead of running it (runs anywhere; touches nothing)
#   --tag NAME                evidence directory name under e2e-20261110T1736/ (default: smoke040)
#   --boot ada|ampere|none    override the detected boot display card (detection reads boot_vga)
#   --display auto|off        what the build was made for (default: auto)
#   --expect-version V        version the CLI must print (default 0.4.0)
# Changes against the earlier smoke040.sh: the Ada slot reservation is gone in 0.4.0 (no --slots, no optin phase, no
# fb_owner record keys, no /proc/driver/nvidia/geforce-sriov-fb); the boot phase also checks the format stamp, the guest
# framebuffer cap and the build record; new phase win135 (Windows on the Ada card); kerr() ignores the boot-time
# Wi-Fi/netconsole mac80211 WARNING (it is not a GPU event) but still stops on every real stop condition.
set -u
TAG=smoke040; PLACEMENT=top; PHASES=""; DRY=${DRY:-0}; BOOTOPT=""; DISPLAY_MODE=auto; EXPECT_VER=0.4.0
while [ $# -gt 0 ]; do
	case $1 in
	--placement) PLACEMENT=$2; shift ;;
	--phase) PHASES=$2; shift ;;
	--tag) TAG=$2; shift ;;
	--dry-run) DRY=1 ;;
	--boot) BOOTOPT=$2; shift ;;
	--display) DISPLAY_MODE=$2; shift ;;
	--expect-version) EXPECT_VER=$2; shift ;;
	--slots) echo "--slots was removed with the slot reservation in 0.4.0" >&2; exit 2 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
case $PLACEMENT in top|default) ;; *) echo "--placement: top or default" >&2; exit 2 ;; esac
# ---- VM lists: the ONLY place to edit per layout -----------------------------------------------------------------
ADA_VMS="134 136 137 138"; ADA_FIRST=134; ADA_LAST=138; ADA_WIN=135
AMP_VMS="144 145 146 147"; AMP_LINUX="144 146 147"; AMP_WIN=145
ADA=$ADA_VMS; ALL="$ADA_VMS $AMP_VMS"
# ------------------------------------------------------------------------------------------------------------------
gen_of() { local id; id=$(cat /sys/bus/pci/devices/$1/device 2>/dev/null) || return 1; id=$((id))
	if [ $id -ge $((0x2200)) ] && [ $id -lt $((0x2600)) ]; then echo ampere; elif [ $id -ge $((0x2680)) ] && [ $id -lt $((0x2900)) ]; then echo ada; else echo other; fi; }
ADA_BDF=""; AMP_BDF=""; BOOT=""
if [ $DRY = 1 ]; then
	ADA_BDF=${GS_ADA_BDF:-0000:03:00.0}; AMP_BDF=${GS_AMP_BDF:-0000:04:00.0}; BOOT=$AMP_BDF
else
	for d in /sys/bus/pci/devices/*; do
		[ -e $d/physfn ] && continue
		b=${d##*/}
		[ "$(cat $d/boot_vga 2>/dev/null)" = 1 ] && BOOT=$b
		[ "$(cat $d/vendor 2>/dev/null)" = 0x10de ] || continue
		case $(cat $d/class) in 0x03*) ;; *) continue ;; esac
		case $(gen_of $b) in ada) [ -n "$ADA_BDF" ] || ADA_BDF=$b ;; ampere) [ -n "$AMP_BDF" ] || AMP_BDF=$b ;; esac
	done
	if [ -z "$ADA_BDF" ] || [ -z "$AMP_BDF" ]; then echo "no card found (Ada=${ADA_BDF:-none} Ampere=${AMP_BDF:-none}); this script covers the mixed host" >&2; exit 2; fi
fi
DETECTED_BOOT=$BOOT
case $BOOTOPT in ada) BOOT=$ADA_BDF ;; ampere) BOOT=$AMP_BDF ;; none) BOOT="" ;; "") ;; *) echo "--boot: ada, ampere or none" >&2; exit 2 ;; esac
if [ $DRY = 0 ] && [ -n "$BOOTOPT" ] && [ "$BOOT" != "$DETECTED_BOOT" ]; then
	echo "--boot $BOOTOPT disagrees with sysfs boot_vga (${DETECTED_BOOT:-none}); refusing" >&2; exit 2
fi
B=$ADA_BDF; A=$AMP_BDF
BOOTGEN=none; [ "$BOOT" = "$ADA_BDF" ] && [ -n "$BOOT" ] && BOOTGEN=ada; [ "$BOOT" = "$AMP_BDF" ] && [ -n "$BOOT" ] && BOOTGEN=ampere
EXP_B=0; EXP_A=0
if [ $DISPLAY_MODE = auto ]; then [ $BOOTGEN = ada ] && EXP_B=1; [ $BOOTGEN = ampere ] && EXP_A=1; fi
drmcount() { local c d n=0; for c in /sys/class/drm/card[0-9]; do [ "$(basename "$(readlink -f $c/device/driver 2>/dev/null)")" = nvidia ] || continue; d=$(readlink -f $c/device); case $d in *"$1"*) n=$((n+1)) ;; esac; done; echo $n; }
vf_of() { local x; x=$(sed -n -e 's/.*host=\([0-9a-f:.]*\).*/\1/p' -e 's/^hostpci[0-9]*: *\([0-9a-f:.]*\).*/\1/p' /etc/pve/qemu-server/$1.conf | head -1); case $x in 0000:*|"") ;; *) x=0000:$x ;; esac; echo $x; }
vmcheck() { local bad=0 v vf pf
	for v in $1; do
		vf=$(vf_of $v); pf=$(basename "$(readlink -f /sys/bus/pci/devices/$vf/physfn 2>/dev/null)" 2>/dev/null)
		if [ -z "$vf" ] || [ "$pf" != "$2" ]; then log "REFUSE: VM$v maps ${vf:-no VF} (card ${pf:-unknown}); expected a VF of the $3 card $2"; bad=1; fi
	done; return $bad; }
if [ -z "$PHASES" ]; then
	PHASES=boot
	[ $BOOTGEN = ada ] && PHASES=$PHASES,firstvf
	PHASES=$PHASES,display,place,managed,load,full,ampere,win135,tail
fi
phase() { case ",$PHASES," in *,$1,*) return 0 ;; esac; return 1; }

E=/root/geforce-sriov-win/e2e-20261110T1736/$TAG
[ $DRY = 1 ] && E=${GS_DRY_DIR:-/tmp/smoke040-dry}/$TAG
T=/root/ko/tool/ada-fbslot; P=/proc/driver/nvidia/geforce-sriov-fb; GFB=/proc/driver/nvidia/geforce-sriov-guest-fb
REC=/var/lib/geforce-sriov/driver-build
CUDA_LIVE=/root/geforce-sriov-win/v040/cuda-live.py
PTX=/root/geforce-sriov-win/e2e-20261110T1736/82-release-850f9b2/ptx-probe.py
# Type 942 slots on the 12282 MiB Ada card: four of 0x7e000000 from 0, ending at 0x1f8000000.
SLOT=(0x0 0x7e000000 0xfc000000 0x17a000000); SLOTSZ=0x7e000000; SLOTTOP=0x1f8000000
S="ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no"
declare -A G=([134]=fe80::be24:11ff:fe94:330e [136]=fe80::be24:11ff:fe90:3ac6 [137]=fe80::be24:11ff:fe30:5078 [138]=fe80::be24:11ff:fedf:2438 [144]=fe80::be24:11ff:fede:77fb [146]=fe80::be24:11ff:fed3:30bb [147]=fe80::be24:11ff:fe95:9b53)
FAIL=0; HOGS=""; LIVEPID=""; MPID=""; ABORT=0; T_START=$(date +"%Y-%m-%d %H:%M:%S")
ABOVE=0; [ $PLACEMENT = top ] && ABOVE=1

D() { [ $DRY = 1 ] && echo "+ $*"; return 0; }
X() { if [ $DRY = 1 ]; then echo "+ $*"; else "$@"; fi; }
log() { if [ $DRY = 1 ]; then echo "$(date +%T) $*"; else echo "$(date +%T) $*" | tee -a $E/run.log; fi; }
check() {
	if [ $DRY = 1 ]; then log "CHECK $1 :: $2"; return 0; fi
	if eval "$2"; then log "PASS $1"; else log "FAIL $1"; FAIL=$((FAIL+1)); fi
}
# Stop conditions since this script began. The boot-time mac80211 WARNING (Wi-Fi under netconsole) is not a GPU event.
kerr() { journalctl -k --since "$T_START" --no-pager | grep -E "NVRM: Xid|AER:.*(Uncorrect|Fatal)|DMAR.*[Ff]ault|Oops|BUG:|WARNING:|fallen off|hung_task|blocked for more|soft lockup" | grep -vE 'r8169|mac80211|ieee80211|iwl_mld'; }
gwait() { for i in $(seq 1 40); do $S root@${G[$1]}%vmbr0 true 2>/dev/null && return 0; sleep 3; done; return 1; }
cuda() { gwait $1 && $S root@${G[$1]}%vmbr0 'timeout 120 /root/geforce-sriov-cuda-probe 2>&1 | tail -1' 2>&1 | grep -q '^PASS'; }
down() { qm shutdown $1 --timeout 180 --forceStop 1 >/dev/null 2>&1; }
up() { qm start $1 >$E/start-$1.out 2>&1; sleep 10; qm status $1 | grep -q running; }
offset_of() { sed -n 's/.*returned: offset=\(0x[0-9a-f]*\).*/\1/p' "$1" | head -1; }
hold() { $T hold --bdf $B --offset 0 --size $2 --align 0x200000 --flags ${3:-0x19100} --share none > $1 2>&1 & h=$!; HOGS="$HOGS $h"; sleep 1.5; }
release_hogs() { local h; for h in $HOGS; do kill -TERM $h 2>/dev/null; done; for h in $HOGS; do wait $h 2>/dev/null; done; HOGS=""; }
fill() {
	NFILL=0; LOWEST=""; local n=0 f
	[ $DRY = 1 ] && return 0
	while [ $n -lt $2 ]; do
		f=$1-$n.out
		$T hold --bdf $B --offset 0 --size 0x10000000 --align 0x200000 --flags 0x19100 --share none > $f 2>&1 & h=$!
		sleep 1.5
		grep -q HANDOFF $f || { kill $h 2>/dev/null; cp $f $1-refused.out; break; }
		HOGS="$HOGS $h"; n=$((n+1)); LOWEST=$(offset_of $f)
	done
	NFILL=$n
}
cleanup() {
	[ $DRY = 1 ] && return 0
	release_hogs
	[ -z "$LIVEPID" ] || kill $LIVEPID 2>/dev/null
	[ -z "$MPID" ] || kill $MPID 2>/dev/null
	[ $ABORT = 1 ] || for v in $ALL; do qm status $v | grep -q running || qm start $v >/dev/null 2>&1; done
}
trap cleanup EXIT

X mkdir -p $E
log "=== smoke040 tag=$TAG placement=$PLACEMENT phases=$PHASES dry=$DRY"
log "=== layout: Ada=${ADA_BDF:-none} Ampere=${AMP_BDF:-none} boot display=${BOOT:-none} ($BOOTGEN); NVIDIA KMS expected: Ada=$EXP_B Ampere=$EXP_A"
[ $DRY = 1 ] && log "dry run: nothing below is executed; 'CHECK name :: condition' lines show what a real run tests"
[ $DRY = 1 ] || log "=== boot $(cut -c1-8 /proc/sys/kernel/random/boot_id 2>/dev/null), kernel $(uname -r), record $(grep -E '^(fb_placement|fb_placement_measured|guest_fb|driver_version_confirmed)=' $REC 2>/dev/null | tr '\n' ' ')"

if phase boot; then
	log "=== boot: version, format stamp, build record, status and check lines"
	check "version is $EXPECT_VER" 'geforce-sriov --version | grep -q "geforce-sriov $EXPECT_VER"'
	check "format stamp names $EXPECT_VER" '[ "$(cat /var/lib/geforce-sriov/format 2>/dev/null)" = "$EXPECT_VER" ]'
	check "record names fb_placement=$PLACEMENT" 'grep -q "^fb_placement=$PLACEMENT$" $REC'
	check "record: no fb_owner key (slot reservation removed in 0.4.0)" '! grep -q "^fb_owner" $REC'
	check "record: no test_only key" '! grep -q "^test_only=" $REC'
	check "record: guest framebuffer cap built (guest_fb=1)" 'grep -q "^guest_fb=1$" $REC'
	check "no /proc/driver/nvidia/geforce-sriov-fb (owner gone)" '[ ! -e $P ]'
	check "guest-fb proc file present, abi 1" 'grep -q "^abi 1" $GFB'
	if [ $PLACEMENT = top ]; then
		check "status: host programs placed from the top" 'geforce-sriov status | grep -q "host programs. video memory: placed from the top of the card down"'
		check "check: same line" 'geforce-sriov check 2>&1 | grep -q "host programs. video memory: placed from the top of the card down"'
		[ $DRY = 1 ] || log "status unconfirmed sentence (expected only while placement_measured=0): $(geforce-sriov status 2>&1 | grep -i 'not yet confirmed' | head -1)"
	else
		check "status: default placement sentence" 'geforce-sriov status | grep -q "can be given the range of a stopped VM"'
	fi
	check "check is ready" 'geforce-sriov check >/dev/null 2>&1'
	check "status exits 0" 'geforce-sriov status >/dev/null 2>&1'
	check "4 VFs on the Ada card" '[ "$(ls -d /sys/bus/pci/devices/$B/virtfn* | wc -l)" = 4 ]'
	check "4 VFs on the Ampere card" '[ "$(ls -d /sys/bus/pci/devices/$A/virtfn* | wc -l)" = 4 ]'
	X nvidia-smi -i $B --query-gpu=memory.used,memory.free --format=csv,noheader
fi
if phase display; then
	log "=== display: console/desktop on the boot display card $BOOT ($BOOTGEN); NVIDIA KMS expected on $BOOT only (display=$DISPLAY_MODE)"
	[ -n "$BOOT" ] && check "boot_vga is 1 on $BOOT" '[ "$(cat /sys/bus/pci/devices/$BOOT/boot_vga)" = 1 ]'
	check "display: NVIDIA DRM cards on $B (Ada): expected $EXP_B" '[ "$(drmcount ${B#0000:})" = $EXP_B ]'
	check "display: NVIDIA DRM cards on $A (Ampere): expected $EXP_A" '[ "$(drmcount ${A#0000:})" = $EXP_A ]'
	if [ $((EXP_A + EXP_B)) = 1 ]; then
		check "display: nvidia_drm modeset is Y" 'grep -q "^Y" /sys/module/nvidia_drm/parameters/modeset'
		check "display: display manager is active" 'systemctl is-active lightdm >/dev/null 2>&1 || systemctl is-active display-manager >/dev/null 2>&1'
	else
		check "display off: no NVIDIA-driver DRM card anywhere" '[ "$(drmcount "")" = 0 ]'
	fi
	[ $DRY = 1 ] || { geforce-sriov status 2>&1 | grep -iE 'display|desktop|boot' > $E/status-display.txt; nvidia-smi > $E/nvidia-smi-display.txt 2>&1; }
fi

if [ $DRY = 1 ]; then
	echo "+ vmcheck: VMs $ADA_VMS $ADA_WIN must map a VF of the Ada card $B; VMs $AMP_VMS must map a VF of the Ampere card $A; otherwise REFUSE, exit 3, no VM started"
else
	vmcheck "$ADA_VMS $ADA_WIN" "$B" Ada && vmcheck "$AMP_VMS" "$A" Ampere || { ABORT=1; log "REFUSED: fix the host= lines of the VMs; no VM was started"; exit 3; }
fi

if phase firstvf; then
	log "=== firstvf: the first Ada VF (VM$ADA_FIRST) must start and restart; stop at the first failure"
	if [ $DRY = 0 ]; then
		ok=1
		up $ADA_FIRST >/dev/null || ok=0
		[ $ok = 1 ] && { cuda $ADA_FIRST || ok=0; }
		[ $ok = 1 ] && { down $ADA_FIRST; up $ADA_FIRST >/dev/null && cuda $ADA_FIRST || ok=0; }
		[ -z "$(kerr)" ] || ok=0
		if [ $ok = 0 ]; then
			ABORT=1; log "FIRST VF FAILED on VM$ADA_FIRST: $(tail -3 $E/start-$ADA_FIRST.out 2>/dev/null | tr '\n' ' ')"
			journalctl -k -b --no-pager > $E/journal-firstvf-fail.txt
			exit 1
		fi
	else
		echo "+ start VM$ADA_FIRST, guest CUDA, stop, start, guest CUDA; on any failure or kernel error: save journal, exit 1"
	fi
	check "firstvf: VM$ADA_FIRST running after one restart" 'qm status $ADA_FIRST | grep -q running'
fi

for v in $ALL; do X qm status $v; done
[ $DRY = 1 ] || for v in $ALL; do qm status $v | grep -q running || up $v >/dev/null; done
[ $DRY = 1 ] && echo "+ for v in $ALL: start the VM if it is not running (up)"
for v in $ALL; do check "VM$v running" 'qm status $v | grep -q running'; done
for v in $ADA_VMS $AMP_LINUX; do check "VM$v guest CUDA" 'cuda $v'; done
check "VM$AMP_WIN (Windows) guest agent" 'for i in $(seq 1 36); do qm guest cmd $AMP_WIN ping >/dev/null 2>&1 && break; sleep 5; done; qm guest cmd $AMP_WIN ping >/dev/null 2>&1'

if phase place; then
	log "=== place: E1 placement of host allocations (all four Ada VMs stopped)"
	for v in $ADA; do D qm shutdown $v --timeout 180 --forceStop 1; done
	[ $DRY = 1 ] || for v in $ADA; do down $v; done
	X hold $E/e1-1.out 0x10000000
	[ $DRY = 1 ] || { OFF1=$(offset_of $E/e1-1.out); log "E1 first host allocation (256 MiB, flags 0x19100): offset ${OFF1:-none}"; }
	X hold $E/e1-2.out 0x10000000
	[ $DRY = 1 ] || { OFF2=$(offset_of $E/e1-2.out); log "E1 second host allocation: offset ${OFF2:-none}"; }
	if [ $ABOVE = 1 ]; then
		check "E1: first allocation lands above the four slot ranges (>= $SLOTTOP)" '[ -n "$OFF1" ] && [ $((OFF1)) -ge $((SLOTTOP)) ]'
		check "E1: second allocation lands directly below the first (top-down)" '[ $((OFF2 + 0x10000000)) -eq $((OFF1)) ]'
		for i in 0 1 2 3; do
			check "E1: slot $i (${SLOT[$i]}) is free of host memory" '$T collide --bdf $B --offset ${SLOT[$i]} --size $SLOTSZ --align 0x2000000 --expect ok >/dev/null 2>&1'
		done
	else
		check "E1 control: first allocation lands in the bottom slot (< ${SLOT[1]}); default placement" '[ -n "$OFF1" ] && [ $((OFF1)) -lt $((${SLOT[1]})) ]'
	fi
	[ $DRY = 1 ] || release_hogs
	[ $DRY = 1 ] && echo "+ release the two E1 allocations (kill the hold processes)"
	for v in $ADA; do D qm start $v; done
	[ $DRY = 1 ] || for v in $ADA; do up $v >/dev/null; done
	for v in $ADA; do check "E1: VM$v starts after the host allocations are freed" 'qm status $v | grep -q running'; done
fi

if phase managed; then
	log "=== managed: CUDA managed memory (512 MiB) resident on the Ada card while VM$ADA_FIRST starts"
	MP=/root/geforce-sriov-win/v040/final/managed-probe.py
	if [ $DRY = 1 ]; then
		echo "+ qm shutdown $ADA_FIRST; python3 $MP --bdf $B --mib 512 > $E/managed.out &  (cuMemAllocManaged, CPU write, GPU kernel, prefetch, hold)"
		echo "+ nvidia-smi memory.used and per-process used_memory before/after; collide on slot 0; qm start $ADA_FIRST; guest CUDA; SIGTERM; VERIFY ok"
	else
		down $ADA_FIRST
		MEM0=$(nvidia-smi -i $B --query-gpu=memory.used --format=csv,noheader,nounits)
		python3 $MP --bdf $B --mib 512 > $E/managed.out 2>&1 & MPID=$!
		for i in $(seq 1 90); do grep -q '^RESIDENT' $E/managed.out 2>/dev/null && break; kill -0 $MPID 2>/dev/null || break; sleep 1; done
		MEM1=$(nvidia-smi -i $B --query-gpu=memory.used --format=csv,noheader,nounits)
		nvidia-smi -i $B --query-compute-apps=pid,process_name,used_memory --format=csv > $E/managed-apps.txt 2>&1
		nvidia-smi -i $B > $E/managed-smi.txt 2>&1
		log "managed: Ada memory.used $MEM0 -> $MEM1 MiB; probe: $(tr '\n' ';' < $E/managed.out | cut -c1-300)"
	fi
	check "managed: the probe reached RESIDENT on the Ada card (kernel ok, last prefetch location = the card's ordinal)" 'grep -q "^KERNEL ok" $E/managed.out && awk "/^RESIDENT/ {exit !(\$2 == \$3)}" $E/managed.out'
	check "managed: the Ada card's memory.used grew by at least 480 MiB (the pages are in video memory)" '[ $((MEM1 - MEM0)) -ge 480 ]'
	check "managed: the probe process is listed on the Ada card with at least 480 MiB (nvidia-smi per-process)" 'awk -F"[ ,]+" -v p=$MPID "\$1 == p && \$(NF-1)+0 >= 480 {f=1} END {exit !f}" $E/managed-apps.txt'
	if [ $ABOVE = 1 ]; then
		# Only VM$ADA_FIRST is stopped here (the other Ada VMs run and hold slots 1-3), so only slot 0 can be tested.
		check "managed: slot 0 (${SLOT[0]}, VM$ADA_FIRST's range) is still free of host memory with the managed pages resident" '$T collide --bdf $B --offset ${SLOT[0]} --size $SLOTSZ --align 0x2000000 --expect ok >/dev/null 2>&1'
	fi
	[ $DRY = 1 ] || { up $ADA_FIRST >/dev/null; }
	check "managed: VM$ADA_FIRST starts while the managed memory is resident" 'qm status $ADA_FIRST | grep -q running'
	check "managed: VM$ADA_FIRST guest CUDA passes" 'cuda $ADA_FIRST'
	check "managed: the probe is still alive and still resident after the VM start" 'kill -0 $MPID 2>/dev/null && tail -1 $E/managed.out | grep -qE "^(STILL|RESIDENT) "'
	if [ $DRY = 0 ]; then
		sleep 6; kill -TERM $MPID 2>/dev/null; wait $MPID 2>/dev/null; MRC=$?; MPID=""
		log "managed: probe exit $MRC; $(tail -2 $E/managed.out | tr '\n' ';')"
	fi
	check "managed: the data survived the VM start (VERIFY ok, exit 0)" 'grep -q "^VERIFY ok" $E/managed.out && [ "$MRC" = 0 ]'
	check "managed: no stop condition" '[ -z "$(kerr)" ]'
	log "managed: where the allocation lands: the CUDA API shows no offset; evidence is the memory.used and per-process delta above, and the free slot ranges (placement top keeps host memory above the VF ranges)"
fi

if phase load; then
	log "=== load: per-process video memory of the Ada VMs, host CUDA from the top, 5 restarts of each Ada VM"
	[ $DRY = 1 ] || { nvidia-smi -i $B > $E/nvidia-smi-vms-up.txt 2>&1; nvidia-smi -i $B --query-compute-apps=pid,process_name,used_memory --format=csv > $E/nvidia-smi-apps-vms-up.txt 2>&1; nvidia-smi vgpu -i $B -q > $E/nvidia-smi-vgpu-q.txt 2>&1; }
	[ $DRY = 1 ] && echo "+ nvidia-smi -i $B > $E/nvidia-smi-vms-up.txt; nvidia-smi vgpu -i $B -q"
	check "nvidia-smi: Ada card memory.used with 4 VMs up is non-zero" '[ "$(nvidia-smi -i $B --query-gpu=memory.used --format=csv,noheader,nounits)" -gt 0 ]'
	D python3 $CUDA_LIVE '>/dev/null &'
	[ $DRY = 1 ] || { timeout 3000 python3 $CUDA_LIVE >/dev/null 2>&1 & LIVEPID=$!; sleep 8; }
	check "host CUDA (cuda-live.py) holds a context on the Ada card" 'kill -0 $LIVEPID 2>/dev/null && nvidia-smi -i $B --query-compute-apps=pid --format=csv,noheader | grep -q .'
	fill $E/load-fill 14
	[ $DRY = 1 ] && echo "+ fill: 14 x 256 MiB host allocations (flags 0x19100) on $B, one at a time"
	[ $DRY = 1 ] || log "host filled $NFILL x 256 MiB, lowest offset ${LOWEST:-none}, free $(nvidia-smi -i $B --query-gpu=memory.free --format=csv,noheader)"
	[ $ABOVE = 1 ] && check "load: every fill allocation above the slots (lowest offset >= $SLOTTOP)" '[ -n "$LOWEST" ] && [ $((LOWEST)) -ge $((SLOTTOP)) ]'
	log "=== 5 restarts of each Ada VM under that load"
	[ $DRY = 1 ] && { echo "+ for c in 1..5: for v in $ADA: down \$v; up \$v; check guest CUDA after the restart"; }
	if [ $DRY = 0 ]; then
		for c in 1 2 3 4 5; do
			for v in $ADA; do
				down $v
				if up $v >/dev/null; then log "restart $c VM$v: started"; else log "restart $c VM$v: START FAILED: $(tail -3 $E/start-$v.out | tr '\n' ' ')"; FAIL=$((FAIL+1)); fi
				cuda $v || { log "restart $c VM$v: guest CUDA FAILED"; FAIL=$((FAIL+1)); }
			done
			log "restart round $c done; free $(nvidia-smi -i $B --query-gpu=memory.free --format=csv,noheader)"
		done
	fi
	for v in $ADA; do check "load: VM$v running after 5 restarts under host load" 'qm status $v | grep -q running'; done
	check "load: no stop condition during the loop" '[ -z "$(kerr)" ]'
	[ $DRY = 1 ] || { release_hogs; kill $LIVEPID 2>/dev/null; pkill -f "v040/cuda-live.py" 2>/dev/null; LIVEPID=""; }
	[ $DRY = 1 ] && echo "+ release the fill; stop cuda-live.py"
fi

if phase full; then
	log "=== full: the host fills the card with VM$ADA_LAST stopped; with placement=top the fill reaches the stopped VM's range last"
	D qm shutdown $ADA_LAST --timeout 180 --forceStop 1
	[ $DRY = 1 ] || down $ADA_LAST
	fill $E/full-fill 40
	[ $DRY = 1 ] && echo "+ fill: up to 40 x 256 MiB host allocations until one is refused"
	[ $DRY = 1 ] || {
		log "full card: $NFILL x 256 MiB granted, lowest offset ${LOWEST:-none}; refusal: $(cat $E/full-fill-refused.out 2>/dev/null | tr '\n' ' ' | cut -c1-200)"
		log "free after the fill: $(nvidia-smi -i $B --query-gpu=memory.free --format=csv,noheader)"
		nvidia-smi -i $B > $E/nvidia-smi-full.txt 2>&1
	}
	check "full: the fill reached into the highest stopped VM range (< $SLOTTOP)" '[ -n "$LOWEST" ] && [ $((LOWEST)) -lt $((SLOTTOP)) ]'
	D qm start $ADA_LAST
	[ $DRY = 1 ] || { up $ADA_LAST >/dev/null; log "VM$ADA_LAST start with the card full: $(qm status $ADA_LAST | tr '\n' ' ') | qm start said: $(tr '\n' ' ' < $E/start-$ADA_LAST.out | cut -c1-300)"; log "kernel/vgpu messages: $(journalctl -b --since -2min --no-pager | grep -iE 'Failed to alloc guest FB|0x51|vgpu.*(fail|error)' | tail -3 | tr '\n' '|')"; }
	check "full: VM$ADA_LAST fails to start while the host holds its range" '! qm status $ADA_LAST | grep -q running'
	check "full: the failure names the reason (qm start output or vGPU log)" 'grep -qiE "Failed to alloc|0x51|0x1a|plugin registration|out of memory|vgpu|error getting device" $E/start-$ADA_LAST.out || journalctl -b --since -2min --no-pager | grep -qiE "Failed to alloc guest FB|0x51|start failed"'
	[ $DRY = 1 ] || release_hogs
	[ $DRY = 1 ] && echo "+ release the fill"
	D qm start $ADA_LAST
	[ $DRY = 1 ] || up $ADA_LAST >/dev/null
	check "full: after the fill is freed VM$ADA_LAST starts" 'qm status $ADA_LAST | grep -q running'
	check "full: VM$ADA_LAST guest CUDA" 'cuda $ADA_LAST'
	check "full: no stop condition" '[ -z "$(kerr)" ]'
fi

if phase ampere; then
	log "=== ampere: regression (VMs, guest CUDA, host CUDA, display)"
	for v in $AMP_VMS; do check "Ampere VM$v running" 'qm status $v | grep -q running'; done
	for v in $AMP_LINUX; do check "Ampere VM$v guest CUDA" 'cuda $v'; done
	check "Ampere VM$AMP_WIN (Windows 582.53) guest agent" 'qm guest cmd $AMP_WIN ping >/dev/null 2>&1'
	[ $DRY = 1 ] || qm guest exec $AMP_WIN -- 'C:\Windows\System32\nvidia-smi.exe' > $E/win$AMP_WIN-smi.txt 2>&1
	check "Ampere VM$AMP_WIN guest nvidia-smi names an NVIDIA vGPU" 'grep -q "NVIDIA" $E/win$AMP_WIN-smi.txt'
	check "host CUDA on both cards (ptx-probe, clean env)" 'env -i PATH=/usr/bin:/bin python3 $PTX 2>&1 | tail -3 | grep -qi "ok\|pass"'
	check "display: NVIDIA DRM cards on $A: expected $EXP_A" '[ "$(drmcount ${A#0000:})" = $EXP_A ]'
	check "display: NVIDIA DRM cards on $B: expected $EXP_B" '[ "$(drmcount ${B#0000:})" = $EXP_B ]'
	check "display: display manager is active" 'systemctl is-active lightdm >/dev/null 2>&1 || systemctl is-active display-manager >/dev/null 2>&1'
	X nvidia-smi -i $A
fi

if phase win135; then
	log "=== win135: Windows 11 (539.19) on the Ada card's VF .7: VM$ADA_LAST stops first, they share the VF"
	[ $DRY = 1 ] && echo "+ qm shutdown $ADA_LAST; qm start $ADA_WIN; wait for the agent; qm guest exec $ADA_WIN nvidia-smi; qm shutdown $ADA_WIN; qm start $ADA_LAST"
	if [ $DRY = 0 ]; then
		down $ADA_LAST
		up $ADA_WIN >/dev/null
	fi
	check "VM$ADA_WIN (Windows, Ada) running" 'qm status $ADA_WIN | grep -q running'
	check "VM$ADA_WIN guest agent answers" 'for i in $(seq 1 36); do qm guest cmd $ADA_WIN ping >/dev/null 2>&1 && break; sleep 5; done; qm guest cmd $ADA_WIN ping >/dev/null 2>&1'
	[ $DRY = 1 ] || qm guest exec $ADA_WIN -- 'C:\Windows\System32\nvidia-smi.exe' > $E/win$ADA_WIN-smi.txt 2>&1
	check "VM$ADA_WIN guest driver shows the Ada vGPU" 'grep -q "RTX6000-Ada" $E/win$ADA_WIN-smi.txt'
	if [ $DRY = 0 ]; then down $ADA_WIN; up $ADA_LAST >/dev/null; fi
	check "VM$ADA_LAST runs again after VM$ADA_WIN stopped" 'qm status $ADA_LAST | grep -q running'
	check "win135: no stop condition" '[ -z "$(kerr)" ]'
fi

if phase tail; then
	log "=== tail"
	check "no stop condition since the script began" '[ -z "$(kerr)" ]'
	check "no /proc/driver/nvidia/geforce-sriov-fb (owner gone)" '[ ! -e $P ]'
	[ $DRY = 1 ] || log "guest-fb state: $(tr '\n' ';' < $GFB 2>/dev/null)"
fi
log "=== smoke040: $FAIL failure(s)"
[ $DRY = 1 ] || [ $FAIL = 0 ]
