# compare DCT32 output b[t][j] (ape buffer) with sum_k S[t][k] cos((2k+1) m pi/64) for the best m
import struct, sys, math
irA = open(sys.argv[1], 'rb').read()   # IRAM before the DCT (T valid)
irB = open(sys.argv[2], 'rb').read()   # IRAM after
TA = int(sys.argv[3], 0); BA = int(sys.argv[4], 0)
def w(ir, a, n):
    return list(struct.unpack_from('<%di' % n, ir, a - 0x40000000))
S = w(irA, TA, 576); B = w(irB, BA, 576)
C = [[math.cos((2 * k + 1) * m * math.pi / 64) for k in range(32)] for m in range(32)]
D = [[sum(S[t * 32 + k] * C[m][k] for k in range(32)) for m in range(32)] for t in range(18)]
for j in range(32):
    best = None
    for m in range(32):
        num = sum(B[t * 32 + j] * D[t][m] for t in range(18)); den = sum(D[t][m] ** 2 for t in range(18))
        if den == 0: continue
        a = num / den
        res = sum((B[t * 32 + j] - a * D[t][m]) ** 2 for t in range(18))
        sig = sum(B[t * 32 + j] ** 2 for t in range(18))
        snr = 10 * math.log10(max(sig, 1e-9) / max(res, 1e-9))
        if best is None or snr > best[0]:
            best = (snr, m, a)
    print('j %2d: m %2d scale %+.4f  %.1f dB' % (j, best[1], best[2], best[0]))
