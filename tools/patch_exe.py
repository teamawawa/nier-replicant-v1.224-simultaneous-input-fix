#!/usr/bin/env python3
"""Statically apply the NierConcurrentInput patches to a copy of the game exe.

Alternative to the ASI plugin for setups without an ASI loader. Never writes to
the input file: it always produces a separate output file.

  tools/patch_exe.py game/"NieR Replicant ver.1.22474487139.exe" -o build/patched.exe
  tools/patch_exe.py <exe> --verify        # report what would be patched, write nothing

Defaults match dist/NierConcurrentInput.ini: the mouse gate and the controller
prompts are patched, keyboard concurrency is not.
"""
import argparse, re, struct, shutil, sys

# --- MouseUsable() device-mode gate (0x1403D3F5A at the default image base) ---
#   cmp byte [rsi+0x8C],0 / jne / cmp byte [rsi+0x8D],0 / jne / mov al,1 / jmp / xor al,al
GATE_SIG = "80 ?? 8C 00 00 00 00 75 0D 80 ?? 8D 00 00 00 00 75 04 B0 01 EB 02 32 C0"
GATE_PATCHED_SIG = "80 ?? 8C 00 00 00 00 90 90 80 ?? 8D 00 00 00 00 90 90 B0 01 EB 02 32 C0"
# offsets of the two `jne rel8` instructions within the signature
GATE_NOPS = (0x07, 0x10)

# --- keyboard concurrency -----------------------------------------------------
# Every axis/button reader in the input module gates its keyboard/mouse
# contribution on `cmp byte [ctx+0x8C],0` + a following jne. They all sit within
# ±0x1000 of MouseUsable(), and nothing else in that window matches.
INPUT_WINDOW = 0x1000
# The two stick-axis readers replace the pad value with the keyboard one instead
# of adding it; movaps (0F 28 F0) and addps (0F 58 F0) are both three bytes.
AXIS_MERGE_SIG = ("48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 F0 "
                  "48 8B CB E8 ?? ?? ?? ?? 84 C0")
AXIS_MERGE_OPCODE = 13

# --- controller prompts -------------------------------------------------------
# (name, signature, rip-disp32 offset, instruction length, may match more than one)
PROMPT_SITES = [
    ("button icon id",        "80 3D ?? ?? ?? ?? 00 0F B6 C3 0F 84", 2, 7, False),
    ("inline text icon",      "80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 49 8B 06 41 B8 5C 01 00 00", 2, 7, False),
    ("face button test",      "80 3D ?? ?? ?? ?? 00 74 7D 48 8D 0D", 2, 7, False),
    ("key help item",         "80 3D ?? ?? ?? ?? 00 8B FA 48 8B D9 75 10 80 79 6A 00", 2, 7, False),
    ("key help message",      "80 3D ?? ?? ?? ?? 00 B8 CB 0A 00 00 B9 96 0A 00 00 0F 45 C1", 2, 7, False),
    ("skip prompt",           "0F B6 3D ?? ?? ?? ?? 74 36 48 8B 85 58 05 00 00", 3, 7, False),
    ("skip prompt rebuild",   "0F B6 3D ?? ?? ?? ?? 74 31 48 8B 83 58 05 00 00", 3, 7, False),
    ("tutorial text",         "0F B6 05 ?? ?? ?? ?? 48 8D 72 04 48 8B FA 48 8B D9 3A 81 C9 02 00 00", 3, 7, False),
    ("tutorial text rebuild", "0F B6 05 ?? ?? ?? ?? 48 8D 8B D8 02 00 00 88 83 C9 02 00 00", 3, 7, False),
    ("memo text",             "0F B6 1D ?? ?? ?? ?? 88 5F 4D 48 8B CD", 3, 7, False),
    ("memo text sibling",     "0F B6 05 ?? ?? ?? ?? 88 46 4D 44 39 AE 40 06 00 00", 3, 7, False),
    ("memo device cache",     "0F B6 05 ?? ?? ?? ?? 3A 41 4D 74 4E 88 41 4D", 3, 7, True),
    ("book text cache",       "0F B6 05 ?? ?? ?? ?? 88 85 35 22 00 00", 3, 7, False),
    ("book text",             "0F B6 1D ?? ?? ?? ?? 49 8B CF E8", 3, 7, False),
    ("book text rebuild",     "0F B6 35 ?? ?? ?? ?? 40 3A B5 35 22 00 00", 3, 7, False),
]


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


