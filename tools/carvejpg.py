"""Carve embedded JPEGs out of a container (e.g. runtimeZune.v3.1.zcp) and
print their dimensions.  py -3 -I tools\\carvejpg.py IN OUTDIR"""
import os, struct, sys


def jpeg_end_and_size(b, s):
    """Walk JPEG segments from SOI at s; return (end offset, (w, h))."""
    o, wh = s + 2, None
    while o + 4 <= len(b):
        if b[o] != 0xFF:
            return None, wh
        m = b[o + 1]
        if m == 0xD9:
            return o + 2, wh
        if m in (0x01,) or 0xD0 <= m <= 0xD7:
            o += 2
            continue
        ln = struct.unpack(">H", b[o + 2:o + 4])[0]
        if m in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", b[o + 5:o + 9])
            wh = (w, h)
        if m == 0xDA:                       # scan: skip entropy data to next marker
            o += 2 + ln
            while o + 1 < len(b) and not (b[o] == 0xFF and b[o + 1] not in (0x00,) and
                                          not 0xD0 <= b[o + 1] <= 0xD7):
                o += 1
            continue
        o += 2 + ln
    return None, wh


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    b = open(src, "rb").read()
    i = n = 0
    while True:
        i = b.find(b"\xff\xd8\xff", i)
        if i < 0:
            break
        end, wh = jpeg_end_and_size(b, i)
        if end:
            p = os.path.join(out, "jpg_%02d_%08x.jpg" % (n, i))
            open(p, "wb").write(b[i:end])
            print("%s %s %d bytes" % (os.path.basename(p), "%dx%d" % wh if wh else "?", end - i))
            n += 1
            i = end
        else:
            i += 3


if __name__ == "__main__":
    main()
