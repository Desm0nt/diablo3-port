"""Diablo III (Switch, Eternal Collection) CPK archives: list / extract / merged base+patch view / byte-pattern scan.

The archive format (Blizzard's own console CPK, not CRIWARE) is described in docs/d3/CPK.md; the reader is tools/d3cpklib.py (vendored
D3CPKUnpack cpklib.py, MIT). This file is only the front-end.

Usage (PYTHONUTF8=1 recommended):
  d3_cpk.py info    <cpk>...                          header fields, counts, sector CRC32 check
  d3_cpk.py verify  <cpk>...                          CRC32 + every chunk inflates to its cs2do size (cpklib verify)
  d3_cpk.py list    <cpk> [glob]                      name, size, estimated compressed bytes, stream offset, copies (TSV)
  d3_cpk.py extract <cpk> <dest> [glob]               extract members ('\\' -> '/', '|' -> '__')
  d3_cpk.py mlist   <romfs> [glob]                    merged view: name, size, archive that supplies it (TSV)
  d3_cpk.py mextract <romfs> <dest> [glob]            extract the merged view (each name from the archive that wins)
  d3_cpk.py scan    <romfs|cpk> <hex> [glob]          members whose bytes contain the pattern (count of hits per member)
Globs are fnmatch, case-insensitive, against the name with '/' separators (e.g. 'Shaders/*.shd', '*.shd').

Merged view = what the game sees: <romfs>/CPKs/*.cpk (base; a name present in several base archives is byte-identical there), then
<romfs>/Update/CPKs/*.cpk in the game's own order -- the directory listing sorted by a natural compare (digit runs compared as numbers:
main+0x743890 called from the comparator main+0x74fff0 used by the '*.cpk' enumeration at main+0x74cf70), i.e. Patch2_6_5 < 2_6_6 < 2_6_7 <
2_6_7a < 2_6_8 < 2_6_9 < 2_6_10 < 2_7_0 < ... < 2_7_7. A later archive replaces a name (matched case-insensitively, as the FNV-1a hash of the
lowercased name) supplied by an earlier one.
"""
import fnmatch, os, re, struct, sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3cpklib import CPKArchive  # noqa: E402


def natural_key(s):
    """the game's CPK name order (main+0x743890): digit runs compare as integers, other characters byte-wise"""
    return [(0, int(t), '') if t.isdigit() else (1, 0, t) for t in re.findall(r'\d+|\D', s)]


def archives(root):
    """[(label, path)] in load order; root may be a RomFS dir or a single .cpk"""
    if os.path.isfile(root): return [(os.path.basename(root)[:-4], root)]
    out = []
    for sub, tag in (('CPKs', ''), (os.path.join('Update', 'CPKs'), 'Update/')):
        d = os.path.join(root, sub)
        if os.path.isdir(d):
            names = sorted((f for f in os.listdir(d) if f.lower().endswith('.cpk')), key=natural_key)
            out += [(tag + f[:-4], os.path.join(d, f)) for f in names]
    return out


def norm(name): return name.replace('\\', '/')


def match(name, pat): return pat is None or fnmatch.fnmatch(norm(name).lower(), pat.lower())


def out_path(dest, name):
    n = norm(name).lstrip('/').replace('|', '__')
    for c in '<>:"?*': n = n.replace(c, '_')
    if '..' in n.split('/'): raise ValueError(f'unsafe member name {name!r}')
    return os.path.join(dest, n)


def write(dest, name, data):
    p = out_path(dest, name)
    os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
    with open(p, 'wb') as f: f.write(data)


def sector_spans(a):
    """per compressed sector: (decompressed start, decompressed end, stored bytes used by its chunks) -- for the compressed-size estimate"""
    d, h = a._data, a.header
    cs = a._cs2do; out = []
    for s in range(a._comp_sector_count):
        p = a._first_sec + s * h.comp_sector; end = min(p + h.comp_sector, len(d)); used = 0; dec = 0
        if s == 0 and d[p:p + 6] == bytes(6): p += 6
        while p + 6 <= end:
            lo, hi, z = struct.unpack_from(h.endian + '3H', d, p)
            if z == 0: break
            n = 6 + (z & 0x7fff); used += n; dec += lo | hi << 16; p += n
        out.append((cs[s], cs[s] + dec, used))
    return out


def comp_estimate(spans, starts, off, size):
    """stored bytes attributable to [off, off+size): each overlapped sector's chunk bytes, prorated by decompressed overlap"""
    import bisect
    i = max(bisect.bisect_right(starts, off) - 1, 0); tot = 0.0
    while i < len(spans) and spans[i][0] < off + size:
        a, b, u = spans[i]
        ov = min(b, off + size) - max(a, off)
        if ov > 0 and b > a: tot += u * ov / (b - a)
        i += 1
    return int(round(tot))


