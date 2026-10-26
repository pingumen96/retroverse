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
- **Per pack**: classic and the hub unchanged. 90s collector: two bean bags, torchieres, the geometric rug, confetti
  posters, oak showcases with a console. Optical era: a two-seat leather sofa along the corner's side turned to the TV
  and rug, the blue rug, large framed cinematic posters (0.68 x 1.0 m), walnut showcases. Handheld archive: a small
  lounge chair, the sage rug, console-art posters, low counter showcases with a handheld. Computer studio: a desk with
  the computer and a swivel chair, banker's lamps, the green rug, cork boards and prints half and half. Arcade: two
  red bar stools, the dark red runner, flyers.
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
- Procedural surfaces 123.0 MiB (80.3 + 42.7 for the five rugs).

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

## Deviations and follow-ups

- The variants fill the hero corner and the decoration sockets only: no new socket, so no counter showcase row
  (step 6), no bookcase of boxes for the study, no coffee table; the sideboards of the computer studio and the arcade
  stay the narrow cabinet. BACKLOG.
- The handheld archive's lounge chair is the armchair's prefab with other proportions; from behind it reads as a box.
- A piece is placed by data per template part; a second template would need its own placements (the test judges every
  plan drawn, so a misplaced piece fails it).
