#!/bin/sh
# SPDX-License-Identifier: MIT
# Offline version-policy checks. Every package, source tree and patch result
# here is synthetic; no NVIDIA file, kernel module or device is involved.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
lib=$here/../../scripts/gs-driver-version.sh
[ -f "$lib" ] || { printf '%s\n' "missing library: $lib" >&2; exit 1; }
# shellcheck source=../../scripts/gs-driver-version.sh
. "$lib"
table=$here/../../scripts/gs-driver-versions.tbl
gs_versions_load "$table"
# Recorded identities, repeated here so the test fails if the table drifts.
expect_host=8fb252d5eafbb5740c9f623c881334119c532c1960834712956eb5e304e7ec41
expect_object_chain=4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449,ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701,f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12,946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29
expect_plugin_chain=7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7,7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6,aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c
expect_display_object=83ce12abba0441812aa135d4f099636513d709090c3da8999cda9243bd3b67ff
second_grid=8de9c10cfda77a89a8056acec476c13549f8f6dc7c7f9675da38dc9a73208a81
second_consumer=32c85d99b0f640c9501f61b39ddad208fd0288d015c4fbc5fd0435c07783fa77
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/work" "$tmp/bin"
failures=0
check() {
    if "$@"; then return 0; fi
    printf 'FAIL: %s\n' "$*" >&2
    failures=$((failures + 1))
}
expect_rc3() {
    rc=0
    "$@" >"$tmp/out" 2>"$tmp/err" || rc=$?
    check test "$rc" -eq 3
}
has_line() { grep -qx -- "$1" "$2"; }
lacks_line() { ! grep -qx -- "$1" "$2"; }
has_text() { grep -q -- "$1" "$2"; }

# A synthetic package: manifest version, Kbuild version, conftest kind flag.
make_pkg() {
    dir=$1 version=$2 kind=$3
    mkdir -p "$dir/kernel"
    printf 'NVIDIA Accelerated Graphics Driver for Linux-x86_64\n%s\n \nnvidia nvidia-vgpu-vfio\n' \
        "$version" >"$dir/.manifest"
    printf 'ccflags-y += -D__KERNEL__ -DMODULE -DNVRM\nccflags-y += -DNV_VERSION_STRING=\\"%s\\"\n' \
        "$version" >"$dir/kernel/Kbuild"
    {
        printf '#!/bin/sh\n# synthetic conftest\n'
        case $kind in
            host) printf 'VGX_KVM_BUILD=1\n' ;;
            grid-guest) printf 'GRID_BUILD=1\n' ;;
            consumer) printf '# GRID_BUILD parameter defined only for GRID builds\nGRID_BUILD_CSP=\n' ;;
        esac
    } >"$dir/kernel/conftest.sh"
}

# A synthetic .run: only the documented --extract-only interface, no installer.
make_run() {
    run=$1 src=$2
    {
        # shellcheck disable=SC2016
        printf '#!/bin/sh\nset -eu\n[ "$#" -eq 1 ] && [ "$1" = --extract-only ] || exit 2\n'
        printf 'cp -a %s ./package\n' "'$src'"
    } >"$run"
    chmod +x "$run"
}

make_pkg "$tmp/host" 580.159.01 host
make_pkg "$tmp/grid" 580.159.03 grid-guest
make_pkg "$tmp/consumer" 580.159.03 consumer
make_run "$tmp/host.run" "$tmp/host"
mkdir -p "$tmp/public-src"
printf 'NVIDIA_VERSION = 580.159.03\nNVIDIA_NVID_VERSION = 580.159.03\n' >"$tmp/public-src/version.mk"
mkdir -p "$tmp/tar/open-gpu-kernel-modules-580.159.03"
cp "$tmp/public-src/version.mk" "$tmp/tar/open-gpu-kernel-modules-580.159.03/version.mk"
tar -cf "$tmp/public.tar" -C "$tmp/tar" open-gpu-kernel-modules-580.159.03

