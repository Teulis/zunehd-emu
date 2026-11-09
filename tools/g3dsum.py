"""Summarize a zunehd 3D method trace (ZUNEHD_GR3D_TRACE) per draw.
Register meanings from grate's docs / tgr_3d.xml.h (AR20 = Tegra 2 3D).
Usage: py -3 -I tools\\g3dsum.py gr3d-trace.bin [--max N]"""
import struct, sys

PRIMS = ["points", "lines", "line_loop", "line_strip", "triangles", "tri_strip", "tri_fan", "?"]


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


def main():
    data = open(sys.argv[1], "rb").read()
    mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 40
    regs, written = [0] * 0x1000, [False] * 0x1000
    vp_words = pseq = alu = tex = dw = mfu = 0
    draws = 0
    progs = {}
    for i in range(0, len(data) - 11, 12):
        c, off, val = struct.unpack_from("<III", data, i)
        if c & 0xffff != 0x60:
            continue
        off &= 0xfff
        regs[off], written[off] = val, True
        if off == 0x205: vp_words = 0
        elif off == 0x206: vp_words += 1
        elif off == 0xe22: pseq = alu = tex = dw = mfu = 0
        elif off == 0x541: pseq += 1
        elif off == 0x604: mfu += 1
        elif off == 0x701: tex += 1
        elif off == 0x804: alu += 1
        elif off == 0x904: dw += 1
        if off != 0x123:
            continue
        key = (vp_words, pseq, alu, tex)
        progs[key] = progs.get(key, 0) + 1
        if draws < mx:
            prm, dp, io = regs[0x122], regs[0x123], regs[0x120]
            attrs = " ".join(f"a{k}@{regs[0x100+2*k]:08x}/t{regs[0x101+2*k]&15}s{(regs[0x101+2*k]>>4)&15}st{regs[0x101+2*k]>>8}"
                             for k in range(16) if io & (1 << (16 + k)))
            rts = " ".join(f"rt{k}={regs[0xe00+k]:08x}/f{(regs[0xe10+k]>>2)&31}p{(regs[0xe10+k]>>8)&0x1ffff}"
                           for k in range(16) if written[0xe00 + k] and regs[0xe00 + k])
            texs = " ".join(f"t{k}={regs[0x710+k]:08x}/f{(regs[0x720+2*k]>>8)&31}/{(regs[0x721+2*k]>>20)&0xfff}x{(regs[0x721+2*k]>>8)&0xfff}"
                            for k in range(16) if written[0x710 + k] and regs[0x710 + k])
            print(f"draw {draws:3d}: {PRIMS[(prm>>24)&7]} n={(dp>>20)+1} first={dp&0xfffff} idx={(prm>>28)&3}@{regs[0x121]:08x}"
                  f" | vp {vp_words//4} insn, fp pseq {pseq} alu {alu} tex {tex} dw {dw}")
            print(f"          {attrs}")
            print(f"          {rts}  {texs}  vpx {f32(regs[0x352]):.1f}+{f32(regs[0x355]):.1f} vpy {f32(regs[0x353]):.1f}+{f32(regs[0x356]):.1f}")
        draws += 1
    print(f"total draws {draws}; distinct (vp words, pseq, alu, tex) program shapes: {progs}")


if __name__ == "__main__":
    main()
