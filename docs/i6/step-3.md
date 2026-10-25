# I6 step 3: theme packs per floor

Roadmap row: I6 step 3. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §1 (the pack is a look,
resolved at scene build) and §5 (packs per floor). The direction: [look.md](look.md) (the table per floor, the rules).

## What was built

- **Six new packs as data** (`assets/world/themes/`): `console_90s_collector` (gen5), `optical_era_lounge` (gen6,
  gen7), `handheld_archive`, `computer_studio`, `arcade_after_dark` and `retro_library_hub` (the ground floor's hall;
  its hall is the classic one until the hub, step 5). Each dresses every kind and role the built-in pack does, the
  `hall` kind included (unit-tested), and adds (`room_look.hpp`):
  - `labelPalette`: the generated spines' and labels' grounds, 8 colours of the era (classic's is
    `assets::kDefaultLabelPalette`, byte for byte; optional since the review, like `neon`: classic's when left out);
  - `neon`: the room's accent; the hero sign's lettering material (`heroParts.sign.picture`, `MAT_Neon_*`, unit-tested
    the same colour), the profile's accent light on the sign and (since the review) its picture light take it;
  - `neonGlow` and `bloomStrength` (optional, the review): the sign glow's intensity and range, the room's bloom;
  - `lightTints` (optional): the profile's role colours multiplied in linear light (`themedLighting()`), so the lighting
    profiles stay evening and afternoon: the optical era's spots, fill and washers cooler, the arcade's magenta, violet
    and cyan, the 90s collector's a little cooler, the computer studio's lamps a little green; classic has none;
  - `decorationWeights`: per decoration kind, read and checked, reserved for step 3b (see below).
- **New materials** (`materials.json`): the rooms' walls per pack (`MAT_Wall_Warm` `#3D3430` for classic, warmer
  than `#383735` as the look review asked; `_Mauve` (a caramel until the review), `_Slate` dark blue-grey, `_Cream` a
  warm taupe, `_Study` olive, `_Night` dark plum), `MAT_Plaster_Slate` (the optical halls), `MAT_Floor_Parquet_Dark`,
  seven neons (strength 2.0 to 2.6: they bloom and stay the room's most saturated light; `MAT_Neon_Diffuser`, 1.5,
  stays the test room's), `MAT_Shelf_Light` and `MAT_Shelf_Glow`, leather, teal and (the review) slate seats, navy and
  green display linings, `MAT_Lamp_Shade_Green` and (the review) `MAT_Lamp_Shade_Warm`.
- **Resolution** (ADR-0014 §1): `building.json` gives each floor a `themePack` (left out of the plans' input digest,
  like the floor's sign); `building.hpp`'s `themePackOf()`: the arcade room takes its kind's (`systems.json`'s
  `arcade_after_dark`) wherever it stands, any other room its floor's, else its kind's default (`systems.json`'s
  defaults now handheld `handheld_archive`, computer and other `computer_studio`); a platform manifest overrides only
  when it names a pack (since the review; `resolvePlatform()`, which takes the room's recorded floor).
  `loadSceneData()` resolves it and never reads the plan's record; `RoomDataRequest::themePack` is gone (the family
  stays recorded and authoritative). The halls take their floor's pack (`floorThemePack()`, `FloorSceneData::hallTheme`,
  tinted by its `hall` and `doorwayFill` tints), not the first room's.
- **The plan's record**: `Room::themePack` is written by every update with the resolved pack, for every stored room
  (kept, held or updated); it is no reason to keep or regenerate a room, `roomDataOf()` no longer compares it, and no
  placement, fixture, light or dressing moves for it (unit-tested on a stored plan of the old packs: only the field
  changes, rebuild none; the next update writes nothing). A schema-1 plan lifted keeps its pack until that update.
- **A neon sign per room**: the hero sign's picture is the new `neon` pattern (`makeNeonLettering()`: since the review
  the sign's strokes as rounded glass tubes, a white core over their inner third, a halo about twice the stroke's
  width, on a panel tinted by the neon), times the pack's neon material.
