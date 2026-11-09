# I12 step 4: full side walls

ADR-0021 §1 and §3 (as amended by this step); roadmap I12 step 4. The side walls held the plan's small decoration
between the modules: a poster, a slim lamp, a narrow cabinet, a snake plant, low against a 3.2 m wall. Now each pack
has pieces that reach up the wall, chosen in a rhythm along it, and a new world dresses three of a room's four corner
sockets where it dressed two. Before the step, the real library's two ride rows that failed since step 2 were found.

## Before the step: the ride rows

- **The lobby's arrays on the ride back** were Gate M4's race (BACKLOG): the hub's favourites' showcase is a pool job
  asked at the swap (about 250 ms on the real library), and the ride's view came back in a hall at once, so the
  record (and the user) sometimes saw the lobby before its showcase. A hall's view now waits for the showcase where its
  floor has one (`WorldSession::rideViewResident()`, its content and its proxies' covers), as a room's waits for its
  covers in view (ADR-0013 §13 amended).
- **The warm ride's timeout** was the machine's load: the floor's job took 1.4 s on that run and 0.8 s on a quiet one,
  where the ride arrived at 1.7 s. The job's parts are logged now ("World: the floor ... built in ..."; "Scene ...
  made in ...: its geometry ..., its meshes, materials and textures ..."), and the job no longer copies and sorts the
  whole building's plan for its rooms' content digests (`roomContentDigests(plan, rooms)`, the floor's rooms only:
  about 0.3 s of every floor's job on the real library, 91,000 placed and unplaced games). The gallery's chooser
  normalizes the media root once rather than for every path.

## What was built

- **Four prefabs** (`geometry/prefab_tall_decor.cpp`, a manifest and a family each, 35 kinds to 39), all of original
  design, their spines plain (no front, no title, never the label palette):

  | Kind | Default size (m) | Roles |
  |---|---|---|
  | `bookcase` | 0.88 x 2.10 x 0.34, 5 levels | wood_primary, wood_secondary, spine_a/b/c, paper; books of uneven spines (12 to 35 mm, one leaning at a row's end), binders with a paper strip, magazine files, `mixed` by the seed |
  | `record_rack` | 0.60 x 1.60 x 0.36, 4 levels | wood_primary, wood_secondary, sleeve_a/b/c; record sleeves 31 cm square, or tapes in their cases with a label each |
  | `pegboard` | 0.88 x 1.20 x 0.06, hung as a poster | board, trim, holes (a 25 mm grid), metal (hooks), plastic_body, plastic_dark: up to ten pads of a style on hooks |
  | `coat_stand` | 0.38 x 1.80 x 0.38 | wood, metal, fabric, fabric_alt: four feet, four hooks, a bag, a scarf, now and then a cap |

  And the plant's forms (`PlantParams::form`, `depthSpread`): a **palm** (three canes, arching fronds), a **ficus** (a
  trunk, a crown of small leaves), a **fern** on a turned stand; their leaves within 0.42 m across and 0.19 m front
  to back, so that a wide crown stays in a side-wall socket. The showcase's `"item": "controllers"`: three pads
  leaning back on its plinth. New materials: `MAT_Book_Cloth_Navy`, `MAT_Book_Cloth_Ochre`, `MAT_Pegboard_Hardboard`,
  `MAT_Pegboard_Hole`, `MAT_Cork_Board`, `MAT_Plant_Trunk`; the plant kind's roles `trunk` and `stand` in every pack.
- **The packs** (`furniture.decoration`, every pack but the hub's): the narrow cabinet's sockets draw a bookcase, a
  rack, a vitrine of pads, a tower speaker (the step 3 speaker at 0.30 x 1.10 x 0.32), a coat stand or the pack's low
  piece; the plant's a snake plant, a palm, a ficus or a fern; the poster's a poster, a pegboard or a tall print. Per
  era: the classic oak bookcases of books and a rack of records; the 90s collector magazine files in teal and purple,
  tapes, grip pads; the lounge binders of manuals and records; the study binders, tapes and a cork pegboard of
  joysticks; the handhelds a bookcase and a coat stand beside their vitrines; the arcade magazines, records and
  joysticks. The built-in pack carries the classic's.
- **The rhythm** (`scene_builder.cpp`'s `chosenVariants()`, once a room with the secondary TVs: `RoomChoices`): the
  decoration fixtures wall by wall in the order their sockets stand along it at home (a piece the editor moves keeps
  its variant), each drawing by weight from the room's seed and its id as before; beside a neighbour (within 1.2 m) a
  draw of the neighbour's piece, or of its class when both stand on the floor (tall: its top 1.5 m up or more), is
  drawn again among the variants that differ, else stands. A hung piece (a poster, a pegboard) leaves the floor under
  it free and takes no part in the heights' alternation.
- **Guards:** a decoration's variant reaching through the ceiling is not drawn (the plan's prefab is, logged once),
  as the hero set's pieces since step 3.
- **The gallery over low pieces** (step 2's open part): a low decoration standing on the floor against a wall has a
  frame over its drawn top (a secondary TV over its sideboard), a tall one none; the wall pictures of the arcade and
  the handhelds hang over the drawn top too and never over a tall piece.
- **The density 0.65** (ADR-0021 §3): the curated preset's `decorationDensityPermille` is 650. Each room records the
  density it was laid out with (`Room::decorationDensityPermille`, named in the plan only when it is not 400, so every
  plan written before keeps its text): an update and the editor lay a stored room out with its own density, a
  generation or a regeneration with the configuration's, and the `decoration-density` rule judges each room by its
  own. `WorldConfig`'s default stays the rules' 400, which the golden plans are made with.

## Tests

- **Unit:**
  - `test_prefabs.cpp`: the four kinds in the kind list, their roles and sockets, exact bounds for the units and the
    coat stand; a case for the tall decoration: spines inside the carcass, the same seed the same spines, an empty
    bookcase its carcass alone, records 31 cm deep, a pegboard hung from z 0 with its pads in front, the joystick
    style's depth, the coat stand within its spread whatever hangs, the plant's forms within their spreads and
    under their height, the fern's stand, the vitrine's pads inside the glass; the refusals.
  - `test_era_furniture.cpp`: the rhythm on the golden plans and every pack (no neighbour alike, the classes
    alternating between floor-standing pieces where the variants allow, the same scene twice, nothing over the
    ceiling, no gallery frame over a tall piece); every pack's variants inside their sockets with the validation as
    it was.
  - `test_world_building_plan.cpp`: three of a base room's four corner sockets at 0.65 and two at 0.4, each room's
    recorded density, the text (none at 0.4, the key at 0.65, a 400 written refused), a building of 0.4 updated
    under 0.65 with every fixture, light and placement kept, its rooms 0.4 and its findings the same.
  - `test_validation.cpp`: the density rule by the room's own density; `test_world_config.cpp`: the preset's 650, the
    configuration's 400; `test_room_dressing.cpp`: the gallery's frames over the modules and the low decorations;
    `test_room_look.cpp`: the built-in pack equal to the classic file; the manifests, families and materials; the
    scene goldens (`tests/fixtures/world/scene/`) rewritten.
- **GPU:** `test_world_floor.cpp`'s leak probes failed since step 1 on the handheld floor (the hall's floor and wall
  beside a doorway 1.9 and 4.2 of 255 brighter than with the room's lights cut): step 1's look pendant, a point light
  1.5 m inside the doorway, reached them through the portal's plane (without it they read as cut). Its light is now a
  spot straight down (42° outer cone): its pool is the floor under the drum, the doorway's wall out of its cone; the
  probes pass.

## Departures

- **No crate or flyer rack kind**, no torchiere among the floor lamps' variants: the lamps keep the plan's light where
  it is, and the packs that list a lamp kind keep theirs.
- **No GPU smoke case for the walls** (`snes-stress` at 0.65): the rhythm and the tall pieces are judged on the scenes
  in the unit tests; the walls are seen in the goldens and in the screenshots under `build/i12/walls/`.
- **Hung pieces are neutral in the alternation** (the plan had them tall): a poster's class pushed every neighbour to a
  low piece, and the walls stayed low.

## Measured

**The goldens:** G02, G02s, G06, G07, G08a, G08b, G10, G12, G13, G15, G16, G17, G18, G19 and G22 rewritten on purpose
(the fixture buildings are planned at 0.65 now: a third corner socket dressed; the walls' variants; the look pendant's
spot); G01, G11, G14, G20 and G21 compare as they were. The scene goldens (`tests/fixtures/world/scene/`) rewritten;
the golden plans unchanged (made at the rules' 0.4).

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 2.83 ms | 3.39 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 3.04 ms | 3.58 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 6.88 ms | 7.59 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.34 ms | 4.92 ms | informative |

**Memory:** the real library's busiest floor (computer, ten rooms, 5,314 items) peaks at 654.6 MiB against the 4 GiB
row (607.0 MiB at step 3): the tall pieces' meshes (a bookcase's spines are one mesh per seed) and the plants' leaves.

**The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed): every row
passes; the library's snapshots differ in none of their 542,121 entries. The lobby shown 3.1 s after the start; the
ground floor's job 0.10 s on a ride (0.37 s before the digests' change); the warm ride to the computers' amstradcpc
room arrived at 2.15 s (its floor built at 0.92 s, its covers in view resident at 1.90 s), the cold one timed out at
2.50 s as always; the lobby's arrays on both rides back 4.8 MiB, the showcase's included.

**The unit suite:** passed but the two `platform_win/process_runner` cases that a set `RV_LAUNCH_ALLOWLIST` refuses
(they pass without it, as `scripts/test.ps1` runs them).