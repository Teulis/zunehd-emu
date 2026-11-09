import struct, sys, hashlib
d = open(sys.argv[1], 'rb').read()
seen = {}; draws = 0; h = hashlib.md5()
for i in range(0, len(d) - 11, 12):
    c, off, val = struct.unpack_from('<III', d, i)
    if c & 0xffff != 0x60: continue
    off &= 0xfff
    if off == 0xe22: h = hashlib.md5()
    if 0x540 <= off <= 0x541 or 0x600 <= off <= 0x604 or 0x700 <= off <= 0x701 or 0x800 <= off <= 0x804 or 0x900 <= off <= 0x901 or off == 0xe20 or off == 0x520:
        h.update(struct.pack('<II', off, val))
    if off == 0x123:
        k = h.hexdigest()
        if k not in seen: seen[k] = [draws, 0]
        seen[k][1] += 1
        draws += 1
for k, v in seen.items(): print(v[0], v[1])
