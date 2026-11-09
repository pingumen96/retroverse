# I12 step 3: the hero corner as a lived-in set

ADR-0021 §1 (as amended by this step); roadmap I12 step 3. The hero corner held its parts and nothing else: a cabinet,
the CRT, the console, a seat, a rug, a lamp. Now it is a corner someone lives in: speakers either side of the TV, a
media rack with amber displays and LEDs under the plaque, a pad on the rack and one on the rug with its cable, a
coffee table with magazines and an open game box, a side table with a small lamp, a cushion on the seat; each pack
its own. And the game played last stands in the console's slot.

## What was built

- **Six prefabs** (`geometry/prefab_hero_set.cpp`, one manifest and family each, 29 kinds to 35), all of original
  design:

  | Kind | Default size (m) | Roles | Sockets |
  |---|---|---|---|
  | `floor_speaker` | 0.20 x 0.72 x 0.26 | cabinet, grille, trim (driver rings), led | |
  | `media_rack` | 0.36 x 0.42 x 0.38, 2 decks | wood, metal (the decks), display, led | `Top` |
  | `pad` | flat, grip or joystick, a cable to `cableTo` sagging by `sag` | plastic_body, plastic_dark, cable | `Cable` |
  | `low_table` | rect 0.80 x 0.42 x 0.42, or round | wood_primary (top), wood_secondary | `Top` |
  | `cushion` | 0.40 x 0.12 x 0.40 | fabric | |
  | `open_box` | a keep case open with its disc and manual, or a tray with its lid | body, paper, disc | |

  New materials: `MAT_Led_Red`, `MAT_Led_Green`, `MAT_Display_Amber` (small, unlit), `MAT_Speaker_Grille`,
  `MAT_Cable_Rubber`, `MAT_Disc_Silver`. Every pack names the six kinds' roles (the built-in pack the classic's).
