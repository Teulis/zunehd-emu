/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zune HD: USB/IP server (protocol 1.1.1) exporting the emulated USB device
 * (zunehd-usb.c, via the zune_usb_dev_* API) so a PC-side USB/IP client
 * (usbip-win2, Linux vhci, or tools\zmtp.py) can drive MTP.
 *
 * Enable with ZUNEHD_USBIP=<tcp port> (e.g. 3240); listens on 127.0.0.1.
 * Busid "1-1". On OP_REQ_IMPORT the cable is plugged (if not already); the
 * import reply is sent once the guest's controller has seen the bus reset and
 * we have addressed the device (USB/IP clients never send SET_ADDRESS).
 * The connection close unplugs the cable again.
 *
 * URBs are queued per endpoint/direction and pushed into the device from a
 * 1 ms virtual-clock timer (the guest driver primes dTDs asynchronously), so
 * a long-pending interrupt IN URB does not block bulk traffic.
 * ZUNEHD_USBIPLOG=1 logs every URB.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "hw/arm/zunehd-usb.h"

#define USBIP_VERSION       0x0111
#define OP_REQ_DEVLIST      0x8005
#define OP_REP_DEVLIST      0x0005
#define OP_REQ_IMPORT       0x8003
#define OP_REP_IMPORT       0x0003
#define CMD_SUBMIT          1
#define CMD_UNLINK          2
#define RET_SUBMIT          3
#define RET_UNLINK          4

#define BUSNUM  1
#define DEVNUM  2
#define BUSID   "1-1"

#define CTRL_TIMEOUT_MS 2000

typedef struct Urb {
    uint32_t seq;
    int ep, in;                 /* endpoint number, direction (1 = IN) */
    int len, done;
    int stage;                  /* control: 0 setup, 1 data, 2 status */
    int64_t t0;
    uint8_t setup[8];
    uint8_t *buf;
    bool internal;              /* our own SET_ADDRESS */
    bool early;                 /* bulk OUT already completed to the client */
} Urb;

/*
 * Bulk OUT write-behind: a bulk OUT URB is completed to the client as soon as
 * it is queued (up to this many bytes not yet taken by the guest per
 * endpoint), so the client sends the next one at once and the device sees a
 * continuous stream. Without it, the gap between "URB n done" and "URB n+1
 * arrives" (TCP + client time) lets the guest's virtual clock run: the Zune
 * MTP driver flushes ep1 OUT ~25 ms after its last dTD completion, dropping
 * the dTD a URB boundary left partly filled (12 bytes short, from the 12-byte
 * MTP header dTD) -> 12 stale bytes in every file > 1 MiB.
 */
#define WRITE_BEHIND_MAX    (8 * 1024 * 1024)

typedef struct ZuneUsbip {
    CharFrontend chr;
    bool connected, imported, import_pending, addressed;
    GByteArray *rx;
    GQueue q[32];               /* index 2*ep + in */
    QEMUTimer *timer;
    bool log;
} ZuneUsbip;

static ZuneUsbip *zip;

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void usbip_send(ZuneUsbip *s, const uint8_t *b, int n)
{
    if (s->connected) {
        qemu_chr_fe_write_all(&s->chr, b, n);
    }
}

/*
 * struct usbip_usb_device (312 bytes) + one usbip_usb_interface (4 bytes),
 * from the descriptors the device reports (see NOTES: 045e:063e, class ff,
 * 1 config, 1 interface ff/ff/ff, high speed).
 */
static int usbip_fill_device(uint8_t *p, bool with_ifaces)
{
    memset(p, 0, 312);
    snprintf((char *)p, 256, "/sys/devices/zunehd/usb1/" BUSID);
    snprintf((char *)p + 256, 32, BUSID);
    put32(p + 288, BUSNUM);
    put32(p + 292, DEVNUM);
    put32(p + 296, 3);                  /* USB_SPEED_HIGH */
    put16(p + 300, 0x045e);
    put16(p + 302, 0x063e);
    put16(p + 304, 0x0100);
    p[306] = 0xff; p[307] = 0; p[308] = 0;  /* class / subclass / protocol */
    p[309] = 1;                         /* bConfigurationValue */
    p[310] = 1;                         /* bNumConfigurations */
    p[311] = 1;                         /* bNumInterfaces */
    if (!with_ifaces) {
        return 312;
    }
    p[312] = 0xff; p[313] = 0xff; p[314] = 0xff; p[315] = 0;
    return 316;
}

