"""Fit fragment-pipeline timing: time(b, unit) = (b // N) * L + (b % N) + d[unit].
  fpfit2.py TRACE N1 N2 ...
"""
import itertools, sys
sys.path.insert(0, r'C:\rea-work\gui')
from fpdump import load
from fpfit import events


def score(ev, N, L, d):
    def tm(e):
        b = e[0]
        return ((b // N) * L + (b % N) + d[e[1]], 'PMTAD'.index(e[1]))
    order = sorted(ev, key=tm)
    st, u, t = {}, 0, 0
    lastw = {}
    for e in ev:
        for r, _ in e[3]:
            lastw[r] = max(lastw.get(r, (-1, 0)), tm(e))
    for e in order:
        b, unit, rd, wr = e
        for r, ty in rd:
            if r not in st:
                u += 1
            elif st[r] != ty and unit != 'A':
                t += 1
        if unit == 'D':
            for r, _ in rd:
                if r in lastw and lastw[r] > tm(e):
                    u += 1
        for r, ty in wr:
            st[r] = ty
    return u, t


def main():
    a = sys.argv
    progs = []
    for n in a[2:]:
        regs, p, nb = load(a[1], int(n))
        ev, N = events(regs, p, nb)
        progs.append((int(n), ev, N))
    rng = [x / 2 for x in range(0, 15)]
    res = []
    dm = 0
    for L in (2, 3, 4, 5, 6, 'N'):
        for dt, da, dd in itertools.product(rng, rng, rng):
            d = {'P': da - 0.25, 'M': dm, 'T': dt, 'A': da, 'D': dd}
            tot = [score(ev, N, N if L == 'N' else max(L, N), d) for _, ev, N in progs]
            res.append((sum(x[0] * 10 + x[1] for x in tot), L, (dm, dt, da, dd), tot))
    res.sort(key=lambda r: r[0])
    for r in res[:30]:
        print(r)


main()
