"""Show symbolic output channels for given model params.
  fpshow.py TRACE DRAW L dt da dd SPILL [kt kd] [send]"""
import sys
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
a = sys.argv
L = int(a[3]); dt, da, dd = float(a[4]), float(a[5]), float(a[6]); sp = int(a[7])
kt = int(a[8]) if len(a) > 8 else 0; kd = int(a[9]) if len(a) > 9 else 0
send = a[10] if len(a) > 10 else 'C'
out = ns['run'](a[1], int(a[2]), send, L, 'P', (0, dt, da - 0.25, da, dd), (0, kt, kd, 0, kd), 0, 0, sp)
vals = []
for t in out:
    vals += [t[1], t[2]] if t[0] == 'x' else [{'FP20': 1}, {'FP20': 1}]
for ch, v in zip('RGBA', vals):
    items = sorted(((str(k), c) for k, c in v.items() if abs(c) > 1e-9), key=lambda kv: kv[0])
    print(ch, ' '.join('%s:%.3f' % kv for kv in items))
