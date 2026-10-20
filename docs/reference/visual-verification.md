# Visual verification

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

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

The Gate B goldens (I3 step 17a, `tests/smoke/test_golden_room.cpp`, `gpu.golden/G02` ... `G10`, `[golden_room]`)
sit beside G01 in `assets/style_tests/screenshots/`, each with a JSON like G01's (the GPU and driver, the commit, the
SHA-256 of the image, of the fixture's tree, of the plan shown, of the input script and of `assets/world`, the view and
every command of the run): G02 (`g02_snes_room_golden.png`: `snes-room`'s 50 titles, all face-out, from the doorway),
G02s (`snes-stress`: spine-out modules, the hero band, the display cases), G06 (`snes-single`: one ROM, no media, a
generated label), G07 (an inspected cartridge with its card, `golden_g07_inspect.json`), G08a (the CRT's clip frozen
at 1.0 s), G08b (the corrupt clip's fallback to the cover, `golden_g08b_corrupt_clip.json`) and G10 (G02 after the ten
`additions/` imported; also compared with G02's golden: every differing pixel on the level the ten took, heatmap
`g10_against_g02_heatmap.png`). Each case imports its fixture, makes the plan and fills the cache in a first run of
300 frames, then renders the stored room hidden and lockstep at 1920 x 1080 (`--video-freeze-pts 1` but for G08b),
checks the run report and compares within G01's tolerance (`comparison.txt` and a heatmap in
`build/<preset>/smoke/golden_room/<case>`), skipped on another GPU or driver. `$env:RV_UPDATE_GOLDEN=1;
build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden/G02:*"` rewrites one (then the case fails: look at it, run it
again without the variable, update `docs/testing/gate-b.md`'s checklist). The room's own screenshots: a data directory
with a fixture recorded and imported (`retroverse-cli --data-dir <dir> library create --home <materialized fixture>
--install <fixture>/Programs/ES-DE`, then `import`), then `retroverse.exe --exit-after-frames --frames 30 --fixed-dt
0.016667 --hidden --data-dir <dir> --screenshot build/screenshots/room.png` (the doorway); the `gpu.world*` cases leave
theirs under `build/<preset>/smoke/world*/`. The benchmark path: `--width 1920 --height 1080 --no-vsync
--exit-after-frames --frames 1270 --fixed-dt 0.016667 --input-script tests/input_scripts/bench_snes_room.json` from the
room's scene camera (the `benchmark` phase of the run report, with its GPU passes).

Another family's room with its items and covers (I4 step 2): a data directory with that system's catalog, then
`retroverse.exe --room-system psx --exit-after-frames --frames 40 --fixed-dt 0.016667 --hidden --data-dir <dir>
--screenshot build/screenshots/psx.png` (with `--spawn -2.45,0,-3.9,0,-8` a hero-wall module up close, `--spawn
-3.3,0,-4.0,-90,-12` the first west side-wall module, `--spawn -0.25,0,1.0,-90,-38` the west display case); the
`esde/building` fixture's systems or any invented library under `build/`.

A generated room's static part is checked as a scene: `retroverse-cli --data-dir build/rvdata worldplan show
tests/fixtures/world/golden/plan-890-seed-102938.json --scene build/scenes/room_890.json`, then `retroverse.exe --scene
build/scenes/room_890.json` as above (the shell, the hero corner, the modules, displays, dressing and lights, without
the cartridges). The collection's arrays and static groups show at a glance with
`build/msvc-x64-debug/bin/rv_collection_probe.exe --probe-scenario chunks --exit-after-frames --frames 240 --hidden
--screenshot build/screenshots/arrays.png --data-dir build/rvdata` (`--probe-scenario groups --probe-camera close
--probe-highlight` for the outline).

To check shader errors or hot reload, never edit the tracked shaders: copy `assets/` to a scratch directory, run a
developer build with `--asset-root <copy>` and a long frame limit (`--hidden --exit-after-frames --frames 3000`),
then edit or break the copy's shaders while it runs; the log shows each rebuild or `<file>:<line>: error ...`.

