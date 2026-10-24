# I6 step 2: dressed halls

Roadmap row: I6 step 2. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) (the styling pass; ADR-0013
§6's derived hall). The direction: [look.md](look.md) ("Halls", the rules).

## What was built

- **The pack's `hall` kind** (`assets/world/themes/classic_16bit_warm.json`, its built-in copy in `room_look.cpp`,
  unit-tested equal): the roles `floor`, `wall`, `ceiling`, `trim`, `baseboard`, `wainscot`, `runner`, `beam`,
  `bench`, `cushion`, `plant` (the pot), `picture` (the prints' frames), `sconce` and `sign`. The hall takes the first
  room's pack (the built-in one without rooms, a role a pack lacks the built-in pack's); its constants are gone. New
  library materials: `MAT_Plaster_Warm` (`#776657`, the painted plaster's relief and sheen without its colour: a warm
  limewash lighter than the rooms' `#383735`), `MAT_Sconce_Shade` (a parchment shade glowing amber) and
  `MAT_Sign_Tungsten` (unlit amber lettering).
- **Trim on every wall** (`floor_scene.cpp`), like the rooms': a walnut baseboard, a dark varnished wainscot panel with
  battens every 0.6 m (whole multiples of the pitch from the floor's origin) under a rail at 1 m, warm plaster above
  as a 5 mm skin (over the rooms' doorway walls too, so the edge reads as one wall), a crown under the ceiling. The
  box primitive gains `"uv": "grain"` (`BoxUv::GrainMeters`), so the wood's grain runs along each piece.
- **A portal at each doorway** on the hall's side: a 10 cm casing, a lintel, plaster over them, the room's sign above
  the lintel (`platform.signText` on the poster frame's `sign` pattern, amber lettering lit on its own: it reads from
  the hall with no light), a walnut beam across the ceiling over the doorway (one where two doorways face each other),
  a sconce either side of the opening.
- **The dressing** (`floor_layout.hpp`, `Hall::dressing`, `HallPiece`): slots every 1.5 m from x = 0.75 m on both
  edges; the north edge hangs a sconce at even slots and a print at odd ones, the south edge the reverse (a sconce
  faces a print). Every 8 slots an edge has a bench (1.2 m, 0.45 m deep with its gap) and a potted plant. A piece is
  left out within its margin of a doorway's opening (sconce 1.5 m, print 0.8 m, floor pieces 0.9 m on either edge:
  the approach and the place across the hall stay clear), before 2.5 m (the elevator's landing) and near the ends.
  Prints are invented covers (`cover` pattern, titles such as "SAVE POINT"), plants the plant prefab, both seeded by
  `core::deriveSeed("hall", floor, piece id)`. A print on the east end wall of a hall (not the lobby's).
- **A runner**: the medallion rug (its own 2.4 x 1.6 m, its picture's size) end to end down the middle from x = 1.8 m
  (past the elevator's zone), at a fixed pitch.
- **Lamps** hang level with the beams' undersides on a short stem, so that a beam over a later doorway never moves one.
- **Collision and navigation**: benches (four boxes) and plants collide; sconces and prints collide by their bounds
  above or flat on the wall; trim, signs, beams and the runner collide with nothing. `floor-unreachable` adds the
  benches and plants to the floor's navigation grid (`validation.cpp`).
- **No new light**: the sconces and signs are emissive; the hall keeps its ceiling lights and doorway fills (step 1's
  budget and portal clipping unchanged).

## Appending, never moving

The slots, battens, runner rugs and beams sit on fixed grids from the hall's west end or on the doorways, which never
move: a longer hall appends pieces. A room that enters later may take a slot's piece away where its portal's margin
falls (never moves one). The walls' trim follows the walls (a doorway splits a run, the east end's trim and print move
with the east wall), as the walls themselves do. Unit-tested on gen4 grown by 1, 2, 3, 5 and 8 clones of room:snes:
every dressing object kept is byte-identical, every one gone is a slot's piece by a new doorway.

## Proof

- Floor scene digests rewritten on purpose (`tests/fixtures/world/scene/building-esde-seed-102938.floors.json`, the
  gen4 scene; each floor about 115 objects more, `hallDressing` added to the record); the plan goldens unchanged.
- Unit tests (`tests/unit/world/test_floor_scene.cpp`, `[dressing]`): portals, signs and beams at every doorway; sconces
  and prints in turn; the floor's pieces off the elevator's zone and the approaches, at least 1.4 m clear across the
  hall at every 10 cm, the navigation grid at 1.4 m reaching every doorway's front from the elevator (the building's
  halls and a full gen4 floor of 24 rooms); through the collision world the walker stands at the arrival point and
  before every doorway, never inside a bench or plant; appending (above). The digests are compared in Debug and
  Release.
- G11 (the lobby) regenerated; the other goldens (G01 to G13) unchanged in Debug.
- The light case (`world_floor_light`): the room from the hall as in step 1 (29 to 51); the hall from the room 48 to
  99 of 255 (step 1: 28 to 42), the lighter plaster seen through the doorway; the leak probes still equal to the
  neighbours-cut views.
- Performance (Release, 1920 x 1080, `--no-vsync`): the benchmark path p50 3.28 ms, p99 3.97 ms (step 1: 3.28, 4.13);
  the handheld floor's dressed hall from its elevator p50 1.79 ms, p99 2.80 ms, 87 draw calls.

## Screenshots and judgement

`build/i6/step2`: `<floor>_elevator.png` (from the elevator), `<floor>_walk_mid.png` (half way, facing east),
`<floor>_walk_back.png` (back to the elevator), `gen4_portal.png` and `handheld_portal_south.png` (a portal from
across the hall), `gen4_side_*.png`, `ground_lobby_mid.png`; beside `build/i6/step1`.

Against `example.png` the halls now read as the museum's galleries between rooms: wood-trimmed portals with a lit sign
above each and a sconce either side, beams, a dark panelled wainscot under warm plaster, sconces alternating with framed
prints, benches, plants and a medallion runner; no blank wall but the lobby's ends. Still plainer than the concept: the
rooms seen through the doorways keep their dark walls (step 3's packs), the sconces light nothing around them (no
punctual light), the runner's rugs show their borders where they meet, and the lobby is a wide corridor until the hub
(step 5).

## Deviations and follow-ups

- **The sconces are emissive only**, as asked (step 1's light budget): no wall wash around them. A picture light or a
  short-range sconce light per portal is a possible follow-up if the halls read flat.
- **The east end wall's print and trim move** with the wall when the hall grows (as the wall does).
- **One pack for the hall**: the first room's; the floor's own pack is step 3's (ADR-0014 §1, §5).
- `retroverse-cli worldplan show --floor` does not list the dressing (its scene does).
- `world elevator`'s fade check allows a worst value 32 (was 24) above the dimmed one and 0.3% of the values off by
  more than 3 (was 0.2%): the anti-aliasing of the prints' frames and the runner's pattern in the darker view (26 and
  0.18% measured); the mean and the brightest dim exactly as before.
