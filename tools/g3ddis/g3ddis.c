/*
 * g3ddis: replay a zunehd 3D method trace (ZUNEHD_GR3D_TRACE) through grate's
 * AR20 disassembler and print per-draw state.
 *
 *   g3ddis trace.bin [--first N] [--count N] [--regs-out dir]
 *
 * Trace format: records of 3 LE u32 {class | ch << 16, offset, value}.
 * Uses grate (MIT): libgrate/{vertex,fragment,linker}_disasm.c, libwrap/disasm.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disasm.h"
#include "utils.h"

/* libwrap glue normally provided by the Linux-side wrapper */
bool libwrap_verbose = true;
enum chip_id tegra_chip_id(void) { return TEGRA20; }

static uint32_t regs[0x1000];
static bool written[0x1000];

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static void dump_state(unsigned draw, const char *regs_out)
{
    uint32_t inout = regs[0x120];
    uint32_t prm = regs[0x122], dp = regs[0x123];
    static const char *prims[8] = { "points", "lines", "line_loop", "line_strip",
                                    "triangles", "tri_strip", "tri_fan", "?7" };

    printf("##### draw %u: %s, %u verts/indices from %u, index mode %u, index ptr 0x%08x\n",
           draw, prims[(prm >> 24) & 7], (dp >> 20) + 1, dp & 0xfffff,
           (prm >> 28) & 3, regs[0x121]);
    printf("  vp in mask 0x%04x out mask 0x%04x\n", inout >> 16, inout & 0xffff);
    for (int i = 0; i < 16; i++) {
        if (inout & (1u << (16 + i))) {
            uint32_t m = regs[0x101 + 2 * i];
            printf("  attr%-2d ptr 0x%08x type %u size %u stride %u\n", i,
                   regs[0x100 + 2 * i], m & 0xf, (m >> 4) & 0xf, m >> 8);
        }
    }
    printf("  viewport bias %g %g %g scale %g %g %g; scissor h 0x%08x v 0x%08x\n",
           f32(regs[0x352]), f32(regs[0x353]), f32(regs[0x354]),
           f32(regs[0x355]), f32(regs[0x356]), f32(regs[0x357]),
           regs[0x350], regs[0x351]);
    for (int i = 0; i < 16; i++) {
        if (written[0xe00 + i] && regs[0xe00 + i]) {
            uint32_t p = regs[0xe10 + i];
            printf("  rt%-2d ptr 0x%08x params 0x%08x (fmt %u pitch %u%s)\n", i,
                   regs[0xe00 + i], p, (p >> 2) & 0x1f, (p >> 8) & 0x1ffff,
                   p & 0x04000000 ? " tiled" : "");
        }
    }
    for (int i = 0; i < 16; i++) {
        if (written[0x710 + i] && regs[0x710 + i]) {
            uint32_t d1 = regs[0x720 + 2 * i], d2 = regs[0x721 + 2 * i];
            printf("  tex%-2d ptr 0x%08x desc1 0x%08x (fmt %u) desc2 0x%08x (%ux%u)\n",
                   i, regs[0x710 + i], d1, (d1 >> 8) & 0x1f, d2,
                   (d2 >> 20) & 0xfff, (d2 >> 8) & 0xfff);
        }
    }
    printf("  fp consts:");
    for (int i = 0; i < 32; i++) {
        if (written[0x820 + i]) {
            printf(" [%d]=%08x", i, regs[0x820 + i]);
        }
    }
    printf("\n  other written regs:");
    for (int i = 0; i < 0x1000; i++) {
        bool skip = (i >= 0x100 && i < 0x124) || (i >= 0x200 && i < 0x210) ||
                    (i >= 0x300 && i < 0x340) || (i >= 0x350 && i < 0x358) ||
                    (i >= 0x700 && i < 0x740) || (i >= 0x800 && i < 0x840) ||
                    (i >= 0xe00 && i < 0xe20);
        if (written[i] && !skip) {
            printf(" %03x=%08x", i, regs[i]);
        }
    }
    printf("\n");
    if (regs_out) {
        char path[512];
        snprintf(path, sizeof(path), "%s/draw%05u.regs", regs_out, draw);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(regs, sizeof(regs), 1, f);
            fclose(f);
        }
    }
}

int main(int argc, char **argv)
{
    unsigned first = 0, count = ~0u, draw = 0;
    const char *regs_out = NULL;
    static struct disasm_state d;
    uint32_t rec[3];
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: %s trace.bin [--first N] [--count N] [--regs-out dir]\n", argv[0]);
        return 1;
    }
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--first")) first = strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--count")) count = strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--regs-out")) regs_out = argv[i + 1];
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    disasm_reset(&d);
    while (fread(rec, sizeof(rec), 1, f) == 1) {
        uint32_t cls = rec[0] & 0xffff, off = rec[1] & 0xfff, val = rec[2];
        bool show = draw >= first && draw - first < count;

        if (cls != 0x60) {
            continue;
        }
        regs[off] = val;
        written[off] = true;
        if (off == 0x123) {
            if (show) {
                dump_state(draw, regs_out);
                libwrap_verbose = true;
                disasm_write_reg(&d, cls, off, val);    /* prints the programs */
            }
            draw++;
            continue;
        }
        libwrap_verbose = true;                         /* collect programs always */
        disasm_write_reg(&d, cls, off, val);
    }
    fclose(f);
    fprintf(stderr, "%u draws in trace\n", draw);
    return 0;
}
