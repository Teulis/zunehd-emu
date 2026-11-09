"""Summarize a zunehd QEMU -d log without loading it all.

Prints: distinct MMIO accesses (first occurrence order, with counts), the
first exceptions with preceding context, and the tail.
usage: py -3 -I zlog.py [log] [--ctx N]
"""
import collections
import os
import sys

path = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'zunehd-mmio.log')
ctx = 12
if '--ctx' in sys.argv:
    ctx = int(sys.argv[sys.argv.index('--ctx') + 1])

mmio = collections.OrderedDict()
first_exc = []
recent = collections.deque(maxlen=ctx)
tail = collections.deque(maxlen=25)
exc_counts = collections.Counter()
n = 0
with open(path, 'r', errors='replace') as f:
    for line in f:
        n += 1
        line = line.rstrip('\n')
        if 'mode switch' in line:
            continue
        tail.append(line)
        if line.startswith('zunehd:'):
            key = line.split('->')[0].split('<-')[0].split('pc~')[0].strip()
            if key in mmio:
                mmio[key][0] += 1
                mmio[key][2] = line
            else:
                mmio[key] = [1, line, line]
        if line.startswith('Taking exception'):
            exc_counts[line] += 1
            if len(first_exc) < 4 and exc_counts[line] == 1:
                first_exc.append(list(recent) + [line])
        recent.append(line)

print(f'{n} lines')
print(f'--- {len(mmio)} distinct MMIO accesses (count, first, last value):')
for k, (c, first, last) in mmio.items():
    print(f'{c:8d}  {first}' + (f'   | last: {last.split()[-1]}' if c > 1 else ''))
print('--- exception counts:')
for k, c in exc_counts.most_common():
    print(f'{c:8d}  {k}')
for i, block in enumerate(first_exc):
    print(f'--- first occurrence #{i + 1} with context:')
    print('\n'.join(block))
print('--- tail:')
print('\n'.join(tail))
