#!/usr/bin/env python3
# [XC2] Xenoblade Chronicles 2 (Switch) offline shader bundles: tools/xc2_shader_jobs.py records -> dmsh_xlate_xc2 (Ryujinx, Vulkan) -> XGS1 <key>.xgs.
# Thin wrapper over dmsh_vk_doom.py (which itself runs dmsh_vk.py's own functions; see its header): imported as a module, its namespace NS is reused
# with these XC2 changes, each marked [XC2]:
#   - inputs: <jobs>/jobs.tsv (key, stage, ..., next, source), <jobs>/programs.tsv (every HCPS / SLCT program variant: vs, gs, ps, cs keys),
#     <jobs>/shader_facts.tsv (compute local size / shared bytes only: XC2 ships no GLSL), records <jobs>/rec/<key>.rec;
#   - key: the bundle is named by the XC2 engine key of tools/xc2_shader_jobs.py (sha256(control[:L] + code area)[:16], L = control +0x20 when
#     0x760 <= it < 0x880, else 0x880; nxnative shader_split.py skey), which is the jobs.tsv key; the XGS1 format (version 1) is unchanged;
#   - decode_tex: the whole NVN c2 handle layout (nxnative dm/gfx/nvn/dmsh_abi.h, the same in every glslc): combined textures 0x20 + 8 r (slot r),
#     images 0x120 + 8 r (8 slots), separate textures 0x168 + 8 r (slot r: the host's nvnCommandBufferBindSeparateTexture writes the same per-stage
#     texture table as BindTexture, hle_nvn.c), separate samplers 0x568 + 8 r (sampler slot r). DOOM's decoder knew only the combined table;
#   - stage linking from programs.tsv: vertex -> its geometry and pixel partners, geometry -> pixel (jobs.tsv "next", the DOOM translator's job
#     "next"), so every pixel input is declared and written by the stage before it (Vulkan interface matching; nxnative's vs_pad does the same for
#     Metal from shader_pairs.tsv). Checked after export by iface_check per program;
#   - StorageImageExtendedFormats added when a storage image has an extended format (as dmsh_vk_eternal.py);
#   - the GLSL slot check is skipped (no GLSL); keys may be given as "@file" (one key per line).
# Transform feedback: 5 XC2 vertex shaders declare it in their control section (+0x6dc strides, listed in jobs.tsv "xfb"); the DOOM translator does
# not ask Ryujinx for it, so they are translated without capture (noted per key in _vk_build.json).
# Usage: dmsh_vk_xc2.py <jobs dir> <output dir> [keys... | @file]
#   environment: XLATE (dmsh_xlate_xc2 executable, required), J (parallel translator processes, default 8), SPIRV_OPT / SPIRV_VAL, REC (default <jobs>/rec),
#                DMSH_VK_KEEP=1, META=<dir> (also write <key>.vk.json / .res.json / .spv there), XC2_LINK=0 (no stage linking), XC2_C1_GUARD=0 (no c1 padding)
import collections, concurrent.futures as cf, hashlib, json, os, pathlib, re, shutil, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pack_game_shaders
import dmsh_vk_doom as D   # its module level loads dmsh_vk.py's functions into D.NS (with the geometry storage-buffer base 0x410)

NS = D.NS
OPT, VAL = D.OPT, D.VAL

# ---- [XC2] the whole NVN c2 handle layout ----
C2_TEX, C2_IMG, C2_IMG_SLOTS, C2_SEPTEX, C2_SAMP, C2_END = 0x20, 0x120, 8, 0x168, 0x568, 0x968
TEX_TABLES = collections.Counter()   # which table each exported texture / image came from (summary)


