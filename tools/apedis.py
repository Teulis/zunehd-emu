"""List the command programs of the AVP's audio vector engine (MMIO 0x6000E000, unmodelled).

The MP3 decoder (AVP library at 0x06490000) builds programs in IRAM and kicks them with
0x6000E074 = program address, 0x6000E070 = 0xA0000000 | (words - 1); reading 0x6000E078
(FUN_0649baa0) is presumably the wait/fence. Instruction layout (inferred, consistent over
all 604 instructions of a 0xD2A-word program):
  word0 = 0x00FFMMLL: one operand word per set bit of MM ([15:8]) plus one per set bit of
          FF & 0x23 ([23:16]: 0x20, 0x02, 0x01) plus one per set bit of LL & 7; the
          instruction ends with a control word whose top nibble is 8 or A (0x82001000,
          0xA2002200, 0xA1000088, ...).
Usage: py -3 -I tools/apedis.py iram.bin 0x4000cc10 0xd2a   (IRAM dump based at 0x40000000)
"""
import collections
import struct
import sys


def split(words):
    ins, cur = [], []
    for x in words:
        cur.append(x)
        if (x >> 28) in (0x8, 0xA):
            ins.append(cur)
            cur = []
    return ins, cur


def main():
    d = open(sys.argv[1], "rb").read()
    addr = int(sys.argv[2], 0)
    n = int(sys.argv[3], 0)
    words = struct.unpack_from("<%dI" % n, d, addr - 0x40000000)
    ins, rest = split(words)
    a = addr
    for i in ins:
        op = i[0]
        exp = 2 + bin((op >> 8) & 0xFF).count("1") + bin((op >> 16) & 0x23).count("1") \
            + bin(op & 7).count("1")
        print("%08x: %s%s" % (a, " ".join("%08x" % x for x in i),
                              "" if exp == len(i) else "   <len %d, expected %d>" % (len(i), exp)))
        a += 4 * len(i)
    if rest:
        print("leftover:", " ".join("%08x" % x for x in rest))
    c = collections.Counter(i[0] for i in ins)
    print("\n%d instructions; opcodes:" % len(ins),
          ", ".join("%08x x%d" % kv for kv in sorted(c.items())))


if __name__ == "__main__":
    main()
