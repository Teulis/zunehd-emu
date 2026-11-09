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
        nt = len({k[:-1] for k in v if isinstance(k, str) and k.startswith('T') and k[-1] == ch})
        bad += max(0, 7 - nt) * 2
        goods.append(good)
    spread = max(goods) - min(goods)
    return bad * 10 + spread + (5 if min(goods) < 0.5 else 0), goods


def main():
    tr = sys.argv[1]
    draws = [int(x) for x in sys.argv[2:]]
    res = []
    SPILL = int(__import__('os').environ.get('SPILL', '0'))
    SWAP = int(__import__('os').environ.get('SWAP', '0'))
    REL = int(__import__('os').environ.get('REL', '0'))
    for L in (4, 6, 8):
        for dt in [x / 2 for x in range(-4, 9)]:
            for da in [x / 2 for x in range(0, 17)]:
                for ddd in (0.5, 1.5, 2.5, 3.5, 4.5, 6):
                    dd = da + ddd
                    for kt in range(4):
                        for kd in range(4):
                            tot, gs = 0, []
                            for d in draws:
                                s, g = score(run(tr, d, 'A', L, 'P', (0, dt, da - 0.25, da, dd), (0, kt, kd, 0, kd), REL, SWAP, SPILL))
                                tot += s
                                gs.append(g)
                            res.append((tot, L, (dt, da, dd), (kt, kd), gs))
    res.sort(key=lambda r: r[0])
    for r in res[:25]:
        print(r)


main()
