"""Extract the fragment program of draw N from a gr3d trace and list per-batch
register reads/writes for each unit (dataflow view).
  fpdump.py TRACE N
"""
import struct, sys


def load(path, want):
    d = open(path, 'rb').read()
    regs = [0] * 0x1000
    st = {k: [] for k in ('pseq', 'mfus', 'mfu', 'tex', 'alus', 'alu', 'dw')}
    ptr = {k: 0 for k in st}
    ports = {0x540: 'pseq', 0x600: 'mfus', 0x603: 'mfu', 0x700: 'tex', 0x800: 'alus', 0x803: 'alu', 0x900: 'dw'}
    draws = 0
    for i in range(0, len(d) - 11, 12):
        c, off, val = struct.unpack_from('<III', d, i)
        if c & 0xffff != 0x60:
            continue
        off &= 0xfff
        regs[off] = val
        if off == 0xe22:
            for k in ptr: ptr[k] = 0
        elif off in ports:
            ptr[ports[off]] = val
        elif off - 1 in ports:
            k = ports[off - 1]
            a = st[k]
            while len(a) <= ptr[k]: a.append(0)
            a[ptr[k]] = val
            ptr[k] += 1
        elif off == 0x123:
            if draws == want:
                n = ptr['pseq']
                return regs, {k: v[:] for k, v in st.items()}, n
            draws += 1
    raise SystemExit('draw not found')


def opnd(op13):
    sel = (op13 >> 6) & 127
    h = 'h' if op13 & 32 else 'l'
    fx = bool(op13 & 8)
    return sel, (h if fx else 'f')


def regname(sel):
    if sel < 16: return 'r%d' % sel
    if sel < 24: return 'g%d' % (sel - 16)
    if sel < 28: return 'alu%d' % (sel - 24)
    if sel < 31: return 'imm%d' % (sel - 28)
    if sel == 31: return '#'
    if sel < 64: return 'u%d' % (sel - 32)
    if sel < 72: return 'cr%d' % (sel - 64)
    return {72: 'posx', 73: 'posy', 74: 'cov', 75: 'face', 76: 'kill'}.get(sel, '?%d' % sel)


def main():
    regs, p, n = load(sys.argv[1], int(sys.argv[2]))
    print('batches', n)
    for b in range(n):
        out = []
        ps = p['pseq'][b] if b < len(p['pseq']) else 0
        out.append('PSEQ %08x' % ps)
        ms = p['mfus'][b] if b < len(p['mfus']) else 0
        for k in range(ms & 3):
            a = ((ms >> 2) & 63) + k
            hi, lo = p['mfu'][2 * a], p['mfu'][2 * a + 1]
            m = (hi << 32) | lo
            vs = []
            for vi in range(4):
                v = (m >> (7 * vi)) & 127
                vs.append('t%d%s' % (v >> 3, ['-', 'f', 'x', '?'][(v >> 1) & 3]) if (v >> 1) & 3 else '-')
            out.append('MFU[%d] reg=%d sfu=%d mul0=%03x mul1=%03x b28=%x ipl=%s' % (a, m >> 58, (m >> 54) & 15,
                       (m >> 32) & 0x7ff, (m >> 43) & 0x7ff, (m >> 28) & 15, ','.join(vs)))
        t = p['tex'][b] if b < len(p['tex']) else 0
        if t & (1 << 10):
            out.append('TEX %08x s%d crd r%d dst r%d' % (t, t & 15, 2 if t & 16 else 0, 2 if t & 32 else 0))
        as_ = p['alus'][b] if b < len(p['alus']) else 0
        for k in range(as_ & 3):
            a = ((as_ >> 2) & 63) + k
            ws = [(p['alu'][a * 8 + s * 2] << 32) | p['alu'][a * 8 + s * 2 + 1] for s in range(4)]
            imm = any(28 <= ((w >> sh) & 127) <= 30 for w in ws[:3] for sh in (38, 25, 12))
            for s in range(3 if imm else 4):
                w = ws[s]
                dst = (w >> 47) & 127
                if dst == 31 and not (w >> 61) & 3:
                    continue
                wl, wh = (w >> 45) & 1, (w >> 46) & 1
                rd = []
                for sh in (32, 19, 6):
                    sel, f = opnd((w >> sh) & 0x1fff)
                    if sel != 31: rd.append(regname(sel) + '.' + f)
                flags = ('R' if w >> 60 & 1 else '') + ('S' if w >> 61 & 1 else '') + ('A' if w >> 59 & 1 else '')
                out.append('ALU%d %s.%s%s <- %s %s' % (s, regname(dst), 'l' if wl else '*', 'h' if wh else '*',
                                                   ' '.join(rd), flags))
        dw = p['dw'][b] if b < len(p['dw']) else 0
        if dw:
            out.append('DW %08x%s rt%d src r%d' % (dw, '' if dw & 1 else '(off)', (dw >> 2) & 15, 2 if dw & 0x8000 else 0))
        print('%2d: ' % b + ' | '.join(out))


if __name__ == "__main__":
    main()

