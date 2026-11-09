# Run to idle, then walk every CE6 process and thread and print each thread's saved context
# plus code-looking words from its stack. Layout (fw 4.5, found 2026-11-07):
#   process list head 0x80BEE010 (DLIST: +0 next); PROCESS: +0x0C id, +0x10 thread-list head, +0x20 name ptr
#   THREAD: +0x00 next (list through PROCESS+0x10), +0x3C id, saved ctx +0x60 psr, +0x64.. r0-r12,
#           +0x98 sp, +0x9C lr, +0xA0 pc
hbreak *0x8002b944
ignore 1 IDLE_SKIPS
continue
delete
set $head = 0x80bee010
set $pp = *(unsigned int*)$head
set $np = 0
while $pp != $head && $np < 40
  set $prc = $pp
  printf "PROC %08x id %08x name ", $prc, *(unsigned int*)($prc+0x0c)
  x/ws *(unsigned int*)($prc+0x20)
  set $th = *(unsigned int*)($prc+0x10)
  set $nt = 0
  while $th != ($prc+0x10) && $th != 0 && $nt < 40
    set $ksp = *(unsigned int*)($th+0x98)
    printf "THR %08x id %08x pc %08x lr %08x sp %08x\n", $th, *(unsigned int*)($th+0x3c), *(unsigned int*)($th+0xa0), *(unsigned int*)($th+0x9c), $ksp
    set $a = $ksp
    set $k = 0
    while $a < $ksp + 0x800 && $k < 24
      set $v = *(unsigned int*)$a
      if ($v >= 0x80000000 && $v < 0x80c00000) || ($v >= 0xc0000000 && $v < 0xc1000000) || ($v >= 0x40000000 && $v < 0x42000000) || ($v >= 0x10000 && $v < 0x100000)
        printf "  STK %08x\n", $v
        set $k = $k + 1
      end
      set $a = $a + 4
    end
    set $th = *(unsigned int*)$th
    set $nt = $nt + 1
  end
  set $pp = *(unsigned int*)$pp
  set $np = $np + 1
end
kill
