# I1 spike (f): emulator launch and return to the borderless fullscreen window

Decision: [ADR-0006](../decisions/ADR-0006-emulator-launch.md). Roadmap row: I1 (f).

## Question and result

Success criterion (roadmap I1): **5 of 5 RetroArch launches back in the foreground, fullscreen, with pad input and
60 FPS within 1 s of the emulator's exit; wrappers wait for the process tree; crashes reported; never a kill.**

**Met.** 30 of 30 RetroArch launches (3 sessions of 5 with each window policy) returned within the criteria, the
latest of the six measurements at most 368 ms after RetroArch's exit (median 74.1 ms keeping the window, 74.9 ms
minimizing it). The 93 launches of the fake emulator met the criteria too, except, by design, the 3 where a hung
window of another application held the foreground at the return: there every remedy was refused and the taskbar button
flashed until that window went away (3.1 s). The slowest returns are those after an exclusive display mode (at most
710 ms). When another application's window held the foreground at the return, the plain raise was refused every time
and AttachThreadInput brought the window back 44-121 ms after the exit (18 of 18). Nothing was ever terminated by the
product.

Every number below, the Notepad check aside, was measured after the review of the first version of this spike, with
the build named in each report (`build.sourceRevision`): commit `bba5105` for the RetroArch, fake keep and minimize
and exclusive sets, commit `a60ee97` for the process tree and the "another window" sets. The only changes between the
two (the tree's end counted at every wake-up of the watcher, AttachThreadInput tried at once after a refused raise)
are on paths the first sets did not take: DirectProcess sessions, no remedy at any of their returns. A first
measurement campaign (commit `b053022`) ran partly with a build older than its own fixes; its reports are superseded.

## What was built

- `rv_launcher` (GPU-free, `tests/unit/launcher`, 33 test cases): Windows command-line quoting as the exact inverse of
  `CommandLineToArgvW` (`quoteWindowsArgument`, `buildCommandLine`; fuzzed against the real parser, 3000 random
  argument lists), `LaunchRequest` (argv, never a shell string; working directory, environment overrides, wait
  strategy, timeouts, output log) and `validateLaunchRequest` (absolute path to an x64/x86/ARM64 PE program; batch
  files, shortcuts, DOS and 16-bit programs refused), `mergeEnvironment`, `classifyExitCode` (NTSTATUS crashes,
  loader errors, Ctrl+C), the runner port `IProcessRunner` / `IRunningProcess` with `ProcessEvent`s, and the
  `LaunchSession` state machine (Idle, Validating, Starting, Running, StillRunning, AwaitingConfirmation, Returned,
  FailedToStart, Crashed).
- `rv_platform`: `Win32ProcessRunner` (`CreateProcessW` suspended into a job object without kill-on-close, completion
  port, watcher thread, output capture, a console of its own for console programs, `AllowSetForegroundWindow`,
  `StartCallbacks::beforeResume`) and `WindowHandover` (window state, minimize or keep, restore, fullscreen on the
  original monitor, foreground with remedies, a foreground event hook and the polled foreground).
  `tests/platform/test_process_runner.cpp` (19 test cases, hidden child processes).
- `spikes/launch`: `rv_fake_emulator`, `spike_launch` (+ GPU-free `spike_launch_core` and its 8 test cases),
  `measure.ps1` (sessions with the machine's load sampled and the build recorded), `summarize_reports.py` (the tables
  and figures below), `snapshot_tree.py` (read-only before/after snapshots). ctest label `desktop`: 10
  `desktop.launch.*` cases (preset `msvc-x64-debug-desktop`).

## Method

**Machine.** Intel i7-9700K, NVIDIA GeForce RTX 2080 SUPER (driver 617.42), two 2560x1440 displays at 164.84 Hz (125%
scaling), Windows 11 build 29671, SDL 3.4.18. Other agents were building and running GPU work during the
measurements: the load sampled every second (CPU: `Win32_PerfFormattedData_PerfOS_Processor`; GPU: `nvidia-smi`) is
given with each set, and the CPU load of each launch in the tables.

**The window.** `spike_launch` (Release) opens a Retroverse-like window in borderless fullscreen at 2560x1440 with vsync
and renders an animated fullscreen shader through `rv_render` (frames of 6.07 ms; the 99th percentile of the warm-up's
frame intervals was 6.2-6.9 ms per session, up to 18.4 ms in sessions under load). Before each launch it makes sure it
is the foreground window (as when a player confirms a game in Retroverse). An SDL virtual gamepad is attached at start.

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
| (b) | Foreground | the window is the foreground window again: the earlier of the hook's first event for it that was confirmed at delivery (`GetForegroundWindow` named it) and the first poll that found it there; negative: before the emulator's process ended |
| (c) | Fullscreen | the window is in fullscreen and covers the monitor it was on |
| (d) | First frame | the first frame after the return is submitted (`SwapBuffers` returned; with vsync it reaches the screen later, which (e) includes) |
| (e) | 60 FPS | the first frame of 30 consecutive frame intervals of at most 20.8 ms (no vblank missed at 60 Hz) |
| (f) | Input | the virtual pad's press reaches `SdlInputCollector` (SDL drops pad presses while no Retroverse window has keyboard focus: this also proves the focus) |

Hook events alone are not evidence of the foreground: a refused `SetForegroundWindow` still reports
`EVENT_SYSTEM_FOREGROUND` for its window (in 20 of the 21 "another window" launches measured with build `bba5105`,
delivered 6-85 ms after the exit while the window stayed behind until AttachThreadInput, 146-576 ms later, or for
2.9-3.1 s behind a hung window), and events are delivered only while the thread pumps its messages, so they can arrive
late and out of date (in the exclusive minimize set, the browser's events delivered 338-545 ms after the exit in 8 of
9 launches were never in the foreground according to `GetForegroundWindow`). The reports keep both: the hook's events
with a `confirmed` flag, and the polled foreground changes from the restore on. The "Other app" columns use the hook
at the launch (from the first other application's event to the target's first) and at the return (from the first
other application's event after the target's last one to Retroverse's first confirmed one).

**The foreground lock.** `ForegroundLockTimeout` is 2147483647 ms on this machine: the lock never expires, so at
every return the window may take the foreground only through another rule of `SetForegroundWindow`, such as having
received the last input. In the fake sets the fake taps Shift into its own foreground window before closing
(`--fake-input`; 90 of 93 launches), so the last input belongs to the emulator, as when a player presses a key; for
RetroArch no input was injected, and in all 30 launches the idle time at the exit exceeded the spike's own uptime: the
last input predates the spike's process, which never received it.

**RetroArch, isolated.** RetroArch 1.21.0 (`E:\Programmi\Retroarch`, d3d11, borderless fullscreen, `pause_nonactive`),
gambatte core, *Mole Mania (USA, Europe) (SGB Enhanced).gb* read from the user's library:

```text
retroarch.exe -L E:\Programmi\Retroarch\cores\gambatte_libretro.dll --appendconfig=<scratch>\retroverse-isolated.cfg
  --max-frames=240 --sram-mode=noload-nosave --log-file=<scratch>\logs\retroarch-<n>.log --verbose
  "I:\Giochi\Emulatori\gb\1 GB US - H-Q\Mole Mania (USA, Europe) (SGB Enhanced).gb"
```

The appended configuration (`spike_launch_core`'s `retroArchAppendConfig`) turns off everything RetroArch saves on its
own (configuration and remaps on exit, history, runtime logs, the core info cache, SRAM autosave, automatic save states)
and points every directory or file it could write at the scratch directory under `build/` (saves, states,
screenshots, logs, the configuration directory with core options and overrides, playlists, history, cache). One of
them was not honored: RetroArch loaded its favorites playlist from its installation folder
(`E:\Programmi\Retroarch\content_favorites.lpl`, per its log), so adding a favorite during such a run would write
there; the automated runs only read it. Snapshots of RetroArch's folder (48,702 files: size, modification time,
SHA-256 of configurations, saves, playlists and logs), of the ROM's folder and of ES-DE's settings and `gb` gamelist,
taken before the first and after the 30th launch, show **0 differences** (and none against the snapshot that closed the
first campaign). RetroArch wrote only into the scratch directory: its logs and `config\Gambatte\Gambatte.opt` (it saves
the core options on exit, which would otherwise have gone into its own folder).

**DuckStation was not launched.** Its settings, memory cards, caches and logs live in a user data folder that only a
`portable.txt` in its installation folder would move, which would be a write into the user's emulator installation,
and it checks for updates at startup. Exclusive fullscreen with a display mode change is covered by the fake
emulator's `--exclusive` mode instead (1176x664 at 60 Hz on the 2560x1440 display).

**Crashes and loader errors.** The fake's `--crash` turns Windows' error dialogs and error reporting off for itself
(`SEM_NOGPFAULTERRORBOX`) before its access violation, so the crash return below was measured without Windows Error
Reporting. Real emulators run with the default error mode (`CREATE_DEFAULT_ERROR_MODE`): a crash goes through Windows
Error Reporting, and on this machine, where Visual Studio is registered as the just-in-time debugger (`AeDebug`, no
`Auto`), a crash would show its modal prompt and hold the crashed process, and the session with it, until the user
answers; a missing DLL shows the loader's modal "The code execution cannot proceed" box with the same effect. Neither
was run here (both need a person to dismiss them); loader errors are covered by the unit tests' classification only.

## Results

### RetroArch, keep (window left in place, not rendering)

Build `bba5105`. CPU load during the sessions: median 58% (14-100%); GPU 2% (0-14%). No foreground remedy at any
return (column left out).

| Session | Launch | Outcome | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest | Other app at launch / return | CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | Returned | 15.0 | -171.4 | 15.0 | 15.1 | 346.2 | 15.1 | 346.2 | 0 / 0 | 66% |
| 1 | 2 | Returned | 17.0 | -235.3 | 17.0 | 17.4 | 74.1 | 17.4 | 74.1 | 0 / 0 | 97% |
| 1 | 3 | Returned | 18.8 | -198.5 | 18.8 | 19.1 | 75.8 | 19.2 | 75.8 | 0 / 0 | 95% |
| 1 | 4 | Returned | 14.1 | -181.1 | 14.1 | 14.5 | 54.3 | 14.5 | 54.3 | 0 / 0 | 30% |
| 1 | 5 | Returned | 15.0 | -181.9 | 15.0 | 15.4 | 58.0 | 15.4 | 58.0 | 0 / 0 | 28% |
| 2 | 1 | Returned | 16.6 | -171.2 | 16.6 | 17.0 | 337.5 | 17.0 | 337.5 | 0 / 0 | 47% |
| 2 | 2 | Returned | 14.7 | -170.7 | 14.7 | 15.1 | 59.0 | 15.1 | 59.0 | 0 / 0 | 48% |
| 2 | 3 | Returned | 23.5 | -353.6 | 23.5 | 23.6 | 75.9 | 23.6 | 75.9 | 0 / 0 | 85% |
| 2 | 4 | Returned | 19.6 | -179.6 | 19.6 | 19.7 | 74.3 | 19.7 | 74.3 | 0 / 0 | 85% |
| 2 | 5 | Returned | 19.0 | -186.3 | 19.0 | 19.6 | 68.0 | 19.6 | 68.0 | 0 / 0 | 46% |
| 3 | 1 | Returned | 15.4 | -170.4 | 15.4 | 15.5 | 338.4 | 15.5 | 338.4 | 0 / 0 | 67% |
| 3 | 2 | Returned | 21.1 | -184.2 | 21.1 | 21.5 | 79.3 | 21.6 | 79.3 | 0 / 0 | 94% |
| 3 | 3 | Returned | 19.0 | -172.9 | 19.0 | 19.4 | 71.2 | 19.4 | 71.2 | 0 / 0 | 43% |
| 3 | 4 | Returned | 15.3 | -183.2 | 15.3 | 15.7 | 65.6 | 15.7 | 65.6 | 0 / 0 | 55% |
| 3 | 5 | Returned | 13.7 | -178.3 | 13.7 | 14.1 | 64.2 | 14.2 | 64.2 | 0 / 0 | 38% |

| | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest |
|---|---|---|---|---|---|---|---|
| Median | 16.6 | -181.1 | 16.6 | 17.0 | 74.1 | 17.0 | 74.1 |
| Best | 13.7 | -353.6 | 13.7 | 14.1 | 54.3 | 14.2 | 54.3 |
| Worst | 23.5 | -170.4 | 23.5 | 23.6 | 346.2 | 23.6 | 346.2 |

Session medians of the latest time: 74.1, 74.3, 71.2. No other application took the foreground at any time during
these 15 launches (hook and polling).

### RetroArch, minimize

Build `bba5105`. CPU load: median 38% (15-100%); GPU 6% (1-50%). No foreground remedy at any return.

| Session | Launch | Outcome | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest | Other app at launch / return | CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | Returned | 26.2 | 26.2 | 26.2 | 26.8 | 344.0 | 26.8 | 344.0 | 104 / 231 | 40% |
| 1 | 2 | Returned | 38.7 | 38.7 | 38.7 | 39.0 | 83.6 | 39.0 | 83.6 | 86 / 391 | 58% |
| 1 | 3 | Returned | 29.7 | 29.7 | 29.7 | 30.1 | 74.1 | 30.1 | 74.1 | 313 / 230 | 44% |
| 1 | 4 | Returned | 29.6 | 29.6 | 29.6 | 30.1 | 74.9 | 30.1 | 74.9 | 97 / 230 | 45% |
| 1 | 5 | Returned | 24.8 | 24.8 | 24.8 | 25.5 | 70.5 | 25.5 | 70.5 | 96 / 222 | 42% |
| 2 | 1 | Returned | 27.3 | 27.3 | 27.3 | 27.7 | 318.2 | 27.7 | 318.2 | 110 / 220 | 44% |
| 2 | 2 | Returned | 31.6 | 31.6 | 31.6 | 32.0 | 79.5 | 32.0 | 79.5 | 108 / 232 | 49% |
| 2 | 3 | Returned | 26.7 | 26.7 | 26.7 | 27.7 | 74.8 | 27.8 | 74.8 | 97 / 246 | 57% |
| 2 | 4 | Returned | 29.5 | 29.5 | 29.5 | 29.9 | 72.9 | 29.9 | 72.9 | 204 / 241 | 44% |
| 2 | 5 | Returned | 29.9 | 29.9 | 29.9 | 30.8 | 78.5 | 30.9 | 78.5 | 99 / 232 | 37% |
| 3 | 1 | Returned | 27.1 | 27.1 | 27.1 | 27.5 | 367.8 | 27.5 | 367.8 | 96 / 239 | 35% |
| 3 | 2 | Returned | 28.4 | 28.4 | 28.4 | 28.6 | 74.9 | 28.6 | 74.9 | 106 / 228 | 38% |
| 3 | 3 | Returned | 23.7 | 23.7 | 23.7 | 24.2 | 71.7 | 24.3 | 71.7 | 137 / 219 | 39% |
| 3 | 4 | Returned | 25.4 | 25.4 | 25.4 | 26.2 | 76.3 | 26.3 | 76.3 | 109 / 224 | 46% |
| 3 | 5 | Returned | 25.3 | 25.3 | 25.3 | 26.1 | 72.0 | 26.1 | 72.0 | 116 / 226 | 41% |

| | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | Latest |
|---|---|---|---|---|---|---|---|
| Median | 27.3 | 27.3 | 27.3 | 27.7 | 74.9 | 27.8 | 74.9 |
| Best | 23.7 | 23.7 | 23.7 | 24.2 | 70.5 | 24.3 | 70.5 |
| Worst | 38.7 | 38.7 | 38.7 | 39.0 | 367.8 | 39.0 | 367.8 |

Session medians of the latest time: 74.9, 78.5, 74.9. "Other app": the user's browser, the only other application
seen, which Windows activated once the window was minimized (29-87 ms after the launch) and again when RetroArch closed
its window (193-352 ms before its process ended): in the foreground for a median 106 ms (86-313) at the launch, the
longer ones while RetroArch's window appeared late, and 230 ms (219-391) at the return.

### Fake emulator (Release, 3 sessions)

| Set | Build | Launches met | Latest: median / best / worst | Session medians (latest) | 60 FPS median | Input median | Foreground median | Other app at launch / return (median) | Load: CPU / GPU median |
|---|---|---|---|---|---|---|---|---|---|
| Keep (5 per session) | `bba5105` | 15 of 15 | 174.3 / 63.8 / 390.1 | 146.8, 114.6, 256.6 | 174.3 | 12.8 | -102.8 | 0 / 0 | 100% / 5% |
| Keep, measured again (5) | `bba5105` | 15 of 15 | 70.2 / 59.8 / 349.1 | 67.7, 113.7, 69.9 | 70.2 | 12.8 | -13.7 | 0 / 0 | 60% / 7% |
| Minimize (5) | `bba5105` | 15 of 15 | 81.8 / 62.5 / 435.3 | 84.5, 72.7, 89.7 | 81.8 | 29.8 | 28.5 | 191 / 74 (15 of 15) | 80% / 8% |
| ProcessTree through a wrapper, keep, 600 frames (3) | `a60ee97` | 9 of 9 | 77.1 / 68.0 / 333.1 | 109.1, 73.8, 77.1 | 77.1 | 14.2 | -13.5 | 0 / 0 | 47% / 26% |
| Exclusive display mode, keep (3) | `bba5105` | 9 of 9 | 390.5 / 310.2 / 709.8 | 484.6, 373.2, 390.5 | 390.5 | 242.1 | -141.8 | 0 / 0 | 100% / 9% |
| Exclusive display mode, minimize (3) | `bba5105` | 9 of 9 | 437.9 / 349.5 / 686.0 | 413.4, 443.0, 375.2 | 437.9 | 313.2 | 179.8 | 1855 / 0 (191 in 1 of 9) | 100% / 6% |
| Another window in front at the exit, keep (3) | `a60ee97` | 9 of 9 | 150.2 / 102.7 / 428.7 | 150.2, 103.0, 178.4 | 150.2 | 49.9 | 49.1 | see Foreground | 49% / 37% |
| The same, foreground locked (3) | `a60ee97` | 9 of 9 | 111.4 / 101.5 / 442.4 | 111.4, 114.7, 110.0 | 111.4 | 52.9 | 51.1 | see Foreground | 51% / 47% |
| The same, window hung (1 session of 3) | `a60ee97` | 0 of 3 (by design) | 3101.4 / 3085.0 / 3142.2 | 3101.4 | 16.8 | 3101.4 | 3101.3 | 0 / 3058 (3 of 3) | 42% / 40% |

The first keep set ran while the CPU was saturated by other builds (median 100%): the frame pacing after the returns
suffered, so the set was measured again at a lower load, and both are given. The re-run's reports name the build
`bba5105 with uncommitted changes`: the change was `summarize_reports.py`; the executable was the same (built
2026-10-16T02:10:57Z, as recorded in every `bba5105` report).

The fake runs 1.3-5.3 s (150 frames at 164.84 Hz after its start-up, which the load stretched): with the startup grace
counted from the moment it runs, its clean exit came within the 2 s grace in 8, 11 and 15 of the 15 DirectProcess
launches of the keep, keep again and minimize sets, which the session reports as a quick exit (`quickExit`, not a
failure). The process tree set lets the child run 600 frames (the tree lived 4.1-4.3 s): no quick exit, as with a real
game; 2 processes in every tree (the console host of the fake's console, also in the job, is not reported).

The "another window" sets leave a small window of another application in the foreground when the fake exits
(`--fake-foreground-window 5000`: it holds the foreground for 5 s; `--fake-lock-foreground`: it also calls
`LockSetForegroundWindow`; `--fake-hung-foreground`: it stops reading its messages, and the fake exits once Windows
considers it hung). That application is a copy of the fake, which `summarize_reports.py` tells apart from the target by
image name only: its time in front at the return is the Foreground column; with the hung window, the window that took
the foreground 25-117 ms after the exit belonged to a process the spike could not open, and is counted. Remedies at
the returns:

- another window, plain or locked: the plain raise refused in 18 of 18 launches, AttachThreadInput, tried at once,
  accepted in 18 of 18; the window in the foreground 44-121 ms after the exit. With the build before (`bba5105`), which
  tried AttachThreadInput on the next frame, after the first frame's stall, it came back 154-633 ms after the exit.
- a hung window: the plain raise and AttachThreadInput refused (the attachment is skipped for a hung window's thread:
  attaching would stall Retroverse's input), the flash shown, 3 of 3. The foreground went to a window of a process the
  spike could not open (presumably the system's replacement of the hung window), and Retroverse's window came back
  only when the hung window went away, 3.1 s after the exit. In development runs an injected Alt key (2 launches) and,
  in its place, a zero-motion mouse input (2 launches) did not get the foreground either.

### Scenarios (ctest `desktop`, Debug)

`./scripts/test.ps1 -Preset msvc-x64-debug-desktop`: 10 of 10 passed (104 s), with each of the two builds.

| Case | What it shows |
|---|---|
| `fake_direct_minimize`, `fake_direct_keep` | the plain return with each policy (3 launches each) |
| `fake_tree_minimize`, `fake_tree_keep` | a wrapper that exits at once: the session waits for its child (ProcessTree), 2 processes seen, ends with the tree |
| `fake_confirm_keep` | UserConfirmReturn: the launcher's exit leads to AwaitingConfirmation, the injected pad press confirms |
| `fake_crash_minimize` | an access violation (error dialogs off in the fake): `Crashed`, `0xC0000005 STATUS_ACCESS_VIOLATION` |
| `fake_exclusive_minimize`, `fake_exclusive_keep` | a display mode change (1176x664 at 60 Hz): fullscreen re-applied, the window moved back to its monitor |
| `fake_steal_focus_keep` | an emulator that forces itself into the foreground: no effect on the return |
| `fake_other_window_keep` | another application's window in the foreground at the exit: AttachThreadInput brings the window back |

The runner's own tests (`tests/platform`, label `unit`, hidden processes): exit codes including 0xFFFFFFFF, an unclean
exit within the startup grace (FailedToStart), a crash, ProcessTree through a wrapper (the child's exit code reported,
not the wrapper's or its console host's), DirectProcess with a wrapper (returns at once, quick exit, the child left
running), a hang that becomes StillRunning and survives the session and the runner (ended by the test harness, also if
the test fails), arguments with spaces, quotes, trailing backslashes, empty strings, `%`, `^`, apostrophes and Unicode
received verbatim by the child, environment overrides, the working directory, captured output, non-programs refused, a
broken image failing without a dialog, a child breaking away from the job (with the console it shares), an exception
from `beforeResume` (the never-run process ended, a failure to start), the start time taken after the hand-over, a
finished session no longer woken by what it left running, a crash after 300 other processes of the tree, and a console
program's own console.

### Notepad

Measured once with the first build (`b053022`); the changes since do not touch its path (a GUI program,
UserConfirmReturn). On this Windows build `C:\Windows\System32\notepad.exe` starts the packaged `Notepad.exe` as a
child inside the job and waits for it. With UserConfirmReturn and keep, the session reached AwaitingConfirmation when
the stub exited (after the harness closed Notepad's window) and the pad press confirmed it 100 ms later; ProcessTree or
DirectProcess would also have worked. A launcher whose program runs outside the job (a packaged application activated
by the system) needs UserConfirmReturn.

## Findings

1. **SDL 3.4.18 ignores `SDL_RestoreWindow` for fullscreen windows** (`WIN_RestoreWindow` does nothing when
   `SDL_WINDOW_FULLSCREEN` is set). The hand-over restores a minimized window with `ShowWindow(SW_RESTORE)`, which also
   brought it to the foreground at once in every minimize launch (39 of 39: 15 RetroArch, 15 fake and 9 exclusive
   launches), the foreground lock notwithstanding; SDL then re-enters fullscreen on the restored event.
2. **Hand the window over after `CreateProcessW`, before the emulator runs.** Minimizing first makes Windows activate
   another window, so Retroverse is no longer the foreground process when the emulator starts. The runner calls
   `StartCallbacks::beforeResume` between creating the suspended process and `ResumeThread`; the session's start time
   (`SessionTimes::started`, from which the startup grace counts) is the resume, after the hand-over (5-30 ms after the
   launch began when keeping the window, medians 6-10 ms per set; 28-122 ms when minimizing it).
3. **`AllowSetForegroundWindow` on the suspended child fails for console-subsystem programs**
   (`ERROR_INVALID_PARAMETER`: the process has no window-manager state yet; every fake launch) and succeeds for
   GUI-subsystem ones (30 of 30 RetroArch launches). Every emulator took the foreground anyway: it was started by the
   foreground process.
4. **Keep beats minimize.** With the window kept, Windows activates it as soon as the emulator's window closes, the next
   window in the z-order (170-354 ms before RetroArch's process ends): no remedy, and no other application shown in
   any of the 15 RetroArch launches of this campaign (in the first campaign, a terminal that took the foreground during
   one launch showed again at its return). Minimized, the next window is whatever the user had behind (the browser in
   every launch), visible for about 100 ms at the launch (86-313 ms) and 220-390 ms at the return with RetroArch, which
   closes its window about 200 ms before its process ends (56-179 ms with the fake).
5. **Remedies.** None was needed after an emulator's exit unless another application's window held the foreground.
   Then the plain raise was always refused, and AttachThreadInput was accepted at every return (18 of 18, also when that
   application had called `LockSetForegroundWindow`); trying it at once rather than on the next frame matters, since
   the first frame after a return stalls. It was refused for a freshly started window against a locked foreground (the
   warm-up of the 3 sessions that followed a locked-foreground session, until that window's 5 s ran out) and for a
   hung foreground window (3 of 3; attaching to it is skipped). The Alt key trick was dropped (ADR-0006): it did not
   help where AttachThreadInput could not, and when the raise still fails the key reaches the foreground application.
   The flash was shown in the hung case; the user's action, or the other window going away, brings Retroverse back.
6. **Detection.** The system's exit time precedes the moment the runner observes the exit by 13-23 ms with RetroArch
   (process teardown; 3-33 ms across every set). The watcher wakes the main loop through `SDL_PushEvent` in 0.07-0.17 ms
   (median per set; at most 3.1 ms), except after an exclusive display mode, where the main thread is busy with the
   mode change: median 7.8 and 8.0 ms, up to 58 ms.
7. **Frame pacing after a return.** The first return of a session stalls twice (about 100 ms, then 147-325 ms;
   sometimes a third of 31-55 ms; 60 FPS back after 307-442 ms, 485-710 ms after an exclusive mode); later returns
   stall once, sometimes twice (31-118 ms, usually 40-60 ms; 60 FPS back after 54-258 ms, 310-539 ms after an exclusive
   mode; the hung-window launches aside). The swap chain (independent flip versus composed presentation) is the likely
   cause; not investigated further (not yet in BACKLOG.md: the integration adds the entry, see ADR-0006).
8. **Exclusive display modes.** The mode change shrinks a kept window (fullscreen re-applied, 9 of 9; back 101-298 ms
   after the exit) and moves a minimized one to the other monitor (8 of 9; moved back to its display). Worst case
   710 ms.
9. **`CreateProcessW` can show a modal box in the calling process.** For a DOS or 16-bit image ("Unsupported 16-Bit
   Application"), with the default error mode (as ctest gives its children), the call blocks on a message box until
   dismissed. Validation refuses images that are not x64, x86 or ARM64 PE programs, and the runner sets
   `SEM_FAILCRITICALERRORS` on its thread around `CreateProcessW` (tested under ctest).
10. **Batch files are refused**: `CreateProcessW` runs them through `cmd.exe`, which re-parses arguments (`%`, `^`, `&`,
    `|`; the "BatBadBut" class of injection). A `.bat` wrapper would need a verified profile with cmd-specific
    escaping.
11. **Children that break away from the job are not followed** (`JOB_OBJECT_LIMIT_BREAKAWAY_OK` lets them): the tree
    ends without them (tested).
12. **Console programs bring their console host into the job.** A console program started with a console of its own
    (`CREATE_NEW_CONSOLE`, or `CREATE_NO_WINDOW` when hidden, which the first version already used for
    `hideConsoleWindow`) starts `conhost.exe` as its child, inside the job. Reported as a process of the tree, it made
    the console host's exit code (0) the session's in a ProcessTree test, and its lifetime follows any program attached
    to the console, a child that broke away from the job included. The runner follows console hosts without reporting
    them, and a job holding only console hosts counts as an empty tree. A GUI program gets no console host with
    `CREATE_NEW_CONSOLE` (checked with Character Map).
13. **Job notifications are not guaranteed.** In one of nine ProcessTree launches (build `bba5105`) the job's
    `ACTIVE_PROCESS_ZERO` came late or not at all after the console host's exit, and the session noticed the tree's
    end 208 ms after the last exit (11-34 ms in the other eight; 11-15 ms in all nine with build `a60ee97`). The
    watcher now counts the job's processes at every wake-up; when the count decides, the pending notifications are
    drained and the exits of tracked processes still without one are read from their handles, so no exit code is lost
    (a race seen once in testing before).

## Deviations from the task

- **A clean exit within the startup grace is not a failure to start.** The task reads "an exit within the first seconds
  = failed to start". The session fails only unclean exits within the grace (and loader errors, at any time); a clean
  one is `Returned` with `quickExit` set, unless the whole tree was waited for (ProcessTree). Under DirectProcess a
  launcher stub that started the real emulator exits 0 at once while the game is starting: calling that a failure would
  bring Retroverse back over a starting game and tell the user it did not start. The product must surface `quickExit`
  instead (ADR-0006): "the game ended right away; if it is still running, its profile needs ProcessTree".
- **The Alt key remedy is not adopted** (finding 5): the remedies are the plain raise, AttachThreadInput and the flash.
- **DuckStation was not launched** (its data folder cannot be isolated without writing into its installation), and
  crashes and loader errors were measured without Windows' error dialogs (see Method).

## Reproduce

```powershell
./scripts/build.ps1 -Preset msvc-x64-release
./spikes/launch/measure.ps1 -Name fake-keep -SpikeArgs '--target','fake','--policy','keep','--repeat','5','--fake-input'
./spikes/launch/measure.ps1 -Name tree-keep -SpikeArgs '--target','fake','--strategy','tree','--policy','keep',
    '--repeat','3','--fake-input','--frames','600'
./spikes/launch/measure.ps1 -Name other-window -SpikeArgs '--target','fake','--policy','keep','--repeat','3',
    '--fake-input','--fake-foreground-window','5000'        # also --fake-lock-foreground, --fake-hung-foreground
./spikes/launch/measure.ps1 -Name retroarch-keep -SpikeArgs '--target','retroarch','--retroarch','<retroarch.exe>',
    '--core','<gambatte_libretro.dll>','--rom','<game.gb>','--max-frames','240','--policy','keep','--repeat','5',
    '--timeout-s','90'
python -I spikes/launch/summarize_reports.py build/spike-launch/measurements/retroarch-keep-1.json ...
python -I spikes/launch/snapshot_tree.py snapshot before.json <RetroArch folder> <ROM folder>   # and compare
./scripts/test.ps1 -Preset msvc-x64-debug-desktop
```
