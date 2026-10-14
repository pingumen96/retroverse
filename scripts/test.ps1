<#
.SYNOPSIS
    Runs CTest for a test preset inside the VS developer environment.

.EXAMPLE
    ./scripts/test.ps1                       # unit tests, Debug (excludes label "gpu")
    ./scripts/test.ps1 -Preset msvc-x64-debug-gpu
    ./scripts/test.ps1 -Filter "aabb"        # ctest -R aabb
#>
param(
    [string]$Preset = 'msvc-x64-debug',
    [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dev = Join-Path $PSScriptRoot 'dev.ps1'
Push-Location $root
try {
    $ctestArgs = @('ctest', '--preset', $Preset)
    if ($Filter) { $ctestArgs += @('-R', $Filter) }
    & $dev @ctestArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
