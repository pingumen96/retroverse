<#
.SYNOPSIS
    Runs spike_launch for several sessions and samples the machine's load meanwhile (docs/spikes/i1-launch.md).

.DESCRIPTION
    Each session is one spike_launch run (its own window, warm-up and --repeat launches) writing its JSON report to
    <OutDir>/<Name>-<session>.json. The CPU load of the whole machine (and the GPU's utilization, when nvidia-smi is
    available) is sampled once per second during the sessions, since other builds or GPU work may run in parallel,
    and written to <OutDir>/<Name>-load.json. Every report records the build (--source-revision: the git commit, and
    whether tracked files differed from it, with a hash of that difference). Run from the repository root, with nothing
    else in the foreground that could take it during the launches. -SpikeArgs go to spike_launch (target, strategy,
    policy, repeat, ...).

.EXAMPLE
    ./spikes/launch/measure.ps1 -Name fake-keep -Sessions 3 -SpikeArgs '--target','fake','--policy','keep','--repeat','5','--fake-input'
#>
param(
    [Parameter(Mandatory)] [string]$Name,
    [int]$Sessions = 3,
    [string]$Preset = 'msvc-x64-release',
    [string]$OutDir = 'build/spike-launch/measurements',
    [string[]]$SpikeArgs = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$spike = Join-Path $root "build/$Preset/bin/spike_launch.exe"
if (-not (Test-Path $spike)) { throw "spike_launch.exe not found: build the $Preset preset first" }
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force $out | Out-Null

# The build's source: the commit, plus a hash of the uncommitted changes to tracked files when there are any.
$revision = (& git -C $root rev-parse HEAD 2>$null | Select-Object -First 1)
if (-not $revision) { $revision = 'unknown' }
$changes = (& git -C $root diff HEAD 2>$null | Out-String)
if ($changes.Trim()) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $digest = -join ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($changes)) | ForEach-Object { $_.ToString('x2') })
    $revision += " with uncommitted changes (git diff HEAD sha256 $($digest.Substring(0, 16)))"
}
$built = (Get-Item $spike).LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ssZ')
Write-Host "Source: $revision; spike_launch.exe built $built"

# Load sampler: Win32_PerfFormattedData (language-neutral, unlike Get-Counter's localized counter paths) and nvidia-smi.
$loadFile = Join-Path $out "$Name-load.json"
$sampler = Start-Job -ScriptBlock {
    param($file)
    $cpu = New-Object System.Collections.Generic.List[double]
    $gpu = New-Object System.Collections.Generic.List[double]
    $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    while ($true) {
        $cpu.Add([double](Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime)
        if ($smi) {
            $value = & $smi.Source --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>$null | Select-Object -First 1
            if ($value) { $gpu.Add([double]$value.Trim()) }
        }
        @{ cpu = $cpu; gpu = $gpu } | ConvertTo-Json -Compress | Set-Content -Path $file -Encoding utf8
        Start-Sleep -Seconds 1
    }
} -ArgumentList $loadFile

$exitCodes = @()
try {
    for ($session = 1; $session -le $Sessions; ++$session) {
        $report = Join-Path $out "$Name-$session.json"
        $data = Join-Path $root "build/spike-launch/data/$Name-$session"
        Write-Host "Session $session of ${Sessions}: $Name"
        & $spike @SpikeArgs --data-dir $data --report $report --source-revision $revision | Out-Null
        $exitCodes += $LASTEXITCODE
        Write-Host "  exit code $LASTEXITCODE, report $report"
    }
}
finally {
    Stop-Job $sampler | Out-Null
    Remove-Job $sampler -Force | Out-Null
}
function Describe([object[]]$values, [string]$what) {
    if (-not $values -or $values.Count -eq 0) { return "${what}: not sampled" }
    $sorted = $values | Sort-Object
    $median = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]
    return ('{0}: {1} samples, median {2}%, min {3}%, max {4}%' -f $what, $sorted.Count, $median, $sorted[0], $sorted[-1])
}
if (Test-Path $loadFile) {
    $load = Get-Content $loadFile -Raw | ConvertFrom-Json
    Write-Host (Describe $load.cpu 'CPU load')
    Write-Host (Describe $load.gpu 'GPU utilization')
}
Write-Host "Exit codes: $($exitCodes -join ', ')"
if ($exitCodes | Where-Object { $_ -ne 0 }) { exit 1 }
exit 0
