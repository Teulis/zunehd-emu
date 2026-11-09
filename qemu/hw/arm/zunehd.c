/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Microsoft Zune HD (NVIDIA Tegra APX 2600 / "AP16", ARM1136JF-S) board.
 *
 * Bring-up model for running the stock Windows Embedded CE 6 image.
 * Everything here is derived from the firmware's own OAL (nk.exe):
 *   - OEMAddressTable @ VA 0x8004EAD0 gives the physical regions below.
 *   - StartUp @ PA 0x23FB0 runs with MMU off and needs no hardware.
 *   - OEMInitDebugSerial (0x80053A18) requires a boot-args block from the
 *     boot loader at PA 0x1000 ('BOOT' @+0x10, size 0x1FC @+0x14,
 *     'ARGS' @+0x110, u16 1 @+0x114, u16 version 0xD @+0x116), otherwise it
 *     hangs printing "Invalid or incompatible boot arguments".
 *
 * Peripheral blocks that have not been reverse engineered yet are covered by
 * a logging register-file stub (reads return the last written value), so a
 * run with "-d unimp" shows exactly what the OAL touches.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/sysbus.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "hw/core/qdev-properties.h"
#include "hw/cpu/cluster.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/rtc.h"
#include "target/arm/cpu.h"
#include "ui/console.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"
#include "hw/arm/zunehd-gr3d.h"
#include "hw/arm/zunehd-usb.h"
#include "hw/arm/zunehd-avp.h"

#define ZUNEHD_SDRAM_SIZE   (128 * MiB)
#define ZUNEHD_IRAM_BASE    0x40000000
#define ZUNEHD_IRAM_SIZE    (256 * KiB)
#define ZUNEHD_BOOTARGS_PA  0x00001000
#define ZUNEHD_INITIAL_SP   0x00013ff0

/* Tegra timer block (PPSB). TMRUS is the free-running 1 MHz counter. */
#define ZUNEHD_TMR_BASE     0x60005000
#define TMR_TMRUS_CNTR      0x10
#define TMR_TMRUS_CFG       0x14

struct ZuneHDMachineState {
    MachineState parent;
    CPUClusterState cpu_cluster;
    ARMCPU *cpu;
    qemu_irq *ictlr_lines;   /* 96 ICTLR inputs */
    uint32_t entry;
    uint32_t emmc_instance;  /* SDMMC instance reported as the boot eMMC */
    char *ext_filename;
};

#define TYPE_ZUNEHD_MACHINE MACHINE_TYPE_NAME("zunehd")
OBJECT_DECLARE_SIMPLE_TYPE(ZuneHDMachineState, ZUNEHD_MACHINE)

/* ---------------------------------------------------------------------- */
/* Logging register-file stub                                              */

/* Guest PC for logs. TB-granular (start of the translation block), which is
 * enough to locate the accessing function in the disassembly. */
static uint32_t zune_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

typedef struct ZuneStub {
    MemoryRegion mr;
    const char *name;
    hwaddr base;
    GHashTable *regs;   /* offset -> value (32-bit) */
    GHashTable *wclear; /* offset -> bits that self-clear after a write */
    GHashTable *wset;   /* offset -> bits that always read back as 1 */
} ZuneStub;

static ZuneStub *zune_apb_stub;     /* APB_MISC block (USB PHY wake-up hook) */
static ZuneStub *zune_ppsb_stub;    /* 0x60000000 block (CAR COP reset hook) */

static uint64_t zune_stub_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneStub *s = opaque;
    gpointer v = g_hash_table_lookup(s->regs, GUINT_TO_POINTER(off & ~3u));
    uint32_t val = GPOINTER_TO_UINT(v);

    val >>= (off & 3) * 8;
    if (size < 4) {
        val &= (1u << (size * 8)) - 1;
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: %s%s read%u  [0x%08" HWADDR_PRIx
                  "] -> 0x%08x  pc~0x%08x\n", zune_cpu_tag(), s->name, size * 8,
                  s->base + off, val, zune_guest_pc());
    return val;
}

static void zune_stub_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    ZuneStub *s = opaque;
    hwaddr key = off & ~3u;
    uint32_t old = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                                        GUINT_TO_POINTER(key)));
    unsigned sh = (off & 3) * 8;
    uint32_t mask = size >= 4 ? 0xffffffffu : (((1u << (size * 8)) - 1) << sh);
    uint32_t nv = (old & ~mask) | (((uint32_t)val << sh) & mask);

    /* "Hardware" completes self-clearing commands instantly. */
    nv &= ~GPOINTER_TO_UINT(g_hash_table_lookup(s->wclear,
                                                GUINT_TO_POINTER(key)));
    nv |= GPOINTER_TO_UINT(g_hash_table_lookup(s->wset, GUINT_TO_POINTER(key)));
    if (s == zune_apb_stub && key == 0x70) {
        nv = zune_usb_apb_wake_write(old, nv);     /* USB1 VBUS wake-up */
    }
    g_hash_table_insert(s->regs, GUINT_TO_POINTER(key), GUINT_TO_POINTER(nv));
    if (s == zune_apb_stub && key == 0xa30) {
        zune_usb_apb_phy_write(nv);                 /* USB1 PHY suspend */
    }
    if (s == zune_ppsb_stub && (key == 0x6004 || key == 0x6300 || key == 0x6304)) {
        zune_avp_car_write(key - 0x6000, key == 0x6004 ? nv : (uint32_t)val);
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: %s%s write%u [0x%08" HWADDR_PRIx
                  "] <- 0x%08" PRIx64 "  pc~0x%08x\n", zune_cpu_tag(), s->name,
                  size * 8, s->base + off, val, zune_guest_pc());
}

static const MemoryRegionOps zune_stub_ops = {
    .read = zune_stub_read,
    .write = zune_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static ZuneStub *zune_stub_create(const char *name, hwaddr base, uint64_t size)
{
    ZuneStub *s = g_new0(ZuneStub, 1);

    s->name = name;
    s->base = base;
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->wclear = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->wset = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&s->mr, NULL, &zune_stub_ops, s, name, size);
    /* Low priority so real device models can overlay parts of a block. */
    memory_region_add_subregion_overlap(get_system_memory(), base, &s->mr,
                                        -1000);
    return s;
}

/* Bits of one stub register (absolute PA) that read back as 0 after a write. */
static void zune_stub_selfclear(ZuneStub *s, hwaddr pa, uint32_t bits)
{
    g_hash_table_insert(s->wclear, GUINT_TO_POINTER(pa - s->base),
                        GUINT_TO_POINTER(bits));
}

/* Bits of one stub register (absolute PA) that stay 1 across writes. */
static void zune_stub_forceset(ZuneStub *s, hwaddr pa, uint32_t bits)
{
    g_hash_table_insert(s->wset, GUINT_TO_POINTER(pa - s->base),
                        GUINT_TO_POINTER(bits));
}

/* APB_MISC + 0x70 = USB1 VBUS wake-up control (logic in zunehd-usb.c). */
uint32_t zune_apb_get(uint32_t off)
{
    return zune_apb_stub ? GPOINTER_TO_UINT(g_hash_table_lookup(
               zune_apb_stub->regs, GUINT_TO_POINTER(off))) : 0;
}

uint32_t zune_apb_get70(void)
{
    return zune_apb_get(0x70);
}

void zune_apb_set70(uint32_t v)
{
    if (zune_apb_stub) {
        g_hash_table_insert(zune_apb_stub->regs, GUINT_TO_POINTER(0x70),
                            GUINT_TO_POINTER(v));
    }
}

static void zune_stub_preset(ZuneStub *s, hwaddr pa, uint32_t val)
{
    g_hash_table_insert(s->regs, GUINT_TO_POINTER(pa - s->base),
                        GUINT_TO_POINTER(val));
}

/* ---------------------------------------------------------------------- */
/* Legacy interrupt controller (ICTLR) @0x60004000: 3 banks x 32 lines.    */
/* Layout per bank (confirmed by the OAL's init writes): VIRQ_CPU +0x00,    */
/* VIRQ_COP +0x04, VFIQ_CPU +0x08, VFIQ_COP +0x0C, ISR +0x10, FIR +0x14,     */
/* FIR_SET +0x18, FIR_CLR +0x1C, CPU_IER +0x20, CPU_IER_SET +0x24,           */
/* CPU_IER_CLR +0x28, CPU_IEP_CLASS +0x2C (1 = FIQ), COP_IER +0x30..+0x3C.   */

#define ZUNEHD_ICTLR_BASE   0x60004000
#define ICTLR_BANKS         3

/*
 * Arbitration semaphores (ARB_SEMA @0x60002000) used by NvRm XPC for CPU<->AVP
 * locking (NvRmXpcInitArbSemaSystem writes SMP_GET then ARBGNT_CPU_ENABLE and
 * waits for the grant interrupt). SMP_GNT_ST +0x00 (RO, granted to CPU),
 * SMP_GET +0x04 (request bits), SMP_PUT +0x08 (release bits), SMP_REQ_ST +0x0C.
 * The AVP is not emulated, so every request is granted immediately. Grant
 * status/enable live in the ICTLR at +0x40/+0x44 (CPU) and +0x48/+0x4C (COP);
 * the CPU grant interrupt is IRQ 29 (INT_GNT_1, enabled by the OAL).
 */
#define ZUNEHD_ARB_BASE     0x60002000
#define ZUNEHD_ARBGNT_IRQ   29

/*
 * Per master (0 = CPU, 1 = COP/AVP): GET grants the bits not held by the other
 * master and queues the rest; PUT releases and hands queued bits over. The
 * accessing master is told apart by the executing CPU. Grant interrupts:
 * CPU IRQ 29, COP IRQ 28 (INT_GNT_0).
 */
typedef struct ZuneArb {
    MemoryRegion mr;
    uint32_t gnt[2];    /* semaphores held by CPU / COP */
    uint32_t req[2];    /* queued requests */
    uint32_t cpu_en;    /* ICTLR ARBGNT_CPU_ENABLE */
    uint32_t cop_en;
    qemu_irq irq, cop_irq;
} ZuneArb;

static void zune_arb_update(ZuneArb *a)
{
    qemu_set_irq(a->irq, (a->gnt[0] & a->cpu_en) != 0);
    if (a->cop_irq) {
        qemu_set_irq(a->cop_irq, (a->gnt[1] & a->cop_en) != 0);
    }
}

static uint64_t zune_arb_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneArb *a = opaque;
    int m = zune_avp_is_current();

    switch (off) {
    case 0x00: return a->gnt[m];
    case 0x0c: return a->req[m];
    }
    return 0;
}

static void zune_arb_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    ZuneArb *a = opaque;
    int m = zune_avp_is_current(), o = !m;
    uint32_t v = val;

    qemu_log_mask(LOG_UNIMP, "zunehd: %sarb_sema %s 0x%08x  pc~0x%08x\n",
                  zune_cpu_tag(), off == 4 ? "GET" : off == 8 ? "PUT" : "?", v,
                  zune_guest_pc());
    switch (off) {
    case 0x04:
        a->gnt[m] |= v & ~a->gnt[o];
        a->req[m] |= v & a->gnt[o];
        break;
    case 0x08: {
        uint32_t freed = v & a->gnt[m], hand = freed & a->req[o];
        a->gnt[m] &= ~v;
        a->req[m] &= ~v;
        a->gnt[o] |= hand;
        a->req[o] &= ~hand;
        break;
    }
    }
    zune_arb_update(a);
}

static const MemoryRegionOps zune_arb_ops = {
    .read = zune_arb_read,
    .write = zune_arb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

typedef struct ZuneIctlr {
    MemoryRegion mr;
    ZuneArb *arb;
    qemu_irq cpu_irq, cpu_fiq;
    uint32_t lines[ICTLR_BANKS];   /* level of the hardware inputs */
    uint32_t fir[ICTLR_BANKS];     /* software-forced interrupts */
    uint32_t cpu_ier[ICTLR_BANKS];
    uint32_t cpu_class[ICTLR_BANKS];
    uint32_t cop_ier[ICTLR_BANKS];
    uint32_t cop_class[ICTLR_BANKS];
} ZuneIctlr;

static uint32_t ictlr_isr(ZuneIctlr *ic, int b)
{
    return ic->lines[b] | ic->fir[b];
}

static void ictlr_update(ZuneIctlr *ic)
{
    bool irq = false, fiq = false;

    bool cirq = false, cfiq = false;

    for (int b = 0; b < ICTLR_BANKS; b++) {
        uint32_t act = ictlr_isr(ic, b) & ic->cpu_ier[b];
        uint32_t cact = ictlr_isr(ic, b) & ic->cop_ier[b];
        irq |= (act & ~ic->cpu_class[b]) != 0;
        fiq |= (act & ic->cpu_class[b]) != 0;
        cirq |= (cact & ~ic->cop_class[b]) != 0;
        cfiq |= (cact & ic->cop_class[b]) != 0;
    }
    qemu_set_irq(ic->cpu_irq, irq);
    qemu_set_irq(ic->cpu_fiq, fiq);
    zune_avp_set_irq(cirq, cfiq);           /* COP side -> AVP */
}

static void ictlr_set_irq(void *opaque, int n, int level)
{
    ZuneIctlr *ic = opaque;
    int b = n / 32;
    uint32_t bit = 1u << (n % 32);

    if (b >= ICTLR_BANKS) {
        return;
    }
    ic->lines[b] = level ? (ic->lines[b] | bit) : (ic->lines[b] & ~bit);
    ictlr_update(ic);
}

static uint64_t zune_ictlr_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneIctlr *ic = opaque;
    int b = off / 0x100;
    uint32_t isr, v = 0;

    if (b >= ICTLR_BANKS) {
        qemu_log_mask(LOG_UNIMP, "zunehd: ictlr read  [0x%08" HWADDR_PRIx
                      "]  pc~0x%08x\n", ZUNEHD_ICTLR_BASE + off, zune_guest_pc());
        return 0;
    }
    isr = ictlr_isr(ic, b);
    if (b == 0 && off >= 0x40 && off < 0x50 && ic->arb) {
        switch (off) {
        case 0x40: return ic->arb->gnt[0] & ic->arb->cpu_en; /* ARBGNT_CPU_STATUS */
        case 0x44: return ic->arb->cpu_en;
        case 0x48: return ic->arb->gnt[1] & ic->arb->cop_en; /* ARBGNT_COP_STATUS */
        case 0x4c: return ic->arb->cop_en;
        }
    }
    switch (off & 0xff) {
    case 0x00: v = isr & ic->cpu_ier[b] & ~ic->cpu_class[b]; break; /* VIRQ_CPU */
    case 0x04: v = isr & ic->cop_ier[b] & ~ic->cop_class[b]; break; /* VIRQ_COP */
    case 0x08: v = isr & ic->cpu_ier[b] & ic->cpu_class[b]; break;  /* VFIQ_CPU */
    case 0x0c: v = isr & ic->cop_ier[b] & ic->cop_class[b]; break;  /* VFIQ_COP */
    case 0x10: v = isr; break;
    case 0x14: v = ic->fir[b]; break;
    case 0x20: v = ic->cpu_ier[b]; break;
    case 0x2c: v = ic->cpu_class[b]; break;
    case 0x30: v = ic->cop_ier[b]; break;
    case 0x3c: v = ic->cop_class[b]; break;
    default:
        qemu_log_mask(LOG_UNIMP, "zunehd: ictlr read  [0x%08" HWADDR_PRIx
                      "]  pc~0x%08x\n", ZUNEHD_ICTLR_BASE + off, zune_guest_pc());
        break;
    }
    /* Trace reads while a non-tick line is pending (tick = IRQ 42, bank 1 bit 10). */
    if ((ictlr_isr(ic, 0) | (ictlr_isr(ic, 1) & ~(1u << 10)) | ictlr_isr(ic, 2))) {
        qemu_log_mask(LOG_UNIMP, "zunehd: ictlr read  [0x%08" HWADDR_PRIx "] -> 0x%08x"
                      " (isr %08x %08x %08x ier %08x %08x %08x)  pc~0x%08x\n",
                      ZUNEHD_ICTLR_BASE + off, v, ictlr_isr(ic, 0), ictlr_isr(ic, 1),
                      ictlr_isr(ic, 2), ic->cpu_ier[0], ic->cpu_ier[1], ic->cpu_ier[2],
                      zune_guest_pc());
    }
    return v;
}

