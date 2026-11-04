"""Diablo III (Switch 2.7.7, main build-id 2607a74f...): UIID -> control path table for the host's UI dump (DM_D3_UIDUMP).

A D3 UIID is a bare 64-bit hash; the release build keeps no names at runtime (UIID_ToString main+0x347580 prints 16 hex digits).
UIID_FromPath main+0x347750 = FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3, main+0x6c90) over the path lowercased
(tolower per byte, main+0x7ab0); a child id continues the parent's hash with ".<name>" (main+0x347360 / 0x6b20), so
id("Root.NormalLayer.X.Y") == FNV-1a64("root.normallayer.x.y"). Special ids: -3 Main-Screen, -2 Root, -1 none, 1 Root.NormalLayer,
2 Root.WindowLayer, 3 Root.TopLayer (jump table main+0xeba350).

Sources: (1) every "Root.*" path string in main and its prefixes; (2) the layout files UI/*.ui. In a .ui (SNO, data at +0x10) every
control reference is {u64 hash; u32 offset; u32 size} with hash = FNV-1a64 of the path RELATIVE to the layer, e.g. the name field
"HardcoreSeasonalHeader" carries fnv("ConsolePause_main.LayoutRoot.HardcoreSeasonalHeader") and is followed (+0x18) by the parent field
"ConsolePause_main.LayoutRoot" carrying its own fnv: so the relative path of every control is recovered exactly (hash checked).
The layer a screen root lives in comes from main's "Root.<Layer>.<Screen>_main..." strings; screens main never names get all three layers.
Controls created at run time (list items cloned from templates, ...) stay unnamed.

Usage:
  python tools/d3_cpk.py mextract work/romfs <tmp>/ui 'UI/*.ui'
  python tools/d3_uiid_names.py work/exefs-update/main.raw <tmp>/ui/UI > work/d3-uiid-names.tsv
Output: '<16 hex digits>\t<path>' sorted by id; push it to the device as dm/ui-names.tsv for named dump / focus lines.
"""
import os, re, struct, sys

FNV_OFF, FNV_PRIME, MASK = 0xcbf29ce484222325, 0x100000001b3, (1 << 64) - 1
LAYERS = ('Root.NormalLayer', 'Root.WindowLayer', 'Root.TopLayer')
SPECIAL = {(-3) & MASK: 'Main-Screen', (-2) & MASK: 'Root', 1: 'Root.NormalLayer', 2: 'Root.WindowLayer', 3: 'Root.TopLayer'}


def fnv(s):
    h = FNV_OFF
    for c in s.lower().encode('latin-1', 'replace'):
        h = ((h ^ c) * FNV_PRIME) & MASK
    return h


def prefixes(p):
    parts = p.split('.')
    return ['.'.join(parts[:i]) for i in range(1, len(parts) + 1)]


def ui_paths(data):
    """relative control paths of one .ui file (hash-checked)"""
    body = data[0x10:]
    out = set()
    def ref(k):
        h, off, size = struct.unpack_from('<QII', body, k)
        if not (0 < size < 512 and off + size <= len(body)) or body[off + size - 1] != 0: return None
        s = body[off:off + size - 1]
        if not s or any(c < 0x20 or c > 0x7e for c in s): return None
        return h, s.decode()
    refs = {}
    for k in range(0, len(body) - 16, 8):
        r = ref(k)
        if r: refs[k] = r
    for k, (h, s) in refs.items():
        if s.startswith('$anon$'): continue
        if fnv(s) == h: out.add(s); continue                      # a path field (parent, target, ...)
        p = refs.get(k + 0x18)
        if p and fnv(p[1] + '.' + s) == h: out.add(p[1] + '.' + s)  # name field followed by its parent
    return out


def main():
    raw, uidir = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None
    paths = set()
    b = open(raw, 'rb').read()
    layer_of = {}
    for m in re.finditer(rb'Root\.[A-Za-z0-9_.\-$]+', b):
        s = m.group().decode().rstrip('.')
        paths.update(x for x in prefixes(s) if x.count('.') >= 1)
        parts = s.split('.')
        if len(parts) >= 3 and 'Root.' + parts[1] in LAYERS: layer_of.setdefault(parts[2].lower(), set()).add('Root.' + parts[1])
    nmain, nui = len(paths), 0
    if uidir:
        for fn in sorted(os.listdir(uidir)):
            if not fn.lower().endswith('.ui'): continue
            for rel in ui_paths(open(os.path.join(uidir, fn), 'rb').read()):
                nui += 1
                for layer in layer_of.get(rel.split('.')[0].lower(), LAYERS):
                    for q in prefixes(rel): paths.add(layer + '.' + q)
    out = {}
    for p in paths: out.setdefault(fnv(p), p)
    for k, v in SPECIAL.items(): out[k] = v
    for k in sorted(out): print('%016x\t%s' % (k, out[k]))
    print('# %d ids (%d path strings from main, %d relative paths from layouts)' % (len(out), nmain, nui), file=sys.stderr)


if __name__ == '__main__':
    main()