static void usbip_reply_devlist(ZuneUsbip *s)
{
    uint8_t b[12 + 316];

    put16(b, USBIP_VERSION);
    put16(b + 2, OP_REP_DEVLIST);
    put32(b + 4, 0);
    put32(b + 8, 1);
    usbip_send(s, b, 12 + usbip_fill_device(b + 12, true));
}

static void usbip_reply_import(ZuneUsbip *s)
{
    uint8_t b[8 + 312];

    put16(b, USBIP_VERSION);
    put16(b + 2, OP_REP_IMPORT);
    put32(b + 4, 0);
    usbip_fill_device(b + 8, false);
    usbip_send(s, b, sizeof(b));
    s->imported = true;
    s->import_pending = false;
    info_report("usbip: device " BUSID " imported");
}

static void usbip_ret_submit(ZuneUsbip *s, Urb *u, int status)
{
    int dlen = (u->in && status == 0) ? u->done : 0;
    uint8_t *b = g_malloc0(48 + dlen);

    put32(b, RET_SUBMIT);
    put32(b + 4, u->seq);
    put32(b + 20, status);
    put32(b + 24, u->done);
    if (dlen) {
        memcpy(b + 48, u->buf, dlen);
    }
    if (s->log) {
        info_report("usbip: ret seq %u ep%d%s len %d -> %d status %d",
                    u->seq, u->ep, u->in ? "in" : "out", u->len, u->done, status);
    }
    usbip_send(s, b, 48 + dlen);
    g_free(b);
}

/* Complete queued bulk OUT URBs early while they fit in WRITE_BEHIND_MAX. */
static void usbip_write_behind(ZuneUsbip *s, int qi)
{
    int64_t pending = 0;

    if (!s->imported) {
        return;
    }
    for (GList *l = s->q[qi].head; l; l = l->next) {
        Urb *u = l->data;
        pending += u->len - u->done;
        if (pending > WRITE_BEHIND_MAX) {
            break;
        }
        if (!u->early && !u->internal && u->len > 0) {
            int done = u->done;
            u->early = true;
            u->done = u->len;                   /* report as fully sent */
            usbip_ret_submit(s, u, 0);
            u->done = done;
        }
    }
}

static void urb_free(Urb *u)
{
    g_free(u->buf);
    g_free(u);
}

static void usbip_flush(ZuneUsbip *s)
{
    for (int i = 0; i < 32; i++) {
        Urb *u;
        while ((u = g_queue_pop_head(&s->q[i]))) {
            urb_free(u);
        }
    }
}

/* One step of a control transfer on ep0. Returns 1 done, 0 pending, <0 error. */
static int usbip_ctrl_step(ZuneUsbip *s, Urb *u)
{
    bool din = u->setup[0] & 0x80;
    uint8_t z = 0;
    int n;

    if (u->stage == 0) {
        zune_usb_dev_setup(u->setup);
        u->stage = u->len ? 1 : 2;
        u->t0 = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        return 0;
    }
    if (zune_usb_ep_stalled(0, din || u->stage == 2)) {
        return -32;                                     /* -EPIPE: STALL */
    }
    if (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - u->t0 > CTRL_TIMEOUT_MS) {
        warn_report("usbip: control %02x %02x timed out in stage %d",
                    u->setup[0], u->setup[1], u->stage);
        return -32;
    }
    if (u->stage == 1) {
        if (din) {
            n = zune_usb_dev_in(0, u->buf + u->done, u->len - u->done);
            if (n >= 0) {
                u->done += n;
                if (u->done >= u->len || n == 0 || (n % 64)) {
                    u->stage = 2;
                }
            }
        } else {
            n = zune_usb_dev_out_xfer(0, u->buf + u->done, u->len - u->done,
                                      (u->len % 64) != 0);
            if (n > 0) {
                u->done += n;
                if (u->done >= u->len) {
                    u->stage = 2;
                }
            }
        }
        return 0;
    }
    /* status stage: opposite direction, zero length */
    n = din ? zune_usb_dev_out(0, NULL, 0) : zune_usb_dev_in(0, &z, 0);
    return n >= 0 ? 1 : 0;
}

