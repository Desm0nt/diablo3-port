"""Extract ExeFS (main, sdk, rtld, main.npdm) from the program NCA of a plain NSP / XCI (hash-verified PFS0).
For an NSZ / XCZ use tools/extract_exefs_nsz.py (tools/setup_d3.py picks the right one itself).
Usage: exefs_nsp.py <update package> <out dir> [--keys prod.keys]
Keys: --keys, else $NX_PROD_KEYS, else ~/.switch/prod.keys (your own console's; never part of this repository).
Diablo III: use the update package (01001B300B9BE800 v1441792); main build id 2607a74f5df7754c..., sdk 3ae34e689d7a35f1...
"""
import argparse, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from content_reader import Package

KEYS = os.environ.get('NX_PROD_KEYS', str(Path.home() / '.switch' / 'prod.keys'))


def extract(pkg, out, keys=KEYS):
    p = Package(pkg, keys)
    try:
        out = Path(out); out.mkdir(parents=True, exist_ok=True)
        for nca in p.ncas():
            if nca.type != 0: continue
            for sec in nca.sections:
                if sec.h[3] != 2: continue
                data, ents = sec.pfs()
                if 'main' not in [e[0] for e in ents]: continue
                for name, o, n in ents:
                    (out / name).write_bytes(data[o:o + n]); print(name, n)
                return
        sys.exit(f'{pkg}: no program NCA with an ExeFS (main) found')
    finally:
        p.close()


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('package'); ap.add_argument('out'); ap.add_argument('--keys', default=KEYS)
    a = ap.parse_args()
    extract(a.package, a.out, a.keys)
