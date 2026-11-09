"""Crop + nearest-neighbour scale a binary PPM (P6) to PNG (stdlib only).

  py -3 -I tools\\ppmcrop.py IN.ppm OUT.png --x 0 --y 400 --w 272 --h 80 --scale 3
"""
import argparse, struct, zlib


def read_ppm(p):
    d = open(p, "rb").read()
    parts, o = [], 0
    while len(parts) < 4:
        while d[o:o + 1].isspace():
            o += 1
        if d[o:o + 1] == b"#":
            o = d.index(b"\n", o) + 1
            continue
        e = o
        while not d[e:e + 1].isspace():
            e += 1
        parts.append(d[o:e])
        o = e
    return int(parts[1]), int(parts[2]), d[o + 1:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--x", type=int, default=0)
    ap.add_argument("--y", type=int, default=0)
    ap.add_argument("--w", type=int, default=0)
    ap.add_argument("--h", type=int, default=0)
    ap.add_argument("--scale", type=int, default=3)
    a = ap.parse_args()
    W, H, px = read_ppm(a.inp)
    w = a.w or W - a.x
    h = a.h or H - a.y
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            o = ((a.y + y) * W + a.x + x) * 3
            line += px[o:o + 3] * a.scale
        rows.extend([b"\0" + bytes(line)] * a.scale)

    def chunk(t, body):
        c = t + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * a.scale, h * a.scale,
                                                             8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(a.out, "wb").write(png)
    print("%s: %dx%d (crop %d,%d %dx%d of %dx%d)" % (a.out, w * a.scale, h * a.scale,
                                                      a.x, a.y, w, h, W, H))


if __name__ == "__main__":
    main()
