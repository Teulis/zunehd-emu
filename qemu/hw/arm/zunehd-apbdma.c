/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD (Tegra AP16): APB DMA controller + I2S audio sink -> QEMU audio.
 *
 * Layout (seen in session 9 MMIO logs; same as Tegra2):
 *   0x6000A000  global: +0x000 CMD ([31] GEN enable); +0x020 / +0x024 channel
 *               interrupt enables, modelled as CPU (ICTLR 26) / AVP (ICTLR 60,
 *               INT_APB_DMA_COP) masks - the CPU OAL writes all 16 bits to
 *               both at boot (UNVERIFIED reading; Tegra2 has MASK/SET/CLR here).
 *               +0x014 / +0x018 read back the pending channels per processor.
 * The AVP kernel (FUN_060ae090 queue, FUN_060adbfc start) keeps a 16-entry
 * ring of PCM buffers; each buffer of S bytes plays as two halves of S/2
 * (WCOUNT = S/8 - 1); its EOC handler writes the NEXT buffer's address to
 * AHB_PTR while the channel runs, which takes effect at the buffer wrap.
 *   0x6000B000 + 0x20 * n  channel n (16 channels):
 *     +0x00 CSR  [31] ENB [30] IE_EOC [29] HOLD [28] DIR (1 = AHB -> APB,
 *                memory to peripheral) [27] ONCE [21] FLOW [20:16] REQ_SEL
 *                [15:2] WCOUNT (words - 1)
 *     +0x04 STA  [31] BSY [30] ISE_EOC (W1C) [28] PING_STATUS [15:2] COUNT
 *     +0x10 AHB_PTR  memory address (the AVP passes its SDRAM alias
 *                0x9xxxxxxx = phys + 0x90000000)
 *     +0x14 AHB_SEQ  [31] INTR_ENB [30:28] bus width [19] DBL_BUF ...
 *     +0x18 APB_PTR  peripheral FIFO (I2S1 FIFO1 = 0x70002840)
 *     +0x1C APB_SEQ
 * The AVP's audio mixer (nvddk_audiomixer_core) uses channel 15: CSR
 * 0xD0220FFC (ENB, IE_EOC, DIR, continuous, REQ 2, 1024 words), AHB_SEQ
 * 0x25080000 (32-bit, DBL_BUF), APB_SEQ 0x20010000, APB_PTR 0x70002840.
 * Double-buffer continuous mode (as in NVIDIA's old mach-tegra dma.c
 * TEGRA_DMA_MODE_CONTINUOUS_DOUBLE): the buffer holds 2 halves of
 * WCOUNT + 1 words; EOC after each half, PING_STATUS toggles.
 *
 * A channel writing to an I2S FIFO is consumed in virtual time at the
 * audio rate (ZUNEHD_AUDIO_RATE, default 44100), one 32-bit word = one
 * stereo frame of two signed 16-bit samples (L low half), and fed to the
 * default QEMU audiodev (e.g. -audiodev dsound,id=snd0 or
 * -audiodev wav,id=snd0,path=out.wav). A half is not completed while the
 * previous half's EOC is still pending (the stream stalls instead), so a
 * slow emulation never replays a buffer the AVP has not refilled yet.
 * ZUNEHD_AUDIO_PACE=<frames/s> (default = ZUNEHD_AUDIO_RATE) sets the
 * consumption speed in virtual time separately from the host voice rate:
 * the emulated MP3 pipeline reaches only ~70 % of real time (session 9),
 * so at 44100 the AVP mixer underruns (512-frame silences + skipped audio
 * in its own buffers); with ZUNEHD_AUDIO_PACE=22050 and a wav audiodev
 * (which writes only what it gets) the recording is gapless and pitch-
 * correct. ZUNEHD_PCMDUMP=<file> writes every consumed frame (raw s16
 * stereo). ZUNEHD_DMALOG=1 logs register
 * accesses and stream events to stderr.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "qemu/audio.h"
#include "target/arm/cpu.h"
#include "hw/arm/zunehd-avp.h"

