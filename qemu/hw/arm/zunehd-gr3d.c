/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD (Tegra AP16) 3D engine: host1x class 0x60, the "AR20" design also used by
 * Tegra 2 (gr3d). Reference interpreter, CPU-side:
 *   attribute fetch -> vertex VLIW program -> linker (exports -> TRAM) -> viewport ->
 *   triangle raster with perspective-correct interpolation -> fragment pipeline
 *   (PSEQ load, MFU varyings, TEX, ALU packets, DW store) -> render target.
 *
 * Register/ISA knowledge comes from the grate project's reverse engineering
 * (github.com/grate-driver/grate, MIT): docs/{vertex,fragment}-shader-isa.md,
 * docs/shader-linking.md, docs/geometry-submission.md, include/tgr_3d.xml.h,
 * src/libgrate/fragment_asm.h. Vertex instruction word order and field layout were
 * checked against this firmware's compositor programs (tools/vpedis.py).
 *
 * Not implemented yet (logged once): VP branching/predicates/relative addressing,
 * MFU sin/cos, depth/stencil, tiled render targets, the PSEQ spill buffer (rt4;
 * not needed: registers persist for the whole program here), lines/points.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "hw/arm/zunehd-gr3d.h"

#define LOGU(...) qemu_log_mask(LOG_UNIMP, "zunehd: gr3d " __VA_ARGS__)

static struct {
    uint32_t regs[0x1000];
    uint32_t vp[1024];
    unsigned vp_ptr;
    float vc[1024];
    unsigned vc_ptr;
    uint32_t pseq[64], mfu_sched[64], mfu[128], tex[64], alu_sched[64], alu[512], dw[64];
    unsigned pseq_n, mfu_sched_n, mfu_n, tex_n, alu_sched_n, alu_n, dw_n;
    unsigned draws;
    uint64_t warned;
} g;

static void warn_once(int bit, const char *what)
{
    if (!(g.warned & (1ull << bit))) {
        g.warned |= 1ull << bit;
        LOGU("unimplemented: %s\n", what);
    }
}

/* ---------------------------------------------------------------- number formats */

static float u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static float fp20_dec(uint32_t v)
{
    int e = (v >> 13) & 63;
    uint32_t m = v & 0x1fff;
    float f;

    if (e == 0) {
        return 0.0f;
    }
    f = ldexpf(1.0f + m / 8192.0f, e - 31);
    return (v & 0x80000) ? -f : f;
}

static uint32_t fp20_enc(float f)
{
    uint32_t s = 0, m;
    int e;
    float mant;

    if (isnan(f) || f == 0.0f) {
        return 0;
    }
    if (f < 0) {
        s = 0x80000;
        f = -f;
    }
    mant = frexpf(f, &e);           /* f = mant * 2^e, mant in [0.5,1) */
    e = e - 1 + 31;                 /* 1.x * 2^(e-1) */
    if (e <= 0) {
        return s;
    }
    if (e > 63) {
        return s | (63u << 13) | 0x1fff;
    }
    m = (uint32_t)lrintf((mant * 2.0f - 1.0f) * 8192.0f);
    if (m > 0x1fff) {
        m = 0;
        if (++e > 63) {
            return s | (63u << 13) | 0x1fff;
        }
    }
    return s | ((uint32_t)e << 13) | m;
}

static float fx10_dec(uint32_t v)
{
    int32_t x = v & 0x3ff;
    if (x & 0x200) {
        x -= 0x400;
    }
    return x / 256.0f;
}

static uint32_t fx10_enc(float f)
{
    long x = lrintf(f * 256.0f);
    if (x < -512) {
        x = -512;
    }
    if (x > 511) {
        x = 511;
    }
    return (uint32_t)x & 0x3ff;
}

/* ---------------------------------------------------------------- memory helpers */

/*
 * Fast path: direct host pointer into SDRAM (RAM at guest physical 0). Every
 * texel / pixel went through address_space_read/write (a flat-view lookup
 * per access, a large part of the per-pixel cost). Reads use it anywhere in
 * SDRAM; writes only inside the GPU carveout (0x06000000..), so guest code
 * pages are never modified behind TCG's back. Nothing in the machine tracks
 * dirty RAM (the display controller re-reads the framebuffer every refresh).
 * Worker threads (see the raster pool) only use these helpers.
 */
#define GPU_CARVEOUT    0x06000000u
static uint8_t *ram_host;
static uint64_t ram_len;

static void ram_init(void)
{
    MemoryRegionSection sec = memory_region_find(get_system_memory(), 0, 1);

    if (sec.mr) {
        if (memory_region_is_ram(sec.mr) && !memory_region_is_rom(sec.mr)) {
            ram_len = memory_region_size(sec.mr) - sec.offset_within_region;
            ram_host = (uint8_t *)memory_region_get_ram_ptr(sec.mr) +
                       sec.offset_within_region;
        }
        memory_region_unref(sec.mr);
    }
    if (!ram_host) {
        ram_len = 1;                    /* no fast path; do not retry */
    }
}

static void rd(hwaddr a, void *buf, int len)
{
    if (likely(ram_host) && a + len <= ram_len) {
        memcpy(buf, ram_host + a, len);
        return;
    }
    address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void wr(hwaddr a, const void *buf, int len)
{
    if (likely(ram_host) && a >= GPU_CARVEOUT && a + len <= ram_len) {
        memcpy(ram_host + a, buf, len);
        return;
    }
    address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, buf, len);
}

/* Render-target / texture pixel <-> 4 channels in memory order (0..1). */
static bool px_load(int fmt, hwaddr a, float c[4])
{
    uint8_t b[4] = { 0 };

    switch (fmt) {
    case 13:    /* RGBA8888 (byte 0 = R) */
    case 14:    /* BGRA8888 */
        rd(a, b, 4);
        for (int i = 0; i < 4; i++) {
            c[i] = b[i] / 255.0f;
        }
        return true;
    case 6: {   /* RGB565 */
        uint16_t v;
        rd(a, b, 2);
        v = b[0] | (b[1] << 8);
        c[0] = (v & 0x1f) / 31.0f;          /* low bits first, memory order */
        c[1] = ((v >> 5) & 0x3f) / 63.0f;
        c[2] = (v >> 11) / 31.0f;
        c[3] = 1.0f;
        return true;
    }
    case 1:     /* A8 */
        rd(a, b, 1);
        c[0] = c[1] = c[2] = 0.0f;
        c[3] = b[0] / 255.0f;
        return true;
    case 2:     /* L8 */
        rd(a, b, 1);
        c[0] = c[1] = c[2] = b[0] / 255.0f;
        c[3] = 1.0f;
        return true;
    case 4:     /* LA88 */
        rd(a, b, 2);
        c[0] = c[1] = c[2] = b[0] / 255.0f;
        c[3] = b[1] / 255.0f;
        return true;
    }
    return false;
}

static int px_bytes(int fmt)
{
    switch (fmt) {
    case 13: case 14: return 4;
    case 6: case 4: case 11: case 12: return 2;
    case 1: case 2: case 3: return 1;
    }
    return 0;
}

static float clamp01(float f) { return f < 0 ? 0 : f > 1 ? 1 : f; }

static void px_store(int fmt, hwaddr a, const float c[4])
{
    uint8_t b[4];

    switch (fmt) {
    case 13:
    case 14:
        for (int i = 0; i < 4; i++) {
            b[i] = (uint8_t)lrintf(clamp01(c[i]) * 255.0f);
        }
        wr(a, b, 4);
        return;
    case 6: {
        uint16_t v = (uint16_t)(lrintf(clamp01(c[0]) * 31) |
                                (lrintf(clamp01(c[1]) * 63) << 5) |
                                (lrintf(clamp01(c[2]) * 31) << 11));
        b[0] = v;
        b[1] = v >> 8;
        wr(a, b, 2);
        return;
    }
    }
    warn_once(0, "render target format");
}

/* ---------------------------------------------------------------- vertex stage */

typedef struct { float v[4]; } V4;

static unsigned __int128 vp_insn(unsigned i)
{
    const uint32_t *w = &g.vp[i * 4];
    return ((unsigned __int128)w[0] << 96) | ((unsigned __int128)w[1] << 64) |
           ((unsigned __int128)w[2] << 32) | w[3];
}

