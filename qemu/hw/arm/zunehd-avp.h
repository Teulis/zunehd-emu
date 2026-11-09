/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD AVP (COP) coprocessor model (zunehd-avp.c).
 */
#ifndef HW_ARM_ZUNEHD_AVP_H
#define HW_ARM_ZUNEHD_AVP_H

#include "system/memory.h"
#include "hw/core/irq.h"

/* Create the AVP CPU, EVP, mailbox; ictlr_lines = the 96 ICTLR inputs. */
void zune_avp_init(Object *parent, MemoryRegion *sysmem, qemu_irq *ictlr_lines);
/* CAR register write (offset within CAR: 0x004, 0x300, 0x304). */
void zune_avp_car_write(uint32_t off, uint32_t val);
/* Flow controller HALT_COP_EVENTS write (from either CPU). */
void zune_avp_flow_halt(uint32_t val);
/* ICTLR COP outputs. */
void zune_avp_set_irq(bool irq, bool fiq);
/* True while the AVP is the CPU executing (for per-master registers). */
bool zune_avp_is_current(void);
/* "AVP " when the AVP is executing, else "" (log prefixes). */
const char *zune_cpu_tag(void);
/* zunehd-bse.c: VDE bitstream engine (MP3 Huffman) used by the AVP decoder */
void zune_bse_init(MemoryRegion *sysmem);
/* zunehd-apbdma.c: APB DMA controller + I2S1 audio sink -> QEMU audio */
void zune_apbdma_init(MemoryRegion *sysmem, qemu_irq irq, qemu_irq cop_irq);
/* zunehd-ape.c: AVP audio vector engine (MP3 hybrid synthesis) at 0x6000E000 */
void zune_ape_init(MemoryRegion *sysmem);

#endif
