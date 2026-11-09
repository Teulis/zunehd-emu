"""Write a small valid MP3 (MPEG-1 Layer III, 128 kbps, 44.1 kHz, silence)
with an ID3v2.3 tag, for MTP sync tests.

  py -3 -I tools\\mksilentmp3.py OUT.mp3 [--seconds 10] [--title T] [--artist A] [--album B]
"""
import argparse, struct


def id3_frame(fid, text):
    body = b"\x01" + b"\xff\xfe" + text.encode("utf-16-le")   # UTF-16 with BOM
    return fid.encode() + struct.pack(">I", len(body)) + b"\0\0" + body


def syncsafe(n):
    return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--title", default="Emulator Test Tone")
    ap.add_argument("--artist", default="QEMU Zune")
    ap.add_argument("--album", default="Sync Tests")
    ap.add_argument("--track", default="1")
    ap.add_argument("--genre", default="Electronic")
    a = ap.parse_args()
    frames = b"".join(id3_frame(f, t) for f, t in
                      (("TIT2", a.title), ("TPE1", a.artist), ("TALB", a.album),
                       ("TRCK", a.track), ("TCON", a.genre)))
    tag = b"ID3\x03\x00\x00" + syncsafe(len(frames)) + frames
    # 128 kbps @ 44.1 kHz: 144*128000/44100 = 417 bytes (no padding); 1152 samples per frame.
    # All-zero side info + main data decodes to silence.
    frame = b"\xff\xfb\x90\x64" + b"\0" * 413
    n = int(a.seconds * 44100 / 1152)
    with open(a.out, "wb") as f:
        f.write(tag + frame * n)
    print("%s: %d frames (%.1f s), %d bytes" % (a.out, n, n * 1152 / 44100, len(tag) + 417 * n))


if __name__ == "__main__":
    main()
