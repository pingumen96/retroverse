<#
.SYNOPSIS
    The measurements of Gate M1 on the real ES-DE library (scripts/gate.ps1 -Gate M1 -RealLibrary): retroverse-cli and
    retroverse.exe as the user runs them, every figure written to a JSON file.

.DESCRIPTION
    Run by scripts/real-library.ps1 (-Measure), between its read-only snapshots of the library and before the
    [real-library] tests, with the library's folders in RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and
    RV_REAL_MEDIA_ROOT and RV_LAUNCH_BACKGROUND=1; never on its own. Everything it writes goes under -OutDir (made anew):
      1. retroverse-cli library create into a fresh user data directory <OutDir>/data, the ES-DE home given as ES-DE's
         --home takes it (its parent folder) with the install folder and the ROM and media roots;
      2. the ROM scan, `import --dry-run`: the first read of the library after the snapshot, which lists the folders but
         opens no file, so the sheets (cue, m3u, gdi) are read cold unless something read them since the last boot;
      3. the first import, a second one (which must write nothing: no identity, entry, disc group, history or review
         row), `db rebuild-index` and `db check`;
      4. `media --coverage` over the library snapshot;
      5. `search` and `show` on the real catalog (informative: a process each);
      6. NFR-01: retroverse.exe of the preset started three times, hidden, without vsync and without --fixed-dt, the
         quick catalog opened on frame 1: from the process's creation to the first page of the real catalog shown
         (the run report's catalog.times), limit 5 s;
      7. with -FullHashPass only, `identities hash`: the whole library's quick-hash pass (about 45 minutes cold on the
         reference machine; every present file read once, at most 128 KiB each, never written).
    Every process runs with RV_LAUNCH_ALLOWLIST naming the build's fake emulator alone: no real emulator can start.
    The figures go to <OutDir>/figures.json; the outputs of each command next to it (<name>.json, <name>.log).
    Exit codes: 0 every check passed; 1 a check failed (figures.json lists why); 2 it could not run.
#>
param(
    [string]$Preset = 'msvc-x64-release',
    [Parameter(Mandatory = $true)]
    [string]$OutDir,
    [switch]$FullHashPass
)
$ErrorActionPreference = 'Stop'
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build/$Preset/bin"
$cli = Join-Path $bin 'retroverse-cli.exe'
$app = Join-Path $bin 'retroverse.exe'
$fake = Join-Path $bin 'rv_fake_emulator.exe'

$esdeHome = $env:RV_REAL_ESDE_HOME
$esdeInstall = $env:RV_REAL_ESDE_INSTALL
$romRoot = $env:RV_REAL_ROM_ROOT
$mediaRoot = $env:RV_REAL_MEDIA_ROOT
foreach ($folder in @($esdeHome, $esdeInstall, $romRoot, $mediaRoot)) {
    if (-not $folder) {
        Write-Host 'gate-m1-real: run by scripts/real-library.ps1 only (the RV_REAL_* variables are not set)'
        exit 2
    }
}
foreach ($program in @($cli, $app, $fake)) {
    if (-not (Test-Path -LiteralPath $program -PathType Leaf)) {
        Write-Host "gate-m1-real: $program is not built: ./scripts/build.ps1 -Preset $Preset"
        exit 2
    }
}
# library create takes the ES-DE home as ES-DE's own --home does: the folder that holds ES-DE\.
if ((Split-Path -Leaf $esdeHome) -ne 'ES-DE') {
    Write-Host "gate-m1-real: the ES-DE home must be a folder named ES-DE (retroverse-cli library create --home takes its parent)"
    exit 2
}
$homeFolder = Split-Path -Parent $esdeHome

# The previous run's outputs go (a locked file of it is a run that cannot start, never an exception).
try {
    if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $OutDir = (Resolve-Path -LiteralPath $OutDir).Path
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
}
catch {
    Write-Host "gate-m1-real: cannot prepare ${OutDir}: $($_.Exception.Message)"
    exit 2
}
$data = Join-Path $OutDir 'data'
$failures = [System.Collections.Generic.List[string]]::new()
$figures = [ordered]@{
    preset    = $Preset
    startedAt = (Get-Date).ToString('o')
    bootTime  = $boot.ToString('o')
    uptimeHours = [math]::Round(((Get-Date) - $boot).TotalHours, 2)
}

# Nothing but the build's fake emulator may start (none of these commands launches anything).
$savedAllowList = [Environment]::GetEnvironmentVariable('RV_LAUNCH_ALLOWLIST', 'Process')
$savedAssetRoot = [Environment]::GetEnvironmentVariable('RV_ASSET_ROOT', 'Process')
[Environment]::SetEnvironmentVariable('RV_LAUNCH_ALLOWLIST', $fake, 'Process')
# The programs read the assets deployed beside them, as a user's run does.
[Environment]::SetEnvironmentVariable('RV_ASSET_ROOT', $null, 'Process')

