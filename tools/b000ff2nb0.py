"""Convert a Windows CE B000FF image (NK.bin, EXT.bin from the Zune HD firmware) into the
flat .nb0 image that mkemmc.py writes into the emulated eMMC (ZBINFS partition).

B000FF layout: "B000FF\\n", image start (u32), image length (u32), then records
{address u32, length u32, checksum u32, data[length]} until a record with address 0.
The .nb0 is image_length bytes, each record copied to (address - image start).

Usage: py -3 -I tools\\b000ff2nb0.py IN.bin OUT.nb0
"""
import struct
import sys


def convert(src, dst):
    d = open(src, "rb").read()
    if d[:7] != b"B000FF\n":
        sys.exit("%s: not a B000FF image" % src)
    start, length = struct.unpack_from("<II", d, 7)
    out = bytearray(length)
    o, n = 15, 0
    while o + 12 <= len(d):
        addr, ln, chk = struct.unpack_from("<III", d, o)
        o += 12
        if addr == 0:
            break
        data = d[o:o + ln]
        if sum(data) & 0xFFFFFFFF != chk:
            sys.exit("%s: checksum error in record @0x%08x" % (src, addr))
        off = addr - start
        if off < 0 or off + ln > length:
            sys.exit("%s: record @0x%08x outside the image" % (src, addr))
        out[off:off + ln] = data
        o += ln
        n += 1
    open(dst, "wb").write(out)
    print("%s -> %s: %d records, %d bytes (image start 0x%08x)" % (src, dst, n, length, start))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    convert(sys.argv[1], sys.argv[2])
