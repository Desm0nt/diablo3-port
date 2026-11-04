"""Find code referencing a string (adrp + add pair) in a decompressed NSO image. Usage: xref_str.py <mod.raw> <text_end hex> <substring>"""
import sys, struct
b = open(sys.argv[1], 'rb').read(); te = int(sys.argv[2], 16); sub = sys.argv[3].encode()
tgt = []
i = b.find(sub)
while i >= 0:
    s = b.rfind(b'\0', 0, i) + 1; tgt.append(s); i = b.find(sub, i + 1)
tgt = set(tgt); pages = {t & ~0xfff for t in tgt}
w = struct.unpack_from(f'<{te // 4}I', b, 0)
for k, x in enumerate(w):
    if x & 0x9f000000 == 0x90000000:
        imm = ((x >> 29) & 3) | ((x >> 5) & 0x7ffff) << 2
        if imm & 0x100000: imm -= 0x200000
        pc = k * 4; page = (pc & ~0xfff) + (imm << 12)
        if page in pages:
            for j in range(k + 1, min(k + 6, len(w))):
                y = w[j]
                if y & 0xffc00000 == 0x91000000 and (y >> 5) & 31 == x & 31:
                    a = page + ((y >> 10) & 0xfff)
                    if a in tgt: print(hex(pc), hex(a), b[a:b.index(b'\0', a)][:70])
