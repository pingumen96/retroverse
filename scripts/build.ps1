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
    # A tree configured before the presets gained the vcpkg feature xr keeps its old VCPKG_MANIFEST_FEATURES, and the
    # manifest install of its next regeneration takes the loader out of the shared build/vcpkg_installed: the preset
    # again, once. A tree with RV_ENABLE_XR off wants no loader.
    $stale = (Test-Path $cache) -and
        -not (Select-String -LiteralPath $cache -Pattern '^VCPKG_MANIFEST_FEATURES:[^=]*=.*\bxr\b' -Quiet) -and
        -not (Select-String -LiteralPath $cache -Pattern '^RV_ENABLE_XR:[^=]*=(OFF|0|FALSE|NO|N)$' -Quiet)
    if ($Fresh -or $stale -or -not (Test-Path $cache)) {
        $configureArgs = @('cmake', '--preset', $Preset)
        if ($Fresh) {
            $configureArgs += '--fresh'
            # A fresh configure runs vcpkg's manifest install again (cmake/VcpkgManifestStamp.cmake).
            Remove-Item -LiteralPath (Join-Path $root 'build/vcpkg_installed/.retroverse-manifest-stamp') -ErrorAction Ignore
        }
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
