#
# Copyright (c) 2026 Xin Li <delphij@FreeBSD.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#

# The objects converted here are built by the Makefile from the sources
# next to it and installed with this script.  Assertions go through
# ctfdump(1) on the in-place result and match specific symbols and
# types, never whole dumps, so that type numbering may differ between
# compilers.  ctfdump cannot read a raw (-o) container, so raw mode is
# checked for its exit status and output file only.

need_ctfdump()
{
	which ctfdump >/dev/null 2>&1 || atf_skip "ctfdump is not installed"
}

# fixture NAME: a writable copy of the fixture object in the work dir.
fixture()
{
	cp "$(atf_get_srcdir)/$1" "$1"
	chmod u+w "$1"
}

# container FILE OUT: the .SUNW_ctf section of FILE, in OUT.
container()
{
	objcopy --dump-section ".SUNW_ctf=$2" "$1" /dev/null
}

atf_test_case types
types_head()
{
	atf_set descr "the type section of a plain C object"
}
types_body()
{
	need_ctfdump
	fixture a.o
	atf_check ctfconv -l L a.o
	atf_check -o match:'STRUCT node \(24 bytes\)' ctfdump -t a.o
	atf_check -o match:'TYPEDEF node_t' ctfdump -t a.o
	atf_check -o match:'ENUM color' ctfdump -t a.o
	atf_check -o match:'UNION u \(4 bytes\)' ctfdump -t a.o
	atf_check -o match:'STRUCT big \(70000 bytes\)' ctfdump -t a.o
	atf_check -o match:'INTEGER unsigned int bitfield' ctfdump -t a.o
	atf_check -o match:'ARRAY' ctfdump -t a.o
	atf_check -o match:'RESTRICT' ctfdump -t a.o
}

atf_test_case symbols
symbols_head()
{
	atf_set descr "object and function entries follow the symbol table"
}
symbols_body()
{
	need_ctfdump
	fixture a.o
	atf_check ctfconv -l L a.o
	# Global, static and qualified objects all get an entry.
	atf_check -o match:' gi \(' ctfdump -d a.o
	atf_check -o match:' si \(' ctfdump -d a.o
	atf_check -o match:' cvi \(' ctfdump -d a.o
	atf_check -o match:' arr2d \(' ctfdump -d a.o
	atf_check -o match:'FUNC \(f1\)' ctfdump -f a.o
	atf_check -o match:'FUNC \(f2\)' ctfdump -f a.o
}

atf_test_case label
label_head()
{
	atf_set descr "-l writes the label, no -l writes none"
}
label_body()
{
	need_ctfdump
	fixture a.o
	cp a.o nolabel.o
	atf_check ctfconv -l "FreeBSD test label" a.o
	atf_check -o match:'FreeBSD test label' ctfdump -l a.o
	atf_check ctfconv nolabel.o
	atf_check -o not-match:'FreeBSD test label' ctfdump -l nolabel.o
}

atf_test_case dwarf_versions
dwarf_versions_head()
{
	atf_set descr "DWARF 2, 3 and 5 convert like DWARF 4"
}
dwarf_versions_body()
{
	need_ctfdump
	for f in a2.o a3.o a5.o; do
		fixture $f
		atf_check ctfconv -l L $f
		atf_check -o match:'STRUCT node \(24 bytes\)' ctfdump -t $f
		atf_check -o match:'FUNC \(f1\)' ctfdump -f $f
		atf_check -o match:' gi \(' ctfdump -d $f
	done
}

atf_test_case compressed
compressed_head()
{
	atf_set descr "compressed debug sections (-gz=zlib) are read"
}
compressed_body()
{
	need_ctfdump
	fixture az.o
	atf_check ctfconv -l L az.o
	atf_check -o match:'STRUCT node \(24 bytes\)' ctfdump -t az.o
	atf_check -o match:'FUNC \(f1\)' ctfdump -f az.o
}

atf_test_case relocatable
relocatable_head()
{
	atf_set descr "ld -r of two units: forwards resolve across units"
}
relocatable_body()
{
	need_ctfdump
	fixture ab.o
	atf_check ctfconv -l L ab.o
	# struct fwd is declared in a.c and defined in b.c.
	atf_check -o match:'STRUCT fwd \(16 bytes\)' ctfdump -t ab.o
	atf_check -o not-match:'FORWARD fwd' ctfdump -t ab.o
	# The same-name statics of both units are typed.
	atf_check -o match:' si \(' ctfdump -d ab.o
	atf_check -o match:'FUNC \(g1\)' ctfdump -f ab.o
}

