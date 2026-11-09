"""Symbolic run of a fragment program (from a gr3d trace) under the emulator's
timing model: which texel channels end up in each output channel.
  fpsym.py TRACE DRAW [--send A|B|C] [--L 6] [--order P|S|R]
"""
import argparse, struct, sys
sys.path.insert(0, r'C:\rea-work\gui')
from fpdump import load

CH = 'RGBA'


def fp20(v):
    e = (v >> 13) & 63
    m = v & 0x1fff
    if e == 0:
        return 0.0
    f = (1 + m / 8192) * 2 ** (e - 31)
    return -f if v & 0x80000 else f


def fx10(v):
    v &= 0x3ff
    if v & 0x200:
        v -= 1024
    return v / 256


# symbolic value: dict term->coef; term 1 = constant; 'G' = garbage
def const(c): return {1: c}
def add(a, b):
    r = dict(a)
    for k, v in b.items():
        r[k] = r.get(k, 0) + v
    return r
def scale(a, s): return {k: v * s for k, v in a.items()}
def isconst(a): return all(k == 1 for k in a)
def mul(a, b):
    if isconst(a): return scale(b, a.get(1, 0))
    if isconst(b): return scale(a, b.get(1, 0))
    return {'NL': 1}


import os
DBG = os.environ.get('DBG')
def short(t):
    def f(v): return '+'.join('%s*%.2f' % (k, c) for k, c in v.items())[:60]
    return f(t[1]) if t[0] == 'f' else '(' + f(t[1]) + ' ; ' + f(t[2]) + ')'
