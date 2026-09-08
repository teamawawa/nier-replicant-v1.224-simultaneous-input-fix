#!/usr/bin/env python3
"""List RUNTIME_FUNCTION entries from .pdata; or find the function containing a VA."""
import sys, struct, bisect
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
pd=[s for s in secs if s[0]=='.pdata'][0]
_,pva,pvs,pra,prs=pd
funcs=[]
for o in range(pra, pra+pvs, 12):
    s,e,u = struct.unpack_from('<III', d, o)
    if s==0: continue
    funcs.append((base+s, base+e))
funcs.sort()
starts=[f[0] for f in funcs]
if len(sys.argv)>1:
    for a in sys.argv[1:]:
        va=int(a,16)
        i=bisect.bisect_right(starts, va)-1
        if i>=0 and funcs[i][0] <= va < funcs[i][1]:
            print(f"0x{va:x} -> func 0x{funcs[i][0]:x} - 0x{funcs[i][1]:x} (size {funcs[i][1]-funcs[i][0]})")
        else:
            print(f"0x{va:x} -> not in .pdata")
else:
    print(f"{len(funcs)} functions")
