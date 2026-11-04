"""Summarize an xbg in-game capture (dm/capture/capture.txt): one line per event with shaders, outputs and inputs.
Usage: cap_chain.py <capture.txt> [last N events] [filter serial]"""
import re, sys
ev = []; cur = None
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if line.startswith('EVENT '):
        cur = {'n': int(re.search(r'event=(\d+)', line).group(1)), 'kind': re.search(r'kind=(\S+)', line).group(1), 'frame': re.search(r'frame=(\d+)', line).group(1), 'out': [], 'in': [], 'sh': ''}
        ev.append(cur); continue
    if cur is None: continue
    if line.startswith('DRAW '):
        m = re.findall(r'(vs|ps|gs|cs)=([0-9a-f]+)', line); cur['sh'] = ' '.join(f'{a}={b[:8]}' for a, b in m)
        c = re.search(r'count=(\d+)', line); cur['sh'] += f' n={c.group(1) if c else "?"}'
    elif line.startswith('IMAGE '):
        role = re.search(r'role=(\S+)', line).group(1); s = re.search(r'serial=(\d+)', line).group(1)
        f = re.search(r'format=(\S+)', line).group(1); e = re.search(r'extent=(\S+)', line).group(1)
        (cur['out'] if 'output' in role else cur['in']).append(f'{s}:{f}:{e}')
N = int(sys.argv[2]) if len(sys.argv) > 2 else 60
flt = sys.argv[3] if len(sys.argv) > 3 else None
sel = [e for e in ev if not flt or any(x.startswith(flt + ':') for x in e['out'] + e['in'])]
print(len(ev), 'events; frames', sorted(set(e['frame'] for e in ev))[:5])
for e in sel[-N:]:
    print(e['n'], e['kind'], e['sh'], 'OUT', ','.join(sorted(set(e['out']))), 'IN', ','.join(sorted(set(e['in'])))[:200])
