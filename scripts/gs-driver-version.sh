#!/bin/sh
# SPDX-License-Identifier: MIT
# Source-only library for the version policy of the driver build step. It has
# no top-level side effects; source it and call the functions. Every function
# returns 0 on success and 3 on refusal, and writes the reason to stderr.
#
# gs_versions_load FILE
#   Names the table of confirmed versions (gs-driver-versions.tbl, installed beside this
#   library) and checks every block of it. Call it once, before any function below that
#   needs a recorded digest. Sets gs_confirmed_hosts (the host versions, separated by
#   spaces, in table order) and gs_confirmed_host_versions (the same as the messages list
#   them: "A" or "A, B").
#
# gs_version_row VERSION
#   Sets the gs_* names of the block of host version VERSION: gs_host_version,
#   gs_host_package, the object and plugin digests, gs_object_chain,
#   gs_object_chain_display and gs_plugin_chain, gs_plain_offsets, the second package and
#   public source identities and the evidence profiles. Returns 1, with every name empty,
#   for a version that has no block: such a version has no recorded digest and is built
#   best effort.
#
# gs_source_version DIR
#   Prints the one version an extracted host tree declares (DIR, or DIR/kernel when it is
#   a package root). It only reads; it needs no scratch directory.
#
# gs_detect_identity ROLE INPUT WORK
#   ROLE is host, second or public. INPUT is an extracted directory, a vGPU or
#   guest .run package (host, second) or a public source tarball or directory
#   (public). WORK is an existing private scratch directory. Prints
#   version=<v> and kind=<host|grid-guest|consumer|public>. The version comes
#   from package metadata and source definitions, never the file name. A .run
#   is unpacked by running its own installer with --extract-only inside WORK;
#   only pass a package you trust and have pinned.
#
# gs_classify HOST_SHA SECOND_SHA PUBLIC_SHA PUBLIC_VERSION OBJECT_CHAIN PLUGIN_CHAIN DISPLAY CUDA [PREFLIGHT_STATE [ADA]]
#   Chains are comma-separated whole-file SHA-256 values in revision order,
#   including both endpoints; use none for an absent input or chain. DISPLAY
#   and CUDA are 0 or 1. Prints confirmation=confirmed-inputs|unconfirmed and
#   evidence_profile=<id>|none. Confirmed-inputs requires every input digest
#   and the complete revision chain to match one recorded tested combination.
#   The optional last argument, the preflight state, must also be confirmed;
#   any other value forces unconfirmed. Record confirmation= from the output
#   of this function, never from a version string. ADA is 1 when an Ada card is selected and the
#   build carries the ada-placement row, 0 otherwise (0 when not given); the display profile of an
#   Ada build is the one that ends in placement_display_object.
#
#   Every block of the table is tried in order. The object and plugin chains are the
#   identity: the host package digest is recorded when a .run is supplied and absent for
#   an extracted tree, and neither is required for a profile. A block names up to three
#   profiles: the plain host build; the display build (the four object revisions plus
#   idx39, the three plugin revisions, a recorded second package and the public NVKMS
#   source at the block's public version); and the display build with nvidia-uvm (host
#   CUDA) built from the second package. No whole-file digest of the public source is
#   recorded, so a display profile requires its version as an identity (PUBLIC_VERSION)
#   and refuses a PUBLIC_SHA value; a digest is not invented here. A combination whose
#   profile the block does not name stays unconfirmed, as host CUDA without display does.
#
# gs_preflight PATCHER OBJECT PLUGIN DISPLAY WORK VERSION [ADA]
#   Runs every required revision through PATCHER in a scratch directory under
#   WORK and discards the outputs. Prints one block per revision (stage= then
#   the patcher's key=value lines) and finally preflight_state=confirmed or
#   preflight_state=unconfirmed. Any nonzero patcher exit, or a result block
#   that does not match the contract or the actual output digest, refuses the
#   whole attempt (return 3) and names each failed anchor on stderr. It never
#   changes an installed file.
#
#   preflight_state is confirmed only when every revision was admitted by its
#   digest and every final revision reports state=confirmed. Two rows report
#   state=unconfirmed:unknown by design, because their output was never
#   measured on its own: plugin-mixed (stock plugin to placement-only), which
#   every chain passes through, and the module-gate row that takes the stock
#   object without the scheduler revision, which the install chain never uses
#   (its module-gate step takes the scheduler output and reports confirmed). The
#   ada-placement row, run only when ADA is 1 (0 when it is not given), was
#   measured on hardware and reports confirmed when admitted by digest; the build record
#   names it (ada_placement=1).
#   That state is not an error and must not make a confirmed input
#   combination unconfirmed. Any admission=anchor makes the
#   build unconfirmed, whatever the states say.
#
#   An unconfirmed result prints the best-effort notice for VERSION once, to
#   stderr, through gs_version_notice.
#
# gs_recorded_second SHA
#   Prints kind=grid-guest, kind=consumer or kind=unknown for a second-package
#   digest of any block. It identifies a recorded input; it does not confirm a build.
#
# gs_version_notice VERSION
#   Prints the best-effort notice to stderr once and does nothing after that.
#   The guard is GS_VERSION_NOTICE_FILE when the caller sets it (a path in its
#   scratch directory, which also survives the preflight subshell), otherwise
#   a shell variable. Call it once, only when the build is unconfirmed.
#
# gs_version_fields H S P CONFIRMATION PROFILE DISPLAY CUDA HOST_SHA SECOND_SHA PUBLIC_SHA
#   Prints the version-policy field lines for the driver-build record. Pass
#   none for an absent S, P, PROFILE or digest. For a display build P is the
#   public source version and PUBLIC_SHA is none, because no whole-file digest
#   of that source was recorded. Pipe through gs_record_emit.
#
# gs_record_module NAME STAMP SHA256
#   Prints the per-module stamp and digest fields; '-' in NAME becomes '_'.
#
# gs_record_emit
#   Reads candidate key=value lines on stdin, rejects malformed lines and
#   duplicate keys, and prints the accepted record. Redirect it to the record
#   file. Values are data and are never evaluated.
#
# gs_record_read FILE
#   Prints the validated record and then record_state=recorded, or
#   record_state=not-recorded when a field this policy writes is absent. An
#   old record is reported as not recorded, never as confirmed.
#
# gs_sha256 FILE
#   Prints the whole-file SHA-256, or refuses.

