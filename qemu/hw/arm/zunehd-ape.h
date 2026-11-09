/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD AVP audio vector engine (APE) - interpreter core, shared by
 * zunehd-ape.c (QEMU) and tools/apesim.c (offline harness). No QEMU
 * dependencies: operates on a little-endian copy of IRAM (0x40000000,
 * 256 KiB). See zunehd-ape.c for the documentation of the format.
 */
#ifndef HW_ARM_ZUNEHD_APE_H
#define HW_ARM_ZUNEHD_APE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APE_IRAM_BASE   0x40000000u
#define APE_IRAM_WORDS  (0x40000 / 4)

typedef struct ApeSlot {
    uint32_t addr, desc;
} ApeSlot;

typedef struct ApeCore {
    uint32_t *iram;             /* APE_IRAM_WORDS words */
    uint8_t *dirty;             /* optional: per word, set on write */
    ApeSlot s[4];
    uint32_t L[3], P[2], SH;
    unsigned ninsn, nbad;
    unsigned maxinsn;           /* debug: stop after this many (0 = all) */
    FILE *trace;                /* optional instruction trace */
} ApeCore;

/* One operand stream of an instruction (a slot's address generator). */
typedef struct ApeStream {
    uint32_t base;              /* byte address */
    int32_t step;               /* inner step (words) */
    uint32_t inner;             /* inner length */
    int32_t outer;              /* outer step (words) */
    uint32_t count;             /* total elements (wraps) */
    int bounce;                 /* inner order 0, n-1, 1, n-2, ... */
    uint32_t circ;              /* circular window size in bytes (0 = none) */
    uint32_t pos;
} ApeStream;

static inline uint32_t ape_pfield(const ApeCore *c, int slot)
{
    uint32_t p = c->P[slot >> 1];
    return (slot & 1) ? (p & 0xffff) : (p >> 16);
}

static inline void ape_stream_init(const ApeCore *c, ApeStream *st, int slot)
{
    uint32_t d = c->s[slot].desc, f = d >> 28;
    uint32_t stride = (d >> 16) & 0xfff;

    memset(st, 0, sizeof(*st));
    st->base = c->s[slot].addr;
    st->count = (d & 0xfff) + 1;
    if (f & 4) {                /* 2D: inner run of +-1, outer stride */
        uint32_t pf = ape_pfield(c, slot);
        st->inner = (pf & 0xfff) + 1;
        st->step = (f & 1) ? -1 : 1;
        st->outer = stride;
        st->bounce = ((pf >> 12) & 3) == 3;
    } else {
        st->inner = st->count;
        st->step = (f & 1) ? -(int32_t)stride : (int32_t)stride;
        st->outer = 0;
    }
    /* L0..L2: per-slot override "inner run of n elements at the descriptor's step, then
       outer step"; flags 9 = circular within an aligned window of n * stride words
       (polyphase V ring), b = plain (short-block IMDCT scatter/gather) */
    if (slot < 3 && (c->L[slot] >> 28) & 1) {
        uint32_t lf = c->L[slot] >> 28, n = ((c->L[slot] >> 16) & 0xfff) + 1;
        st->inner = n;
        st->outer = (int16_t)(c->L[slot] & 0xffff);
        st->bounce = 0;
        if (f & 4) {
            st->step = (f & 1) ? -1 : 1;
        }
        if (!(lf & 2)) {
            st->circ = n * stride * 4;
        }
    }
}

static inline uint32_t ape_stream_addr(ApeStream *st)
{
    uint32_t e = st->pos % st->count;
    uint32_t q = e / st->inner, r = e % st->inner;
    int32_t off;
    uint32_t a;

    st->pos++;
    if (st->bounce) {
        r = (r & 1) ? st->inner - 1 - (r >> 1) : (r >> 1);
    }
    off = (int32_t)q * st->outer + (int32_t)r * st->step;
    a = st->base + 4 * off;
    if (st->circ) {
        a = (st->base & ~(st->circ - 1)) | (a & (st->circ - 1));
    }
    return a;
}

static inline int32_t ape_rd(ApeCore *c, uint32_t a)
{
    uint32_t i = (a - APE_IRAM_BASE) >> 2;
    if (i >= APE_IRAM_WORDS) {
        c->nbad++;
        return 0;
    }
    return (int32_t)c->iram[i];
}

