set pagination off
set confirm off
set architecture arm
target remote 127.0.0.1:1234
hbreak *0x80032644
continue
printf "=== hit 1: lr=0x%08x r1(module)=0x%x r2(inst)=0x%x r3(off)=0x%x\n", $lr, $r1, $r2, $r3
continue
printf "=== hit 2: lr=0x%08x r1(module)=0x%x r2(inst)=0x%x r3(off)=0x%x\n", $lr, $r1, $r2, $r3
kill
quit
