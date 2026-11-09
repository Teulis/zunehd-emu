/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD (Tegra AP16) USB device controller @0xC5000000, IRQ 20.
 *
 * ChipIdea/ARC-style dual-role controller used in device mode by NvDdkUsbf
 * (libnvddk_misc.dll) under libnvusbf.dll (CE USB function driver). Register
 * offsets as used by the driver (FUN_c08ee0fc etc.):
 *   0x100 CAPLENGTH/HCIVERSION  0x124 DCCPARAMS
 *   0x140 USBCMD ([0] RS, [1] RST self-clearing, [13] SUTW, [14] ATDTW)
 *   0x144 USBSTS (W1C: [0] UI, [1] UEI, [2] PCI, [6] URI, [7] SRI, [8] SLI)
 *   0x148 USBINTR  0x14C FRINDEX  0x154 DEVICEADDR  0x158 ENDPOINTLISTADDR
 *   0x184 PORTSC1 ([0] CCS, [2] PE, [7] SUSP, [8] PR, [27:26] PSPD 2 = HS)
 *   0x1A4 OTGSC (status [15:8] RO: [8] ID, [11] BSV, [12] BSE; W1C int status
 *         [23:16]; enables [31:24])
 *   0x1A8 USBMODE ([1:0] CM, 2 = device)  0x1AC ENDPTSETUPSTAT (W1C)
 *   0x1B0 ENDPTPRIME  0x1B4 ENDPTFLUSH  0x1B8 ENDPTSTATUS  0x1BC ENDPTCOMPLETE
 *   0x1C0+4n ENDPTCTRLn   0x400 PHY/legacy control ([7] PHY clock valid)
 *
 * Stage A: registers, reset, cable (VBUS) state and IRQ. Endpoint DMA (dQH /
 * dTD processing) and the USB/IP bridge are added on top of this.
 *
 * Set ZUNEHD_USBLOG=1 to log register accesses to stderr.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "hw/arm/zunehd-usb.h"

#define USB_REGS        0x1000

#define R_USBCMD        0x140
#define R_USBSTS        0x144
#define R_USBINTR       0x148
#define R_FRINDEX       0x14c
#define R_DEVICEADDR    0x154
#define R_EPLISTADDR    0x158
#define R_PORTSC1       0x184
#define R_OTGSC         0x1a4
#define R_USBMODE       0x1a8
#define R_EPSETUPSTAT   0x1ac
#define R_EPPRIME       0x1b0
#define R_EPFLUSH       0x1b4
#define R_EPSTATUS      0x1b8
#define R_EPCOMPLETE    0x1bc
#define R_EPCTRL0       0x1c0
#define R_PHYCTRL       0x400

typedef struct ZuneUsb {
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t regs[USB_REGS / 4];
    MemoryRegion local_ram;     /* controller-local memory (dQH list) */
    bool vbus;          /* cable attached (B-session valid) */
    bool attach_pending;    /* plug/unplug not yet seen by the controller */
    bool bus_reset_done;
    QEMUTimer *bus_timer;
    hwaddr cur_dtd[32];         /* per dQH index: dTD being transferred */
    uint32_t cur_pos[32];       /* bytes already moved in that dTD */
    bool log;
    bool dtdlog;                /* ZUNEHD_USBDTDLOG: dTD pick/retire on ep >= 1 */
    /* self-test host */
    bool selftest;
    QEMUTimer *host_timer;
    unsigned hstep;
    int hstate, hlen, htries;
    uint8_t hbuf[1024];
} ZuneUsb;

static ZuneUsb *zune_usb;

#define DTDLOG(...) do { fprintf(stderr, "usbdtd: %lld ", (long long)qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)); fprintf(stderr, __VA_ARGS__); } while (0)

#define REG(s, off) ((s)->regs[(off) / 4])

/* APB_MISC + 0x70 bits (USB1 PHY VBUS wake-up logic, NvRm FUN_c0955db8) */
#define WAKE_EN         (1u << 16)
#define WAKE_CLR        (1u << 17)
#define WAKE_STS        (1u << 18)
#define PHY_SUSP_DONE   (1u << 2)   /* polled by NvRm FUN_c0955a14 power-down */

/*
 * APB_MISC + 0xA30 bit 0: USB1 PHY suspended (NvRm FUN_c0955a14 sets it on
 * power-down, clears it on power-up). While suspended the controller has no
 * clock: its own interrupts stay silent, and only the PHY VBUS wake-up event
 * (0x70 bits 16/18) reaches IRQ 20. NvDdkUsbf's ISR then sees its "suspended"
 * flag, reads/clears the wake status via NvRmAnalogInterfaceControl mode 0 and
 * signals its IST, which calls NvDdkUsbfResume (PHY power-up, controller init).
 */
