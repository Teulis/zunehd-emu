"""Brute-force which spill store each spill load (PSEQ rt4) reads.
  fpassign.py TRACE DRAW L dt da dd"""
import itertools, sys
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
import functools
ns['load'] = functools.lru_cache(None)(ns['load'])
run = ns['run']
sys.argv, args = sys.argv[:1], sys.argv[1:]
src2 = open(r'C:\rea-work\gui\fpsearch2.py').read()
score_src = src2[src2.index('def chan_terms'):src2.index('def main')]
exec(score_src, ns)
score = ns['score']
tr, d = args[0], int(args[1])
L = int(args[2]); dt, da, dd = map(float, args[3:6])
nl = int(args[6]) if len(args) > 6 else 7
ns_ = int(args[7]) if len(args) > 7 else 5
res = []
for asg in itertools.product(range(-1, ns_), repeat=nl):
    out = run(tr, d, 'C', L, 'P', (0, dt, da - 0.25, da, dd), (0, 0, 0, 0, 0), 0, 0, 9, asg)
    s, g = score(out)
    res.append((s, asg, g))
res.sort(key=lambda r: r[0])
for r in res[:15]:
    print(r)