# 1. Identity detection from metadata, not the file name.
identity() { gs_detect_identity "$@" >"$tmp/out" 2>"$tmp/err"; }

identity host "$tmp/host" "$tmp/work"
check has_line 'version=580.159.01' "$tmp/out"
check has_line 'kind=host' "$tmp/out"
identity host "$tmp/host.run" "$tmp/work"
check has_line 'version=580.159.01' "$tmp/out"
check has_line 'kind=host' "$tmp/out"
identity second "$tmp/grid" "$tmp/work"
check has_line 'version=580.159.03' "$tmp/out"
check has_line 'kind=grid-guest' "$tmp/out"
identity second "$tmp/consumer" "$tmp/work"
check has_line 'kind=consumer' "$tmp/out"
identity public "$tmp/public.tar" "$tmp/work"
check has_line 'version=580.159.03' "$tmp/out"
check has_line 'kind=public' "$tmp/out"
identity public "$tmp/public-src" "$tmp/work"
check has_line 'version=580.159.03' "$tmp/out"
check test -z "$(ls -A "$tmp/work")"

# 1b. Refusals: contradictory, missing, wrong role, unreadable, bad kind flags.
mkdir -p "$tmp/bad-version/kernel"
cp "$tmp/host/.manifest" "$tmp/bad-version/.manifest"
printf 'ccflags-y += -DNV_VERSION_STRING=\\"580.159.02\\"\n' >"$tmp/bad-version/kernel/Kbuild"
cp "$tmp/host/kernel/conftest.sh" "$tmp/bad-version/kernel/conftest.sh"
expect_rc3 gs_detect_identity host "$tmp/bad-version" "$tmp/work"
check has_text 'contradictory' "$tmp/err"
mkdir -p "$tmp/no-version/kernel"
cp "$tmp/host/kernel/conftest.sh" "$tmp/no-version/kernel/conftest.sh"
expect_rc3 gs_detect_identity host "$tmp/no-version" "$tmp/work"
check has_text 'missing or contradictory' "$tmp/err"
expect_rc3 gs_detect_identity host "$tmp/grid" "$tmp/work"
check has_text 'grid-guest' "$tmp/err"
expect_rc3 gs_detect_identity second "$tmp/host" "$tmp/work"
expect_rc3 gs_detect_identity host "$tmp/absent" "$tmp/work"
expect_rc3 gs_detect_identity other "$tmp/host" "$tmp/work"
mkdir -p "$tmp/two-kinds/kernel"
cp "$tmp/host/.manifest" "$tmp/two-kinds/.manifest"
cp "$tmp/host/kernel/Kbuild" "$tmp/two-kinds/kernel/Kbuild"
printf 'VGX_KVM_BUILD=1\nGRID_BUILD=1\n' >"$tmp/two-kinds/kernel/conftest.sh"
expect_rc3 gs_detect_identity host "$tmp/two-kinds" "$tmp/work"
check has_text 'contradictory package kind' "$tmp/err"
printf '#!/bin/sh\nexit 1\n' >"$tmp/broken.run"
expect_rc3 gs_detect_identity host "$tmp/broken.run" "$tmp/work"
check has_text 'extraction failed' "$tmp/err"
expect_rc3 gs_detect_identity public "$tmp/host" "$tmp/work"
check has_text 'binary package' "$tmp/err"

# 2. Classification: only the exact recorded chains are confirmed.
host_sha=$expect_host
object_chain=$expect_object_chain
plugin_chain=$expect_plugin_chain
display_chain=$object_chain,$expect_display_object
public_version=580.159.03
classify() { gs_classify "$@" >"$tmp/out" 2>"$tmp/err"; }
classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 0
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
check has_line 'evidence_profile=host-580.159.01-plain' "$tmp/out"
classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 0 confirmed
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 0 unconfirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
expect_rc3 gs_classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 0 anchor
expect_rc3 gs_classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 0 ''
classify "$host_sha" none none none "$object_chain" "$plugin_chain" 1 0
check has_line 'confirmation=unconfirmed' "$tmp/out"
check has_line 'evidence_profile=none' "$tmp/out"
classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 1
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" "$second_grid" none none "$object_chain" "$plugin_chain" 0 0
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" none none none "${object_chain},deadbeef" "$plugin_chain" 0 0
check has_line 'confirmation=unconfirmed' "$tmp/out"
expect_rc3 gs_classify "$host_sha" none none none "$object_chain" "$plugin_chain" 2 0
expect_rc3 gs_classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0