static bool usb_phy_suspended(void)
{
    return zune_apb_get(0xa30) & 1;
}

static bool usb_ctrl_active(ZuneUsb *s)
{
    return (REG(s, R_USBCMD) & 1) && !usb_phy_suspended();
}

/*
 * Attach as a host would do it, once the controller runs (RS=1 = D+ pull-up):
 * bus idle -> suspend (SLI) + port change, ~20 ms later bus reset (URI) +
 * port change at high speed. NvDdkUsbf only reports a reset (event 0x40)
 * when a suspend was seen before it (ISR flag +0x75).
 */
static void usb_deliver_attach(ZuneUsb *s)
{
    if (!s->attach_pending || !usb_ctrl_active(s)) {
        return;
    }
    s->attach_pending = false;
    REG(s, R_OTGSC) |= 1u << 19;                    /* BSVIS */
    REG(s, R_USBSTS) |= 1u << 2;                    /* PCI */
    if (s->vbus) {
        REG(s, R_USBSTS) |= 1u << 8;                /* SLI: bus idle */
        timer_mod(s->bus_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20);
    }
    if (s->log) {
        fprintf(stderr, "usb: attach delivered to controller (vbus %d)\n", s->vbus);
    }
}

static void usb_update_irq(ZuneUsb *s);
static void usb_host_start(ZuneUsb *s);

static void usb_bus_timer_cb(void *opaque)
{
    ZuneUsb *s = opaque;

    if (!s->vbus || !usb_ctrl_active(s)) {
        return;
    }
    REG(s, R_USBSTS) |= (1u << 6) | (1u << 2);      /* URI + PCI (HS) */
    s->bus_reset_done = true;
    if (s->log) {
        fprintf(stderr, "usb: bus reset\n");
    }
    usb_update_irq(s);
    usb_host_start(s);
}

/* ---------------------------------------------------------------------- */
/* Endpoint engine: device-mode dQH / dTD processing (ChipIdea layout).    */
/*                                                                          */
/* dQH (64 B, list at ENDPOINTLISTADDR, index 2*ep + dir, dir 1 = IN):      */
/*   +0 caps ([26:16] max packet, [15] IOS, [29] ZLT off), +4 current dTD,  */
/*   +8 overlay: next dTD, +0xC token, +0x10.. buffer pages, +0x28 setup.   */
/* dTD (32 B): +0 next (bit 0 = terminate), +4 token ([30:16] total bytes,  */
/*   [15] IOC, [7] active, [6] halted), +8..+0x18 buffer pages 0..4        */
/*   (page 0 keeps the byte offset in its low 12 bits).                     */