static unsigned vbits(unsigned __int128 v, int lo, int n)
{
    return (unsigned)(v >> lo) & ((1u << n) - 1);
}

static V4 vp_src(unsigned __int128 in, int tyb, int regb, int swb, int negb, int absb,
                 const V4 *r, const V4 *a)
{
    unsigned ty = vbits(in, tyb, 2), reg = vbits(in, regb, 6), sw = vbits(in, swb, 8);
    V4 s, o;

    switch (ty) {
    case 1:
        s = r[reg & 31];
        break;
    case 3: {
        unsigned ci = vbits(in, 76, 10) & 255;
        memcpy(s.v, &g.vc[ci * 4], sizeof(s.v));
        break;
    }
    default:
        s = a[vbits(in, 72, 4)];
        break;
    }
    for (int i = 0; i < 4; i++) {
        float f = s.v[(sw >> (6 - 2 * i)) & 3];
        if (vbits(in, absb, 1)) {
            f = fabsf(f);
        }
        if (vbits(in, negb, 1)) {
            f = -f;
        }
        o.v[i] = f;
    }
    return o;
}

static void wmask(V4 *d, const V4 *s, unsigned m)
{
    for (int i = 0; i < 4; i++) {
        if (m & (8 >> i)) {
            d->v[i] = s->v[i];
        }
    }
}

/* Run the vertex program for one vertex; exports in o[16]. */
static void vp_run(const V4 *attr, V4 *o)
{
    V4 r[32] = { 0 };

    for (unsigned pc = 0; pc < 256; pc++) {
        unsigned __int128 in = vp_insn(pc);
        V4 A = vp_src(in, 55, 57, 63, 71, 117, r, attr);
        V4 B = vp_src(in, 38, 40, 46, 54, 118, r, attr);
        V4 C = vp_src(in, 21, 23, 29, 37, 119, r, attr);
        unsigned vop = vbits(in, 86, 5), sop = vbits(in, 91, 5);
        V4 vres = { { 0 } }, sres;
        float sv = C.v[0], s = 0;
        bool sat = vbits(in, 122, 1);

        if (vbits(in, 109, 1) || vbits(in, 1, 1) || vbits(in, 123, 2)) {
            warn_once(1, "VP predicates / relative addressing");
        }
        for (int i = 0; i < 4; i++) {
            float x = A.v[i], y = B.v[i], z = C.v[i], t = 0;
            switch (vop) {
            case 1: t = x; break;
            case 2: t = x * y; break;
            case 3: t = x + z; break;
            case 4: t = x * y + z; break;
            case 5: t = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2]; break;
            case 6: t = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2] + A.v[3]; break;
            case 7: t = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2] +
                        A.v[3] * B.v[3]; break;
            case 8: t = i == 0 ? 1 : i == 1 ? A.v[1] * B.v[1] : i == 2 ? B.v[2] : A.v[3];
                break;
            case 9: t = fminf(x, y); break;
            case 10: t = fmaxf(x, y); break;
            case 11: t = x < y; break;
            case 12: t = x >= y; break;
            case 14: t = x - floorf(x); break;
            case 15: t = floorf(x); break;
            case 16: t = x == y; break;
            case 17: t = 0; break;
            case 18: t = x > y; break;
            case 19: t = x <= y; break;
            case 20: t = x != y; break;
            case 21: t = 1; break;
            case 22: t = (x > 0) - (x < 0); break;
            case 0: break;
            default: warn_once(2, "VP vector opcode"); break;
            }
            vres.v[i] = sat ? clamp01(t) : t;
        }
        switch (sop) {
        case 0: break;
        case 1: s = sv; break;          /* scalar MOV moves the whole vector */
        case 2: s = 1.0f / sv; break;
        case 3: s = 1.0f / fabsf(sv);
            s = fminf(fmaxf(s, 5.42101e-20f), 1.84467e19f) * (sv < 0 ? -1 : 1); break;
        case 4: s = 1.0f / sqrtf(sv); break;
        case 13: s = log2f(sv); break;
        case 14: s = exp2f(sv); break;
        case 15: s = sinf(sv); break;
        case 16: s = cosf(sv); break;
        default: warn_once(3, "VP scalar opcode"); break;
        }
        for (int i = 0; i < 4; i++) {
            sres.v[i] = sat ? clamp01(sop == 1 ? C.v[i] : s) : (sop == 1 ? C.v[i] : s);
        }
        if (sop) {
            unsigned sd = vbits(in, 7, 6);
            if (sd < 32) {
                wmask(&r[sd], &sres, vbits(in, 17, 4));
            }
        }
        if (vop) {
            unsigned vd = vbits(in, 111, 6);
            if (vd < 32) {
                wmask(&r[vd], &vres, vbits(in, 13, 4));
            }
        }
        {
            unsigned ex = vbits(in, 2, 5);
            if (ex < 16) {
                if (vbits(in, 126, 1)) {
                    wmask(&o[ex], &vres, vbits(in, 13, 4));
                } else {
                    wmask(&o[ex], &sres, vbits(in, 17, 4));
                }
            }
        }
        if (vbits(in, 0, 1)) {
            return;
        }
    }
}

static float attr_comp(int type, hwaddr a, int i)
{
    uint8_t b[4];

    switch (type) {
    case 13: rd(a + 4 * i, b, 4); return u2f(ldl_le_p(b));
    case 12: rd(a + 4 * i, b, 4); return (int32_t)ldl_le_p(b) / 65536.0f;
    case 0: rd(a + i, b, 1); return b[0];
    case 1: rd(a + i, b, 1); return b[0] / 255.0f;
    case 2: rd(a + i, b, 1); return (int8_t)b[0];
    case 3: rd(a + i, b, 1); return fmaxf((int8_t)b[0] / 127.0f, -1.0f);
    case 4: rd(a + 2 * i, b, 2); return lduw_le_p(b);
    case 5: rd(a + 2 * i, b, 2); return lduw_le_p(b) / 65535.0f;
    case 6: rd(a + 2 * i, b, 2); return (int16_t)lduw_le_p(b);
    case 7: rd(a + 2 * i, b, 2); return fmaxf((int16_t)lduw_le_p(b) / 32767.0f, -1.0f);
    }
    warn_once(4, "attribute type");
    return 0;
}

/* ---------------------------------------------------------------- setup / TRAM */

#define NROWS 16
typedef struct {
    float x, y, z, iw;              /* screen pixels, 1/w */
    float f[NROWS][4], lo[NROWS][4], hi[NROWS][4];
} Vtx;

static void link_vertex(const V4 *o, Vtx *v)
{
    unsigned outmask = g.regs[0x120] & 0xffff, map[16], n = 0;
    unsigned nlink = ((g.regs[0x343] >> 5) & 31) + 1;
    float w = o[0].v[3] != 0 ? o[0].v[3] : 1.0f;

    for (int i = 0; i < 16; i++) {
        if (outmask & (1u << i)) {
            map[n++] = i;
        }
    }
    memset(v->f, 0, sizeof(v->f) * 3);
    for (unsigned k = 0; k < nlink && k < 32; k++) {
        uint32_t w0 = g.regs[0x300 + 2 * k], w1 = g.regs[0x301 + 2 * k];
        unsigned ex = (w0 >> 3) & 15, row = (w0 >> 9) & 63;
        const V4 *e;
        if (ex >= n || row >= NROWS) {
            continue;
        }
        e = &o[map[ex]];
        for (int c = 0; c < 4; c++) {
            unsigned swz = (w1 >> (4 * c)) & 3, ty = (w1 >> (4 * c + 2)) & 3;
            float val = e->v[c];
            if (ty == 3) {
                v->f[row][swz] = val;
            } else if (ty == 1) {
                v->lo[row][swz] = val;
            } else if (ty == 2) {
                v->hi[row][swz] = val;
            }
        }
    }
    v->iw = 1.0f / w;
    /* viewport: clip -> screen in 1/16 pixel */
    v->x = (o[0].v[0] * v->iw * u2f(g.regs[0x355]) + u2f(g.regs[0x352])) / 16.0f;
    v->y = (o[0].v[1] * v->iw * u2f(g.regs[0x356]) + u2f(g.regs[0x353])) / 16.0f;
    v->z = o[0].v[2] * v->iw * u2f(g.regs[0x357]) + u2f(g.regs[0x354]);
}

