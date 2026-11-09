/*
 * Offline harness for the AVP audio vector engine interpreter (qemu/hw/arm/zunehd-ape.h).
 *
 * Input: kick captures written by QEMU with ZUNEHD_APEDUMP=<dir> (k00001.bin ...: 16-byte header
 * {'APEK', program address, 0x6000E070 value, kick number} + 256 KiB IRAM snapshot taken at the kick).
 *
 *   apesim run <dir> <first> <last> <out.raw> [-t trace.txt] [-s snapdir]
 *     Runs kicks first..last in order as one chain: before each kick the captured IRAM is used,
 *     except words the engine wrote earlier that the AVP CPU did not change between the two
 *     captures (engine state: overlap buffers, V ring). Appends the 576 staged PCM words
 *     (0x400101a4, L = low half) of each kick to out.raw (s16 stereo). -s writes the IRAM after
 *     each kick to snapdir/aNNNNN.bin.
 *   apesim one <capture> <out_iram.bin> [-t trace.txt]
 *     Runs one kick on its own snapshot, writes the resulting IRAM.
 * Build (MSYS2 UCRT64): gcc -O2 -o tools/apesim.exe tools/apesim.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../qemu/hw/arm/zunehd-ape.h"

#define NW APE_IRAM_WORDS
#define STAGE 0x400101a4u

static int load(const char *fn, uint32_t *hdr, uint32_t *ir)
{
    FILE *f = fopen(fn, "rb");
    if (!f) {
        return -1;
    }
    if (fread(hdr, 4, 4, f) != 4 || fread(ir, 4, NW, f) != NW) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void save(const char *fn, const uint32_t *ir)
{
    FILE *f = fopen(fn, "wb");
    if (f) {
        fwrite(ir, 4, NW, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    static uint32_t cap[NW], prev[NW], mine[NW], work[NW];
    static uint8_t dirty[NW];
    uint32_t hdr[4];
    const char *trace = NULL, *snap = NULL;
    int maxinsn = 0;
    ApeCore c;

    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-t")) {
            trace = argv[i + 1];
        } else if (!strcmp(argv[i], "-n")) {
            maxinsn = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "-s")) {
            snap = argv[i + 1];
        }
    }
    memset(&c, 0, sizeof(c));
    c.iram = work;
    c.dirty = dirty;
    if (trace) {
        c.trace = fopen(trace, "w");
    }
    if (argc >= 4 && !strcmp(argv[1], "one")) {
        if (load(argv[2], hdr, work)) {
            fprintf(stderr, "cannot load %s\n", argv[2]);
            return 1;
        }
        c.maxinsn = maxinsn;
        ape_run(&c, hdr[1], (hdr[2] & 0xffff) + 1);
        save(argv[3], work);
        printf("insns %u bad %u\n", c.ninsn, c.nbad);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "run")) {
        int first = atoi(argv[3]), last = atoi(argv[4]);
        FILE *out = fopen(argv[5], "wb");
        int have_prev = 0;
        unsigned kept_total = 0;
        for (int k = first; k <= last; k++) {
            char fn[1024];
            unsigned kept = 0;
            snprintf(fn, sizeof(fn), "%s/k%05d.bin", argv[2], k);
            if (load(fn, hdr, cap)) {
                fprintf(stderr, "cannot load %s\n", fn);
                break;
            }
            for (int i = 0; i < NW; i++) {
                work[i] = cap[i];
                if (have_prev && dirty[i] && cap[i] == prev[i]) {
                    work[i] = mine[i];
                    kept++;
                }
            }
            kept_total += kept;
            memcpy(prev, cap, sizeof(cap));
            memset(dirty, 0, sizeof(dirty));
            if (c.trace) {
                fprintf(c.trace, "=== kick %d prog %08x ctl %08x\n", k, hdr[1], hdr[2]);
            }
            c.nbad = 0;
            ape_run(&c, hdr[1], (hdr[2] & 0xffff) + 1);
            memcpy(mine, work, sizeof(work));
            have_prev = 1;
            fwrite(&work[(STAGE - APE_IRAM_BASE) / 4], 4, 576, out);
            if (snap) {
                snprintf(fn, sizeof(fn), "%s/a%05d.bin", snap, k);
                save(fn, work);
            }
            if (c.nbad) {
                fprintf(stderr, "kick %d: %u bad\n", k, c.nbad);
            }
        }
        fclose(out);
        printf("insns %u, state words kept %u\n", c.ninsn, kept_total);
        return 0;
    }
    fprintf(stderr, "usage: see source\n");
    return 1;
}