def section(secs, want):
    for name, va, vs, ra, rs in secs:
        if name == want:
            return ra, rs, va
    sys.exit(f"{want} section not found")


def compile_sig(sig):
    return re.compile(b''.join(
        b'.' if t.startswith('?') else re.escape(bytes([int(t, 16)]))
        for t in sig.split()), re.DOTALL)


def find_all(data, ra, rs, sig):
    return [ra + m.start() for m in compile_sig(sig).finditer(data[ra:ra+rs])]


def find_unique(data, ra, rs, sig, label):
    hits = find_all(data, ra, rs, sig)
    if len(hits) > 1:
        sys.exit(f"{label}: signature is ambiguous ({len(hits)} matches) — refusing to patch")
    return hits[0] if hits else None


class Image:
    """File-offset <-> VA helper for one loaded PE."""

    def __init__(self, data):
        self.data = data
        self.base, self.secs = sections(data)

    def off2va(self, off):
        for _, va, vs, ra, rs in self.secs:
            if ra <= off < ra + rs:
                return self.base + va + (off - ra)
        return None

    def va2off(self, addr):
        rva = addr - self.base
        for _, va, vs, ra, rs in self.secs:
            if va <= rva < va + vs:
                return ra + (rva - va)
        return None


def patch_mouse(img, verify):
    data, ra, rs = img.data, *section(img.secs, '.text')[:2]
    if find_unique(data, ra, rs, GATE_PATCHED_SIG, "patched gate") is not None:
        print("mouse gate: already patched")
        return
    site = find_unique(data, ra, rs, GATE_SIG, "gate")
    if site is None:
        sys.exit("mouse gate signature not found — wrong or already modified executable")
    print(f"mouse gate: va 0x{img.off2va(site):x}")
    if verify:
        return
    for off in GATE_NOPS:
        if data[site+off] != 0x75:
            sys.exit(f"unexpected byte 0x{data[site+off]:02x} at +0x{off:x}, expected 0x75 (jne)")
        data[site+off:site+off+2] = b'\x90\x90'
        print(f"  nopped jne at va 0x{img.off2va(site) + off:x}")


def patch_keyboard(img, verify):
    data = img.data
    ra, rs, _ = section(img.secs, '.text')
    anchor = find_unique(data, ra, rs, GATE_SIG, "gate")
    if anchor is None:
        anchor = find_unique(data, ra, rs, GATE_PATCHED_SIG, "patched gate")
    if anchor is None:
        sys.exit("MouseUsable gate not found, cannot locate the input module")

    lo = max(ra, anchor - INPUT_WINDOW)
    hi = min(ra + rs, anchor + INPUT_WINDOW)
    n = done = 0
    for p in range(lo, hi - 16):
        if data[p] != 0x80 or (data[p+1] & 0xF8) != 0xB8:
            continue
        if data[p+2] != 0x8C or data[p+3] or data[p+4] or data[p+5] or data[p+6]:
            continue
        if p > ra and (data[p-1] & 0xF0) == 0x40:
            continue
        if p == anchor:
            continue
        q = p + 7
        if data[q] == 0x0F and data[q+1] == 0xB6:
            q += 3
        elif data[q:q+4] == b'\x48\x8b\x5c\x24':
            q += 5
        if data[q] == 0x90 or data[q:q+2] == b'\xb0\x01':
            done += 1
            continue
        if data[q:q+3] == b'\x0f\x94\xc0':
            n += 1
            print(f"  device gate at va 0x{img.off2va(p):x}, sete al at 0x{img.off2va(q):x}")
            if not verify:
                data[q:q+3] = b'\xb0\x01\x90'   # mov al, 1 + nop
            continue
        if data[q] == 0x75:
            ln = 2
        elif data[q] == 0x0F and data[q+1] == 0x85:
            ln = 6
        else:
            print(f"  !! gate at va 0x{img.off2va(p):x}: unrecognised shape, skipped")
            continue
        n += 1
        print(f"  device gate at va 0x{img.off2va(p):x}, jne at 0x{img.off2va(q):x}")
        if not verify:
            data[q:q+ln] = b'\x90' * ln
    print(f"keyboard concurrency: {n} device gate(s)"
          + (f", {done} already patched" if done else ""))

    merges = find_all(data, ra, rs, AXIS_MERGE_SIG)
    if not merges and n == 0:
        print("  axis merges: already patched")
        return
    if len(merges) != 2:
        sys.exit(f"expected 2 axis merge sites, found {len(merges)}")
    for m in merges:
        print(f"  axis merge at va 0x{img.off2va(m + AXIS_MERGE_OPCODE):x}: movaps -> addps")
        if not verify:
            data[m + AXIS_MERGE_OPCODE] = 0x58


