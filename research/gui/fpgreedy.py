"""Greedy search: for each spill load choose (store index, register in stored row).
  fpgreedy.py TRACE DRAW L dt da dd NLOADS NSTORES [send]"""
import sys, functools, itertools
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
ns['load'] = functools.lru_cache(None)(ns['load'])
src2 = open(r'C:\rea-work\gui\fpsearch2.py').read()
exec(src2[src2.index('def chan_terms'):src2.index('def main')], ns)
run, score = ns['run'], ns['score']
a = sys.argv[1:]
tr, d, L = a[0], int(a[1]), int(a[2])
dt, da, dd = map(float, a[3:6])
nl, nst = int(a[6]), int(a[7])
send = a[8] if len(a) > 8 else 'C'
DP = float(a[9]) if len(a) > 9 else da - 0.25
opts = [(-1, 0)] + [(s, i) for s in range(nst) for i in range(4)]


def ev(asg):
    out = run(tr, d, send, L, 'P', (0, dt, DP, da, dd), (0, 0, 0, 0, 0), 0, 0, 10, tuple(asg))
    return score(out)


best = [(-1, 0)] * nl
bs = ev(best)[0]
for it in range(6):
    changed = False
    for k in range(nl):
        for o in opts:
            t = list(best); t[k] = o
            s = ev(t)[0]
            if s < bs - 1e-9:
                bs, best, changed = s, t, True
    # pairwise refinement
    for k1, k2 in itertools.combinations(range(nl), 2):
        for o1 in opts:
            for o2 in opts:
                t = list(best); t[k1] = o1; t[k2] = o2
                s = ev(t)[0]
                if s < bs - 1e-9:
                    bs, best, changed = s, t, True
    print(it, bs, best, ev(best)[1], flush=True)
    if not changed:
        break
