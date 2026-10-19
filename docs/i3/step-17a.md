# I3 step 17a: the Gate B goldens

Roadmap row: I3 step 17a (after 15a, 15b and 16; 17b runs beside it). Style §26.1 (fixed camera and seed, comparable
screenshots, a video frame frozen at a fixed time), the goldens the roadmap names: G02, G02s, G06, G07, G08 (a and b)
and G10. G01 is unchanged (0 pixels apart).

## First: the inspect regression

`gpu.world inspect: the cartridge lifted into the hand, ...` failed since step 14b: Vedi media on the north button
(frame 230) opens the media viewer, so the `ui_back` of frame 260 closed the viewer instead of returning to Focus.
`tests/input_scripts/world_inspect.json` gains a `ui_back` at frame 245 that closes the viewer (Inspect stays); the
state events stay at 40, 260 and 300, and `test_world_inspect.cpp` now also checks the screens' events
(`230:media:card`, `245:closed:card`).

## The goldens

All at 1920 x 1080 from `msvc-x64-debug` on the reference machine (RTX 2080 SUPER, driver 617.42), under
`assets/style_tests/screenshots/`, each with a JSON beside it like G01's: the GPU and driver, the preset and build type,
the commit, the SHA-256 of the image, of the fixture's tree (`tests/fixtures/esde/<name>`), of the plan shown (its digest
and its file), of the input script and of `assets/world`, the view, and every command of the run (`<case>` and
`<fixture>` for the case's and the fixture's folders).

| Golden | Image | Fixture, view | Run |
|---|---|---|---|
| G02 | `g02_snes_room_golden.png` | `snes-room`, 50 titles all face-out; the doorway (the plan's scene camera) | 30 frames |
| G02s | `g02s_snes_stress_golden.png` | `snes-stress`, 2,000 titles: spine-out hero and side-wall modules, the band, the display cases; the doorway | 30 frames |
| G06 | `g06_snes_single_golden.png` | `snes-single`, one ROM and no media: a generated label, the CRT's idle pattern; the doorway | 30 frames |
| G07 | `g07_inspect_card_golden.png` | `snes-room`; the Focus pose of `fx:hw-w/s:5.4`, `golden_g07_inspect.json`: inspected, turned 15 degrees, its card | 90 frames |
| G08a | `g08a_crt_frozen_golden.png` | `snes-room`; 1.6 m in front of the CRT's ScreenSurface: the start choice's clip held at 1.0 s | 30 frames |
| G08b | `g08b_crt_fallback_golden.png` | `snes-room`; G08a's view, `golden_g08b_corrupt_clip.json`: Lilac Loom's corrupt clip falls back to its cover | 240 frames |
| G10 | `g10_snes_room_added_golden.png` | `snes-room` plus `additions/` imported after a first plan; the doorway | 30 frames |

Every run is hidden and lockstep (`--fixed-dt 0.016667`), from a stored plan: a first run of 300 frames generates and
writes the plan and fills the derived-texture cache (on a loaded machine the first frame's wait for the room, 10 s, can
run out; 300 frames leave it time), and the golden's run shows the stored plan's room at frame 0 (checked: `firstFrame`
0, the items in view resident). Every run but G08b's holds the CRT's clip at 1.0 s (`--video-freeze-pts 1`), so the
screen shows the same frame whichever game the CRT plays; G08b runs without it, long enough (240 frames) for the clip to
reach its damage and fall back. The product UI is drawn into the screenshots as always (G07's card, laid out for the
default 1280 x 720 window and scaled uniformly).

## Tests

`tests/smoke/test_golden_room.cpp` (`gpu.golden/G02: ...` to `gpu.golden/G10: ...`, tags `[golden][golden_room]`):
each case materializes its fixture, records it as the library and imports it with `retroverse-cli`, runs the first
run and the golden's run, checks the run report (G02: 50 items, none spine-out, 50 fronts resident; G02s: over 1,000
spine-out; G06: 1 item; G07: Inspect with the lift done and the card on the item's game; G08a: the video picture held
at 1.0 s; G08b: the quick catalog closed, the CRT on the selected game's cover after a `corrupt-data` fallback; G10: 60
items, 10 added, 50 kept, 0 moved) and compares the screenshot with the golden within G01's tolerance (6/255 per
channel, at most 0.5% of the pixels; `comparison.txt` and a heatmap in `build/<preset>/smoke/golden_room/<case>`),
skipped on another GL_RENDERER or GL_VERSION. A plan other than the golden's is warned about before the pixels. G10 is
also compared with G02's golden: every pixel more than 6/255 apart lies on the level of the hero module the ten
additions took (their boxes in height, the module's in width, 24 px of margin: the level's dressing goes with it, ADR-0010
§10); 1,224 pixels differ, all in that level (`g10_against_g02_heatmap.png`).

