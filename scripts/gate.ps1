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
      6. in Release, the hidden benchmarks: rv_tests "[bench]" (the GPU-free budgets: generation, update, validation,
         the room's snapshot query, covers, picking, NFR-02) and rv_smoke_tests "[bench]" (the snes-stress room's
         figures, its covers cold and warm, the animations' frame budget, and the benchmark path of 2,000 titles at
         1920 x 1080 with --no-vsync), the benchmark's frame times, GPU passes and GPU memory read from its run report
         against the roadmap's budgets (docs/roadmap.md, I3, "Budgets");
      7. with -RealLibrary, scripts/real-library.ps1 -Filter '[real-library][room-real]' (ES-DE closed, read-only
         snapshots before and after, RV_LAUNCH_ALLOWLIST naming the fake emulator alone): the real SNES room's plan
         generated twice, the restart after a second import, the covers cold and warm, the benchmark path, a refresh
         of the whole library with the quick catalog open; the figures read from the test's run reports (time to the
         room, covers resident cold and warm, frame p99 on the benchmark path and over the refresh, NFR-01 with the
         room) against their limits.
    Gates M1 and B run under scripts/focus_watch.py (--watch build): a test process that takes the foreground or
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
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('A', 'M1', 'B')]
    [string]$Gate,
    [switch]$Clean,
    [switch]$RealLibrary,
    [string]$EsdeHome = $env:RV_REAL_ESDE_HOME,
    [string]$EsdeInstall = $env:RV_REAL_ESDE_INSTALL,
    [string]$RomRoot = $env:RV_REAL_ROM_ROOT,
    [string]$MediaRoot = $env:RV_REAL_MEDIA_ROOT,
    [string]$RetroArchExe = $env:RV_REAL_RETROARCH_EXE,
    [switch]$FullHashPass
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
    Write-Host 'gate: -RealLibrary belongs to -Gate M1 and B, -FullHashPass to -Gate M1' -ForegroundColor Red
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

