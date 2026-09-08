#!/usr/bin/env python3
"""Disassemble the game exe at a virtual address. Usage: dis.py <va-hex> [count]"""
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = "/mnt/data/code/nier-replicant-kbm-controller/game/NieR Replicant ver.1.22474487139.exe"

def load():
    d = open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    optsz = struct.unpack_from('<H', d, pe+20)[0]
    base = struct.unpack_from('<Q', d, pe+24+24)[0]
    off = pe+24+optsz
    secs = []
    for _ in range(nsec):
        name = d[off:off+8].rstrip(b'\0').decode(errors='replace')
        vs, va, rs, ra = struct.unpack_from('<IIII', d, off+8)
        secs.append((name, va, vs, ra, rs)); off += 40
    return d, base, secs

def va2off(base, secs, addr):
    rva = addr - base
    for name, va, vs, ra, rs in secs:
        if va <= rva < va+vs:
            if rva - va >= rs: return None
            return ra + (rva - va)
    return None

def main():
    d, base, secs = load()
    va = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    off = va2off(base, secs, va)
    if off is None:
        print("VA not mapped"); return
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    for i in md.disasm(d[off:off+count*15], va):
        print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")
        count -= 1
        if count <= 0: break

if __name__ == '__main__':
    main()
