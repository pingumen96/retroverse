# ADR-0010: The world generator and the WorldPlan

- Status: accepted
- Date: 2026-10-18

## Context

M2 (specification §22) asks for "a real room generated from the data" that "shows all the cartridges of the system, with
stability after an incremental import", with a persistent WorldPlan, procedural placement and an incremental update on
ROM addition; style §27 phase B asks for a room generated from the real data of an ES-DE library with 25 to 100
cartridges filled procedurally, and Gate B for a room that "already looks like the concept". Specification §7 describes
the generator (inputs, the 14-step pipeline, capacity formulas, determinism, the rules on a catalog change), §21.3 its
snapshot and property tests, style §5, §6, §13 the room template, the measures, the local seeds and the rebuild levels.
The approved plan sketched a design (one `console_room_9x10` template, XXH3 seeds, a JSON plan with a SQLite mirror,
steps 14 to 20 after M1) that none of I2's steps built.

A read-only survey of the reference library (2026-10-18) found:

- The SNES system holds **7,797 games**: 1,560 gamelist entries, 1,535 of them with a cover; the other 6,237 (hacks,
  translations, betas, BS-X episodes, a second GoodTools-named copy of the set) have no media at all and file names for
  titles. Among the 1,535 covered games there are **890 distinct display titles**, mostly a USA and a Europe release
  side by side under one name; deduplicated by title preferring the USA release, then Europe, then the rest: 727 USA, 57
  Europe only, 106 other. No SNES game is a disc member; every entry is available.
- Titles: two lines of about 20 characters hold 90% of the covered titles; 290 carry a `" : "` or `" - "` subtitle.
- Other cartridge systems look alike (nes 2,621 distinct covered titles, gba 2,311): the rules must not be SNES-only.

The documents disagree on the points this ADR settles: "all the cartridges" against "25-100 cartridges" and style §5.6's
"over 600: several expansion rooms, not an infinite wall" (C2); the plan's XXH3 against ADR-0008 §1, which dropped
xxhash (C6); specification §16.1's JSON files against §6.3's SQLite tables and ADR-0008 §6's "choices made later go to
the database" (C5); one room template against style §5.3's rectangular, L-shaped and lounge rooms (C15); several layout
candidates (style §13.5) against one (C14); the growth reserve (0.20 in specification §16.2, 0.15 in style §18.2) and
the passage widths (1.2 m, 1.4 m, 1.1 m minimum).

## Decision

### 1. Module

`rv_world` (namespace `rv::world`, `src/world/include/rv/world/*.hpp`), GPU-free like the catalog's libraries: on
`rv_core`, `rv_geometry` (the prefabs' slot sockets), `rv_assets` (manifests), `rv_scene` (the static part as a
`SceneDescription`) and `rv_catalog` (GameIds, the domain); `rv_forbid_link` keeps it from SDL3, glad, Dear ImGui,
RmlUi, SQLite, pugixml, `rv_media`, `rv_esde`, `rv_catalog_db`, `rv_platform_win` and `rv_platform`. It computes plans
and never creates a mesh, a texture or a GPU object (specification §7.2); the app materializes what a plan describes.
Its tests run in `rv_tests` (`tests/unit/world`). `retroverse-cli worldplan` (`generate`, `update`, `show`, `check`) and
`retroverse-cli where-is` run it from the command line. No ECS: the app keeps `app::SceneInstance` for the static part
and adds a collection layer (ADR-0011); EnTT is not adopted.

### 2. Inputs and outputs

`world::generate` is a pure function of a `RoomInput`, a `WorldConfig`, a `RoomTemplate`, the manifests, the previous
plan (or none) and the overrides, and returns a `GenerationResult` (the plan, a change report, a validation report):

