# Retry the locked build until the link succeeds (the other agent runs build\qemu-system-arm.exe)
for ($i = 0; $i -lt 40; $i++) {
    while (Get-Process | Where-Object { $_.Path -like 'C:\rea-work\qemu\build\*' }) { Start-Sleep 10 }
    $out = powershell -NoProfile -File C:\rea-work\gui\build.ps1 | Out-String
    if ($out -match 'BUILD OK') { Write-Host 'BUILD OK'; exit 0 }
    Write-Host 'build failed, retrying'; Start-Sleep 15
}
exit 1