def decode_tex_xc2(slots, h, image):
    """(texture or image slot, separate sampler slot or None, handle type) of a c2 handle, as dmsh_vk.decode_tex returns it; (None, None, ty) when
    the handle is not in the driver's c2 tables (a bindless handle from a uniform buffer: export() then fails with the handle in the message)"""
    tb = (slots & 0xffff) if slots >= 0 else 2
    sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if tb != 2: return None, None, ty
    words = [o0] + ([o1] if ty in (1, 2) else [])
    def slot(word, base, end): return (word * 4 - base) // 8 if base <= word * 4 < end and (word * 4 - base) % 8 == 0 else None
    if image:
        s = next((x for x in (slot(w, C2_IMG, C2_IMG + 8 * C2_IMG_SLOTS) for w in words) if x is not None), None)
        if s is not None: TEX_TABLES['image 0x120'] += 1
        return s, None, ty
    t = next((x for x in (slot(w, C2_TEX, C2_SEPTEX) for w in words) if x is not None), None)
    if t is not None: TEX_TABLES['combined 0x20'] += 1
    else:
        t = next((x for x in (slot(w, C2_SEPTEX, C2_SAMP) for w in words) if x is not None), None)
        if t is not None: TEX_TABLES['separate texture 0x168'] += 1
    s = None
    if ty in (1, 2) and sb == 2:
        s = next((x for x in (slot(w, C2_SAMP, C2_END) for w in words) if x is not None), None)
        if s is not None: TEX_TABLES['separate sampler 0x568'] += 1
    return t, s, ty
NS['decode_tex'] = decode_tex_xc2

# ---- [XC2] StorageImageExtendedFormats (dmsh_vk_eternal.py add_format_caps_eternal) ----
EXT_FMT = set(range(6, 21)) | set(range(25, 30)) | set(range(34, 40))
_add_format_caps = NS['add_format_caps']


def add_format_caps_xc2(spv):
    spv = _add_format_caps(spv)
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    caps = {a[0] for i, op, a in NS['insts'](w) if op == 17}
    if 49 not in caps and any(op == 25 and a[6] == 2 and a[7] in EXT_FMT for i, op, a in NS['insts'](w)):
        w[5:5] = [(2 << 16) | 17, 49]
    return struct.pack('<%dI' % len(w), *w)
NS['add_format_caps'] = add_format_caps_xc2


# ---- [XC2] geometry input topology ----
# The Maxwell geometry code does not record its input primitive (the draw does); Ryujinx needs it for the SPIR-V execution mode and the gl_in length,
# and Vulkan requires it to match the pipeline's primitive topology class. XC2's three geometry shaders (shd_post.wishp variants 100.0 / 100.1 / 112.0)
# read only input vertex 0 and emit a 4-vertex triangle strip: point sprites. The topology is derived per shader from a probe translation with
# Ryujinx's optimizer on, then spirv-opt -O (the vertex indices become constants): highest input vertex read 0 -> points, 1 -> lines, 2 -> triangles, 3 -> lines adjacency,
# 5 -> triangles adjacency; a non-constant index keeps triangles. XC2_GS_TOPOLOGY=<0..4> forces one value for every geometry shader.
TOPO_NAME = ['points', 'lines', 'lines_adjacency', 'triangles', 'triangles_adjacency']


def gs_input_vertex_max(spv):
    """highest constant vertex index of any access to a per-vertex Input array, or None when an index is not constant / nothing is read"""
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    consts, ptr, var, arr, m, dyn = {}, {}, {}, set(), -1, False
    for i, op, a in NS['insts'](w):
        if op == 43 and len(a) >= 3: consts[a[1]] = a[2]
        elif op == 28: arr.add(a[0])                                   # OpTypeArray
        elif op == 32: ptr[a[0]] = (a[1], a[2])                        # OpTypePointer: storage class, pointee
        elif op == 59 and a[2] == 1: var[a[1]] = a[0]                  # Input variables
    for i, op, a in NS['insts'](w):
        if op in (65, 66) and a[2] in var and ptr.get(var[a[2]], (0, 0))[1] in arr and len(a) > 3:
            if a[3] in consts: m = max(m, consts[a[3]])
            else: dyn = True
    return None if dyn or m < 0 else m


