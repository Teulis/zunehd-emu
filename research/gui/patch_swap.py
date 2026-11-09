s = open(r'C:\rea-work\gui\fpsym.py').read()
s = s.replace("REL=0):", "REL=0, SWAP=0):")
s = s.replace('''                R[d] = ('x', {tag + 'R': 1}, {tag + 'G': 1})
                R[d + 1] = ('x', {tag + 'B': 1}, {tag + 'A': 1})''', '''                p0 = ('x', {tag + 'R': 1}, {tag + 'G': 1})
                p1 = ('x', {tag + 'B': 1}, {tag + 'A': 1})
                if SWAP == 1 and tw & 32 or SWAP == 2 and tw & 16 or SWAP == 3:
                    p0, p1 = p1, p0
                R[d], R[d + 1] = p0, p1''')
open(r'C:\rea-work\gui\fpsym.py', 'w').write(s)
t = open(r'C:\rea-work\gui\fpsearch2.py').read()
t = t.replace("(0, kt, kd, 0, kd), REL))", "(0, kt, kd, 0, kd), REL, SWAP))")
t = t.replace("    REL = int(", "    SWAP = int(__import__('os').environ.get('SWAP', '0'))\n    REL = int(")
open(r'C:\rea-work\gui\fpsearch2.py', 'w').write(t)
print('ok')
