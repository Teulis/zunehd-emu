/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD USB device controller (zunehd-usb.c).
 */
#ifndef HW_ARM_ZUNEHD_USB_H
#define HW_ARM_ZUNEHD_USB_H

#include "system/memory.h"
#include "hw/core/irq.h"

void zune_usb_init(MemoryRegion *sysmem, hwaddr base, qemu_irq irq);
void zune_usb_set_vbus(bool on);
/*
 * Device-side transfer API for a host model (self-test, USB/IP bridge).
 * out/in return bytes moved, or -1 = NAK (endpoint not primed).
 */
bool zune_usb_dev_ready(void);
void zune_usb_dev_setup(const uint8_t setup[8]);
int zune_usb_dev_out(int ep, const uint8_t *buf, int len);
int zune_usb_dev_out_xfer(int ep, const uint8_t *buf, int len, bool short_end);
int zune_usb_dev_in(int ep, uint8_t *buf, int maxlen);
bool zune_usb_ep_stalled(int ep, int in);
/* zunehd-usbip.c: USB/IP server, enabled by ZUNEHD_USBIP=<port> */
void zune_usbip_init(void);
/*
 * APB_MISC + 0x70 (USB1 VBUS wake-up): [16] enable, [17] W1 clears [18],
 * [18] latched wake status. Called from the APB stub on every write with
 * the old and the written value; returns the value to store.
 */
uint32_t zune_usb_apb_wake_write(uint32_t old, uint32_t val);
/* APB_MISC + 0xA30 written (bit 0 = USB1 PHY suspended, controller clock off) */
void zune_usb_apb_phy_write(uint32_t val);
/* in zunehd.c: raw access to stored APB_MISC registers */
uint32_t zune_apb_get(uint32_t off);
uint32_t zune_apb_get70(void);
void zune_apb_set70(uint32_t v);

#endif
