<#
.SYNOPSIS
    Runs the checks of a gate end to end and prints a summary table.

.DESCRIPTION
    Gate A (milestone M0, style.md section 27 phase A; docs/testing/gate-a.md):
      1. builds msvc-x64-debug, msvc-x64-relwithdebinfo and msvc-x64-release: every project target compiles with /WX,
         so a compiler warning fails its build; the build output is also scanned for any compiler, linker or CMake
         warning (with -Clean every target is rebuilt, so that all of them are seen);
      2. runs the unit tests (label unit) of Debug and Release;
      3. runs the GPU smoke tests (label gpu, tests/smoke) of Debug and Release: the reference scenes, the rendering
         self-checks and --gl-selftest, the key light's shadow and the ambient occlusion, the instance buffer growing
         mid-run, the developer overlay and the asset preview (Debug), Release rendering from the assets deployed next to
         it (which its build, incremental too, mirrors from the source tree);
      4. compares the test room with golden G01 in Debug and Release (within 6/255 per channel, at most 0.5% of the
         pixels; on the reference machine only, skipped elsewhere).
    Every step logs into build/gate-<gate>/. The script prints one row per step and exits 0 when no step failed: a
    skipped step (no OpenGL 4.5 context, another GPU than the golden's) is reported, not failed.
    Each command runs inside the Visual Studio developer environment (scripts/dev.ps1).

.EXAMPLE
    ./scripts/gate.ps1 -Gate A
    ./scripts/gate.ps1 -Gate A -Clean      # rebuild every target of every preset first
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('A')]
    [string]$Gate,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dev = Join-Path $PSScriptRoot 'dev.ps1'
$logDir = Join-Path $root ("build/gate-" + $Gate.ToLowerInvariant())
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$results = [System.Collections.Generic.List[object]]::new()

function Add-Result([string]$Step, [string]$Result, [string]$Details, [double]$Seconds) {
    $results.Add([pscustomobject]@{
            Step    = $Step
            Result  = $Result
            Details = $Details
            Time    = '{0:N0} s' -f $Seconds
        })
    Write-Host ('{0,-34} {1,-8} {2}' -f $Step, $Result, $Details)
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

# Runs ctest with a test preset and extra arguments; the counts come from its JUnit report. Returns the parsed suite.
function Invoke-Tests([string]$Step, [string]$Name, [string]$Preset, [string[]]$Extra) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $logDir "$Name.log"
    $junit = Join-Path $logDir "$Name.xml"
    Remove-Item -ErrorAction SilentlyContinue $log, $junit
    $code = Invoke-Logged $log (@('ctest', '--preset', $Preset, '--output-junit', $junit) + $Extra)
    if (-not (Test-Path $junit)) {
        Add-Result $Step 'FAILED' "no test results (exit code $code), see $log" $timer.Elapsed.TotalSeconds
        return $null
    }
    [xml]$report = Get-Content -Raw $junit
    $suite = $report.testsuite
    $total = [int]$suite.tests
    $failed = [int]$suite.failures
    $skipped = @($suite.testcase | Where-Object { $_.status -eq 'notrun' -or $_.skipped }).Count
    $passedCount = $total - $failed - $skipped
    $result = if ($code -ne 0 -or $failed -gt 0 -or $total -eq 0) { 'FAILED' }
              elseif ($passedCount -eq 0) { 'skipped' }
              else { 'passed' }
    $details = "$passedCount passed, $failed failed, $skipped skipped"
    if ($result -eq 'FAILED') { $details += ", see $log" }
    Add-Result $Step $result $details $timer.Elapsed.TotalSeconds
    return $result
}

# The golden comparison: its ctest result, with the verdict the test writes next to its screenshot.
function Invoke-Golden([string]$Preset, [string]$TestPreset) {
    $verdictFile = Join-Path $root "build/$Preset/smoke/golden_g01/comparison.txt"
    Remove-Item -ErrorAction SilentlyContinue $verdictFile
    $result = Invoke-Tests "Golden G01 $Preset" "golden-$Preset" $TestPreset @('-R', 'golden')
    if (Test-Path $verdictFile) {
        $last = $results[$results.Count - 1]
        $last.Details = (Get-Content -Raw $verdictFile).Trim()
        Write-Host ('{0,-34} {1,-8} {2}' -f '', '', $last.Details)
    }
    return $result
}

$gateTimer = [Diagnostics.Stopwatch]::StartNew()
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
            Add-Result "Golden G01 $preset" 'not run' 'the build failed' 0
            continue
        }
        Invoke-Tests "Unit tests $preset" "unit-$preset" $preset @() | Out-Null
        Invoke-Tests "GPU smoke tests $preset" "gpu-$preset" "$preset-gpu" @('-L', 'gpu', '-E', 'golden') | Out-Null
        Invoke-Golden $preset "$preset-gpu" | Out-Null
    }
}
finally {
    Pop-Location
}

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
$results | Format-Table -AutoSize -Wrap -Property Step, Result, Details, Time | Out-String -Width 220 | Write-Host
$verdict = if ($failedSteps.Count -gt 0) { 'FAILED' }
           elseif ($skippedSteps.Count -gt 0) { 'PASSED with skipped steps' }
           else { 'PASSED' }
Write-Host ('Gate {0}: {1} ({2} step(s), {3} failed, {4} skipped, {5:N0} s)' -f $Gate, $verdict, $results.Count,
    $failedSteps.Count, $skippedSteps.Count, $gateTimer.Elapsed.TotalSeconds)
exit $(if ($failedSteps.Count -gt 0) { 1 } else { 0 })
