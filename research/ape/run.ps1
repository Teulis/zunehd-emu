param([int]$Seconds = 240, [string]$Tag = 'run', [string]$Extra = '')
# Boot avp=on, sync Up_30s.mp3, start playback. Env vars (ZUNEHD_*) set by the caller are inherited.
$ErrorActionPreference = 'Continue'
Remove-Item C:\rea-work\ape\out.wav -ErrorAction SilentlyContinue
py -3 -I C:\rea-work\tools\mkemmc.py --out C:\rea-work\ape\emmc.img | Out-Null
$env:ZUNEHD_USBIP = '3270'
$steps = "160:key home; 188:key home; 191:shot menu; 196:tap 78 282; 204:shot play1; 230:shot play2$Extra"
$p = Start-Process powershell -PassThru -NoNewWindow -ArgumentList @('-NoProfile', '-File', 'C:\rea-work\tools\zseq.ps1',
    '-Seconds', "$Seconds", '-Machine', 'avp=on', '-OutDir', 'C:\rea-work\ape', '-MonPort', '4475',
    '-Qemu', 'C:\rea-work\ape\qemu\qemu-system-arm.exe', '-EmmcImage', 'C:\rea-work\ape\emmc.img',
    '-QemuArgs', '"-audiodev wav,id=snd0,path=C:\rea-work\ape\out.wav"', '-Steps', "`"$steps`"") `
    -RedirectStandardOutput "C:\rea-work\ape\$Tag-zseq.out" -RedirectStandardError "C:\rea-work\ape\$Tag-zseq.err"
Start-Sleep 168
py -3 -I C:\rea-work\tools\zmtp.py --port 3270 sync "C:\rea-work\Test audio\Up_30s.mp3" > "C:\rea-work\ape\$Tag-zmtp.out" 2>&1
$p.WaitForExit()
Write-Output "done $Tag"