atf_test_case optimized
optimized_head()
{
	atf_set descr "an -O2 object: out-of-line inline instance, atomic"
}
optimized_body()
{
	need_ctfdump
	fixture d.o
	atf_check ctfconv -l L d.o
	atf_check -o match:'FUNC \(use_h\)' ctfdump -f d.o
	atf_check -o match:'FUNC \(h\)' ctfdump -f d.o
	atf_check -o match:' at \(' ctfdump -d d.o
	atf_check -o match:'FLOAT complex' ctfdump -t d.o
}

atf_test_case locals
locals_head()
{
	atf_set descr "function-local statics and weak aliases are typed"
}
locals_body()
{
	need_ctfdump
	fixture c.o
	atf_check ctfconv -l L c.o
	atf_check -o match:'counter \(' ctfdump -d c.o
	atf_check -o match:'FUNC \(weakalias\)' ctfdump -f c.o
	atf_check -o match:'FUNC \(strong\)' ctfdump -f c.o
	atf_check -o match:'STRUCT local' ctfdump -t c.o
}

atf_test_case gcc_objects
gcc_objects_head()
{
	atf_set descr "GCC objects: DW_AT_specification, zero-length array"
}
gcc_objects_body()
{
	need_ctfdump
	for f in g-gcc.o g-gcc32.o; do
		fixture $f
		atf_check ctfconv -l L $f
		# Declared before their definitions: GCC puts the address
		# on a DIE that carries only DW_AT_specification.  The
		# type index must not be 0 (void, the placeholder).
		atf_check -o match:'\] [1-9][0-9]* +gspec \(' ctfdump -d $f
		atf_check -o match:'\] [1-9][0-9]* +gname \(' ctfdump -d $f
		# int tail[0]: DW_AT_count 0.
		atf_check -o match:'ARRAY \(anon\) content: [0-9]+ index: [0-9]+ nelems: 0' \
		    ctfdump -t $f
		atf_check -o match:'STRUCT zl \(4 bytes\)' ctfdump -t $f
		# A plain char is SIGNED CHAR whatever the compiler says.
		atf_check -o match:'INTEGER char encoding=SIGNED CHAR' \
		    ctfdump -t $f
		atf_check -o match:'FUNC \(use\)' ctfdump -f $f
	done
}

atf_test_case gcc_empty_bound
gcc_empty_bound_head()
{
	atf_set descr "a -1 upper bound written as all ones in data4"
}
gcc_empty_bound_body()
{
	need_ctfdump
	# Older GCC wrote the -1 upper bound of a zero-length array in
	# the unsigned form of the target's size type: all ones in data4
	# on a 32-bit target.  gcc14 writes DW_AT_count 0 instead, so
	# the shape is made from g-gcc32.o by patching its subrange
	# abbreviation (DW_AT_type ref4, DW_AT_count data1 -> DW_AT_upper_bound
	# data4, DW_AT_lower_bound data1) and the DIE's bytes (0xffffffff, 0);
	# both edits keep the sizes.  Offsets are into the checked-in file.
	fixture g-gcc32.o
	cp g-gcc32.o bound.o
	printf '\057\006\042\013' | \
	    dd of=bound.o bs=1 seek=454 conv=notrunc 2>/dev/null
	printf '\377\377\377\377\000' | \
	    dd of=bound.o bs=1 seek=216 conv=notrunc 2>/dev/null
	atf_check ctfconv -l L bound.o
	atf_check -o match:'ARRAY \(anon\) content: [0-9]+ index: [0-9]+ nelems: 0' \
	    ctfdump -t bound.o
	atf_check -o match:'\] [1-9][0-9]* +gspec \(' ctfdump -d bound.o
}

atf_test_case cross_unit
cross_unit_head()
{
	atf_set descr "ThinLTO: references between units are followed"
}
cross_unit_body()
{
	need_ctfdump
	[ -f "$(atf_get_srcdir)/lto.o" ] || atf_skip "no ThinLTO fixture"
	fixture lto.o
	atf_check -e match:'references into other units: [1-9]' \
	    -e match:'references to unknown DIEs made void: 0' \
	    ctfconv -v -l L lto.o
	# use_scale returns int and takes int; both DIEs are in another
	# unit than the function.  Type 0 would be void.
	atf_check -o match:'FUNC \(use_scale\) returns: [1-9][0-9]* args: \([1-9]' \
	    ctfdump -f lto.o
	atf_check -o match:'FUNC \(use_sum\) returns: [1-9][0-9]* args: \([1-9]' \
	    ctfdump -f lto.o
	atf_check -o match:'STRUCT pair \(8 bytes\)' ctfdump -t lto.o
	atf_check -o match:'\] [1-9][0-9]* +gp \(' ctfdump -d lto.o
}

