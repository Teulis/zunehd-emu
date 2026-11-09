/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD (Tegra AP16): the AVP ("COP"), an ARM7TDMI-class coprocessor that
 * runs NVIDIA's nvrm_avp.axf kernel and the multimedia blocks (audio mixer
 * core, decoders). Modelled as a second ARM CPU in its own cluster and address
 * space. It must be ARMv4T (ti925t), not ARMv5 (arm926): the AVP kernel resumes
 * interrupted Thumb threads by restoring CPSR (T=1) and then "ldm ...{pc}" with
 * bit 0 clear; ARMv5 interworks on that load and faults (prefetch abort,
 * alignment, at the halfword address), ARMv4T stays in Thumb.
 *
 * How NvRm starts it (MMIO log, session 7d): CAR CLK_OUT_ENB_L = ~0, EVP
 * COP_RESET_VECTOR (0x6000F200) = entry, CAR RST_DEVICES_L bit 1 (COP)
 * pulsed, then a message in the mailbox (RES_SEMA +0x20).
 *
 * AVP view of memory = system memory plus:
 *  - 0x00000000..0x3F: exception vectors. The AVP kernel writes its handler
 *    addresses into the EVP registers (0x6000F200 reset, +4 undef, +8 swi,
 *    +0xC pabt, +0x10 dabt, +0x18 irq, +0x1C fiq); hardware redirects vector
 *    fetches there. Modelled as 8 x "ldr pc, [pc, #0x18]" + a copy of the 8
 *    EVP COP words, refreshed on every EVP write.
 *  - 0xF0000000 (64 KiB): AVP-private registers (cache / MMU TLB block at
 *    0xF000F000 on Tegra2). Logging stub for now.
 *
 * Mailbox (RES_SEMA @0x60001000, bits as in Linux arch/arm/mach-tegra avp.c):
 *   +0x10 "inbox"  = MBOX_FROM_AVP (AVP -> CPU), +0x20 "outbox" = MBOX_TO_AVP;
 *   [31] pending-message int enable, [30] message-read int enable,
 *   [29] valid, [27:0] data (message area phys >> 4).
 *   ICTLR lines: 4 inbox full, 5 inbox empty, 6 outbox full, 7 outbox empty
 *   (each gated by its enable bit); CPU and COP pick them with their IERs.
 *   +0x00 SHRD_SMP_STA, +0x04 SET, +0x08 CLR: shared semaphore bits.
 *
 * Flow controller HALT_COP_EVENTS (0x60007004): written by the AVP with a
 * non-zero mode [31:29] = sleep until an interrupt (like WFI); written by
 * the CPU with mode != 0 = hold the AVP stopped, 0 = let it run.
 *
 * ZUNEHD_AVPLOG=1 logs starts/stops, mailbox and EVP traffic to stderr.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/cpu/cluster.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/arm/cpu.h"
#include "hw/arm/zunehd-usb.h"
#include "hw/arm/zunehd-avp.h"

#define EVP_BASE        0x6000f000
#define SEMA_BASE       0x60001000
#define AVP_PRIV_BASE   0xf0000000

typedef struct ZuneAvp {
    CPUClusterState cluster;
    ARMCPU *cpu;
    MemoryRegion as;            /* AVP address space container */
    MemoryRegion sys_alias;
    MemoryRegion vectors;       /* RAM, 4 KiB, AVP-only overlay at 0 */
    MemoryRegion irom;          /* RAM, 8 KiB, AVP-only overlay at 0xFFFF0000 */
    MemoryRegion dram_alias;    /* SDRAM seen at 0x90000000 */
    MemoryRegion evp_mr;
    MemoryRegion sema_mr;
    MemoryRegion priv_mr;
    uint32_t evp[0x1000 / 4];
    uint32_t priv[0x10000 / 4];
    uint32_t inbox, outbox, smp_sta;
    qemu_irq mbox_irq[4];       /* ICTLR lines 4..7 */
    bool in_reset, held, running;
    bool log;
} ZuneAvp;

static ZuneAvp *avp;

bool zune_avp_is_current(void)
{
    return avp && current_cpu == CPU(avp->cpu);
}

const char *zune_cpu_tag(void)
{
    return zune_avp_is_current() ? "AVP " : "";
}

