"""Dump Diablo III's (Switch 2.7.7, main build-id 2607a74f...) named UI callback tables.

D3's console UI layouts bind events (OnButton1, OnLeftStick, OnDPadLeft, *_Update, *_OnClick, ...) to native handlers by NAME.
Two static initializers fill name -> handler tables in .bss with 0x18-byte entries {char *name; u32 hash; void *fn}:
  main+0x3c2c20 .. 0x3c9968  (table at main+0x186f168)
  main+0x3c9970 .. 0x3cc27c  (second table)
hash = main+0xa49690: h = h * 33 + (signed char)c, starting at 0, case-sensitive.
The handler pointers come from GOT slots fixed by R_AARCH64_RELATIVE relocations, so they are resolved through .rela.dyn.

Usage: python tools/d3_ui_callbacks.py [work/exefs-update/main.raw] > work/d3-ui-callbacks.tsv
Output TSV: entry address, handler (main offset or -), hash, name.  Read-only analysis; needs capstone.
"""
import re, struct, sys
import capstone

RAW = sys.argv[1] if len(sys.argv) > 1 else 'work/exefs-update/main.raw'
b = open(RAW, 'rb').read()
TEXT_END = 0xc72b90
INITS = [(0x3c2c20, 0x3c9968), (0x3c9970, 0x3cc27c)]

# --- dynamic relocations (RELATIVE) to resolve GOT slots
mo = struct.unpack_from('<I', b, 4)[0]
assert b[mo:mo + 4] == b'MOD0'
dyn = mo + struct.unpack_from('<i', b, mo + 4)[0]
dt = {}
o = dyn
while True:
    t, v = struct.unpack_from('<qQ', b, o); o += 16
    if t == 0: break
    dt.setdefault(t, v)
REL = {}
for o in range(dt[7], dt[7] + dt[8], 24):
    off, info, add = struct.unpack_from('<QQq', b, o)
    if info & 0xffffffff == 1027: REL[off] = add
ptr = lambda a: REL.get(a, struct.unpack_from('<Q', b, a)[0])

def cstr(x):
    if 0 <= x < len(b):
        s = b[x:b.find(b'\0', x)]
        if 0 < len(s) < 200 and all(32 <= c < 127 for c in s): return s.decode()
    return None

def h33(s):
    h = 0
    for c in s.encode(): h = (h * 33 + (c - 256 if c > 127 else c)) & 0xffffffff
    return h

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.skipdata = True
mem = {}   # absolute address -> ('s', str) | ('f', fn) | ('z',)
for lo, hi in INITS:
    val = {}
    for i in md.disasm(b[lo:hi + 4], lo):
        m, ops = i.mnemonic, [x.strip() for x in i.op_str.split(',')]
        if m == 'adrp':
            val[ops[0]] = int(ops[1].lstrip('#'), 16)
        elif m == 'add' and len(ops) == 3 and ops[2].startswith('#') and ops[1] in val:
            val[ops[0]] = val[ops[1]] + int(ops[2].lstrip('#'), 16)
        elif m == 'ldr' and len(ops) >= 2:
            mm = re.match(r'(x\d+), \[(x\d+), #(0x[0-9a-f]+)\]$', i.op_str)
            if mm and mm.group(2) in val:
                val[mm.group(1)] = ('f', ptr(val[mm.group(2)] + int(mm.group(3), 16)))
            else: val.pop(ops[0], None)
        elif m in ('str', 'stp'):
            mm = re.match(r'(x\d+|xzr)(?:, (x\d+|xzr))?, \[(x\d+), #(0x[0-9a-f]+)\]$', i.op_str)
            if mm and mm.group(3) in val and isinstance(val[mm.group(3)], int):
                base = val[mm.group(3)] + int(mm.group(4), 16)
                regs = [mm.group(1)] + ([mm.group(2)] if mm.group(2) else [])
                for k, r in enumerate(regs):
                    a = base + 8 * k; v = ('z',) if r == 'xzr' else val.get(r)
                    if isinstance(v, int): v = ('s', cstr(v)) if cstr(v) else None
                    if v: mem[a] = v
        elif m in ('bl', 'blr'):
            for r in ['x%d' % k for k in range(19)]: val.pop(r, None)

print('entry\thandler\thash\tname')
n = 0
for a in sorted(mem):
    v = mem[a]
    if v[0] != 's': continue
    f = mem.get(a + 0x10)
    fn = '%#08x' % f[1] if f and f[0] == 'f' and 0 < f[1] < TEXT_END else '-'
    print('%#x\t%s\t%08x\t%s' % (a, fn, h33(v[1]), v[1])); n += 1
print('# %d entries' % n, file=sys.stderr)