GS_VERSION_POLICY=0.3.0

_gs_refuse() {
    printf 'version policy: %s\n' "$*" >&2
    return 3
}

# The recorded identities are in the table of confirmed versions, one block per host version, and
# nothing in the shell repeats them. The patch tool (driver/*.c) holds the input and output digest
# of each revision in its own tables; those are the source, and the tool reports any mismatch as
# admission=anchor.
_gs_row_clear() {
    gs_host_version='' gs_host_package=''
    gs_stock_object='' gs_scheduler_object='' gs_gate_object='' gs_patched_object='' gs_display_object=''
    gs_placement_object='' gs_placement_display_object='' gs_placement_offsets=''
    gs_stock_plugin='' gs_mixed_plugin='' gs_patched_plugin=''
    gs_plain_offsets=''
    gs_second_version='' gs_second_grid='' gs_second_consumer='' gs_public_version=''
    gs_profile_plain='' gs_profile_display='' gs_profile_display_cuda=''
    gs_object_chain='' gs_object_chain_display='' gs_object_chain_placement_display='' gs_plugin_chain=''
}

gs_version_row() {
    _gs_row_clear
    [ "$#" -eq 1 ] || { _gs_refuse 'a table row is selected by one version'; return 3; }
    [ -n "${_gs_versions_file:-}" ] || { _gs_refuse 'the table of confirmed versions was not loaded'; return 3; }
    case $1 in
        ''|*[!0-9.]*) return 1 ;;
    esac
    _gs_row=$(awk -v want="host=$1" '
        /^host=/ { on = ($0 == want) }
        on && !/^#/ && !/^[ \t]*$/ { print }' "$_gs_versions_file") ||
        { _gs_refuse "cannot read the table of confirmed versions: $_gs_versions_file"; return 3; }
    [ -n "$_gs_row" ] || return 1
    # shellcheck disable=SC2034 # the fields are read by the driver step
    while IFS= read -r _gs_line; do
        _gs_value=${_gs_line#*=}
        case ${_gs_line%%=*} in
            host) gs_host_version=$_gs_value ;;
            package) gs_host_package=$_gs_value ;;
            stock_object) gs_stock_object=$_gs_value ;;
            scheduler_object) gs_scheduler_object=$_gs_value ;;
            gate_object) gs_gate_object=$_gs_value ;;
            patched_object) gs_patched_object=$_gs_value ;;
            display_object) gs_display_object=$_gs_value ;;
            placement_object) gs_placement_object=$_gs_value ;;
            placement_display_object) gs_placement_display_object=$_gs_value ;;
            placement_offsets) gs_placement_offsets=$_gs_value ;;
            stock_plugin) gs_stock_plugin=$_gs_value ;;
            mixed_plugin) gs_mixed_plugin=$_gs_value ;;
            patched_plugin) gs_patched_plugin=$_gs_value ;;
            plain_offsets) gs_plain_offsets=$_gs_value ;;
            second) gs_second_version=$_gs_value ;;
            second_grid) gs_second_grid=$_gs_value ;;
            second_consumer) gs_second_consumer=$_gs_value ;;
            public) gs_public_version=$_gs_value ;;
            profile_plain) gs_profile_plain=$_gs_value ;;
            profile_display) gs_profile_display=$_gs_value ;;
            profile_display_cuda) gs_profile_display_cuda=$_gs_value ;;
            # Read by the build for the CLI's rows; the driver step has no use for them.
            guest_ampere|guest_ada) ;;
            *) _gs_row_clear
                _gs_refuse "the table of confirmed versions has a line that is not a field of a block, in the block of $1: $_gs_line"
                return 3 ;;
        esac
    done <<EOF
