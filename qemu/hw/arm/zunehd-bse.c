/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD (Tegra AP16): the bitstream engine inside the VDE block that the
 * AVP's MP3 decoder uses for Layer III Huffman decoding (reverse-engineered
 * from the AVP decoder FUN_400176dc running in IRAM, session 8).
 *
 * Registers (VDE base 0x6001A000; NvRm modules 38/47):
 *   0x6001CC14  command-list kick: write 0xA0000000 | (IRAM word address of the
 *               list); [31] reads 1 while the list runs. 0 = idle.
 *   0x6001CC00 / CC10  configuration (stored).
 *   0x6001D018  status: [3],[4] idle (always), [11] list done (sticky, W1C),
 *               [0] busy, error mask 0x5FC000 (never set here).
 *   0x6001D064  output pointer: [30:16] = IRAM word index after the last value.
 *   0x6001D000 / D008 / D044  control (stored).
 * Command list (IRAM, 32-bit words, ends with 0xFFFFFFFF):
 *   0x7C rr vvvv  engine register rr = vvvv:
 *                 0x44 mode (0x2084 setup, 0x3084 big_values pairs,
 *                 0x3086 count1 quads), 0x54/0x56 bitstream limit lo/hi,
 *                 0x60 reset (0x41), 0x64 Huffman bit budget (from here),
 *                 0x66 output pointer (IRAM word index, (addr & 0x1FFFF) >> 2).
 *   0x4000000N, A  bitstream read pointer: byte address A, first bit = 7 - N
 *                 counted from the MSB (the decoder passes N = (7 - skip) & 7).
 *                 The AVP keeps the main data in a 2 KiB ring of 32-bit words
 *                 holding the stream big-endian (its software bit reader
 *                 shifts words MSB first), so stream byte k is at memory byte
 *                 k ^ 3; A = word address + consumed bits / 8 is an address in
 *                 that big-endian word view. The AVP copies the ring head past
 *                 its end before the kick, so reading linearly is fine.
 *   0x3........   decode: [4:0] engine codebook (NOT the ISO table number, see
 *                 bse_iso_table), [13:5] count - 1, [24:21] linbits;
 *                 pairs (big_values) or quads (count1 mode; codebook 0/1 =
 *                 table A/B). Each value x (linbits added) -> one int32
 *                 sign(x) * |x|^(4/3) in Q13 at the output pointer (see
 *                 pow43_q13; ZUNEHD_BSE_RAW=1 writes x itself, which plays
 *                 as near-silence). Count1 stops once the bits
 *                 used since the 0x64 write exceed the budget (that quad is
 *                 dropped, as in ISO 11172-3 / minimp3).
 *   0x74000001    end of decode (sets the done status bit).
 * Budget (0x64) = part2_3_length minus the scalefactor bits (part3; the AVP
 * reads the scalefactors itself and afterwards advances its own reader by the
 * budget, it never reads the engine's bit position back).
 * The Huffman tables are minimp3's (CC0), see zunehd-mp3tab.h.
 * Validated (session 9) against minimp3 with tools/mp3ref.c + tools/bsecmp.py.
 *
 * Enabled with the AVP (always present); ZUNEHD_BSELOG=1 logs per list:
 * budget, bits used and overshoot at the count1 stop (1..6 bits, a partly
 * read count1 codeword). ZUNEHD_BSEDUMP=<file> writes per list the command
 * words, the raw bitstream bytes, budget/used and the values (input of
 * tools/bsecmp.py). ZUNEHD_BSE_NOSWAP=1 reads the bitstream bytes in plain
 * memory order (wrong; kept for experiments).
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/arm/zunehd-avp.h"
#include "zunehd-mp3tab.h"

#define BSE_CC_BASE     0x6001cc00
#define BSE_D0_BASE     0x6001d000
#define IRAM_BASE       0x40000000

typedef struct ZuneBse {
    MemoryRegion cc_mr, d0_mr;
    uint32_t cc[0x100 / 4], d0[0x100 / 4];
    uint16_t reg[256];
    uint32_t sticky;            /* D018 sticky status */
    /* bitstream reader */
    uint32_t bs_addr;           /* byte address of bit 0 */
    uint64_t bs_bit;            /* bits consumed from bs_addr */
    uint64_t budget_start;
    uint32_t budget;
    bool exhausted;
    uint32_t outptr;            /* IRAM word index */
    bool swap, log, raw;
    /* stats for the log */
    unsigned lists, pairs, quads;
    int64_t last_over;
    /* ZUNEHD_BSEDUMP=<file>: per-list dump for offline validation */
    FILE *dump;
    int32_t dvals[1024];
    unsigned ndv;
} ZuneBse;

static ZuneBse *bse;

static uint8_t bs_byte(ZuneBse *b, uint64_t idx)
{
    uint32_t a = b->bs_addr + (uint32_t)idx;

    if (b->swap) {
        a ^= 3;
    }
    if (a >= 0x90000000 && a < 0xa0000000) {
        a -= 0x90000000;                /* AVP DRAM alias */
    }
    return address_space_ldub(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, NULL);
}

/* Peek n (<= 24) bits at the current position, MSB first. */
static uint32_t bs_peek(ZuneBse *b, int n)
{
    uint64_t pos = b->bs_bit;
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        v = (v << 8) | bs_byte(b, (pos >> 3) + i);
    }
    v <<= pos & 7;
    v |= bs_byte(b, (pos >> 3) + 4) >> (8 - (pos & 7));
    return n ? v >> (32 - n) : 0;
}

