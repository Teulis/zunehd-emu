$lock = 'C:\rea-work\qemu\.build.lock'
for ($i = 0; $i -lt 120; $i++) {
    try { $fs = [System.IO.File]::Open($lock, 'CreateNew', 'Write'); $fs.Close(); break } catch { Start-Sleep 5 }
}
if ($i -ge 120) { Write-Output 'lock timeout'; exit 1 }
try {
    $env:MSYSTEM = 'UCRT64'
    & C:\msys64\usr\bin\bash.exe -l /c/rea-work/qemu/zbuild.sh
    $rc = $LASTEXITCODE
} finally { Remove-Item $lock -Force }
if ($rc -eq 0) {
    New-Item -ItemType Directory -Force C:\rea-work\ape\qemu | Out-Null
    Copy-Item C:\rea-work\qemu\build\qemu-system-arm.exe C:\rea-work\ape\qemu\ -Force
    Write-Output 'BUILD OK'
} else { Write-Output "BUILD FAILED $rc" }
