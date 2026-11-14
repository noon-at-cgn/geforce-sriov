#!/bin/sh
# SPDX-License-Identifier: MIT
# Writes the summary file of `make check-report`.
#   usage: check-report.sh REPORT OUTDIR MAKE_EXIT_CODE WALL_SECONDS NAME...
# Line 1 is the commit, line 2 says whether the tree was clean, then one line per suite (the
# summary run-suite.sh kept; "NOT RUN" when the suite left none), then the exit code. The exit
# code is that of `make -k check`, so a suite that failed or never ran makes it non-zero.
set -u
report=$1 out=$2 mkrc=$3 secs=$4
shift 4
commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)
# Build products, check-out and the report itself are ignored by .gitignore.
if [ -z "$(git status --porcelain 2>/dev/null)" ]; then tree=clean; else tree=dirty; fi
{
	printf 'commit %s\n' "$commit"
	printf 'tree %s\n' "$tree"
	printf 'date %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'wall %ss\n' "$secs"
	printf 'jobs %s\n' "${GS_REPORT_JOBS:-1}"
	for n in "$@"; do
		if [ -s "$out/$n.summary" ]; then
			cat "$out/$n.summary"
		else
			printf '%s: NOT RUN\n' "$n"
		fi
	done
	# A suite split into shards (name@N) also gets one line with the summed counts, so the
	# total stays comparable with the unsplit suite; a shard that left no summary shows as missing.
	for g in $(for n in "$@"; do case $n in *@*) echo "${n%@*}" ;; esac; done | sort -u); do
		want=0
		for n in "$@"; do case $n in "$g"@*) want=$((want + 1)) ;; esac; done
		cat "$out/$g"@*.summary 2>/dev/null |
			sed -n 's/.*: \([0-9][0-9]*\) passed, \([0-9][0-9]*\) failed$/\1 \2/p' |
			awk -v g="$g" -v want="$want" '{ p += $1; f += $2; n++ }
				END { printf "%s: %d passed, %d failed (%d of %d shards reported)\n", g, p, f, n, want }'
	done
	printf 'exit code %s\n' "$mkrc"
} >"$report"
