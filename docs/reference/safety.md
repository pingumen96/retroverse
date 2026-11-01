# Safety rules

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

- Never write into the user's ES-DE data directory, ROM root or media directory. Retroverse is read-only towards
  the collection by default (spec §4.6). Tests that read the real library carry the hidden tag `[.real-library]`, take
  its paths from the `RV_REAL_*` variables and run only through `./scripts/real-library.ps1` (ES-DE closed, read-only
  snapshots before and after, 0 differences). Product code never names this machine's library paths (they come from
  ES-DE's configuration, discovery or the user), and no test but those reads them (strings shaped like them, such as
  quoting cases, are only data; new tests use invented paths).
- Anything automated that may start an emulator (harnesses, agents) runs with `RV_LAUNCH_ALLOWLIST` set (absolute
  executable paths separated by `;`; set but empty allows nothing). `Win32ProcessRunner::start` then refuses, before
  creating anything, every executable the list does not name (matched by final path and file id;
  `rv/launcher/launch_guard.hpp`); any other code that starts a `LaunchRequest` calls `launcher::checkLaunchGuard()`
  first. Unset, the guard does nothing: `scripts/gate.ps1` and `scripts/test.ps1` do not set it yet (BACKLOG), but every
  GPU smoke test case (`rv_smoke_tests`) has it, naming the build's fake emulator alone (ctest `ENVIRONMENT`). A run
  that sets it lists `build/<preset>/bin/rv_fake_emulator.exe`, which the runner, launcher, launch smoke and I1 spike
  tests start (the `retroverse-cli launch` tests set it for their own duration, `ScopedLaunchAllowList`); a
  `retroverse.exe --mock-catalog` or `retroverse-cli launch --game` run made by hand or by an agent sets it the same way
  (a real emulator only isolated, as below). DuckStation
  (`duckstation*.exe`) never starts, listed or not; RetroArch (`retroarch*.exe`) only isolated, every `--appendconfig`
  naming an existing ASCII file (the isolated configuration under `build/`, as `spike_launch` writes it). The guard is a
  safety net, not a security boundary: a hard link or copy under another name is kept out by the list alone.
- A launch test in the product (ADR-0018 §6) is a real launch and fails closed: an unattended run (frame-limited, or
  `RV_LAUNCH_BACKGROUND` set) refuses it while `RV_LAUNCH_ALLOWLIST` is unset. `[real-library]` runs resolve launches and
  never start one.
- A frame-limited run never discovers ES-DE by itself (ADR-0018 §4): without `library.json` it keeps the test room and
  its notice, and the wizard runs only with `--wizard --esde-home <dir>`, a fixture's in tests, so that no test reads the
  machine's library through discovery. The file watcher (ADR-0018 §10) only reads, is off in automated runs unless
  `--watch-library` asks, and its tests add files under `build/` only, never to the real folders.
- Automated tests never take the Windows foreground: no console window, no window shown on a monitor or activated, no
  foreground granted, no input injected, no modal dialog, while someone works at the machine. Only the tests labelled
  `desktop` (on demand, never the gate) may take over the screen. `RV_LAUNCH_BACKGROUND=1` (set, and neither empty nor
  `0`) makes `Win32ProcessRunner` start every program in the background (console programs with `CREATE_NO_WINDOW`, GUI
  programs inactive and off-screen through `STARTF_USEPOSITION` and `SW_SHOWNOACTIVATE`, no
  `AllowSetForegroundWindow`, an inherited error mode without dialogs) and `rv_fake_emulator` keep its window inactive
  and off-screen and skip its foreground and input options; the unit and gpu test presets, `rv_tests`, `rv_smoke_tests`
  (unless the variable is set) and `scripts/real-library.ps1` set it, the desktop preset and registrations set `0`.
  `rv::platform_win::disableFailureDialogs()` (no loader, crash, C runtime assertion or `abort()` box; reports go to
  standard error) runs in `rv_tests`, `rv_smoke_tests` and the fake always, and in `retroverse.exe`, the asset preview,
  `spike_ui` and `spike_video` when they run unattended (frame-limited, `--no-dialogs`, or the variable set), in the CLI
  and `spike_launch` when the variable is set. A new test that starts a program starts it through the runner or
  `runChild` (`CREATE_NO_WINDOW`), runs apps `--hidden`, and creates SDL windows with `SDL_WINDOW_HIDDEN`; one that must
  show or focus a window is a desktop test. Check a change with `python -I scripts/focus_watch.py --watch build/<preset>
  --out build/focus/<name>.json -- pwsh -NoProfile -File scripts/test.ps1 [-Preset <preset>]` (exit 1 on a foreground
  change to, or a visible window of, a process of the build; read-only).
- No brand logos, commercial artwork or trademarked names in `assets/` (style §1, §17.2). Every third-party asset
  needs provenance and license entries in `assets/registry/` before it is committed; `assets/staging/` is never
  committed or packaged. The user's covers and clips are read at runtime only, never copied into assets, fixtures or
  goldens: tests generate theirs with invented titles (`fixture_payloads.hpp`), and the real room is checked through
  `scripts/real-library.ps1` with its outputs under `build/`.
- Changing the vcpkg baseline, the toolchain or a decision recorded in `docs/decisions/` requires a new ADR.
- Keep scope to the current milestone; park ideas in `BACKLOG.md`.

