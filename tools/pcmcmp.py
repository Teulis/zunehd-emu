"""Compare emulator audio (QEMU -audiodev wav output) with a reference decode (tools/mp3pcm.exe).

Usage: py -3 -I tools/pcmcmp.py out.wav ref.raw
Both 44.1 kHz s16 stereo. Prints per-second peaks of the emulator output, finds the alignment of
the first loud stretch against the reference (cross-correlation of the left channel) and reports
gain ratio and SNR over the aligned overlap.
"""
import array
import math
import sys


def load_wav(fn):
    d = open(fn, "rb").read()
    i = d.find(b"data")
    a = array.array("h")
    a.frombytes(d[i + 8:][: (len(d) - i - 8) // 2 * 2])
    return a


def load_raw(fn):
    a = array.array("h")
    a.frombytes(open(fn, "rb").read())
    return a


def main():
    out = load_wav(sys.argv[1])
    ref = load_raw(sys.argv[2])
    L = out[0::2]
    R = ref[0::2]
    sr = 44100
    print("emulator: %d frames (%.1f s), ref %d frames" % (len(L), len(L) / sr, len(R)))
    print("peaks per second:", [max(abs(x) for x in L[i:i + sr]) for i in range(0, len(L), sr)])
    # first window with signal
    start = next((i for i in range(0, len(L) - 4096, 1024)
                  if max(abs(x) for x in L[i:i + 1024]) > 200), None)
    if start is None:
        print("no signal above 200 in emulator output")
        return
    # coarse: block-RMS envelopes (256 frames), normalised correlation over ~4 s
    B = 256

    def env(x, a, nb):
        return [math.sqrt(sum(v * v for v in x[a + k * B:a + (k + 1) * B]) / B) for k in range(nb)]

    nb = min(700, (len(L) - start) // B - 1)
    ee = env(L, start, nb)
    renv = env(R, 0, len(R) // B)
    mean_e = sum(ee) / nb
    ee = [v - mean_e for v in ee]
    cb = (-2, 0)
    for k in range(0, len(renv) - nb):
        seg = renv[k:k + nb]
        m = sum(seg) / nb
        num = sum(a * (b - m) for a, b in zip(ee, seg))
        den = math.sqrt(sum((b - m) ** 2 for b in seg) * sum(a * a for a in ee)) or 1
        if num / den > cb[0]:
            cb = (num / den, k)
    print("envelope match: block %d (corr %.3f)" % (cb[1], cb[0]))
    win = L[start:start + 4096]
    best = (0, None)
    en_w = math.sqrt(sum(x * x for x in win)) or 1
    for lag in range(max(0, cb[1] * B - 2 * B), min(len(R) - 4096, cb[1] * B + 2 * B)):
        seg = R[lag:lag + 4096]
        c = sum(a * b for a, b in zip(win, seg))
        en = math.sqrt(sum(x * x for x in seg)) or 1
        c /= en_w * en
        if c > best[0]:
            best = (c, lag)
    c, lag = best
    print("emulator frame %d matches reference frame %d (corr %.4f)" % (start, lag, c))
    # refine lag around best
    off = lag - start
    n = min(len(L) - start, len(R) - lag)
    e = L[start:start + n]
    r = R[lag:lag + n]
    num = sum(a * b for a, b in zip(e, r))
    den = sum(b * b for b in r) or 1
    g = num / den
    err = sum((a - g * b) ** 2 for a, b in zip(e, r))
    sig = sum((g * b) ** 2 for b in r) or 1
    print("overlap %d frames, gain emu/ref %.4f, SNR %.1f dB (reference offset %d)" % (
        n, g, 10 * math.log10(sig / (err or 1e-9)), off))


if __name__ == "__main__":
    main()
