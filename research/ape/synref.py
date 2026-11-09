# minimp3 synthesis (scalar path, transcribed) fed with the engine's DCT32 outputs, compared with
# the engine's PCM. Usage: synref.py snapdir first last sim.raw [scale]
import struct, sys, math, array
g_win = [
    -1,26,-31,208,218,401,-519,2063,2000,4788,-5517,7134,5959,35640,-39336,74992,
    -1,24,-35,202,222,347,-581,2080,1952,4425,-5879,7640,5288,33791,-41176,74856,
    -1,21,-38,196,225,294,-645,2087,1893,4063,-6237,8092,4561,31947,-43006,74630,
    -1,19,-41,190,227,244,-711,2085,1822,3705,-6589,8492,3776,30112,-44821,74313,
    -1,17,-45,183,228,197,-779,2075,1739,3351,-6935,8840,2935,28289,-46617,73908,
    -1,16,-49,176,228,153,-848,2057,1644,3004,-7271,9139,2037,26482,-48390,73415,
    -2,14,-53,169,227,111,-919,2032,1535,2663,-7597,9389,1082,24694,-50137,72835,
    -2,13,-58,161,224,72,-991,2001,1414,2330,-7910,9592,70,22929,-51853,72169,
    -2,11,-63,154,221,36,-1064,1962,1280,2006,-8209,9750,-998,21189,-53534,71420,
    -2,10,-68,147,215,2,-1137,1919,1131,1692,-8491,9863,-2122,19478,-55178,70590,
    -3,9,-73,139,208,-29,-1210,1870,970,1388,-8755,9935,-3300,17799,-56778,69679,
    -3,8,-79,132,200,-57,-1283,1817,794,1095,-8998,9966,-4533,16155,-58333,68692,
    -4,7,-85,125,189,-83,-1356,1759,605,814,-9219,9959,-5818,14548,-59838,67629,
    -4,7,-91,117,177,-106,-1428,1698,402,545,-9416,9916,-7154,12980,-61289,66494,
    -5,6,-97,111,163,-127,-1498,1634,185,288,-9585,9838,-8540,11455,-62684,65290]

def synth_pair(pcm, idx, nch, z, zo):
    a = (z[zo + 14*64] - z[zo]) * 29
    a += (z[zo + 1*64] + z[zo + 13*64]) * 213
    a += (z[zo + 12*64] - z[zo + 2*64]) * 459
    a += (z[zo + 3*64] + z[zo + 11*64]) * 2037
    a += (z[zo + 10*64] - z[zo + 4*64]) * 5153
    a += (z[zo + 5*64] + z[zo + 9*64]) * 6574
    a += (z[zo + 8*64] - z[zo + 6*64]) * 37489
    a += z[zo + 7*64] * 75038
    pcm[idx] = a
    zo += 2
    a = z[zo + 14*64] * 104 + z[zo + 12*64] * 1567 + z[zo + 10*64] * 9727 + z[zo + 8*64] * 64019
    a += z[zo + 6*64] * -9975 + z[zo + 4*64] * -45 + z[zo + 2*64] * 146 + z[zo + 0*64] * -5
    pcm[idx + 16*nch] = a