# 2b. The display profile: the four object revisions plus idx39, a recorded
# second package and the public 580.159.03 identity.
classify "$host_sha" "$second_grid" none "$public_version" "$display_chain" "$plugin_chain" 1 0 confirmed
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
check has_line 'evidence_profile=display-580.159.01-idx39' "$tmp/out"
classify "$host_sha" "$second_consumer" none "$public_version" "$display_chain" "$plugin_chain" 1 0 confirmed
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
classify "$host_sha" "$second_grid" none "$public_version" "$display_chain" "$plugin_chain" 1 0 unconfirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
check has_line 'evidence_profile=none' "$tmp/out"
# A different display output digest is not the recorded chain.
classify "$host_sha" "$second_grid" none "$public_version" "${object_chain},deadbeef" "$plugin_chain" 1 0 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
# A pinned public digest is not invented; only the version identity is accepted.
classify "$host_sha" "$second_grid" "$second_consumer" none "$display_chain" "$plugin_chain" 1 0 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" "$second_grid" none none "$display_chain" "$plugin_chain" 1 0 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" none none "$public_version" "$display_chain" "$plugin_chain" 1 0 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
# Display with host CUDA is its own recorded profile on the same chains; host CUDA alone is not.
classify "$host_sha" "$second_grid" none "$public_version" "$display_chain" "$plugin_chain" 1 1 confirmed
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
check has_line 'evidence_profile=display-cuda-580.159.01-idx39' "$tmp/out"
classify none "$second_grid" none "$public_version" "$display_chain" "$plugin_chain" 1 1 confirmed
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
classify "$host_sha" none none "$public_version" "$display_chain" "$plugin_chain" 1 1 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" "$second_grid" none "$public_version" "$display_chain" "$plugin_chain" 1 1 unconfirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" "$second_grid" none none "$object_chain" "$plugin_chain" 0 1 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify "$host_sha" "$second_grid" none "$public_version" "$display_chain" "$plugin_chain" 0 0 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"

# 2c. The recorded second-package digests identify an input, not a build.
gs_recorded_second "$second_grid" >"$tmp/out"
check has_line 'kind=grid-guest' "$tmp/out"
gs_recorded_second "$second_consumer" >"$tmp/out"
check has_line 'kind=consumer' "$tmp/out"
gs_recorded_second "$host_sha" >"$tmp/out"
check has_line 'kind=unknown' "$tmp/out"

# 3. Preflight against a fake patcher that follows the tool contract.
cat >"$tmp/bin/geforce-sriov-patch" <<'EOF'
#!/bin/sh
set -eu
[ "$#" -ge 3 ] || exit 2
queue=${FAKE_QUEUE:?}
line=$(sed -n '1p' "$queue")
tail -n +2 "$queue" >"$queue.next"
mv "$queue.next" "$queue"
pop() {
    head=${1%% *}
    case $1 in
        *' '*) rest=${1#* } ;;
        *) rest= ;;
    esac
}
pop "$line"
rev=$head
pop "$rest"
admission=$head
pop "$rest"
state=$head
family=$1
case $family in
    apply) sub=apply ;;
    ada|display) [ "$#" -ge 3 ] || exit 2; sub=$2; shift ;;
    *) exit 2 ;;
esac
[ "$sub" = apply ] || exit 2
[ "$#" -eq 3 ] || exit 2
case $rev in
    scheduler) [ "$family" = apply ] || exit 2 ;;
    display-idx39) [ "$family" = display ] || exit 2 ;;
    *) [ "$family" = ada ] || exit 2 ;;
