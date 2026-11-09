# Boot zunehd headless, tap the touch screen through the QEMU monitor, take screenshots.
#   powershell -NoProfile -File tools\ztap.ps1 -At 47 -X 110 -Y 100 [-Seconds 58] [-Drag 0,0] [-Log]
# -X/-Y are screen pixels (272x480). -Drag dx,dy turns the tap into a drag of that many pixels.
# Screenshots: shots\tap_before.png and shots\tap_NN.png (one per second after the tap).
# -Log writes the MMIO/I2C log (zunehd-mmio.log, -d unimp) so touch reads can be checked.
param([double]$At = 47, [int]$X = 136, [int]$Y = 240, [int]$Seconds = 58,
      [int[]]$Drag = @(0, 0), [switch]$Log, [switch]$DumpRam)
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"; $out = "$r\shots"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\tap_*" -ErrorAction SilentlyContinue
$qa = @('-M', "zunehd,ext=$b\EXT.bin", '-m', '128M', '-kernel', "$b\NK.bin", '-display', 'none',
        '-serial', "file:$r\zunehd-serial.log", '-monitor', 'tcp:127.0.0.1:4445,server,nowait',
        '-drive', "if=sd,index=0,format=raw,file=$r\zunehd-emmc16.img")
if ($Log) { $qa += @('-d', 'unimp', '-D', "$r\zunehd-mmio.log") }
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList $qa -PassThru -NoNewWindow `
        -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep 1
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4445); $s = $c.GetStream()
$w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
function Mon($cmd) { $w.WriteLine($cmd); Start-Sleep -Milliseconds 100; while ($s.DataAvailable) { [void]$s.ReadByte() } }
$t0 = Get-Date
$o = $out -replace '\\', '/'
while (((Get-Date) - $t0).TotalSeconds -lt ($At - 1)) { Start-Sleep -Milliseconds 200 }
Mon "screendump $o/tap_before.ppm"
Start-Sleep 1
# mouse_move is relative (pixels): park at the top-left corner, then move to X,Y
Mon "mouse_move -2000 -2000"
Mon "mouse_move $X $Y"
Mon 'mouse_button 1'
$steps = 8
if ($Drag[0] -or $Drag[1]) {
    for ($i = 1; $i -le $steps; $i++) {
        Mon ("mouse_move {0} {1}" -f [int]($Drag[0] / $steps), [int]($Drag[1] / $steps))
        Start-Sleep -Milliseconds 30
    }
} else {
    Start-Sleep -Milliseconds 150
}
Mon 'mouse_button 0'
$n = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep 1
    Mon ("screendump {0}/tap_{1:d2}.ppm" -f $o, $n); $n++
}
if ($DumpRam) { Mon ('pmemsave 0 0x8000000 "{0}/zunehd-ram.bin"' -f ($r -replace '\\','/')); Start-Sleep 3 }
$w.WriteLine('quit'); Start-Sleep 1; $c.Close()
if (-not $p.HasExited) { Stop-Process $p -Force }
Get-ChildItem "$out\tap_*.ppm" | Sort-Object Name | ForEach-Object {
    $png = $_.FullName -replace '\.ppm$', '.png'
    $res = py -3 -I "$r\tools\ppm2png.py" $_.FullName $png --scale 1
    Remove-Item $_.FullName
    $res
}
