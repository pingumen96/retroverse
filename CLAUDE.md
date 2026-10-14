# Retroverse: guidance for Claude Code

Retroverse is a native Windows frontend (C++20, OpenGL 4.5 core, SDL3, no game engine) that turns an ES-DE game
collection into an explorable, procedurally generated 3D world. Authoritative design documents (Italian):

- `specification.md`: product, architecture, data model, milestones M0-M6, requirements FR/NFR.
- `style.md`: art bible (palette, lighting, prefab catalog, glTF conventions, asset licensing).
- `example.png`: approved visual concept.
- `docs/roadmap.md`: iteration plan and M0 step checklist. `docs/architecture.md`: modules and layering.
- `docs/decisions/ADR-*.md`: binding technical decisions (toolchain, coordinates/units/UV, ...).

## Build and test (PowerShell)

Every CMake/CTest/Ninja command must run inside the Visual Studio 2026 developer environment. `scripts/dev.ps1`
imports it for the current call (it also compacts the over-long machine PATH that breaks `VsDevCmd.bat`). Never
call `cmake`, `ctest` or `ninja` directly: the PATH contains an old CMake 3.26 and msys64 tools.

```powershell
./scripts/build.ps1                                   # configure on first use + build msvc-x64-debug
./scripts/build.ps1 -Preset msvc-x64-release          # other presets: msvc-x64-relwithdebinfo, msvc-x64-profile
./scripts/build.ps1 -Target rv_tests                  # single target
./scripts/test.ps1                                    # unit tests (label "unit"); GPU tests excluded
./scripts/test.ps1 -Filter aabb                       # ctest -R aabb
./scripts/test.ps1 -Preset msvc-x64-debug-gpu         # also GPU smoke tests (label "gpu")
./scripts/dev.ps1 cmake --preset msvc-x64-debug       # any raw command inside the dev environment
```

- Outputs: `build/<preset>/bin` holds `retroverse.exe`, `rv_tests.exe` and the vcpkg DLLs.
- vcpkg manifest mode, baseline pinned in `vcpkg.json`, packages installed in `build/vcpkg_installed`. The first
  configure in a fresh checkout or worktree restores packages from the local binary cache (a few minutes).
- Unit tests run with `ctest`; a single Catch2 test can also run directly:
  `build/msvc-x64-debug/bin/rv_tests.exe "core/aabb*"`.

## Running the app

`retroverse.exe --help` lists every option. Automated runs must always be frame-limited so no process is left
behind: `--smoke` (render `--frames N`, default 3, then exit; any high-severity GL debug message fails the run) or
`--exit-after-frames --frames N`. `--hidden`, `--frames` and `--screenshot` require one of them; frame-limited runs
never show message boxes. Exit codes: 0 ok, 1 smoke check failed, 2 initialization failure (window, context,
GL < 4.5, paths, scene file, input script), 3 runtime failure (unexpected error, `--screenshot` not written, scene
shader never built). Logs: `<user data>/logs/retroverse.log` (`--data-dir <dir>` keeps test runs out of
`%LOCALAPPDATA%`; use `--data-dir build/rvdata`).

- `--scene <name|file>`: a name loads `<asset root>/style_tests/reference_scenes/<name>.json` (default
  `m0_testroom`; also `material_test`, `instancing_stress`), anything with a path separator or `.json` is a file.
- `--screenshot <file.png>` writes the last frame of a frame-limited run; a run that ends before that frame (Esc,
  window closed) exits 3 like a failed write. `--screenshot-size WxH` (default 1920x1080, independent of the
  window) also applies to F12.
- Controls go through the action map (`assets/config/default_bindings.json`, overridden per action by
  `<user data>/config/bindings.json`): WASD/arrows walk (Left Shift sprints), the mouse looks around once captured,
  gamepad sticks walk and look. Interactive runs capture the mouse when the window has focus and on a click into it;
  frame-limited runs only on a click. Esc (gamepad B) releases the mouse, or quits when it is free. Alt+Enter toggles
  fullscreen, F5 reloads the shaders, F12 writes `<user data>/screenshots/retroverse-<timestamp>.png`. Developer
  builds also reload a shader within ~0.5 s of saving any of its files (`assets/shaders`); a broken edit logs
  `<file>:<line>: error ...` and the previous program stays in use.
- `--input-script <json>` replaces the actions of chosen frames (`rv/input/input_script.hpp`: an array of
  `{"fromFrame": a, "toFrame": b, "actions": {"move_forward": 1.0, "look_delta_x": 20.0}}`, frames counted from 0,
  both included) and logs the pose at the end of each step; `--fixed-dt <seconds>` replaces the measured frame time,
  so scripted runs are reproducible. Frame-limited runs log the final camera (`Final camera after N frame(s): feet
  ..., yaw ..., pitch ...`) and the nearest collider with its clearance.

