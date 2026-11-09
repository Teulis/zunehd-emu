import sys, functools
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
ns['load'] = functools.lru_cache(None)(ns['load'])
src2 = open(r'C:\rea-work\gui\fpsearch2.py').read()
exec(src2[src2.index('def chan_terms'):src2.index('def main')], ns)
run, score = ns['run'], ns['score']
base = [(1, 0), (1, 1), (1, 0), (1, 1), (2, 1), (3, 1), (4, 3)]
D = (0, -3.5, -2.5, 0, 3.5)
for d in (3130, 3132):
    for k in range(7):
        ok = []
        for s in range(-1, 5):
            for r in range(4):
                t = list(base); t[k] = (s, r)
                if score(run('tr_hold.bin', d, 'A', 8, 'P', D, (0,) * 5, 0, 0, 10, tuple(t)))[0] < 1e-6:
                    ok.append((s, r))
        print(d, 'load', k, ok)