/* ---------------------------------------------------------------- fragment stage */

typedef struct {
    uint32_t row[16];               /* raw 20-bit (fp20 or 2 x fx10) */
    uint32_t glob[8];
    float alu_f[4], alu_lo[4], alu_hi[4];
    uint32_t cond[8];
    bool kill;
    float px, py;
    int rt_h;
    /* spill buffer (render target 4), per fragment: see frag_spill_* */
    uint32_t spill_a[4];            /* DW rt4 with bit 0 set: one slot */
    uint32_t spill_q[8][4];         /* DW rt4 with bit 0 clear: FIFO */
    uint8_t spill_qn, spill_qrd;
} Frag;

static uint64_t alu_word(unsigned packet, unsigned slot)
{
    unsigned i = packet * 8 + slot * 2;
    return ((uint64_t)g.alu[i] << 32) | g.alu[i + 1];
}

/* read a 20-bit register value (raw) by ALU register selector */
static uint32_t frag_raw(Frag *fr, unsigned sel, uint64_t imm, bool *is_float, float *fv)
{
    *is_float = false;
    if (sel < 16) {
        return fr->row[sel];
    }
    if (sel < 24) {
        return fr->glob[sel - 16];
    }
    if (sel < 32 && sel >= 28 && sel <= 30) {
        return (uint32_t)(imm >> (4 + 20 * (sel - 28))) & 0xfffff;
    }
    if (sel == 31) {
        return 256u << 10;          /* lowp vec2(0, 1): .l = 0.0, .h = 1.0 */
    }
    if (sel >= 32 && sel < 64) {
        return g.regs[0x820 + sel - 32] & 0xfffff;
    }
    if (sel >= 64 && sel < 72) {
        return fr->cond[sel - 64];
    }
    *is_float = true;
    switch (sel) {
    case 72: *fv = 8192.0f + fr->px; break;
    case 73: *fv = 8192.0f + (fr->rt_h - 1) - fr->py; break;
    case 74: *fv = 1.0f; break;
    default: *fv = 0.0f; break;
    }
    return 0;
}

static float alu_operand(Frag *fr, unsigned op13, uint64_t imm)
{
    unsigned sel = (op13 >> 6) & 127;
    bool high = op13 & 32, m1 = op13 & 16, fx = op13 & 8, ab = op13 & 4, ng = op13 & 2,
         x2 = op13 & 1;
    float f;

    if (sel >= 24 && sel < 28) {
        int k = sel - 24;
        f = fx ? (high ? fr->alu_hi[k] : fr->alu_lo[k]) : fr->alu_f[k];
    } else if (sel == 31) {
        /* lowp vec2(0, 1): #0 / #1 by the sub-register select alone, whatever
         * the fixed10 flag (reading it as fp20 gave #0 = 2.0, which made the
         * clipped-text shader's clip tests always fire) */
        f = high ? 1.0f : 0.0f;
    } else {
        bool isf;
        float fv = 0;
        uint32_t raw = frag_raw(fr, sel, imm, &isf, &fv);
        f = isf ? fv : fx ? fx10_dec(high ? raw >> 10 : raw) : fp20_dec(raw);
    }
    if (m1) {
        f -= 1.0f;
    }
    if (ab) {
        f = fabsf(f);
    }
    if (ng) {
        f = -f;
    }
    if (x2) {
        f *= 2.0f;
    }
    return f;
}

static float alu_rd(Frag *fr, uint64_t in, uint64_t imm)
{
    unsigned rd = in & 63;
    unsigned base;
    bool high, m1, fx, ab;
    float f;

    if (!(rd & 4)) {
        return 1.0f;
    }
    base = (rd & 32) ? (unsigned)(in >> 6) & 0x1fff : (unsigned)(in >> 19) & 0x1fff;
    fx = rd & 1;
    ab = rd & 2;
    m1 = rd & 8;
    high = rd & 16;
    /* re-use the selected operand's register with rD's own modifiers */
    f = alu_operand(fr, (base & ~0x3fu) | (high ? 32 : 0) | (fx ? 8 : 0), imm);
    if (m1) {
        f -= 1.0f;
    }
    if (ab) {
        f = fabsf(f);
    }
    return f;
}

static bool uses_imm(uint64_t in)
{
    unsigned a = (in >> 38) & 127, b = (in >> 25) & 127, c = (in >> 12) & 127;
    return (a >= 28 && a <= 30) || (b >= 28 && b <= 30) || (c >= 28 && c <= 30);
}

/*
 * ALU "send" (bit 61): a sender contributes only its product rA*rB, which is
 * added to the receiving ALU's result; the receiver keeps its own rC*rD (mode A).
 * The blur's senders all carry the receiver's accumulator as rC, so counting
 * it once per sender (old mode C: receiver rC := sum of the senders' full
 * results) added it 2-3 times. ZUNEHD_ALU_SEND=B|C selects the old variants.
 */
static char g_send_mode = 'A';

static void alu_packet(Frag *fr, unsigned p)
{
    uint64_t in[4];
    bool imm = false;
    float nf[4], nlo[4], nhi[4];
    int nslots;

    for (int j = 0; j < 4; j++) {
        in[j] = alu_word(p, j);
    }
    for (int j = 0; j < 3; j++) {
        imm |= uses_imm(in[j]);
    }
    /*
     * The ALU3 word holding immediates is NOT word-swapped like instruction
     * words (grate fragment-shader-isa.md): imm0 = bits 4..23, imm1 = 24..43,
     * imm2 = 44..63 of (word1 << 32 | word0). Verified on the music-hub blur
     * (weights 0.176/0.12/0.076/0.039/0.017, tap offsets 1/6..5/6); the
     * swapped read gave imm1 = 573.3 and sampled far outside the texture.
     */
    if (imm) {
        unsigned w = p * 8 + 6;
        in[3] = ((uint64_t)g.alu[w + 1] << 32) | g.alu[w];
    }
    nslots = imm ? 3 : 4;
    memcpy(nf, fr->alu_f, sizeof(nf));
    memcpy(nlo, fr->alu_lo, sizeof(nlo));
    memcpy(nhi, fr->alu_hi, sizeof(nhi));

    /*
     * Result accumulation (grate docs/fragment-shader-isa.md, patent US8521800):
     * a receiving ALU's operand rC is replaced by the accumulation value.
     *   bit 60 "this" (recv): ALU1<-ALU0, ALU2<-ALU1, ALU3<-ALU2, ALU0<-ALU3
     *          (ALU0 takes ALU3 of the previous instruction).
     *   bit 61 "other" (send): ALU1 -> ALU0, ALU2 -> ALU0, ALU3 -> ALU2.
     * The UI text blend uses recv (green term); blur/glow shaders use send.
     * Senders are evaluated first; a receiver of sends gets the sum of them.
     */
    bool done[4] = { false };
    float acc[4] = { 0 };
    bool has_acc[4] = { false };
    static const int send_to[4] = { -1, 0, 0, 2 };

    for (int pass = 0; pass < 2; pass++)
    for (int j = 0; j < nslots; j++) {
        uint64_t x = in[j];
        bool send = (x >> 61) & 1, recv = (x >> 60) & 1;

        if (done[j] || (pass == 0 && !send)) {
            continue;                   /* pass 0: senders only */
        }
        if (pass == 0 && j == 3 && has_acc[3]) {
            continue;
        }
        float A = alu_operand(fr, (x >> 32) & 0x1fff, in[3]);
        float B = alu_operand(fr, (x >> 19) & 0x1fff, in[3]);
        float C = alu_operand(fr, (x >> 6) & 0x1fff, in[3]);
        float D = alu_rd(fr, x, in[3]);
        unsigned op = (x >> 62) & 3, scale = (x >> 57) & 3, cc = (x >> 54) & 3;
        bool wl = (x >> 45) & 1, wh = (x >> 46) & 1, sat = (x >> 56) & 1,
             adddis = (x >> 59) & 1;
        float r, lo, hi;

        if (has_acc[j] && g_send_mode == 'C') {
            C = acc[j];
        } else if (recv) {
            C = j ? nf[j - 1] : fr->alu_f[3];
        }
        switch (op) {
        case 0: r = A * B + C * D; break;
        case 1: r = fminf(A * B, C * D); break;
        case 2: r = fmaxf(A * B, C * D); break;
        default: r = A < 0 ? B : C * D; break;
        }
        if (has_acc[j] && g_send_mode != 'C') {
            r += acc[j];
        }
        if (adddis) {
            lo = wh && !wl ? C * D : A * B;
            hi = wh && !wl ? A * B : C * D;
        } else {
            lo = hi = r;
        }
        {
            static const float sc[4] = { 1, 2, 4, 0.5f };
            r *= sc[scale];
            lo *= sc[scale];
            hi *= sc[scale];
        }
        if (sat) {
            r = clamp01(r);
            lo = clamp01(lo);
            hi = clamp01(hi);
        }
        if (cc) {
            float t = adddis ? lo : r;
            bool b = cc == 1 ? t == 0 : cc == 2 ? t > 0 : t >= 0;
            r = lo = hi = b ? 1.0f : 0.0f;
        }
        nf[j] = r;
        nlo[j] = lo;
        nhi[j] = hi;
        done[j] = true;
        if (send && send_to[j] >= 0) {
            acc[send_to[j]] += g_send_mode == 'A' ? A * B : r;
            has_acc[send_to[j]] = true;
        }
    }

    /* write back in slot order */
    for (int j = 0; j < nslots; j++) {
        uint64_t x = in[j];
        unsigned dst = (x >> 47) & 127;
        bool wl = (x >> 45) & 1, wh = (x >> 46) & 1;
        float r = nf[j], lo = nlo[j], hi = nhi[j];

        if (dst == 76) {
            if (r != 0) {
                fr->kill = true;
            }
            continue;
        }
        if (dst >= 64 && dst < 72) {
            /* condition register: two fx10 halves holding !!result; the high
             * half only with "write high", else the low half (grate ISA doc) */
            uint32_t v = fx10_enc(r != 0 ? 1.0f : 0.0f);
            uint32_t *cr = &fr->cond[dst - 64];
            *cr = wh ? (*cr & 0x3ffu) | (v << 10) : (*cr & ~0x3ffu) | v;
            continue;
        }
        {
            uint32_t *reg = dst < 16 ? &fr->row[dst] : dst < 24 ? &fr->glob[dst - 16] : NULL;
            if (!reg) {
                continue;           /* 31 = discard (lp), others not modelled */
            }
            /*
             * Write mask = destination format (grate assembler-syntax.md):
             * both halves -> one fp20, one half -> fx10 in that half, none ->
             * no write. (Writing "lh" as two fx10 halves broke the music hub's
             * multi-pass blur, whose texcoords are fp20 "r2.lh" results.)
             * Addition-disable splits the two products into the halves.
             */
            if (wl && wh && !((x >> 59) & 1)) {
                *reg = fp20_enc(r);
            } else {
                if (wl) {
                    *reg = (*reg & ~0x3ffu) | fx10_enc(lo);
                }
                if (wh) {
                    *reg = (*reg & 0x3ffu) | (fx10_enc(hi) << 10);
                }
            }
        }
    }
    memcpy(fr->alu_f, nf, sizeof(nf));
    memcpy(fr->alu_lo, nlo, sizeof(nlo));
    memcpy(fr->alu_hi, nhi, sizeof(nhi));
}