static void bs_skip(ZuneBse *b, int n)
{
    b->bs_bit += n;
}

static uint32_t bs_get(ZuneBse *b, int n)
{
    uint32_t v = bs_peek(b, n);
    bs_skip(b, n);
    return v;
}

/*
 * The engine also does the |x|^(4/3) of the dequantisation: the AVP's next
 * step (FUN_06498c08 in the decoder at 0x06490000) only multiplies by
 * 2^15 * 2^(-p/4) (table 2^31 * 2^(-k/4) at IRAM 0x400049e8, then
 * >> 4 * (p >> 4) + 16), p = 210 - global_gain + scalefactor terms - i.e.
 * it expects sign * |x|^(4/3) in Q13 (8206^(4/3) * 2^13 just fits in an
 * int32; the result is xr in Q28, libmad's format). Raw integers gave
 * audio about 2^14 too quiet (digital silence after the 16-bit PCM stage).
 * Validated end to end with the vector engine (zunehd-ape.c): the PCM
 * matches minimp3 with gain 1.0000 and 99 % of the samples bit-exact.
 */
static int32_t pow43_small[16];

static int32_t pow43_q13(int v)
{
    int a = v < 0 ? -v : v;
    double r;

    if (a < 16) {
        r = pow43_small[a];
    } else {
        r = pow(a, 4.0 / 3.0) * 8192.0;
        if (r > INT32_MAX) {
            r = INT32_MAX;
        }
        r = (double)lround(r);
    }
    return v < 0 ? -(int32_t)r : (int32_t)r;
}

static void out_word(ZuneBse *b, int32_t v)
{
    address_space_stl_le(&address_space_memory, IRAM_BASE + 4 * (b->outptr & 0xffff),
                         (uint32_t)(b->raw ? v : pow43_q13(v)),
                         MEMTXATTRS_UNSPECIFIED, NULL);
    b->outptr++;
    if (b->ndv < ARRAY_SIZE(b->dvals)) {
        b->dvals[b->ndv++] = v;
    }
}

/*
 * Engine codebook number (byte 2 of the AVP's 3-byte table descriptors at
 * IRAM 0x40004500 {flag, linbits, codebook}, indexed by ISO table_select;
 * entries 32/33 are the count1 tables) -> ISO 11172-3 big_values table.
 * The engine numbers its 17 distinct codebooks densely: 0/1 count1 A/B,
 * 2..4 = tables 1..3, 5..13 = tables 5..13, 14 = table 15, 15 = tables
 * 16..23, 16 = tables 24..31 (linbits come separately in the command).
 */
static const uint8_t bse_iso_table[32] = {
    0, 0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 24,
};

