#!/usr/bin/env python3
"""Semantic comparison of two ctfdump outputs (Phase 1 harness).

Compares:
  1. canonical type string of every object and function symbol;
  2. the set of named struct/union/enum/typedef definitions;
  3. counts.
IDs, order, isroot and the FORWARD ctt_type are ignored by construction.

Usage: ctfcmp.py [--strict] OLD.txt NEW.txt
The module name, for the allowlist of known errors in the old containers
(oldmerge.allow), is the basename of OLD.txt up to its first dot
(difftest.sh names it so).

--strict makes no allowance at all: oldmerge.allow is not consulted and
every bucket counts as a difference.  It is for comparing the container
a build left in a file with a reconversion of that same file by the same
tool, where any divergence is a bug in the build glue (a symbol table
rewritten after the CTF step) or in the tool.
"""
import os, re, sys

KIND_RE = re.compile(r'^  [<\[](\d+)[>\]] ?(.*)$')
ALLOWLIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'oldmerge.allow')

def parse(path):
    types, objs, funcs = {}, [], []
    sect = None
    cur = None
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('- '):
            sect = line[2:].split(' -')[0].strip()
            cur = None
            continue
        if sect == 'Types':
            m = KIND_RE.match(line)
            if m:
                tid, rest = int(m.group(1)), m.group(2)
                cur = {'id': tid, 'raw': rest, 'members': []}
                types[tid] = cur
                continue
            if line.startswith('\t') and cur is not None:
                cur['members'].append(line.strip())
        elif sect == 'Data Objects':
            # ctfdump prints the type ID in a 5-wide field; a wider ID
            # runs into the name (identifiers never start with a digit).
            m = re.match(r'^  \[(\d+)\] (\d+)\s*([^\s(]*) \((\d+)\)', line)
            if m:
                objs.append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))))
        elif sect == 'Functions':
            m = re.match(r'^  \[(\d+)\] FUNC \((\S+)\) returns: (\d+) args: \(([^)]*)\)', line)
            if m:
                args = [int(a) for a in m.group(4).split(', ') if a.strip()]
                funcs.append((int(m.group(1)), m.group(2), int(m.group(3)), args))
    return types, objs, funcs

def kind_of(t):
    raw = t['raw']
    if raw == '':
        return 'HOLE', None, raw
    kind = raw.split(' ', 1)[0]
    rest = raw[len(kind):].strip()
    return kind, rest, raw

class Canon:
    def __init__(self, types):
        self.types = types
        self.memo = {}

    def c(self, tid, top=False, depth=0):
        if tid == 0:
            return 'none'
        if tid not in self.types:
            return '?%d' % tid
        key = (tid, top)
        if key in self.memo:
            return self.memo[key]
        if depth > 60:
            return '...'
        t = self.types[tid]
        kind, rest, raw = kind_of(t)
        out = None
        if kind in ('INTEGER', 'FLOAT'):
            m = re.match(r'^(.*) encoding=(.*) offset=\d+ bits=(\d+)$', rest)
            enc = m.group(2).replace('0x0', '')
            out = '%s %s[%s,%s]' % (kind.lower(), m.group(1), m.group(3), enc.strip())
        elif kind in ('POINTER', 'VOLATILE', 'CONST', 'RESTRICT', 'TYPEDEF'):
            m = re.match(r'^(.*) refers to (\d+)$', rest)
            name, ref = m.group(1), int(m.group(2))
            if kind == 'TYPEDEF':
                if top:
                    out = 'typedef %s = %s' % (name, self.c(ref, False, depth + 1))
                else:
                    out = 'typedef %s' % name
            else:
                out = '%s(%s)' % (kind.lower(), self.c(ref, False, depth + 1))
        elif kind == 'ARRAY':
            m = re.match(r'^.* content: (\d+) index: (\d+) nelems: (\d+)$', rest)
            out = 'array[%s](%s; idx %s)' % (m.group(3), self.c(int(m.group(1)), False, depth + 1),
                                             self.c(int(m.group(2)), False, depth + 1))
        elif kind in ('STRUCT', 'UNION'):
            m = re.match(r'^(.*) \((\d+) bytes\)$', rest)
            name, size = m.group(1), m.group(2)
            if name != '(anon)' and not top:
                out = '%s %s' % (kind.lower(), name)
            else:
                mem = []
                for ml in t['members']:
                    mm = re.match(r'^(.*) type=(\d+) off=(\d+)$', ml)
                    mem.append('%s@%s:%s' % (mm.group(1), mm.group(3), self.c(int(mm.group(2)), False, depth + 1)))
                out = '%s %s {%s bytes; %s}' % (kind.lower(), name, size, '; '.join(mem))
        elif kind == 'ENUM':
            name = rest
            if name != '(anon)' and not top:
                out = 'enum %s' % name
            else:
                out = 'enum %s {%s}' % (name, '; '.join(t['members']))
        elif kind == 'FORWARD':
            out = 'forward %s' % rest
        elif kind == 'FUNCTION':
            m = re.match(r'^.* returns: (\d+) args: \(([^)]*)\)$', rest)
            args = [a for a in m.group(2).split(', ') if a.strip()]
            out = 'func(%s)(%s)' % (self.c(int(m.group(1)), False, depth + 1),
                                   ', '.join('...' if a == '0' else self.c(int(a), False, depth + 1) for a in args))
        elif kind == 'HOLE':
            out = 'hole'
        else:
            out = raw
        self.memo[key] = out
        return out

