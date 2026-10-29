# I4b step 8: expansion rooms in the app

Roadmap row: I4b step 8. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §7 and §4 (both amended on
2026-10-29 by this step). Plan: [plan.md](plan.md), "Risks and open questions" items 11 and 12.

## What was built

- **The signs** (`world::chainSignText()`, applied by `loadSceneData()`): an expansion's platform sign is its head's
  with a space and its number, so its portal's plaque, its blade signs, the hub's fascia sign and its own neon sign in
  the room read "HOME COMPUTERS 2", "ARCADE 2"; a head's stay as they were (no golden moves).
- **The directory's chains** (`app::directoryBuildingOf()`, `ui::DirectoryRoom::chainIndex`, `chainRooms`, `ranges`,
  `scattered`; `directory.rml`): a floor lists a chain's rooms together, the head where it stands along the hall and its
  expansions after it by number (a gap kept), each expansion indented and named with its number ("Lumen Desk Computer
  2", "Sala condivisa 2", "Sala giochi arcade 2"); each room of a chain shows its count and its titles' range ("850
  giochi · A – Mo"), a shared room's per section it holds titles of (its system's name before each range). The ranges
  (`chainRangesOf()`, `rangeInitial()`, `rangePrefix()`) come from the rooms' items files (`chainTitlesOf()`), read on
  the pool once per plan digest and per build that wrote the items files (a first start writes them after its plan
  shows); a plan without a chain reads nothing. "Modifica stanza" and "Rigenera stanza" name an expansion with its
  number and a chain as "tutte le stanze di ...".
- **Navigation**: the directory's row of an expansion teleports to its doorway (or rides to its floor); "Mostra nella
  stanza" and where-is lead into it (the plan's index already named its room: proven, no code changed).
- **The room**: an expansion's CRT plays its own placed games (ADR-0013 §9's rule, unchanged: proven on the walk).
- **The editor within a chain** (`WorldEditState::chain`, `WorldSession::enterEditRoom()`,
  `EditInteraction::enterRoom()`, `heldSlot()`): a session edits the chain of the room it began in; the walker entering
  another room of it while browsing or holding a game makes that room the edited one (its targets, fixtures and the
  bar's name with its number, "Modifica stanza · Lumen Desk Computer 2"), the held game drawn in the hand from its own
  room's collection and kept hidden there, so it drops into a free slot of the expansion (a move) or onto a game there
  (a swap): one operation, both rooms laid out again (`templateOf()` keeps the chain's scene data, and
  `editedRoomCollections()` gathers the titles and covers over every room it lays out, so the game keeps its own in the
  room it enters). The run report's `world.edits[].rooms` lists the rooms a session edited.
- **The notice** (`world_notice.rml`'s `rooms-added`, `WorldSession::followRoomsAdded()`): when an update that saved the
  building added expansions, the notice names the first ("Sala giochi arcade 2") and counts the others, after the
  build's own notice; the run report's `world.roomsAdded`.
- **Loading**: the app's update reads a chain's data once, its head's (`world_build.cpp`), as the CLI does.

## Decisions beyond ADR-0015

- **How ranges show** (ADR-0015 §7 as amended): the initials of each room's first and last title in the chain's
  alphabetical order (the head's sections, the sort key, the GameId), so that a range never leaves out a title its room
  holds; two letters where two rooms meet on one initial (the overflow fixture's invented titles split inside "M": "A –
  Mo", "Mo – Y"); overlapping ranges after updates shown as they are, never trimmed to a "main run" (which would send a
  reader to the wrong room), with "in ordine sparso" on a room whose titles interleave with another's; a shared room's
  chain per section, only the sections a room holds titles of.
- **The editor's scope is the chain**, not the room the session began in (ADR-0015 §4 as amended): the plan's risk 4
  ("games move only within their room chain") is met by carrying the game through the hall, the way a user would; no
  menu action "move to the next room" was added. Furniture keeps its room: a held fixture, the menu or an edit being
  applied keep the room edited.
- **The notice after updates only**: a first building's chains and a regeneration's (its preview counts the rooms)
  raise none.
- **Ranges read off the main thread** (a lockstep run waits for them, `chainRangesWait()`): an expansion's items file
  holds 1,700 titles on the fixture; reading it on a frame would stall it.

## Proofs

- Unit (`tests/unit/app/test_directory_chains.cpp`, 5 cases): a chain together under its head on a plan made in the
  test (expansions by number wherever they stand, a gap kept, a room alone as before); each room's ranges, a shared
  room's per section, the catalog's full names; an unknown title left out; an update's addition making two rooms'
  ranges overlap, "scattered" on both; a boundary on one initial in two letters; the initials and prefixes; the systems
  read and the titles from an items file; the directory's teleport, "Mostra nella stanza" and where-is into an
  expansion. `test_world_expansion_signs.cpp` (2 cases): `chainSignText()`, and on the overflow counts' fresh building
  the computer floor's plaque and blade signs and each room's neon, the hub's fascia and portal signs of
  `area:arcade:2`. `test_world_edit.cpp`: a game carried into another room of its chain (held from no slot there, a
  swap and a move into the expansion, back in the head its own slot again, a game an undo moved let go of as before).
  `test_directory.cpp`: a chain's rows and the expansion chosen, the selection kept by id; the document's bindings.
  `test_world_notice_document.cpp`: the `rooms-added` block. `test_run_report.cpp`: `roomsAdded`, `edits[].rooms`.
- GPU (`tests/smoke/test_world_expansion.cpp`, 5 cases on `esde/overflow` planned fresh): the arcade's 60 sets placed
  (30 and 30, none for capacity), `area:arcade:2` a start room; the hall's portals, a walk from the hall into
  `room:amiga:2` (the eye's room, its CRT's playlist of its own games), a face-out cover of it inspected (the hero
  texture on the item in the hand); the directory's chain, the teleport from its row, "Mostra nella stanza" of an
  expansion's game from the head (a teleport, then its Focus pose); the editor carrying a display case's game from
  `room:amiga` through the hall into a free slot of `room:amiga:2` and saving it (the stored plan 849 and 851, one pin
  of the editor); an update adding 20 arcade sets over a room at its limit (`area:arcade:2` added, the notice
  `rooms-added`, the next start silent).

## Screenshots

`build/i4b/step8/` (from the GPU cases' runs): `hall-portals.png` (the computer floor's hall: "HOME COMPUTERS" and
"HOME COMPUTERS 2" on the plaques and blade signs), `directory-chain.png` (the computer floor: the hall, "Lumen Desk
Computer · 850 giochi · A – Mo", "Lumen Desk Computer 2 · 850 giochi · Mo – Y" indented), `inside-expansion.png` (its
neon "HOME COMPUTERS 2", its CRT), `inspect-expansion.png`, `arcade-expansion.png` ("ARCADE 2"),
`carried-into-expansion.png` and `holding-over-free-slot.png` (the bar "Modifica stanza · Lumen Desk Computer 2", the
game in the hand), `moved-into-expansion.png` (the game dropped there with its title and cover),
`show-in-expansion.png` ("Mostra nella stanza": its Focus pose in the expansion), `notice-rooms-added.png` (the lobby: "Una nuova stanza per la collezione", "Sala giochi arcade 2").

## Real library

`./scripts/real-library.ps1 -Filter '[real-library][building]'` in Release, ES-DE closed, 0 differences among 542,120
entries. Nothing changed for the reference library, which has no chain: its directory lists 44 rooms on 9 floors, each
alone without a range, no items file read for ranges, no room added (`world.roomsAdded` empty at every start); the
building planned twice gives one plan digest and one file SHA-256 (19,985 placements, 71,290 unplaced), the unchanged
starts plan nothing (skipped by the stamp) and the update writes nothing. The timings did not hold this time: the run
shared the machine with two other worktrees' GPU suites and a build (the CPU at 100%, the import 40.0 s against step
7's 12.4 s), and the lobby showed 5.5 s and 9.0 s after the process started (the quick catalog's first page 4.0 s and
7.6 s; the 5 s row), and the cold ride back to the lobby reported its arrays before the arcade room's second array came
in (2.5 MiB against 5.0 MiB; the run's end equal). The step changes nothing those runs execute on a plan without a
chain (the signs of rooms alone, the ranges' read skipped, the update's data unchanged), so these are the load's; the
row is to be measured again on a quiet machine (the gate's run).

## Release

`./scripts/test.ps1 -Preset msvc-x64-release-gpu`: 2,343 of 2,344 passed; `gpu.world floor: light reaches through the
doorways ...` timed out under the same load and passed alone (376 s). The goldens G01 to G19 skipped: the machine's
driver is NVIDIA 621.01, the goldens were recorded under 617.42 (`docs/testing/reference-hardware.md`: compared on the
reference machine only).

## Goldens

None changed: a room alone keeps its sign, every golden plan and scene holds rooms alone, and the step adds no golden.

## Follow-ups

- A room of a chain shows its range once the items files are read; a plan whose items files are missing (a first
  start before the building job) shows none until they are written.
- The editor's session keeps one draft for the chain; a fixture cannot move from one room of the chain into another
  (ADR-0015 §4: furniture stays in its room).
