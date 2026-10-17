# Renders Lumen screens to PNG files for visual review (no window is shown).
# Usage: powershell -File tools\snap.ps1 -Out <dir> [-Name home] [-Args "--tab library"]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Name = "home",
    [string]$Args = ""
)
$exe = Join-Path $PSScriptRoot "..\build\release\Lumen.exe"
$png = Join-Path $Out "$Name.png"
$p = Start-Process -FilePath $exe -ArgumentList "--snapshot `"$png`" $Args" -PassThru -Wait
"$Name exit=$($p.ExitCode)"
