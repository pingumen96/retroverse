# ADR-0006: Emulator launch and the return to Retroverse

- Status: accepted
- Date: 2026-10-16

## Context

Retroverse starts emulators that are already installed (specification §11) and must come back, in the foreground, in
borderless fullscreen, with the gamepad working and the frame rate steady, as soon as the game ends (§11.4: "Recover
foreground/window/display state", "Resume input/video/audio"). The risks were Windows' foreground lock (a background
process may not take the foreground), wrappers and launchers that exit while the emulator runs (§11.4:
waitDirectProcess, waitProcessTree, userConfirmReturn), crashes, emulators that change the display mode, and never
ending an emulator without the user's confirmation (§11.5). Arguments come from untrusted data (§11.3, §19.1): no shell.

I1 spike (f) built the first version of `rv_launcher` and of the Windows side in `rv_platform`, and measured the return
with RetroArch 1.21 and a fake emulator: [docs/spikes/i1-launch.md](../spikes/i1-launch.md). Success criterion: 5 of 5
RetroArch launches back in the foreground, fullscreen, with pad input and 60 FPS within 1 s of the emulator's exit. It
was met: 30 of 30 RetroArch launches, the latest of the six measurements at most 400 ms after the exit (median 72 ms).

## Decision

**Starting a program** (`rv::platform::Win32ProcessRunner` behind the port `rv::launcher::IProcessRunner`):

- No shell and no command interpreter. A launch is a `LaunchRequest`: the executable, arguments as argv (each element
  one argument, never a command string), working directory, environment overrides, wait strategy, timeouts, an optional
  output log. `CreateProcessW` gets the executable as `lpApplicationName` and a command line quoted as the exact inverse
  of `CommandLineToArgvW` (`launcher::buildCommandLine`).
- Validation before anything starts: an absolute path to a `.exe` or `.com` file holding an x64, x86 or ARM64 PE image.
  Batch files (run by `cmd.exe`, which re-parses `%`, `^`, `&`, `|`), shortcuts, documents, DOS and 16-bit programs are
  refused; so are NUL characters, malformed UTF-8 and command lines over 32766 UTF-16 units. The launching thread turns
  off hard-error dialogs around `CreateProcessW` (a DOS image would otherwise block Retroverse on a modal box).
- `CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP` (and
  `CREATE_NO_WINDOW` on request, ES-DE's `%HIDEWINDOW%`); only the output log and NUL handles are inherited. The
  suspended process joins a job object **without** `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and with
  `JOB_OBJECT_LIMIT_BREAKAWAY_OK`, whose completion port reports the tree; `AllowSetForegroundWindow` is called (it
  succeeds for GUI programs only, harmless otherwise); the window is handed over (`StartCallbacks::beforeResume`, while
  Retroverse is still the foreground process); then the process resumes. A watcher thread per launch blocks on the
  process handle and the port (checking the job's process count and the tree's windows every 100 ms) and wakes the main
  loop. **Retroverse never terminates an emulator**; destroying a session only stops following it.

**Waiting** (`rv::launcher::LaunchSession`):

| Strategy | Use | The session ends |
|---|---|---|
| DirectProcess (default) | emulators that run in the process started: RetroArch, DuckStation, PCSX2, ... | when the started process exits |
| ProcessTree | profiles marked as launchers or wrappers | when the job is empty (a crash anywhere in the tree makes it Crashed) |
| UserConfirmReturn | programs whose emulator runs outside the job (packaged applications, launchers that break away); the fallback when the job cannot be set up | when the user confirms the return; a crash, a loader error or an unclean exit within 2 s still end it at once |

Outcomes: an NTSTATUS exception code is `Crashed` (named: `0xC0000005 STATUS_ACCESS_VIOLATION`); a loader error or an
unclean exit within the 2 s startup grace is `FailedToStart`; otherwise `Returned` with the exit code. A clean exit
within the grace under DirectProcess is reported as a quick exit: the profile probably needs ProcessTree.

**Timeouts**: the startup grace (2 s) only classifies exits. The window timeout (30 s): no window of the tree visible
for 30 s, at the start or after the last one closed, while its processes run, makes the session `StillRunning`, and
Retroverse offers the return; a window shown again resumes the session. No timeout ends a session or an emulator by
itself.

**The window: keep it.** While the emulator runs, Retroverse's window stays in borderless fullscreen behind it, renders
nothing (no GPU work) and ignores input (SDL delivers gamepad events only to a focused Retroverse window). When the
emulator's window closes, Windows activates the kept window itself, the next in the z-order: in the measurements
Retroverse was in the foreground before RetroArch's process even ended, steady at 60 FPS about 70 ms after the exit,
with no other application ever shown in between. Minimizing instead (available as `HandoverPolicy::Minimize`) also met
the criterion, but showed the user's other windows for 120-250 ms at each launch and return.

**The return** (`rv::platform::WindowHandover`): restore a minimized window (`SDL_RestoreWindow`, then
`ShowWindow(SW_RESTORE)`, since SDL 3.4 ignores the former for fullscreen windows); apply borderless fullscreen again,
over the monitor the window was on, if a display mode change shrank or moved it; raise it. While it is not in the
foreground, the remedies follow one at a time, 100 ms apart: `AttachThreadInput` to the foreground window's thread
(never a hung one, always detached), an injected Alt key around `SetForegroundWindow`, and as the last resort the
taskbar button flashes until the user returns (`FlashWindowEx`). Rendering and input resume at once; the pad works as
soon as the window has the keyboard focus.

**What the user sees while playing**: the emulator only. If the user switches to Retroverse while the emulator runs,
it shows that a game is running and offers to return without closing it (the session ends with the emulator left
running) or to go back to the game (product UI in I2 and I7). Closing Retroverse never ends the emulator.

## Consequences

- I2's launcher builds on `rv_launcher` and `rv_platform` as they are: emulator profiles and ES-DE commands become
  `LaunchRequest`s (tokens resolved to argv elements, never a command string), the application drives a
  `LaunchSession` in its ExternalEmulatorRunning state (§15.5) and a `WindowHandover` with the keep policy.
- The roadmap's fallbacks are in place: `AllowSetForegroundWindow` before the launch (best effort), a minimized window
  with a notification (`Minimize` and the flash), a direct process with a "return" confirmation (`UserConfirmReturn`,
  automatic when the job cannot be set up).
- Open risks:
  - The first return of a run stalls twice (about 100 and 250 ms, 60 FPS back within 410 ms); later returns once
    (40-65 ms). Presentation mode changes are the likely cause (BACKLOG).
  - Exclusive-fullscreen emulators change the display mode: the return took up to 785 ms (fullscreen re-applied). Not
    measured with a real exclusive-mode emulator: DuckStation was not launched, as its data folder cannot be isolated
    without writing into its installation.
  - A window that another application brings to the front during the game makes the plain raise fail; AttachThreadInput
    worked every time it was needed, the Alt key and the flash were never reached on the reference machine.
  - While the emulator shuts down after closing its window (about 200-350 ms for RetroArch), the kept window shows its
    last frame: render a "returning" state, or resume when the tree's windows are gone (`WindowsGone`).
  - Children that break away from the job and programs activated by the system (packaged applications) are outside
    the tree: their profiles need UserConfirmReturn. A launcher that leaves a helper process running keeps a ProcessTree
    session alive until the window timeout offers the return.
  - Console emulators that run without a window would reach StillRunning after 30 s: such a profile sets a longer or no
    window timeout.
  - Emulators on another monitor than Retroverse's: Retroverse stays visible on its monitor (keep); untested with a real
    emulator.