/* One step of a bulk/interrupt transfer. Returns 1 done, 0 pending. */
static int usbip_data_step(ZuneUsbip *s, Urb *u)
{
    int mp = u->ep == 2 ? 64 : 512, n;

    if (u->in) {
        n = zune_usb_dev_in(u->ep, u->buf + u->done, u->len - u->done);
        if (n < 0) {
            return 0;
        }
        u->done += n;
        return (n == 0 || (n % mp) || u->done >= u->len) ? 1 : 0;
    }
    if (u->len == 0) {
        return zune_usb_dev_out(u->ep, NULL, 0) >= 0 ? 1 : 0;
    }
    /* short-packet end is a property of the whole URB, not of the remainder */
    n = zune_usb_dev_out_xfer(u->ep, u->buf + u->done, u->len - u->done,
                              (u->len % mp) != 0);
    if (n > 0) {
        u->done += n;
    }
    return u->done >= u->len ? 1 : 0;
}

static void usbip_kick(ZuneUsbip *s)
{
    if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

static void usbip_timer_cb(void *opaque)
{
    ZuneUsbip *s = opaque;
    bool busy = false;

    if (s->import_pending) {
        if (!zune_usb_dev_ready()) {
            usbip_kick(s);
            return;
        }
        if (!s->addressed && g_queue_is_empty(&s->q[0])) {
            Urb *u = g_new0(Urb, 1);
            static const uint8_t set_addr[8] = { 0x00, 5, DEVNUM, 0, 0, 0, 0, 0 };
            memcpy(u->setup, set_addr, 8);
            u->internal = true;
            g_queue_push_tail(&s->q[0], u);
        }
    }
    if (!zune_usb_dev_ready()) {
        /* cable not up (yet): keep URBs queued */
        for (int i = 0; i < 32; i++) {
            busy |= !g_queue_is_empty(&s->q[i]);
        }
        if (busy) {
            timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
        }
        return;
    }
    for (int i = 0; i < 32; i++) {
        for (int iter = 0; iter < 64; iter++) {
            Urb *u = g_queue_peek_head(&s->q[i]);
            int r;

            if (!u) {
                break;
            }
            r = u->ep == 0 ? usbip_ctrl_step(s, u) : usbip_data_step(s, u);
            if (r == 0) {
                if (u->ep == 0 && u->stage == 1 && iter < 63) {
                    continue;               /* let the data stage poll again */
                }
                busy = true;
                break;
            }
            g_queue_pop_head(&s->q[i]);
            if (u->internal) {
                if (r > 0) {
                    s->addressed = true;
                    info_report("usbip: device addressed (%d)", DEVNUM);
                    if (s->import_pending) {
                        usbip_reply_import(s);
                    }
                } else {
                    warn_report("usbip: SET_ADDRESS failed (%d), retrying", r);
                }
            } else if (!u->early) {
                usbip_ret_submit(s, u, r > 0 ? 0 : r);
            }
            urb_free(u);
            if (i >= 2 && !(i & 1)) {
                usbip_write_behind(s, i);       /* room freed: complete more */
            }
        }
    }
    if (busy || s->import_pending) {
        usbip_kick(s);
    }
}

static void usbip_unlink(ZuneUsbip *s, uint32_t seq, uint32_t target)
{
    uint8_t b[48] = { 0 };
    int status = 0;

    for (int i = 0; i < 32; i++) {
        for (GList *l = s->q[i].head; l; l = l->next) {
            Urb *u = l->data;
            if (!u->internal && !u->early && u->seq == target) {
                g_queue_delete_link(&s->q[i], l);
                urb_free(u);
                status = -104;                          /* -ECONNRESET */
                goto found;
            }
        }
    }
found:
    if (s->log) {
        info_report("usbip: unlink seq %u (target %u) -> %d", seq, target, status);
    }
    put32(b, RET_UNLINK);
    put32(b + 4, seq);
    put32(b + 20, status);
    usbip_send(s, b, sizeof(b));
}

/* Parse as many complete messages from the receive buffer as possible. */
static void usbip_parse(ZuneUsbip *s)
{
    for (;;) {
        const uint8_t *p = s->rx->data;
        int avail = s->rx->len, need;

        if (!s->imported && !s->import_pending) {
            uint16_t code;
            if (avail < 8) {
                return;
            }
            code = get16(p + 2);
            if (code == OP_REQ_DEVLIST) {
                g_byte_array_remove_range(s->rx, 0, 8);
                usbip_reply_devlist(s);
                continue;
            }
            if (code == OP_REQ_IMPORT) {
                if (avail < 40) {
                    return;
                }
                info_report("usbip: import request for '%.32s'", (const char *)p + 8);
                g_byte_array_remove_range(s->rx, 0, 40);
                s->import_pending = true;
                s->addressed = false;
                zune_usb_set_vbus(true);
                usbip_kick(s);
                continue;
            }
            warn_report("usbip: unknown op 0x%04x, closing", code);
            qemu_chr_fe_disconnect(&s->chr);
            return;
        }
        if (s->import_pending) {
            return;                             /* client waits for the reply */
        }
        if (avail < 48) {
            return;
        }
        if (get32(p) == CMD_SUBMIT) {
            int in = get32(p + 12) != 0, ep = get32(p + 16) & 15;
            int len = (int32_t)get32(p + 24);
            Urb *u;

            if (len < 0 || len > 16 * 1024 * 1024) {
                warn_report("usbip: bad length %d, closing", len);
                qemu_chr_fe_disconnect(&s->chr);
                return;
            }
            need = 48 + (in ? 0 : len);
            if (avail < need) {
                return;
            }
            u = g_new0(Urb, 1);
            u->seq = get32(p + 4);
            u->ep = ep;
            u->len = len;
            memcpy(u->setup, p + 40, 8);
            /* ep0: direction comes from the setup packet */
            u->in = ep == 0 ? !!(u->setup[0] & 0x80) : in;
            u->buf = g_malloc0(MAX(len, 1));
            if (!in && len) {
                memcpy(u->buf, p + 48, len);
            }
            if (s->log) {
                info_report("usbip: submit seq %u ep%d%s len %d%s", u->seq, ep,
                            u->in ? "in" : "out", len, ep == 0 ? " (control)" : "");
            }
            g_byte_array_remove_range(s->rx, 0, need);
            g_queue_push_tail(&s->q[ep == 0 ? 0 : 2 * ep + u->in], u);
            if (ep != 0 && !u->in) {
                usbip_write_behind(s, 2 * ep);
            }
            usbip_kick(s);
            continue;
        }
        if (get32(p) == CMD_UNLINK) {
            uint32_t seq = get32(p + 4), target = get32(p + 20);
            g_byte_array_remove_range(s->rx, 0, 48);
            usbip_unlink(s, seq, target);
            continue;
        }
        warn_report("usbip: unknown command %u, closing", get32(p));
        qemu_chr_fe_disconnect(&s->chr);
        return;
    }
}

static int usbip_can_read(void *opaque)
{
    return 64 * 1024;
}

static void usbip_read(void *opaque, const uint8_t *buf, int size)
{
    ZuneUsbip *s = opaque;

    g_byte_array_append(s->rx, buf, size);
    usbip_parse(s);
}

static void usbip_event(void *opaque, QEMUChrEvent ev)
{
    ZuneUsbip *s = opaque;

    switch (ev) {
    case CHR_EVENT_OPENED:
        s->connected = true;
        s->imported = s->import_pending = false;
        g_byte_array_set_size(s->rx, 0);
        info_report("usbip: client connected");
        break;
    case CHR_EVENT_CLOSED:
        if (s->connected) {
            info_report("usbip: client disconnected");
        }
        s->connected = false;
        s->imported = s->import_pending = false;
        s->addressed = false;
        usbip_flush(s);
        g_byte_array_set_size(s->rx, 0);
        zune_usb_set_vbus(false);
        break;
    default:
        break;
    }
}

void zune_usbip_init(void)
{
    const char *port = getenv("ZUNEHD_USBIP");
    g_autofree char *spec = NULL;
    Chardev *chr;

    if (!port || atoi(port) <= 0) {
        return;
    }
    spec = g_strdup_printf("tcp:127.0.0.1:%d,server=on,wait=off", atoi(port));
    chr = qemu_chr_new("zunehd-usbip", spec, NULL);
    if (!chr) {
        error_report("usbip: cannot listen on %s", spec);
        return;
    }
    zip = g_new0(ZuneUsbip, 1);
    zip->rx = g_byte_array_new();
    zip->log = getenv("ZUNEHD_USBIPLOG") != NULL;
    for (int i = 0; i < 32; i++) {
        g_queue_init(&zip->q[i]);
    }
    zip->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, usbip_timer_cb, zip);
    qemu_chr_fe_init(&zip->chr, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&zip->chr, usbip_can_read, usbip_read, usbip_event,
                             NULL, zip, NULL, true);
    info_report("usbip: listening on 127.0.0.1:%d (busid " BUSID ")", atoi(port));
}
