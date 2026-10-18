# Retroverse: guidance for Claude Code

Retroverse is a native Windows frontend (C++20, OpenGL 4.5 core, SDL3, no game engine) that turns an ES-DE game
collection into an explorable, procedurally generated 3D world. Authoritative design documents (Italian):

- `specification.md`: product, architecture, data model, milestones M0-M6, requirements FR/NFR.
- `style.md`: art bible (palette, lighting, prefab catalog, glTF conventions, asset licensing).
- `example.png`: approved visual concept.
- `docs/roadmap.md`: iteration plan and step checklists. `docs/architecture.md`: modules and layering.
  `docs/i2/step-<n>.md`: what each I2 step built and decided (ES-DE's behaviour as verified, issue codes, measurements);
  `docs/i3/step-<n>.md` the same for I3 (the world generator, the collection's rendering, the hero CRT).
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
./scripts/test.ps1 -Preset msvc-x64-debug-desktop     # on demand: tests that take over the screen (label "desktop")
./scripts/test.ps1 -Preset msvc-x64-release-gpu       # the same for Release (rendering from its deployed assets)
./scripts/gate.ps1 -Gate A                            # Gate A end to end: build matrix, tests, golden G01; -Clean
./scripts/gate.ps1 -Gate M1                           # Gate M1: Gate A + the catalog's checks, under focus_watch.py
./scripts/gate.ps1 -Gate M1 -RealLibrary              # also the real library (ES-DE closed): NFR-01, NFR-02, imports
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
  `collection-probe.log`), the I1 spike executables
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
  path equal to the dynamic one to the bit, the hover outline), the hero CRT's video (`gpu.video: ...`) and the
  catalog's refresh (`gpu.refresh: ...`). Every clean run is also checked for the run report's I3 sections (the GPU
  memory adding up, at most one decoder thread, joined at exit). Every case of `rv_smoke_tests` (the gpu
  cases and the desktop one) runs with `RV_LAUNCH_ALLOWLIST` naming the build's `rv_fake_emulator.exe` (ctest
  `ENVIRONMENT`; the launch tests set it themselves when run outside ctest). A case is skipped (exit code 77) when no
  OpenGL 4.5 context can be created, and the golden comparison on another GPU or driver. One case:
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
  the update, picking, the plan's scene, `[bench][covers]`, the static groups' planning, `snes-stress` materialized).
  `rv_tests.exe "[label-preview]"` writes sample spines, labels and fitted covers to `build/<preset>/label-preview` for
  a visual review. Golden files are compared byte for byte and rewritten only with `RV_UPDATE_GOLDEN=1` (the test then
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
  '[real-library][media]'` (the media tree and the coverage against ADR-0003), `-Filter '[real-library][launch]'` (the readiness of the real systems and the five launches ES-DE logged,
  read from the git-ignored, machine-local `tests/real_library/local/esde_logged_launches.json`; these two tests are
  skipped without `-RetroArchExe`), `-Filter '[real-library][import]'` (the whole library imported into a new database
  under `build/<preset>/real-library/import`: a first and a second import, `db rebuild-index`, the ROM root unplugged
  and back), `-Filter '[real-library][reconciliation]'` (gb imported alone, then the quick-hash pass over it, twice;
  cold it reads for minutes: the whole library's cold pass, about 45 minutes, is not part of any filter, only of
  `gate.ps1 -Gate M1 -RealLibrary -FullHashPass`), `-Filter '[real-library][app]'` (NFR-01: the library imported under
  `build/<preset>/real-library/app`, then `retroverse.exe` started twice on it, the first page timed from the process's
  creation, and scrolled through every game) and `-Filter '[real-library][query]'` (NFR-02 on the real catalog: the
  benchmark's kinds of search with every order and a filter, p95 within 100 ms, pages within 5 ms); since I3 `-Filter
  '[real-library][room_selection]'` (snes imported alone under `build/<preset>/real-library/room-selection`, its room
  selection printed) and `-Filter '[real-library][covers]'` (a sample of the real SNES covers through the cover
  pipeline, its cache in `build/<preset>/real-library-covers`). `-OutDir <dir>`
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

## Running the app

`retroverse.exe --help` lists every option. Automated runs must always be frame-limited so no process is left
behind: `--smoke` (render `--frames N`, default 3, then exit; any high-severity GL debug message fails the run) or
`--exit-after-frames --frames N`. `--hidden`, `--frames` and `--screenshot` require one of them; frame-limited runs
never show message boxes. Exit codes: 0 ok, 1 smoke check failed, 2 initialization failure (window, context,
GL < 4.5, paths, scene file, input script, mock catalog), 3 runtime failure (unexpected error, `--screenshot` not
written, scene shader never built, `--self-check` failed, developer overlay asked for by a frame-limited run unable to
start, run report not written, the GPU memory accounting missing a texture of the cache). Logs:
`<user data>/logs/retroverse.log` (`--data-dir <dir>` keeps test runs out of `%LOCALAPPDATA%`; use `--data-dir
build/rvdata`).

- Host options of both executables, besides those described below: `--width`/`--height` (the window, default
  1280x720), `--fullscreen` (borderless), `--no-vsync`, `--gl-selftest` (provokes one GL_INVALID_ENUM to prove that
  OpenGL errors reach the log; a `--smoke` run still passes when it is the only one), `--asset-root <dir>` (else
  `RV_ASSET_ROOT`, the source tree in developer builds, `<exe>/assets`), `--data-dir <dir>` or `--portable` (user data
  in `<exe>/RetroverseData`, else `%LOCALAPPDATA%\Retroverse`), `--log-level trace|debug|info|warn|error`,
  `--no-dialogs` (implied by every frame-limited run), `--version`, `--exit` (alias of `--exit-after-frames`).
- `--run-report <file.json>` (both executables) writes a JSON report when the run ends, also when it fails, once the
  command line is valid (an invalid one, `--help`, `--version` and `--write-fixture` write none;
  `rv/app/run_report.hpp`): version and build type, overlay and debug output built in, the asset root and its source,
  exit code, outcome and first failure, `openGlUnavailable` (no OpenGL 4.5 core context), the GL vendor, renderer and
  version (the driver), debug messages per severity (those `--gl-selftest` provoked included; its `glSelfTest` record
  counts them apart, so in a clean run `high` equals its `highSeverityCaptured`), frames, the last view's statistics
  (draw calls, batches, instances, triangles, culled items, shadow and depth draw calls, GPU times), the instance
  buffer's capacity, the screenshot written, the walker's final pose (feet, yaw, pitch as `--spawn` takes them), the
  self-check, the quick catalog (`catalog`: `source` `esde` or `mock`, file, games, times `opened`, `open` at the end,
  the selection; for the ES-DE catalog since I2 step 13 also `notice`, `library`, `systems`, `search`, `filter`,
  `times` {`openedMs`, `firstListMs`, `firstPageMs`} from the process's creation (NFR-01), `pages` and `frames` (p50,
  p95, p99, max of the page fetches and of the frames while it showed games), `interruptedSessions`, `cardSqueezed`
  (the card's blocks the layout cut: empty in every smoke case), `listed` and `shownCount` (the games of the list and
  the count the screen showed, equal once a list from the catalog's worker is shown)) and every launch (`launches`: game, profile, final
  state, `outcome` with `reason` and `detail`, exit code, the panels shown in order (`panels`), `frames` of the request,
  start and end, `idleWakes`, `times`; for the ES-DE catalog `before` and `after`, the selection, search, filter and
  walker's pose when Avvia asked and on the first frame after the end, and `session` {`id`, `error`, `verified`}), and
  since I3 these sections, written by every report (empty or not): `frameTimes` (p50, p95, p99, max swap to swap, the
  first 30 frames left out), `phases` (named spans opened by the code, `refresh` for one, or by an input script, each
  with its frames and frame times), `cpuSections` (main-thread sections: `productUi`, `sceneSubmit`,
  `collectionPlanning`, `layerUploads`, `videoUpload`; p50, p95, p99, max over the frames each ran in), `gpuMemory` (the
  renderer's own accounting per kind: `proceduralSurfaces`, `collectionArrays`, `heroTextures`, `video`, `targets`,
  `sceneTextures`, `buffers`; now and peak), `video` (the hero CRT: the game and picture shown, the one decoder, the
  first frame and uploads, the fallbacks with their reasons, the launches' stops and reopenings; `{}` without a CRT),
  `world` and `residency` (empty until I3 steps 10b and 12); also `catalog.refreshes` (one per refresh: trigger,
  outcome, times, the longest slice, counts, the lists asked meanwhile, `before` and `after`), and in `lastView` the
  static instance groups (`staticGroups`), `outlinedInstances` and `gpuMilliseconds.outline`, only when the view has
  them. Frame-limited runs log the frame times, phases, CPU sections and GPU memory at the end. Read it instead of the
  log in scripts and tests.
- `--scene <name|file>`: a name loads `<asset root>/style_tests/reference_scenes/<name>.json` (default
  `m0_testroom`; also `material_test`, `instancing_stress`, `instancing_growth`), anything with a path separator or
  `.json` is a file. Scene objects can name glTF models (`"model": "<asset-relative path>"`, loaded once per path).
- `--model <file.gltf|.glb>` (not with `--scene`): model viewer, the model at the origin with a three-point light rig
  and a camera framing its bounds from the front right and above, on a dark mid-grey background (about sRGB 61, so
  dark and light models both stand out), without bloom or vignette; a file that cannot be loaded is shown as the
  magenta placeholder (logged).
  `--debug-draw` draws debug lines over any scene or model: object bounds (amber), socket axes (X red, Y green, Z
  blue; the parts hidden by surfaces dimmed, so a socket under a surface still shows its origin) and a world grid on
  y = 0 (1 m cells in scenes; for a model, cells of the power of ten that makes it span 2 to 20 of them: 10 cm for a
  0.5 m model, 1 mm for a 1 cm part).
- Frame statistics (draw calls, batches, instances, triangles, culled items, shader and material switches, the draw
  calls of the shadow map and of the depth pre-pass, GPU times from timer queries, `GPU shadow ... ms, depth ... ms,
  SSAO ... ms, scene ... ms, post ... ms`, frame times) are logged once per second in developer builds and as `Frame
  stats of the run` at the end of every frame-limited run; measure frame times with `--no-vsync` in Release (at
  1920x1080 on the reference GPU the test room's frame takes about 2.9 ms: shadow map 0.03 ms, depth pre-pass 0.04 ms,
  ambient occlusion 0.4 ms, scene 0.4 ms, post-processing with FXAA 0.45 ms: `--width 1920 --height 1080 --no-vsync
  --hidden --exit-after-frames --frames 600`).
- `--screenshot <file.png>` writes the last frame of a frame-limited run; a run that ends before that frame (Esc,
  window closed) exits 3 like a failed write. `--screenshot-size WxH` (default 1920x1080, independent of the
  window) also applies to F12.
- `--self-check <name>` (frame-limited runs only) checks the rendering after initialization with the programs the
  views use, else the run exits 3. `srgb`: an unlit grey quad of the radiance the tonemap shows as linear 0.5, drawn
  into an HDR target and taken through the post-processing into an sRGB target, must read back as 188 +- 2, without
  and then with bloom, dithering and anti-aliasing. `bloom`: known patterns through the bloom chain: a lit square just
  below the highlight threshold must not bloom, an emitter must keep its energy (+-5%), a single very bright pixel must
  glow alike (+-20%) whatever its value and place. `fxaa`: a hard edge slanted 12 degrees between black and white,
  post-processed without dithering, without and then with anti-aliasing: at least half the rows it crosses must get
  intermediate values, every pixel of a flat 3x3 neighborhood must stay exactly as it was. Use `retroverse.exe
  --self-check srgb --smoke --hidden --data-dir build/rvdata` (and `--self-check bloom`, `--self-check fxaa`).
- Scenes are lit (PBR and unlit emissive materials, scene lights, hemispheric ambient) in linear HDR (`GL_RGBA16F`) and
  post-processed once per view: bloom of the light surfaces emit (screens, lamp shades, neon) and of the highlights
  above a soft threshold (lit paper, wood and walls never bloom), vignette, exposure + ACES, dithering, then FXAA (the
  overlay's Rendering panel turns it off for comparisons). The look is tuned in the scene files (`environment` exposure,
  ambient and `clearColor`, which is background radiance post-processed like the surfaces; `bloomStrength`, the share of
  that light moved into the glow, 0.06, `bloomRadius` 0.7 and `vignette` 0.05 by default; `lights`, at most 16
  point/spot plus one directional), material presets in `assets/materials/base/materials.json`, shaders in
  `assets/shaders/{common,pbr,unlit,depth,ssao,post}`.
- Surfaces are procedural, generated at startup (`rv/assets/surface_textures.hpp`, about 0.6 s in Release, 3 s in Debug,
  logged per surface): `oak_planks`, `walnut`, `varnished_wood`, `oak`, `painted_plaster`, `ornate_rug`,
  `brushed_metal`, `aged_plastic`, each with base color, metallic-roughness and (except the rug) normal maps. Materials
  name a map as `"procedural:<surface>/<map>"` in a texture slot, scene files as `{"procedural": "<surface>/<map>"}` in
  `textures`; they repeat, at the scale each surface's description gives (the furniture prefabs and the room shell map
  their faces in meters along the grain). Never add downloaded textures: new looks are new generators or parameters. A
  screen material with a `crt` block (`MAT_Screen_CRT`: line count, scanline and mask strength, curvature, which also
  rounds the picture's corners, vignette, glass reflection, 0.25: a faint sheen that must never wash out the picture)
  shows its picture as a CRT tube; the scanlines and the mask fade out below 4 pixels per period, so at 1920x1080 they
  appear only within about 1 m of the TV.
- Shadows and ambient occlusion: the first light with `"castsShadows": true` is the key light. A spot key light gets a
  perspective 2048x2048 shadow map over twice its cone, a directional one an orthographic map fitted to the shadow
  casters (objects with `castsShadows`, default true; blended ones cast nothing) and snapped to whole texels; point
  lights cast no shadow, nor does a directional light while no object casts (the log and the Lights panel say so: make
  key lights spots). The lit shader filters the map with a 3x3 tent of hardware 2x2 PCF fetches; slope-scaled depth
  bias and a normal offset of 1.5 texels keep surfaces from shadowing themselves (no acne) with shadows attached to
  their casters; `"shadowStrength"` (0..1, default 1) on the light sets how much of its light its shadows take away.
  Thin geometry on walls (posters, signs) gets `"castsShadows": false` in scene files: its shadows would be narrower
  than the filter and break into dashes. Every view first draws a depth pre-pass, from which screen-space ambient
  occlusion (`environment.ssao`: `{"enabled": true, "radius": 0.4, "intensity": 1.0}` by default; half-resolution
  estimate, depth-aware blur; occluders farther than the radius in depth count for nothing, so no halos) darkens the
  ambient light only, never a light's (a subtle effect in rooms lit mostly by lights). The model viewer has neither.
  Shadows are world-locked (the maps depend on the light, not on the camera): walking never makes them shimmer.
- The collection's rendering (I3 steps 0c, 6, 8; `retroverse.exe` draws no collection before step 10b,
  `rv_collection_probe` does): every instance carries an 80-byte record (`scene::InstanceData`: the matrix, then the
  front and spine layers, flags and pick id at attribute location 8). Covers and spines live in texture arrays per
  class (fronts 256 x 188, spines 40 x 256, BC1 sRGB; the log's extensions line names `EXT_texture_compression_s3tc` and
  `EXT_texture_sRGB`), layer 0 the pending picture, uploads within 1 ms a frame (`layerUploads`);
  `--no-texture-compression`, or either extension missing, keeps them in RGBA8 sRGB at half the resolution, and the
  log names the choice (`Collection texture arrays: ...`). Static instance groups upload their records once and cull
  clusters per view (`collectionPlanning`). An instance flagged highlighted gets the hover outline after the
  post-processing (a 2 px ring at 1080p in #F4EFE6, scaled with the output's height, its own GPU timer); one flagged
  hidden is not drawn.
- The hero CRT (I3 step 11, `docs/i3/step-11.md`; the scene's first `crt` prefab with a picture on its screen, the test
  room's): one 480-row decoder, never two decoder threads. It plays the selected game's clip (the quick catalog's
  selection after a 300 ms dwell; before any, the most recently played game with a video), else its screenshot, title
  screen or cover (fitted to 640 x 480 on a worker), else its colour bars; it pauses out of view, stops (joined within 1
  s) before a launch and reopens at its position after the return. Its games come from `--mock-catalog`'s `"media"`
  (`video`, `screenshot`, `titlescreen`, `cover`; absolute or relative to the file) and `"lastPlayed"`; the ES-DE
  catalog gives it none until I3 step 10c (the colour bars meanwhile). `--video-freeze-pts <seconds>` (frame-limited
  runs, 0 to 86400) opens every clip paused at that time and holds the frame: two runs give the same screenshot byte for
  byte. NVIDIA's message 131154 (every video upload) is dropped like the informational ids.
- Controls go through the action map (`assets/config/default_bindings.json`, overridden per action by
  `<user data>/config/bindings.json`): WASD/arrows walk (Left Shift sprints), the mouse looks around once captured,
  gamepad sticks walk and look; Tab (gamepad Y) opens the quick catalog (below); Esc (gamepad B)
  leaves one level: it closes a popup of the developer overlay (a combo's list, a color picker), else the game-session
  panel, else steps back through the quick catalog and closes it, else releases the mouse, else closes the overlay, else
  quits; F1 toggles the overlay; F5 reloads the shaders, F12 saves `<user data>/screenshots/retroverse-<timestamp>.png`.
  Interactive runs capture the mouse when the window has focus and on a click into it; frame-limited runs only on a
  click. Alt+Enter toggles fullscreen: the host handles it before the action map (fixed, not rebindable, never seen as
  Enter). The walker collides with walls and furniture and can step into a doorway but not leave the room. Developer
  builds also reload a shader within ~0.5 s of saving any of its files (`assets/shaders`); a broken edit logs
  `<file>:<line>: error ...` and the previous program stays in use.
- The product UI reads sixteen `ui_*` actions from the same map (`rv/input/action.hpp`, `input::isUiAction`; defaults
  in `default_bindings.json` and `ActionMap::defaults()`, which a test keeps equal): `ui_up` ... `ui_right` (arrows,
  D-pad), `ui_nav_x`/`ui_nav_y` (the left stick, bound without a dead zone: `ui::NavInput`'s hysteresis of 0.55 / 0.35
  of the travel is its dead zone), `ui_page_up`/`ui_page_down` (Page Up/Down, the triggers), `ui_home`/`ui_end` (Home,
  End, the stick clicks), `ui_activate` (Enter, A), `ui_back` (Esc, B), `ui_media` (Y), `ui_info` (X),
  `ui_filter_previous`/`ui_filter_next` (the shoulders). They share keys and buttons with the world's actions; letters,
  digits, space, Backspace, Delete and Tab stay with RmlUi's text fields. A user's `bindings.json` overrides them per
  action like any other. `spike_ui` reads them, and so does `retroverse.exe` while the quick catalog or the game-session
  panel is open, the world's actions while both are closed (`rv/app/input_context.hpp`: the walker stands still under
  the catalog).
- Developer overlay (Dear ImGui, `rv_devui`; developer builds only: Release compiles without it and only warns about the
  two options below): F1 shows and hides it, `--devui-open` opens it from the first frame (automation, screenshots).
  Docked panels around the scene: Stats (FPS, a graph of the last 240 frame times, GPU times of the shadow map, the
  depth pre-pass, the ambient occlusion, the scene pass, the post-processing and the outline, what the last view drew
  with its static groups' draw calls, instances and clusters, GPU resources, the GPU memory per kind now and at its
  peak, the hero CRT's "Video" section), Camera (feet, yaw, pitch, FOV, speeds, mouse sensitivity, invert Y, collision
  off = noclip, reset to the scene camera), Lights (the key light, every scene light with its shadow flag and strength,
  the background and the ambient, live), Materials (base color, metallic, roughness, emissive strength, live and never
  saved), Rendering (debug line kinds, wireframe, vsync, the key light's shadow on/off with its slope and constant bias
  and normal offset, ambient occlusion on/off with radius and intensity, post-processing: exposure, bloom strength and
  radius, vignette, all live and never saved; shader reload with the last compile errors), Log (level filter,
  autoscroll) and Scene (the objects, with "go to", which frames the object in the part of the view the panels leave
  visible). The default layout widens its side columns with the display scale (at least 18 and 22 font sizes).
  Interactive runs keep the layout in `<user data>/config/imgui.ini` (delete it for the default layout); frame-limited
  runs always start from the default layout and save nothing, so overlay screenshots never depend on earlier runs. The
  overlay sees every event first and keeps what it uses (a click on a panel, keys while a value is edited, so Esc in a
  text field never quits). Opening it frees a captured mouse for the panels; a click into the scene captures it again,
  Esc releases it, and closing the overlay gives back the capture it took and closes its popups. Screenshots leave the
  overlay out (the product UI, when open, is always in them) unless `--screenshot-include-ui` is given: the overlay is
  then drawn into `--screenshot` and F12 images too, scaled uniformly to fit `--screenshot-size` and centered (another
  aspect than the window's leaves bands of scene beside it). A frame-limited run given `--devui-open` or
  `--screenshot-include-ui` fails (exit 3) when the overlay cannot start.
- `--spawn x,y,z[,yaw[,pitch]]` starts the walker with its feet at (x, y, z) meters (the eye 1.65 m above), looking
  along yaw and pitch degrees (0 looks at -Z, positive yaw turns right; the scene camera's when left out), instead of at
  the scene camera; the final camera that frame-limited runs log reads the same way. The walk scripts start at `--spawn
  0,0,3.2,0,-6`, on the way in of the test room, so they do not depend on its scene camera.
- `--input-script <json>` replaces the actions of chosen frames (`rv/input/input_script.hpp`: an array of
  `{"fromFrame": a, "toFrame": b, "actions": {"move_forward": 1.0, "look_delta_x": 20.0}}`, frames counted from 0,
  both included; UI actions too, `{"ui_down": 1}`, `{"ui_nav_y": -0.9}`, in [-1, 1], a button action held at the press
  threshold) and logs the pose at the end of each step; a step may carry `"text"` (UTF-8, 1 to 256 bytes, no control
  character), typed once on its `fromFrame` into the product UI's focused text field (the quick catalog's search field
  is two `ui_up` above the list's first game), or `"phase": "<name>"`, a phase of the run report open on its frames,
  with or without actions (`measurement_phases.json`). Only the listed actions are scripted: in a visible window with
  focus, keys and pads still drive the others, so automated runs add `--hidden`. `--fixed-dt <seconds>`
  replaces the measured frame time, for the walker and the shader time alike, so scripted runs are reproducible.
  Frame-limited runs log the final camera (`Final camera after N frame(s) (T s simulated): feet ..., yaw ..., pitch
  ...`) and the nearest collider with its clearance.
- The quick catalog (`rv_ui`'s, over the scene, below the developer overlay) shows the ES-DE catalog of the user data
  directory (I2 step 13, `docs/i2/step-13.md`; not in the model viewer): `library.json` and the database that
  `retroverse-cli library create` and `import` made, opened on a catalog worker thread (`rv_app_catalog`'s
  `CatalogWorker`: the database's writer, the lists, each game's launch readiness, Avvia's resolution right before the
  launch and the play sessions; the main thread fetches pages on its own read-only connection and never waits). Without
  them the list says how to make them (notices `no-library`, `no-database`, `data-directory`, `library-mismatch`,
  `unavailable`; nothing is written into the data directory but the log). The card shows each game's readiness (one
  Italian line) and its first blocker's sentence; Avvia on a game that is not ready shows why and records a blocked
  session. Runs with `--fixed-dt` wait between frames for the worker (lockstep, at most 10 s at a time) so that scripted
  frames see the same lists and launches every run; other runs never wait. "Aggiorna libreria" (a row under the header,
  Up from the search field; I3 step 13r, `docs/i3/step-13r.md`) and `--refresh-library` (once the catalog opened with
  its first list) refresh the catalog from ES-DE: the import of `retroverse-cli import` as a job of the catalog worker,
  in slices of about 4 ms, lists and readiness answered meanwhile (Avvia and sessions outside its write transaction);
  after its commit the systems' readiness is resolved again and the list runs again with the selection kept by GameId; a
  line beside the button tells the progress and the result; quitting cancels it (the import rolled back or committed
  whole). The mock catalog has no refresh (`--refresh-library` records `unavailable`); a refresh is no lockstep wait.
- `--mock-catalog <file.json>` replaces the ES-DE catalog with a file's games, for the tests (`docs/i2/step-A1.md`;
  `rv/app/mock_catalog.hpp`: JSON with comments, `schemaVersion` 1, `games` each with an optional `launch` {`profile`,
  `rom` absolute or relative to the file, `system`, `variables`}, optional `profiles` over the shipped and the user's;
  unknown fields are errors, a file that cannot be loaded exits 2). Either way, Tab opens and
  closes the catalog (the pad's Y opens it and is "Vedi media" inside, which does nothing yet, like "Informazioni"),
  arrows or the D-pad move, Enter (A) on a game is "Avvia": the launch is planned with the game's profile and started
  through `LaunchSession` on `Win32ProcessRunner` (the guard applies) with ADR-0006's keep policy. While the game runs
  the host renders nothing and waits for events, at most 100 ms at a time, woken by the runner: frames, the `--frames`
  budget and input-script frames stand still. The game-session panel ("Gioco in corso", `assets/ui/game_session.rml`,
  not reloaded when saved yet) asks when the session needs it and tells the outcome. Quitting or closing the window
  mid-game leaves the game running; `--launch-wait-limit <seconds>` (frame-limited runs only; default 120, at most
  3600) leaves a game still running then and lets the run finish its frames. Hidden windows hand nothing over and count
  as focused. To try it, copy `tests/smoke/catalogs/fake_emulator_games.json` under `build/` with `@FAKE_EMULATOR@`
  replaced by the build's `rv_fake_emulator.exe` and its `roms/*.gb` created (as `tests/smoke/test_launch.cpp` does),
  set `RV_LAUNCH_ALLOWLIST` to the fake, and run `--mock-catalog <copy> --input-script
  tests/input_scripts/quick_catalog_launch.json --exit-after-frames --frames 50 --hidden --fixed-dt 0.016667
  --data-dir build/rvdata --run-report build/a1/run.json`.

## Visual verification

Rendering changes must be checked visually, not only compiled. The app renders offscreen and writes PNG files:

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 5 `
  --screenshot build/screenshots/m0_testroom.png --data-dir build/rvdata
```

Open the PNG with the Read tool and compare it against `style.md` and `example.png`. The PNG (RGB, no alpha) holds
sRGB-encoded values exactly as the window shows them (`--self-check srgb` proves the encoding), so pixels can also be
checked numerically, e.g. with Python and Pillow; the post pass dithers the scene by one 8-bit step (the overlay is
drawn after it and keeps its exact colors), so compare scene pixels with that tolerance, and crop and enlarge to judge
the bloom, which is subtle by design (in the test room the CRT and the lamp shade glow softly onto the bezel and the
wall). Check the log for GL debug messages (`GL debug output summary: 0 high, 0 medium, ...`). The test room (Gate A,
`docs/testing/gate-a.md`): the north wall holds the collection (two shelves of 96 cartridges, each shelf under its own
spot), the magenta "16-BIT CORNER" sign above the middle of the wall, and right of it the hero corner (the CRT on its
cabinet with the console, the rug, an armchair turned towards it, a sideboard with a table lamp); a varnished wainscot
runs around the room. The ceiling key light (`ceiling_key`) grounds the hero wall: the shelf boards shade the
compartments below them, the CRT, the console and the cabinet throw soft shadows on the wall and the floor; zoom in on
contact lines for acne (speckles on lit surfaces) and peter-panning (a lit gap between an object and its shadow). The
lamps show no shadow: their own lights cast none (one light per view does; BACKLOG "More shadowed lights"), so the
table lamp's down light ends above the floor (its range), or it would light the floor under the sideboard through its
top. The CRT and the sign are the most saturated elements, in tungsten light with a light magenta accent; the upper
walls and the ceiling fall to black (no bounce light). Standing 1 m from the main shelf (`--spawn -1.5,0,-3.5,0,-14`)
the titles must read at 1:1: two lines of whole words, glyphs about 8 px tall around eye height, softened by the
texture filtering and FXAA to about 3:1 against their band; those of the lowest levels read enlarged. The CRT close-up
is `--spawn 1.62,0,-4.0,0,-48` (rounded picture corners; the glass's faint sheen shows on the tube's dark border when a
lamp's reflection falls there, as seen seated at `--spawn 1.62,-0.65,-3.55,0,0`, never as a patch washing out the
picture). To judge one effect alone, render a scratch copy of the scene under `build/` (scene files are plain JSON with
comments): the key light without `castsShadows`, `"ssao": {"enabled": false}`, or only a strong ambient (no lights) to
see the ambient occlusion, then compare the two PNGs pixel by pixel. The other reference scenes: `material_test`
(roughness grows along X, metallic along Y; the floor under the lowest spheres darkens a little), `instancing_stress`
(1000 boxes in 4 draw calls; the sun casts the directional shadows, so each frame writes about 2000 instances, the
view's and the shadow map's, and the instance buffer grows on the first frame; frame times with `--no-vsync`) and
`instancing_growth`, without shadows, which grows the renderer's instance buffer mid-run: turned around, it shows about
2000 boxes, more than the 1024 the buffer starts with (the log reads `Instance buffer 'SceneRenderer.Instances' grown to
...`; for `instancing_growth` with `--frames 1` it must not):

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene instancing_growth --exit-after-frames --frames 10 --fixed-dt 0.016667 `
  --hidden --input-script tests/input_scripts/turn_around.json --screenshot build/screenshots/instancing_growth.png `
  --data-dir build/rvdata
```

Other viewpoints and movement are scripted: `tests/input_scripts/*.json` (each file names its expected result; the
unit tests in `tests/unit/integration/test_free_roam_navigation.cpp` play the walk scripts; `quick_catalog_select.json`
drives `spike_ui`'s quick catalog, see "I1 spikes"):

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --spawn 0,0,3.2,0,-6 --exit-after-frames --frames 120 `
  --fixed-dt 0.016667 --hidden --input-script tests/input_scripts/walk_forward_2s.json `
  --screenshot build/screenshots/walk.png --data-dir build/rvdata
```

`walk_forward_2s.json` ends 3.2 m from the hero wall: a close-up of the sign, the main shelf, the left poster and the
TV on its cabinet (a cartridge is about 50x30 px at 1920x1080). Two consecutive frames of it (`--frames 90` and
`--frames 91`) show the shadows on the hero wall scaling with the wall and nothing else: walking straight at it, frame
90 magnified about the focus of expansion matches frame 91 on the wall within about one 8-bit step, shadow edges
included (no shimmering). `turn_around.json` (`--frames 10`) faces the entrance half: the south wall and the doorway,
which must read in free roam too.

The Gate A golden G01, `assets/style_tests/screenshots/m0_testroom_golden.png`, is the scene camera's 30th frame at
1920x1080 from `msvc-x64-debug` on the reference machine; `m0_testroom_golden.json` beside it records the GPU, the
driver, the commit and the SHA-256 of the scene file and of the image, and `docs/testing/gate-a.md` answers the
checklist of style §26.2 for it. Regenerate it (the command is in its JSON) only when a change alters the look on
purpose, and update the JSON and the page with it. The GPU smoke test `gpu.golden/G01` compares the frame with it on
the reference machine (GL_RENDERER and GL_VERSION as in the JSON; skipped elsewhere): a pixel differs when a channel
moves by more than 6/255, and at most 0.5% may differ (0 do today). Compare any two screenshots the same way with
`retroverse-screenshot-diff <expected.png> <actual.png> [--threshold 6] [--max-percent 0.5] [--heatmap <png>]` (exit
code 0 match, 1 mismatch, 2 error; the heatmap shows unchanged pixels as dark grey, changes within the threshold dim
blue, beyond it red to yellow), e.g. two frames of a walk or a change that should not alter the look.

The developer overlay is checked the same way. Its pixels must hold Dear ImGui's theme colors exactly (they are sRGB
already: the title bar of the focused panel reads 41,74,122), neither washed out nor darkened, with readable text; the
same run without `--screenshot-include-ui` must show the scene alone. `--input-script` can press `toggle_dev_ui` and
`back` on chosen frames:

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --devui-open --exit-after-frames --frames 30 --hidden `
  --screenshot build/screenshots/devui.png --screenshot-include-ui --data-dir build/rvdata
```

glTF models are checked with `--model` and models written to `build/` (never commit generated or third-party models;
downloads go to the git-ignored `assets/staging`). `python -I scripts/make_test_models.py build/models` (Python 3 with
Pillow >= 10.1) writes `orientation_cube.gltf`, a 0.5 m cube whose faces are labeled FRONT +Z, TOP +Y, RIGHT +X, ...:
in the default view FRONT faces the camera, TOP is up and every label reads unmirrored; `orientation_cube_1cm.gltf`,
the same cube 1 cm wide (framing, clip planes, the 1 mm grid); and `material_swatches.gltf`, four cubes from black
plastic to aged ivory whose silhouettes must all stand out from the viewer's background. Add `--debug-draw` for the
grid (scale) and the socket axes:

```powershell
build/msvc-x64-debug/bin/retroverse.exe --model build/models/orientation_cube.gltf --debug-draw --exit-after-frames `
  --frames 5 --hidden --screenshot build/screenshots/model.png --data-dir build/rvdata
```

A generated room's static part is checked as a scene: `retroverse-cli --data-dir build/rvdata worldplan show
tests/fixtures/world/golden/plan-890-seed-102938.json --scene build/scenes/room_890.json`, then `retroverse.exe --scene
build/scenes/room_890.json` as above (the shell, the hero corner, the modules, displays, dressing and lights; the
cartridges come with I3 step 10b). The collection's arrays and static groups show at a glance with
`build/msvc-x64-debug/bin/rv_collection_probe.exe --probe-scenario chunks --exit-after-frames --frames 240 --hidden
--screenshot build/screenshots/arrays.png --data-dir build/rvdata` (`--probe-scenario groups --probe-camera close
--probe-highlight` for the outline).

To check shader errors or hot reload, never edit the tracked shaders: copy `assets/` to a scratch directory, run a
developer build with `--asset-root <copy>` and a long frame limit (`--hidden --exit-after-frames --frames 3000`),
then edit or break the copy's shaders while it runs; the log shows each rebuild or `<file>:<line>: error ...`.

## Asset preview

`retroverse-asset-preview.exe` (style §19.3, `tools/asset-preview`; developer builds only, it needs the Dear ImGui
overlay) shows one asset in the model viewer's neutral studio with the host, renderer, scene instantiation and overlay
of `rv_app` (`app::SceneGraphics`, `app::SceneInstance`, `devui::DevUi`): `retroverse-asset-preview <file.glb |
file.gltf | manifest.json | prefab:<kind>>` with `--params '<json object>'` for a prefab, `--view lit | base-color |
normals | uv-checker | wireframe`, `--panel <title>` (a tab brought forward at start, for screenshots) and the host
options of `retroverse.exe` (`--smoke`, `--exit-after-frames`, `--frames`, `--hidden`, `--screenshot`,
`--screenshot-size`, `--data-dir`, `--fixed-dt`, ...); without a target it shows the empty studio and waits for a file
dropped on the window. It logs to `<user data>/logs/asset-preview.log` and keeps its layout in
`<user data>/config/asset-preview-imgui.ini`. `--write-fixture <file.glb>` writes a CRT-like test model (Body,
ScreenSurface, Anchor_TV_Stand, Anchor_ScreenLook, ColliderProxy; 71 x 61 x 56 cm) and exits without a window.

- Controls: left drag orbits, middle or right drag pans, the wheel zooms (gamepad: right stick, left stick, triggers); F
  frames the subject in the part of the view the panels leave visible (the framing follows the panels until the camera
  is moved), and pressed again before the camera moves frames it with its sockets and scale references (the empty
  studio frames those); R toggles the auto-rotation, 1 to 5 pick the view mode; F1 hides the panels (the labels in the
  scene stay), F5 reloads the shaders, F12 writes a screenshot; Esc closes a popup, else hides the panels, else quits.
  The subject's files are polled: a change reloads it, also one of a .gltf's buffer or image files and a manifest
  created next to a model; the procedural surfaces stay generated across reloads.
- Overlays (Asset panel): floor grid just below y = 0, so that a floor there hides it (10 cm lines on subjects up to 6 m
  across, brighter every meter), a 1 m cube and a 1.70 m marker left of the subject, the bounds with W/H/D in cm, node
  bounds, pivot axes, socket axes with their names, slot volumes (green) and level volumes (teal) with the sockets,
  ScreenSurface outlines, collision proxy outlines, vertex normals.
  The labels are drawn by the overlay, kept clear of each other and of the labelled points: in screenshots they line
  up with the scene when `--screenshot-size` has the window's aspect (1920x1080 for the default 1280x720 window). The
  inspection views (base color, normals, uv checker) show their values exactly (no tonemap, no FXAA) on the lit view's
  background.
- Panels: Asset, Model (node tree with each primitive's triangles, vertices and material, or a prefab's parts and
  sockets), Statistics (bounds in cm, primitives as potential draw calls, triangles, vertices, textures with estimated
  GPU memory, glTF extensions, the slots by volume: "2.0 x 12.5 x 8.5 cm: 231 in 7 levels"), Materials (factors and
  texture thumbnails; glTF materials listed by their own name, then the model file), Manifest (its fields and the
  `validatePrefabManifest` results; brought forward when a manifest comes with a new target or appears next to the model
  as `<name>.manifest.json` or `<name>.json`, not on reloads nor over `--panel`), Screen (each ScreenSurface with its
  size as placed, node scale included, and the color bars it shows: glTF screens are drawn with `MAT_Screen_CRT` and the
  pattern instead of their own material), Stats, Warnings, Log. Screenshots include the panels from the third frame on
  (`--frames 3` or more; an earlier `--screenshot` logs a warning).

```powershell
build/msvc-x64-debug/bin/retroverse-asset-preview.exe --write-fixture build/preview/crt_fixture.glb
build/msvc-x64-debug/bin/retroverse-asset-preview.exe build/preview/crt_fixture.glb --exit-after-frames --frames 5 `
  --hidden --screenshot build/screenshots/preview_fixture.png --data-dir build/rvdata
build/msvc-x64-debug/bin/retroverse-asset-preview.exe assets/manifests/prefabs/electronics.crt.consumer.generic.json `
  --exit-after-frames --frames 5 --hidden --screenshot build/screenshots/preview_manifest.png --data-dir build/rvdata
```

The fixture shows W 71.0 / H 61.0 / D 56.0 cm, three socket names (Anchor_ScreenLook, 0.9 m in front of the screen,
lies outside the default framing; a second F shows it) and the color bars on its screen; the CRT manifest builds
`prefab:crt` and reports one warning (the placeholder hash), and with `--panel Materials` shows that tab instead of the
Manifest one; a copy under `build/` whose `runtime.sockets.screen` names a missing socket reports that error.

## retroverse-cli

`retroverse-cli [--data-dir <dir> | --portable] [--asset-root <dir>] [--log-level <level>] <command> [options]` is the
ES-DE catalog and, since I3, the rooms' plans from the command line (`tools/cli`, one `src/cmd_<name>.cpp` per command,
`cmd_worldplan_<name>.cpp` per worldplan subcommand; `--help`, `<command> --help`).
Results go to stdout, or atomically to `--output <file>`, as text or with `--json` as one object; logs go to stderr and
`<user data>/logs/retroverse-cli.log`. Pass `--data-dir build/rvdata`. Every command of the table is implemented.

- `discover [--home <dir>] [--install <dir>] [--json]` finds ES-DE as ES-DE finds itself (`--home` is ES-DE's own
  option: the ES-DE home is `<dir>\ES-DE`; `--install` defaults to the uninstall entry) and prints the ES-DE home, `~`,
  the install and version, the settings it uses, the roots, find rules, systems, gamelists and issues, each with its
  source. It reads ES-DE's files and writes nothing but its log; exit 1 on an error issue.
- `library [show]` prints `<user data>/config/library.json` and the roots it gives; `library create [--id <slug>]
  [--home <dir>] [--install <dir>] [--rom-root <dir>] [--media-root <dir>] [--force]` discovers ES-DE and writes it
  atomically, never over an existing file without `--force`; the roots must resolve, from the settings or the options.
  The folder options take a bare drive (`E:`) as its root, as ES-DE does. The catalog's `rv_libraries` is the authority
  on the library's id: `import`, `db rebuild-index` and `identities` (not `identities show`) refuse a library.json that
  names another library (exit 2), as `launch`, `media` and `assign` refuse a `--game` of another library; `assign
  <system>` cannot store a choice for a library the catalog does not hold (exit 3). `library create --id <the catalog's
  library> --force` (with the folder options it was made with) names the catalog's library again, which changes no
  state and no GameId.
- `db migrate | check | info [--json]` on `<user data>/database/retroverse.sqlite`: `migrate` creates it or brings it to
  the program's schema (backed up first when it holds anything, the newest 3 backups kept in `database/backups`);
  `check` (integrity, foreign keys, FTS5, the schema against the program's; exit 1 on a problem) and `info` (versions,
  sizes, `rv_meta`, row counts, backups) write nothing into it. Schema v1 stays open until the M1 gate: delete a
  database an earlier build made (`db check` shows the difference), never migrate it. `db rebuild-index [--json]` is an
  import that clears the index tables and writes every row again, every cue sheet and playlist read again; the state
  rows stay (identities, aliases, sessions, choices); it equals a refresh and repairs a damaged index; exit codes as
  `import`'s.
- `import --dry-run [--system <name>] [--json]` prints the library snapshot an import would read
  (`esde::EsdeLibrarySource`, `docs/i2/step-7.md`: the ROM folders listed with ES-DE's rules, the gamelists merged in,
  disc tracks grouped under their sheet, every sheet read): per system its entries by state, disc groups, issues, its
  folder's availability and its gamelist's state, then the totals and "games as ES-DE lists them"; exit 1 on an error
  issue.
- `import [--system <name>] [--accept-removals] [--json]` imports the library library.json records into the catalog
  database (made and migrated when needed; `docs/i2/step-11a.md`, `step-11b.md`): identities, index rows, disc groups,
  media presence, files moved or renamed paired with their identities and what no rule pairs safely as review rows. It
  prints one line per system with rows, changes, issues or an outcome other than applied or source-unavailable (added,
  changed, removed, unchanged, unavailable, kept; a missing system folder that holds no rows is only in the JSON), the
  totals, the identities' changes, each move and review row, the issues and the times; an import that finds nothing
  changed writes no row and no identity, only its record in `rv_imports`. A system with more than half of its present
  files gone is kept as it was (`kept-by-guard`) until `--accept-removals`; a folder that cannot be listed keeps its
  rows as source unavailable; `--system` imports one system. Exit 0; 1 an error issue (a folder that cannot be listed, a
  failed gamelist) or a system kept by the guard; 2 invalid options, no library.json, an unknown system, a library.json
  naming another library; 3 the library or the database cannot be read or written, another import runs (in this or
  another process).
- `media --system <name> --path <relative path>` names per kind the media file ES-DE shows for a ROM path (taken as
  the disk spells it), or `none`; `media --coverage [--system <name>]` gives the share of each kind over four
  populations (every `<game>` entry, those whose file exists, the scraped games, every game) of the library snapshot
  that `import --dry-run` reads (`tools/cli/src/library_snapshot.hpp`); exit 1 when the media root, a media or ROM
  folder or a gamelist could not be read. `media --game <id>` gives a game's media as the last import stored them, each
  file's path derived under the media root of library.json's library; nothing on disk is looked at (exit 1 an unknown
  game, 2 a game of another library, 3 no catalog).
- `systems`, `games [--system <name>] [--state <list>] [--availability <list>] [--include-unavailable] [--sort
  title|system|release-date] [--offset <n>] [--limit <n>]`, `search <text>` (the options of `games`; text that starts
  with `-` after `--`) and `show <gameId>` read the catalog database through a reader (`SqliteCatalogQuery`) and never
  create, migrate or write it: exit 3 without a database or with another schema version, 2 for an unknown `--system`,
  1 for `show` of an unknown game. `show` prints the media paths (as `media --game`), no readiness yet.
- `identities` (`docs/i2/step-11b.md`): `list --system <name>` (GameId, state `present`, `missing` or `unseen`, quick
  hash `current`, `stale`, `none` or `folder`, last path), `show <gameId>` (file facts, quick hash, aliases, history,
  review rows; an alias prints what it stands for; needs no library.json), `reviews [--system <name>] [--all]` (open
  rows with the GameId each candidate has now), `resolve <review> --with <gameId> [--force]` (the candidate becomes an
  alias of the vanished identity, which takes its path, its state rows following; `--force` for an identity that is none
  of the candidates: the merge cannot be undone) or `resolve <review> --keep-separate`, and `hash [--system <name>]
  [--limit <files>]` (the quick-hash pass: resumable, committed in batches of 1,000 files or 10 s, Ctrl+C ends it and
  the batches already committed stay; `--limit` bounds a run; a progress line every 2 s on stderr). `list`, `show` and
  `reviews` write nothing; `resolve` and `hash` take the import lock. Exit 1 an unknown identity or review row, a row
  resolved already or whose file is back, files `hash` could not read; 2 usage errors, another library, a system without
  identities, an identity that cannot be paired with; 3 no catalog, another import runs.
- `profiles [--system <name>]` lists the emulator profiles, `assign`'s and emulators.json's system choices and, for
  every configured system, the command or profile its games launch with, where that choice comes from and why, its
  readiness, the program and core with the find rule that found them, the attempts behind "not found", and each command
  and profile with its own readiness (exit 1 with `--system` when that system is not ready). `launch --dry-run --system
  <name> --path <relative path>` and `launch --dry-run --game <id>` show the request a game would start (argv, working
  folder, environment, command line, the automation guard's verdict, the database's choices read only) and start nothing
  (exit 0 ready, 1 not). Both look at this machine (`Win32SystemProbe`).
- `launch --game <id> [--wait [--wait-limit <seconds>]] [--test] [--json]` (`docs/i2/step-12.md`) resolves a game of the
  catalog right before the launch (its file looked for on disk now, the database's choices) and starts it through
  `LaunchSession` on `Win32ProcessRunner`, so the automation guard applies: automated runs set `RV_LAUNCH_ALLOWLIST`.
  Every attempt is a play session (`rv_play_sessions`): a game not ready is recorded `blocked` and not started; without
  `--wait` the launch is followed through its startup grace (2 s) and half a second more, then a game still running is
  recorded `left-running` (`not-waited`) and the command returns 0 with the game running on; `--wait` follows it to its
  end as its wait strategy says (Ctrl+C stops waiting, `--wait-limit` 1 to 86400 s; nothing is ever terminated);
  `--test` waits and records a launch verification when the game returned normally, else exits 1 (every normal return
  records one too). Exit 0 returned or left running; 1 blocked, failed to start, crashed, a quick exit, a failed
  `--test`, an unknown game; 2 invalid options, a game of another library or that is not a game; 3 ES-DE's
  configuration or the catalog cannot be read (an import makes it), a session that could not be recorded (printed).
- `assign <system> [<label|profile>] [--command <label> | --profile <id> | --clear] [--json]` and `assign --game <id>
  ...` store Retroverse's choice of an ES-DE command label or a profile serving the system in the catalog database,
  never in the user's files; without a value they show the stored choice. The choice of command, most specific first:
  the game's `assign`, its `<altemulator>`, the system's `assign` before emulators.json's `"systemChoices"` (`--clear`
  gives that back), the gamelist's `<alternativeEmulator>`, the first command; a stored choice that disappears blocks
  the launch, never skipped. Choices survive restarts and `db rebuild-index`. Exit 1 an unknown game; 2 invalid options,
  an unknown system, a value the system does not have; 3 the catalog cannot be read or written.
- `sessions [--game <id>] [--limit <n>] [--json]` lists the attempts, newest first (50; 0 for all), after closing as
  interrupted the open sessions whose owner process is gone (`launch --game` does it at its start too; a session another
  live process follows stays open). `exitKind` is null when the session has no exit code (an attempt that never
  started, a start that failed, an open or interrupted session). Exit 1 a `--game` the catalog does not know; 2 invalid
  options; 3 the catalog cannot be read. A GameId that became an alias stands for its game in `launch`, `assign` and
  `sessions`: the JSON of `launch` and `sessions` gives the identity as `game` and the alias as `requestedGame`,
  `assign`'s as `game.id` and `game.requested`; the text says "(for <alias>, an alias)".
- `worldplan` (I3, ADR-0010, `docs/i3/step-3.md`, `step-4.md`, `step-9.md`, `step-10a.md`): a room's plan, the canonical
  JSON `<user data>/worlds/<library id>/world-plan.json` (beside it `world-plan.meta.json`, `overrides.json`'s pins,
  read and never written, the last 10 plans in `backups/` and the world lock `world.lock`, waited for 2 s).
  `worldplan generate --system <system> [--selection <file>] [--seed <n>] [--library <id>] [--replace | --plan <file>] [--json]`
  generates a fresh plan from the catalog's selection (or a canonical selection file), validates it and saves it under
  the world lock, never over an existing plan unless `--replace` (the explicit regeneration, which chooses the face-out
  titles again, after a backup); `--seed` defaults to `world.json`'s, else the library's seed.
  `worldplan update --system <system> [--selection <file>] [--previous <plan>] [--seed <n>] [--library <id>] [--plan <file>] [--diff] [--json]`
  applies ADR-0010 §10's rules (every placement kept, new titles spine-out next to their alphabetical predecessor, then
  a dressed level, a new module in place of decoration, growth, `capacity`; the face-out set unchanged) and prints the
  change report. Both exit 0 written or up to date; 1 nothing written (a plan exists without `--replace`; no plan to
  update, one edited by hand or newer, one that needs `generate --replace`: another seed, preset, template or generator
  major version; the media unknown; no such system in the catalog); 2 usage; 3 the data, catalog, selection or overrides
  unreadable, a hard rule broken, the world locked by another program, a failed write.
  `worldplan show [<plan>] [--library <id>] [--placements] [--scene <file>] [--json]` summarizes a plan (exit 1 no plan,
  3 a plan refused or a scene not written); `--scene` also writes the room's static part (shell, fixtures, dressing,
  lights; no collection items) as a scene file that `retroverse.exe --scene <file>` renders.
  `worldplan check <plan.json> [--selection <file>] [--json]` validates a plan against its templates, the prefabs' slots
  and the world's configuration (29 rules, `rv/world/validation.hpp`; the plan's lights counted, no pins judged: exit 1
  a hard rule broken or not a plan this program reads, soft findings and the LayoutScore reported; 2 usage; 3
  unreadable). Every subcommand takes `--output <file>`.
- `where-is <gameId> [--plan <file> | --library <id>] [--json]` tells where a game stands in its room's plan (room,
  fixture, slot, face-out or spine-out) or why it is not placed, with the version that stands for it; exit 1 no plan
  or a game the plan does not hold, 2 usage, 3 a plan refused.
- `import`, `db rebuild-index`, `media`, `profiles`, `launch`, `assign` and `identities` (but `identities show`) read
  the library `library.json` records (exit 2 without one: `library create` first); so do `worldplan` and `where-is`
  for the world's own plan unless `--library` names it (always with the catalog's selection).
- Nothing is written, the log file included, until the user data directory has passed the guard against the library's
  folders (the ES-DE home, the install, the ROM and media roots, the ROM folder of a system outside the ROM root):
  `discover` and `library` check the folders they find, every other command those of `library.json`.

## I1 spikes

Measurement executables for the three integration risks of I1 (`spikes/`, reports in `docs/spikes/`, decisions in
ADR-0004 to ADR-0006). They run frame- or time-limited like the app; measure performance in Release.

- Video (`docs/spikes/i1-video.md`): `spike_video.exe --generate-fixtures build/spike_video/fixtures --seconds 10
  --hidden --report build/spike_video/report.json --screenshot build/screenshots/video.png --data-dir build/rvdata`
  plays two generated 1280x720 30 fps clips with burned-in frame counters. `--videos <file>...` plays files read in
  place (never written; the ES-DE media folder only read), `--error-cases` shows each damaged file falling back to a
  card naming its failure; also `--count`, `--max-height`, `--no-vsync`, `--upload-fences`, `--priority above-normal`,
  `--list-encoders`. `./spikes/video/run_measurements.ps1` runs the Release series (3 x 60 s, interleaved). GPU checks:
  `./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike/video`.
- Product UI (`docs/spikes/i1-ui.md`, `docs/i2/step-6.md`): `spike_ui.exe --smoke --frames 2271 --hidden --no-vsync
  --fixed-dt 0.016667 --data-dir build/rvdata --report build/spike_ui/report.json` drives the virtualized quick catalog
  over the test room and 100,000 generated games (fixed seed; the only game by "Studio Mezzanotte" sits at position
  50,000) with a scripted SDL virtual gamepad through the UI actions (`--scenario quick-catalog`, the default: 153
  checks; exit 3 when one fails), with `--screenshot-at <frame>:<png>` (repeatable), `--ui-scale`, `--high-contrast`
  and `--interactive` (a real pad or keyboard). `--scenario user-bindings --frames 80 --data-dir
  build/spike_ui/user_bindings` needs `spikes/ui/fixtures/user_bindings.json` copied to that directory's
  `config/bindings.json` (never into `build/rvdata`, which the other runs share: the fixture moves `ui_down` off the
  D-pad); `--input-script <json>` drives the screen with UI actions alone and `--expect-selection <position>` fails the
  run unless the selection ends there (`--frames 120 --input-script tests/input_scripts/quick_catalog_select.json
  --expect-selection 99998`). The documents in `assets/ui` reload when saved in developer builds. GPU checks
  `gpu.spike_ui.scenario`, `gpu.spike_ui.input_script` and `gpu.spike_ui.user_bindings`: `./scripts/test.ps1 -Preset
  msvc-x64-debug-gpu -Filter spike_ui`.
- Emulator return (`docs/spikes/i1-launch.md`): `spike_launch.exe --target fake|retroarch|notepad --strategy
  direct|tree|confirm --policy minimize|keep --repeat N --report <json> --data-dir build/...`;
  `./spikes/launch/measure.ps1` measures. RetroArch only ever runs with the isolated append configuration `spike_launch`
  writes under `build/` (its own config, saves and the ROM folder untouched, checked with before/after snapshots); never
  launch emulators whose data cannot be isolated (DuckStation). Keep visible runs short. The `desktop` tests run the
  fake emulator (`tests/fake_emulator`) in fullscreen: `./scripts/test.ps1 -Preset msvc-x64-debug-desktop`.

## Layout and layering

- `src/<module>/include/rv/<module>/*.hpp` public headers, `src/<module>/src/*.cpp`, `src/<module>/CMakeLists.txt`
  with an explicit source list. Include as `#include <rv/<module>/<file>.hpp>`.
- Dependency order: `core` -> `geometry` -> `input` -> `assets` -> `scene`, `media` (FFmpeg only inside its sources),
  `launcher` (GPU-free; `rv_forbid_link` fails the configure if they reach SDL3, glad or ImGui) -> `catalog` (I2,
  ADR-0008: the catalog's domain and ports, on `launcher`; never SQLite, pugixml or the Windows-only targets) -> `esde`
  (the ES-DE adapter, pugixml private behind `src/esde/src/xml.hpp`), `catalog_db` (the SQLite store, SQLite private),
  `world` (I3, ADR-0010: the world generator, GPU-free, on `core`, `geometry`, `assets`, `scene` and `catalog`; never
  RmlUi, SQLite, pugixml, `media`, `esde`, `catalog_db`, `platform_win` or `platform`:
  `src/world/forbidden_links.cmake`, proven by `unit.cmake/layer_rules`), `platform_win` (Windows without SDL, namespace
  `rv::platform_win`: the system probe `Win32SystemProbe`, the process runner `Win32ProcessRunner`, `win32_util.hpp`; on
  `core` and `launcher`) -> `platform` (SDL3; the window hand-over; links `platform_win`) -> `render` (OpenGL through
  glad; never includes SDL) -> `devui` (Dear ImGui, developer builds; its GPU-free part `rv_devui_model` is always
  built), `ui` (RmlUi product UI; its GPU-free part `rv_ui_model` is always built) -> `app` (composition root) ->
  `retroverse.exe`. `catalog` holds the domain, the identities, one header per port (I2 step 1), the media coverage (8)
  and the launch resolution (10); `esde` reads ES-DE's configuration and gamelists (steps 2 and 3), scans the ROM
  folders (7), resolves media (8) and translates ES-DE commands (10); `catalog` also holds the import service (11a),
  fingerprints and reconciliation (11b); `catalog_db` holds the database, the catalog store (step 4, with the import's
  writes of 11a and 11b), the queries and search (9) and the session store (12); the system probe's port is in `core`
  (`rv/core/system_probe.hpp`, ADR-0008 §2), and `rv_forbid_link(<target> EXTRA <library>...)` forbids more libraries
  per target (`cmake/LayerRules.cmake`). Code for the overlay in `rv_app` sits under `#if defined(RV_DEV_UI)`. `rv_app`
  links `rv_ui`, `rv_launcher` and `rv_platform_win` for the quick catalog's launches (I2 step A1) and, since step 13,
  `rv_app_catalog` (GPU-free and tools-free, between `rv_app_cli` and `rv_app`: the catalog worker, its
  `ui::CatalogSource`, the app's copy of the CLI's launch library, `AppCatalog` over the ES-DE or the mock catalog; on
  `rv_catalog`, `rv_esde`, `rv_catalog_db`, `rv_platform_win`, `rv_ui_model`; tested by `tests/unit/app`; since I3 step
  13r also the refresh, `library_refresh.hpp`: the import in slices, GPU-free). Since I3 `rv_app` links `rv_media` (the
  hero CRT's `ScreenService`, private, step 11) and `rv_world` (also linked by `rv_app_cli` and `rv_cli_core`);
  `rv_app_cli` holds the frame timing and phases (`frame_timing.hpp`, step 0d), the interaction model
  (`world_interaction.hpp`, step 7: the world's states, input contexts and Back levels; unused until step 13a) and the
  hero screen's policy (`screen_policy.hpp`, step 11). Files that later I3 steps fill are stubs naming their step
  (`docs/i3/step-0a.md`).
- `rv_world` (`src/world`, `docs/i3/`): ids (`room:snes/fx:w03/s:3.12`), the WorldPlan and its canonical JSON
  (`plan_json.hpp`: `writePlan`, `readPlan`, `planDigest`; a newer schema refused, a hand-edited plan refused; integers
  only: millimetres, hundredths of a degree, permille), `WorldConfig` with the preset and `world.json`, the readers of
  `assets/world` (errors name `<file>:<line>` and a JSON pointer; 1a); `selectRoom` and `title_key.hpp` (the room's
  titles, `title-key/1`, representatives by region rank, status and revision, priority; 2); `generation.hpp`
  (`generate`, `loadWorldData`), `capacity.hpp`, `seeds.hpp`, the plan's lights (3); `validation.hpp` (`validatePlan`,
  29 rules) and `nav_grid.hpp` (4); `pick_index.hpp` (picking, the 3-frame hover) and `slot_navigation.hpp` (the D-pad
  steps; 7); `world_store.hpp` (the world's files and lock, `savePlan`), `incremental.hpp` (`update`), `overrides.hpp`
  (pins: protected, never applied) and `where_is.hpp` (`PlanIndex`; 9); `scene_builder.hpp` (`toSceneFile`,
  `toSceneDescription`, `loadSceneData`, the evening lighting profile, the theme's materials) and
  `collection_layout.hpp` (items with transforms, pick ids, layer keys and flags, clusters, groups, occluders,
  navigation; 10a). I3's other modules: `rv_geometry` the collection's prefabs (`display_case`, `wall_shelf`,
  `storage_box`, `magazine_stack`, `bookends`, the cartridge's `spineLabel`, `shelf`'s display levels and front inset;
  `Slot_<level>_<index>` and `Level_<n>` sockets, `SlotFacing`, `slotLevels`; 1b); `rv_assets` the prefab families
  (`assets/manifests/families.json`, `resolvePrefab`: style §21's object fallbacks, one warning each; 1b) and the
  collection's layers (5, GPU-free, safe on the worker pool: `cover_fit`, `mip_chain`, `bc1`, `text_raster`,
  `collection_labels`, `rvtex`, `derived_texture_cache`); `rv_scene` the 80-byte `InstanceData` (0c), the eased
  `PoseAnimator` (7) and the static groups' layout and the outline's geometry (`instance_clusters.hpp`, 8); `rv_catalog`
  `RoomEntry`, `RoomSnapshot` and `ICatalogQuery::roomEntries` (1a), which `SqliteCatalogQuery` answers in one read with
  the media left unknown (2); `rv_esde` `checkRoomMedia` and `roomMediaPaths` (`media_resolver.hpp`, 2); `rv_render`
  the texture arrays (`Texture2DArray`, `CollectionArrays`; their layout, allocator and budget in
  `rv_render_uniforms`'s `texture_array_layout.hpp`; 6), the static instance groups and the outline pass (8), a
  material's texture override (11), and in `rv_render_uniforms` the GPU memory accounting (`gpu_memory.hpp`, 0d).
- `tools/asset-preview`: `retroverse-asset-preview.exe` on `rv_app` and `rv_devui` (built only with `RV_DEV_UI`), and
  its GPU-free core `rv_preview_core` (`include/rv/preview/*.hpp`, namespace `rv::preview`), built in every
  configuration and tested by `tests/unit/preview`.
- `tools/cli`: `retroverse-cli.exe` and its GPU-free core `rv_cli_core` (`include/rv/cli/*.hpp`, namespace `rv::cli`:
  the command table, the dispatcher with the data-directory guard, the output contract; one `src/cmd_<name>.cpp` per
  command), built in every configuration and tested by `tests/unit/cli` and, as a process, by
  `tests/cmake/test_cli_process.cmake`.
- `tools/screenshot-diff`: `retroverse-screenshot-diff.exe` and its GPU-free core `rv_screenshot_diff_core`
  (`include/rv/screenshot_diff/*.hpp`, namespace `rv::screenshot_diff`), built in every configuration, tested by
  `tests/unit/screenshot_diff` and linked by the GPU smoke tests.
- Tests: `tests/unit/<module>/test_*.cpp` (globbed into `rv_tests`, label `unit`, no SDL or OpenGL; GLB files for
  tests come from `rv/assets/glb_writer.hpp`; `tests/unit/platform_win`, `tests/unit/launcher` and
  `tests/unit/support` start `rv_fake_emulator.exe` from the directory of `rv_tests.exe`, and `tests/unit/cli` also
  `retroverse-cli.exe` as a second process: `rv_tests` depends on both; `tests/unit/catalog_db/import_test_support.hpp`
  opens catalogs, dumps the index and state tables, scripts sources and resolvers and settles folder times,
  `settleFolderTimes()`; `tests/unit/cli/test_cli_reconciled_sessions.cpp` tests reconciliation with sessions and
  choices on the real code paths),
  `tests/platform/test_*.cpp` (globbed into `rv_platform_tests`, label `unit`: SDL platform layer, no OpenGL),
  `tests/support/*` helpers (globbed into `rv_test_support`, namespace `rv::test`: `TempDir`, `LogCapture`, `Fixture`
  (`fixture.hpp`: `tests/fixtures/<name>/` materialized into a fresh folder under `build/<preset>/test-fixtures`,
  removed afterwards; `{{FIXTURE_ROOT}}` in .xml, .json, .txt and .cfg files becomes that folder, escaped for the file;
  the fixture's `files.json` generates ROM payloads from a size and a seed, with an optional `mtime` and `hidden`,
  copies programs of the build with `copyOf` (from `testProgramsDir()`; such a copy finds its DLLs through PATH only)
  and makes empty folders, so that no payload is committed; since I3 step 1c `cover` and `video` payloads,
  `fixture_payloads.hpp`: an original picture with the title, PNG or JPEG, landscape, portrait or a cartridge, or
  truncated or not an image; a 4:3 clip of `media::writeTestVideo`, or empty, truncated, corrupt or of an unsupported
  codec; a whole generated system, `"collections"`, `fixture_collection.hpp`; and `Fixture::applyOverlay(folder)`, a
  materialized folder laid over the root), `writeProgramStub` (`program_stub.hpp`), `runChild`
  (`child_process.hpp`: a program run to completion, its command line built by `launcher::buildCommandLine`, stdout and
  stderr in files of the test's choosing, apart or together, a timeout), `FakeSystemProbe` (`fake_system_probe.hpp`: a
  machine as maps of registry values, files and folders), `Junction`, `ListingDenied`, `DataReadDenied`,
  `writeFileAnyLength` (`file_system.hpp`: junctions without admin rights, a folder this user cannot list, a file whose
  data cannot be read, paths past MAX_PATH), `ScopedLaunchAllowList`, `ScopedEnvironmentVariable`, `ScopedPathPrefix`
  (`scoped_environment.hpp`: tests that launch set `RV_LAUNCH_ALLOWLIST` for their own duration, then put it back),
  `ensureBackgroundLaunches`, `prepareAutomatedRun` (`background_launches.hpp`: `RV_LAUNCH_BACKGROUND=1` unless set,
  and no failure dialogs; `rv_tests` and `rv_smoke_tests` call it before any test),
  `generateCatalogDatabase` (`catalog_generator.hpp`: a deterministic catalog database of any size, written in one
  import transaction with the search index suspended; `rv_test_support` links `rv_catalog` and `rv_catalog_db`)),
  `tests/fixtures/<name>/` (original, minimal fixtures; text committed as LF, so CRLF, BOM and oversized files are
  written at test time; `tests/fixtures/esde/standard` is the ES-DE installation the catalog steps share: its README
  lists what each part exercises and the counts they assert, and it only ever grows additively; a step that needs
  another situation adds a fixture folder of its own: `esde/portable` (a portable installation, `portable.txt` variants
  written into the copy), `esde/custom-dirs` (ROM and media roots and system folders moved with `~` and `%ESPATH%`),
  `esde/custom-systems` (the user's `custom_systems` with ES-DE's quirks), `esde/nested-roms`, `esde/multi-disc`,
  `esde/arcade`, `esde/noload` and `esde/long-path` (the ROM scan: namesakes, game directories and gamelist entries in
  sub-folders, disc groups, MAME sets and names, `noload.txt`, hidden folders and `ParseGamelistOnly`, paths past
  MAX_PATH), `esde/media-roots` (an empty media root; a missing one and one on an unmapped drive as variants), each
  README stating its expectations; their settings hold fake scraper credentials that nothing may show, log or
  record; for I3 `esde/snes-room` (50 invented titles with generated covers and clips, damaged ones among them, G10's
  ten in `additions/`), `esde/snes-single` (one ROM, no media) and `esde/snes-stress` (2,000 generated titles, 300
  second releases), their system `snes` with the invented full name "Lumen 16 Home System", their READMEs stating the
  counts the tests assert; `tests/fixtures/world/` the golden selections, plans and scenes), `tests/fake_emulator`
  (`rv_fake_emulator`, built with the tests or the spikes), `tests/collection_probe` (`rv_collection_probe`, built with
  the tests; its `probe_layout.hpp` shared with the smoke tests),
  `tests/input_scripts` (scripted verification runs),
  `tests/smoke/*.cpp` GPU smoke tests (globbed into `rv_smoke_tests`, label `gpu`: GPU-free itself, it starts the
  executables of its build with `tests/support/child_process.hpp` and reads their run reports and screenshots;
  `smoke_support.hpp` runs them, `image_checks.hpp` measures screenshots, `launch_support.hpp` prepares the launches)
  with their test scenes in `tests/smoke/scenes` and mock catalogs in `tests/smoke/catalogs`,
  `tests/cmake/test_*.cmake` (label `unit`, listed in `tests/cmake/CMakeLists.txt`, run by ctest in CMake script mode:
  the build's CMake scripts, e.g. the Release asset mirror, `retroverse-cli` as a process, its exit codes with
  stdout and stderr apart, and the layer rules, `unit.cmake/layer_rules`).
- Configuration files, JSON with comments: shipped defaults under the asset root, the user's under
  `<user data>/config`, which Retroverse reads and never writes: `assets/config/default_bindings.json` under
  `bindings.json` (per action), `assets/launcher/emulators.json` (the emulator profiles: one RetroArch profile for 50
  systems, `rv/launcher/emulator_profile.hpp`) under `emulators.json` (a user profile replaces the shipped one with its
  id whole; a broken user file or profile is left out with an issue; `"systemChoices"` gives Retroverse's choice for a
  system, `{"snes": {"command": "bsnes"}, "gb": {"profile": "retroarch"}}`: one of the system's ES-DE command labels,
  compared exactly, or a profile id, the user's replacing the shipped one per system; the shipped file sets none, and a
  stale label or unknown profile makes the system "requires launcher configuration"), and since I3
  `assets/world/presets/<id>.json` under `world.json` (per key: `preset` names another preset, `seed` a seed or null,
  integers only, millimetres and permille; a broken file is left out whole with one issue). The world's data, read
  strictly (unknown or duplicate keys and fractions refused with their line): `assets/world/templates/` (the room
  template `console_room_9x10.json`), `platforms/`, `media_families.json`; prefab families in
  `assets/manifests/families.json`. The one exception,
  `library.json` (the library's id, the ES-DE home and install, root overrides), is written only by `retroverse-cli
  library create`; choices made later go to the database (ADR-0008 §6).
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
- Text and paths are UTF-8; convert with `rv::core::pathFromUtf8` / `rv::core::pathToUtf8`, which throw on text that
  does not convert: names from a file system or an untrusted file go through `rv/core/utf8.hpp` (`tryPathFromUtf8`,
  `tryPathToUtf8`, `pathToUtf8Lossy` for messages, `utf8ToWide`/`wideToUtf8`, `isValidUtf8`/`findInvalidUtf8`,
  `appendUtf8`; `utf8WithWindows1252Fallback` and `windows1252ToUtf8` for text other programs wrote).
- Core helpers of I2, one header each in `rv/core/`: `sha256.hpp` (SHA-256, streaming and one-shot, hex);
  `case_fold.hpp` (`fileSystemUpperCase`: the file-system casing of lookup keys and path comparisons, code unit by code
  unit as NTFS and exFAT compare names, never ASCII-only folding or Unicode normalization; `invariantLowerCase`, the
  same table's lower case for the catalog's sort keys; `casingTableVersion`);
  `time.hpp` (UTC ISO-8601: `formatUtcIso8601`, `utcNow`, `parseUtcIso8601`); `relative_path.hpp`
  (`resolveRelativePath`: a path read from a file, a gamelist's for one, relative to its folder; `OutOfRange` when it
  lies outside the folder, `InvalidArgument` when malformed); `tree_walk.hpp` (`walkTree`/`listTree`: big trees through
  `\\?\` paths, sorted, links followed without loops, errors as entries, a folder that cannot be listed `Unavailable`);
  `file_io.hpp` (`readFileShared` for files another program rewrites, `extendedLengthPath` for folders the user names,
  `extendedLengthPathBelow` to reopen a walked name exactly: a plain path loses trailing dots and spaces;
  `SharedFileReader`, positioned reads of a file opened with full sharing, its facts from the handle; `listedFileFacts`,
  a file's size and time as a listing gives them, which is what the import records);
  `system_error.hpp` (Windows errors as text and as `ErrorCode`: a missing drive is `Unavailable`, a locked file
  `Conflict`; `ErrorCode::Cancelled` is an operation stopped on request); `paths.hpp` (`Paths::databaseDir`,
  `pathIsWithin` (lexical), `checkUserDataDirectory`: the data-directory guard, links and junctions resolved);
  `system_probe.hpp` (the `ISystemProbe` port with `appPath`, `findUninstallEntry`, `matchesWildcard`;
  `platform_win::Win32SystemProbe` asks Windows, `test::FakeSystemProbe` maps).
- Core helpers of I3: `seed.hpp` (`deriveSeed`: SHA-256 of the NUL-separated tuple `rv-seed/1`, parts..., integers as
  decimal digits, the first 8 bytes big-endian), `random.hpp` (`Random`, SplitMix64 with its own `below`, `between`,
  `unit`, `shuffle`, `kRandomAlgorithm` `splitmix64/1`; never the standard distributions or `std::shuffle`),
  `canonical_json.hpp` (`writeCanonicalJson`, `canonicalJsonDigest`, `parseCanonicalJson`, which refuses any text that
  is not canonical: keys by code point, integers only, top-level arrays one element per line, a final LF),
  `worker_pool.hpp` (`WorkerPool`: `min(6, hardware threads - 3)` threads below normal priority, bounded (a full queue
  refuses, never blocks), priorities, `JobGeneration` cancellation, a 2 s shutdown bound), `backups.hpp`
  (`backUpFile`, the newest 10 by default, named as the database's backups), `cpu_timers.hpp` (`CpuTimers`,
  `mainThreadCpuTimers`, `CpuSectionScope`, `summarizeDurations`, the one nearest-rank percentile rule); `file_io.hpp`'s
  `writeFileAtomic` flushes before a write-through rename; `log.hpp`'s `logLinesSince` reads the in-memory lines and
  their count under one lock.
- GPU memory (`rv/render/gpu_memory.hpp`: `GpuMemoryLedger`, `gpuMemoryLedger()`, `GpuMemoryAllocation`,
  `textureBytes`): every texture and buffer of `rv_render` records its bytes under a `GpuMemoryKind`. A texture created
  outside the texture cache records a kind of its own (`TextureDesc::memoryKind = HeroTextures` for the hero CRT's
  stills; the texture arrays and the video texture record theirs): frame-limited runs fail (exit 3) when the ledger's
  procedural surfaces and scene textures differ from what the cache and the defaults hold. A new storage format adds a
  `GpuFormat` and its line in `textureLevelBytes`.
- The collection's layers (`rv_assets`, I3 step 5): fronts 256 x 188 and spines 40 x 256 in BC1 sRGB (RGBA8 at half
  the resolution as the fallback), mips in linear light, spines and generated labels drawn with stb_truetype from the
  bundled fonts, loaded only when their SHA-256 matches `assets/registry/assets.json`; the derived-texture cache under
  `<user data>/cache/textures/<library id>` (`.rvtex` entries named without the source, found while it is out of
  reach, invalidated by its size and time, trimmed to 512 MiB). Raise `kCoverPipelineVersion` (`rvtex.hpp`) with any
  change of a layer's pixels.
- World plans (ADR-0010): a plan is `writePlan()`'s canonical text and is only ever written through
  `world::savePlan()` under the world lock (a hard rule broken is never saved); never edit one by hand (it is then
  refused). Placements are never moved by an update; only `worldplan generate --replace` chooses the face-out titles
  again.
- Catalog identities (ADR-0008 §4, `rv_catalog`): a GameId is derived once from (`rv-game/1`, library, system, lookup
  key) and then persisted; never derive it again for a stored identity. The golden GameIds of
  `tests/unit/catalog/test_identity.cpp` come from `scripts/game_id_oracle.py` (Python's hashlib): a change of the
  derivation changes both, and `python -I scripts/game_id_oracle.py --check tests/unit/catalog/test_identity.cpp`
  must pass. Likewise the golden quick hashes (rule 1, `rv/catalog/fingerprint.hpp`: SHA-256 of a file up to 128 KiB,
  else of its first and last 64 KiB) of `tests/unit/catalog/test_fingerprint.cpp` come from
  `scripts/quick_hash_oracle.py`, and this must pass:
  `python -I scripts/quick_hash_oracle.py --check tests/unit/catalog/test_fingerprint.cpp`. Reconciliation
  (`rv/catalog/reconciliation.hpp`, ADR-0008 §4) pairs an equal quick hash only with an equal file name or time
  (ADR-0009); anything it cannot pair safely becomes a review row, never a merge.
- ES-DE files (`rv_esde`, ADR-0008; ES-DE 3.4.1's behaviour as verified in its source is in `docs/i2/step-2.md` and
  `step-3.md` (configuration, gamelists), `step-7.md` (the ROM scan, disc groups, MAME lists), `step-8.md` (media) and
  `step-10.md` (launch commands and find rules)): follow ES-DE, and report every deliberate difference as a
  `catalog::Issue` or in the header. `~` is ES-DE's home folder (the parent of the ES-DE home) and every `~` of a text
  is replaced. The settings reader extracts only its nine whitelisted keys (`esde::settingKeys()`): never read, log or
  return the scraper credentials or the UI mode's passkey. `rv/esde/gamelist.hpp`: `readGamelist` (NotFound means no
  gamelist; any other error is retried by the caller), `gamelistPath`, `entryForListed`; issue codes `gamelist-*`, those
  about single entries listed up to 100 per code; of the entries of one kind and path the last counts, entries of one
  file that differ in kind or case are all kept for the scan to choose. The user's paths are logged at debug level only:
  parse files under the user's folders with `core::readFileShared` and `detail::parseXml`, as `readGamelist` does
  (`detail::loadXmlFile` still logs the loader's warnings with the path at warning level, BACKLOG). The library snapshot
  comes from `rv/esde/library_source.hpp` (`EsdeLibrarySource`; `scripts/esde_census.py` must agree with it), media from
  `rv/esde/media_resolver.hpp` (`EsdeMediaResolver`: ES-DE 3.4.1's paths and extensions in its order,
  `mediaRelativePath`; a media root out of reach or a media folder that cannot be listed is `Unavailable`, never "no
  media"; open a media file through `core::extendedLengthPathBelow`, since names may end with a dot or a space), the
  coverage from `rv/catalog/coverage.hpp` (`measureCoverage` over four populations), launch requests from
  `rv/esde/launch_translation.hpp` (`EsdeCommandTranslator`) through `catalog::resolveLaunch`
  (`rv/catalog/launch_resolution.hpp`, the readiness of a system or a game).
- Catalog database (`rv_catalog_db`, `docs/i2/step-4.md`): only `src/catalog_db/src/database.cpp` includes `sqlite3.h`;
  other code uses `Connection`, `Statement` (kept statements reset after each use, `ScopedReset`) and `Transaction`.
  One writer per process on one thread (`Database::writer()`), read-only readers on other threads
  (`Database::openReader()`). Schema v1 stays open until the M1 gate: a step adds its tables to v1 (`AreaStep{1, ...}`)
  in its area's source (`schema_identity.cpp`, `schema_index.cpp`, `schema_search.cpp`, `schema_launch.cpp`), STRICT,
  foreign keys only into `rv_*` (never into `idx_*`) and never cascading; writes are upserts, never `REPLACE`; tests
  keep databases under their temporary folder. A state table that names a game refers to `rv_game_identities (id)`
  through a single-column foreign key: `putAlias()` finds those columns in the schema and re-points their rows when an
  identity becomes an alias (a table with one row per game keeps the moved identity's), which a composite key would
  escape. Since I2 step 11 the identity area also holds the import records (`rv_imports`), the quick hash read and
  written with the identity (`StoredIdentity::quickHash`; the pass writes it alone with `putQuickHash()`) and the
  history, alias and review rows; the index area the disc references (`idx_disc_references`); the launch area (step 12)
  `rv_play_sessions`, `rv_launch_game_choices`, `rv_launch_system_choices` and `rv_launch_verifications`. Whatever
  writes identities, aliases or index rows holds the import lock (`ICatalogStore::lockImport()`, never
  waiting: `Conflict`): the import from before it reads the library (it reads the store before its write transaction,
  which holds the writes alone), the quick-hash pass for its run, a review's resolution. Session and choice writes take
  none and touch neither; another process's bulk import can hold the write lock for longer than the 5 s busy timeout,
  so they go through `SessionStore` (`BusyRetry`, `SessionRecorder`, `isBusyError()`). Unchanged rows and identities
  are not written: `rv_game_identities.last_seen` is when an import last wrote the identity with its file present, not
  the last import that saw it, which `rv_imports` records (the newest `kKeptImportRecords`, 100, per library, each
  holding only the systems with something to say). Bulk writes call `suspendSearchIndex(writer)` once their transaction
  has begun and `resumeSearchIndex(writer)` before its commit (`rv/catalog_db/search_index.hpp`; a commit in between
  leaves the index behind, which `db check` reports); the import's store does it through `prepareWrites()`
  (`SearchIndexPolicy::Automatic`: a first import, or rows written at least a fifth of the index). Queries go through
  `SqliteCatalogQuery` (`catalog_query.hpp`, step 9), one instance per thread on its own reader; user text never reaches
  FTS5 as syntax.
- OpenGL calls only in `rv_render`/`rv_devui`/`rv_ui`/`rv_app` (and the I1 spike executables), only on the main
  thread, DSA style (GL 4.5; the vendored RmlUi GL3 renderer in `third_party/rmlui_backends` excepted).
- Determinism: no `rand()`, no iteration over unordered containers into outputs, explicit seeds (`core::deriveSeed`,
  `core::Random`); the world's arithmetic is integer, so Debug and Release write the same plans. The golden seeds,
  sequences and canonical texts come from `scripts/world_seed_oracle.py` (Python's hashlib and json) and the title
  keys and version choice of `tests/unit/world/test_title_key.cpp` from `scripts/title_key_oracle.py`; a rule change
  changes both, and these must pass: `python -I scripts/world_seed_oracle.py --check
  tests/unit/core/test_seed_random.cpp tests/unit/core/test_canonical_json.cpp` and
  `python -I scripts/title_key_oracle.py --check tests/unit/world/test_title_key.cpp`.
- Product code starts processes only through `rv_launcher` and `rv_platform_win` (`Win32ProcessRunner`): argv, never a
  shell string, no batch files; the product never terminates an emulator. An emulator profile's argument template
  gives one argument per element, never split again (`rv/launcher/arg_template.hpp`); `{env:NAME}` takes only the
  allow-listed folder variables, since command lines are logged. (Test harnesses start their own children:
  `tests/support/child_process.hpp`, `rv_fake_emulator`.)
- Product UI strings live in the RML documents under `assets/ui` (Italian until localization tables exist); sizes in dp,
  bundled fonts only, no color transitions (RmlUi rebuilds the geometry of every animated frame).
- Language: identifiers, comments, documentation and commit messages in English.

## Safety rules

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

## Commits

Small focused commits, imperative subject line (`render: add shader hot reload`), body explaining why when not
obvious. Run the unit tests before committing. Follow the commit trailer instructions given by the session.