`RV_UPDATE_GOLDEN=1` rewrites a golden's image and JSON from the run and fails the case (as the golden plans do):

```powershell
$env:RV_UPDATE_GOLDEN=1; build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden/G02:*"; Remove-Item Env:RV_UPDATE_GOLDEN
build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden/G02:*"     # then compare, and look at the image
```

Results (Debug, reference machine, other worktrees building): each golden written, then reproduced by a second run 0
pixels apart (largest difference 0/255); G01 0 pixels apart. `rv_smoke_tests "[world],golden/G01*"`: every case
passed but the two hidden `[.bench]` figures the tag also selects (`covers: the figures of the snes-stress room's
covers` and `world: the figures of the snes-stress room`: 1,710 to 1,998 of 2,000 layers resident under the machine's
load, step 16's note). Unit tests 1,709 of 1,709. Release was not run in this step (the gate runs both).

## What the goldens show (style §26.2, against `example.png`)

- **The display cases are nearly black and heavy** (G02, G02s, G06, G10): from the doorway the two cases fill the lower
  third of the frame as dark boxes, their bodies' luma about 4 of 255 (the floor beside them 63, the upper walls,
  which should fall to black, 10); their tall opaque sides hide most of what they show, only the slanted front levels'
  covers read, and nothing tells glass from wood. Style §4.1 (accent spots on the cases) and the concept's lit, glassy
  cases ask for the opposite: the cases should be among the lit, readable elements. Candidates for a look step:
  an accent spot per case in the profiles, a lighter or lower case body (or glass sides), the lining lit from inside.
- **G02's 50 titles are hard to find from the doorway**: they stand in the cases (hidden as above), the featured wall
  shelf and one band level per hero module; the hero modules otherwise show dressing (paper stacks, boxes, books).
  Expected for a 50-title room (ADR-0010 §3), but with the cases dark the room reads as having few games.
- **G06's single cartridge is barely visible from the doorway**: it stands at the left end of the featured wall shelf's
  top level (`fx:fs01/s:1.0`), a pale blue generated label about 20 px wide beside two storage boxes, nothing marking
  it as the room's one game. The room itself reads as furnished and finished, the CRT on its idle pattern. Style §21's
  P0 room would profit from the one title in the most prominent slot (the band at eye height or a lit case).
- **G07**: the held cartridge at 0.6 m stays small and, from the band's Focus pose, sits in line with its neighbours
  (the band behind it dimmed by one stop), so it reads as "in the shelf" more than "in the hand"; the card is clear and
  its cover matches. The concept holds the case larger and closer; a nearer default distance or a stronger dim is a
  look question for I8.
- **G08a/b**: the CRT reads as a physical object (body, bezel, curvature, the spill on the cabinet); the frozen frame and
  the fallback cover are sharp. The bottom edge of both shows a thin dark band (the armchair's back below the view).
- **G02s** is the closest to the concept: full spine-out shelves on three walls, the sign and the CRT the most
  saturated elements, warm wood; the ceiling and upper walls fall to black.
- No golden shows a real title, cover or trademarked name: the fixtures' invented titles and generated pictures only.

## For the merge

- **CLAUDE.md**, "Visual verification": the Gate B goldens beside G01 (the table above, `test_golden_room.cpp`,
  `RV_UPDATE_GOLDEN=1` to rewrite one, the G10 heatmap against G02); GPU smoke tests: `gpu.golden/G02` ... `G10`
  (`[golden_room]`); input scripts `golden_g07_inspect.json`, `golden_g08b_corrupt_clip.json`.
- **scripts/gate.ps1**: Gate A's golden step runs `-R golden`, which now also runs the seven room goldens (several
  minutes more per preset, G02s's 2,000-title fixture the longest) while its verdict reads G01's `comparison.txt` only;
  step 18 (`-Gate B`) should read `build/<preset>/smoke/golden_room/<case>/comparison.txt` for each, or Gate A should
  filter to `golden/G01`.
- **docs/roadmap.md**: row 17a done (the goldens, both checks; Release left to the gate).
- **docs/testing/gate-b.md** (step 18): the style §26.2 checklist for G02 and G02s can start from the notes above.
- **BACKLOG.md**: "Display cases lit and readable from the doorway" (the first note above); "The held cartridge
  larger in Inspect".