static void zune_ictlr_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    ZuneIctlr *ic = opaque;
    int b = off / 0x100;

    if (b >= ICTLR_BANKS) {
        qemu_log_mask(LOG_UNIMP, "zunehd: ictlr write [0x%08" HWADDR_PRIx
                      "] <- 0x%08" PRIx64 "  pc~0x%08x\n",
                      ZUNEHD_ICTLR_BASE + off, val, zune_guest_pc());
        return;
    }
    if (b == 0 && (off == 0x44 || off == 0x4c) && ic->arb) {
        if (off == 0x44) {
            ic->arb->cpu_en = val;
        } else {
            ic->arb->cop_en = val;
        }
        zune_arb_update(ic->arb);
        return;
    }
    if ((off & 0xff) == 0x24 || (off & 0xff) == 0x28) {
        for (int i = 0; i < 32; i++) {
            if (val & (1u << i)) {
                qemu_log_mask(LOG_UNIMP, "zunehd: ictlr CPU IRQ %d %s  pc~0x%08x\n",
                              b * 32 + i, (off & 0xff) == 0x24 ? "enable" : "disable",
                              zune_guest_pc());
            }
        }
    }
    switch (off & 0xff) {
    case 0x18: ic->fir[b] |= val; break;
    case 0x1c: ic->fir[b] &= ~val; break;
    case 0x24: ic->cpu_ier[b] |= val; break;
    case 0x28: ic->cpu_ier[b] &= ~val; break;
    case 0x2c: ic->cpu_class[b] = val; break;
    case 0x34:
    case 0x38:
        if (getenv("ZUNEHD_DMALOG")) {      /* AVP interrupt enables (audio debug) */
            fprintf(stderr, "ictlr: COP IER bank %d %s 0x%08x\n", b,
                    (off & 0xff) == 0x34 ? "set" : "clr", (uint32_t)val);
        }
        if ((off & 0xff) == 0x34) {
            ic->cop_ier[b] |= val;
        } else {
            ic->cop_ier[b] &= ~val;
        }
        break;
    case 0x3c: ic->cop_class[b] = val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "zunehd: ictlr write [0x%08" HWADDR_PRIx
                      "] <- 0x%08" PRIx64 "  pc~0x%08x\n",
                      ZUNEHD_ICTLR_BASE + off, val, zune_guest_pc());
        break;
    }
    ictlr_update(ic);
}

static const MemoryRegionOps zune_ictlr_ops = {
    .read = zune_ictlr_read,
    .write = zune_ictlr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------------- */
/* Tegra timers @0x60005000: TMR1 +0x00, TMR2 +0x08, TMRUS +0x10,          */
/* TMR3 +0x50, TMR4 +0x58. PTV: [31] EN, [30] PERIODIC, [28:0] count-1 us;  */
/* PCR (+4): read [28:0] current count, write [30] clears the interrupt.    */
/* IRQs (Tegra numbering, TMR4 = 42 confirmed by the OAL's IER writes):     */
/* TMR1 0, TMR2 1, TMR3 41, TMR4 42.                                        */

typedef struct ZuneTmrChan {
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t ptv;
    int64_t start_ns;
    bool pending;
} ZuneTmrChan;

typedef struct ZuneTimer {
    MemoryRegion mr;
    ZuneTmrChan ch[4];
    uint32_t usec_cfg;
} ZuneTimer;

static int zune_tmr_chan(hwaddr off)
{
    switch (off & ~7u) {
    case 0x00: return 0;
    case 0x08: return 1;
    case 0x50: return 2;
    case 0x58: return 3;
    }
    return -1;
}

static int64_t zune_tmr_period_ns(ZuneTmrChan *c)
{
    return ((int64_t)(c->ptv & 0x1fffffff) + 1) * 1000;
}

static void zune_tmr_arm(ZuneTmrChan *c)
{
    c->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (c->ptv & 0x80000000) {
        timer_mod(c->timer, c->start_ns + zune_tmr_period_ns(c));
    } else {
        timer_del(c->timer);
    }
}

static void zune_tmr_expire(void *opaque)
{
    ZuneTmrChan *c = opaque;

    c->pending = true;
    qemu_irq_raise(c->irq);
    if (c->ptv & 0x40000000) {
        c->start_ns += zune_tmr_period_ns(c);
        timer_mod(c->timer, c->start_ns + zune_tmr_period_ns(c));
    } else {
        c->ptv &= ~0x80000000u;  /* one-shot: disable after firing */
    }
}

static uint64_t zune_tmr_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneTimer *t = opaque;
    int n = zune_tmr_chan(off);

    if (off == TMR_TMRUS_CNTR) {
        return (uint32_t)qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    }
    if (off == TMR_TMRUS_CFG) {
        return t->usec_cfg;
    }
    if (n >= 0) {
        ZuneTmrChan *c = &t->ch[n];
        if (!(off & 4)) {
            return c->ptv;
        } else {
            uint64_t el = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - c->start_ns)
                          / 1000;
            uint32_t cnt = c->ptv & 0x1fffffff;
            uint32_t pcv = (c->ptv & 0x80000000) && el <= cnt ? cnt - el : 0;
            return pcv | (c->pending ? 0x40000000 : 0);
        }
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: tmr read  [0x%08" HWADDR_PRIx "]\n",
                  ZUNEHD_TMR_BASE + off);
    return 0;
}

