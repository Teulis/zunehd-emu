/*
 * Piecewise alignment of an emulator recording against a reference decode (both s16 stereo).
 * pcmalign <emu.wav|raw> <ref.raw> [window=4096] [hop=22050] [search=30000]
 * For each window of the emulator's left channel, finds the reference offset with the highest
 * normalized correlation and prints it, plus gain and SNR at that offset (both channels) - shows
 * dropped / repeated buffers as offset jumps.
 * Build: gcc -O2 -o tools/pcmalign.exe tools/pcmalign.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static short *load(const char *fn, long *frames)
{
    FILE *f = fopen(fn, "rb");
    long n, off = 0;
    unsigned char *d;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) {
        return NULL;
    }
    fclose(f);
    if (n > 44 && !memcmp(d, "RIFF", 4)) {
        for (off = 12; off + 8 < n; ) {
            long len = d[off + 4] | d[off + 5] << 8 | d[off + 6] << 16 | (long)d[off + 7] << 24;
            if (!memcmp(d + off, "data", 4)) {
                off += 8;
                break;
            }
            off += 8 + len;
        }
    }
    *frames = (n - off) / 4;
    return (short *)(d + off);
}

int main(int argc, char **argv)
{
    long ne, nr;
    short *e, *r;
    int win = argc > 3 ? atoi(argv[3]) : 4096;
    long hop = argc > 4 ? atol(argv[4]) : 22050, search = argc > 5 ? atol(argv[5]) : 30000;
    long prev = 0;

    if (argc < 3 || !(e = load(argv[1], &ne)) || !(r = load(argv[2], &nr))) {
        fprintf(stderr, "usage: pcmalign emu ref [win hop search]\n");
        return 1;
    }
    printf("emu %ld frames, ref %ld frames\n", ne, nr);
    for (long p = 0; p + win <= ne; p += hop) {
        double ee = 0, best = -2;
        long bo = 0;
        for (int i = 0; i < win; i++) {
            ee += (double)e[2 * (p + i)] * e[2 * (p + i)];
        }
        if (ee < (double)win * 100 * 100) {
            printf("%8ld (%.2f s): quiet\n", p, p / 44100.0);
            continue;
        }
        for (long o = p + prev - search; o <= p + prev + search; o++) {
            double xy = 0, rr = 0;
            if (o < 0 || o + win > nr) {
                continue;
            }
            for (int i = 0; i < win; i += 2) {
                double a = e[2 * (p + i)], b = r[2 * (o + i)];
                xy += a * b;
                rr += b * b;
            }
            if (rr > 0) {
                double c = xy / sqrt(ee / 2 * rr);
                if (c > best) {
                    best = c;
                    bo = o;
                }
            }
        }
        {
            double snr[2], g[2];
            for (int ch = 0; ch < 2; ch++) {
                double xy = 0, rr = 0, err = 0;
                for (int i = 0; i < win; i++) {
                    double a = e[2 * (p + i) + ch], b = r[2 * (bo + i) + ch];
                    xy += a * b;
                    rr += b * b;
                }
                g[ch] = rr > 0 ? xy / rr : 0;
                for (int i = 0; i < win; i++) {
                    double a = e[2 * (p + i) + ch], b = g[ch] * r[2 * (bo + i) + ch];
                    err += (a - b) * (a - b);
                }
                snr[ch] = 10 * log10((g[ch] * g[ch] * rr + 1e-9) / (err + 1e-9));
            }
            printf("%8ld (%.2f s): ref %8ld (offset %+ld) corr %.4f gain %.3f/%.3f SNR %.1f/%.1f dB\n",
                   p, p / 44100.0, bo, bo - p, best, g[0], g[1], snr[0], snr[1]);
        }
        prev = bo - p;
    }
    return 0;
}
