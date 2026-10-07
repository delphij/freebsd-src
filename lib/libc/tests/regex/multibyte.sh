atf_test_case bmpat
bmpat_head()
{
	atf_set "descr" "Check matching multibyte characters (PR153502)"
}
bmpat_body()
{
	export LC_CTYPE="C.UTF-8"

	printf 'é' | atf_check -o "inline:é" \
	    sed -ne '/^.$/p'
	printf 'éé' | atf_check -o "inline:éé" \
	    sed -ne '/^..$/p'
	printf 'aéa' | atf_check -o "inline:aéa" \
	    sed -ne '/a.a/p'
	printf 'aéa'| atf_check -o "inline:aéa" \
	    sed -ne '/a.*a/p'
	printf 'aaéaa' | atf_check -o "inline:aaéaa" \
	    sed -ne '/aa.aa/p'
	printf 'aéaéa' | atf_check -o "inline:aéaéa" \
	    sed -ne '/a.a.a/p'
	printf 'éa' | atf_check -o "inline:éa" \
	    sed -ne '/.a/p'
	printf 'aéaa' | atf_check -o "inline:aéaa" \
	    sed -ne '/a.aa/p'
	printf 'éaé' | atf_check -o "inline:éaé" \
	    sed -ne '/.a./p'
}

atf_test_case icase
icase_head()
{
	atf_set "descr" "Check case-insensitive matching for characters 128-255"
}
icase_body()
{
	export LC_CTYPE="C.UTF-8"

	a=$(printf '\302\265\n')	# U+00B5
	b=$(printf '\316\234\n')	# U+039C
	c=$(printf '\316\274\n')	# U+03BC

	echo $b | atf_check -o "inline:$b\n" sed -ne "/$a/Ip"
	echo $c | atf_check -o "inline:$c\n" sed -ne "/$a/Ip"
}

atf_test_case mbset cleanup
mbset_head()
{
	atf_set "descr" "Check multibyte sets matching"
}
mbset_body()
{
	export LC_CTYPE="C.UTF-8"

	# This involved an erroneously implemented optimization which reduces
	# single-element sets to an exact match with a single codepoint.
	# Match sets record small-codepoint characters in a bitmap and
	# large-codepoint characters in an array; the optimization would falsely
	# trigger if either the bitmap or the array was a singleton, ignoring
	# the members of the other side of the set.
	#
	# To exercise this, we construct sets which have one member of one side
	# and one or more of the other, and verify that all members can be
	# found.
	printf "a" > mbset; atf_check -o not-empty sed -ne '/[aà]/p' mbset
	printf "à" > mbset; atf_check -o not-empty sed -ne '/[aà]/p' mbset
	printf "a" > mbset; atf_check -o not-empty sed -ne '/[aàá]/p' mbset
	printf "à" > mbset; atf_check -o not-empty sed -ne '/[aàá]/p' mbset
	printf "á" > mbset; atf_check -o not-empty sed -ne '/[aàá]/p' mbset
	printf "à" > mbset; atf_check -o not-empty sed -ne '/[abà]/p' mbset
	printf "a" > mbset; atf_check -o not-empty sed -ne '/[abà]/p' mbset
	printf "b" > mbset; atf_check -o not-empty sed -ne '/[abà]/p' mbset
	printf "a" > mbset; atf_check -o not-empty sed -Ene '/[aà]/p' mbset
	printf "à" > mbset; atf_check -o not-empty sed -Ene '/[aà]/p' mbset
	printf "a" > mbset; atf_check -o not-empty sed -Ene '/[aàá]/p' mbset
	printf "à" > mbset; atf_check -o not-empty sed -Ene '/[aàá]/p' mbset
	printf "á" > mbset; atf_check -o not-empty sed -Ene '/[aàá]/p' mbset
	printf "à" > mbset; atf_check -o not-empty sed -Ene '/[abà]/p' mbset
	printf "a" > mbset; atf_check -o not-empty sed -Ene '/[abà]/p' mbset
	printf "b" > mbset; atf_check -o not-empty sed -Ene '/[abà]/p' mbset
}
mbset_cleanup()
{
	rm -f mbset
}

