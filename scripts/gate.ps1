<#
.SYNOPSIS
    Runs the checks of a gate end to end and prints a summary table.

.DESCRIPTION
    Gate A (milestone M0, style.md section 27 phase A; docs/testing/gate-a.md):
      1. builds msvc-x64-debug, msvc-x64-relwithdebinfo and msvc-x64-release: every project target compiles with /WX,
         so a compiler warning fails its build; the build output is also scanned for any compiler, linker or CMake
         warning (with -Clean every target is rebuilt, so that all of them are seen);
      2. runs the unit tests (label unit: rv_tests, rv_platform_tests, the CMake script tests, spike_launch_tests) of
         Debug and Release;
      3. runs the GPU checks (label gpu) of Debug and Release: the smoke tests of tests/smoke (the reference scenes, the
         rendering self-checks and --gl-selftest, the key light's shadow and the ambient occlusion, the instance buffer
         growing mid-run, the developer overlay and the asset preview (Debug), Release rendering from the assets
         deployed next to it, which its build, incremental too, mirrors from the source tree; since I2 the launches
         from the quick catalog and the ES-DE catalog in the app) and the I1 spike checks (spike/video: two generated
         clips on time and the damaged files falling back; spike_ui.scenario);
      4. compares the test room with golden G01 in Debug and Release (within 6/255 per channel, at most 0.5% of the
         pixels; on the reference machine only, skipped elsewhere): ctest -R 'gpu.golden/G01' (Gate B runs every
         golden), one row per golden with the verdict its case writes next to its screenshot (comparison.txt).

    Gate M1 (milestone M1, the ES-DE catalog; docs/testing/gate-m1.md): everything Gate A runs, and
      5. the M1 checks on fixtures, per Debug and Release: the catalog's unit tests and the GPU catalog and launch cases
         (counted from the runs of steps 2 and 3: they are in the presets), and retroverse-cli as a process on the
         standard fixture (library create, import, a second import that writes nothing, db rebuild-index, db check,
         search, show, assign, launch --game --wait into the fake emulator with RV_LAUNCH_BACKGROUND=1 and
         RV_LAUNCH_ALLOWLIST naming the fake alone, sessions; the fixture unchanged: the unit test "cli/process");
      6. with -RealLibrary, on the real ES-DE library (its folders from -EsdeHome, -EsdeInstall, -RomRoot and
         -MediaRoot, or the RV_REAL_* variables, and -RetroArchExe, optional: the RetroArch executable App Paths registers,
         for the real launch-translation tests, which start nothing; ES-DE must be closed): the NFR-02 benchmark (10,000
         generated games),
         then scripts/real-library.ps1, which snapshots the library read-only, runs scripts/gate-m1-real.ps1 (the ROM
         scan, the first and second imports, db rebuild-index, db check, the media coverage, a search, NFR-01 with
         retroverse.exe; the whole quick-hash pass with -FullHashPass only, about 45 minutes cold) and the hidden
         [real-library] tests of Release (among them NFR-01 and NFR-02 on the real catalog and the quick-hash pass on
         a sample, gb), snapshots again and requires 0 differences. Each figure is a row of the summary with its limit
         where one exists. Nothing starts a real emulator: every launch is the fake's, under the allow-list.
    Gate B (milestone M2, the SNES room; style.md section 27 phase B; docs/testing/gate-b.md): everything Gate A runs,
    with every golden (G01, and the room goldens G02, G02s, G06, G07, G08a, G08b and G10 of
    tests/smoke/test_golden_room.cpp, their verdicts in build/<preset>/smoke/golden_room/<case>/comparison.txt), and
      5. per Debug and Release, the I3 test cases counted from the unit and GPU runs (the world's snapshot goldens and
         property suites, the room in the app: gpu.world*, gpu.covers, gpu.media viewer, gpu.refresh, ...);
      6. in Release, the benchmark path of 2,000 titles at 1920 x 1080 with --no-vsync (rv_smoke_tests
         "[benchmark]"): its frame times and GPU memory read from its run report against the acceptable limits
         (docs/roadmap.md, "Acceptable performance"), its GPU passes reported; with -Benchmarks also every hidden
         benchmark, rv_tests "[bench]" and rv_smoke_tests "[bench]", as informative rows (a slow figure never fails
         the gate);
      7. with -RealLibrary, scripts/real-library.ps1 -Filter '[real-library][room-real]' (ES-DE closed, read-only
         snapshots before and after, RV_LAUNCH_ALLOWLIST naming the fake emulator alone): the real SNES room's plan
         generated twice, the restart after a second import, the covers cold and warm, the benchmark path, a refresh
         of the whole library with the quick catalog open; the figures read from the test's run reports (time to the
         room, covers resident cold and warm, frame p99 on the benchmark path and over the refresh, NFR-01 with the
         room) against the acceptable limits.
    Gate C (milestone M3 part 1, the building of ADR-0013; docs/testing/gate-c.md): everything Gate A runs, with every
    golden (G01, G02 to G10 and the building's G11, G12 and G13) and Gate B's benchmark path in Release against the
    acceptable limits (Gate B's step 6; not its step 5: the I3 cases count in the unit and GPU totals, without rows of
    their own), and
      5. per Debug and Release, rows for the I4 test cases counted from the unit and GPU runs, one per area (the
         world's building, the floors, the shared rooms, the update suite, the directory, the elevator, the floor in
         the app, the persisted start and live updates);
      6. with -RealLibrary, scripts/real-library.ps1 -Filter '[real-library][building],[real-library][room-real]'
         (ES-DE closed, read-only snapshots before and after, RV_LAUNCH_ALLOWLIST naming the fake emulator alone): the
         whole library's building (tests/unit/app/test_building_real.cpp) and I3's SNES room, their figures read from
         the tests' run reports against the acceptable limits: the building planned twice with one plan, an unchanged
         restart's building job skipped by its stamp (3 s), NFR-01 to the lobby (5 s), the busiest floor's GPU memory
         (512 MiB), the rides cold and warm (the view back within 10 s cold and 3 s warm, the room within 30 s and 3 s),
         and Gate B's rows of the SNES room.
    Gate D (I6, the styling pass of the building, ADR-0014; docs/testing/gate-d.md): everything Gate C runs, with every
    golden (Gate C's and I6's G14 to G18) and Gate B's benchmark path in Release, and
      7. per Debug and Release, rows for the I6 test cases counted from the unit and GPU runs, one per area (light through
         the doorways, the dressed halls, the theme packs, the side-face spines and their migration, the era furniture,
         the hub, the arcade and the handhelds); a case may count in an I4 row too;
      8. with -RealLibrary, Gate C's real-library run with the family migrations' case too ([real-library][building]
         holds it): the library planned by generator 1.3's retroverse-cli.exe (-LegacyCli or RV_LEGACY_CLI; the case
         and its row fail without it), then started in this build: its rows read the case's run reports in
         build/msvc-x64-release/real-library/migration (the rooms of succeeded families migrated once, the others byte
         for byte, none over capacity, the ground floor's rooms re-placed onto the hub in the same update, a second
         start and the whole job writing nothing).
    -TimingOnly (Gate D) runs the timing parts alone, for a rerun on a quiet machine: the Release build, the benchmark
    path and, with -RealLibrary, the real library's run (its correctness rows too: the snapshots, the tests).
    Gates M1, B, C and D run under scripts/focus_watch.py (--watch build): a test process that takes the foreground or
    shows a window where the user sees it, or a system error dialog during the run, fails the gate. The watcher counts
    a failing program of the user's (a WerFault or loader box) against the run too: its row names the rule that
    matched, so such a finding can be told from a test's.

    Every step logs into build/gate-<gate>/. The script prints one row per step and exits 0 when no step failed: a
    skipped step (no OpenGL 4.5 context, another GPU than the golden's) is reported, not failed; an informative row
    (a figure without a limit) neither passes nor fails. Each build and ctest command runs inside the Visual Studio
    developer environment (scripts/dev.ps1).

.EXAMPLE
    ./scripts/gate.ps1 -Gate A
    ./scripts/gate.ps1 -Gate A -Clean      # rebuild every target of every preset first
    ./scripts/gate.ps1 -Gate M1
    ./scripts/gate.ps1 -Gate M1 -RealLibrary -EsdeHome C:\Users\me\ES-DE -EsdeInstall 'C:\Program Files\ES-DE' `
        -RomRoot D:\ROMs -MediaRoot C:\Users\me\ES-DE\downloaded_media     # example paths: give your own
    ./scripts/gate.ps1 -Gate B
    ./scripts/gate.ps1 -Gate B -RealLibrary     # the folders from the RV_REAL_* variables, or the options above
    ./scripts/gate.ps1 -Gate B -Benchmarks      # also every hidden benchmark, informative
    ./scripts/gate.ps1 -Gate C
    ./scripts/gate.ps1 -Gate C -RealLibrary     # the real library's building and SNES room, ES-DE closed
    ./scripts/gate.ps1 -Gate D
    ./scripts/gate.ps1 -Gate D -RealLibrary -LegacyCli <generator 1.3's retroverse-cli.exe>   # ES-DE closed
    ./scripts/gate.ps1 -Gate D -TimingOnly -RealLibrary     # the timing parts again, on a quiet machine
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('A', 'M1', 'B', 'C', 'D')]
    [string]$Gate,
    [switch]$Clean,
    [switch]$RealLibrary,
    [string]$EsdeHome = $env:RV_REAL_ESDE_HOME,
    [string]$EsdeInstall = $env:RV_REAL_ESDE_INSTALL,
    [string]$RomRoot = $env:RV_REAL_ROM_ROOT,
    [string]$MediaRoot = $env:RV_REAL_MEDIA_ROOT,
    [string]$RetroArchExe = $env:RV_REAL_RETROARCH_EXE,
    [switch]$FullHashPass,
    [switch]$Benchmarks,
    [string]$LegacyCli = $env:RV_LEGACY_CLI,
    [switch]$TimingOnly
)
$ErrorActionPreference = 'Stop'
# Numbers in the summary are formatted with the invariant culture: an Italian Windows would print 1235 s as "1.235 s".
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
$dev = Join-Path $PSScriptRoot 'dev.ps1'
$logDir = Join-Path $root ("build/gate-" + $Gate.ToLowerInvariant())
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$resultsFile = Join-Path $logDir 'results.json'
$focusReport = Join-Path $logDir 'focus_watch.json'

if (($RealLibrary -and $Gate -eq 'A') -or ($FullHashPass -and $Gate -ne 'M1')) {
    Write-Host 'gate: -RealLibrary belongs to -Gate M1, B, C and D, -FullHashPass to -Gate M1' -ForegroundColor Red
    exit 2
}
if ($Benchmarks -and $Gate -notin @('B', 'C', 'D')) {
    Write-Host 'gate: -Benchmarks belongs to -Gate B, C and D' -ForegroundColor Red
    exit 2
}
if ($TimingOnly -and $Gate -ne 'D') {
    Write-Host 'gate: -TimingOnly belongs to -Gate D' -ForegroundColor Red
    exit 2
}
if ($FullHashPass -and -not $RealLibrary) {
    Write-Host 'gate: -FullHashPass needs -RealLibrary' -ForegroundColor Red
    exit 2
}
if ($RealLibrary -and (Get-Process -Name 'ES-DE' -ErrorAction SilentlyContinue)) {
    Write-Host 'gate: ES-DE is running: close it first (it rewrites its gamelists while it runs).' -ForegroundColor Red
    exit 2
}

$results = [System.Collections.Generic.List[object]]::new()

function Add-Result([string]$Step, [string]$Result, [string]$Details, [double]$Seconds, [string]$Limit = '') {
    $results.Add([pscustomobject]@{
            Step    = $Step
            Result  = $Result
            Details = $Details
            Limit   = $Limit
            Time    = '{0:N0} s' -f $Seconds
        })
    Write-Host ('{0,-40} {1,-8} {2}' -f $Step, $Result, $Details)
}

# Prints the summary table and the verdict; returns the exit code (1 when a step failed).
function Write-Summary {
    $failedSteps = @($results | Where-Object { $_.Result -in @('FAILED', 'not run') })
    $skippedSteps = @($results | Where-Object { $_.Result -eq 'skipped' })
    Write-Host ''
    Write-Host "Gate $Gate summary"
    # The GPU and driver the smoke tests ran on, from the run report of the golden comparison.
    $report = Join-Path $root 'build/msvc-x64-debug/smoke/golden_g01/m0_testroom.json'
    if (Test-Path $report) {
        $context = (Get-Content -Raw $report | ConvertFrom-Json).context
        if ($context) { Write-Host "OpenGL: $($context.renderer), $($context.version)" }
    }
    # Details last: Format-Table drops the columns that do not fit, never wraps the last one away.
    $properties = if (@($results | Where-Object { $_.Limit }).Count -gt 0) { 'Step', 'Result', 'Time', 'Limit', 'Details' }
                  else { 'Step', 'Result', 'Time', 'Details' }
    $results | Format-Table -AutoSize -Wrap -Property $properties | Out-String -Width 240 | Write-Host
    $verdict = if ($failedSteps.Count -gt 0) { 'FAILED' }
               elseif ($skippedSteps.Count -gt 0) { 'PASSED with skipped steps' }
               else { 'PASSED' }
    Write-Host ('Gate {0}: {1} ({2} step(s), {3} failed, {4} skipped, {5:N0} s)' -f $Gate, $verdict, $results.Count,
        $failedSteps.Count, $skippedSteps.Count, $gateTimer.Elapsed.TotalSeconds)
    return $(if ($failedSteps.Count -gt 0) { 1 } else { 0 })
}

$gateTimer = [Diagnostics.Stopwatch]::StartNew()

# --- Gates M1, B and C under the focus watcher -------------------------------------------------------------------------
# The outer run starts the gate again under scripts/focus_watch.py, with RV_GATE_INNER set; the inner run does the work
# and leaves its rows in results.json, which the outer one prints with the watcher's verdict.
if ($Gate -in @('M1', 'B', 'C', 'D') -and $env:RV_GATE_INNER -ne '1') {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        Write-Host 'gate: Python 3.10 or later (python) is not on PATH: scripts/focus_watch.py needs it.' -ForegroundColor Red
        exit 2
    }
    Remove-Item -ErrorAction SilentlyContinue $resultsFile, $focusReport
    $arguments = @('-NoProfile', '-File', $PSCommandPath, '-Gate', $Gate)
    if ($Clean) { $arguments += '-Clean' }
    if ($RealLibrary) {
        $arguments += '-RealLibrary'
        # The folders given (the inner run reads the RV_REAL_* variables itself for the others).
        foreach ($folder in @(@('-EsdeHome', $EsdeHome), @('-EsdeInstall', $EsdeInstall), @('-RomRoot', $RomRoot),
                @('-MediaRoot', $MediaRoot), @('-RetroArchExe', $RetroArchExe))) {
            if ($folder[1]) { $arguments += $folder }
        }
    }
    if ($FullHashPass) { $arguments += '-FullHashPass' }
    if ($Benchmarks) { $arguments += '-Benchmarks' }
    if ($LegacyCli) { $arguments += @('-LegacyCli', $LegacyCli) }
    if ($TimingOnly) { $arguments += '-TimingOnly' }
    $shell = (Get-Process -Id $PID).Path
    Write-Host "Gate $Gate under scripts/focus_watch.py (logs in $logDir)"
    $env:RV_GATE_INNER = '1'
    try {
        & $python.Source -I (Join-Path $PSScriptRoot 'focus_watch.py') --watch (Join-Path $root 'build') --tail 2 `
            --out $focusReport '--' $shell @arguments
        $watchCode = $LASTEXITCODE
    }
    finally {
        Remove-Item Env:RV_GATE_INNER -ErrorAction SilentlyContinue
    }
    if (Test-Path $resultsFile) {
        foreach ($row in @(Get-Content -Raw $resultsFile | ConvertFrom-Json)) { $results.Add($row) }
    }
    else {
        Add-Result 'Gate steps' 'FAILED' "the gate's run left no results ($resultsFile)" 0
    }
    if (Test-Path $focusReport) {
        $focus = Get-Content -Raw $focusReport | ConvertFrom-Json
        $counts = $focus.counts
        $findings = @(@($focus.findings.foreground) + @($focus.findings.windows) | Where-Object { $_ })
        $details = '{0} foreground change(s), {1} to test processes; {2} window(s) shown, {3} visible of test processes, {4} hidden or off-screen' -f
            $counts.foregroundChanges, $counts.foregroundChangesToTestProcesses, $counts.windowsShown,
            $counts.visibleWindowsOfTestProcesses, $counts.allowedWindowsOfTestProcesses
        foreach ($finding in $findings) {
            $details += "; $($finding.image) '$($finding.title)' ($($finding.reason))"
        }
        $result = if ($findings.Count -gt 0 -or $watchCode -eq 1) { 'FAILED' } else { 'passed' }
        Add-Result 'Focus watch (whole gate)' $result $details $focus.seconds
    }
    else {
        Add-Result 'Focus watch (whole gate)' 'FAILED' "no report (focus_watch.py exit code $watchCode)" 0
    }
    exit (Write-Summary)
}

