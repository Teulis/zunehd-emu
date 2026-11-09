# Boot zunehd under gdb, stop at the first execution of -Addr, dump regs + stack,
# and symbolize every module address found on the stack.
param([Parameter(Mandatory)] [string]$Addr, [string]$Image = 'NK', [int]$TimeoutSec = 90, [int]$Emmc = -1)
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$gdbScript = Join-Path $env:TEMP 'zbreak-run.gdb'
(Get-Content "$r\tools\break-stack.gdb" -Raw).Replace('BREAK_ADDR', $Addr) | Set-Content -Encoding ascii $gdbScript
Remove-Item "$r\zunehd-serial.log" -ErrorAction SilentlyContinue
$m = "zunehd,ext=$b\EXT.bin"; $extra = @()
if ($Emmc -ge 0) { $m += ",emmc-instance=$Emmc"; $extra = @('-drive', "if=sd,index=0,format=raw,file=$r\zunehd-emmc16.img") }
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList (@('-M', $m,
    '-m', '128M', '-kernel', "$b\$Image.bin", '-display', 'none', '-serial', "file:$r\zunehd-serial.log",
    '-monitor', 'none', '-s', '-S') + $extra) -PassThru -NoNewWindow -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep 1
$job = Start-Job { param($g, $s) & $g -q -batch -x $s 2>&1 } -ArgumentList 'C:\msys64\ucrt64\bin\gdb-multiarch.exe', $gdbScript
if (-not (Wait-Job $job -Timeout $TimeoutSec)) { "gdb timed out (breakpoint not hit in ${TimeoutSec}s)" }
$out = Receive-Job $job; Remove-Job $job -Force
if (-not $p.HasExited) { Stop-Process $p -Force }
$out | Select-String '===|^r\d' | ForEach-Object Line
"--- module addresses on stack (innermost first):"
($out | Select-String '^0x[0-9a-f]+:' | ForEach-Object Line) -join "`n" | py -3 -I "$r\tools\zsym.py"