- **Lit shelf interiors** (`geometry::ShelfParams::boardLight`, `WallShelfParams::boardLight`): a 3 mm strip under each
  board above a compartment (role `board_light`) and, on a shelf with a back, a quad over each compartment's back panel
  (role `board_glow`) drawn with the new `shelfglow` surface (alpha falling from under the strip, faint at the sides),
  blended like the halls' wall wash. No light of their own; sockets and bounds unchanged. The scene builder sets the
  parameter when the pack names `board_light` for the kind.
- **Label palettes in the app**: `CoverItem::palette` from the room's pack, carried by `SpineRequest` and
  `FrontLabelRequest::palette` (empty: the default palette); the cache keys follow the colour, so classic rooms keep
  their entries.

## Proof

- Plan goldens: `building-esde-seed-102938.json` and `building-update-esde-seed-102938.json` rewritten on purpose:
  only `themePack` of `room:psx` (`console_90s_collector`), `room:gb` and `area:shared:handheld`
  (`handheld_archive`), `area:shared:computer` (`computer_studio`) and `area:arcade` (`arcade_after_dark`), and the
  `inputDigest` that hashes the rooms' platforms (provenance only); `room:snes` and every SNES plan golden unchanged.
  Scene digests (`tests/fixtures/world/scene/`) rewritten: the walls, the lit shelves, the neon pattern.
- Unit tests (`tests/unit/world/test_theme_packs.cpp`): seven packs, each complete, its materials the library's, its
  neon its sign's colour, 8 distinct palette colours; the reader's refusals; the pack per floor, per kind (the arcade
  room on every floor), a floor without one, a manifest's override, a room on another floor; `themedLighting()`
  (classic leaves both profiles equal, optical tints only colours); a stale plan drawn with the resolved packs and its
  halls with their floor's; the update rewriting the pack and moving nothing; each pack's palette colouring spines.
  Geometry (the strips and glows), the `neon` pattern and the `shelfglow` surface.
