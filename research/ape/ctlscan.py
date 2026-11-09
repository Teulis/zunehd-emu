import struct, glob, collections
d = open(r'C:\rea-work\bse\avp_0648.bin', 'rb').read()
base = 0x06480000
lit = collections.Counter()
for off in range(0x17000, 0x20000, 4):     # 0x06497000..0x064a0000
    w, = struct.unpack_from('<I', d, off)
    if (w >> 28) in (8, 0xa) and ((w >> 24) & 0xf) in (1, 2, 7) and (w & 0x000f0000) in (0, 0x20000):
        lit[w] += 1
seen = set()
for fn in (r'C:\rea-work\ape\t4.txt', r'C:\rea-work\ape\t5.txt'):
    for line in open(fn):
        if ' ctl ' in line:
            seen.add(int(line.split(' ctl ')[1].split()[0], 16))
print('literals not seen in kicks:', ['%08x' % w for w in sorted(lit) if w not in seen])
print('seen:', ['%08x' % w for w in sorted(seen)])
hdr = collections.Counter()
for off in range(0x17000, 0x20000, 4):
    w, = struct.unpack_from('<I', d, off)
    if (w & 0xff400000) == 0x00400000 and (w & 0xf8) == 0 and (w >> 16) & 0xdc == 0x40:
        hdr[w] += 1
print('header-like literals:', ['%08x' % w for w in sorted(hdr)])
