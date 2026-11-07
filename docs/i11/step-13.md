# I11 step 13: shortcuts and Windows games

ADR-0020 §3, §4 and §5; roadmap I11 step 13. ES-DE launches a Windows game, an RPCS3 or shadPS4 shortcut and a store's
game with `%HIDEWINDOW% %ESCAPESPECIALS% %EMULATOR_OS-SHELL% /C %ROM%`: cmd.exe opens the game's file. Retroverse
refused that command (cmd.exe is a command interpreter), so 15 of ES-DE's systems never started. This step opens the
game's file natively and follows games that run outside the process Retroverse starts. It also tells the user when a
game in exclusive fullscreen cannot be shown on the CRT.

## What was built

- **Shortcuts read, never resolved** (`core::ISystemProbe::readShortcut()`, `platform_win/src/shortcut.cpp`):
  - `IShellLinkW` and `IPersistFile` load the `.lnk` file for reading, in a COM apartment of the calling thread (one is
    made for the call when the thread has none). It is never saved and never resolved, so no search for a moved target
    and no link tracking.
  - What it returns: the target with its environment variables expanded, the argument string, the working folder and
    the "Run as administrator" flag.
  - A shortcut whose target is no file (a shell folder, a store app, a web address) is refused.
  - Probes that read no shortcuts (the default) say so. `FakeSystemProbe::addShortcut()` describes them in tests.
- **"Open the game's file"** (`esde/src/launch_translation.cpp`). The command is recognized when its program is one
  `%EMULATOR_X%` element finding cmd.exe, followed by exactly `/C` and `%ROM%` (flags aside). Then cmd.exe never runs,
  and Retroverse opens the file by its extension:
  - **`.lnk`:** its target must be a `.exe` or `.com` that exists and is no command interpreter or launcher. Its
    arguments are split as `CommandLineToArgvW` splits them. The working folder is the shortcut's, else
    `%STARTDIR%`'s, else the target's folder.
  - **`.exe`, `.com`:** the file starts as itself, in `%STARTDIR%` or its own folder. `%HIDEWINDOW%` hides a console
    program's window, as cmd.exe's hidden console did.
  - **`.url`:** its `[InternetShortcut]` `URL=` is activated when the scheme is one of `emulators.json`'s `protocols`.
  - **Refused, each requiring launcher configuration and named:**
    - a batch file (`.bat`, `.cmd`) or a shortcut to one;
    - a shortcut to a document;
    - a scheme not listed;
    - a folder;
    - anything else, a disc image for example (with "choose another command for <system>").
  - A system's translation names no program and leaves the file to each game (a note says so).
  - `%ESCAPESPECIALS%` means nothing there; in any other command it still requires configuration.
- **`emulators.json`'s `protocols`** (schema 2, `launcher::ProtocolRule`): a scheme and its launcher's own processes.
  - The user's rule for a scheme replaces the shipped one.
  - A scheme that `checkActivationUrl()` refuses can never be listed: `http`, `https`, `file`, `shell`, `search`, the
    `ms-` and `microsoft-` schemes, and the like.
  - Shipped: `steam`, `com.epicgames.launcher`, `goggalaxy`, `uplay`, `origin2`.
- **Protocol activations** (`LaunchRequest::activationUrl`). `validateLaunchRequest()` accepts one that names nothing
  else. The runner opens it with `ShellExecuteExW`: the verb "open", the URL as one value, synchronous, no user
  interface, COM in a single-threaded apartment. The automation guard refuses every activation (`GuardRule::Activation`):
  `RV_LAUNCH_ALLOWLIST` names programs, not schemes.
- **`WaitStrategy::FollowWindow`**, for `%RUNINBACKGROUND%` (any command, not only the shortcuts) and for activations.
  - **Adoption:** besides its job, the runner adopts processes outside the tree by their windows. A window qualifies
    when the scorer does not exclude it and its process meets all of these:
    - created after the launch;
    - started, directly or through processes created since the launch, by the tree, by Retroverse or by a launcher
      process;
    - none of the launcher processes, Explorer, a console or Windows' error reporting.

    Adopted processes are reported as `ChildStarted` and `ChildExited`, and their windows join the ones the window
    follower chooses from.
  - **The end:** once a window has shown, the session ends when every process followed has exited.
  - **Before a window:** a launcher that exits cleanly has handed over; one that fails within the startup grace is a
    failure to start. Without any window, the session waits: StillRunning after the window timeout, which the user
    ends.
- **Exclusive fullscreen** (ADR-0020 §5). The runner reports `ExclusiveFullscreen` once a session, when a window it
  follows holds the foreground while `SHQueryUserNotificationState` says `QUNS_RUNNING_D3D_FULL_SCREEN`. In VR, a
  capture that shows only black for 5 s (step 11) counts as the same sign.
  - The run report's launch records it (`exclusiveFullscreen`), with the activation's scheme (`activation`).
  - In VR the session panel says "Il gioco è in schermo intero esclusivo: impostalo su finestra senza bordi per vederlo
    qui", and the panel stands in front of the CRT, which otherwise hides it while a game shows (`exclusiveNoticeFrames`
    in the VR record).
  - On the desktop it is logged.
- **`retroverse-cli launch --dry-run`** shows an activation ("Activates", "Waits", "Launcher processes"; JSON
  `activationUrl` and `launcherProcesses`) and a `FollowWindow` wait.
