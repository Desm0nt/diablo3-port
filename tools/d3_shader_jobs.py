"""Diablo III (Switch) shader jobs for the offline translator (dmsh_xlate via dm/gfx/tools/dmsh_vk_xc2.py). Notes: docs/d3/SHADERS.md

D3 counterpart of xc2_shader_jobs.py (the Xenoblade 2 port's shader job tool, not in this repository). D3 keeps every NVN shader in the Shaders\\*.shd SNO files of ClientCommon.cpk and the
patch CPKs (tools/d3_cpk.py mextract ... '*.shd' gives the merged base+patch set, one file per name). A .shd file (SNO header 0xDEADBEEF) holds
one or more passes; each pass stores its vertex stage and then its pixel stage as a raw GLSLC control section immediately followed by its code
area:
    control  magic 0x98761234, length = u32 @ +0x20 (0x850 for every D3 stage), code area length = u32 @ +0x704, c1 = [u32 @ +0x700, +0x704)
    code     at control + (u32 @ control+0x20): 0x30-byte header 0x12345678 (graphics) + 0x50-byte SPH + instructions + c1
The SNO body around them is serialized reflection data (HLSLcc names: constant buffers, hlslcc_mtx4x4..., in_/vs_ varyings), not needed here.
Program pairing: consecutive (vertex, pixel) records of one file = one pass (checked: every file alternates vs, ps).

Inputs:  <shd dir> (default work/cpk-shaders: Shaders/*.shd + _sources.tsv written by d3_cpk.py mextract)
Outputs (<out>, default work/shaders-jobs), the same files and columns as xc2_shader_jobs.py so dmsh_vk_xc2.py runs unchanged:
    rec/<key>.rec       bytes(0x38) + code area + control section (dmsh_vk.job_for layout)
    jobs.tsv            key, stage, code_bytes, ctrl_bytes, ctl_key_bytes, c1_bytes, xfb, programs, next, source
    programs.tsv        label (<shd path>#<pass>), vs, gs, ps, cs
    shader_map.tsv      key, stage, every (.shd path @ control offset, archive) that holds it
    shader_facts.tsv    per-key translator facts: {"depth_zero": true} for every vertex shader (the game uses NVN depth mode NEAR_IS_ZERO,
                        see main()); D3 has no compute shaders. D3_DEPTH_ZERO=0 leaves them out (XC2's near=-w remap)
    xfb.tsv             transform feedback layouts (control +0x6dc strides)
    report.txt
KEY (nxnative shader_split.py skey == host dm/gfx/vulkan/engine/xbg_shader_key.c xbg_shader_program_key; recomputed here twice per record:
from the .shd bytes and, independently, from the written .rec the way the host sees control + code pointers):
    n = u32 @ control+0x20; L = n if 0x760 <= n < 0x880 else 0x880; nc = u32 @ control+0x704
    key = sha256(control[0:L] + (code[0:nc] if nc and nc < 1<<24 else b''))[:8].hex()
Usage: d3_shader_jobs.py [shd dir] [out dir]
"""
import collections, hashlib, json, os, re, shutil, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CTRL, GFX, CSH = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}


def host_key(ctl, code):
    """xbg_shader_program_key: control pointer + code pointer exactly as the host gets them"""
    own, = struct.unpack_from('<I', ctl, 0x20); nc, = struct.unpack_from('<I', ctl, 0x704)
    h = hashlib.sha256(ctl[:own if 0x760 <= own < 0x880 else 0x880])
    if code is not None and nc and nc < (1 << 24): h.update(code[:nc])
    return h.hexdigest()[:16]


def xfb_layout(ctl):
    """transform feedback as the control section declares it (nx2android shader_vk.py xfb_layout, MIT, seedds; same as xc2_shader_jobs.py)"""
    out = []
    for b, stride in enumerate(struct.unpack_from('<4H', ctl, 0x6dc)):
        if not stride: continue
        n = ctl[0x6e8 + b]
        out.append((b, stride, [(o - 0x20, k) for o, k in zip(ctl[0xdc + 0x80 * b:0xdc + 0x80 * b + n], ctl[0x4dc + 0x80 * b:0x4dc + 0x80 * b + n])]))
    return out


