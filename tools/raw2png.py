"""Raw 32-bpp surface dump -> PNG (stdlib only).

  py -3 -I tools\\raw2png.py IN.bin OUT.png --w 272 --h 480 [--pitch 1088] [--order rgba|bgra]
     [--alpha bg|ignore]   (bg: composite over a grey checkerboard to show transparency)
"""
import argparse, struct, zlib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--w", type=int, default=272)
    ap.add_argument("--h", type=int, default=480)
    ap.add_argument("--pitch", type=int, default=0)
    ap.add_argument("--order", default="rgba")
    ap.add_argument("--alpha", default="ignore")
    a = ap.parse_args()
    pitch = a.pitch or a.w * 4
    d = open(a.inp, "rb").read()
    rows = []
    for y in range(a.h):
        row = bytearray(b"\0")
        for x in range(a.w):
            o = y * pitch + 4 * x
            p = d[o:o + 4] if o + 4 <= len(d) else b"\0\0\0\0"
            r, g, b, al = (p[0], p[1], p[2], p[3]) if a.order == "rgba" else (p[2], p[1], p[0], p[3])
            if a.alpha == "bg":
                bg = 96 if ((x // 8) + (y // 8)) & 1 else 160
                r, g, b = [(c * al + bg * (255 - al)) // 255 for c in (r, g, b)]
            row += bytes((r, g, b))
        rows.append(bytes(row))

    def chunk(t, body):
        c = t + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", a.w, a.h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(a.out, "wb").write(png)
    print("%s: %dx%d" % (a.out, a.w, a.h))


if __name__ == "__main__":
    main()
