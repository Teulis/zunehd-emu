"""Symbolize addresses against the XIP module map (extracted\\{NK,EXT}\\modules.txt).

usage: py -3 -I zsym.py 0xc008f004 0x8006522c ...   (or pipe text containing 0x... words)
"""
import re
import os
import sys

mods = []
for img in ('NK', 'EXT'):
    try:
        with open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'extracted', img, 'modules.txt'), encoding='utf-8-sig') as f:
            for line in f:
                m = re.match(r'^(\S+): vbase 0x([0-9A-F]+) entry 0x[0-9A-F]+ vsize 0x([0-9A-F]+)', line)
                if m:
                    mods.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), img))
    except FileNotFoundError:
        pass


def sym(a):
    # vsize of XIP modules can overlap the next module (data RVAs), so pick the
    # containing module with the highest base.
    best = None
    for base, size, name, img in mods:
        if base <= a < base + size and (best is None or base > best[0]):
            best = (base, name)
    return f'{best[1]}+0x{a - best[0]:x}' if best else None


words = sys.argv[1:] or re.findall(r'0x[0-9a-fA-F]{8}', sys.stdin.read())
seen = set()
for w in words:
    a = int(w, 16)
    s = sym(a)
    if s and (a, s) not in seen:
        seen.add((a, s))
        print(f'0x{a:08x}  {s}')
