#!/usr/bin/env python3
"""Find direct call/jmp (E8/E9) sites and 8-byte/rip-relative data refs to given VAs."""
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
    n=d[off:off+8].rstrip(b'\0').decode(errors='replace')
    vs,va,rs,ra=struct.unpack_from('<IIII',d,off+8); secs.append((n,va,vs,ra,rs)); off+=40
_,tva,tvs,tra,trs=[s for s in secs if s[0]=='.text'][0]
targets={int(a,16) for a in sys.argv[1:]}
buf=d[tra:tra+trs]
res=[]
for j in range(len(buf)-5):
    op=buf[j]
    if op in (0xE8,0xE9):
        rel=struct.unpack_from('<i',buf,j+1)[0]
        ins=base+tva+j
        tgt=ins+5+rel
        if tgt in targets:
            res.append((ins,'call' if op==0xE8 else 'jmp',tgt))
for ins,k,t in res:
    print(f"0x{ins:x}  {k} 0x{t:x}")
# absolute 8-byte pointers anywhere (vtables etc.)
for t in targets:
    p=struct.pack('<Q',t); i=0
    while True:
        i=d.find(p,i)
        if i<0: break
        for n,va,vs,ra,rs in secs:
            if ra<=i<ra+rs:
                print(f"  ptr in {n} at va=0x{base+va+(i-ra):x} -> 0x{t:x}")
        i+=1
