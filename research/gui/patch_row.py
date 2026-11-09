s = open(r'C:\rea-work\gui\fpsym.py').read()
s = s.replace('''                elif SPILL == 9:''', '''                elif SPILL == 10:
                    a = ASSIGN[nload[0]] if nload[0] < len(ASSIGN) else (-1, 0)
                    if 0 <= a[0] < len(spill):
                        R[rb + (ps & 3)] = spill[a[0]][3][a[1]]
                elif SPILL == 9:''')
s = s.replace('''                spill.append((dw, R[d], R[d + 1]))''', '''                spill.append((dw, R[d], R[d + 1], [R[rb + i] for i in range(4)]))''')
open(r'C:\rea-work\gui\fpsym.py', 'w').write(s)
print('ok')