/* mode: 0 = word, 2 = low half, 3 = high half */
static inline void ape_wr(ApeCore *c, uint32_t a, int32_t v, int mode)
{
    uint32_t i = (a - APE_IRAM_BASE) >> 2;
    if (i >= APE_IRAM_WORDS) {
        c->nbad++;
        return;
    }
    if (mode == 2) {
        c->iram[i] = (c->iram[i] & 0xffff0000u) | ((uint32_t)v & 0xffff);
    } else if (mode == 3) {
        c->iram[i] = (c->iram[i] & 0xffffu) | ((uint32_t)v << 16);
    } else {
        c->iram[i] = (uint32_t)v;
    }
    if (c->dirty) {
        c->dirty[i] = 1;
    }
}

/* (v >> sh) with optional rounding, saturated to (bits) signed bits */
static inline int32_t ape_out(int64_t v, int sh, int bits, int rnd)
{
    int64_t hi, lo;

    if (sh > 0) {
        if (rnd) {
            v += (int64_t)1 << (sh - 1);
        }
        v >>= sh;
    }
    hi = ((int64_t)1 << bits) - 1;
    lo = -hi - 1;
    if (v > hi) {
        v = hi;
    } else if (v < lo) {
        v = lo;
    }
    return (int32_t)v;
}

#ifndef APE_ROUND
#define APE_ROUND 1
#endif

static void ape_exec(ApeCore *c, uint32_t pc, uint32_t hdr, uint32_t ctl)
{
    int cls = (ctl >> 24) & 0xf;
    int op = (ctl >> 12) & 0xf;
    int wmode = (ctl >> 20) & 0xf;
    int sh = (c->SH >> 8) & 0xff;
    int bits = (c->SH & 0xff) > 31 ? 31 : (c->SH & 0xff);
    int rnd = APE_ROUND && ((ctl >> 8) & 2);
    ApeStream s0, s1, s2, s3;

    (void)hdr;
    (void)pc;
    ape_stream_init(c, &s0, 0);
    ape_stream_init(c, &s1, 1);
    ape_stream_init(c, &s2, 2);
    ape_stream_init(c, &s3, 3);
    if (cls == 2) {
        /* element-wise: op 0 mul -> s2, 1 add -> s2, 2 sub -> s3, 3 add -> s2 & sub -> s3 */
        uint32_t n = (op & 2) && !(op & 1) ? s3.count : s2.count;
        for (uint32_t i = 0; i < n; i++) {
            int64_t a = ape_rd(c, ape_stream_addr(&s0));
            int64_t b = ape_rd(c, ape_stream_addr(&s1));
            switch (op) {
            case 0:
                ape_wr(c, ape_stream_addr(&s2), ape_out(a * b, sh, bits, rnd), wmode);
                break;
            case 1:
                ape_wr(c, ape_stream_addr(&s2), ape_out(a + b, sh, bits, rnd), wmode);
                break;
            case 2:
                ape_wr(c, ape_stream_addr(&s3), ape_out(a - b, sh, bits, rnd), wmode);
                break;
            case 3:
                ape_wr(c, ape_stream_addr(&s2), ape_out(a + b, sh, bits, rnd), wmode);
                ape_wr(c, ape_stream_addr(&s3), ape_out(a - b, sh, bits, rnd), wmode);
                break;
            default:
                c->nbad++;
                return;
            }
        }
    } else if (cls == 1) {
        /* multiply-accumulate: groups of nd data (s1), nc coefficients (s0) -> nc/nd outputs (s2) */
        int nd = ((ctl >> 4) & 0xf) + 1, nc = (ctl & 0xf) + 1, g = nc / nd;
        uint32_t nout = s2.count;
        int32_t d[16];
        for (uint32_t o = 0; o < nout; o += g) {
            for (int t = 0; t < nd; t++) {
                d[t] = ape_rd(c, ape_stream_addr(&s1));
            }
            for (int j = 0; j < g; j++) {
                int64_t acc = 0;
                for (int t = 0; t < nd; t++) {
                    acc += (int64_t)ape_rd(c, ape_stream_addr(&s0)) * d[t];
                }
                ape_wr(c, ape_stream_addr(&s2), ape_out(acc, sh, bits, rnd), wmode);
            }
        }
    } else if (cls == 7) {
        /* DCT32 recombination stage (semantics under test): reverse scan of each run of n
           (s1 -> s2): out[j] = in[j] + in[j+1] for the odd half, else copy */
        uint32_t n = s2.count, m = s1.inner, h = m / 2;
        int32_t in[64];
        uint32_t ad[64];
        if (m < 2 || m > 64) {
            c->nbad++;
            return;
        }
        for (uint32_t o = 0; o < n; o += m) {
            /* both streams run backwards through the group: index m-1-j */
            for (uint32_t j = 0; j < m; j++) {
                in[m - 1 - j] = ape_rd(c, ape_stream_addr(&s1));
                ad[m - 1 - j] = ape_stream_addr(&s2);
            }
            for (uint32_t k = 0; k < h; k++) {
                int64_t odd = (int64_t)in[h + k] + (k + 1 < h ? in[h + k + 1] : 0);
                ape_wr(c, ad[2 * k], ape_out(in[k], sh, bits, rnd), wmode);
                ape_wr(c, ad[2 * k + 1], ape_out(odd, sh, bits, rnd), wmode);
            }
        }
    } else {
        c->nbad++;
    }
}