$_gs_row
EOF
    # The plain build's module is compared at these offsets as text, so they have one spelling.
    for _gs_field in plain_offsets:eight:8 placement_offsets:one:1; do
        _gs_name=${_gs_field%%:*} _gs_want=${_gs_field##*:} _gs_word=${_gs_field#*:} _gs_word=${_gs_word%:*}
        eval "_gs_list=\$gs_$_gs_name"
        _gs_count=0 _gs_last=-1
        for _gs_at in $_gs_list; do
            case $_gs_at in
                0x*[!0-9a-f]*|0x) _gs_last=bad ;;
                0x*) [ "$(printf '0x%x' "$((_gs_at))")" = "$_gs_at" ] && [ "$((_gs_at))" -gt "$_gs_last" ] || _gs_last=bad ;;
                *) _gs_last=bad ;;
            esac
            [ "$_gs_last" != bad ] || break
            _gs_last=$((_gs_at)) _gs_count=$((_gs_count + 1))
        done
        if [ -n "$_gs_list" ] && { [ "$_gs_last" = bad ] || [ "$_gs_count" -ne "$_gs_want" ]; }; then
            _gs_row_clear
            if [ "$_gs_want" -eq 1 ]; then
                _gs_refuse "the table of confirmed versions: $_gs_name of $1 must be one offset, written as 0x and lowercase hexadecimal digits"
            else
                _gs_refuse "the table of confirmed versions: $_gs_name of $1 must be $_gs_word ascending offsets, each written as 0x and lowercase hexadecimal digits"
            fi
            return 3
        fi
    done
    # The chains in revision order, both endpoints included. A block that lacks one digest of a
    # chain has no such chain, and nothing is confirmed against it.
    if [ -n "$gs_stock_object" ] && [ -n "$gs_scheduler_object" ] && [ -n "$gs_gate_object" ] && [ -n "$gs_patched_object" ]; then
        gs_object_chain=$gs_stock_object,$gs_scheduler_object,$gs_gate_object,$gs_patched_object
        [ -z "$gs_display_object" ] || gs_object_chain_display=$gs_object_chain,$gs_display_object
        [ -z "$gs_placement_display_object" ] || gs_object_chain_placement_display=$gs_object_chain,$gs_placement_display_object
    fi
    if [ -n "$gs_stock_plugin" ] && [ -n "$gs_mixed_plugin" ] && [ -n "$gs_patched_plugin" ]; then
        gs_plugin_chain=$gs_stock_plugin,$gs_mixed_plugin,$gs_patched_plugin
    fi
}

gs_versions_load() {
    [ "$#" -eq 1 ] && [ -f "$1" ] && [ -r "$1" ] ||
        { _gs_refuse "the table of confirmed versions is missing: ${1:-}"; return 3; }
    _gs_versions_file=$1
    gs_confirmed_hosts=$(sed -n 's/^host=//p' "$1" | tr '\n' ' ') ||
        { _gs_refuse "cannot read the table of confirmed versions: $1"; return 3; }
    gs_confirmed_hosts=${gs_confirmed_hosts% }
    [ -n "$gs_confirmed_hosts" ] || { _gs_refuse "the table of confirmed versions names no version: $1"; return 3; }
    _gs_seen=' '
    for _gs_host in $gs_confirmed_hosts; do
        case $_gs_host in
            *[!0-9.]*) _gs_refuse "the table of confirmed versions names an invalid version: $_gs_host"; return 3 ;;
        esac
        case $_gs_seen in
            *" $_gs_host "*) _gs_refuse "the table of confirmed versions has two blocks for $_gs_host"; return 3 ;;
        esac
        _gs_seen="$_gs_seen$_gs_host "
        gs_version_row "$_gs_host" || { _gs_refuse "the table of confirmed versions cannot be used: $1"; return 3; }
    done
    _gs_row_clear
    gs_confirmed_host_versions=$(printf '%s' "$gs_confirmed_hosts" | sed 's/ /, /g')
}

# gs_record_get FILE KEY
#   Prints the value of KEY in a record or a journal (key=value lines); nothing when the key is
#   absent. A key that is not a field name is refused, so a name taken from a file cannot become
#   part of a pattern.
gs_record_get() {
    case $2 in
        ''|*[!a-z0-9_]*) _gs_refuse "invalid record field: $2"; return 3 ;;
    esac
    sed -n "s/^$2=//p" "$1"
}

