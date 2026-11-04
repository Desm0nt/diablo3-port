"""Diablo III RomFS (base + BKTR update) via the NSP/XCI/NCA readers in content_reader.py.
Usage: romfs_d3.py list    <base package> <update package> [out.tsv]       -> work/romfs-list.tsv (path, size)
       romfs_d3.py extract <base package> <update package> <dest> [manifest] -> files + work/romfs-manifest.json (IVFC verified first)
Packages are plain NSP / XCI (an NSZ / XCZ is expanded first: tools/ncz2nsp.py; tools/setup_d3.py does that itself). Keys: $NX_PROD_KEYS
or ~/.switch/prod.keys (your own console's). The result is 37 files, ~16 GiB: CPKs/*.cpk + Update/CPKs/Patch*.cpk (docs/d3/CPK.md).
tools/setup_d3.py calls extract() with its own paths.
"""
import os, sys, json, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from content_reader import Package, Patched, verify_ivfc

W = Path(__file__).resolve().parents[1] / 'work'
KEYS = str(Path(os.environ.get('NX_PROD_KEYS', Path.home() / '.switch' / 'prod.keys')))   # your own console keys; never part of this repository


def program(nsp, keys=KEYS, name=None):
    """RomFS section of the program NCA in a package; name: that NCA's content id (needed when the package holds several
    programs, e.g. an XCI with the base game and its update)."""
    want = name.split('.')[0].lower() if name else None
    nca = next(n for n in Package(nsp, keys).ncas() if n.type == 0 and (want is None or n.name.split('.')[0].lower() == want))
    return next(s for s in nca.sections if s.h[3] == 3)


def romfs_view(s, verify, progress=None):
    if verify:
        off, size, _ = verify_ivfc(s, progress)
    else:
        lv = [struct.unpack_from('<QQI', s.h, 0x18 + i * 24) for i in range(6)]
        off, size = lv[-1][0], lv[-1][1]
    read = lambda o, n: s.read(off + o, n)
    h = struct.unpack('<10Q', read(0, 80))
    assert h[0] == 80
    return read, h


def walk(read, h):
    dirs, files = read(h[3], h[4]), read(h[7], h[8])
    out = []

    def rec(di, parent):
        _, sib, child, fi, _, ns = struct.unpack_from('<6I', dirs, di)
        folder = f'{parent}/{dirs[di + 24:di + 24 + ns].decode()}' if ns else parent
        while fi != 0xffffffff:
            _, nxt, fo, fs, _, fns = struct.unpack_from('<IIQQII', files, fi)
            out.append((f'{folder}/{files[fi + 32:fi + 32 + fns].decode()}', h[9] + fo, fs))
            fi = nxt
        if child != 0xffffffff: rec(child, folder)
        if sib != 0xffffffff: rec(sib, parent)
    rec(0, '')
    return out


def extract(base_pkg, update_pkg, keys, dest, manifest, base_nca=None, update_nca=None, verify_progress=None, file_progress=None):
    """IVFC-verify the merged RomFS, then write every file under `dest` and a manifest (path, size, sha256).
    base_nca / update_nca: program NCA names (see program()); verify_progress(done, total) during the IVFC check and
    file_progress(index, count, bytes_done, bytes_total) per file replace the default prints."""
    import hashlib
    read, h = romfs_view(Patched(program(update_pkg, keys, update_nca), program(base_pkg, keys, base_nca)), True, verify_progress)
    man = []
    files = walk(read, h)
    total, done = sum(s for *_, s in files), 0
    for i, (p, o, s) in enumerate(files):
        t = Path(dest) / p.lstrip('/')
        t.parent.mkdir(parents=True, exist_ok=True)
        d = hashlib.sha256()
        with t.open('wb') as f:
            for pos in range(0, s, 1 << 24):
                b = read(o + pos, min(1 << 24, s - pos)); f.write(b); d.update(b)
        man.append({'path': p, 'size': s, 'sha256': d.hexdigest()})
        done += s
        if file_progress: file_progress(i + 1, len(files), done, total)
        else: print(p, s, flush=True)
    Path(manifest).write_text(json.dumps(man, indent=1))
    return man


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ('list', 'extract'): sys.exit(__doc__)
    base_pkg, upd_pkg = sys.argv[2], sys.argv[3]
    if sys.argv[1] == 'list':
        patched = Patched(program(upd_pkg), program(base_pkg))
        print('BKTR mappings', len(patched.reloc), 'subsections', len(patched.subs), flush=True)
        read, h = romfs_view(patched, False)
        ents = walk(read, h)
        out = Path(sys.argv[4]) if len(sys.argv) > 4 else W / 'romfs-list.tsv'
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(''.join(f'{p}\t{s}\n' for p, _, s in ents))
        print(len(ents), 'files', sum(s for *_, s in ents) / 2**30, 'GiB ->', out)
    else:
        if len(sys.argv) < 5: sys.exit(__doc__)
        W.mkdir(parents=True, exist_ok=True)
        extract(base_pkg, upd_pkg, KEYS, sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else W / 'romfs-manifest.json')


if __name__ == '__main__':
    main()
