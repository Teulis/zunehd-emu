s = open(r'C:\rea-work\gui\fpsym.py').read()
s = s.replace('''                elif SPILL == 10:''', '''                elif SPILL == 11:
                    b6, b8 = (ps >> 6) & 1, (ps >> 8) & 1
                    if not b8:
                        st = [x for x in spill if x[0] & 1]
                        src = st[-1] if st else None
                        reg = b6
                    else:
                        st = [x for x in spill if not x[0] & 1]
                        src = st[fifo[0]] if fifo[0] < len(st) else None
                        fifo[0] += 1
                        reg = 3 if b6 else 1
                    if src:
                        R[rb + (ps & 3)] = src[3][reg]
                    src = None
                elif SPILL == 10:''')
s = s.replace('''    nload = [0]''', '''    nload = [0]
    fifo = [0]''')
open(r'C:\rea-work\gui\fpsym.py', 'w').write(s)
print('ok')