# gs_record_modules FILE
#   Prints the modules a record or a journal names, one per line, as they appear in its field
#   names ('-' is '_').
gs_record_modules() {
    sed -n 's/^module_\([a-z0-9_]*\)_path=.*/\1/p' "$1"
}

gs_sha256() {
    [ "$#" -eq 1 ] && [ -f "$1" ] && [ -r "$1" ] || { _gs_refuse "unreadable input: $1"; return 3; }
    _gs_sum=$(sha256sum -- "$1") || { _gs_refuse "cannot hash: $1"; return 3; }
    printf '%s\n' "${_gs_sum%% *}"
}

# Print the one version the input declares, or refuse when it declares none or
# more than one. The caller has already chosen $root and $kernel. Nothing is written.
_gs_read_version() {
    _gs_root=$1
    _gs_kernel=$2
    _gs_versions=
    if [ -f "$_gs_root/.manifest" ]; then
        [ -r "$_gs_root/.manifest" ] || { _gs_refuse "unreadable manifest: $_gs_root/.manifest"; return 3; }
        _gs_found=$(awk 'NR == 2 { gsub(/^[ \t]+|[ \t\r]+$/, ""); if ($0 ~ /^[0-9]+\.[0-9]+(\.[0-9]+)+$/) print; else exit 1 }' \
            "$_gs_root/.manifest") || { _gs_refuse "invalid manifest version: $_gs_root/.manifest"; return 3; }
        _gs_versions=$_gs_found
    fi
    for _gs_file in "$_gs_root/version.mk" "$_gs_kernel/Kbuild"; do
        [ -e "$_gs_file" ] || continue
        [ -r "$_gs_file" ] || { _gs_refuse "unreadable version definition: $_gs_file"; return 3; }
        _gs_found=$(awk '/^[ \t]*NVIDIA_(NVID_)?VERSION[ \t]*[:?+]?=/ || /-DNV_VERSION_STRING=/ {
                 line = $0; gsub(/\\/, "", line); gsub(/"/, "", line);
                 if (match(line, /[0-9]+\.[0-9]+(\.[0-9]+)+/)) print substr(line, RSTART, RLENGTH); else exit 1
             }' "$_gs_file") || { _gs_refuse "invalid version definition: $_gs_file"; return 3; }
        _gs_versions="$_gs_versions
$_gs_found"
    done
    _gs_unique=$(printf '%s\n' "$_gs_versions" | sed '/^$/d' | sort -u) ||
        { _gs_refuse 'cannot read version definitions'; return 3; }
    [ -n "$_gs_unique" ] && [ "$(printf '%s\n' "$_gs_unique" | wc -l)" -eq 1 ] ||
        { _gs_refuse "missing or contradictory versions in $1"; return 3; }
    printf '%s\n' "$_gs_unique"
}

gs_source_version() {
    [ "$#" -eq 1 ] && [ -d "$1" ] || { _gs_refuse "not a source directory: ${1:-}"; return 3; }
    if [ -d "$1/kernel" ]; then
        _gs_read_version "$1" "$1/kernel"
    else
        _gs_read_version "$1" "$1"
    fi
}

# gs_package_check ROLE FILE [NAME]
#   ROLE is host or second. A .run package is a shell script, so extracting it runs it as root.
#   Before that happens, a file with a digest the table records for that role is accepted. A file
#   whose name states a confirmed version of that role and has another digest is refused (return
#   3) and not run. A file with any other name is run after a notice that names it and its
#   digest, because no digest is recorded for it.
#   The check holds for the bytes that are run only when FILE cannot change in between: the caller
#   passes a copy of its own, which nobody else can write to, and runs that copy. NAME is the file
#   the copy was made from, for the messages.
gs_package_check() (
    [ "$#" -eq 2 ] || [ "$#" -eq 3 ] || { _gs_refuse 'package check requires a role and a file'; exit 3; }
    role=$1 file=$2 shown=${3:-$2}
    case $role in
        host|second) ;;
        *) _gs_refuse "unknown package role: $role"; exit 3 ;;
    esac
    have=$(gs_sha256 "$file") || exit 3
    named=
    for version in $gs_confirmed_hosts; do
        gs_version_row "$version" || continue
        if [ "$role" = host ]; then
            want=$gs_host_version
            set -- "$gs_host_package"
        else
            want=$gs_second_version
            set -- "$gs_second_grid" "$gs_second_consumer"
        fi
        for digest do
            [ -z "$digest" ] || [ "$have" != "$digest" ] || exit 0
        done
        [ -n "$want" ] || continue
        case ${shown##*/} in
            *"$want"*) [ -n "$named" ] || named=$want ;;
        esac
    done
    if [ -n "$named" ]; then
        _gs_refuse "${shown##*/} is named as NVIDIA's $named package, but its SHA-256 is $have, which is not a recorded digest of that package. It was not run. Download it again from NVIDIA, or rename the file if it is a different package"
        exit 3
    fi
    printf 'Notice: no digest is recorded for %s (SHA-256 %s). It is a shell script and is run to extract it, so use only a package you trust.\n' \
        "$shown" "$have" >&2
)

