"""Fit fragment-pipeline unit timing to real programs (dataflow consistency).
  fpfit.py TRACE N1 [N2 ...]
For each draw: build per-batch events (unit, regs read/written with type f=fp20 / x=fx10),
row = batch % alu_buffer_size for MFU/TEX/PSEQ/DW; ALU regs absolute. For each
candidate unit delay set, simulate in time order and count reads of registers that
were never written (U) or written with the other type (T).
"""
import itertools, sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from fpdump import load  # noqa


def events(regs, p, n, nrows_override=None):
    N = (regs[0xe20] & 3) + 1 if nrows_override is None else nrows_override
    ev = []   # (batch, unit, reads[(reg,type)], writes[(reg,type)])
    for b in range(n):
        R = 4 * (b % N)
        ps = p['pseq'][b] if b < len(p['pseq']) else 0
        if ps & (1 << 23):
            d = R + (2 if ps & 2 else 0)
            ev.append((b, 'P', [], [(d, 'x'), (d + 1, 'x')]))
        ms = p['mfus'][b] if b < len(p['mfus']) else 0
        w = []
        for k in range(ms & 3):
            a = ((ms >> 2) & 63) + k
            m = (p['mfu'][2 * a] << 32) | p['mfu'][2 * a + 1]
            for vi in range(4):
                v = (m >> (7 * vi)) & 127
                op = (v >> 1) & 3
                if op:
                    w.append((R + vi, 'f' if op == 1 else 'x'))
        if w:
            ev.append((b, 'M', [], w))
        t = p['tex'][b] if b < len(p['tex']) else 0
        if t & (1 << 10):
            c = R + (2 if t & 16 else 0)
            d = R + (2 if t & 32 else 0)
            ev.append((b, 'T', [(c, 'f'), (c + 1, 'f')], [(d, 'x'), (d + 1, 'x')]))
        as_ = p['alus'][b] if b < len(p['alus']) else 0
        rd, wr = [], []
        for k in range(as_ & 3):
            a = ((as_ >> 2) & 63) + k
            ws = [(p['alu'][a * 8 + s * 2] << 32) | p['alu'][a * 8 + s * 2 + 1] for s in range(4)]
            imm = any(28 <= ((x >> sh) & 127) <= 30 for x in ws[:3] for sh in (38, 25, 12))
            for s in range(3 if imm else 4):
                x = ws[s]
                for sh in (32, 19, 6):
                    o = (x >> sh) & 0x1fff
                    sel = (o >> 6) & 127
                    if sel < 16:
                        rd.append((sel, 'x' if o & 8 else 'f'))
                dst = (x >> 47) & 127
                wl, wh = (x >> 45) & 1, (x >> 46) & 1
                if dst < 16 and (wl or wh):
                    wr.append((dst, 'f' if (wl and wh and not (x >> 59) & 1) else 'x'))
        if rd or wr:
            ev.append((b, 'A', rd, wr))
        dw = p['dw'][b] if b < len(p['dw']) else 0
        if dw & 1 and (dw >> 2) & 15 == 1:
            c = R + (2 if dw & 0x8000 else 0)
            ev.append((b, 'D', [(c, 'x'), (c + 1, 'x')], []))
    return ev, N


def score(ev, delay):
    order = sorted(ev, key=lambda e: (e[0] + delay[e[1]], 'PMTAD'.index(e[1])))
    st = {}
    u = t = 0
    for b, unit, rd, wr in order:
        for r, ty in rd:
            if r not in st:
                u += 1
            elif st[r] != ty and not (unit == 'A'):
                t += 1
        for r, ty in wr:
            st[r] = ty
    return u, t


def main():
    a = sys.argv
    progs = []
    for n in a[2:]:
        regs, p, nb = load(a[1], int(n))
        ev, N = events(regs, p, nb)
        progs.append((int(n), ev, N, nb))
        print('draw', n, 'batches', nb, 'alu_buffer_size', N, 'e20=%08x' % regs[0xe20])
    res = []
    rng = [x / 2 for x in range(0, 17)]
    for dm, dt, da, dd in itertools.product(rng, rng, rng, rng):
        delay = {'P': 0, 'M': dm, 'T': dt, 'A': da, 'D': dd}
        tot = [score(ev, delay) for _, ev, _, _ in progs]
        res.append((sum(x[0] * 10 + x[1] for x in tot), (dm, dt, da, dd), tot))
    res.sort()
    for r in res[:25]:
        print(r)


if __name__ == '__main__':
    main()

