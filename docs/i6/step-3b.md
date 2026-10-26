# I6 step 3b: era furniture

Roadmap row: I6 step 3b. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §1, amended on 2026-10-25 (a
pack chooses furniture variants within the footprint of the part or socket they fill). The direction:
[look.md](look.md) ("Furniture and dressing"). Step 3's look review found every pack with the same armchair, red rug,
prints and sideboards; this step gives each era its own.

## What was built

- **Seven procedural prefabs** (`rv/geometry/prefabs.hpp`, each with its manifest, family and test): `sofa` (the
  armchair's build with 1 to 4 seat and back cushions, throw pillows), `beanbag`, `desk` (a drawer pedestal and a
  generic home computer: system unit, monitor with a `screen`, keyboard), `office_chair`, `bar_stool`, `banker_lamp`
  (a green half-tube shade on a small two-drawer stand, its bulb where a floor lamp's is), `showcase` (a glass vitrine
  on a two-door base, a generic console or upright handheld inside); `floor_lamp` gains `shade: "torchiere"` (the 90s
  uplighter). Every pack dresses the new kinds (the theme test's "every kind of the built-in pack").
- **Rugs and pictures** (`rv_assets`): the ornamental rug's generator takes a palette and a layout:
  `ornate_rug_blue`, `ornate_rug_sage`, `ornate_rug_green`, the arcade's `runner_rug` (medallions along 1.0 x 2.5 m)
  and the 90s `geometric_rug` (confetti on charcoal), 1152 x 768 (the runner 512 x 1280), `MAT_Carpet_*`;
  `ornate_rug` byte for byte as before. New scene patterns: `poster_memphis`, `poster_lounge`, `poster_console`,
  `poster_flyer`, `cork_board`, `terminal` (the desk's screen).
- **The packs' furniture** (`room_look.hpp`'s `"furniture"`, `ThemeFurniture`): pieces by hero corner part role (in
  the hero socket's frame, the part's place when left out; several pieces: a desk and its chair, two stools) and
  weighted variants by decoration kind. `scene_builder.cpp` draws them: hero pieces at their place and the part's
  height; a decoration's variant drawn by weight (`deriveSeed(roomSeed, "furniture", fixture)`), its back on the
  socket's back edge and centred as the plan's prefab stood. The plan, its fixtures, slots and lights are untouched;
  `drawnFixtures()` gives the boxes the pieces cover. `resolveSceneFurniture()` leaves out (logged) a piece that does
  not resolve or build: its part is drawn as planned.
- **Per pack** (as the review left them, below): classic and the hub unchanged. 90s collector: two bean bags,
  torchieres, the geometric rug, confetti posters, lit oak showcases with a console. Optical era: a two-seat leather
  sofa across the rug's south edge facing the TV, the blue rug, large framed cinematic posters (0.68 x 1.0 m), lit
  walnut showcases. Handheld archive: a one-seat lounge chair on tall legs, the sage rug, console-art posters, low lit
  showcases with a handheld. Computer studio: a desk side-on along the corner's east edge with the computer and a
  swivel chair facing it, banker's lamps on card catalogues, the green rug, cork boards and prints half and half, lit
  showcases of big-box software for the sideboards. Arcade: two red bar stools by the TV's console side, the dark red
  runner, flyers, graphite-framed showcases of big-box software.
- **`decorationWeights` dropped**: step 3 read them and nothing used them; they could not choose decoration (a plan
  fact). The weights are now the decoration variants' own (`"weight"`, 0 to 1; the reader refuses the old key).

## Proof

- `tests/unit/world/test_era_furniture.cpp`: each pack's pieces resolve and build; the reader's refusals; five golden
  plans drawn with each of the seven packs and every room of the golden building with its floor's pack: every piece
  inside its decoration socket (on the floor and under the ceiling), or inside the hero corner (the lamp inside its
  own part's footprint), floor coverings staying floor coverings, colliders inside the bounds; `validatePlan()` of the
  plan holding the drawn fixtures: no hard finding and the same soft findings (the navigation's reach and main
  passages, the TV's line of sight); every lamp variant's bulb within 0.15 m of its plan light. The test caught the
  first sofa and bean bags closing the 1.4 m passage to the CRT, and the first wide posters leaving the 0.7 m corner
  sockets; both were moved or narrowed.
- Plan goldens untouched; `building-esde-seed-102938.floors.json` rewritten on purpose (computer, gen5, ground with
  the arcade, handheld; gen4, the SNES floor, unchanged); the golden plans' scene digests unchanged (classic). G12 and
  G13 regenerated on purpose (Debug); G01-G11 unchanged (the classic SNES rooms; G11's arcade doorway within tolerance).
- Procedural surfaces 123.0 MiB (80.3 + 42.7 for the five rugs), generated at a world run's start and held for the
  whole run (`docs/reference/running-the-app.md`): about 0.9 s in Release, the rugs 0.5 s of it (the geometric rug
  0.24 s), before the first room, so no ride waits for one. The busiest floor of the real library measured 407 MiB in
  I4 step 7b; with the rugs about 450 MiB, within the acceptable 512 MiB (not measured again on the real library: the
  step does not touch it).

## Screenshots and judgement

`build/i6/step3b` (`shots.sh` and `compare.py` in `build/i6/step3b-work`; the data and the ps2 scene as step 3's): each
room from its doorway and from its hero corner (`*_corner.png`), `compare_<pack>.png` beside the concept's panel. Each
era now reads by its furniture: the 90s room's purple and teal bean bags on a confetti rug under torchieres, the
optical lounge's leather sofa on a blue rug with large dark posters and lit showcases (panel 4's furniture), the
handheld archive's console-art and small showcases (panel 6), the study's desk with a green-screen computer, swivel
chair, banker's lamps and cork boards, the arcade's stools on a red runner (panel 5). Measured with step 3's
`table.py` (the doorway frames at 768 x 432):

| Room | median luma | calm / colour / accent % | warm / red / magenta / blue / cyan / green / grey % |
|---|---|---|---|
| classic_16bit_warm (step 3 -> 3b) | 37.1 -> 37.1 | 93.9 / 5.3 / 0.8 -> same | 69 / 6 / 2 / 0 / 0 / 0 / 22 -> same |
| console_90s_collector | 47.8 -> 47.4 | 96.3 / 2.7 / 1.0 -> 96.2 / 3.0 / 0.9 | 80 / 4 / 0 / 1 / 1 / 0 / 14 -> 80 / 4 / 1 / 0 / 1 / 0 / 14 |
| optical_era_lounge | 30.4 -> 30.5 | 97.7 / 1.6 / 0.8 -> 97.7 / 1.5 / 0.8 | 48 / 0 / 0 / 6 / 0 / 0 / 45 -> 48 / 0 / 0 / 7 / 0 / 0 / 45 |
| handheld_archive (shared) | 42.3 -> 42.5 | 97.8 / 1.0 / 1.1 -> 98.2 / 0.6 / 1.2 | 86 / 0 / 0 / 0 / 0 / 3 / 10 -> 87 / 0 / 0 / 0 / 0 / 3 / 10 |
| computer_studio | 50.6 -> 49.0 | 97.5 / 1.4 / 1.0 -> 98.0 / 1.0 / 1.1 | 79 / 0 / 0 / 0 / 0 / 4 / 15 -> 80 / 0 / 0 / 0 / 0 / 4 / 14 |
| arcade_after_dark | 25.1 -> 26.2 | 86.5 / 12.3 / 1.3 -> 85.6 / 13.1 / 1.2 | 31 / 12 / 14 / 1 / 0 / 0 / 41 -> 32 / 12 / 14 / 1 / 0 / 0 / 40 |

From the hero corner (`*_corner.png`) colour / accent: classic 12.4 / 1.5, 90s 6.9 / 1.6 (cyan 5, magenta 4), optical
2.4 / 1.5 (blue 7), handheld 0.8 / 2.1, computer 2.1 / 1.4, arcade 29.7 / 2.9. The shares from the doorway barely move:
the furniture is a small part of the frame and was chosen for shape, not colour (style §1.2); the colour gap to the
concept (its posters, screens and signs) stays as step 3 measured it.

## The step's review

The look review (`build/i6/review3b`) found the optical sofa turned away from the TV, its back hiding the TV cabinet;
the study's desk in front of the CRT, its monitor covering it; the banker's lamps on tall dark chests; the handheld
lounge chair a slate box from behind; the showcases reading as open cabinets; the bean bags as two stacked glossy
spheres; the arcade's stools in the middle of the room. Fixed:

- **Optical sofa**: across the rug's south edge facing the TV (x -150, z 2560 in the hero corner's frame, yaw 180), its
  back 0.74 m so the screen stays clear of it from the doorway (concept panel 4).
- **Study desk**: 1.0 x 0.6 m side-on along the hero corner's east edge (x 880, z 2200, yaw 270), the chair facing
  it from the west (x 430, z 2300). The reviewer's z 1900 closed the main passage to the CRT's interaction point (the
  test's soft finding `passage-narrow`): 300 mm further south it stays as it was.
- **Banker's lamp**: the stand is a card catalogue, three columns of six drawers, each with a brass label holder and a
  pull; the study's drawers oak in a walnut carcass.
- **Handheld lounge chair**: a one-seat sofa (`sofa` now from 0.7 m wide) on 0.2 m legs, its back cushion apart, no
  pillows: a chair from the doorway, still a plain upholstered back from behind (BACKLOG).
- **Showcases**: a lit strip under the cap (the kind's `board_light`, `ShowcaseParams::boardLight`, as a lit shelf's),
  the plinth 0.08 m higher, an item never drawn under 0.9 of its size (a smaller vitrine is refused), and a new item
  `boxes`: up to three upright big boxes of software with their cover (role `cover`, the piece's texture pattern
  `cover`). The computer studio and the arcade draw their sideboards as showcases of boxes, the arcade's framed in
  graphite. Every pack's showcase kind names `board_light` and `cover` (the built-in pack's too).
- **Bean bags**: a lower body (0.5 of the height) under a lump as wide as it and deeper (0.6), the purple vinyl's
  roughness 0.45 -> 0.6. Still two ellipsoids, now matte (BACKLOG).
- **Arcade stools**: by the TV's console side (x 640 and 1020, z 750; seat and base radius 0.18 to stay in the hero
  corner) until step 6's cabinets.
- **The runner's material** is its piece's (`"materials": {"fabric": "MAT_Carpet_Runner"}`); the arcade's rug kind
  keeps `MAT_Carpet_Ornate`, so a plan's rug drawn as planned (its piece left out) is never a stretched runner.
- **A runtime guard** (`drawnPieces`): a decoration's variant that leaves its socket's footprint (turned with the
  socket), or a hero part's piece that leaves the hero corner (with the planned part's own floor where the part
  reaches out of the corner: the lamp beside the TV), is not drawn; the plan's prefab is, a warning logged once per
  pack, fixture and piece. Tested with a socket narrowed under its variant and a sofa moved out of the corner.
- **The reader refuses** furniture for the hero roles `crt`, `sign` and `console` (the room's video, its neon and its
  system's console).
- `test_era_furniture`: `validatePlan()` of a plan once, not per pack (and none again when nothing is replaced): the
  golden building's case 4.5 s in Debug, the file's five cases 20.8 s (the five golden plans
  with the seven packs 15.5 s of it).
- Floor scene digests rewritten again (computer, gen5, ground, handheld; gen4 unchanged); G12 and G13 regenerated on
  purpose (Debug: G12's bean bags and showcases, G13's lounge chair) then compared clean; G11 within tolerance.
- Tests: the whole unit suite in Debug and Release (1,912 cases each), every GPU case in Release (108, one skipped:
  the developer overlay's pick), every golden in Debug (G01 to G13); the benchmark path in Release p50 3.45 ms, p99
  4.86 ms, 303.6 MiB at its peak (acceptable: 16.7 ms, 33.3 ms, 512 MiB); Debug and Release built without a warning.
- Not done: the rugs' metallic-roughness maps at a quarter of their size (about 20 MiB saved): optional, it needs a
  map size per surface map; in BACKLOG.

Screenshots after the review in `build/i6/step3b` (`build/i6/step3b-before-review` before it; the same `shots.sh`).
From the doorway the shares barely move again (the 90s room 96.1 / 3.0 / 0.9, the optical 97.9 / 1.3 / 0.7, the
study 97.9 / 1.0 / 1.1, the arcade 85.7 / 13.0 / 1.3); what changes is what the furniture says: the lounge's sofa
faces the TV with its screen clear above the back, the study's CRT stands free beside the desk and its computer, its
lamps on card catalogues and its sideboards showcases of software boxes, the handheld chair stands on legs, the
showcases are lit inside with the console or handheld standing in the glass's middle, the arcade's stools wait by the
TV and its sideboards show boxes in graphite frames. Left as they are: the bean bags still read as two soft lumps, the
lounge chair's back from behind.

## Deviations and follow-ups

- The variants fill the hero corner and the decoration sockets only: no new socket, so no counter showcase row
  (step 6), no bookcase of boxes for the study, no coffee table. BACKLOG.
- The handheld archive's lounge chair is a one-seat sofa on tall legs: from the doorway a chair, from behind still a
  plain upholstered back (a lounge chair of its own, its back thinner than its arms, stays in BACKLOG).
- A piece is placed by data per template part; a second template would need its own placements (the test judges every
  plan drawn, so a misplaced piece fails it).
