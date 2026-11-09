# Build QEMU under the shared build lock, then copy the exe to C:\rea-work\gui\qemu
$lock = 'C:\rea-work\qemu\.build.lock'
while ($true) {
    try { New-Item -ItemType File $lock -ErrorAction Stop | Out-Null; break }
    catch { Write-Host 'build lock busy, waiting'; Start-Sleep 15 }
}
try {
    $env:MSYSTEM = 'UCRT64'
    & C:\msys64\usr\bin\bash.exe -l /c/rea-work/qemu/zbuild.sh
    $ok = $LASTEXITCODE -eq 0
    if ($ok) { Copy-Item C:\rea-work\qemu\build\qemu-system-arm.exe C:\rea-work\gui\qemu\ -Force; Write-Host 'BUILD OK, copied' }
    else { Write-Host "BUILD FAILED ($LASTEXITCODE)" }
} finally { Remove-Item $lock -Force }
Get-Content C:\rea-work\qemu\build-zunehd.log -Tail 5