esac
emit() {
    cp "$2" "$3"
    in_hash=$(sha256sum -- "$2" | cut -d ' ' -f 1)
    out_hash=$(sha256sum -- "$3" | cut -d ' ' -f 1)
    printf 'admission=%s\nstate=%s\nrevision=%s\nrecipe=fake\ninput_sha256=%s\noutput_sha256=%s\nfunction=fake\nfunction_offset=0x0\nfile_offset=0x0\n' \
        "$admission" "$state" "$rev" "$in_hash" "$out_hash"
}
[ -n "$rest" ] || { emit "$@"; exit 0; }
case $rest in
    fail*) printf '%s\n' "${rest#fail }" >&2; exit 3 ;;
    exit*) exit "${rest#exit }" ;;
    badhash*)
        cp "$2" "$3"
        printf 'admission=%s\nstate=%s\nrevision=%s\ninput_sha256=%s\noutput_sha256=%s\n' \
            "$admission" "$state" "$rev" 0000000000000000000000000000000000000000000000000000000000000000 \
            "$(sha256sum -- "$3" | cut -d ' ' -f 1)"
        exit 0 ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$tmp/bin/geforce-sriov-patch"
patcher=$tmp/bin/geforce-sriov-patch
object=$tmp/host/kernel/nv-kernel.o_binary
plugin=$tmp/libnvidia-vgpu.so.580.159.01
printf 'synthetic host object\n' >"$object"
printf 'synthetic plugin\n' >"$plugin"
object_before=$(gs_sha256 "$object")
plugin_before=$(gs_sha256 "$plugin")

# 3a. Every required revision confirmed, display off.
export FAKE_QUEUE="$tmp/queue"
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest confirmed
vf-count digest confirmed
plugin-mixed digest confirmed
plugin-div4 digest confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
check has_line 'preflight_state=confirmed' "$tmp/out"
for stage in scheduler module-gate vf-count plugin-mixed plugin-div4; do
    check has_line "stage=$stage" "$tmp/out"
done
check test "$(grep -c '^stage=' "$tmp/out")" -eq 5
check test ! -s "$tmp/err"
check test "$(gs_sha256 "$object")" = "$object_before"
check test "$(gs_sha256 "$plugin")" = "$plugin_before"
check test -z "$(ls -A "$tmp/work")"

# 3b. Display adds the idx 39 revision after the object revisions.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest unconfirmed:unknown
vf-count digest confirmed
plugin-mixed digest unconfirmed:unknown
plugin-div4 digest confirmed
display-idx39 digest confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 1 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
check has_line 'preflight_state=confirmed' "$tmp/out"
check has_line 'stage=display-idx39' "$tmp/out"
check test "$(grep -c '^stage=' "$tmp/out")" -eq 6
check test "$(tail -n 1 "$tmp/out")" = 'preflight_state=confirmed'
check test -z "$(ls -A "$tmp/work")"

# 3c. One failed anchor refuses everything and names the anchor.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest confirmed
vf-count digest confirmed fail anchor vf-count.context: no implemented structural locator for this input
plugin-mixed digest confirmed
plugin-div4 digest confirmed
display-idx39 digest confirmed
EOF
rc=0
gs_preflight "$patcher" "$object" "$plugin" 1 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3
check has_text 'anchor vf-count.context: no implemented structural locator for this input' "$tmp/err"
check lacks_line 'stage=vf-count' "$tmp/out"
check lacks_line 'stage=display-idx39' "$tmp/out"
check has_line 'stage=plugin-div4' "$tmp/out"
check test -z "$(ls -A "$tmp/work")"

# 3d. Any nonzero exit, not only 3, refuses the attempt.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest confirmed
vf-count digest confirmed
plugin-mixed digest confirmed exit 1
plugin-div4 digest confirmed
EOF
rc=0
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3
check has_text 'anchor plugin-mixed: patch command failed with exit 1' "$tmp/err"

