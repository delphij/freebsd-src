#
# SPDX-License-Identifier: BSD-2-Clause
#

atf_test_case basic
basic_body()
{
	atf_check -o inline:"cba\n" -x "echo abc | rev"
	atf_check -o inline:"54321\n" -x "echo 12345 | rev"
}

atf_test_case multiline
multiline_body()
{
	atf_check -o inline:"cba\nfed\n" -x "printf 'abc\ndef\n' | rev"
}

atf_test_case empty
empty_body()
{
	atf_check -o inline:"\n" -x "echo | rev"
	atf_check -o inline:"" -x "printf '' | rev"
}

atf_test_case file_input
file_input_body()
{
	printf 'abc\ndef\n' > input.txt
	atf_check -o inline:"cba\nfed\n" rev input.txt
}

atf_test_case nul_separator
nul_separator_body()
{
	# Test with NUL separator - input has NUL-separated strings
	atf_check -o inline:"cba\0fed\0" -x "printf 'abc\0def\0' | rev -0"

	# Test that newlines are preserved within NUL-separated records
	atf_check -o inline:"c\nba\0f\nde\0" -x "printf 'ab\nc\0ed\nf\0' | rev -0"
}

atf_test_case nul_vs_newline
nul_vs_newline_body()
{
	# Without -0, newline is the separator
	atf_check -o inline:"cba\nfed\n" -x "printf 'abc\ndef\n' | rev"

	# With -0, NUL is the separator, newlines are part of the line
	atf_check -o inline:"c\nba\0" -x "printf 'ab\nc\0' | rev -0"
}

atf_init_test_cases()
{
	atf_add_test_case basic
	atf_add_test_case multiline
	atf_add_test_case empty
	atf_add_test_case file_input
	atf_add_test_case nul_separator
	atf_add_test_case nul_vs_newline
}