/*
 * Swizzled texture layout (desc1 bit 4 on AP16; grate calls the bit "compressed").
 * Found from the boot bar texture (1x380 RGBA8888): 16x16-texel blocks of 1 KiB,
 * blocks row-major; inside a block the byte address bits are
 *   [2] x0  [3] y0  [4] x1  [5] y1  [6] y2  [7] y3  [8] x2  [9] x3.
 * The y bits are verified on that texture; the x-bit order is inferred from the
 * address bits a 1-wide texture leaves unused (to be confirmed on a wider one).
 */
static hwaddr tex_swizzle_offset(int x, int y, int w, int h, int bpp)
{
    /*
     * 4 bpp: the 1 KiB tile shrinks in height for short textures (lock-screen
     * bar gradients are 128x1, 4 KiB apart: tile = 128x2, bits x0 y0 x1 x2..x6,
     * verified from memory). Tile height = max(2, h) rounded to a power of two,
     * at most 16; bit order x0 y0 x1, then the other y bits, then the other x
     * bits. For h >= 16 this is the 16x16 layout below.
     */
    if (bpp == 4) {
        int yb = h >= 16 ? 4 : h > 4 ? 3 : h > 2 ? 2 : 1;
        unsigned trb = 1024u >> yb;                 /* bytes per tile row */
        unsigned bx4 = (unsigned)x * 4;
        int tpr4 = (w * 4 + trb - 1) / trb;
        hwaddr tile4 = ((hwaddr)(y >> yb) * tpr4 + bx4 / trb) * 1024;
        unsigned xin = (bx4 % trb) >> 2, off = ((xin & 1) << 2) | ((y & 1) << 3) |
                       (((xin >> 1) & 1) << 4);
        int bit = 5;
        for (int k = 1; k < yb; k++) {
            off |= ((y >> k) & 1u) << bit++;
        }
        for (int k = 2; bit < 10; k++) {
            off |= ((xin >> k) & 1u) << bit++;
        }
        return tile4 | off;
    }
    /* Tiles are 64 bytes x 16 rows (1 KiB). Bit layout inside a tile:
     *  4 bpp (boot bar, y bits verified): [2]x0 [3]y0 [4]x1 [5]y1 [6]y2 [7]y3 [8]x2 [9]x3
     *  1 bpp (UI glyph atlas, found by smoothness search over all y placements):
     *        [0]x0 [1]x1 [2]y0 [3]y1 [4]x2 [5]x3 [6]y2 [7]y3 [8]x4 [9]x5
     *  2 bpp: unverified, assumed like 1 bpp in bytes. */
    unsigned bx = x * bpp;
    int tpr = (w * bpp + 63) >> 6;
    hwaddr tile = ((hwaddr)(y >> 4) * tpr + (bx >> 6)) * 1024;

    if (bpp == 4) {
        return tile | (bx & 7) | ((y & 1) << 3) | ((bx & 8) << 1) | ((y & 2) << 4) |
               ((y & 4) << 4) | ((y & 8) << 4) | ((bx & 16) << 4) | ((bx & 32) << 4);
    }
    return tile | (bx & 3) | ((y & 3) << 2) | ((bx & 12) << 2) | ((y & 12) << 4) |
           ((bx & 48) << 4);
}

static hwaddr tex_addr(uint32_t ptr, uint32_t d1, int x, int y, int w, int h, int bpp,
                       uint32_t pitch)
{
    /* Rows are stored top-down. (A bottom-up flip here looked right only while the
     * host1x context restore was missing; with INDDATA restores it is upside down.) */
    if (d1 & 0x10) {
        return ptr + tex_swizzle_offset(x, y, w, h, bpp);
    }
    return ptr + (hwaddr)y * pitch + x * bpp;
}

