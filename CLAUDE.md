# Retroverse: guidance for Claude Code

Retroverse is a native Windows frontend (C++20, OpenGL 4.5 core, SDL3, no game engine) that turns an ES-DE game
collection into an explorable, procedurally generated 3D world. Design documents (Italian): `specification.md`
(product, architecture, milestones, requirements), `style.md` (art bible), `example.png` (the approved concept).
Engineering documents: `docs/roadmap.md` (iterations, the working method, the acceptable performance),
`docs/architecture.md`, `docs/decisions/ADR-*.md` (binding decisions), `docs/i2/`, `docs/i3/` (what each step built
and decided), `docs/testing/` (the gates), `BACKLOG.md`.

The details this file leaves out are in `docs/reference/`: `build-and-test.md` (presets, test executables, the GPU
smoke tests, the gates, the real-library tests), `running-the-app.md` (every option, the run report, the room, the
controls, the quick catalog), `visual-verification.md`, `asset-preview.md`, `cli.md` (`retroverse-cli`),
`layout.md` (modules and what each holds), `conventions.md`, `safety.md`, `spikes.md` (the I1 spikes). Read the one a
task needs before working on it.

## Build and test (PowerShell)

Every CMake/CTest/Ninja command runs inside the Visual Studio 2026 developer environment: use the scripts, or
`./scripts/dev.ps1 <command>`. Never call `cmake`, `ctest` or `ninja` directly (the PATH holds an old CMake and msys64
tools).

```powershell
./scripts/build.ps1                                   # msvc-x64-debug; -Preset msvc-x64-release | -relwithdebinfo
./scripts/build.ps1 -Target rv_tests                  # one target
./scripts/test.ps1                                    # unit tests (label "unit")
./scripts/test.ps1 -Filter aabb                       # ctest -R aabb
./scripts/test.ps1 -Preset msvc-x64-debug-gpu         # also the GPU smoke tests (label "gpu")
./scripts/gate.ps1 -Gate B                            # the latest gate: build matrix, tests, goldens, benchmark path
./scripts/real-library.ps1 -Filter '[real-library][room-real]'   # the real library, ES-DE closed (see Safety)
```