static void zune_tmr_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    ZuneTimer *t = opaque;
    int n = zune_tmr_chan(off);

    if (off == TMR_TMRUS_CFG) {
        t->usec_cfg = val;
        return;
    }
    if (n >= 0) {
        ZuneTmrChan *c = &t->ch[n];
        if (!(off & 4)) {
            c->ptv = val;
            zune_tmr_arm(c);
        } else if (val & 0x40000000) {
            c->pending = false;
            qemu_irq_lower(c->irq);
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: tmr write [0x%08" HWADDR_PRIx
                  "] <- 0x%08" PRIx64 "\n", ZUNEHD_TMR_BASE + off, val);
}

static const MemoryRegionOps zune_tmr_ops = {
    .read = zune_tmr_read,
    .write = zune_tmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------------- */
/* GPIO controller @0x6000D000: 7 banks x 4 ports x 8 pins (Tegra layout).   */
/*   bank b at +0x80*b, port p register at +4*p: 0x00 CNF, 0x10 OE, 0x20    */
/*   OUT, 0x30 IN, 0x40 INT_STA, 0x50 INT_ENB, 0x60 INT_LVL ([7:0] high/     */
/*   rising, [15:8] edge, [23:16] both edges), 0x70 INT_CLR (W1C). Masked    */
/*   copies at +0x800: value [15:8] = bit mask, [7:0] = bits.                */
/*   Bank IRQs 32, 33, 34, 35, 55, 87, 89.                                   */

#define ZUNEHD_GPIO_BASE    0x6000d000
#define GPIO_BANKS          7
#define GPIO_PORTS          (GPIO_BANKS * 4)

/*
 * Bit-banged I2C target on GPIO C4 (SCL) / C5 (SDA): NvRmI2cTransaction falls
 * back to software I2C on the I2C1 pins (FUN_c096331c) for transfers the
 * controller can't do. The host drives a line low with OE=1/OUT=0 and releases
 * it with OE=0 (pull-up). This target ACKs every byte and returns 0x00 on
 * reads (same as the hardware I2C model for unmodelled devices).
 */
#define BB_PORT     2           /* C */
#define BB_SCL      4
#define BB_SDA      5

typedef struct ZuneBbI2c {
    bool scl, sda;              /* resolved line levels */
    bool active, reading, ack_phase, sda_low;   /* sda_low: target pulls SDA */
    int bit;                    /* bits received/sent in the current byte */
    uint8_t shift;
    bool addr_byte;
} ZuneBbI2c;

typedef struct ZuneGpio {
    MemoryRegion mr;
    qemu_irq irq[GPIO_BANKS];
    uint32_t cnf[GPIO_PORTS], oe[GPIO_PORTS], out[GPIO_PORTS];
    uint32_t sta[GPIO_PORTS], enb[GPIO_PORTS], lvl[GPIO_PORTS];
    uint8_t in[GPIO_PORTS];             /* levels driven from outside */
    ZuneBbI2c bb;
} ZuneGpio;

static uint8_t gpio_level(ZuneGpio *g, int p)
{
    uint8_t v = (g->out[p] & g->oe[p]) | (g->in[p] & ~g->oe[p]);

    if (p == BB_PORT && g->bb.sda_low) {
        v &= ~(1u << BB_SDA);           /* open drain: target pulls SDA low */
    }
    return v;
}

/* Run the bit-bang target after any write that may have moved SCL/SDA. */
static void bb_i2c_step(ZuneGpio *g)
{
    ZuneBbI2c *b = &g->bb;
    uint8_t lv = gpio_level(g, BB_PORT);
    bool scl = (lv >> BB_SCL) & 1, sda = (lv >> BB_SDA) & 1;
    bool host_sda = !((g->oe[BB_PORT] >> BB_SDA) & 1) ||
                    ((g->out[BB_PORT] >> BB_SDA) & 1);  /* host releases SDA */

    if (scl && b->scl && sda != b->sda && !b->sda_low) {
        if (!sda) {                     /* START / repeated START */
            b->active = true;
            b->addr_byte = true;
            b->reading = false;
            b->ack_phase = false;
            b->bit = 0;
            b->shift = 0;
        } else {                        /* STOP */
            b->active = false;
        }
    } else if (b->active && scl && !b->scl) {           /* SCL rising */
        if (b->ack_phase) {
            /* host samples our ACK (write) or gives ACK/NACK (read) */
            if (b->reading && host_sda) {
                b->active = false;      /* NACK from host: end of read */
            }
        } else if (!b->reading) {
            b->shift = (b->shift << 1) | (sda ? 1 : 0);
            b->bit++;
        } else {
            b->bit++;
        }
    } else if (b->active && !scl && b->scl) {           /* SCL falling */
        if (b->ack_phase) {
            b->ack_phase = false;
            b->sda_low = false;
            b->bit = 0;
            if (b->reading) {
                b->sda_low = true;      /* first data bit of the next byte: 0 */
            }
        } else if (b->bit == 8) {
            b->ack_phase = true;
            if (!b->reading) {
                if (b->addr_byte) {
                    b->reading = b->shift & 1;
                    b->addr_byte = false;
                }
                b->sda_low = true;      /* ACK */
            } else {
                b->sda_low = false;     /* release for the host's ACK/NACK */
            }
        } else if (b->reading) {
            b->sda_low = true;          /* data bits 0x00 */
        }
    }
    lv = gpio_level(g, BB_PORT);
    b->scl = (lv >> BB_SCL) & 1;
    b->sda = (lv >> BB_SDA) & 1;
}

static void gpio_update(ZuneGpio *g)
{
    for (int p = 0; p < GPIO_PORTS; p++) {
        uint8_t lv = gpio_level(g, p);
        for (int b = 0; b < 8; b++) {
            bool edge = g->lvl[p] & (1u << (8 + b));
            bool high = g->lvl[p] & (1u << b);
            /* level interrupt stays asserted while active; only for enabled
             * pins (unconfigured pins default to level-low with input 0 and
             * would otherwise all read as pending, so the kernel's GPIO ISR
             * masked whole ports) */
            if (!edge && ((lv >> b) & 1) == high && (g->enb[p] & (1u << b))) {
                g->sta[p] |= 1u << b;
            }
        }
    }
    for (int k = 0; k < GPIO_BANKS; k++) {
        bool on = false;
        for (int p = 0; p < 4; p++) {
            on |= (g->sta[k * 4 + p] & g->enb[k * 4 + p] & 0xff) != 0;
        }
        qemu_set_irq(g->irq[k], on);
    }
}

/* Drive an input pin from a modelled peripheral. */
static void gpio_set_input(ZuneGpio *g, int port, int bit, bool level)
{
    uint8_t old = (g->in[port] >> bit) & 1;

    g->in[port] = (g->in[port] & ~(1u << bit)) | (level << bit);
    if (old != level && (g->lvl[port] & (1u << (8 + bit)))) {
        bool both = g->lvl[port] & (1u << (16 + bit));
        bool rising = g->lvl[port] & (1u << bit);
        if (both || level == rising) {
            g->sta[port] |= 1u << bit;
        }
    }
    gpio_update(g);
}

static uint32_t *gpio_reg(ZuneGpio *g, unsigned r, int p)
{
    switch (r) {
    case 0x00: return &g->cnf[p];
    case 0x10: return &g->oe[p];
    case 0x20: return &g->out[p];
    case 0x40: return &g->sta[p];
    case 0x50: return &g->enb[p];
    case 0x60: return &g->lvl[p];
    }
    return NULL;
}

static uint64_t gpio_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneGpio *g = opaque;
    unsigned o = off & 0x7ff, r = o & 0x70;
    int p = (o >> 7) * 4 + ((o >> 2) & 3);
    uint32_t *reg;

    if ((o >> 7) >= GPIO_BANKS) {
        return 0;
    }
    if (r == 0x30) {
        return gpio_level(g, p);
    }
    reg = gpio_reg(g, r, p);
    return reg ? *reg : 0;
}

static void gpio_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneGpio *g = opaque;
    unsigned o = off & 0x7ff, r = o & 0x70;
    int p = (o >> 7) * 4 + ((o >> 2) & 3);
    uint32_t *reg;

    if ((o >> 7) >= GPIO_BANKS) {
        return;
    }
    if (r == 0x70) {                    /* INT_CLR */
        g->sta[p] &= ~(uint32_t)(val & 0xff);
    } else if ((reg = gpio_reg(g, r, p))) {
        if (off & 0x800) {              /* masked write */
            uint32_t m = (val >> 8) & 0xff;
            *reg = (*reg & ~m) | (val & m);
        } else {
            *reg = val;
        }
    }
    if (p == BB_PORT) {
        bb_i2c_step(g);
    }
    if (r == 0x50 || r == 0x60) {
        qemu_log_mask(LOG_UNIMP, "zunehd: gpio port %c %s <- 0x%06x (enb 0x%02x "
                      "lvl 0x%06x)\n", 'A' + p, r == 0x50 ? "INT_ENB" : "INT_LVL",
                      (uint32_t)val, g->enb[p], g->lvl[p]);
    }
    gpio_update(g);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------------- */
/* Synaptics touch controller, RMI4 over I2C (I2C2, 7-bit address 0x2C),    */
/* driven by synaptics.dll (loaded by ZTouch.dll). Register map (page 0):   */
/*   0x00 F01 device status, 0x01 F01 interrupt status (read clears),        */
/*   0x02.. F11 2D data: [0] finger status (finger0 [1:0] = 1 present),      */
/*   [1] X[11:4], [2] Y[11:4], [3] Y[3:0]<<4 | X[3:0], [4] W, [5] Z,          */
/*   then finger 1 (unused). F01 ctrl 0x30, cmd 0x40, query 0x50; F11 ctrl   */
/*   0x31, cmd 0x41, query 0x60. PDT at 0xEE (F01) and 0xE8 (F11), each      */
/*   entry read top-down: fn#, int count, data, ctrl, cmd, query base.       */
/*   Raw range X 0..1764 by Y 0..3136 (driver scales by these).              */
/* ATTN is GPIO U0, active low (level); it is released when the host reads the       */
/* interrupt status or the F11 data. The host mouse (absolute pointer) is    */
/* the finger: left button held = touching.                                  */

#define TOUCH_I2C_ADDR      0x2c
#define TOUCH_XMAX          1764
#define TOUCH_YMAX          3136
#define TOUCH_ATTN_PORT     20          /* U (bank 5 port 0), seen in INT_LVL/ENB */
#define TOUCH_ATTN_BIT      0

typedef struct ZuneTouch {
    uint8_t regs[256];
    uint8_t ptr;
    ZuneGpio *gpio;
    int px, py;                         /* pointer, 0..INPUT_EVENT_ABS_MAX */
    bool btn, down;
    int sent_x, sent_y;
} ZuneTouch;

static ZuneTouch *zune_touch;

static void touch_attn(ZuneTouch *t, bool assert)
{
    if (t->gpio) {
        gpio_set_input(t->gpio, TOUCH_ATTN_PORT, TOUCH_ATTN_BIT, !assert);
    }
}

static void touch_init_regs(ZuneTouch *t)
{
    static const uint8_t pdt[] = {
        /* 0xE3..0xE8: F11 query, cmd, ctrl, data, int count, fn# */
        0x60, 0x41, 0x31, 0x02, 0x01, 0x11,
        /* 0xE9..0xEE: F01 */
        0x50, 0x40, 0x30, 0x00, 0x01, 0x01,
    };

    memset(t->regs, 0, sizeof(t->regs));
    memcpy(&t->regs[0xe3], pdt, sizeof(pdt));
}

static void touch_report(ZuneTouch *t)
{
    int x = MAX(1, MIN(TOUCH_XMAX, (int)((int64_t)t->px * TOUCH_XMAX /
                                         INPUT_EVENT_ABS_MAX)));
    /* The panel's Y axis runs bottom-up (raw 0 = bottom edge of the screen). */
    int y = MAX(1, MIN(TOUCH_YMAX, (int)((int64_t)(INPUT_EVENT_ABS_MAX - t->py) *
                                         TOUCH_YMAX / INPUT_EVENT_ABS_MAX)));
    uint8_t *d = &t->regs[0x02];

    memset(d, 0, 11);
    d[0] = t->down ? 1 : 0;
    d[1] = x >> 4;
    d[2] = y >> 4;
    d[3] = (y & 0xf) << 4 | (x & 0xf);
    d[4] = t->down ? 0x33 : 0;
    d[5] = t->down ? 0x40 : 0;
    t->regs[0x01] |= 1u << 1;           /* F11 interrupt source */
    t->sent_x = t->px;
    t->sent_y = t->py;
    touch_attn(t, true);
}

static void touch_i2c(ZuneTouch *t, uint8_t *buf, int n, bool rd)
{
    if (!rd) {
        t->ptr = buf[0];
        for (int i = 1; i < n; i++) {
            uint8_t r = t->ptr++;
            if (r == 0x40 && (buf[i] & 1)) {        /* F01 reset */
                touch_init_regs(t);
                touch_report(t);
                t->ptr = r + 1;
                continue;
            }
            if (r < 0xe3 || r > 0xee) {
                t->regs[r] = buf[i];
            }
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        uint8_t r = t->ptr++;
        buf[i] = t->regs[r];
        if (r == 0x01) {
            t->regs[0x01] = 0;          /* interrupt status: read clears */
            touch_attn(t, false);
        } else if (r == 0x02) {
            touch_attn(t, false);
        }
    }
}

static void touch_input_event(DeviceState *dev, QemuConsole *src,
                              QemuInputEvent *evt)
{
    ZuneTouch *t = zune_touch;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            t->px = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            t->py = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_REL: {
        /* relative motion (e.g. the monitor's mouse_move): pixels -> abs units */
        int *v = evt->rel.axis == INPUT_AXIS_X ? &t->px : &t->py;
        int span = evt->rel.axis == INPUT_AXIS_X ? 272 : 480;
        if (evt->rel.axis == INPUT_AXIS_X || evt->rel.axis == INPUT_AXIS_Y) {
            *v = MAX(0, MIN(INPUT_EVENT_ABS_MAX,
                            *v + (int)((int64_t)evt->rel.value *
                                       INPUT_EVENT_ABS_MAX / span)));
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            t->btn = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void touch_input_sync(DeviceState *dev)
{
    ZuneTouch *t = zune_touch;

    if (t->btn != t->down) {
        t->down = t->btn;
        touch_report(t);
    } else if (t->down && (t->px != t->sent_x || t->py != t->sent_y)) {
        touch_report(t);
    }
}

static const QemuInputHandler touch_input_handler = {
    .name  = "Zune HD touch panel",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_REL,
    .event = touch_input_event,
    .sync  = touch_input_sync,
};

/* ---------------------------------------------------------------------- */
/* Hardware buttons on GPIOs (zkeypad.dll): active low, idle high.          */
/* interrupts on U1, K5, N7; zkeypad reports level 0 as pressed (active     */
/* low). Host keys drive the pins.                                          */

typedef struct ZuneButton {
    unsigned key;           /* Linux key code (QEMU input) */
    int port, bit;
    const char *name;
} ZuneButton;

/*
 * zkeypad.dll pins (ODM peripheral "zkeypad0", board type 10): index 0 = S5,
 * VK 0x24 (Home; short press / hold handled in the driver); index 1 = S6,
 * VK 0x74 (presumably the side media button). Host keys: Home or H = home,
 * M = media. Power is not on the keypad (still to do).
 */
static const ZuneButton zune_buttons[] = {
    { KEY_HOME, 18, 5, "home" },
    { KEY_H,    18, 5, "home" },
    { KEY_M,    18, 6, "media" },
};

static ZuneGpio *zune_gpio;
static void zune_pmic_power_key(bool down);

static void buttons_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    if (evt->type != INPUT_EVENT_KIND_KEY || !zune_gpio) {
        return;
    }
    if (getenv("ZUNEHD_USBLOG")) {
        fprintf(stderr, "key %u %s\n", evt->key.key, evt->key.down ? "down" : "up");
    }
    if (evt->key.key == KEY_U && evt->key.down) {   /* TEMP: toggle USB cable */
        static bool usb_on;
        usb_on = !usb_on;
        zune_usb_set_vbus(usb_on);
        return;
    }
    if (evt->key.key == KEY_END || evt->key.key == KEY_P) {   /* power: WM8350 */
        zune_pmic_power_key(evt->key.down);
        return;
    }
    for (int i = 0; i < ARRAY_SIZE(zune_buttons); i++) {
        if (zune_buttons[i].key == evt->key.key) {
            qemu_log_mask(LOG_UNIMP, "zunehd: button %s %s\n", zune_buttons[i].name,
                          evt->key.down ? "down" : "up");
            gpio_set_input(zune_gpio, zune_buttons[i].port, zune_buttons[i].bit,
                           !evt->key.down);             /* active low */
        }
    }
}

static const QemuInputHandler buttons_input_handler = {
    .name  = "Zune HD buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = buttons_event,
};

static void zunehd_input_init(qemu_irq *lines)
{
    static const int bank_irq[GPIO_BANKS] = { 32, 33, 34, 35, 55, 87, 89 };
    ZuneGpio *g = g_new0(ZuneGpio, 1);
    ZuneTouch *t = g_new0(ZuneTouch, 1);
    QemuInputHandlerState *hs;

    for (int k = 0; k < GPIO_BANKS; k++) {
        g->irq[k] = lines[bank_irq[k]];
    }
    /* Inputs read 0 (as with the old stub: all-high hangs the OAL before the
     * kernel banner, presumably a "key held" check); ATTN idles high. */
    g->in[TOUCH_ATTN_PORT] |= 1u << TOUCH_ATTN_BIT;
    /*
     * Board strap pins read by the OAL (FUN_8004b6b8: bit i = pin i high, pins
     * T4 D5 L0..L7 T2 T3). The user's device caches strap 0x802 -> board type
     * 10 (kernel 0x80BE8CA0/A4, read via Lyra), which selects the ODM
     * peripheral table with zkeypad0 (type 2, all-low straps, selected the
     * NVIDIA dev-board table without it). So D5 and T3 read high.
     */
    g->in[3] |= 1u << 5;        /* D5 */
    g->in[19] |= 1u << 3;       /* T3 */
    /* U7 is sampled first with its pull-up enabled (FUN_8004b7c8); high
     * selects the strap decode where 0x802 -> type 10 (low gives 0x802 -> 0,
     * which the OAL traps on at 0x8004a0ec). */
    g->in[20] |= 1u << 7;       /* U7 */
    /* NvRm bit-banged I2C (FUN_c096331c, used once the PMU interrupt path is
     * enabled): SCL = C4, SDA = C5, open drain with pull-ups. Released (input)
     * lines must read high or every clock waits for a stretch timeout. */
    g->in[2] |= (1u << 4) | (1u << 5);
    for (int i = 0; i < ARRAY_SIZE(zune_buttons); i++) {   /* released = high */
        g->in[zune_buttons[i].port] |= 1u << zune_buttons[i].bit;
    }
    zune_gpio = g;
    memory_region_init_io(&g->mr, NULL, &gpio_ops, g, "zunehd.gpio", 0x1000);
    memory_region_add_subregion(get_system_memory(), ZUNEHD_GPIO_BASE, &g->mr);

    t->gpio = g;
    touch_init_regs(t);
    zune_touch = t;
    hs = qemu_input_handler_register(NULL, &touch_input_handler);
    qemu_input_handler_activate(hs);
    hs = qemu_input_handler_register(NULL, &buttons_input_handler);
    qemu_input_handler_activate(hs);
}

/* ---------------------------------------------------------------------- */
/* DVC I2C controller @0x7000D000 (IRQ 53). The OAL polls I2C_STATUS; the   */
/* kernel NvRm I2C driver waits for the DVC "I2C done" interrupt instead.   */
/* No I2C devices are modelled: every transfer completes at once with ACK   */
/* and reads return 0.                                                      */
/*   DVC_CTRL_REG3 +0x08: [26] I2C_DONE_INTR_EN    DVC_STATUS +0x0C: [30]    */
/*   I2C_DONE_INTR (W1C). Legacy I2C block at +0x40: CNFG +0x40 ([9] SEND), */
/*   CMD_ADDR0 +0x44 (addr<<1 | read), CMD_DATA1 +0x4C, CMD_DATA2 +0x50,     */
/*   STATUS +0x5C ([8] BUSY, [3:0] CMD1_STAT, 0 = ok).                       */

#define ZUNEHD_DVC_BASE     0x7000d000
#define ZUNEHD_DVC_IRQ      53

/*
 * The only device on this bus is at 7-bit address 0x1A, used both by the OAL
 * PMU/RTC code and by the audio-codec ODM: consistent with a Wolfson WM8350
 * (PMIC + codec + RTC, 16-bit registers, MSB first). Modelled as a 256 x 16-bit
 * register file with auto-increment; R0 = ID 0x6143, R16..R19 = RTC in BCD
 * (R16 min<<8|sec, R17 hour<<8|weekday, R18 month<<8|date, R19 century<<8|year)
 * taken from the host clock. An all-zero RTC makes the OAL's date conversion
 * (FUN_8004a49c) loop with month 0 and fault.
 */
#define ZUNEHD_WM_ADDR      0x1a

/*
 * Generic Tegra I2C controller. DVC (power I2C) has its legacy I2C block at
 * +0x40 and signals completion through DVC_STATUS[30] (W1C). Plain I2C
 * controllers (I2C1 0x7000C000 IRQ 38, I2C2 0x7000C400 IRQ 84 â€” 84 confirmed
 * by the driver's IER write) have the block at +0 and no interrupt status: the
 * NvRm ISR (FUN_c094de10) only reads I2C_STATUS, so the line is cleared on
 * that read.
 */
typedef struct ZuneDvc {
    MemoryRegion mr;
    qemu_irq irq;
    const char *name;
    hwaddr ro;          /* offset of the I2C register block */
    bool dvc;
    bool done;          /* plain I2C: completion pending until STATUS read */
    uint32_t regs[0x200 / 4];
    uint16_t wm[256];
    uint8_t wm_ptr;
    /* WM8350 power key (libnvodm_pmu FUN_c09b385c): R0x18 bit 0x40 summarises
     * R0x1E (read-to-clear) bit 3 = key changed; level = R0xE6 bit 3 (1 = down).
     * Interrupt line = Tegra external PMU IRQ 86. */
    qemu_irq pmu_irq;
    bool onkey;
    uint16_t onkey_int;
} ZuneDvc;

static ZuneDvc *zune_pmic;

static void zune_pmic_update(ZuneDvc *d)
{
    qemu_set_irq(d->pmu_irq, d->onkey_int != 0);
}

static void zune_pmic_power_key(bool down)
{
    ZuneDvc *d = zune_pmic;

    if (!d || d->onkey == down) {
        return;
    }
    d->onkey = down;
    d->onkey_int |= 0x08;
    qemu_log_mask(LOG_UNIMP, "zunehd: wm8350 power key %s\n", down ? "down" : "up");
    zune_pmic_update(d);
}

static uint8_t bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static uint16_t zune_wm_read(ZuneDvc *d, uint8_t reg)
{
    struct tm tm;

    if (reg >= 0x10 && reg <= 0x13) {
        qemu_get_timedate(&tm, 0);
        switch (reg) {
        case 0x10: return bcd(tm.tm_min) << 8 | bcd(tm.tm_sec);
        case 0x11: return bcd(tm.tm_hour) << 8 | (tm.tm_wday + 1);
        case 0x12: return bcd(tm.tm_mon + 1) << 8 | bcd(tm.tm_mday);
        case 0x13: return bcd((tm.tm_year + 1900) / 100) << 8 |
                          bcd((tm.tm_year + 1900) % 100);
        }
    }
    switch (reg) {
    case 0x18:                              /* system interrupts summary */
        return (d->wm[reg] & ~0x40) | (d->onkey_int ? 0x40 : 0);
    case 0x1e: {                            /* key interrupt status, R2C */
        uint16_t v = d->wm[reg] | d->onkey_int;
        d->onkey_int = 0;
        zune_pmic_update(d);
        return v;
    }
    case 0xe6:                              /* GPIO levels: bit 3 = power key */
        return (d->wm[reg] & ~0x08) | (d->onkey ? 0x08 : 0);
    }
    return d->wm[reg];
}

/* Run one I2C transfer of n bytes packed little-endian in DATA1/DATA2. */
static void zune_i2c_xfer(ZuneDvc *d, uint8_t slave, bool rd, int n)
{
    uint8_t buf[8] = { 0 };

    for (int i = 0; i < 4; i++) {
        buf[i] = d->regs[(d->ro + 0x0c) / 4] >> (8 * i);
        buf[i + 4] = d->regs[(d->ro + 0x10) / 4] >> (8 * i);
    }
    if (!d->dvc && slave == TOUCH_I2C_ADDR && zune_touch &&
        !strcmp(d->name, "i2c2")) {
        touch_i2c(zune_touch, buf, n, rd);
    } else if (!d->dvc || slave != ZUNEHD_WM_ADDR) {
        if (rd) {
            memset(buf, 0, sizeof(buf));
        }
    } else if (!rd) {
        /* reg pointer, then MSB/LSB pairs */
        d->wm_ptr = buf[0];
        for (int i = 1; i + 1 < n; i += 2) {
            d->wm[d->wm_ptr++] = buf[i] << 8 | buf[i + 1];
        }
    } else {
        for (int i = 0; i + 1 < n; i += 2) {
            uint16_t v = zune_wm_read(d, d->wm_ptr++);
            buf[i] = v >> 8;
            buf[i + 1] = v & 0xff;
        }
    }
    if (rd) {
        d->regs[(d->ro + 0x0c) / 4] = ldl_le_p(buf);
        d->regs[(d->ro + 0x10) / 4] = ldl_le_p(buf + 4);
    }
}

static void zune_dvc_update(ZuneDvc *d)
{
    bool lvl = d->dvc ? (d->regs[0x0c / 4] & (1u << 30)) &&
                        (d->regs[0x08 / 4] & (1u << 26))
                      : d->done;
    qemu_set_irq(d->irq, lvl);
}

static uint64_t zune_dvc_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneDvc *d = opaque;

    if (off >= sizeof(d->regs)) {
        return 0;
    }
    if (!d->dvc && off == d->ro + 0x1c && d->done) {
        d->done = false;                    /* STATUS read acks completion */
        zune_dvc_update(d);
    }
    return d->regs[off / 4];
}

static void zune_dvc_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    ZuneDvc *d = opaque;

    if (off >= sizeof(d->regs)) {
        return;
    }
    if (d->dvc && off == 0x0c) {            /* DVC_STATUS: W1C done bit */
        d->regs[0x0c / 4] &= ~(val & (1u << 30));
        zune_dvc_update(d);
        return;
    }
    d->regs[off / 4] = val;
    if (off == d->ro && (val & (1u << 9))) { /* I2C_CNFG.SEND */
        uint32_t a = d->regs[(d->ro + 4) / 4];
        bool rd = a & 1;
        qemu_log_mask(LOG_UNIMP, "zunehd: i2c(%s) %s slave 0x%02x cnfg 0x%03x "
                      "data1 0x%08x data2 0x%08x  pc~0x%08x\n", d->name,
                      rd ? "READ " : "WRITE", (a >> 1) & 0x7f, (uint32_t)val,
                      d->regs[(d->ro + 0x0c) / 4], d->regs[(d->ro + 0x10) / 4],
                      zune_guest_pc());
        /* I2C_CNFG[3:1] = LENGTH (bytes - 1) */
        zune_i2c_xfer(d, (a >> 1) & 0x7f, rd, ((val >> 1) & 7) + 1);
        d->regs[d->ro / 4] &= ~(1u << 9);
        d->regs[(d->ro + 0x1c) / 4] = 0;    /* not busy, CMD1/CMD2 ok */
        if (d->dvc) {
            d->regs[0x0c / 4] |= 1u << 30;  /* I2C_DONE_INTR */
        } else {
            d->done = true;
        }
        zune_dvc_update(d);
    }
}

static ZuneDvc *zune_i2c_create(const char *name, hwaddr base, qemu_irq irq,
                                bool dvc)
{
    ZuneDvc *d = g_new0(ZuneDvc, 1);

    d->name = name;
    d->irq = irq;
    d->dvc = dvc;
    d->ro = dvc ? 0x40 : 0;
    d->wm[0] = 0x6143;                      /* WM8350 reset/ID register */
    return d;
}

static const MemoryRegionOps zune_dvc_ops = {
    .read = zune_dvc_read,
    .write = zune_dvc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------------- */
/* Flow controller: HALT_{CPU,COP}_EVENTS low byte counts down in usec     */
/* (OAL delay FUN_8002bc24 writes 0x02000000|n and polls until (r&0xff)==0) */

#define ZUNEHD_FLOW_BASE    0x60007000

typedef struct ZuneFlow {
    MemoryRegion mr;
    uint32_t regs[0x100 / 4];
    int64_t start_us[2];
} ZuneFlow;

static uint64_t zune_flow_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneFlow *f = opaque;
    uint32_t v = f->regs[off / 4];

    if (off <= 4 && (v & 0xff)) {
        int64_t el = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) - f->start_us[off / 4];
        uint32_t left = el >= (v & 0xff) ? 0 : (v & 0xff) - el;
        v = (v & ~0xffu) | left;
        f->regs[off / 4] = v;
    }
    return v;
}

static void zune_flow_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    ZuneFlow *f = opaque;

    f->regs[off / 4] = val;
    if (off == 4) {
        zune_avp_flow_halt(val);            /* HALT_COP_EVENTS */
    }
    if (off <= 4) {
        f->start_us[off / 4] = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    } else {
        qemu_log_mask(LOG_UNIMP, "zunehd: flow write [0x%08" HWADDR_PRIx
                      "] <- 0x%08" PRIx64 "\n", ZUNEHD_FLOW_BASE + off, val);
    }
}