# 3e. The intermediate rows report unconfirmed:unknown by design and do not
# make a fully digest-admitted chain unconfirmed.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest unconfirmed:unknown
vf-count digest confirmed
plugin-mixed digest unconfirmed:unknown
plugin-div4 digest confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
check has_line 'preflight_state=confirmed' "$tmp/out"
check test -z "$(ls -A "$tmp/work")"

# 3f. An unconfirmed final row makes the build unconfirmed.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest unconfirmed:unknown
vf-count digest unconfirmed:580.159.04
plugin-mixed digest unconfirmed:unknown
plugin-div4 digest confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
check has_line 'preflight_state=unconfirmed' "$tmp/out"
check test "$(grep -c '^Best effort: driver 580.159.01 is not guaranteed to work' "$tmp/err")" -eq 1
check test -z "$(ls -A "$tmp/work")"

# 3g. An anchor admission makes the build unconfirmed even with confirmed states.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler anchor confirmed
module-gate anchor unconfirmed:unknown
vf-count anchor confirmed
plugin-mixed anchor unconfirmed:unknown
plugin-div4 anchor confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
check has_line 'preflight_state=unconfirmed' "$tmp/out"

# 3g-bis. Only the display row admits by anchor: the build is unconfirmed.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest unconfirmed:unknown
vf-count digest confirmed
plugin-mixed digest unconfirmed:unknown
plugin-div4 digest confirmed
display-idx39 anchor confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 1 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
check has_line 'stage=display-idx39' "$tmp/out"
check has_line 'preflight_state=unconfirmed' "$tmp/out"
check test "$(grep -c '^Best effort: driver 580.159.01 is not guaranteed to work' "$tmp/err")" -eq 1

# 3h. A result that contradicts the actual digests refuses the attempt.
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest confirmed
vf-count digest confirmed badhash
plugin-mixed digest confirmed
plugin-div4 digest confirmed
EOF
rc=0
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3
check has_text 'anchor vf-count.contract' "$tmp/err"

# 3i. Unusable inputs refuse before running the patcher.
expect_rc3 gs_preflight "$tmp/bin/absent" "$object" "$plugin" 0 "$tmp/work" 580.159.01
expect_rc3 gs_preflight "$patcher" "$tmp/absent" "$plugin" 0 "$tmp/work" 580.159.01
expect_rc3 gs_preflight "$patcher" "$object" "$plugin" 2 "$tmp/work" 580.159.01

# 4. The notice is printed once, with the exact text, on stderr only.
unset GS_VERSION_NOTICE_SHOWN || :
gs_version_notice 580.159.04 2>"$tmp/err"
gs_version_notice 580.159.04 2>>"$tmp/err"
check test "$(grep -c . "$tmp/err")" -eq 1
check has_line 'Best effort: driver 580.159.04 is not guaranteed to work. Confirmed versions: 580.159.01. Required patch anchors matched; this does not establish runtime compatibility.' "$tmp/err"
gs_version_notice 580.159.04 >"$tmp/out" 2>/dev/null
check test ! -s "$tmp/out"

# 4b. A file guard makes the notice once across the preflight subshell.
export GS_VERSION_NOTICE_FILE="$tmp/notice"
cat >"$FAKE_QUEUE" <<'EOF'
scheduler digest confirmed
module-gate digest unconfirmed:unknown
vf-count digest unconfirmed:580.159.04
plugin-mixed digest unconfirmed:unknown
plugin-div4 digest confirmed
EOF
gs_preflight "$patcher" "$object" "$plugin" 0 "$tmp/work" 580.159.01 >"$tmp/out" 2>"$tmp/err"
gs_version_notice 580.159.01 2>>"$tmp/err"
check test "$(grep -c '^Best effort:' "$tmp/err")" -eq 1
unset GS_VERSION_NOTICE_FILE
check test -z "$(ls -A "$tmp/work")"

