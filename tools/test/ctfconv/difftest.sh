#!/bin/sh
#
# Differential harness for ctfconv (see README.md).
#
# For every ELF file listed in the list file that already carries a
# .SUNW_ctf section: strip that section from a copy, run ctfconv on the
# copy, attach the result and compare the two ctfdump outputs
# semantically with ctfcmp.py.  Results land in the output directory.
#
# Two uses.  Without -s (Phase 1): the files were converted by the
# previous tools, and the known differences between those and ctfconv
# are bucketed.  With -s (the build-level check): the files come from a
# ctfconv build, so the container the build left in each must equal a
# reconversion of that same file exactly; ctfcmp.py runs with --strict
# and any difference fails.  This is what catches a build rule that
# rewrites the symbol table after the CTF step.
#
# -b (the refactoring oracle) is stricter still: the reconverted
# container must be byte-identical to the one in the file, so the label
# has to match (LABEL, default L).  No dump or comparison is made; a
# file whose bytes differ is listed and its artifacts kept.
#
# Environment: CTFCONV (default: ctfconv in PATH), LABEL (-b only).
#
CTFCONV=${CTFCONV:-ctfconv}
LABEL=${LABEL:-L}
CTFCMP=$(dirname "$0")/ctfcmp.py
strict=; bytes=
while [ $# -gt 0 ]; do
	case $1 in
	-s) strict=--strict; shift;;
	-b) bytes=1; shift;;
	*) break;;
	esac
done
list=$1; out=$2
[ -n "$list" ] && [ -n "$out" ] || { echo "usage: $0 [-s|-b] listfile outdir" >&2; exit 2; }
mkdir -p "$out"
ok=0; diff=0; fail=0; noctf=0; dropped=0
while read f; do
	b=${f##*/}; b=${b%%.*}
	if ! readelf -S "$f" 2>/dev/null | grep -q '\.SUNW_ctf'; then
		noctf=$((noctf+1)); continue
	fi
	[ -z "$bytes" ] && ctfdump "$f" > "$out/$b.old.txt" 2>/dev/null
	cp "$f" "$out/$b.elf" && chmod u+w "$out/$b.elf" &&
	    objcopy --remove-section .SUNW_ctf "$out/$b.elf"
	# In-place mode: ctfconv adds the section itself.
	if ! $CTFCONV -v -l "$LABEL" "$out/$b.elf" > "$out/$b.log" 2>&1; then
		echo "FAIL $f"; fail=$((fail+1)); continue
	fi
	if [ -n "$bytes" ]; then
		objcopy --dump-section ".SUNW_ctf=$out/$b.sec.old" "$f" /dev/null 2>/dev/null
		objcopy --dump-section ".SUNW_ctf=$out/$b.sec.new" "$out/$b.elf" /dev/null 2>/dev/null
		if cmp -s "$out/$b.sec.old" "$out/$b.sec.new"; then
			ok=$((ok+1))
			rm -f "$out/$b.elf" "$out/$b.sec.old" "$out/$b.sec.new" "$out/$b.log"
		else
			echo "BYTES $f"; diff=$((diff+1))
		fi
		continue
	fi
	if ! readelf -S "$out/$b.elf" 2>/dev/null | grep -q '\.SUNW_ctf'; then
		# No DWARF or no C compilation unit: ctfconv leaves the file
		# alone where the old tools wrote an empty
		# container.  Counted apart from noctf so that a regression
		# in CU detection shows up.
		dropped=$((dropped+1)); rm -f "$out/$b.elf" "$out/$b.old.txt"; continue
	fi
	if ! ctfdump "$out/$b.elf" > "$out/$b.new.txt" 2> "$out/$b.new.err"; then
		echo "CTFDUMP-FAIL $f"; fail=$((fail+1)); continue
	fi
	if python3 -I "$CTFCMP" $strict "$out/$b.old.txt" "$out/$b.new.txt" > "$out/$b.cmp" 2>&1; then
		ok=$((ok+1))
		rm -f "$out/$b.elf" "$out/$b.old.txt" "$out/$b.new.txt" "$out/$b.new.err"
	else
		echo "DIFF $f: $(grep -c '^SYMBOL\|^NAMED\|^TYPE' "$out/$b.cmp") unclassified"
		diff=$((diff+1))
	fi
done < "$list"
echo "identical=$ok different=$diff failed=$fail noctf=$noctf oldctf-newnone=$dropped"
[ "$diff" -eq 0 ] && [ "$fail" -eq 0 ]