## Visual verification

Rendering changes must be checked visually, not only compiled. The app renders offscreen and writes PNG files:

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 5 `
  --screenshot build/screenshots/m0_testroom.png --data-dir build/rvdata
```

Open the PNG with the Read tool and compare it against `style.md` and `example.png`. The PNG (RGB, no alpha) holds
sRGB-encoded values exactly as the window shows them (a `#383735` wall reads back as `#383735`), so pixels can also
be checked numerically. Check the log for GL debug messages (`GL debug output summary: 0 high, 0 medium, ...`).

Other viewpoints and movement are scripted: `tests/input_scripts/*.json` (each file names its expected result; the
unit tests in `tests/unit/integration/test_free_roam_navigation.cpp` play the same scripts):

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 120 --fixed-dt 0.016667 `
  --input-script tests/input_scripts/walk_forward_2s.json --screenshot build/screenshots/walk.png --data-dir build/rvdata
```

To check shader errors or hot reload, never edit the tracked shaders: copy `assets/` to a scratch directory, run a
developer build with `--asset-root <copy>` and a long frame limit (`--hidden --exit-after-frames --frames 3000`),
then edit or break the copy's shaders while it runs; the log shows each rebuild or `<file>:<line>: error ...`.

## Layout and layering

- `src/<module>/include/rv/<module>/*.hpp` public headers, `src/<module>/src/*.cpp`, `src/<module>/CMakeLists.txt`
  with an explicit source list. Include as `#include <rv/<module>/<file>.hpp>`.
- Dependency order: `core` -> `geometry` -> `input` -> `assets` -> `scene` (GPU-free; `rv_forbid_link` fails the
  configure if they reach SDL3, glad or ImGui) -> `platform` (SDL3) -> `render` (OpenGL through glad; never
  includes SDL) -> `devui` (Dear ImGui, developer builds) -> `app` (composition root) -> `retroverse.exe`.
- Tests: `tests/unit/<module>/test_*.cpp` (globbed into `rv_tests`, label `unit`, no SDL or OpenGL),
  `tests/platform/test_*.cpp` (globbed into `rv_platform_tests`, label `unit`: SDL platform layer, no OpenGL),
  `tests/support/*` helpers (globbed into `rv_test_support`), `tests/input_scripts` (scripted verification runs),
  `tests/smoke` GPU tests (label `gpu`).
- `third_party/glad` is generated and vendored (see its README); do not edit it by hand.

## Conventions

- C++20 with MSVC `/W4 /WX /permissive- /utf-8`. Fix warnings instead of silencing them.
- Namespaces `rv::<module>`; files `snake_case`; types `PascalCase`; functions and variables `camelCase`;
  private data members `m_name`; constants `kName`; `enum class` values `PascalCase`.
- Errors: return `rv::Expected<T>` / `rv::Status` (`rv/core/expected.hpp`). No exceptions across module
  boundaries. Bad or missing assets never crash the app: log, fall back, continue (spec §19, style §21).
- Logging: `spdlog::info/warn/error` on the default logger configured by `rv::core::initLogging`.
- Units and axes (ADR-0002): meters, +Y up, right-handed, +Z towards the viewer/front of a prefab, CCW front
  faces, glTF UV origin at the image top-left, images never flipped on load.
- Colors are authored as sRGB hex codes (style §3) and converted to linear for shading
  (`rv::core::hexToLinearRgb`). Shading happens in linear space; output through an sRGB framebuffer.
- Text and paths are UTF-8; convert with `rv::core::pathFromUtf8` / `rv::core::pathToUtf8`.
- OpenGL calls only in `rv_render`/`rv_devui`/`rv_app`, only on the main thread, DSA style (GL 4.5).
- Determinism: no `rand()`, no iteration over unordered containers into outputs, explicit seeds.
- Language: identifiers, comments, documentation and commit messages in English.

## Safety rules

- Never write into the user's ES-DE data directory, ROM root or media directory. Retroverse is read-only towards
  the collection by default (spec §4.6). Tests that read the real library must be tagged `[real-library]`.
- No brand logos, commercial artwork or trademarked names in `assets/` (style §1, §17.2). Every third-party asset
  needs provenance and license entries in `assets/registry/` before it is committed; `assets/staging/` is never
  committed or packaged.
- Changing the vcpkg baseline, the toolchain or a decision recorded in `docs/decisions/` requires a new ADR.
- Keep scope to the current milestone; park ideas in `BACKLOG.md`.

## Commits

Small focused commits, imperative subject line (`render: add shader hot reload`), body explaining why when not
obvious. Run the unit tests before committing. Follow the commit trailer instructions given by the session.
