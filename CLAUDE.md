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

- Outputs: `build/<preset>/bin` holds `retroverse.exe`, `rv_tests.exe`, `rv_platform_tests.exe` and the vcpkg DLLs.
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
shader never built, `--self-check` failed, developer overlay asked for by a frame-limited run unable to start). Logs:
`<user data>/logs/retroverse.log` (`--data-dir <dir>` keeps test runs out of `%LOCALAPPDATA%`; use
`--data-dir build/rvdata`).

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
  stats of the run` at the end of every frame-limited run; measure frame times with `--no-vsync` (at 1920x1080 on the
  reference GPU the test room's shadow map takes about 0.02 ms, its depth pre-pass 0.02 ms, the ambient occlusion
  0.36 ms, the post-processing 0.3 ms: `--width 1920 --height 1080 --no-vsync --hidden --exit-after-frames --frames
  600`).
- `--screenshot <file.png>` writes the last frame of a frame-limited run; a run that ends before that frame (Esc,
  window closed) exits 3 like a failed write. `--screenshot-size WxH` (default 1920x1080, independent of the
  window) also applies to F12.
- `--self-check <name>` (frame-limited runs only) checks the rendering after initialization with the programs the
  views use, else the run exits 3. `srgb`: an unlit grey quad of the radiance the tonemap shows as linear 0.5, drawn
  into an HDR target and taken through the post-processing into an sRGB target, must read back as 188 +- 2, without
  and then with bloom and dithering. `bloom`: known patterns through the bloom chain: a lit square just below the
  highlight threshold must not bloom, an emitter must keep its energy (+-5%), a single very bright pixel must glow
  alike (+-20%) whatever its value and place. Use `retroverse.exe --self-check srgb --smoke --hidden --data-dir
  build/rvdata` (and `--self-check bloom`).
- Scenes are lit (PBR and unlit emissive materials, scene lights, hemispheric ambient) in linear HDR (`GL_RGBA16F`)
  and post-processed once per view: bloom of the light surfaces emit (screens, lamp shades, neon) and of the
  highlights above a soft threshold (lit paper, wood and walls never bloom), vignette, exposure + ACES, dithering. The
  look is tuned in the scene files (`environment` exposure, ambient and `clearColor`, which is background radiance
  post-processed like the surfaces; `bloomStrength`, the share of that light moved into the glow, 0.06, `bloomRadius`
  0.7 and `vignette` 0.05 by default; `lights`, at most 8 point/spot plus one directional), material presets in
  `assets/materials/base/materials.json`, shaders in `assets/shaders/{common,pbr,unlit,depth,ssao,post}`.
- Shadows and ambient occlusion: the first light with `"castsShadows": true` is the key light. A spot key light gets a
  perspective 2048x2048 shadow map over twice its cone, a directional one an orthographic map fitted to the shadow
  casters (objects with `castsShadows`, default true) and snapped to whole texels; point lights cast no shadow (the log
  and the Lights panel say so: make key lights spots). The lit shader filters the map with a 3x3 tent of hardware
  2x2 PCF fetches; slope-scaled depth bias and a normal offset of 1.5 texels keep surfaces from shadowing themselves
  (no acne) with shadows attached to their casters; `"shadowStrength"` (0..1, default 1) on the light sets how much of
  its light its shadows take away. Every view first draws a depth pre-pass, from which screen-space ambient occlusion
  (`environment.ssao`: `{"enabled": true, "radius": 0.4, "intensity": 1.0}` by default; half-resolution estimate,
  depth-aware blur) darkens the ambient light only, never a light's (a subtle effect in rooms lit mostly by lights).
  The model viewer has neither. Shadows are world-locked (the maps depend on the light, not on the camera): walking
  never makes them shimmer.
- Controls go through the action map (`assets/config/default_bindings.json`, overridden per action by
  `<user data>/config/bindings.json`): WASD/arrows walk (Left Shift sprints), the mouse looks around once captured,
  gamepad sticks walk and look; Esc (gamepad B) leaves one level: it closes a popup of the developer overlay (a
  combo's list, a color picker), else releases the mouse, else closes the overlay, else quits; F1 toggles the
  overlay; F5 reloads the shaders, F12 writes `<user data>/screenshots/retroverse-<timestamp>.png`. Interactive runs
  capture the mouse when the window has focus and on a click into it; frame-limited runs only on a click. Alt+Enter
  toggles fullscreen: the host handles it before the action map (fixed, not rebindable, never seen as Enter). The
  walker collides with walls and furniture and can step into a doorway but not leave the room. Developer builds also
  reload a shader within ~0.5 s of saving any of its files (`assets/shaders`); a broken edit logs
  `<file>:<line>: error ...` and the previous program stays in use.
- Developer overlay (Dear ImGui, `rv_devui`; developer builds only: Release compiles without it and only warns about the
  two options below): F1 shows and hides it, `--devui-open` opens it from the first frame (automation, screenshots).
  Docked panels around the scene: Stats (FPS, a graph of the last 240 frame times, GPU times of the shadow map, the
  depth pre-pass, the ambient occlusion, the scene pass and the post-processing, what the last view drew, GPU
  resources), Camera (feet, yaw, pitch, FOV, speeds, mouse sensitivity, invert Y, collision off = noclip, reset to the
  scene camera), Lights (the key light, every scene light with its shadow flag and strength, the background and the
  ambient, live), Materials (base color, metallic, roughness, emissive strength, live and never saved), Rendering
  (debug line kinds, wireframe, vsync, the key light's shadow on/off with its slope and constant bias and normal
  offset, ambient occlusion on/off with radius and intensity, post-processing: exposure, bloom strength and radius,
  vignette, all live and never saved; shader reload with the last compile errors), Log (level filter, autoscroll) and
  Scene (the objects,
  with "go to", which frames the object in the part of the view the panels leave visible). The default layout widens its
  side columns with the display scale (at least 18 and 22 font sizes). Interactive runs keep the layout in
  `<user data>/config/imgui.ini` (delete it for the default layout); frame-limited runs always start from the default
  layout and save nothing, so overlay screenshots never depend on earlier runs. The overlay sees every event first and
  keeps what it uses (a click on a panel, keys while a value is edited, so Esc in a text field never quits). Opening it
  frees a captured mouse for the panels; a click into the scene captures it again, Esc releases it, and closing the
  overlay gives back the capture it took and closes its popups. Screenshots show the scene only unless
  `--screenshot-include-ui` is given: the overlay is then drawn into `--screenshot` and F12 images too, scaled uniformly
  to fit `--screenshot-size` and centered (another aspect than the window's leaves bands of scene beside it). A
  frame-limited run given `--devui-open` or `--screenshot-include-ui` fails (exit 3) when the overlay cannot start.
- `--input-script <json>` replaces the actions of chosen frames (`rv/input/input_script.hpp`: an array of
  `{"fromFrame": a, "toFrame": b, "actions": {"move_forward": 1.0, "look_delta_x": 20.0}}`, frames counted from 0,
  both included) and logs the pose at the end of each step. Only the listed actions are scripted: in a visible window
  with focus, keys and pads still drive the others, so automated runs add `--hidden`. `--fixed-dt <seconds>`
  replaces the measured frame time, for the walker and the shader time alike, so scripted runs are reproducible.
  Frame-limited runs log the final camera (`Final camera after N frame(s) (T s simulated): feet ..., yaw ..., pitch
  ...`) and the nearest collider with its clearance.

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
wall). Check the log for GL debug messages (`GL debug output summary: 0 high, 0 medium, ...`). In the test room the
ceiling key light (`ceiling_key`) grounds the furniture: each shelf board shades the compartment below it and the shelf
a band of the wall to its left, the CRT, the console and the cabinet throw soft shadows on the wall and the floor to
their right, the floor under the cabinet is dark; zoom in on contact lines for acne (speckles on lit surfaces) and
peter-panning (a lit gap between an object and its shadow). To judge one effect alone, render a scratch copy of the
scene under `build/` (scene files are plain JSON with comments): the key light without `castsShadows`, `"ssao":
{"enabled": false}`, or only a strong ambient (no lights) to see the ambient occlusion, then compare the two PNGs
pixel by pixel. The other reference scenes: `material_test` (roughness grows along X, metallic along Y; the floor
under the lowest spheres darkens a little), `instancing_stress` (1000 boxes in 4 draw calls; the sun casts the
directional shadows; frame times with `--no-vsync`) and `instancing_growth`, which grows the renderer's instance buffer
mid-run: turned around, it shows about 2000 boxes, more than the 1024 the buffer starts with (the log reads `Instance
buffer 'SceneRenderer.Instances' grown to ...`; with `--frames 1` it must not):

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene instancing_growth --exit-after-frames --frames 10 --fixed-dt 0.016667 `
  --hidden --input-script tests/input_scripts/turn_around.json --screenshot build/screenshots/instancing_growth.png `
  --data-dir build/rvdata
```

