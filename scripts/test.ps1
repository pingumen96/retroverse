<#
.SYNOPSIS
    Runs CTest for a test preset inside the VS developer environment.

.DESCRIPTION
    -Jobs runs that many tests at once. The default (1) is what the gates use: some unit tests wait for a job against
    a wall-clock deadline (app/world_flow's 60 s), and 8 at once on the development machine made one of them miss it.
    A few jobs are a quick local check; a verdict runs them one at a time.

.EXAMPLE
    ./scripts/test.ps1                       # unit tests, Debug (excludes label "gpu")
    ./scripts/test.ps1 -Preset msvc-x64-debug-gpu
    ./scripts/test.ps1 -Filter "aabb"        # ctest -R aabb
    ./scripts/test.ps1 -Jobs 4               # four tests at once
#>
param(
    [string]$Preset = 'msvc-x64-debug',
    [string]$Filter = '',
    [ValidateRange(1, 64)]
    [int]$Jobs = 1
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dev = Join-Path $PSScriptRoot 'dev.ps1'
Push-Location $root
try {
    $ctestArgs = @('ctest', '--preset', $Preset)
    if ($Filter) { $ctestArgs += @('-R', $Filter) }
    if ($Jobs -gt 1) { $ctestArgs += @('-j', "$Jobs") }
    & $dev @ctestArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
