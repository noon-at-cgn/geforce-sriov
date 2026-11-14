#!/bin/bash
# Shared helpers for the 0.4.0 final hardware run. Source it from a step script on pve (root):
#   . "$(dirname "$0")/common.sh"; begin 2X-040f-name
# DRY=1 makes run/check/snap print instead of act; only smoke040.sh --dry-run is a full dry run. The step scripts are
# checked with bash -n here and are run for real on pve only.
# Every step prints "=== <name>: N failure(s)" and exits non-zero on a failure. A stop condition
# (Xid, fatal AER, DMAR fault, Oops/BUG, hung task, lockup, "fallen off the bus") exits 90 at once.
set -u
DRY=${DRY:-0}
ROOT=/root/geforce-sriov-win
E2E=$ROOT/e2e-20261110T1736
[ $DRY = 1 ] && E2E=${GS_DRY_DIR:-/tmp/final-dry}
FIN=$ROOT/v040/final
INP=$ROOT/v030-inputs
DRV=$INP/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run
GRID=$INP/NVIDIA-Linux-x86_64-580.159.03-grid.run
OGKM=$INP/ogkm-580.159.03.tar.gz
DRV_SHA=8fb252d5eafbb5740c9f623c881334119c532c1960834712956eb5e304e7ec41
GRID_SHA=8de9c10cfda77a89a8056acec476c13549f8f6dc7c7f9675da38dc9a73208a81
OGKM_SHA=c6e6de2829d62b3c3db8ea7c747c3e7bc373ef06a8d4dc88ba935facd615e70f
R030=/root/geforce-sriov-0.3.0.tar.gz
R030_SHA=8334196e507a39f7e97c4e3aae959a700dfb198c08586862a8168c51a2c15d00
# The integrated 0.4.0 release archive (built after the merge and the version bump; sha256 in $FIN/R040.sha256).
R040=${R040:-/root/geforce-sriov-0.4.0-final.tar.gz}
SRC030=/root/rel030/geforce-sriov-0.3.0
SRC040=/root/rel040f/geforce-sriov-0.4.0
OLD040=/root/rel040/geforce-sriov-0.4.0        # the 88d003b test build that is installed at the start
EXPECT_VER=${GS_EXPECT_VERSION:-0.4.0}
STATE=/var/lib/geforce-sriov
CFG=/etc/geforce-sriov/config
GB=/root/ko/tool/ada-fbslot
GUESTFB=/proc/driver/nvidia/geforce-sriov-guest-fb
ADA_VMS="134 136 137 138"; ADA_WIN=135; AMP_LINUX="144 146 147"; AMP_WIN=145
TEST_VMS="134 135 136 137 138 144 145 146 147"
LEGACY_VMS="104 120 124 141"
SSHO="-o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no"
declare -A G=([134]=fe80::be24:11ff:fe94:330e [136]=fe80::be24:11ff:fe90:3ac6 [137]=fe80::be24:11ff:fe30:5078 [138]=fe80::be24:11ff:fedf:2438 [144]=fe80::be24:11ff:fede:77fb [146]=fe80::be24:11ff:fed3:30bb [147]=fe80::be24:11ff:fe95:9b53)
STOP_RE='NVRM: Xid|AER:.*(Uncorrect|Fatal)|BUG:|Oops|DMAR.*[Ff]ault|hung_task|blocked for more|fallen off|soft lockup|WARNING:'
FAILS=0; EV=""; NAME=""; T0=""; RC=0

gen_of() { local id; id=$(cat /sys/bus/pci/devices/$1/device 2>/dev/null) || return 1; id=$((id))
	if [ $id -ge $((0x2200)) ] && [ $id -lt $((0x2600)) ]; then echo ampere; elif [ $id -ge $((0x2680)) ] && [ $id -lt $((0x2900)) ]; then echo ada; else echo other; fi; }
