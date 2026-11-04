"""PLT stub -> import name for a decompressed NSO image (MOD0 -> .dynamic -> DT_JMPREL / DT_SYMTAB / DT_STRTAB).
Usage: plt.py <mod.raw> <addr> [addr...]     (addresses of `bl` targets, hex)"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

b = open(sys.argv[1], 'rb').read()
m0 = struct.unpack_from('<I', b, 4)[0]
dyn = m0 + struct.unpack_from('<i', b, m0 + 4)[0]
d = {}
for i in range(0, 0x400, 16):
    t, v = struct.unpack_from('<QQ', b, dyn + i)
    if t == 0: break
    d[t] = v
symtab, strtab, jmprel, pltsz = d[6], d[5], d[23], d[2]
got = {}
for o in range(jmprel, jmprel + pltsz, 24):
    off, info, _ = struct.unpack_from('<QQq', b, o)
    st = struct.unpack_from('<I', b, symtab + 24 * (info >> 32))[0]
    got[off] = b[strtab + st:b.index(b'\0', strtab + st)].decode()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
for a in (int(x, 16) for x in sys.argv[2:]):
    ins = list(md.disasm(b[a:a + 12], a))
    page = ins[0].operands[1].imm if ins[0].mnemonic == 'adrp' else None
    slot = page + ins[1].operands[1].mem.disp if page is not None and len(ins) > 1 else None
    print(hex(a), got.get(slot, f'? (got {slot and hex(slot)})'))