def cmd_info(paths):
    for p in paths:
        a = CPKArchive.open(p); h = a.header
        print(f'{p}\n  version {h.version} fmt_const {h.fmt_const} endian {h.endian!r} header {h.header_size} B\n'
              f'  files {h.file_count} locations {h.loc_count} decompressed stream {h.decomp_fs} B\n'
              f'  header_sector {h.header_sector:#x} -> first sector at {a._first_sec:#x}; comp_sector {h.comp_sector:#x} read_sector {h.read_sector:#x}; '
              f'{a._comp_sector_count} sectors\n'
              f'  bit widths: size {h.fs_bc} copies {h.flcnt_bc} first-location {h.flidx_bc} location {h.loc_bc} cs2do {h.cs2do_bc} ds2cs {h.ds2cs_bc}\n'
              f'  sector CRC32 {h.sector_crc32:08x}; top extensions: ' + ', '.join(f'{e} {n}' for e, n in Counter(e.ext for e in a).most_common(8)))


def cmd_verify(paths):
    bad = 0
    for p in paths:
        r = CPKArchive.open(p).verify()
        print(p, 'OK' if r.ok else 'FAIL', {k: v for k, v in vars(r).items() if not isinstance(v, (list, dict))})
        bad += not r.ok
    sys.exit(1 if bad else 0)


def cmd_list(p, pat=None):
    a = CPKArchive.open(p)
    spans = sector_spans(a); starts = [s[0] for s in spans]
    print('name\tsize\tcomp_est\toffset\tcopies')
    for e in sorted(a, key=lambda e: e.offset):
        if match(e.name, pat):
            print(f'{e.name}\t{e.size}\t{comp_estimate(spans, starts, e.offset, e.size) if e.loc_count else 0}\t{e.offset:#x}\t{e.loc_count}')


def cmd_extract(p, dest, pat=None):
    a = CPKArchive.open(p); n = 0
    for e in sorted(a, key=lambda e: e.offset):
        if match(e.name, pat): write(dest, e.name, a.read(e)); n += 1
    print(f'{n} files -> {dest}; zero-filled chunks: {a.chunk_errors}', file=sys.stderr)


def merged(root, pat=None):
    """{lower name: (name, archive label, CPKArchive, entry)} with the later archive winning; plus per-archive override counts"""
    view, stats = {}, Counter()
    for label, p in archives(root):
        a = CPKArchive.open(p)
        for e in a:
            if not match(e.name, pat): continue
            k = e.name.lower()
            if k in view: stats[label + ' replaces'] += 1
            view[k] = (e.name, label, a, e)
    return view, stats


def cmd_mlist(root, pat=None):
    view, stats = merged(root, pat)
    print('name\tsize\tarchive')
    for k in sorted(view):
        n, label, a, e = view[k]; print(f'{n}\t{e.size}\t{label}')
    print(f'{len(view)} names; ' + ', '.join(f'{k} {v}' for k, v in stats.items()), file=sys.stderr)


def cmd_mextract(root, dest, pat=None):
    view, stats = merged(root, pat)
    by_arc = {}
    for k, (n, label, a, e) in view.items(): by_arc.setdefault(label, (a, []))[1].append(e)
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, '_sources.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('name\tsize\tarchive\n')
        for k in sorted(view): n, label, a, e = view[k]; f.write(f'{n}\t{e.size}\t{label}\n')
    for label, (a, es) in by_arc.items():
        for e in sorted(es, key=lambda e: e.offset): write(dest, e.name, a.read(e))
        print(f'{label}: {len(es)} files', file=sys.stderr)
    print(f'{len(view)} names -> {dest} (_sources.tsv); ' + ', '.join(f'{k} {v}' for k, v in stats.items()), file=sys.stderr)


def cmd_scan(root, hexpat, pat=None):
    needle = bytes.fromhex(hexpat); hits = Counter()
    for label, p in archives(root):
        a = CPKArchive.open(p)
        for e in sorted(a, key=lambda e: e.offset):
            if not e.loc_count or not e.size or not match(e.name, pat): continue
            c = a.read(e).count(needle)
            if c: hits[(label, e.name)] = c
        print(f'{label}: done', file=sys.stderr)
    print('archive\tname\thits')
    for (label, n), c in sorted(hits.items()): print(f'{label}\t{n}\t{c}')


def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    c, args = sys.argv[1], sys.argv[2:]
    {'info': lambda: cmd_info(args), 'verify': lambda: cmd_verify(args), 'list': lambda: cmd_list(*args), 'extract': lambda: cmd_extract(*args),
     'mlist': lambda: cmd_mlist(*args), 'mextract': lambda: cmd_mextract(*args), 'scan': lambda: cmd_scan(*args)}.get(c, lambda: sys.exit(__doc__))()


if __name__ == '__main__':
    main()
