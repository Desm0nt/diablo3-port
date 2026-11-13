#!/usr/bin/env python3
"""Diablo III Mobile: ASTC texture cache for phones without BC textures (Mali, PowerVR, Xclipse). Run by setup.

  d3_astc_cache.py ROMFS OUT [--aoc DIR]... [--jobs N] [--quality medium] [--formats bc1,bc2,bc3] [--force] [--check] [--limit N]
                   [--only GLOB]

The game's textures (Textures/*.tex in the CPK archives, docs/d3/CPK.md) are NVN packaged textures: an SNO header, then an 'NvFD'
blob whose 'NvBH' blocks hold the texture description (format, size, levels, block height) and the storage exactly as the game
puts it into its texture memory (block-linear, BC1 / BC2 / BC3). GPUs without BC sampled those only after decoding them to RGBA8
(4-8x the memory: 1.6 GB after 45 minutes on a Mali phone, issue #20). This tool encodes every BC texture once, on the PC, to ASTC:

Every texture must keep mip 0 at >= 42 dB PSNR (RGBA) against its decoded BC (owner, 13.11.2026). Per texture the steps below are
tried in order and the first one that reaches 42 dB is kept; a texture that stays below 42 dB even at 4x4 -thorough is left out
(the engine decodes its BC as before), listed in quality.tsv as "bc":
  BC1      (8 bytes / 4x4, BC1 RGBA) -> 6x6 -fast, 5x5 -fast, 4x4 -fast, 4x4 -medium, 4x4 -thorough (alpha encoded as well)
  BC2, BC3 (16 bytes / 4x4)          -> 4x4 -fast, 4x4 -medium, 4x4 -thorough (DOOM 2016's block size)

D3's BC1 is 3.6 GB of the 5.7 GB of BC storage, so an ASTC block of BC1's own size is what keeps the cache (and the phone's
memory) near the BC size; the quality rule keeps every texture visually where its BC was. Unlike DOOM's overlay (same block size,
the game loads the substitute blocks itself) the data is not given to the game: D3's BC1 blocks are 8 bytes and no ASTC block is.
The engine instead recognises a texture by its storage (dm/gfx/vulkan/engine/xbg_astc_cache.h: CRC-32 + Adler-32 of the bytes,
their length, NVN format, size and levels) when it creates the Vulkan image, creates it in the cache's ASTC format and uploads the
cached blocks; a texture the cache does not have (or whose memory the game changed) takes the BC decoder as before.

Output in OUT (by setup: <work>/Diablo3Mobile/astc, i.e. Download/Diablo3Mobile/astc on the phone):
  d3-astc.pack      'D3AC' v1: 64-byte header, the ASTC blocks of every texture (level 0 first, each level tight rows of
                    blocks), then the index (48 bytes an entry, sorted by key)
  manifest.json     written last: version, encoder and settings, counts, sizes, source identity
  quality.tsv       per texture: name, NVN format, size, levels, ASTC block (or bc), preset, PSNR of mip 0 against the decoded BC
All-zero ASTC blocks (the error block on Mali, KB TEX-04) never reach the file: such a block becomes the void-extent block of
transparent black (counted in the manifest; the engine checks again when it uploads).

The RomFS is only read. The output is deterministic for the same input and encoder (work is spread over --jobs worker processes,
results are written in a fixed order). An interrupted run resumes from OUT/.journal; a finished one is skipped while manifest.json
matches this script (its SHA-256), the encoder versions, the settings and the source archives (names and sizes). --force rebuilds,
--check only reports whether OUT is up to date (exit 0) or not (exit 1). --limit / --only are for tests (a partial cache).
Dependencies (pinned in tools/requirements-astc.txt, installed by setup into its private Python): astc-encoder-py (MIT; ARM
astc-encoder, Apache-2.0), numpy (BSD-3-Clause). The BC decoders are this file's (the engine's CPU decoder in numpy).
"""
import argparse, fnmatch, hashlib, json, os, struct, sys, time, zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