# Runs retroverse-cli --data-dir <data> <arguments> --json --output <OutDir>/<Name>.json, its log (stderr) in
# <Name>.log; a check fails when its exit code is not among $Allowed. Returns the exit code, the wall time and the
# result.
function Invoke-Cli([string]$Name, [string[]]$Arguments, [int[]]$Allowed = @(0)) {
    $output = Join-Path $OutDir "$Name.json"
    $log = Join-Path $OutDir "$Name.log"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $ErrorActionPreference = 'Continue'
    & $cli --data-dir $data @Arguments --json --output $output 2> $log
    $code = $LASTEXITCODE
    $seconds = $timer.Elapsed.TotalSeconds
    $result = if (Test-Path -LiteralPath $output) { Get-Content -Raw -LiteralPath $output | ConvertFrom-Json } else { $null }
    if ($code -notin $Allowed) { $failures.Add("retroverse-cli $($Arguments -join ' ') exited $code (see $log)") }
    Write-Host ('{0,-18} exit {1}, {2:N2} s' -f $Name, $code, $seconds)
    return [pscustomobject]@{ Code = $code; Seconds = [math]::Round($seconds, 3); Result = $result }
}

function Get-ImportFigures($run) {
    $statistics = $run.Result.statistics
    return [ordered]@{
        exitCode          = $run.Code
        wallSeconds       = $run.Seconds
        seconds           = $statistics.seconds
        loadSeconds       = $statistics.loadSeconds
        mediaSeconds      = $statistics.mediaSeconds
        compareSeconds    = $statistics.compareSeconds
        writeSeconds      = $statistics.writeSeconds
        entries           = $statistics.entries
        identitiesWritten = $statistics.identitiesWritten
        entriesWritten    = $statistics.entriesWritten
        entriesRemoved    = $statistics.entriesRemoved
        discGroupsWritten = $statistics.discGroupsWritten
        historyWritten    = $statistics.historyWritten
        reviewsWritten    = $statistics.reviewsWritten
        newIdentities     = $run.Result.totals.newIdentities
        unchanged         = $run.Result.totals.unchanged
    }
}