#define DMA_GLOBAL_BASE 0x6000a000
#define DMA_CH_BASE     0x6000b000
#define DMA_NCH         16
#define TICK_NS         (2 * 1000 * 1000)       /* 2 ms pacing tick */
#define RING_FRAMES     (1 << 15)               /* host-side buffer, ~0.75 s */

#define CSR_ENB     (1u << 31)
#define CSR_IE_EOC  (1u << 30)
#define CSR_HOLD    (1u << 29)
#define CSR_DIR     (1u << 28)
#define CSR_ONCE    (1u << 27)
#define STA_BSY     (1u << 31)
#define STA_ISE_EOC (1u << 30)
#define STA_PING    (1u << 28)
#define SEQ_DBL_BUF (1u << 19)

typedef struct DmaCh {
    uint32_t csr, sta, ahb_ptr, ahb_seq, apb_ptr, apb_seq;
    bool audio;                 /* running as an I2S audio stream */
    uint32_t cur_ahb;           /* AHB_PTR latched at start / buffer wrap */
    uint32_t pos;               /* words done in the current half */
    int half;                   /* 0/1 in double-buffer mode */
    double frac;                /* fractional frames carried between ticks */
} DmaCh;

typedef struct ZuneDma {
    MemoryRegion gmr, cmr;
    uint32_t glob[0x1000 / 4];
    DmaCh ch[DMA_NCH];
    qemu_irq irq, cop_irq;
    QEMUTimer *timer;
    int64_t last_ns;
    int rate;                   /* host voice rate (ZUNEHD_AUDIO_RATE) */
    int pace;                   /* DMA consumption, frames per virtual second */
    bool log;
    FILE *pcmdump;              /* ZUNEHD_PCMDUMP: every frame the DMA consumes */
    /* host audio */
    AudioBackend *be;
    SWVoiceOut *voice;
    uint32_t ring[RING_FRAMES];
    unsigned rd, wr;            /* free-running indices */
    uint64_t frames_total, frames_dropped, stalls;
    int64_t peak;
} ZuneDma;

static ZuneDma *zdma;

/* Channels with a pending, enabled EOC interrupt (bit n = channel n). */
static uint32_t dma_pending(ZuneDma *d)
{
    uint32_t p = 0;

    for (int i = 0; i < DMA_NCH; i++) {
        if ((d->ch[i].sta & STA_ISE_EOC) && (d->ch[i].csr & CSR_IE_EOC)) {
            p |= 1u << i;
        }
    }
    return p;
}

/*
 * Interrupt enables per processor (AP16 reading of the boot sequence: the
 * CPU OAL writes every channel bit to 0x24 and then to 0x20): 0x20 = CPU
 * channel mask (ICTLR 26, INT_APB_DMA), 0x24 = AVP channel mask (ICTLR 60,
 * INT_APB_DMA_COP). Each processor's driver only services its own channels.
 */
static void dma_update_irq(ZuneDma *d)
{
    uint32_t p = dma_pending(d);

    qemu_set_irq(d->irq, (p & d->glob[0x20 / 4]) != 0);
    qemu_set_irq(d->cop_irq, (p & d->glob[0x24 / 4]) != 0);
}

static hwaddr dma_mem_addr(uint32_t a)
{
    return (a >= 0x90000000u && a < 0xa0000000u) ? a - 0x90000000u : a;
}

static bool dma_is_audio(DmaCh *c)
{
    return (c->csr & CSR_DIR) && (c->apb_ptr & 0xfffffe00u) == 0x70002800u;
}

static uint32_t dma_half_words(DmaCh *c)
{
    return ((c->csr >> 2) & 0x3fff) + 1;
}

/* ---- host audio ring --------------------------------------------------- */

static void ring_push(ZuneDma *d, uint32_t frame)
{
    if (d->wr - d->rd >= RING_FRAMES) {
        d->rd++;                                /* overrun: drop oldest */
        d->frames_dropped++;
    }
    d->ring[d->wr++ % RING_FRAMES] = frame;
}

