param([string]$BinDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'ZuneHD_Firmware\Bins'))
$sp = Split-Path -Parent $PSScriptRoot
Add-Type -Path (Join-Path $PSScriptRoot 'CeBin.cs')
$out = Join-Path $sp 'out'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($f in 'ZBoot','NK','EXT','Recovery') {
    [CeDump]::Summary((Join-Path $BinDir "$f.bin"), $out) | Out-File -Encoding utf8 (Join-Path $out "$f.txt")
}
