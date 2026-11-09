"""Make Ghidra/REA-loadable copies of WinCE ARM PE modules.

REA's Ghidra bridge only admits ARMNT PEs, so copy each module to
extracted\\<image>\\raw\\<name>_armnt.<ext> with the PE machine field
patched 0x01C2 (ARM) -> 0x01C4 (ARMNT). The code itself is unchanged.

Usage: py -3 -I tools\\armnt.py zdisplay.dll libnvddk_disp.dll [--image NK]
"""
import argparse, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("modules", nargs="+")
    ap.add_argument("--image", default="NK")
    a = ap.parse_args()
    src_dir = os.path.join(ROOT, "extracted", a.image, "modules")
    out_dir = os.path.join(ROOT, "extracted", a.image, "raw")
    os.makedirs(out_dir, exist_ok=True)
    for name in a.modules:
        data = bytearray(open(os.path.join(src_dir, name), "rb").read())
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[pe:pe + 4] == b"PE\0\0", name
        mach = struct.unpack_from("<H", data, pe + 4)[0]
        if mach == 0x1C2:
            struct.pack_into("<H", data, pe + 4, 0x1C4)
        base, ext = os.path.splitext(name)
        out = os.path.join(out_dir, f"{base}_armnt{ext}")
        open(out, "wb").write(data)
        print(f"{name}: machine 0x{mach:x} -> {out}")


if __name__ == "__main__":
    main()