static void audio_out_cb(void *opaque, int avail)
{
    ZuneDma *d = opaque;
    uint8_t buf[4096];

    while (avail >= 4 && d->rd != d->wr) {
        int n = 0;
        while (n + 4 <= (int)sizeof(buf) && n + 4 <= avail && d->rd != d->wr) {
            uint32_t f = d->ring[d->rd++ % RING_FRAMES];
            memcpy(buf + n, &f, 4);             /* host is little-endian */
            n += 4;
        }
        size_t w = audio_be_write(d->be, d->voice, buf, n);
        avail -= w;
        if (w < (size_t)n) {
            d->rd -= (n - w) / 4;               /* give back what was not taken */
            break;
        }
    }
}

static void audio_open(ZuneDma *d)
{
    struct audsettings as = { .freq = d->rate, .nchannels = 2,
                              .fmt = AUDIO_FORMAT_S16, .big_endian = false };
    Error *err = NULL;

    if (d->voice) {
        return;
    }
    if (!d->be) {
        /* An explicit -audiodev is not a default backend: look it up by id. */
        const char *id = getenv("ZUNEHD_AUDIODEV");
        d->be = audio_be_by_name(id ? id : "snd0", NULL);
    }
    if (!d->be && !audio_be_check(&d->be, &err)) {
        warn_report("zunehd: audio: no audio backend (%s); sound is dropped",
                    err ? error_get_pretty(err) : "?");
        error_free(err);
        d->be = NULL;
        return;
    }
    d->voice = audio_be_open_out(d->be, NULL, "zunehd.i2s1", d, audio_out_cb, &as);
    if (d->voice) {
        audio_be_set_active_out(d->be, d->voice, true);
        info_report("zunehd: audio: I2S1 -> host audio, %d Hz S16 stereo", d->rate);
    }
}

/* ---- stream engine ---------------------------------------------------------- */

/* Consume up to 'frames' frames; returns the number consumed (less on a stall). */
static uint64_t dma_audio_step(ZuneDma *d, DmaCh *c, int idx, uint64_t frames)
{
    uint64_t done = 0;
    uint32_t hw = dma_half_words(c);
    bool dbl = c->ahb_seq & SEQ_DBL_BUF;
    while (frames--) {
        hwaddr a;
        /*
         * Never finish a half while the previous half's EOC is still pending:
         * when the emulation falls behind, one timer step can cover several
         * halves, and the DMA would wrap and replay a buffer before the AVP's
         * EOC handler has queued the next one (heard as repeated ~10 ms
         * chunks). Stall instead; real hardware never gets that far ahead.
         */
        if (c->pos + 1 >= hw && (c->sta & STA_ISE_EOC) && (c->csr & CSR_IE_EOC)) {
            d->stalls++;
            break;
        }
        a = dma_mem_addr(c->cur_ahb) + 4ull * ((dbl && c->half ? hw : 0) + c->pos);
        uint32_t f = address_space_ldl_le(&address_space_memory, a,
                                          MEMTXATTRS_UNSPECIFIED, NULL);
        int16_t l = f & 0xffff, r = f >> 16;
        int64_t m = MAX(ABS((int)l), ABS((int)r));

        d->peak = MAX(d->peak, m);
        ring_push(d, f);
        if (d->pcmdump) {
            fwrite(&f, 4, 1, d->pcmdump);           /* host is little-endian */
        }
        d->frames_total++;
        done++;
        if (++c->pos >= hw) {
            c->pos = 0;
            c->sta |= STA_ISE_EOC;
            if (dbl) {
                c->half ^= 1;
                c->sta ^= STA_PING;
            }
            if (!dbl || c->half == 0) {
                c->cur_ahb = c->ahb_ptr;        /* buffer wrap: reload AHB_PTR */
            }
            if (d->log) {
                fprintf(stderr, "apbdma: %lld ch%d EOC half %d (frames %llu, peak %lld)\n",
                        (long long)qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), idx, c->half,
                        (unsigned long long)d->frames_total, (long long)d->peak);
            }
            d->peak = 0;
            if (c->csr & CSR_ONCE) {
                c->csr &= ~CSR_ENB;
                c->sta &= ~STA_BSY;
                c->audio = false;
                break;
            }
        }
    }
    c->sta = (c->sta & ~0xfffcu) | ((c->pos << 2) & 0xfffcu);
    return done;
}

