#!/usr/bin/env python3
"""Find `call/jmp qword [rip+X]` sites in .text that target given IAT VAs."""
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
text=[s for s in secs if s[0]=='.text'][0]
_,tva,tvs,tra,trs = text
targets = {int(a,16) for a in sys.argv[1:]}
buf = d[tra:tra+trs]
i=0
while True:
    j = buf.find(b'\xff\x15', i)
    k = buf.find(b'\xff\x25', i)
    if j<0 and k<0: break
    if j<0 or (0<=k<j): j,op = k,'jmp'
    else: op='call'
    i = j+1
    rel = struct.unpack_from('<i', buf, j+2)[0]
    insva = base+tva+j
    tgt = insva+6+rel
    if tgt in targets:
        print(f"0x{insva:x}  {op} [0x{tgt:x}]")
