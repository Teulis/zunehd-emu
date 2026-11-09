# Boot zunehd headless and run a timed list of monitor actions, e.g.
#   powershell -NoProfile -File tools\zseq.ps1 -Seconds 55 -Steps "25:tap 110 100; 32:key 1; 35:shot a"
# Actions: "tap X Y" (touch tap at screen pixels), "key K [ms]" (QEMU sendkey, e.g. 1, home, end),
#          "shot NAME" (screenshot -> shots\seq_NAME.png), any other text is sent to the monitor as-is.
# -Log writes zunehd-mmio.log (-d unimp).
param([int]$Seconds = 55, [string]$Steps = '', [switch]$Log, [string]$Machine = '',
      [string]$OutDir = '', [int]$MonPort = 4445, [string]$Qemu = '', [string]$EmmcImage = '',
      [string]$QemuArgs = '')
# -QemuArgs: extra QEMU arguments, space separated (e.g. "-audiodev wav,id=snd0,path=C:\x\out.wav").
# -Machine "avp=on" appends machine properties (e.g. keep the AVP image patches off).
# Parallel runs: -OutDir D (logs -> D\zunehd-*.log, shots -> D\shots), -MonPort, -Qemu (a private
# copy of qemu-system-arm.exe so rebuilds do not clash), -EmmcImage (own image; the guest writes to it).
# Also set ZUNEHD_USBIP to a free port (or leave it unset) when another QEMU is running.
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"
$od = if ($OutDir) { $OutDir } else { $r }; $out = if ($OutDir) { "$OutDir\shots" } else { "$r\shots" }
if (-not $Qemu) { $Qemu = "$r\qemu\build\qemu-system-arm.exe" }
if (-not $EmmcImage) { $EmmcImage = "$r\zunehd-emmc16.img" }
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\seq_*" -ErrorAction SilentlyContinue; Remove-Item "$od\zunehd-mon.txt" -ErrorAction SilentlyContinue
$mopt = "zunehd,ext=$b\EXT.bin"; if ($Machine) { $mopt += ",$Machine" }
$qa = @('-M', $mopt,'-m', '128M', '-kernel', "$b\NK.bin", '-display', 'none',
        '-serial', "file:$od\zunehd-serial.log", '-monitor', "tcp:127.0.0.1:$MonPort,server,nowait",
        '-drive', "if=sd,index=0,format=raw,file=$EmmcImage")
if ($QemuArgs) { $qa += ($QemuArgs -split '\s+' | Where-Object { $_ }) }
if ($Log) { $dflags = if ($env:ZSEQ_D) { $env:ZSEQ_D } else { 'unimp' }; $qa += @('-d', $dflags, '-D', "$od\zunehd-mmio.log"); if ($env:ZSEQ_DFILTER) { $qa += @('-dfilter', $env:ZSEQ_DFILTER) } }
$p = Start-Process -FilePath $Qemu -ArgumentList $qa -PassThru -NoNewWindow `
        -RedirectStandardError "$od\zunehd-stderr.log"
Start-Sleep 1
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $MonPort); $s = $c.GetStream()
$w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
function Mon($cmd, [switch]$Keep) { $w.WriteLine($cmd); Start-Sleep -Milliseconds 300; $buf = New-Object System.Collections.Generic.List[byte]; while ($s.DataAvailable) { $buf.Add([byte]$s.ReadByte()) }; if ($Keep) { Add-Content -Path "$od\zunehd-mon.txt" -Value ([Text.Encoding]::ASCII.GetString($buf.ToArray())) } }
$o = $out -replace '\\', '/'
$t0 = Get-Date
foreach ($step in ($Steps -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $at, $act = $step -split ':', 2
    while (((Get-Date) - $t0).TotalSeconds -lt [double]$at) { Start-Sleep -Milliseconds 100 }
    $a = $act -split '\s+'
    switch ($a[0]) {
        'tap'  { Mon 'mouse_move -2000 -2000'; Mon "mouse_move $($a[1]) $($a[2])"; Mon 'mouse_button 1'
                 Start-Sleep -Milliseconds 150; Mon 'mouse_button 0' }
        'key'  { $hold = if ($a.Count -gt 2) { $a[2] } else { 200 }; Mon "sendkey $($a[1]) $hold" }
        'shot' { Mon "screendump $o/seq_$($a[1]).ppm" }
        'mon'  { Mon ($act.Substring(4)) -Keep }
        default { Mon $act }
    }
}
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 200 }
$w.WriteLine('quit'); Start-Sleep 1; $c.Close()
if (-not $p.HasExited) { Stop-Process $p -Force }
Get-ChildItem "$out\seq_*.ppm" | Sort-Object Name | ForEach-Object {
    $png = $_.FullName -replace '\.ppm$', '.png'
    py -3 -I "$r\tools\ppm2png.py" $_.FullName $png --scale 1
    Remove-Item $_.FullName
}

