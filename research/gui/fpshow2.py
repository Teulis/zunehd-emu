import sys, os
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
tr, d = sys.argv[1], int(sys.argv[2])
asg = eval(sys.argv[3])
D = eval(sys.argv[4]) if len(sys.argv) > 4 else (0, -2, -2.5, 0, 3.5)
L = int(sys.argv[5]) if len(sys.argv) > 5 else 8
send = sys.argv[6] if len(sys.argv) > 6 else 'A'
out = ns['run'](tr, d, send, L, 'P', D, (0, 0, 0, 0, 0), 0, 0, 10, asg)
vals = []
for t in out:
    vals += [t[1], t[2]] if t[0] == 'x' else [{'FP20': 1}, {'FP20': 1}]
for ch, v in zip('RGBA', vals):
    print(ch, ' '.join('%s:%.3f' % kv for kv in sorted(((str(k), c) for k, c in v.items() if abs(c) > 1e-9))))