def gs_topologies(xlate, jobs, keys, work):
    """{gs key: (Ryujinx InputTopology, reason)} for the geometry jobs among keys"""
    gs = [k for k in keys if jobs[k]['stage'] == 'gs']
    if not gs: return {}
    force = os.environ.get('XC2_GS_TOPOLOGY')
    if force is not None: return {k: (int(force), 'XC2_GS_TOPOLOGY') for k in gs}
    pd = os.path.join(work, 'gs_probe'); os.makedirs(pd, exist_ok=True)
    jp = os.path.join(pd, 'jobs.json')
    json.dump([{**{x: y for x, y in jobs[k].items() if x != 'next'}, 'debug': False} for k in gs], open(jp, 'w'))
    subprocess.run([xlate, jp, pd], capture_output=True)
    out = {}
    for k in gs:
        p, po = os.path.join(pd, k + '.spv'), os.path.join(pd, k + '.o.spv')
        ok = os.path.exists(p) and subprocess.run([OPT, '-O', p, '-o', po], capture_output=True).returncode == 0
        m = gs_input_vertex_max(open(po, 'rb').read()) if ok else None
        t = {0: 0, 1: 1, 2: 3, 3: 2, 5: 4}.get(m, 3)
        out[k] = (t, f'geometry input topology {TOPO_NAME[t]} (highest input vertex read: {m if m is not None else "not constant / probe failed"})')
    return out


# ---- [XC2] interface check per program (pixel inputs vs the stage before) ----
def iface_check(progs, spvs):
    out = collections.Counter(); bad = []
    for label, p in progs:
        up = p.get('gs') or p.get('vs')
        if 'ps' not in p or up not in spvs or p['ps'] not in spvs: continue
        od, wr, _, _ = D.spv_io(spvs[up]); _, _, idl, rd = D.spv_io(spvs[p['ps']])
        miss = sorted(l for l in idl if l not in od)
        part = sorted((l, sorted(c for c in rd[l] - wr[l] if c is not None)) for l in idl if l in od and rd[l] - wr[l] - {None})
        out['programs'] += 1
        if miss: out['pixel input not declared upstream'] += 1; bad.append(f'{label}: {up} -> {p["ps"]} undeclared {miss}')
        if part: out['pixel input component not written upstream'] += 1; bad.append(f'{label}: {up} -> {p["ps"]} unwritten {part}')
    return out, bad


def chain_manifest(xlate, jobs_dir, opts):
    xdir = os.path.dirname(os.path.abspath(xlate)); src = pathlib.Path(HERE).parents[2]   # .../src
    m = {'xlate_source': subprocess.run([xlate, '--source-sha'], capture_output=True, text=True).stdout.strip()}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll', 'Spv.Generator.dll'):
        p = os.path.join(xdir, dll); m[dll] = D.sha16(open(p, 'rb').read()) if os.path.exists(p) else 'missing'
    ryu = src / 'dm' / 'third_party' / 'ryujinx' / 'src'
    h = hashlib.sha256()
    for f in sorted(ryu.rglob('*.cs')): h.update(f.relative_to(ryu).as_posix().encode() + b'\0' + f.read_bytes() + b'\0')
    m['ryujinx_src'] = h.hexdigest()[:16]
    m['spirv-opt'], m['spirv-val'] = D.tool_version(OPT), D.tool_version(VAL)
    for f in ('dmsh_vk_xc2.py', 'dmsh_vk_doom.py', 'dmsh_vk.py', 'pack_game_shaders.py'): m[f] = D.sha16(open(os.path.join(HERE, f), 'rb').read())
    jt = src.parent / 'tools' / 'xc2_shader_jobs.py'
    m['xc2_shader_jobs.py'] = D.sha16(jt.read_bytes()) if jt.exists() else 'missing'
    for f in ('jobs.tsv', 'shader_facts.tsv', 'programs.tsv'): m[f] = D.sha16(open(os.path.join(jobs_dir, f), 'rb').read())
    m['options'] = opts
    m['chain'] = hashlib.sha256(json.dumps(m, sort_keys=True).encode()).hexdigest()[:12]
    return m