# 5. Record round trip, and old records read as not recorded.
gs_version_fields 580.159.01 none none confirmed-inputs host-580.159.01-plain 0 0 \
    "$host_sha" none none >"$tmp/fields"
gs_record_module nvidia 580.159.01 "$host_sha" >>"$tmp/fields"
gs_record_module nvidia-vgpu-vfio 580.159.01 "$host_sha" >>"$tmp/fields"
gs_record_emit <"$tmp/fields" >"$tmp/record"
check has_line 'driver_version=580.159.01' "$tmp/record"
check has_line 'driver_version_confirmed=1' "$tmp/record"
check has_line 'driver_version_policy=0.3.0' "$tmp/record"
check has_line "host_sha256=$host_sha" "$tmp/record"
check has_line 'second_sha256=none' "$tmp/record"
check has_line 'public_sha256=none' "$tmp/record"
check has_line 'display=0' "$tmp/record"
check has_line 'host_cuda=0' "$tmp/record"
check has_line 'confirmation=confirmed-inputs' "$tmp/record"
check has_line 'evidence_profile=host-580.159.01-plain' "$tmp/record"
check has_line 'module_nvidia_stamp=580.159.01' "$tmp/record"
check has_line "module_nvidia_vgpu_vfio_sha256=$host_sha" "$tmp/record"
gs_record_read "$tmp/record" >"$tmp/out"
check has_line 'record_state=recorded' "$tmp/out"
check has_line 'driver_version_confirmed=1' "$tmp/out"

gs_version_fields 580.159.04 none none unconfirmed none 1 1 \
    "$host_sha" "$second_consumer" none >"$tmp/fields"
gs_record_emit <"$tmp/fields" >"$tmp/record-unconfirmed"
check has_line 'driver_version_confirmed=0' "$tmp/record-unconfirmed"
check has_line 'confirmation=unconfirmed' "$tmp/record-unconfirmed"
gs_record_read "$tmp/record-unconfirmed" >"$tmp/out"
check has_line 'record_state=recorded' "$tmp/out"

# 5a. The display record keeps the public version and no public digest.
gs_version_fields 580.159.01 580.159.03 580.159.03 confirmed-inputs display-580.159.01-idx39 1 0 \
    "$host_sha" "$second_grid" none >"$tmp/fields"
gs_record_emit <"$tmp/fields" >"$tmp/record-display"
check has_line 'driver_version_confirmed=1' "$tmp/record-display"
check has_line 'driver_second_version=580.159.03' "$tmp/record-display"
check has_line 'driver_public_version=580.159.03' "$tmp/record-display"
check has_line 'public_sha256=none' "$tmp/record-display"
check has_line "second_sha256=$second_grid" "$tmp/record-display"
check has_line 'display=1' "$tmp/record-display"
check has_line 'host_cuda=0' "$tmp/record-display"
check has_line 'confirmation=confirmed-inputs' "$tmp/record-display"
check has_line 'evidence_profile=display-580.159.01-idx39' "$tmp/record-display"
gs_record_read "$tmp/record-display" >"$tmp/out"
check has_line 'record_state=recorded' "$tmp/out"

printf 'kernel=7.0.14-16-pve\nmodule_sha256=5efecf2946e318419bdc52af608641ce93a3b66626865f7f9a3763b3bc1b86b2\nmodule_build_id=9271bc1b91b68522421eac69b71429744e53079f\nstock_sha256=5ad2b281ba7a63768bc3c98857475f52a4de2ce7941c0e18db365f15682259fb\n' >"$tmp/old-record"
gs_record_read "$tmp/old-record" >"$tmp/out"
check has_line 'record_state=not-recorded' "$tmp/out"
check has_line 'kernel=7.0.14-16-pve' "$tmp/out"
check has_line 'module_build_id=9271bc1b91b68522421eac69b71429744e53079f' "$tmp/out"
check test "$(gs_record_emit <"$tmp/old-record")" = "$(cat "$tmp/old-record")"
expect_rc3 gs_record_read "$tmp/absent-record"

