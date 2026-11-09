param([string]$BinDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'ZuneHD_Firmware\Bins'), [string]$OutRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'extracted'))
Add-Type -Path (Join-Path $PSScriptRoot 'CeBin.cs'), (Join-Path $PSScriptRoot 'CeExtract.cs')
foreach ($f in 'ZBoot','NK','EXT','Recovery') {
    $o = Join-Path $OutRoot $f
    New-Item -ItemType Directory -Force $o | Out-Null
    [CeExtract]::Run((Join-Path $BinDir "$f.bin"), $o, '') | Out-File -Encoding utf8 (Join-Path $o 'modules.txt')
}