static void tex_sample(unsigned s, float u, float v, float c[4])
{
    uint32_t ptr = g.regs[0x710 + s], d1 = g.regs[0x720 + 2 * s], d2 = g.regs[0x721 + 2 * s];
    int fmt = (d1 >> 8) & 31, w, h;
    int bpp = px_bytes(fmt);
    uint32_t pitch;
    float fx, fy;
    int x0, y0;

    /* DESC2: NOT_POW2 (0x40) -> width [31:20], height [19:8];
     * otherwise log2 width [31:28], log2 height [27:24]. */
    if (d2 & 0x40) {
        w = (d2 >> 20) & 0xfff;
        h = (d2 >> 8) & 0xfff;
    } else {
        w = 1 << ((d2 >> 28) & 15);
        h = 1 << ((d2 >> 24) & 15);
    }
    c[0] = c[1] = c[2] = 0;
    c[3] = 1;
    if (!ptr || !bpp || w <= 0 || h <= 0) {
        return;
    }
    /* desc1 bit 4 is "compressed" per grate, but this firmware sets it on plain
     * RGBA8888 textures; only the DXT/ETC formats are treated as compressed. */
    if (fmt == 0x16 || fmt == 0x18 || fmt == 0x19 || fmt == 0x1a) {
        warn_once(6, "compressed textures");
        return;
    }
    /* Linear pitch = row bytes rounded up to 64: the music hub renders text into
     * 120x64 RGBA targets with pitch 512 (480 -> 512) and samples them back as
     * textures; 272-wide layers (1088 = 17 * 64) are unaffected. */
    pitch = (w * bpp + 63) & ~63u;
    /* wrap: clamp-to-edge if requested, else repeat */
    if (!(d1 & 2)) {
        u -= floorf(u);
    }
    if (!(d1 & 1)) {
        v -= floorf(v);
    }
    fx = u * w - 0.5f;
    fy = v * h - 0.5f;
    if (d1 & 0x20000000) {          /* linear mag filter */
        float t[4][4], ax, ay;
        x0 = (int)floorf(fx);
        y0 = (int)floorf(fy);
        ax = fx - x0;
        ay = fy - y0;
        for (int k = 0; k < 4; k++) {
            int xx = MIN(MAX(x0 + (k & 1), 0), w - 1), yy = MIN(MAX(y0 + (k >> 1), 0), h - 1);
            px_load(fmt, tex_addr(ptr, d1, xx, yy, w, h, bpp, pitch), t[k]);
        }
        for (int i = 0; i < 4; i++) {
            c[i] = (t[0][i] * (1 - ax) + t[1][i] * ax) * (1 - ay) +
                   (t[2][i] * (1 - ax) + t[3][i] * ax) * ay;
        }
    } else {
        x0 = MIN(MAX((int)floorf(fx + 0.5f), 0), w - 1);
        y0 = MIN(MAX((int)floorf(fy + 0.5f), 0), h - 1);
        px_load(fmt, tex_addr(ptr, d1, x0, y0, w, h, bpp, pitch), c);
    }
}

static void rt_addr(unsigned rt, int x, int y, hwaddr *a, int *fmt)
{
    uint32_t p = g.regs[0xe10 + rt];
    *fmt = (p >> 2) & 31;
    *a = g.regs[0xe00 + rt] + (hwaddr)y * ((p >> 8) & 0x1ffff) + x * px_bytes(*fmt);
    if (p & 0x04000000) {
        warn_once(7, "tiled render target");
    }
}

static void put4(Frag *fr, int r0, const float c[4])
{
    fr->row[r0] = fx10_enc(c[0]) | (fx10_enc(c[1]) << 10);
    fr->row[r0 + 1] = fx10_enc(c[2]) | (fx10_enc(c[3]) << 10);
}

static void get4(Frag *fr, int r0, float c[4])
{
    c[0] = fx10_dec(fr->row[r0]);
    c[1] = fx10_dec(fr->row[r0] >> 10);
    c[2] = fx10_dec(fr->row[r0 + 1]);
    c[3] = fx10_dec(fr->row[r0 + 1] >> 10);
}

static bool g_debug_frag, g_debug_kill;

static void frag_dw(Frag *fr, unsigned i, unsigned R, int x, int y, float dbg_s,
                    float dbg_t, const float *dbg_tex)
{
    hwaddr a;
    int fmt;
    float c[4];

    if (i >= g.dw_n || !(g.dw[i] & 1) || fr->kill) {
        return;
    }
    get4(fr, R + ((g.dw[i] & 0x8000) ? 2 : 0), c);
    rt_addr((g.dw[i] >> 2) & 15, x, y, &a, &fmt);
    px_store(fmt, a, c);
    if (g_debug_frag && ((g.dw[i] >> 2) & 15) == 1) {     /* first colour store */
        LOGU("  frag (%d,%d) st (%.3f,%.3f) -> rt%u fmt %d @0x%08" HWADDR_PRIx
             " = %.3f %.3f %.3f %.3f (texel %.3f %.3f %.3f %.3f)\n", x, y,
             dbg_s, dbg_t, (g.dw[i] >> 2) & 15, fmt, a, c[0], c[1], c[2], c[3],
             dbg_tex[0], dbg_tex[1], dbg_tex[2], dbg_tex[3]);
        g_debug_frag = false;
    }
}

/*
 * Spill buffer (render target 4, fmt 20, pitch 1216). Programs with more live
 * values than fit (the 48-batch 4-row blur behind the music-hub album-row glow
 * and the held settings-item glow) save and reload row registers through it:
 *   DW to rt4 saves the batch's whole row (4 registers). With DW bit 0 set it
 *   goes to a single slot (the blur keeps the centre texel there); with bit 0
 *   clear it is appended to a queue (the blur's per-channel partial sums).
 *   PSEQ with rt field 4 and bit 23 clear reloads ONE register into the batch's
 *   row register (word & 3): bit 8 clear -> from the slot, register = bit 6;
 *   bit 8 set -> the next queue entry, register = bit 6 ? 3 : 1.
 * Derived from both blur passes with gui\fpsym.py / fpgreedy.py / fpalt.py: every
 * load has exactly one source that makes all four output channels the same
 * 11-tap kernel (0.176 / 2x0.16 / 2x0.12 / 2x0.076 / 2x0.039 / 2x0.017, sum 1.0);
 * the queue order and register rule above reproduce all seven loads of both
 * passes. Kept per fragment (no memory traffic), which is thread-safe.
 */
static void frag_spill_store(Frag *fr, unsigned R, uint32_t dw)
{
    if (dw & 1) {
        memcpy(fr->spill_a, &fr->row[R], sizeof(fr->spill_a));
    } else if (fr->spill_qn < 8) {
        memcpy(fr->spill_q[fr->spill_qn++], &fr->row[R], sizeof(fr->spill_a));
    }
}

static void frag_spill_load(Frag *fr, unsigned R, uint32_t p)
{
    unsigned b6 = (p >> 6) & 1;

    if (!(p & 0x100)) {
        fr->row[R + (p & 3)] = fr->spill_a[b6];
    } else if (fr->spill_qrd < fr->spill_qn) {
        fr->row[R + (p & 3)] = fr->spill_q[fr->spill_qrd++][b6 ? 3 : 1];
    }
}

/*
 * Fragment pipeline schedule.
 *
 * The pixel packet has ALU_BUFFER_SIZE (0xe20 [1:0] + 1) rows of 4 registers
 * (r0-r3, r4-r7, ...). Batch b of the program works on row b % N: MFU varyings,
 * TEX coordinates/results, PSEQ loads and DW sources address that row's r0-r3
 * (r(4*row)..); ALU operands address r0-r15 absolutely. The units are pipeline
 * stages a row passes one after another, so a later batch's MFU/TEX can complete
 * before an earlier batch's ALU. Unit u of batch b runs at time
 *     (b / N) * L + (b % N) + D[u],   L = max(N, 8) slots per pass,
 *     D: MFU -5, TEX -3.5, PSEQ -2.5, ALU 0, DW 3.5.
 * (Session 9b refit with the spill buffer modelled: the blur's TEX of batch 15
 * must land before the ALU of batch 12, its spill reloads of batches 42/46 (row 2)
 * before the row-0 ALUs of batches 40/44, its rt1 DW after the blend of batches
 * 45-47; all other UI programs give the same results as with the old offsets.)
 * Fitted to every program the Zune UI uses (tools: gui\fpfit2.py): the music
 * hub's 48-batch 4-row blur (ALU of batch 0 uses the texcoords its MFU of batch 3
 * interpolates into r14/r15; its rt1 DW at batch 44 stores the blend computed by
 * batches 45-47), the 2-row clipped-text and radial-gradient programs (ALU batch
 * 0 must read the clip distances in r4/r5 before MFU batch 3 reuses them) and
 * the 1-row text programs (PSEQ batch 1 reloads r0/r1 after ALU batch 0 used
 * the texel). For 1-row programs this equals the old "ALU sees the next
 * batch's MFU/TEX, DW one batch late" timing for every program seen before.
 */
enum { EV_MFU, EV_TEX, EV_PSEQ, EV_ALU, EV_DW };
typedef struct { float t; uint8_t unit, b; } FpEvent;
static FpEvent g_sched[64 * 5];
static unsigned g_sched_n, g_rows;

static int fp_ev_cmp(const void *pa, const void *pb)
{
    const FpEvent *a = pa, *b = pb;
    if (a->t != b->t) {
        return a->t < b->t ? -1 : 1;
    }
    return a->unit != b->unit ? a->unit - b->unit : a->b - b->b;
}

