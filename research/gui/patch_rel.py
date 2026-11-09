s = open(r'C:\rea-work\gui\fpsym.py').read()
s = s.replace("K=(0, 0, 0, 0, 0)):", "K=(0, 0, 0, 0, 0), REL=0):")
s = s.replace('''        elif sel < 16:
            v = rd_half(sel, h) if fx else rd_full(sel)''', '''        elif sel < 16:
            sel = (sel + REL * cur[0]) % (4 * N) if sel < 4 * N else sel
            v = rd_half(sel, h) if fx else rd_full(sel)''')
s = s.replace('''                    if dst < 16 and (wl or wh):''', '''                    if dst < 4 * N:
                        dst = (dst + REL * cur[0]) % (4 * N)
                    if dst < 16 and (wl or wh):''')
s = s.replace('''    for t, u, b in ev:
        rb = 4 * ((b + K[u]) % N)''', '''    cur = [0]
    for t, u, b in ev:
        rb = 4 * ((b + K[u]) % N)
        cur[0] = rb''')
open(r'C:\rea-work\gui\fpsym.py', 'w').write(s)
t = open(r'C:\rea-work\gui\fpsearch2.py').read()
t = t.replace("(0, kt, kd, 0, kd)))", "(0, kt, kd, 0, kd), REL))")
t = t.replace("    for L in (4, 6, 8):", "    REL = int(__import__('os').environ.get('REL', '0'))\n    for L in (4, 6, 8):")
open(r'C:\rea-work\gui\fpsearch2.py', 'w').write(t)
print('ok')