/* ---------------------------------------------------------------- vectors */

static void avp_vectors_refresh(ZuneAvp *a)
{
    uint32_t *v = memory_region_get_ram_ptr(&a->vectors);

    for (int i = 0; i < 8; i++) {
        v[i] = cpu_to_le32(0xe59ff018);             /* ldr pc, [pc, #0x18] */
        v[8 + i] = cpu_to_le32(a->evp[(0x200 / 4) + i]);
    }
    memory_region_set_dirty(&a->vectors, 0, 0x40);
}

/*
 * Boot ROM page as the AVP sees it. The AVP's NvRmOpen (FUN_060aeb70) always
 * reads the relocation table pointer at IROM+0x40 and parses 4 KiB there
 * (module id/instance/version, base, size triples). We have no boot ROM, and
 * the CPU side runs the pre-silicon path that uses the OAL's built-in chip
 * 0x16 table (FUN_80030b48 -> 0x80BD7460, PA 0x00BD7460). Give the AVP the
 * same table so both NvRm instances agree on the module map.
 */
#define OAL_RELOC_PA    0x00bd7460

static void avp_irom_refresh(ZuneAvp *a)
{
    uint8_t *rom = memory_region_get_ram_ptr(&a->irom);
    uint32_t hdr[2];

    address_space_read(&address_space_memory, OAL_RELOC_PA, MEMTXATTRS_UNSPECIFIED,
                       hdr, sizeof(hdr));
    if (le32_to_cpu(hdr[0]) != 1 || le32_to_cpu(hdr[1]) != 0x00020010) {
        warn_report("zunehd: AVP: no relocation table at 0x%08x (%08x %08x)",
                    OAL_RELOC_PA, le32_to_cpu(hdr[0]), le32_to_cpu(hdr[1]));
        return;
    }
    address_space_read(&address_space_memory, OAL_RELOC_PA, MEMTXATTRS_UNSPECIFIED,
                       rom + 0x1000, 0x1000);
    stl_le_p(rom + 0x40, 0xffff1000);
    memory_region_set_dirty(&a->irom, 0, 0x2000);
}

/* ---------------------------------------------------------------- run state */

static void avp_start_work(CPUState *cs, run_on_cpu_data d)
{
    ARMCPU *c = ARM_CPU(cs);

    cpu_reset(cs);
    cpu_set_pc(cs, (uint32_t)d.host_int);
    cs->halted = 0;
    c->power_state = PSCI_ON;
}

static void avp_stop_work(CPUState *cs, run_on_cpu_data d)
{
    ARMCPU *c = ARM_CPU(cs);

    c->power_state = PSCI_OFF;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
}

static void avp_update_run(ZuneAvp *a)
{
    bool want = !a->in_reset && !a->held;

    if (want == a->running) {
        return;
    }
    a->running = want;
    if (want) {
        uint32_t entry = a->evp[0x200 / 4];
        avp_irom_refresh(a);
        info_report("zunehd: AVP started at 0x%08x", entry);
        avp_vectors_refresh(a);
        async_run_on_cpu(CPU(a->cpu), avp_start_work, RUN_ON_CPU_HOST_INT(entry));
    } else {
        if (a->log) {
            info_report("zunehd: AVP stopped (%s)", a->in_reset ? "reset" : "halted");
        }
        async_run_on_cpu(CPU(a->cpu), avp_stop_work, RUN_ON_CPU_NULL);
    }
}

/* CAR write: RST_DEVICES_L (0x004), RST_DEV_L_SET (0x300), _CLR (0x304). */
void zune_avp_car_write(uint32_t off, uint32_t val)
{
    bool r;

    if (!avp) {
        return;
    }
    switch (off) {
    case 0x004: r = val & 2; break;
    case 0x300: if (!(val & 2)) { return; } r = true; break;
    case 0x304: if (!(val & 2)) { return; } r = false; break;
    default: return;
    }
    if (r != avp->in_reset) {
        avp->in_reset = r;
        if (avp->log) {
            info_report("zunehd: AVP reset %s", r ? "asserted" : "released");
        }
        avp_update_run(avp);
    }
}

