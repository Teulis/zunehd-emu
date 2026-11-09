"""Minimal USB/IP client + MTP initiator for the emulated Zune HD (stdlib only).

QEMU side: ZUNEHD_USBIP=3240 (hw/arm/zunehd-usbip.c). This attaches busid 1-1
over TCP, configures the device and runs MTP over bulk ep1.

Usage:
  py -3 -I tools\\zmtp.py [--port 3240] info
  py -3 -I tools\\zmtp.py storage
  py -3 -I tools\\zmtp.py ls [--storage ID] [--parent H]
  py -3 -I tools\\zmtp.py put FILE [--storage ID] [--parent H] [--format 0x3009]
Commands can be chained: info storage put a.mp3 ls
"""
import argparse, os, socket, struct, sys, time

OPS = {0x1001: "GetDeviceInfo", 0x1002: "OpenSession", 0x1003: "CloseSession",
       0x1004: "GetStorageIDs", 0x1005: "GetStorageInfo", 0x1007: "GetObjectHandles",
       0x1008: "GetObjectInfo", 0x100C: "SendObjectInfo", 0x100D: "SendObject"}
RESP = {0x2001: "OK", 0x2002: "General_Error", 0x2003: "Session_Not_Open",
        0x2005: "Operation_Not_Supported", 0x2006: "Parameter_Not_Supported",
        0x2008: "Invalid_StorageID", 0x2009: "Invalid_ObjectHandle", 0x200C: "Store_Full",
        0x200F: "Access_Denied", 0x201A: "Invalid_Parent_Object", 0x201D: "Invalid_Parameter",
        0x201E: "Session_Already_Open", 0x2019: "Device_Busy"}
FORMATS = {".mp3": 0x3009, ".wma": 0xB901, ".wav": 0x3008, ".jpg": 0x3801, ".jpeg": 0x3801,
           ".mp4": 0xB982, ".wmv": 0xB981, ".m4a": 0xB984, ".txt": 0x3004}


