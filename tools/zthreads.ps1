# Boot to idle under gdb and dump every WinCE thread (saved pc/lr/sp + symbolized stack words).
#   powershell -NoProfile -File tools\zthreads.ps1 [-IdleSkips 300] [-TimeoutSec 200] [-Filter devmgr]
# -IdleSkips: idle-loop hits to let pass before dumping (more = later in boot).
param([int]$IdleSkips = 300, [int]$TimeoutSec = 200, [string]$Filter = '', [string]$Machine = '')
$r = Split-Path -Parent $PSScriptRoot
$gs = Join-Path $env:TEMP 'zthreads.gdb'
(Get-Content "$r\tools\zthreads.gdb" -Raw).Replace('IDLE_SKIPS', "$IdleSkips") | Set-Content -Encoding ascii $gs
$out = & powershell -NoProfile -File "$r\tools\zgdb.ps1" -Script $gs -TimeoutSec $TimeoutSec -Machine $Machine
$addrs = @($out | Select-String '(pc|lr|STK) ([0-9a-f]{8})' -AllMatches | ForEach-Object { $_.Matches } |
           ForEach-Object { '0x' + $_.Groups[2].Value }) | Sort-Object -Unique
$map = @{}
if ($addrs.Count) {
    # zsym prints "0xADDR  module+off" and may skip unmapped addresses, so key by address
    foreach ($s in (py -3 -I "$r\tools\zsym.py" $addrs)) {
        $p = $s.Trim() -split '\s+', 2
        if ($p.Count -eq 2) { $map[('0x{0:x8}' -f [Convert]::ToUInt32($p[0], 16))] = $p[1] }
    }
}
$block = @()
foreach ($l in $out) {
    if ($l -match '^PROC') { if ($block -and (-not $Filter -or ($block -join "`n") -match $Filter)) { $block }; $block = @($l -replace '\s+0x[0-9a-f]+:\s+U?', ' ') ; continue }
    if ($l -match '^(THR|  STK)') {
        $l = [regex]::Replace($l, '(pc|lr|STK) ([0-9a-f]{8})', { param($m) $k = '0x' + $m.Groups[2].Value; "$($m.Groups[1].Value) $($m.Groups[2].Value)" + $(if ($map[$k]) { " <$($map[$k])>" } else { '' }) })
        $block += $l
    } elseif ($l -match '^0x[0-9a-f]+:\s+U"') { $block[-1] += ' ' + ($l -replace '^0x[0-9a-f]+:\s+', '') }
}
if ($block -and (-not $Filter -or ($block -join "`n") -match $Filter)) { $block }