# Runs a command inside the developer environment with all its output in $LogFile; returns its exit code.
function Invoke-Logged([string]$LogFile, [string[]]$Command) {
    & $dev @Command *> $LogFile
    return $LASTEXITCODE
}

# Compiler (C4xxx), linker (LNKxxxx), command line (Dxxxx) and CMake warnings in log files.
function Measure-Warnings([string[]]$LogFiles) {
    $existing = @($LogFiles | Where-Object { Test-Path $_ })
    if ($existing.Count -eq 0) { return 0 }
    return @(Select-String -Path $existing -Pattern '\bwarning (C|LNK|D)\d{4}\b|CMake Warning').Count
}

function Invoke-Build([string]$Preset) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $configureLog = Join-Path $logDir "configure-$Preset.log"
    $buildLog = Join-Path $logDir "build-$Preset.log"
    Remove-Item -ErrorAction SilentlyContinue $configureLog, $buildLog
    $code = 0
    if (-not (Test-Path (Join-Path $root "build/$Preset/CMakeCache.txt"))) {
        $code = Invoke-Logged $configureLog @('cmake', '--preset', $Preset)
    }
    if ($code -eq 0) {
        $buildCommand = @('cmake', '--build', '--preset', $Preset)
        if ($Clean) { $buildCommand += '--clean-first' }
        $code = Invoke-Logged $buildLog $buildCommand
    }
    $warnings = Measure-Warnings @($configureLog, $buildLog)
    $passed = $code -eq 0 -and $warnings -eq 0
    $details = if ($code -ne 0) { "exit code $code, see $buildLog" } else { "$warnings warning(s)" }
    Add-Result "Build $Preset" ($(if ($passed) { 'passed' } else { 'FAILED' })) $details $timer.Elapsed.TotalSeconds
    return $passed
}

# The verdict of a set of JUnit test cases: 'FAILED' when one failed, 'skipped' when none passed, else 'passed'.
function Get-CaseCounts($Cases) {
    $cases = @($Cases)
    $failed = @($cases | Where-Object { $_.status -eq 'fail' -or $_.failure -or $_.error }).Count
    $skipped = @($cases | Where-Object { $_.status -eq 'notrun' -or $_.skipped }).Count
    return [pscustomobject]@{ Total = $cases.Count; Failed = $failed; Skipped = $skipped
        Passed = $cases.Count - $failed - $skipped }
}

