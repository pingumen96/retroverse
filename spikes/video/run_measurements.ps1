<#
.SYNOPSIS
    Runs the measurement series of docs/spikes/i1-video.md with spike_video and summarizes them (median and best run).

.DESCRIPTION
    Every series runs -Repeat times (default 3) for -Seconds (default 60) after a 1 s warm-up, hidden (a hidden window is
    still paced by vsync on the reference machine), with its JSON report under -OutDir/<series>/run<i>.json. The runs are
    interleaved (the first run of every series, then the second, ...), so that series compared with each other see
    similar loads on a machine shared with other work. Each report records the machine's load during the run
    (GetSystemTimes). A series named "<series>@above-normal" (or "@high") runs at that process priority, which tells the
    pipeline's own cost from the preemption by other programs. The summary (console and -OutDir/summary.md) gives, per
    series, the median of the runs and the best one for each metric. The real ES-DE videos are read in place, read-only,
    from -MediaRoot.

.EXAMPLE
    ./spikes/video/run_measurements.ps1
    ./spikes/video/run_measurements.ps1 -Series fixtures-2x720p30 -Repeat 1 -Seconds 10
#>
param(
    [string]$Preset = 'msvc-x64-release',
    [int]$Repeat = 3,
    [double]$Seconds = 60,
    [string[]]$Series = @('fixtures-2x720p30', 'fixtures-2x720p30@above-normal', 'real-snes-ps2',
                          'real-snes-ps2@above-normal', 'fixtures-4x720p30@above-normal',
                          'fixtures-2x1080p30-full@above-normal', 'fixtures-2x1080p30-to720@above-normal',
                          'fences-2x720p30@above-normal', 'novsync-2x720p30@above-normal', 'errors'),
    [string]$OutDir = 'build/spike_video/measurements',
    [string]$MediaRoot = 'E:/Programmi/ES-DE_media/downloaded_media',
    [string[]]$RealVideos = @(
        'snes/videos/1 US - S/Super Metroid (Japan, USA) (En,Ja).mp4',
        'snes/videos/1 US - A-E/Donkey Kong Country (USA) (Rev 2).mp4',
        'ps2/videos/Gran Turismo 4 (Europe, Australia) (En,Fr,De,Es,It).mp4')
)
$ErrorActionPreference = 'Stop'
# Numbers in the summary use a decimal point whatever the Windows locale.
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root "build/$Preset/bin/spike_video.exe"
if (-not (Test-Path $exe)) { throw "spike_video not built: $exe (./scripts/build.ps1 -Preset $Preset)" }
$OutDir = Join-Path $root $OutDir
$fixtures = Join-Path $OutDir 'fixtures'
$dataDir = Join-Path $OutDir 'data'

function Get-SeriesArguments([string]$series) {
    # "<series>@above-normal" or "<series>@high": the same series at that process priority (spike_video --priority).
    $name, $priority = $series -split '@', 2
    $common = @('--seconds', "$Seconds", '--hidden', '--data-dir', $dataDir)
    if ($priority) { $common += @('--priority', $priority) }
    switch ($name) {
        'fixtures-2x720p30' { return $common + @('--generate-fixtures', $fixtures, '--count', '2') }
        'fixtures-4x720p30' { return $common + @('--generate-fixtures', $fixtures, '--count', '4') }
        'fixtures-2x1080p30-full' {
            return $common + @('--generate-fixtures', $fixtures, '--count', '2', '--fixture-size', '1920x1080',
                               '--max-height', '1080')
        }
        'fixtures-2x1080p30-to720' {
            return $common + @('--generate-fixtures', $fixtures, '--count', '2', '--fixture-size', '1920x1080',
                               '--max-height', '720')
        }
        'fences-2x720p30' { return $common + @('--generate-fixtures', $fixtures, '--count', '2', '--upload-fences') }
        'novsync-2x720p30' { return $common + @('--generate-fixtures', $fixtures, '--count', '2', '--no-vsync') }
        'real-snes-ps2' {
            $videos = $RealVideos | ForEach-Object { Join-Path $MediaRoot $_ }
            foreach ($video in $videos) { if (-not (Test-Path -LiteralPath $video)) { throw "missing video: $video" } }
            return $common + @('--videos') + $videos
        }
        'errors' {
            return @('--seconds', '5', '--hidden', '--data-dir', $dataDir, '--generate-fixtures', $fixtures,
                     '--error-cases', '--fixture-size', '640x360', '--fixture-seconds', '3')
        }
        default { throw "unknown series '$name'" }
    }
}

function Get-Median([double[]]$values) {
    $sorted = $values | Sort-Object
    if ($sorted.Count -eq 0) { return [double]::NaN }
    return $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]
}