- **The set** (`room_look.hpp`'s `HeroDressingPiece`, a pack's `furniture.heroDressing`): named pieces placed in the
  hero corner socket's frame, each with an **anchor**, the hero part it belongs with. A piece is drawn only where its
  anchor is drawn (a room without a console has no pads; the arcade's cabinet leaves its parts out) and moves with it
  when the plan's part stands elsewhere. A **prop** (a pad, a cushion, magazines, a box, the table lamp) never collides.
  The scene builder draws them after the parts (`dressing:hero:<name>`, seeded from the anchor's fixture), with a guard:
  a piece leaving the hero corner socket or rising through the ceiling is not drawn (logged once). The reader refuses a
  bad or repeated name, a sign anchor, a piece without `xMm` and `zMm`, and a screen (CRT, secondary TV, arcade
  cabinet).
- **The packs:**

  | Pack | Its set |
  |---|---|
  | `classic_16bit_warm` | walnut speakers and rack, flat pads, a coffee table with magazines and a cardboard tray, a round side table with a lamp, a green cushion |
  | `handheld_archive` | the classic's without pads (the consoles are in the hand), oak, a keep case, a cushion on the chair |
  | `console_90s_collector` | charcoal speakers and rack, grip pads, a crate with magazines between the bean bags, a keep case on the rug |
  | `optical_era_lounge` | walnut speakers and rack, grip pads, a side table beside the sofa with a lamp and a keep case, a cushion on the sofa |
  | `computer_studio` | ivory speakers, an oak rack, a joystick on the desk |
  | `arcade_after_dark`, `retro_library_hub` | none |

- **Validation:** `drawnFixtures()` adds each colliding piece as a part of the corner of its own,
  `<socket>.dress-<name>` (`kHeroDressingPartPrefix`), which the fixture-socket rule takes as the corner's, so that
  `validatePlan()` judges the passage, the TV's line of sight and the overlaps piece by piece.
- **The last-played game** (`app/room_last_played.hpp`, GPU-free; `world_last_played.cpp`): each console room's placed
  game Retroverse started last (its play sessions, from the items files or from the catalog where the floor's build
  reads them), when the floor is built (`RoomCollection::lastPlayed`). It is the room's own item of that game, standing
  where Avvia's insertion leaves an item: half down in the console's media socket, upright, shrunk into the opening,
  its cover the room's. A collection of its own per room (`collection/lastplayed:<room>`) with one cover loader for the
  floor's proxies. It is hidden whenever the walker is not roaming free (Focus, Inspect, the insertion, the editor) and
  while a launch runs; a launch in the room that played its game (any end but a refusal or a failure to start: the
  `WorldLaunch` records the room and the game) moves the proxy to it at once; a console whose socket moves (its model
  came, I11 step 7) takes the proxy with it.
- **The run report's `world.lastPlayed`:** `heroDressing` (the floor scene's set pieces), `games` (room and game),
  `builds`, `played`, `resited`, `hiddenFrames`, `frontsResident`.

## Tests

- **Unit:**
  - `test_prefabs.cpp`: the six kinds in the kind list, their roles, sizes and sockets; a case for the set: the tops'
    anchors, the rack's decks and LEDs, a pad's cable ending where asked and never under the floor, none at its back,
    the joystick's stick, the speaker's drivers and LED on its front, an open box's halves, the refusals.
  - `test_era_furniture.cpp`: every console pack has a set, the arcade and the hub none; on the golden plans every
    colliding piece is a corner part inside the hero corner and under the ceiling, the validation of the plan with them
    has no hard finding and the same soft ones; on plan-57 every pack's set stays clear of the cabinet's top (the CRT,
    the console model's reserve) and the plaque, and no two pieces stand in one another; a room without a console
    draws no pads; a piece pushed out of the corner is not drawn; the props never collide; a moved part takes its
    pieces with it; the reader and its refusals.
  - `test_room_last_played.cpp` (new): the choice (the latest, the smaller GameId on a tie, the catalog's sessions over
    the items', none played), the pose at the insertion's end, the one-item layout and its group, the cover.
  - `test_prefab_manifest.cpp`, `test_prefab_resolver.cpp`, `test_material_library.cpp`: the manifests, families and
    materials; the scene goldens (`tests/fixtures/world/scene/`) rewritten.
- **GPU** (`test_world_launch.cpp`, `[lastplayed]`): the snes room's set drawn (at least ten pieces) and no proxy at a
  first start; Avvia with the insertion skipped puts its game in the console at the return, hidden meanwhile; the next
  start chooses it again from the catalog's session, its front resident.

## Departures

- **One manifest per kind:** the round side table is the `low_table` with `"shape": "round"` in the pack's piece.
- **No crate kind:** the 90s room's crate is a `storage_box` sized as one.
- **The proxy is not picked:** hovering it does nothing and Avvia stays the shelf's (BACKLOG); a console model without
  a media socket shows no proxy rather than one lying on it.
- **The editor knows nothing of the set:** a part it moves takes its pieces along, but its probe does not judge them
  (BACKLOG).
- **Goldens regenerated now**, not with step 4's: the step keeps every golden passing.

## Measured

**Gate D's measures** do not move on the fixture rooms' goldens (G02, G12, G16, G17: the median, the top quarter and
the shares within 0.1): from the doorway the set is a few small, dark pieces in the corner. It is seen from the seat
and from the corner's sides (checked by eye on the golden building's snes, psx, gb and shared computer rooms, Release
screenshots under `build/i12/hero/`). Goldens: G02, G02s, G06, G08a, G08b, G10, G12, G13, G16, G17, G19 and G22
rewritten on purpose; G07, G15 and G18 render as before (their metadata refreshed); G01, G11, G14, G20 and G21
untouched.

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 3.44 ms | 4.85 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 3.13 ms | 3.91 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 7.29 ms | 8.75 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 5.33 ms | 6.95 ms | informative |

**Memory:** the real library's busiest floor (computer, ten rooms, 5,314 items) peaks at 607.0 MiB against the 4 GiB
row (606.9 MiB at step 2): the set's meshes are shared per kind and its materials the library's.

**The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed): the library's
snapshots differ in none of their 542,121 entries; the memory rows pass, and on this quieter run the lobby's rows
pass too (shown 3.9 and 3.7 s after the start, under 5 s). The two ride rows that failed at step 2 still fail: the
warm ride to the computers' amstradcpc room times out (its view back at 2,648 ms against 2,500 ms, 761 of the room's
1,646 covers pending; the room whole 2,851 ms) and the lobby's arrays measured on the ride back are half the first
start's (2.4 against 4.8 MiB, the arcade's covers not yet resident). Step 1 passed them and step 2 failed them with
the gallery on and off, so they are step 2's or the machine's, not this step's; they are the first thing to look into
before step 4 (BACKLOG).

**The unit suite:** 2,641 of 2,644 at `-Jobs 8`; the three `app/world_flow` cases that failed there (a building job of
30 s under eight processes, past their flow's wait) pass alone and at the default jobs.