# 5b. Record validation rejects duplicates, malformed lines and CR.
printf 'kernel=a\nkernel=b\n' >"$tmp/dup"
rc=0
gs_record_emit <"$tmp/dup" >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3
printf 'Kernel=a\n' >"$tmp/malformed"
rc=0
gs_record_emit <"$tmp/malformed" >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3
printf 'kernel=a\r\n' >"$tmp/cr"
rc=0
gs_record_emit <"$tmp/cr" >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3
printf 'no separator\n' >"$tmp/nosep"
rc=0
gs_record_emit <"$tmp/nosep" >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 3

# 6. The table of confirmed versions. A block sets the names of its version, and a version with
# no block has none.
gs_version_row 580.159.01
check test "$gs_host_version/$gs_host_package" = "580.159.01/$expect_host"
check test "$gs_object_chain" = "$expect_object_chain"
check test "$gs_object_chain_display" = "$expect_object_chain,$expect_display_object"
check test "$gs_plugin_chain" = "$expect_plugin_chain"
check test "$gs_second_version/$gs_second_grid/$gs_second_consumer" = "580.159.03/$second_grid/$second_consumer"
check test "$gs_plain_offsets" = '0x14a7c4 0x14a7c5 0x14a7c6 0x14a7c7 0xa16708 0xa16709 0xd8966f 0xd89670'
rc=0
gs_version_row 580.159.04 || rc=$?
check test "$rc" -eq 1
check test -z "$gs_host_version$gs_host_package$gs_stock_object$gs_object_chain$gs_plugin_chain$gs_plain_offsets$gs_profile_plain"
check test "$(gs_source_version "$tmp/host")" = 580.159.01
check test "$(gs_source_version "$tmp/host/kernel")" = 580.159.01
expect_rc3 gs_source_version "$tmp/no-version"
expect_rc3 gs_source_version "$tmp/absent"