gs_detect_identity() (
    [ "$#" -eq 3 ] || { _gs_refuse 'identity requires role, input and work directory'; exit 3; }
    role=$1 input=$2 work=$3
    case $role in
        host|second|public) ;;
        *) _gs_refuse "unknown identity role: $role"; exit 3 ;;
    esac
    [ -d "$work" ] || { _gs_refuse "work directory missing: $work"; exit 3; }
    [ -r "$input" ] || { _gs_refuse "unreadable input: $input"; exit 3; }
    temp=$(mktemp -d "$work/identity.XXXXXXXX") || { _gs_refuse 'cannot create identity scratch directory'; exit 3; }
    trap 'rm -rf -- "$temp"' EXIT HUP INT TERM
    if [ -d "$input" ]; then
        root=$input
    elif [ "$role" = public ]; then
        [ -f "$input" ] || { _gs_refuse "not a source archive: $input"; exit 3; }
        tar -tf "$input" >"$temp/list" 2>/dev/null || { _gs_refuse "unreadable source archive: $input"; exit 3; }
        awk '/(^|\/)version\.mk$/ { print }' "$temp/list" >"$temp/names" ||
            { _gs_refuse "cannot read the archive listing: $input"; exit 3; }
        [ "$(wc -l <"$temp/names")" -eq 1 ] || { _gs_refuse "source archive must hold exactly one version.mk: $input"; exit 3; }
        name=$(cat "$temp/names")
        tar -xOf "$input" -- "$name" >"$temp/version.mk" 2>/dev/null ||
            { _gs_refuse "cannot read the source version: $input"; exit 3; }
        root=$temp
    else
        [ -f "$input" ] || { _gs_refuse "not a package: $input"; exit 3; }
        case $input in /*) ;; *) input=$(CDPATH='' cd -- "$(dirname -- "$input")" && pwd)/$(basename -- "$input") ||
            { _gs_refuse "cannot resolve the package path: $input"; exit 3; } ;; esac
        # The file that is checked is the file that is run: a copy in this scratch directory.
        package=$temp/package/${input##*/}
        mkdir "$temp/package" "$temp/extracted" && cp -P -- "$(readlink -f -- "$input")" "$package" &&
            [ -f "$package" ] && [ ! -L "$package" ] ||
            { _gs_refuse "cannot copy the package: $input"; exit 3; }
        gs_package_check "$role" "$package" "$input" || exit 3
        ( cd "$temp/extracted" && sh "$package" --extract-only ) >"$temp/extract.log" 2>&1 ||
            { _gs_refuse "package extraction failed: $input"; exit 3; }
        found=0
        for candidate in "$temp"/extracted/*/; do
            [ -d "$candidate" ] || continue
            found=$((found + 1))
            root=${candidate%/}
        done
        [ "$found" -eq 1 ] || { _gs_refuse "package extraction produced $found directories: $input"; exit 3; }
    fi
    kernel=$root
    [ ! -d "$root/kernel" ] || kernel=$root/kernel
    if [ "$role" = public ] && [ -f "$root/.manifest" ]; then
        _gs_refuse "public source must be source, not a binary package: $input"
        exit 3
    fi
    version=$(_gs_read_version "$root" "$kernel") || exit 3
    kind=$role
    if [ "$role" != public ]; then
        file=$kernel/conftest.sh
        [ -r "$file" ] || { _gs_refuse "missing package kind metadata: $file"; exit 3; }
        flags=$(awk '/^[ \t]*(VGX_KVM_BUILD|VGX_BUILD|GRID_BUILD)[ \t]*=[ \t]*1([ \t]*|[ \t]*#.*)$/ { sub(/=.*/, ""); gsub(/[ \t]/, ""); print }' "$file" | sort -u) ||
            { _gs_refuse "cannot read package kind metadata: $file"; exit 3; }
        case $flags in
            VGX_KVM_BUILD|VGX_BUILD) kind=host ;;
            GRID_BUILD) kind=grid-guest ;;
            '') kind=consumer ;;
            *) _gs_refuse "contradictory package kind metadata: $file"; exit 3 ;;
        esac
        case $role:$kind in
            host:host|second:grid-guest|second:consumer) ;;
            *) _gs_refuse "package is $kind, which the $role input does not accept: $input"; exit 3 ;;
        esac
    fi
    printf 'version=%s\nkind=%s\n' "$version" "$kind"
)