static void decode_pairs(ZuneBse *b, int table, int count, int linbits)
{
    const int16_t *codebook = mp3_tabs + mp3_tabindex[bse_iso_table[table & 31]];

    for (int i = 0; i < count; i++) {
        int w = 5;
        int leaf = codebook[bs_peek(b, w)];

        while (leaf < 0) {
            bs_skip(b, w);
            w = leaf & 7;
            leaf = codebook[bs_peek(b, w) - (leaf >> 3)];
        }
        bs_skip(b, leaf >> 8);
        for (int j = 0; j < 2; j++, leaf >>= 4) {
            int v = leaf & 0x0f;
            if (v == 15 && linbits) {
                v += bs_get(b, linbits);
            }
            if (v && bs_get(b, 1)) {
                v = -v;
            }
            out_word(b, v);
        }
        b->pairs++;
    }
}

static void decode_quads(ZuneBse *b, int table, int count)
{
    const uint8_t *cb = (table & 1) ? mp3_tab33 : mp3_tab32;

    for (int i = 0; i < count && !b->exhausted; i++) {
        uint64_t save = b->bs_bit;
        int leaf = cb[bs_peek(b, 4)];

        if (!(leaf & 8)) {
            int extra = leaf & 3;
            uint32_t more = (bs_peek(b, 4 + extra) & ((1u << extra) - 1));
            leaf = cb[(leaf >> 3) + more];
        }
        bs_skip(b, leaf & 7);
        if (b->bs_bit - b->budget_start > b->budget) {
            b->last_over = (int64_t)(b->bs_bit - b->budget_start) - b->budget;
            b->bs_bit = save;
            b->exhausted = true;
            break;
        }
        for (int s = 0; s < 4; s++) {
            int v = 0;
            if (leaf & (128 >> s)) {
                v = bs_get(b, 1) ? -1 : 1;
            }
            out_word(b, v);
        }
        b->quads++;
    }
}

static void bse_run_list(ZuneBse *b, uint32_t word_index)
{
    uint32_t a = IRAM_BASE + 4 * (word_index & 0xffff);
    bool decoded = false;

    b->lists++;
    b->ndv = 0;
    if (b->dump) {
        fprintf(b->dump, "L %u", b->lists);
        for (int n = 0; n < 64; n++) {
            uint32_t w = address_space_ldl_le(&address_space_memory, a + 4 * n,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
            fprintf(b->dump, " %08x", w);
            if (w == 0xffffffff) {
                break;
            }
        }
        fprintf(b->dump, "\n");
    }
    for (int n = 0; n < 4096; n++, a += 4) {
        uint32_t w = address_space_ldl_le(&address_space_memory, a,
                                          MEMTXATTRS_UNSPECIFIED, NULL);
        if (w == 0xffffffff) {
            break;
        }
        if (b->dump && (w & 0xf0000000) == 0x40000000) {
            /* raw memory bytes (no swap) from the word holding the start byte */
            uint32_t sa = address_space_ldl_le(&address_space_memory, a + 4,
                                               MEMTXATTRS_UNSPECIFIED, NULL);
            uint32_t pa = sa & ~3u;
            if (pa >= 0x90000000 && pa < 0xa0000000) {
                pa -= 0x90000000;
            }
            fprintf(b->dump, "S %08x %u ", sa, (7 - (w & 7)) & 7);
            for (int k = 0; k < 1024; k++) {
                fprintf(b->dump, "%02x", address_space_ldub(&address_space_memory, pa + k,
                                                            MEMTXATTRS_UNSPECIFIED, NULL));
            }
            fprintf(b->dump, "\n");
        }
        if ((w >> 24) == 0x7c) {
            unsigned r = (w >> 16) & 0xff;
            b->reg[r] = w & 0xffff;
            if (r == 0x64) {
                b->budget = w & 0xffff;
                b->budget_start = b->bs_bit;
                b->exhausted = false;
                b->last_over = -1;
            } else if (r == 0x66) {
                b->outptr = w & 0xffff;
            }
        } else if ((w & 0xf0000000) == 0x40000000) {
            a += 4;
            b->bs_addr = address_space_ldl_le(&address_space_memory, a,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
            b->bs_bit = (7 - (w & 7)) & 7;
        } else if ((w & 0xf0000000) == 0x30000000) {
            int table = w & 0x1f, count = ((w >> 5) & 0x1ff) + 1, linbits = (w >> 21) & 0xf;
            if (b->reg[0x44] & 2) {
                decode_quads(b, table, count);
            } else {
                decode_pairs(b, table, count, linbits);
            }
            decoded = true;
        } else if (w == 0x74000001) {
            decoded = true;
        } else {
            qemu_log_mask(LOG_UNIMP, "zunehd: bse: unknown command 0x%08x @0x%08x\n", w, a);
        }
    }
    if (decoded) {
        b->sticky |= 0x800;
        if (b->log) {
            info_report("zunehd: bse list %u: budget %u used %" PRIu64 " over %" PRId64
                        " pairs %u quads %u out %u%s", b->lists, b->budget,
                        b->bs_bit - b->budget_start, b->last_over, b->pairs, b->quads,
                        b->outptr, b->exhausted ? "" : " (budget not reached)");
        }
        if (b->dump) {
            fprintf(b->dump, "U %u %" PRIu64 " %" PRId64 " %u\nV", b->budget,
                    b->bs_bit - b->budget_start, b->last_over, b->ndv);
            for (unsigned k = 0; k < b->ndv; k++) {
                fprintf(b->dump, " %d", b->dvals[k]);
            }
            fprintf(b->dump, "\n");
            fflush(b->dump);
        }
        b->pairs = b->quads = 0;
    }
}

static uint64_t bse_cc_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneBse *b = opaque;
    return b->cc[off / 4];
}

static void bse_cc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneBse *b = opaque;

    b->cc[off / 4] = val;
    if (off == 0x14 && (val & 0x80000000)) {
        bse_run_list(b, val & 0xffff);
        b->cc[off / 4] = val & ~0x80000000u;    /* list done */
    }
}

