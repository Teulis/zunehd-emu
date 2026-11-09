# Boot zunehd under gdb with several breakpoints; print the first -Stops hits (pc/lr/r0-r3)
# with symbolized pc and lr. Breakpoints persist (each hit continues to the next).
param([Parameter(Mandatory)] [string[]]$Addr, [int]$Stops = 6, [string]$Image = 'NK', [int]$TimeoutSec = 90,
      [int]$Emmc = -1, [int]$EmmcSlot = -1, [string]$Machine = '')
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$gdbScript = Join-Path $env:TEMP 'zbreakm-run.gdb'
# powershell -File may hand us one comma-joined string of signed decimals; normalize to hex.
$list = foreach ($tok in (($Addr -join ',') -split ',')) {
    $t = $tok.Trim(); if (-not $t) { continue }
    if ($t -match '^0x') { $t } else { $v = [int64]$t; if ($v -lt 0) { $v += 4294967296 }; '0x{0:x8}' -f $v }
}
$hb = ($list | ForEach-Object { "hbreak *$_" }) -join "`n"
$cont = (1..$Stops | ForEach-Object { 'continue' }) -join "`n"
(Get-Content "$r\tools\break-multi.gdb" -Raw).Replace('HBREAKS', $hb).Replace('CONTINUES', $cont) |
    Set-Content -Encoding ascii $gdbScript
Remove-Item "$r\zunehd-serial.log" -ErrorAction SilentlyContinue
$m = "zunehd,ext=$b\EXT.bin"; if ($Machine) { $m += ",$Machine" }; $extra = @()
if ($Emmc -ge 0) { $m += ",emmc-instance=$Emmc"   # card always on the HSMMC (if=sd,index=0)
    $extra = @('-drive', "if=sd,index=0,format=raw,file=$r\zunehd-emmc16.img") }
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList (@('-M', $m,
    '-m', '128M', '-kernel', "$b\$Image.bin", '-display', 'none', '-serial', "file:$r\zunehd-serial.log",
    '-monitor', 'none', '-s', '-S') + $extra) -PassThru -NoNewWindow -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep 1
# gdb block-buffers stdout when it is not a tty: send it to a file and end the session by
# killing QEMU on timeout (gdb then exits and flushes).
$gout = Join-Path $env:TEMP 'zbreakm-gdb.out'
$g = Start-Process -FilePath 'C:\msys64\ucrt64\bin\gdb-multiarch.exe' -ArgumentList @('-q', '-batch', '-x', $gdbScript) `
        -PassThru -NoNewWindow -RedirectStandardOutput $gout -RedirectStandardError "$gout.err"
if (-not $g.WaitForExit($TimeoutSec * 1000)) {
    "(timeout after ${TimeoutSec}s; fewer than $Stops hits)"
    if (-not $p.HasExited) { Stop-Process $p -Force }
    $null = $g.WaitForExit(10000); if (-not $g.HasExited) { Stop-Process $g -Force }
}
if (-not $p.HasExited) { Stop-Process $p -Force }
$out = Get-Content $gout -ErrorAction SilentlyContinue
foreach ($l in ($out | Select-String '^=== stop' | ForEach-Object Line)) {
    $pc = [regex]::Match($l, 'pc=(0x[0-9a-f]+)').Groups[1].Value
    $lr = [regex]::Match($l, 'lr=(0x[0-9a-f]+)').Groups[1].Value
    $syms = (py -3 -I "$r\tools\zsym.py" $pc $lr) -join ' | '
    "$l`n      $syms"
}

