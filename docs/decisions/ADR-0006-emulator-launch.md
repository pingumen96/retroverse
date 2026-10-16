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
was met: 30 of 30 RetroArch launches, the latest of the six measurements at most 368 ms after the exit (median 74 ms
with either window policy). The figures here are those of the measurements made after the spike's review.

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
- `CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP`, and a console
  of its own for a console program: `CREATE_NEW_CONSOLE`, as when ES-DE (a GUI program) starts it, or `CREATE_NO_WINDOW`
  on request (ES-DE's `%HIDEWINDOW%`); never Retroverse's console, whose closing (`CTRL_CLOSE_EVENT`) or Ctrl+Break
  would end every program attached to it. GUI programs ignore both flags. Only the output log and NUL handles are
  inherited. The suspended process joins a job object **without** `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and with
  `JOB_OBJECT_LIMIT_BREAKAWAY_OK`, whose completion port reports the tree; `AllowSetForegroundWindow` is called (it
  succeeds for GUI programs only, harmless otherwise); the window is handed over (`StartCallbacks::beforeResume`, while
  Retroverse is still the foreground process); then the process resumes, and the session's start time is that moment.
  Until then any failure, an exception from the hand-over included, ends the suspended process, which never ran;
  `start()` reports errors and never throws.
- A watcher thread per launch blocks on the process handle and the port (counting the job's processes at every
  wake-up, and checking the tree's windows every 100 ms) and wakes the main loop. The console host (`conhost.exe`) that
  a console program starts inside the job is followed but is no part of the tree: a job left with only console hosts
  is an empty tree. **Retroverse never terminates an emulator**: a session that ends stops following what still runs
  (`IRunningProcess::stopWatching`), and destroying it changes nothing for the processes.

**Waiting** (`rv::launcher::LaunchSession`):

| Strategy | Use | The session ends |
|---|---|---|
| DirectProcess (default) | emulators that run in the process started: RetroArch, DuckStation, PCSX2, ... | when the started process exits |
| ProcessTree | profiles marked as launchers or wrappers | when the job is empty (a crash anywhere in the tree makes it Crashed) |
| UserConfirmReturn | programs whose emulator runs outside the job (packaged applications, launchers that break away); the fallback when the job cannot be set up | when the user confirms the return; a crash, a loader error or an unclean exit within 2 s still end it at once |

Outcomes: an NTSTATUS exception code is `Crashed` (named: `0xC0000005 STATUS_ACCESS_VIOLATION`); a loader error or an
unclean exit within the 2 s startup grace is `FailedToStart`; otherwise `Returned` with the exit code. A clean exit
within the grace is deliberately **not** a failure to start (the task's first wording said it was): under
DirectProcess a launcher stub exits 0 at once while the game it started is loading, and an emulator may also give up
at once with exit code 0 (a missing BIOS shown in its own dialog). The session reports it as `Returned` with
`quickExit` when the tree was not waited for (DirectProcess, UserConfirmReturn), and the product must tell the user
("the game ended right away; if it is still running, its profile needs ProcessTree"). Under ProcessTree, which waited
for the whole tree, a quick end is a plain return.

**Timeouts**: the startup grace (2 s, counted from the moment the process runs, after the hand-over) only classifies
exits. The window timeout (30 s): no window of the tree visible for 30 s, at the start or after the last one closed,
while its processes run, makes the session `StillRunning`, and Retroverse offers the return; a window shown again
resumes the session. No timeout ends a session or an emulator by itself.

**The window: keep it.** While the emulator runs, Retroverse's window stays in borderless fullscreen behind it, renders
nothing (no GPU work) and ignores input (SDL delivers gamepad events only to a focused Retroverse window). When the
emulator's window closes, Windows activates the kept window itself, the next in the z-order: in the measurements
Retroverse was in the foreground 170-354 ms before RetroArch's process ended, steady at 60 FPS a median 74 ms after the
exit, with no other application shown in between in any of the 15 launches (an application that takes the foreground
during the game would show at the return until the remedies below). Minimizing instead (available as
`HandoverPolicy::Minimize`) also met the criterion, but showed the user's other windows for about 100 ms at each launch
and 220-390 ms at each return.

**The return** (`rv::platform::WindowHandover`): restore a minimized window (`SDL_RestoreWindow`, then
`ShowWindow(SW_RESTORE)`, since SDL 3.4 ignores the former for fullscreen windows); apply borderless fullscreen again,
over the monitor the window was on, if a display mode change shrank or moved it (the window is in borderless
fullscreen; one in exclusive fullscreen would come back borderless); raise it. When Windows refuses the raise, another
application's window holds the foreground, and the remedies follow: `AttachThreadInput` to the foreground window's
thread at once (never a hung one, always detached), then, 100 ms later, the last resort: the taskbar button flashes
until the user returns (`FlashWindowEx`). In the measurements the raise was refused whenever another window held the
foreground, and the attachment brought Retroverse back 44-121 ms after the exit in 18 of 18 such returns, also against
`LockSetForegroundWindow`. The Alt key trick (an injected key press around `SetForegroundWindow`) is **not** used: it
did not help where the attachment could not (a hung foreground window), and when the raise still fails the injected
key reaches the foreground application (Alt opens a classic menu bar). Rendering and input resume at once; the pad
works as soon as the window has the keyboard focus.

**What the user sees while playing**: the emulator only (and, for a console emulator not hidden, its own console
window, as under ES-DE). If the user switches to Retroverse while the emulator runs, it shows that a game is running
and offers to return without closing it (the session ends with the emulator left running) or to go back to the game
(product UI in I2 and I7). Closing Retroverse never ends the emulator.

## Consequences

- I2's launcher builds on `rv_launcher` and `rv_platform` as they are: emulator profiles and ES-DE commands become
  `LaunchRequest`s (tokens resolved to argv elements, never a command string), the application drives a
  `LaunchSession` in its ExternalEmulatorRunning state (§15.5) and a `WindowHandover` with the keep policy, and shows
  `quickExit` and `processesRemained` to the user.
- The roadmap's fallbacks are in place: `AllowSetForegroundWindow` before the launch (best effort), a minimized window
  with a notification (`Minimize` and the flash), a direct process with a "return" confirmation (`UserConfirmReturn`,
  automatic when the job cannot be set up).
- Open risks:
  - The first return of a run stalls twice (about 100 ms, then 147-325 ms; 60 FPS back within 307-442 ms, up to
    710 ms after an exclusive display mode); later returns once (usually 40-60 ms). Presentation mode changes are the
    likely cause. In BACKLOG.md since the merge of the I1 spikes ("Frame stalls after returning from an emulator",
    with these ranges).
  - Exclusive-fullscreen emulators change the display mode: the return took up to 710 ms (fullscreen re-applied, the
    window moved back to its monitor). Not measured with a real exclusive-mode emulator: DuckStation was not launched,
    as its data folder cannot be isolated without writing into its installation.
  - Crash dialogs hold the session. Emulators run with the default error mode: a crash goes through Windows Error
    Reporting, and where a just-in-time debugger is registered without `Auto` (Visual Studio on the reference machine)
    its modal prompt holds the crashed process, and the session, until answered; a missing DLL shows the loader's modal
    box with the same effect. The spike's crash measurements ran with the fake's own dialogs off; the product's answer
    is the window timeout and the user's return, not a kill.
  - A hung window of another application in the foreground at the return defeats every remedy (3 of 3 measured): the
    taskbar button flashes and the user, or that window going away, brings Retroverse back.
  - While the emulator shuts down after closing its window (about 170-350 ms for RetroArch), the kept window shows its
    last frame: render a "returning" state, or resume when the tree's windows are gone (`WindowsGone`).
  - Children that break away from the job and programs activated by the system (packaged applications) are outside
    the tree: their profiles need UserConfirmReturn. A launcher that leaves a helper process running keeps a ProcessTree
    session alive until the window timeout offers the return.
  - Console emulators that run without a window would reach StillRunning after 30 s: such a profile sets a longer or no
    window timeout.
  - `retroverse.exe` is a console-subsystem program (double-clicked, it opens a console window): console emulators no
    longer share it, but a GUI-subsystem release build would also spare the user that window (deferred at the merge
    of the I1 spikes: BACKLOG.md, "Release subsystem").
  - Emulators on another monitor than Retroverse's: Retroverse stays visible on its monitor (keep); untested with a real
    emulator.
