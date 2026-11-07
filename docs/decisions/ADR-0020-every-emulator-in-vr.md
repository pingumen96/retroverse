# ADR-0020: Every emulator in VR: launch profiles for the window, the window followed, shortcuts and Windows games

- Status: accepted (the user's approval of the I11 plan on 2026-11-07; the scope, every emulator and the Windows
  games, chosen in planning: [i11/plan.md](../i11/plan.md))
- Date: 2026-11-07
- Amends:
  - ADR-0006: shortcuts are no longer refused but resolved to a program (§3). A protocol activation joins the ways a
    game starts (§3), and a wait strategy, `FollowWindow`, joins the three (§4).
  - ADR-0008 §8: ES-DE's "open the game's file" command (`%EMULATOR_OS-SHELL% /C %ROM%`) is translated natively, never
    run through `cmd.exe`, and `%RUNINBACKGROUND%` becomes a detached session (§3). Emulator profiles move to schema
    2 (§1).
  - ADR-0017 §7:
    - the window is followed, not kept (§2);
    - Retroverse asks for windowed or borderless play through launch arguments (§1), and detects exclusive fullscreen
      (§5);
    - the large screen is built, and a command stops watching the game (§6).
- Relates to: ADR-0018 §5 and §6 (the emulators screen, launch tests and the automation guard), ADR-0016 (the screens
  the captured game joins), ADR-0019 (I11's other half); roadmap I11

## Context

**The request.** The user asked that every emulator work in VR (2026-11-07), the Windows games included. Only RetroArch
has been tried in the headset, with an isolated configuration set by hand to windowed fullscreen and "do not pause when
inactive" (I10 steps 6, 10 and 11).

**What the code does** (the survey of 2026-11-07):
- **The window.** `Win32ProcessRunner::checkWindows()` reports the first visible top-level window of the job's
  processes, every 100 ms. It then sends nothing more while any window stays visible. That window is captured for the
  whole session.
  - A splash, a game list, a dialog or a console window shown first stays on the CRT.
  - A render window opened later is never picked up.
  - A window that is destroyed and made again (a fullscreen toggle, a renderer restart) freezes the CRT.
  - `WindowCapture::closed()` is never called.
- **Exclusive fullscreen.** Not detected. A black or frozen capture looks like success.
- **Foreground.** Given once per window, with no retry.
- **Per-emulator knowledge.** None. `assets/launcher/emulators.json` holds the `retroarch` profile alone, and ES-DE
  commands get `DirectProcess` and a 30 s window timeout.
- **Leaving the game.** In VR the return comes only with the game's end. `UserConfirmReturn` cannot be confirmed in a
  headset.
- **Where the picture shows.** A launch from the lobby or a hall captures a picture shown nowhere.
- **Refused launches.** `cmd.exe` is refused, so ES-DE's `%EMULATOR_OS-SHELL% /C %ROM%` commands never run: ps3's
  "RPCS3 Shortcut" (a `.lnk`) and every game of the `windows` system (`.lnk`, `.url`, `.exe`). `%RUNINBACKGROUND%`
  makes a game "requires launcher configuration".

**The user's emulators:**

| Systems | Emulator |
|---|---|
| ~36 systems | RetroArch, in their real configuration (I10's isolated copy aside) |
| gc | Dolphin standalone, `-b -e` |
| wii | Dolphin's RetroArch core |
| ps2 | PCSX2 standalone, `-batch` |
| ps3 | RPCS3 through a shortcut |
| atari800 | Altirra |
| apple2 | MAME's core, chosen over AppleWin's `-f` |
| trs-80 | sdl2trs |
| psx (alternative) | DuckStation, `-batch` |

## Decision

### 1. VR launch profiles

`emulators.json` moves to schema 2 (schema 1 files keep working). A profile gains an optional `vr` block, applied only
to a launch made in VR:
- `addArguments`, with a position (after the program or before the game), and `dropArguments` (exact elements, such as
  `-f` or `--fullscreen`).
- `appendConfig`: a file Retroverse writes under `<user data>/launcher/vr/<profile>.<ext>` from the profile's lines,
  passed with the profile's own option. RetroArch's is `--appendconfig`, with:
  - `video_fullscreen = "true"`;
  - `video_windowed_fullscreen = "true"` (borderless: capturable);
  - `pause_nonactive = "false"`.
- `window`: rules for §2 (class and title patterns, a minimum size, a window to prefer or to skip).
- `inGame`: `light` or `heavy`. A heavy emulator lowers the scene's render scale cap during the game from 0.8 to 0.65,
  so that the emulator keeps the GPU.

**ES-DE commands** are matched to a profile by the program's file name: the `%EMULATOR_X%` entry's executable
(`retroarch.exe`, `Dolphin.exe`, `pcsx2-qt.exe`, `rpcs3.exe`, `Altirra64.exe`, `AppleWin.exe`, `sdl2trs.exe`,
`duckstation-qt-*.exe`, `PPSSPPWindows64.exe`).

**Shipped profiles** for these programs carry their `vr` blocks. The arguments are checked against each program's help
on the user's builds in step 12:
- Dolphin: `-C Dolphin.Display.Fullscreen=False`, its render window preferred over the main window;
- PCSX2 and DuckStation: `-nofullscreen`;
- RPCS3: `--fullscreen` dropped;
- AppleWin: `-f` dropped;
- Altirra and sdl2trs: windowed by default.

**Retroverse never writes an emulator's configuration.** The user's `retroarch.cfg` and every other emulator file are
read at most. The append-file is Retroverse's own, in its data directory.

**On the desktop** launches are unchanged.

### 2. The window followed

`findVisibleWindow()` gives way to a ranking of the tree's visible top-level windows (`launcher::chooseGameWindow()`,
GPU- and Win32-free), best first:
1. **Preferred:** a window a profile's rule prefers;
2. **Normal:** a window of its own, at least 200 x 150 pixels of client area;
3. **Lesser:** a tool window or an owned dialog, as large;
4. **Last resort:** a smaller window or a console's, so that a console emulator or a single small window is still
   followed;
5. **Excluded:** a window a rule skips, a cloaked or a minimized one.

Within a rank the largest client area wins, then the window shown longest. A splash is not told apart by its look:
smaller than the game's, it loses to the game's window as soon as that shows.

**The choice is made again every 100 ms** (`launcher::WindowFollower`). A better window must stay better for 300 ms
before the choice moves (no flapping while a window is resized). A followed window that closes, is minimized or is
cloaked gives way at once. The tree's WindowShown and WindowsGone stay as they were, so the session's waiting rules do
not change. A new event, `WindowChanged`, carries the move, so `ProcessEvent::window` and the session's window follow
it. The capture restarts on the new window. When `closed()` reports the window gone it restarts too, a few times
2 s apart: Windows may give a window made again its old handle. The CRT never freezes on a dead window.

**Foreground** is given again until it holds: every 500 ms for 10 s after each new window, never in hidden or
background runs (ADR-0017 step 10's rule).

**The capture's frames are watched.** A window that is not minimized and yields only black frames for 5 s is reported
(§5; at most 0.2% of a sample's pixels lit, so that a border line does not hide it, and longer than a game's usual
black loading).

### 3. Shortcuts and Windows games

ES-DE's `%EMULATOR_OS-SHELL% /C %ROM%`, with any of `%HIDEWINDOW%`, `%ESCAPESPECIALS%` and `%RUNINBACKGROUND%`, is
recognised as "open the game's file". Retroverse opens it itself, by its extension; `cmd.exe` never runs, and every
other use of a shell stays refused.

**`.lnk`**
- Read with `IShellLinkW` and `IPersistFile`: read-only, no resolution search (`SLR_NO_UI | SLR_NOSEARCH |
  SLR_NOTRACK`), no user interface.
- Its target must pass ADR-0006's validation: an absolute PE `.exe`. A shortcut to a batch file, a document or another
  shortcut is refused, with the target named.
- Its argument string is split as `CommandLineToArgvW` splits it and passed as argv (`buildCommandLine` is its exact
  inverse).
- Its working folder becomes the launch's.

**`.exe`** starts directly, as any program.

**`.url`**
- An Internet Shortcut whose `URL=` names a scheme of the profile list `protocols` (shipped: `steam`,
  `com.epicgames.launcher`, `goggalaxy`, `uplay`, `origin2`).
- Activated by `ShellExecuteExW` with the URL as its file, the verb `open`, `SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI`.
- Never `http`, `https`, `file`, `shell`, `search`, `ms-*` or any scheme not listed. The URL is one value, never a
  command line.

**`.bat` and `.cmd`** stay refused (BACKLOG, "Batch launchers").

**Detached games.** `%RUNINBACKGROUND%`, a protocol activation, and a program that exits within the startup grace while
a new window appears are followed as detached sessions (§4).

**Automated runs:** a protocol activation is refused when `RV_LAUNCH_ALLOWLIST` is set, since the allow-list names
programs, not schemes. A `.lnk`'s target must be listed like any program.

### 4. The detached session: `FollowWindow`

A fourth wait strategy (ADR-0006):
1. **The game's window.** After the start or the activation, the session waits within the window timeout for the first
   new top-level window that §2's scorer accepts. Its process must have been created after the launch and be none of
   these: Retroverse, Explorer, or a process the profile names in `ignoreProcesses` (Steam's own, by default:
   `steam.exe`, `steamwebhelper.exe`).
2. **Then the process.** The session follows that window's process, opened with `SYNCHRONIZE |
   PROCESS_QUERY_LIMITED_INFORMATION`, to its end. That end is the return: `Returned`, with the exit code when it can be
   read.
3. **Without a window.** When no window qualifies within the timeout, the session becomes `UserConfirmReturn`, which
   VR can now confirm (§6).

Retroverse never terminates the followed process, as for every launch.

### 5. Exclusive fullscreen detected

Exclusive fullscreen is detected by either sign:
- while the game's process holds the foreground, `SHQueryUserNotificationState` returns
  `QUNS_RUNNING_D3D_FULL_SCREEN`;
- the capture yields only black frames for 5 s from a window that is not minimized.

**What happens then:**
- VR shows a notice: "Il gioco è in schermo intero esclusivo: impostalo su finestra senza bordi per vederlo qui". The
  game keeps running.
- The desktop logs it.
- The run report's `launch` section records it.

### 6. Watching from VR

**Stop watching.** Holding a Touch controller's menu button for one second stops watching:
- the room's picture and decoders come back while the game runs, and the hero CRT shows its playlist again;
- the card of the running game offers "Torna al gioco", which captures it again and gives it the foreground;
- the card also offers "Il gioco è terminato", which confirms the return as `UserConfirmReturn` does: the session ends,
  the game keeps running if it does.

The pad and the keyboard stay the game's.

**The large screen** (ADR-0017 §7's option): a quad 2.4 m wide, at 2.8 m in front of the head when the launch began,
shown at the captured window's aspect. It is used:
- when no hero CRT is bound (a launch from the lobby, a hall or the quick catalog);
- when `xr.json`'s `gameScreen` is `large`.

**The game's sound.** At a launch in VR the headset's audio endpoint, from `XR_OCULUS_audio_device_guid` where the
runtime offers it, is compared with Windows' default render endpoint. When they differ, a notice says the game will
play on the PC's speakers. Retroverse never changes the default device.

### 7. Tests

**The fake emulator gains modes:**

| Mode | What it does |
|---|---|
| `splash` | a small window, then the game window |
| `two-windows` | a main window and a render window |
| `recreate` | destroys and makes its window again |
| `child` | the window from a child process |
| `late` | the window after 20 s |
| `detached` | starts another fake outside the job and exits |
| `black` | only black frames |

**Unit tests:**
- the scorer's table;
- each profile's argv with and without VR on ES-DE 3.4.1's commands;
- the append-file's bytes;
- `.lnk` reading on shortcuts made in a `TempDir` at test time (hostile names, a target that is a batch file);
- the `.url` scheme list;
- `FollowWindow`'s choice;
- the stop-watching command's states.

**GPU tests** on `--vr --xr-runtime fake`, `RV_LAUNCH_ALLOWLIST` naming the fake:
- each mode shows the fake's game window on the CRT, its counter advancing after every change;
- a `.lnk` to the fake;
- the detached fake followed to its end;
- the exclusive notice on `black`;
- stop and resume watching;
- the large screen from the lobby.

No test activates a protocol or starts a real emulator. DuckStation never starts in an automated run. The real
emulators are the user's trial in the headset, at Gate I11.

### 8. Acceptable performance

ADR-0017 §9's rows hold with a captured game, the heavy `inGame` profile included. They are measured in Release with
the fake emulator, and by the user with the real emulators.

## Rules that change

- **Shortcuts launch**, by their target, under ADR-0006's checks. A `.url` launches through a listed protocol only.
  `cmd.exe` still never runs, and batch files stay refused.
- **Retroverse never writes an emulator's own configuration.** A VR launch changes arguments and adds Retroverse's own
  append-file.
- **The captured window may change during a game**, and exclusive fullscreen is reported, never silent.

## Consequences

**Gains**
- Every emulator of the user's library, and the Windows games, can be played in the headset.
- What blocks a picture is said in the headset instead of a frozen CRT.
- The user can leave a game's picture without leaving the game.

**Costs**
- A profile per emulator to keep current with each program's command-line options.
- A fourth wait strategy, and a scan of new top-level windows during detached sessions.
- Protocol activations that tests cannot run.

**Deferred**
- The Touch controllers as the emulator's pad (ViGEmBus, BACKLOG).
- The D3D11 bridge (ADR-0017).
- Batch launchers.
- Moving an emulator's window to another monitor.

See `BACKLOG.md`, "Parked at I11's planning".
