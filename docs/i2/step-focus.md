# I2 step focus: automated tests never take the Windows foreground

A step of [I2](../roadmap.md) outside the numbered plan: running the tests (`./scripts/test.ps1`, the gpu presets,
`scripts/gate.ps1`, the agents' runs of `rv_tests.exe`) made the machine impossible to use, because programs the tests
start took the Windows foreground while the user worked. Now every automated run (labels `unit` and `gpu`, everything
the gate, `scripts/test.ps1` with its default and `-gpu` presets and `scripts/real-library.ps1` run) starts its programs
in the background: no console window, no window activated or shown on a monitor, no foreground granted, no input
injected, no loader or crash dialog. Only the tests labelled `desktop` (on demand, never the gate) still take over the
screen. `scripts/focus_watch.py` checks it.

## What was found

A first read listed suspects; `focus_watch.py` (below) confirmed one culprit before the fix, on one test case of
`rv_tests` with the runner of `main` (`cli/launch: hostile and long-path arguments reach the emulator byte-identical`,
3.5 s): **2 foreground changes to test processes and 2 visible windows of test processes**, one per program launched.

- **The culprit: console windows of launched programs.** `Win32ProcessRunner` starts a console program with
  `CREATE_NEW_CONSOLE` unless the request sets `hideConsoleWindow` (ADR-0006: a real console emulator keeps its
  console). `rv_fake_emulator.exe` is a console program. Step 12's CLI tests (`test_cli_sessions.cpp`,
  `test_cli_reconciled_sessions.cpp`: `launch --game` through emulator profiles that did not hide the console) and
  every ES-DE command of the standard fixture without `%HIDEWINDOW%` (`home-emulator.exe`, the fixture's copy of the
  fake) opened a console for each launch. On this machine the default terminal application is Windows Terminal
  (`HKCU\Console\%%Startup\DelegationConsole`): each console became a new **Windows Terminal window, activated**, owned
  by `WindowsTerminal.exe` (started by the system, outside the test's process tree) and titled "Windows Terminal". The
  focus went back to the user's browser a few hundred ms later, then was taken again by the next launch: dozens of
  times per unit run.
- **Not culprits today, closed anyway** (defense in depth):
  - `rv_fake_emulator`'s window modes (`--frames` fullscreen, `--exclusive`, `--steal-focus` with
    `SDL_HINT_FORCE_RAISEWINDOW` and `SDL_RaiseWindow`, `--input-at-end` with `SendInput`, `--leave-foreground-window`
    and `--foreground-window` with `LockSetForegroundWindow`) are used only by the `desktop` tests (spike_launch's
    `desktop.launch.*` cases and `desktop.smoke.launch_visible` through the mock catalog's `fake-window` profile).
  - `AllowSetForegroundWindow` in the runner only grants a permission; `platform::WindowHandover`'s remedies
    (`AttachThreadInput`, `SetForegroundWindow`, `SW_RESTORE`) run only for a visible window (A1 skips the hand-over
    when `--hidden`); every smoke and spike run of the gpu label is `--hidden`; `rv_platform_tests` uses one hidden SDL
    window and synthetic SDL events; `spike_ui` drives an SDL virtual gamepad (SDL-internal, no OS input).
  - `tests/support/child_process.hpp` (`runChild`) and the smoke tests' `StartedApp` already use `CREATE_NO_WINDOW`;
    frame-limited runs of `retroverse.exe` never show message boxes (`--no-dialogs` implied).
  - Error dialogs: the runner gave programs `CREATE_DEFAULT_ERROR_MODE`, so a program missing a DLL (a fixture's copy of
    the fake without its DLLs on PATH) would show the loader's modal "system error" box; the fake sets its own mode
    before `--crash`. Nothing configured the C runtime's own message boxes, which the error mode does not cover: in
    Debug builds a failed `_ASSERTE`, a debug iterator or invalid parameter report, `assert()` in a GUI program and
    `abort()` show a task-modal `MB_SETFOREGROUND` box ("Debug Assertion Failed", "abort() has been called") in
    `rv_tests`, `rv_smoke_tests`, the programs they run and the fake, which takes the foreground and blocks the test
    until its ctest timeout (found by review, fixed below).

## What was built

| File | What |
|---|---|
| `src/platform_win/include/rv/platform_win/win32_process_runner.hpp`, `src/win32_process_runner.cpp` | Background launches: `kLaunchBackgroundVariable` (`RV_LAUNCH_BACKGROUND`), `backgroundLaunchesRequested()`, `Win32ProcessRunnerOptions::background` (nullopt: the variable), `Win32StartInfo::background`, `processCreationFlags(request, background)` |
| `src/platform_win/...` (the same files) | `disableFailureDialogs()`: no loader, crash or C runtime dialog in this process |
| `tests/support/background_launches.hpp` | `rv::test::ensureBackgroundLaunches()`: sets `RV_LAUNCH_BACKGROUND=1` unless it is set; `prepareAutomatedRun()`: that and `disableFailureDialogs()` |
| `tests/unit/support/background_launches.cpp` | rv_tests prepares its run before `main()` (a static initializer) |
| `tests/smoke/main.cpp` | rv_smoke_tests prepares its run first thing |
| `tests/fake_emulator/fake_emulator.cpp`, `CMakeLists.txt` | The fake's background mode; `--print-console` also writes `hasWindow`; no failure dialogs, ever; `--abort`; links `rv_platform_win` |
| `src/app/src/app_host.cpp`, `tools/cli/src/main.cpp` (and its `CMakeLists.txt`), `spikes/launch/src/main.cpp` | `disableFailureDialogs()` in unattended runs |
| `CMakePresets.json` | `RV_LAUNCH_BACKGROUND` `1` in the four unit and gpu test presets, `0` in `msvc-x64-debug-desktop` |
| `tests/smoke/CMakeLists.txt`, `spikes/launch/CMakeLists.txt` | The desktop registrations set `RV_LAUNCH_BACKGROUND=0` |
| `scripts/real-library.ps1` | Sets `RV_LAUNCH_BACKGROUND=1` for rv_tests with the `RV_REAL_*` variables |
| `tests/unit/cli/test_cli_sessions.cpp`, `test_cli_reconciled_sessions.cpp` | Their fake profiles hide the console (`hideConsoleWindow: true`) |
| `tests/unit/platform_win/test_process_runner.cpp` | Tests of the background mode (`[background]`) |
| `scripts/focus_watch.py` | The checker |

## RV_LAUNCH_BACKGROUND

Set, and neither empty nor `0`, it makes `Win32ProcessRunner` start every program in the background (the product, a
user's run, never sets it; ADR-0006's behaviour is unchanged without it):

- console programs get `CREATE_NO_WINDOW` instead of `CREATE_NEW_CONSOLE`, whatever `hideConsoleWindow` says: a
  console of their own (still never Retroverse's), without a window, so no terminal window opens;
- GUI programs get `STARTF_USESHOWWINDOW` with `SW_SHOWNOACTIVATE` and `STARTF_USEPOSITION` 8000 px left of the
  virtual screen (`SM_XVIRTUALSCREEN - 8000`, `SM_YVIRTUALSCREEN`): a program that lets Windows place its first window
  (`CW_USEDEFAULT`) and show it (its first `ShowWindow`, whatever it passes) opens it inactive and off every monitor,
  like the fake's background window: still a visible window for the runner's window detection, no taskbar button of a
  minimized window. Checked with a scratch Win32 program (`CW_USEDEFAULT`, then `ShowWindow` with the `WinMain`
  value, `SW_SHOW`, `SW_SHOWNORMAL` and `SW_SHOWDEFAULT`): each window opened at (-8000, 0), visible, not minimized,
  not in the foreground. A program that places or activates its window itself escapes this, and `focus_watch.py`
  reports it: such a test is a desktop test;
- no `AllowSetForegroundWindow` (`Win32StartInfo::foregroundError` says why, the log line reads `in the background:
  RV_LAUNCH_BACKGROUND`);
- no `CREATE_DEFAULT_ERROR_MODE`: the runner calls `disableFailureDialogs()` (below) once, and the program inherits its
  error mode, so a missing DLL ends it with `STATUS_DLL_NOT_FOUND` (0xC0000135) and a crash with its code, without a
  dialog.

`Win32ProcessRunnerOptions::background` forces it either way (the tests use `true`); nullopt, the default, reads the
variable at each `start()`. The CLI and the app construct the runner with the default options, so the variable reaches
them through their environment: in-process CLI tests, `retroverse-cli` and `retroverse.exe` run as processes by tests.

Where it is set:

- the test presets `msvc-x64-debug`, `msvc-x64-debug-gpu`, `msvc-x64-release`, `msvc-x64-release-gpu` (`environment`),
  which `scripts/test.ps1` and `scripts/gate.ps1` use; `msvc-x64-debug-desktop` sets `0`;
- `rv_tests` and `rv_smoke_tests` themselves, before any test, unless it is set already
  (`rv::test::ensureBackgroundLaunches`): a run by hand or by an agent (`rv_tests.exe "cli/launch*"`) is covered too;
  a test (`platform_win/process_runner: background launches never open a console window`) fails when it is not set in
  rv_tests;
- `scripts/real-library.ps1`, for rv_tests with the `RV_REAL_*` variables;
- the desktop registrations (`desktop.smoke.launch_visible`, `desktop.launch.*`) set `0`, so that a desktop run started
  from an environment that has it still takes over the screen as it must.

### No failure dialogs

`rv::platform_win::disableFailureDialogs()` turns off, for the rest of the process's life, every modal box a failure
would show: the error mode gets `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX` (the loader's
and the crash dialogs; inherited by the programs the runner starts in the background); `_CrtSetReportMode` sends
`_CRT_ASSERT` and `_CRT_ERROR` reports to standard error and the debugger (`_CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG`,
Debug builds); `_set_error_mode(_OUT_TO_STDERR)` does the same for `assert()` and runtime error messages of GUI
programs; `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` makes `abort()` end the process with exit code
3, without its box or a fault report. Called by:

- `rv_tests` and `rv_smoke_tests` before any test, whatever `RV_LAUNCH_BACKGROUND` says (`rv::test::prepareAutomatedRun`:
  a test executable never wants a modal box);
- `rv_fake_emulator`, first thing in `main`, always;
- `retroverse.exe`, `retroverse-asset-preview`, `spike_ui` and `spike_video` (`app::AppHost`) in frame-limited or
  `--no-dialogs` runs and when `RV_LAUNCH_BACKGROUND` is set; `retroverse-cli` and `spike_launch` when it is set;
- the runner, before its first background launch.

An interactive run of the product keeps the C runtime's defaults (a developer debugging an assertion wants its box).
`rv_platform_tests` and `spike_launch_tests` start no program and create no visible window; they are left with the
defaults (an assertion failing in them would still show its box: BACKLOG candidate, below).

### The fake emulator in the background

`rv_fake_emulator` reads the same variable. With it:

- `--frames` shows a 320x180 tool window (`SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_UTILITY`: no taskbar button), placed
  2000 px left of the virtual screen at the bottom of the z-order and shown with `SW_SHOWNOACTIVATE`; no fullscreen, no
  exclusive mode (a warning in its log). It is still a visible top-level window with a non-empty rectangle, so the
  runner's window detection (`WindowShown`, the window timeout) sees it;
- `--steal-focus`, `--input-at-end` (no `SendInput`), `--leave-foreground-window` and `--foreground-window` are skipped;
  its `--log` records `background: true` and `skipped` for each;
- the windowless modes are unchanged.

Whatever the variable says, it never shows a failure dialog (`disableFailureDialogs()`), and `--abort` ends it through a
failed `_ASSERTE` (Debug builds) and `abort()`: both are reported on standard error and it exits with code 3.

## focus_watch.py

`python -I scripts/focus_watch.py --watch <dir> [--watch <dir>...] [--out <report.json>] [--duration <s>] [--tail <s>]
[--interval-ms 20] [-- <command...>]` (Python 3.10+, standard library and ctypes; read-only: it never sends input, never
activates, moves or closes a window and never ends a process). It runs the command (inheriting its console) or watches
for `--duration` seconds. It follows the foreground with an out-of-context `EVENT_SYSTEM_FOREGROUND` hook (so that a
foreground change shorter than a poll is recorded too; the report's `foregroundHook` says whether it was set) and a
`GetForegroundWindow` poll every 20 ms, polls `EnumWindows`, and records each foreground change and each top-level
window that becomes visible: time, window handle, title, class, rectangle, minimized, cloaked, on a monitor,
`WS_EX_NOACTIVATE`/`WS_EX_TOOLWINDOW`, the owning process id, its image path and its parent chain. A window first seen
off every monitor, cloaked or empty is looked at again on every poll while it stays visible; when it moves onto a
monitor, is uncloaked or grows, its record gets `seenAt`, the new state and its `firstState`, and it counts from then.

Process ids are reused, and a process snapshot every 250 ms keeps processes that have exited, so the history keeps
every process seen per id with its creation time: a window's process is the entry whose creation time matches the live
process's (`GetProcessTimes`; a fresh snapshot, or a new entry, when none does), and each ancestor in the chain is the
process of that id that existed when its child was created. (Before this, a window of the fake was once attributed to
a `conhost.exe` that had had its id, and missed.)

A window belongs to a **test process** when its process's image lies under a `--watch` folder, or an ancestor's does,
or its title names a watched folder, or it is a terminal window (`CASCADIA_HOSTING_WINDOW_CLASS`,
`ConsoleWindowClass`) that owns a test process's `PseudoConsoleWindow` (the window of a console that a default terminal
application shows) or appears within 3 s of one. System error dialogs are shown by processes outside the test's tree,
so they get rules of their own: a window of `WerFault.exe` (Windows Error Reporting) is the run's when its command line
(`NtQueryInformationProcess(ProcessCommandLineInformation)`) names with `-p` a test process, a process never seen, or
cannot be read; a dialog (`#32770`) of `csrss.exe` (the loader's hard-error box) cannot be attributed and is always the
run's. A failing program of the user's during the run can therefore fail it; the report's `reason` says which rule
matched.

Findings (exit 1): a foreground change to a test process's window; a visible window of a test process that the user
would see at any time while it was visible (on a monitor, or minimized: it sits in the taskbar; not cloaked, not empty).
Recorded as allowed: test windows that stayed off every monitor, cloaked or of zero size (hidden `PseudoConsoleWindow`s,
the fake's off-screen window). A window shown and hidden again within one poll (20 ms) can be missed. Exit codes: 0
clean, 1 findings, 2 usage or the command could not start, 3 clean but the command failed. The report holds `counts`,
`findings`, `allowed`, and every foreground change and window seen.

Checked against scratch programs: a window shown at (-30000, -30000) with `SW_SHOWNOACTIVATE` and moved to (40, 40)
0.3 s later is a finding (`seenAt` 0.415 s, `firstState` off-screen; the watcher before this reported it as allowed and
exited 0); a cached entry of another image under a live process's id, and one of the same name and parent with another
creation time, both give way to the live process (its image and chain).

```powershell
python -I scripts/focus_watch.py --watch build/msvc-x64-debug --out build/focus/unit-debug.json -- `
    pwsh -NoProfile -File scripts/test.ps1
python -I scripts/focus_watch.py --watch build/msvc-x64-debug --out build/focus/gpu-debug.json -- `
    pwsh -NoProfile -File scripts/test.ps1 -Preset msvc-x64-debug-gpu
python -I scripts/focus_watch.py --watch build/msvc-x64-release --out build/focus/gpu-release.json -- `
    pwsh -NoProfile -File scripts/test.ps1 -Preset msvc-x64-release-gpu
```

## Measurements

On the reference machine (Windows 11, Windows Terminal as the default terminal application), the user working at it
during the runs; `focus_watch.py --watch build/<preset>` around `scripts/test.ps1`:

| Run | Tests | Watched | Foreground changes (to test processes) | Windows shown (visible of test processes, allowed) |
|---|---|---|---|---|
| Before: `rv_tests` one CLI launch test, `main`'s runner | 1 | 3.5 s | 4 (**2**: Windows Terminal windows of the fake's and `home-emulator.exe`'s consoles) | 4 (**2**, 2 hidden `PseudoConsoleWindow`s) |
| After: `test.ps1` (msvc-x64-debug) | 1240 passed | 292 s | 1 (0) | 1 (0, 1) |
| After: `test.ps1 -Preset msvc-x64-debug-gpu` | 1266 passed | 506 s | 23 (0) | 10 (0, 1) |
| After: `test.ps1 -Preset msvc-x64-release-gpu` | 1264 passed | 350 s | 22 (0) | 7 (0, 1) |

The foreground changes after the fix all went to the user's own programs (Chrome, VS Code, Explorer, Settings, an
installer). The one allowed window in each run is the fake emulator's background window of
`platform_win/process_runner: in the background the fake's window is seen ...`, at (-1600, 0)-(-1330, 182), off every
monitor. The hidden SDL windows of `retroverse.exe`, the asset preview and the spikes (`--hidden`) never become
visible, so the watcher does not even list them. The msvc-x64-debug unit run used the build before the error-mode change
(the gpu presets, which include every unit test, ran the final code). `scripts/gate.ps1` runs these same presets; it
was not run in full for this step.

## Decisions taken within the step

- **One switch, read by the runner, set by the harnesses.** Fixing the test profiles alone (`hideConsoleWindow`) would
  leave every future test, and ES-DE commands without `%HIDEWINDOW%` that the fixtures must keep, one forgotten flag
  away from the same problem. An environment variable, like `RV_LAUNCH_ALLOWLIST`, reaches every process the tests
  start (the CLI and the app as processes, the fake) without new command-line options, and the product never sets it.
  The test profiles also hide the console explicitly, so that their meaning does not depend on the variable.
- **The test executables set it themselves.** ctest's presets cover `scripts/test.ps1` and the gate, but agents and
  developers run `rv_tests.exe` and `rv_smoke_tests.exe` directly; both set it before any test unless it is set (an
  explicit `0` is honoured). `rv_platform_tests` and `spike_launch_tests` start no program and are left alone. The
  catch_discover_tests registration of the gpu cases is not changed: its `ENVIRONMENT` holds one entry and a second one
  (a `;` inside `PROPERTIES`) is not passed through reliably; `main.cpp` and the presets cover it.
- **Off-screen, not minimized, for the fake's window and for GUI programs.** A minimized window has a taskbar button,
  may flash, and is what `focus_watch.py` counts as seen; an inactive window off every monitor is invisible to the
  user and still a visible window for `IsWindowVisible`-based detection, which `focus_watch.py` records as allowed.
  The runner first told GUI programs `SW_SHOWMINNOACTIVE`; review found that inconsistent with the checker, and the
  runner now gives them the fake's treatment through `STARTF_USEPOSITION` and `SW_SHOWNOACTIVATE`. (A normal window
  off-screen still has a taskbar button, unlike the fake's tool window; no non-desktop test starts a GUI program
  today.)
- **Error mode inherited in the background only.** `CREATE_DEFAULT_ERROR_MODE` stays for the product (an emulator
  gets the default error mode, ADR-0006). In the background the runner's process takes the no-dialog error mode once,
  for the rest of its life: an automated run never wants a modal box.
- **The C runtime's dialogs off in unattended runs only.** `disableFailureDialogs()` is called by every test program
  and by the product's programs when they run unattended (frame-limited, `--no-dialogs`, or `RV_LAUNCH_BACKGROUND`
  set, which every program a test starts inherits). An interactive Debug run keeps the boxes, which a developer
  debugging an assertion wants. The reports still reach standard error (and the debugger), so a failure is not lost:
  ctest captures standard error, the smoke tests keep it in their case folder.
- **Windows Terminal attribution.** Its windows belong to `WindowsTerminal.exe` (started through DCOM by the system), so
  neither the process tree nor the title (the terminal titles the tab, not the window) links them to the test. The
  pseudo console window that conhost creates for the test's program is owned by the terminal's window once the
  terminal attached (`GetWindow(GW_OWNER)`), which links them; the 3 s timing rule covers a terminal window that
  showed before the owner was set.

## Tests

- `platform_win/process_runner: background launches never open a console window` (`[background]`): the flags
  (`CREATE_NO_WINDOW`, no `CREATE_NEW_CONSOLE`, no `CREATE_DEFAULT_ERROR_MODE`), the variable's values (unset, empty,
  `0`, `1`, long values), rv_tests running with it, and a console program whose request shows its console started with
  a console but no window (`--print-console`'s `hasWindow`), without `AllowSetForegroundWindow`.
- `platform_win/process_runner: in the background a program missing a DLL ends without the loader's dialog`: a copy of
  the fake without its DLLs, PATH limited to the system folder, ends with 0xC0000135.
- `platform_win/process_runner: in the background the fake's window is seen but never takes the foreground`: the fake
  with `--frames 30 --steal-focus --input-at-end --leave-foreground-window 1000` shows a window the session sees
  (`firstWindow`), never has the foreground, sends no input and starts no helper.
- `platform_win/process_runner: a console program never shares Retroverse's console` also checks `hasWindow` false.
- `platform_win/process_runner: background launches never open a console window`, section `rv_tests itself shows no
  failure dialog`: the error mode, `abort()`'s behaviour, `_set_error_mode` and, in Debug builds, the report modes of
  `_CRT_ASSERT` and `_CRT_ERROR` as `disableFailureDialogs()` sets them.
- `platform_win/process_runner: a failed assertion and abort() of a program end it without a dialog`: the fake with
  `--abort` (a failed `_ASSERTE` in Debug, then `abort()`) ends with exit code 3, not the 124 of its 30 s
  `--exit-timeout-ms` (which would end it if a box waited for a click).

## For the merge

- CLAUDE.md, "Safety rules", a new bullet after the launch guard's:
  - Automated tests never take the Windows foreground: no console window, no window shown on a monitor or activated,
    no foreground granted, no input injected, no modal dialog, while someone works at the machine. Only the tests
    labelled `desktop` (on demand, never the gate) may take over the screen. `RV_LAUNCH_BACKGROUND=1` (set, and neither
    empty nor `0`) makes `Win32ProcessRunner` start every program in the background (console programs with
    `CREATE_NO_WINDOW`, GUI programs inactive and off-screen through `STARTF_USEPOSITION` and `SW_SHOWNOACTIVATE`, no
    `AllowSetForegroundWindow`, an inherited error mode without dialogs) and `rv_fake_emulator` keep its window
    inactive and off-screen and skip its foreground and input options; the unit and gpu test presets, `rv_tests`,
    `rv_smoke_tests` (unless the variable is set) and `scripts/real-library.ps1` set it, the desktop preset and
    registrations set `0`. `rv::platform_win::disableFailureDialogs()` (no loader, crash, C runtime assertion or
    `abort()` box; reports go to standard error) runs in `rv_tests`, `rv_smoke_tests` and the fake always, and in
    `retroverse.exe`, the asset preview, the CLI and the spikes when they run unattended (frame-limited,
    `--no-dialogs`, or the variable set). A new test that starts a program
    starts it through the runner or `runChild` (`CREATE_NO_WINDOW`), runs apps `--hidden`, and creates SDL windows with
    `SDL_WINDOW_HIDDEN`; one that must show or focus a window is a desktop test. Check a change with
    `python -I scripts/focus_watch.py --watch build/<preset> --out build/focus/<name>.json -- pwsh -NoProfile -File
    scripts/test.ps1 [-Preset <preset>]` (exit 1 on a foreground change to, or a visible window of, a process of the
    build; read-only).
- CLAUDE.md, "Build and test", after the GPU smoke test bullet: "`python -I scripts/focus_watch.py --watch <dir> --out
  <json> -- <command...>` (or `--duration <s>`) records foreground changes and new windows while a command runs and
  fails when a process under `<dir>` (or its descendants, or the terminal window of their console) takes the foreground
  or shows a window where the user sees it, at any time while the window is visible, or when a system error dialog
  (WerFault for a test process, a csrss hard-error box) appears (docs/i2/step-focus.md)."
- CLAUDE.md, the fake emulator in "Outputs": add "with `RV_LAUNCH_BACKGROUND` set, an inactive off-screen window and no
  foreground or input options".
- docs/architecture.md, `platform_win`: `Win32ProcessRunner`'s background launches (`RV_LAUNCH_BACKGROUND`,
  `Win32ProcessRunnerOptions::background`).
- docs/roadmap.md, I2 table: the step "focus" (automated tests never take the foreground), done.
- BACKLOG.md: the gate and `scripts/test.ps1` could also run `focus_watch.py` around their runs and fail on a finding.
- BACKLOG.md: `rv_platform_tests` and `spike_launch_tests` keep the C runtime's default dialogs (they start no program
  and show no window); SDL's own assertion handler (`SDL_assert`, a message box) is not configured either.

## Review findings

Two reviews of the committed step reported eight findings (two pairs were the same); all were verified and fixed:

- `focus_watch.py` judged a window once, when it became visible (both reviews): a window shown off-screen, cloaked
  or empty that then moved on-screen stayed allowed. Every such window is now looked at again on every poll while it
  stays visible, and counts from the moment it is seen (`seenAt`, `firstState`). The foreground is also followed with
  an `EVENT_SYSTEM_FOREGROUND` hook, so that a change shorter than a poll is not missed (the review's optional part).
- `focus_watch.py` attributed a window to a cached process that had had the same id (the review's gpu run recorded
  the fake's window as a `conhost.exe`'s and missed it): processes are now told apart by their creation time, and
  chains take each ancestor as it was when its child was created. The measurements below were rerun with the fixed
  watcher.
- `focus_watch.py` could not see system error dialogs (WerFault, csrss): now rules of their own (above).
- The C runtime's message boxes were not covered by the error mode: `disableFailureDialogs()` (above).
- The runner's `SW_SHOWMINNOACTIVE` for GUI programs contradicted the checker: off-screen and inactive instead.
- `tests/unit/support/background_launches.cpp` named a test file that does not exist: it names the real check.