def closure(types, cn, tid, acc, depth=0):
    """Canonical strings of every type reachable from tid."""
    if tid == 0 or tid not in types or depth > 60:
        return
    t = types[tid]
    c = cn.c(tid, top=True)
    if c in acc:
        return
    acc.add(c)
    kind, rest, raw = kind_of(t)
    refs = []
    if kind in ('POINTER', 'VOLATILE', 'CONST', 'RESTRICT', 'TYPEDEF'):
        refs.append(int(rest.rsplit(' ', 1)[1]))
    elif kind == 'ARRAY':
        m = re.match(r'^.* content: (\d+) index: (\d+) nelems: (\d+)$', rest)
        refs += [int(m.group(1)), int(m.group(2))]
    elif kind in ('STRUCT', 'UNION'):
        for ml in t['members']:
            mm = re.match(r'^(.*) type=(\d+) off=(\d+)$', ml)
            refs.append(int(mm.group(2)))
    elif kind == 'FUNCTION':
        m = re.match(r'^.* returns: (\d+) args: \(([^)]*)\)$', rest)
        refs.append(int(m.group(1)))
        refs += [int(a) for a in m.group(2).split(', ') if a.strip()]
    for r in refs:
        closure(types, cn, r, acc, depth + 1)

class Summary:
    pass

def summarize(path):
    types, objs, funcs = parse(path)
    cn = Canon(types)
    s = Summary()
    s.types, s.cn = types, cn
    # types reachable from function-local statics (``f.x'' symbols)
    s.local = set()
    for idx, tid, name, symidx in objs:
        if '.' in name:
            closure(types, cn, tid, s.local)
    # Symbols are keyed by (kind, name): a multiset of canonical strings,
    # so that symbol-table order differences do not matter.
    syms, symtids = {}, {}
    for idx, tid, name, symidx in objs:
        syms.setdefault(('obj', name), []).append(cn.c(tid))
        symtids.setdefault(('obj', name), []).append(tid)
    for idx, name, ret, args in funcs:
        syms.setdefault(('func', name), []).append('func(%s)(%s)' % (cn.c(ret), ', '.join(cn.c(a) for a in args)))
        symtids.setdefault(('func', name), []).extend([ret] + args)
    for k in syms:
        syms[k] = sorted(syms[k])
    s.syms = syms
    s.symtids = symtids    # every type id a symbol of that name refers to
    # Named definitions: key -> list of (canonical string, type id).
    named = {}
    for tid, t in types.items():
        kind, rest, raw = kind_of(t)
        if kind in ('STRUCT', 'UNION', 'ENUM', 'FORWARD', 'TYPEDEF'):
            if kind == 'TYPEDEF':
                name = rest.split(' refers to ')[0]
            elif kind in ('STRUCT', 'UNION'):
                name = rest.rsplit(' (', 1)[0]
            else:
                name = rest
            if name == '(anon)':
                continue
            key = ('struct/union' if kind in ('STRUCT', 'UNION', 'FORWARD') else kind.lower(), name)
            named.setdefault(key, []).append((cn.c(tid, top=True), tid))
    s.named = named
    s.ntypes = sum(1 for t in types.values() if t['raw'] != '')
    s.nobjs, s.nfuncs = len(objs), len(funcs)
    s.allcanon = sorted(cn.c(tid, top=True) for tid in types if types[tid]['raw'] != '')
    return s

