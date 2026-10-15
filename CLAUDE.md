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
./scripts/test.ps1 -Preset msvc-x64-release-gpu       # the same for Release (rendering from its deployed assets)
./scripts/gate.ps1 -Gate A                            # Gate A end to end: build matrix, tests, golden G01; -Clean
./scripts/dev.ps1 cmake --preset msvc-x64-debug       # any raw command inside the dev environment
```

- Outputs: `build/<preset>/bin` holds `retroverse.exe`, `retroverse-asset-preview.exe` (developer builds only, see
  "Asset preview"), `retroverse-screenshot-diff.exe`, `rv_tests.exe`, `rv_platform_tests.exe`, `rv_smoke_tests.exe`
  and the vcpkg DLLs.
- GPU smoke tests (`tests/smoke`, label `gpu`, ctest names `gpu.<case>`): each case starts the executables of its
  build frame-limited with `--hidden`, `--data-dir` and `--run-report` under `build/<preset>/smoke/<case>` (logs,
  reports, screenshots, heatmaps stay there) and checks the reports and screenshots: the reference scenes with frame
  statistics bounds, `instancing_growth` growing mid-run, the three self-checks, the key light's shadow and the ambient
  occlusion on `tests/smoke/scenes` (with and without the effect, compared at projected probe points), the developer
  overlay and the asset preview (developer builds), golden G01 (reference machine only). A case is skipped (exit code
  77) when no OpenGL 4.5 context can be created, and the golden comparison on another GPU or driver. One case:
  `build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden*"` (Catch2 test names), or `./scripts/test.ps1 -Preset
  msvc-x64-debug-gpu -Filter golden`.
- `scripts/gate.ps1 -Gate A` builds `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` (a compiler,
  linker or CMake warning fails the step; `-Clean` rebuilds every target), runs the unit tests and the GPU smoke tests
  of Debug and Release and the golden comparison of both, prints a summary table (logs in `build/gate-a`) and exits 1
  when a step failed (skipped steps are reported, not failed).
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
shader never built, `--self-check` failed, developer overlay asked for by a frame-limited run unable to start, run
report not written). Logs: `<user data>/logs/retroverse.log` (`--data-dir <dir>` keeps test runs out of
`%LOCALAPPDATA%`; use `--data-dir build/rvdata`).

- Host options of both executables, besides those described below: `--width`/`--height` (the window, default
  1280x720), `--fullscreen` (borderless), `--no-vsync`, `--gl-selftest` (provokes one GL_INVALID_ENUM to prove that
  OpenGL errors reach the log; a `--smoke` run still passes when it is the only one), `--asset-root <dir>` (else
  `RV_ASSET_ROOT`, the source tree in developer builds, `<exe>/assets`), `--data-dir <dir>` or `--portable` (user data
  in `<exe>/RetroverseData`, else `%LOCALAPPDATA%\Retroverse`), `--log-level trace|debug|info|warn|error`,
  `--no-dialogs` (implied by every frame-limited run), `--version`, `--exit` (alias of `--exit-after-frames`).
- `--run-report <file.json>` (both executables) writes a JSON report when the run ends, also when it fails
  (`rv/app/run_report.hpp`): version and build type, overlay and debug output built in, the asset root and its source,
  exit code, outcome and first failure, `openGlUnavailable` (no OpenGL 4.5 core context), the GL vendor, renderer and
  version (the driver), debug messages per severity, frames, the last view's statistics (draw calls, batches,
  instances, triangles, culled items, shadow and depth draw calls, GPU times), the instance buffer's capacity, the
  screenshot written, the walker's final pose (feet, yaw, pitch as `--spawn` takes them) and the self-check. Read it
  instead of the log in scripts and tests.
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
- `--spawn x,y,z[,yaw[,pitch]]` starts the walker with its feet at (x, y, z) meters (the eye 1.65 m above), looking
  along yaw and pitch degrees (0 looks at -Z, positive yaw turns right; the scene camera's when left out), instead of at
  the scene camera; the final camera that frame-limited runs log reads the same way. The walk scripts start at `--spawn
  0,0,3.2,0,-6`, on the way in of the test room, so they do not depend on its scene camera.
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
unit tests in `tests/unit/integration/test_free_roam_navigation.cpp` play the same scripts):

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
  bounds, pivot axes, socket axes with their names, ScreenSurface outlines, collision proxy outlines, vertex normals.
  The labels are drawn by the overlay, kept clear of each other and of the labelled points: in screenshots they line
  up with the scene when `--screenshot-size` has the window's aspect (1920x1080 for the default 1280x720 window). The
  inspection views (base color, normals, uv checker) show their values exactly (no tonemap, no FXAA) on the lit view's
  background.
- Panels: Asset, Model (node tree with each primitive's triangles, vertices and material, or a prefab's parts and
  sockets), Statistics (bounds in cm, primitives as potential draw calls, triangles, vertices, textures with estimated
  GPU memory, glTF extensions), Materials (factors and texture thumbnails; glTF materials listed by their own name,
  then the model file), Manifest (its fields and the `validatePrefabManifest` results; brought forward when a manifest
  comes with a new target or appears next to the model as `<name>.manifest.json` or `<name>.json`, not on reloads nor
  over `--panel`), Screen (each ScreenSurface with its size as placed, node scale included, and the color bars it shows:
  glTF screens are drawn with `MAT_Screen_CRT` and the pattern instead of their own material), Stats, Warnings, Log.
  Screenshots include the panels from the third frame on (`--frames 3` or more; an earlier `--screenshot` logs a
  warning).

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

## Layout and layering

- `src/<module>/include/rv/<module>/*.hpp` public headers, `src/<module>/src/*.cpp`, `src/<module>/CMakeLists.txt`
  with an explicit source list. Include as `#include <rv/<module>/<file>.hpp>`.
- Dependency order: `core` -> `geometry` -> `input` -> `assets` -> `scene` (GPU-free; `rv_forbid_link` fails the
  configure if they reach SDL3, glad or ImGui) -> `platform` (SDL3) -> `render` (OpenGL through glad; never
  includes SDL) -> `devui` (Dear ImGui, developer builds; its GPU-free part `rv_devui_model` is always built) -> `app`
  (composition root) -> `retroverse.exe`. Code for the overlay in `rv_app` sits under `#if defined(RV_DEV_UI)`.
- `tools/asset-preview`: `retroverse-asset-preview.exe` on `rv_app` and `rv_devui` (built only with `RV_DEV_UI`), and
  its GPU-free core `rv_preview_core` (`include/rv/preview/*.hpp`, namespace `rv::preview`), built in every
  configuration and tested by `tests/unit/preview`.
- `tools/screenshot-diff`: `retroverse-screenshot-diff.exe` and its GPU-free core `rv_screenshot_diff_core`
  (`include/rv/screenshot_diff/*.hpp`, namespace `rv::screenshot_diff`), built in every configuration, tested by
  `tests/unit/screenshot_diff` and linked by the GPU smoke tests.
- Tests: `tests/unit/<module>/test_*.cpp` (globbed into `rv_tests`, label `unit`, no SDL or OpenGL; GLB files for
  tests come from `rv/assets/glb_writer.hpp`),
  `tests/platform/test_*.cpp` (globbed into `rv_platform_tests`, label `unit`: SDL platform layer, no OpenGL),
  `tests/support/*` helpers (globbed into `rv_test_support`), `tests/input_scripts` (scripted verification runs),
  `tests/smoke/*.cpp` GPU smoke tests (globbed into `rv_smoke_tests`, label `gpu`: GPU-free itself, it starts the
  executables of its build and reads their run reports and screenshots; `smoke_support.hpp` runs them,
  `image_checks.hpp` measures screenshots; Win32 child processes in `child_process.hpp`) and their test scenes in
  `tests/smoke/scenes`.
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
