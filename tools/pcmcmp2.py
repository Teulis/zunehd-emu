"""Compare a raw s16 stereo stream with the reference at small offsets.
py -3 -I tools/pcmcmp2.py sim.raw ref.raw [maxoff=3000] [skip=0]
Searches the frame offset (ref = sim shifted by off) with the best correlation of the left
channel over the sim's length, then reports gain and SNR per channel."""
import array
import math
import sys


def load(fn):
    a = array.array('h')
    d = open(fn, 'rb').read()
    if d[:4] == b'RIFF':
        d = d[d.find(b'data') + 8:]
    a.frombytes(d[:len(d) // 4 * 4])
    return a


def main():
    sim = load(sys.argv[1]); ref = load(sys.argv[2])
    maxoff = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    sl = sim[0::2][skip:]; sr = sim[1::2][skip:]
    rl = ref[0::2]; rr = ref[1::2]
    n = min(len(sl), 40000)
    best = None
    step = 1
    for off in range(-maxoff, maxoff + 1, step):
        lo = max(0, -off - skip)
        num = 0; e1 = 0; e2 = 0
        for i in range(lo, n, 7):
            j = i + skip + off
            if j < 0 or j >= len(rl):
                continue
            a = sl[i]; b = rl[j]
            num += a * b; e1 += a * a; e2 += b * b
        if e1 and e2:
            c = num / math.sqrt(e1 * e2)
            if best is None or c > best[0]:
                best = (c, off)
    c, off = best
    print('best offset %d (ref frame = sim frame + %d), corr %.4f' % (off, off + skip, c))
    for name, s, r in (('L', sim[0::2], rl), ('R', sim[1::2], rr)):
        num = e1 = e2 = 0
        pairs = [(s[i], r[i + off]) for i in range(len(s)) if 0 <= i + off < len(r)]
        for a, b in pairs:
            num += a * b; e2 += b * b; e1 += a * a
        g = num / e2 if e2 else 0
        err = sum((a - g * b) ** 2 for a, b in pairs)
        sig = sum((g * b) ** 2 for a, b in pairs)
        print('%s: gain sim/ref %.4f, SNR %.1f dB (%d frames)' % (name, g, 10 * math.log10(sig / max(err, 1e-9)) if sig else 0, len(pairs)))


if __name__ == '__main__':
    main()
