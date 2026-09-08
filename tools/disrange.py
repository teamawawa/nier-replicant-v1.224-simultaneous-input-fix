#!/usr/bin/env python3
"""Disassemble every .pdata function whose start VA lies in [lo,hi). Usage: disrange.py <lo> <hi>"""
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
EXE = "/mnt/data/code/nier-replicant-kbm-controller/game/NieR Replicant ver.1.22474487139.exe"
d = open(EXE,'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]
optsz = struct.unpack_from('<H', d, pe+20)[0]
base = struct.unpack_from('<Q', d, pe+24+24)[0]
off = pe+24+optsz
secs=[]
for _ in range(nsec):
    n=d[off:off+8].rstrip(b'\0').decode(errors='replace')
    vs,va,rs,ra=struct.unpack_from('<IIII',d,off+8); secs.append((n,va,vs,ra,rs)); off+=40
def rva2off(r):
    for n,va,vs,ra,rs in secs:
        if va<=r<va+vs:
            return None if r-va>=rs else ra+(r-va)
    return None
_,pva,pvs,pra,prs=[s for s in secs if s[0]=='.pdata'][0]
funcs=[]
for o in range(pra,pra+pvs,12):
    s,e,u=struct.unpack_from('<III',d,o)
    if s: funcs.append((base+s,base+e))
funcs=sorted(set(funcs))
lo=int(sys.argv[1],16); hi=int(sys.argv[2],16)
md=Cs(CS_ARCH_X86,CS_MODE_64)
for s,e in funcs:
    if not (lo<=s<hi): continue
    o=rva2off(s-base)
    if o is None: continue
    print(f"\n;--- func 0x{s:x} .. 0x{e:x} ---")
    for i in md.disasm(d[o:o+(e-s)], s):
        print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")
