#!/usr/bin/env python3
"""Locate MSVC RTTI vtables by mangled class name substring. Usage: rtti.py <substr>"""
import sys, struct
EXE = "/mnt/data/code/nier-replicant-kbm-controller/game/NieR Replicant ver.1.22474487139.exe"
d = open(EXE,'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]
optsz = struct.unpack_from('<H', d, pe+20)[0]
base = struct.unpack_from('<Q', d, pe+24+24)[0]
off = pe+24+optsz
secs=[]
for _ in range(nsec):
    name=d[off:off+8].rstrip(b'\0').decode(errors='replace')
    vs,va,rs,ra=struct.unpack_from('<IIII',d,off+8); secs.append((name,va,vs,ra,rs)); off+=40

def off2rva(o):
    for n,va,vs,ra,rs in secs:
        if ra <= o < ra+rs: return va+(o-ra), n
    return None,None
def rva2off(r):
    for n,va,vs,ra,rs in secs:
        if va <= r < va+vs:
            if r-va >= rs: return None
            return ra+(r-va)
    return None

needle = sys.argv[1].encode()
i = 0
while True:
    i = d.find(needle, i)
    if i < 0: break
    start = i
    # find start of the name string (preceded by NUL)
    while start > 0 and d[start-1] != 0: start -= 1
    name = d[start:d.find(b'\0', start)].decode(errors='replace')
    tdoff = start - 0x10
    tdrva, sec = off2rva(tdoff)
    if tdrva is not None:
        # find 4-byte references to tdrva (COL.pTypeDescriptor at +0xC)
        pat = struct.pack('<I', tdrva)
        j = 0
        cols = []
        while True:
            j = d.find(pat, j)
            if j < 0: break
            rva_j, s_j = off2rva(j)
            if s_j in ('.rdata', '_RDATA'):
                colrva = rva_j - 0xC
                colva = base + colrva
                # find 8-byte pointer to COL -> vtable-8
                p8 = struct.pack('<Q', colva)
                k = 0
                while True:
                    k = d.find(p8, k)
                    if k < 0: break
                    r_k, s_k = off2rva(k)
                    if s_k in ('.rdata','_RDATA'):
                        cols.append((colva, base+r_k+8))
                    k += 1
            j += 1
        print(f"{name}\n  TD va=0x{base+tdrva:x}")
        for colva, vt in cols:
            print(f"  COL va=0x{colva:x}  VTABLE va=0x{vt:x}")
            o = rva2off(vt-base)
            if o:
                fns = []
                for m in range(12):
                    p = struct.unpack_from('<Q', d, o+8*m)[0]
                    if base+0x1000 <= p < base+0xab8000: fns.append(f"0x{p:x}")
                    else: break
                print("    vfuncs:", ', '.join(fns))
    i += 1