static void dma_timer_cb(void *opaque)
{
    ZuneDma *d = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double dt = (now - d->last_ns) / 1e9;
    bool any = false;

    d->last_ns = now;
    if (dt > 0.25) {
        dt = 0.25;                              /* after a pause: do not burst */
    }
    for (int i = 0; i < DMA_NCH; i++) {
        DmaCh *c = &d->ch[i];
        if (!c->audio) {
            continue;
        }
        any = true;
        if (c->csr & CSR_HOLD) {
            continue;
        }
        /* frames owed in virtual time; a backlog (stall / late tick) of up to 0.25 s is kept */
        c->frac += dt * d->pace;
        if (c->frac > 0.25 * d->pace) {
            c->frac = 0.25 * d->pace;
        }
        c->frac -= dma_audio_step(d, c, i, (uint64_t)c->frac);
    }
    dma_update_irq(d);
    if (any) {
        timer_mod_ns(d->timer, now + TICK_NS);
    }
}

static void dma_ch_csr_write(ZuneDma *d, int idx, uint32_t val)
{
    DmaCh *c = &d->ch[idx];
    bool was = c->csr & CSR_ENB;

    c->csr = val;
    if ((val & CSR_ENB) && !was) {
        c->pos = 0;
        c->half = 0;
        c->frac = 0;
        c->sta = (c->sta & ~(STA_PING | 0xfffcu)) | STA_BSY;
        c->audio = dma_is_audio(c);
        c->cur_ahb = c->ahb_ptr;
        if (c->audio) {
            audio_open(d);
            if (d->log) {
                fprintf(stderr, "apbdma: ch%d audio start mem 0x%08x -> 0x%08x, %u words/half%s\n",
                        idx, c->ahb_ptr, c->apb_ptr, dma_half_words(c),
                        (c->ahb_seq & SEQ_DBL_BUF) ? " x2 (double buffer)" : "");
            }
            d->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod_ns(d->timer, d->last_ns + TICK_NS);
        }
    } else if (!(val & CSR_ENB) && was) {
        c->sta &= ~STA_BSY;
        if (c->audio && d->log) {
            fprintf(stderr, "apbdma: ch%d stop (frames %llu, dropped %llu, stalls %llu)\n", idx,
                    (unsigned long long)d->frames_total,
                    (unsigned long long)d->frames_dropped, (unsigned long long)d->stalls);
        }
        c->audio = false;
    }
    dma_update_irq(d);
}

/* ---- MMIO ------------------------------------------------------------------- */

static uint64_t dma_ch_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneDma *d = opaque;
    int idx = off / 0x20;
    DmaCh *c = &d->ch[idx % DMA_NCH];
    uint32_t v = 0;

    if (idx >= DMA_NCH) {
        return 0;
    }
    switch (off & 0x1f) {
    case 0x00: v = c->csr; break;
    case 0x04: v = c->sta; break;
    case 0x10: v = c->ahb_ptr; break;
    case 0x14: v = c->ahb_seq; break;
    case 0x18: v = c->apb_ptr; break;
    case 0x1c: v = c->apb_seq; break;
    }
    if (d->log && idx == 15) {
        fprintf(stderr, "apbdma: %s ch%d read  +0x%02x -> 0x%08x pc~0x%08x\n",
                zune_avp_is_current() ? "AVP" : "CPU", idx, (unsigned)(off & 0x1f), v,
                current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    return v;
}

static void dma_ch_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneDma *d = opaque;
    int idx = off / 0x20;
    DmaCh *c;

    if (idx >= DMA_NCH) {
        return;
    }
    c = &d->ch[idx];
    if (d->log && (idx == 15 || (off & 0x1f) == 0)) {
        fprintf(stderr, "apbdma: %s ch%d write +0x%02x <- 0x%08x\n",
                zune_avp_is_current() ? "AVP" : "CPU", idx, (unsigned)(off & 0x1f),
                (uint32_t)val);
    }
    switch (off & 0x1f) {
    case 0x00: dma_ch_csr_write(d, idx, val); break;
    case 0x04:
        if (val & STA_ISE_EOC) {
            c->sta &= ~STA_ISE_EOC;             /* W1C */
            dma_update_irq(d);
        }
        break;
    case 0x10: c->ahb_ptr = val; break;
    case 0x14: c->ahb_seq = val; break;
    case 0x18: c->apb_ptr = val; break;
    case 0x1c: c->apb_seq = val; break;
    }
}