static uint32_t dma_rd32(hwaddr a)
{
    return address_space_ldl_le(&address_space_memory, a,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void dma_wr32(hwaddr a, uint32_t v)
{
    address_space_stl_le(&address_space_memory, a, v,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

static hwaddr usb_qh(ZuneUsb *s, int ep, int in)
{
    return (REG(s, R_EPLISTADDR) & ~0x7ffu) + (2 * ep + in) * 64;
}

static uint32_t usb_ep_bit(int ep, int in)
{
    return 1u << (ep + (in ? 16 : 0));
}

/* Address of byte 'pos' (from the start of the dTD's buffer). */
static hwaddr usb_dtd_addr(hwaddr dtd, uint32_t pos)
{
    uint32_t p0 = dma_rd32(dtd + 8);
    uint32_t off = (p0 & 0xfff) + pos;
    uint32_t page = off >> 12;

    if (page == 0) {
        return (p0 & ~0xfffu) + off;
    }
    if (page > 4) {
        return 0;
    }
    return (dma_rd32(dtd + 8 + 4 * page) & ~0xfffu) + (off & 0xfff);
}

/* Retire the current dTD of an endpoint; 'left' = bytes not transferred. */
static void usb_dtd_retire(ZuneUsb *s, int ep, int in, uint32_t left)
{
    int i = 2 * ep + in;
    hwaddr dtd = s->cur_dtd[i];
    hwaddr qh = usb_qh(s, ep, in);
    uint32_t tok = dma_rd32(dtd + 4);
    uint32_t next = dma_rd32(dtd);

    if (s->dtdlog && ep) {
        DTDLOG("ep%d%s retire dtd 0x%08x pos %u left %u ioc %d next 0x%08x\n",
                ep, in ? "in" : "out", (uint32_t)dtd, s->cur_pos[i], left,
                !!(tok & (1u << 15)), next);
    }
    tok = (tok & ~(0x7fffu << 16) & ~0xffu) | ((left & 0x7fff) << 16);
    dma_wr32(dtd + 4, tok);
    dma_wr32(qh + 4, dtd);
    dma_wr32(qh + 8, next);
    dma_wr32(qh + 0xc, tok);
    if (tok & (1u << 15)) {                         /* IOC */
        REG(s, R_EPCOMPLETE) |= usb_ep_bit(ep, in);
        REG(s, R_USBSTS) |= 1u;                     /* UI */
    }
    s->cur_dtd[i] = 0;
    s->cur_pos[i] = 0;
    if (next & 1) {
        REG(s, R_EPSTATUS) &= ~usb_ep_bit(ep, in);
    }
}

/* Current active dTD of a primed endpoint, or 0 (NAK). */
static hwaddr usb_ep_dtd(ZuneUsb *s, int ep, int in)
{
    int i = 2 * ep + in;
    hwaddr qh;
    uint32_t next;

    if (!(REG(s, R_EPSTATUS) & usb_ep_bit(ep, in))) {
        return 0;
    }
    if (s->cur_dtd[i]) {
        return s->cur_dtd[i];
    }
    qh = usb_qh(s, ep, in);
    next = dma_rd32(qh + 8);
    while (!(next & 1)) {
        uint32_t tok = dma_rd32((next & ~0x1fu) + 4);
        if (tok & 0x80) {                           /* active */
            s->cur_dtd[i] = next & ~0x1fu;
            s->cur_pos[i] = 0;
            if (s->dtdlog && ep) {
                DTDLOG("ep%d%s pick dtd 0x%08x total %u tok 0x%08x buf 0x%08x\n",
                        ep, in ? "in" : "out", (uint32_t)s->cur_dtd[i],
                        (tok >> 16) & 0x7fff, tok, dma_rd32(s->cur_dtd[i] + 8));
            }
            return s->cur_dtd[i];
        }
        next = dma_rd32(next & ~0x1fu);
    }
    REG(s, R_EPSTATUS) &= ~usb_ep_bit(ep, in);
    return 0;
}

static uint32_t usb_ep_maxpkt(ZuneUsb *s, int ep, int in)
{
    uint32_t mp = (dma_rd32(usb_qh(s, ep, in)) >> 16) & 0x7ff;
    return mp ? mp : 64;
}

/*
 * Host OUT transfer (host -> device): 'len' bytes in one transfer. Returns the
 * number of bytes accepted, or -1 = NAK (no primed dTD).
 */
int zune_usb_dev_out(int ep, const uint8_t *buf, int len)
{
    ZuneUsb *s = zune_usb;

    return zune_usb_dev_out_xfer(ep, buf, len,
                                 len == 0 || !s || (len % usb_ep_maxpkt(s, ep, 0)) != 0);
}

/*
 * 'short_end': the host transfer ends with a short packet (or ZLP) at the end
 * of this chunk. A host transfer that ends on a full packet does not end the
 * device's transfer (USB short-packet rule), so a partly filled dTD stays
 * active. The caller must decide this from the WHOLE host transfer: judging
 * by the remaining chunk length (e.g. 16372 bytes left of a 1 MiB URB after
 * a 12-byte header dTD) wrongly retired a dTD 12 bytes early and broke MTP
 * SendObject for files > ~1 MB (MtpSvc then wrote at a bogus offset and exFAT
 * zero-filled clusters forever).
 */
int zune_usb_dev_out_xfer(int ep, const uint8_t *buf, int len, bool short_end)
{
    ZuneUsb *s = zune_usb;
    int i = 2 * ep, done = 0;
    bool zlp = len == 0;

    if (!s || !usb_ep_dtd(s, ep, 0)) {
        return -1;
    }
    while (done < len || zlp) {
        hwaddr dtd = usb_ep_dtd(s, ep, 0);
        uint32_t total, room, n;

        if (!dtd) {
            break;
        }
        total = (dma_rd32(dtd + 4) >> 16) & 0x7fff;
        room = total - s->cur_pos[i];
        n = MIN(room, (uint32_t)(len - done));
        for (uint32_t k = 0; k < n; k++) {
            address_space_stb(&address_space_memory,
                              usb_dtd_addr(dtd, s->cur_pos[i] + k), buf[done + k],
                              MEMTXATTRS_UNSPECIFIED, NULL);
        }
        s->cur_pos[i] += n;
        done += n;
        if (s->cur_pos[i] >= total || (done >= len && short_end)) {
            /* full, or the transfer ended with a short packet */
            usb_dtd_retire(s, ep, 0, total - s->cur_pos[i]);
        }
        zlp = false;
        if (done >= len) {
            break;
        }
    }
    usb_update_irq(s);
    return done;
}

/*
 * Host IN transfer (device -> host): up to 'maxlen' bytes. Ends at a dTD
 * whose length is not a multiple of the max packet size (short packet) or
 * at a zero-length dTD. Returns bytes, or -1 = NAK.
 */
int zune_usb_dev_in(int ep, uint8_t *buf, int maxlen)
{
    ZuneUsb *s = zune_usb;
    int i = 2 * ep + 1, done = 0;
    uint32_t mp;

    if (!s || !usb_ep_dtd(s, ep, 1)) {
        return -1;
    }
    mp = usb_ep_maxpkt(s, ep, 1);
    while (done < maxlen) {
        hwaddr dtd = usb_ep_dtd(s, ep, 1);
        uint32_t total, avail, n;
        bool short_end;

        if (!dtd) {
            break;
        }
        total = (dma_rd32(dtd + 4) >> 16) & 0x7fff;
        avail = total - s->cur_pos[i];
        n = MIN(avail, (uint32_t)(maxlen - done));
        for (uint32_t k = 0; k < n; k++) {
            buf[done + k] = address_space_ldub(&address_space_memory,
                              usb_dtd_addr(dtd, s->cur_pos[i] + k),
                              MEMTXATTRS_UNSPECIFIED, NULL);
        }
        s->cur_pos[i] += n;
        done += n;
        if (s->cur_pos[i] < total) {
            break;                                  /* host buffer full */
        }
        short_end = total == 0 || (total % mp) != 0;
        usb_dtd_retire(s, ep, 1, 0);
        if (short_end) {
            break;
        }
    }
    usb_update_irq(s);
    return done;
}

/* SETUP packet to endpoint 0. */
void zune_usb_dev_setup(const uint8_t setup[8])
{
    ZuneUsb *s = zune_usb;
    hwaddr qh;

    if (!s) {
        return;
    }
    qh = usb_qh(s, 0, 0);
    for (int k = 0; k < 8; k++) {
        address_space_stb(&address_space_memory, qh + 0x28 + k, setup[k],
                          MEMTXATTRS_UNSPECIFIED, NULL);
    }
    /* A SETUP flushes whatever was primed on ep0 and clears an ep0 STALL. */
    REG(s, R_EPSTATUS) &= ~(usb_ep_bit(0, 0) | usb_ep_bit(0, 1));
    REG(s, R_EPCTRL0) &= ~((1u << 16) | 1u);
    s->cur_dtd[0] = s->cur_dtd[1] = 0;
    REG(s, R_EPSETUPSTAT) |= 1;
    REG(s, R_USBSTS) |= 1u;                         /* UI */
    usb_update_irq(s);
}

/* ENDPTCTRLn: [0] RX (OUT) stall, [16] TX (IN) stall. */
bool zune_usb_ep_stalled(int ep, int in)
{
    ZuneUsb *s = zune_usb;
    return s && (REG(s, R_EPCTRL0 + 4 * ep) & (in ? (1u << 16) : 1u));
}

bool zune_usb_dev_ready(void)
{
    ZuneUsb *s = zune_usb;
    return s && s->vbus && s->bus_reset_done && usb_ctrl_active(s);
}

/* ---------------------------------------------------------------------- */
/* Built-in test host (ZUNEHD_USB_SELFTEST=1): enumerates the device and    */
/* logs the descriptors, to check the engine without a PC side.             */

enum { HS_IDLE, HS_SETUP, HS_DATA_IN, HS_DATA_OUT, HS_STATUS_IN, HS_STATUS_OUT,
       HS_BULK_CMD, HS_BULK_IN1, HS_BULK_IN2, HS_DONE };

/* MTP GetDeviceInfo (0x1001), transaction 0, as a PTP command container. */
static const uint8_t mtp_getdeviceinfo[12] = {
    12, 0, 0, 0, 1, 0, 0x01, 0x10, 0, 0, 0, 0
};

static void usb_host_log_data(const char *what, const uint8_t *b, int n);

/* After the control script: one MTP transaction over bulk ep1 OUT / IN. */
static bool usb_host_bulk_step(ZuneUsb *s)
{
    int n;

    switch (s->hstate) {
    case HS_BULK_CMD:
        n = zune_usb_dev_out(1, mtp_getdeviceinfo, sizeof(mtp_getdeviceinfo));
        if (n >= 0) {
            info_report("usb host: MTP GetDeviceInfo sent (%d bytes)", n);
            s->hlen = 0;
            s->hstate = HS_BULK_IN1;
        }
        return true;
    case HS_BULK_IN1:
    case HS_BULK_IN2:
        n = zune_usb_dev_in(1, s->hbuf + s->hlen, sizeof(s->hbuf) - s->hlen);
        if (n >= 0) {
            s->hlen += n;
            if (n % 512 || n == 0 || s->hlen >= (int)sizeof(s->hbuf)) {
                usb_host_log_data(s->hstate == HS_BULK_IN1 ? "MTP data" :
                                  "MTP response", s->hbuf, s->hlen);
                s->hlen = 0;
                s->hstate = s->hstate == HS_BULK_IN1 ? HS_BULK_IN2 : HS_DONE;
            }
        }
        return true;
    case HS_DONE:
        return false;
    }
    return false;
}

typedef struct HostReq {
    uint8_t setup[8];
    const char *what;
} HostReq;

static const HostReq host_script[] = {
    { { 0x80, 6, 0, 1, 0, 0, 64, 0 },   "GET_DESCRIPTOR device (64)" },
    { { 0x00, 5, 5, 0, 0, 0, 0, 0 },    "SET_ADDRESS 5" },
    { { 0x80, 6, 0, 1, 0, 0, 18, 0 },   "GET_DESCRIPTOR device (18)" },
    { { 0x80, 6, 0, 2, 0, 0, 9, 0 },    "GET_DESCRIPTOR config (9)" },
    { { 0x80, 6, 0, 2, 0, 0, 0xff, 0 }, "GET_DESCRIPTOR config (255)" },
    { { 0x80, 6, 0, 3, 0, 0, 0xff, 0 }, "GET_DESCRIPTOR string 0" },
    { { 0x80, 6, 1, 3, 9, 4, 0xff, 0 }, "GET_DESCRIPTOR string 1" },
    { { 0x80, 6, 2, 3, 9, 4, 0xff, 0 }, "GET_DESCRIPTOR string 2" },
    { { 0x80, 6, 0xee, 3, 0, 0, 0xff, 0 }, "GET_DESCRIPTOR string 0xEE (MS OS)" },
    { { 0x00, 9, 1, 0, 0, 0, 0, 0 },    "SET_CONFIGURATION 1" },
};

static void usb_host_log_data(const char *what, const uint8_t *b, int n)
{
    GString *g = g_string_new(NULL);
    for (int k = 0; k < n; k++) {
        g_string_append_printf(g, "%02x%s", b[k], (k & 15) == 15 ? "\n    " : " ");
    }
    info_report("usb host: %s -> %d bytes:\n    %s", what, n, g->str);
    g_string_free(g, true);
}

static void usb_host_timer_cb(void *opaque)
{
    ZuneUsb *s = opaque;
    const HostReq *r;
    int wlen, n, old_step, old_state, old_len;
    uint8_t zero = 0;

    if (!zune_usb_dev_ready()) {
        return;
    }
    old_step = s->hstep;
    old_state = s->hstate;
    old_len = s->hlen;
    if (s->hstep >= ARRAY_SIZE(host_script)) {
        if (s->hstate < HS_BULK_CMD) {
            s->hstate = HS_BULK_CMD;
        }
        if (!usb_host_bulk_step(s)) {
            return;
        }
        r = &host_script[0];
        goto next;
    }
    r = &host_script[s->hstep];
    wlen = r->setup[6] | (r->setup[7] << 8);
    switch (s->hstate) {
    case HS_IDLE:
    case HS_SETUP:
        zune_usb_dev_setup(r->setup);
        s->hlen = 0;
        s->htries = 0;
        s->hstate = wlen ? ((r->setup[0] & 0x80) ? HS_DATA_IN : HS_DATA_OUT)
                         : HS_STATUS_IN;
        break;
    case HS_DATA_IN:
        n = zune_usb_dev_in(0, s->hbuf + s->hlen, wlen - s->hlen);
        if (n >= 0) {
            s->hlen += n;
            if (s->hlen >= wlen || n % usb_ep_maxpkt(s, 0, 1) || n == 0) {
                usb_host_log_data(r->what, s->hbuf, s->hlen);
                s->hstate = HS_STATUS_OUT;
            }
        }
        break;
    case HS_DATA_OUT:
        s->hstate = HS_STATUS_IN;                   /* no OUT data in script */
        break;
    case HS_STATUS_IN:
        n = zune_usb_dev_in(0, &zero, 0);
        if (n >= 0) {
            info_report("usb host: %s: status ok", r->what);
            s->hstep++;
            s->hstate = HS_SETUP;
        }
        break;
    case HS_STATUS_OUT:
        n = zune_usb_dev_out(0, NULL, 0);
        if (n >= 0) {
            s->hstep++;
            s->hstate = HS_SETUP;
        }
        break;
    }
next:
    if (s->hstep != old_step || s->hstate != old_state || s->hlen != old_len) {
        s->htries = 0;
    }
    /* ~3 s without progress (control), ~15 s for the MTP responder */
    if (++s->htries > (s->hstep < ARRAY_SIZE(host_script) ? 300 : 1500)) {
        info_report("usb host: %s: timeout in state %d (epstat %08x prime %08x)",
                    s->hstep < ARRAY_SIZE(host_script) ? r->what : "MTP",
                    s->hstate, REG(s, R_EPSTATUS), REG(s, R_EPPRIME));
        s->hstep = ARRAY_SIZE(host_script);
        s->hstate = HS_DONE;
        return;
    }
    timer_mod(s->host_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
}

static void usb_host_start(ZuneUsb *s)
{
    if (s->selftest) {
        s->hstep = 0;
        s->hstate = HS_IDLE;
        s->htries = 0;
        timer_mod(s->host_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

static void usb_update_irq(ZuneUsb *s)
{
    uint32_t otg = REG(s, R_OTGSC);
    uint32_t apb = zune_apb_get70();
    bool ctrl, wake, lvl;

    usb_deliver_attach(s);
    ctrl = !usb_phy_suspended() &&
           ((REG(s, R_USBSTS) & REG(s, R_USBINTR) & 0x1ff) ||
            (((otg >> 16) & (otg >> 24)) & 0x7f));
    wake = (apb & WAKE_EN) && (apb & WAKE_STS);
    lvl = ctrl || wake;
    if (s->log) {
        static int last = -1;
        if (last != lvl) {
            fprintf(stderr, "usb: irq %d (ctrl %d wake %d sts %08x intr %08x "
                    "otgsc %08x apb70 %08x)\n", lvl, ctrl, wake,
                    REG(s, R_USBSTS), REG(s, R_USBINTR), REG(s, R_OTGSC), apb);
            last = lvl;
        }
    }
    qemu_set_irq(s->irq, lvl);
}

uint32_t zune_usb_apb_wake_write(uint32_t old, uint32_t val)
{
    ZuneUsb *s = zune_usb;
    uint32_t nv = (val & ~(WAKE_CLR | WAKE_STS | PHY_SUSP_DONE)) | (old & WAKE_STS);

    if (val & WAKE_CLR) {
        nv &= ~WAKE_STS;
    }
    if (usb_phy_suspended()) {
        nv |= PHY_SUSP_DONE;
    }
    if (s) {
        if (s->log) {
            static uint32_t lo, lv, lpc, rep;
            uint32_t pc = current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
            uint32_t lr = 0;    /* NvRegw: caller's return address at [sp+12] */
            if (current_cpu) {
                cpu_memory_rw_debug(current_cpu,
                                    ARM_CPU(current_cpu)->env.regs[13] + 12,
                                    &lr, 4, false);
            }
            pc = lr;
            if (old == lo && val == lv && pc == lpc) {
                rep++;
                if ((rep & (rep - 1)) == 0) {
                    fprintf(stderr, "usb: apb70 (same) x%u\n", rep);
                }
            } else {
                fprintf(stderr, "usb: apb70 %08x <- %08x => %08x pc~%08x lr %08x\n",
                        old, val, nv, pc, lr);
                lo = old; lv = val; lpc = pc; rep = 0;
            }
        }
        zune_apb_set70(nv);
        usb_update_irq(s);
    }
    return nv;
}

void zune_usb_apb_phy_write(uint32_t val)
{
    ZuneUsb *s = zune_usb;
    uint32_t v70 = zune_apb_get70();

    v70 = (val & 1) ? (v70 | PHY_SUSP_DONE) : (v70 & ~PHY_SUSP_DONE);
    zune_apb_set70(v70);
    if (s) {
        if (s->log) {
            fprintf(stderr, "usb: PHY %s (a30 %08x)\n",
                    (val & 1) ? "suspended" : "powered", val);
        }
        usb_update_irq(s);
    }
}

static uint32_t usb_otgsc_status(ZuneUsb *s)
{
    /* [8] ID (1 = B device), [9] AVV, [10] ASV, [11] BSV, [12] BSE */
    return (1u << 8) | (s->vbus ? (1u << 9) | (1u << 10) | (1u << 11) : (1u << 12));
}

static void usb_controller_reset(ZuneUsb *s)
{
    REG(s, R_USBSTS) = 0;
    REG(s, R_USBINTR) = 0;
    REG(s, R_DEVICEADDR) = 0;
    REG(s, R_EPSETUPSTAT) = 0;
    REG(s, R_EPPRIME) = 0;
    REG(s, R_EPSTATUS) = 0;
    REG(s, R_EPCOMPLETE) = 0;
    REG(s, R_USBMODE) = 0;
    for (int i = 0; i < 16; i++) {
        REG(s, R_EPCTRL0 + 4 * i) = i ? 0 : 0x00800080;   /* ep0 always enabled */
    }
    memset(s->cur_dtd, 0, sizeof(s->cur_dtd));
    memset(s->cur_pos, 0, sizeof(s->cur_pos));
    s->bus_reset_done = false;
}

static uint64_t usb_read(void *opaque, hwaddr off, unsigned size)
{
    ZuneUsb *s = opaque;
    uint32_t v;

    if (off >= USB_REGS) {
        return 0;
    }
    switch (off & ~3) {
    case 0x000: v = 0x0042fa05; break;              /* ID */
    case 0x004: v = 0x000000a5; break;              /* HWGENERAL */
    case 0x008: v = 0x10020001; break;              /* HWHOST */
    case 0x00c: v = 0x00000021; break;              /* HWDEVICE: dc, 16 eps */
    case 0x010: v = 0x80070908; break;              /* HWTXBUF */
    case 0x014: v = 0x00000508; break;              /* HWRXBUF */
    case 0x100: v = 0x01000040; break;              /* CAPLENGTH, HCIVERSION */
    case 0x104: v = 0x00010011; break;              /* HCSPARAMS */
    case 0x108: v = 0x00000006; break;              /* HCCPARAMS */
    case 0x120: v = 0x00000001; break;              /* DCIVERSION */
    case 0x124: v = 0x00000190; break;              /* DCCPARAMS: HC, DC, DEN 16 */
    case R_PORTSC1:
        v = REG(s, R_PORTSC1) & ~0x0c000005u;
        if (s->vbus) {
            v |= 1u | 4u | (2u << 26);              /* CCS, PE, high speed */
        }
        break;
    case R_OTGSC:
        v = (REG(s, R_OTGSC) & 0xffff00ffu) | usb_otgsc_status(s);
        break;
    case R_PHYCTRL:
        v = REG(s, R_PHYCTRL) | 0x80;               /* PHY clock valid */
        break;
    default:
        v = REG(s, off & ~3);
        break;
    }
    if (s->log) {
        fprintf(stderr, "usb: rd [%03x] -> %08x pc~%08x\n", (unsigned)off, v,
                current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    return v;
}

static void usb_write(void *opaque, hwaddr off, uint64_t val64, unsigned size)
{
    ZuneUsb *s = opaque;
    uint32_t val = val64;

    if (off >= USB_REGS) {
        return;
    }
    if (s->log) {
        fprintf(stderr, "usb: wr [%03x] <- %08x pc~%08x\n", (unsigned)off, val,
                current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    switch (off & ~3) {
    case R_USBCMD:
        if (val & 2) {                              /* RST: completes at once */
            usb_controller_reset(s);
            val &= ~2u;
        }
        if ((val & 1) && !(REG(s, R_USBCMD) & 1) && s->vbus) {
            s->attach_pending = true;               /* pull-up on: host sees us */
        }
        if (!(val & 1)) {
            s->bus_reset_done = false;
        }
        REG(s, R_USBCMD) = val;
        break;
    case R_USBSTS:
        REG(s, R_USBSTS) &= ~val;                   /* W1C */
        break;
    case R_OTGSC:
        REG(s, R_OTGSC) = (REG(s, R_OTGSC) & ~(val & 0x00ff0000)) |  /* W1C */
                          (val & 0xff0000ffu & ~0x00ff0000u);
        break;
    case R_EPSETUPSTAT:
    case R_EPCOMPLETE:
        REG(s, off & ~3) &= ~val;                   /* W1C */
        break;
    case R_EPFLUSH:
        if (s->dtdlog && (val & ~0x10001u)) {
            DTDLOG("FLUSH 0x%08x (ep1out cur 0x%08x pos %u)\n", (uint32_t)val,
                    (uint32_t)s->cur_dtd[2], s->cur_pos[2]);
        }
        REG(s, R_EPSTATUS) &= ~val;
        REG(s, R_EPPRIME) &= ~val;
        REG(s, R_EPFLUSH) = 0;                      /* completes at once */
        for (int b = 0; b < 32; b++) {
            if (val & (1u << b)) {
                int idx = 2 * (b & 15) + (b >> 4);
                s->cur_dtd[idx] = 0;
                s->cur_pos[idx] = 0;
            }
        }
        break;
    case R_EPPRIME:
        /* Priming completes at once: the dTD list hangs off dQH.next. */
        for (int b = 0; b < 32; b++) {
            if (val & (1u << b)) {
                int ep = b & 15, in = b >> 4;
                hwaddr qh = usb_qh(s, ep, in);
                uint32_t next = dma_rd32(qh + 8);
                if (s->dtdlog && ep) {
                    DTDLOG("PRIME ep%d%s epstat %d next 0x%08x cur 0x%08x pos %u\n",
                            ep, in ? "in" : "out", !!(REG(s, R_EPSTATUS) & (1u << b)), next,
                            (uint32_t)s->cur_dtd[2 * ep + in], s->cur_pos[2 * ep + in]);
                }
                if (!(REG(s, R_EPSTATUS) & (1u << b)) && !(next & 1)) {
                    /*
                     * Like the hardware: load the first dTD as the current
                     * one (dQH +4) and copy it into the overlay (+8..+0x24).
                     * NvDdkUsbfEndpointStatus walks its dTD pool up to +4.
                     */
                    hwaddr dtd = next & ~0x1fu;
                    dma_wr32(qh + 4, dtd);
                    for (int w = 0; w < 7; w++) {
                        dma_wr32(qh + 8 + 4 * w, dma_rd32(dtd + 4 * w));
                    }
                    s->cur_dtd[2 * ep + in] = dtd;
                    s->cur_pos[2 * ep + in] = 0;
                }
                if (!(next & 1)) {
                    REG(s, R_EPSTATUS) |= 1u << b;
                }
                if (s->log) {
                    fprintf(stderr, "usb: prime ep%d%s qh next %08x tok %08x\n",
                            ep, in ? "in" : "out", next,
                            (next & 1) ? 0 : dma_rd32((next & ~0x1fu) + 4));
                }
            }
        }
        REG(s, R_EPPRIME) = 0;
        break;
    default:
        REG(s, off & ~3) = val;
        break;
    }
    usb_update_irq(s);
}

static const MemoryRegionOps usb_ops = {
    .read = usb_read,
    .write = usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Cable plug/unplug (VBUS): B-session valid change interrupt + port change. */
void zune_usb_set_vbus(bool on)
{
    ZuneUsb *s = zune_usb;

    if (!s || s->vbus == on) {
        return;
    }
    s->vbus = on;
    s->attach_pending = true;
    if (!on) {
        s->bus_reset_done = false;
        REG(s, R_EPSTATUS) = 0;
    }
    if (zune_apb_get70() & WAKE_EN) {               /* PHY wake-up armed */
        zune_apb_set70(zune_apb_get70() | WAKE_STS);
    }
    info_report("zunehd: USB cable %s", on ? "attached" : "detached");
    usb_update_irq(s);
}

static void usb_plug_timer_cb(void *opaque)
{
    zune_usb_set_vbus(true);
}

void zune_usb_init(MemoryRegion *sysmem, hwaddr base, qemu_irq irq)
{
    ZuneUsb *s = g_new0(ZuneUsb, 1);
    const char *plug_at = getenv("ZUNEHD_USB_PLUG_AT");   /* virtual seconds */

    s->irq = irq;
    s->log = getenv("ZUNEHD_USBLOG") != NULL;
    s->dtdlog = getenv("ZUNEHD_USBDTDLOG") != NULL;
    s->selftest = getenv("ZUNEHD_USB_SELFTEST") != NULL;
    s->bus_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, usb_bus_timer_cb, s);
    s->host_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, usb_host_timer_cb, s);
    usb_controller_reset(s);
    memory_region_init_io(&s->mr, NULL, &usb_ops, s, "zunehd.usb", USB_REGS);
    memory_region_add_subregion(sysmem, base, &s->mr);
    /*
     * The rest of the 1 MiB aperture (OEMAddressTable VA 0x9FC00000) is
     * controller-local memory: NvDdkUsbf puts the dQH list at +0x1000.
     */
    memory_region_init_ram(&s->local_ram, NULL, "zunehd.usb-ram",
                           0x100000 - USB_REGS, &error_fatal);
    memory_region_add_subregion(sysmem, base + USB_REGS, &s->local_ram);
    zune_usb = s;
    if (plug_at && atoi(plug_at) > 0) {
        QEMUTimer *t = timer_new_ms(QEMU_CLOCK_VIRTUAL, usb_plug_timer_cb, s);
        timer_mod(t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + atoi(plug_at) * 1000LL);
    }
}
