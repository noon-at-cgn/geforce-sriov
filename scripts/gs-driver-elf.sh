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
# gs_elf_closed_same OBJECT BEFORE AFTER
#   Refuse unless every function that OBJECT (NVIDIA's closed nv-kernel.o) defines under a name
#   that is unique has the same bytes in the module AFTER as in the module BEFORE. Prints how many
#   functions were compared. Requires python3. Both modules are relocatable files: the bytes are
#   those of the files, with every relocation field still empty. Where the relocations point and
#   the object's data are not compared; the caller checks that OBJECT itself is unchanged.
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

gs_elf_closed_same() (
	[ "$#" -eq 3 ] || { gs_elf_refuse 'the closed-code comparison requires OBJECT BEFORE AFTER'; exit 3; }
	python3 - "$1" "$2" "$3" <<'PY'
import struct, sys

def refuse(msg):
    print(msg)
    sys.exit(3)

def functions(path):
    """name -> bytes of each function symbol that is defined once in an ELF64 little-endian relocatable file."""
    try:
        data = open(path, 'rb').read()
    except OSError as e:
        refuse('cannot read %s: %s' % (path, e))
    if data[:6] != b'\x7fELF\x02\x01' or len(data) < 64:
        refuse('%s is not a 64-bit little-endian ELF file' % path)
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3a)
    if shentsize != 64 or shoff + shnum * 64 > len(data):
        refuse('%s has no usable section table' % path)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * 64) for i in range(shnum)]
    found, twice = {}, set()
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        names = sections[sec[6]]
        for at in range(sec[4], sec[4] + sec[5], 24):
            name, info, _, shndx, value, size = struct.unpack_from('<IBBHQQ', data, at)
            if info & 0xf != 2 or shndx == 0 or shndx >= 0xff00 or size == 0 or shndx >= shnum:
                continue  # not a function with a body in this file
            end = data.index(b'\0', names[4] + name)
            text = data[names[4] + name:end].decode('latin-1')
            home = sections[shndx]
            if home[1] == 8 or value + size > home[5]:  # SHT_NOBITS, or outside its section
                refuse('%s: function %s lies outside its section' % (path, text))
            if text in found:
                twice.add(text)
            found[text] = data[home[4] + value:home[4] + value + size]
    for text in twice:
        del found[text]
    return found

closed, before, after = (functions(p) for p in sys.argv[1:4])
if not closed:
    refuse('%s defines no function' % sys.argv[1])
compared = 0
for name in closed:
    if name not in before:
        continue  # a name that is not unique in the module cannot be followed
    if name not in after:
        refuse('function %s of the closed object is not in the module that was built with the owner' % name)
    if before[name] != after[name]:
        refuse('function %s of the closed object differs between the two modules' % name)
    compared += 1
if compared == 0 or compared * 2 < len(closed):
    refuse('only %d of the %d functions of the closed object could be followed into the modules' % (compared, len(closed)))
print('the %d functions of the closed object that the modules name are the same bytes in both (%d defined); relocations and data are not compared' % (compared, len(closed)))
PY
)

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

