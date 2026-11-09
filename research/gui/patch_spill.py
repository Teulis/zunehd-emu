s = open(r'C:\rea-work\gui\fpsym.py').read()
s = s.replace("REL=0, SWAP=0):", "REL=0, SWAP=0, SPILL=0):")
s = s.replace('''    cur = [0]
    for t, u, b in ev:''', '''    cur = [0]
    spill = []
    nload = [0]
    for t, u, b in ev:''')
s = s.replace('''            if ps & (1 << 23) and ((ps >> 16) & 15) == 1:''', '''            if SPILL and ((ps >> 16) & 15) == 4 and not ps & (1 << 23):
                d = rb + (2 if ps & 2 else 0)
                src = None
                if SPILL == 1 and spill:
                    src = spill[-1]
                elif SPILL == 2:
                    st = [x for x in spill if x[0] & 1]
                    src = st[-1] if st else None
                elif SPILL == 3 and spill:
                    src = spill[nload[0] % len(spill)]
                elif SPILL == 4:
                    st = [x for x in spill if x[0] & 1]
                    src = st[nload[0] % len(st)] if st else None
                elif SPILL == 5:
                    st = [x for x in spill if x[0] & 1]
                    src = st[0] if st else None
                nload[0] += 1
                if src:
                    R[d], R[d + 1] = src[1], src[2]
            if ps & (1 << 23) and ((ps >> 16) & 15) == 1:''')
s = s.replace('''            if dw & 1 and ((dw >> 2) & 15) == 1:''', '''            if SPILL and ((dw >> 2) & 15) == 4:
                d = rb + (2 if dw & 0x8000 else 0)
                spill.append((dw, R[d], R[d + 1]))
            if dw & 1 and ((dw >> 2) & 15) == 1:''')
open(r'C:\rea-work\gui\fpsym.py', 'w').write(s)
t = open(r'C:\rea-work\gui\fpsearch2.py').read()
t = t.replace("REL, SWAP))", "REL, SWAP, SPILL))")
t = t.replace("    SWAP = int(", "    SPILL = int(__import__('os').environ.get('SPILL', '0'))\n    SWAP = int(")
open(r'C:\rea-work\gui\fpsearch2.py', 'w').write(t)
print('ok')
