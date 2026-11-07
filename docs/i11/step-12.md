# I11 step 12: every emulator's launch made for VR

ADR-0020 §1, roadmap I11 step 12. A game launched in the headset must show in a window the capture can read (windowed
or borderless, never exclusive fullscreen) and go on when it loses the focus. Each emulator asks for that in its own
way, and the same emulator is started by ES-DE commands and by Retroverse's profiles alike. This step describes the
programs once and adapts every launch to its program.

## What was built

- **`emulators.json` schema 2** (`rv/launcher/emulator_profile.hpp`): a `programs` list beside the profiles. Schema 1
  files are still read. A program has:
  - `files`: its executable's file names, wildcards allowed;
  - `window`: rules for the window followed (step 11), applied to every launch;
  - `vr`: what a launch in VR changes:
    - `addArguments`, put right after the program;
    - `dropArguments`, exact elements removed;
    - `appendConfig`: a file Retroverse writes, and the program's option that passes it;
    - `inGame` and `heavyWith`: the load.

  The user's programs replace the shipped ones by id and are matched first (`EmulatorProfiles::programFor()`).
- **`launcher::adaptLaunch()`** (`rv/launcher/vr_launch.hpp`, GPU-free): a request and its program become the request
  to start. The window rules always; in VR:
  - the dropped arguments removed;
  - the append-file's option and path in front, or joined to the option's value the launch already passes
    (`joinWith`, RetroArch's `a|b`);
  - the added arguments first;
  - the load: heavy for a heavy program, or when an argument's file name matches `heavyWith` (a RetroArch core).

  A request Windows would refuse (a command line too long) keeps the one asked for, with the reason.
  `writeAppendFile()` writes `<user data>/launcher/vr/<file>` (`Paths::vrLaunchDir()`) only when its bytes change.
- **The app.** `LaunchFlow::start()` takes an adapter, which the product UI's hook (`LaunchHooks::adapting`) fills
  from the app:
  - `emulators.json` is read again at each launch;
  - the program is found and the request adapted;
  - in VR the append-file is written (when it cannot be, the launch goes on without it).

  The run report's launch gains `program`: the id, VR, the arguments added and dropped, the append-file, the load, the
  reason when refused. A heavy launch caps the scene's render scale at 0.65 while its game shows, instead of 0.8.
- **The shipped programs** (`assets/launcher/emulators.json`):

  | Program | Files | In VR |
  |---|---|---|
  | RetroArch | `retroarch.exe` | `--appendconfig <data>\launcher\vr\retroarch.cfg`: `video_fullscreen`, `video_windowed_fullscreen` true, `pause_nonactive`, `config_save_on_exit` false; heavy with the GPU-hungry cores; its window class preferred |
  | Dolphin | `Dolphin.exe` | `-C Dolphin.Display.Fullscreen=False`, `RenderToMain=False`, `Interface.PauseOnFocusLost=False`, `Input.BackgroundInput=True`; heavy; the render window (title with " \| ") preferred |
  | PCSX2 | `pcsx2-qt*.exe` | `-nofullscreen -nogui`; `-fullscreen`, `-bigpicture` dropped; heavy |
  | DuckStation | `duckstation-qt-*.exe` | `-nofullscreen -nogui`; `-fullscreen`, `-bigpicture` dropped |
  | RPCS3 | `rpcs3.exe` | `--fullscreen` dropped; heavy |
  | AppleWin | `AppleWin.exe` | `-f`, `-full-screen` dropped; `-no-full-screen` added |
  | Altirra | `Altirra64.exe`, `Altirra.exe`, `AltirraARM64.exe` | `/f` dropped |
  | sdl2trs | `sdl2trs*.exe` | `-fullscreen` dropped |

  Where the options come from:
  - Dolphin's, PCSX2 2.7's, DuckStation's and AppleWin 1.30's were read from their own help text and option tables in
    the user's builds. Only the binaries' text was read; none was started.
  - RetroArch's are its documented configuration keys.
  - RPCS3, Altirra and sdl2trs are not installed on the user's machine. Theirs come from their documentation, and
    their entries are not verified.
- **`retroverse-cli`:**
  - `launch --dry-run --vr` shows the request a launch in VR starts: the argv, the program, what it adds and drops, the
    load, and the append-file's path and lines. A dry run never writes the file.
  - `profiles` lists the programs, the one each system's launch starts, and the programs in use.
- **The fake emulator** gains `--config FILE`: it reads the file as RetroArch reads its append-file, logs its text, and
  takes its `color = RRGGBB` line as its window's colour.

## Tests

- **Unit** (`test_vr_launch.cpp`):
  - a program with every field, and its defaults;
  - each malformed program left out and named;
  - programs only in schema 2;
  - matching by file name;
  - the user's programs first;
  - the shipped programs found by the file names ES-DE's find rules give;
  - ES-DE 3.4.1's commands with and without VR for each program: RetroArch's `-L <core> %ROM%` (light and heavy, an
    existing `--appendconfig` joined), Dolphin's `-b -e`, PCSX2's and DuckStation's `-batch`, RPCS3's `--no-gui`,
    AppleWin's `-f -d1`, Altirra and sdl2trs unchanged, an unknown program as asked;
  - a request too long kept as asked;
  - the append-file's bytes, written once and not rewritten when unchanged;
  - an emulator's own read-only configuration left as it was.
- **Unit** (`test_cli_launch.cpp`): `launch --dry-run --vr` in JSON and text, nothing written, `--vr` refused outside a
  dry run; `profiles`' programs and `programProfile`.
- **GPU** (`--vr --xr-runtime fake`): the fake as a program of the user's `emulators.json`. In VR its own `--black` is
  dropped, its log is asked for, and Retroverse's append-file is passed with `--config`. The fake receives those
  arguments first, reads the file, and its window turns the file's green, which the CRT shows. The run report names the
  program, the arguments, the file and the heavy load.
- **Real library** (`test_cli_profiles_real.cpp`, `[real-library][programs]`, through `scripts/real-library.ps1`, 0
  differences in the snapshots) on 2026-11-07. Of the 195 systems:
  - **RetroArch:** 140, among them wii (Dolphin's core, so heavy) and apple2 (MAME's core);
  - **Dolphin:** gc;
  - **PCSX2:** ps2;
  - **15 systems through `cmd.exe`:** ps3, windows, steam, epic, ports, ... Step 13 turns those launches into
    shortcuts;
  - **34 with no emulator found**, among them atari800 (Altirra) and trs-80 (sdl2trs);
  - **4 with their folder unavailable.**

## Departures

- **Programs, not profiles, carry the VR settings** (ADR-0020 §1 first put a `vr` block on each profile). The same
  program is started by ES-DE commands and by profiles, and the shipped file had no profile for Dolphin, PCSX2 or the
  others. A profile for each would have offered launch choices nobody asked for. ADR-0020 §1 is rewritten.
- **The window rules sit on the program and apply on the desktop too**: they describe the program's windows, which the
  desktop's foreground hand-back follows as well.
- **One position for the added arguments**, right after the program: every program here reads its options before the
  game.
- **`config_save_on_exit = "false"` joins RetroArch's file.** RetroArch saves its whole configuration on exit by default
  (the user's has it on), which would copy the appended lines into the user's `retroarch.cfg`. During a game in VR,
  RetroArch's menu changes are therefore not saved on exit. Its "Save Current Configuration" still saves them, VR
  lines included: the user's own act.
- **Dolphin gets four options, not one**:
  - `RenderToMain=False` keeps its render window a window of its own while batch mode hides the main window;
  - `PauseOnFocusLost=False` and `BackgroundInput=True` mirror RetroArch's `pause_nonactive`.

  All four are for the run only: Dolphin never saves options given with `-C`.
- **PCSX2 and DuckStation get `-nogui`** besides `-nofullscreen`. Without it their game list shows beside the render
  window, and is often larger.
- **The append-file's path is passed as Retroverse spells it.** A program that reads its command line in the system's
  code page, as many do, cannot open a path holding characters outside it. The default data directory
  (`%LOCALAPPDATA%\Retroverse`) holds the user's name, which is ASCII on this machine. Not handled in this step.