# Runs ctest with a test preset and extra arguments; the counts come from its JUnit report. Returns the result, with
# the report's test cases for the M1 rows.
function Invoke-Tests([string]$Step, [string]$Name, [string]$Preset, [string[]]$Extra) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $logDir "$Name.log"
    $junit = Join-Path $logDir "$Name.xml"
    Remove-Item -ErrorAction SilentlyContinue $log, $junit
    $code = Invoke-Logged $log (@('ctest', '--preset', $Preset, '--output-junit', $junit) + $Extra)
    if (-not (Test-Path $junit)) {
        Add-Result $Step 'FAILED' "no test results (exit code $code), see $log" $timer.Elapsed.TotalSeconds
        return [pscustomobject]@{ Result = 'FAILED'; Cases = @() }
    }
    [xml]$report = Get-Content -Raw $junit
    $suite = $report.testsuite
    $total = [int]$suite.tests
    $failed = [int]$suite.failures
    $cases = @($suite.testcase)
    $skipped = @($cases | Where-Object { $_.status -eq 'notrun' -or $_.skipped }).Count
    $passedCount = $total - $failed - $skipped
    $result = if ($code -ne 0 -or $failed -gt 0 -or $total -eq 0) { 'FAILED' }
              elseif ($passedCount -eq 0) { 'skipped' }
              else { 'passed' }
    $details = "$passedCount passed, $failed failed, $skipped skipped"
    if ($result -eq 'FAILED') { $details += ", see $log" }
    Add-Result $Step $result $details $timer.Elapsed.TotalSeconds
    return [pscustomobject]@{ Result = $result; Cases = $cases }
}

# The golden comparisons (G01 alone for Gates A and M1, every golden for Gates B and C): one row per golden, its ctest result
# with the verdict its case writes next to its screenshot (G01's in smoke/golden_g01, the rooms' in
# smoke/golden_room/<case>), and a failed row for each golden the gate expects that the run does not report; the run's
# own row stays only when it holds no golden case (a failure before any ran).
function Invoke-Goldens([string]$Preset, [string]$TestPreset) {
    $smokeDir = Join-Path $root "build/$Preset/smoke"
    $verdictOf = { param($id) if ($id -eq 'G01') { Join-Path $smokeDir 'golden_g01/comparison.txt' }
                   else { Join-Path $smokeDir ('golden_room/{0}/comparison.txt' -f $id.ToLowerInvariant()) } }
    Remove-Item -ErrorAction SilentlyContinue (Join-Path $smokeDir 'golden_g01/comparison.txt')
    Get-ChildItem -ErrorAction SilentlyContinue -Path (Join-Path $smokeDir 'golden_room') -Filter comparison.txt -Recurse |
        Remove-Item
    $pattern = if ($Gate -in @('B', 'C', 'D')) { '^gpu\.golden/' } else { '^gpu\.golden/G01:' }
    # The goldens the gate's page promises: one that the run does not report (renamed, untagged, not discovered) is a
    # failed row, never a row that silently disappears.
    $expected = switch ($Gate) {
        'B' { @('G01', 'G02', 'G02s', 'G06', 'G07', 'G08a', 'G08b', 'G10') }
        'C' { @('G01', 'G02', 'G02s', 'G06', 'G07', 'G08a', 'G08b', 'G10', 'G11', 'G12', 'G13') }
        'D' { @('G01', 'G02', 'G02s', 'G06', 'G07', 'G08a', 'G08b', 'G10', 'G11', 'G12', 'G13', 'G14', 'G15', 'G16',
                'G17', 'G18') }
        default { @('G01') }
    }
    $run = Invoke-Tests "Goldens $Preset" "golden-$Preset" $TestPreset @('-R', $pattern)
    $cases = @($run.Cases | Where-Object { $_.name -match '^gpu\.golden/(\w+):' })
    if ($cases.Count -eq 0) { return }
    $results.RemoveAt($results.Count - 1)
    foreach ($case in $cases) {
        $id = [regex]::Match($case.name, '^gpu\.golden/(\w+):').Groups[1].Value
        $counts = Get-CaseCounts $case
        $result = if ($counts.Failed -gt 0) { 'FAILED' } elseif ($counts.Passed -eq 0) { 'skipped' } else { 'passed' }
        $verdictFile = & $verdictOf $id
        $details = if (Test-Path $verdictFile) { (Get-Content -Raw $verdictFile).Trim() }
                   else { "no comparison written, see $(Join-Path $logDir "golden-$Preset.log")" }
        Add-Result "Golden $id $Preset" $result $details ([double]$case.time) '<= 0.5% of the pixels'
    }
    $seen = @($cases | ForEach-Object { [regex]::Match($_.name, '^gpu\.golden/(\w+):').Groups[1].Value })
    foreach ($id in $expected) {
        if ($id -cnotin $seen) {
            Add-Result "Golden $id $Preset" 'FAILED' ("not run: no case gpu.golden/${id}: in the run, see " +
                (Join-Path $logDir "golden-$Preset.log")) 0 '<= 0.5% of the pixels'
        }
    }
}

# A row counting the test cases of an earlier run whose names match $Pattern (the M1 part of a preset's tests).
function Add-CaseRow([string]$Step, $Cases, [string]$Pattern, [string]$What) {
    $matched = @($Cases | Where-Object { $_.name -match $Pattern })
    $counts = Get-CaseCounts $matched
    $result = if ($counts.Total -eq 0 -or $counts.Failed -gt 0) { 'FAILED' }
              elseif ($counts.Passed -eq 0) { 'skipped' }
              else { 'passed' }
    $time = ($matched | ForEach-Object { [double]$_.time } | Measure-Object -Sum).Sum
    Add-Result $Step $result ("{0} passed, {1} failed, {2} skipped ({3})" -f $counts.Passed, $counts.Failed,
        $counts.Skipped, $What) $time
}

# The M1 test cases among the unit tests (ctest names "unit.<test case>"): the catalog's modules, the core primitives
# of I2, the test support, the app's catalog and launches, the quick catalog's source and model.
$m1UnitPattern = '^unit\.(catalog|catalog_db|esde|cli|launcher|platform_win|support)/|' +
    '^unit\.core/(sha256|case_fold|utf8|time|relative_path|tree_walk|paths|system_probe|file_io)|' +
    '^unit\.app/(catalog_|launch_|game_session|mock_catalog|run_report)|^unit\.ui/(catalog_source|quick_catalog)|' +
    '^unit\.input/input_script|^unit\.cmake/cli_process'
# The GPU cases of M1: the ES-DE catalog in the app, the launches from the quick catalog.
$m1GpuPattern = '^gpu\.(catalog|launch):'

# --- The M1 measurements on the real library ---------------------------------------------------------------------------
function Format-Seconds($Value) { if ($null -eq $Value) { 'n/a' } else { '{0:N1} s' -f [double]$Value } }