def main():
    t0 = time.time()
    jobs_dir, out = sys.argv[1:3]
    want = set()
    for a in sys.argv[3:]:
        want |= {l.strip() for l in open(a[1:])} - {''} if a.startswith('@') else {a}
    xlate = os.environ.get('XLATE') or sys.exit('[red] XLATE=<dmsh_xlate executable> is required')
    rec_dir = os.environ.get('REC') or os.path.join(jobs_dir, 'rec')
    NS['FACTS'] = {k: json.loads(v) for k, v in (l.rstrip('\n').split('\t', 1) for l in open(os.path.join(jobs_dir, 'shader_facts.tsv'))
                                                 if l.strip() and not l.startswith('#'))}
    meta = {}
    for ln in open(os.path.join(jobs_dir, 'jobs.tsv'), encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if c[0] != 'key': meta[c[0]] = {'stage': c[1], 'xfb': c[6] == '1', 'next': [] if c[8] == '-' else c[8].split(','), 'source': c[9]}
    progs = []
    for ln in open(os.path.join(jobs_dir, 'programs.tsv'), encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if c[0] != 'label': progs.append((c[0], {s: k for s, k in zip(('vs', 'gs', 'ps', 'cs'), c[1:5]) if k != '-'}))
    NS['stage_of'] = {k: v['stage'] for k, v in meta.items()}
    unknown = want - set(meta)
    if unknown: sys.exit(f'[red] {len(unknown)} keys not in jobs.tsv, e.g. {sorted(unknown)[:3]}')
    keys = sorted(want) if want else sorted(meta)
    NJ = int(os.environ.get('J', '8'))
    os.makedirs(out, exist_ok=True)
    metadir = os.environ.get('META')
    if metadir: os.makedirs(metadir, exist_ok=True)
    work = tempfile.mkdtemp(prefix='dmsh_vk_xc2_', dir=os.environ.get('WORK'))
    bundle = os.path.join(work, 'bundle'); os.makedirs(bundle)
    link = os.environ.get('XC2_LINK', '1') != '0'; c1_guard = os.environ.get('XC2_C1_GUARD', '1') != '0'
    nxt = {k: v['next'] for k, v in meta.items()}
    jobs, c1s = {}, {}
    need = set(keys) | ({p for k in keys for p in nxt.get(k, ())} if link else set())
    for key in sorted(need):
        rec = open(os.path.join(rec_dir, key + '.rec'), 'rb').read()
        jobs[key], c1s[key] = NS['job_for'](key, meta[key]['source'], rec, work)
    topo = gs_topologies(xlate, jobs, keys, work)
    for key, (t, why) in topo.items(): jobs[key]['geometry_topology'] = t
    if link:
        for key in keys:
            if nxt.get(key): jobs[key]['next'] = [{'key': p, 'code': jobs[p]['code'], 'c1': jobs[p]['c1']} for p in nxt[key]]
    # several small batches per process slot: one slow batch does not hold the pool
    nb = max(NJ, min(len(keys), NJ * 8)); chunks = [keys[i::nb] for i in range(nb)]

    def xl(ch):
        jp = os.path.join(work, f'job_{ch[0]}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
        return subprocess.run([xlate, jp, work], capture_output=True)
    with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, [c for c in chunks if c]))
    t_xl = time.time() - t0
    xerr = {}
    for r in xr:   # dmsh_xlate prints "[RED] <key>: <Exception>: <message>"
        for ln in r.stderr.decode('utf-8', 'replace').splitlines():
            m = re.search(r'([0-9a-f]{16})\W{0,3}(\w+(?:Exception|Error)\W{0,3}.*)', ln)
            if m: xerr[m.group(1)] = re.sub(r'[^\x20-\x7e]+', ': ', m.group(2))[:200]
    use_opt = D.opt_has_trim()
    trim_stats = collections.Counter(); spvs = {}

    def one(key):
        if key in xerr or not os.path.exists(os.path.join(work, key + '.spv')): return key, 'translate failed', xerr.get(key, 'no spv'), None
        stage = jobs[key]['stage']
        spv = NS['rewrite'](open(os.path.join(work, key + '.spv'), 'rb').read(), stage)
        f1, f2 = os.path.join(work, key + '.r.spv'), os.path.join(work, key + '.t.spv')
        open(f1, 'wb').write(spv)
        if use_opt:
            r = subprocess.run([OPT, '--trim-capabilities', f1, '-o', f2], capture_output=True, text=True, errors='replace')
            if r.returncode: return key, 'spirv-opt failed', r.stderr.strip()[:200], None
        else:
            t, dropped = D.py_trim(spv); open(f2, 'wb').write(t)
            for c in dropped: trim_stats[c] += 1
        if D.INLINE_CS and stage == 'cs' and NS['helper_has_subgroup'](open(f2, 'rb').read()):
            f3 = os.path.join(work, key + '.i.spv')
            r = subprocess.run([OPT, '--merge-return', '--inline-entry-points-exhaustive', '--eliminate-dead-functions', f2, '-o', f3], capture_output=True, text=True, errors='replace')
            if r.returncode: return key, 'spirv-opt inline failed', r.stderr.strip()[:200], None
            os.replace(f3, f2)
        spv = NS['add_format_caps'](open(f2, 'rb').read()); open(f2, 'wb').write(spv)
        r = subprocess.run([VAL, '--target-env', 'vulkan1.1', f2], capture_output=True, text=True, errors='replace')
        if r.returncode: return key, 'spirv-val failed', (r.stderr.strip() or r.stdout.strip() or '?').splitlines()[0][:200], None
        res = json.load(open(os.path.join(work, key + '.res.json'), encoding='utf-8'))
        try: vk = NS['export'](key, stage, c1s[key], spv, res)
        except (ValueError, KeyError) as e: return key, 'export failed', re.sub(r'[^\x20-\x7e]+', ' ', str(e))[:200], None
        dyn = D.c1_dynamic(spv, vk)
        if dyn and c1_guard: vk['frozen_job']['c1'] = (c1s[key] + bytes(65536 - len(c1s[key]))).hex()
        open(os.path.join(bundle, key + '.spv'), 'wb').write(spv); spvs[key] = spv
        json.dump(vk, open(os.path.join(bundle, key + '.vk.json'), 'w', encoding='utf-8'), ensure_ascii=False)
        try: k2, data = pack_game_shaders.pack(pathlib.Path(os.path.join(bundle, key + '.vk.json')))
        except (ValueError, KeyError) as e: return key, 'pack failed', str(e)[:200], None
        if k2 != key: return key, 'pack failed', f'bundle key {k2}', None
        open(os.path.join(out, k2 + '.xgs'), 'wb').write(data)
        if metadir:
            for ext in ('.vk.json', '.spv'): shutil.copyfile(os.path.join(bundle, key + ext), os.path.join(metadir, key + ext))
            shutil.copyfile(os.path.join(work, key + '.res.json'), os.path.join(metadir, key + '.res.json'))
        kinds = collections.Counter((d['source'].get('nvn') or {}).get('kind', d['source']['category']) for d in vk['descriptors'])
        notes = res.get('notes', []) + (['transform feedback declared in the control section: translated without capture'] if meta[key]['xfb'] else []) + \
            ([topo[key][1]] if key in topo else [])
        info = {'caps': vk['capabilities'], 'notes': notes, 'linked': res.get('linked'), 'c1_dynamic': dyn, 'kinds': dict(kinds), 'bytes': len(data)}
        return key, 'ok', '', info

    with cf.ThreadPoolExecutor(NJ) as ex: res = list(ex.map(one, keys))
    if os.environ.get('DMSH_VK_KEEP'): print('work dir kept:', work)
    else: shutil.rmtree(work, ignore_errors=True)
    st = collections.Counter(r[1] for r in res)
    lines = [f'stages {len(res)}: ok {st["ok"]}' + ''.join(f', {k} {v}' for k, v in st.items() if k != 'ok') + f'; {time.time() - t0:.0f} s (translator {t_xl:.0f} s), J={NJ}',
             'capability trim: ' + ('spirv-opt --trim-capabilities' if use_opt else f'py_trim (spirv-opt lacks --trim-capabilities); dropped {dict(trim_stats)}')]
    for lvl in [k for k in st if k != 'ok']:
        for e, n in collections.Counter(re.sub(r'\b[0-9a-f]{16}\b', 'KEY', r[2]) for r in res if r[1] == lvl).most_common(12): lines.append(f'  [{lvl}] {n:5d}  {e}')
    for r in res:
        if r[1] != 'ok': lines.append(f'  FAILED {r[0]} ({meta[r[0]]["stage"]}, {meta[r[0]]["source"]}): {r[1]}: {r[2]}')
    by_stage = collections.Counter((meta[r[0]]['stage'], r[1]) for r in res)
    lines.append('by stage: ' + ', '.join(f'{s}/{k} {v}' for (s, k), v in sorted(by_stage.items())))
    ok = [r for r in res if r[1] == 'ok']
    lines.append('capabilities in bundles: ' + ', '.join(f'{c}:{n}' for c, n in sorted(collections.Counter(c for r in ok for c in r[3]['caps']).items())))
    lines.append('descriptor kinds: ' + ', '.join(f'{k} {n}' for k, n in sorted(sum((collections.Counter(r[3]['kinds']) for r in ok), collections.Counter()).items())))
    lines.append('c2 handle tables used (exported descriptors): ' + ', '.join(f'{k} {n}' for k, n in sorted(TEX_TABLES.items())))
    for label, tr in (('queries answered by default', False), ('translator messages', True)):
        nt = collections.Counter(re.sub(r'[0-9A-F]{3,}|\d+', 'N', x) for r in ok for x in r[3]['notes'] if x.startswith('translator: ') == tr)
        lines.append(f'notes, {label}: {sum(any(x.startswith("translator: ") == tr for x in r[3]["notes"]) for r in ok)} stages')
        for e, n in nt.most_common(12): lines.append(f'  {n:5d}  {e}')
    lines.append(f'stage linking: {"on" if link else "off"}; linked stages {sum(r[3]["linked"] is not None for r in ok)}')
    lines.append(f'c1 guard: {sum(r[3]["c1_dynamic"] > 0 for r in ok)} stages index c1 dynamically' + (' (c1 padded to 64 KiB)' if c1_guard else ' (not padded)'))
    ic, ib = iface_check(progs, spvs)
    lines.append('interface check (pixel inputs vs the stage before, per program): ' + ', '.join(f'{k} {v}' for k, v in ic.items()))
    for b in ib[:12]: lines.append('  ' + b)
    lines.append(f'bundle bytes: {sum(r[3]["bytes"] for r in ok):,}')
    man = chain_manifest(xlate, jobs_dir, {'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1',
                                           'link': link, 'c1_guard': c1_guard, 'inline_cs': D.INLINE_CS, 'trim': 'spirv-opt' if use_opt else 'py_trim'})
    json.dump(man, open(os.path.join(out, '_chain.json'), 'w'), indent=1, sort_keys=True)
    lines.append(f'chain {man["chain"]} (translator source {man["xlate_source"]}, ryujinx {man["ryujinx_src"]})')
    print('\n'.join(lines))
    json.dump([{'key': r[0], 'stage': meta[r[0]]['stage'], 'st': r[1], 'err': r[2], **(r[3] or {})} for r in res],
              open(os.path.join(out, '_vk_build.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    open(os.path.join(out, '_vk_build.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    sys.exit(0 if st['ok'] == len(res) else 1)


if __name__ == '__main__':
    main()