gs_classify() (
    case $# in
        8|9|10) ;;
        *) _gs_refuse 'classification requires eight to ten arguments'; exit 3 ;;
    esac
    host=$1 second=$2 public_sha=$3 public_version=$4 object_chain=$5 plugin_chain=$6 display=$7 cuda=$8
    if [ "$#" -ge 9 ]; then preflight=$9; else preflight=none; fi
    if [ "$#" -eq 10 ]; then ada=${10}; else ada=0; fi
    case $ada in
        0|1) ;;
        *) _gs_refuse "invalid ada flag: $ada"; exit 3 ;;
    esac
    case $preflight in
        confirmed|unconfirmed|none) ;;
        *) _gs_refuse "invalid preflight state: $preflight"; exit 3 ;;
    esac
    case $display:$cuda in
        0:0|0:1|1:0|1:1) ;;
        *) _gs_refuse "invalid feature flags: display=$display host_cuda=$cuda"; exit 3 ;;
    esac
    confirmation=unconfirmed
    profile=none
    rows=$gs_confirmed_hosts
    [ "$preflight" != unconfirmed ] || rows=
    for version in $rows; do
        gs_version_row "$version" || continue
        [ -n "$gs_plugin_chain" ] && [ "$plugin_chain" = "$gs_plugin_chain" ] || continue
        # The object and plugin chains are the identity: an extracted source tree (--source)
        # has no package .run digest, so the host digest is recorded when known and absent
        # otherwise. A recorded host digest that is not the block's still refuses.
        [ "$host" = none ] || { [ -n "$gs_host_package" ] && [ "$host" = "$gs_host_package" ]; } || continue
        second_known=0
        [ -z "$gs_second_grid" ] || [ "$second" != "$gs_second_grid" ] || second_known=1
        [ -z "$gs_second_consumer" ] || [ "$second" != "$gs_second_consumer" ] || second_known=1
        candidate=
        if [ -n "$gs_object_chain" ] && [ "$object_chain" = "$gs_object_chain" ] &&
            [ "$second" = none ] && [ "$public_sha" = none ] && [ "$public_version" = none ] &&
            [ "$display:$cuda" = 0:0 ]; then
            candidate=$gs_profile_plain
        elif [ "$ada" = 1 ] && [ "$display" = 1 ] && [ -n "$gs_object_chain_placement_display" ] &&
            [ "$object_chain" = "$gs_object_chain_placement_display" ] &&
            [ "$second_known" -eq 1 ] &&
            [ "$public_sha" = none ] &&
            [ -n "$gs_public_version" ] && [ "$public_version" = "$gs_public_version" ]; then
            case $cuda in
                1) candidate=$gs_profile_display_cuda ;;
                *) candidate=$gs_profile_display ;;
            esac
        elif [ "$ada" = 0 ] && [ -n "$gs_object_chain_display" ] && [ "$object_chain" = "$gs_object_chain_display" ] &&
            [ "$second_known" -eq 1 ] &&
            [ "$public_sha" = none ] &&
            [ -n "$gs_public_version" ] && [ "$public_version" = "$gs_public_version" ] &&
            [ "$display" = 1 ]; then
            case $cuda in
                1) candidate=$gs_profile_display_cuda ;;
                *) candidate=$gs_profile_display ;;
            esac
        fi
        # A combination with no profile has no recorded run.
        [ -n "$candidate" ] || continue
        confirmation='confirmed-inputs'
        profile=$candidate
        break
    done
    printf 'confirmation=%s\nevidence_profile=%s\n' "$confirmation" "$profile"
)

# gs_recorded_second SHA
#   Prints kind=grid-guest, kind=consumer or kind=unknown for a second-package
#   digest of any block. This identifies a recorded input; it does not confirm a build.
gs_recorded_second() (
    [ "$#" -eq 1 ] && [ -n "$1" ] || { _gs_refuse 'second-package lookup requires one digest'; exit 3; }
    kind=unknown
    for version in $gs_confirmed_hosts; do
        gs_version_row "$version" || continue
        if [ "$1" = "$gs_second_grid" ]; then kind=grid-guest; break; fi
        if [ "$1" = "$gs_second_consumer" ]; then kind=consumer; break; fi
    done
    printf 'kind=%s\n' "$kind"
)

gs_version_notice() {
    [ "$#" -eq 1 ] || { _gs_refuse 'notice requires one version'; return 3; }
    version=$1
    [ -n "$version" ] || version=unknown
    # One notice per build. A file guard survives the subshell that runs the
    # preflight, so the caller can set GS_VERSION_NOTICE_FILE in its scratch
    # directory and never print the notice twice.
    if [ -n "${GS_VERSION_NOTICE_FILE:-}" ]; then
        [ ! -e "$GS_VERSION_NOTICE_FILE" ] || return 0
        : >"$GS_VERSION_NOTICE_FILE" 2>/dev/null || :
    else
        [ "${GS_VERSION_NOTICE_SHOWN:-0}" != 1 ] || return 0
        GS_VERSION_NOTICE_SHOWN=1
    fi
    printf 'Best effort: driver %s is not guaranteed to work. Confirmed versions: %s. Required patch anchors matched; this does not establish runtime compatibility.\n' "$version" "$gs_confirmed_host_versions" >&2
}