try {
    # 1. The library, as a user records it.
    $library = Invoke-Cli 'library-create' @('library', 'create', '--home', $homeFolder, '--install', $esdeInstall,
        '--rom-root', $romRoot, '--media-root', $mediaRoot)
    if ($library.Code -ne 0) { throw 'library create failed: nothing else can run' }

    # 2. The ROM scan: the first read of the library's files after the snapshot.
    $scan = Invoke-Cli 'rom-scan' @('import', '--dry-run')
    $statistics = $scan.Result.statistics
    $figures.romScan = [ordered]@{
        exitCode        = $scan.Code
        wallSeconds     = $scan.Seconds
        seconds         = $statistics.seconds
        configurationSeconds = $statistics.configurationSeconds
        scanSeconds     = $statistics.scanSeconds
        gamelistSeconds = $statistics.gamelistSeconds
        sheetSeconds    = $statistics.sheetSeconds
        systemsScanned  = $statistics.systemsScanned
        filesListed     = $statistics.filesListed
        foldersListed   = $statistics.foldersListed
        gamelistsRead   = $statistics.gamelistsRead
        sheetsRead      = $statistics.sheetsRead
        sheetsUnreadable = $statistics.sheetsUnreadable
        entries         = $scan.Result.totals.entries
        games           = $scan.Result.totals.games
    }

    # 3. The imports, the rebuild, the check.
    $first = Invoke-Cli 'import-1' @('import')
    $figures.firstImport = Get-ImportFigures $first
    if ($first.Result -and -not $first.Result.firstImport) { $failures.Add('the first import was not a first import') }
    $second = Invoke-Cli 'import-2' @('import')
    $figures.secondImport = Get-ImportFigures $second
    foreach ($key in @('identitiesWritten', 'entriesWritten', 'entriesRemoved', 'discGroupsWritten', 'historyWritten',
            'reviewsWritten', 'newIdentities')) {
        if ($figures.secondImport[$key] -ne 0) {
            $failures.Add("the second import wrote something: $key = $($figures.secondImport[$key])")
        }
    }
    $rebuild = Invoke-Cli 'rebuild-index' @('db', 'rebuild-index')
    $figures.rebuildIndex = Get-ImportFigures $rebuild
    if ($figures.rebuildIndex.identitiesWritten -ne 0) { $failures.Add('db rebuild-index wrote identities') }
    $check = Invoke-Cli 'db-check' @('db', 'check')
    $figures.dbCheck = [ordered]@{ exitCode = $check.Code; wallSeconds = $check.Seconds }

    # 4. Media coverage over the library snapshot (exit 1 means issues of error severity: reported, not failed).
    $coverage = Invoke-Cli 'media-coverage' @('media', '--coverage') @(0, 1)
    $games = $coverage.Result.total.games
    $kinds = [ordered]@{}
    if ($games) {
        foreach ($property in $games.withKind.PSObject.Properties) {
            $kinds[$property.Name] = [math]::Round(100.0 * $property.Value / [math]::Max(1, $games.entries), 1)
        }
    }
    $figures.mediaCoverage = [ordered]@{
        exitCode     = $coverage.Code
        wallSeconds  = $coverage.Seconds
        entries      = $coverage.Result.entries
        games        = $games.entries
        gamesWithAny = $games.withAny
        withAnyPercent = if ($games) { [math]::Round(100.0 * $games.withAny / [math]::Max(1, $games.entries), 1) } else { $null }
        percentPerKind = $kinds
        issues       = @($coverage.Result.issues).Count
    }

    # 5. A search and a game's details on the real catalog.
    $search = Invoke-Cli 'search' @('search', 'super', '--limit', '5')
    $figures.search = [ordered]@{ exitCode = $search.Code; wallSeconds = $search.Seconds; count = $search.Result.count }
    if ($search.Result -and @($search.Result.games).Count -gt 0) {
        $show = Invoke-Cli 'show' @('show', $search.Result.games[0].id)
        $figures.show = [ordered]@{ exitCode = $show.Code; wallSeconds = $show.Seconds }
    }

    # 6. NFR-01: the app started with the indexed catalog, the quick catalog opened on frame 1.
    $script = Join-Path $OutDir 'open-catalog.json'
    Set-Content -LiteralPath $script -Encoding utf8 -Value '[{"fromFrame": 1, "toFrame": 1, "actions": {"quick_catalog": 1}}]'
    $runs = [System.Collections.Generic.List[object]]::new()
    foreach ($index in 1..3) {
        $report = Join-Path $OutDir "nfr01-run$index.json"
        $log = Join-Path $OutDir "nfr01-run$index.log"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $ErrorActionPreference = 'Continue'
        & $app --scene m0_testroom --hidden --no-vsync --exit-after-frames --frames 3000 --data-dir $data `
            --run-report $report --input-script $script *> $log
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $wall = $timer.Elapsed.TotalSeconds
        $run = [ordered]@{ exitCode = $code; wallSeconds = [math]::Round($wall, 2) }
        if (Test-Path -LiteralPath $report) {
            $catalog = (Get-Content -Raw -LiteralPath $report | ConvertFrom-Json).catalog
            $run.games = $catalog.games
            $run.notice = $catalog.notice
            $run.openedMs = $catalog.times.openedMs
            $run.firstListMs = $catalog.times.firstListMs
            $run.firstPageMs = $catalog.times.firstPageMs
            $run.pagesP95Ms = $catalog.pages.p95Ms
        }
        if ($code -ne 0) { $failures.Add("retroverse.exe NFR-01 run $index exited $code (see $log)") }
        if ($null -eq $run.firstPageMs) { $failures.Add("NFR-01 run ${index}: no page of the catalog shown") }
        elseif ($run.firstPageMs -ge 5000) { $failures.Add("NFR-01 run ${index}: first page after $($run.firstPageMs) ms") }
        if ($run.notice) { $failures.Add("NFR-01 run ${index}: the catalog showed the notice '$($run.notice)'") }
        Write-Host ('nfr01-run{0}        exit {1}, first page {2} ms' -f $index, $code, $run.firstPageMs)
        $runs.Add($run)
    }
    $figures.nfr01 = $runs

    # 7. The whole library's quick-hash pass, on demand only.
    if ($FullHashPass) {
        $hash = Invoke-Cli 'quick-hash-all' @('identities', 'hash') @(0, 1)
        $figures.fullHashPass = [ordered]@{
            exitCode = $hash.Code
            wallSeconds = $hash.Seconds
            result = $hash.Result
        }
    }
}
catch {
    $failures.Add("gate-m1-real stopped: $($_.Exception.Message)")
}
finally {
    [Environment]::SetEnvironmentVariable('RV_LAUNCH_ALLOWLIST', $savedAllowList, 'Process')
    [Environment]::SetEnvironmentVariable('RV_ASSET_ROOT', $savedAssetRoot, 'Process')
}

$figures.failures = @($failures)
$figures.finishedAt = (Get-Date).ToString('o')
$figures | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDir 'figures.json') -Encoding utf8
foreach ($failure in $failures) { Write-Host "gate-m1-real: $failure" -ForegroundColor Red }
exit $(if ($failures.Count -gt 0) { 1 } else { 0 })