static const MemoryRegionOps zune_flow_ops = {
    .read = zune_flow_read,
    .write = zune_flow_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------------- */
/* NVIDIA HSMMC controller @0x70008500 (the eMMC host; not SDHCI)          */
/*
 * Register map from libnvddk_misc (NvDdkHsmmcSendCommand 0xc08e47b4, ISR
 * 0xc08e4d44, data setup 0xc08e4bf0, PreIssueStopTransmission 0xc08e4ae0,
 * GetCommandResponse 0xc08e43b4):
 *   +0x00 CTRL     driver writes 1 (clock set / transfer end)
 *   +0x04 STATUS   [5:0] errors, 0x800, 0x1000 write data done, 0x2000 command
 *                  done, 0x4000 read data done, 0x8000 busy end (R1b),
 *                  0x10000/0x20000; all W1C. [30] transfer active (RO),
 *                  [31] written by the driver before a data phase.
 *   +0x08 CLK      +0x14/+0x18 timeouts (0x7f / 0xffff)
 *   +0x10 CMD      [1:0] response (0 none, 1 48-bit, 2 136-bit, 3 48-bit no
 *                  CRC), [2] data, [3] write, [5] multi-block, [7] busy (R1b),
 *                  [8] CMD0, [9] HOLD, [11:10] bus width, [13] always set,
 *                  [31] mode. Writing with HOLD clear issues the command.
 *   +0x1C BLKSZ    +0x20 BLKCNT   +0x24 INTEN (non-zero while a command runs)
 *   +0x28 CMDIDX   +0x2C ARG
 *   +0x34 RESP     FIFO of the raw response bit stream from the start bit,
 *                  MSB first: R1 = raw[47:16], raw[15:0]<<16; R2 = raw[135:8].
 *   +0x40 DMALEN   [24:0] bytes-1, [31] start   +0x44 DMAADDR (physical)
 * Data phase: the command completes (0x2000) first, then the driver arms
 * DMALEN; the whole transfer is done at that point and raises 0x4000/0x1000.
 * For CMD25 the driver "pre-issues" CMD12 before arming the DMA, so that stop
 * is held back until the data has been written.
 */
#define TYPE_ZUNE_HSMMC     "zunehd-hsmmc"
OBJECT_DECLARE_SIMPLE_TYPE(ZuneHsmmc, ZUNE_HSMMC)

#define ZUNEHD_HSMMC_BASE   0x70008500
#define ZUNEHD_HSMMC_IRQ    22      /* enabled by NvDdkHsmmcOpen just before CMD0 */

#define HS_CTRL     0x00
#define HS_STATUS   0x04
#define HS_CMD      0x10
#define HS_INTEN    0x24
#define HS_CMDIDX   0x28
#define HS_ARG      0x2c
#define HS_RESP     0x34
#define HS_DMALEN   0x40
#define HS_DMAADDR  0x44

#define HS_ST_ERR_TIMEOUT   0x00000001
#define HS_ST_WR_DONE       0x00001000
#define HS_ST_CMD_DONE      0x00002000
#define HS_ST_RD_DONE       0x00004000
#define HS_ST_BUSY_END      0x00008000
#define HS_ST_W1C           0x3fffffff

#define HS_CMD_RESP_MASK    0x3
#define HS_CMD_DATA         0x4
#define HS_CMD_WRITE        0x8
#define HS_CMD_BUSY         0x80
#define HS_CMD_HOLD         0x200

struct ZuneHsmmc {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    SDBus sdbus;
    qemu_irq irq;
    uint32_t regs[0x100 / 4];
    uint32_t status;
    uint32_t resp[4];
    unsigned resp_pos;
    bool data_pending;      /* data command issued, DMA not armed yet */
    bool data_write;
    bool stop_deferred;     /* CMD12 pre-issued during a CMD25 */
};

static void hsmmc_update_irq(ZuneHsmmc *s)
{
    qemu_set_irq(s->irq, (s->status & HS_ST_W1C) && s->regs[HS_INTEN / 4]);
}

static void hsmmc_exec(ZuneHsmmc *s, uint8_t cmd, uint32_t arg, uint32_t flags)
{
    SDRequest req = { .cmd = cmd, .arg = arg };
    uint8_t rsp[16], raw[16] = { 0 };
    unsigned rtype = flags & HS_CMD_RESP_MASK;
    size_t rlen = sdbus_do_command(&s->sdbus, &req, rsp, sizeof(rsp));

    s->resp_pos = 0;
    if (rtype == 2 && rlen == 16) {
        raw[0] = 0x3f;
        memcpy(raw + 1, rsp, 15);
    } else if (rtype && rlen == 4) {
        raw[0] = rtype == 3 ? 0x3f : cmd;
        memcpy(raw + 1, rsp, 4);
        raw[5] = 0x01;
    } else if (rtype) {
        s->status |= HS_ST_ERR_TIMEOUT;     /* no response from the card */
    }
    for (int i = 0; i < 4; i++) {
        s->resp[i] = ldl_be_p(raw + 4 * i);
    }
    s->status |= HS_ST_CMD_DONE;
    if (flags & HS_CMD_BUSY) {
        s->status |= HS_ST_BUSY_END;
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: hsmmc CMD%u arg 0x%08x flags 0x%x -> "
                  "rlen %zu resp %08x %08x %08x %08x st 0x%x  pc~0x%08x\n",
                  cmd, arg, flags, rlen, s->resp[0], s->resp[1], s->resp[2],
                  s->resp[3], s->status, zune_guest_pc());
}

static void hsmmc_issue(ZuneHsmmc *s)
{
    uint32_t flags = s->regs[HS_CMD / 4];
    uint8_t cmd = s->regs[HS_CMDIDX / 4] & 0x3f;

    if (cmd == 12 && s->data_pending && s->data_write) {
        s->stop_deferred = true;
        return;
    }
    if (flags & HS_CMD_DATA) {
        s->data_pending = true;
        s->data_write = flags & HS_CMD_WRITE;
        s->stop_deferred = false;
    }
    hsmmc_exec(s, cmd, s->regs[HS_ARG / 4], flags);
}

static void hsmmc_dma(ZuneHsmmc *s)
{
    uint32_t len = (s->regs[HS_DMALEN / 4] & 0x1ffffff) + 1;
    hwaddr addr = s->regs[HS_DMAADDR / 4];
    g_autofree uint8_t *buf = g_malloc0(len);

    if (!s->data_pending) {
        qemu_log_mask(LOG_GUEST_ERROR, "zunehd: hsmmc DMA without data cmd\n");
    }
    if (s->data_write) {
        address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                           buf, len);
        sdbus_write_data(&s->sdbus, buf, len);
        s->status |= HS_ST_WR_DONE;
    } else {
        sdbus_read_data(&s->sdbus, buf, len);
        address_space_write(&address_space_memory, addr,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
        s->status |= HS_ST_RD_DONE;
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: hsmmc DMA %s %u bytes @0x%08" HWADDR_PRIx
                  "\n", s->data_write ? "write" : "read", len, addr);
    s->data_pending = false;
    s->regs[HS_DMALEN / 4] &= ~0x80000000u;
    if (s->stop_deferred) {
        s->stop_deferred = false;
        hsmmc_exec(s, 12, 0, 0x81);
    }
}

static uint64_t hsmmc_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneHsmmc *s = opaque;

    switch (off) {
    case HS_STATUS:
        return s->status | (s->regs[HS_STATUS / 4] & 0x80000000u);
    case HS_RESP:
        return s->resp[s->resp_pos++ & 3];
    default:
        return s->regs[off / 4];
    }
}

static void hsmmc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneHsmmc *s = opaque;

    switch (off) {
    case HS_STATUS:
        s->status &= ~(val & HS_ST_W1C);
        s->regs[HS_STATUS / 4] = val & 0x80000000u;
        break;
    case HS_CMD:
        s->regs[off / 4] = val;
        if (!(val & HS_CMD_HOLD)) {
            hsmmc_issue(s);
        }
        break;
    case HS_DMALEN:
        s->regs[off / 4] = val;
        if (val & 0x80000000u) {
            hsmmc_dma(s);
        }
        break;
    default:
        s->regs[off / 4] = val;
        break;
    }
    hsmmc_update_irq(s);
}