# --- Gates M1 and B under the focus watcher ----------------------------------------------------------------------------
# The outer run starts the gate again under scripts/focus_watch.py, with RV_GATE_INNER set; the inner run does the work
# and leaves its rows in results.json, which the outer one prints with the watcher's verdict.
if ($Gate -in @('M1', 'B') -and $env:RV_GATE_INNER -ne '1') {
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

# The golden comparisons (G01 alone for Gates A and M1, every golden for Gate B): one row per golden, its ctest result
# with the verdict its case writes next to its screenshot (G01's in smoke/golden_g01, the rooms' in
# smoke/golden_room/<case>); the run's own row stays only when it holds no golden case (a failure before any ran).
function Invoke-Goldens([string]$Preset, [string]$TestPreset) {
    $smokeDir = Join-Path $root "build/$Preset/smoke"
    $verdictOf = { param($id) if ($id -eq 'G01') { Join-Path $smokeDir 'golden_g01/comparison.txt' }
                   else { Join-Path $smokeDir ('golden_room/{0}/comparison.txt' -f $id.ToLowerInvariant()) } }
    Remove-Item -ErrorAction SilentlyContinue (Join-Path $smokeDir 'golden_g01/comparison.txt')
    Get-ChildItem -ErrorAction SilentlyContinue -Path (Join-Path $smokeDir 'golden_room') -Filter comparison.txt -Recurse |
        Remove-Item
    $pattern = if ($Gate -eq 'B') { '^gpu\.golden/' } else { '^gpu\.golden/G01:' }
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

# The budgets of the GPU passes (docs/roadmap.md, I3, "Budgets"), checked at each pass's median as the benchmark case
# does: a timer query measures the GPU's time between two commands, so on a desktop GPU shared with other programs a
# pass's tail holds their time slices too (docs/testing/gate-b.md); the p99 is reported beside it.
$passBudgets = [ordered]@{ shadow = 0.4; depth = 0.4; ssao = 0.5; scene = 2.0; post = 0.6; outline = 0.1; total = 5.0 }

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

# The benchmark path's rows from a run report: the "benchmark" phase's frame times, the GPU passes at their p99 and the
# GPU memory, each against its budget.
function Add-BenchmarkRows([string]$Label, [string]$ReportFile) {
    $report = Read-Report $ReportFile
    $phase = Get-Phase $report 'benchmark'
    if (-not $phase -or [int]$phase.count -eq 0) {
        Add-Result "$Label, frame times" 'FAILED' "no benchmark phase in $ReportFile" 0 'p99 <= 16.7 ms, p50 <= 8 ms'
        return
    }
    $framesOk = [double]$phase.p99Ms -le 16.7 -and [double]$phase.p50Ms -le 8.0
    Add-Result "$Label, frame times" ($(if ($framesOk) { 'passed' } else { 'FAILED' })) `
        ('p50 {0}, p95 {1}, p99 {2}, max {3} over {4} frames ({5:N0} FPS at p99)' -f (Format-Ms $phase.p50Ms),
            (Format-Ms $phase.p95Ms), (Format-Ms $phase.p99Ms), (Format-Ms $phase.maxMs), $phase.count,
            (1000.0 / [double]$phase.p99Ms)) 0 'p99 <= 16.7 ms, p50 <= 8 ms'
    $over = @()
    $texts = @()
    foreach ($name in $passBudgets.Keys) {
        $pass = @($phase.gpuPasses | Where-Object { $_.name -eq $name }) | Select-Object -First 1
        if (-not $pass) {
            # The outline is drawn only while an item is hovered: the path need not hover one.
            if ($name -ne 'outline') { $over += "$name missing" }
            $texts += "$name not drawn"
            continue
        }
        if ([double]$pass.p50Ms -gt $passBudgets[$name]) { $over += $name }
        $texts += ('{0} {1:N3}/{2:N3}' -f $name, [double]$pass.p50Ms, [double]$pass.p99Ms)
    }
    Add-Result "$Label, GPU per pass" ($(if ($over.Count -eq 0) { 'passed' } else { 'FAILED' })) `
        (('p50/p99 ms: ' + ($texts -join ', ')) + $(if ($over.Count -gt 0) { '; over: ' + ($over -join ', ') } else { '' })) 0 `
        'p50: shadow 0.4, depth 0.4, ssao 0.5, scene 2.0, post 0.6, outline 0.1, total 5 ms'
    $arrays = [double]$report.residency.arrayBytes
    $peak = [double]$report.gpuMemory.peakTotalBytes
    Add-Result "$Label, GPU memory" ($(if ($arrays -le 80MB -and $peak -le 256MB) { 'passed' } else { 'FAILED' })) `
        ('collection arrays {0} ({1}, {2} items), all textures at most {3}' -f (Format-MiB $arrays),
            $report.residency.format, $report.residency.items, (Format-MiB $peak)) 0 'arrays <= 80 MiB, all <= 256 MiB'
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

# The hidden benchmarks of Release: the GPU-free ones, then the GPU ones with the benchmark path's budgets.
function Invoke-Benchmarks {
    $bin = Join-Path $root 'build/msvc-x64-release/bin'
    Invoke-Catch 'Benchmarks, GPU-free (Release)' 'bench-unit' (Join-Path $bin 'rv_tests.exe') '[bench]' | Out-Null
    $cases = Invoke-Catch 'Benchmarks, GPU (Release)' 'bench-gpu' (Join-Path $bin 'rv_smoke_tests.exe') '[bench]'
    # The figures each case printed (WARN), a row per case.
    foreach ($case in $cases) {
        $warnings = @($case.SelectNodes('.//Warning') | ForEach-Object { ($_.InnerText -replace '\s+', ' ').Trim() })
        $result = if ($case.OverallResult.success -ne 'true') { 'FAILED' } else { 'passed' }
        Add-Result ('  ' + $case.name) $result ($warnings -join ' | ') ([double]$case.OverallResult.durationInSeconds)
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
                ('{0} placements ({1} featured, {2} band, {3} spine), {4} unplaced; {5} lights, {6} clusters; plan {7}' -f
                    $w.placements.total, $w.placements.featured, $w.placements.band, $w.placements.spine,
                    $w.unplaced.total, $w.lights, $w.clusters, $w.planDigest) 0 'one SHA-256'
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
            Add-Result 'Real room: covers resident, cold' ($(if ($null -ne $coldMs -and [double]$coldMs -le 10000) { 'passed' } else { 'FAILED' })) `
                ('{0} after the room showed ({1} items; {2} cache misses, {3} stored); coverLoad p99 {4}' -f
                    (Format-Ms $coldMs), $cold.residency.items, $cold.residency.cache.misses, $cold.residency.cache.stored,
                    (Format-Ms (Get-Phase $cold 'coverLoad').p99Ms)) 0 '<= 10 s'
            $warm = @($again.residency.times.residentAfterShownMs, $restart.residency.times.residentAfterShownMs)
            $warmOk = @($warm | Where-Object { $null -eq $_ -or [double]$_ -gt 1000 }).Count -eq 0
            Add-Result 'Real room: covers resident, warm' ($(if ($warmOk) { 'passed' } else { 'FAILED' })) `
                ('{0} (the plan generated again), {1} (restart) after the room showed ({2} cache hits)' -f
                    (Format-Ms $warm[0]), (Format-Ms $warm[1]), $restart.residency.cache.hits) 0 '<= 1.0 s'
            $loads = @(@($cold, $again, $restart) | ForEach-Object { Get-Phase $_ 'coverLoad' } | Where-Object { $_ -and [int]$_.count -gt 0 })
            $loadP99 = ($loads | ForEach-Object { [double]$_.p99Ms } | Measure-Object -Maximum).Maximum
            Add-Result 'Real room: frames while covers load' ($(if ($loads.Count -gt 0 -and $loadP99 -le 33.3) { 'passed' } else { 'FAILED' })) `
                ('coverLoad p99 {0} (cold), {1} (again), {2} (restart)' -f (Format-Ms (Get-Phase $cold 'coverLoad').p99Ms),
                    (Format-Ms (Get-Phase $again 'coverLoad').p99Ms), (Format-Ms (Get-Phase $restart 'coverLoad').p99Ms)) 0 'p99 <= 33.3 ms (NFR-04)'
        }
        Add-BenchmarkRows 'Real room: benchmark path' (Join-Path $roomDir 'benchmark.json')
        $phase = Get-Phase $refresh 'refresh'
        if ($refresh -and $phase -and $refresh.catalog) {
            $record = @($refresh.catalog.refreshes)[0]
            $live = @($refresh.world.liveUpdates)
            $liveMoved = ($live | ForEach-Object { [int]$_.moved } | Measure-Object -Sum).Sum
            Add-Result 'Real room: refresh of the library' ($(if ($record.outcome -eq 'done' -and [double]$phase.p99Ms -le 33.3 -and $live.Count -gt 0 -and $liveMoved -eq 0) { 'passed' } else { 'FAILED' })) `
                ('refresh phase {0} frames p50 {1}, p99 {2}, max {3}; the import {4} in {5} ({6} entries, {7} added); the room''s live update {8}, {9} moved' -f
                    $phase.frames, (Format-Ms $phase.p50Ms), (Format-Ms $phase.p99Ms), (Format-Ms $phase.maxMs), $record.outcome,
                    (Format-Ms $record.times.ms), $record.entries, $record.added, (@($live | ForEach-Object { $_.kind }) -join ', '),
                    $liveMoved) 0 'p99 <= 33.3 ms (NFR-04), 0 moves'
            $page = $refresh.catalog.times.firstPageMs
            Add-Result 'NFR-01 with the room' ($(if ($null -ne $page -and [double]$page -lt 5000) { 'passed' } else { 'FAILED' })) `
                ('the quick catalog''s first page {0} after the process started ({1} games; the room shown at {2})' -f
                    (Format-Ms $page), $refresh.catalog.games, (Format-Ms $refresh.residency.times.shownAtMs)) 0 '< 5,000 ms'
        }
        else {
            Add-Result 'Real room: refresh of the library' 'FAILED' "no refresh report, see $testLog" 0 'p99 <= 33.3 ms (NFR-04), 0 moves'
        }
    }
    if ($realCode -ne 0 -and -not (@($results | Select-Object -Skip $firstRow) | Where-Object { $_.Result -eq 'FAILED' })) {
        Add-Result 'Real library' 'FAILED' "scripts/real-library.ps1 exited $realCode, see $realLog" 0
    }
}

Write-Host "Gate $Gate (logs in $logDir)"
Push-Location $root
try {
    $built = @{}
    foreach ($preset in @('msvc-x64-debug', 'msvc-x64-relwithdebinfo', 'msvc-x64-release')) {
        $built[$preset] = Invoke-Build $preset
    }
    foreach ($preset in @('msvc-x64-debug', 'msvc-x64-release')) {
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
    }
    if ($Gate -eq 'B') {
        if ($built['msvc-x64-release']) {
            try { Invoke-Benchmarks }
            catch { Add-Result 'Benchmarks (Release)' 'FAILED' "stopped: $($_.Exception.Message)" 0 }
        }
        else { Add-Result 'Benchmarks (Release)' 'not run' 'the Release build failed' 0 }
    }
    if ($RealLibrary) {
        if ($built['msvc-x64-release']) {
            # An exception must not lose the rows of the steps before it (the inner run writes them at its end).
            try { if ($Gate -eq 'M1') { Invoke-RealLibrary } else { Invoke-RealRoom } }
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
