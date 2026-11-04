"""Symbolize 'bt:' lines (sdk+0x.., main+0x..) from a dmhost log using MOD0 symbols (sdk exports; main: its exports).
Usage: symbt.py <log> [exefs dir]   (default work/exefs-update, with sdk.symbols.json / main.symbols.json from tools/nso_symbols.py)"""
import bisect, json, re, sys
from pathlib import Path
W = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1] / 'work' / 'exefs-update'
tabs = {}
for mod in ('sdk', 'main'):
    ex = sorted((x['value'], x['name']) for x in json.load(open(W / f'{mod}.symbols.json'))['exports'] if x['type'] == 2 and x['value'])
    tabs[mod] = ([v for v, _ in ex], [n for _, n in ex])
def sym(mod, off):
    vs, ns = tabs[mod]
    i = bisect.bisect_right(vs, off) - 1
    return f'{ns[i]}+{off - vs[i]:#x}' if i >= 0 and mod == 'sdk' else f'{mod}+{off:#x}'
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if 'bt:' in line or 'Break' in line or 'UNIMPL' in line:
        print(line.rstrip())
        for m, o in re.findall(r'(sdk|main)\+0x([0-9a-f]+)', line):
            print('    ', m, '+0x' + o, sym(m, int(o, 16)))
