# Build and test

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

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
./scripts/test.ps1 -Preset msvc-x64-debug-desktop     # on demand: tests that take over the screen (label "desktop")
./scripts/test.ps1 -Preset msvc-x64-release-gpu       # the same for Release (rendering from its deployed assets)
./scripts/gate.ps1 -Gate A                            # Gate A end to end: build matrix, tests, golden G01; -Clean
./scripts/gate.ps1 -Gate M1                           # Gate M1: Gate A + the catalog's checks, under focus_watch.py
./scripts/gate.ps1 -Gate M1 -RealLibrary              # also the real library (ES-DE closed): NFR-01, NFR-02, imports
./scripts/gate.ps1 -Gate B                            # Gate B: Gate A + every golden, the I3 checks, the benchmark path
./scripts/gate.ps1 -Gate B -RealLibrary               # also the real SNES room (ES-DE closed), acceptable limits
./scripts/gate.ps1 -Gate B -Benchmarks                # also every hidden benchmark, informative
./scripts/real-library.ps1                            # the [real-library] tests, ES-DE closed, snapshots before/after
./scripts/dev.ps1 cmake --preset msvc-x64-debug       # any raw command inside the dev environment
```

- Outputs: `build/<preset>/bin` holds `retroverse.exe`, `retroverse-asset-preview.exe` (developer builds only, see
  "Asset preview"), `retroverse-screenshot-diff.exe`, `retroverse-cli.exe` (the ES-DE catalog from the command line,
  see "retroverse-cli"), `rv_tests.exe`,
  `rv_platform_tests.exe`, `rv_smoke_tests.exe`, `rv_fake_emulator.exe` (the stand-in emulator the launch tests start,
  `tests/fake_emulator`: exit codes, crashes, hangs, wrappers, windows, `--out`/`--err` text, `--exit-timeout-ms` so
  that an abandoned copy ends by itself, `--abort`; never a failure dialog; with `RV_LAUNCH_BACKGROUND` set, an inactive
  off-screen window and no foreground or input options; `--help` lists its options), `rv_collection_probe.exe` (the
  GPU smoke tests' driver of the collection's texture arrays and static instance groups, `tests/collection_probe`:
  `--probe-scenario chunks|growth|groups`, `--probe-view lit|base-color`, `--probe-path static|dynamic`,
  `--probe-camera wall|close|moving`, `--probe-highlight`, `--probe-hide`, then `retroverse.exe`'s options; it logs to
  `collection-probe.log`), `rv_ui_probe.exe` (I4 step 7a: the product UI's screens from fixed models for the GPU smoke
  tests, `tests/ui_probe`: the directory over the test room from `directory_fixture.hpp`'s model, driven by
  `--input-script`; `--probe-shots <dir>` writes its checkpoints' screenshots, `--probe-report <json>` its checkpoints,
  commands and RmlUi's log counts, then `retroverse.exe`'s options; it logs to `ui-probe.log`), the I1 spike executables
  (`spike_video.exe`, `spike_ui.exe`, `spike_launch.exe`, `spike_launch_tests.exe`) and the vcpkg DLLs (FFmpeg and RmlUi
  included: every preset enables the vcpkg features `video` and `ui`; `pugixml.dll` and `sqlite3.dll`).
  Release (`RV_DEV_ASSET_ROOT` off) also holds `assets/`, which every build mirrors from the source tree (target
  `rv_runtime_assets`, `cmake/CopyAssets.cmake`: changed files copied, deleted ones removed, `staging` and the golden
  screenshots left out), so an edit of the assets alone reaches the next Release build.
- GPU smoke tests (`tests/smoke`, label `gpu`, ctest names `gpu.<case>`): each case starts the executables of its
  build frame-limited with `--hidden`, `--data-dir` and `--run-report` under `build/<preset>/smoke/<case>` (logs,
  reports, screenshots, heatmaps stay there) and checks the reports and screenshots: the reference scenes with frame
  statistics bounds, `instancing_growth` growing mid-run, the three self-checks, `--gl-selftest` (its provoked error in
  the report, the run clean), the key light's shadow and the ambient occlusion on `tests/smoke/scenes` (with and
  without the effect, compared at projected probe points), the developer overlay and the asset preview (developer
  builds), golden G01 (reference machine only), and launches from a mock catalog into the fake emulator
  (`test_launch.cpp`, `tests/smoke/catalogs`: a return with no frame rendered meanwhile, nine failure paths, the
  game-session panel's questions, the wait limit, the window closed mid-game), and since I2 step 13 the ES-DE catalog
  in the app (`test_catalog.cpp`, `gpu.catalog: ...`: the standard fixture imported with `retroverse-cli` into the
  case's data directory, a game found by search and filter launched into the fake and back with the same selection,
  search, filter and camera, a blocked game's readiness and Avvia refused, a blocked game's card whole, the notice of a
  data directory without `library.json`); since I3 the measurements (`gpu.measurements/...`), the collection's texture
  arrays and static instance groups through `rv_collection_probe` (`gpu.collection arrays/...`: 2,049 items in two
  arrays per class, arrays grown mid-run, BC1 and the RGBA8 fallback; `gpu.static groups/...`: 2,000 items, the static
  path equal to the dynamic one to the bit, the hover outline), the hero CRT's video (`gpu.video: ...`), the
  catalog's refresh (`gpu.refresh: ...`) and the generated room in `retroverse.exe`, each case on a fixture recorded as
  the case's library and imported with `retroverse-cli`: the room (`gpu.world: ...` of `test_world_room.cpp`, the
  persisted start of `test_world_start.cpp`, `[world_start]`, and the live update after a refresh of
  `test_world_live_update.cpp`, `[world_live]`), its covers (`gpu.covers: ...`), hover, pull-out and Focus (`gpu.world
  focus: ...`), inspection (`gpu.world inspect: ...`), the card (`gpu.world card: ...`), the media viewer and
  Informazioni (`gpu.media viewer: ...`), launching from the room and "Mostra nella stanza" (`gpu.world launch: ...`),
  the look's probes (`gpu.world look: ...`), the media families' rooms of the `esde/building` fixture through
  `--start-room` (`gpu.world families: ...`, I4 step 2), the floors of the `esde/building` fixture (`gpu.world
  floor: ...`, I4 step 3: `tests/smoke/scenes/two_areas.json` from each area and without areas, each area's lights and
  its key light's shadow; gen4's walk from the elevator into room:snes; the ground floor's lobby), the floor in the app
  (`gpu.world floor app: ...`, I4 step 5a, `test_world_floor_app.cpp`, `[world_floor_app]`: two rooms of one family on
  a floor, each sampling its own arrays and showing its own covers, the residency per room and the floor's GPU memory
  within 512 MiB, the cover loader's first view without the items of a room whose doorway it does not see; a walk from
  the hall into one room, out and into the other, a cover of the second inspected; the start in the lobby without
  `--start-room`, and a `--start-room` the plan lacks refused with exit 2; since I4 step 6b the hero CRT rebound on
  that walk (`video.binds`, the first room's clip stopped, `waitsBeforeExit` 0) and on a floor that runs freely, the
  walker backing from the first room's playing clip into the second (the `screenBind` phase's frames within the
  loading row in Release), from the lobby into the arcade room (its CRT the run's first), the quick catalog's
  selection played only in its game's room, the handheld floor previewed and reconciled room by room with nothing
  moved or uploaded again (also after an import that changed nothing), then rebuilt live around the fixture's
  `additions/` after "Aggiorna libreria" with room:gb's pictures taken over and the walker, who walked first, where
  the same walk ends without them, gen4 rebuilt as megadrive enters its new shared room with the walker kept in the
  hall's frame, and gen5's new titles, only saved meanwhile, shown at its next start; the rooms, the doorways, the
  hall and the arrival point are read from the generated plan and its floors' layout, never named), the room goldens
  (`gpu.golden/G02` ... `G10`, `[golden_room]`, see "Visual verification"), and the directory through `rv_ui_probe`
  (`gpu.directory: ...`, I4 step 7a: `tests/input_scripts/directory_panel.json` over a fixed model, each checkpoint's
  view and RmlUi focus, the focus's edge in its screenshot, Mostra nella stanza's command), and the elevator and the
  directory in the app (`gpu.world elevator: ...`, I4 step 7b, `test_world_elevator.cpp`, `[world_elevator]`, on
  `esde/building` under the shipped threshold, lockstep: a ride from the ground floor to gen4 and back through the
  elevator's prompt (the walker turned to its doors) and the directory, the walker at each arrival point facing the
  hall, `world.rides` with their frames (the swap in the frame after the fade), the GPU memory a floor holds after each
  ride a direct run's of that floor (arrays, buffers, scene textures, procedural surfaces), in Release the first ride
  within 10 s and the second within 3 s; a room of the handheld floor chosen with Map (the ride, its doorway, its arrays
  not the lobby's), the floor's other room teleported to, then the floor's own hall (back at its elevator); the quick
  catalog's "Mostra nella stanza" of a gen4 game from the lobby ending in its Focus pose; the favourites and recent
  games tab against the fixture's gamelists and a card's room key; Avvia on the directory's card into the fake and
  back, and one that cannot start with its outcome on the card; interact and Map in one frame inspecting an item and
  opening nothing, the directory opened once back in free roam; the fade's view against the view before it, each
  value times the fade's share; a game launched from the lobby starting its room's CRT on another floor after a ride);
  the room's test files
  repeat a few helpers each (BACKLOG). Their hidden `[.bench]` cases run by hand in Release,
  `build/msvc-x64-release/bin/rv_smoke_tests.exe "[bench]"`: the snes-stress room's figures, its covers cold and warm,
  the animations' frame budget and the benchmark path (`[benchmark]`,
  `tests/input_scripts/bench_snes_room.json`: 1,230 frames at 1920 x 1080 with `--no-vsync` and `--fixed-dt 0.016667`,
  frame p99 at most 33.3 ms and p50 at most 16.7 ms in Release, the acceptable limits of `docs/roadmap.md`; the GPU
  passes printed). Every clean run is also
  checked for the run report's I3 sections (the GPU memory adding up, at most one decoder thread, joined at exit).
  Every case of `rv_smoke_tests` (the gpu cases and the desktop one) runs with `RV_LAUNCH_ALLOWLIST` naming the build's
  `rv_fake_emulator.exe` (ctest `ENVIRONMENT`; the launch tests set it themselves when run outside ctest). A case is
  skipped (exit code 77) when no OpenGL 4.5 context can be created, and the golden comparison on another GPU or
  driver. One case:
  `build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden*"` (Catch2 test names), or `./scripts/test.ps1 -Preset
  msvc-x64-debug-gpu -Filter golden` (`-Filter gpu.launch` for the launches: a bare `launch` also matches unit tests).
  The visible launch, `desktop.smoke.launch_visible` (Catch2 tag `[.desktop]`), takes over the screen and runs only
  through ctest, `./scripts/test.ps1 -Preset msvc-x64-debug-desktop -Filter launch_visible` (a bare `launch` also runs
  every `desktop.launch.*` spike case), whose registration sets `RV_SMOKE_DESKTOP=1`; run any other way it skips.
- `python -I scripts/focus_watch.py --watch <dir> --out <json> -- <command...>` (or `--duration <s>`; read-only)
  records foreground changes and new windows while a command runs and fails (exit 1) when a process under `<dir>` (or
  its descendants, or the terminal window of their console) takes the foreground or shows a window where the user sees
  it, at any time while the window is visible, or when a system error dialog (WerFault for a test process, a csrss
  hard-error box) appears (`docs/i2/step-focus.md`).
- `scripts/gate.ps1 -Gate A` builds `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` (a compiler,
  linker or CMake warning fails the step; `-Clean` rebuilds every target), runs the unit tests and the GPU checks (the
  smoke tests and the I1 spike checks) of Debug and Release and the golden comparison of both, prints a summary table
  (logs in `build/gate-a`) and exits 1
  when a step failed (skipped steps are reported, not failed).
- `scripts/gate.ps1 -Gate M1` (`docs/testing/gate-m1.md`, logs in `build/gate-m1`) runs Gate A's steps and, per Debug
  and Release, rows for the M1 unit tests and the `gpu.catalog`/`gpu.launch` cases (counted from those runs) and the
  unit test `cli/process` (`tests/unit/cli/test_cli_m1_flow.cpp`: `retroverse-cli` as processes on the standard
  fixture, from `library create` to `launch --game --wait` into the fake and `sessions`, the fixture unchanged), the
  whole gate under `scripts/focus_watch.py --watch build` (a finding fails it; its row names the rule that matched).
  `-RealLibrary` (with `-EsdeHome`, `-EsdeInstall`, `-RomRoot`, `-MediaRoot` or the `RV_REAL_*` variables; refused while
  ES-DE runs) adds the NFR-02 benchmark, then `scripts/real-library.ps1 -Measure scripts/gate-m1-real.ps1`: between
  its snapshots (0 differences required) the ROM scan, the two imports (the second writes nothing), `db
  rebuild-index`, `db check`, `media --coverage`, NFR-01 with `retroverse.exe` (three starts, limit 5 s) into
  `build/gate-m1/real-measurements` (`figures.json`), then the `[real-library]` tests of Release; each figure a row with
  its limit. `-FullHashPass` adds the whole library's quick-hash pass (`identities hash`, about 45 minutes cold).
- `scripts/gate.ps1 -Gate B` (`docs/testing/gate-b.md`, logs in `build/gate-b`) runs Gate A's steps with every golden
  (G01 and the room goldens; Gates A and M1 run `gpu.golden/G01` alone), a row per golden and preset with the verdict
  its case writes next to its screenshot (`build/<preset>/smoke/golden_g01/comparison.txt`,
  `smoke/golden_room/<case>/comparison.txt`), rows for the I3 unit tests and GPU cases (counted from those runs), then
  in Release the benchmark path (`rv_smoke_tests "[benchmark]"`): its frame times and GPU memory read from its run
  report against the roadmap's acceptable limits, its GPU passes as an informative row; `-Benchmarks` also runs every
  hidden benchmark, `rv_tests "[bench]"` and `rv_smoke_tests "[bench]"`, as informative rows (a row per GPU case with
  the figures it printed; their XML reports in `build/gate-b`); the whole gate under `scripts/focus_watch.py`.
  `-RealLibrary` adds `scripts/real-library.ps1 -Filter '[real-library][room-real]'` (snapshots before and after, 0
  differences) and reads the test's run reports (`build/msvc-x64-release/real-library/room/<run>.json`): the plan
  generated twice, the restart after a second import (no rebuild, 0 moves), the time to the room (the restart within 5
  s), the covers resident cold (30 s) and warm (3 s), the frames while they load (p99 50 ms), the benchmark path's
  acceptable limits, the refresh of the whole library (p99 50 ms, 0 moves) and NFR-01 with the room.
- vcpkg manifest mode, baseline pinned in `vcpkg.json`, packages installed in `build/vcpkg_installed`, built by the
  vcpkg clone that `VCPKG_ROOT` names (`E:/Progetti/vcpkg`, shared with other projects; ADR-0007): `scripts/dev.ps1`
  restores the variable after the developer environment points it at Visual Studio's bundled copy. The first configure
  in a fresh checkout or worktree restores packages from the local binary cache (a few minutes). A tree configured with
  another toolchain keeps it until `./scripts/build.ps1 -Preset <preset> -Fresh`.
- Unit tests run with `ctest`; a single Catch2 test can also run directly:
  `build/msvc-x64-debug/bin/rv_tests.exe "core/aabb*"` (`rv_tests` runs its launches in the background, see "Safety
  rules"). The catalog query benchmarks (I2 step 9: NFR-02 on 10,000 games, a UI page on 100,000, what the search area
  costs the writes) are hidden (`[.bench]`) and run by hand in Release: `build/msvc-x64-release/bin/rv_tests.exe
  "[bench]"` (informative unless the machine is quiet); so are I3's budgets (the room query, generation, validation,
  the update, picking, the plan's scene, `[bench][covers]`, the static groups' planning, `snes-stress` materialized)
  and I4 step 6b's `[world_build][bench]` (the building job on 90,000 generated entries: generated, unchanged through
  the whole job, unchanged through the building stamp).
  `rv_tests.exe "[label-preview]"` writes sample spines, labels and fitted covers to `build/<preset>/label-preview` for
  a visual review (since I4 step 2 also `shape_<family>_*.png`: covers and generated labels made for a jewel case's,
  a keep case's and a big box's label quad, the stored layer beside the same layer stretched back to its quad). Golden files are compared byte for byte and rewritten only with `RV_UPDATE_GOLDEN=1` (the test then
  fails, so a rewrite never passes unnoticed): selections in `tests/fixtures/world/selection`, plans and update
  sequences in `tests/fixtures/world/golden`, the plans' scenes and collections in `tests/fixtures/world/scene`;
  `RV_WORLD_SCENE_OUT=<folder>` makes `rv_tests "cli/worldplan show --scene: the snes-room*"` leave the `snes-room`
  fixture's plan and scene file there.
- Tests that read the real ES-DE library carry the hidden Catch2 tag `[.real-library]` (ctest never runs them) and take
  its folders from `RV_REAL_ESDE_HOME`, `RV_REAL_ESDE_INSTALL`, `RV_REAL_ROM_ROOT`, `RV_REAL_MEDIA_ROOT` and, optional,
  `RV_REAL_RETROARCH_EXE`. Run them only through `./scripts/real-library.ps1` (parameters `-EsdeHome`, `-EsdeInstall`,
  `-RomRoot`, `-MediaRoot`, `-RetroArchExe` or those variables; `-Preset`, default `msvc-x64-release`; `-Filter`): it
  refuses to run while ES-DE runs, snapshots the home and install (configuration files hashed) and the ROM and media
  roots (listings only) with `scripts/snapshot_tree.py`, runs `rv_tests.exe "[real-library]"` (with
  `RV_LAUNCH_BACKGROUND=1` and `RV_LAUNCH_ALLOWLIST` naming the build's fake emulator alone), snapshots again and
  fails on a failed test or any difference (outputs in `build/real-library`). Narrower runs: `-Filter
  '[real-library][media]'` (the media tree and the coverage against ADR-0003), `-Filter '[real-library][launch]'` (the
  readiness of the real systems and the five launches ES-DE logged, read from the git-ignored, machine-local
  `tests/real_library/local/esde_logged_launches.json`; these two tests are
  skipped without `-RetroArchExe`), `-Filter '[real-library][import]'` (the whole library imported into a new database
  under `build/<preset>/real-library/import`: a first and a second import, `db rebuild-index`, the ROM root unplugged
  and back), `-Filter '[real-library][reconciliation]'` (gb imported alone, then the quick-hash pass over it, twice;
  cold it reads for minutes: the whole library's cold pass, about 45 minutes, is not part of any filter, only of
  `gate.ps1 -Gate M1 -RealLibrary -FullHashPass`), `-Filter '[real-library][app]'` (NFR-01: the library imported under
  `build/<preset>/real-library/app`, then `retroverse.exe` started twice on it, the first page timed from the process's
  creation, and scrolled through every game) and `-Filter '[real-library][query]'` (NFR-02 on the real catalog: the
  benchmark's kinds of search with every order and a filter, p95 within 100 ms, pages within 5 ms); since I3 `-Filter
  '[real-library][room_selection]'` (snes imported alone under `build/<preset>/real-library/room-selection`, its room
  selection printed), `-Filter '[real-library][covers]'` (a sample of the real SNES covers through the cover
  pipeline, its cache in `build/<preset>/real-library-covers`), `-Filter '[real-library][world]'` (the real room in
  `retroverse.exe`, its data directory under `build/<preset>/real-library/world`), `-Filter
  '[real-library][covers-room]'` (its covers cold and warm, close-ups under `build/<preset>/real-library/covers`) and
  `-Filter '[real-library][room-real]'` (Gate B's: snes imported alone under `build/<preset>/real-library/room`, the
  plan generated twice with one SHA-256, a restart after a second import moving nothing, the covers cold and warm, the
  benchmark path, four screenshots, a refresh of the whole library with the quick catalog open; the runs' reports and
  screenshots stay there, never committed: the user's covers); since I4 step 6b `-Filter '[real-library][building]'`
  (the whole library imported under `build/<preset>/real-library/building`, then `retroverse.exe`: the first
  building, the unchanged start as the user starts it with NFR-01 to the lobby and the quick catalog's first page, the
  whole building job with its stamp removed, a start in the computer floor's largest room within 512 MiB cold (its
  covers derived in that run) then warm, since I4 step 7b, the derived covers emptied, a ride from the lobby to that
  room through the elevator and back to the lobby's elevator with Map as the user rides (no lockstep), cold then warm,
  within the acceptable row (10 s cold, 3 s warm) and with the destination's arrays, and `worldplan update`; the
  figures printed as "Real building:" lines). `-OutDir <dir>`
  moves its outputs; `-Measure <script.ps1>` (with `-MeasureArguments @{...}`) runs that script between the first
  snapshot and the tests, with the same variables (Gate M1's measurements). `python -I scripts/snapshot_tree.py snapshot
  <out.json> --hash <dir> --listing <dir>` and `compare <before> <after>` (exit 1 on a difference) work alone (Python
  3.10 or later, output in UTF-8; links are recorded and never entered, what they lead to is snapshotted as a linked
  root unless a root covers it).
- `python -I scripts/esde_census.py --esde-home <ES-DE home> [--esde-install <dir>] [--rom-root <dir>] [--system
  <name>] [--json] [--output <file>]` is the independent oracle of the ROM scan (I2 step 7; standard library only,
  read-only, written apart from the C++ and naming no machine path): it prints per system ES-DE's count, the entries by
  state, the disc groups and the totals, the same table `test_library_source_real.cpp` prints, which must match it byte
  for byte. Run it on a fixture's materialized copy, or on the real library with ES-DE closed.

