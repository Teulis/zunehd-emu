$root = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
Add-Type -Path (Join-Path $PSScriptRoot 'FindOat.cs')
[FindOat]::Scan((Join-Path $root 'extracted\NK.nb0'), [uint32]2147565568)    # 0x80014000
'--- ZBoot (base VA 0x07000000)'
[FindOat]::Scan((Join-Path $root 'extracted\ZBoot.nb0'), [uint32]117440512) # 0x07000000
