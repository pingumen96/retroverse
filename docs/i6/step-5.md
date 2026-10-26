# I6 step 5: the hub

Roadmap row: I6 step 5. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §3 (amended on 2026-10-26:
the hub's size as data, the re-placement rule, what the hub holds), ADR-0013 §10 (the directory, the favourites as
runtime proxies). The direction: [look.md](look.md) (the hub's row, concept panel 1).

## What was built

- **The hub** (`floor_layout.hpp`'s `Hub`, `building.json`'s `layout.hub`): the ground floor's derived hall is 18 x 14
  x 5 m (longer when its rows reach further east), its rooms on its long walls from 4.35 m, so that the arcade room
  stands in the middle of the north wall; a wall of the hub's own closes each edge above a room (`HallWall::bottomMm`).
  It holds, all derived and fixed around its core (a longer hub appends columns and bookcases, moves nothing):
  - a balcony 1.2 m deep along the north, south and east walls (slab, fascia, a railing set back behind the columns;
    not walkable) on walnut columns every 3 m and at the east edge, none before a portal or the showcase;
  - a coffered ceiling (beams at the columns and the balcony's edges, a downlight disc in each coffer, more under the
    balcony), a chandelier of two rings of bulbs over the landmark, a lantern over each of its lights;
  - the landmark (`hub_landmark`): an original wireframe globe of meridians and parallels around a glowing core, its
    axis tilted, on a pedestal in a railed circular glass showcase of six generic consoles, four plants inside;
  - portals: the arcade room's (pilasters and an entablature carrying its sign, the sign again on the balcony's
    fascia), the elevator's (its floor's sign), the closed entrance's ("MAIN ENTRANCE", the floor's sign on the fascia);
  - the directory's totem (`info_totem`) west of the landmark facing the elevator, its screen the building's floors on
    the new `board` pattern, its zone 1.5 m deep before it; an information screen ("BUILDING DIRECTORY") above the
    favourites' showcase on the south wall;
  - eight lit bookcases of books (the museum-library: the pack's shelf with its strips and glow, `bookends` on each
    level), two leather sofas facing the landmark on rugs, a runner of rugs, eight large plants, sconces and prints on
    the walls under the balcony and a sconce in every bay of the gallery;
  - its lights: nine over the core (the hall role's, 1.6 times as bright, 2 m further), four under the balcony, the
    arcade doorway's fill: 14 in its area (step 1's selection of 16 applies).
  Every solid piece keeps off every doorway's approach (0.9 m beside the opening, 2.5 m deep), so a room entering the
  hub later takes a piece away, never moves one (unit-tested on a hub of three rooms: the showcase gives way to a south
  room's doorway). `floor-unreachable` adds the hub's solid pieces and reaches the totem's zone and the showcase's
  front from the arrival point.
- **The re-placement** (`building.hpp`'s `placeOnHub()`, ADR-0014 §3 as amended): a ground floor room whose doorway
  wall is not on the hub's edge (a stored building's, at I4's lobby) is re-placed by the next update, before any system
  enters, at the east end of its own row on the hub, its yaw and contents unchanged; the change report's
  `replacedRooms` (the CLI's "Room re-placed", the run report's `world.replacedRooms`, the notice `rooms-migrated`), the
  rebuild level full; the next update moves nothing. A plan shown as it is (the persisted start's preview, a plan kept
  for unknown media) draws its rooms on the hub already (`buildFloor()`). The generator stays 1.4 (a room's place on
  its floor moves, no game in its room). The plans' `inputDigest` hashes the hub only when a ground floor room is among
  their rooms: the SNES plan goldens are untouched; the two building plan goldens are rewritten (the arcade room at x
  9000, z -12150 and their digest); I4's `lobbyWidthMm` stays as provenance only.
- **The totem** (app): in its zone facing its screen (the elevator's `facesElevator()` rule) the elevator's prompt
  reads "Indice" ... "Piani e stanze" (`elevator_prompt.rml`'s `totem`); interact opens the directory at its floors
  (`via` "totem", `world.totemPromptFrames`).
- **The favourites' showcase** (`hub_showcase.hpp`, `world_showcase.cpp`): a display case of 2 x 6 face-out slots
  holds up to 12 runtime proxies, never plan placements: the catalog's `favoritesAndRecent` read on the world reader
  in a pool job, completed by the rooms' items files (their covers; a generated label otherwise), drawn as keep cases
  of `disc_case_dvd_v2` by a collection of their own (`collection/hub:showcase/...` registry keys) with a cover loader
  of their own, picked by a pick index of their own when no room's item is hovered (outline and title label); interact
  opens the directory straight at the game's card (`DirectoryModel::openGame()`: Avvia, "Mostra nella stanza", the list
  behind it). Made again when a launched game returns and after a refresh; lockstep runs wait for it like the floor's
  covers. The run report's `world.showcase`.
- **New prefabs** with the recipe (params, builder, JSON reader, `kPrefabs`, a family and a generic manifest, a kind in
  every pack and the built-in one, materials): `column`, `railing`, `hub_landmark`, `info_totem`; `MeshBuilder::torus`;
  the `board` pattern; `MAT_Globe_Lines`, `MAT_Globe_Core`, `MAT_Downlight`, `MAT_Bulb_Warm`, `MAT_Screen_Board`; the
  packs' `hub` kind (balcony, fascia, downlight, chandelier, bulb, screen, pilaster, seat, rug).

## Proof

- **Unit** (`rv_tests`): `test_hub.cpp` (the hub's size, balcony, columns and pieces; pieces off every doorway's
  approach and apart, the walker reaching the doorway, the totem's zone and the showcase's front; a walk from the
  elevator past the landmark into the arcade room on `tests/input_scripts/hub_walk_into_arcade.json`; the scene's
  objects; the showcase's twelve slots; `placeOnHub()`; a longer hub; `building.json`'s reader), `test_hub_showcase.cpp`,
  the building, migration, shared-room and suite cases on the hub (the suite's rows start at the hub's 4.35 m), the
  new prefabs, `torus`, the `board` pattern, the run report's fields, the prompt's words, `openGame()`. Debug: 1928
  tests, all passed; Release: 1928 unit and 112 GPU tests, all passed (the overlay's pick case skipped as
  always), no foreground taken (`scripts/focus_watch.py`); the launch case added after, with the other hub cases, too.
- **GPU** (`gpu.world hub`): the totem's prompt and the directory at its floors; the showcase's proxies (the directory's
  first twelve, fronts resident) and a proxy's card; a proxy's Avvia into the fake emulator, its return and the
  showcase made again; the arcade room reached past the landmark. G11 (the hub from the arrival point) regenerated and
  G14 (the hub from its entrance) added on purpose (Debug); G01-G10, G12, G13 unchanged. Adjusted on purpose: the
  elevator's memory check compares the rooms' arrays (the hub's showcase adds its own), the static groups count the
  showcase's, the light probes on the hub's doorway (see Deviations).
- **Goldens**: the SNES plan goldens untouched; the two building plan goldens rewritten (the arcade room's placement
  and the input digest); `building-esde-seed-102938.floors.json` rewritten (the ground floor).
- **Benchmark** (Release, 1920 x 1080, `--no-vsync`): the benchmark path p50 3.38 ms, p99 4.13 ms, 303.6 MiB; a walk
  across the hub into the arcade room (`hub_walk_into_arcade.json`, 870 frames) p50 3.34 ms, p99 4.58 ms, GPU 1.26 ms
  a frame, 252 MiB at its peak (one 262 ms frame: the arcade CRT's first bind, as before this step).
- **The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, `RV_LEGACY_CLI`
  of generator 1.3; 0 differences among 542,120 entries): planned from nothing, the arcade room stands on the hub at x
  9000, z -12150 and nothing is re-placed; the lobby (the hub) shown 2.67 s and 2.73 s after the process started on
  the second and third starts (NFR-01: 5 s), the building planned twice to one digest and SHA-256 (19,985 placements);
  the stored building of generator 1.3 updated once in 3.6 s: `area:arcade` re-placed from x 7650, z -8150 to x 9000,
  z -12150 with 24 rooms migrated, the other 20 byte for byte (the theme pack every update writes compared as it
  stood: 30 rooms took their floor's), the second start and the CLI's update writing nothing. The busiest floor
  (computer) peaks at 510.0 MiB (512); the warm ride's view back in 1.87 s.

## Screenshots and judgement

`build/i6/step5` (Release, 1920 x 1080, the building fixture; `compare_*.png` beside concept panel 1), measured at
768 x 432 like the concept (median luma; calm / colour / accent %; share below 12):

| image | median luma | calm / colour / accent % | luma < 12 % |
|---|---|---|---|
| concept panel 1 | 42.0 | 88.8 / 5.5 / 5.6 | 6.6 |
| hub_elevator (G11's view) | 41.7 | 93.9 / 5.0 / 1.1 | 2.8 |
| hub_entrance (G14's view) | 37.0 | 93.9 / 5.0 / 1.1 | 3.8 |
| hub_balcony | 46.3 | 96.1 / 1.1 / 2.8 | 2.8 |
| hub_landmark | 39.4 | 94.8 / 1.8 / 3.4 | 3.7 |
| hub_arcade_portal | 41.3 | 92.1 / 6.7 / 1.2 | 3.0 |
| hub_showcase | 38.9 | 88.2 / 11.1 / 0.7 | 10.1 |
| hub_totem | 42.3 | 97.7 / 1.6 / 0.7 | 5.3 |

Judgement beside panel 1: the same warm brightness (median 37 to 46 against 42) and the same reading: a tall
two-storey hall of dark wood under a coffered ceiling with downlights and a chandelier, a gallery with a railing on
three sides, the glowing globe in a railed circular showcase in the middle, rugs, leather sofas, lit bookcases and
plants. Calmer than the panel: its colour comes from brand neons over each room and posters (accent 5.6%); ours are
the rooms' lit signs and the globe's blue (1 to 3%). The arcade's portal reads as a portal (pilasters, an entablature
with its lit sign, the purple room beyond); the totem's and the screen's lists are legible from a few metres.

## Deviations and follow-ups

- The concept's room signs are brand neons; ours are the rooms' own lit signs, so the hub's accent share is lower than
  the panel's (1 to 3% against 5.6%); step 6's arcade cabinets and step 7's review may add more colour.
- One information screen (above the showcase) beside the totem's; no stairs to the balcony (decorative, as decided).
- The showcase draws every proxy as a keep case (`disc_case_dvd_v2`) whatever its system's family; a family per proxy
  would need each family's prefab and arrays in the hub (BACKLOG).
- Two GPU checks were made to fit the hub on purpose: the ride's fade allows 0.2% of the blocks off by more than 2
  (the balusters' and rugs' edges anti-aliased a pixel apart in the darker view, 0.074% measured), and the light probe
  through a doorway onto the hub asks the hub's light added (+12 over I4, +5 over the neighbours cut), not a multiple:
  the hub glows by itself (chandelier, lanterns, downlights, signs) in every variant.
- `g11_lobby_golden.png` keeps its name (G11 is the building's start view, now the hub's).

## The review's fixes (2026-10-26)

The step's review (`build/i6/review5`) found six correctness faults, a memory margin too thin for step 6 and a look
too bright on the floor, too dark on the walls and calmer than concept panel 1. What changed:

- **The showcase on a refresh** (`world_showcase.cpp`, `world_live_update.cpp`): a refresh that keeps the floor
  (unchanged, textures, items) asks for the showcase's content again. Each rebuild used to register a new key prefix
  (meshes, materials, a uniform block more until the floor went; the stale layered and hero materials pointed at freed
  arrays and textures: a use-after-free for the overlay's materials panel). Now one collection per floor under a stable
  prefix takes a new layout in place (`CollectionInstance::applyLayout`); a content equal to the one shown draws
  nothing. The run report's `world.showcase.draws` (the registries' sizes after each drawing) and `.unchanged`; a GPU
  case marks a favourite in the fixture's gamelist, presses "Aggiorna libreria" and checks the rebuild in place, the
  registries as they were.
- **The re-placement** (`placeOnHub()`, `incremental.cpp`): a room never changes rows; when its own row is full nothing
  is re-placed and the update refuses explicitly (`RequiresExplicitRegeneration`, the rooms named); an update that
  re-places a room computes its inputDigest again (a partial one kept the previous, which no longer described the plan).
  `worldplan show --floor` and `worldplan check` apply the re-placement in memory as `buildFloor()` does (show lists the
  rooms drawn on the hub, `floorScene.drawnOnHub`). Unit tests: a mixed ground floor, a full row, a partial update of
  generator 1.3's building plus its re-placement and the next update; the CLI's "Room re-placed" line and JSON.
- **The bookcases' books** stand on the sockets of the shelf built from the very parameters drawn (the two agreed
  already: no change in the scene).
- **Memory** (`texture.cpp`, `texture_cache.cpp`): every procedural surface's metallic-roughness map is all metal or
  none, so it goes up as R8 (its roughness), swizzled to sample as the RGBA8 map did (`gl::metallicRoughnessFormat`;
  RG8 for a map whose metal varies): the procedural surfaces take 85 MiB instead of 123, the same texels (G01 to G10,
  G12, G13 unchanged). Making them resident per floor (about 34 MiB more on a floor) is parked: every surface is
  generated at the start, so that a commit never waits for one.
- **The look** (concept panel 1; `retro_library_hub.json`, `floor_scene.cpp`, `floor_layout.cpp`, `hub_landmark`):
  a darker floor (`MAT_Floor_Parquet`) and rugs (`MAT_Carpet_Ornate_Hub`, the classic picture under a darker tint: no
  new surface), a lighter warm plaster on the hub's walls, the core's lights 1.2 times the hall role's (1.6), those
  under the balcony as bright as it (0.45); the globe lower and larger (its centre at 1.92 m, radius 0.65) with a cool
  light of its own (`#4C8BC9`) at its socket, nine consoles 1.4 times larger and a boxed game on a riser between each
  two, a bordeaux counter, a glass a little more present; the fascia's sign in its room's neon (the arcade's pink), a
  small plaque on the portal's entablature, the elevator's floor sign signage green, the entrance's a calm blue; the
  showcase and its screen in the column bay west of the core's middle, off the arcade portal's axis (a south column at
  9 m appears), its back level 24 cm up and its glass top above it (the front rail no longer crosses the titles); large
  posters on the gallery's walls and on both end walls; oxblood leather sofas; warm bindings (bordeaux, green the
  minority); larger downlights; the board's lines spread and centred. A light per room sign was tried and left out: a
  hub of 16 lights pushed the arcade doorway's fill out of the arcade's view through its doorway (the light probes);
  the neon glows by its bloom. The orange case in the showcase is a fixture cover ("The Linen Kiln and the Velvet
  Garret"), not a missing label.
- **G11 and G14** moved on purpose: G11 a step north of the arrival point turned a little north of east (`--spawn
  1.2,0,-2.4,84,-6`: the arcade's portal and the globe together, the entrance's signs beside it), G14 a step south of
  the entrance's middle (`--spawn 16.3,0,1.8,-96,-3`: the elevator's portal and its green sign beside the globe);
  regenerated from Debug. `building-esde-seed-102938.floors.json` rewritten (the ground floor alone). No plan golden
  changed.

`build/i6/step5-fix` (Release, 1920 x 1080; `compare_*.png` beside panel 1; `work/shots.sh`, `work/table.md`),
measured at 768 x 432 (floor band: the bottom quarter's median luma; wall band: rows 35 to 60 %; warm and cool: hues
10 to 48 and 150 to 260 degrees with chroma 20 or more), before (`build/i6/step5`) and after:

| image | median luma | floor band | wall band | calm / colour / accent % | warm % | cool % |
|---|---|---|---|---|---|---|
| concept panel 1 | 42.0 | 47.9 | 47.0 | 88.8 / 5.5 / 5.6 | 55.0 | 6.8 |
| hub_elevator, before | 41.7 | 77.2 | 26.2 | 93.9 / 5.0 / 1.1 | 85.1 | 1.3 |
| hub_elevator | 41.7 | 48.9 | 26.7 | 92.4 / 6.2 / 1.4 | 80.2 | 1.8 |
| hub_g11 (G11's view) | 41.1 | 45.3 | 28.2 | 94.4 / 4.5 / 1.1 | 83.9 | 2.3 |
| hub_entrance, before | 37.0 | 74.8 | 26.3 | 93.9 / 5.0 / 1.1 | 82.6 | 1.4 |
| hub_entrance | 37.9 | 46.8 | 32.7 | 91.2 / 7.6 / 1.2 | 77.2 | 2.0 |
| hub_g14 (G14's view) | 35.2 | 32.9 | 33.8 | 90.3 / 8.6 / 1.1 | 74.3 | 2.3 |
| hub_balcony, before | 46.3 | 29.8 | 40.6 | 96.1 / 1.1 / 2.8 | 90.1 | 3.1 |
| hub_balcony | 45.8 | 45.2 | 36.2 | 92.8 / 2.7 / 4.5 | 86.1 | 4.7 |
| hub_landmark, before | 39.4 | 31.4 | 39.5 | 94.8 / 1.8 / 3.4 | 84.3 | 3.8 |
| hub_landmark | 41.3 | 30.0 | 55.8 | 90.6 / 5.4 / 4.0 | 82.4 | 5.9 |
| hub_totem, before | 42.3 | 59.5 | 27.7 | 97.7 / 1.6 / 0.7 | 83.8 | 0.4 |
| hub_totem | 42.3 | 44.9 | 30.1 | 93.1 / 4.2 / 2.6 | 81.0 | 1.1 |
| from_arcade (the review's) | 47.9 | 47.3 | 42.3 | 91.0 / 6.9 / 2.1 | 83.3 | 3.3 |

Judgement beside panel 1: the floor band is the panel's now (45 to 49 against 48, from 75 to 77), the globe the
brightest thing below the ceiling, the median kept (35 to 46 against 42), the warm share down (74 to 86 % from 83 to
90) and the cool one up (1.8 to 5.9 % from 0.4 to 3.8). Short of the panel still: the wall band in the long views (27
to 34 against 47: the dark wainscot, columns and the far wall under the balcony; a brighter wainscot only warmed it)
and the accent share (1.1 to 4.5 % against 5.6: one room's neon on this fixture's hub against the panel's four brand
signs). Both are parked (BACKLOG).

Proof after the review: Debug and Release unit tests (1906 cases), all passed; Release `msvc-x64-release-gpu`, 2046
tests, all passed (the overlay's pick case skipped as always); the benchmark path (Release) p50 3.32 ms, p99 3.77 ms,
265.5 MiB at its peak (303.6 before: the procedural surfaces' 38 MiB). The real library
(`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, `RV_LEGACY_CLI` of generator 1.3; 0
differences among 542,120 entries): the busiest floor (computer) peaks at 471.8 MiB, cold and warm (510.0 before;
512 acceptable); the hub shown 2.71 s and 2.69 s after the process started on the second and third starts (NFR-01:
5 s); the building planned twice to one digest and SHA-256 (19,985 placements); the stored building of generator 1.3
updated once in 3.5 s, `area:arcade` re-placed from x 7650, z -8150 to x 9000, z -12150, 24 rooms migrated and the
other 20 byte for byte; the warm ride's view back in 1.87 s.
