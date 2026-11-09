import struct, sys, math
cap = open(sys.argv[1], 'rb').read()
hdr = struct.unpack_from('<4I', cap); ir0 = cap[16:]
ir1 = open(sys.argv[2], 'rb').read()
def w(ir, a, n):
    return list(struct.unpack_from('<%di' % n, ir, a - 0x40000000))
src = struct.unpack_from('<I', ir0, hdr[1] - 0x40000000 + 12)[0]
X = w(ir0, src, 1152)
win = [math.sin(math.pi / 36 * (i + 0.5)) for i in range(36)]
for ch in range(2):
    ovl0 = w(ir0, 0x40012acc + 0x900 * ch, 576)
    T = w(ir1, 0x400141a4 + 0x900 * ch, 576)
    err = sig = 0
    for sb in range(32):
        x = X[576 * ch + sb * 18: 576 * ch + (sb + 1) * 18]
        z = [sum(x[k] * math.cos(math.pi / 72 * (2 * i + 19) * (2 * k + 1)) for k in range(18)) for i in range(18)]
        for i in range(18):
            ref = win[i] * z[i] / 4 + ovl0[sb * 18 + i]
            if sb & 1 and i & 1:
                ref = -ref
            ref *= 2
            got = T[i * 32 + sb]
            err += (got - ref) ** 2; sig += ref ** 2
            if sb in (0, 1) and i < 3:
                print(ch, sb, i, round(ref), got)
    print('ch%d T SNR %.1f dB' % (ch, 10 * math.log10(sig / max(err, 1e-9))))
