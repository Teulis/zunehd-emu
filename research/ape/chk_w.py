import struct, sys, math
cap = open(sys.argv[1], 'rb').read()
hdr = struct.unpack_from('<4I', cap); ir0 = cap[16:]
ir1 = open(sys.argv[2], 'rb').read()
ADDR = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40011808
MODE = sys.argv[4] if len(sys.argv) > 4 else 'w'     # w: W layout [sb][18]; t: T layout [18][32] x2
def w(ir, a, n):
    return list(struct.unpack_from('<%di' % n, ir, a - 0x40000000))
src = struct.unpack_from('<I', ir0, hdr[1] - 0x40000000 + 12)[0]
X = w(ir0, src, 1152)
def window(t):
    s = [math.sin(math.pi / 36 * (i + 0.5)) for i in range(36)]
    if t == 1:
        s = s[:18] + [1] * 6 + [math.sin(math.pi / 12 * (i - 18 + 0.5)) for i in range(24, 30)] + [0] * 6
    if t == 3:
        s = [0] * 6 + [math.sin(math.pi / 12 * (i - 6 + 0.5)) for i in range(6, 12)] + [1] * 6 + s[18:]
    return s
for ch in range(2):
    ovl0 = w(ir0, 0x40012acc + 0x900 * ch, 576)
    T = w(ir1, ADDR + 0x900 * ch, 576)
    res = []
    for bt in (0, 1, 3):
        win = window(bt)
        err = sig = 0
        for sb in range(32):
            x = X[576 * ch + sb * 18: 576 * ch + (sb + 1) * 18]
            z = [sum(x[k] * math.cos(math.pi / 72 * (2 * i + 19) * (2 * k + 1)) for k in range(18)) for i in range(18)]
            for i in range(18):
                ref = win[i] * z[i] / 4 + ovl0[sb * 18 + i]
                if sb & 1 and i & 1:
                    ref = -ref
                if MODE == 't':
                    got = T[i * 32 + sb] / 2
                else:
                    got = T[sb * 18 + i]
                err += (got - ref) ** 2; sig += ref ** 2
        res.append('type%d %.1f dB' % (bt, 10 * math.log10(sig / max(err, 1e-9))))
    print('ch%d' % ch, res)