gs_version_fields() (
    [ "$#" -eq 10 ] || { _gs_refuse 'version fields require ten arguments'; exit 3; }
    h=$1 s=$2 p=$3 confirmation=$4 profile=$5 display=$6 cuda=$7 host_sha=$8 second_sha=$9 public_sha=${10}
    case $confirmation in
        confirmed-inputs|unconfirmed) ;;
        *) _gs_refuse "invalid confirmation: $confirmation"; exit 3 ;;
    esac
    case $display:$cuda in
        0:0|0:1|1:0|1:1) ;;
        *) _gs_refuse "invalid feature flags: display=$display host_cuda=$cuda"; exit 3 ;;
    esac
    [ -n "$h" ] || { _gs_refuse 'missing host version'; exit 3; }
    case $confirmation in
        confirmed-inputs) confirmed=1 ;;
        *) confirmed=0 ;;
    esac
    [ -n "$profile" ] || profile=none
    [ -n "$s" ] || s=none
    [ -n "$p" ] || p=none
    [ -n "$host_sha" ] || host_sha=none
    [ -n "$second_sha" ] || second_sha=none
    [ -n "$public_sha" ] || public_sha=none
    printf 'driver_version=%s\ndriver_version_confirmed=%s\ndriver_version_policy=%s\n' "$h" "$confirmed" "$GS_VERSION_POLICY"
    printf 'driver_second_version=%s\ndriver_public_version=%s\n' "$s" "$p"
    printf 'host_sha256=%s\nsecond_sha256=%s\npublic_sha256=%s\n' "$host_sha" "$second_sha" "$public_sha"
    printf 'display=%s\nhost_cuda=%s\nconfirmation=%s\nevidence_profile=%s\n' "$display" "$cuda" "$confirmation" "$profile"
)

gs_record_module() {
    [ "$#" -eq 3 ] || { _gs_refuse 'module fields require name, stamp and digest'; return 3; }
    name=$(printf '%s' "$1" | tr '-' '_')
    case $name in
        ''|*[!a-z0-9_]*) _gs_refuse "invalid module name: $1"; return 3 ;;
    esac
    [ -n "$2" ] && [ -n "$3" ] || { _gs_refuse "module $1 needs a stamp and a digest"; return 3; }
    printf 'module_%s_stamp=%s\nmodule_%s_sha256=%s\n' "$name" "$2" "$name" "$3"
}

gs_record_emit() {
    awk '
        BEGIN { FS = "=" }
        {
            if (index($0, sprintf("%c", 13))) exit 1
            if ($0 !~ /^[a-z][a-z0-9_]*=/) exit 1
            if ($1 in seen) exit 1
            seen[$1] = 1
            print
        }' || { _gs_refuse 'invalid build record'; return 3; }
}

gs_record_read() (
    [ "$#" -eq 1 ] || { _gs_refuse 'record read requires one file'; exit 3; }
    [ -f "$1" ] && [ -r "$1" ] || { _gs_refuse "unreadable build record: $1"; exit 3; }
    data=$(gs_record_emit <"$1") || exit 3
    [ -z "$data" ] || printf '%s\n' "$data"
    if printf '%s\n' "$data" | awk '
        /^driver_version=.+$/ { v = 1 }
        /^driver_version_policy=.+$/ { p = 1 }
        /^driver_version_confirmed=[01]$/ { c = 1 }
        /^confirmation=(confirmed-inputs|unconfirmed)$/ { s = 1 }
        END { exit !(v && p && c && s) }'; then
        printf 'record_state=recorded\n'
    else
        printf 'record_state=not-recorded\n'
    fi
)

