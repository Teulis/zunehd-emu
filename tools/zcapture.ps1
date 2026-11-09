# Boot the zunehd machine for N seconds, then dump CPU state via the HMP monitor.
# Outputs: zunehd-serial.log, zunehd-mmio.log (-d flags), zunehd-regs.txt in the repo root.
param([int]$Seconds = 20, [string]$DebugFlags = 'unimp,guest_errors,int', [string]$Image = 'NK',
      [string]$DFilter = '', [switch]$DumpRam, [int]$Emmc = -1, [int]$EmmcSlot = -1, [string]$EmmcImage = '', [switch]$Screenshot, [string[]]$Extra = @())
$r = Split-Path -Parent $PSScriptRoot; $b = "$r\ZuneHD_Firmware\Bins"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
Remove-Item "$r\zunehd-mmio.log", "$r\zunehd-serial.log", "$r\zunehd-regs.txt" -ErrorAction SilentlyContinue
$qa = @('-M', "zunehd,ext=$b\EXT.bin", '-m', '128M', '-kernel', "$b\$Image.bin", '-display', 'none',
        '-serial', "file:$r\zunehd-serial.log", '-monitor', 'tcp:127.0.0.1:4444,server,nowait',
        '-d', $DebugFlags, '-D', "$r\zunehd-mmio.log") + $Extra
if ($DFilter) { $qa += @('-dfilter', $DFilter) }
if ($Emmc -ge 0) {
    # -Emmc = HSMMC instance reported in boot args (0). The card always sits on the HSMMC
    # host (if=sd,index=0); -EmmcSlot is obsolete. -EmmcImage overrides the image file.
    $qa[1] += ",emmc-instance=$Emmc"
    if (-not $EmmcImage) { $EmmcImage = "$r\zunehd-emmc16.img" }
    $qa += @('-drive', "if=sd,index=0,format=raw,file=$EmmcImage")
}
$p = Start-Process -FilePath "$r\qemu\build\qemu-system-arm.exe" -ArgumentList $qa -PassThru -NoNewWindow `
        -RedirectStandardError "$r\zunehd-stderr.log"
Start-Sleep $Seconds
function Send-Hmp([string[]]$cmds) {
    $c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444); $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s); $rd = New-Object IO.StreamReader($s); $w.AutoFlush = $true
    Start-Sleep -Milliseconds 300; $out = ''
    foreach ($cmd in $cmds) {
        $w.WriteLine($cmd); Start-Sleep -Milliseconds 700
        while ($s.DataAvailable) { $out += [char]$rd.Read() }
    }
    $c.Close(); return $out
}
try {
    $cmds = @('stop', 'info registers', 'x/8i $pc', 'info mtree -f')
    if ($DumpRam) { $cmds += "pmemsave 0 0x8000000 `"$($r -replace '\\','/')/zunehd-ram.bin`"" }
    if ($Screenshot) { $cmds += "screendump $($r -replace '\\','/')/zunehd-screen.ppm" }
    $o = Send-Hmp $cmds
    if ($DumpRam) { Start-Sleep 3 }
    ($o -replace "`e\[[0-9;]*[A-Za-z]", '' -replace "`r", '') | Out-File -Encoding utf8 "$r\zunehd-regs.txt"
    Send-Hmp @('quit') | Out-Null
} catch { "monitor error: $_" }
Start-Sleep 1; if (-not $p.HasExited) { Stop-Process $p -Force }
if ($Screenshot -and (Test-Path "$r\zunehd-screen.ppm")) { py -3 -I "$r\tools\ppm2png.py" "$r\zunehd-screen.ppm" "$r\zunehd-screen.png" }