function Invoke-RealLibrary {
    $preset = 'msvc-x64-release'
    $realDir = Join-Path $logDir 'real-library'
    $measureDir = Join-Path $logDir 'real-measurements'
    $rvTests = Join-Path $root "build/$preset/bin/rv_tests.exe"

    # NFR-02 on 10,000 generated games (the step 9 benchmark; it reads no library, so it runs before the snapshots).
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $benchLog = Join-Path $logDir 'nfr02-bench.log'
    $env:RV_LAUNCH_BACKGROUND = '1'
    & $rvTests 'catalog_db/bench: NFR-02*' --reporter compact *> $benchLog
    $benchCode = $LASTEXITCODE
    $bench = Select-String -Path $benchLog -Pattern '(\d+) queries materialized .*p50 ([\d.]+) ms, p95 ([\d.]+) ms, max ([\d.]+) ms' |
        Select-Object -First 1
    if ($bench) {
        $g = $bench.Matches[0].Groups
        $p95 = [double]$g[3].Value
        Add-Result 'NFR-02 benchmark, 10,000 games' ($(if ($benchCode -eq 0 -and $p95 -le 100) { 'passed' } else { 'FAILED' })) `
            ("p95 {0:N2} ms (p50 {1} ms, max {2} ms) over {3} indexed queries" -f $p95, $g[2].Value, $g[4].Value,
                $g[1].Value) $timer.Elapsed.TotalSeconds '<= 100 ms'
    }
    else {
        Add-Result 'NFR-02 benchmark, 10,000 games' 'FAILED' "no figure (exit code $benchCode), see $benchLog" $timer.Elapsed.TotalSeconds '<= 100 ms'
    }

    # The library between two snapshots: the measurements, then the [real-library] tests. The outputs of an earlier
    # run go first: a run that stops before writing its own must not show the old figures as this run's.
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $realLog = Join-Path $logDir 'real-library.log'
    $testLog = Join-Path $realDir 'tests.log'
    Remove-Item -LiteralPath (Join-Path $measureDir 'figures.json'), $testLog -ErrorAction SilentlyContinue
    $measureArguments = @{ Preset = $preset; OutDir = $measureDir }
    if ($FullHashPass) { $measureArguments.FullHashPass = $true }
    & (Join-Path $PSScriptRoot 'real-library.ps1') -EsdeHome $EsdeHome -EsdeInstall $EsdeInstall -RomRoot $RomRoot `
        -MediaRoot $MediaRoot -RetroArchExe $RetroArchExe -Preset $preset -OutDir $realDir -Measure (Join-Path $PSScriptRoot 'gate-m1-real.ps1') `
        -MeasureArguments $measureArguments *> $realLog
    $realCode = $LASTEXITCODE
    $realSeconds = $timer.Elapsed.TotalSeconds
    $firstRow = $results.Count
    Add-RealLibraryRows $preset $realLog $testLog $measureDir $realCode $realSeconds
    # real-library.ps1's exit code counts on its own: a failure no row explains still fails the gate.
    if ($realCode -ne 0 -and -not (@($results | Select-Object -Skip $firstRow) | Where-Object { $_.Result -eq 'FAILED' })) {
        Add-Result 'Real library' 'FAILED' "scripts/real-library.ps1 exited $realCode, see $realLog" 0
    }
}

# The rows of the real library's run: the snapshots, the [real-library] tests, the figures of the measurement script.
# The rows every real-library run gives: the snapshots before and after, the tests' verdict. False when
# real-library.ps1 could not run (its row says so).
function Add-RealLibraryCheckRows([string]$preset, [string]$realLog, [string]$testLog, [int]$realCode,
        [double]$realSeconds, [string]$Tests) {
    $compare = Select-String -Path $realLog -Pattern '(\d+) difference\(s\) among (\d+) entries' | Select-Object -Last 1
    if ($realCode -eq 2 -or -not $compare) {
        Add-Result 'Real library' 'FAILED' "could not run (exit code $realCode), see $realLog" $realSeconds
        return $false
    }
    $differences = [int]$compare.Matches[0].Groups[1].Value
    Add-Result 'Real library: snapshots (NFR-10)' ($(if ($differences -eq 0) { 'passed' } else { 'FAILED' })) `
        ("{0} differences among {1} entries (ES-DE home and install hashed, ROM and media roots listed)" -f $differences,
            $compare.Matches[0].Groups[2].Value) $realSeconds '0 differences'

    # The tests (Catch2's compact reporter ends with its totals).
    $totals = if (Test-Path $testLog) {
        Select-String -Path $testLog -Pattern '^(test cases:|All tests passed|No tests ran)' | Select-Object -Last 1
    }
    $testsPassed = [bool](Select-String -Path $realLog -Pattern '^Tests:\s+passed' -Quiet)
    Add-Result "Real library: $Tests tests $preset" ($(if ($testsPassed) { 'passed' } else { 'FAILED' })) `
        ($(if ($totals) { $totals.Line.Trim() } else { "no totals, see $testLog" })) 0
    return $true
}

function Add-RealLibraryRows([string]$preset, [string]$realLog, [string]$testLog, [string]$measureDir, [int]$realCode,
        [double]$realSeconds) {
    if (-not (Add-RealLibraryCheckRows $preset $realLog $testLog $realCode $realSeconds '[real-library]')) { return }

    # The figures of the measurement script.
    $figuresFile = Join-Path $measureDir 'figures.json'
    if (-not (Test-Path $figuresFile)) {
        Add-Result 'Real library: measurements' 'FAILED' "no figures, see $realLog" 0
        return
    }
    $figures = Get-Content -Raw $figuresFile | ConvertFrom-Json
    $failures = @($figures.failures)
    # real-library.ps1's verdict on the measurement script, read like the tests' ("Measurements: passed").
    $measured = Select-String -Path $realLog -Pattern '^Measurements:\s+(.*)$' | Select-Object -Last 1
    if ($failures.Count -gt 0) {
        Add-Result 'Real library: measurements' 'FAILED' ($failures -join '; ') 0
    }
    elseif (-not $measured -or $measured.Matches[0].Groups[1].Value.Trim() -ne 'passed') {
        Add-Result 'Real library: measurements' 'FAILED' ("{0}, see $realLog" -f
            $(if ($measured) { $measured.Line.Trim() } else { 'no verdict on the measurements' })) 0
    }
    $nfr01 = @($figures.nfr01 | Where-Object { $null -ne $_.firstPageMs })
    if ($nfr01.Count -gt 0) {
        $pages = @($nfr01 | ForEach-Object { [double]$_.firstPageMs })
        $worst = ($pages | Measure-Object -Maximum).Maximum
        $first = $nfr01[0]
        Add-Result 'NFR-01 start to the first page' ($(if ($pages.Count -eq 3 -and $worst -lt 5000) { 'passed' } else { 'FAILED' })) `
            ("{0} ms over 3 runs ({1} games; catalog opened {2} ms, first list {3} ms in run 1)" -f
                (($pages | ForEach-Object { '{0:N0}' -f $_ }) -join ', '), $first.games, ('{0:N0}' -f [double]$first.openedMs),
                ('{0:N0}' -f [double]$first.firstListMs)) 0 '< 5,000 ms'
    }
    else {
        Add-Result 'NFR-01 start to the first page' 'FAILED' 'no first page shown' 0 '< 5,000 ms'
    }
    $real02 = if (Test-Path $testLog) {
        Select-String -Path $testLog -Pattern 'NFR-02 on the real catalog: (\d+) games listed, (\d+) indexed queries .*p50 ([\d.]+) ms, p95 ([\d.]+) ms, max ([\d.]+) ms; the default list ([\d.]+) ms; (\d+) pages of (\d+) rows: p50 ([\d.]+) ms, p95 ([\d.]+) ms' |
            Select-Object -First 1
    }
    if ($real02) {
        $g = $real02.Matches[0].Groups
        Add-Result 'NFR-02 on the real catalog' ($(if ([double]$g[4].Value -le 100) { 'passed' } else { 'FAILED' })) `
            ("p95 {0} ms (p50 {1}, max {2}) over {3} indexed queries on {4} games; a page of {5} rows p95 {6} ms" -f
                $g[4].Value, $g[3].Value, $g[5].Value, $g[2].Value, $g[1].Value, $g[8].Value, $g[10].Value) 0 '<= 100 ms'
    }
    else {
        Add-Result 'NFR-02 on the real catalog' 'FAILED' "no figure in $testLog" 0 '<= 100 ms'
    }
    $scan = $figures.romScan
    if ($scan) {
        Add-Result 'ROM scan (import --dry-run)' 'info' `
            ("{0} wall: listing {1}, gamelists {2}, sheets {3} ({4} read); {5} entries, {6} games, {7} files listed; first read after the snapshot, {8} h after the boot" -f
                (Format-Seconds $scan.wallSeconds), (Format-Seconds $scan.scanSeconds), (Format-Seconds $scan.gamelistSeconds),
                (Format-Seconds $scan.sheetSeconds), $scan.sheetsRead, $scan.entries, $scan.games, $scan.filesListed,
                $figures.uptimeHours) $scan.wallSeconds
    }
    foreach ($import in @(@('First import', $figures.firstImport), @('Second import', $figures.secondImport),
            @('db rebuild-index', $figures.rebuildIndex))) {
        $name, $figure = $import
        if (-not $figure) { continue }
        $writes = "{0} identities, {1} entries written, {2} removed, {3} disc groups" -f $figure.identitiesWritten,
            $figure.entriesWritten, $figure.entriesRemoved, $figure.discGroupsWritten
        $result = if ($figure.exitCode -ne 0) { 'FAILED' }
                  elseif ($name -eq 'Second import') {
                      $(if ($figure.identitiesWritten -eq 0 -and $figure.entriesWritten -eq 0 -and $figure.entriesRemoved -eq 0 -and
                            $figure.discGroupsWritten -eq 0) { 'passed' } else { 'FAILED' })
                  }
                  else { 'info' }
        Add-Result $name $result ("{0} wall ({1} in the import: read {2}, media {3}, writes {4}); {5}" -f
            (Format-Seconds $figure.wallSeconds), (Format-Seconds $figure.seconds), (Format-Seconds $figure.loadSeconds),
            (Format-Seconds $figure.mediaSeconds), (Format-Seconds $figure.writeSeconds), $writes) $figure.wallSeconds `
            $(if ($name -eq 'Second import') { 'nothing written' } else { '' })
    }
    $coverage = $figures.mediaCoverage
    if ($coverage) {
        $kinds = @($coverage.percentPerKind.PSObject.Properties | ForEach-Object { "$($_.Name) $($_.Value)%" }) -join ', '
        Add-Result 'Media coverage (media --coverage)' ($(if ($coverage.exitCode -in @(0, 1)) { 'info' } else { 'FAILED' })) `
            ("{0} wall; {1} of {2} games with a medium ({3}%): {4}; {5} issue(s), exit code {6}" -f
                (Format-Seconds $coverage.wallSeconds), $coverage.gamesWithAny, $coverage.games, $coverage.withAnyPercent,
                $kinds, $coverage.issues, $coverage.exitCode) $coverage.wallSeconds
    }
    $hash = if (Test-Path $testLog) { Select-String -Path $testLog -Pattern '^quick-hash pass, gb: .*' | Select-Object -First 1 } else { $null }
    $estimate = if (Test-Path $testLog) { Select-String -Path $testLog -Pattern '^whole library: .*' | Select-Object -First 1 } else { $null }
    if ($hash) {
        Add-Result 'Quick-hash pass, sample (gb)' 'info' `
            ($hash.Line.Trim() + $(if ($estimate) { '; ' + $estimate.Line.Trim() } else { '' })) 0
    }
    else {
        Add-Result 'Quick-hash pass, sample (gb)' 'FAILED' "no figure in $testLog" 0
    }
    if ($FullHashPass) {
        $full = $figures.fullHashPass
        $fullResult = $full.result
        Add-Result 'Quick-hash pass, whole library' ($(if ($full -and $full.exitCode -in @(0, 1)) { 'info' } else { 'FAILED' })) `
            ("{0} wall; {1} files hashed of {2}, {3:N1} MiB read, {4} left, exit code {5}" -f (Format-Seconds $full.wallSeconds),
                $fullResult.hashed, $fullResult.candidates, ([double]$fullResult.bytesRead / 1MB), $fullResult.remaining,
                $full.exitCode) $full.wallSeconds
    }
    else {
        Add-Result 'Quick-hash pass, whole library' 'off' 'not run: -FullHashPass runs it (about 45 minutes cold)' 0
    }
}

# --- Gate B: the I3 checks -----------------------------------------------------------------------------------------------
# The I3 test cases among the unit tests (the world's domain, generator, validation, update and their snapshot goldens
# and property suites; the room in the app; covers and texture arrays; the interaction, the card, the screens) and
# among the GPU cases (the room in retroverse.exe, its covers, focus, inspection, card, launch, live update and look,
# the media viewer, the refresh, the texture arrays and static groups, the measurements, the hero CRT's video).
$i3UnitPattern = '^unit\.(world|app/(world|room|collection|cover|hero|screen_policy|frame_timing|library_refresh)|' +
    'assets/(cover|collection|bc1|rvtex|derived_texture|families|mip_chain|text_raster)|' +
    'scene/(instance.clusters|pose_animator|outline)|render/(gpu_memory|texture_array|texture_memory)|' +
    'geometry/collection|ui/game_card|core/(seed|random|canonical_json|worker_pool|backups|cpu_timers)|catalog_db/room|' +
    'cli/(worldplan|where-is)|esde/snes_fixtures|integration/room_walk|game screens|media viewer|Informazioni)'
