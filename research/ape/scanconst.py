import os, struct, sys
pats = {k: struct.pack('<I', k) for k in (0x0060cf05, 0xa2002200, 0x00603f07, 0x0063ff00, 0xa1000213, 0x00433f00)}
root = sys.argv[1]
for dp, dn, fn in os.walk(root):
    for f in fn:
        p = os.path.join(dp, f)
        try:
            d = open(p, 'rb').read()
        except Exception:
            continue
        hits = [hex(k) for k, b in pats.items() if b in d]
        if len(hits) >= 2:
            print(p, len(d), hits)
