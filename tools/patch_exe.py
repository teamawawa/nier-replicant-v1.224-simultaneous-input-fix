#!/usr/bin/env python3
"""Statically apply the concurrent-input patch to a copy of the game exe.

Alternative to the ASI plugin for setups without an ASI loader. Never writes to
the input file: it always produces a separate output file.

  tools/patch_exe.py game/"NieR Replicant ver.1.22474487139.exe" -o build/patched.exe
  tools/patch_exe.py <exe> --verify        # report current patch state only
"""
import argparse, re, struct, shutil, sys

# MouseUsable() device-mode gate (0x1403D3F5A at the default image base):
#   cmp byte [rsi+0x8C],0 / jne / cmp byte [rsi+0x8D],0 / jne / mov al,1 / jmp / xor al,al
GATE_SIG = "80 ?? 8C 00 00 00 00 75 0D 80 ?? 8D 00 00 00 00 75 04 B0 01 EB 02 32 C0"
GATE_PATCHED_SIG = "80 ?? 8C 00 00 00 00 90 90 80 ?? 8D 00 00 00 00 90 90 B0 01 EB 02 32 C0"
# offsets of the two `jne rel8` instructions within the signature
GATE_NOPS = (0x07, 0x10)


def sections(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe+4] != b'PE\0\0':
        sys.exit("not a PE file")
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    optsz = struct.unpack_from('<H', d, pe+20)[0]
    base = struct.unpack_from('<Q', d, pe+24+24)[0]
    off = pe+24+optsz
    out = []
    for _ in range(nsec):
        name = d[off:off+8].rstrip(b'\0').decode(errors='replace')
        vs, va, rs, ra = struct.unpack_from('<IIII', d, off+8)
        out.append((name, va, vs, ra, rs))
        off += 40
    return base, out


def text_range(secs):
    for name, va, vs, ra, rs in secs:
        if name == '.text':
            return ra, rs, va
    sys.exit(".text section not found")


def compile_sig(sig):
    return re.compile(b''.join(
        b'.' if t.startswith('?') else re.escape(bytes([int(t, 16)]))
        for t in sig.split()), re.DOTALL)


def find_unique(data, ra, rs, sig, label):
    hits = [m.start() for m in compile_sig(sig).finditer(data[ra:ra+rs])]
    if len(hits) > 1:
        sys.exit(f"{label}: signature is ambiguous ({len(hits)} matches) — refusing to patch")
    return ra + hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe')
    ap.add_argument('-o', '--output')
    ap.add_argument('--verify', action='store_true', help='report patch state, write nothing')
    args = ap.parse_args()

    if not args.verify and not args.output:
        ap.error("-o/--output is required unless --verify is given")

    data = bytearray(open(args.exe, 'rb').read())
    base, secs = sections(data)
    ra, rs, va = text_range(secs)

    already = find_unique(data, ra, rs, GATE_PATCHED_SIG, "patched gate")
    if already is not None:
        print(f"already patched at file 0x{already:x} "
              f"(va 0x{base + va + (already - ra):x})")
        if args.verify:
            return
        shutil.copyfile(args.exe, args.output)
        print(f"copied unchanged to {args.output}")
        return

    site = find_unique(data, ra, rs, GATE_SIG, "gate")
    if site is None:
        sys.exit("gate signature not found — wrong or already modified executable")

    site_va = base + va + (site - ra)
    print(f"gate at file 0x{site:x} (va 0x{site_va:x})")
    if args.verify:
        print("not patched")
        return

    for off in GATE_NOPS:
        if data[site+off] != 0x75:
            sys.exit(f"unexpected byte 0x{data[site+off]:02x} at +0x{off:x}, expected 0x75 (jne)")
        data[site+off:site+off+2] = b'\x90\x90'
        print(f"  nopped jne at va 0x{site_va + off:x}")

    with open(args.output, 'wb') as f:
        f.write(data)
    print(f"wrote {args.output}")


if __name__ == '__main__':
    main()