atf_test_case no_c_unit
no_c_unit_head()
{
	atf_set descr "an object without a C unit is left alone, exit 0"
}
no_c_unit_body()
{
	for f in e.o f.o; do
		fixture $f
		cp $f $f.orig
		atf_check ctfconv -l L $f
		atf_check cmp -s $f $f.orig
		atf_check ctfconv -l L -o $f.ctf $f
		atf_check test ! -e $f.ctf
	done
}

atf_test_case unsupported_version
unsupported_version_head()
{
	atf_set descr "a DWARF version the reader does not handle is refused"
}
unsupported_version_body()
{
	fixture a.o
	# Patch the unit header's version to 6.
	atf_check objcopy --dump-section .debug_info=info.bin a.o /dev/null
	printf '\006\000' | dd of=info.bin bs=1 seek=4 conv=notrunc 2>/dev/null
	atf_check objcopy --update-section .debug_info=info.bin a.o
	cp a.o a.o.orig
	atf_check -s exit:1 -e match:'DWARF version 6 is not supported' \
	    ctfconv -l L a.o
	atf_check cmp -s a.o a.o.orig
	atf_check -s exit:1 -e match:'not supported' ctfconv -l L -o a.ctf a.o
	atf_check test ! -e a.ctf
}

atf_test_case raw_mode
raw_mode_head()
{
	atf_set descr "-o writes the container alone and leaves the input"
}
raw_mode_body()
{
	fixture a.o
	cp a.o a.o.orig
	atf_check ctfconv -l L -o a.ctf a.o
	atf_check test -s a.ctf
	atf_check cmp -s a.o a.o.orig
	# -d cannot be combined with -o or -l.
	atf_check -s exit:1 -e match:'usage' ctfconv -d -o x.ctf a.o
}

atf_test_case dump_mode
dump_mode_head()
{
	atf_set descr "-d prints the types without writing anything"
}
dump_mode_body()
{
	fixture a.o
	cp a.o a.o.orig
	atf_check -o match:'STRUCT node' ctfconv -d a.o
	atf_check cmp -s a.o a.o.orig
}

atf_test_case idempotent
idempotent_head()
{
	atf_set descr "converting twice gives the same container"
}
idempotent_body()
{
	fixture a.o
	atf_check ctfconv -l L a.o
	container a.o first.ctf
	atf_check ctfconv -l L a.o
	container a.o second.ctf
	atf_check cmp -s first.ctf second.ctf
}

atf_test_case missing_input
missing_input_head()
{
	atf_set descr "a missing input is an error"
}
missing_input_body()
{
	atf_check -s exit:1 -e match:'nosuch.o' ctfconv -l L nosuch.o
}

atf_test_case directory_input
directory_input_head()
{
	atf_set descr "a directory as input is an error"
}
directory_input_body()
{
	mkdir dir
	atf_check -s exit:1 -e not-empty ctfconv -l L dir
}

atf_test_case unwritable_directory
unwritable_directory_head()
{
	atf_set descr "an output directory that cannot be written is an error"
	atf_set require.user unprivileged
}
unwritable_directory_body()
{
	fixture a.o
	mkdir ro
	chmod 555 ro
	atf_check -s exit:1 -e not-empty ctfconv -l L -o ro/a.ctf a.o
	atf_check test ! -e ro/a.ctf
	cp a.o ro.o 2>/dev/null || true
	# In-place mode makes its temporary in the input's directory.
	mkdir ro2
	cp a.o ro2/a.o
	chmod 555 ro2
	cp ro2/a.o a.o.orig
	atf_check -s exit:1 -e not-empty ctfconv -l L ro2/a.o
	atf_check cmp -s ro2/a.o a.o.orig
	chmod 755 ro ro2
}

atf_init_test_cases()
{
	atf_add_test_case types
	atf_add_test_case symbols
	atf_add_test_case label
	atf_add_test_case dwarf_versions
	atf_add_test_case compressed
	atf_add_test_case relocatable
	atf_add_test_case optimized
	atf_add_test_case locals
	atf_add_test_case gcc_objects
	atf_add_test_case gcc_empty_bound
	atf_add_test_case cross_unit
	atf_add_test_case no_c_unit
	atf_add_test_case unsupported_version
	atf_add_test_case raw_mode
	atf_add_test_case dump_mode
	atf_add_test_case idempotent
	atf_add_test_case missing_input
	atf_add_test_case directory_input
	atf_add_test_case unwritable_directory
}