static uint64_t dma_glob_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneDma *d = opaque;
    uint32_t v = d->glob[off / 4];

    if (off == 0x14) {
        v = dma_pending(d) & d->glob[0x20 / 4];     /* pending for the CPU (guess) */
    } else if (off == 0x18) {
        v = dma_pending(d) & d->glob[0x24 / 4];     /* pending for the AVP (guess) */
    }

    if (d->log) {
        fprintf(stderr, "apbdma: %s global read  +0x%03x -> 0x%08x\n",
                zune_avp_is_current() ? "AVP" : "CPU", (unsigned)off, v);
    }
    return v;
}

static void dma_glob_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneDma *d = opaque;

    if (d->log && (zune_avp_is_current() || (off != 0x20 && off != 0x24))) {
        fprintf(stderr, "apbdma: %s global write +0x%03x <- 0x%08x\n",
                zune_avp_is_current() ? "AVP" : "CPU", (unsigned)off, (uint32_t)val);
    }
    if (off == 0x20 || off == 0x24) {
        d->glob[off / 4] |= val;                /* per-processor channel enables */
    } else {
        d->glob[off / 4] = val;
    }
    dma_update_irq(d);
}

static const MemoryRegionOps dma_ch_ops = {
    .read = dma_ch_read, .write = dma_ch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static const MemoryRegionOps dma_glob_ops = {
    .read = dma_glob_read, .write = dma_glob_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

void zune_apbdma_init(MemoryRegion *sysmem, qemu_irq irq, qemu_irq cop_irq)
{
    ZuneDma *d = g_new0(ZuneDma, 1);
    const char *r = getenv("ZUNEHD_AUDIO_RATE");

    d->irq = irq;
    d->cop_irq = cop_irq;
    d->log = getenv("ZUNEHD_DMALOG") != NULL;
    if (getenv("ZUNEHD_PCMDUMP")) {
        d->pcmdump = fopen(getenv("ZUNEHD_PCMDUMP"), "wb");
        if (!d->pcmdump) {
            error_report("zunehd: apbdma: cannot open ZUNEHD_PCMDUMP file %s",
                         getenv("ZUNEHD_PCMDUMP"));
        }
    }
    d->rate = r ? atoi(r) : 44100;
    if (d->rate < 8000 || d->rate > 96000) {
        d->rate = 44100;
    }
    d->pace = getenv("ZUNEHD_AUDIO_PACE") ? atoi(getenv("ZUNEHD_AUDIO_PACE")) : d->rate;
    if (d->pace < 1000 || d->pace > 96000) {
        d->pace = d->rate;
    }
    d->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma_timer_cb, d);
    memory_region_init_io(&d->gmr, NULL, &dma_glob_ops, d, "zunehd.apbdma", 0x1000);
    memory_region_add_subregion_overlap(sysmem, DMA_GLOBAL_BASE, &d->gmr, 10);
    memory_region_init_io(&d->cmr, NULL, &dma_ch_ops, d, "zunehd.apbdma-ch", 0x1000);
    memory_region_add_subregion_overlap(sysmem, DMA_CH_BASE, &d->cmr, 10);
    zdma = d;
}