atf_test_case wordbound cleanup
wordbound_head()
{
	atf_set "descr" "Check word boundaries next to multibyte characters"
}
wordbound_body()
{
	export LC_CTYPE="C.UTF-8"

	# A fullwidth A (three bytes) and a sigma (two bytes) are letters,
	# '|' is not.  Whether a character is part of a word used to be
	# judged from one byte of it, and from the byte before a match
	# rather than the character; with the input below, that drove the
	# matcher past the end of the line.
	printf 'Ａ|K|σσＡ|\n' > in
	atf_check -o inline:'Ａ|K|σσＡ|\n' sed -e 's/[[:>:]]Ａ/X/' in
	atf_check -o inline:'X|K|σσＡ|\n' sed -e 's/[[:<:]]Ａ/X/' in
	atf_check -o inline:'ＡXK|σσＡ|\n' sed -e 's/[[:>:]]|/X/' in
	atf_check -o inline:'Ａ|K|XσＡ|\n' sed -e 's/[[:<:]]σ/X/' in
	atf_check -o inline:'Ａ|K|σσＡ|\n' sed -e 's/σ[[:>:]]/X/' in
	atf_check -o inline:'Ａ|K|σX|\n' sed -e 's/σＡ[[:>:]]/X/' in
	atf_check -o empty sed -ne '/[[:>:]]Ａ/p' in
	atf_check -o inline:'Ａ|K|σσＡ|\n' sed -ne '/[[:<:]]Ａ/p' in

	# At the start and end of a line.
	printf 'σＡ\n' > in
	atf_check -o inline:'XＡ\n' sed -e 's/[[:<:]]σ/X/' in
	atf_check -o inline:'σX\n' sed -e 's/Ａ[[:>:]]/X/' in
	atf_check -o inline:'σＡ\n' sed -e 's/[[:<:]]Ａ/X/' in
	atf_check -o inline:'σＡ\n' sed -e 's/σ[[:>:]]/X/' in

	# A byte that is not part of any character is not part of a word.
	printf 'Ａ\377b\n' > in
	printf 'Ａ\377X\n' > exp
	atf_check -o file:exp sed -e 's/[[:<:]]b/X/' in
	printf 'X\377b\n' > exp
	atf_check -o file:exp sed -e 's/Ａ[[:>:]]/X/' in
}
wordbound_cleanup()
{
	rm -f in exp
}

atf_test_case wordbound_sjis cleanup
wordbound_sjis_head()
{
	atf_set "descr" \
	    "Check word boundaries where a trailing byte can be ASCII"
}
wordbound_sjis_body()
{
	local comma

	export LC_CTYPE="ja_JP.SJIS"
	if [ "$(locale charmap 2> /dev/null)" != "SJIS" ]; then
		atf_skip "the ja_JP.SJIS locale is not available"
	fi

	# The ideographic comma is 0x81 0x41, and 0x41 alone is 'A'.  Read
	# backwards from the 'b', the line could end in "Ab"; the matcher,
	# reading forwards, knows that it does not, and must not disagree
	# with itself about the boundary before the 'b'.  Finding the
	# subexpression used to loop forever.
	comma=$(printf '\201\101')
	printf '%sb\n' "$comma" > in
	atf_check -o inline:'X\n' sed -e "s/\\($comma\\)*[[:<:]]b/X/" in
	printf '%sX\n' "$comma" > exp
	atf_check -o file:exp sed -e 's/[[:<:]]b/X/' in
	atf_check -o file:in sed -e 's/A[[:>:]]b/X/' in
	printf '%sAb\n' "$comma" > in
	atf_check -o file:in sed -e 's/[[:<:]]b/X/' in
	atf_check -o file:exp sed -e 's/\(A\)b[[:>:]]/X/' in
}
wordbound_sjis_cleanup()
{
	rm -f in exp
}

atf_init_test_cases()
{
	atf_add_test_case bmpat
	atf_add_test_case icase
	atf_add_test_case mbset
	atf_add_test_case wordbound
	atf_add_test_case wordbound_sjis
}