def synth(grbuf, xo, pcm, po, nch, lins, lo):
    xl = lambda k: grbuf[xo + k]
    xr = lambda k: grbuf[xo + 576 * (nch - 1) + k]
    zl = lo + 15*64
    lins[zl + 4*15] = xl(18*16); lins[zl + 4*15 + 1] = xr(18*16)
    lins[zl + 4*15 + 2] = xl(0); lins[zl + 4*15 + 3] = xr(0)
    lins[zl + 4*31] = xl(1 + 18*16); lins[zl + 4*31 + 1] = xr(1 + 18*16)
    lins[zl + 4*31 + 2] = xl(1); lins[zl + 4*31 + 3] = xr(1)
    synth_pair(pcm, po + 1, nch, lins, lo + 4*15 + 1)
    synth_pair(pcm, po + 1 + 32*nch, nch, lins, lo + 4*15 + 64 + 1)
    synth_pair(pcm, po, nch, lins, lo + 4*15)
    synth_pair(pcm, po + 32*nch, nch, lins, lo + 4*15 + 64)
    for i in range(14, -1, -1):
        lins[zl + 4*i] = xl(18*(31 - i)); lins[zl + 4*i + 1] = xr(18*(31 - i))
        lins[zl + 4*i + 2] = xl(1 + 18*(31 - i)); lins[zl + 4*i + 3] = xr(1 + 18*(31 - i))
        lins[zl + 4*(i + 16)] = xl(1 + 18*(1 + i)); lins[zl + 4*(i + 16) + 1] = xr(1 + 18*(1 + i))
        lins[zl + 4*(i - 16) + 2] = xl(18*(1 + i)); lins[zl + 4*(i - 16) + 3] = xr(18*(1 + i))
        a = [0.0] * 4; b = [0.0] * 4; w = 0
        for k in range(8):
            w0 = g_win[(14 - i)*16 + w]; w1 = g_win[(14 - i)*16 + w + 1]; w += 2
            vz = zl + 4*i - k*64; vy = zl + 4*i - (15 - k)*64
            for j in range(4):
                bb = lins[vz + j]*w1 + lins[vy + j]*w0
                if k == 0:
                    b[j] = bb; a[j] = lins[vz + j]*w0 - lins[vy + j]*w1
                elif k & 1:
                    b[j] += bb; a[j] += lins[vy + j]*w1 - lins[vz + j]*w0
                else:
                    b[j] += bb; a[j] += lins[vz + j]*w0 - lins[vy + j]*w1
        pcm[po + (15 - i)*nch + 1] = a[1]; pcm[po + (17 + i)*nch + 1] = b[1]
        pcm[po + (15 - i)*nch] = a[0]; pcm[po + (17 + i)*nch] = b[0]
        pcm[po + (47 - i)*nch + 1] = a[3]; pcm[po + (49 + i)*nch + 1] = b[3]
        pcm[po + (47 - i)*nch] = a[2]; pcm[po + (49 + i)*nch] = b[2]

def main():
    snap, first, last, simf = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    scale = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    sim = array.array('h'); sim.frombytes(open(simf, 'rb').read())
    qmf = [0.0] * (15 * 64)
    tot_e = tot_s = 0
    ej = [[0, 0] for j in range(32)]; sj = [[0, 0] for j in range(32)]
    for k in range(first, last + 1):
        ir = open('%s/a%05d.bin' % (snap, k), 'rb').read()
        grbuf = [0.0] * 1152
        for ch in range(2):
            T = struct.unpack_from('<576i', ir, 0x141a4 + 0x900 * ch)
            for t in range(18):
                v = T[t * 32:(t + 1) * 32]
                X = [v[0]] + [v[16 - m] for m in range(1, 16)] + list(v[16:32])
                for m in range(32):
                    grbuf[576 * ch + m * 18 + t] = X[m] * scale
        lins = qmf + [0.0] * (18 * 64)
        pcm = [0.0] * 1152
        for i in range(0, 18, 2):
            synth(grbuf, i, pcm, 32 * 2 * i, 2, lins, i * 64)
        qmf = lins[18 * 64: 18 * 64 + 15 * 64]
        g = (k - first)
        s = sim[(k - first) * 1152:(k - first + 1) * 1152]
        e = sum((x - y) ** 2 for x, y in zip(s, pcm)); sg = sum(y * y for y in pcm)
        num = sum(x * y for x, y in zip(s, pcm))
        tot_e += e; tot_s += sg
        print('kick %d: ref rms %.1f sim rms %.1f gain %.4f snr %.1f dB' % (k, math.sqrt(sg / 1152),
              math.sqrt(sum(x * x for x in s) / 1152), num / sg if sg else 0, 10 * math.log10(sg / e) if e and sg else 0))
        for j in range(32):
            for n in range(0, 1152, 64):
                for c2 in range(2):
                    ej[j][c2] += (s[n + 2*j + c2] - pcm[n + 2*j + c2]) ** 2; sj[j][c2] += pcm[n + 2*j + c2] ** 2
        if k == first + 3:
            print(' ref', [round(x) for x in pcm[:16]]); print(' sim', list(s[:16]))
    for j in range(32):
        print(j, ['%.1f' % (10 * math.log10(sj[j][c] / max(ej[j][c], 1e-9))) for c in range(2)])
    return

main()