# 6a. A second block is one more confirmed version: it is confirmed by its own digests and
# profile, never by a mix with another block's, and the notice lists both.
digest() { printf '%064d' "$1"; }
{
    cat "$table"
    printf '\n# A synthetic second version.\nhost=999.1.2\npackage=%s\n' "$(digest 1)"
    printf 'stock_object=%s\nscheduler_object=%s\ngate_object=%s\npatched_object=%s\n' \
        "$(digest 2)" "$(digest 3)" "$(digest 4)" "$(digest 5)"
    printf 'stock_plugin=%s\nmixed_plugin=%s\npatched_plugin=%s\n' "$(digest 6)" "$(digest 7)" "$(digest 8)"
    printf 'plain_offsets=0x10 0x11 0x12 0x13 0x20 0x21 0x30 0x31\n'
    printf 'second=999.1.3\nsecond_grid=%s\nprofile_plain=host-999.1.2-plain\n' "$(digest 9)"
} >"$tmp/two.tbl"
gs_versions_load "$tmp/two.tbl"
check test "$gs_confirmed_hosts" = '580.159.01 999.1.2'
check test "$gs_confirmed_host_versions" = '580.159.01, 999.1.2'
synth_objects=$(digest 2),$(digest 3),$(digest 4),$(digest 5)
synth_plugins=$(digest 6),$(digest 7),$(digest 8)
classify "$(digest 1)" none none none "$synth_objects" "$synth_plugins" 0 0
check has_line 'confirmation=confirmed-inputs' "$tmp/out"
check has_line 'evidence_profile=host-999.1.2-plain' "$tmp/out"
classify none none none none "$synth_objects" "$synth_plugins" 0 0 confirmed
check has_line 'evidence_profile=host-999.1.2-plain' "$tmp/out"
classify "$host_sha" none none none "$object_chain" "$plugin_chain" 0 0
check has_line 'evidence_profile=host-580.159.01-plain' "$tmp/out"
# The package of one block with the chains of the other, and chains of two blocks together.
classify "$host_sha" none none none "$synth_objects" "$synth_plugins" 0 0
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify none none none none "$synth_objects" "$plugin_chain" 0 0
check has_line 'confirmation=unconfirmed' "$tmp/out"
classify none none none none "$object_chain" "$synth_plugins" 0 0
check has_line 'confirmation=unconfirmed' "$tmp/out"
# The synthetic block names no display profile and no display object: display is not confirmed.
classify none "$(digest 9)" none 999.1.3 "$synth_objects,$(digest 10)" "$synth_plugins" 1 0 confirmed
check has_line 'confirmation=unconfirmed' "$tmp/out"
check has_line 'evidence_profile=none' "$tmp/out"
gs_recorded_second "$(digest 9)" >"$tmp/out"
check has_line 'kind=grid-guest' "$tmp/out"
gs_recorded_second "$second_consumer" >"$tmp/out"
check has_line 'kind=consumer' "$tmp/out"
unset GS_VERSION_NOTICE_SHOWN || :
gs_version_notice 777.0.1 2>"$tmp/err"
check has_line 'Best effort: driver 777.0.1 is not guaranteed to work. Confirmed versions: 580.159.01, 999.1.2. Required patch anchors matched; this does not establish runtime compatibility.' "$tmp/err"
# A package named as either version must have that version's digest; any other name is a notice.
printf 'not the package\n' >"$tmp/NVIDIA-Linux-x86_64-999.1.2-vgpu-kvm.run"
expect_rc3 gs_package_check host "$tmp/NVIDIA-Linux-x86_64-999.1.2-vgpu-kvm.run"
check has_text "named as NVIDIA's 999.1.2 package" "$tmp/err"
cp "$tmp/NVIDIA-Linux-x86_64-999.1.2-vgpu-kvm.run" "$tmp/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run"
expect_rc3 gs_package_check host "$tmp/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run"
check has_text "named as NVIDIA's 580.159.01 package" "$tmp/err"
cp "$tmp/NVIDIA-Linux-x86_64-999.1.2-vgpu-kvm.run" "$tmp/NVIDIA-Linux-x86_64-999.1.3-grid.run"
expect_rc3 gs_package_check second "$tmp/NVIDIA-Linux-x86_64-999.1.3-grid.run"
cp "$tmp/NVIDIA-Linux-x86_64-999.1.2-vgpu-kvm.run" "$tmp/other.run"
rc=0
gs_package_check host "$tmp/other.run" >"$tmp/out" 2>"$tmp/err" || rc=$?
check test "$rc" -eq 0
check has_text 'Notice: no digest is recorded for' "$tmp/err"

# 6b. A table that cannot be used is refused when it is loaded.
bad_table() {
    { cat "$table"; printf '%s\n' "$@"; } >"$tmp/bad.tbl"
    expect_rc3 gs_versions_load "$tmp/bad.tbl"
}
bad_table 'host=999.1.2' 'plain_offsets=0x10 0x11 0x12 0x13 0x20 0x21 0x30'
check has_text 'eight ascending offsets' "$tmp/err"
bad_table 'host=999.1.2' 'plain_offsets=0x10 0x11 0x12 0x13 0x20 0x21 0x31 0x30'
bad_table 'host=999.1.2' 'plain_offsets=0x10 0x11 0x12 0x13 0x20 0x21 0x30 0X31'
bad_table 'host=999.1.2' 'no_such_field=1'
check has_text 'not a field of a block' "$tmp/err"
bad_table 'host=580.159.01'
check has_text 'two blocks for 580.159.01' "$tmp/err"
bad_table 'host=nine'
expect_rc3 gs_versions_load "$tmp/absent.tbl"
: >"$tmp/empty.tbl"
expect_rc3 gs_versions_load "$tmp/empty.tbl"
gs_versions_load "$table"

if [ "$failures" -ne 0 ]; then
    printf '%s checks failed\n' "$failures" >&2
    exit 1
fi
printf '%s\n' 'offline version-policy checks passed'
