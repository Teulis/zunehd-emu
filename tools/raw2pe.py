"""Wrap a raw ARM image in a minimal PE so REA/Ghidra can analyse it.

  py -3 -I tools\\raw2pe.py IN.bin OUT.dll --base 0x060ab000 --entry 0x060abf30 [--imagebase 0x06000000]

One RWX section at --base containing the file; machine 0x1C4 (ARMNT, as the
_armnt copies of the CE modules). Code is ARM/Thumb as in the original.
"""
import argparse, struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--entry", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--imagebase", type=lambda x: int(x, 0), default=None)
    a = ap.parse_args()
    data = open(a.inp, "rb").read()
    ib = a.imagebase if a.imagebase is not None else a.base & ~0xffff
    sec_rva = a.base - ib
    falign, salign = 0x200, 0x1000
    raw = data + b"\0" * (-len(data) % falign)
    hdr_size = 0x400
    vsize = (len(data) + salign - 1) & ~(salign - 1)
    image_size = sec_rva + vsize

    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3c, 0x80)
    coff = struct.pack("<4sHHIIIHH", b"PE\0\0", 0x1C4, 1, 0, 0, 0, 0xE0, 0x2102)
    opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
                      0x10B, 1, 0, len(raw), 0, 0, a.entry - ib, sec_rva, sec_rva, ib,
                      salign, falign, 4, 0, 0, 0, 4, 0, 0, image_size, hdr_size, 0,
                      2, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b"\0" * (16 * 8)
    sec = struct.pack("<8sIIIIIIHHI", b".text", vsize, sec_rva, len(raw), hdr_size,
                      0, 0, 0, 0, 0xE0000020)
    head = bytes(dos) + coff + opt + sec
    head += b"\0" * (hdr_size - len(head))
    open(a.out, "wb").write(head + raw)
    print("%s: imagebase %#x section %#x+%#x entry %#x" % (a.out, ib, a.base, len(data), a.entry))


if __name__ == "__main__":
    main()