# Metrics of one report; "Lower" tells which way is better for the "best" column.
function Get-Metrics($report) {
    $playing = @($report.videos | Where-Object { -not $_.fallback })
    $late = ($playing | ForEach-Object { $_.presentation.latePercent } | Measure-Object -Maximum).Maximum
    $drift = ($playing | ForEach-Object { [math]::Abs($_.drift.percent) } | Measure-Object -Maximum).Maximum
    $cpu = ($playing | ForEach-Object { $_.decoder.threadCpuPercentOfOneCore } | Measure-Object -Average).Average
    $decode = ($playing | ForEach-Object { $_.decoder.decodeMs.p50 } | Measure-Object -Average).Average
    $convert = ($playing | ForEach-Object { $_.decoder.convertMs.p50 } | Measure-Object -Average).Average
    $dropped = ($playing | ForEach-Object { $_.presentation.dropped } | Measure-Object -Sum).Sum
    $repeated = ($playing | ForEach-Object { $_.presentation.repeated } | Measure-Object -Sum).Sum
    return [ordered]@{
        'frame interval p50 ms' = $report.frameIntervalMs.p50
        'frame interval p99 ms' = $report.frameIntervalMs.p99
        'frame interval max ms' = $report.frameIntervalMs.max
        'frame work p99 ms' = $report.frameWorkMs.p99
        'upload CPU p50 ms' = $report.uploadCpuMs.p50
        'upload CPU p99 ms' = $report.uploadCpuMs.p99
        'upload CPU max ms' = $report.uploadCpuMs.max
        'upload copy p50 ms' = $report.uploadCopyCpuMs.p50
        'upload GPU p50 ms' = $report.uploadGpuMs.p50
        'upload GPU p99 ms' = $report.uploadGpuMs.p99
        'late % (worst video)' = $late
        'dropped (all videos)' = $dropped
        'repeated (all videos)' = $repeated
        'absolute drift % (worst video)' = $drift
        'decode p50 ms (mean of videos)' = $decode
        'convert p50 ms (mean of videos)' = $convert
        'decoder thread CPU % of a core (mean)' = $cpu
        'working set growth MiB' = $report.memory.workingSetGrowthMiB
        'working set end MiB' = $report.memory.workingSetEndMiB
        'machine busy %' = $report.load.machineBusyPercent
        'main thread CPU % of a core' = $report.load.mainThreadPercentOfOneCore
        'render FPS' = $report.run.framesPerSecond
        'videos playing' = $playing.Count
        'criteria met' = $report.criteria.all
    }
}
$higherIsBetter = @('render FPS', 'videos playing')

New-Item -ItemType Directory -Force $OutDir | Out-Null
$reportsBySeries = @{}
foreach ($name in $Series) { $reportsBySeries[$name] = [System.Collections.Generic.List[object]]::new() }
# Interleaved: run 1 of every series, then run 2, ... (the error cases run once).
for ($run = 1; $run -le $Repeat; $run++) {
    foreach ($name in $Series) {
        if ($name -eq 'errors' -and $run -gt 1) { continue }
        $seriesDir = Join-Path $OutDir ($name -replace '@', '_')
        New-Item -ItemType Directory -Force $seriesDir | Out-Null
        $arguments = Get-SeriesArguments $name
        $report = Join-Path $seriesDir "run$run.json"
        $screenshot = Join-Path $seriesDir "run$run.png"
        Remove-Item -LiteralPath $report, $screenshot -ErrorAction SilentlyContinue
        $started = Get-Date
        & $exe @arguments --report $report --screenshot $screenshot *> (Join-Path $seriesDir "run$run.log")
        $exit = $LASTEXITCODE
        $elapsed = ((Get-Date) - $started).TotalSeconds
        if (-not (Test-Path -LiteralPath $report)) {
            Write-Host ('{0} run {1}: exit {2}, no report' -f $name, $run, $exit)
            continue
        }
        $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
        $reportsBySeries[$name].Add($data)
        $line = ('{0} run {1}: exit {2} in {3:N0} s, machine {4}% busy, frame p99 {5} ms, upload p99 {6} ms, ' +
                 'criteria {7}') -f $name, $run, $exit, $elapsed, $data.load.machineBusyPercent,
                $data.frameIntervalMs.p99, $data.uploadCpuMs.p99, $data.criteria.all
        Write-Host $line
    }
}

$summary = [System.Collections.Generic.List[string]]::new()
$summary.Add("# spike_video measurements ($Preset, $Repeat run(s) of $Seconds s per series, interleaved)")
$summary.Add('')
foreach ($name in $Series) {
    $reports = @($reportsBySeries[$name])
    if ($reports.Count -eq 0) { continue }
    $arguments = Get-SeriesArguments $name
    $summary.Add("## $name")
    $summary.Add('')
    $summary.Add(('Arguments: `{0}`' -f ($arguments -join ' ')))
    $summary.Add('')
    $metrics = @($reports | ForEach-Object { Get-Metrics $_ })
    $header = '| Metric | Median | Best |' + (($metrics | ForEach-Object -Begin { $i = 0 } -Process { $i++; " Run $i |" }) -join '')
    $summary.Add($header)
    $summary.Add('|---|---|---|' + ('---|' * $metrics.Count))
    foreach ($key in $metrics[0].Keys) {
        $values = @($metrics | ForEach-Object { $_[$key] })
        $numbers = @($values | Where-Object { $_ -is [ValueType] -and $_ -isnot [bool] } | ForEach-Object { [double]$_ })
        if ($numbers.Count -eq $values.Count -and $numbers.Count -gt 0) {
            $median = Get-Median $numbers
            $best = if ($higherIsBetter -contains $key) { ($numbers | Measure-Object -Maximum).Maximum }
                    else { ($numbers | Measure-Object -Minimum).Minimum }
            $cells = ($numbers | ForEach-Object { ' {0:0.###} |' -f $_ }) -join ''
            $summary.Add(('| {0} | {1:0.###} | {2:0.###} |{3}' -f $key, $median, $best, $cells))
        } else {
            $summary.Add(('| {0} | | |{1}' -f $key, (($values | ForEach-Object { " $_ |" }) -join '')))
        }
    }
    $summary.Add('')
}
$summaryFile = Join-Path $OutDir 'summary.md'
$summary | Set-Content -LiteralPath $summaryFile -Encoding utf8
Write-Host "Summary: $summaryFile"
$summary | ForEach-Object { Write-Host $_ }
