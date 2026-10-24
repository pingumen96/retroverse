# I6 step 3: theme packs per floor

Roadmap row: I6 step 3. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §1 (the pack is a look,
resolved at scene build) and §5 (packs per floor). The direction: [look.md](look.md) (the table per floor, the rules).

## What was built

- **Six new packs as data** (`assets/world/themes/`): `console_90s_collector` (gen5), `optical_era_lounge` (gen6,
  gen7), `handheld_archive`, `computer_studio`, `arcade_after_dark` and `retro_library_hub` (the ground floor's hall;
  its hall is the classic one until the hub, step 5). Each dresses every kind and role the built-in pack does, the
  `hall` kind included (unit-tested), and adds (`room_look.hpp`):
  - `labelPalette`: the generated spines' and labels' grounds, 8 colours of the era (classic's is
    `assets::kDefaultLabelPalette`, byte for byte);
  - `neon`: the room's accent; the hero sign's lettering material (`heroParts.sign.picture`, `MAT_Neon_*`, unit-tested
    the same colour) and the profile's accent light on the sign take it;
  - `lightTints` (optional): the profile's role colours multiplied in linear light (`themedLighting()`), so the lighting
    profiles stay evening and afternoon: the optical era's spots cooler and its sign's picture light blue, the arcade's
    tinted magenta, the computer studio's lamps a little green; classic has none (its light unchanged);
  - `decorationWeights`: per decoration kind, read and checked, used by nothing yet (see below).
- **New materials** (`materials.json`): the rooms' walls per pack (`MAT_Wall_Warm` `#3D3430` for classic, warmer
  than `#383735` as the look review asked; `_Caramel`, `_Slate` dark blue-grey, `_Cream` a warm taupe, `_Study` olive,
  `_Night` plum-black), `MAT_Plaster_Slate` (the optical halls), `MAT_Floor_Parquet_Dark`, seven neons (strength 2.0 to
  2.6: they bloom and stay the room's most saturated light; `MAT_Neon_Diffuser`, 1.5, stays the test room's), `MAT_Shelf_Light` and `MAT_Shelf_Glow`, leather and teal
  seats, navy and green display linings, `MAT_Lamp_Shade_Green`.
- **Resolution** (ADR-0014 §1): `building.json` gives each floor a `themePack` (left out of the plans' input digest,
  like the floor's sign); `building.hpp`'s `themePackOf()`: the arcade room takes its kind's (`systems.json`'s
  `arcade_after_dark`) wherever it stands, any other room its floor's, else its kind's default (`systems.json`'s
  defaults now handheld `handheld_archive`, computer and other `computer_studio`); a platform manifest still overrides
  (`resolvePlatform()`, which takes the room's recorded floor). `loadSceneData()` resolves it and never reads the plan's
  record; `RoomDataRequest::themePack` is gone (the family stays recorded and authoritative). The halls take their
  floor's pack (`floorThemePack()`, `FloorSceneData::hallTheme`, tinted by its `hall` and `doorwayFill` tints), not the
  first room's.
- **The plan's record**: `Room::themePack` is written by every update with the resolved pack, for every stored room
  (kept, held or updated); it is no reason to keep or regenerate a room, `roomDataOf()` no longer compares it, and no
  placement, fixture, light or dressing moves for it (unit-tested on a stored plan of the old packs: only the field
  changes, rebuild none; the next update writes nothing). A schema-1 plan lifted keeps its pack until that update.
- **A neon sign per room**: the hero sign's picture is the new `neon` pattern (`makeNeonLettering()`: the sign's
  strokes white with a soft halo, at most 45% grey, on black), times the pack's neon material.
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

- **Decoration weights** are data only: decorations are plan facts and a pack never moves one (ADR-0014 §1); the
  layout candidates ADR-0014 defers would read them. BACKLOG.
- **A manifest's pack overrides the floor's** (snes.json names classic, which gen4 gives anyway).
- **The neon** is the sign's pixel lettering with a halo, not tube-shaped glyphs.
- **The prints of the halls** keep one palette for every floor (BACKLOG, step 2).
