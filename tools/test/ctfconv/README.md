# ctfconv developer tooling

Black-box checks of `ctfconv` against existing containers and against
itself.  Nothing here is installed; nothing here reads or links against
the previous CTF tools, `ctfdump` is used as a dumper only.

## Tools

- `ctfcmp.py [--strict] OLD.txt NEW.txt`: compares two `ctfdump` outputs
  semantically: the canonical type string of every object and function
  symbol, the set of named struct/union/enum/typedef definitions, and the
  counts.  IDs, order, `isroot` and the FORWARD `ctt_type` never matter.
  Divergences that a known rule explains are reported as buckets and do
  not fail (what the previous tools did differently, see below); exit
  status 1 means an unclassified divergence.  `--strict` makes no
  allowance at all: `oldmerge.allow` is not consulted and every bucket
  counts as a difference.  The module name, for `oldmerge.allow`, is the
  basename of `OLD.txt` up to its first dot.

- `difftest.sh [-s|-b] LIST OUTDIR`: for every ELF file named in `LIST`
  that carries a `.SUNW_ctf` section, strips the section from a copy,
  reconverts the copy in place and compares the two containers.  Without
  a flag the files were converted by the previous tools and the known
  differences are bucketed.  `-s` is the **build-level check**: the files
  come from a ctfconv build, so the container the build left in each must
  equal a reconversion of that same file exactly (`--strict`); it is what
  catches a build rule that rewrites the symbol table after the CTF step.
  `-b` is the **refactoring oracle**: the reconverted container must be
  byte-identical to the one in the file, so `LABEL` must name the build's
  label (default `L`).  `CTFCONV` names the binary to test.  Files the
  previous tools gave a container and ctfconv does not (no C compilation
  unit) are counted as `oldctf-newnone`.

- `buildcmp.sh OLDDIR NEWDIR OUTDIR`: compares two kernel build
  directories, `kernel.full` and every `*.ko.full`, with `ctfcmp.py`; for
  a build made the old way against one made post-link.  Secondary to
  `difftest.sh -s`: two builds that share a glue bug look identical to it.

- `mutate.py [-n COUNT] [-s SEED] [-t SECONDS] CTFCONV OBJECT WORKDIR`:
  robustness of the DWARF reader.  Runs `CTFCONV` on mutated copies of
  `OBJECT`: the debug sections truncated at random offsets or with random
  bytes flipped, plus the targeted cases a random flip is unlikely to
  hit (the unit's `DW_AT_addr_base` and `DW_AT_str_offsets_base` set
  beyond the section, the address size set to 0 and 255).  A clean
  conversion or exit 1 with a message is fine; a signal, a timeout or a
  silent failure is reported and the mutated file kept.  Compressed
  debug sections (userland) are inflated first, since objcopy cannot
  update them.  Needs python3 and objcopy.

The unit tests of the tool itself are Kyua tests under
`usr.bin/ctfconv/tests`, built and installed with the tree.

Known divergence by design, kept for the differential runs: a container
written by the old per-object converter into a big-endian object is in
the host's order (an intermediate for the merger); ctfconv writes the
target's order, so such a pair never compares byte for byte.

## What the buckets contain

The previous tools converted every object and merged; ctfconv converts
the linked file once.  `ctfcmp.py` accepts, as buckets, the differences
that follow from that and from known errors in the previous tools'
containers:

- function-local statics typed (the old merge left them placeholders),
  symbols typed that were placeholders, same-name statics typed per
  definition, named definitions the old merge kept as duplicates,
  conflicting definitions it collapsed, types unreachable from any
  symbol or named type, enumerators outside int32 (old keeps the high
  word);
- in userland, what the post-link path sees that the merge of a
  program's own objects never did: the csu objects, every member of a
  static archive, libc's symbol-versioned compat functions; a tag that
  is only a FORWARD in old and has a definition in new;
- the stale symbol types of the old kernel module containers: `kmod.mk`
  generated CTF before the `EXPORT_SYMS` step reordered `.symtab`, which
  CTF indexes by position, so in nine GENERIC modules a group of symbols
  carry each other's types.  The rotation rule (symbols whose old type is
  another differing symbol's new type and vice versa) is symmetric, so it
  applies only in the modules named by a `module rotation` line in
  `oldmerge.allow`; the symbols it absorbs are printed one per line.
  `module kind name` lines cover the old merger's name-matching errors
  the rule does not reach.  The enum-truncation bucket prints the
  enumerators; a new enum appearing there needs a look at its source.