class UsbIp:
    def __init__(self, host, port, busid="1-1", timeout=900):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.seq = 0
        req = struct.pack(">HHI", 0x0111, 0x8003, 0) + busid.encode().ljust(32, b"\0")
        self.s.sendall(req)
        hdr = self._recv(8)
        ver, code, status = struct.unpack(">HHI", hdr)
        if code != 0x0003 or status != 0:
            raise RuntimeError("import failed: code %#x status %d" % (code, status))
        dev = self._recv(312)
        vid, pid = struct.unpack(">HH", dev[300:304])
        print("usbip: imported %04x:%04x" % (vid, pid))

    def _recv(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError("connection closed")
            b += c
        return bytes(b)

    def submit(self, ep, din, length, data=b"", setup=b"\0" * 8):
        self.seq += 1
        hdr = struct.pack(">IIIII", 1, self.seq, (1 << 16) | 2, 1 if din else 0, ep)
        hdr += struct.pack(">IiiiI", 0, length if din else len(data), 0, 0, 0) + setup
        self.s.sendall(hdr + (b"" if din else data))
        while True:
            r = self._recv(48)
            cmd, seq = struct.unpack(">II", r[:8])
            status, actual = struct.unpack(">ii", r[20:28])
            payload = self._recv(actual) if (cmd == 3 and din and status == 0) else b""
            if cmd == 3 and seq == self.seq:
                if status != 0:
                    raise IOError("URB ep%d%s failed: status %d" % (ep, "in" if din else "out", status))
                return payload if din else actual

    def control(self, bm, req, val, idx, length=0, data=b""):
        setup = struct.pack("<BBHHH", bm, req, val, idx, length if bm & 0x80 else len(data))
        return self.submit(0, bool(bm & 0x80), length, data, setup)


def mtp_str(s):
    if not s:
        return b"\0"
    u = (s + "\0").encode("utf-16-le")
    return bytes([len(s) + 1]) + u


def get_str(b, o):
    n = b[o]
    o += 1
    s = b[o:o + 2 * n].decode("utf-16-le").rstrip("\0")
    return s, o + 2 * n


class Mtp:
    def __init__(self, usb):
        self.u = usb
        self.tid = 0
        self.session = False

    def _send_container(self, ctype, code, payload):
        c = struct.pack("<IHHI", 12 + len(payload), ctype, code, self.tid) + payload
        chunk = int(os.environ.get('ZMTP_CHUNK', 1 << 20))
        for o in range(0, len(c), chunk):
            t0 = time.time()
            self.u.submit(1, False, 0, c[o:o + chunk])
            if len(c) > (1 << 20):
                print('  urb %d/%d (%d bytes) %.1f s' % (o >> 20, (len(c) - 1) >> 20, min(1 << 20, len(c) - o), time.time() - t0), flush=True)
        if len(c) % 512 == 0:
            self.u.submit(1, False, 0, b"")                 # ZLP

    def _read_container(self):
        b = self.u.submit(1, True, 64 * 1024)
        if len(b) < 12:
            raise IOError("short container (%d bytes)" % len(b))
        total = struct.unpack("<I", b[:4])[0]
        while len(b) < total:
            b += self.u.submit(1, True, min(1 << 20, total - len(b) + 512))
        ctype, code, tid = struct.unpack("<HHI", b[4:12])
        return ctype, code, b[12:total]

    def op(self, code, params=(), data_out=None, quiet=False):
        self.tid = self.tid + 1 if code != 0x1002 else 0
        self._send_container(1, code, b"".join(struct.pack("<I", p) for p in params))
        if data_out is not None:
            self._send_container(2, code, data_out)
        data = None
        while True:
            ctype, rcode, payload = self._read_container()
            if ctype == 2:
                data = payload
                continue
            if ctype == 3:
                rparams = struct.unpack("<%dI" % (len(payload) // 4), payload[:len(payload) // 4 * 4])
                name = RESP.get(rcode, "%#06x" % rcode)
                if not quiet:
                    print("mtp: %s -> %s %s" % (OPS.get(code, "%#06x" % code), name,
                                               " ".join("%#x" % p for p in rparams)))
                return rcode, rparams, data

    def open(self):
        r, _, _ = self.op(0x1002, (1,))
        self.session = r in (0x2001, 0x201E)
        return self.session


def cmd_info(m):
    r, _, d = m.op(0x1001)
    # DeviceInfo: StdVer u16, VendorExtID u32, VendorExtVer u16, ExtDesc str, FuncMode u16, arrays...
    o = 8
    ext, o = get_str(d, o)
    o += 2
    arrays = []
    for _ in range(5):
        n = struct.unpack("<I", d[o:o + 4])[0]
        arrays.append(struct.unpack("<%dH" % n, d[o + 4:o + 4 + 2 * n]))
        o += 4 + 2 * n
    fields = []
    for _ in range(4):
        s, o = get_str(d, o)
        fields.append(s)
    print("  manufacturer %r model %r version %r serial %r" % tuple(fields))
    print("  extensions: %s" % ext)
    print("  %d operations, %d events, %d props, %d capture fmts, %d playback fmts" %
          tuple(len(a) for a in arrays))
    print("  playback formats: %s" % " ".join("%#06x" % f for f in arrays[4]))
    print("  vendor ops: %s" % " ".join("%#06x" % f for f in arrays[0] if f >= 0x9000))


def storages(m):
    r, _, d = m.op(0x1004)
    n = struct.unpack("<I", d[:4])[0]
    return list(struct.unpack("<%dI" % n, d[4:4 + 4 * n]))


def cmd_storage(m):
    for sid in storages(m):
        r, _, d = m.op(0x1005, (sid,))
        if r != 0x2001:
            continue
        stype, fstype, access, cap, free, freeobj = struct.unpack("<HHHQQI", d[:26])
        desc, o = get_str(d, 26)
        vol, o = get_str(d, o)
        print("  storage %#010x type %d fs %d access %d capacity %d MiB free %d MiB desc %r vol %r"
              % (sid, stype, fstype, access, cap >> 20, free >> 20, desc, vol))


def cmd_ls(m, sid, parent):
    r, _, d = m.op(0x1007, (sid, 0, parent))
    if r != 0x2001:
        return
    n = struct.unpack("<I", d[:4])[0]
    hs = struct.unpack("<%dI" % n, d[4:4 + 4 * n])
    print("  %d objects" % n)
    for h in hs:
        r, _, oi = m.op(0x1008, (h,), quiet=True)
        if r != 0x2001:
            print("  %#010x (GetObjectInfo %#x)" % (h, r))
            continue
        st, fmt, prot, size = struct.unpack("<IHHI", oi[:12])
        par = struct.unpack("<I", oi[38:42])[0]
        name, _ = get_str(oi, 52)
        print("  %#010x fmt %#06x size %10d parent %#010x %s" % (h, fmt, size, par, name))


def cmd_put(m, path, sid, parent, fmt):
    data = open(path, "rb").read()
    name = os.path.basename(path)
    if fmt is None:
        fmt = FORMATS.get(os.path.splitext(name)[1].lower(), 0x3000)
    oi = struct.pack("<IHHI", sid, fmt, 0, len(data))
    oi += struct.pack("<HIIIIIII", 0, 0, 0, 0, 0, 0, 0, parent)
    oi += struct.pack("<HII", 0, 0, 0)
    stamp = time.strftime("%Y%m%dT%H%M%S")
    oi += mtp_str(name) + mtp_str(stamp) + mtp_str(stamp) + mtp_str("")
    r, p, _ = m.op(0x100C, (sid, parent), data_out=oi)
    if r != 0x2001:
        return False
    t0 = time.time()
    r, _, _ = m.op(0x100D, data_out=data)
    dt = time.time() - t0
    print("  sent %s: %d bytes in %.1f s (%.0f KiB/s), handle %#x" %
          (name, len(data), dt, len(data) / 1024 / max(dt, 1e-3), p[2] if len(p) > 2 else 0))
    return r == 0x2001


def read_id3(data):
    """Minimal ID3v2.3/2.4 text-frame reader (TIT2/TPE1/TALB/TRCK/TCON)."""
    tags = {}
    if data[:3] != b"ID3":
        return tags
    size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9]
    o, end = 10, 10 + size
    while o + 10 <= end:
        fid = data[o:o + 4].decode("latin-1")
        n = struct.unpack(">I", data[o + 4:o + 8])[0]
        if not fid.strip("\0") or n == 0:
            break
        body = data[o + 10:o + 10 + n]
        if fid.startswith("T") and body:
            enc, raw = body[0], body[1:]
            txt = raw.decode("utf-16" if enc == 1 else "utf-16-be" if enc == 2 else
                             "utf-8" if enc == 3 else "latin-1", "replace")
            tags[fid] = txt.rstrip("\0")
        o += 10 + n
    return tags


def mp3_duration_ms(data):
    """Duration from MPEG-1 Layer III frame headers (CBR estimate)."""
    o = 0
    if data[:3] == b"ID3":
        o = 10 + ((data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9])
    rates = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
    srs = [44100, 48000, 32000, 0]
    if o + 4 > len(data) or data[o] != 0xFF or (data[o + 1] & 0xE0) != 0xE0:
        return 0
    br = rates[data[o + 2] >> 4] * 1000
    return int((len(data) - o) * 8 * 1000 / br) if br else 0


def prop(code, dtype, value):
    if dtype == 0xFFFF:
        v = mtp_str(value)
    else:
        v = struct.pack({0x0004: "<H", 0x0006: "<I", 0x0008: "<Q"}[dtype], value)
    return struct.pack("<IHH", 0, code, dtype) + v


def cmd_putmeta(m, path, sid, parent, fmt):
    """SendObjectPropList (0x9808) with music metadata, then SendObject."""
    data = open(path, "rb").read()
    name = os.path.basename(path)
    if fmt is None:
        fmt = FORMATS.get(os.path.splitext(name)[1].lower(), 0x3000)
    t = read_id3(data)
    title = t.get("TIT2", os.path.splitext(name)[0])
    props = [prop(0xDC07, 0xFFFF, name),                      # ObjectFileName
             prop(0xDC44, 0xFFFF, title)]                     # Name
    if "TPE1" in t:
        props.append(prop(0xDC46, 0xFFFF, t["TPE1"]))         # Artist
        props.append(prop(0xDC9B, 0xFFFF, t["TPE1"]))         # AlbumArtist
    if "TALB" in t:
        props.append(prop(0xDC9A, 0xFFFF, t["TALB"]))         # AlbumName
    if "TCON" in t:
        props.append(prop(0xDC8C, 0xFFFF, t["TCON"]))         # Genre
    if t.get("TRCK", "").split("/")[0].isdigit():
        props.append(prop(0xDC8B, 0x0004, int(t["TRCK"].split("/")[0])))  # Track
    dur = mp3_duration_ms(data) if fmt == 0x3009 else 0
    if dur:
        props.append(prop(0xDC89, 0x0006, dur))               # Duration (ms)
    plist = struct.pack("<I", len(props)) + b"".join(props)
    print("  metadata: %r / %r / %r, %d ms" % (title, t.get("TPE1"), t.get("TALB"), dur))
    r, p, _ = m.op(0x9808, (sid, parent, fmt, len(data) >> 32, len(data) & 0xFFFFFFFF),
                   data_out=plist)
    if r != 0x2001:
        return False
    t0 = time.time()
    r, _, _ = m.op(0x100D, data_out=data)
    dt = time.time() - t0
    print("  sent %s: %d bytes in %.1f s, handle %#x" % (name, len(data), dt,
                                                          p[2] if len(p) > 2 else 0))
    return r == 0x2001


def enc_value(dtype, value):
    if dtype == 0xFFFF:
        return mtp_str(value)
    return struct.pack({0x0002: "<B", 0x0004: "<H", 0x0006: "<I", 0x0008: "<Q"}[dtype], value)


def set_prop(m, h, code, dtype, value):
    r, _, _ = m.op(0x9804, (h, code), data_out=enc_value(dtype, value), quiet=True)
    if r != 0x2001:
        print("  SetObjectPropValue %#x on %#x -> %s" % (code, h, RESP.get(r, hex(r))))
    return r == 0x2001


def get_prop_str(m, h, code):
    r, _, d = m.op(0x9803, (h, code), quiet=True)
    return get_str(d, 0)[0] if r == 0x2001 and d else None


def handles(m, sid, fmt=0):
    r, _, d = m.op(0x1007, (sid, fmt, 0), quiet=True)
    if r != 0x2001:
        return []
    n = struct.unpack("<I", d[:4])[0]
    return list(struct.unpack("<%dI" % n, d[4:4 + 4 * n]))


def create_abstract(m, sid, fmt, filename):
    oi = struct.pack("<IHHI", sid, fmt, 0, 0) + struct.pack("<HIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0)
    oi += struct.pack("<HII", 0, 0, 0) + mtp_str(filename) + b"\0\0\0"
    r, p, _ = m.op(0x100C, (sid, 0), data_out=oi, quiet=True)
    if r != 0x2001:
        print("  create %s -> %s" % (filename, RESP.get(r, hex(r))))
        return None
    m.op(0x100D, data_out=b"", quiet=True)
    return p[2]


def find_by_name(m, sid, fmt, name, artist=None):
    for h in handles(m, sid, fmt):
        if get_prop_str(m, h, 0xDC44) == name and \
                (artist is None or get_prop_str(m, h, 0xDC46) == artist):
            return h
    return None


def cmd_sync(m, paths, sid):
    """Zune-style music sync (as zune-explorer does it): tracks with
    metadata, then artist (0xB218) and album (0xBA03) objects, ArtistId links
    (0xDAB9) and album -> track references (SetObjectReferences 0x9811)."""
    albums = {}
    for path in paths:
        data = open(path, "rb").read()
        name = os.path.basename(path)
        fmt = FORMATS.get(os.path.splitext(name)[1].lower(), 0x3000)
        t = read_id3(data)
        r, p, _ = m.op(0x100C, (sid, 0), data_out=objinfo(sid, fmt, len(data), name), quiet=True)
        if r != 0x2001:
            print("  %s: SendObjectInfo -> %s" % (name, RESP.get(r, hex(r))))
            continue
        h = p[2]
        m.op(0x100D, data_out=data, quiet=True)
        title = t.get("TIT2", os.path.splitext(name)[0])
        artist = t.get("TPE1", "Unknown Artist")
        album = t.get("TALB")
        set_prop(m, h, 0xDC44, 0xFFFF, title)
        set_prop(m, h, 0xDC46, 0xFFFF, artist)
        if album:
            set_prop(m, h, 0xDC9A, 0xFFFF, album)
            set_prop(m, h, 0xDC9B, 0xFFFF, artist)
        if "TCON" in t:
            set_prop(m, h, 0xDC8C, 0xFFFF, t["TCON"])
        if t.get("TRCK", "").split("/")[0].isdigit():
            set_prop(m, h, 0xDC8B, 0x0004, int(t["TRCK"].split("/")[0]))
        dur = mp3_duration_ms(data) if fmt == 0x3009 else 0
        if dur:
            set_prop(m, h, 0xDC89, 0x0006, dur)
        print("  track %#x: %r / %r / %r (%d ms)" % (h, title, artist, album, dur))
        albums.setdefault((artist, album), []).append(h)
    for (artist, album), tracks in albums.items():
        ah = find_by_name(m, sid, 0xB218, artist)
        if ah is None:
            ah = create_abstract(m, sid, 0xB218, "%s.art" % artist)
            if ah:
                set_prop(m, ah, 0xDC44, 0xFFFF, artist)
                print("  artist %#x: %r (new)" % (ah, artist))
        else:
            print("  artist %#x: %r (existing)" % (ah, artist))
        for h in tracks:
            if ah:
                set_prop(m, h, 0xDAB9, 0x0006, ah)
        if not album:
            continue
        bh = find_by_name(m, sid, 0xBA03, album, artist)
        refs = list(tracks)
        if bh is None:
            bh = create_abstract(m, sid, 0xBA03, "%s--%s.alb" % (artist, album))
            if not bh:
                continue
            set_prop(m, bh, 0xDC44, 0xFFFF, album)
            set_prop(m, bh, 0xDC46, 0xFFFF, artist)
            if ah:
                set_prop(m, bh, 0xDAB9, 0x0006, ah)
            print("  album %#x: %r (new)" % (bh, album))
        else:
            r, _, d = m.op(0x9810, (bh,), quiet=True)
            if r == 0x2001 and d:
                n = struct.unpack("<I", d[:4])[0]
                refs = list(struct.unpack("<%dI" % n, d[4:4 + 4 * n])) + refs
            print("  album %#x: %r (existing)" % (bh, album))
        data = struct.pack("<I", len(refs)) + b"".join(struct.pack("<I", x) for x in refs)
        r, _, _ = m.op(0x9811, (bh,), data_out=data, quiet=True)
        print("  album %#x -> %d track refs: %s" % (bh, len(refs), RESP.get(r, hex(r))))


def objinfo(sid, fmt, size, name, parent=0):
    oi = struct.pack("<IHHI", sid, fmt, 0, size)
    oi += struct.pack("<HIIIIIII", 0, 0, 0, 0, 0, 0, 0, parent)
    oi += struct.pack("<HII", 0, 0, 0)
    stamp = time.strftime("%Y%m%dT%H%M%S")
    return oi + mtp_str(name) + mtp_str(stamp) + mtp_str(stamp) + mtp_str("")


def main():
    # Track titles can be non-ASCII; the Windows console code page cannot print them all.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3240)
    ap.add_argument("--storage", type=lambda x: int(x, 0), default=None)
    ap.add_argument("--parent", type=lambda x: int(x, 0), default=0xFFFFFFFF)
    ap.add_argument("--format", type=lambda x: int(x, 0), default=None)
    ap.add_argument("cmds", nargs="+")
    a = ap.parse_args()

    usb = UsbIp(a.host, a.port)
    usb.control(0x00, 9, 1, 0)                               # SET_CONFIGURATION 1
    print("usb: configured")
    m = Mtp(usb)
    m.open()
    i = 0
    while i < len(a.cmds):
        c = a.cmds[i]
        if c == "info":
            cmd_info(m)
        elif c == "storage":
            cmd_storage(m)
        elif c == "ls":
            sid = a.storage if a.storage is not None else 0xFFFFFFFF
            cmd_ls(m, sid, a.parent if a.parent != 0xFFFFFFFF else 0)
        elif c == "put":
            i += 1
            sid = a.storage if a.storage is not None else storages(m)[0]
            cmd_put(m, a.cmds[i], sid, a.parent, a.format)
        elif c == "sync":
            files = []
            while i + 1 < len(a.cmds) and os.path.isfile(a.cmds[i + 1]):
                i += 1
                files.append(a.cmds[i])
            sid = a.storage if a.storage is not None else storages(m)[0]
            cmd_sync(m, files, sid)
        elif c == "rm":
            i += 1
            m.op(0x100B, (int(a.cmds[i], 0),))
        elif c == "putmeta":
            i += 1
            sid = a.storage if a.storage is not None else storages(m)[0]
            cmd_putmeta(m, a.cmds[i], sid, a.parent, a.format)
        else:
            sys.exit("unknown command %r" % c)
        i += 1
    m.op(0x1003)


if __name__ == "__main__":
    main()