static void fp_schedule(void)
{
    static float D[5] = { -5.0f, -3.5f, -2.5f, 0.0f, 3.5f };
    static unsigned Lmin = 8;
    static bool init;
    unsigned nb = MIN(g.pseq_n, 64u), L;

    if (!init) {        /* debug override: ZUNEHD_FP_TIMING="L,dm,dt,dp,da,dd" */
        const char *p = getenv("ZUNEHD_FP_TIMING");
        const char *sm = getenv("ZUNEHD_ALU_SEND");
        init = true;
        if (sm && (*sm == 'A' || *sm == 'B' || *sm == 'C')) {
            g_send_mode = *sm;
        }
        if (p) {
            sscanf(p, "%u,%f,%f,%f,%f,%f", &Lmin, &D[0], &D[1], &D[2], &D[3], &D[4]);
        }
    }
    g_rows = (g.regs[0xe20] & 3) + 1;
    L = MAX(g_rows, Lmin);
    g_sched_n = 0;
    for (unsigned b = 0; b < nb; b++) {
        float base = (float)((b / g_rows) * L + b % g_rows);
        for (int u = 0; u < 5; u++) {
            g_sched[g_sched_n++] = (FpEvent){ base + D[u], (uint8_t)u, (uint8_t)b };
        }
    }
    qsort(g_sched, g_sched_n, sizeof(g_sched[0]), fp_ev_cmp);
}

/* MFU special function on a row register (grate fragment-shader-isa.md). The
 * "rcp r4" of the barycentric prologue (register 4 = w) is implicit in our
 * perspective-correct interpolation and skipped. The result replaces the
 * operand register and feeds the multipliers as source 10. */
static float mfu_sfu(Frag *fr, unsigned R, uint64_t m)
{
    unsigned op = (m >> 54) & 15, reg = (m >> 58) & 63;
    float a, r;

    if (!op || reg >= 4) {
        return 0.0f;
    }
    a = fp20_dec(fr->row[R + reg]);
    switch (op) {
    case 1: r = 1.0f / a; break;
    case 2: r = 1.0f / sqrtf(a); break;
    case 3: r = log2f(a); break;
    case 4: case 9: r = exp2f(a); break;    /* PREEX2 + EX2 */
    case 5: r = sqrtf(a); break;
    case 8: r = a - floorf(a); break;
    default:
        warn_once(8, "MFU sin/cos special functions");
        return a;
    }
    if (op == 9) {
        return a;                           /* first step: keep the operand */
    }
    fr->row[R + reg] = fp20_enc(r);
    return r;
}

static float mfu_mul_src(Frag *fr, unsigned R, unsigned s, float sfu)
{
    if (s < 4) {
        return fp20_dec(fr->row[R + s]);
    }
    if (s == 10) {
        return sfu;
    }
    if (s == 13) {
        return 1.0f;
    }
    warn_once(10, "MFU multiplier source");
    return 0.0f;
}

static void frag_run(int x, int y, const float tf[NROWS][4], const float tlo[NROWS][4],
                     const float thi[NROWS][4], int rt_h)
{
    Frag fr = { .px = x, .py = y, .rt_h = rt_h };
    float dbg_s = 0, dbg_t = 0, dbg_tex[4] = { 0 };
    /* Debug: ZUNEHD_GR3D_FRAG="X,Y,NB" logs the register file after every unit
     * event of fragment (X,Y) in programs of NB batches (needs -d unimp). */
    static int fdbg_init, fdbg_x = -1, fdbg_y = -1, fdbg_nb = -1;
    bool fdbg;

    if (!fdbg_init) {
        const char *p = getenv("ZUNEHD_GR3D_FRAG");
        fdbg_init = 1;
        if (p) {
            sscanf(p, "%d,%d,%d", &fdbg_x, &fdbg_y, &fdbg_nb);
        }
    }
    fdbg = x == fdbg_x && y == fdbg_y && (int)g.pseq_n == fdbg_nb;

    for (unsigned e = 0; e < g_sched_n; e++) {
        float c[4];
        unsigned i = g_sched[e].b, R = 4 * (i % g_rows);

        if (fdbg && e) {
            const FpEvent *pe = &g_sched[e - 1];
            LOGU("  fdbg draw %u b%u u%u t%.2f:", g.draws, pe->b, pe->unit, pe->t);
            for (int k = 0; k < 16; k++) {
                qemu_log_mask(LOG_UNIMP, " %05x", fr.row[k]);
            }
            qemu_log_mask(LOG_UNIMP, "%s\n", fr.kill ? " KILL" : "");
        }

        switch (g_sched[e].unit) {
        case EV_MFU:
        if (i < g.mfu_sched_n) {                    /* MFU: varyings */
            unsigned s = g.mfu_sched[i], n = s & 3, addr = (s >> 2) & 63;
            for (unsigned k = 0; k < n && addr + k < 64; k++) {
                uint64_t m = ((uint64_t)g.mfu[2 * (addr + k)] << 32) | g.mfu[2 * (addr + k) + 1];
                unsigned base = R;
                float sfu = mfu_sfu(&fr, R, m);
                for (int mi = 0; mi < 2; mi++) {
                    unsigned mu = (m >> (32 + 11 * mi)) & 0x7ff, dst = (mu >> 8) & 7;
                    if (dst >= 4) {
                        float f = mfu_mul_src(&fr, R, mu & 15, sfu) *
                                  mfu_mul_src(&fr, R, (mu >> 4) & 15, sfu);
                        fr.row[R + dst - 4] = fp20_enc(f);
                    }
                }
                for (int vi = 0; vi < 4; vi++) {
                    unsigned var = (m >> (7 * vi)) & 127, op = (var >> 1) & 3, src = var >> 3;
                    bool sat = var & 1;
                    if (!op || src >= NROWS) {
                        continue;
                    }
                    if (op == 1) {
                        float f = tf[src][vi];
                        fr.row[base + vi] = fp20_enc(sat ? clamp01(f) : f);
                    } else {
                        float lo = tlo[src][vi], hi = thi[src][vi];
                        if (sat) {
                            lo = clamp01(lo);
                            hi = clamp01(hi);
                        }
                        fr.row[base + vi] = fx10_enc(lo) | (fx10_enc(hi) << 10);
                    }
                }
            }
        }
        break;
        case EV_TEX:
        if (i < g.tex_n && (g.tex[i] & (1u << 10))) {   /* TEX on the batch's row */
            uint32_t tw = g.tex[i];
            unsigned sr = R + ((tw & 16) ? 2 : 0);
            dbg_s = fp20_dec(fr.row[sr]);
            dbg_t = fp20_dec(fr.row[sr + 1]);
            tex_sample(tw & 15, dbg_s, dbg_t, c);
            memcpy(dbg_tex, c, sizeof(dbg_tex));
            put4(&fr, R + ((tw & 32) ? 2 : 0), c);
        }
        break;
        case EV_PSEQ: {
            uint32_t p = g.pseq[i];

            if (((p >> 16) & 15) == 4 && !(p & (1u << 23))) {
                frag_spill_load(&fr, R, p);         /* spill reload */
            } else if (p & (1u << 23)) {            /* PSEQ: load render target */
                hwaddr ad;
                int fmt;
                rt_addr((p >> 16) & 15, x, y, &ad, &fmt);
                if (px_load(fmt, ad, c)) {
                    put4(&fr, R + ((p & 2) ? 2 : 0), c);
                }
            }
            break;
        }
        case EV_ALU:
            if (i < g.alu_sched_n) {                /* ALU packets */
                unsigned s = g.alu_sched[i], n = s & 3, addr = (s >> 2) & 63;
                for (unsigned k = 0; k < n && addr + k < 64; k++) {
                    alu_packet(&fr, addr + k);
                }
            }
            if (fr.kill) {
                if (g_debug_kill) {
                    g_debug_kill = false;
                    LOGU("  kill (%d,%d) r0..7 fp20: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f"
                         " (tf0 %.3f %.3f %.3f %.3f)\n", x, y,
                         fp20_dec(fr.row[0]), fp20_dec(fr.row[1]), fp20_dec(fr.row[2]),
                         fp20_dec(fr.row[3]), fp20_dec(fr.row[4]), fp20_dec(fr.row[5]),
                         fp20_dec(fr.row[6]), fp20_dec(fr.row[7]),
                         tf[0][0], tf[0][1], tf[0][2], tf[0][3]);
                }
                return;
            }
            break;
        case EV_DW:
            if (i < g.dw_n && ((g.dw[i] >> 2) & 15) == 4) {
                frag_spill_store(&fr, R, g.dw[i]);  /* spill store */
                break;
            }
            frag_dw(&fr, i, R, x, y, dbg_s, dbg_t, dbg_tex);
            break;
        }
    }
}

