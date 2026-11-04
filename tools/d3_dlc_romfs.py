"""Diablo III add-on content (AOC / language-pack DLC) RomFS listing and extraction.

Input: a DLC package (plain NSP / XCI; an NSZ is expanded with tools/ncz2nsp.py first, tools/setup_d3.py does that itself).
AOC index = title ID - base title ID (01001B300B9BE000) - 0x1000, taken from the meta NCA's CNMT (type 0x82 AddOnContent,
application id field); a [01001B300B9BF0XX] in the file name is cross-checked. Language packs are indices 1..17 (e.g. 1 enGB,
10 ruRU); the host reads them from Download/Diablo3Mobile/aoc/<index> (dm/android/jni_android.c, dm/host/hle_misc.c). Every data /
public-data NCA (content type 4 / 5) with an IVFC RomFS section is listed / extracted:

    d3_dlc_romfs.py list    <pkg> [--out TSV]                       -> work/dlc-<index>-list.tsv (path, size) + summary
    d3_dlc_romfs.py extract <pkg> [--dest DIR] [--only SUBSTR] [--no-verify]
                                                                    -> DIR/<index>/... + DIR/manifest.json (default DIR work/romfs-dlc)
Prints the adb push command to use (never runs adb):
    adb push work/romfs-dlc/<index> /sdcard/Download/Diablo3Mobile/aoc/<index>
Keys: --keys, else $NX_PROD_KEYS, else ~/.switch/prod.keys (your own console's; never part of this repository).
"""
import argparse, hashlib, json, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from content_reader import Package, verify_ivfc
from romfs_d3 import walk

ROOT = Path(__file__).resolve().parents[1]
W = ROOT / 'work'
KEYS = os.environ.get('NX_PROD_KEYS', str(Path.home() / '.switch' / 'prod.keys'))
BASE_TID = 0x01001B300B9BE000


def cnmt_info(pk):
    """(title id, type, version, application id) from the meta NCA's .cnmt, or None."""
    for nca in pk.ncas():
        if nca.type != 1: continue
        for s in nca.sections:
            if s.h[3] != 2: continue      # PFS0
            data, ents = s.pfs()
            for name, o, n in ents:
                if name.endswith('.cnmt'):
                    c = data[o:o + n]
                    tid, ver = struct.unpack_from('<QI', c, 0)
                    app = struct.unpack_from('<Q', c, 0x20)[0]   # extended header: application id
                    return tid, c[0xC], ver, app
    return None


def romfs_sections(pk, verify, progress=None, ncas=None):
    for nca in pk.ncas():
        if nca.type not in (4, 5): continue   # data / public data (AOC)
        if ncas is not None and nca.name.split('.')[0].lower() not in ncas: continue
        for s in nca.sections:
            if s.h[3] != 3: continue          # IVFC = RomFS
            if verify:
                off, _, _ = verify_ivfc(s, progress)
            else:
                off = struct.unpack_from('<QQI', s.h, 0x18 + 5 * 24)[0]
            read = lambda o, n, s=s, off=off: s.read(off + o, n)
            h = struct.unpack('<10Q', read(0, 80)); assert h[0] == 80, 'RomFS header'
            yield nca, read, h


def open_pkg(pkg, keys=KEYS, quiet=False, tid=None):
    """(Package, title id, AOC index, version). tid: the DLC's title id when the caller already knows it (a package with several
    titles, e.g. an XCI); otherwise it comes from the CNMT."""
    pkg = Path(pkg)
    pk = Package(pkg, keys)
    if tid is not None:
        return pk, tid, tid - BASE_TID - 0x1000, 0
    m = re.search(r'\[(0100[0-9A-Fa-f]{12})\]', pkg.name)
    tid = int(m.group(1), 16) if m else None
    info = cnmt_info(pk)
    if info:
        ctid, ctype, ver, app = info
        if not quiet: print(f'CNMT: title {ctid:016X} type 0x{ctype:02X} v{ver} application {app:016X}')
        assert tid is None or tid == ctid, 'file name title id != CNMT'
        assert ctype == 0x82 and app == BASE_TID, 'not a Diablo III AddOnContent'
        tid = ctid
    if tid is None: raise ValueError(f'{pkg.name}: no content meta and no title id in the name')
    idx = tid - BASE_TID - 0x1000
    if not quiet: print(f'AOC index {idx} (title {tid:016X}) {pkg.name}')
    return pk, tid, idx, (info[2] if info else 0)


