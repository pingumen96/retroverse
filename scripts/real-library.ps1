<#
.SYNOPSIS
    Runs the tests that read the real ES-DE library ([real-library]) between two read-only snapshots of it, and fails
    on a failed test or on any change.

.DESCRIPTION
    The tests tagged [.real-library] in rv_tests read this machine's ES-DE installation, ROMs and media (ADR-0008
    section 10). Catch2 hides them from ctest; this script runs them the way the M1 gate will:
      1. it refuses to run while ES-DE runs (ES-DE.exe rewrites its gamelists after every game);
      2. it takes the library's folders from its parameters, else from the environment variables RV_REAL_ESDE_HOME,
         RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT, and optionally the RetroArch executable
         registered in App Paths from -RetroArchExe or RV_REAL_RETROARCH_EXE; every one given must exist. Nothing in
         the product knows these paths: they are this machine's;
      3. it snapshots the ES-DE home and install folder (configuration-like files hashed) and the ROM and media roots
         (their folder listings only: sizes and times, no file opened) with scripts/snapshot_tree.py;
      4. it runs build/<preset>/bin/rv_tests.exe with the tag filter (default "[real-library]"), the RV_REAL_*
         variables set for it only (Release by default: build it first, ./scripts/build.ps1 -Preset msvc-x64-release);
      5. it snapshots again and compares: any added, removed or changed file or folder is a failure.
    Everything it writes goes to build/real-library/ (the snapshots, the test log). The library is only read.
    Exit codes: 0 the tests passed and nothing changed; 1 a test failed or a snapshot differs; 2 it could not run (ES-DE
    running, a folder missing, rv_tests.exe not built, Python missing).

.EXAMPLE
    ./scripts/real-library.ps1 -EsdeHome C:\Users\me\ES-DE -EsdeInstall 'C:\Program Files\ES-DE' -RomRoot D:\ROMs `
        -MediaRoot C:\Users\me\ES-DE\downloaded_media -RetroArchExe C:\RetroArch\retroarch.exe
    Example paths only: give your own, or set the RV_REAL_* variables once for your user.

.EXAMPLE
    ./scripts/real-library.ps1 -Preset msvc-x64-debug -Filter '[real-library][system_probe]'
#>
param(
    [string]$EsdeHome = $env:RV_REAL_ESDE_HOME,
    [string]$EsdeInstall = $env:RV_REAL_ESDE_INSTALL,
    [string]$RomRoot = $env:RV_REAL_ROM_ROOT,
    [string]$MediaRoot = $env:RV_REAL_MEDIA_ROOT,
    [string]$RetroArchExe = $env:RV_REAL_RETROARCH_EXE,
    [string]$Preset = 'msvc-x64-release',
    [string]$Filter = '[real-library]'
)
$ErrorActionPreference = 'Stop'
# Numbers in messages use a decimal point whatever the Windows locale.
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'build/real-library'
$snapshotScript = Join-Path $PSScriptRoot 'snapshot_tree.py'
$rvTests = Join-Path $root "build/$Preset/bin/rv_tests.exe"

function Stop-Run([string]$Message) {
    Write-Host "real-library: $Message" -ForegroundColor Red
    exit 2
}

function Test-EsdeRunning {
    return [bool](Get-Process -Name 'ES-DE' -ErrorAction SilentlyContinue)
}

# --- Preconditions ----------------------------------------------------------------------------------------------------
if (Test-EsdeRunning) {
    Stop-Run 'ES-DE is running: close it first (it rewrites its gamelists while it runs).'
}
$folders = [ordered]@{
    'ES-DE home (-EsdeHome, RV_REAL_ESDE_HOME)'          = $EsdeHome
    'ES-DE install (-EsdeInstall, RV_REAL_ESDE_INSTALL)' = $EsdeInstall
    'ROM root (-RomRoot, RV_REAL_ROM_ROOT)'              = $RomRoot
    'media root (-MediaRoot, RV_REAL_MEDIA_ROOT)'        = $MediaRoot
}
foreach ($name in $folders.Keys) {
    $folder = $folders[$name]
    if (-not $folder) { Stop-Run "no $name given." }
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { Stop-Run "the $name does not exist: $folder" }
}
if ($RetroArchExe -and -not (Test-Path -LiteralPath $RetroArchExe -PathType Leaf)) {
    Stop-Run "the RetroArch executable (-RetroArchExe, RV_REAL_RETROARCH_EXE) does not exist: $RetroArchExe"
}
if (-not (Test-Path -LiteralPath $rvTests -PathType Leaf)) {
    Stop-Run "$rvTests is not built: ./scripts/build.ps1 -Preset $Preset"
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { Stop-Run 'Python 3.10 or later (python) is not on PATH: scripts/snapshot_tree.py needs it.' }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Invoke-Snapshot([string]$File) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $python.Source -I $snapshotScript snapshot $File --hash $EsdeHome --hash $EsdeInstall --listing $RomRoot `
        --listing $MediaRoot
    if ($LASTEXITCODE -ne 0) { Stop-Run "the snapshot failed (exit code $LASTEXITCODE)" }
    Write-Host ('Snapshot taken in {0:N1} s' -f $timer.Elapsed.TotalSeconds)
}

# --- Before, the tests, after -----------------------------------------------------------------------------------------
$before = Join-Path $outDir 'before.json'
$after = Join-Path $outDir 'after.json'
$testLog = Join-Path $outDir 'tests.log'
Remove-Item -ErrorAction SilentlyContinue $before, $after, $testLog

Write-Host 'Snapshot before the tests'
Invoke-Snapshot $before

$variables = [ordered]@{
    RV_REAL_ESDE_HOME     = $EsdeHome
    RV_REAL_ESDE_INSTALL  = $EsdeInstall
    RV_REAL_ROM_ROOT      = $RomRoot
    RV_REAL_MEDIA_ROOT    = $MediaRoot
    RV_REAL_RETROARCH_EXE = $RetroArchExe
}
$saved = @{}
foreach ($name in $variables.Keys) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $(if ($variables[$name]) { $variables[$name] } else { $null }),
        'Process')
}
Write-Host "Running $rvTests `"$Filter`""
$timer = [Diagnostics.Stopwatch]::StartNew()
try {
    & $rvTests $Filter --reporter compact *>&1 | Tee-Object -FilePath $testLog
    $testCode = $LASTEXITCODE
}
finally {
    foreach ($name in $variables.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
}
Write-Host ('Tests ran in {0:N1} s, exit code {1}' -f $timer.Elapsed.TotalSeconds, $testCode)

Write-Host 'Snapshot after the tests'
Invoke-Snapshot $after
& $python.Source -I $snapshotScript compare $before $after
$compareCode = $LASTEXITCODE
if (Test-EsdeRunning) {
    Write-Host 'real-library: ES-DE started meanwhile: differences may be its own.' -ForegroundColor Yellow
}

# --- Verdict ----------------------------------------------------------------------------------------------------------
$testsPassed = $testCode -eq 0
$unchanged = $compareCode -eq 0
Write-Host ''
Write-Host ('Tests:     {0}' -f $(if ($testsPassed) { 'passed' } else { "FAILED (exit code $testCode, see $testLog)" }))
$snapshotVerdict = if ($unchanged) { '0 differences' } else { "DIFFERENT (see above; $before, $after)" }
Write-Host "Snapshots: $snapshotVerdict"
exit $(if ($testsPassed -and $unchanged) { 0 } else { 1 })