/* ---------------------------------------------------------------- rasterizer */

/*
 * Fill rule for pixel centres exactly on an edge: the edge p->q (taken in the
 * triangle's winding, s = sign of its area) owns them only for one of the two
 * directions, so two triangles sharing an edge never both draw a pixel. Without
 * it the diagonal of every translucent tri_fan quad was blended twice (visible
 * line across the now-playing art placeholder and the album tiles).
 */
static bool edge_owns(const Vtx *p, const Vtx *q, float s)
{
    float dx = (q->x - p->x) * s, dy = (q->y - p->y) * s;
    return dy > 0 || (dy == 0 && dx < 0);
}

typedef struct RJob {
    const Vtx *a, *b, *c;
    float area, s;
    int x0, x1, y0, y1, ymax;
} RJob;

/* Rows y0 + k, y0 + k + n, ... of one triangle (worker k of n). */
static void raster_rows(const RJob *j, int wk, int n)
{
    const Vtx *a = j->a, *b = j->b, *c = j->c;
    float area = j->area, s = j->s;
    int x0 = j->x0, x1 = j->x1, ymax = j->ymax;
    float tf[NROWS][4], tlo[NROWS][4], thi[NROWS][4];

    for (int y = j->y0 + wk; y < j->y1; y += n) {
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = (b->x - px) * (c->y - py) - (b->y - py) * (c->x - px);
            float w1 = (c->x - px) * (a->y - py) - (c->y - py) * (a->x - px);
            float w2 = (a->x - px) * (b->y - py) - (a->y - py) * (b->x - px);
            float l0 = w0 / area, l1 = w1 / area, l2 = 1.0f - l0 - l1;
            float p0, p1, p2, den;
            if (l0 < 0 || l1 < 0 || w2 * s < 0 ||
                (w0 == 0 && !edge_owns(b, c, s)) || (w1 == 0 && !edge_owns(c, a, s)) ||
                (w2 == 0 && !edge_owns(a, b, s))) {
                continue;
            }
            p0 = l0 * a->iw;
            p1 = l1 * b->iw;
            p2 = l2 * c->iw;
            den = p0 + p1 + p2;
            p0 /= den;
            p1 /= den;
            p2 /= den;
            for (int r = 0; r < NROWS; r++) {
                for (int k = 0; k < 4; k++) {
                    tf[r][k] = p0 * a->f[r][k] + p1 * b->f[r][k] + p2 * c->f[r][k];
                    tlo[r][k] = p0 * a->lo[r][k] + p1 * b->lo[r][k] + p2 * c->lo[r][k];
                    thi[r][k] = p0 * a->hi[r][k] + p1 * b->hi[r][k] + p2 * c->hi[r][k];
                }
            }
            frag_run(x, y, tf, tlo, thi, ymax);
        }
    }
}

/*
 * Raster worker pool (ZUNEHD_GR3D_THREADS, default 8; 1 = single-threaded).
 * Each triangle's rows are interleaved over the workers; the calling (vCPU)
 * thread is worker 0 and waits for all rows before the next triangle, so draw
 * order and per-pixel blend order are unchanged. Per-fragment state is local
 * (Frag on the stack); the program/schedule globals are read-only while a
 * draw runs. Disabled when the single-fragment debug trace is on.
 */
#define POOL_MAX 32
static struct {
    int n;                      /* total workers incl. the caller; 0 = not set up */
    QemuThread th[POOL_MAX];
    QemuMutex m;
    QemuCond go, done;
    unsigned gen;
    int pending;
    RJob job;
} pool;

static void *pool_worker(void *opaque)
{
    int k = (intptr_t)opaque;
    unsigned seen = 0;

    rcu_register_thread();              /* address_space_* fallback uses RCU */
    for (;;) {
        RJob j;
        qemu_mutex_lock(&pool.m);
        while (pool.gen == seen) {
            qemu_cond_wait(&pool.go, &pool.m);
        }
        seen = pool.gen;
        j = pool.job;
        qemu_mutex_unlock(&pool.m);
        raster_rows(&j, k, pool.n);
        qemu_mutex_lock(&pool.m);
        if (--pool.pending == 0) {
            qemu_cond_signal(&pool.done);
        }
        qemu_mutex_unlock(&pool.m);
    }
    return NULL;
}

static void pool_init(void)
{
    const char *e = getenv("ZUNEHD_GR3D_THREADS");
    int n = e ? atoi(e) : 8;

    if (getenv("ZUNEHD_GR3D_FRAG")) {
        n = 1;
    }
    n = MAX(1, MIN(n, POOL_MAX));
    pool.n = n;
    if (n == 1) {
        return;
    }
    qemu_mutex_init(&pool.m);
    qemu_cond_init(&pool.go);
    qemu_cond_init(&pool.done);
    for (int k = 1; k < n; k++) {
        qemu_thread_create(&pool.th[k], "zunehd-gr3d", pool_worker,
                           (void *)(intptr_t)k, QEMU_THREAD_DETACHED);
    }
    info_report("zunehd: gr3d raster pool: %d threads", n);
}

static void raster_tri(const Vtx *a, const Vtx *b, const Vtx *c)
{
    uint32_t sh = g.regs[0x350], sv = g.regs[0x351];
    int xmin = sh >> 16, xmax = sh & 0xffff, ymin = sv >> 16, ymax = sv & 0xffff;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    RJob j;

    if (fabsf(area) < 1e-6f) {
        return;
    }
    g_debug_frag = true;
    j.a = a;
    j.b = b;
    j.c = c;
    j.area = area;
    j.s = area > 0 ? 1.0f : -1.0f;
    j.ymax = ymax;
    j.x0 = MAX((int)floorf(fminf(a->x, fminf(b->x, c->x))), xmin);
    j.x1 = MIN((int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x))), xmax);
    j.y0 = MAX((int)floorf(fminf(a->y, fminf(b->y, c->y))), ymin);
    j.y1 = MIN((int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y))), ymax);
    if (j.x1 <= j.x0 || j.y1 <= j.y0) {
        return;
    }
    if (!pool.n) {
        pool_init();
    }
    /* small triangles: not worth waking the pool */
    if (pool.n == 1 || (j.x1 - j.x0) * (j.y1 - j.y0) < 4096 || j.y1 - j.y0 < pool.n) {
        raster_rows(&j, 0, 1);
        return;
    }
    qemu_mutex_lock(&pool.m);
    pool.job = j;
    pool.pending = pool.n - 1;
    pool.gen++;
    qemu_cond_broadcast(&pool.go);
    qemu_mutex_unlock(&pool.m);
    raster_rows(&j, 0, pool.n);
    qemu_mutex_lock(&pool.m);
    while (pool.pending) {
        qemu_cond_wait(&pool.done, &pool.m);
    }
    qemu_mutex_unlock(&pool.m);
}

/* ---------------------------------------------------------------- draw */

/*
 * Debug: ZUNEHD_GR3D_DUMP="ADDR,LEN,DIR" writes LEN bytes at ADDR to DIR/dNNNNN.bin
 * after every draw whose render target 1 is at ADDR (max 400 files), to look at
 * offscreen render-to-texture passes.
 */
static void gr3d_debug_dump(void)
{
    static int init, n;
    static uint64_t addr, len;
    static char dir[256];

    if (!init) {
        const char *p = getenv("ZUNEHD_GR3D_DUMP");
        init = 1;
        if (p && sscanf(p, "%" SCNx64 ",%" SCNi64 ",%255s", &addr, &len, dir) == 3) {
            init = 2;
            info_report("zunehd: gr3d dump rt1 0x%" PRIx64 " len %" PRIu64 " -> %s",
                        addr, len, dir);
        }
    }
    /* ADDR 0: any draw into an offscreen target (scissor narrower than 272) */
    if (init != 2 || n >= 400 || len > (1 << 22) ||
        (addr ? g.regs[0xe01] != addr : (g.regs[0x350] & 0xffff) >= 272)) {
        return;
    }
    g_autofree uint8_t *buf = g_malloc(len);
    g_autofree char *fn = g_strdup_printf("%s/d%05u_%08x.bin", dir, g.draws,
                                          g.regs[0xe01]);
    FILE *f = fopen(fn, "wb");
    rd(g.regs[0xe01], buf, len);
    if (f) {
        fwrite(buf, 1, len, f);
        fclose(f);
        n++;
    }
}