/* Run a program of nwords words at byte address pc. */
static void ape_run(ApeCore *c, uint32_t pc, uint32_t nwords)
{
    uint32_t end = pc + 4 * nwords;

    while (pc < end && !(c->maxinsn && c->ninsn >= c->maxinsn)) {
        uint32_t hdr = (uint32_t)ape_rd(c, pc), p = pc + 4, ctl;
        uint32_t mm = (hdr >> 8) & 0xff, ff = (hdr >> 16) & 0xff, ll = hdr & 0xff;
        uint32_t ipc = pc;

        if ((hdr >> 28) == 8 || (hdr >> 28) == 0xa || (hdr & 0xff000000u)) {
            c->nbad++;
            if (c->trace) {
                fprintf(c->trace, "%08x: bad header %08x\n", pc, hdr);
            }
            pc += 4;
            continue;
        }
        for (int i = 0; i < 3; i++) {
            if (ll & (1 << i)) {
                c->L[i] = (uint32_t)ape_rd(c, p);
                p += 4;
            }
        }
        for (int i = 0; i < 8; i++) {
            if (mm & (1 << i)) {
                uint32_t v = (uint32_t)ape_rd(c, p);
                p += 4;
                if (i & 1) {
                    c->s[i >> 1].desc = v;
                } else {
                    c->s[i >> 1].addr = v;
                }
            }
        }
        if (ff & 0x01) {
            c->P[0] = (uint32_t)ape_rd(c, p);
            p += 4;
        }
        if (ff & 0x02) {
            c->P[1] = (uint32_t)ape_rd(c, p);
            p += 4;
        }
        if (ff & 0x20) {
            c->SH = (uint32_t)ape_rd(c, p);
            p += 4;
        }
        ctl = (uint32_t)ape_rd(c, p);
        p += 4;
        if ((ctl >> 28) != 8 && (ctl >> 28) != 0xa) {
            c->nbad++;
            if (c->trace) {
                fprintf(c->trace, "%08x: bad control word %08x (hdr %08x)\n", ipc, ctl, hdr);
            }
        }
        if (c->trace) {
            fprintf(c->trace, "%08x: %08x ctl %08x L %08x %08x %08x s0 %08x/%08x s1 %08x/%08x "
                    "s2 %08x/%08x s3 %08x/%08x P %08x %08x SH %08x\n", ipc, hdr, ctl,
                    c->L[0], c->L[1], c->L[2], c->s[0].addr, c->s[0].desc, c->s[1].addr,
                    c->s[1].desc, c->s[2].addr, c->s[2].desc, c->s[3].addr, c->s[3].desc,
                    c->P[0], c->P[1], c->SH);
        }
        ape_exec(c, ipc, hdr, ctl);
        c->ninsn++;
        pc = p;
    }
}

#endif