cards() { # sets ADA and AMP by PCI id (physical functions of class 03xx), never by address
	ADA=""; AMP=""
	if [ $DRY = 1 ]; then ADA=0000:03:00.0; AMP=0000:04:00.0; return 0; fi
	local d b
	for d in /sys/bus/pci/devices/*; do
		[ -e $d/physfn ] && continue; b=${d##*/}
		[ "$(cat $d/vendor 2>/dev/null)" = 0x10de ] || continue
		case $(cat $d/class) in 0x03*) ;; *) continue ;; esac
		case $(gen_of $b) in ada) [ -n "$ADA" ] || ADA=$b ;; ampere) [ -n "$AMP" ] || AMP=$b ;; esac
	done
}
bootid() { cut -c1-8 /proc/sys/kernel/random/boot_id; }
log() { echo "$(date +%T) $*" | { if [ -n "$EV" ] && [ $DRY = 0 ]; then tee -a $EV/run.log; else cat; fi; }; }
begin() { NAME=$1; EV=$E2E/$NAME; mkdir -p $EV; T0=$(date +"%Y-%m-%d %H:%M:%S"); cards
	log "=== $NAME start (boot $(bootid), kernel $(uname -r), Ada=${ADA:-none} Ampere=${AMP:-none}, dry=$DRY)"
	:; }
# begin_unique NAME: like begin, but the evidence directory is $E2E/NAME-<boot id> and is created with a plain
# mkdir, so a run never shares a directory with an earlier one (the rc3 re-smoke overwrote the rc2 evidence by
# reusing one name). An existing directory is refused (exit 2); a rerun in the same boot needs a new boot.
# Only steps that opt in use it: other steps read each other's evidence by the fixed name begin gives.
begin_unique() { NAME=$1; EV=$E2E/$NAME-$(bootid); mkdir -p "$E2E"
	if ! mkdir "$EV" 2>/dev/null; then echo "refused: evidence directory $EV already exists; every run writes its own directory (name + boot id) and never reuses one" >&2; exit 2; fi
	T0=$(date +"%Y-%m-%d %H:%M:%S"); cards
	log "=== $NAME start (boot $(bootid), kernel $(uname -r), Ada=${ADA:-none} Ampere=${AMP:-none}, dry=$DRY, evidence $EV)"
	:; }
finish() { log "=== $NAME: $FAILS failure(s)"; echo "$(date -Is) $NAME failures=$FAILS boot=$(bootid)" >> $E2E/FINAL-STATE.log; [ $FAILS = 0 ]; }
check() { # check "name" 'shell condition'
	if [ $DRY = 1 ]; then log "CHECK $1 :: $2"; return 0; fi
	if eval "$2"; then log "PASS $1"; else log "FAIL $1"; FAILS=$((FAILS+1)); fi
}
halt() { log "STOP: $*"; [ $DRY = 1 ] || journalctl -k -b --no-pager > $EV/journal-stop.txt 2>&1
	log "No retry, no second reboot. Evidence in $EV. Report to Main; recovery brief; cold boot only via request-pve-reboot."; exit 90; }