Other viewpoints and movement are scripted: `tests/input_scripts/*.json` (each file names its expected result; the
unit tests in `tests/unit/integration/test_free_roam_navigation.cpp` play the same scripts):

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 120 --fixed-dt 0.016667 `
  --hidden --input-script tests/input_scripts/walk_forward_2s.json --screenshot build/screenshots/walk.png `
  --data-dir build/rvdata
```

`walk_forward_2s.json` ends 3.2 m from the hero wall: a close-up of the shelf, a poster and the TV on its cabinet
(a cartridge is about 50x30 px at 1920x1080). Two consecutive frames of it (`--frames 90` and `--frames 91`) show the
shadows on the hero wall scaling with the wall and nothing else: walking straight at it, frame 90 magnified about the
focus of expansion matches frame 91 on the wall within about one 8-bit step, shadow edges included (no shimmering).
`turn_around.json` (`--frames 10`) faces the entrance half: the south wall and the doorway, which must read in free
roam too.

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

To check shader errors or hot reload, never edit the tracked shaders: copy `assets/` to a scratch directory, run a
developer build with `--asset-root <copy>` and a long frame limit (`--hidden --exit-after-frames --frames 3000`),
then edit or break the copy's shaders while it runs; the log shows each rebuild or `<file>:<line>: error ...`.

## Layout and layering

- `src/<module>/include/rv/<module>/*.hpp` public headers, `src/<module>/src/*.cpp`, `src/<module>/CMakeLists.txt`
  with an explicit source list. Include as `#include <rv/<module>/<file>.hpp>`.
- Dependency order: `core` -> `geometry` -> `input` -> `assets` -> `scene` (GPU-free; `rv_forbid_link` fails the
  configure if they reach SDL3, glad or ImGui) -> `platform` (SDL3) -> `render` (OpenGL through glad; never
  includes SDL) -> `devui` (Dear ImGui, developer builds; its GPU-free part `rv_devui_model` is always built) -> `app`
  (composition root) -> `retroverse.exe`. Code for the overlay in `rv_app` sits under `#if defined(RV_DEV_UI)`.
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
