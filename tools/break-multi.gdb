set pagination off
set confirm off
set architecture arm
target remote 127.0.0.1:1234
# the next line is substituted by zbreakm.ps1 (one hbreak per address)
HBREAKS
define hook-stop
printf "=== stop pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x sp=0x%08x [sp]=0x%08x [sp+4]=0x%08x [sp+8]=0x%08x\n", $pc, $lr, $r0, $r1, $r2, $r3, $sp, *(unsigned int*)$sp, *(unsigned int*)($sp+4), *(unsigned int*)($sp+8)
end
# Up to N stops, each reported, then quit
CONTINUES
kill
quit
