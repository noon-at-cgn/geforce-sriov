#!/bin/sh
# SPDX-License-Identifier: MIT
# Sourced library; no commands run on import. ELF checks for the driver step's
# display build. Every result comes from the files the caller names; nothing
# here loads a module or touches a device. ABI and CRC checking is the patch
# tool's `abi-check`, not this library.
#
# gs_elf_sections FILE
#   Print "index name address offset" for every section.
# gs_elf_symbol FILE NAME
#   Print "value size section" for a unique defined symbol. Return 1 when it is
#   absent and 3 when it is defined more than once.
# gs_elf_bytes FILE OFFSET COUNT
#   Print COUNT bytes as lowercase hex, no separators.
# gs_idx39_finished CORE FUNCTION OFFSET
#   Refuse unless the five bytes at FUNCTION's recorded function-relative
#   OFFSET in the finished module are 31 c0 90 90 90 and no relocation other
#   than R_X86_64_NONE overlaps them. OFFSET is decimal.

gs_elf_refuse() {
	printf 'ELF check: %s\n' "$*" >&2
	return 3
}

# Print "index name address offset" for every section of FILE. readelf pads a
# one-digit index inside the brackets, so both bracket forms are accepted.
gs_elf_sections() {
	readelf -SW -- "$1" 2>/dev/null | awk '
		$1 == "[" { i = $2; sub(/\]$/, "", i); print i, $3, $5, $6; next }
		$1 ~ /^\[[0-9]+\]$/ { i = $1; gsub(/[][]/, "", i); print i, $2, $4, $5 }
	'
}

gs_elf_symbol() (
	[ "$#" -eq 2 ] || { gs_elf_refuse 'symbol lookup requires FILE NAME'; exit 3; }
	rows=$(readelf -sW -- "$1" 2>/dev/null | awk -v want="$2" '
		$8 == want && $7 != "UND" && $7 != "ABS" { print $2, $3, $7 }')
	[ -n "$rows" ] || exit 1
	[ "$(printf '%s\n' "$rows" | wc -l)" -eq 1 ] ||
		{ printf 'ELF check: symbol %s is defined more than once in %s\n' "$2" "$1" >&2; exit 3; }
	printf '%s\n' "$rows"
)

gs_elf_bytes() {
	[ "$#" -eq 3 ] || { gs_elf_refuse 'byte read requires FILE OFFSET COUNT'; return 3; }
	od -An -tx1 -j "$2" -N "$3" -- "$1" 2>/dev/null | tr -d ' \n'
}

gs_idx39_finished() (
	[ "$#" -eq 3 ] || { gs_elf_refuse 'idx 39 check requires CORE FUNCTION OFFSET'; exit 3; }
	core=$1 function=$2 offset=$3
	case $offset in ''|*[!0-9]*) gs_elf_refuse "invalid idx 39 offset: $offset"; exit 3 ;; esac
	row=$(gs_elf_symbol "$core" "$function") || {
		gs_elf_refuse "the finished module has no unique $function"
		exit 3
	}
	value=$(printf '%s\n' "$row" | awk '{print $1}')
	ndx=$(printf '%s\n' "$row" | awk '{print $3}')
	section=$(gs_elf_sections "$core" | awk -v want="$ndx" '$1 == want { print $3, $4; exit }')
	[ -n "$section" ] || { gs_elf_refuse "cannot map $function to a section of $core"; exit 3; }
	address=$(printf '%s\n' "$section" | awk '{print $1}')
	file_off=$(printf '%s\n' "$section" | awk '{print $2}')
	relative=$((0x$value + offset))
	position=$((0x$file_off + relative - 0x$address))
	bytes=$(gs_elf_bytes "$core" "$position" 5) ||
		{ gs_elf_refuse "cannot read the finished $function"; exit 3; }
	[ "$bytes" = 31c0909090 ] || {
		gs_elf_refuse "$function+$offset in ${core##*/} is $bytes; expected 31c0909090"
		exit 3
	}
	quote=$(printf '%c' 39)
	overlap=$(readelf -rW -- "$core" 2>/dev/null | awk -v start="$relative" -v limit="$((relative + 5))" -v q="$quote" '
		function hex(s,   i, c, v) {
			sub(/^0[xX]/, "", s); s = tolower(s); v = 0
			for (i = 1; i <= length(s); i++) {
				c = index("0123456789abcdef", substr(s, i, 1))
				if (c == 0) return -1
				v = v * 16 + c - 1
			}
			return v
		}
		/^Relocation section/ { in_text = index($3, q ".rela.text") == 1; next }
		in_text && $1 ~ /^[0-9a-fA-F]+$/ && $3 != "R_X86_64_NONE" {
			off = hex($1)
			if (off >= 0 && off < limit && off + 8 > start) { print $1, $3, $8; exit }
		}')
	[ -z "$overlap" ] || {
		gs_elf_refuse "an active relocation overlaps $function+$offset in ${core##*/}: $overlap"
		exit 3
	}
	printf 'idx 39: %s+%s in %s holds 31c0909090 with no active relocation\n' "$function" "$offset" "${core##*/}"
	exit 0
)

