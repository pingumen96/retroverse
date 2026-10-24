# I6 step 2: dressed halls

Roadmap row: I6 step 2. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) (the styling pass; ADR-0013
§6's derived hall). The direction: [look.md](look.md) ("Halls", the rules).

## What was built

- **The pack's `hall` kind** (`assets/world/themes/classic_16bit_warm.json`, its built-in copy in `room_look.cpp`,
  unit-tested equal): the roles `floor`, `wall`, `ceiling`, `trim`, `baseboard`, `wainscot`, `runner`, `beam`,
  `bench`, `cushion`, `plant` (the pot), `saucer`, `picture` (the prints' frames), `sconce`, `wash` and `sign`. The
  hall takes the first room's pack (the built-in one without rooms); a role the pack lacks is the built-in pack's, and
  so is a role of the `plant` and `poster_frame` kinds the hall uses; its constants are gone. New library materials:
  `MAT_Plaster_Warm` (`#5E4C3E`, the painted plaster's relief and sheen without its colour: a dark warm limewash a
  little lighter and browner than the rooms' `#383735`), `MAT_Plaster_Ceiling` (`#3B3029`, darker than the walls),
  `MAT_Sconce_Shade` (a parchment shade glowing amber, strength 2.5), `MAT_Wall_Wash` (the sconce's light on its wall,
  below), `MAT_Sign_Tungsten` (unlit amber lettering, strength 2.0: a faint glow), `MAT_Ceramic_Saucer` (`#4A2E20`)
  and `MAT_Metal_Brass` (the elevator's doors, the brushed metal in a warm tint).
- **Trim on every wall** (`floor_scene.cpp`), like the rooms': a walnut baseboard, a dark varnished wainscot panel with
  battens every 0.6 m (whole multiples of the pitch from the floor's origin) under a rail at 1 m, the plaster above as
  a 5 mm skin (over the rooms' doorway walls too, so the edge reads as one wall), a crown under the ceiling. The box
  primitive gains `"uv": "grain"` (`BoxUv::GrainMeters`), so the wood's grain runs along each piece, and `"uvOrigin"`,
  the box's place, so that the plaster's boxes side by side (the hall's walls, the skins, the portals' plaster) tile
  on without a seam.
- **A portal at each doorway** on the hall's side: a 10 cm casing, a lintel, plaster over them, the room's sign above
  the lintel (`platform.signText` on the poster frame's `sign` pattern, amber lettering lit on its own: it reads from
  the hall with no light; "GAME ROOM" for a room without a sign text, never its id), a sconce either side of the
  opening, and a **blade sign**: the same lettering on both faces of a 0.8 x 0.24 m panel centred 2.62 m high, 0.46 m
  out from the wall on a graphite arm, 1.05 m from the doorway's centre towards the elevator, so that the rooms read
  from down the hall either way (above the head and the sconces; none in a hall narrower than 2.4 m, where two facing
  blades would meet).
- **Beams** across the ceiling every 3 m from the west end (while 1 m short of the east end), and one over each doorway
  that no beam of that grid passes within 0.75 m of (one where two doorways face each other).
- **Lamps**: a drum pendant (`MAT_Lamp_Shade`) just above each of the hall's lights, its bulb at the drum's foot, on a
  stem from the ceiling (through a beam over it), high enough that from down the hall it never hides the floor's sign
  over the elevator.
- **The dressing** (`floor_layout.hpp`, `Hall::dressing`, `HallPiece`): slots every 1.5 m from x = 0.75 m on both
  edges; the north edge hangs a sconce at even slots and a print at odd ones, the south edge the reverse (a sconce
  faces a print); an edge's prints portrait (0.56 x 0.76 m) and landscape (0.76 x 0.56 m) in turn. Every 8 slots the
  floor holds a bench (1.2 m, 0.45 m deep with its gap) with a potted plant beside it, on each edge in turn every 6 m
  (north: a plant at slot 2 and a bench at 3; south: a bench at 6 and a plant at 7). A piece is left out within its
  margin of a doorway's opening (sconce 1.5 m, print 0.8 m, floor pieces 0.9 m on either edge: the approach and the
  place across the hall stay clear), before 2.5 m (the elevator's landing), near the ends, and a floor piece where it
  would leave less than 1.4 m across the hall beside what the other edge holds (a hall 2 m wide holds benches but no
  plants, one 1.8 m wide neither).
- **Sconces**: a walnut backplate and a lit shade, above the head, colliding with nothing, and the **wash** of their
  light on the wall: an unlit, alpha-blended quad of 1.0 x 1.5 m centred 5 cm above the shade, 1.5 mm off the plaster,
  `#FFB46E` at strength 1 and alpha 0.4 times the new procedural `wallwash` picture (a strong scallop rising from the
  shade, a weaker one below it, a soft core, nothing at the edges; `surface_textures.hpp`, 128 x 192, 256 KiB with both
  maps). Every wash is the same quad and material: one instanced draw. No light of their own.
- **Prints**: walnut frames around the new `print` pattern (`test_patterns.hpp`'s `makePrintPlaceholder()`: the
  covers' layouts in a muted palette, the walnut, oak, wall and graphite grounds, bordeaux, the signage green and the
  cold blue at about 60% of their saturation and a softer tungsten, on an aged paper band). A print's title is one of
  16 invented ones (style §17.2), chosen by its place along its edge (its slot / 2) plus its edge's offset (8 for the
  south edge) and its floor's (`core::deriveSeed("hall-prints", floor)`), so neighbours never repeat (a title comes
  back every 16 prints of an edge, 48 m) and a longer hall appends titles; each title is drawn with its own seed, so a
  floor's prints share at most 16 pictures (and the east end wall's print, larger). A print on the east end wall of a
  hall (not the lobby's).
- **Plants**: the plant prefab on a saucer of its own (`PlantParams::saucerRadius`, the role `saucer`; the rooms'
  plants, without it, are byte-identical), 1.15 times the pot, of `MAT_Ceramic_Saucer`.
- **A runner**: the medallion rug (its own 2.4 x 1.6 m, its picture's size) end to end down the middle from x = 1.8 m
  (past the elevator's zone), at a fixed pitch.
- **The halls' environment**: the evening profile's hall ambient from 0.45 to 0.38 (the lobby keeps 0.45 until it
  becomes the hub, step 5: the profile's optional `hall.lobbyEnvironment`, by default the hall's).
- **Collision and navigation**: benches (four boxes) and plants collide; sconces (backplate, shade, wash), prints and
  the blade signs collide with nothing walkable (above the head, or flat on the wall); trim, signs, beams, lamps and the
  runner collide with nothing. `floor-unreachable` adds the benches and plants to the floor's navigation grid
  (`validation.cpp`).
- **No new light**: the sconces, their washes and the signs are emissive; the hall keeps its ceiling lights and
  doorway fills (step 1's budget and portal clipping unchanged).

## Appending, never moving

The slots, the prints' titles, battens, runner rugs, the beams' grid and the lamps sit on fixed grids from the hall's
west end or on the doorways, which never move: a longer hall appends pieces. A room that enters later may take a slot's
piece away where its portal's margin falls (never moves one), and adds its portal, blade sign and, off the grid, its
beam. The walls' trim follows the walls (a doorway splits a run, the east end's trim and print move with the east
wall), as the walls themselves do. Unit-tested on gen4 grown by 1, 2, 3, 5 and 8 clones of room:snes: every dressing
object kept is byte-identical, every one gone is a slot's piece by a new doorway.

## Proof

- Floor scene digests rewritten on purpose (`tests/fixtures/world/scene/building-esde-seed-102938.floors.json`, the
  gen4 scene; `hallDressing` in the record); the plan goldens and the v1 oracles unchanged.
- Unit tests (`tests/unit/world/test_floor_scene.cpp`, `[dressing]`): portals, flat and blade signs (title, height,
  place, no collision) and a beam within 0.75 m of every doorway, the beams' grid; sconces with their washes (one quad
  and material for all, on the plaster) and prints in turn, portrait and landscape in turn, no title twice among five
  prints in a row, at most 16 pictures a floor; benches and plants at their slots, the plants on their saucers; the
  lamps over their lights below the beams; the battens on the 0.6 m grid by their place, the plaster's uvs anchored;
  the lobby without an east print; the floor's pieces off the elevator's zone and the approaches, at least 1.4 m clear
  across the hall at every 10 cm (the building's halls and a full gen4 floor of 24 rooms). A grid over the hall's
  interior with the elevator's frame and the floor pieces dilated by 0.7 m (the walls not) reaches the hall's centre
  line in front of every doorway from the arrival point; through the collision world the walker stands at the arrival
  point and before every doorway, never inside a bench or plant. A pack without the hall kind, or with a hall of its
  own lacking roles and without the plant kind: the built-in pack's roles, the pack's own kept. A room without a sign
  text: "GAME ROOM" on its portal and blade sign. A narrow hall (2.1, 2.0 and 1.8 m): its floor pieces leave 1.4 m,
  the plants go below 2.03 m, the benches below 1.85 m. Appending (above). The digests are compared in Debug and
  Release.
- Validation (`test_validation.cpp`): a bench of the hall's dressing makes `floor-unreachable` fire where nothing else
  does (a body 1.5 m wide walks a hall 2 m wide past it to the doorway, and a hall 1.9 m wide but not past the bench).
- Assets and geometry: the `wallwash` surface (white, zero at its edges, the scallop above stronger than below), the
  `print` pattern (its picture's chroma under 80% of the cover's over 24 seeds, opaque), the materials, the plant's
  saucer, the box's `uvOrigin`.
- G11 (the lobby) regenerated; the other goldens (G01 to G13) unchanged in Debug.
- The light case (`world_floor_light`): the room from the hall 27 to 51 of 255 (step 1: 29 to 51); the hall from the
  room 46 to 78 (before the review 48 to 99: the darker plaster seen through the doorway); the leak probes equal to the
  neighbours-cut views within 0.06.
- `world elevator`'s fade check compares each 4 x 4 block's mean of each colour channel with the share of the one
  before (blind to the anti-aliasing of the edges inside a block, which moved single values by up to 26 on the prints'
  frames and the runner): worst 4.4, 0.012% of the blocks' values off by more than 2; the bounds 8 and 0.05% (the
  per-value check allowed 32 and 0.3%); the mean and the brightest dim exactly as before. The test room's GPU memory
  case counts every procedural surface but the wall wash (the halls' alone).
- Performance (Release, `--no-vsync`, `--fixed-dt`): the benchmark path p50 3.25 ms, p99 3.97 ms, GPU memory peak
  260.9 MiB (before the review 3.28, 3.97, 260.2); the handheld floor's dressed hall from its elevator, 600 frames, at
  1920 x 1080 p50 3.18 ms, p99 3.68 ms, at 1280 x 720 p50 1.81 ms, p99 2.45 ms (before the review 1.79 and 2.80 at
  1280 x 720, which this note gave as 1920 x 1080), 100 draw calls (87); gen4's 86 (78). All well within the acceptable
  limits (p50 16.7 ms, p99 33.3 ms).
- Tests: Debug unit suite 1882 of 1882; Release unit and GPU suite 1987 of 1988, the test room's memory case failing
  until it left the wall wash out (above), then passing alone; the real library's building and its floors
  (`[real-library][building]`, ES-DE closed): passed, 0 differences among 542,120 entries.

## Screenshots and judgement

`build/i6/step2` (the views before the review in `build/i6/step2-before-review`): `<floor>_elevator.png` (from the
elevator), `<floor>_walk_mid.png` (half way, facing east), `<floor>_walk_back.png` (back to the elevator),
`gen4_portal.png` and `handheld_portal_south.png` (a portal from across the hall), `gen4_side_*.png`,
`ground_lobby_mid.png`; made by `build/i6/step2r-work/shots.sh` from `retroverse-cli worldplan show --floor <id>
--scene` scenes.

Against `example.png` the halls now read as the museum's dark warm galleries between rooms rather than a lit hotel
corridor: the median luma of the views from the elevator 43 to 44 (the lobby 36), down the hall 32 to 38 (the concept's
panels 37 to 48, before the review 61), about 90% of the pixels calm (a 3 x 3 neighbourhood within 12 levels). Each
sconce throws a soft amber scallop up the plaster, the rooms' names read from the elevator on the blade signs, the
prints are muted and varied, the ceiling recedes behind walnut beams and drum pendants, a bench stands with its plant (a
doorway's approach still takes one of a pair on the handheld floor's south edge), the elevator's doors are brass. Still plainer than the concept: the rooms seen through the doorways keep their dark walls
(step 3's packs), the runner's rugs show their borders where they meet, and the lobby is a wide corridor until the hub
(step 5).

## The review

The look review prototyped the darker variant ("v5": the plaster, a darker ceiling, the halls' ambient, the sconces'
strength and wash, the blade signs, the walnut frames and a tint on the prints) and the correctness review found minor
issues; all taken:

- The plaster `#776657` to `#5E4C3E`, a darker ceiling role, the halls' ambient 0.45 to 0.38 (the lobby's kept), the
  shades 0.8 to 2.5, a baked wash per sconce, the blade signs, the signs' lettering 1.2 to 2.0.
- The prints: walnut frames and a muted palette of their own (the `print` pattern) rather than a tint over the
  covers' saturated one; 16 titles by place instead of 8 by a hash (repeats side by side), portrait and landscape in
  turn. Their textures: every print had its own seed, so each was a texture of its own and the scene's textures grew
  with the hall's length (the review measured them doubling on gen4 grown by five rooms); now a floor's prints share at
  most 16, whatever its length.
- A bench with its plant beside it on each edge in turn every 6 m; the plants on a darker saucer 1.15 times the pot;
  drum pendants instead of the flat ceiling panels; beams on a fixed 3 m grid merged with the doorways' (append-safe:
  the grid's beams never go, a doorway's beam is added only off the grid); the elevator's doors in brass.
- The 1.4 m clearance is enforced (a floor piece that would leave less is left out), not only tested on the fixture's
  3 m halls; the plant and poster frame roles fall back per role to the built-in pack's; the sconces' parts collide
  with nothing; the trim's runs clamp their cuts to the wall; a sign without text reads "GAME ROOM"; the plaster's uvs
  are anchored at each box's place (no seam where a skin meets a portal's plaster); new tests for a pack missing the
  hall kind or a role, `floor-unreachable` firing because of a bench, the battens by their place, the lobby's missing
  east print; this note's navigation claim stated as what the test checks.
- The elevator's fade check made content-robust (block means) with a tight bound again.

## Deviations and follow-ups

- **The benches' and plants' slots** are the review's mirrored (north a plant at 2 and a bench at 3, south a bench at 6
  and a plant at 7, rather than a bench at 3 and a plant at 4, a plant at 6 and a bench at 7): the same rhythm, and in
  the fixture's 12 m halls each piece keeps its partner (with the review's slots a doorway's approach or the east end
  took one of each pair on every floor).
- **The sconces light nothing but their baked wash**, as asked (step 1's light budget); a picture light over a print
  is a possible follow-up.
- **The prints' palette is one** for every hall, not their floor pack's (step 3's packs could carry one; BACKLOG).
- **The runner's rugs show their borders** where they meet; a runner surface of its own (a long border and a repeating
  field) is in the BACKLOG.
- **The east end wall's print and trim move** with the wall when the hall grows (as the wall does).
- **One pack for the hall**: the first room's; the floor's own pack is step 3's (ADR-0014 §1, §5).
- `retroverse-cli worldplan show --floor` does not list the dressing (its scene does).
- The G02 to G13 metadata keep their `assets/world` hashes (their pictures did not change; only G11 was regenerated).
