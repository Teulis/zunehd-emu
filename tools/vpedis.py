"""Decode AR20 (Tegra 2 / AP16) vertex-processor VLIW instructions from a zunehd 3D trace,
using the bit layout in grate docs/vertex-shader-isa.md (instruction = 4 upload words,
word0 = bits 127..96 ... word3 = bits 31..0).
Usage: py -3 -I tools\\vpedis.py gr3d-trace.bin [--draws 0,1]"""
import struct, sys

VOP = ["NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE",
       "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "ARR", "MVA", "TXL",
       "PSH", "POP", "V28", "V29", "V30", "V31"]
SOP = ["NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CLA", "CLI", "RET",
       "LG2", "EX2", "SIN", "COS", "S17", "S18", "PUSHA", "POPA"] + [f"S{i}" for i in range(21, 32)]
SW = "xyzw"


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def swz(s):
    return "".join(SW[bits(s, 6 - 2 * i, 2)] for i in range(4))


def wm(m):
    return "".join(c if m & (8 >> i) else "*" for i, c in enumerate(SW))


def src(v, ty, reg, sw, neg, absb, attr, const):
    t = bits(v, ty, 2)
    r = bits(v, reg, 6)
    name = {1: f"r{r}", 2: f"a[{bits(v, 72, 4)}]", 3: f"c[{bits(v, 76, 10)}]", 0: f"?{r}"}[t]
    s = f"{name}.{swz(bits(v, sw, 8))}"
    if bits(v, absb, 1):
        s = f"|{s}|"
    if bits(v, neg, 1):
        s = "-" + s
    return s


def decode(w):
    v = (w[0] << 96) | (w[1] << 64) | (w[2] << 32) | w[3]
    rA = src(v, 55, 57, 63, 71, 117, 72, 76)
    rB = src(v, 38, 40, 46, 54, 118, 72, 76)
    rC = src(v, 21, 23, 29, 37, 119, 72, 76)
    vop, sop = bits(v, 86, 5), bits(v, 91, 5)
    vd, sd = bits(v, 111, 6), bits(v, 7, 6)
    vwm, swm = bits(v, 13, 4), bits(v, 17, 4)
    exp = bits(v, 2, 5)
    expv = bits(v, 126, 1)
    pred = ""
    if bits(v, 109, 1):
        pred = f" (check c{bits(v, 121, 1)}.{swz(bits(v, 98, 8))} gt{bits(v,108,1)} eq{bits(v,107,1)} lt{bits(v,106,1)})"
    sat = " SAT" if bits(v, 122, 1) else ""
    parts = []
    if vop:
        dst = f"r{vd}" if vd != 63 else "_"
        parts.append(f"{VOP[vop]}v {dst}.{wm(vwm)}, {rA}, {rB}, {rC}")
    if sop:
        parts.append(f"{SOP[sop]}s r{sd}.{wm(swm)}, {rC}")
    if exp != 31:
        parts.append(f"-> o[{exp}] ({'vector' if expv else 'scalar'})")
    eop = " END" if bits(v, 0, 1) else ""
    return ("; ".join(parts) or "NOP") + sat + pred + eop


def main():
    data = open(sys.argv[1], "rb").read()
    want = {int(x) for x in sys.argv[sys.argv.index("--draws") + 1].split(",")} if "--draws" in sys.argv else {0, 1}
    words, consts, cid, draw = [], {}, 0, 0
    for i in range(0, len(data) - 11, 12):
        c, o, val = struct.unpack_from("<III", data, i)
        if c & 0xffff != 0x60:
            continue
        o &= 0xfff
        if o == 0x205:
            words = []
        elif o == 0x206:
            words.append(val)
        elif o == 0x207:
            cid = val * 4
        elif o == 0x208:
            consts[cid] = val
            cid += 1
        elif o == 0x123:
            if draw in want:
                print(f"=== draw {draw}: {len(words)//4} instructions")
                for k in range(0, len(words) - 3, 4):
                    print(f"  {k//4}: {decode(words[k:k+4])}")
                nz = {k: struct.unpack('<f', struct.pack('<I', v))[0] for k, v in consts.items() if v}
                print("  consts:", ", ".join(f"c[{k//4}].{SW[k%4]}={f:g}" for k, f in sorted(nz.items())))
            draw += 1


if __name__ == "__main__":
    main()
