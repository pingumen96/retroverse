# I1 spike (f): emulator launch and return to the borderless fullscreen window

Decision: [ADR-0006](../decisions/ADR-0006-emulator-launch.md). Roadmap row: I1 (f).

## Question and result

Success criterion (roadmap I1): **5 of 5 RetroArch launches back in the foreground, fullscreen, with pad input and
60 FPS within 1 s of the emulator's exit; wrappers wait for the process tree; crashes reported; never a kill.**

**Met.** 30 of 30 RetroArch launches (3 sessions of 5 with each window policy) returned within the criteria, the
latest of the six measurements at most 400 ms after RetroArch's exit (median 71.5 ms keeping the window, 82.7 ms
minimizing it). The 77 launches of the fake emulator (exit codes, wrapper and process tree, crash, exclusive display
mode, focus stealing, confirmation) all returned within the criteria too, the exclusive display mode being the slowest
(at most 785 ms). No launch needed a foreground remedy after the emulator's exit; nothing was ever terminated by the
product.

## What was built

- `rv_launcher` (GPU-free, `tests/unit/launcher`, 30 test cases): Windows command-line quoting as the exact inverse of
  `CommandLineToArgvW` (`quoteWindowsArgument`, `buildCommandLine`; fuzzed against the real parser, 3000 random
  argument lists), `LaunchRequest` (argv, never a shell string; working directory, environment overrides, wait
  strategy, timeouts, output log) and `validateLaunchRequest` (absolute path to an x64/x86/ARM64 PE program; batch
  files, shortcuts, DOS and 16-bit programs refused), `mergeEnvironment`, `classifyExitCode` (NTSTATUS crashes,
  loader errors, Ctrl+C), the runner port `IProcessRunner` / `IRunningProcess` with `ProcessEvent`s, and the
  `LaunchSession` state machine (Idle, Validating, Starting, Running, StillRunning, AwaitingConfirmation, Returned,
  FailedToStart, Crashed).
- `rv_platform`: `Win32ProcessRunner` (`CreateProcessW` suspended into a job object without kill-on-close, completion
  port, watcher thread, output capture, `AllowSetForegroundWindow`, `StartCallbacks::beforeResume`) and
  `WindowHandover` (window state, minimize or keep, restore, fullscreen on the original monitor, foreground with
  remedies, a foreground event hook). `tests/platform/test_process_runner.cpp` (14 test cases, hidden child processes).
