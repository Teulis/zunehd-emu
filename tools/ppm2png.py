"""Convert a binary PPM (P6, as written by QEMU's screendump) to PNG. stdlib only.
Usage: py -3 -I tools\\ppm2png.py in.ppm out.png [--scale N]"""
import struct, sys, zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 1
    d = open(src, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:                       # magic, width, height, maxval
        while d[pos:pos + 1].isspace():
            pos += 1
        if d[pos:pos + 1] == b"#":
            pos = d.index(b"\n", pos) + 1
            continue
        end = pos
        while not d[end:end + 1].isspace():
            end += 1
        parts.append(d[pos:end])
        pos = end
    pos += 1
    assert parts[0] == b"P6"
    w, h = int(parts[1]), int(parts[2])
    px = d[pos:pos + w * h * 3]
    rows = []
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        if scale > 1:
            row = b"".join(row[i:i + 3] * scale for i in range(0, len(row), 3))
        rows.extend([b"\0" + row] * scale)
    W, H = w * scale, h * scale

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    nz = sum(1 for b in px if b)
    print(f"{dst}: {W}x{H}, {nz} non-zero bytes of {len(px)}")


if __name__ == "__main__":
    main()
