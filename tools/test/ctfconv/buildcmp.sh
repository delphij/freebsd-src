#!/bin/sh
#
# Compare the CTF of two kernel build directories, for example one built
# the old way (ctfconvert per object, ctfmerge) and one built with
# WITH_CTF_POSTLINK.  For kernel.full and every *.ko.full under OLDDIR
# the same module in NEWDIR is dumped with ctfdump and compared
# semantically with ctfcmp.py; artifacts of the files that differ stay
# in OUTDIR.
#
# Usage: buildcmp.sh OLDDIR NEWDIR OUTDIR
#
CTFCMP=$(dirname "$0")/ctfcmp.py
old=$1; new=$2; out=$3
[ -n "$old" ] && [ -n "$new" ] && [ -n "$out" ] ||
    { echo "usage: $0 olddir newdir outdir" >&2; exit 2; }
mkdir -p "$out"
# Both files are appended to below: start clean, or a rerun in the
# same OUTDIR counts the previous run's modules again.
rm -f "$out/counts.txt" "$out/missing.txt"
ok=0; diff=0; fail=0; missing=0

compare() {
	o=$1; n=$2; b=$3
	if ! ctfdump "$o" > "$out/$b.old.txt" 2>/dev/null; then
		echo "NO-CTF old $o"; fail=$((fail+1)); return
	fi
	if ! ctfdump "$n" > "$out/$b.new.txt" 2>/dev/null; then
		echo "NO-CTF new $n"; fail=$((fail+1)); return
	fi
	if python3 -I "$CTFCMP" "$out/$b.old.txt" "$out/$b.new.txt" > "$out/$b.cmp" 2>&1; then
		ok=$((ok+1))
		rm -f "$out/$b.old.txt" "$out/$b.new.txt"
	else
		echo "DIFF $b: $(grep -c '^SYMBOL\|^NAMED\|^TYPE' "$out/$b.cmp") unclassified"
		diff=$((diff+1))
	fi
}

compare "$old/kernel.full" "$new/kernel.full" kernel

# The right-hand side of the pipe runs in a subshell, so counters it
# increments are lost when it exits: it starts its own at zero (not at
# the kernel's values it inherits) and writes them to counts.txt, which
# is added to the kernel's count afterward.
find "$old" -name '*.ko.full' | sort | { ok=0; diff=0; fail=0; while read f; do
	rel=${f#*/sys/modules/}
	n=$(find "$new" -path "*/sys/modules/$rel" | head -1)
	if [ -z "$n" ]; then
		echo "MISSING $rel"; echo "$rel" >> "$out/missing.txt"; continue
	fi
	b=${rel##*/}; b=${b%%.*}
	compare "$f" "$n" "$b"
done; echo "$ok $diff $fail" > "$out/counts.txt"; }
if [ -f "$out/counts.txt" ]; then
	read mok mdiff mfail < "$out/counts.txt"
	ok=$((ok+mok)); diff=$((diff+mdiff)); fail=$((fail+mfail))
fi
[ -f "$out/missing.txt" ] && missing=$(wc -l < "$out/missing.txt")
echo "identical=$ok different=$diff failed=$fail missing=$missing"
[ "$diff" -eq 0 ] && [ "$fail" -eq 0 ] && [ "$missing" -eq 0 ]
