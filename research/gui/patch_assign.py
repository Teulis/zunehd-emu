s = open(r'C:\rea-work\gui\fpsym.py').read()
s = s.replace("SPILL=0):", "SPILL=0, ASSIGN=None):")
s = s.replace('''                elif SPILL == 5:''', '''                elif SPILL == 9:
                    k = ASSIGN[nload[0]] if nload[0] < len(ASSIGN) else -1
                    src = spill[k] if 0 <= k < len(spill) else None
                elif SPILL == 5:''')
open(r'C:\rea-work\gui\fpsym.py', 'w').write(s)
print('ok')