static void gr3d_draw_impl(uint32_t dp);

/* ZUNEHD_PROF=1: share of wall time spent in 3D draws, printed every 5 s. */
static void gr3d_draw(uint32_t dp)
{
    static int prof = -1;
    static int64_t t_win, t_in, n_draw;
    int64_t t0;

    if (prof < 0) {
        prof = getenv("ZUNEHD_PROF") != NULL;
    }
    if (!ram_len) {
        ram_init();
    }
    if (!prof) {
        gr3d_draw_impl(dp);
        return;
    }
    t0 = g_get_monotonic_time();
    if (!t_win) {
        t_win = t0;
    }
    gr3d_draw_impl(dp);
    t_in += g_get_monotonic_time() - t0;
    n_draw++;
    if (t0 - t_win >= 5000000) {
        fprintf(stderr, "prof: gr3d %.1f%% of wall time, %lld draws, %.2f ms/draw\n",
                100.0 * t_in / (t0 - t_win), (long long)n_draw,
                n_draw ? t_in / 1000.0 / n_draw : 0.0);
        t_win = t0;
        t_in = n_draw = 0;
    }
}

static void gr3d_draw_impl(uint32_t dp)
{
    uint32_t prm = g.regs[0x122], io = g.regs[0x120];
    unsigned prim = (prm >> 24) & 7, imode = (prm >> 28) & 3;
    /*
     * Vertex index = DRAW_PRIMITIVES.OFFSET + k. DRAW_PARAMS.FIRST is NOT an
     * extra offset: grate-mesa writes the same start into both fields, and
     * the Zune UI's tri_fan quads set FIRST=1 with OFFSET 0 over a 4-vertex
     * buffer (adding it fetched the 4th vertex past the end: half-drawn
     * wallpaper quad, garbage clip distances -> lock-screen arrow killed).
     */
    unsigned count = (dp >> 20) + 1, first = dp & 0xfffff;
    (void)prm;
    g_autofree Vtx *vt = g_new0(Vtx, count);

    for (unsigned k = 0; k < count; k++) {
        V4 attr[16], o[16];
        unsigned idx = first + k;

        if (imode) {
            uint8_t b[2] = { 0 };
            if (imode == 1) {
                rd(g.regs[0x121] + idx, b, 1);
                idx = b[0];
            } else {
                rd(g.regs[0x121] + 2 * idx, b, 2);
                idx = b[0] | (b[1] << 8);
            }
        }
        for (int i = 0; i < 16; i++) {
            attr[i] = (V4){ { 0, 0, 0, 1 } };
            o[i] = (V4){ { 0, 0, 0, 1 } };
            if (io & (1u << (16 + i))) {
                uint32_t m = g.regs[0x101 + 2 * i];
                int type = m & 15, size = (m >> 4) & 15;
                hwaddr a = g.regs[0x100 + 2 * i] + (hwaddr)idx * (m >> 8);
                for (int c = 0; c < size && c < 4; c++) {
                    attr[i].v[c] = attr_comp(type, a, c);
                }
            }
        }
        vp_run(attr, o);
        link_vertex(o, &vt[k]);
    }
    LOGU("draw %u: prim %u, %u vertices, v0 (%.1f,%.1f) v1 (%.1f,%.1f) v2 (%.1f,%.1f)\n",
         g.draws, prim, count, vt[0].x, vt[0].y, count > 1 ? vt[1].x : 0,
         count > 1 ? vt[1].y : 0, count > 2 ? vt[2].x : 0, count > 2 ? vt[2].y : 0);
    for (unsigned k = 0; k < count && k < 4; k++) {
        LOGU("  v%u tram0 %.3f %.3f %.3f %.3f tram1 %.3f %.3f %.3f %.3f\n", k,
             vt[k].f[0][0], vt[k].f[0][1], vt[k].f[0][2], vt[k].f[0][3],
             vt[k].f[1][0], vt[k].f[1][1], vt[k].f[1][2], vt[k].f[1][3]);
    }
    g_debug_kill = true;
    fp_schedule();
    switch (prim) {
    case 4:
        for (unsigned k = 0; k + 2 < count; k += 3) {
            raster_tri(&vt[k], &vt[k + 1], &vt[k + 2]);
        }
        break;
    case 5:
        for (unsigned k = 0; k + 2 < count; k++) {
            raster_tri(&vt[k], &vt[k + 1], &vt[k + 2]);
        }
        break;
    case 6:
        for (unsigned k = 1; k + 1 < count; k++) {
            raster_tri(&vt[0], &vt[k], &vt[k + 1]);
        }
        break;
    default:
        warn_once(9, "points/lines");
        break;
    }
    gr3d_debug_dump();
    g.draws++;
}

/* ---------------------------------------------------------------- methods */

/*
 * Register read through host1x indirect access (INDOFF read mode). NvRm's 3D
 * context save reads plain registers and drains the program-upload ports: it
 * writes the *_ID index register, then reads the data port N times.
 */
uint32_t zune_gr3d_reg_read(uint32_t off)
{
    uint32_t v = 0;

    off &= 0xfff;
    switch (off) {
    case 0x206: return g.vp_ptr < 1024 ? g.vp[g.vp_ptr++] : 0;
    case 0x208:
        if (g.vc_ptr < 1024) {
            memcpy(&v, &g.vc[g.vc_ptr++], 4);
        }
        return v;
    case 0x541: return g.pseq_n < 64 ? g.pseq[g.pseq_n++] : 0;
    case 0x601: return g.mfu_sched_n < 64 ? g.mfu_sched[g.mfu_sched_n++] : 0;
    case 0x604: return g.mfu_n < 128 ? g.mfu[g.mfu_n++] : 0;
    case 0x701: return g.tex_n < 64 ? g.tex[g.tex_n++] : 0;
    case 0x801: return g.alu_sched_n < 64 ? g.alu_sched[g.alu_sched_n++] : 0;
    case 0x804: return g.alu_n < 512 ? g.alu[g.alu_n++] : 0;
    case 0x901: return g.dw_n < 64 ? g.dw[g.dw_n++] : 0;
    }
    return g.regs[off];
}

void zune_gr3d_method(uint32_t off, uint32_t val)
{
    off &= 0xfff;
    g.regs[off] = val;

    switch (off) {
    case 0x205: g.vp_ptr = val; break;
    case 0x206:
        if (g.vp_ptr < 1024) {
            g.vp[g.vp_ptr++] = val;
        }
        break;
    case 0x207: g.vc_ptr = val * 4; break;
    case 0x208:
        if (g.vc_ptr < 1024) {
            g.vc[g.vc_ptr++] = u2f(val);
        }
        break;
    case 0xe22:
        g.pseq_n = g.mfu_sched_n = g.mfu_n = g.tex_n = 0;
        g.alu_sched_n = g.alu_n = g.dw_n = 0;
        break;
    case 0x540: g.pseq_n = val; break;
    case 0x541: if (g.pseq_n < 64) g.pseq[g.pseq_n++] = val; break;
    case 0x600: g.mfu_sched_n = val; break;
    case 0x601: if (g.mfu_sched_n < 64) g.mfu_sched[g.mfu_sched_n++] = val; break;
    case 0x603: g.mfu_n = val; break;
    case 0x604: if (g.mfu_n < 128) g.mfu[g.mfu_n++] = val; break;
    case 0x700: g.tex_n = val; break;
    case 0x701: if (g.tex_n < 64) g.tex[g.tex_n++] = val; break;
    case 0x800: g.alu_sched_n = val; break;
    case 0x801: if (g.alu_sched_n < 64) g.alu_sched[g.alu_sched_n++] = val; break;
    case 0x803: g.alu_n = val; break;
    case 0x804: if (g.alu_n < 512) g.alu[g.alu_n++] = val; break;
    case 0x900: g.dw_n = val; break;
    case 0x901: if (g.dw_n < 64) g.dw[g.dw_n++] = val; break;
    case 0x123: gr3d_draw(val); break;
    }
}
