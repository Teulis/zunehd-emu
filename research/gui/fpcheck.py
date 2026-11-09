"""Check every program shape with given timing: print output channel terms."""
import sys
src = open(r'C:\rea-work\gui\fpsym.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'fpsym', 'exec'), ns)
tr = sys.argv[1]
draws = [int(x) for x in sys.argv[2].split(',')]
D = eval(sys.argv[3]); L = int(sys.argv[4]); send = sys.argv[5]
asg = ((1, 0), (1, 1), (1, 0), (1, 1), (2, 1), (3, 1), (4, 3))
for d in draws:
    out = ns['run'](tr, d, send, L, 'P', D, (0,) * 5, 0, 0, 10, asg)
    if not out:
        print(d, 'no rt1 store'); continue
    vals = []
    for t in out:
        vals += [t[1], t[2]] if t[0] == 'x' else [{'FP20': 1}, {'FP20': 1}]
    s = []
    for ch, v in zip('RGBA', vals):
        s.append(ch + '[' + ' '.join('%s:%.2f' % kv for kv in sorted(((str(k), c) for k, c in v.items() if abs(c) > 1e-9)))[:70] + ']')
    print(d, ' '.join(s))
