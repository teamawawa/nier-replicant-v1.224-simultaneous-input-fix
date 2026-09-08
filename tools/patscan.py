#!/usr/bin/env python3
"""Scan the game exe for IDA-style byte patterns; report file offsets and RVAs/VAs."""
import sys, re, struct

EXE = "/mnt/data/code/nier-replicant-kbm-controller/game/NieR Replicant ver.1.22474487139.exe"

def sections(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    optsz = struct.unpack_from('<H', d, pe+20)[0]
    base = struct.unpack_from('<Q', d, pe+24+24)[0]
    off = pe+24+optsz
    secs = []
    for _ in range(nsec):
        name = d[off:off+8].rstrip(b'\0').decode(errors='replace')
        vs, va, rs, ra = struct.unpack_from('<IIII', d, off+8)
        secs.append((name, va, vs, ra, rs))
        off += 40
    return base, secs

def off2va(base, secs, off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra+rs:
            return base + va + (off - ra), name
    return None, None

def va2off(base, secs, addr):
    rva = addr - base
    for name, va, vs, ra, rs in secs:
        if va <= rva < va+vs:
            return ra + (rva - va)
    return None

def compile_pat(pat):
    out = b''
    for tok in pat.split():
        if tok in ('?', '??'):
            out += b'.'
        else:
            out += re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)

def main():
    pat = sys.argv[1]
    d = open(EXE, 'rb').read()
    base, secs = sections(d)
    rx = compile_pat(pat)
    n = 0
    for m in rx.finditer(d):
        va, sec = off2va(base, secs, m.start())
        if sec != '.text':
            continue
        n += 1
        print(f"match file=0x{m.start():x} va=0x{va:x} rva=0x{va-base:x} sec={sec}")
        print("   ", d[m.start():m.start()+32].hex(' '))
        if n > 20: break
    if not n:
        print("no matches in .text")

if __name__ == '__main__':
    main()
