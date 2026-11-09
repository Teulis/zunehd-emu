param([int]$Seconds = 60, [string]$Steps = '', [string]$Tag = 'run', [switch]$Log, [string]$UsbIp = '', [string]$Machine = '')
# Private zseq wrapper for the GUI work: own monitor port / exe / image; shots copied to shots\<Tag>_*.png
Remove-Item Env:ZUNEHD_USBIP -ErrorAction SilentlyContinue
if ($UsbIp) { $env:ZUNEHD_USBIP = $UsbIp }
$a = @('-NoProfile', '-File', 'C:\rea-work\tools\zseq.ps1', '-Seconds', $Seconds, '-OutDir', 'C:\rea-work\gui',
       '-MonPort', '4455', '-Qemu', 'C:\rea-work\gui\qemu\qemu-system-arm.exe', '-EmmcImage', 'C:\rea-work\gui\emmc.img',
       '-Steps', $Steps)
if ($Log) { $a += '-Log' }
if ($Machine) { $a += @('-Machine', $Machine) }
& powershell @a
Get-ChildItem C:\rea-work\gui\shots\seq_*.png | ForEach-Object {
    Copy-Item $_.FullName ("C:\rea-work\gui\shots\" + $Tag + "_" + ($_.Name -replace '^seq_', ''))
}
