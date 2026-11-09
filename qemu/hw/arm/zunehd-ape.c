/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD (Tegra AP16): the AVP's "audio vector engine" (APE) at 0x6000E000,
 * which the AVP MP3 decoder (library at 0x06490000) uses for the whole
 * hybrid synthesis: IMDCT 36/12 + windowing + overlap-add, frequency
 * inversion, DCT32 and the polyphase window, writing 16-bit stereo PCM.
 * Reverse-engineered in session 9 from the programs the decoder builds
 * (FUN_06497d0c/FUN_0649cb20/FUN_0649eb70 ...) and validated numerically:
 * 300 consecutive real kicks (Up_30s.mp3, long + start/stop + short
 * blocks) reproduce minimp3's PCM with 99.3 % of the samples bit-exact,
 * the rest +-1 LSB (SNR 98 dB), see tools/apesim.c.
 * The interpreter itself is in zunehd-ape.h (shared with tools/apesim.c).
 *
 * Registers (only these three are used per frame):
 *   +0x74  program address (IRAM byte address)
 *   +0x70  kick: 0xA0000000 | (words - 1). The program runs synchronously
 *          on the write; [31] reads back 0 (done).
 *   +0x78  read by FUN_0649baa0 before building the next program
 *          (wait/fence; the value is only stored): reads 0 = idle.
 *   others are stored and read back.
 *
 * Program (IRAM words) = sequence of instructions:
 *   header 0x00FFMMLL, then operand words in this order:
 *     LL bits 0..2  -> L0, L1, L2   (per-slot address override, see below)
 *     MM bits 0..7  -> slot0.addr, slot0.desc, slot1.addr, slot1.desc,
 *                      slot2.addr, slot2.desc, slot3.addr, slot3.desc
 *     FF bit 0      -> P0, bit 1 -> P1 (2D inner lengths), bit 5 -> SH
 *     (FF bit 6 always set, meaning unknown)
 *   then one control word (top nibble 8 or A).
 *   All registers persist between instructions (most instructions only
 *   reload one or two addresses). Each instruction restarts every slot's
 *   address stream at its base address.
 *
 * Slot descriptor: [31:28] flags, [27:16] stride (words), [11:0] count - 1.
 *   flags bit 2 = 2D: runs of n elements with step +1 (bit 0: -1), the run
 *   start advancing by "stride"; n = P field + 1, P0 = slot0 <<16 | slot1,
 *   P1 = slot2 << 16 | slot3 (16-bit fields: [11:0] n - 1, [13:12] = 3:
 *   "both ends inward" order 0, n-1, 1, n-2, ... used for the window
 *   output y[i] / y[17-i]). Without bit 2: 1D, step = stride (bit 0:
 *   negative). Element indices wrap modulo count (count 1 = a constant).
 *   flags bit 3 (always with bit 2) unknown.          [confidence: high]
 * L0..L2 (slot 0..2 override, [31:28] bit 0 = on): [27:16] n - 1, [15:0]
 *   outer step (words): runs of n elements at the descriptor step, then
 *   outer step. Flags 9 = additionally circular in an aligned window of
 *   n * stride words (the 2 x 512-word polyphase V rings at 0x40007000),
 *   flags b = not circular (short-block IMDCT gather/scatter). [high]
 * SH: [15:8] right shift of the result, [7:0] saturation (signed, value+1
 *   bits: 0x1F = 32 bit, 0x0F = 16 bit PCM).                    [high]
 *
 * Control word: [31:28] 8/A ([29] maybe "wait for previous"; ignored),
 *   [27:24] class, [23:20] write mode (0 = word, 2 = low halfword,
 *   3 = high halfword: L / R PCM into the same staging word), [19:16]
 *   unknown (2 only in an unused PCM pack sequence), [15:12] op,
 *   [11:8] bit 1 = round (add 1 << (shift-1) before the shift; required
 *   for bit-exact PCM), [7:0] class specific.
 *   class 2, element-wise over the destination count, a = slot0, b = slot1:
 *     op 0: slot2 = a * b         (64-bit product >> shift; also "clear"
 *                                  with a = 0 and "x2" with a = 2)  [high]
 *     op 1: slot2 = a + b         (copies use a = constant 0)      [high]
 *     op 2: slot3 = a - b         (IMDCT pre-differences, frequency
 *                                  inversion with a = 0)            [high]
 *     op 3: slot2 = a + b, slot3 = a - b (DCT32 butterflies)       [high]
 *   class 1, multiply-accumulate: [7:4] = nd - 1 data taps (slot1),
 *     [3:0] = nc - 1 coefficients (slot0) per group; each group reads nd
 *     data words and produces nc / nd outputs (slot2), output j =
 *     sum_t coef[next] * data[t] (64-bit) >> shift. Used for the 9x9 and
 *     6x6 IMDCT matrices (0x88, 0x55), the 2-tap window butterfly 0x13
 *     (2 outputs y[i], y[17-i] per data pair), 0x11, and the 16/8-tap
 *     polyphase window (0xFF, 0x77).                                [high]
 *   class 7, DCT32 recombination (Lee): [7:0] = stage s, group n = 2^(s+1)
 *     (from slot1's 2D run length); slot1 and slot2 run backwards through
 *     each group; in natural order out[2k] = in[k], out[2k+1] =
 *     in[n/2+k] + in[n/2+k+1] (last one without the add).           [high]
 *   Opcode/control combinations seen: 00603f00/82001000 (copy), a2000000,
 *   a2002200, a2002000, a2001000, a2001200, a2000200, a2003200, a1000088,
 *   a1000055, a1000213, a1000011, a1000277, a12002ff/a13002ff,
 *   a1200277/a1300277, a2201200/a2301200, a7000001..a7000004.
 *
 * Decoder data flow (per granule kick, both channels, ctx 0x400117a0):
 *   spectra 1152 words (0x40008010 / 0x40009210 double buffer, antialiased
 *   on the AVP CPU, BSE Q13 pow43 x dequant) -> W 0x40011808 -> IMDCT ->
 *   x/4 windowed, overlap 0x40012acc (+0x900 for ch1) -> frequency
 *   inversion -> transpose x2 into 0x400141a4 [18][32] -> DCT32 -> V ring
 *   0x40007000 (ch0) / 0x40007800 (ch1), 32 words per slot, rotated by 64
 *   words per granule -> window -> PCM staging 0x400101a4, 576 words
 *   (L low half, R high half), which IRAM 0x40017bf8 then copies into the
 *   NvMM output buffer (the APB DMA ring). Not exercised yet: mixed
 *   blocks (block types 1/2 with mixed_block_flag) and mono programs.
 *
 * Debug: ZUNEHD_APELOG=1 logs each kick; ZUNEHD_APEDUMP=<dir> writes
 * <dir>/kNNNNN.bin per kick (16-byte header 'APEK', program address,
 * +0x70 value, kick number; then the 256 KiB IRAM before execution;
 * ZUNEHD_APEDUMP_MAX limits the count, default 300) for tools/apesim.c;
 * ZUNEHD_APE_OFF=1 does not execute programs (old silent behaviour).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/arm/zunehd-avp.h"
#include "zunehd-ape.h"

#define APE_BASE        0x6000e000
#define IRAM_SIZE       0x40000

typedef struct ZuneApe {
    MemoryRegion mr;
    uint32_t reg[0x100 / 4];
    unsigned kicks;
    const char *dumpdir;
    unsigned dumpmax;
    bool log, off;
    uint32_t *iram;             /* working copy of IRAM (little-endian words) */
    uint8_t *dirty;
    ApeCore core;
} ZuneApe;

static void ape_dump(ZuneApe *a, uint32_t addr, uint32_t ctrl)
{
    char path[1024];
    FILE *f;
    uint32_t hdr[4] = { 0x4b455041, addr, ctrl, a->kicks };

    snprintf(path, sizeof(path), "%s/k%05u.bin", a->dumpdir, a->kicks);
    f = fopen(path, "wb");
    if (!f) {
        error_report("zunehd: ape: cannot write %s", path);
        a->dumpdir = NULL;
        return;
    }
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(a->iram, IRAM_SIZE, 1, f);
    fclose(f);
}

static void ape_kick(ZuneApe *a, uint32_t ctrl)
{
    uint32_t addr = a->reg[0x74 / 4], nwords = (ctrl & 0xffff) + 1;
    unsigned insns, written = 0;

    a->kicks++;
    address_space_read(&address_space_memory, APE_IRAM_BASE, MEMTXATTRS_UNSPECIFIED,
                       a->iram, IRAM_SIZE);
    for (unsigned i = 0; i < APE_IRAM_WORDS; i++) {
        a->iram[i] = le32_to_cpu(a->iram[i]);
    }
    if (a->dumpdir && a->kicks <= a->dumpmax) {
        ape_dump(a, addr, ctrl);
    }
    if (a->off) {
        return;
    }
    memset(a->dirty, 0, APE_IRAM_WORDS);
    a->core.nbad = 0;
    insns = a->core.ninsn;
    ape_run(&a->core, addr, nwords);
    insns = a->core.ninsn - insns;
    /* write back the words the program wrote, in contiguous runs */
    for (unsigned i = 0; i < APE_IRAM_WORDS;) {
        unsigned j;
        if (!a->dirty[i]) {
            i++;
            continue;
        }
        for (j = i; j < APE_IRAM_WORDS && a->dirty[j]; j++) {
            a->iram[j] = cpu_to_le32(a->iram[j]);
        }
        address_space_write(&address_space_memory, APE_IRAM_BASE + 4 * i,
                            MEMTXATTRS_UNSPECIFIED, &a->iram[i], 4 * (j - i));
        written += j - i;
        i = j;
    }
    if (a->core.nbad) {
        qemu_log_mask(LOG_GUEST_ERROR, "zunehd: ape kick %u (prog 0x%08x): %u bad "
                      "operations (unknown control word / address outside IRAM)\n",
                      a->kicks, addr, a->core.nbad);
    }
    if (a->log) {
        info_report("zunehd: ape kick %u: prog 0x%08x words %u insns %u written %u bad %u",
                    a->kicks, addr, nwords, insns, written, a->core.nbad);
    }
}

static uint64_t ape_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneApe *a = opaque;

    if (off == 0x78) {
        return 0;
    }
    if (off == 0x70) {
        return a->reg[off / 4] & ~0x80000000u;
    }
    return a->reg[(off & 0xff) / 4];
}

static void ape_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneApe *a = opaque;

    a->reg[(off & 0xff) / 4] = val;
    if (off == 0x70 && (val & 0x80000000)) {
        ape_kick(a, val);
    }
}

static const MemoryRegionOps ape_ops = {
    .read = ape_read, .write = ape_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

void zune_ape_init(MemoryRegion *sysmem)
{
    ZuneApe *a = g_new0(ZuneApe, 1);
    const char *m = getenv("ZUNEHD_APEDUMP_MAX");

    a->log = getenv("ZUNEHD_APELOG") != NULL;
    a->off = getenv("ZUNEHD_APE_OFF") != NULL;
    a->dumpdir = getenv("ZUNEHD_APEDUMP");
    a->dumpmax = m ? strtoul(m, NULL, 0) : 300;
    a->iram = g_malloc0(IRAM_SIZE);
    a->dirty = g_malloc0(APE_IRAM_WORDS);
    a->core.iram = a->iram;
    a->core.dirty = a->dirty;
    memory_region_init_io(&a->mr, NULL, &ape_ops, a, "zunehd.ape", 0x100);
    memory_region_add_subregion_overlap(sysmem, APE_BASE, &a->mr, 10);
}
