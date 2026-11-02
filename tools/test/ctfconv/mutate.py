#!/usr/bin/env python3
"""Robustness of the DWARF reader: run ctfconv on mutated copies of an
object.

Usage: mutate.py [-n COUNT] [-s SEED] [-t SECONDS] CTFCONV OBJECT WORKDIR

Three kinds of mutation of .debug_info, .debug_abbrev, .debug_str,
.debug_str_offsets and .debug_addr, written back with objcopy
--update-section: truncation at a random offset, random byte flips (1 to
16 bytes), and the targeted cases a random flip is unlikely to hit:
the unit's DW_AT_addr_base and DW_AT_str_offsets_base set to 0xffffffff
and the unit's address size set to 0 and 255 (DWARF 5 units only; the
attribute is found by walking the unit DIE's abbreviation).

Acceptable outcomes are exit 0 (a clean conversion of what was left) and
exit 1 with a message.  Anything else, a signal, a timeout or an exit
without a message, is a bug and is reported with the mutated file kept
in WORKDIR.  Exit status 0 iff nothing was reported."""
import os, random, shutil, subprocess, sys

SECTIONS = ['.debug_info', '.debug_abbrev', '.debug_str',
            '.debug_str_offsets', '.debug_addr']
DW_AT_str_offsets_base, DW_AT_addr_base = 0x72, 0x73
# Fixed-size forms (bytes) at address size 8 and 32-bit DWARF; variable
# ones are decoded.
FIXED = {0x01: 8, 0x03: None, 0x04: None, 0x05: 2, 0x06: 4, 0x07: 8,
         0x0a: None, 0x0b: 1, 0x0c: 1, 0x0e: 4, 0x10: 4, 0x11: 1, 0x12: 2,
         0x13: 4, 0x14: 8, 0x17: 4, 0x19: 0, 0x1e: 16, 0x1f: 4, 0x20: 8,
         0x21: 0, 0x25: 1, 0x26: 2, 0x27: 3, 0x28: 4, 0x29: 1, 0x2a: 2,
         0x2b: 3, 0x2c: 4}

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)

def section(path, name):
    out = run(['objcopy', '--dump-section', '%s=/dev/stdout' % name, path,
               '/dev/null'])
    return out.stdout if out.returncode == 0 else b''

def uleb(b, i):
    r = s = 0
    while i < len(b):
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not x & 0x80:
            return r, i
    raise ValueError

def abbrev_for(ab, off, code):
    i = off
    while i < len(ab):
        c, i = uleb(ab, i)
        if c == 0:
            return None
        tag, i = uleb(ab, i); i += 1
        attrs = []
        while True:
            at, i = uleb(ab, i); fm, i = uleb(ab, i)
            if at == 0 and fm == 0:
                break
            if fm == 0x21:
                _, i = uleb(ab, i)
            attrs.append((at, fm))
        if c == code:
            return attrs
    return None

def unit_die_attr_offsets(info, ab):
    """(offset in .debug_info, attribute) of every fixed-size attribute
    of the first DIE of every DWARF 5 unit, plus the unit's address size
    offsets."""
    found, asz_offs = [], []
    i = 0
    while i + 12 <= len(info):
        length = int.from_bytes(info[i:i+4], 'little')
        ver = int.from_bytes(info[i+4:i+6], 'little')
        if ver == 5:
            asz_offs.append(i + 7)
            aboff = int.from_bytes(info[i+8:i+12], 'little')
            p = i + 12
            try:
                code, p = uleb(info, p)
                attrs = abbrev_for(ab, aboff, code) or []
                for at, fm in attrs:
                    size = FIXED.get(fm)
                    if size is None:
                        break          # variable: stop walking
                    found.append((p, at, fm, size))
                    p += size
            except ValueError:
                pass
        i += 4 + length
    return found, asz_offs

def write_back(path, name, data):
    tmp = path + '.sec'
    with open(tmp, 'wb') as f:
        f.write(data)
    r = run(['objcopy', '--update-section', '%s=%s' % (name, tmp), path])
    os.unlink(tmp)
    return r.returncode == 0

