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
      4. with -Measure, it runs that PowerShell script first (with -MeasureArguments splatted), with the same
         variables set: the measurements of scripts/gate.ps1 -Gate M1 -RealLibrary (scripts/gate-m1-real.ps1), which
         must read the library before the tests do, so that its ROM scan is the first read after the snapshot; its
         exit code counts like the tests';
      5. it runs build/<preset>/bin/rv_tests.exe with the tag filter (default "[real-library]"), the RV_REAL_*
         variables, RV_LAUNCH_BACKGROUND=1, RV_AUDIO=null and RV_LAUNCH_ALLOWLIST naming the build's fake emulator alone
         set for it only, as for the measurement script (Release by default: build it first, ./scripts/build.ps1 -Preset
         msvc-x64-release);
      6. it snapshots again and compares: any added, removed or changed file or folder is a failure.
    With -FullVideoScan it sets RV_REAL_FULL_VIDEO_SCAN=1 for the tests: [real-library][videos] (I5 step 7) then checks
    the clips of every system, not only the SNES ones (hours, resumable: tests/unit/cli/test_cli_media_videos_real.cpp).
    Everything it writes goes to -OutDir, build/real-library/ by default (the snapshots, the test log). The library is
    only read.
    Exit codes: 0 the tests passed and nothing changed; 1 a test or the measurements failed, or a snapshot differs; 2 it
    could not run (ES-DE running, a folder missing, rv_tests.exe not built, Python missing).

.EXAMPLE
    ./scripts/real-library.ps1 -EsdeHome C:\Users\me\ES-DE -EsdeInstall 'C:\Program Files\ES-DE' -RomRoot D:\ROMs `
        -MediaRoot C:\Users\me\ES-DE\downloaded_media -RetroArchExe C:\RetroArch\retroarch.exe
    Example paths only: give your own, or set the RV_REAL_* variables once for your user.

.EXAMPLE
    ./scripts/real-library.ps1 -Preset msvc-x64-debug -Filter '[real-library][system_probe]'

.EXAMPLE
    ./scripts/real-library.ps1 -Filter '[real-library][videos]' -FullVideoScan
    The verdicts of every clip of the library (I5 step 7).
#>
param(
    [string]$EsdeHome = $env:RV_REAL_ESDE_HOME,
    [string]$EsdeInstall = $env:RV_REAL_ESDE_INSTALL,
    [string]$RomRoot = $env:RV_REAL_ROM_ROOT,
    [string]$MediaRoot = $env:RV_REAL_MEDIA_ROOT,
    [string]$RetroArchExe = $env:RV_REAL_RETROARCH_EXE,
    [string]$Preset = 'msvc-x64-release',
    [string]$Filter = '[real-library]',
    [string]$OutDir,
    [string]$Measure,
    [hashtable]$MeasureArguments = @{},
    [switch]$FullVideoScan
)
$ErrorActionPreference = 'Stop'
# Numbers in messages use a decimal point whatever the Windows locale.
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
$outDir = if ($OutDir) { $OutDir } else { Join-Path $root 'build/real-library' }
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
if ($Measure -and -not (Test-Path -LiteralPath $Measure -PathType Leaf)) {
    Stop-Run "the measurement script (-Measure) does not exist: $Measure"
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
    # Automated: whatever a test starts runs in the background, never taking the foreground (rv_tests sets it too).
    RV_LAUNCH_BACKGROUND  = '1'
    # The build's fake emulator alone (CLAUDE.md's safety rule): no real emulator may start, whatever a test or the
    # measurement script would launch. (Not empty: PowerShell removes a variable set to an empty value.)
    RV_LAUNCH_ALLOWLIST   = (Join-Path $root "build/$Preset/bin/rv_fake_emulator.exe")
    # The clips' sound never reaches the system's audio (ADR-0016 §9): every run the tests start mixes into the null
    # output.
    RV_AUDIO              = 'null'
    # [real-library][videos] checks every system's clips, not only the SNES ones (I5 step 7).
    RV_REAL_FULL_VIDEO_SCAN = $(if ($FullVideoScan) { '1' } else { $null })
}
$saved = @{}
foreach ($name in $variables.Keys) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $(if ($variables[$name]) { $variables[$name] } else { $null }),
        'Process')
}
$measureCode = 0
try {
    if ($Measure) {
        Write-Host "Running $Measure"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        # An exception of the script fails the measurements, never the run: the tests and the second snapshot follow.
        try {
            & $Measure @MeasureArguments
            $measureCode = $LASTEXITCODE
        }
        catch {
            Write-Host "real-library: the measurement script stopped: $($_.Exception.Message)" -ForegroundColor Red
            $measureCode = 1
        }
        Write-Host ('Measurements ran in {0:N1} s, exit code {1}' -f $timer.Elapsed.TotalSeconds, $measureCode)
    }
    Write-Host "Running $rvTests `"$Filter`""
    $timer = [Diagnostics.Stopwatch]::StartNew()
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
$measured = $measureCode -eq 0
$unchanged = $compareCode -eq 0
Write-Host ''
if ($Measure) {
    Write-Host ('Measurements: {0}' -f $(if ($measured) { 'passed' } else { "FAILED (exit code $measureCode)" }))
}
Write-Host ('Tests:     {0}' -f $(if ($testsPassed) { 'passed' } else { "FAILED (exit code $testCode, see $testLog)" }))
$snapshotVerdict = if ($unchanged) { '0 differences' } else { "DIFFERENT (see above; $before, $after)" }
Write-Host "Snapshots: $snapshotVerdict"
exit $(if ($testsPassed -and $measured -and $unchanged) { 0 } else { 1 })
