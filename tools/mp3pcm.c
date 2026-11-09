/*
 * mp3pcm: decode an MP3 to raw interleaved s16le PCM with minimp3 (CC0) - reference for
 * checking the emulator's audio output (tools/pcmcmp.py).
 * Build (MSYS2 UCRT64): gcc -O0 -I ../ext/minimp3 -o mp3pcm.exe mp3pcm.c -lm
 * Usage: mp3pcm in.mp3 out.raw
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mp3pcm in.mp3 out.raw\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) {
        return 1;
    }
    fclose(f);
    FILE *o = fopen(argv[2], "wb");
    static mp3dec_t dec;
    mp3dec_init(&dec);
    long off = 0, total = 0;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    while (off < n) {
        mp3dec_frame_info_t info;
        int s = mp3dec_decode_frame(&dec, d + off, (int)(n - off), pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (s) {
            fwrite(pcm, 2 * info.channels, s, o);
            total += s;
        }
    }
    fclose(o);
    fprintf(stderr, "%ld frames of PCM\n", total);
    return 0;
}