- Outputs in `build/<preset>/bin`: `retroverse.exe`, `retroverse-cli.exe`, `retroverse-asset-preview.exe` (developer
  builds), `retroverse-screenshot-diff.exe`, `rv_tests.exe` (unit), `rv_smoke_tests.exe` (GPU smoke tests, which start
  the build's executables hidden and read their run reports and screenshots), `rv_fake_emulator.exe`.
- One Catch2 test: `build/msvc-x64-debug/bin/rv_tests.exe "core/aabb*"`. Hidden cases: `[.bench]` (benchmarks, by hand
  in Release, informative), `[.real-library]` (only through `scripts/real-library.ps1`), `[.desktop]` (take over the
  screen; only `./scripts/test.ps1 -Preset msvc-x64-debug-desktop`).
- Golden files (plans, scenes, selections, screenshots) are compared exactly and rewritten only with
  `RV_UPDATE_GOLDEN=1` (the test then fails, so a rewrite never passes unnoticed).
- The first configure of a fresh worktree restores vcpkg packages from the local binary cache (a few minutes).

## Running the app

`retroverse.exe --help` lists every option. Automated runs are always frame-limited (`--smoke` or `--exit-after-frames
--frames N`), `--hidden`, with `--data-dir build/rvdata` (or a folder under `build/`); `--fixed-dt 0.016667` and
`--input-script <json>` make scripted runs reproducible; `--run-report <file.json>` writes what scripts and tests read
(read it instead of the log). Exit codes: 0 ok, 1 smoke check failed, 2 initialization failure, 3 runtime failure.

- Without `--scene`, `--model` or `--mock-catalog` the app opens the generated room of its data directory's ES-DE
  catalog (made with `retroverse-cli --data-dir <dir> library create ...` and `import`); `--scene m0_testroom` shows
  the test room; `--room-system <system>` shows another system's room (its media family). Fixtures for rooms:
  `tests/fixtures/esde/snes-room`, `snes-single`, `snes-stress`, and `building` (I4: every kind of system).
- Anything that may launch a game runs with `RV_LAUNCH_ALLOWLIST` naming the build's `rv_fake_emulator.exe` (Safety).

## Visual verification

Rendering changes are checked visually, not only compiled:

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 5 --hidden `
  --screenshot build/screenshots/m0_testroom.png --data-dir build/rvdata
```

Open the PNG with the Read tool and compare it with `style.md` and `example.png`; check the log for GL debug messages.
`retroverse-screenshot-diff <expected.png> <actual.png> [--heatmap <png>]` compares two images with the goldens'
tolerance (6/255, at most 0.5% of the pixels). Goldens live in `assets/style_tests/screenshots/` with a JSON naming
the command that made them; regenerate one only when the look changes on purpose. Viewpoints, scripts and the room's
screenshots: `docs/reference/visual-verification.md`.

## Layout and layering

- `src/<module>/include/rv/<module>/*.hpp` public headers, `src/<module>/src/*.cpp`, an explicit source list in
  `src/<module>/CMakeLists.txt`; include as `#include <rv/<module>/<file>.hpp>`.
- Dependency order: `core` -> `geometry` -> `input` -> `assets` -> `scene`, `media` (FFmpeg only inside),
  `launcher` (GPU-free) -> `catalog` (domain and ports) -> `esde` (pugixml private), `catalog_db` (SQLite private),
  `world` (the generator, GPU-free), `platform_win` -> `platform` (SDL3) -> `render` (OpenGL through glad, no SDL)
  -> `devui` (Dear ImGui, developer builds), `ui` (RmlUi) -> `app` (composition root) -> `retroverse.exe`. Forbidden
  links are enforced at configure time (`cmake/LayerRules.cmake`, `rv_forbid_link`); GPU-free parts of the app live in
  `rv_app_cli` and `rv_app_catalog`. What each module holds: `docs/reference/layout.md`.
- Tools: `tools/cli`, `tools/asset-preview`, `tools/screenshot-diff`, each with a GPU-free core tested by `rv_tests`.
- Tests: `tests/unit/<module>/test_*.cpp` (no SDL or OpenGL), `tests/platform`, `tests/smoke/*.cpp` (GPU), helpers in
  `tests/support` (`TempDir`, `Fixture`, `runChild`, `FakeSystemProbe`, ...), fixtures in `tests/fixtures/<name>/`
  (original and minimal, payloads generated at test time, never a real title, cover or trademark), input scripts in
  `tests/input_scripts`.
- Configuration: shipped defaults under `assets/` (JSON with comments), the user's overrides under
  `<user data>/config`, which Retroverse reads and never writes (`library.json` is written only by `retroverse-cli
  library create`). `third_party/glad` is generated: never edit it by hand.

## Conventions

- C++20, MSVC `/W4 /WX /permissive- /utf-8`: fix warnings, never silence them. Namespaces `rv::<module>`; files
  `snake_case`; types `PascalCase`; functions and variables `camelCase`; members `m_name`; constants `kName`.
- Errors as `rv::Expected<T>` / `rv::Status`, no exceptions across modules. Bad or missing assets or media never crash:
  log, fall back, continue. Logging with `spdlog` on the default logger.
- Units: meters, +Y up, right-handed, +Z towards the front, CCW front faces, glTF UV origin top-left (ADR-0002). Colors
  authored as sRGB hex, shaded in linear space.
- Text and paths are UTF-8; names from a file system or an untrusted file go through `rv/core/utf8.hpp`.
- OpenGL only in `rv_render`, `rv_devui`, `rv_ui`, `rv_app`, on the main thread, DSA style.
- Determinism: no `rand()`, no unordered iteration into outputs, seeds through `core::deriveSeed` and `core::Random`;
  the world's arithmetic is integer. Golden seeds, GameIds, quick hashes and title keys come from the Python oracles in
  `scripts/` (`*_oracle.py --check ...`): a rule change changes both.
- World plans are written only through `world::savePlan()` under the world lock, never edited by hand; an update never
  moves a placement. A GameId is derived once and then persisted.
- The catalog database: only `database.cpp` includes `sqlite3.h`; one writer thread, read-only readers; writes of
  identities, aliases or index rows hold the import lock. ES-DE's scraper credentials are never read, logged or stored.
- Product UI strings live in the RML documents under `assets/ui` (Italian); bundled fonts only.
- Identifiers, comments, documentation and commit messages in English. More: `docs/reference/conventions.md`.

## Safety rules

- Never write into the user's ES-DE data directory, ROM root or media directory: Retroverse is read-only towards the
  collection. Tests that read the real library are `[.real-library]`, take its folders from the `RV_REAL_*` variables
  and run only through `./scripts/real-library.ps1` (ES-DE closed, read-only snapshots before and after, 0
  differences). Product code and new tests never name this machine's library paths.
- Anything automated that may start an emulator sets `RV_LAUNCH_ALLOWLIST` (absolute paths, `;`-separated; the build's
  `rv_fake_emulator.exe`). DuckStation never starts; RetroArch only with an isolated configuration under `build/`.
  Product code starts processes only through `rv_launcher` / `Win32ProcessRunner`: argv, never a shell string, and it
  never terminates an emulator.
- Automated tests never take the Windows foreground: no visible or activated window, no console window, no injected
  input, no modal dialog (`RV_LAUNCH_BACKGROUND=1`, apps `--hidden`, SDL windows hidden). Only `desktop` tests may.
  Check with `python -I scripts/focus_watch.py --watch build/<preset> --out build/focus/<name>.json -- pwsh -NoProfile
  -File scripts/test.ps1`.
- No brand logos, commercial artwork or trademarked names in `assets/`; third-party assets need provenance and license
  entries in `assets/registry/`; `assets/staging/` is never committed. The user's covers and clips are read at runtime
  only, never copied into assets, fixtures or goldens.
- Changing the vcpkg baseline, the toolchain or a recorded decision requires a new ADR. Keep to the current milestone;
  park ideas in `BACKLOG.md`. Details: `docs/reference/safety.md`.

## Working method

Follow `docs/roadmap.md`, "Working method (from I4)" and "Acceptable performance": steps that deliver something to see
or try, short step notes, performance checked against acceptable limits only (never per-pass budgets), few goldens,
the real library when a step touches it, gates as one script run that the user then tries.

## Commits

Small focused commits, imperative subject line (`render: add shader hot reload`), body explaining why when not
obvious. Run the unit tests before committing. Follow the commit trailer instructions given by the session.
