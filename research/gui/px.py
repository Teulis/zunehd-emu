"""Tiny PNG/PPM pixel inspector (stdlib).
  px.py IMG x y [w h]        -> print RGB of a region (hex, row by row; capped)
  px.py IMG --hist x y w h   -> colour histogram of a region (top 20)
  px.py IMG --zoom x y w h S OUT.png  -> nearest-neighbour zoom crop
"""
import struct, sys, zlib
from collections import Counter


def read(path):
    d = open(path, 'rb').read()
    if d[:2] == b'P6':
        parts, o = [], 2
        while len(parts) < 3:
            while d[o:o+1].isspace(): o += 1
            s = o
            while not d[o:o+1].isspace(): o += 1
            parts.append(int(d[s:o]))
        o += 1
        w, h = parts[0], parts[1]
        px = d[o:o + w*h*3]
        return w, h, [px[i*w*3:(i+1)*w*3] for i in range(h)]
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    o, idat = 8, b''
    while o < len(d):
        ln, = struct.unpack('>I', d[o:o+4]); t = d[o+4:o+8]; c = d[o+8:o+8+ln]; o += 12 + ln
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT': idat += c
    bpp = {2: 3, 6: 4}[ct]
    raw = zlib.decompress(idat); stride = w*bpp; rows = []; prev = bytearray(stride); p = 0
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p+1:p+1+stride]); p += 1 + stride
        for i in range(stride):
            a = line[i-bpp] if i >= bpp else 0; b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b)//2) & 255
            elif f == 4:
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        if bpp == 4: rows.append(bytes(v for i in range(w) for v in line[i*4:i*4+3]))
        else: rows.append(bytes(line))
    return w, h, rows


def wpng(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def ch(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))


def main():
    a = sys.argv; w, h, rows = read(a[1])
    pix = lambda x, y: tuple(rows[y][x*3:x*3+3])
    if a[2] == '--hist':
        x, y, ww, hh = map(int, a[3:7]); c = Counter(pix(i, j) for j in range(y, y+hh) for i in range(x, x+ww))
        for k, v in c.most_common(20): print('%02x%02x%02x' % k, v)
    elif a[2] == '--zoom':
        x, y, ww, hh, s = map(int, a[3:8]); out = []
        for j in range(y, y+hh):
            r = b''.join(bytes(pix(i, j)) * s for i in range(x, x+ww))
            out += [r] * s
        wpng(a[8], ww*s, hh*s, out)
    else:
        x, y = int(a[2]), int(a[3]); ww = int(a[4]) if len(a) > 4 else 1; hh = int(a[5]) if len(a) > 5 else 1
        for j in range(y, y+hh):
            print(j, ' '.join('%02x%02x%02x' % pix(i, j) for i in range(x, min(x+ww, x+16))))


main()
