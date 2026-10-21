# I4 step 4: shared rooms and the arcade room

Roadmap row: I4 step 4. Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §3, §6, §7, §11.

## What was built

- **The threshold.** A preset and `world.json` key `sharedRoomMinTitles` (integer, 0 to 1,000,000; the shipped preset
  30), in the configuration's canonical form. `selectRoom()` gives each selection `distinctTitles`: the title keys of
  every game but a missing file, with a cover or not (the titles under include "all"), a key of the selection's
  canonical document. Callers need no new input.
- **Entering the building** (`generateBuilding()`, order: floor, systems.json `order`, name):
  - every arcade system takes the next section of `area:arcade` on the ground floor, whatever its size;
  - a system with fewer distinct titles than the threshold takes the next section of `area:shared:<floor>`;
  - any other gets its console room;
  - a shared or arcade room enters its floor when its first section does, placed like any room (its width decides the
    rooms after it; its z follows its depth once its sections are known);
  - on a full floor a console room joins the floor's shared room when it stands; a shared room that finds no place has
    its sections' games unplaced for capacity, naming it (`room-unknown` accepts that, as for a console room).
- **The rooms.** `console_room_9x10` v2 unchanged, family `generic_case` (`kSharedRoomFamily`), the platform
  `sharedRoomPlatform()`: no console (the hero corner's console part left out by kind), the theme pack and plastics of
  the defaults of the floor's first kind (arcade's for the arcade room), the sign of those defaults (`systems.json`'s
  new optional `sign`: arcade's "ARCADE"), else the fallback's ("MORE GAMES"). The CRT stays. The scene builder draws
  them with that platform.
- **Order by section.** The collection is ordered by (section, sort key, title key, GameId): the face-out titles are the
  room's D + B by priority across its sections, each section's titles follow the previous section's, and the update's
  predecessor key carries the section. A test proves a shared room's generation and update equal to a console room's
  whose sort keys lead with the section. Placement seeds stay per game and family.
- **The data.** `loadWorldData()` of a system also reads the data of the shared or arcade room its section would stand
  in (`WorldData::sharedRoom`, `WorldData::kind`; the prefabs both name measured once), so `generateBuilding()` and its
  callers (the CLI, the app) keep their signatures and inputs. `loadRoomData()` reads a stored shared room by its kind
  and floor. `updateBuilding()` takes either.
- **The update.** A selection goes to the room that holds its system's section, whatever its count now (sticky). A
  shared or arcade room is updated with the selections of all its sections; with some missing it is left as it is, its
  sections' selections not read, since the plan does not say which section an unplaced game was of. New systems are
  still refused (step 6a).
- **CLI.** `worldplan update` updates every room by default; `--system` of a section updates its whole room; selection
  files must give every section of a shared room (exit 2). [cli.md](../reference/cli.md).

## Decisions

- **The count is part of the selection's canonical document**, so the `inputDigest` hashes what decided a room. With the
  new configuration key, every plan's `inputDigest` line moved once; nothing else in the I3 goldens.
- **The I3 room suites generate with `sharedRoomMinTitles` 0** (`roomConfig()`), as ADR-0013 §5 asks: a room of any size
  stays a console room. So do G06's smoke case, the families smoke cases and the app's custom-system test, by their
  data directory's `world.json`.
- **A system's data always comes with its shared room's**, rather than a new input for the callers: `loadWorldData()`
  of snes with both reads in about 23 ms in Release (the bench of `world/generation`, the machine busy).
- **The shared room's look is its floor's**, not its first system's: the defaults of the floor's first kind. A sign for
  every system of the room is the styling pass's.

## The goldens

- `building-esde-seed-102938.json` rewritten: mame moves into `area:arcade`, amiga (20 titles, under 30) into
  `area:shared:computer`, pocketlab (gb's row, entering first) and pokemini into `area:shared:handheld`. The rooms of
  gb, psx and snes are byte for byte as they were. Debug equals Release.
- Every other plan golden: its `inputDigest` line only. The selection goldens gain `distinctTitles`. The floors' scene
  digests move for ground, handheld and computer; gen4's scene file is unchanged.

## Proof by eye

The building fixture imported and planned by `gpu.world floor`'s case (`worldplan generate --seed 102938`: area:arcade,
area:shared:computer, area:shared:handheld, room:gb, room:psx, room:snes), `worldplan show --floor ground | handheld |
computer --scene`, rendered with `retroverse.exe --scene` (Debug, frame-limited, hidden; no GL message):

- the lobby from the elevator: the arcade room's doorway on the north side, the entrance doors at the east end;
- the arcade room from its doorway: the "ARCADE" sign, the CRT with its colour bars on its cabinet and no console, the
  display cases' riser cards (its 15 titles stand face-out; a floor scene draws no cartridges), the hero modules
  dressed;
- the handheld hall from the elevator: gb's doorway north, the shared room's south;
- the shared room from its doorway (the south row, turned): "MORE GAMES", the CRT, no console; up close the CRT alone
  on its cabinet, where gb's room across the hall shows its console beside it;
- the computer floor's shared room (amiga) alike.

## Deviations and limits

- **The app until step 5a** shows a system's console room only: a system in a shared or arcade room has no room to show
  there. The families smoke case of mame (`--room-system mame`, now `area:arcade`) fails on this branch alone; step
  5a's `--start-room area:arcade` replaces it at the merge.
- In the building fixture amiga's 20 titles fall under the threshold too: the computer floor's shared room holds them,
  as generic cases (its big-box room shows only with `sharedRoomMinTitles` 0, as the families smoke case writes).
- G02 to G10's JSON files record plan digests that moved with the `inputDigest` (their test only warns).

## Follow-ups

- **Step 5a:** `--start-room area:arcade` and `area:shared:<floor>` in the app; the families case of mame.
- **Step 6a:** a new small system joins its floor's shared room as the last section.
- **The styling pass:** a sign per section, the arcade cabinet.
