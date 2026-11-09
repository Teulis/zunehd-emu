"""Scan a gr3d trace: per draw, list RTs stored by DW and textures; filter by address.
  trscan.py TRACE [--addr 0xADDR] [--from N] [--to M]
"""
import struct, sys

def main():
    d = open(sys.argv[1], 'rb').read()
    a = sys.argv
    addr = int(a[a.index('--addr') + 1], 0) if '--addr' in a else None
    lo = int(a[a.index('--from') + 1]) if '--from' in a else 0
    hi = int(a[a.index('--to') + 1]) if '--to' in a else 1 << 30
    regs = [0] * 0x1000
    dw = []
    draws = 0
    for i in range(0, len(d) - 11, 12):
        c, off, val = struct.unpack_from('<III', d, i)
        if c & 0xffff != 0x60:
            continue
        off &= 0xfff
        regs[off] = val
        if off == 0xe22: dw = []
        elif off == 0x900: dw = dw[:val]
        elif off == 0x901: dw.append(val)
        if off != 0x123:
            continue
        rts = sorted({(w >> 2) & 15 for w in dw if w & 1})
        rtd = [(r, regs[0xe00 + r], (regs[0xe10 + r] >> 2) & 31, (regs[0xe10 + r] >> 8) & 0x1ffff) for r in rts]
        texs = [(k, regs[0x710 + k]) for k in range(16) if regs[0x710 + k] and k < 4]
        hit = addr is None or any(b <= addr < b + p * 512 for _, b, _, p in rtd) or any(t == addr for _, t in texs)
        if lo <= draws <= hi and hit:
            print(draws, 'n=%d' % ((val >> 20) + 1), 'prog pseq=%d' % len(dw),
                  'stores', ' '.join('rt%d=%08x/f%d/p%d' % x for x in rtd),
                  'tex', ' '.join('t%d=%08x' % x for x in texs), 'scis %08x %08x' % (regs[0x350], regs[0x351]))
        draws += 1

main()
