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