/* Flow controller HALT_COP_EVENTS write. */
void zune_avp_flow_halt(uint32_t val)
{
    if (!avp) {
        return;
    }
    if (zune_avp_is_current()) {
        if (val >> 29) {
            cpu_interrupt(CPU(avp->cpu), CPU_INTERRUPT_HALT);   /* sleep */
        }
        return;
    }
    if (avp->log) {
        info_report("zunehd: CPU writes HALT_COP 0x%08x", val);
    }
    avp->held = (val >> 29) != 0;
    avp_update_run(avp);
}

void zune_avp_set_irq(bool irq, bool fiq)
{
    if (avp) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(avp->cpu), ARM_CPU_IRQ), irq);
        qemu_set_irq(qdev_get_gpio_in(DEVICE(avp->cpu), ARM_CPU_FIQ), fiq);
    }
}

/* ---------------------------------------------------------------- EVP */

static uint64_t evp_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneAvp *a = opaque;
    return a->evp[off / 4];
}

static void evp_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneAvp *a = opaque;

    a->evp[off / 4] = val;
    if (a->log) {
        info_report("zunehd: %sEVP [0x%03x] <- 0x%08x", zune_cpu_tag(),
                    (unsigned)off, (uint32_t)val);
    }
    if (off >= 0x200 && off < 0x220) {
        avp_vectors_refresh(a);
    }
}

static const MemoryRegionOps evp_ops = {
    .read = evp_read,
    .write = evp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------- mailbox */

#define MBOX_PEND_INT_EN    (1u << 31)
#define MBOX_READ_INT_EN    (1u << 30)
#define MBOX_VALID          (1u << 29)

static void sema_update(ZuneAvp *a)
{
    uint32_t in = a->inbox, out = a->outbox;

    qemu_set_irq(a->mbox_irq[0], (in & MBOX_VALID) && (in & MBOX_PEND_INT_EN));
    qemu_set_irq(a->mbox_irq[1], !(in & MBOX_VALID) && (in & MBOX_READ_INT_EN));
    qemu_set_irq(a->mbox_irq[2], (out & MBOX_VALID) && (out & MBOX_PEND_INT_EN));
    qemu_set_irq(a->mbox_irq[3], !(out & MBOX_VALID) && (out & MBOX_READ_INT_EN));
}

static uint64_t sema_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneAvp *a = opaque;

    if (a->log && (off == 0x10 || off == 0x20)) {
        info_report("zunehd: %" PRId64 " %smbox read %s -> 0x%08x pc~0x%08x", qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), zune_cpu_tag(),
                    off == 0x10 ? "FROM_AVP" : "TO_AVP", off == 0x10 ? a->inbox : a->outbox,
                    current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    switch (off) {
    case 0x00: return a->smp_sta;
    case 0x10: return a->inbox;
    case 0x20: return a->outbox;
    }
    qemu_log_mask(LOG_UNIMP, "zunehd: %ssema read [0x%08" HWADDR_PRIx "]\n",
                  zune_cpu_tag(), SEMA_BASE + off);
    return 0;
}

static void sema_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneAvp *a = opaque;

    switch (off) {
    case 0x04: a->smp_sta |= val; break;
    case 0x08: a->smp_sta &= ~val; break;
    case 0x10: a->inbox = val; break;
    case 0x20: a->outbox = val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "zunehd: %ssema write [0x%08" HWADDR_PRIx
                      "] <- 0x%08" PRIx64 "\n", zune_cpu_tag(), SEMA_BASE + off, val);
        return;
    }
    if (a->log && (off == 0x10 || off == 0x20)) {
        info_report("zunehd: %" PRId64 " %smbox %s <- 0x%08x pc~0x%08x", qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), zune_cpu_tag(),
                    off == 0x10 ? "FROM_AVP(inbox)" : "TO_AVP(outbox)", (uint32_t)val,
                    current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    sema_update(a);
}

static const MemoryRegionOps sema_ops = {
    .read = sema_read,
    .write = sema_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------- private */

static uint64_t priv_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneAvp *a = opaque;
    uint32_t v = a->priv[off / 4];

    qemu_log_mask(LOG_UNIMP, "zunehd: AVP priv read  [0x%08" HWADDR_PRIx
                  "] -> 0x%08x  pc~0x%08x\n", AVP_PRIV_BASE + off, v,
                  a->cpu->env.regs[15]);
    return v;
}

