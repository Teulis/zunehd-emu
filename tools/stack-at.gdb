set pagination off
set confirm off
set architecture arm
target remote 127.0.0.1:1234
printf "=== pc=0x%08x lr=0x%08x sp=0x%08x\n", $pc, $lr, $sp
info registers r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12
x/96wx $sp
kill
quit