- **RoomInput**: the system's entries as the catalog gives them on its worker (one bulk query, never a page per game):
  GameId, lookup key, file name, display title, catalog sort key and its version, year, rating, ES-DE favourite and play
  count, Retroverse's last session time, entry state and availability, media presence per kind (`covers`, `screenshots`,
  `titlescreens`, `videos`, ...). Text is used to decide, never copied into the plan (point 8).
- **WorldConfig**: the seed, the preset (`RetroMuseumCozy`, the one curated preset of style §18, theme pack
  `Classic16BitWarm` for the SNES), decoration density (0.4), growth reserve (**0.15**, style §18.2: the curated
  preset's value; specification §16.2's 0.20 is a default for the generic case), main passage width (**1.4 m**, style
  §18.2) with a hard minimum clear width of **1.2 m** (specification §7.5; style's 1.10 m is not used), the inclusion
  rule and the face-out threshold (point 3). Shipped defaults in `assets/world/presets/`; the user's
  `<user data>/config/world.json` overrides them per key, read and never written, like the other configuration files.
  The default seed is derived from the library id (point 7), so a library always gets the same world unless the user
  names another seed.
- **RoomTemplate** and **Manifests**: `assets/world/templates/console_room_9x10.json` (point 4), the platform visual
  manifest `assets/world/platforms/snes.json` (system to media family, theme pack, plastic colours, console prefab, the
  sign's text, all original and brand-free) and the media family registry `assets/world/media_families.json`
  (`cartridge_home_16bit`: the item's size from `media.cartridge.home16.generic`, 12.5 x 8.5 x 2.0 cm, its spine-out and
  face-out slot sizes, gaps and clearances). The generator resolves prefabs by manifest id and kind, never by a `.glb`
  path (specification §17.3).
- **Overrides**: `<user data>/worlds/<library>/overrides.json` (point 9); empty unless a test or, from I4, the editor
  wrote it.

### 3. Which games the room holds (the user's decision)

- **Eligible**: an entry in state game (not hidden, not a disc member) of the room's system that has a cover. When no
  game of the system has a cover, every game is eligible, so that a library without media still gets a furnished room
  (style §21's P0 requirement, golden G06). `WorldConfig.placement.include = "all"` makes every game eligible; the
  default is `"covered"`.
- **One cartridge per title.** The title key is the catalog's sort-key folding (`catalog::sortKeyOf`, its version
  recorded) of the display title (the gamelist name, else the catalog's cleaned file-name title) with every `(...)` and
  `[...]` group removed, the separators `" : "`, `": "`, `" - "` and `" – "` made one, white space collapsed. Among the
  eligible games of one title key the **representative** is the first by: region rank (0 when the file name's region
  tag, the first parenthesis made only of No-Intro region names or a GoodTools code, names USA or World; 1 when it names
  Europe; 2 otherwise, no tag included), then without a status tag (`Beta`, `Proto`, `Demo`, `Sample`, `Pirate`, a
  `[h]`, `[b]` or `[a]` mark) before with one, then the highest revision (`Rev N`, none counting as 0, as No-Intro's
  one-game-one-ROM sets choose), then the lookup key in ordinal order, then the GameId.
- The other versions, hacks, translations and games without media stay launchable from the quick catalog and are listed
  in the plan's `unplaced` with a reason: `duplicate-title` (with the representative's GameId), `no-cover`,
  `unavailable` (a missing file at its first consideration), `capacity` (point 5). Specification §21.3's "every active
  game has a valid placement or an element accessible in the global catalog" holds: the quick catalog lists every game.
- **Display mode**, decided at the room's first generation and kept: up to **200** placed titles (style §5.6's medium
  room) every collection shelf is face-out, as in the concept; above it the collection shelves are spine-out and dense,
  and the room's display fixtures (two low display cases and the featured wall shelf of the template) hold some dozens
  of face-out **featured** titles (44 in `console_room_9x10`): ES-DE favourites first, then by play count (ES-DE's plus
  Retroverse's sessions), then by rating, then by sort key and GameId. A featured title has no spine-out slot: one
  placement per title. A face-out room that outgrows its face-out modules goes on with spine-out modules; no module is
  ever converted.
- A title's representative is chosen once: a better-ranked version that arrives later is `duplicate-title`, and a placed
  game that loses its cover keeps its slot with a generated label. A game that becomes eligible later (scraped in ES-DE
  afterwards) is a new game for point 10.

### 4. The room template

`console_room_9x10` (template version 1), rectangular only in I3 (L-shaped and lounge rooms in I4):

- Room frame: origin at the floor's center, +X east, +Y up, **north = -Z** (ADR-0002, right-handed; the test room's hero
  wall is its north wall and `--spawn ...,0` looks at it), 9.0 x 10.0 m, ceiling 3.2 m, the entrance a 1.6 m doorway in
  the south wall (+Z) with 1.6 x 1.5 m kept free inside it.
- Zones (style §5.3, §6.1): the **hero wall** north (TV cabinet, CRT, console, the sign, a rug and an armchair turned to
  the TV, a lamp), visible from the doorway; **collection sockets** of 0.9 m along the east and west walls; one
  **door-reserved** socket of 1.8 m in each side wall, never occupied (M3 opens doorways there without moving anything);
  the **display** sockets (two low display cases and the featured wall shelf); **decoration** sockets (posters, lamps)
  on the south wall and the unused collection sockets; a central circulation band 1.4 m wide.
- **Growth**: when the collection sockets are full the room grows south in 2.0 m steps up to 16.0 m deep (style §5.3:
  modules of 2-3 m); beyond that the remaining titles are `unplaced` with reason `capacity` (linked expansion rooms are
  I4). Growth moves the south wall and what stands on it, never a collection shelf or a placed game.
- **Fixture order** (specification §7.3, style §13.4): doorway clearance, hero corner, collection modules (alternating
  west and east from the north corners, so the collection frames the hero wall), display fixtures, decoration within the
  density and only on sockets capacity does not need, lights. One layout candidate; style §13.5's LayoutScore is
  computed and reported (`validationReport.layoutScore`) but chooses nothing in I3.
- **Lights** (style §4.2, the renderer's limits): the ceiling key spot on the hero wall casts the shadows (the only
  shadowed light, as in the test room), one wall-washer spot per two collection modules, a pendant over the circulation
  band, the table or floor lamp, a picture light: 16 point and spot lights at most per view, checked by validation.

### 5. Capacity

From the manifests and the prefabs' own slot math (`geometry::buildPrefab("shelf", ...)`'s `Slot_<level>_<index>`
sockets), never from constants in the generator: `slotsPerLevel` and `levels` per module from the item's slot size
(spine-out: the cartridge stands on its end, its top face outward, a 2.0 x 12.5 cm spine, 8.5 cm deep; face-out: front
outward, as in the test room), the gap and the margins; `requiredSlots = ceil(titles * (1 + reserve))`;
`modules = ceil(requiredSlots / slotsPerModule)`. The reserve is spread over the levels: each level is filled to
`floor(slotsPerLevel / (1 + reserve))` items and keeps its last slots free, so that a new title lands near its
alphabetical neighbours. A spine-out module of 0.9 m has about 230 slots (33 per level at a 2.4 cm pitch, 7 levels
between 0.35 and 1.9 m): the reference library's 890 titles fill about five modules, the 2,000-title stress fixture ten.