def load_allowlist(module):
    """What oldmerge.allow says about this module: the set of (kind,
    name) symbols whose old type is a known error of the old container
    (lines ``module kind name``), and whether the rotation rule may be
    applied to it at all (a line ``module rotation``)."""
    allow, rotation = set(), False
    try:
        f = open(ALLOWLIST)
    except OSError:
        return allow, rotation
    for line in f:
        line = line.split('#', 1)[0].split()
        if len(line) == 3 and line[0] == module:
            allow.add((line[1], line[2]))
        elif len(line) == 2 and line[0] == module and line[1] == 'rotation':
            rotation = True
    return allow, rotation

def multiset_contains(b, a):
    """True when multiset b contains multiset a."""
    rest = list(b)
    for x in a:
        if x not in rest:
            return False
        rest.remove(x)
    return True

def compare_symbols(old, new, allow, rotation_ok):
    """Returns (rc, report lines, canonical strings of the types
    reachable from the symbols that are typed in new and were
    placeholders in old) for the per-symbol comparison."""
    rc, out = 0, []
    expected = newly_typed = perdef = more = 0
    diffs = {}
    new_reach = set()
    for k in sorted(set(old.syms) | set(new.syms)):
        a, b = old.syms.get(k, ['<placeholder>']), new.syms.get(k, ['<placeholder>'])
        if a == b:
            continue
        # Expected improvement (plan, Phase 1 exit): old placeholder,
        # new typed: function-local statics (``f.x''), weak aliases,
        # anything else the old tool could not match by name.  In
        # userland also every symbol of an object the old path never
        # converted (csu, the members of a static archive).
        # The types reachable from such symbols are expected too.
        if all(x in ('none', '<placeholder>') for x in a) \
           and not any(x in ('none', '<placeholder>') for x in b):
            if '.' in k[1]:
                expected += 1
            else:
                newly_typed += 1
                out.append('newly typed: %s %s -> %s' % (k[0], k[1], b))
            for tid in new.symtids.get(k, []):
                closure(new.types, new.cn, tid, new_reach)
            continue
        # Several same-name symbols (static in more than one file): old
        # gave all of them the first definition's type, new types each
        # by its own definition (address matching).
        if len(a) > 1 and len(set(a)) == 1 and len(b) == len(a) and a[0] in b:
            perdef += 1
            continue
        # Several same-name symbols of which old typed only some (ctfdump
        # omits placeholder functions, so old lists fewer, and prints
        # placeholder objects as ``none''): the symbol-versioned compat
        # functions of libc, static tables from a header included by two
        # files.  New types every one by address; what old typed must be
        # among them.
        a_typed = [x for x in a if x not in ('none', '<placeholder>')]
        if (len(b) > len(a) or len(a_typed) < len(a)) and len(b) >= len(a) \
           and not any(x in ('none', '<placeholder>') for x in b) \
           and multiset_contains(b, a_typed):
            more += 1
            out.append('more typed: %s %s: %d in old, %d in new' % (k[0], k[1], len(a), len(b)))
            for tid in new.symtids.get(k, []):
                closure(new.types, new.cn, tid, new_reach)
            continue
        diffs[k] = (a, b)
    # Stale old container (results file, section 1): kmod.mk generated
    # the module's CTF before the EXPORT_SYMS/PREFIX_SYMS step reordered
    # .symtab, and CTF indexes symbols by table position, so within one
    # module the old types of a group of symbols are a permutation of
    # their new types, i.e. each symbol carries the type of another one.
    # ctfmerge was right when it ran.  A symbol whose old type is
    # another differing symbol's new type and whose new type is another
    # differing symbol's old type is in such a group.  The rule is
    # symmetric: a permutation the new tool introduced would look the
    # same.  So it applies only in the modules where the error was
    # verified against the source (``module rotation'' in
    # oldmerge.allow), and the symbols it absorbs are listed one per
    # line for review.  Names in oldmerge.allow cover the rest.
    rotated, allowed = [], []
    for k, (a, b) in sorted(diffs.items()):
        others = [j for j in diffs if j != k and j[0] == k[0]]
        onew = set(x for j in others for x in diffs[j][1])
        oold = set(x for j in others for x in diffs[j][0])
        if rotation_ok and len(a) == 1 and len(b) == 1 \
           and a[0] in onew and b[0] in oold:
            rotated.append('  rotated %s %s: old %s, new %s' % (k[0], k[1], a[0], b[0]))
            continue
        if k in allow:
            allowed.append('  allowed %s %s: old %s, new %s' % (k[0], k[1], a, b))
            continue
        rc = 1
        out.append('SYMBOL %s %s:\n   old: %s\n   new: %s' % (k[0], k[1], a, b))
    if expected:
        out.append('expected improvements (local statics typed): %d' % expected)
    if newly_typed:
        out.append('bucket: %d symbol(s) typed in new, placeholder in old' % newly_typed)
    if perdef:
        out.append('bucket: %d same-name symbol group(s) typed per definition in new, one type in old' % perdef)
    if more:
        out.append('bucket: %d same-name symbol group(s) with more members typed in new' % more)
    if rotated:
        out.append('bucket: %d symbol(s) whose old type is another symbol\'s (old container stale after .symtab reorder)' % len(rotated))
        out += rotated
    if allowed:
        out.append('bucket: %d symbol(s) in oldmerge.allow (known error in the old container)' % len(allowed))
        out += allowed
    return rc, out, new_reach

