import struct, sys, glob, os
for fn in sorted(glob.glob(os.path.join(sys.argv[1], 'k*.bin')))[:int(sys.argv[2]) if len(sys.argv) > 2 else 9999]:
    d = open(fn, 'rb').read()
    mag, addr, ctrl, k = struct.unpack_from('<4I', d)
    n = (ctrl & 0xffff) + 1
    ir = d[16:]
    end = addr + 4 * n
    first = struct.unpack_from('<I', ir, addr - 0x40000000)[0]
    nz = sum(1 for i in range(0, 1152) if struct.unpack_from('<i', ir, 0x8010 + 4 * i)[0])
    nz2 = sum(1 for i in range(0, 1152) if struct.unpack_from('<i', ir, 0x8010 + 0x1200 + 4 * i)[0])
    print(os.path.basename(fn), hex(addr), hex(ctrl), 'end', hex(end), 'first %08x' % first, 'spec nz', nz, nz2)
