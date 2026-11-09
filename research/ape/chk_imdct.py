import struct, sys, math
cap = open(sys.argv[1], 'rb').read()
hdr = struct.unpack_from('<4I', cap); ir0 = cap[16:]
ir1 = open(sys.argv[2], 'rb').read()
def w(ir, a, n):
    return list(struct.unpack_from('<%di' % n, ir, a - 0x40000000))
# source spectra address = operand 3 of the first instruction
src = struct.unpack_from('<I', ir0, hdr[1] - 0x40000000 + 12)[0]
print('prog %08x src %08x' % (hdr[1], src))
X = w(ir0, src, 1152)
ovl0 = w(ir0, 0x40012acc, 576)
W = w(ir1, 0x40011808, 576)
ovl1 = w(ir1, 0x40012acc, 576)
win = [math.sin(math.pi / 36 * (i + 0.5)) for i in range(36)]
err = 0; sig = 0; err2 = 0; sig2 = 0
for sb in range(32):
    x = X[sb * 18:(sb + 1) * 18]
    z = [sum(x[k] * math.cos(math.pi / 72 * (2 * i + 19) * (2 * k + 1)) for k in range(18)) for i in range(36)]
    for i in range(18):
        ref = win[i] * z[i] / 4 + ovl0[sb * 18 + i]
        got = W[sb * 18 + i]
        err += (got - ref) ** 2; sig += ref ** 2
        ref2 = win[18 + i] * z[18 + i] / 4
        got2 = ovl1[sb * 18 + i]
        err2 += (got2 - ref2) ** 2; sig2 += ref2 ** 2
        if sb == 1 and i < 4:
            print(sb, i, ref, got, ref2, got2)
print('out SNR %.1f dB, ovl SNR %.1f dB' % (10 * math.log10(sig / max(err, 1e-9)), 10 * math.log10(sig2 / max(err2, 1e-9))))