ENUM_RE = re.compile(r'^enum (.*) \{(.*)\}$')

def enum_members(canon):
    """[(name, value)] of a canonical top-level enum string, else None."""
    m = ENUM_RE.match(canon)
    if not m:
        return None
    out = []
    for e in m.group(2).split('; '):
        if ' = ' in e:
            n, v = e.rsplit(' = ', 1)
            out.append((n, v))
    return out

def enum_trunc(a, b):
    """The enumerators whose value differs between the single old and
    new definitions of one enum, when the enumerator lists are the
    same: how the old tools and ctfconv truncate a value outside
    int32 (old keeps the high 32 bits, new the low 32 bits)."""
    if len(a) != 1 or len(b) != 1:
        return None
    ea, eb = enum_members(a[0]), enum_members(b[0])
    if ea is None or eb is None or [n for n, v in ea] != [n for n, v in eb]:
        return None
    return [(n, va, vb) for (n, va), (_, vb) in zip(ea, eb) if va != vb]

FWD_RE = re.compile(r'forward ([^\s;{}()]+)')

def compare_named(old, new):
    """Returns (rc, report lines, canonical strings of old-only types
    explained here, same for new-only, and a function rewriting a
    canonical string of old with the forwards new resolved) for the
    named-definition comparison."""
    rc, out = 0, []
    olddups = kept = newnames = fwd = fwdref = 0
    old_types, kept_types = set(), set()
    # Tags that are only a FORWARD in old and have a definition in new:
    # the definition lives in an object the old path never converted
    # (csu, a static archive's member: libc's empty struct pthread).
    # Every reference to such a tag reads ``forward X'' in old and
    # ``struct X'' in new; fix() rewrites the former.
    resolved = {}
    for k in set(old.named) & set(new.named):
        a = [c for c, t in old.named[k]]
        b = [c for c, t in new.named[k]]
        if k[0] == 'struct/union' and all(c.startswith('forward ') for c in a):
            defs = [c for c in b if not c.startswith('forward ')]
            if defs:
                resolved[k[1]] = defs[0].split(' ', 1)[0]
    def fix(c):
        return FWD_RE.sub(lambda m: '%s %s' % (resolved[m.group(1)], m.group(1))
                          if m.group(1) in resolved else m.group(0), c)
    for k in sorted(set(old.named) | set(new.named)):
        a = sorted(c for c, t in old.named.get(k, []))
        b = sorted(c for c, t in new.named.get(k, []))
        if a == b:
            continue
        if k[0] == 'struct/union' and k[1] in resolved:
            fwd += 1
            old_types.update(a)
            for c, tid in new.named[k]:
                closure(new.types, new.cn, tid, kept_types)
            continue
        if sorted(fix(c) for c in a) == b:
            fwdref += 1
            old_types.update(a)
            kept_types.update(b)
            continue
        # One enum, same enumerators, values outside int32 truncated
        # differently (results file, section 2): old-tool bug.
        trunc = enum_trunc(a, b) if k[0] == 'enum' else None
        if trunc:
            out.append('bucket: enum %s: %d enumerator(s) outside int32, old keeps the high word: %s' % (
                k[1], len(trunc), ', '.join('%s old %s new %s' % t for t in trunc)))
            old_types.add(a[0])
            kept_types.add(b[0])
            continue
        # Same definitions, old just kept duplicate copies (its merge
        # distinguishes them by deeper structure): expected.
        if set(a) == set(b) and len(a) > len(b):
            olddups += 1
            continue
        # New keeps a conflicting definition (different size or
        # members) that old collapsed into another one: expected, and
        # so is everything reachable only from it.  A name old does not
        # have at all comes from an object old never converted (csu,
        # a static archive's members); counted apart.
        if set(a) < set(b):
            if a:
                kept += 1
            else:
                newnames += 1
            for c, tid in new.named[k]:
                if c not in set(a):
                    closure(new.types, new.cn, tid, kept_types)
            continue
        rc = 1
        out.append('NAMED %s %s:\n   old: %s\n   new: %s' % (k[0], k[1], a, b))
    if olddups:
        out.append('bucket: %d named definition(s) duplicated in old, merged in new' % olddups)
    if kept:
        out.append('bucket: %d conflicting named definition(s) kept in new, collapsed in old' % kept)
    if newnames:
        out.append('bucket: %d named definition(s) only in new (from objects old never converted)' % newnames)
    if fwd:
        out.append('bucket: %d forward(s) in old resolved in new (definition in an object old never converted): %s' % (
            fwd, ', '.join(sorted(resolved))))
    if fwdref:
        out.append('bucket: %d named definition(s) differing only in references to those forwards' % fwdref)
    return rc, out, old_types, kept_types, fix