static uint64_t bse_d0_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneBse *b = opaque;

    switch (off) {
    case 0x18: return b->sticky | 0x18;
    case 0x64: return (b->outptr & 0x7fff) << 16;
    }
    return b->d0[off / 4];
}

static void bse_d0_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneBse *b = opaque;

    if (off == 0x18) {
        b->sticky &= ~(uint32_t)val;            /* W1C */
        return;
    }
    b->d0[off / 4] = val;
}

static const MemoryRegionOps bse_cc_ops = {
    .read = bse_cc_read, .write = bse_cc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static const MemoryRegionOps bse_d0_ops = {
    .read = bse_d0_read, .write = bse_d0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

void zune_bse_init(MemoryRegion *sysmem)
{
    ZuneBse *b = g_new0(ZuneBse, 1);

    b->log = getenv("ZUNEHD_BSELOG") != NULL;
    b->swap = getenv("ZUNEHD_BSE_NOSWAP") == NULL;
    b->raw = getenv("ZUNEHD_BSE_RAW") != NULL;
    for (int i = 0; i < 16; i++) {
        pow43_small[i] = (int32_t)lround(pow(i, 4.0 / 3.0) * 8192.0);
    }
    if (getenv("ZUNEHD_BSEDUMP")) {
        b->dump = fopen(getenv("ZUNEHD_BSEDUMP"), "w");
        if (!b->dump) {
            error_report("zunehd: bse: cannot open ZUNEHD_BSEDUMP file %s",
                         getenv("ZUNEHD_BSEDUMP"));
        }
    }
    memory_region_init_io(&b->cc_mr, NULL, &bse_cc_ops, b, "zunehd.bse-cc", 0x100);
    memory_region_add_subregion(sysmem, BSE_CC_BASE, &b->cc_mr);
    memory_region_init_io(&b->d0_mr, NULL, &bse_d0_ops, b, "zunehd.bse-d0", 0x100);
    memory_region_add_subregion(sysmem, BSE_D0_BASE, &b->d0_mr);
    bse = b;
}
