"""Make a short test copy of an MP3: minimal ID3v2.3 tag (title/artist/album/track/
genre copied, no pictures) + the first N seconds of MPEG audio frames.

  py -3 -I tools\\mp3cut.py IN.mp3 OUT.mp3 [--seconds 30] [--title-suffix " (30s)"]
"""
import argparse, struct

BR = {1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]}
SR = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def read_tags(d):
    tags = {}
    if d[:3] != b"ID3":
        return tags, 0
    size = (d[6] << 21) | (d[7] << 14) | (d[8] << 7) | d[9]
    o, end = 10, 10 + size
    while o + 10 <= end:
        fid = d[o:o + 4].decode("latin-1")
        n = struct.unpack(">I", d[o + 4:o + 8])[0]
        if not fid.strip("\0") or n == 0:
            break
        if fid in ("TIT2", "TPE1", "TALB", "TRCK", "TCON", "TPE2"):
            tags[fid] = d[o + 10:o + 10 + n]       # keep encoded body as is
        o += 10 + n
    return tags, end


def frame_len(h):
    ver = (h[1] >> 3) & 3
    layer = (h[1] >> 1) & 3
    bri = h[2] >> 4
    sri = (h[2] >> 2) & 3
    pad = (h[2] >> 1) & 1
    if h[0] != 0xFF or (h[1] & 0xE0) != 0xE0 or layer != 1 or ver != 3:
        return 0, 0
    br = BR[1][bri] * 1000
    sr = SR[ver][sri]
    return 144 * br // sr + pad, sr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--title-suffix", default=" (30s)")
    a = ap.parse_args()
    d = open(a.inp, "rb").read()
    tags, o = read_tags(d)
    start, frames, samples, sr = o, 0, 0, 44100
    while o + 4 <= len(d):
        n, s = frame_len(d[o:o + 4])
        if not n:
            break
        sr = s
        o += n
        frames += 1
        samples += 1152
        if samples >= a.seconds * sr:
            break
    audio = d[start:o]
    if "TIT2" in tags and a.title_suffix:
        body = tags["TIT2"]
        if body[:1] == b"\x01":
            # strip whole UTF-16 code units: rstrip(b"\0") also ate the high byte of
            # the last character ("p" = 70 00), breaking the frame by one byte
            while len(body) >= 3 and body[-2:] == b"\0\0":
                body = body[:-2]
            tags["TIT2"] = body + a.title_suffix.encode("utf-16-le")
        else:
            tags["TIT2"] = body.rstrip(b"\0") + a.title_suffix.encode("latin-1")
    fr = b"".join(k.encode() + struct.pack(">I", len(v)) + b"\0\0" + v for k, v in tags.items())
    ss = len(fr)
    tag = b"ID3\x03\x00\x00" + bytes([(ss >> 21) & 0x7f, (ss >> 14) & 0x7f, (ss >> 7) & 0x7f, ss & 0x7f]) + fr
    open(a.out, "wb").write(tag + audio)
    print("%s: %d frames (%.1f s), %d bytes audio, tag %d bytes" % (a.out, frames, samples / sr,
                                                                    len(audio), len(tag)))


if __name__ == "__main__":
    main()