static void priv_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ZuneAvp *a = opaque;

    a->priv[off / 4] = val;
    qemu_log_mask(LOG_UNIMP, "zunehd: AVP priv write [0x%08" HWADDR_PRIx
                  "] <- 0x%08" PRIx64 "  pc~0x%08x\n", AVP_PRIV_BASE + off, val,
                  a->cpu->env.regs[15]);
}

static const MemoryRegionOps priv_ops = {
    .read = priv_read,
    .write = priv_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---------------------------------------------------------------- init */

void zune_avp_init(Object *parent, MemoryRegion *sysmem, qemu_irq *ictlr_lines)
{
    ZuneAvp *a = g_new0(ZuneAvp, 1);
    Object *cpuobj;

    a->log = getenv("ZUNEHD_AVPLOG") != NULL;
    /*
     * On Tegra2-class parts the AVP is the boot processor; when CE starts it
     * is out of reset but parked by the flow controller (HALT_COP). NvRm then
     * sets the vector, pulses CAR COP reset and writes HALT_COP = 0. (Starting
     * "in reset" let NvRm's early full CAR writes, bit 1 = 0, release it at
     * vector 0.)
     */
    a->in_reset = false;
    a->held = true;

    /* AVP address space: system memory + AVP-only overlays. */
    memory_region_init(&a->as, NULL, "zunehd.avp-as", UINT64_C(0x100000000));
    memory_region_init_alias(&a->sys_alias, NULL, "zunehd.avp-sys", sysmem, 0,
                             UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(&a->as, 0, &a->sys_alias, 0);
    memory_region_init_ram(&a->vectors, NULL, "zunehd.avp-vectors", 0x1000,
                           &error_fatal);
    memory_region_add_subregion_overlap(&a->as, 0, &a->vectors, 10);
    /*
     * DRAM alias at 0x90000000: the AVP kernel's mailbox handler
     * (FUN_060acebc) accesses the message area at (mbox data << 4) -
     * 0x70000000, i.e. phys + 0x90000000. 256 MiB covers SDRAM + carveout.
     */
    memory_region_init_alias(&a->dram_alias, NULL, "zunehd.avp-dram-9", sysmem, 0,
                             0x10000000);
    memory_region_add_subregion_overlap(&a->as, 0x90000000, &a->dram_alias, 10);
    memory_region_init_ram(&a->irom, NULL, "zunehd.avp-irom", 0x2000, &error_fatal);
    memory_region_add_subregion_overlap(&a->as, 0xffff0000, &a->irom, 10);
    memory_region_init_io(&a->priv_mr, NULL, &priv_ops, a, "zunehd.avp-priv",
                          0x10000);
    memory_region_add_subregion_overlap(&a->as, AVP_PRIV_BASE, &a->priv_mr, 10);

    /* Shared blocks, visible to both CPUs. */
    memory_region_init_io(&a->evp_mr, NULL, &evp_ops, a, "zunehd.evp", 0x1000);
    memory_region_add_subregion(sysmem, EVP_BASE, &a->evp_mr);
    memory_region_init_io(&a->sema_mr, NULL, &sema_ops, a, "zunehd.res-sema", 0x100);
    memory_region_add_subregion(sysmem, SEMA_BASE, &a->sema_mr);
    for (int i = 0; i < 4; i++) {
        a->mbox_irq[i] = ictlr_lines[4 + i];
    }

    /* The CPU, in cluster 1 (own TB cache: different core, different view). */
    object_initialize_child(parent, "avp-cluster", &a->cluster, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&a->cluster), "cluster-id", 1);
    cpuobj = object_new(ARM_CPU_TYPE_NAME("ti925t"));
    object_property_add_child(OBJECT(&a->cluster), "avp-cpu", cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(&a->as), &error_fatal);
    object_property_set_bool(cpuobj, "start-powered-off", true, &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    object_unref(cpuobj);
    qdev_realize(DEVICE(&a->cluster), NULL, &error_fatal);
    a->cpu = ARM_CPU(cpuobj);
    a->cpu->power_state = PSCI_OFF;

    avp = a;
    avp_vectors_refresh(a);
    info_report("zunehd: AVP (ti925t = ARMv4T, cluster 1) created, parked (HALT_COP)");
}
