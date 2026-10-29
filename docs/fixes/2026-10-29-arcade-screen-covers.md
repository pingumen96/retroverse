# Fix of 2026-10-29: an arcade cabinet moving when aimed at, covers drawn rotated

Two bugs the user reported after trying the real library. Decision:
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §3, amended on 2026-10-29 (fix).

## 1. "Aiming at the arcade's screen, it should not move"

**What moved:** the cabinet's screen itself. The arcade's items are its cabinets' screens (the family
`arcade_cabinet`, I6 step 6), drawn face-out, so they took the face-out pull-out of ADR-0012 §3: 150 ms after the
reticle rested on a screen, the screen tipped 10 degrees about its bottom front edge, its top coming out of the
bezel towards the viewer (the run report's events: `hover` on frame 2, `pull-out` on frame 10, `pulled` on frame 24),
and it stood so in Focus after Indietro. Not the video front (the dynamic path draws the clip at the item's own
transform: across attract's swap from the cover to the clip only the screen's pixels change), not attract's switching
(it starts after a 1 s dwell whether aimed at or not), not the CRT's curvature (one material for the cover and the
clip).

**The fix:** an item that is part of its fixture (`world::CollectionItem::inFixture`, set for a family facing front:
the cabinets' screens) never moves in the room. `app::pulledItemTransform()` keeps it at its static transform at
every progress, and the hover's dwell no longer pulls it (`RoomItem::inFixture`): aimed at, the screen keeps its
outline and title label and nothing else changes. Focus after Indietro shows it where it stands (its tipped pose was
what jumped: the screen sticking out of its bezel), the walker easing as before. Inspect still lifts it into the hand.

## 2. "Sometimes the games' covers are rotated; they must be upright"

**What rotated:** every cartridge standing on its end, once pulled out. A spine-out cartridge (the cartridge
families: `cartridge_home_8bit`, `_16bit`, `_64bit`, `cartridge_handheld`) stands with its top face out and its long
side up; pulled out after the hover's dwell or in Focus it turned a quarter about the vertical and showed its cover
sideways, its picture's top to the left (I3 step 13a's decision 1: "the spine-out cartridge stays on its end when it
turns"). The same system's face-out cartridges (the hero band, the display cases) stood upright beside it: some
covers upright, others rotated. The cases and boxes standing on their side (the `*_v2` families of I6 step 4) were
upright already; the cabinets, the display cases, the counters, the hand (Inspect holds the item upright), the
card's picture and the quick catalog never rotated a cover.

**The fix:** while it turns, a cartridge on its end rolls a quarter about the view's axis
(`world_focus.cpp`'s `uprightRoll()`: the roll that brings the item's +Y up once its front faces the viewer; 0 for an
item on its side), its cover upright and not mirrored. A roll about the view's axis moves nothing along the slot's
depth, so the turn's clearance holds; its corners sweep past the boards above and below, which stand a little proud
of the slots, so `rowReach()` counts what the roll sweeps and the cartridge settles a little farther out (80.6% of
its depth instead of 70%; the 2,000-title plan's every pull checked against its neighbours and boards at 50 steps).

**EXIF orientation, checked and fixed as well:** stb_image ignores a file's orientation tag, so a cover whose pixels
a camera or an editor stored turned would draw turned. The user's media do not do this. A read-only scan of
the media folder (every `covers`, `backcovers`, `3dboxes`, `physicalmedia`, `screenshots`, `titlescreens`,
`miximages`, `marquees` and `fanart` file: 309,168 PNG and JPEG files, 5,154 with an EXIF block; among them the
36,386 covers, 139 with an EXIF block) found **no** orientation other than 1. So EXIF does not explain the report. The decoder now honours the tag anyway for the
user's media (`assets::decodeUprightImage()`: `exifOrientation()` reads a JPEG's APP1 "Exif" segment or a PNG's eXIf
chunk, `orientImage()` applies tags 2 to 8), used by the front layers (`buildCoverLayer()`), the hero pictures (the
hand and the card), the screens' stills and the media viewer. The bundled assets and glTF images keep their rows as
encoded (ADR-0002). `kCoverPipelineVersion` is 3, so a cover cached before the fix is made again.

On the real library (the stored plan of `build/msvc-x64-release/real-library/building`, 19,985 placements), the
pulled covers that drew sideways: **8,913** cartridges spine-out (3,929 handheld, 3,410 8-bit, 1,557 16-bit, 17
64-bit), against 1,247 of the same families face-out, always upright. The covers' own shapes were not the cause:
the 576 landscape Japanese Game Boy Advance boxes (680 x 423) and the 120 portrait Super Famicom boxes and re-release
pictures (476 x 864) among the SNES covers are upright in their files and are fitted with bands, never turned.

## Proof

- Unit: `test_image_orientation.cpp` (6 cases: `orientImage()` against EXIF's table for each tag, a generated PNG
  with an eXIf chunk and a generated JPEG with an EXIF segment for each tag in both byte orders, a cover's front
  layer fitted upright, missing or broken tags read as 1); `test_world_focus.cpp`: every spine-out item of the golden
  plans and the generated psx and amiga rooms pulled out with its +Y up and its +X to the viewer's right, never
  through its neighbours or boards; an arcade room's screens never moved by the pull, the hover never pulling one,
  Focus after Indietro at its static pose.
- GPU (`test_cabinets.cpp`, `[aim]` and `[covers]`): aimed at a cabinet from its Focus pose, screenshots at frames 6,
  40 and 80 identical (0 pixels changed) and no pull-out before the interact; Focus after Indietro 0 pixels from free
  roam; attract's swap from the cover to the clip changes the screen's box only (0 pixels outside it); the mame
  covers rewritten as PNGs stored turned with EXIF orientation 6 draw exactly as the same pictures stored upright, on
  the cabinet and in the hand with its card (0 pixels). `test_world_focus.cpp`'s spine-out case: the pulled
  cartridge outlined wider than tall.
- Goldens: G20 (`g20_cabinet_focus_golden.png`, a cabinet in Focus) rewritten on purpose: its screen no longer tips
  out of its bezel. No other golden changed.

## Results

- Debug (`./scripts/test.ps1`): 2,179 unit tests, all passed.
- Release (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2,339 tests (2,179 unit, 160 GPU). The first run
  passed its first 2,277; from test 2,270 on, every child process failed to start (`0xc000026b`, a DLL's
  initialisation failing machine-wide while the session was being restarted, not a test's fault). Those 62 run again
  (`ctest --rerun-failed`): all passed but the overlay's pick, skipped as always. The new cases (`[aim]`, `[covers]`),
  G07 and G20 passed in Debug and Release.
- Goldens: G20 rewritten on purpose (Debug, within the tolerance in Release); no other golden changed.

## Screenshots

`build/fix/arcade-covers/` (hidden, frame-limited, Debug, the building fixture; never committed):

- `before/before_1.png`, `before/before_2.png`: a cabinet aimed at from its Focus pose, frames 4 and 40 before the
  fix: at 40 its screen stands tipped out of its bezel, bigger and its top nearer (8.1% of the pixels changed);
- `after/aim_1.png` to `after/aim_4.png`: the same view after the fix at frames 6, 40 and 80 and in Focus after
  Indietro: identical (0 pixels);
- `after/aim_attract_1.png`, `after/aim_attract_2.png`: attract's cover, then its clip, on the cabinet in front of the
  walker: only the screen changed;
- `before/spine_focus_before.png`: a spine-out cartridge in Focus before the fix, its cover sideways (the title reading
  upwards) under the band's upright covers; `after/spine_focus_after.png`: the same after the fix, upright;
- `after/covers_upright_1.png`, `_2.png` and `after/covers_turned_1.png`, `_2.png`: the cabinet's cover, then the item
  in the hand with its card, from pictures stored upright and from the same pictures stored turned with EXIF
  orientation 6: identical.

Beside style.md and `example.png`: the cabinet reads as one solid piece whose screen glows inside its bezel, as a real
cabinet's does, the outline and title label alone marking the aim; the pulled cartridge shows its cover the right way
up, as the face-out band and the hand show it.