### 6. Allocation

Placed titles in the order (catalog sort key of the representative, title key, GameId); featured titles first into the
display fixtures in that order, then the rest as one reads a page: the top level first, left to right as one faces the
shelf, then the level below, module by module in fill order. A placement names its slot, not a transform: the app
derives the transform from the fixture's prefab and the slot, plus a small deterministic offset from the placement's
seed (spine-out: pushed in by 0 to 4 mm; face-out: a yaw of at most 1.5 degrees), so that a row does not look machined.

### 7. Seeds and the random generator (ADR-0008 §1: no new hash library)

- `core::deriveSeed(parts...)`: SHA-256 (`rv/core/sha256.hpp`) of the UTF-8, NUL-separated tuple (`rv-seed/1`,
  parts...), its first 8 bytes big-endian. The world seed defaults to `deriveSeed("world", libraryId)`; local seeds
  follow style §13.3: `roomSeed = deriveSeed(worldSeed, "room", roomId)`,
  `fixtureSeed = deriveSeed(roomSeed, "fixture", fixtureId)`, `propSeed = deriveSeed(roomSeed, "prop", socketId)`,
  `placementSeed = deriveSeed(roomSeed, "place", gameId, family)` (integers in decimal ASCII), so that one decision
  never shifts another's random sequence.
