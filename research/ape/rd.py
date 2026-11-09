import struct, sys
d = open(sys.argv[1], 'rb').read()
base = 0x40000000
a = int(sys.argv[2], 0)
n = int(sys.argv[3], 0)
for i in range(n):
    w, = struct.unpack_from('<I', d, a - base + 4 * i)
    s = w - (1 << 32) if w & 0x80000000 else w
    if i % 4 == 0:
        print('\n%08x:' % (a + 4 * i), end='')
    print(' %08x(%+.6f)' % (w, s / 2.0 ** 31), end='')
print()
