<#
.SYNOPSIS
    Runs a command inside the Visual Studio 2026 x64 developer environment.

.DESCRIPTION
    Each invocation imports the VS developer environment (cl.exe, the VS-bundled CMake and Ninja,
    Windows SDK) into the current process and then runs the given command. When already inside a
    developer shell the environment is reused. Set RV_VS_INSTALL to override the Visual Studio path.

.EXAMPLE
    ./scripts/dev.ps1 cmake --preset msvc-x64-debug
    ./scripts/dev.ps1 cmake --build --preset msvc-x64-debug
    ./scripts/dev.ps1 ctest --preset msvc-x64-debug
#>
# No param() block on purpose: every argument (including --flags) is forwarded verbatim via $args.
$ErrorActionPreference = 'Stop'

function Get-CompactPath([string]$PathValue) {
    # VsDevCmd.bat runs through cmd.exe, which rejects lines longer than 8191 characters. Long PATH values
    # with duplicates (common on developer machines) break it, so drop duplicate and non-existent entries.
    # Only this process is affected; the user's persistent PATH is never modified.
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $kept = New-Object 'System.Collections.Generic.List[string]'
    foreach ($entry in ($PathValue -split ';')) {
        $dir = $entry.Trim().TrimEnd('\')
        if (-not $dir -or -not $seen.Add($dir)) { continue }
        $exists = $false
        try { $exists = Test-Path -LiteralPath $dir -PathType Container -ErrorAction Stop } catch { $exists = $false }
        if ($exists) { $kept.Add($dir) }
    }
    return ($kept -join ';')
}

# The presets take the vcpkg toolchain from VCPKG_ROOT, which names the pinned clone (ADR-0007). The developer
# environment's vcpkg extension (VC\vcpkg\vcpkg-init.cmd) points VCPKG_ROOT at Visual Studio's bundled copy whatever it
# was, so the value is read before entering the environment and restored after it. Inside a Visual Studio developer
# shell the process already holds the bundled copy: the user's (or the machine's) own setting is taken instead.
function Test-BundledVcpkg([string]$Root) {
    return [bool]$Root -and (Test-Path -LiteralPath (Join-Path $Root 'vcpkg-bundle.json'))
}
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot -or (Test-BundledVcpkg $vcpkgRoot)) {
    $vcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
    if (-not $vcpkgRoot) { $vcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'Machine') }
}
if (-not $vcpkgRoot -or (Test-BundledVcpkg $vcpkgRoot)) {
    throw "VCPKG_ROOT must name your vcpkg clone (e.g. E:/Progetti/vcpkg), not Visual Studio's bundled vcpkg (ADR-0007)."
}

if (-not $env:VSCMD_VER) {
    $installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $vswhere = Join-Path $installerDir 'vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere" }
    # Some VsDevCmd extension scripts invoke vswhere.exe through PATH.
    $env:PATH = Get-CompactPath "$installerDir;$env:PATH"

    $vsPath = $env:RV_VS_INSTALL
    if (-not $vsPath) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (-not $vsPath) { throw 'No Visual Studio installation with the C++ x64 toolset was found.' }
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

    # Always prefer the CMake and Ninja shipped with Visual Studio over older copies found on PATH.
    $vsCMake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    $vsNinja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    foreach ($dir in @($vsNinja, $vsCMake)) {
        if (Test-Path $dir) { $env:PATH = "$dir;$env:PATH" }
    }
}

$env:VCPKG_ROOT = $vcpkgRoot

if ($args.Count -eq 0) { return }

$exe = $args[0]
$rest = @()
if ($args.Count -gt 1) { $rest = $args[1..($args.Count - 1)] }
& $exe @rest
exit $LASTEXITCODE
