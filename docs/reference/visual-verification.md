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
300 frames, then renders the stored room hidden and lockstep at 1920 x 1080 (`--start-room room:snes`, in the room's
own frame since I4 step 5a, so the floor around it changes no pixel; `--video-freeze-pts 1` but for G08b),
checks the run report and compares within G01's tolerance (`comparison.txt` and a heatmap in
`build/<preset>/smoke/golden_room/<case>`), skipped on another GPU or driver. The Gate C goldens of the building (I4
step 8, the same file, `gpu.golden/G11` ... `G13`) are made the same way from `esde/building` under the shipped
threshold (no `world.json`): G11 (`g11_lobby_golden.png`: the ground floor's hub from the elevator's arrival point,
no `--start-room`; since I6 step 5 the hub, and G14, `g14_hub_entrance_golden.png`, the hub from its entrance,
`--spawn 16.3,0,0.6,-92,-3`), G12 (`g12_jewel_case_room_golden.png`: `--start-room room:psx`, the jewel cases of
`disc_case_cd_v2` from the doorway, every one face-out) and G13 (`g13_shared_room_golden.png`: `--start-room
area:shared:handheld`, the handheld floor's shared room of two sections, generic cases of `generic_case_v2`, from the
doorway). I6 step 6 adds G15 (`g15_arcade_room_golden.png`: `--start-room area:arcade`, the arcade room's rows of
cabinets of `arcade_cabinet`, the hero cabinet carrying the room's clip held at 1.0 s, from the doorway) and G16
(`g16_handheld_room_golden.png`: `--start-room room:gb`, the gb room's counter showcases and stations, from the
doorway); the arcade's views of the step (the doorway, the north wall from the room's middle `--spawn 0,0,0.8,0,-3`,
the hero cabinet `--spawn -0.625,0,-2.4,0,-6`, a west row `--spawn -2.3,0,-2.6,-90,-6`, the aisle's pairs `--spawn
0.3,0,1.0,-90,-8`) and the gb room's counter up close (`--spawn 0.4,0,1.0,-90,-28`) are in `docs/i6/step-6.md`;
the arcade's cabinets up close and a launch from a cabinet's screen are `gpu.world families: the arcade room's
cabinets ...` and `... a game launched from its cabinet's screen ...`. Gate D (I6 step 7) adds G17
(`g17_optical_era_room_golden.png`: `esde/case-shelves`, `--start-room room:ps2`, the optical-era lounge's room on gen6
with its DVD keep cases, from the doorway) and G18 (`g18_handheld_hall_golden.png`: `esde/building`, `--start-room
room:gb --spawn 3.85,0,6.65,270,-4`, the handheld floor's hall from east of its doorways looking back to the elevator:
`--spawn` is in the start room's frame, here the floor's x 11.5 m on the hall's centre line). I5 step 4 adds G20
(`g20_cabinet_focus_golden.png`: `esde/building`, `--start-room area:arcade`, the walker at the Focus pose of the
cabinet of the fixture's one mame clip, `tests/input_scripts/golden_g20_cabinet_focus.json`: inspected, then in Focus
after Indietro, its clip held at 1.0 s and letterboxed on its portrait screen, the bars the tube's off-black since the
step's review). I5 step 3 adds G19 (`g19_secondary_tv_golden.png`: `esde/snes-room`, `--start-room room:snes`, the
walker in front of the middle between the hero CRT and the room's secondary TV, both playing their clips held at
1.0 s, the secondary's playlist one game through `video.json`'s per-screen keys). The room of 14 m the generator grew
(its one secondary TV beside the doorway since the step's review) is `rv_tests "[.secondary_tv_look]"`, which writes
`build/i5/step3/grown-room.scene.json` for `--scene`. The cases' upright
spines up close are `gpu.world families: jewel cases stand on their side` (the
`esde/case-shelves` fixture: 160 jewel cases, 100 keep cases, 120 big boxes, most of them spine-out, their dark shells
and full-height printed spines since the step's review) and `gpu.world families: a stored building of generator 1.3 is
migrated once` (`tests/fixtures/world/legacy/`). `$env:RV_UPDATE_GOLDEN=1;
build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden/G02:*"` rewrites one (then the case fails: look at it, run it
again without the variable, update `docs/testing/gate-b.md`'s or `gate-c.md`'s page). The room's own screenshots: a data directory
with a fixture recorded and imported (`retroverse-cli --data-dir <dir> library create --home <materialized fixture>
--install <fixture>/Programs/ES-DE`, then `import`), then `retroverse.exe --start-room room:snes --exit-after-frames
--frames 30 --fixed-dt 0.016667 --hidden --data-dir <dir> --screenshot build/screenshots/room.png` (the doorway;
without `--start-room` the run starts in the ground floor's hub, at the elevator); the `gpu.world*` cases leave
theirs under `build/<preset>/smoke/world*/`. The benchmark path: `--start-room room:snes --width 1920 --height 1080
--no-vsync --exit-after-frames --frames 1270 --fixed-dt 0.016667 --input-script
tests/input_scripts/bench_snes_room.json` from the room's scene camera (the `benchmark` phase of the run report, with
its GPU passes).

Another family's room with its items and covers (I4 step 2): a data directory with that system's catalog, then
`retroverse.exe --start-room room:psx --exit-after-frames --frames 40 --fixed-dt 0.016667 --hidden --data-dir <dir>
--screenshot build/screenshots/psx.png` (with `--spawn -2.45,0,-3.9,0,-8` a hero-wall module up close, `--spawn
-3.3,0,-4.0,-90,-12` the first west side-wall module, `--spawn -0.25,0,1.0,-90,-38` the west display case; the poses
are in the start room's frame); the `esde/building` fixture's systems or any invented library under `build/`. The
start room's floor is drawn around it (I4 step 5a): its other rooms' collections and covers too, and the hall.

A generated room's static part is checked as a scene: `retroverse-cli --data-dir build/rvdata worldplan show
tests/fixtures/world/golden/plan-890-seed-102938.json --scene build/scenes/room_890.json`, then `retroverse.exe --scene
build/scenes/room_890.json` as above (the shell, the hero corner, the modules, displays, dressing and lights, without
the cartridges). The collection's arrays and static groups show at a glance with
`build/msvc-x64-debug/bin/rv_collection_probe.exe --probe-scenario chunks --exit-after-frames --frames 240 --hidden
--screenshot build/screenshots/arrays.png --data-dir build/rvdata` (`--probe-scenario groups --probe-camera close
--probe-highlight` for the outline).

A floor of the building (I4 step 3) is checked the same way: a data directory with the building fixture recorded and
imported (`tests/fixtures/esde/building`, materialized by the tests; the GPU case `gpu.world floor: ...` leaves one in
`build/<preset>/smoke/world_floor/data`), `retroverse-cli --data-dir <dir> worldplan generate --seed 102938`, then
`worldplan show --floor gen4 --scene build/scenes/gen4.json` and `retroverse.exe --scene build/scenes/gen4.json
--exit-after-frames --frames 5 --hidden --screenshot build/screenshots/gen4.png --data-dir build/rvdata`: the hall
from the elevator facing east, room:snes's doorway on its north side, no gap in its walls. The elevator and the floor's
sign: `--spawn 8,0,0,-90,0`; the north-row room from inside, looking out: `--spawn 7.65,0,-4.5,180,0`; the walk from
the elevator into it: `--input-script tests/input_scripts/floor_walk_into_room.json --fixed-dt 0.016667 --frames 255`
(since I6 step 1 each area also packs its neighbours' lights seen through the doorway: from the hall the room reads
lit through its doorway, its key light, washers and sign spot on its hero wall and the hall's fill spot aimed into it,
and from the room the hall beyond it; the upper walls and ceilings warm and dark, never black.
`tests/smoke/test_world_floor.cpp`'s `world_floor_light` case renders every floor of `esde/building` from its
elevator, and each room from the hall and looking out, with the same views with only the neighbours cut
(`*_cut.png`) and as I4 lit them (`*_i4.png`), and views tilted up and down beside the doorway for the leak probes,
in `build/<preset>/smoke/world_floor_light`). Since I6 step 2 the hall is dressed (`docs/i6/step-2.md`): from the
elevator the runner, the benches and plants, the sconces and prints in turn, the beams and the portal of room:snes with
its sign "16-BIT CORNER" above the lintel and on its blade sign; the portal from across the hall: `--spawn 7.65,0,1.1,0,4` (gen4) and
`--spawn 7.65,0,-1.1,180,4` (handheld's south-row room); half way down the hall `--spawn 5,0,0.3,90,-4`, back towards
the elevator `--spawn 11.5,0,0,270,-4`; the step's views of every floor are in `build/i6/step2`. gen4 holds one room; the fixture's floor with rooms on both rows is `handheld` (`worldplan show --floor
handheld --scene build/scenes/handheld.json`): from the elevator, room:gb's doorway on the north side faces, doors
aligned across the 12.3 m hall, that of the floor's shared room `area:shared:handheld` on the south side (I4 step 4: the
sections of pocketlab and pokemini, the sign "MORE GAMES", the CRT on its cabinet without a console); the south-row
room, turned a half turn, from inside looking out: `--spawn 7.65,0,4.5,0,0` (room:gb's hero wall across the hall
through both doorways); the walk into it: `tests/input_scripts/floor_walk_into_south_room.json` (the same flags).
The same floors with their collections (I4 step 5a): `retroverse.exe --data-dir <dir> --exit-after-frames --frames 60
--fixed-dt 0.016667 --hidden --screenshot build/screenshots/lobby.png` starts at the ground floor's elevator (the
poses in the floor's frame, as the scene files'), `--start-room room:gb` at room:gb's doorway on `handheld` (its
covers, and through the doorways those of `area:shared:handheld` across the hall; the poses in room:gb's frame),
`--start-room area:arcade` at the arcade room's doorway on `ground`; `gpu.world floor app: ...` leaves its runs under
`build/<preset>/smoke/world_floor_app/`.

The directory (I4 step 7a; not wired into the app until step 7b) shows from a fixed model through `rv_ui_probe`, over
the test room: `build/msvc-x64-debug/bin/rv_ui_probe.exe --input-script tests/input_scripts/directory_panel.json
--probe-shots build/screenshots/directory --exit-after-frames --frames 240 --hidden --fixed-dt 0.016667 --width 1920
--height 1080 --data-dir build/rvdata` writes `floors.png` (gen4 selected, "Sei qui"), `rooms.png` (gen4's hall and
rooms, room:snes selected, the shared room's five long system names wrapped), `games.png` (the favourites and recent
games, the long title selected and ending in an ellipsis) and `card.png` (a game's card with Avvia and "Mostra nella
stanza", beside the favourites: a long system name after its date); `--width 1280 --height 720` shows the compact
theme. The model is `tests/ui_probe/directory_fixture.hpp`'s (invented systems and titles).

Expansion rooms (I4b step 8, `docs/i4b/step-8.md`): `rv_smoke_tests.exe "world_expansion: *"` plans the
`esde/overflow` fixture fresh (room:amiga and room:amiga:2 facing each other across the computer floor's hall,
area:arcade and area:arcade:2 on the hub) and leaves its runs and screenshots under
`build/<preset>/smoke/world_expansion/<case>/`: `walk/hall-portals.png` (both portals, "HOME COMPUTERS" and "HOME
COMPUTERS 2" on their plaques and blade signs, from 4 m down the hall), `walk/inside-expansion.png` (its neon sign, its
CRT), `walk/inspect-expansion.png`, `chains/arcade-expansion.png` ("ARCADE 2"), `directory/directory-chain.png` (the
computer floor's rooms: the chain with its ranges), `edit/carried-into-expansion.png` and
`edit/holding-over-free-slot.png` (the editor's bar "Modifica stanza · Lumen Desk Computer 2", a game of the head in
the hand), `notice/notice-rooms-added.png` (the lobby's notice naming "Sala giochi arcade 2"). The poses are in the
start room's frame (`--start-room room:amiga`; the test computes them from the plan).

To check shader errors or hot reload, never edit the tracked shaders: copy `assets/` to a scratch directory, run a
developer build with `--asset-root <copy>` and a long frame limit (`--hidden --exit-after-frames --frames 3000`),
then edit or break the copy's shaders while it runs; the log shows each rebuild or `<file>:<line>: error ...`.

