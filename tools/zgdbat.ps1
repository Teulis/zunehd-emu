# Boot zunehd normally (gdb stub enabled, not halted), wait -AfterSec seconds of wall time,
# then attach gdb and run a command file (it should end with "kill").
#   powershell -NoProfile -File tools\zgdbat.ps1 -Script my.gdb -AfterSec 200 [-TimeoutSec 120]
# Output: gdb stdout; serial goes to zunehd-serial.log.
param([Parameter(Mandatory)] [string]$Script, [int]$AfterSec = 120, [int]$TimeoutSec = 120,
      [switch]$NoEmmc, [string]$Image = 'NK', [string]$Machine = '')
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$hdr = "set pagination off`nset confirm off`nset architecture arm`ntarget remote 127.0.0.1:1234`n"
$gs = Join-Path $env:TEMP 'zgdbat-run.gdb'
($hdr + (Get-Content $Script -Raw)) | Set-Content -Encoding ascii $gs
Remove-Item "$r\zunehd-serial.log" -ErrorAction SilentlyContinue
$mo = "zunehd,ext=$b\EXT.bin"; if ($Machine) { $mo += ",$Machine" }
$qa = @('-M', $mo, '-m', '128M', '-kernel', "$b\$Image.bin", '-display', 'none',
        '-serial', "file:$r\zunehd-serial.log", '-monitor', 'none', '-s')
if (-not $NoEmmc) { $qa += @('-drive', "if=sd,index=0,format=raw,file=$r\zunehd-emmc16.img") }
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList $qa -PassThru -NoNewWindow `
        -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep $AfterSec
$gout = Join-Path $env:TEMP 'zgdbat.out'
$g = Start-Process -FilePath 'C:\msys64\ucrt64\bin\gdb-multiarch.exe' -ArgumentList @('-q', '-batch', '-x', $gs) `
        -PassThru -NoNewWindow -RedirectStandardOutput $gout -RedirectStandardError "$gout.err"
if (-not $g.WaitForExit($TimeoutSec * 1000)) {
    "(timeout after ${TimeoutSec}s)"
    $null = $g.WaitForExit(1); if (-not $g.HasExited) { Stop-Process $g -Force }
}
if (-not $p.HasExited) { Stop-Process $p -Force }
Get-Content $gout -ErrorAction SilentlyContinue