def patch_prompts(img, verify):
    data = img.data
    ra, rs, _ = section(img.secs, '.text')
    rra, rrs, _ = section(img.secs, '.rdata')
    one = data.find(b'\x01', rra, rra + rrs)
    if one < 0:
        sys.exit("no constant 1 byte in .rdata")
    one_va = img.off2va(one)
    print(f"controller prompts: reading the flag as 1 from va 0x{one_va:x}")

    found, flag = [], None
    for name, sig, dispoff, length, multi in PROMPT_SITES:
        hits = find_all(data, ra, rs, sig)
        if not hits:
            print(f"  !! {name}: pattern not found")
            continue
        if not multi and len(hits) > 1:
            print(f"  !! {name}: pattern is ambiguous ({len(hits)} matches), skipped")
            continue
        for h in hits:
            disp = struct.unpack_from('<i', data, h + dispoff)[0]
            target = img.off2va(h) + length + disp
            if flag is None:
                flag = target
            if target != flag:
                print(f"  !! {name} at va 0x{img.off2va(h):x}: reads 0x{target:x}, "
                      f"expected 0x{flag:x}, skipped")
                continue
            found.append((name, h, dispoff, length))
    if not found:
        sys.exit("controller prompts: nothing to patch")
    print(f"  device flag is va 0x{flag:x}")
    for name, h, dispoff, length in found:
        print(f"  {name} at va 0x{img.off2va(h):x}")
        if not verify:
            struct.pack_into('<i', data, h + dispoff, one_va - (img.off2va(h) + length))
    print(f"controller prompts: {len(found)} read(s) redirected")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe')
    ap.add_argument('-o', '--output')
    ap.add_argument('--verify', action='store_true', help='report patch state, write nothing')
    ap.add_argument('--no-mouse', action='store_true',
                    help='skip the MouseUsable gate patch')
    ap.add_argument('--keyboard', action='store_true',
                    help='also make the keyboard work while a controller is active')
    ap.add_argument('--no-prompts', action='store_true',
                    help='skip pinning the button prompts to controller glyphs')
    args = ap.parse_args()

    if not args.verify and not args.output:
        ap.error("-o/--output is required unless --verify is given")

    img = Image(bytearray(open(args.exe, 'rb').read()))

    if not args.no_mouse:
        patch_mouse(img, args.verify)
    if args.keyboard:
        patch_keyboard(img, args.verify)
    if not args.no_prompts:
        patch_prompts(img, args.verify)

    if args.verify:
        return
    with open(args.output, 'wb') as f:
        f.write(img.data)
    print(f"wrote {args.output}")


if __name__ == '__main__':
    main()