def classify(ctfconv, path, timeout):
    try:
        r = run([ctfconv, '-l', 'L', path], timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', ''
    if r.returncode == 0:
        return 'ok', ''
    if r.returncode == 1 and r.stderr.strip():
        return 'refused', r.stderr.decode(errors='replace').strip().splitlines()[-1]
    if r.returncode < 0:
        return 'SIGNAL %d' % -r.returncode, r.stderr.decode(errors='replace')[-200:]
    return 'EXIT %d' % r.returncode, r.stderr.decode(errors='replace')[-200:]

def main():
    args = sys.argv[1:]
    count, seed, timeout = 200, 1, 30
    while args and args[0].startswith('-'):
        if args[0] == '-n':
            count = int(args[1]); args = args[2:]
        elif args[0] == '-s':
            seed = int(args[1]); args = args[2:]
        elif args[0] == '-t':
            timeout = int(args[1]); args = args[2:]
        else:
            sys.exit(__doc__)
    if len(args) != 3:
        sys.exit(__doc__)
    ctfconv, obj, work = args
    os.makedirs(work, exist_ok=True)
    rnd = random.Random(seed)
    base = os.path.basename(obj)
    # objcopy cannot update a compressed section (userland is built with
    # -gz=zlib), so every trial starts from an inflated copy; ctfconv
    # reads both kinds.
    src = os.path.join(work, base + '.inflated')
    shutil.copy(obj, src)
    os.chmod(src, 0o644)
    if run(['objcopy', '--decompress-debug-sections', src]).returncode != 0:
        sys.exit('%s: cannot inflate the debug sections' % obj)
    secs = {n: section(src, n) for n in SECTIONS}
    secs = {n: v for n, v in secs.items() if v}
    bad = 0; counts = {}

    def trial(label, name, data):
        nonlocal bad
        path = os.path.join(work, '%s.%s' % (base, label))
        shutil.copy(src, path)
        if not write_back(path, name, data):
            os.unlink(path); return
        kind, msg = classify(ctfconv, path, timeout)
        counts[kind] = counts.get(kind, 0) + 1
        if kind in ('ok', 'refused'):
            os.unlink(path)
        else:
            bad += 1
            print('%s: %s %s' % (os.path.basename(path), kind, msg))

    # Targeted cases first.
    info = secs.get('.debug_info', b'')
    ab = secs.get('.debug_abbrev', b'')
    if info and ab:
        attrs, asz = unit_die_attr_offsets(info, ab)
        for n, (off, at, fm, size) in enumerate(attrs):
            if at in (DW_AT_addr_base, DW_AT_str_offsets_base) and size == 4:
                m = bytearray(info); m[off:off+4] = b'\xff\xff\xff\xff'
                trial('base%d' % n, '.debug_info', bytes(m))
                m = bytearray(info); m[off:off+4] = (len(info) - 2).to_bytes(4, 'little')
                trial('base%dend' % n, '.debug_info', bytes(m))
        for n, off in enumerate(asz):
            for v in (0, 255, 3):
                m = bytearray(info); m[off] = v
                trial('asz%d_%d' % (n, v), '.debug_info', bytes(m))
    # Random truncations and flips.
    names = list(secs)
    for n in range(count):
        name = rnd.choice(names)
        data = secs[name]
        if rnd.random() < 0.3:
            cut = rnd.randrange(0, len(data) + 1)
            trial('trunc%d' % n, name, data[:cut])
        else:
            m = bytearray(data)
            for _ in range(rnd.randint(1, 16)):
                p = rnd.randrange(len(m))
                m[p] = rnd.randrange(256)
            trial('flip%d' % n, name, bytes(m))
    os.unlink(src)
    print('%s: %s; %d bad' % (base, ', '.join('%s=%d' % kv for kv in sorted(counts.items())), bad))
    sys.exit(1 if bad else 0)

main()
