#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs one `make check` suite in a scratch directory of its own and keeps its output.
#   usage: run-suite.sh OUTDIR NAME COMMAND [ARGUMENT...]
# The suite sees TMPDIR=<its own mktemp -d directory under the caller's TMPDIR>, which is removed
# afterwards, so suites do not share a path and may run side by side. The output goes to
# OUTDIR/NAME.log, the exit status to OUTDIR/NAME.rc, a one-line summary (the last line the suite
# printed and its run time) to OUTDIR/NAME.summary. The log is printed in one piece when the suite
# ends, so that `make -jN` does not interleave suites. Only the processes this script starts are
# ever signalled; nothing is killed by name.
set -u
out=$1 name=$2
shift 2
mkdir -p "$out" || exit 2
scratch=$(mktemp -d "${TMPDIR:-/tmp}/gs-suite-$name.XXXXXX") || exit 2
trap 'rm -rf "$scratch"' EXIT
trap 'exit 1' HUP INT TERM
rm -f "$out/$name.rc" "$out/$name.summary"
start=$(date +%s)
(TMPDIR=$scratch; export TMPDIR; exec "$@") >"$out/$name.log" 2>&1
rc=$?
secs=$(($(date +%s) - start))
last=$(awk 'NF { l = $0 } END { print l }' "$out/$name.log")
printf '%s: rc=%s, %ss: %s\n' "$name" "$rc" "$secs" "$last" >"$out/$name.summary"
printf '%s\n' "$rc" >"$out/$name.rc"
{
	printf '== %s\n' "$name"
	cat "$out/$name.log"
	printf '== %s: exit %s after %ss\n' "$name" "$rc" "$secs"
}
exit "$rc"
