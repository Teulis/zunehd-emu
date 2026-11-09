"""List the segments of an emulator recording that match the reference decode (within +-1 LSB).
py -3 -I tools/pcmsegs.py emu.wav ref.raw [maxsegs]
Prints emu start, ref start, length (frames) per matching run and the gap/jump to the next one,
to show dropped / repeated / inserted audio."""
import array
import sys


def load(fn):
    d = open(fn, 'rb').read()
    if d[:4] == b'RIFF':
        d = d[d.find(b'data') + 8:]
    a = array.array('i')
    a.frombytes(d[:len(d) // 4 * 4])
    return a


def same(x, y):
    lx = (x & 0xffff) - ((x & 0x8000) << 1); ly = (y & 0xffff) - ((y & 0x8000) << 1)
    return abs(lx - ly) <= 1 and abs((x >> 16) - (y >> 16)) <= 1


def main():
    e = load(sys.argv[1]); r = load(sys.argv[2])
    maxsegs = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    p = 0; q = None; segs = 0
    # locate by brute force around the expected position
    def find(p, guess):
        for d in range(0, 200000):
            for qq in (guess + d, guess - d):
                if 0 <= qq < len(r) - 32 and all(same(e[p + i], r[qq + i]) for i in range(32)):
                    return qq
        return None
    guess = 0
    total_match = 0
    while p < len(e) - 32 and segs < maxsegs:
        if e[p] == 0 and all(v == 0 for v in e[p:p + 32]):
            p0 = p
            while p < len(e) and e[p] == 0:
                p += 1
            print('emu %8d: %d frames of silence' % (p0, p - p0))
            continue
        q = find(p, guess)
        if q is None:
            print('emu %8d: no match' % p)
            p += 1024
            continue
        p0, q0 = p, q
        while p < len(e) and q < len(r) and same(e[p], r[q]):
            p += 1; q += 1
        segs += 1
        total_match += p - p0
        print('emu %8d -> ref %8d len %6d (ref jump from prev end: %+d)' % (p0, q0, p - p0, q0 - guess))
        guess = q
    print('matched %d of %d frames' % (total_match, len(e)))


if __name__ == '__main__':
    main()
