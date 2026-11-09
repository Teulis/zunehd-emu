# Linear-algebra check of the long-block IMDCT section of the program (one subband).
import math, sys
SUB = sys.argv[1] if len(sys.argv) > 1 else 'sub'
SC = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5   # x0,x1 scale (shift 1)
def unit(k):
    v = [0.0] * 18; v[k] = 1.0; return v
x = [unit(k) for k in range(18)]
def op(a, b):
    return [ai - bi for ai, bi in zip(a, b)] if SUB == 'sub' else [ai + bi for ai, bi in zip(a, b)]
# x16 = op(x16,x17); x15=op(x15,x16) ... x0 = op(x0,x1)
for i in range(16, -1, -1):
    x[i] = op(x[i], x[i + 1])
for i in range(15, 0, -2):
    x[i] = op(x[i], x[i + 2])
x[0] = [v * SC for v in x[0]]; x[1] = [v * SC for v in x[1]]
cosr = [[math.cos(math.radians((2 * r + 1) * k * 10)) for k in range(9)] for r in range(9)]
E = [[sum(cosr[r][k] * x[2 * k][j] for k in range(9)) for j in range(18)] for r in range(9)]
O = [[sum(cosr[r][k] * x[2 * k + 1][j] for k in range(9)) for j in range(18)] for r in range(9)]
tw = [math.cos(math.radians((2 * r + 1) * 5)) for r in range(9)]
O = [[v * tw[r] for v in O[r]] for r in range(9)]
# reference IMDCT z[i] = sum_k X[k] cos(pi/72 (2i+1+18)(2k+1))
Z = [[math.cos(math.pi / 72 * (2 * i + 19) * (2 * k + 1)) for k in range(18)] for i in range(36)]
# try to express each z[i] as a*E[r] + b*O[r] (least squares over r)
for i in range(36):
    best = None
    for r in range(9):
        for s in range(9):
            # solve 2x2 least squares
            e, o, z = E[r], O[s], Z[i]
            ee = sum(t * t for t in e); oo = sum(t * t for t in o); eo = sum(p * q for p, q in zip(e, o))
            ez = sum(p * q for p, q in zip(e, z)); oz = sum(p * q for p, q in zip(o, z))
            det = ee * oo - eo * eo
            if abs(det) < 1e-12: continue
            a = (ez * oo - oz * eo) / det; b = (oz * ee - ez * eo) / det
            res = sum((a * p + b * q - t) ** 2 for p, q, t in zip(e, o, z))
            if best is None or res < best[0]:
                best = (res, r, s, a, b)
    print(i, 'res %.2e E%d O%d a=%+.6f b=%+.6f' % best)
