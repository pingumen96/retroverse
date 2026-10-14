<#
.SYNOPSIS
    Configures (if needed) and builds a preset inside the VS developer environment.

.EXAMPLE
    ./scripts/build.ps1                      # msvc-x64-debug
    ./scripts/build.ps1 -Preset msvc-x64-release
    ./scripts/build.ps1 -Preset msvc-x64-debug -Target retroverse
    ./scripts/build.ps1 -Fresh               # re-run configure from scratch
#>
param(
    [string]$Preset = 'msvc-x64-debug',
    [string]$Target = '',
    [switch]$Fresh
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dev = Join-Path $PSScriptRoot 'dev.ps1'
Push-Location $root
try {
    $cache = Join-Path $root "build/$Preset/CMakeCache.txt"
    if ($Fresh -or -not (Test-Path $cache)) {
        $configureArgs = @('cmake', '--preset', $Preset)
        if ($Fresh) { $configureArgs += '--fresh' }
        & $dev @configureArgs
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $buildArgs = @('cmake', '--build', '--preset', $Preset)
    if ($Target) { $buildArgs += @('--target', $Target) }
    & $dev @buildArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
