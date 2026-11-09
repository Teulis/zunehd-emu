# Boot zunehd and take a screenshot every -Every seconds for -Seconds seconds.
# Output: <repo>\shots\shot_NNN.png (+ summary of non-black frames).
#   powershell -NoProfile -File tools\zshots.ps1 [-Seconds 90] [-Every 2]
param([int]$Seconds = 90, [double]$Every = 2)
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"; $out = "$r\shots"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\shot_*" -ErrorAction SilentlyContinue
$qa = @('-M', "zunehd,ext=$b\EXT.bin", '-m', '128M', '-kernel', "$b\NK.bin", '-display', 'none',
        '-serial', "file:$r\zunehd-serial.log", '-monitor', 'tcp:127.0.0.1:4445,server,nowait',
        '-drive', "if=sd,index=0,format=raw,file=$r\zunehd-emmc16.img")
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList $qa -PassThru -NoNewWindow `
        -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep 1
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4445); $s = $c.GetStream()
$w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
$t0 = Get-Date; $n = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $f = ('{0}/shot_{1:d3}.ppm' -f ($out -replace '\\','/'), $n)
    $w.WriteLine("screendump $f"); $n++
    Start-Sleep -Milliseconds ([int]($Every * 1000))
    while ($s.DataAvailable) { [void]$s.ReadByte() }
}
$w.WriteLine('quit'); Start-Sleep 1; $c.Close()
if (-not $p.HasExited) { Stop-Process $p -Force }
Get-ChildItem "$out\shot_*.ppm" | Sort-Object Name | ForEach-Object {
    $png = $_.FullName -replace '\.ppm$', '.png'
    $res = py -3 -I "$r\tools\ppm2png.py" $_.FullName $png --scale 1
    Remove-Item $_.FullName
    if ($res -notmatch ' 0 non-zero') { $res }
}
