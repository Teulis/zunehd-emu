set pagination off
set confirm off
set architecture arm
target remote 127.0.0.1:1234
hbreak *0xc092314c if $r0 == 0
continue
printf "=== NULL call: pc=0x%08x lr=0x%08x sp=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x\n", $pc, $lr, $sp, $r4, $r5, $r6
x/64wx $sp
kill
quit