def run(path, n, send='A', L=6, order='P', D=(0, 1, 4.25, 4.5, 8), K=(0, 0, 0, 0, 0), REL=0, SWAP=0, SPILL=0, ASSIGN=None):
    regs, p, nb = load(path, n)
    N = (regs[0xe20] & 3) + 1
    Lp = max(N, L)
    ev = []
    for b in range(nb):
        base = (b // N) * Lp + b % N
        for u in range(5):
            ev.append((base + D[u], u, b))
    ev.sort()
    R = [('f', {'U': 1})] * 16           # (type, val) or ('x', lo, hi)
    alu_prev = [const(0)] * 4
    out = None
    texn = 0

    def rd_half(i, h):
        t = R[i]
        if t[0] == 'x':
            return t[2] if h else t[1]
        return {'G%d' % i: 1}

    def rd_full(i):
        t = R[i]
        if t[0] == 'f':
            return t[1]
        return {'G%d' % i: 1}

    def operand(op13, imm):
        sel = (op13 >> 6) & 127; h = bool(op13 & 32); m1 = op13 & 16; fx = op13 & 8; ng = op13 & 2; x2 = op13 & 1
        if 24 <= sel < 28:
            v = alu_prev[sel - 24]
        elif sel == 31:
            v = const(1.0 if h else 0.0)
        elif sel < 16:
            sel = (sel + REL * cur[0]) % (4 * N) if sel < 4 * N else sel
            v = rd_half(sel, h) if fx else rd_full(sel)
        elif 28 <= sel <= 30:
            raw = (imm >> (4 + 20 * (sel - 28))) & 0xfffff
            v = const(fx10(raw >> 10 if h else raw) if fx else fp20(raw))
        elif 32 <= sel < 64:
            raw = regs[0x820 + sel - 32] & 0xfffff
            v = const(fx10(raw >> 10 if h else raw) if fx else fp20(raw))
        else:
            v = {'S%d' % sel: 1}
        if m1: v = add(v, const(-1))
        if ng: v = scale(v, -1)
        if x2: v = scale(v, 2)
        return v

    cur = [0]
    spill = []
    nload = [0]
    fifo = [0]
    for t, u, b in ev:
        rb = 4 * ((b + K[u]) % N)
        cur[0] = rb
        if u == 0:
            ms = p['mfus'][b] if b < len(p['mfus']) else 0
            for k in range(ms & 3):
                a = ((ms >> 2) & 63) + k
                m = (p['mfu'][2 * a] << 32) | p['mfu'][2 * a + 1]
                for vi in range(4):
                    v = (m >> (7 * vi)) & 127
                    if (v >> 1) & 3:
                        R[rb + vi] = ('f', {'C%d.%d' % (v >> 3, vi): 1})
        elif u == 1:
            tw = p['tex'][b] if b < len(p['tex']) else 0
            if tw & (1 << 10):
                d = rb + (2 if tw & 32 else 0)
                tag = 'T%d' % b
                p0 = ('x', {tag + 'R': 1}, {tag + 'G': 1})
                p1 = ('x', {tag + 'B': 1}, {tag + 'A': 1})
                if SWAP == 1 and tw & 32 or SWAP == 2 and tw & 16 or SWAP == 3:
                    p0, p1 = p1, p0
                R[d], R[d + 1] = p0, p1
        elif u == 2:
            ps = p['pseq'][b] if b < len(p['pseq']) else 0
            if SPILL and ((ps >> 16) & 15) == 4 and not ps & (1 << 23):
                d = rb + (2 if ps & 2 else 0)
                src = None
                if SPILL == 1 and spill:
                    src = spill[-1]
                elif SPILL == 2:
                    st = [x for x in spill if x[0] & 1]
                    src = st[-1] if st else None
                elif SPILL == 3 and spill:
                    src = spill[nload[0] % len(spill)]
                elif SPILL == 4:
                    st = [x for x in spill if x[0] & 1]
                    src = st[nload[0] % len(st)] if st else None
                elif SPILL == 11:
                    b6, b8 = (ps >> 6) & 1, (ps >> 8) & 1
                    if not b8:
                        st = [x for x in spill if x[0] & 1]
                        src = st[-1] if st else None
                        reg = b6
                    else:
                        st = [x for x in spill if not x[0] & 1]
                        src = st[fifo[0]] if fifo[0] < len(st) else None
                        fifo[0] += 1
                        reg = 3 if b6 else 1
                    if src:
                        R[rb + (ps & 3)] = src[3][reg]
                    src = None
                elif SPILL == 10:
                    a = ASSIGN[nload[0]] if nload[0] < len(ASSIGN) else (-1, 0)
                    if 0 <= a[0] < len(spill):
                        R[rb + (ps & 3)] = spill[a[0]][3][a[1]]
                elif SPILL == 9:
                    k = ASSIGN[nload[0]] if nload[0] < len(ASSIGN) else -1
                    src = spill[k] if 0 <= k < len(spill) else None
                elif SPILL == 5:
                    st = [x for x in spill if x[0] & 1]
                    src = st[0] if st else None
                nload[0] += 1
                if DBG: print('LOAD  b%d t%.2f ps %08x row %d dst r%d' % (b, t, ps, rb//4, d))
                if src:
                    R[d], R[d + 1] = src[1], src[2]
            if ps & (1 << 23) and ((ps >> 16) & 15) == 1:
                d = rb + (2 if ps & 2 else 0)
                R[d] = ('x', {'dR': 1}, {'dG': 1})
                R[d + 1] = ('x', {'dB': 1}, {'dA': 1})
        elif u == 3:
            as_ = p['alus'][b] if b < len(p['alus']) else 0
            for k in range(as_ & 3):
                a = ((as_ >> 2) & 63) + k
                ws = [(p['alu'][a * 8 + s * 2] << 32) | p['alu'][a * 8 + s * 2 + 1] for s in range(4)]
                imm = any(28 <= ((x >> sh) & 127) <= 30 for x in ws[:3] for sh in (38, 25, 12))
                ns = 3 if imm else 4
                immw = (p['alu'][a * 8 + 7] << 32) | p['alu'][a * 8 + 6] if imm else ws[3]
                res = [None] * 4; ab = [None] * 4
                acc = {}
                slots = list(range(ns))
                newprev = list(alu_prev)
                for pas in (0, 1):
                    for j in slots:
                        x = ws[j]
                        snd, rcv = (x >> 61) & 1, (x >> 60) & 1
                        if res[j] is not None or (pas == 0 and not snd):
                            continue
                        A = operand((x >> 32) & 0x1fff, immw); B = operand((x >> 19) & 0x1fff, immw)
                        C = operand((x >> 6) & 0x1fff, immw)
                        rdf = x & 63
                        if rdf & 4:
                            basef = (x >> 6) & 0x1fff if rdf & 32 else (x >> 19) & 0x1fff
                            Dv = operand((basef & ~0x3f) | (32 if rdf & 16 else 0) | (8 if rdf & 1 else 0), immw)
                            if rdf & 8: Dv = add(Dv, const(-1))
                        else:
                            Dv = const(1)
                        if j in acc and send == 'C':
                            C = acc[j]
                        elif rcv:
                            C = newprev[j - 1] if j else alu_prev[3]
                        op = (x >> 62) & 3
                        v = add(mul(A, B), mul(C, Dv)) if op == 0 else {'OP%d' % op: 1}
                        if j in acc and send != 'C':
                            v = add(v, acc[j])
                        if (x >> 54) & 3:
                            v = {'CC': 1}
                        res[j] = v; ab[j] = mul(A, B)
                        newprev[j] = v
                        if snd and j in (1, 2, 3):
                            tgt = 0 if j < 3 else 2
                            acc[tgt] = add(acc.get(tgt, {}), ab[j] if send == 'A' else v)
                for j in slots:
                    x = ws[j]
                    dst = (x >> 47) & 127; wl, wh = (x >> 45) & 1, (x >> 46) & 1
                    if dst < 4 * N:
                        dst = (dst + REL * cur[0]) % (4 * N)
                    if dst < 16 and (wl or wh):
                        if wl and wh and not (x >> 59) & 1:
                            R[dst] = ('f', res[j])
                        else:
                            t0 = R[dst]
                            lo, hi = (t0[1], t0[2]) if t0[0] == 'x' else ({'G%d' % dst: 1}, {'G%d' % dst: 1})
                            if wl: lo = res[j]
                            if wh: hi = res[j]
                            R[dst] = ('x', lo, hi)
                alu_prev = newprev
        elif u == 4:
            dw = p['dw'][b] if b < len(p['dw']) else 0
            if SPILL and ((dw >> 2) & 15) == 4:
                d = rb + (2 if dw & 0x8000 else 0)
                spill.append((dw, R[d], R[d + 1], [R[rb + i] for i in range(4)]))
                if DBG: print('STORE b%d t%.2f dw %08x row %d src r%d:' % (b, t, dw, rb//4, d), short(R[d]), '|', short(R[d+1]))
            if dw & 1 and ((dw >> 2) & 15) == 1:
                d = rb + (2 if dw & 0x8000 else 0)
                out = (R[d], R[d + 1])
    return out


def summarize(v):
    # group texel terms by channel
    agg = {}
    for k, c in v.items():
        if isinstance(k, str) and k.startswith('T'):
            key = 'tex' + k[-1]
        elif k == 1:
            key = 'const'
        else:
            key = str(k)
        agg[key] = agg.get(key, 0) + c
    return ' '.join('%s:%.3f' % kv for kv in sorted(agg.items()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace'); ap.add_argument('draw', type=int)
    ap.add_argument('--send', default='A'); ap.add_argument('--L', type=int, default=6)
    a = ap.parse_args()
    out = run(a.trace, a.draw, a.send, a.L)
    if not out:
        print('no rt1 store'); return
    (t0, t1) = out
    names = []
    for t in (t0, t1):
        if t[0] == 'x':
            names += [t[1], t[2]]
        else:
            names += [{'FP20': 1}, {'FP20': 1}]
    for ch, v in zip(CH, names):
        print(ch, summarize(v))


main()