def shd_records(d, where, bad):
    """[(control offset, stage, control bytes, code bytes)] of one .shd file"""
    out = []
    for m in re.finditer(re.escape(CTRL), d):
        o = m.start()
        if o + 0x760 > len(d): bad.append(f'{where}@{o:#x}: control magic too close to the end'); continue
        n, = struct.unpack_from('<I', d, o + 0x20); nc, = struct.unpack_from('<I', d, o + 0x704); c1o, c1t = struct.unpack_from('<2I', d, o + 0x700)
        c = o + n; code = d[c:c + nc]
        if not 0x760 <= n < 0x880: bad.append(f'{where}@{o:#x}: control length {n:#x} outside [0x760, 0x880)'); continue
        if len(code) != nc or code[:4] not in (GFX, CSH): bad.append(f'{where}@{o:#x}: no code area right after the control section ({code[:4].hex()})'); continue
        if not c1o <= c1t == nc: bad.append(f'{where}@{o:#x}: c1 {c1o:#x}..{c1t:#x} vs code {nc:#x}'); continue
        if code[:4] == CSH: st = 'cs'
        else: st = SPH_STAGE.get(struct.unpack_from('<I', code, 0x30)[0] >> 10 & 0xf)
        if st is None: bad.append(f'{where}@{o:#x}: SPH stage unknown'); continue
        if CTRL in code: bad.append(f'{where}@{o:#x}: control magic inside the code area'); continue
        out.append((o, st, d[o:o + n], code))
    return out


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'work' / 'cpk-shaders'
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'work' / 'shaders-jobs'
    if (out / 'rec').exists(): shutil.rmtree(out / 'rec')
    (out / 'rec').mkdir(parents=True)
    arch = {}
    if (src / '_sources.tsv').exists():
        for ln in open(src / '_sources.tsv', encoding='utf-8'):
            c = ln.rstrip('\n').split('\t')
            if c[0] != 'name': arch[c[0].replace('\\', '/').lower()] = c[2]
    files = sorted(p for p in src.rglob('*') if p.suffix.lower() == '.shd')
    meta, where, progs, bad, rep, xfbs = {}, collections.defaultdict(list), [], [], collections.Counter(), {}
    ctl_code = collections.defaultdict(set)
    for p in files:
        rel = p.relative_to(src).as_posix(); d = p.read_bytes()
        if d[:4] != bytes.fromhex('efbeadde'): bad.append(f'{rel}: no SNO header')
        recs = shd_records(d, rel, bad)
        rep[f'records per file {len(recs)}'] += 1
        seq = ''.join({'vs': 'v', 'ps': 'p'}.get(r[1], 'x') for r in recs)
        if not re.fullmatch(r'(vp)+', seq): bad.append(f'{rel}: stage order {seq} (expected vs, ps pairs)')
        for i, (o, st, ctl, code) in enumerate(recs):
            k = host_key(ctl, code)
            rec = bytes(0x38) + code + ctl
            # second, independent recomputation: the host's view of the written record (control found by magic, code before it)
            ci = rec.find(CTRL)
            if ci != 0x38 + len(code) or host_key(rec[ci:], rec[0x38:ci]) != k: bad.append(f'{rel}@{o:#x}: key recomputation from the record differs')
            ctl_code[hashlib.sha256(ctl).hexdigest()].add(hashlib.sha256(code).hexdigest())
            where[k].append(f'{rel}@{o:#x}' + (f' ({arch[rel.lower()]})' if rel.lower() in arch else ''))
            if k in meta:
                if (out / 'rec' / f'{k}.rec').read_bytes() != rec: bad.append(f'{k}: same key, different bytes ({rel}@{o:#x})')
                continue
            (out / 'rec' / f'{k}.rec').write_bytes(rec)
            c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
            xfb = st == 'vs' and any(struct.unpack_from('<4H', ctl, 0x6dc))
            if xfb: xfbs[k] = xfb_layout(ctl)
            own, = struct.unpack_from('<I', ctl, 0x20)
            meta[k] = {'stage': st, 'code': len(code), 'ctrl': len(ctl), 'kl': own if 0x760 <= own < 0x880 else 0x880, 'c1': c1t - c1o, 'src': rel,
                       'xfb': xfb, 'progs': 0, 'next': set()}
            rep[f'{st} c1 bytes {c1t - c1o}'] += 1
        for j in range(0, len(recs) - 1, 2):
            if recs[j][1] == 'vs' and recs[j + 1][1] == 'ps':
                progs.append((f'{rel}#{j // 2}', host_key(recs[j][2], recs[j][3]), '-', host_key(recs[j + 1][2], recs[j + 1][3]), '-'))
    for label, vs, gs, ps, cs in progs:
        for k in (vs, ps): meta[k]['progs'] += 1
        meta[vs]['next'].add(ps)
    shared = sum(len(v) > 1 for v in ctl_code.values())
    if shared: bad.append(f'{shared} control sections appear with more than one code area')   # informational: the key covers the code too
    with open(out / 'programs.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('label\tvs\tgs\tps\tcs\n' + ''.join('\t'.join(r) + '\n' for r in progs))
    with open(out / 'jobs.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('key\tstage\tcode_bytes\tctrl_bytes\tctl_key_bytes\tc1_bytes\txfb\tprograms\tnext\tsource\n')
        for k in sorted(meta):
            v = meta[k]
            fp.write(f"{k}\t{v['stage']}\t{v['code']}\t{v['ctrl']}\t{v['kl']}\t{v['c1']}\t{int(v['xfb'])}\t{v['progs']}\t{','.join(sorted(v['next'])) or '-'}\t{v['src']}\n")
    with open(out / 'shader_map.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('key\tstage\tlocations (.shd path @ control offset (archive))\n')
        for k in sorted(meta): fp.write(f"{k}\t{meta[k]['stage']}\t{' | '.join(where[k])}\n")
    # Depth mode: right after nvnDeviceInitialize the game calls nvnDeviceSetWindowOriginMode(dev, 1 = UPPER_LEFT) and nvnDeviceSetDepthMode(dev,
    # 1 = NEAR_IS_ZERO) (main+0xe7738..0xe7764, through the proc pointers loaded at main+0xe27a4 / +0xe27c0), unconditionally: clip z is already
    # [0, w] as in Vulkan, so the translator must NOT add XC2's near=-w remap (z' = z/2 + w/2). dmsh_vk.job_for merges these facts into each job.
    dz = os.environ.get('D3_DEPTH_ZERO', '1') != '0'
    with open(out / 'shader_facts.tsv', 'w', newline='\n') as fp:
        fp.write('# D3 translation-query facts (tools/d3_shader_jobs.py): key <TAB> facts JSON merged into the translator job. No compute shaders;\n'
                 '# depth_zero: the game runs NVN_DEPTH_MODE_NEAR_IS_ZERO (main+0xe7764), so vertex shaders keep z unchanged\n')
        if dz:
            for k in sorted(meta):
                if meta[k]['stage'] in ('vs', 'gs'): fp.write(k + '\t' + json.dumps({'depth_zero': True}, separators=(',', ':')) + '\n')
    with open(out / 'xfb.tsv', 'w', newline='\n') as fp:
        fp.write('# vertex shaders whose control section declares transform feedback: key, buffer, stride, location:components...\n')
        for k in sorted(xfbs):
            for b, stride, o in xfbs[k]: fp.write(f'{k}\t{b}\t{stride}\t{" ".join(f"{l}:{c}" for l, c in o)}\n')
    sc = collections.Counter(v['stage'] for v in meta.values())
    occ = sum(len(v) for v in where.values())
    lines = [f'source: {src} ({len(files)} .shd files)',
             f'stage records: {occ}; unique keys: {len(meta)} ({", ".join(f"{s} {n}" for s, n in sorted(sc.items()))}); problems: {len(bad)}',
             f'programs (vs+ps passes): {len(progs)}, distinct (vs, ps) pairs {len({(p[1], p[3]) for p in progs})}; keys in no program: {sum(not v["progs"] for v in meta.values())}',
             f'vertex shaders with pixel partners: {sum(1 for v in meta.values() if v["stage"] == "vs" and v["next"])}/{sc["vs"]}',
             'control lengths (+0x20): ' + ', '.join(f'{n:#x} {c}' for n, c in collections.Counter(v['ctrl'] for v in meta.values()).most_common()),
             f'distinct control sections: {len(ctl_code)}; shared by different code areas: {shared}',
             f'transform feedback: {" ".join(sorted(xfbs)) or "none"}',
             f'depth_zero facts (NVN depth mode NEAR_IS_ZERO): ' + (f'on, {sc["vs"] + sc["gs"]} vertex-pipeline stages' if dz else 'OFF (D3_DEPTH_ZERO=0)'),
             'key check: computed from the .shd bytes and recomputed from every written record (host xbg_shader_program_key logic): '
             + ('all equal' if not any('recomputation' in b for b in bad) else 'MISMATCHES')] + \
            [f'  {n:5d}  {k}' for k, n in sorted(rep.items())] + [f'  PROBLEM {b}' for b in bad]
    (out / 'report.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