$i3GpuPattern = '^gpu\.(world|covers|media viewer|refresh|collection arrays|static groups|measurements|video)'

# The GPU passes the benchmark path reports (informative: the frame times are what must be acceptable).
$passNames = @('shadow', 'depth', 'ssao', 'scene', 'post', 'outline', 'total')

function Read-Report([string]$File) {
    if (Test-Path -LiteralPath $File) { return Get-Content -Raw -LiteralPath $File | ConvertFrom-Json }
    return $null
}

function Get-Phase($Report, [string]$Name) {
    if (-not $Report) { return $null }
    return @($Report.phases | Where-Object { $_.name -eq $Name }) | Select-Object -First 1
}

function Format-Ms($Value) { if ($null -eq $Value) { 'n/a' } else { '{0:N2} ms' -f [double]$Value } }
function Format-MiB($Bytes) { '{0:N1} MiB' -f ([double]$Bytes / 1MB) }

# The benchmark path's rows from a run report: the "benchmark" phase's frame times and the GPU memory against the
# acceptable limits (docs/roadmap.md, "Acceptable performance"), the GPU passes at their p50 and p99 for information.
function Add-BenchmarkRows([string]$Label, [string]$ReportFile) {
    $report = Read-Report $ReportFile
    $phase = Get-Phase $report 'benchmark'
    if (-not $phase -or [int]$phase.count -eq 0) {
        Add-Result "$Label, frame times" 'FAILED' "no benchmark phase in $ReportFile" 0 'p99 <= 33.3 ms, p50 <= 16.7 ms'
        return
    }
    $framesOk = [double]$phase.p99Ms -le 33.3 -and [double]$phase.p50Ms -le 16.7
    Add-Result "$Label, frame times" ($(if ($framesOk) { 'passed' } else { 'FAILED' })) `
        ('p50 {0}, p95 {1}, p99 {2}, max {3} over {4} frames ({5:N0} FPS at p99)' -f (Format-Ms $phase.p50Ms),
            (Format-Ms $phase.p95Ms), (Format-Ms $phase.p99Ms), (Format-Ms $phase.maxMs), $phase.count,
            (1000.0 / [double]$phase.p99Ms)) 0 'p99 <= 33.3 ms, p50 <= 16.7 ms'
    $texts = @()
    foreach ($name in $passNames) {
        # The outline is drawn only while an item is hovered: the path need not hover one.
        $pass = @($phase.gpuPasses | Where-Object { $_.name -eq $name }) | Select-Object -First 1
        $texts += $(if ($pass) { '{0} {1:N3}/{2:N3}' -f $name, [double]$pass.p50Ms, [double]$pass.p99Ms } else { "$name not drawn" })
    }
    Add-Result "$Label, GPU per pass" 'info' ('p50/p99 ms: ' + ($texts -join ', ')) 0
    $arrays = [double]$report.residency.arrayBytes
    $peak = [double]$report.gpuMemory.peakTotalBytes
    Add-Result "$Label, GPU memory" ($(if ($peak -gt 0 -and $peak -le 512MB) { 'passed' } else { 'FAILED' })) `
        ('collection arrays {0} ({1}, {2} items), all textures at most {3}' -f (Format-MiB $arrays),
            $report.residency.format, $report.residency.items, (Format-MiB $peak)) 0 'all <= 512 MiB'
}

# Runs a Catch2 executable of the Release build with a test spec (hidden cases included), its XML report and log in
# build/gate-b; one row with the counts (and the failed cases' names); returns the XML's test cases.
function Invoke-Catch([string]$Step, [string]$Name, [string]$Executable, [string]$Spec) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $logDir "$Name.log"
    $xmlFile = Join-Path $logDir "$Name.xml"
    Remove-Item -ErrorAction SilentlyContinue $log, $xmlFile
    $env:RV_LAUNCH_BACKGROUND = '1'
    & $Executable $Spec --durations yes --reporter "xml::out=$xmlFile" --reporter console *> $log
    $code = $LASTEXITCODE
    if (-not (Test-Path $xmlFile)) {
        Add-Result $Step 'FAILED' "no test results (exit code $code), see $log" $timer.Elapsed.TotalSeconds
        return @()
    }
    [xml]$xml = Get-Content -Raw $xmlFile
    $cases = @($xml.DocumentElement.TestCase)
    $failedCases = @($cases | Where-Object { $_.OverallResult.success -ne 'true' })
    $skipped = @($cases | Where-Object { $_.OverallResult.success -eq 'true' -and [int]$_.OverallResult.skips -gt 0 }).Count
    $result = if ($code -ne 0 -or $failedCases.Count -gt 0 -or $cases.Count -eq 0) { 'FAILED' } else { 'passed' }
    $details = '{0} passed, {1} failed, {2} skipped' -f ($cases.Count - $failedCases.Count - $skipped), $failedCases.Count, $skipped
    if ($failedCases.Count -gt 0) { $details += ': ' + (($failedCases | ForEach-Object { $_.name }) -join '; ') }
    if ($result -eq 'FAILED') { $details += ", see $log" }
    Add-Result $Step $result $details $timer.Elapsed.TotalSeconds
    return $cases
}