CACHE_FORMAT, CACHE_VERSION = 'd3-astc-cache', 1
PACK, MAGIC = 'd3-astc.pack', b'D3AC'
HEADER = struct.Struct('<4sIIIQQQ24x')         # magic, version, entries, flags, index offset, data offset, data bytes (64)
ENTRY = struct.Struct('<IIIHHHHBBHQII8x')      # crc32, adler32, storage bytes, NVN format, levels, width, height, block w, block h,
                                               # flags, data offset, data bytes, PSNR x100 of mip 0 (48)
assert HEADER.size == 64 and ENTRY.size == 48
BC = {0x41: ('bc1', 8, False), 0x42: ('bc1', 8, False), 0x43: ('bc2', 16, False), 0x44: ('bc3', 16, False),
      0x45: ('bc1', 8, True), 0x46: ('bc1', 8, True), 0x47: ('bc2', 16, True), 0x48: ('bc3', 16, True)}   # NVN format -> (kind, block bytes, sRGB)
QUALITY = {'fastest': 0, 'fast': 10, 'medium': 60, 'thorough': 98}
TARGET_DB = 42.0                               # mip 0 at or above this: keep (DOOM's retry threshold)
STEPS = {'bc1': ((6, 'fast'), (5, 'fast'), (4, 'fast'), (4, 'medium'), (4, 'thorough')),   # block, preset: tried in order
         'bc2': ((4, 'fast'), (4, 'medium'), (4, 'thorough')), 'bc3': ((4, 'fast'), (4, 'medium'), (4, 'thorough'))}
PRESET_CODE = {'fast': 0, 'medium': 1, 'thorough': 2}   # in the index entry's flags
VOID_BLACK = bytes([0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]) + bytes(8)   # ASTC LDR void extent, transparent black
PACKAGES = ('astc-encoder-py', 'numpy')


class Fail(Exception):
    pass


# ---------------------------------------------------------------- .tex (SNO + NVN packaged texture)
def parse_tex(d):
    """A Textures/*.tex member -> dict(fmt, w, h, levels, th, offsets, storage) or None when it is not a 2D NVN packaged texture.
    SNO header 0x10 bytes; at 0x40 the (offset, size) of the 'NvFD' blob, relative to 0x10. In the blob: 'NvFD' (16 bytes), then
    'NvBH' blocks (36-byte header: magic, 0x24, payload bytes, 0, payload offset from the block, 0, type, index, 0): type 2 = the
    texture info (storage bytes, 0, alignment, width, height, depth, layers, NVN format, levels, storage bytes, 0, 16 level
    offsets, block height log2, flags), type 3 = the storage, type 5 = a trailer."""
    if len(d) < 0x48 or d[:4] != b'\xef\xbe\xad\xde': return None
    off, size = struct.unpack_from('<2I', d, 0x40)
    b = d[0x10 + off:0x10 + off + size]
    if len(b) < 0x34 or b[:4] != b'DFvN': return None
    blocks, o = {}, 0x10
    while o + 36 <= len(b) and b[o:o + 4] == b'HBvN':
        _, n, _, rel, _, typ, _, _ = struct.unpack_from('<8I', b, o + 4)
        blocks.setdefault(typ, (o + rel, n)); o += rel + n
    if 2 not in blocks or 3 not in blocks: return None
    io, _ = blocks[2]
    ssz, _, _, w, h, depth, layers, fmt, levels, _, _ = struct.unpack_from('<11I', b, io)
    offsets = struct.unpack_from('<16I', b, io + 40)
    th = struct.unpack_from('<I', b, io + 27 * 4)[0]
    so, sn = blocks[3]
    st = b[so:so + sn]
    if sn != ssz or len(st) != sn or depth != 1 or layers != 1 or not 1 <= levels <= 16 or not w or not h: return None
    return dict(fmt=fmt, w=w, h=h, levels=levels, th=th, offsets=offsets[:levels], storage=st)


