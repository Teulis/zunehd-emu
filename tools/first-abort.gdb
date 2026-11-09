set pagination off
set confirm off
set architecture arm
target remote 127.0.0.1:1234
# High-vector data abort entry. Hardware breakpoint so it survives MMU enable.
hbreak *0xffff0010
continue
printf "=== first data abort: faulting PC = 0x%08x (lr_abt-8), cpsr=0x%08x\n", $lr - 8, $cpsr
info registers
x/6i $lr - 16
kill
quit