- **The fixture `esde/shortcuts`:** an invented `windows` system with ES-DE's two "open the game's file" commands, and
  an Internet Shortcut of an invented scheme. The tests write their `.lnk` files (`support/shortcut_file.hpp`).

## Tests

- **Unit:**
  - `test_activation.cpp`:
    - the schemes and URLs accepted and refused;
    - an activation names nothing else;
    - the guard refuses every activation;
    - `protocols` parsed, refused, merged and shipped.
  - `test_launch_session.cpp`, `FollowWindow`:
    - a launcher that hands over to an adopted process;
    - a failure before the window;
    - a clean exit that waits for the window and then for the user;
    - an activation (no process of its own) whose game crashes;
    - exclusive fullscreen recorded.
  - `test_open_game_file.cpp`:
    - a shortcut's target, arguments (quotes and backslashes) and working folder;
    - `%STARTDIR%`, `%RUNINBACKGROUND%`;
    - the administrator flag noted;
    - shortcuts to a batch file, a document, cmd.exe, a missing program; an unreadable shortcut;
    - a `.exe` with `%HIDEWINDOW%`;
    - `.url` files: a listed scheme (a byte order mark, CRLF, a lower-case key), an unlisted one, none listed, no URL,
      a URL with a blank;
    - a batch file, a disc image, a system;
    - no shell found: refused as ES-DE refuses it, nothing opened.
  - Five new blocker codes, each with its sentence on the card, the quick catalog and the directory: `file-not-opened`,
    `shortcut-unreadable`, `shortcut-target`, `batch-file`, `protocol-not-listed`.
  - The older translation tests now expect cmd.exe's other commands refused as before, the "open the game's file" ones
    opened.
  - `test_shortcut.cpp`: shortcuts written at test time and read back (arguments in UTF-8, the folder, the flag, a
    target that does not exist), the file never written, what is no shortcut, a thread without COM.
  - `test_process_runner.cpp`: `FollowWindow` with the fake emulator. `--spawn-child-and-exit --child-breakaway`
    starts the game outside the job and exits at once; the game's window is adopted, and the session ends with its exit
    code (5) after 2.5 s.
- **Unit, real launches** (`test_cli_shortcuts.cpp`, `retroverse-cli` as a process, in the background, the allow-list
  naming the fake):
  - a `.lnk` to the fake starts it with the shortcut's arguments in its working folder, and the dry run never names
    cmd.exe;
  - the `%RUNINBACKGROUND%` command follows a game outside the launcher's tree to its end (its exit code 4, 2.5 s);
  - the `.url` dry run shows the activation and `FollowWindow`, and the launch is refused by the guard.
- **GPU** (`--vr --xr-runtime fake`): the black window of step 11 now also records the launch's exclusive fullscreen,
  and the notice stands in front of the CRT. Checked by eye on a screenshot of the run's last frame while the notice
  showed (both eyes, the panel's three lines).
- **Real library** (`test_cli_profiles_real.cpp`, `[real-library][shortcuts]`, through `scripts/real-library.ps1`, 0
  differences in the snapshots) on 2026-11-07:
  - 15 systems' chosen command opens the game's file;
  - windows: its one game, a shortcut, is ready and starts its program;
  - ps3: the 13 disc images are refused (`file-not-opened`): ES-DE's default "RPCS3 Shortcut" command opens the file,
    and an image is not a file Retroverse opens. "RPCS3 ISO (Standalone)" will run them once RPCS3 is installed;
  - the other 13 systems hold no game.

## Departures

- **`protocols` live in `emulators.json`, each scheme with its own launcher processes** (`launcherProcesses`), instead
  of a profile's `protocols` and `ignoreProcesses`. The translation needs the schemes before any profile is chosen, and
  each store has its own processes.
- **The detached session adopts processes, not one window** (ADR-0020 §4 rewritten). It adopts every process whose
  window could be the game's, provided its ancestry leads back to the launch. It ends when all of them have exited, so
  a game's own launcher window that starts the game and exits does not end the session.
  - Any window the scorer does not exclude qualifies: the ancestry is the filter, so a small or tool window of the
    game's process counts.
  - An automated run's fake shows a small tool window, which the step-11 ranks would have refused.
- **A program that exits within the startup grace while a new window appears** is not followed as detached. Under
  DirectProcess it stays a quick exit, which the session reports, as before. Only `%RUNINBACKGROUND%` and activations
  follow windows.
- **The shortcut is never resolved:** `IShellLinkW::Resolve` is not called at all, so the target stays as stored. A
  moved target is "the program does not exist".
- **The administrator flag is not honoured.** The program starts without elevation and the translation notes it. A
  program whose manifest requires elevation fails as before ("requires administrator rights").
- **The protocol's handler process** that `ShellExecuteExW` returns (a launcher passing the URL to its running copy) is
  not followed: the game is the process the launcher starts.
- **The shell's exclusive fullscreen state** is read only while a followed window holds the foreground. The black-frames
  sign exists only in VR, where the window is captured.
- **The `detached` and `child` fake modes** are the fake's `--spawn-child-and-exit` with and without
  `--child-breakaway`, which already existed.
- **The `.lnk` and the detached session are proved by real launches through `retroverse-cli`** rather than in the
  headset: both go through the same launch request, runner and session as the app.
- **The shell refuses to save a shortcut to a program stub** (a PE of headers only). The shortcut tests write the
  shortcut first and the stub after it; the real-launch tests point at the fake.
