# One-time setup: convert the Zune HD firmware and build the emulated 16 GB eMMC image.
#   1. Put NK.bin and EXT.bin (Zune HD firmware 4.5, build 114) into .\firmware\
#   2. powershell -ExecutionPolicy Bypass -File .\setup.ps1
# Re-run with -Rebuild to reset the eMMC image (wipes synced music and settings).
param([switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

$script:PyExe = $null; $script:PyPre = @()
foreach ($c in @(@{ e = 'py'; a = @('-3') }, @{ e = 'python'; a = @() }, @{ e = 'python3'; a = @() })) {
    $cmd = Get-Command $c.e -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cmd) { continue }
    $v = & $cmd.Source @($c.a + @('-c', 'import sys; print(sys.version_info[0])')) 2>$null
    if ($v -eq '3') { $script:PyExe = $cmd.Source; $script:PyPre = $c.a; break }
}
if (-not $script:PyExe) { throw 'Python 3 not found. Install it from https://www.python.org/ (tick "Add to PATH").' }
function Invoke-Python {
    & $script:PyExe @($script:PyPre + @('-I') + $args)
    if ($LASTEXITCODE) { throw "python $args failed" }
}

$expected = @{
    'NK.bin'  = '43B4EFCC5718E123D929873F3951E6500564C0733E422D8EC7BFAD4D39DC2963'
    'EXT.bin' = 'C2255DAAB91FBF013A0DD615DA6EBB52865DE511910E57A1700514B123821B63'
}
foreach ($n in 'NK.bin', 'EXT.bin') {
    $f = Join-Path $root "firmware\$n"
    if (-not (Test-Path $f)) { throw "firmware\$n is missing. Copy NK.bin and EXT.bin from the Zune HD 4.5 firmware into .\firmware\" }
    $h = (Get-FileHash $f -Algorithm SHA256).Hash
    if ($h -ne $expected[$n]) {
        Write-Warning "$n does not match the tested firmware (4.5.114). It may not boot."
    }
}

New-Item -ItemType Directory -Force (Join-Path $root 'extracted') | Out-Null
foreach ($n in 'NK', 'EXT') {
    $out = Join-Path $root "extracted\$n.nb0"
    if (-not (Test-Path $out)) { Invoke-Python tools\b000ff2nb0.py "firmware\$n.bin" $out }
}

$img = Join-Path $root 'zunehd-emmc16.img'
if ((Test-Path $img) -and -not $Rebuild) {
    Write-Host "eMMC image already exists ($img). Use -Rebuild to recreate it."
} else {
    Write-Host 'Building the eMMC image (16 GB sparse file, ~60 MB on disk; needs an NTFS drive)...'
    Invoke-Python tools\mkemmc.py --out $img
}
Write-Host ''
Write-Host 'Setup done. Start the emulator with:  powershell -ExecutionPolicy Bypass -File .\run.ps1'