stopcheck() { # stopcheck [since]: any stop-condition line in the kernel log since the step began is fatal
	[ $DRY = 1 ] && { log "CHECK stopcheck since ${1:-$T0}"; return 0; }
	journalctl -k --since "${1:-$T0}" --no-pager | grep -E "$STOP_RE" | grep -vE 'r8169|mac80211|ieee80211|iwl_mld' > $EV/stopcheck.txt
	if [ -s $EV/stopcheck.txt ]; then log "stop condition lines:"; cat $EV/stopcheck.txt; halt "kernel log shows a stop condition (see $EV/stopcheck.txt)"; fi
	log "PASS stopcheck clean since ${1:-$T0}"
}
run() { # run NAME EXPECT_RC cmd...   output in $EV/NAME.txt, rc in $RC; EXPECT_RC "-" = do not judge
	local n=$1 want=$2; shift 2
	if [ $DRY = 1 ]; then log "+ $* > $EV/$n.txt   (expect rc $want)"; RC=0; return 0; fi
	"$@" > $EV/$n.txt 2>&1; RC=$?; echo $RC > $EV/$n.rc
	if [ "$want" = - ]; then log "rc=$RC  $n"; elif [ $RC = "$want" ]; then log "PASS $n (rc $RC)"; else log "FAIL $n (rc $RC, expected $want)"; FAILS=$((FAILS+1)); fi
}
has() { check "$1 prints /$2/" "grep -Eq -- '$2' $EV/$1.txt"; }
hasnt() { check "$1 does not print /$2/" "! grep -Eq -- '$2' $EV/$1.txt"; }
count() { check "$1: /$2/ appears $3 time(s)" "[ \"\$(grep -Ec -- '$2' $EV/$1.txt)\" = $3 ]"; }
rec() { grep -E "^$1=" ${2:-$STATE/driver-build} 2>/dev/null | head -1 | cut -d= -f2-; }
vms_stopped() { local v bad=0; for v in $TEST_VMS $LEGACY_VMS; do qm status $v 2>/dev/null | grep -q running && { log "VM$v is running"; bad=1; }; done; return $bad; }
down() { qm shutdown $1 --timeout 180 --forceStop 1 >/dev/null 2>&1; }
up() { qm start $1 >$EV/start-$1.out 2>&1; sleep 10; qm status $1 | grep -q running; }
down_all() { local v; for v in $TEST_VMS; do qm status $v | grep -q running && down $v; done; }
gwait() { local i; for i in $(seq 1 40); do ssh $SSHO root@${G[$1]}%vmbr0 true 2>/dev/null && return 0; sleep 3; done; return 1; }
cuda() { gwait $1 && ssh $SSHO root@${G[$1]}%vmbr0 'timeout 120 /root/geforce-sriov-cuda-probe 2>&1 | tail -1' 2>&1 | grep -q '^PASS'; }
gexec() { gwait $1 && ssh $SSHO root@${G[$1]}%vmbr0 "${@:2}"; }
agent() { local i; for i in $(seq 1 36); do qm guest cmd $1 ping >/dev/null 2>&1 && return 0; sleep 5; done; return 1; }
# A hash of everything install/uninstall owns, so "nothing changed" is a comparison, not a claim.
statehash() { # statehash LABEL -> $EV/state-LABEL.txt, prints a short hash
	[ $DRY = 1 ] && { echo dry; return 0; }
	{
		for d in /etc/geforce-sriov /usr/local/bin /usr/local/lib/geforce-sriov /usr/local/share/geforce-sriov /etc/modprobe.d /etc/kernel /etc/X11/xorg.conf.d; do
			[ -e $d ] && find $d -xdev \( -type f -o -type l \) -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum 2>/dev/null
		done
		[ -e $STATE ] && find $STATE -xdev -type f -not -path "$STATE/nvidia/*" -not -path "$STATE/.package/*" -not -path "$STATE/inputs/*" -not -name driver-step.log -not -path "$STATE/build/*" -print0 | sort -z | xargs -0 -r sha256sum
		find /etc/systemd/system -maxdepth 3 -name '*geforce-sriov*' -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum 2>/dev/null
		systemctl is-enabled geforce-sriov-livepatch geforce-sriov-activate 2>&1
		dkms status 2>&1 | grep -E 'geforce|nvidia'
		sha256sum /lib/modules/*/updates/dkms/*.ko 2>/dev/null
		sha256sum /usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so* 2>/dev/null
		readlink /usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so 2>/dev/null
	} > $EV/state-$1.txt 2>&1
	sha256sum < $EV/state-$1.txt | cut -c1-16
}
snap() { # snap LABEL: status, check (and doctor, only while the installed build is 0.3.x, which has it) and the numbers a reader compares across steps
	[ $DRY = 1 ] && { log "+ snap $1"; return 0; }
	{ echo "## status"; geforce-sriov status 2>&1; echo "rc=$?"; echo "## check"; geforce-sriov check 2>&1; echo "rc=$?"
	  if geforce-sriov --version 2>&1 | grep -q 'geforce-sriov 0\.3\.'; then echo "## doctor"; geforce-sriov doctor 2>&1; echo "rc=$?"; fi
	  echo "## record"; cat $STATE/driver-build 2>/dev/null; echo "## config"; cat $CFG 2>/dev/null; echo "## selection"; cat $STATE/selection 2>/dev/null
	  echo "## lsmod"; lsmod | grep -E 'nvidia|geforce' ; echo "## memory"; nvidia-smi --query-gpu=index,pci.bus_id,memory.used --format=csv 2>&1
	  echo "## vfs"; for c in $ADA $AMP; do echo "$c: $(ls -d /sys/bus/pci/devices/$c/virtfn* 2>/dev/null | wc -l) VFs"; done
	  echo "## units"; systemctl is-active geforce-sriov-livepatch geforce-sriov-activate lightdm nvidia-vgpud nvidia-vgpu-mgr 2>&1
	} > $EV/snap-$1.txt 2>&1
	log "snapshot $EV/snap-$1.txt"
}
nextboot() { # nextboot "why"  -> the one place that asks for a cold boot
	bootid > $E2E/LAST-REQUEST-BOOT
	echo "$(date -Is) COLD BOOT REQUESTED: $1 (evidence $EV)" >> $E2E/FINAL-STATE.log
	log "NEXT: cold power cycle ($1). Use the request-pve-reboot skill, then END THE TURN after QUEUED. After the boot run the next step of FINAL-PLAN.md."
}
