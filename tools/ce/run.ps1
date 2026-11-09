param([string]$Expr)
# Runs in a fresh process so the C# types can be recompiled on every run.
$sp = Split-Path -Parent $PSScriptRoot
Add-Type -Path (Join-Path $PSScriptRoot 'CeBin.cs')
Invoke-Expression $Expr
