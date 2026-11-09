"""Search pipeline timing parameters so that the blur programs come out channel-clean.
  fpsearch.py TRACE DRAW1 [DRAW2 ...]
"""
import itertools, sys
sys.path.insert(0, r'C:\rea-work\gui')
import importlib.util
spec = importlib.util.spec_from_file_location('fpsym', r'C:\rea-work\gui\fpsym.py')
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
import functools
_ld = ns['load']
ns['load'] = functools.lru_cache(None)(_ld)
run = ns['run']


def chan_terms(t):
    if t[0] == 'x':
        return [t[1], t[2]]
    return [{'FP20': 1}, {'FP20': 1}]


def score(out):
    if not out:
        return 1e9, None
    vals = chan_terms(out[0]) + chan_terms(out[1])
    bad = 0.0
    goods = []
    for ch, v in zip('RGBA', vals):
        good = 0.0
        for k, c in v.items():
            if isinstance(k, str) and k.startswith('T') and k[-1] in 'RGBA':
                if k[-1] == ch:
                    good += c
                else:
                    bad += abs(c)
            elif k in (1, 'NL') or (isinstance(k, str) and k.startswith('d')):
                pass
            else:
                bad += abs(c) if c else 0
        goods.append(good)
    spread = max(goods) - min(goods)
    return bad * 10 + spread + (5 if min(goods) < 0.5 else 0), goods


def main():
    tr = sys.argv[1]
    draws = [int(x) for x in sys.argv[2:]]
    res = []
    vals = [x / 2 for x in range(0, 17)]
    for L in (4, 5, 6, 7, 8):
        for dt, da, dd in itertools.product([0.5, 1, 1.5, 2, 2.5, 3], vals, vals):
            dp = da - 0.25
            if not (dt < da and dp <= da and dd > da):
                continue
            for send in 'AC':
                tot, gs = 0, []
                for d in draws:
                    s, g = score(run(tr, d, send, L, 'P', (0, dt, dp, da, dd)))
                    tot += s
                    gs.append(g)
                res.append((tot, L, send, (dt, dp, da, dd), gs))
    res.sort(key=lambda r: r[0])
    for r in res[:20]:
        print(r)


main()