- G02 to G13 regenerated on purpose (Debug), 0 pixels apart in Release: the SNES rooms keep classic (the warmer
  walls, the brighter neon, the lit shelves), G12 the 90s collector, G13 the handheld archive, G11 the lobby with the
  arcade's doorway. `world look`'s chroma check caught a first neon too bright (3.0: the tonemap washed the sign to 108
  against a rug tile's 109); at 2.0 the sign stays above every other tile.
- Tests: Debug unit suite 1892 of 1892; Release unit and GPU suite 1997 of 1998 (the overlay's
  picking case skipped, a developer build's) with every golden; the
  benchmark path (Release, `--no-vsync`, 1,230 frames) p50 3.32 ms, p99 3.79 ms, GPU memory peak 261.0 MiB (step 2:
  3.25, 3.97, 260.9), within the acceptable 16.7 and 33.3 ms. The procedural surfaces 80.3 MiB (the shelf glow adds
  85 KiB). No real library: the step touches no catalog code (the plan's `themePack` on the real building is rewritten
  at its next update, nothing moved).

## Screenshots and judgement

`build/i6/step3` (`shots.sh` and `compare.py` in `build/i6/step3-work`; the rooms from a copy of the G12 case's data
directory, 780 frames past its media notice; the optical era's room a scene of a synthetic ps2 plan,
`worldplan generate --selection` then `show --scene`, so without covers); `compare_<pack>.png` beside the concept's
panel:

- **classic_16bit_warm** (panel 2): as before but warmer walls, a magenta neon that blooms, bookcases glowing inside.
- **console_90s_collector** (panels 2 and 4): oak shelving and wainscot, caramel walls, a teal armchair, black frames
  and a red neon: brighter and more cheerful than classic, as style §14.2 asks.
- **optical_era_lounge** (panel 4): navy-grey walls, graphite trim and case frames, a leather armchair, a blue neon and
  a blue wash on the hero wall against warm lamps: the concept's mood, without its blue rug and posters.
- **handheld_archive** (panel 6): taupe walls, walnut shelving lit inside, green-lined cases, a cyan neon: an exhibition
  room, still lacking the low counter showcases (step 6).
- **computer_studio** (no panel): olive walls, green-shaded lamps on brass, oak wainscot, green linings: a study.
- **arcade_after_dark** (panel 5): plum-black walls, graphite everywhere, a hot pink neon, magenta-tinted spots:
  darker than every other room; no cabinets yet (step 6).
- **retro_library_hub** (panel 1): the classic hall until the hub (step 5).

## Deviations and follow-ups

- **Decoration weights** are data only, reserved for step 3b's furniture variants per pack within the same
  footprints: decorations are plan facts and a pack never moves one (ADR-0014 §1). BACKLOG.
- **The neon** is the pixel font's lettering as rounded tubes, not a neon typeface; the halls' portal signs stay
  tungsten (BACKLOG).
- **The prints of the halls** keep one palette for every floor (BACKLOG, step 2).

## The review (2026-10-25)

The step's review found the sign lit in the wrong colour, too little era colour, the arcade's doorway a black hole from
the hub, history that did not build commit by commit, and a manifest freezing its system's pack. Fixed:

- **The sign's light from its neon** (`themedLighting()`): the picture light on the sign is the profile's warm white
  (`evening.json` `#FFF0E8`) times `signLightTint(neon)`, the neon brought to full brightness and mixed with white
  (`kSignLightNeonShare` 0.45), so no pack tints it by hand (the handheld room's pool was magenta, now cyan; classic's
  `#F0ACE8` beside the old `#F0B8E8`). Packs name a `neonGlow` (the sign glow, 3.4 to 4.5 over 3 to 3.5 m, was 1.5
  over 2.5) and may name a `bloomStrength` (the arcade's 0.28): an area term since this review
  (`AreaEnvironment::bloomStrength`, eased like exposure), so in a floor's scene the arcade's area blooms and its hall
  keeps the hub's.
- **Era colour**: the optical era's key, fill and washers cooler (`#E6EEFF`, `#C8D6FF`, `#B8C8FF`) and walnut cases;
  the arcade's spots magenta, fill violet, washers cyan, its walls a lighter plum (`#40324A`; the review's `#2E2434`
  left the median luma at 19), walnut trim, sideboards and cases on a dark varnished wainscot; the 90s collector's walls
  a mauve-taupe (`MAT_Wall_Mauve` `#45393A`), its shelves backed in navy, its spots and fill a little cooler (the
  review's washer tint alone left it 93% orange); navy linings `#283450`; the handheld archive's armchair slate-blue
  (`MAT_Fabric_Slate` `#3A4656`); the shelf strip `#FFC88A` x 1.6 and a glow twice as steep; the rooms' lamp shade
  `MAT_Lamp_Shade_Warm` `#E0C49A` (`MAT_Lamp_Shade` stays the test room's: G01 untouched).
- **The neon lettering** as rounded tubes (`makeNeonLettering()`): a white core over the strokes' inner third, the rim
  at 62% grey, a halo about twice the stroke's width, the panel at 5% (the neon's tint) instead of black.
- **Correctness**: a platform manifest's `themePack` is optional; one naming a pack overrides the floor's, one without
  keeps it (`snes.json` names none; ADR-0014 §1 amended). `labelPalette` and `neon` are optional (classic's); a shelf
  names `board_light` and `board_glow` together, a wall shelf no glow. The digest comments say how a floor's pack
  reaches the provenance (as the pack each room's platform resolves, which `Room::themePack` records); an update that
  only rewrites packs keeps the plan's provenance. `world::roomThemePack()` resolves a stored room's pack;
  `worldplan show` prints it ("... (recorded X until the next update)"). A room's covers share one palette
  (`app::RoomPalette`). `decorationWeights` stay as data, reserved for step 3b's furniture variants.
- **History**: the step's commits were rewritten so that each builds and passes (the material library's test moved
  into the commit that adds the materials, the goldens into the commit that changes the look).

Measured with the review's script (`build/i6/step3r-work/table.py`: the frames at 768 x 432; calm / colour / accent
the review's shares; hue bands of pixels with chroma >= 20), before (`build/i6/step3-before-review`) and after
(`build/i6/step3`):

| Room | median luma | calm / colour / accent % | luma < 12 % | warm / red / magenta / blue / green / grey % |
|---|---|---|---|---|
| concept panel 2 (16-bit) | 38.8 | 79.6 / 12.9 / 7.5 | 17.2 | 48 / 8 / 13 / 1 / 4 / 26 |
| classic_16bit_warm | 36.9 -> 37.1 | 94.5 / 4.7 / 0.8 -> 93.9 / 5.3 / 0.8 | 1.3 -> 1.3 | 70 / 5 / 1 / 0 / 0 / 23 -> 69 / 6 / 2 / 0 / 0 / 22 |
| console_90s_collector | 49.5 -> 47.8 | 97.8 / 1.3 / 1.0 -> 96.3 / 2.7 / 1.0 | 1.0 -> 0.8 | 93 / 1 / 0 / 0 / 1 / 4 -> 80 / 4 / 0 / 1 / 0 / 14 |
| concept panel 4 (optical) | 34.9 | 82.5 / 13.0 / 4.6 | 13.7 | 22 / 8 / 7 / 19 / 2 / 42 |
| optical_era_lounge | 32.1 -> 30.4 | 98.1 / 1.2 / 0.8 -> 97.7 / 1.6 / 0.8 | 6.6 -> 4.3 | 45 / 0 / 0 / 2 / 0 / 53 -> 48 / 0 / 0 / 6 / 0 / 45 |
| concept panel 6 (handhelds) | 55.1 | 85.3 / 8.7 / 6.0 | 6.9 | 58 / 5 / 6 / 7 / 4 / 20 |
| handheld_archive | 42.0 -> 42.3 | 97.9 / 1.0 / 1.1 -> 97.8 / 1.0 / 1.1 | 0.9 -> 0.9 | 87 / 0 / 0 / 0 / 3 / 7 -> 86 / 0 / 0 / 0 / 3 / 10 |
| computer_studio | 50.4 -> 50.6 | 97.9 / 1.1 / 1.0 -> 97.5 / 1.4 / 1.0 | 0.3 -> 0.3 | 80 / 0 / 0 / 0 / 3 / 16 -> 79 / 0 / 0 / 0 / 4 / 15 |
| concept panel 5 (arcade) | 27.6 | 57.3 / 30.6 / 12.1 | 27.0 | 14 / 15 / 30 / 11 / 1 / 28 |
| arcade_after_dark | 17.1 -> 25.1 | 97.1 / 1.8 / 1.0 -> 86.5 / 12.3 / 1.3 | 40.6 -> 5.4 | 27 / 4 / 2 / 0 / 0 / 66 -> 31 / 12 / 14 / 1 / 0 / 41 |
| G11, the arcade's doorway (x 295-445, y 275-690) | 29.3 (step 2) / 23.3 -> 27.2 | | 2.4 (step 2) / 26.3 -> 4.6 | |

Judgement (`build/i6/step3/compare_<pack>.png` beside the concept's panels): the arcade reads as a plum room in pink
and violet light, still the darkest (median 25 against the others' 30 to 51), its doorway from the hub glowing; the
sign of every room lights its wall in its own hue; the optical lounge is cooler and bluer; the 90s den keeps its warm
oak under mauve walls, but its era colour stays low (2.7% colour: the concept's comes from posters, screens and a
rug the pack does not choose yet: BACKLOG); classic is as before but a deeper magenta pool and a stronger glow.

Tests: Debug unit suite 1894 of 1894; Release unit and GPU suite 1999 of 2000 (the overlay's picking case skipped, a
developer build's), every golden within its tolerance; G02-G13 regenerated once on purpose (Debug), G01 untouched;
the scene fixtures rewritten (the sign's light, its glow, the lamp shade), no plan golden (the inputDigest
unchanged). Every commit of the step builds and passes its Debug unit suite and the GPU goldens (checked in a
separate worktree; a few GPU timing cases that failed while the machine ran two suites at once passed again alone).
Benchmark path (Release, `--no-vsync`, 1,230 frames, two runs): p50 3.31 and 3.33 ms, p99 4.21 and 4.82 ms, GPU
memory peak 261.0 MiB, within the acceptable 16.7 and 33.3 ms.
