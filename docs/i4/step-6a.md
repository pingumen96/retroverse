# I4 step 6a: the building update

Roadmap row: I4 step 6a. Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §3, §5, §7, §8, §11.

## What was built

- **New systems in the update** (`rv/world/incremental.hpp`'s `updateBuilding()`). A selection whose system no room
  holds is no longer refused. The system enters among the rooms that stand, by the fresh building's rules and in its
  order (floor order, systems.json order, name):
  - an arcade system takes a section of `area:arcade`, a system under `sharedRoomMinTitles` distinct titles a section
    of its floor's shared room, any other its own console room;
  - a section is appended after the last one of its room when that room stands; its titles are new titles of the room
    (spine-out, after the previous section's);
  - a new room is generated as a fresh building generates it (its own face-out titles), placed at the east end of the
    shorter row of its floor, its z following its depth; a floor that held no room appears;
  - on a full floor a console room joins the floor's shared room, else the system's games are unplaced for capacity,
    naming the room it would have had.
  Nothing that stands moves. The change report gains `roomsAdded`, `systemsEntered` and `systemsWaiting`.
- **One entry code** (`detail::enterBuilding()`, `generateEnteredRoom()`): `generateBuilding()` is the entry into a
  building of no room. Every plan golden is unchanged, and the update of the building of no room equals the fresh
  building byte for byte (`building-esde-seed-102938.json`).
- **The building's NFR-08 property suite** (`tests/unit/world/test_world_building_suite.cpp`), on a test-only template
  (`tests/fixtures/world/building-suite/`: one side-wall socket, one growth step) with test copies of building.json
  (two rooms per row) and systems.json. Random sequences of titles arriving and leaving, bursts that add modules, grow
  rooms and reach capacity, new systems (large, small, arcade, a custom system with gb's platform tag, one no tag
  places), threshold crossings, full floors and selections left out. After every step: every placement as it stood;
  every stored room's floor, kind, x, yaw and expansion head kept, its sections a prefix of the new ones; z changed
  only with growth; the halls' lights only appended; every selected game placed or unplaced exactly once; no hard
  finding; a new room at the east end of its row; a left-out system's room unchanged. 2 seeds of 8 steps in Debug
  (about 12 s: entries, growth, crossings, capacity), 16 of 24 in Release (about 20 s: also held rooms and a waiting
  system).
- **A golden update sequence** (`building-update-esde-seed-102938.json`): the building fixture's systems, then
  `additions/`, then neogeo (a section after megadrive), fbneo (after mame) and nes (its own room on a new floor,
  `gen3`). Debug equals Release.
- **CLI.** `worldplan update` without `--system` or `--selection` reads every room of the plan and every other catalog
  system that gets a room and has a placeable title. A named system without a room enters. The report prints
  `Entered:`, `Waiting:` and `Room held:` lines (JSON `systemsEntered`, `systemsWaiting`, `roomsAdded`, `heldRooms`,
  `leftOut`). [cli.md](../reference/cli.md).
- **The building fixture's `additions/`** through the CLI: an update of snes, psx and pokemini places 34 titles; the
  whole update then opens gen4's shared room for megadrive's 12 titles (under 30) in the south row; pokemini, past 30,
  stays in its section; nothing moves.

## Decisions

- **A system waits for a held room.** A system that would take a section of a shared or arcade room whose sections
  are not all given (held, or not updated at all) does not enter: the plan does not say which section an unplaced
  game was of, so the room cannot be updated in part. It is reported (`systemsWaiting`) and enters with the next
  update given every section. From the catalog, the CLI reads that room's sections itself, so a named new system
  never waits there.
- **A kept room takes the section.** A shared room kept as it is (ADR-0013 §5) has the new section appended to its
  record, and the section's games are `room-kept`: every system stands in one room, every game has its entry.
- **Capacity entries are recomputed.** The entries of a system that found no place name a room the plan does not
  hold. An update given that system's selection drops them and enters it again; without its selection they stand
  (SC-06).
- **Digests.** An entering system is hashed with the data of the room it stands in, as a fresh building hashes it, so
  the next update with nothing new writes nothing.
- **Expansion rooms stay I4b's** (step 2's census: no reference system overflows a room grown to its limit).
- **The suite reads each system's data once**: a stored room's data is its first section's `loadWorldData()`, which
  `updateBuilding()` accepts when the family and theme pack are the room's (elsewhere `loadRoomData()`, as the CLI).

## The real library

Through `scripts/real-library.ps1 -Filter '[real-library][room_selection]' -Measure <a local script>` (ES-DE closed;
the Release CLI; data directories under `build/i4-real`, never committed): 0 differences among the snapshots' 542,120
entries, the tests passed.

- **The building twice**, `library create`, `import` and `worldplan generate` in two data directories: one SHA-256
  (`365db53f…`). The import took 20.8 s cold and 12.7 s warm; the whole building's generation 14.4 s.
- **A second import** wrote nothing (5.4 s); the **update** after it was Updated, no-rebuild, 19,985 placements kept,
  nothing written (14.3 s, the catalog's selections included, as for the generation).
- **The lift**: the real schema-1 SNES plan (generator 1.2) as the stored plan of a third data directory, then the update
  of the whole library. The SNES room's record and its 28 fixtures, 890 placements, 3 dressed levels, 10 lights and
  6,907 unplaced entries equal the oracle's; 46 systems entered 43 new rooms (19,095 placements). The building is
  **byte for byte the fresh one** (the same SHA-256). A second update wrote nothing.
- **The building**: 9 floors (ground, gen2 to gen7, handheld, computer), 44 rooms. Rooms per floor: ground 1, gen2 6,
  gen3 4, gen4 4, gen5 6, gen6 3, gen7 2, handheld 8, computer 10. Shared sections: arcade `mame`; gen2 `channelf`;
  gen4 `segacd`; gen5 `pcfx`; gen7 `wii`, `ps3`; handheld `psp`, `pokemini`; computer `pc`, `windows`.
- The largest room is `room:nes`, 2,217 placements, not grown; no room grew. 19,985 placements; 71,290 unplaced: 53,761
  no-cover, 16,498 duplicate-title, 1,031 unavailable, **0 capacity**. 148 catalog systems were left out: 3 excluded,
  145 with no placeable title.

## Deviations and limits

- **The app.** It still updates its SNES room only on this branch. Step 5a's building job passes the console rooms'
  selections only; at the merge it should pass every system's (shared rooms' sections and systems without a room
  too), with `loadWorldData()`'s data for a system without a room. The app shows floors from 5a, so no smoke case is
  added here.
- A system waiting for a held room has no entry in the plan meanwhile: the quick catalog cannot say where it will
  stand.

## Follow-ups

- **Merge with 5a:** the building job's update with every system (above).
- **Step 6b:** the app's update of the resident floor room by room, with the systems that entered.
- **I4b:** expansion rooms, per-room regeneration.