- `core::Random`: SplitMix64, algorithm id `splitmix64/1` recorded in the plan, with its own integer and unit-interval
  distributions (the standard library's distributions are implementation-defined). Golden sequences come from an
  independent Python oracle (`hashlib`), as the GameIds' do.
- Identifiers never depend on the generator's random choices: `room:snes`, fixtures `room:snes/fx:<socket>` (`w03`,
  `e01`, `hero`, `dc01`, `door-e`), slots `room:snes/fx:w03/s:<level>.<index>`.

### 8. Determinism and the canonical form

The plan holds only integers and identifiers: lengths in millimetres, angles in hundredths of a degree, counts, GameIds,
ids, versions; no floating-point number, no title or path (titles and media are the catalog's, read when the room is
drawn; the plan file carries nothing from the user's library but GameIds). It is written as canonical JSON
(`core::writeCanonicalJson`: keys sorted by code point, arrays in a documented order, each placement and fixture sorted
by id, one element per line in the top-level arrays and no other white space, UTF-8), so that the same input, seed and
versions give byte-identical files in Debug and Release; its SHA-256 is the plan digest. Versions recorded in the plan:
`schemaVersion` (the format), `generatorVersion` `major.minor` (a minor change adds rules that keep every existing
placement; a major change may place games differently), `templateVersion`, `randomAlgorithm`, `titleKeyRule`,
`catalogSortKeyVersion`, and `inputDigest`, the SHA-256 of the canonical form of the inputs the generator read. No
unordered container is iterated into the plan; inputs are sorted by an explicit key first.

### 9. Persistence (no database tables)

- `<user data>/worlds/<library id>/world-plan.json` (the canonical plan), `world-plan.meta.json` (what is not canonical:
  when and by which build it was written, the duration, the change report's summary), `backups/` (the last 10 plans,
  newest kept, named by UTC time), `overrides.json` (pins: a GameId held in a slot, a fixture held in its socket; read,
  never written by the generator). Written atomically (`core::writeFileAtomic`, which flushes the file before the rename
  since I3) under a lock file of the world folder that the app and `retroverse-cli` both take; the data-directory guard
  runs before any of it is written.
- No tables: the catalog's schema v1 closed at Gate M1 and nothing here needs a migration. The plan is a few thousand
  placements (the reference SNES room 890, the stress fixture 2,000); read and indexed in memory (GameId to slot, slot
  to GameId) in well under 10 ms. **Where-is** is that index: `retroverse-cli where-is <gameId>` reads the plan, the app
  keeps the index of the plan it shows, and an unplaced game answers with its reason. A table becomes worth a migration
  only when worlds hold far more placements than a search result can map in memory (I4 measures it with the hub and
  several rooms).

### 10. Incremental rules (specification §7.6, style §13.6, NFR-08)

With a previous plan of the same `schemaVersion`, generator major version, template version, seed and preset:

- **Every existing placement is kept as it is**, also for a game that is now unavailable (its file missing or its folder
  unreachable: ADR-0008's identities keep their GameIds across renames and moves, so a rename moves nothing), and for a
  game that is no longer eligible (cover gone: it keeps its slot with a generated label). A placed identity that becomes
  an alias of another (ADR-0008 §4) hands its slot to that identity when the latter has none, else frees it; free slots
  are never compacted.
- Media and metadata changes rebuild no geometry (`NoRebuild`): the room swaps the textures and the spine text.
- A new eligible title takes the first free slot after its alphabetical predecessor's slot in fill order, then the first
  free slot anywhere in fill order (`LocalReflow`); with none free, a new collection module in the next collection
  socket without one (the decoration standing there is removed: decoration is never pinned in I3), then the room's
  growth (`RoomExpansion`); beyond the template's limit it is `unplaced` with reason `capacity`. A new version of a
  placed title is `duplicate-title`. New titles are never featured: display slots are filled only by a fresh generation.
- Overrides are applied last; a pin that names a game or slot that no longer exists is reported as orphaned and never
  moved. A change that would move a pinned object is not applied and is reported (`needsConfirmation`): the confirmation
  dialog of specification §7.6 point 8 comes with the editor (I4).
- Another seed, preset, template version or generator major version gives `RequiresExplicitRegeneration`: the previous
  plan is kept and shown, the app says so (a notice on the quick catalog), and
  `retroverse-cli worldplan generate --replace` regenerates after a backup. Regenerating from the app with an impact
  preview is I4's.
- The change report lists the placements added, kept, orphaned and refused, the fixtures added, the rebuild level and
  the plan digests before and after.

### 11. Validation (specification §7.3 step 13, §21.3)

Hard rules (a plan that breaks one is never saved or shown; the app keeps the previous plan, or the empty room shell and
the quick catalog when there is none, and logs every violation): no two fixtures' footprints overlap (axis-aligned boxes
in millimetres) and none enters a door-reserved socket or the doorway's clearance; a 0.1 m navigation grid with every
obstacle dilated by half the minimum clear width reaches, by breadth-first search from the doorway, the interaction
point of every shelf module, display fixture and the hero TV; every eligible game has exactly one placement or one
`unplaced` entry; no slot holds two games; lights within the renderer's limits; the room within the template's size
limits. Soft rules (reported, the plan kept): the hero TV in line of sight from the doorway, the main passage at least
1.4 m wide, the LayoutScore and decoration density.

### 12. Where it runs

Generation and incremental updates run on the app's catalog worker as one more job (the worker that holds the database's
writer, I2 step 13; step 13r's import job is its neighbour), from the bulk snapshot query; the result is an immutable
`GenerationResult` posted to the main thread, which commits it at a frame boundary (specification §15.3, §18.2). At
start the app reads the persisted plan on the worker and shows the room at once, then reconciles it with the catalog's
snapshot (the incremental rules) and applies the difference; after an import (step 13r) it does the same. Frame-limited
runs with a fixed frame time wait for the plan between frames, bounded like the catalog's lockstep.

## Consequences

- The room shows one cartridge for each of the 890 titles of the reference SNES collection that have media, spine-out
  and dense, with some dozens face-out; small libraries get a face-out room like the concept. The 6,907 other SNES games
  stay one search away in the quick catalog, each with its reason in the plan. M2's "all the cartridges of the system"
  is read as "every title with media, one cartridge each" (the user's choice, 2026-10-18).
- Rooms are reproducible from (catalog, seed, versions), so snapshot goldens and property tests (specification §21.3)
  guard them; a change of the rules that moves games is a generator major version and needs an explicit regeneration.
- The plan file holds no titles or paths, so it can be attached to a bug report without leaking the library's content
  beyond GameIds.
- Very large systems overflow into `unplaced` with reason `capacity` until I4's linked rooms; the 9 x 10 m room holds
  about 16 spine-out modules (some 3,200 titles with the reserve) and the template's limit, 16 m deep, about 28 (some
  5,600), above every system of the reference library once deduplicated.
- No migration: overrides and pins live in a JSON file the editor of I4 will write, which revisits ADR-0008 §6's
  "choices go to the database" for world state only.
