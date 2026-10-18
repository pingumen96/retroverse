<#
.SYNOPSIS
    Prepares the manual session of Gate M1 (docs/testing/gate-m1.md): a user data directory with the real ES-DE
    library imported and RetroArch assigned to one system through an isolated profile. Starts nothing.

.DESCRIPTION
    For the user, by hand, with ES-DE closed. Using the Release build (./scripts/build.ps1 -Preset msvc-x64-release):
      1. retroverse-cli --data-dir <DataDir> library create, the ES-DE home given as ES-DE's --home takes it (the
         folder that holds ES-DE\), with the install folder and, when given, the ROM and media roots;
      2. retroverse-cli import (the whole library, about 15 s warm);
      3. the isolated RetroArch profile, as I2 step 12 ran it: a scratch folder <DataDir>\retroarch-scratch with the
         append configuration of the I1 launch spike without its frame limit and its mute (nothing saved on RetroArch's
         own initiative, every file it could write moved into the scratch folder, fullscreen, with sound, offline) and
         a user emulators.json in <DataDir>\config holding the profile "retroarch-isolated" (the RetroArch executable
         given, the system's core, --appendconfig, --sram-mode=noload-nosave and a log file in the scratch folder; no
         frame limit, so the game is played until it is quit);
      4. retroverse-cli assign <System> --profile retroarch-isolated, then `profiles --system <System>` to show it ready
         (RV_LAUNCH_ALLOWLIST naming RetroArch for these two commands only; the caller's environment is restored).
    It then prints the commands of both runs of the session (docs/testing/gate-m1.md, "Manual session"): the normal run
    without RV_LAUNCH_ALLOWLIST (checklist items 1-11: every ready system launches as ES-DE would, RetroArch with the
    user's own configuration and saves outside <System>), and the isolated run with it (item 12: only the systems
    assigned to the isolated profile can launch, the automation guard refuses the others). Nothing is written outside
    <DataDir>; the library and RetroArch's folder are only read. Exit codes: 0 ready; 1 a command failed; 2 it could
    not start (ES-DE running, a folder or program missing, a data directory that exists already without -Force).

.EXAMPLE
    ./scripts/m1-manual-setup.ps1 -DataDir D:\RetroverseM1 -EsdeHome C:\Users\me\ES-DE -EsdeInstall 'C:\Program Files\ES-DE' `
        -RetroArchExe C:\RetroArch\retroarch.exe
    Example paths only: give your own (the ROM and media roots come from ES-DE's settings unless -RomRoot and
    -MediaRoot are given).
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$DataDir,
    [string]$EsdeHome = $env:RV_REAL_ESDE_HOME,
    [string]$EsdeInstall = $env:RV_REAL_ESDE_INSTALL,
    [string]$RomRoot = $env:RV_REAL_ROM_ROOT,
    [string]$MediaRoot = $env:RV_REAL_MEDIA_ROOT,
    [string]$RetroArchExe = $env:RV_REAL_RETROARCH_EXE,
    [string]$System = 'gb',
    [string]$Core = 'gambatte_libretro.dll',
    [string]$Preset = 'msvc-x64-release',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build/$Preset/bin"
$cli = Join-Path $bin 'retroverse-cli.exe'

function Stop-Setup([string]$Message) {
    Write-Host "m1-manual-setup: $Message" -ForegroundColor Red
    exit 2
}

if (Get-Process -Name 'ES-DE' -ErrorAction SilentlyContinue) { Stop-Setup 'ES-DE is running: close it first.' }
if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) { Stop-Setup "$cli is not built: ./scripts/build.ps1 -Preset $Preset" }
if (-not $EsdeHome -or -not (Test-Path -LiteralPath $EsdeHome -PathType Container)) { Stop-Setup 'give the ES-DE home (-EsdeHome)' }
if ((Split-Path -Leaf $EsdeHome) -ne 'ES-DE') { Stop-Setup 'the ES-DE home must be the folder named ES-DE (library create takes its parent)' }
if (-not $EsdeInstall -or -not (Test-Path -LiteralPath $EsdeInstall -PathType Container)) { Stop-Setup 'give the ES-DE install folder (-EsdeInstall)' }
if (-not $RetroArchExe -or -not (Test-Path -LiteralPath $RetroArchExe -PathType Leaf)) { Stop-Setup 'give the RetroArch executable (-RetroArchExe)' }
$coreFile = Join-Path (Split-Path -Parent $RetroArchExe) "cores\$Core"
if (-not (Test-Path -LiteralPath $coreFile -PathType Leaf)) { Stop-Setup "the core $coreFile does not exist (-Core)" }
if ((Test-Path -LiteralPath $DataDir) -and -not $Force) {
    Stop-Setup "$DataDir exists: give another folder, or -Force to record the library and import again into it"
}
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
$DataDir = (Resolve-Path -LiteralPath $DataDir).Path

# Runs retroverse-cli --data-dir <DataDir> <arguments>; stops at the first failure.
function Invoke-Cli([string[]]$Arguments) {
    Write-Host "retroverse-cli $($Arguments -join ' ')"
    & $cli --data-dir $DataDir @Arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Host "m1-manual-setup: retroverse-cli $($Arguments -join ' ') exited $LASTEXITCODE" -ForegroundColor Red
        exit 1
    }
}

# 1. and 2. The library and its import.
$create = @('library', 'create', '--home', (Split-Path -Parent $EsdeHome), '--install', $EsdeInstall)
if ($RomRoot) { $create += @('--rom-root', $RomRoot) }
if ($MediaRoot) { $create += @('--media-root', $MediaRoot) }
if ($Force) { $create += '--force' }
Invoke-Cli $create
Invoke-Cli @('import')

# 3. The isolated profile: the scratch folders, the append configuration (the content of the I1 launch spike's
# retroArchAppendConfig(), spikes/launch/core/src/targets.cpp), the user's emulators.json.
$scratch = Join-Path $DataDir 'retroarch-scratch'
foreach ($folder in @('saves', 'states', 'screenshots', 'logs', 'config', 'config\remaps', 'playlists', 'playlists\logs',
        'cache')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $scratch $folder) | Out-Null
}
if ($scratch -match '[^\x20-\x7E]') { Stop-Setup "the scratch folder $scratch must be ASCII (the guard checks --appendconfig)" }
$entries = [ordered]@{
    config_save_on_exit = 'false'; remap_save_on_exit = 'false'; history_list_enable = 'false'
    content_runtime_log = 'false'; content_runtime_log_aggregate = 'false'; core_info_cache_enable = 'false'
    savestate_auto_save = 'false'; savestate_auto_load = 'false'; autosave_interval = '0'
    auto_overrides_enable = 'false'; auto_remaps_enable = 'false'; savefiles_in_content_dir = 'false'
    savestates_in_content_dir = 'false'; systemfiles_in_content_dir = 'false'; screenshots_in_content_dir = 'false'
    savefile_directory = "$scratch\saves"; savestate_directory = "$scratch\states"
    screenshot_directory = "$scratch\screenshots"; log_dir = "$scratch\logs"; rgui_config_directory = "$scratch\config"
    input_remapping_directory = "$scratch\config\remaps"
    core_options_path = "$scratch\config\retroarch-core-options.cfg"; playlist_directory = "$scratch\playlists"
    runtime_log_directory = "$scratch\playlists\logs"
    content_history_path = "$scratch\playlists\content_history.lpl"
    content_favorites_path = "$scratch\playlists\content_favorites.lpl"
    content_image_history_path = "$scratch\playlists\content_image_history.lpl"
    content_music_history_path = "$scratch\playlists\content_music_history.lpl"
    content_video_history_path = "$scratch\playlists\content_video_history.lpl"
    cache_directory = "$scratch\cache"
    video_fullscreen = 'true'; audio_mute_enable = 'false'; cheevos_enable = 'false'; discord_allow = 'false'
    network_cmd_enable = 'false'
}
$config = "# Written by scripts/m1-manual-setup.ps1 (Retroverse, Gate M1's manual session): appended to the user's`n" +
    "# configuration so that the session writes nothing outside this directory.`n"
foreach ($key in $entries.Keys) { $config += "$key = `"$($entries[$key])`"`n" }
$configFile = Join-Path $scratch 'retroverse-isolated.cfg'
[IO.File]::WriteAllText($configFile, $config)
$retroArchProfile = [ordered]@{
    id          = 'retroarch-isolated'
    displayName = 'RetroArch (isolated, Gate M1)'
    executable  = $RetroArchExe
    arguments   = @('-L', '{var:core}', "--appendconfig=$configFile", '--sram-mode=noload-nosave',
        "--log-file=$scratch\logs\retroarch.log", '{rom}')
    variables   = [ordered]@{ core = "{emulator_dir}\cores\$Core" }
    requiredFiles = @('{var:core}')
    systems     = [ordered]@{ $System = [ordered]@{} }
}
$emulators = [ordered]@{ schemaVersion = 1; profiles = @($retroArchProfile) }
New-Item -ItemType Directory -Force -Path (Join-Path $DataDir 'config') | Out-Null
$emulatorsFile = Join-Path $DataDir 'config\emulators.json'
[IO.File]::WriteAllText($emulatorsFile, ($emulators | ConvertTo-Json -Depth 6) + "`n")

# 4. The profile assigned to the system, and its readiness, with the automation guard naming RetroArch alone (as in the
# isolated run). The variable is set for these two commands only: a script run with ./ shares the caller's process, so
# the caller's PowerShell gets its environment back as it was (an exit within Invoke-Cli included).
$allowList = (Resolve-Path -LiteralPath $RetroArchExe).Path
$previousAllowList = [Environment]::GetEnvironmentVariable('RV_LAUNCH_ALLOWLIST', 'Process')
try {
    $env:RV_LAUNCH_ALLOWLIST = $allowList
    Invoke-Cli @('assign', $System, '--profile', 'retroarch-isolated')
    Invoke-Cli @('profiles', '--system', $System)
} finally {
    [Environment]::SetEnvironmentVariable('RV_LAUNCH_ALLOWLIST', $previousAllowList, 'Process')
}

$app = Join-Path $bin 'retroverse.exe'
Write-Host ''
Write-Host 'Ready. The session has two kinds of run (docs/testing/gate-m1.md, "Manual session"):'
Write-Host ''
Write-Host "1. The normal run, checklist items 1-11: a PowerShell WITHOUT RV_LAUNCH_ALLOWLIST. Every ready system launches"
Write-Host "   as ES-DE would: $System through the isolated profile, the others with ES-DE's own command (RetroArch with your"
Write-Host '   own configuration and saves).'
Write-Host '     Remove-Item Env:RV_LAUNCH_ALLOWLIST -ErrorAction SilentlyContinue'
Write-Host "     & '$app' --data-dir '$DataDir' --fullscreen"
Write-Host ''
Write-Host "2. The isolated run, checklist item 12 only: RV_LAUNCH_ALLOWLIST naming RetroArch turns on the automation guard."
Write-Host "   Only the systems assigned to the isolated profile ($System) can launch; the others are refused (their panel"
Write-Host "   says the protection for automated runs blocked them). Snapshot first (gate-m1.md, preparation step 4), then:"
Write-Host "     `$env:RV_LAUNCH_ALLOWLIST = '$allowList'"
Write-Host "     & '$app' --data-dir '$DataDir' --fullscreen"
Write-Host '     Remove-Item Env:RV_LAUNCH_ALLOWLIST    # afterwards, before playing normally again'
Write-Host ''
if ($null -ne $previousAllowList) {
    Write-Host 'Note: RV_LAUNCH_ALLOWLIST is set in this PowerShell now: remove it before the normal run.' -ForegroundColor Yellow
}
Write-Host "Afterwards: & '$cli' --data-dir '$DataDir' sessions"