static const MemoryRegionOps hsmmc_ops = {
    .read = hsmmc_read,
    .write = hsmmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void hsmmc_instance_init(Object *obj)
{
    ZuneHsmmc *s = ZUNE_HSMMC(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(obj), "sd-bus");
    memory_region_init_io(&s->mr, obj, &hsmmc_ops, s, "zunehd.hsmmc", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const TypeInfo zune_hsmmc_typeinfo = {
    .name = TYPE_ZUNE_HSMMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ZuneHsmmc),
    .instance_init = hsmmc_instance_init,
};

/* ---------------------------------------------------------------------- */
/* host1x (command DMA + sync points) @0x50000000 and display controller   */
/*
 * Layout matches the Tegra host1x family and the guest's own accesses
 * (libnvrm_impl channel setup 0xc096b208.., sync init via NvRm regw):
 *   channel n @ 0x50000000 + n*0x4000: +0x14 DMASTART, +0x18 DMAPUT,
 *     +0x1C DMAGET, +0x20 DMAEND, +0x24 DMACTRL (bit0 stop, bit1 get-reset,
 *     bit2 init-get: GET := PUT)
 *   sync @ 0x50003000: +0x400+4n SYNCPT_n, +0x500+4n INT_THRESH_n,
 *     +0x600+4n SYNCPT_BASE_n, +0x700 CPU_INCR (bit mask)
 * Command words (physical pushbuffers in the carveout):
 *   op[31:28]: 0 SETCLASS(off[27:16], class[15:6], mask[5:0]),
 *   1 INCR / 2 NONINCR (off[27:16], count[15:0]), 3 MASK(off, mask[15:0]),
 *   4 IMM(off, data[15:0]), 5 RESTART(addr[27:0]<<4),
 *   6 GATHER(off, insert[15], incr[14], count[13:0]; next word = address).
 * Method 0 of every client class is INCR_SYNCPT ([15:8] cond, [7:0] index);
 * it is executed immediately (the work it would wait for is already done).
 * The display controller (class guessed 0x70/0x71 until confirmed) receives
 * method writes as register writes at word offset = method; the CPU also
 * reaches it directly at 0x54200000 (DC A) / 0x54240000 (DC B).
 */
#define ZUNEHD_HOST1X_BASE  0x50000000
#define HOST1X_NCH          8
#define HOST1X_NSYNCPT      32
#define ZUNEHD_DC_BASE      0x54200000
#define DC_NREGS            0x1000

typedef struct ZuneDC {
    MemoryRegion mr;
    uint32_t regs[DC_NREGS];
    QemuConsole *con;
    int idx;
    struct ZuneHost1x *h;
} ZuneDC;

/*
 * Sync point threshold interrupts (NvRm ISR FUN_c0968a30, IRQ 65):
 *   +0x3040 THRESH_CPU0_INT_STATUS (bit per sync point, W1C)
 *   +0x3050/+0x3054 interrupt enables, 2 bits per sync point (bit 2*i = CPU0)
 *   +0x3500+4i threshold (16 bit). The ISR wakes waiters and re-arms.
 * Indirect register access from a channel (host class methods 0x2D INDOFF,
 * 0x2E INDDATA): INDOFF [31:28] byte enables, [27] autoinc, [25:18] module
 * (6 = 3D), [17:2] register, [0] read. A read-mode INDDATA slot still consumes
 * its (dummy) data word and pushes the register value into the channel's
 * output FIFO, which the CPU drains via FIFOSTAT[28:24] (OUTFENTRIES) and the
 * channel's INDDATA register (+0x0C). This is how NvRm saves 3D context
 * (FUN_c0967c6c), followed by a CPU increment of the 3D sync point.
 */
#define HOST1X_SYNCPT_IRQ   65
#define HOST1X_OFIFO        16384

typedef struct ZuneHost1x {
    MemoryRegion mr;
    qemu_irq syncpt_irq;
    uint32_t syncpt[HOST1X_NSYNCPT];
    uint32_t thresh[HOST1X_NSYNCPT];
    uint32_t base[HOST1X_NSYNCPT];
    uint32_t int_status;                /* 0x3040 */
    uint32_t sync_regs[0x400 / 4];      /* 0x3000..0x33ff */
    struct {
        uint32_t start, put, get, end, ctrl;
        uint32_t cls;
        uint32_t indoff;
        uint32_t ofifo[HOST1X_OFIFO];
        unsigned of_head, of_count;
    } ch[HOST1X_NCH];
    ZuneDC dc[2];
    QEMUTimer *vsync;
} ZuneHost1x;

static void host1x_syncpt_update(ZuneHost1x *h)
{
    for (int i = 0; i < HOST1X_NSYNCPT; i++) {
        uint32_t en = h->sync_regs[(0x50 + 4 * (i / 16)) / 4] >> (2 * (i % 16));
        if ((en & 1) &&
            (int16_t)((uint16_t)h->syncpt[i] - (uint16_t)h->thresh[i]) >= 0) {
            h->int_status |= 1u << i;
        }
    }
    qemu_set_irq(h->syncpt_irq, h->int_status != 0);
}

static void host1x_syncpt_incr(ZuneHost1x *h, unsigned id, const char *who)
{
    id &= HOST1X_NSYNCPT - 1;
    h->syncpt[id]++;
    qemu_log_mask(LOG_UNIMP, "zunehd: host1x syncpt %u -> %u (%s)\n", id,
                  h->syncpt[id], who);
    host1x_syncpt_update(h);
}

static void host1x_ofifo_push(ZuneHost1x *h, int ch, uint32_t v)
{
    if (h->ch[ch].of_count >= HOST1X_OFIFO) {
        warn_report_once("zunehd: host1x output FIFO overflow");
        return;
    }
    h->ch[ch].ofifo[(h->ch[ch].of_head + h->ch[ch].of_count++) % HOST1X_OFIFO] = v;
}

static uint32_t host1x_ofifo_pop(ZuneHost1x *h, int ch)
{
    uint32_t v;

    if (!h->ch[ch].of_count) {
        return 0;
    }
    v = h->ch[ch].ofifo[h->ch[ch].of_head];
    h->ch[ch].of_head = (h->ch[ch].of_head + 1) % HOST1X_OFIFO;
    h->ch[ch].of_count--;
    return v;
}

/* Host class INDDATA: one indirect register access at the current INDOFF. */
static void host1x_inddata(ZuneHost1x *h, int ch, uint32_t val)
{
    uint32_t io = h->ch[ch].indoff;
    unsigned mod = (io >> 18) & 0xff, reg = (io >> 2) & 0xffff;

    if (mod == 6) {                     /* 3D */
        if (io & 1) {
            host1x_ofifo_push(h, ch, zune_gr3d_reg_read(reg));
        } else {
            zune_gr3d_method(reg, val);
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "zunehd: host1x ch%d indirect %s module %u "
                      "reg 0x%04x\n", ch, (io & 1) ? "read" : "write", mod, reg);
        if (io & 1) {
            host1x_ofifo_push(h, ch, 0);
        }
    }
    if (io & (1u << 27)) {              /* autoinc */
        reg = (reg + 1) & 0xffff;
        h->ch[ch].indoff = (io & ~(0xffffu << 2)) | (reg << 2);
    }
}

static void dc_reg_write(ZuneDC *dc, uint32_t idx, uint32_t val, bool via_ch);

/*
 * 2D engines. Register use as seen in the guest's command streams:
 * class 0x51 (G2, trigger reg = method 0x009, here 0x03A dstps):
 *   0x01F controlmain ([6] solid fill, [17:16] bpp 0=8 1=16 2=32), 0x020 rop,
 *   0x02B dstba, 0x02E dststride, 0x031 srcba, 0x033 srcstride,
 *   0x035 fill colour, 0x038 dstsize (h<<16|w), 0x039 srcps, 0x03A dstps (y<<16|x)
 * class 0x52 (stretch blit, trigger 0x038): 0x01F controlmain (bpp [17:16]),
 *   0x02B/0x02E dst, 0x031/0x033 src, 0x037 srcsize / 0x038 dstsize
 *   ((h-1)<<16 | w), 0x011/0x013 DDA steps (0x1000 = 1.0).
 */
static uint32_t g2_regs[2][0x80];

static void g2_execute(int unit)
{
    uint32_t *r = g2_regs[unit];
    int bpp = 1 << ((r[0x1f] >> 16) & 3);
    hwaddr dst = r[0x2b], src = r[0x31];
    uint32_t dstride = r[0x2e], sstride = r[0x33];
    int w, hgt, sw, sh, dx = 0, dy = 0, sx = 0, sy = 0;
    bool fill = false;

    if (unit == 0) {                    /* class 0x51 */
        w = r[0x38] & 0xffff;
        hgt = r[0x38] >> 16;
        dx = r[0x3a] & 0xffff;
        dy = r[0x3a] >> 16;
        sx = r[0x39] & 0xffff;
        sy = r[0x39] >> 16;
        sw = w;
        sh = hgt;
        fill = r[0x1f] & (1u << 6);
    } else {                            /* class 0x52 */
        w = r[0x38] & 0xffff;
        hgt = (r[0x38] >> 16) + 1;
        sw = r[0x37] & 0xffff;
        sh = (r[0x37] >> 16) + 1;
    }
    if (w <= 0 || hgt <= 0 || w > 4096 || hgt > 4096 || !dst || !dstride) {
        return;
    }
    /*
     * class 0x52 0x01C sbformat = dst_fmt << 8 | src_fmt (grate host1x-gr2d.c):
     * 14 = BGRA8888, 15 = RGBA8888. The UI's textures and 3D render target
     * are RGBA (byte 0 = R; checked against the lock-screen wallpaper PNG)
     * while the display window is B8G8R8A8, so the compositor's back->front
     * stretch blit converts 15 -> 14 and swaps R/B.
     */
    bool swap_rb = false;
    if (unit == 1 && bpp == 4) {
        uint32_t sf = g2_regs[1][0x1c] & 0xff, df = (g2_regs[1][0x1c] >> 8) & 0xff;
        static uint32_t seen[8];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen; i++) {
            known |= seen[i] == g2_regs[1][0x1c];
        }
        if (!known && nseen < 8) {
            seen[nseen++] = g2_regs[1][0x1c];
            info_report("zunehd: SB copy sbformat 0x%04x (src %u dst %u) controlmain 0x%08x",
                        g2_regs[1][0x1c], sf, df, r[0x1f]);
        }
        swap_rb = (sf == 14 && df == 15) || (sf == 15 && df == 14);
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: g2%c %s %dx%d bpp%d dst 0x%08" HWADDR_PRIx
                  " +%d,%d src 0x%08" HWADDR_PRIx " colour 0x%08x\n",
                  unit ? 'B' : 'A', fill ? "fill" : "copy", w, hgt, bpp * 8,
                  dst, dx, dy, src, r[0x35]);
    g_autofree uint8_t *line = g_malloc(w * bpp);
    g_autofree uint8_t *sline = g_malloc((sw > 0 ? sw : 1) * bpp);
    for (int y = 0; y < hgt; y++) {
        hwaddr da = dst + (hwaddr)(dy + y) * dstride + dx * bpp;
        if (fill) {
            for (int x = 0; x < w; x++) {
                memcpy(line + x * bpp, &r[0x35], bpp);
            }
        } else {
            int syy = sy + (sh == hgt ? y : y * sh / hgt);
            address_space_read(&address_space_memory,
                               src + (hwaddr)syy * sstride + sx * bpp,
                               MEMTXATTRS_UNSPECIFIED, sline, sw * bpp);
            for (int x = 0; x < w; x++) {
                int sxx = sw == w ? x : x * sw / w;
                memcpy(line + x * bpp, sline + sxx * bpp, bpp);
                if (swap_rb) {
                    uint8_t t = line[x * bpp];
                    line[x * bpp] = line[x * bpp + 2];
                    line[x * bpp + 2] = t;
                }
            }
        }
        address_space_write(&address_space_memory, da, MEMTXATTRS_UNSPECIFIED,
                            line, w * bpp);
    }
}

/*
 * 3D method trace for offline analysis (tools\g3ddis): set ZUNEHD_GR3D_TRACE to a
 * file path; each class-0x60 method write is appended as 3 LE words
 * {class | ch << 16, offset, value}.
 */
static FILE *gr3d_trace;

static void gr3d_trace_write(int ch, uint32_t cls, uint32_t off, uint32_t val)
{
    static bool init;
    uint32_t rec[3] = { cpu_to_le32(cls | (ch << 16)), cpu_to_le32(off),
                        cpu_to_le32(val) };

    if (!init) {
        const char *p = getenv("ZUNEHD_GR3D_TRACE");
        init = true;
        if (p && *p) {
            gr3d_trace = fopen(p, "wb");
            info_report("zunehd: 3D method trace -> %s", p);
        }
    }
    if (gr3d_trace) {
        fwrite(rec, sizeof(rec), 1, gr3d_trace);
        if (off == 0x123) {             /* draw: keep the file usable if killed */
            fflush(gr3d_trace);
        }
    }
}

/* One method write from a channel, in the channel's current class. */
static void host1x_method(ZuneHost1x *h, int ch, uint32_t off, uint32_t val)
{
    uint32_t cls = h->ch[ch].cls;

    if (cls == 0x60) {
        gr3d_trace_write(ch, cls, off, val);
        if (off != 0) {
            zune_gr3d_method(off, val);     /* zunehd-gr3d.c (draws run here) */
            return;
        }
    }

    if (cls == 0x70 || cls == 0x71) {
        dc_reg_write(&h->dc[cls - 0x70], off, val, true);
        return;
    }
    if ((cls == 0x51 || cls == 0x52) && off != 0 && off < 0x80) {
        int unit = cls - 0x51;
        g2_regs[unit][off] = val;
        if (off == (g2_regs[unit][0x09] & 0x7f)) {
            g2_execute(unit);
        }
        return;
    }
    if (off == 0) {
        char who[24];
        snprintf(who, sizeof(who), "class 0x%x", cls);
        host1x_syncpt_incr(h, val & 0xff, who);
        return;
    }
    if (cls == 0x01 && off == 0x2d) {   /* INDOFF */
        h->ch[ch].indoff = val;
        return;
    }
    if (cls == 0x01 && off == 0x2e) {   /* INDDATA */
        host1x_inddata(h, ch, val);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: host1x ch%d class 0x%x method 0x%03x <- "
                  "0x%08x\n", ch, cls, off, val);
}

static uint32_t host1x_fetch(hwaddr a)
{
    return address_space_ldl_le(&address_space_memory, a,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

/* Execute words [a, a+4*n) as a command stream; returns false on RESTART. */
static void host1x_exec(ZuneHost1x *h, int ch, hwaddr a, hwaddr end, int depth)
{
    while (a < end) {
        uint32_t w = host1x_fetch(a);
        uint32_t op = w >> 28, off = (w >> 16) & 0xfff;
        a += 4;
        switch (op) {
        case 0:     /* SETCLASS */
            h->ch[ch].cls = (w >> 6) & 0x3ff;
            for (int b = 0; b < 6; b++) {
                if (w & (1u << b)) {
                    host1x_method(h, ch, off + b, host1x_fetch(a));
                    a += 4;
                }
            }
            qemu_log_mask(LOG_UNIMP, "zunehd: host1x ch%d SETCLASS 0x%x\n", ch,
                          h->ch[ch].cls);
            break;
        case 1:     /* INCR */
        case 2:     /* NONINCR */
            for (uint32_t i = 0; i < (w & 0xffff); i++, a += 4) {
                host1x_method(h, ch, op == 1 ? off + i : off, host1x_fetch(a));
            }
            break;
        case 3:     /* MASK */
            for (int b = 0; b < 16; b++) {
                if (w & (1u << b)) {
                    host1x_method(h, ch, off + b, host1x_fetch(a));
                    a += 4;
                }
            }
            break;
        case 4:     /* IMM */
            host1x_method(h, ch, off, w & 0xffff);
            break;
        case 5:     /* RESTART: continue at a new address (ring wrap) */
            h->ch[ch].get = (w & 0x0fffffff) << 4;
            return;
        case 6: {   /* GATHER */
            uint32_t n = w & 0x3fff;
            hwaddr ga = host1x_fetch(a);
            a += 4;
            if (depth > 2) {
                break;
            }
            if (w & (1u << 15)) {           /* insert: methods at off */
                for (uint32_t i = 0; i < n; i++) {
                    host1x_method(h, ch, (w & (1u << 14)) ? off + i : off,
                                  host1x_fetch(ga + 4 * i));
                }
            } else {
                host1x_exec(h, ch, ga, ga + 4 * n, depth + 1);
            }
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP, "zunehd: host1x ch%d opcode %u word "
                          "0x%08x @0x%08" HWADDR_PRIx "\n", ch, op, w, a - 4);
            break;
        }
    }
    if (depth == 0) {
        h->ch[ch].get = end;
    }
}

static void host1x_kick(ZuneHost1x *h, int ch)
{
    int guard = 0;

    if (h->ch[ch].ctrl & 1) {
        return;                         /* DMASTOP */
    }
    while (h->ch[ch].get != h->ch[ch].put && guard++ < 16) {
        uint32_t get = h->ch[ch].get, put = h->ch[ch].put;
        hwaddr end = put > get ? put : h->ch[ch].end;
        host1x_exec(h, ch, get, end, 0);
        if (h->ch[ch].get == get) {
            break;                      /* nothing consumed */
        }
    }
}

static uint64_t host1x_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneHost1x *h = opaque;

    if ((off & 0x3fff) >= 0x3000) {
        off &= 0x3fff;                  /* sync block mirrored per channel */
    }
    if (off >= 0x3400 && off < 0x3400 + 4 * HOST1X_NSYNCPT) {
        return h->syncpt[(off - 0x3400) / 4];
    }
    if (off >= 0x3500 && off < 0x3500 + 4 * HOST1X_NSYNCPT) {
        return h->thresh[(off - 0x3500) / 4];
    }
    if (off >= 0x3600 && off < 0x3600 + 4 * HOST1X_NSYNCPT) {
        return h->base[(off - 0x3600) / 4];
    }
    if (off == 0x3040) {
        return h->int_status;
    }
    if (off >= 0x3000 && off < 0x3400) {
        return h->sync_regs[(off - 0x3000) / 4];
    }
    if ((off & 0x3fff) < 0x40 && off / 0x4000 < HOST1X_NCH) {
        int ch = off / 0x4000;
        switch (off & 0x3fff) {
        case 0x00:                          /* FIFOSTAT: [28:24] OUTFENTRIES */
            return MIN(h->ch[ch].of_count, 31u) << 24;
        case 0x0c: return host1x_ofifo_pop(h, ch);     /* INDDATA */
        case 0x14: return h->ch[ch].start;
        case 0x18: return h->ch[ch].put;
        case 0x1c: return h->ch[ch].get;
        case 0x20: return h->ch[ch].end;
        case 0x24: return h->ch[ch].ctrl;
        }
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: host1x read  [0x%08" HWADDR_PRIx "]  "
                  "pc~0x%08x\n", ZUNEHD_HOST1X_BASE + off, zune_guest_pc());
    return 0;
}

static void host1x_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneHost1x *h = opaque;

    if ((off & 0x3fff) >= 0x3000) {
        off &= 0x3fff;                  /* e.g. NvRm's ch*0x4000 + 0x3700 */
    }
    if (off >= 0x3400 && off < 0x3400 + 4 * HOST1X_NSYNCPT) {
        h->syncpt[(off - 0x3400) / 4] = val;
        host1x_syncpt_update(h);
        return;
    }
    if (off >= 0x3500 && off < 0x3500 + 4 * HOST1X_NSYNCPT) {
        h->thresh[(off - 0x3500) / 4] = val;
        host1x_syncpt_update(h);
        return;
    }
    if (off == 0x3040) {                    /* THRESH_CPU0_INT_STATUS: W1C */
        h->int_status &= ~(uint32_t)val;
        host1x_syncpt_update(h);
        return;
    }
    if (off == 0x3050 || off == 0x3054) {   /* threshold interrupt enables */
        h->sync_regs[(off - 0x3000) / 4] = val;
        host1x_syncpt_update(h);
        return;
    }
    if (off >= 0x3600 && off < 0x3600 + 4 * HOST1X_NSYNCPT) {
        h->base[(off - 0x3600) / 4] = val;
        return;
    }
    if (off == 0x3700) {                    /* SYNCPT_CPU_INCR */
        for (int i = 0; i < HOST1X_NSYNCPT; i++) {
            if (val & (1u << i)) {
                host1x_syncpt_incr(h, i, "cpu");
            }
        }
        return;
    }
    if (off >= 0x3000 && off < 0x3400) {
        h->sync_regs[(off - 0x3000) / 4] = val;
        qemu_log_mask(LOG_UNIMP, "zunehd: host1x sync [0x%03" HWADDR_PRIx
                      "] <- 0x%08" PRIx64 "\n", off - 0x3000, val);
        return;
    }
    if ((off & 0x3fff) < 0x40 && off / 0x4000 < HOST1X_NCH) {
        int ch = off / 0x4000;
        switch (off & 0x3fff) {
        case 0x14: h->ch[ch].start = val; return;
        case 0x18:
            h->ch[ch].put = val;
            host1x_kick(h, ch);
            return;
        case 0x1c: h->ch[ch].get = val; return;
        case 0x20: h->ch[ch].end = val; return;
        case 0x24:
            h->ch[ch].ctrl = val;
            if ((val & 6) == 6) {
                h->ch[ch].get = h->ch[ch].put;  /* GETRST + INITGET */
            }
            host1x_kick(h, ch);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: host1x write [0x%08" HWADDR_PRIx "] <- "
                  "0x%08" PRIx64 "  pc~0x%08x\n", ZUNEHD_HOST1X_BASE + off, val,
                  zune_guest_pc());
}

static const MemoryRegionOps host1x_ops = {
    .read = host1x_read,
    .write = host1x_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Display controller. Provisional Tegra-family word offsets (to be confirmed
 * against the guest's writes, which are all logged):
 *   0x000 GENERAL_INCR_SYNCPT  0x028 CONT_SYNCPT_VSYNC ([8] en, [7:0] id)
 *   0x041 STATE_CONTROL (ACT_REQ bits self-clear)  0x042 WINDOW_HEADER [6:4]
 *   0x406..0x40A REF_TO_SYNC, SYNC_WIDTH, BACK_PORCH, DISP_ACTIVE (V<<16|H),
 *   FRONT_PORCH (guest sets active 0x01E00110 = 272x480)
 *   window bank 0x500..0x8FF: 0x601.. H filter, 0x619.. V filter, 0x70F.. blend
 *   window bank (selected by WINDOW_HEADER): 0x700 WIN_OPTIONS ([30] enable)
 *   0x703 COLOR_DEPTH  0x704 POSITION  0x705 SIZE  0x706 PRESCALED_SIZE
 *   0x70A LINE_STRIDE  0x800 WINBUF_START_ADDR  0x806 H_OFFSET  0x808 V_OFFSET
 */
#define DC_WIN_FIRST    0x500           /* windowed: palette, filters, win, winbuf */
#define DC_WIN_LAST     0x8ff
#define DC_NWIN         3

typedef struct ZuneDCWin {
    uint32_t r[DC_WIN_LAST - DC_WIN_FIRST + 1];
} ZuneDCWin;

static ZuneDCWin zune_dc_win[2][DC_NWIN];

static uint32_t *dc_reg_ptr(ZuneDC *dc, uint32_t idx, int win)
{
    if (idx >= DC_WIN_FIRST && idx <= DC_WIN_LAST) {
        return &zune_dc_win[dc->idx][win].r[idx - DC_WIN_FIRST];
    }
    return &dc->regs[idx & (DC_NREGS - 1)];
}

static void dc_reg_write(ZuneDC *dc, uint32_t idx, uint32_t val, bool via_ch)
{
    qemu_log_mask(LOG_UNIMP, "zunehd: dc%c %s [0x%03x] <- 0x%08x\n",
                  'A' + dc->idx, via_ch ? "ch " : "cpu", idx, val);
    if (idx == 0x000) {                 /* GENERAL_INCR_SYNCPT */
        host1x_syncpt_incr(dc->h, val & 0xff, "dc general");
        return;
    }
    if (idx >= DC_WIN_FIRST && idx <= DC_WIN_LAST) {
        uint32_t sel = (dc->regs[0x042] >> 4) & 7;
        for (int w = 0; w < DC_NWIN; w++) {
            if (sel & (1u << w)) {
                *dc_reg_ptr(dc, idx, w) = val;
            }
        }
        return;
    }
    if (idx == 0x041) {
        val &= ~0x1f1fu;                /* ACT_REQ / UPDATE bits: done */
    }
    dc->regs[idx & (DC_NREGS - 1)] = val;
}

static uint64_t dc_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneDC *dc = opaque;
    uint32_t idx = off / 4;
    int w = 0;
    uint32_t sel = (dc->regs[0x042] >> 4) & 7;

    while (w < DC_NWIN - 1 && sel && !(sel & (1u << w))) {
        w++;
    }
    return *dc_reg_ptr(dc, idx, w);
}

static void dc_mmio_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    dc_reg_write(opaque, off / 4, val, false);
}

static const MemoryRegionOps dc_ops = {
    .read = dc_mmio_read,
    .write = dc_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Composite enabled windows into the console surface (32bpp xRGB). */
static bool dc_gfx_update(void *opaque)
{
    ZuneDC *dc = opaque;
    uint32_t act = dc->regs[0x409];     /* DC_DISP_DISP_ACTIVE: V<<16 | H */
    int W = act & 0xffff, H = act >> 16;
    DisplaySurface *s;
    uint8_t *dst;
    int dstride;

    if (W < 16 || H < 16 || W > 2048 || H > 2048) {
        W = 272;                        /* Zune HD panel scanout, portrait */
        H = 480;
    }
    if (qemu_console_get_width(dc->con, 0) != W ||
        qemu_console_get_height(dc->con, 0) != H) {
        qemu_console_resize(dc->con, W, H);
    }
    s = qemu_console_surface(dc->con);
    dst = surface_data(s);
    dstride = surface_stride(s);
    if (getenv("ZUNEHD_DCLOG")) {       /* log window state changes */
        static char last[2][1024];
        GString *g = g_string_new(NULL);
        for (int w = 0; w < DC_NWIN; w++) {
            uint32_t *r = zune_dc_win[dc->idx][w].r;
#define WR(i) r[(i) - DC_WIN_FIRST]
            g_string_append_printf(g, "\n  win%c opt %08x depth %u pos %08x size %08x "
                                   "psize %08x stride %08x base %08x+h%x+v%x "
                                   "blend %08x %08x %08x %08x",
                                   'A' + w, WR(0x700), WR(0x703), WR(0x704), WR(0x705),
                                   WR(0x706), WR(0x70a), WR(0x800), WR(0x806), WR(0x808),
                                   WR(0x70f), WR(0x710), WR(0x711), WR(0x712));
#undef WR
        }
        if (strcmp(last[dc->idx], g->str)) {
            g_strlcpy(last[dc->idx], g->str, sizeof(last[0]));
            info_report("zunehd: dc%c windows (t=%" PRId64 " ms):%s", 'A' + dc->idx,
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), g->str);
        }
        g_string_free(g, true);
    }
    for (int y = 0; y < H; y++) {
        memset(dst + y * dstride, 0, W * 4);
    }
    for (int w = 0; w < DC_NWIN; w++) {
        uint32_t *r = zune_dc_win[dc->idx][w].r;
#define WR(i) r[(i) - DC_WIN_FIRST]
        uint32_t opt = WR(0x700), depth = WR(0x703) & 0x1f;
        int px = WR(0x704) & 0x1fff, py = (WR(0x704) >> 16) & 0x1fff;
        int ww = WR(0x705) & 0x1fff, wh = (WR(0x705) >> 16) & 0x1fff;
        uint32_t stride = WR(0x70a) & 0xffff;
        hwaddr base = WR(0x800) + WR(0x806) + (hwaddr)WR(0x808) * stride;
        int bpp = (depth == 6 || depth == 5 || depth == 7 || depth == 4) ? 2 : 4;
        g_autofree uint8_t *line = NULL;
#undef WR
        if (!(opt & (1u << 30)) || !base || ww <= 0 || wh <= 0) {
            continue;
        }
        if (!stride) {
            stride = ww * bpp;
        }
        line = g_malloc(stride);
        for (int y = 0; y < wh && py + y < H; y++) {
            uint32_t *out = (uint32_t *)(dst + (py + y) * dstride) + px;
            address_space_read(&address_space_memory, base + (hwaddr)y * stride,
                               MEMTXATTRS_UNSPECIFIED, line, stride);
            for (int x = 0; x < ww && px + x < W; x++) {
                uint32_t c;
                if (bpp == 2) {
                    uint16_t v = lduw_le_p(line + 2 * x);
                    c = ((v & 0xf800) << 8) | ((v & 0x07e0) << 5) |
                        ((v & 0x001f) << 3);
                } else if (depth == 13) {       /* R8G8B8A8: swap R/B */
                    uint32_t v = ldl_le_p(line + 4 * x);
                    c = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
                } else {                        /* B8G8R8A8 = xRGB in LE */
                    c = ldl_le_p(line + 4 * x) & 0xffffff;
                }
                out[x] = c;
            }
        }
    }
    qemu_console_update(dc->con, 0, 0, W, H);
    return true;
}

static const GraphicHwOps dc_gfx_ops = {
    .gfx_update = dc_gfx_update,
};

/* ~60 Hz: drive the vsync sync point (DC_CMD_CONT_SYNCPT_VSYNC). */
static void host1x_vsync_tick(void *opaque)
{
    ZuneHost1x *h = opaque;

    for (int i = 0; i < 2; i++) {
        uint32_t v = h->dc[i].regs[0x028];
        if (v & 0x100) {
            h->syncpt[v & (HOST1X_NSYNCPT - 1)]++;
        }
    }
    host1x_syncpt_update(h);
    timer_mod(h->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void zunehd_gfx_init(MemoryRegion *sysmem, qemu_irq syncpt_irq)
{
    ZuneHost1x *h = g_new0(ZuneHost1x, 1);

    h->syncpt_irq = syncpt_irq;
    memory_region_init_io(&h->mr, NULL, &host1x_ops, h, "zunehd.host1x",
                          0x20000);
    memory_region_add_subregion(sysmem, ZUNEHD_HOST1X_BASE, &h->mr);
    for (int i = 0; i < 2; i++) {
        ZuneDC *dc = &h->dc[i];
        dc->idx = i;
        dc->h = h;
        memory_region_init_io(&dc->mr, NULL, &dc_ops, dc,
                              i ? "zunehd.dcB" : "zunehd.dcA", 0x40000);
        memory_region_add_subregion(sysmem, ZUNEHD_DC_BASE + i * 0x40000,
                                    &dc->mr);
    }
    h->dc[0].con = qemu_graphic_console_create(NULL, 0, &dc_gfx_ops, &h->dc[0]);
    h->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, host1x_vsync_tick, h);
    timer_mod(h->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

/* ---------------------------------------------------------------------- */
/* Image patches (high-level stand-ins for hardware that is not emulated)  */
/*
 * Applied to B000FF records as they are loaded, only where the original bytes
 * match, so other firmware builds are left alone.
 *
 * AVP: the audio driver (libnvwavedev -> libnvddk_audiomixer_impl) calls
 * NvRmLoadLibraryEx (libnvrm_impl 0xC096AF3C, XIP at PA 0x00734F3C) to load
 * codecs on the AVP. That first starts the AVP, then NvRmTransportConnect to
 * "RPC_AVP_PORT" with an infinite timeout; the AVP is not emulated, so device
 * manager hangs there and no later driver (display, GWES) loads. With avp=off
 * the function returns NvError 2 immediately and the audio driver fails init.
 */
typedef struct ZunePatch {
    const char *what;
    uint32_t pa;
    uint8_t orig[8];
    uint8_t repl[8];
} ZunePatch;

static const ZunePatch zunehd_avp_patches[] = {
    { "NvRmLoadLibraryEx -> NvError 2 (no AVP)", 0x00734f3c,
      { 0xf0, 0x47, 0x2d, 0xe9, 0x04, 0xd0, 0x4d, 0xe2 },   /* stmdb; sub sp */
      { 0x02, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 } }, /* mov r0,#2; bx lr */
    /* The audio mixer's error path still frees the library, which sends an
     * RPC on the never-created port (NULL deref at 0xC096A0A8). */
    { "NvRmFreeLibrary -> success (no AVP)", 0x00735044,
      { 0xf0, 0x43, 0x2d, 0xe9, 0x20, 0xd0, 0x4d, 0xe2 },
      { 0x00, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 } }, /* mov r0,#0; bx lr */
    /* Even after a failed init the audio mixer leaves threads parked inside
     * NvRm holding a power mutex; the eMMC driver's resume
     * (NvRmPowerModuleClockControl) then blocks on it and every later page-in
     * from ZBINFS hangs. Audio needs the AVP anyway: make WAV_Init (libnvwavedev
     * 0xC0875504) fail at once so the mixer never starts. */
    { "WAV_Init -> 0 (audio disabled, no AVP)", 0x00658504,
      { 0x10, 0x40, 0x2d, 0xe9, 0x0a, 0x3c, 0xa0, 0xe3 },
      { 0x00, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 } }, /* mov r0,#0; bx lr */
    /* NvRm PMU init (libnvrm_impl FUN_c096252c @0xC096255C) only registers the
     * PMU interrupt on silicon (chip mode 1); we run as QuickTurn (mode 2), so
     * the WM8350 power key never reached the OS. Drop the 'bne'. */
    { "NvRm PMU interrupt also in emulation mode", 0x0072c55c,
      { 0x01, 0x00, 0x54, 0xe3, 0x35, 0x00, 0x00, 0x1a },   /* cmp r4,#1; bne */
      { 0x01, 0x00, 0x54, 0xe3, 0x00, 0x00, 0xa0, 0xe1 } }, /* cmp r4,#1; nop */
    /* ...but skip the follow-up FUN_c0956e78 @0xC0962638: on a non-silicon chip
     * it marks all 10 PMU rails unsupported (0xFFFFFFFF) and the display never
     * powers up. Without the patch above it was never called. */
    /* ...and keep the "PMU present" flag (pmu+0x20) clear, as without the
     * patch, so rail/voltage requests stay no-ops instead of going to our
     * minimal WM8350 model (with it set the display rails never came up). */
    { "NvRm PMU: leave PMU-present flag clear", 0x0072c568,
      { 0x05, 0x00, 0xa0, 0xe1, 0x20, 0x30, 0xc5, 0xe5 },   /* cpy r0,r5; strb r3,[r5,#0x20] */
      { 0x05, 0x00, 0xa0, 0xe1, 0x00, 0x00, 0xa0, 0xe1 } }, /* cpy r0,r5; nop */
    /* USB PHY PLL setup (NvRmAnalogInterfaceControl -> FUN_c09557ec) only knows
     * 13/19.2/12/26 MHz oscillators; QuickTurn mode reports 8330 kHz, so it
     * returned NvError 2 and NvDdkUsbfOpen failed. Use the 13 MHz entry. */
    { "USB PHY: accept QuickTurn oscillator (use 13 MHz table)", 0x0071f824,
      { 0x02, 0x00, 0xa0, 0x23, 0x74, 0x00, 0x00, 0x2a },   /* movcs r0,#2; bcs */
      { 0x00, 0x70, 0xa0, 0x23, 0x00, 0x00, 0xa0, 0xe1 } }, /* movcs r7,#0; nop */
    /* QuickTurn clock init (minor 1) seeds NvRm's rate cache with constants and
     * never computes PLLP_OUT1 (id 0xE), the PLLA input -> PLLA = 0 Hz and
     * FUN_c0954abc (audio clock, PLLA_OUT0 divider) divides by it
     * (STATUS_INTEGER_DIVIDE_BY_ZERO in the audio mixer). Use the PLLA rate NvRm
     * itself reports at boot ("PLLA0: 12288 Khz") on this path. */
    { "NvRm audio clock: PLLA rate 12288 kHz (QuickTurn)", 0x0071ebc0,
      { 0x0c, 0x40, 0x9d, 0xe5, 0x04, 0x30, 0x86, 0xe5 },   /* ldr r4,[sp,#0xc]; str r3,[r6,#4] */
      { 0x03, 0x4a, 0xa0, 0xe3, 0x04, 0x30, 0x86, 0xe5 } }, /* mov r4,#0x3000; str r3,[r6,#4] */
    { "NvRm PMU: keep rail table", 0x0072c634,
      { 0x06, 0x00, 0xa0, 0xe1, 0x0e, 0xd2, 0xff, 0xeb },   /* cpy r0,r6; bl */
      { 0x06, 0x00, 0xa0, 0xe1, 0x00, 0x00, 0xa0, 0xe1 } }, /* cpy r0,r6; nop */
    /* MtpSvc.dll (EXT, VA 0x416C0000, code at PA 0x025A3000): plain MTP without
     * the key vault. KEY1: stays ERROR_NOT_READY (PKV header is wrapped with the
     * chip key), so Initialize FUN_416d2f74's device-cert fetch (IOCTL 0x220018)
     * fails and the whole service exits. Ignore that failure... */
    { "MtpSvc: ignore KEY1 cert-fetch failure (no key vault)", 0x025b5088,
      { 0x02, 0x00, 0x00, 0x5a, 0x16, 0x31, 0xa0, 0xe3 },   /* bpl +2; mov r3,#.. */
      { 0x02, 0x00, 0x00, 0xea, 0x16, 0x31, 0xa0, 0xe3 } }, /* b   +2; mov r3,#.. */
    /* ...and force the MTPZ flag (svc+0x28, registry DeviceInfo\MTPZ) to 0 in
     * FUN_416cb71c, so OpenSession mounts storage without the MTPZ handshake.
     * r8 is 0 throughout this function. */
    { "MtpSvc: MTPZ off (no device keys)", 0x025ad808,
      { 0x28, 0x80, 0x85, 0x15, 0x28, 0x30, 0x95, 0xe5 },   /* strne r8,[r5,#0x28]; ldr */
      { 0x28, 0x80, 0x85, 0xe5, 0x28, 0x30, 0x95, 0xe5 } }, /* str   r8,[r5,#0x28]; ldr */
    /* EXT is paged in from ZBINFS (eMMC), not run from the RAM copy above, so
     * tools\mkemmc.py applies the MtpSvc patches there too. zbinfs.dll (NK,
     * code VA 0xC0B71000 = PA 0x008B1000) verifies every page against a SHA
     * hash tree (zbinfs_ext.hash); a mismatch prints both digests (ZPrintHex)
     * and the page-in raises an exception. Accept mismatching leaf hashes:
     * FUN_c0b75a84 (full page) and FUN_c0b759a0 (partial page). */
    { "zbinfs: accept modified pages (full-page hash)", 0x008b5f34,
      { 0x00, 0x00, 0x50, 0xe3, 0x0c, 0x00, 0x00, 0x0a },   /* cmp r0,#0; beq ok */
      { 0x00, 0x00, 0x50, 0xe3, 0x0c, 0x00, 0x00, 0xea } }, /* cmp r0,#0; b   ok */
    { "zbinfs: accept modified pages (partial-page hash)", 0x008b5a2c,
      { 0x00, 0x00, 0x50, 0xe3, 0x0c, 0x00, 0x00, 0x0a },
      { 0x00, 0x00, 0x50, 0xe3, 0x0c, 0x00, 0x00, 0xea } },
};

static bool zunehd_patch_avp = true;

static void zunehd_apply_patches(uint8_t *data, uint32_t addr, uint32_t len)
{
    for (int i = 0; i < ARRAY_SIZE(zunehd_avp_patches); i++) {
        const ZunePatch *pt = &zunehd_avp_patches[i];

        /* avp=on only drops the patches that keep the AVP/audio path off;
         * the others (PMU, USB PHY, MtpSvc, zbinfs) stay. */
        if (!zunehd_patch_avp && (pt->pa == 0x00734f3c || pt->pa == 0x00735044 ||
                                  pt->pa == 0x00658504)) {
            continue;
        }
        if (pt->pa < addr || pt->pa + 8 > addr + len) {
            continue;
        }
        if ((pt->pa == 0x0072c55c || pt->pa == 0x0072c568 || pt->pa == 0x0072c634) &&
            getenv("ZUNEHD_NOPMUIRQ")) {
            continue;                   /* debug: keep NvRm's silicon-only check */
        }
        if ((pt->pa == 0x008b5f34 || pt->pa == 0x008b5a2c) && getenv("ZUNEHD_NOZBINFSPATCH")) {
            continue;                   /* debug: keep zbinfs hash checks */
        }
        if (memcmp(data + (pt->pa - addr), pt->orig, 8) == 0) {
            memcpy(data + (pt->pa - addr), pt->repl, 8);
            info_report("zunehd: patched %s @0x%08x", pt->what, pt->pa);
        } else {
            warn_report("zunehd: patch '%s' skipped: bytes differ", pt->what);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Windows CE B000FF (.bin) loader                                         */

static bool zunehd_load_b000ff(const char *path, uint32_t *entry, Error **errp)
{
    g_autofree uint8_t *buf = NULL;
    gsize len = 0, p;
    GError *gerr = NULL;
    unsigned nrec = 0;

    if (!g_file_get_contents(path, (gchar **)&buf, &len, &gerr)) {
        error_setg(errp, "cannot read %s: %s", path, gerr->message);
        g_error_free(gerr);
        return false;
    }
    if (len < 15 || memcmp(buf, "B000FF\n", 7) != 0) {
        error_setg(errp, "%s is not a Windows CE B000FF image", path);
        return false;
    }
    p = 15;
    while (p + 12 <= len) {
        uint32_t addr = ldl_le_p(buf + p);
        uint32_t rlen = ldl_le_p(buf + p + 4);
        uint32_t sum = ldl_le_p(buf + p + 8);
        p += 12;
        if (addr == 0 && sum == 0) {
            if (entry) {
                *entry = rlen;
            }
            info_report("zunehd: %s: %u records, entry 0x%08x", path, nrec,
                        rlen);
            return true;
        }
        if (p + rlen > len) {
            error_setg(errp, "%s: record at 0x%08x overruns file", path, addr);
            return false;
        }
        zunehd_apply_patches(buf + p, addr, rlen);
        rom_add_blob_fixed(path, buf + p, rlen, addr);
        p += rlen;
        nrec++;
    }
    error_setg(errp, "%s: missing B000FF terminator record", path);
    return false;
}

/* Boot-args block normally left in RAM by ZBoot. Only the fields the OAL
 * validates are filled; everything else is zero until reverse engineered. */
static void zunehd_add_bootargs(uint32_t emmc_instance)
{
    uint8_t *ba = g_malloc0(0x200);

    stl_le_p(ba + 0x10, 0x544f4f42);    /* 'BOOT' */
    stl_le_p(ba + 0x14, 0x1fc);         /* size */
    stl_le_p(ba + 0x110, 0x53475241);   /* 'ARGS' */
    stw_le_p(ba + 0x114, 1);
    stw_le_p(ba + 0x116, 0xd);          /* version expected by this OAL */
    /*
     * Debug transport config lives at +0x154 (FUN_8002bddc / FUN_8002bcf0):
     * +0x08 type: 1 = none (OAL default), 2 = JTAG DCC (cp14),
     *             3..7 = NvRm UART instance 0..4 at 115200, 16550 polling.
     */
    stl_le_p(ba + 0x154 + 0x08, 3);
    /*
     * Boot-device description handed to the eMMC block driver (libnvemmc
     * DSK_Init) through OAL IOCTLs 0x01013E97 / EA3 / EAF:
     *   +0x1DC must be 4; +0x1E0 = 4-byte flag (left 0);
     *   +0x1EC..+0x1F8 = { NvDdkBlockDevMgr device type (must be 0x11),
     *                      HSMMC instance, minor, DSK1 base }.
     * DSK_Init stores the base in dev+0x18 and FUN_c08c14b0 maps every DSK1
     * sector to base + N, in block-device sectors of 2048 bytes. The real
     * layout keeps a 215.5 MiB boot reserve (441344 native blocks) in front.
     * +0x1E4/+0x1E8 = NVIDIA partition table location {start, count} in
     * block-device sectors (OAL IOCTL 0x01013EAB, used by libnvfsstream
     * NFS_Open to find named partitions, e.g. KeyVault's "PKV").
     * All values below were read from the user's device (Lyra, 2026-11-07).
     */
    stl_le_p(ba + 0x1dc, 4);
    stl_le_p(ba + 0x1e0, 1);
    stl_le_p(ba + 0x1e4, 0x600);
    stl_le_p(ba + 0x1e8, 0x40);
    stl_le_p(ba + 0x1ec, 0x11);
    stl_le_p(ba + 0x1f0, emmc_instance);
    stl_le_p(ba + 0x1f4, 0x0e);
    stl_le_p(ba + 0x1f8, 441344 / 4);
    rom_add_blob_fixed("zunehd.bootargs", ba, 0x200, ZUNEHD_BOOTARGS_PA);
    g_free(ba);
}

static void zunehd_cpu_reset(void *opaque)
{
    ZuneHDMachineState *zms = opaque;
    CPUState *cs = CPU(zms->cpu);

    cpu_reset(cs);
    /*
     * StartUp is ARM code, entered with MMU and caches off. ZBoot leaves a
     * valid SVC stack; without one the first push wraps to 0xFFFFFFFC.
     * Use free RAM between the boot-args pages and the NK image (0x14000).
     */
    zms->cpu->env.regs[13] = ZUNEHD_INITIAL_SP;
    cpu_set_pc(cs, zms->entry);
}

/* ---------------------------------------------------------------------- */

static void zunehd_init(MachineState *machine)
{
    ZuneHDMachineState *zms = ZUNEHD_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *iram = g_new(MemoryRegion, 1);
    ZuneTimer *tmr = g_new0(ZuneTimer, 1);
    ZuneStub *apb, *ppsb;
    Error *err = NULL;

    if (machine->ram_size != ZUNEHD_SDRAM_SIZE) {
        error_report("zunehd: RAM size must be 128M (the OAL hardcodes it)");
        exit(EXIT_FAILURE);
    }

    /*
     * The CPU is in cluster 0 so the gdbstub lists it as process 1 (the AVP
     * in zunehd-avp.c is cluster 1 / process 2); with the CPU unclustered gdb
     * attached to the AVP first.
     */
    object_initialize_child(OBJECT(machine), "cpu-cluster", &zms->cpu_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&zms->cpu_cluster), "cluster-id", 0);
    zms->cpu = ARM_CPU(object_new(machine->cpu_type));
    object_property_add_child(OBJECT(&zms->cpu_cluster), "cpu", OBJECT(zms->cpu));
    /*
     * The Tegra ARM1136 implements the CP15 c7,c8 VA->PA translation ops and
     * the OAL uses them (FUN_8002e5d0: mcr p15,0,rX,c7,c8,1). QEMU's arm1136
     * model lacks ARM_FEATURE_VAPA, so those ops would raise UNDEF.
     */
    zms->cpu->env.features |= 1ULL << ARM_FEATURE_VAPA;
    qdev_realize(DEVICE(zms->cpu), NULL, &error_fatal);
    object_unref(OBJECT(zms->cpu));
    qdev_realize(DEVICE(&zms->cpu_cluster), NULL, &error_fatal);

    memory_region_add_subregion(sysmem, 0, machine->ram);

    memory_region_init_ram(iram, NULL, "zunehd.iram", ZUNEHD_IRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, ZUNEHD_IRAM_BASE, iram);

    /* Every non-RAM region from OEMAddressTable. */
    zune_stub_create("iram-hi",  ZUNEHD_IRAM_BASE + ZUNEHD_IRAM_SIZE,
                     1 * MiB - ZUNEHD_IRAM_SIZE);
    zune_stub_create("host1x",   0x50000000, 1 * MiB);
    zune_stub_create("gfx",      0x54000000, 4 * MiB);
    zune_stub_create("gart",     0x58000000, 16 * MiB);
    ppsb = zune_stub_create("ppsb", 0x60000000, 1 * MiB);
    zune_ppsb_stub = ppsb;
    /*
     * CAR PLL{C,M,P,A,U,D}_BASE @0x60006080..0x600060D0: libnvrm_impl
     * FUN_c0952bbc returns 0 Hz unless BYPASS (bit31) or ENABLE (bit30) is set,
     * and with ENABLE it computes osc*N/M â€” the guest writes ENABLE with M=0,
     * so callers divide by zero (audio mixer -> STATUS_INTEGER_DIVIDE_BY_ZERO).
     * Pin BYPASS so each PLL reports its input (oscillator) rate.
     */
    for (hwaddr pll = 0x60006080; pll <= 0x600060d0; pll += 0x10) {
        zune_stub_preset(ppsb, pll, 0x80000000);
        zune_stub_forceset(ppsb, pll, 0x80000000);
    }
    /*
     * PLLP_OUTA (0x600060A4): the bootloader normally sets PLLP_OUT1/OUT2 up
     * before CE runs. NvRm only reconfigures OUT2, so with 0 here the cached
     * PLLP_OUT1 rate stayed 0 -> PLLA (input PLLP_OUT1, id 0xE) = 0 Hz ->
     * PLLA_OUT0 divider (FUN_c0954abc) divides by zero when the audio mixer
     * sets the I2S clock. OUT1: RSTN | CLKEN | OVRRIDE, ratio 0x0D
     * (2 * PLLP / 15 = 28.8 MHz on silicon); OUT2 bits as the guest writes them.
     */
    zune_stub_preset(ppsb, 0x600060a4, 0x00070d07);
    /*
     * Crypto/bitstream engines @0x6001B000 and 0x6001D000 (libnvddk_misc
     * FUN_c08fc90c): idle when STATUS(+0x18) has bit0=0 (busy), bit3=1 (queue
     * empty), bit9=0. Report idle; operations themselves are not emulated.
     */
    zune_stub_preset(ppsb, 0x6001b018, 0x8);
    zune_stub_forceset(ppsb, 0x6001b018, 0x8);
    zune_stub_preset(ppsb, 0x6001d018, 0x8);
    zune_stub_forceset(ppsb, 0x6001d018, 0x8);
    apb = zune_stub_create("apb", 0x70000000, 1 * MiB);
    zune_apb_stub = apb;
    /*
     * APB_MISC_GP_HIDREV, decoded by NvRm FUN_80039e20: [15:8] chip id,
     * [7:4] major rev, [19:16] minor rev, [3:0] family. Major rev 0 is the
     * pre-silicon (FPGA/emulation) path, on which FUN_80030b48 uses the
     * relocation table compiled into the OAL for chip 0x16 (AP16). Silicon
     * revisions instead fetch the table pointer from boot ROM @0xFFFF0040,
     * which we do not have.
     */
    /*
     * Minor rev selects the pre-silicon flavour in libnvrm_impl FUN_c095ce84:
     * minor 0 = FPGA (all module clocks forced to 1 kHz, then EMC clock config
     * NULL-derefs DFS state), minor !=0 = QuickTurn (fixed 0x208A kHz clocks).
     */
    zune_stub_preset(apb, 0x70000804, 0x00011600);
    /*
     * APB_MISC_EMU_REVID [15:0] netlist, [31:16] patch. NvRmOpenNew only runs
     * DMA HAL init (FUN_c095dec0) when major rev != 0 or netlist != 0; without
     * it NvRmDmaAllocate calls a NULL function pointer.
     */
    zune_stub_preset(apb, 0x70000860, 0x00000001);
    /* APB_MISC + 0x28 bit 7: USB PHY clock stable (NvRmAnalogInterfaceControl
     * USB mode 2 polls it after enabling the PHY, else NvDdkUsbfOpen times out) */
    zune_stub_forceset(apb, 0x70000028, 0x80);
    zune_stub_preset(apb, 0x70000028, 0x80);
    /* NvRm module 0x32 @0x7000E800 reg 0x30: FUN_8002b0bc writes 0xFF12 and
     * spins until the low byte (command/busy) clears. */
    zune_stub_selfclear(apb, 0x7000e830, 0xff);
    /*
     * SPI/SLINK controllers @0x7000D400/D600/D800 (the OLED panel is set up
     * over D800, IRQ 83). STATUS (+0x08): libnvrm_impl 0xc094b224 sets the
     * FIFO-flush bits [27:26] and spins until hardware clears them; transfers
     * wait for RDY [30]. Transfers complete instantly: flush bits self-clear,
     * RDY always reads 1, BSY [31] always 0.
     */
    for (hwaddr spi = 0x7000d400; spi <= 0x7000d800; spi += 0x200) {
        zune_stub_selfclear(apb, spi + 0x08, 0x8c000000);
        zune_stub_forceset(apb, spi + 0x08, 0x40000000);
        zune_stub_preset(apb, spi + 0x08, 0x40000000);
    }
    zune_stub_create("pa80000000", 0x80000000, 4 * MiB);
    zune_stub_create("paB0000000", 0xB0000000, 2 * MiB);
    zune_stub_create("paB8000000", 0xB8000000, 2 * MiB);
    zune_stub_create("paC3000000", 0xC3000000, 1 * MiB);
    zune_stub_create("paC5000000", 0xC5000000, 1 * MiB);
    zune_stub_create("paC8000000", 0xC8000000, 1 * MiB);
    zune_stub_create("irom",     0xFFF00000, 1 * MiB);

    {
        ZuneIctlr *ic = g_new0(ZuneIctlr, 1);
        qemu_irq *lines;
        static const int tmr_irq[4] = { 0, 1, 41, 42 };

        ic->cpu_irq = qdev_get_gpio_in(DEVICE(zms->cpu), ARM_CPU_IRQ);
        ic->cpu_fiq = qdev_get_gpio_in(DEVICE(zms->cpu), ARM_CPU_FIQ);
        memory_region_init_io(&ic->mr, NULL, &zune_ictlr_ops, ic,
                              "zunehd.ictlr", 0x400);
        memory_region_add_subregion(sysmem, ZUNEHD_ICTLR_BASE, &ic->mr);
        lines = qemu_allocate_irqs(ictlr_set_irq, ic, ICTLR_BANKS * 32);
        zms->ictlr_lines = lines;

        ic->arb = g_new0(ZuneArb, 1);
        ic->arb->irq = lines[ZUNEHD_ARBGNT_IRQ];
        ic->arb->cop_irq = lines[28];       /* INT_GNT_0 (COP) */
        memory_region_init_io(&ic->arb->mr, NULL, &zune_arb_ops, ic->arb,
                              "zunehd.arb-sema", 0x100);
        memory_region_add_subregion(sysmem, ZUNEHD_ARB_BASE, &ic->arb->mr);

        /* AVP (COP) coprocessor: second CPU, EVP vectors, mailbox. Held in
         * reset until NvRm releases CAR RST_DEVICES_L bit 1. */
        zune_avp_init(OBJECT(machine), sysmem, lines);
        zune_bse_init(sysmem);          /* VDE bitstream engine (AVP MP3 decoder) */

        for (int i = 0; i < 4; i++) {
            tmr->ch[i].irq = lines[tmr_irq[i]];
            tmr->ch[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            zune_tmr_expire, &tmr->ch[i]);
        }

        /*
         * SDMMC1..4 @0xC8000000 + n*0x200: standard SDHCI (NvDdkSdioOpen uses
         * CLOCK_CONTROL 0x2C, CAPABILITIES 0x40, HOST_CONTROL 0x28); Tegra
         * vendor registers at +0x100 stay on the logging stub. IRQs follow
         * the Tegra numbering (SDMMC1 14, SDMMC2 15, SDMMC3 19, SDMMC4 31).
         * SDMMC1 is the AR6002 Wi-Fi slot; no cards are attached here.
         */
        static const int sdmmc_irq[4] = { 14, 15, 19, 31 };
        for (int i = 0; i < 4; i++) {
            DeviceState *sd = qdev_new(TYPE_SYSBUS_SDHCI);

            qdev_prop_set_uint8(sd, "sd-spec-version", 2);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(sd), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(sd), 0, 0xc8000000 + i * 0x200);
            sysbus_connect_irq(SYS_BUS_DEVICE(sd), 0, lines[sdmmc_irq[i]]);
        }

        /*
         * The eMMC (Phison PS8006 managed NAND) sits on NVIDIA's HSMMC host:
         *   -drive if=sd,index=0,format=raw,file=zunehd-emmc.img
         */
        {
            DeviceState *hs = qdev_new(TYPE_ZUNE_HSMMC);
            DriveInfo *di = drive_get(IF_SD, 0, 0);

            sysbus_realize_and_unref(SYS_BUS_DEVICE(hs), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(hs), 0, ZUNEHD_HSMMC_BASE);
            sysbus_connect_irq(SYS_BUS_DEVICE(hs), 0, lines[ZUNEHD_HSMMC_IRQ]);
            if (di) {
                DeviceState *card = qdev_new(TYPE_EMMC);
                qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                        &error_fatal);
                qdev_realize_and_unref(card, qdev_get_child_bus(hs, "sd-bus"),
                                       &error_fatal);
                info_report("zunehd: eMMC attached to HSMMC");
            }
        }

        static const struct {
            const char *name; hwaddr base; int irq; bool dvc;
        } i2cs[] = {
            { "dvc",  ZUNEHD_DVC_BASE, ZUNEHD_DVC_IRQ, true },
            { "i2c1", 0x7000c000,      38,             false },
            { "i2c2", 0x7000c400,      84,             false },
        };
        for (int i = 0; i < ARRAY_SIZE(i2cs); i++) {
            ZuneDvc *d = zune_i2c_create(i2cs[i].name, i2cs[i].base,
                                         lines[i2cs[i].irq], i2cs[i].dvc);
            if (i2cs[i].dvc) {
                d->pmu_irq = lines[86];     /* WM8350 IRQ -> external PMU */
                zune_pmic = d;
            }
            memory_region_init_io(&d->mr, NULL, &zune_dvc_ops, d,
                                  i2cs[i].name, i2cs[i].dvc ? 0x200 : 0x100);
            memory_region_add_subregion(sysmem, i2cs[i].base, &d->mr);
        }
    }
    zunehd_gfx_init(sysmem, zms->ictlr_lines[HOST1X_SYNCPT_IRQ]);
    zunehd_input_init(zms->ictlr_lines);
    zune_usb_init(sysmem, 0xc5000000, zms->ictlr_lines[20]);   /* USB1, IRQ 20 */
    /* APB DMA + I2S1 audio: IRQ 26 (CPU), 60 (INT_APB_DMA_COP, AVP) */
    zune_apbdma_init(sysmem, zms->ictlr_lines[26], zms->ictlr_lines[60]);
    zune_ape_init(sysmem);                  /* AVP audio vector engine 0x6000E000 */
    zune_usbip_init();                                          /* ZUNEHD_USBIP=<port> */

    memory_region_init_io(&tmr->mr, NULL, &zune_tmr_ops, tmr, "zunehd.tmr",
                          0x100);
    memory_region_add_subregion(sysmem, ZUNEHD_TMR_BASE, &tmr->mr);

    {
        ZuneFlow *flow = g_new0(ZuneFlow, 1);

        memory_region_init_io(&flow->mr, NULL, &zune_flow_ops, flow,
                              "zunehd.flow", 0x100);
        memory_region_add_subregion(sysmem, ZUNEHD_FLOW_BASE, &flow->mr);
    }

    /* Tegra UARTs are 16550-compatible with 32-bit register spacing.
     * Base addresses follow the Tegra family layout; which one the OAL uses
     * for debug output is still to be confirmed. */
    serial_mm_init(sysmem, 0x70006000, 2, NULL, 115200, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sysmem, 0x70006040, 2, NULL, 115200, serial_hd(1),
                   DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sysmem, 0x70006200, 2, NULL, 115200, serial_hd(2),
                   DEVICE_LITTLE_ENDIAN);

    if (!machine->kernel_filename) {
        error_report("zunehd: pass the OS image with -kernel NK.bin");
        exit(EXIT_FAILURE);
    }
    if (!zunehd_load_b000ff(machine->kernel_filename, &zms->entry, &err)) {
        error_report_err(err);
        exit(EXIT_FAILURE);
    }
    if (zms->ext_filename &&
        !zunehd_load_b000ff(zms->ext_filename, NULL, &err)) {
        error_report_err(err);
        exit(EXIT_FAILURE);
    }
    zunehd_add_bootargs(zms->emmc_instance);
    qemu_register_reset(zunehd_cpu_reset, zms);
}

static char *zunehd_get_ext(Object *obj, Error **errp)
{
    return g_strdup(ZUNEHD_MACHINE(obj)->ext_filename);
}

static void zunehd_set_ext(Object *obj, const char *value, Error **errp)
{
    ZuneHDMachineState *zms = ZUNEHD_MACHINE(obj);

    g_free(zms->ext_filename);
    zms->ext_filename = g_strdup(value);
}

static void zunehd_get_emmc(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    uint32_t val = ZUNEHD_MACHINE(obj)->emmc_instance;

    visit_type_uint32(v, name, &val, errp);
}

static void zunehd_set_emmc(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    if (val > 3) {
        error_setg(errp, "emmc-instance must be 0..3");
        return;
    }
    ZUNEHD_MACHINE(obj)->emmc_instance = val;
}

static bool zunehd_get_avp(Object *obj, Error **errp)
{
    return !zunehd_patch_avp;
}

static void zunehd_set_avp(Object *obj, bool value, Error **errp)
{
    zunehd_patch_avp = !value;
}

static void zunehd_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Microsoft Zune HD (NVIDIA Tegra APX 2600, WinCE 6 bring-up)";
    mc->init = zunehd_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1136");
    /* CPU (arm1136) + AVP/COP (zunehd-avp.c): MTTCG sizes its contexts from
     * the CPU count, so declare both. */
    mc->default_cpus = 2;
    mc->min_cpus = 2;
    mc->max_cpus = 2;
    mc->default_ram_size = ZUNEHD_SDRAM_SIZE;
    mc->default_ram_id = "zunehd.sdram";

    object_class_property_add_str(oc, "ext", zunehd_get_ext, zunehd_set_ext);
    object_class_property_add(oc, "emmc-instance", "uint32",
                              zunehd_get_emmc, zunehd_set_emmc, NULL, NULL);
    object_class_property_set_description(oc, "emmc-instance",
        "SDMMC instance (0-3) reported to the OS as the eMMC boot device");
    object_class_property_set_description(oc, "ext",
        "EXT.bin (B000FF) image loaded alongside the -kernel NK.bin");
    object_class_property_add_bool(oc, "avp", zunehd_get_avp, zunehd_set_avp);
    object_class_property_set_description(oc, "avp",
        "off (default): patch NvRmLoadLibraryEx to fail instead of waiting "
        "forever for the unemulated AVP coprocessor");
}

static void zunehd_machine_instance_init(Object *obj)
{
    /*
     * HSMMC instance reported as the boot eMMC. Only instance 0 exists on
     * AP16 (NvDdkHsmmcBlockDevInit creates per-instance state from the NvRm
     * instance count); it is NVIDIA's HSMMC block @0x70008500, not SDHCI.
     */
    ZUNEHD_MACHINE(obj)->emmc_instance = 0;
}

static const TypeInfo zunehd_machine_typeinfo = {
    .name = TYPE_ZUNEHD_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_init = zunehd_machine_instance_init,
    .class_init = zunehd_machine_class_init,
    .instance_size = sizeof(ZuneHDMachineState),
    .interfaces = arm_machine_interfaces,
};

static void zunehd_machine_register_types(void)
{
    type_register_static(&zune_hsmmc_typeinfo);
    type_register_static(&zunehd_machine_typeinfo);
}
type_init(zunehd_machine_register_types);
