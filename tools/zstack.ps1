# Boot for N seconds, then attach gdb and dump registers + stack; list candidate
# return addresses that fall inside nk.exe .text (0x80015000-0x8005BE00) or kernel.dll.
param([int]$Seconds = 6, [string]$Image = 'NK')
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList @('-M', "zunehd,ext=$b\EXT.bin",
    '-m', '128M', '-kernel', "$b\$Image.bin", '-display', 'none', '-serial', "file:$r\zunehd-serial.log",
    '-monitor', 'none', '-s') -PassThru -NoNewWindow -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep $Seconds
$out = & C:\msys64\ucrt64\bin\gdb-multiarch.exe -q -batch -x "$r\tools\stack-at.gdb" 2>&1
if (-not $p.HasExited) { Stop-Process $p -Force }
$out | Select-String '===|^r\d' | ForEach-Object Line
$words = $out | Select-String '^0x[0-9a-f]+:' | ForEach-Object { ($_.Line -split '\s+')[1..4] }
"--- code addresses on stack (nk.exe .text / kernel.dll):"
$words | Where-Object { $_ -match '^0x8' } | ForEach-Object { [Convert]::ToUInt32($_.Substring(2), 16) } |
    Where-Object { ($_ -ge 0x80015000 -and $_ -lt 0x8005BE00) -or ($_ -ge 0x80060000 -and $_ -lt 0x80098000) } |
    ForEach-Object { '0x{0:x8}' -f $_ }
