import sys
sys.path.insert(0, r'C:\rea-work\gui')
from fpdump import load
for n in [0,1,98,150,151,153,174,175,184,321,1178,1343,1344,1345,1348]:
    regs, p, nb = load(sys.argv[1], n)
    print(n, 'nb', nb, 'e20=%08x 520=%08x 546=%08x 501=%08x 545=%08x' % (regs[0xe20], regs[0x520], regs[0x546], regs[0x501], regs[0x545]))