def push_hint(idx, dest='work/romfs-dlc'):
    print(f'push: adb push {dest}/{idx} /sdcard/Download/Diablo3Mobile/aoc/{idx}')


def extract(pkg, dest_root, keys=KEYS, only=None, verify=True, verify_progress=None, file_progress=None, manifest=None, tid=None,
            ncas=None):
    """Extract the RomFS of a DLC package into dest_root/<index>/ and record it in `manifest` (default dest_root/manifest.json);
    returns the index. tid / ncas (content ids of its data NCAs): the DLC to take from a package that holds several titles."""
    pk, tid, idx, ver = open_pkg(pkg, keys, quiet=file_progress is not None, tid=tid)
    dest_root = Path(dest_root); dest = dest_root / str(idx)
    files = []
    try:
        for nca, read, h in romfs_sections(pk, verify, verify_progress, ncas):
            ents = [e for e in walk(read, h) if not only or only in e[0]]
            for i, (path, o, n) in enumerate(ents):
                t = dest / path.lstrip('/'); t.parent.mkdir(parents=True, exist_ok=True)
                d = hashlib.sha256()
                with t.open('wb') as f:
                    for pos in range(0, n, 1 << 24):
                        b = read(o + pos, min(1 << 24, n - pos)); f.write(b); d.update(b)
                files.append({'path': path, 'size': n, 'sha256': d.hexdigest(), 'nca': nca.name})
                if file_progress: file_progress(i + 1, len(ents), path)
                else: print(f'  [{idx}] {path} {n}', flush=True)
    finally:
        pk.close()
    mf = Path(manifest) if manifest else dest_root / 'manifest.json'
    man = {m['index']: m for m in json.loads(mf.read_text())} if mf.exists() else {}
    man[idx] = {'index': idx, 'title_id': f'{tid:016X}', 'version': ver, 'partial': bool(only), 'files': files}
    mf.parent.mkdir(parents=True, exist_ok=True)
    mf.write_text(json.dumps([man[k] for k in sorted(man)], indent=1))
    if not file_progress: print(f'AOC {idx}: {len(files)} files, {sum(f["size"] for f in files) / 2**20:.1f} MiB -> {dest}')
    return idx


def cmd_list(a):
    pk, tid, idx, _ = open_pkg(a.pkg, a.keys)
    rows = []
    for nca, read, h in romfs_sections(pk, False):
        print(f'NCA {nca.name} type {nca.type} title {nca.title:016X}')
        rows += [(p, n) for p, _, n in walk(read, h)]
    out = Path(a.out) if a.out else W / f'dlc-{idx}-list.tsv'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(''.join(f'{p}\t{n}\n' for p, n in rows))
    print(f'{len(rows)} files, {sum(n for _, n in rows) / 2**30:.3f} GiB -> {out}')
    dirs = {}
    for p, n in rows:
        d = p.rsplit('/', 1)[0] or '/'
        dirs.setdefault(d, [0, 0]); dirs[d][0] += 1; dirs[d][1] += n
    for d, (c, n) in sorted(dirs.items()):
        print(f'  {d}: {c} files {n / 2**20:.1f} MiB')
    push_hint(idx)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keys', default=KEYS)
    sub = ap.add_subparsers(dest='cmd', required=True)
    l = sub.add_parser('list'); l.add_argument('pkg'); l.add_argument('--out')
    e = sub.add_parser('extract'); e.add_argument('pkg')
    e.add_argument('--dest', default=str(W / 'romfs-dlc'), help='output root (default work/romfs-dlc); files go to <dest>/<index>/')
    e.add_argument('--only', help='extract only paths containing this substring')
    e.add_argument('--no-verify', action='store_true', help='skip the IVFC hash check (slow on multi-GB packs)')
    a = ap.parse_args()
    if a.cmd == 'list': cmd_list(a)
    else:
        idx = extract(a.pkg, a.dest, a.keys, a.only, not a.no_verify)
        push_hint(idx, a.dest)


if __name__ == '__main__':
    main()