def key_of(t):
    s = t['storage']
    return (zlib.crc32(s) & 0xffffffff, zlib.adler32(s) & 0xffffffff, len(s), t['fmt'], t['w'], t['h'], t['levels'])


# ---------------------------------------------------------------- block-linear (Tegra X1; dm/gfx/tex/nvn_deswizzle.c, nvn_tex.c)
def _np():
    import numpy
    return numpy


def adj_tile(shift, unit, dim):
    if not shift: return 0
    x = unit << (shift - 1)
    if x >= dim:
        while True:
            shift -= 1
            if not shift: break
            x >>= 1
            if x < dim: break
    return shift


def deswizzle_level(t, level, bpb):
    """Level `level` of the storage -> (blocks_x, blocks_y, tight linear block bytes)."""
    np = _np()
    w, h = max(1, t['w'] >> level), max(1, t['h'] >> level)
    bx, by = (w + 3) // 4, (h + 3) // 4
    row_bytes = bx * bpb
    th = adj_tile(t['th'], 8, by)
    bh = 1 << th
    gobs_x = (row_bytes + 63) // 64
    block = 512 * bh
    x = (np.arange(bx, dtype=np.int64) * bpb)[None, :]
    y = np.arange(by, dtype=np.int64)[:, None]
    gob = (x % 64 // 32) * 256 + (y % 8 // 2) * 64 + (x % 32 // 16) * 32 + (y % 2) * 16 + x % 16
    off = (y // (8 * bh)) * gobs_x * block + (x // 64) * block + ((y % (8 * bh)) // 8) * 512 + gob + t['offsets'][level]
    idx = off[..., None] + np.arange(bpb, dtype=np.int64)
    src = np.frombuffer(t['storage'], np.uint8)
    if idx.max() >= len(src): raise Fail(f'level {level} reaches past the storage')
    return bx, by, src[idx.reshape(-1)].tobytes()


# ---------------------------------------------------------------- BC decode (the engine's CPU decoder semantics, xbg_texture.c)
def _bc_color(b, four, opaque):
    """(n, 8) colour halves -> (n, 16, 4) uint32 RGBA (xbg_texture.c color())."""
    np = _np()
    c0 = b[:, 0].astype(np.uint32) | b[:, 1].astype(np.uint32) << 8
    c1 = b[:, 2].astype(np.uint32) | b[:, 3].astype(np.uint32) << 8
    def rgb(c):
        r, g, bl = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2)], 1)
    v0, v1 = rgb(c0), rgb(c1)
    f4 = (four | (c0 > c1))[:, None]
    pal = np.zeros((len(b), 4, 4), np.uint32)
    pal[:, 0, :3], pal[:, 1, :3] = v0, v1
    pal[:, 2, :3] = np.where(f4, (2 * v0 + v1) // 3, (v0 + v1) // 2)
    pal[:, 3, :3] = np.where(f4, (v0 + 2 * v1) // 3, 0)
    pal[:, :3, 3] = 255
    pal[:, 3, 3] = np.where(f4[:, 0] | opaque, 255, 0)
    bits = np.ascontiguousarray(b[:, 4:8]).view('<u4').ravel()
    idx = ((bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3).astype(np.int64)
    return np.take_along_axis(pal, idx[..., None].repeat(4, 2), 1)


def _bc3_alpha(b):
    """(n, 8) BC3 alpha halves -> (n, 16) uint32 (xbg_texture.c channel(), 16-bit interpolants rounded to 8 bits)."""
    np = _np()
    a0, a1 = b[:, 0].astype(np.int64) * 257, b[:, 1].astype(np.int64) * 257
    k = np.arange(8, dtype=np.int64)[None, :]
    seven = ((8 - k) * a0[:, None] + (k - 1) * a1[:, None] + 3) // 7
    five = np.where(k < 6, ((6 - k) * a0[:, None] + (k - 1) * a1[:, None] + 2) // 5, np.where(k == 6, 0, 65535))
    pal = np.where((b[:, 0] > b[:, 1])[:, None], seven, five)
    pal[:, 0], pal[:, 1] = a0, a1
    bits = np.zeros(len(b), np.uint64)
    for i in range(6): bits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = ((bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
    return ((np.take_along_axis(pal, idx, 1) + 128) // 257).astype(np.uint32)


def decode_bc(kind, fmt, data, bx, by):
    """Linear BC blocks -> (by*4, bx*4, 4) RGBA8 exactly as the engine's CPU decoder (sRGB stays encoded)."""
    np = _np()
    if kind == 'bc1':
        px = _bc_color(np.frombuffer(data, np.uint8).reshape(-1, 8), False, fmt in (0x41, 0x45))
    else:
        b = np.frombuffer(data, np.uint8).reshape(-1, 16)
        px = _bc_color(b[:, 8:], True, True)
        if kind == 'bc2':
            a = np.ascontiguousarray(b[:, :8]).view('<u8').ravel()
            px[:, :, 3] = ((a[:, None] >> (np.uint64(4) * np.arange(16, dtype=np.uint64))) & np.uint64(15)).astype(np.uint32) * 17
        else:
            px[:, :, 3] = _bc3_alpha(b[:, :8])
    return px.astype(np.uint8).reshape(by, bx, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(by * 4, bx * 4, 4)


# ---------------------------------------------------------------- ASTC (worker processes)
_ctx = {}


def _astc(srgb, bw, bh, quality):
    k = (srgb, bw, bh, quality)
    if k not in _ctx:
        import astc_encoder as ae
        prof = ae.ASTCProfile.LDR_SRGB if srgb else ae.ASTCProfile.LDR
        _ctx[k] = (ae, ae.ASTCContext(ae.ASTCConfig(prof, bw, bh, 1, QUALITY[quality]), 1), ae.ASTCSwizzle())
    return _ctx[k]


def encode(img, w, h, srgb, bw, bh, quality):
    """The w x h top-left of img -> ASTC bw x bh blocks (zero blocks made void-extent black), zero-block count."""
    np = _np()
    ae, ctx, sw = _astc(srgb, bw, bh, quality)
    im = ae.ASTCImage(ae.ASTCType.U8, w, h, 1, np.ascontiguousarray(img[:h, :w]).tobytes())
    out = bytearray(ctx.compress(im, sw))
    n = ((w + bw - 1) // bw) * ((h + bh - 1) // bh)
    if len(out) != n * 16: raise Fail(f'astcenc gave {len(out)} bytes for {n} blocks')
    a = np.frombuffer(bytes(out), np.uint8).reshape(n, 16)
    zero = np.nonzero(~a.any(1))[0]
    for i in zero: out[i * 16:i * 16 + 16] = VOID_BLACK
    return bytes(out), len(zero)


def decode_astc(blocks, w, h, srgb, bw, bh, quality):
    np = _np()
    ae, ctx, sw = _astc(srgb, bw, bh, quality)
    im = ctx.decompress(blocks, ae.ASTCImage(ae.ASTCType.U8, w, h, 1), sw)
    return np.frombuffer(im.data, np.uint8).reshape(h, w, 4)


def psnr(a, b):
    np = _np()
    mse = float(np.mean((a.astype(np.float64) - b) ** 2))
    return 99.0 if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))


def convert(t):
    """One texture -> (block w, block h, [level bytes], PSNR of mip 0, zero blocks fixed, preset, steps tried); block 0 and no levels
    when no step reaches TARGET_DB (the texture stays BC; the PSNR is the best one seen)."""
    kind, bpb, srgb = BC[t['fmt']]
    imgs = []
    for lv in range(t['levels']):
        bx, by, data = deswizzle_level(t, lv, bpb)
        imgs.append((max(1, t['w'] >> lv), max(1, t['h'] >> lv), decode_bc(kind, t['fmt'], data, bx, by)))
    w0, h0, i0 = imgs[0]
    best, tried = -1.0, 0
    for b, q in STEPS[kind]:
        tried += 1
        enc, _ = encode(i0, w0, h0, srgb, b, b, q)
        db = psnr(i0[:h0, :w0], decode_astc(enc, w0, h0, srgb, b, b, q))
        best = max(best, db)
        if db >= TARGET_DB: break
    else:
        return 0, 0, [], best, 0, 'bc', tried
    levels, zeros = [], 0
    for w, h, img in imgs:
        enc, z = encode(img, w, h, srgb, b, b, q); levels.append(enc); zeros += z
    return b, b, levels, db, zeros, q, tried


_arc = {}


def job(args):
    path, name = args
    from d3cpklib import CPKArchive
    a = _arc.get(path)
    if a is None: a = _arc[path] = CPKArchive.open(path)
    t = parse_tex(a.read(a.get(name)))
    if t is None: raise Fail(f'{name}: not a texture any more')
    k = key_of(t)
    bw, bh, levels, db, zeros, q, tried = convert(t)
    return k, bw, bh, levels, db, zeros, q, tried


# ---------------------------------------------------------------- planning
def archives(roots, aocs):
    import d3_cpk
    out = []
    for r in roots: out += [(lab, p) for lab, p in d3_cpk.archives(r)]
    for d in aocs:
        for f in sorted(os.listdir(d)):
            if f.lower().endswith('.cpk'): out.append((f'aoc/{os.path.basename(os.path.normpath(d))}/{f[:-4]}', os.path.join(d, f)))
    return out


def plan(romfs, aocs, formats, only=None, progress=None):
    """Unique BC textures the game can load: the merged view of each RomFS (base CPKs, then Update/CPKs in the game's order, a later
    archive replacing a name: tools/d3_cpk.py merged) plus every texture of the language packs. A texture repeated in several
    archives is one job, keyed by its storage: [(key, archive path, member name)] sorted by key, and counts of what was left out."""
    import d3_cpk
    from d3cpklib import CPKArchive
    seen, skipped = {}, {}
    def take(p, e, a):
        if only and not fnmatch.fnmatch(e.name.replace(chr(92), '/').lower(), only.lower()): return
        t = parse_tex(a.read(e))
        why = ('not a 2D packaged texture' if t is None else f'format {t["fmt"]:#x}' if t['fmt'] not in BC else
               f'{BC[t["fmt"]][0]} not selected' if BC[t['fmt']][0] not in formats else None)
        if why: skipped[why] = skipped.get(why, 0) + 1; return
        k = key_of(t)
        if k not in seen: seen[k] = (p, e.name)
    for r in romfs:
        view, _ = d3_cpk.merged(r, '*.tex')
        for i, (n, lab, a, e) in enumerate(view.values()):
            if e.loc_count: take(a.path, e, a)
            if progress and i % 5000 == 4999: progress(f'{r}: {i + 1} of {len(view)} textures read, {len(seen)} unique BC')
    for d in aocs:
        for f in sorted(os.listdir(d)):
            if not f.lower().endswith('.cpk'): continue
            a = CPKArchive.open(os.path.join(d, f))
            for e in a:
                if e.loc_count and e.name.lower().endswith('.tex'): take(a.path, e, a)
        if progress: progress(f'{d}: {len(seen)} unique BC textures so far')
    return sorted((k, p, n) for k, (p, n) in seen.items()), skipped


# ---------------------------------------------------------------- driver
def sha256_file(p, chunk=1 << 22):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(chunk), b''): h.update(b)
    return h.hexdigest()


def encoder_versions():
    from importlib import metadata
    v = {}
    for p in PACKAGES:
        try: v[p] = metadata.version(p)
        except Exception: v[p] = None
    return v


def missing_packages():
    import importlib.util as iu
    return [p for p, m in zip(PACKAGES, ('astc_encoder', 'numpy')) if iu.find_spec(m) is None]


def identity(arcs, a):
    return dict(format=CACHE_FORMAT, version=CACHE_VERSION, tool_sha256=sha256_file(__file__), encoders=encoder_versions(),
                settings=dict(formats=sorted(a.formats), target_db=TARGET_DB, steps={k: [f'{b}x{b} {q}' for b, q in v] for k, v in STEPS.items()}, only=a.only, limit=a.limit),
                sources=[[lab, os.path.getsize(p)] for lab, p in arcs])


def up_to_date(out, ident):
    m = Path(out) / 'manifest.json'
    if not m.is_file() or not (Path(out) / PACK).is_file(): return False
    try: old = json.loads(m.read_text(encoding='utf-8'))
    except Exception: return False
    return all(old.get(k) == v for k, v in ident.items()) and old.get('complete') is True and \
        os.path.getsize(Path(out) / PACK) == old.get('pack_bytes')


def build(a, log=lambda s: print(s, flush=True)):
    arcs = archives(a.romfs, a.aoc)
    if not arcs: raise Fail(f'no CPK archives under {a.romfs}')
    miss = missing_packages()
    if miss: raise Fail('missing Python packages: ' + ', '.join(miss) + ' (pip install -r tools/requirements-astc.txt)')
    ident = identity(arcs, a)
    out = Path(a.out)
    if up_to_date(out, ident) and not a.force:
        log(f'ASTC cache in {out} is up to date'); return 0
    if a.check: log(f'ASTC cache in {out} is missing or out of date'); return 1
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    jobs, skipped = plan(a.romfs, a.aoc, a.formats, a.only, lambda s: log('  ' + s))
    if a.limit: jobs = jobs[:a.limit]
    log(f'{len(jobs)} BC textures to encode (left as they are: {skipped or "none"}), plan {time.time() - t0:.0f} s')
    part, jpath = out / (PACK + '.part'), out / '.journal'
    jsig = hashlib.sha256(json.dumps(ident, sort_keys=True).encode()).hexdigest()
    done, end = {}, HEADER.size
    if not a.force and part.is_file() and jpath.is_file():   # resume: entries journaled before the interruption
        lines = jpath.read_text(encoding='utf-8').splitlines()
        if lines and lines[0] == jsig:
            for ln in lines[1:]:
                f = ln.split('\t')
                if len(f) == 13:
                    k = tuple(int(x) for x in f[:7]); done[k] = tuple(int(x) for x in f[7:12]) + (f[12],); end = max(end, done[k][3] + done[k][4])
    if not done:
        with open(part, 'wb') as f: f.write(bytes(HEADER.size))
        jpath.write_text(jsig + '\n', encoding='utf-8')
    else:
        with open(part, 'r+b') as f: f.truncate(end)
        log(f'resuming: {len(done)} textures already encoded')
    todo = [j for j in jobs if j[0] not in done]
    n_ok, zeros, stats = 0, 0, {}
    with open(part, 'ab') as f, open(jpath, 'a', encoding='utf-8') as jf, ProcessPoolExecutor(max_workers=a.jobs) as ex:
        t1 = time.time()
        for i, (k, bw, bh, levels, db, z, q, tried) in enumerate(ex.map(job, [(p, n) for _, p, n in todo], chunksize=4)):
            data = b''.join(levels); at = f.tell(); f.write(data)
            done[k] = (bw, bh, int(round(db * 100)), at if bw else 0, len(data), f'{q}/{tried}')
            jf.write('\t'.join(str(x) for x in k + done[k][:5]) + f'\t{q}/{tried}\n')
            zeros += z; n_ok += 1
            if (i + 1) % 200 == 0 or i + 1 == len(todo):
                f.flush(); jf.flush()
                el = time.time() - t1
                log(f'  {i + 1} / {len(todo)} textures, {f.tell() / 2**30:.2f} GB, {el:.0f} s, about {el / (i + 1) * (len(todo) - i - 1) / 60:.0f} min left')
    names = {k: n for k, _, n in jobs}
    stayed = sorted(k for k in done if k in names and not done[k][0])   # below 42 dB at 4x4 -thorough: left to the BC decoder
    keys = sorted(k for k in done if k in names and done[k][0])
    with open(part, 'r+b') as f:
        f.seek(0, 2); index_off = f.tell(); data_bytes = index_off - HEADER.size
        for k in keys:
            crc, adl, size, fmt, w, h, lv = k; bw, bh, db, at, n, q = done[k]
            f.write(ENTRY.pack(crc, adl, size, fmt, lv, w, h, bw, bh, PRESET_CODE[q.split('/')[0]], at, n, db))
        f.seek(0); f.write(HEADER.pack(MAGIC, CACHE_VERSION, len(keys), 0, index_off, HEADER.size, data_bytes))
    by_block = {}
    with open(out / 'quality.tsv', 'w', encoding='utf-8', newline='\n') as q:
        q.write('name\tformat\twidth\theight\tlevels\tblock\tpreset\tsteps\tpsnr_db\n')
        retried = 0
        for k in sorted(keys + stayed):
            bw, bh, db, at, n, pre = done[k]; blk = f'{bw}x{bh}' if bw else 'bc'; preset, tried = pre.split('/'); retried += int(tried) > 1
            s = by_block.setdefault(f'{BC[k[3]][0]} {blk} {preset}', [0, 0, 99.0]); s[0] += 1; s[1] += n; s[2] = min(s[2], db / 100)
            q.write(f'{names[k]}\t{k[3]:#x}\t{k[4]}\t{k[5]}\t{k[6]}\t{blk}\t{preset}\t{tried}\t{db / 100:.2f}\n')
    pack = out / PACK
    if pack.exists(): pack.unlink()
    part.rename(pack)
    jpath.unlink()
    man = dict(ident, complete=True, entries=len(keys), pack_bytes=os.path.getsize(pack), zero_blocks_fixed=zeros, skipped=skipped,
               stayed_bc=len(stayed), retried=retried,
               by_kind_block_preset={k: dict(textures=v[0], bytes=v[1], min_psnr_db=round(v[2], 2)) for k, v in sorted(by_block.items())},
               seconds=round(time.time() - t0))
    (out / 'manifest.json').write_text(json.dumps(man, indent=1), encoding='utf-8')
    log(f'ASTC cache: {len(keys)} textures ({retried} needed more than the first step, {len(stayed)} stay BC), {man["pack_bytes"] / 2**30:.2f} GB in {out / PACK}, {man["seconds"]} s')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('romfs', nargs='+', help='RomFS folder(s) with CPKs/ (and Update/CPKs/)')
    ap.add_argument('out')
    ap.add_argument('--aoc', action='append', default=[], help='a language-pack folder of CPK archives (repeatable)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--formats', default='bc1,bc2,bc3', help='BC kinds to encode (default bc1,bc2,bc3; bc2,bc3 is the small cache)')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--only', default=None, help='member glob, e.g. "Textures/p6_*" (tests)')
    a = ap.parse_args(argv)
    a.formats = {f.strip() for f in a.formats.split(',') if f.strip()}
    if not a.formats <= {'bc1', 'bc2', 'bc3'}: ap.error('--formats takes bc1, bc2, bc3')
    try:
        return build(a)
    except Fail as e:
        print(f'd3_astc_cache: {e}', file=sys.stderr); return 2


if __name__ == '__main__':
    sys.exit(main())