- `spikes/launch`: `rv_fake_emulator`, `spike_launch` (+ GPU-free `spike_launch_core` and its 8 test cases),
  `measure.ps1` (sessions with the machine's load sampled), `summarize_reports.py` (the tables below),
  `snapshot_tree.py` (read-only before/after snapshots). ctest label `desktop`: 9 `desktop.launch.*` cases (preset
  `msvc-x64-debug-desktop`).

## Method

**Machine.** Intel i7-9700K, NVIDIA GeForce RTX 2080 SUPER (driver 617.42), two 2560x1440 displays at 164.84 Hz (125%
scaling), Windows 11 build 29671, SDL 3.4.18. `ForegroundLockTimeout` is 2147483647 ms on this machine: the foreground
lock never expires through idleness, so a background process is refused the foreground unless another rule of
`SetForegroundWindow` lets it through. Other agents were building and running a GPU video spike during the
measurements: the load sampled every second (CPU: `Win32_PerfFormattedData_PerfOS_Processor`; GPU: `nvidia-smi`) is
given with each set.

**The window.** `spike_launch` (Release) opens a Retroverse-like window in borderless fullscreen at 2560x1440 with vsync
and renders an animated fullscreen shader through `rv_render` (6.07 ms frames; warm-up p99 6.4 ms). Before each launch
it makes sure it is the foreground window (as when a player confirms a game in Retroverse). An SDL virtual gamepad is
attached at start.

**A launch.** `WindowHandover::prepare()` (state, foreground hook), `LaunchSession::start()` through
`Win32ProcessRunner`, the window handed over in `beforeResume` (minimized, or kept), then the main loop blocks in
`SDL_WaitEventTimeout` without rendering or reading input, woken by the runner's thread (`SDL_PushEvent`). When the
session ends: `WindowHandover::restore()`, then every frame `update()`, rendering, and a press of the virtual pad's A
button toggled every frame until the input collector reports it.

**The six measurements**, in milliseconds after the emulator's exit, which is the system's exit time of the process
that ends the session (`GetProcessTimes`, converted to the steady clock):

| | Measurement | Taken when |
|---|---|---|
| (a) | Restored | the window is no longer minimized (at once when kept) |
| (b) | Foreground | the foreground hook (EVENT_SYSTEM_FOREGROUND) sees the window take the foreground back from the emulator; negative: before the emulator's process ended |
| (c) | Fullscreen | the window is in fullscreen and covers the monitor it was on |
| (d) | First frame | the first frame after the return is presented |
| (e) | 60 FPS | the first frame of 30 consecutive frame intervals of at most 20.8 ms (no vblank missed at 60 Hz) |
| (f) | Input | the virtual pad's press reaches `SdlInputCollector` (SDL drops pad presses while no Retroverse window has keyboard focus: this also proves the focus) |

**Foreground lock active.** The fake emulator taps Shift into its own window before closing (`--input-at-end`), so the
last input belongs to the emulator, as when a player presses a key; every launch below ran with the lock active
(column "Lock").

**RetroArch, isolated.** RetroArch 1.21.0 (`E:\Programmi\Retroarch`, d3d11, borderless fullscreen, `pause_nonactive`),
gambatte core, *Mole Mania (USA, Europe) (SGB Enhanced).gb* read from the user's library:

```text
retroarch.exe -L E:\Programmi\Retroarch\cores\gambatte_libretro.dll --appendconfig=<scratch>\retroverse-isolated.cfg
  --max-frames=240 --sram-mode=noload-nosave --log-file=<scratch>\logs\retroarch-<n>.log --verbose
  "I:\Giochi\Emulatori\gb\1 GB US - H-Q\Mole Mania (USA, Europe) (SGB Enhanced).gb"
```

The appended configuration (`spike_launch_core`'s `retroArchAppendConfig`) turns off everything RetroArch saves on its
own (configuration and remaps on exit, history, runtime logs, the core info cache, SRAM autosave, automatic save states)
and moves every directory or file it could write into the scratch directory under `build/` (saves, states,
screenshots, logs, the configuration directory with core options and overrides, playlists, history, cache). Snapshots
of RetroArch's folder (48,702 files: size, modification time, SHA-256 of configurations, saves, playlists and logs),
of the ROM's folder and of ES-DE's settings and `gb` gamelist, taken before the first and after the 31st launch, show
**0 differences**. RetroArch wrote only into the scratch directory: its log and `config\Gambatte\Gambatte.opt` (it
saves the core options on exit, which would otherwise have gone into its own folder). It read its favorites playlist
from its folder.

**DuckStation was not launched.** Its settings, memory cards, caches and logs live in a user data folder that only a
`portable.txt` in its installation folder would move, which would be a write into the user's emulator installation,
and it checks for updates at startup. Exclusive fullscreen with a display mode change is covered by the fake
emulator's `--exclusive` mode instead (1176x664 at 60 Hz on the 2560x1440 display).

## Results

### RetroArch, keep (window left in place, not rendering)

CPU load during the sessions: median 69% (33-100%); GPU 26% (9-43%).

| Session | Launch | Outcome | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest | Remedies | Lock | Other app at launch / return |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | Returned | 18.6 | -200.1 | 18.6 | 18.7 | 376.2 | 18.7 | 376.2 | none | yes | 0 / 0 |
| 1 | 2 | Returned | 18.8 | -180.3 | 18.8 | 19.2 | 71.5 | 19.2 | 71.5 | none | yes | 0 / 0 |
| 1 | 3 | Returned | 21.9 | -348.7 | 21.9 | 22.1 | 74.5 | 22.1 | 74.5 | none | yes | 0 / 0 |
| 1 | 4 | Returned | 16.1 | -175.3 | 16.1 | 16.4 | 65.5 | 16.4 | 65.5 | none | yes | 0 / 0 |
| 1 | 5 | Returned | 19.2 | -192.3 | 19.2 | 19.5 | 80.8 | 19.5 | 80.8 | none | yes | 0 / 0 |
| 2 | 1 | Returned | 20.2 | -180.6 | 20.2 | 20.2 | 367.3 | 20.3 | 367.3 | none | yes | 0 / 0 |
| 2 | 2 | Returned | 17.7 | -182.8 | 17.7 | 18.1 | 60.5 | 18.1 | 60.5 | none | yes | 0 / 0 |
| 2 | 3 | Returned | 17.1 | -186.9 | 17.1 | 17.4 | 68.0 | 17.4 | 68.0 | none (before the launch: raise, attach-thread-input) | yes | 107.7 / 813.8 |
| 2 | 4 | Returned | 17.3 | -181.1 | 17.3 | 17.5 | 71.5 | 17.5 | 71.5 | none | yes | 0 / 0 |
| 2 | 5 | Returned | 18.0 | -171.0 | 18.0 | 18.3 | 67.7 | 18.3 | 67.7 | none | yes | 0 / 0 |
| 3 | 1 | Returned | 17.3 | -197.8 | 17.3 | 17.3 | 368.8 | 17.4 | 368.8 | none | yes | 0 / 0 |
| 3 | 2 | Returned | 20.2 | -298.1 | 20.2 | 20.4 | 80.4 | 20.5 | 80.4 | none | yes | 0 / 0 |
| 3 | 3 | Returned | 17.6 | -318.4 | 17.6 | 18.0 | 71.3 | 18.0 | 71.3 | none | yes | 0 / 0 |
| 3 | 4 | Returned | 16.4 | -178.5 | 16.4 | 16.6 | 70.4 | 16.6 | 70.4 | none | yes | 0 / 0 |
| 3 | 5 | Returned | 17.8 | -191.4 | 17.8 | 18.1 | 74.8 | 18.1 | 74.8 | none | yes | 0 / 0 |

| | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest |
|---|---|---|---|---|---|---|---|
| Session medians | 18.8, 17.7, 17.6 | -192.3, -181.1, -197.8 | 18.8, 17.7, 17.6 | 19.2, 18.1, 18.0 | 74.5, 68.0, 74.8 | 19.2, 18.1, 18.0 | 74.5, 68.0, 74.8 |
| Median of the 15 | 17.8 | -186.9 | 17.8 | 18.1 | 71.5 | 18.1 | 71.5 |
| Best | 16.1 | -348.7 | 16.1 | 16.4 | 60.5 | 16.4 | 60.5 |
| Worst | 21.9 | -171.0 | 21.9 | 22.1 | 376.2 | 22.1 | 376.2 |

Launch 2.3: a terminal window took the foreground between two launches (outside activity on the machine), so the spike
took it back before launching (plain raise refused, AttachThreadInput accepted) and the terminal showed briefly again
at the return; the measurements are unaffected.

### RetroArch, minimize

CPU load: median 61% (31-100%); GPU 31% (7-47%).

| Session | Launch | Outcome | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest | Remedies | Lock | Other app at launch / return |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | Returned | 31.3 | 31.3 | 31.3 | 31.7 | 359.9 | 31.7 | 359.9 | none | yes | 118.4 / 243.1 |
| 1 | 2 | Returned | 32.2 | 32.3 | 32.2 | 32.9 | 86.2 | 32.9 | 86.2 | none | yes | 117.8 / 236.3 |
| 1 | 3 | Returned | 32.9 | 33.0 | 32.9 | 33.2 | 88.1 | 33.2 | 88.1 | none | yes | 146.5 / 257.4 |
| 1 | 4 | Returned | 33.8 | 33.8 | 33.8 | 34.1 | 87.7 | 34.1 | 87.7 | none | yes | 792.0 / 254.7 |
| 1 | 5 | Returned | 32.7 | 32.9 | 32.7 | 34.5 | 89.7 | 34.5 | 89.7 | none | yes | 126.2 / 439.1 |
| 2 | 1 | Returned | 30.9 | 31.0 | 30.9 | 31.8 | 370.4 | 31.8 | 370.4 | none | yes | 101.4 / 4695.9 |
| 2 | 2 | Returned | 31.0 | 31.0 | 31.0 | 31.4 | 73.2 | 31.4 | 73.2 | none | yes | 158.1 / 234.2 |
| 2 | 3 | Returned | 29.2 | 29.2 | 29.2 | 29.4 | 81.4 | 29.4 | 81.4 | none | yes | 106.5 / 230.7 |
| 2 | 4 | Returned | 26.6 | 26.7 | 26.6 | 26.9 | 74.3 | 26.9 | 74.3 | none | yes | 120.6 / 239.1 |
| 2 | 5 | Returned | 35.4 | 35.5 | 35.4 | 35.6 | 82.7 | 35.6 | 82.7 | none | yes | 113.8 / 242.2 |
| 3 | 1 | Returned | 31.1 | 31.3 | 31.1 | 31.7 | 399.9 | 31.7 | 399.9 | none | yes | 110.3 / 253.9 |
| 3 | 2 | Returned | 28.1 | 28.1 | 28.1 | 28.8 | 70.0 | 28.8 | 70.0 | none | yes | 122.2 / 253.0 |
| 3 | 3 | Returned | 27.7 | 27.7 | 27.7 | 27.8 | 62.0 | 27.8 | 62.0 | none | yes | 114.9 / 239.7 |
| 3 | 4 | Returned | 30.0 | 30.1 | 30.0 | 30.8 | 78.2 | 30.8 | 78.2 | none | yes | 105.9 / 233.6 |
| 3 | 5 | Returned | 28.0 | 28.0 | 28.0 | 28.1 | 78.7 | 28.1 | 78.7 | none | yes | 114.3 / 229.1 |

| | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest |
|---|---|---|---|---|---|---|---|
| Session medians | 32.7, 30.9, 28.1 | 32.9, 31.0, 28.1 | 32.7, 30.9, 28.1 | 33.2, 31.4, 28.8 | 88.1, 81.4, 78.2 | 33.2, 31.4, 28.8 | 88.1, 81.4, 78.2 |
| Median of the 15 | 31.0 | 31.0 | 31.0 | 31.7 | 82.7 | 31.7 | 82.7 |
| Best | 26.6 | 26.7 | 26.6 | 26.9 | 62.0 | 26.9 | 62.0 |
| Worst | 35.4 | 35.5 | 35.4 | 35.6 | 399.9 | 35.6 | 399.9 |

"Other app": the user's browser, which Windows activated once the window was minimized and again when RetroArch's window
closed, held the foreground for a median 118 ms at the launch and 242 ms at the return (RetroArch closes its window
about 200 ms before its process ends). The 4.7 s and 792 ms values include a terminal that took the foreground during
those launches (outside activity).

### Fake emulator (Release, foreground lock active, 3 sessions x 5 launches)

| Set | Launches | Latest: median / best / worst | 60 FPS median | Input median | Foreground median | Other app at launch / return (median) | Load: CPU / GPU median |
|---|---|---|---|---|---|---|---|
| Keep | 15 of 15 | 74.6 / 68.0 / 364.8 | 74.6 | 14.6 | -16.6 | 0 / 0 (0 of 15 launches) | 61% / 39% |
| Minimize | 15 of 15 | 81.0 / 71.8 / 408.5 | 81.0 | 30.1 | 29.4 | 157 / 69 (15 of 15) | 59% / 31% |
| ProcessTree through a wrapper, keep (3 x 3) | 9 of 9 | 88.7 / 68.8 / 403.7 | 88.7 | 14.0 | -25.2 | 0 / 0 | 80% / 44% |
| Exclusive display mode, keep (3 x 3) | 9 of 9 | 507.1 / 170.6 / 784.6 | 507.1 | 248.9 | -168.4 | 0 / 0 | 100% / 28% |
| Exclusive display mode, minimize (3 x 3) | 9 of 9 | 373.5 / 249.1 / 776.3 | 373.5 | 272.4 | 268.6 | 1867 / 0 | 100% / 24% |

Session medians of the latest measurement: keep 77.1, 75.3, 73.4; minimize 78.4, 81.1, 81.0; wrapper 78.6, 220.0, 88.7;
exclusive keep 290.9, 412.1, 667.5; exclusive minimize 373.5, 449.4, 365.5.

### Scenarios (ctest `desktop`, Debug)

`./scripts/test.ps1 -Preset msvc-x64-debug-desktop`: 9 of 9 passed (94 s).

| Case | What it shows |
|---|---|
| `fake_direct_minimize`, `fake_direct_keep` | the plain return with each policy (3 launches each) |
| `fake_tree_minimize`, `fake_tree_keep` | a wrapper that exits after 30-96 ms: the session waits for its child (ProcessTree), 2 processes seen, ends with the tree ~1.4 s later |
| `fake_confirm_keep` | UserConfirmReturn: the launcher's exit leads to AwaitingConfirmation, the injected pad press confirms |
| `fake_crash_minimize` | an access violation: `Crashed`, `0xC0000005 STATUS_ACCESS_VIOLATION`, the window back within 45 ms |
| `fake_exclusive_minimize`, `fake_exclusive_keep` | a display mode change (1176x664 at 60 Hz): fullscreen re-applied, the window moved back to its monitor |
| `fake_steal_focus_keep` | an emulator that forces itself into the foreground: no effect on the return |

The runner's own tests (`tests/platform`, label `unit`, hidden processes): exit codes including 0xFFFFFFFF, an unclean
exit within the startup grace (FailedToStart), a crash, ProcessTree through a wrapper (the child's exit code reported,
not the wrapper's), DirectProcess with a wrapper (returns at once, quick exit, the child left running), a hang that
becomes StillRunning and survives the session and the runner (ended by the test harness), arguments with spaces,
quotes, trailing backslashes, empty strings, `%`, `^`, apostrophes and Unicode received verbatim by the child,
environment overrides, the working directory, captured output, non-programs refused, a broken image failing without
a dialog, a child breaking away from the job.

### Notepad

On this Windows build `C:\Windows\System32\notepad.exe` starts the packaged `Notepad.exe` as a child inside the job and
waits for it. With UserConfirmReturn and keep, the session reached AwaitingConfirmation when the stub exited (after the
harness closed Notepad's window) and the pad press confirmed it 100 ms later; ProcessTree or DirectProcess would also
have worked. A launcher whose program runs outside the job (a packaged application activated by the system) needs
UserConfirmReturn.

## Findings

1. **SDL 3.4.18 ignores `SDL_RestoreWindow` for fullscreen windows** (`WIN_RestoreWindow` does nothing when
   `SDL_WINDOW_FULLSCREEN` is set). The hand-over restores a minimized window with `ShowWindow(SW_RESTORE)`, which also
   brought it to the foreground at once in every minimize launch, the foreground lock notwithstanding; SDL then
   re-enters fullscreen on the restored event.
2. **Hand the window over after `CreateProcessW`, before the emulator runs.** Minimizing first makes Windows activate
   another window, so Retroverse is no longer the foreground process when the emulator starts. The runner calls
   `StartCallbacks::beforeResume` between creating the suspended process and `ResumeThread`.
3. **`AllowSetForegroundWindow` on the suspended child fails for console-subsystem programs**
   (`ERROR_INVALID_PARAMETER`: the process has no window-manager state yet; 0 of 77 fake launches) and succeeds for
   GUI-subsystem ones (30 of 30 RetroArch launches). Every emulator took the foreground anyway: it was started by the
   foreground process.
4. **Keep beats minimize.** With the window kept, Windows activates it as soon as the emulator's window closes, the next
   window in the z-order (170-350 ms before RetroArch's process ends): no remedy, no other application ever visible.
   Minimized, the next window is whatever the user had behind (the browser in every launch), visible for about 120-160
   ms at the launch and 70-250 ms at the return.
5. **Remedies.** None was needed after an emulator's exit in the 107 measured launches. When an outside window had
   taken the foreground (a terminal, between two launches, and in a development run during the emulator's run) the plain
   raise was refused and AttachThreadInput succeeded; the Alt key and the flash were never reached.
6. **Detection.** The system's exit time precedes the signaled process handle by 13-18 ms (process teardown); the
   watcher thread wakes the main loop through `SDL_PushEvent` in 0.13-0.18 ms (median).
7. **Frame pacing after a return.** The first return of a session stalls twice (about 100 ms, then 200-340 ms; 60 FPS
   back after 350-410 ms); later returns stall once (40-65 ms; 60 FPS back after about 70 ms). The swap chain
   (independent flip versus composed presentation) is the likely cause; not investigated further (BACKLOG).
8. **Exclusive display modes.** The mode change shrinks a kept window (fullscreen re-applied, 200-250 ms) and can move
   a minimized one to the other monitor (7 of 9 launches; moved back to its display). Worst case 785 ms.
9. **`CreateProcessW` can show a modal box in the calling process.** For a DOS or 16-bit image ("Unsupported 16-Bit
   Application"), with the default error mode (as ctest gives its children), the call blocks on a message box until
   dismissed. Validation now refuses images that are not x64, x86 or ARM64 PE programs, and the runner sets
   `SEM_FAILCRITICALERRORS` on its thread around `CreateProcessW` (tested under ctest).
10. **Batch files are refused**: `CreateProcessW` runs them through `cmd.exe`, which re-parses arguments (`%`, `^`, `&`,
    `|`; the "BatBadBut" class of injection). A `.bat` wrapper would need a verified profile with cmd-specific
    escaping.
11. **Children that break away from the job are not followed** (`JOB_OBJECT_LIMIT_BREAKAWAY_OK` lets them): the tree
    ends without them (tested).

## Reproduce

```powershell
./scripts/build.ps1 -Preset msvc-x64-release
./spikes/launch/measure.ps1 -Name fake-keep -SpikeArgs '--target','fake','--policy','keep','--repeat','5','--fake-input'
./spikes/launch/measure.ps1 -Name retroarch-keep -SpikeArgs '--target','retroarch','--retroarch','<retroarch.exe>',
    '--core','<gambatte_libretro.dll>','--rom','<game.gb>','--max-frames','240','--policy','keep','--repeat','5'
python -I spikes/launch/summarize_reports.py build/spike-launch/measurements/retroarch-keep-*.json
python -I spikes/launch/snapshot_tree.py snapshot before.json <RetroArch folder> <ROM folder>   # and compare
./scripts/test.ps1 -Preset msvc-x64-debug-desktop
```