def compare_types(old, new, old_types, kept_types, new_reach, fix):
    rc, out = 0, []
    # a type of old that only differs by a forward new resolved is the
    # same type
    fixed_old = set(fix(c) for c in old.allcanon)
    onlyold = sorted(c for c in set(old.allcanon) - set(new.allcanon) - old_types
                     if fix(c) not in new.allcanon)
    # types present only because a local static, or a symbol old left
    # as a placeholder, is now typed are expected
    onlynew = sorted(set(new.allcanon) - set(old.allcanon) - new.local - kept_types - new_reach - fixed_old)
    nexpected = len(set(new.allcanon) - set(old.allcanon)) - len(onlynew)
    if nexpected:
        out.append('expected improvements (types of local statics, of newly typed symbols, of kept definitions): %d' % nexpected)
    # A type present on one side only and reachable from neither a
    # symbol nor a named type: nothing in D can name it.  Old emits such
    # anonymous aggregates and enums inconsistently; new emits every
    # enum and no other such type.  Buckets, not failures.
    unreach_old = [x for x in onlyold if x.startswith(('struct (anon)', 'union (anon)', 'enum (anon)', 'pointer(', 'array[', 'const(', 'volatile(', 'func('))]
    unreach_new = [x for x in onlynew if x.startswith('enum (anon)')]
    if unreach_old:
        out.append('bucket: %d type(s) unreachable from symbols, old only (anonymous, local)' % len(unreach_old))
    if unreach_new:
        out.append('bucket: %d anonymous enum(s) unreachable from symbols, new only' % len(unreach_new))
    for x in onlyold:
        if x not in unreach_old:
            out.append('TYPE only in old: %s' % x); rc = 1
    for x in onlynew:
        if x not in unreach_new:
            out.append('TYPE only in new: %s' % x); rc = 1
    return rc, out

BUCKET_PREFIXES = ('bucket:', 'expected improvements', 'newly typed:',
                   'more typed:')

def main():
    args = sys.argv[1:]
    strict = False
    if args and args[0] == '--strict':
        strict = True
        args = args[1:]
    if len(args) != 2:
        sys.stderr.write('usage: ctfcmp.py [--strict] OLD.txt NEW.txt\n')
        return 2
    oldpath, newpath = args
    module = os.path.basename(oldpath).split('.')[0]
    old, new = summarize(oldpath), summarize(newpath)
    print('counts: types %d -> %d, objects %d -> %d, functions %d -> %d' % (
        old.ntypes, new.ntypes, old.nobjs, new.nobjs, old.nfuncs, new.nfuncs))
    if strict:
        allow, rotation_ok = set(), False
    else:
        allow, rotation_ok = load_allowlist(module)
    rc1, out1, new_reach = compare_symbols(old, new, allow, rotation_ok)
    rc2, out2, old_types, kept_types, fix = compare_named(old, new)
    rc3, out3 = compare_types(old, new, old_types, kept_types, new_reach, fix)
    for line in out1 + out2 + out3:
        print(line)
    rc = rc1 | rc2 | rc3
    if strict and any(l.startswith(BUCKET_PREFIXES) for l in out1 + out2 + out3):
        print('strict: buckets count as differences')
        rc = 1
    print('result: %s' % ('IDENTICAL' if rc == 0 else 'DIFFERENT'))
    return rc

if __name__ == '__main__':
    sys.exit(main())