gs_preflight() (
    [ "$#" -eq 6 ] || [ "$#" -eq 7 ] || { _gs_refuse 'preflight requires patcher, object, plugin, display, work and version, and may name the Ada flag'; exit 3; }
    patcher=$1 object=$2 plugin=$3 display=$4 work=$5 version=$6 ada=${7:-0}
    case $ada in
        0|1) ;;
        *) _gs_refuse "invalid ada flag: $ada"; exit 3 ;;
    esac
    case $display in
        0|1) ;;
        *) _gs_refuse "invalid display flag: $display"; exit 3 ;;
    esac
    [ -x "$patcher" ] || { _gs_refuse "patcher not executable: $patcher"; exit 3; }
    [ -f "$object" ] && [ -r "$object" ] || { _gs_refuse "object input unreadable: $object"; exit 3; }
    [ -f "$plugin" ] && [ -r "$plugin" ] || { _gs_refuse "plugin input unreadable: $plugin"; exit 3; }
    [ -d "$work" ] || { _gs_refuse "work directory missing: $work"; exit 3; }
    temp=$(mktemp -d "$work/preflight.XXXXXXXX") || { _gs_refuse 'cannot create preflight scratch directory'; exit 3; }
    trap 'rm -rf -- "$temp"' EXIT HUP INT TERM
    failed=0
    unconfirmed=0
    obj=$object
    plug=$plugin
    obj_failed=
    plug_failed=
    for stage in scheduler module-gate vf-count ada-placement plugin-mixed plugin-div4 display-idx39; do
        if [ "$stage" = display-idx39 ] && [ "$display" != 1 ]; then
            continue
        fi
        case $stage in ada-placement) [ "$ada" = 1 ] || continue ;; esac
        case $stage in
            plugin-*) source=$plug prior=$plug_failed ;;
            *) source=$obj prior=$obj_failed ;;
        esac
        if [ -n "$prior" ]; then
            printf 'preflight %s: skipped after %s failed\n' "$stage" "$prior" >&2
            continue
        fi
        output=$temp/$stage.out
        rc=0
        case $stage in
            scheduler) "$patcher" apply "$source" "$output" >"$temp/result" 2>"$temp/error" || rc=$? ;;
            display-idx39) "$patcher" display apply "$source" "$output" >"$temp/result" 2>"$temp/error" || rc=$? ;;
            *) "$patcher" ada apply "$source" "$output" >"$temp/result" 2>"$temp/error" || rc=$? ;;
        esac
        if [ "$rc" -ne 0 ]; then
            printf 'preflight %s: ' "$stage" >&2
            if [ -s "$temp/error" ]; then
                cat "$temp/error" >&2
            else
                printf 'anchor %s: patch command failed with exit %s\n' "$stage" "$rc" >&2
            fi
            failed=1
            case $stage in
                plugin-*) plug_failed=$stage ;;
                *) obj_failed=$stage ;;
            esac
            continue
        fi
        input_hash=$(gs_sha256 "$source") || exit 3
        [ -f "$output" ] && [ -r "$output" ] || {
            printf 'anchor %s.output: the patch command wrote no output\n' "$stage" >&2
            failed=1
            case $stage in plugin-*) plug_failed=$stage ;; *) obj_failed=$stage ;; esac
            continue
        }
        output_hash=$(gs_sha256 "$output") || exit 3
        if ! awk -F= -v rev="$stage" -v ih="$input_hash" -v oh="$output_hash" '
            $1 == "revision" { r++; if ($2 != rev) bad = 1 }
            $1 == "state" { s++; if ($2 != "confirmed" && $2 !~ /^unconfirmed:.+$/) bad = 1 }
            $1 == "input_sha256" { i++; if ($2 != ih) bad = 1 }
            $1 == "output_sha256" { o++; if ($2 != oh) bad = 1 }
            $1 == "admission" { a++; if ($2 != "digest" && $2 != "anchor") bad = 1 }
            END { exit !(r == 1 && s == 1 && i == 1 && o == 1 && a == 1 && !bad) }' "$temp/result"; then
            printf 'anchor %s.contract: the result of the patch tool does not match the byte patch, the input digest or the output digest\n' "$stage" >&2
            failed=1
            case $stage in plugin-*) plug_failed=$stage ;; *) obj_failed=$stage ;; esac
            continue
        fi
        # module-gate and plugin-mixed are intermediate rows: the patcher reports
        # state=unconfirmed:unknown for them because they were never measured alone.
        # Only the final rows decide the state, and any anchor admission decides against
        # confirmation. Do not "fix" the intermediate rows.
        verdict=$(awk -F= -v rev="$stage" '
            $1 == "admission" { admission = $2 }
            $1 == "state" { state = $2 }
            END {
                if (admission == "anchor") { print "unconfirmed"; exit }
                if (rev == "module-gate" || rev == "plugin-mixed") { print "confirmed"; exit }
                print (state == "confirmed" ? "confirmed" : "unconfirmed")
            }' "$temp/result")
        [ "$verdict" = confirmed ] || unconfirmed=1
        printf 'stage=%s\n' "$stage"
        cat "$temp/result"
        case $stage in
            plugin-*) plug=$output ;;
            *) obj=$output ;;
        esac
    done
    if [ "$failed" -ne 0 ]; then
        _gs_refuse 'preflight refused: a required patch anchor did not match; nothing was installed'
        exit 3
    fi
    if [ "$unconfirmed" -eq 1 ]; then
        gs_version_notice "$version" || exit 3
        printf 'preflight_state=unconfirmed\n'
    else
        printf 'preflight_state=confirmed\n'
    fi
)