# The benchmark path in Release against the acceptable limits; with -Benchmarks also every hidden benchmark, whose
# rows are informative: a slow figure (a budget the case checks) is reported, never a failure of the gate.
function Invoke-Benchmarks {
    $bin = Join-Path $root 'build/msvc-x64-release/bin'
    $first = $results.Count
    $spec = '[benchmark]'
    if ($Benchmarks) {
        Invoke-Catch 'Benchmarks, GPU-free (Release)' 'bench-unit' (Join-Path $bin 'rv_tests.exe') '[bench]' | Out-Null
        $spec = '[bench]'
    }
    $cases = Invoke-Catch 'Benchmarks, GPU (Release)' 'bench-gpu' (Join-Path $bin 'rv_smoke_tests.exe') $spec
    # The figures each case printed (WARN), a row per case.
    $pathFailed = $false
    foreach ($case in $cases) {
        $warnings = @($case.SelectNodes('.//Warning') | ForEach-Object { ($_.InnerText -replace '\s+', ' ').Trim() })
        $result = if ($case.OverallResult.success -ne 'true') { 'FAILED' } else { 'passed' }
        $isPath = $case.name -like 'world benchmark: the benchmark path*'
        if ($result -eq 'FAILED' -and $isPath) { $pathFailed = $true }
        if ($result -eq 'FAILED' -and -not $isPath) { $result = 'info' }
        Add-Result ('  ' + $case.name) $result ($warnings -join ' | ') ([double]$case.OverallResult.durationInSeconds)
    }
    # The suites' rows count every failed case: only the benchmark path's failure (or a suite that did not run) fails.
    foreach ($row in @($results | Select-Object -Skip $first)) {
        if ($row.Result -ne 'FAILED' -or $row.Details -match 'no test results') { continue }
        if (($row.Step -eq 'Benchmarks, GPU (Release)' -and $pathFailed) -or
            $row.Step -like '  world benchmark: the benchmark path*') { continue }
        $row.Result = 'info'
    }
    Add-BenchmarkRows 'Benchmark path, snes-stress (2,000)' `
        (Join-Path $root 'build/msvc-x64-release/smoke/world_benchmark/stress/benchmark.json')
}

# The real SNES room through scripts/real-library.ps1 (tests/unit/app/test_room_real.cpp, [real-library][room-real]),
# its figures read from the run reports the test leaves in build/msvc-x64-release/real-library/room.
function Invoke-RealRoom {
    $preset = 'msvc-x64-release'
    $realDir = Join-Path $logDir 'real-library'
    $roomDir = Join-Path $root "build/$preset/real-library/room"
    $realLog = Join-Path $logDir 'real-library.log'
    $testLog = Join-Path $realDir 'tests.log'
    # An earlier run's reports must not stand for this run's.
    Remove-Item -LiteralPath $testLog -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $roomDir -Filter '*.json' -ErrorAction SilentlyContinue | Remove-Item
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & (Join-Path $PSScriptRoot 'real-library.ps1') -EsdeHome $EsdeHome -EsdeInstall $EsdeInstall -RomRoot $RomRoot `
        -MediaRoot $MediaRoot -RetroArchExe $RetroArchExe -Preset $preset -OutDir $realDir `
        -Filter '[real-library][room-real]' *> $realLog
    $realCode = $LASTEXITCODE
    $firstRow = $results.Count
    if (Add-RealLibraryCheckRows $preset $realLog $testLog $realCode $timer.Elapsed.TotalSeconds '[room-real]') {
        Add-RealRoomRows $roomDir $testLog
    }
    if ($realCode -ne 0 -and -not (@($results | Select-Object -Skip $firstRow) | Where-Object { $_.Result -eq 'FAILED' })) {
        Add-Result 'Real library' 'FAILED' "scripts/real-library.ps1 exited $realCode, see $realLog" 0
    }
}

# The real SNES room's rows (Gates B and C): the reports [real-library][room-real] leaves in $roomDir.
function Add-RealRoomRows([string]$roomDir, [string]$testLog) {
    $cold = Read-Report (Join-Path $roomDir 'cold.json')
    $again = Read-Report (Join-Path $roomDir 'again.json')
    $restart = Read-Report (Join-Path $roomDir 'restart.json')
    $refresh = Read-Report (Join-Path $roomDir 'refresh.json')
    if (-not ($cold -and $again -and $restart)) {
        Add-Result 'Real room' 'FAILED' "the runs' reports are missing, see $testLog" 0
    }
    else {
        $w = $cold.world
        $same = $w.planDigest -and $w.planDigest -eq $again.world.planDigest -and $w.planDigest -eq $restart.world.planDigest
        Add-Result 'Real room: the plan generated twice' ($(if ($same) { 'passed' } else { 'FAILED' })) `
            ('{0} placements ({1} featured, {2} band, {3} spine), {4} unplaced; {5} lights, {6} clusters; one plan digest in the cold, again and restart runs (the test compares the files'' SHA-256; never written here: it derives from the user''s titles)' -f
                $w.placements.total, $w.placements.featured, $w.placements.band, $w.placements.spine,
                $w.unplaced.total, $w.lights, $w.clusters) 0 'one SHA-256'
        $r = $restart.world
        $moved = if ($r.reconcile) { [int]$r.reconcile.moved } else { 0 }
        $restartOk = [string]$r.outcome -in @('unchanged', 'updated') -and [string]$r.rebuild -in @('', 'no-rebuild') -and $moved -eq 0
        Add-Result 'Real room: restart after a second import' ($(if ($restartOk) { 'passed' } else { 'FAILED' })) `
            ('outcome {0}, rebuild {1}, plan written {2}; reconciliation {3}: {4} kept, {5} moved' -f $r.outcome,
                $(if ($r.rebuild) { $r.rebuild } else { '-' }), $r.planWritten, $r.reconcile.kind, $r.reconcile.kept,
                $moved) 0 'NoRebuild, 0 moves'
        $shown = [double]$restart.residency.times.shownAtMs
        Add-Result 'Real room: time to the room' ($(if ($shown -gt 0 -and $shown -le 5000) { 'passed' } else { 'FAILED' })) `
            ('shown {0} (restart, the stored plan), {1} (cold: the plan generated), {2} (again) after the process started' -f
                (Format-Ms $restart.residency.times.shownAtMs), (Format-Ms $cold.residency.times.shownAtMs),
                (Format-Ms $again.residency.times.shownAtMs)) 0 'restart <= 5 s (NFR-01)'
        $coldMs = $cold.residency.times.residentAfterShownMs
        Add-Result 'Real room: covers resident, cold' ($(if ($null -ne $coldMs -and [double]$coldMs -le 30000) { 'passed' } else { 'FAILED' })) `
            ('{0} after the room showed ({1} items; {2} cache misses, {3} stored); coverLoad p99 {4}' -f
                (Format-Ms $coldMs), $cold.residency.items, $cold.residency.cache.misses, $cold.residency.cache.stored,
                (Format-Ms (Get-Phase $cold 'coverLoad').p99Ms)) 0 '<= 30 s'
        $warm = @($again.residency.times.residentAfterShownMs, $restart.residency.times.residentAfterShownMs)
        $warmOk = @($warm | Where-Object { $null -eq $_ -or [double]$_ -gt 3000 }).Count -eq 0
        Add-Result 'Real room: covers resident, warm' ($(if ($warmOk) { 'passed' } else { 'FAILED' })) `
            ('{0} (the plan generated again), {1} (restart) after the room showed ({2} cache hits)' -f
                (Format-Ms $warm[0]), (Format-Ms $warm[1]), $restart.residency.cache.hits) 0 '<= 3 s'
        $loads = @(@($cold, $again, $restart) | ForEach-Object { Get-Phase $_ 'coverLoad' } | Where-Object { $_ -and [int]$_.count -gt 0 })
        $loadP99 = ($loads | ForEach-Object { [double]$_.p99Ms } | Measure-Object -Maximum).Maximum
        Add-Result 'Real room: frames while covers load' ($(if ($loads.Count -gt 0 -and $loadP99 -le 50) { 'passed' } else { 'FAILED' })) `
            ('coverLoad p99 {0} (cold), {1} (again), {2} (restart)' -f (Format-Ms (Get-Phase $cold 'coverLoad').p99Ms),
                (Format-Ms (Get-Phase $again 'coverLoad').p99Ms), (Format-Ms (Get-Phase $restart 'coverLoad').p99Ms)) 0 'p99 <= 50 ms'
    }
    Add-BenchmarkRows 'Real room: benchmark path' (Join-Path $roomDir 'benchmark.json')
    $phase = Get-Phase $refresh 'refresh'
    if ($refresh -and $phase -and $refresh.catalog) {
        $record = @($refresh.catalog.refreshes)[0]
        $live = @($refresh.world.liveUpdates)
        $liveMoved = ($live | ForEach-Object { [int]$_.moved } | Measure-Object -Sum).Sum
        Add-Result 'Real room: refresh of the library' ($(if ($record.outcome -eq 'done' -and [double]$phase.p99Ms -le 50 -and $live.Count -gt 0 -and $liveMoved -eq 0) { 'passed' } else { 'FAILED' })) `
            ('refresh phase {0} frames p50 {1}, p99 {2}, max {3}; the import {4} in {5} ({6} entries, {7} added); the room''s live update {8}, {9} moved' -f
                $phase.frames, (Format-Ms $phase.p50Ms), (Format-Ms $phase.p99Ms), (Format-Ms $phase.maxMs), $record.outcome,
                (Format-Ms $record.times.ms), $record.entries, $record.added, (@($live | ForEach-Object { $_.kind }) -join ', '),
                $liveMoved) 0 'p99 <= 50 ms, 0 moves'
        $page = $refresh.catalog.times.firstPageMs
        # The stored room shows first (the persisted start's preview); since I4 the refresh's building then enters the
        # library's other systems, whose floor rebuilt around the room is the last shown.
        $preview = if ($refresh.world.preview) { $refresh.world.preview.committedAtMs } else { $null }
        Add-Result 'NFR-01 with the room' ($(if ($null -ne $page -and [double]$page -lt 5000) { 'passed' } else { 'FAILED' })) `
            ('the quick catalog''s first page {0} after the process started ({1} games; the stored room shown at {2}, the floor last shown at {3})' -f
                (Format-Ms $page), $refresh.catalog.games, (Format-Ms $preview),
                (Format-Ms $refresh.residency.times.shownAtMs)) 0 '< 5,000 ms'
    }
    else {
        Add-Result 'Real room: refresh of the library' 'FAILED' "no refresh report, see $testLog" 0 'p99 <= 50 ms, 0 moves'
    }
}

# --- Gate C: the I4 checks -----------------------------------------------------------------------------------------------
# The I4 test cases among the unit tests (ctest names "unit.<case>") and the GPU cases ("gpu.<case>"), by area. A case
# counts in one area only; the rows are counts of the runs' results (the GPU cases of the I3 room, its goldens and the
# I4 goldens have their own rows).
$i4Areas = [ordered]@{
    'world building' = '^unit\.(world/(building|building plan|lift|ids|families):|assets/families:|' +
        'esde/building_fixture:|cli/worldplan|cli/where-is:|world/where-is:)|^gpu\.world families:'
    'floors'         = '^unit\.(world/floor:|scene/areas:|render/light_packing:)|^gpu\.world floor:'
    'shared rooms'   = '^unit\.(world/shared rooms:|app/room_screen: a shared room)|' +
        '^gpu\.world floor app: the arcade room and a floor''s shared room'
    'update suite'   = '^unit\.world/(building update|building suite):'
    'directory'      = '^unit\.(ui/directory:|app/directory_data:|catalog/recent_games:|catalog_db/recent_query:)|' +
        '^gpu\.directory:'
    'elevator'       = '^unit\.(app/elevator_ride:|app/elevator_prompt_document:|app/world_build: a ride''s floor|' +
        'app/run_report: the floors, the rides)|^gpu\.world elevator:'
    'floor app'      = '^unit\.(app/world_build: (?!a ride''s floor|an unchanged start)|app/room_snapshot:|' +
        'app/room_items:|app/cover_requests: (on a floor|a room''s items))|' +
        '^gpu\.world floor app: (two rooms|a walk|without --start-room)'
    'start and live' = '^unit\.(app/world_flow:|app/world_live_plan:|app/world_reconcile:|app/screen_room:|' +
        'app/world_build: an unchanged start|app/room_screen: the room)|' +
        '^gpu\.world floor app: (a stored floor|a system entering|the hero CRT rebound|from the lobby|' +
        'the quick catalog''s selection)'
}

# A row per area of $Areas (I4's or I6's): its unit and GPU cases of the preset's runs.
function Add-AreaRows([string]$Label, $Areas, [string]$Preset, $UnitCases, $GpuCases) {
    foreach ($area in $Areas.Keys) {
        $pattern = $Areas[$area]
        $unit = @($UnitCases | Where-Object { $_.name -match $pattern })
        $gpu = @($GpuCases | Where-Object { $_.name -match $pattern })
        $counts = Get-CaseCounts (@($unit) + @($gpu))
        $result = if ($counts.Total -eq 0 -or $counts.Failed -gt 0) { 'FAILED' }
                  elseif ($counts.Passed -eq 0) { 'skipped' }
                  else { 'passed' }
        $time = (@($unit) + @($gpu) | ForEach-Object { [double]$_.time } | Measure-Object -Sum).Sum
        $failedNames = @(@($unit) + @($gpu) | Where-Object { $_.status -eq 'fail' -or $_.failure -or $_.error } |
                ForEach-Object { $_.name })
        $details = '{0} passed, {1} failed, {2} skipped ({3} unit, {4} GPU)' -f $counts.Passed, $counts.Failed,
            $counts.Skipped, $unit.Count, $gpu.Count
        if ($failedNames.Count -gt 0) { $details += ': ' + ($failedNames -join '; ') }
        Add-Result "$Label $area $Preset" $result $details $time
    }
}

# --- Gate D: the I6 checks -----------------------------------------------------------------------------------------------
# The I6 test cases among the unit and GPU runs, by area (the step that made them; docs/i6/step-*.md). A case counts in
# one I6 area only, and may count in an I4 row too (the floors, the families, the light packing grew at I6); the goldens
# have their own rows.
$i6Areas = [ordered]@{
    'light'            = '^unit\.(scene/areas:|render/light_packing:|world/room_look:|' +
        'world/floor: (a full floor: the hall facing|the review''s cases))|' +
        '^gpu\.world floor: light reaches through the doorways'
    'halls'            = '^unit\.(world/floor: (the hall''s|a narrow hall|a longer hall appends its dressing)|' +
        'assets/surface_textures:|assets/test_patterns: a hall''s print)'
    'theme packs'      = '^unit\.(world/theme packs:|cli/worldplan: a stale theme pack|' +
        'world/building plan: a stored room''s recorded family is authoritative, its theme pack|' +
        'assets/test_patterns: neon|geometry/prefabs: a shelf lit inside|' +
        'render/gpu_memory: (the procedural surfaces|a metallic-roughness map))|' +
        '^gpu\.world look:'
    'spines'           = '^unit\.(world/families: cases|world/migration:|geometry/collection: a case''s side spine|' +
        'assets/collection_labels: a printed spine|cli/worldplan update: generator 1\.3)|' +
        '^gpu\.world families: (jewel cases stand|a stored building of generator 1\.3)'
    'era furniture'    = '^unit\.(world/era furniture:|geometry/prefabs: the era|assets/test_patterns: the era)'
    'hub'              = '^unit\.(world/hub:|app/hub_showcase:|app/elevator_prompt_document: the totem|' +
        'ui/directory: openGame|geometry/prefabs: the hub|geometry/mesh_builder: torus|' +
        'assets/test_patterns: the hub)|^gpu\.world hub:'
    'arcade handhelds' = '^unit\.(world/families: the arcade|geometry/prefabs: (the arcade|the handhelds|the review of I6)|' +
        'assets/test_patterns: an arcade)|^gpu\.world families: (the arcade room''s cabinets|a game launched from its cabinet)'
}

# The real library's building (tests/unit/app/test_building_real.cpp, [real-library][building]) and I3's SNES room
# ([real-library][room-real]) through one scripts/real-library.ps1 run (one pair of snapshots), their figures read from
# the run reports the tests leave in build/msvc-x64-release/real-library/building and .../room.
# Gate D (-Migration): the same run with generator 1.3's CLI named (RV_LEGACY_CLI, from -LegacyCli), so that the
# migrations' case ([real-library][building][migration]) runs too; its rows read its reports in .../migration.
function Invoke-RealBuilding([switch]$Migration) {
    $preset = 'msvc-x64-release'
    $realDir = Join-Path $logDir 'real-library'
    $buildingDir = Join-Path $root "build/$preset/real-library/building"
    $roomDir = Join-Path $root "build/$preset/real-library/room"
    $migrationDir = Join-Path $root "build/$preset/real-library/migration"
    $realLog = Join-Path $logDir 'real-library.log'
    $testLog = Join-Path $realDir 'tests.log'
    # An earlier run's reports must not stand for this run's.
    Remove-Item -LiteralPath $testLog -ErrorAction SilentlyContinue
    foreach ($dir in @($buildingDir, $roomDir, $migrationDir)) {
        Get-ChildItem -LiteralPath $dir -Filter '*.json' -ErrorAction SilentlyContinue | Remove-Item
    }
    $savedLegacy = $env:RV_LEGACY_CLI
    if ($Migration -and $LegacyCli) { $env:RV_LEGACY_CLI = $LegacyCli }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        & (Join-Path $PSScriptRoot 'real-library.ps1') -EsdeHome $EsdeHome -EsdeInstall $EsdeInstall -RomRoot $RomRoot `
            -MediaRoot $MediaRoot -RetroArchExe $RetroArchExe -Preset $preset -OutDir $realDir `
            -Filter '[real-library][building],[real-library][room-real]' *> $realLog
        $realCode = $LASTEXITCODE
    }
    finally {
        $env:RV_LEGACY_CLI = $savedLegacy
    }
    $firstRow = $results.Count
    if (Add-RealLibraryCheckRows $preset $realLog $testLog $realCode $timer.Elapsed.TotalSeconds '[building] and [room-real]') {
        Add-RealBuildingRows $buildingDir $testLog
        if ($Migration) { Add-RealMigrationRows $migrationDir $testLog }
        Add-RealRoomRows $roomDir $testLog
    }
    if ($realCode -ne 0 -and -not (@($results | Select-Object -Skip $firstRow) | Where-Object { $_.Result -eq 'FAILED' })) {
        Add-Result 'Real library' 'FAILED' "scripts/real-library.ps1 exited $realCode, see $realLog" 0
    }
}

function Format-Seconds2($Ms) { if ($null -eq $Ms) { 'n/a' } else { '{0:N2} s' -f ([double]$Ms / 1000.0) } }

# The real building's rows: the reports of its runs (first, again, second, whole, third, busiest-cold, busiest-warm,
# ride-cold, ride-warm) against the acceptable limits (docs/roadmap.md; ADR-0013 §13).
function Add-RealBuildingRows([string]$buildingDir, [string]$testLog) {
    $names = @('first', 'again', 'second', 'whole', 'third', 'busiest-cold', 'busiest-warm', 'ride-cold', 'ride-warm')
    $runs = [ordered]@{}
    foreach ($name in $names) { $runs[$name] = Read-Report (Join-Path $buildingDir "$name.json") }
    $missing = @($names | Where-Object { -not $runs[$_] })
    if ($missing.Count -gt 0) {
        Add-Result 'Real building' 'FAILED' ("the reports of {0} are missing, see $testLog" -f ($missing -join ', ')) 0
        return
    }
    # Planned twice: the first start and again from nothing give one plan (the test compares the files' SHA-256); every
    # later run shows that plan. The digest itself is never written here: it derives from the user's titles.
    $first = $runs['first'].world
    $floors = @($first.floors)
    $placed = ($floors | ForEach-Object { [int]$_.placed } | Measure-Object -Sum).Sum
    $rooms = ($floors | ForEach-Object { [int]$_.rooms } | Measure-Object -Sum).Sum
    $digests = @($names | ForEach-Object { [string]$runs[$_].world.planDigest } | Select-Object -Unique)
    $twiceOk = $first.outcome -eq 'generated' -and $runs['again'].world.outcome -eq 'generated' -and
        $runs['again'].world.planWritten -and $digests.Count -eq 1 -and $digests[0]
    Add-Result 'Real building: planned twice' ($(if ($twiceOk) { 'passed' } else { 'FAILED' })) `
        ('{0} placements in {1} rooms on {2} floors; one plan digest over the {3} runs (first and again generated it, the test compares the files'' SHA-256); the jobs {4} and {5}' -f
            $placed, $rooms, $floors.Count, $names.Count, (Format-Seconds2 $first.times.job.totalMs),
            (Format-Seconds2 $runs['again'].world.times.job.totalMs)) 0 'one SHA-256'
    # An unchanged restart: the building job skipped by its stamp, within 3 s.
    $restarts = @($runs['second'], $runs['third'])
    $skipOk = @($restarts | Where-Object { -not $_.world.times.job.skipped -or [double]$_.world.times.job.totalMs -gt 3000 -or
            $_.world.outcome -ne 'unchanged' -or $_.world.planWritten }).Count -eq 0
    Add-Result 'Real building: unchanged restart' ($(if ($skipOk) { 'passed' } else { 'FAILED' })) `
        ('the job skipped by its stamp in {0} and {1}, nothing written (the whole job on the unchanged library, its stamp removed: {2})' -f
            (Format-Seconds2 $runs['second'].world.times.job.totalMs), (Format-Seconds2 $runs['third'].world.times.job.totalMs),
            (Format-Seconds2 $runs['whole'].world.times.job.totalMs)) 0 'skipped, <= 3 s'
    # NFR-01 to the lobby: the stored lobby and the quick catalog's first page within 5 s of the process's creation.
    $lobby = @($restarts | ForEach-Object { $_.world.preview.committedAtMs })
    $pages = @($restarts | ForEach-Object { $_.catalog.times.firstPageMs })
    $nfrOk = @(@($lobby) + @($pages) | Where-Object { $null -eq $_ -or [double]$_ -ge 5000 }).Count -eq 0
    Add-Result 'NFR-01 to the lobby' ($(if ($nfrOk) { 'passed' } else { 'FAILED' })) `
        ('the stored lobby shown {0} and {1}, the quick catalog''s first page {2} and {3} after the process started ({4} games)' -f
            (Format-Ms $lobby[0]), (Format-Ms $lobby[1]), (Format-Ms $pages[0]), (Format-Ms $pages[1]),
            $runs['second'].catalog.games) 0 '< 5,000 ms'
    # The busiest floor's GPU memory, cold (its covers derived in that run) and warm: the runs started on a floor that
    # holds the most placements of the plan (the test picks it; a tie may name either).
    $most = ($floors | ForEach-Object { [int]$_.placed } | Measure-Object -Maximum).Maximum
    $measured = @($runs['busiest-cold'], $runs['busiest-warm'])
    $floor = $floors | Where-Object { $_.id -eq $measured[0].world.floor } | Select-Object -First 1
    $memoryOk = $floor -and [int]$floor.placed -eq $most -and @($measured | Where-Object {
            [double]$_.gpuMemory.peakTotalBytes -le 0 -or [double]$_.gpuMemory.peakTotalBytes -gt 512MB -or
            $_.world.floor -ne $floor.id }).Count -eq 0
    Add-Result 'Real building: the busiest floor''s GPU memory' ($(if ($memoryOk) { 'passed' } else { 'FAILED' })) `
        ('the {0} floor ({1} rooms, {2} items; {3} placements, the most of the {4} floors: {5}): at most {6} cold, {7} warm (collection arrays {8}, {9})' -f
            $measured[0].world.floor, @($measured[0].world.rooms).Count, $measured[0].world.items, $floor.placed,
            $floors.Count, $(if ($floor -and [int]$floor.placed -eq $most) { 'yes' } else { "NO, the most is $most" }),
            (Format-MiB $measured[0].gpuMemory.peakTotalBytes), (Format-MiB $measured[1].gpuMemory.peakTotalBytes),
            (Format-MiB $measured[0].residency.arrayBytes), $measured[0].residency.format) 0 'all <= 512 MiB'
    # The rides, cold (the derived covers emptied) and warm: the view back, then the room that holds the walker whole.
    foreach ($ride in @(@('cold', 10000, 30000), @('warm', 3000, 3000))) {
        $kind, $viewLimit, $roomLimit = $ride
        $records = @($runs["ride-$kind"].world.rides)
        if ($records.Count -ne 2) {
            Add-Result "Real building: rides $kind" 'FAILED' "$($records.Count) ride(s) recorded, 2 expected" 0
            continue
        }
        $there, $back = $records
        $viewOk = @($records | Where-Object { $null -eq $_.totalMs -or [double]$_.totalMs -gt $viewLimit -or
                $_.outcome -notin @('arrived', 'timed-out') -or [double]$_.gpuBytes -gt 512MB }).Count -eq 0
        Add-Result "Real building: rides $kind" ($(if ($viewOk) { 'passed' } else { 'FAILED' })) `
            ('the lobby to {0} ({1}): the view back {2} ({3}; the floor built at {4}; its {5} covers in view resident at {6}, {7} pending), {8} GPU memory; back to the lobby''s elevator {9} ({10})' -f
                $there.room, $there.to, (Format-Seconds2 $there.totalMs), $there.outcome, (Format-Seconds2 $there.builtMs),
                $there.viewItems, (Format-Seconds2 $there.residentMs), $there.viewPending, (Format-MiB $there.gpuBytes),
                (Format-Seconds2 $back.totalMs), $back.outcome) 0 ("view back <= {0} s" -f ($viewLimit / 1000))
        $roomOk = $null -ne $there.roomResidentMs -and [double]$there.roomResidentMs -le $roomLimit
        Add-Result "Real building: the room after the $kind ride" ($(if ($roomOk) { 'passed' } else { 'FAILED' })) `
            ('{0} whole {1} after the ride was asked (the room that holds the walker, ADR-0013 section 13)' -f $there.room,
                (Format-Seconds2 $there.roomResidentMs)) 0 ("<= {0} s" -f ($roomLimit / 1000))
    }
}

# The family migrations on the real library (Gate D; tests/unit/app/test_building_real.cpp's second case, I6 steps 4 to
# 6): the library planned by generator 1.3, then started in this build (`migrate`), again (`second`) and with the
# building stamp removed (`whole`). The case checks every room (the test's verdict is its own row); these rows give
# what it found. Nothing derived from the user's titles is printed: counts, the families' and the rooms' ids.
function Add-RealMigrationRows([string]$migrationDir, [string]$testLog) {
    $names = @('migrate', 'second', 'whole')
    $runs = [ordered]@{}
    foreach ($name in $names) { $runs[$name] = Read-Report (Join-Path $migrationDir "$name.json") }
    $missing = @($names | Where-Object { -not $runs[$_] })
    if ($missing.Count -gt 0) {
        $why = if (-not $LegacyCli) { 'no generator 1.3 CLI: give -LegacyCli or RV_LEGACY_CLI (docs/i6/step-4.md)' }
               else { "the reports of {0} are missing, see $testLog" -f ($missing -join ', ') }
        Add-Result 'Real building: generator 1.3 migrated once' 'FAILED' $why 0 'once, 0 over capacity'
        Add-Result 'Real building: re-placed onto the hub' 'FAILED' 'not run' 0 'in the same update'
        return
    }
    $summary = if (Test-Path $testLog) {
        Select-String -Path $testLog -Pattern 'Real building: (\d+) room\(s\) migrated, (\d+) other room\(s\) byte for byte in their elements, (\d+) title\(s\) over capacity' |
            Select-Object -Last 1
    }
    $m = $runs['migrate'].world
    $migrated = @($m.migratedRooms)
    $families = @($migrated | Group-Object { "$($_.from) -> $($_.to)" } | Sort-Object Name |
            ForEach-Object { '{0} {1}' -f $_.Count, $_.Name }) -join ', '
    $later = @($runs['second'].world, $runs['whole'].world)
    $onceOk = $summary -and $m.outcome -eq 'updated' -and $m.planWritten -and $m.notice -eq 'rooms-migrated' -and
        $migrated.Count -gt 0 -and [int]$summary.Matches[0].Groups[1].Value -eq $migrated.Count -and
        [int]$summary.Matches[0].Groups[3].Value -eq 0 -and
        @($later | Where-Object { $_.outcome -ne 'unchanged' -or @($_.migratedRooms).Count -gt 0 -or
                @($_.replacedRooms).Count -gt 0 }).Count -eq 0 -and -not $runs['second'].world.planWritten
    $details = if ($summary) {
        '{0} rooms migrated ({1}), {2} other rooms byte for byte in their elements, {3} titles over capacity; the update {4} (notice {5}); a second start and the whole job (its stamp removed): {6} and {7}, nothing migrated or written' -f
            $migrated.Count, $families, $summary.Matches[0].Groups[2].Value, $summary.Matches[0].Groups[3].Value,
            (Format-Seconds2 $m.times.job.totalMs), $m.notice, $runs['second'].world.outcome, $runs['whole'].world.outcome
    }
    else { "no summary line in $testLog" }
    Add-Result 'Real building: generator 1.3 migrated once' ($(if ($onceOk) { 'passed' } else { 'FAILED' })) $details 0 `
        'once, 0 over capacity'
    $replaced = @($m.replacedRooms)
    $placed = @($replaced | ForEach-Object { '{0} from x {1} z {2} to x {3} z {4} mm' -f $_.room, $_.fromXMm, $_.fromZMm,
            $_.toXMm, $_.toZMm }) -join '; '
    Add-Result 'Real building: re-placed onto the hub' ($(if ($replaced.Count -gt 0 -and $onceOk) { 'passed' } else { 'FAILED' })) `
        ('{0} ground-floor room(s) re-placed in the migrating update: {1}' -f $replaced.Count, $placed) 0 'in the same update'
}

Write-Host "Gate $Gate (logs in $logDir)"
Push-Location $root
try {
    $built = @{}
    # -TimingOnly: the Release build alone, then the benchmark path and the real library below.
    $buildPresets = if ($TimingOnly) { @('msvc-x64-release') }
                    else { @('msvc-x64-debug', 'msvc-x64-relwithdebinfo', 'msvc-x64-release') }
    foreach ($preset in $buildPresets) {
        $built[$preset] = Invoke-Build $preset
    }
    foreach ($preset in $(if ($TimingOnly) { @() } else { @('msvc-x64-debug', 'msvc-x64-release') })) {
        if (-not $built[$preset]) {
            Add-Result "Unit tests $preset" 'not run' 'the build failed' 0
            Add-Result "GPU smoke tests $preset" 'not run' 'the build failed' 0
            Add-Result "Goldens $preset" 'not run' 'the build failed' 0
            if ($Gate -ne 'A') { Add-Result "$Gate checks $preset" 'not run' 'the build failed' 0 }
            continue
        }
        $unit = Invoke-Tests "Unit tests $preset" "unit-$preset" $preset @()
        $gpu = Invoke-Tests "GPU smoke tests $preset" "gpu-$preset" "$preset-gpu" @('-L', 'gpu', '-E', 'golden')
        Invoke-Goldens $preset "$preset-gpu"
        if ($Gate -eq 'M1') {
            Add-CaseRow "M1 unit tests $preset" $unit.Cases $m1UnitPattern 'catalog, catalog_db, esde, cli, launcher, platform_win, the app''s catalog and launches; of the unit run'
            Add-CaseRow "M1 GPU catalog and launch $preset" $gpu.Cases $m1GpuPattern 'gpu.catalog, gpu.launch; of the GPU run'
            Invoke-Tests "M1 CLI as a process $preset" "cli-process-$preset" $preset @('-R', 'cli/process') | Out-Null
        }
        if ($Gate -eq 'B') {
            Add-CaseRow "I3 unit tests $preset" $unit.Cases $i3UnitPattern 'world, the room in the app, covers, interaction, card and screens; of the unit run'
            Add-CaseRow "I3 GPU cases $preset" $gpu.Cases $i3GpuPattern 'gpu.world*, covers, media viewer, refresh, arrays, static groups, video; of the GPU run'
        }
        if ($Gate -in @('C', 'D')) {
            Add-AreaRows 'I4' $i4Areas $preset $unit.Cases $gpu.Cases
        }
        if ($Gate -eq 'D') {
            Add-AreaRows 'I6' $i6Areas $preset $unit.Cases $gpu.Cases
        }
    }
    if ($Gate -in @('B', 'C', 'D')) {
        if ($built['msvc-x64-release']) {
            try { Invoke-Benchmarks }
            catch { Add-Result 'Benchmarks (Release)' 'FAILED' "stopped: $($_.Exception.Message)" 0 }
        }
        else { Add-Result 'Benchmarks (Release)' 'not run' 'the Release build failed' 0 }
    }
    if ($RealLibrary) {
        if ($built['msvc-x64-release']) {
            # An exception must not lose the rows of the steps before it (the inner run writes them at its end).
            try {
                switch ($Gate) {
                    'M1' { Invoke-RealLibrary }
                    'B' { Invoke-RealRoom }
                    'C' { Invoke-RealBuilding }
                    'D' { Invoke-RealBuilding -Migration }
                }
            }
            catch { Add-Result 'Real library' 'FAILED' "stopped: $($_.Exception.Message)" 0 }
        }
        else { Add-Result 'Real library' 'not run' 'the Release build failed' 0 }
    }
}
finally {
    Pop-Location
}

if ($env:RV_GATE_INNER -eq '1') {
    # Under the focus watcher: the outer run prints the summary with the watcher's row.
    ConvertTo-Json -InputObject @($results) -Depth 4 | Set-Content -LiteralPath $resultsFile -Encoding utf8
    exit 0
}
exit (Write-Summary)
