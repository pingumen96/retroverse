# I4 step 5a: the floor in the app

Roadmap row: I4 step 5a. Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §1, §6, §9, §11; the building
API frozen in [step 3](step-3.md).

## What was built

- **The building job** (`rv/app/world_build.hpp`, `planBuilding()`, one job on the worker pool): every system's
  snapshot read within one read transaction (`takeBuildingSnapshot()`, `rv/app/room_snapshot.hpp`) on a read-only
  connection of its own that the catalog worker opens with the catalog (`CatalogOpened::world`,
  `AppCatalog::worldReader()`), with the aliases of the placed games (`SqliteCatalogQuery::aliasTarget`). The systems
  are those with a room (`roomSystemsFilter()`: systems.json's classification). The selections are merged by system and
  planned as `retroverse-cli worldplan generate` and `update` do: a first building from every system with a placeable
  title, or the update of the stored building's console rooms. The plan is validated and saved under the world lock.
  Each system's room items file (`cache/world/<library>/room-<system>.json`) is written only when it changed.
  `kRoomSystem` and the catalog worker's room job are gone.
- **The floor-build job** (`buildFloor()`, keyed by floor and plan digest): `toFloorSceneFile()` of the start floor,
  then per collection room its `collectionLayout()` moved into the scene's frame (`floorCollectionLayout()`), its covers
  from the items, and its doorway's passage as a box (`RoomCollection`). It reads no catalog: the stored floor at start
  is built the same way from the items files.
- **The scene's frame**: the start room's when `--start-room` is given, else the floor's. The floor's scene file is
  moved by the start room's inverse placement (`moveSceneFileToRoom()`), every number rounded again to the scene
  builder's 1e-6 m and 1e-4 degree. The round trip is exact, so the start room's objects keep the coordinates of its
  standalone scene and G02-G10 stay 0 pixels apart. `--spawn`, input scripts and the report's walker poses are in that
  frame.
- **Residency of one floor** (`world_session.cpp`): one `CollectionInstance` and one static group per collection room.
  Every registry key is `collection/<room id>/<family>/...`, and the picture sets carry the room id, so two rooms of
  one family sample their own arrays. One `CoverLoader` serves the floor with one upload budget. Its view groups are
  the room that holds the eye plus the rooms whose doorway passage the frustum meets; the rest load by distance.
  `WorldHover` keeps a pick index per room and switches to the eye's room (`scene::areaAt` on the floor's areas) once
  nothing is hovered or pulled out. Hover, pull-out, Focus, Inspect, the card and launches work in that room.
- **The start**: the ground floor's lobby at the elevator's arrival point, facing east, with no hero CRT
  (`findHeroScreen()` with an empty prefix). `--start-room <room id>` starts at that room's doorway camera on its
  floor, its CRT the hero CRT. Once the plan exists, a room it lacks fails the run with exit 2 (`AppContext::failRun`
  with a code). `--room-system` is removed. The shell before a plan is the start room's empty room, else the lobby.
- **Run report**: `world.floor`; `world.rooms[]` `{id, floor, x, z, yaw}` (the placements on the floor);
  `world.room`, the room holding the eye at the end ("" in a hall); `residency.rooms[]` (`room`, `items`,
  `frontsResident`, `spinesResident`, `pending`, `arrayBytes`, `arrays`, `ownArrays`), the floor's totals beside them.
- **Tests**: every `gpu.world*` case, G02-G10, the benchmark path and the `[real-library]` room tests run with
  `--start-room room:snes`. `gpu.world families: ...` runs through `--start-room <the system's room>` read from the
  plan. `gpu.world floor app: ...` (`test_world_floor_app.cpp`, `building_support.*`) covers four things on
  `esde/building`:
  - two same-family rooms read from the plan, each sampling its own arrays and showing its own covers up close;
  - a walk from the hall into room A, back out and into room B, and a cover of B inspected through the quick catalog's
    "Mostra nella stanza";
  - the lobby start, and a `--start-room` the plan lacks refused with exit 2;
  - the residency per room and the floor's GPU memory within 512 MiB.

## Decisions

- **The building job waits for a refresh that is running.** It is queued while the catalog's refresh writes, then
  reads the refreshed catalog. Lockstep runs with `--refresh-library` then plan the same building every run.
- **The update covers the stored building's console rooms only.** A system without a console room in the stored plan is
  logged and waits for step 6a (new systems, sections and floors).
- **One floor at a time**: the start floor. The other floors are planned and saved, not built (step 7's ride).
- **The active room switches only in free roam**, with nothing hovered or pulled out. A Focus or Inspect never jumps to
  another room's pick index mid-gesture.

## Deviations and limits (step 6b's)

- The persisted start previews and reconciles the **whole start floor**: the same plan, floor and collections give
  `textures`, anything else rebuilds the floor with the walker kept. Reconciling room by room is 6b's.
- Live updates after a refresh apply to the whole floor the same way (`ShownFloor`, `planLiveUpdate`). The hero CRT
  stays bound to the start room's CRT and is not rebound while walking (none in the lobby).
- "Mostra nella stanza" serves the room that holds the eye. Another room's game says it is not in the room (across
  rooms and floors is step 7's).
- The items files hold no CRT media paths, so the hero CRT's games come from the building job's snapshots, not the
  preview.

## Measurements

On the reference machine, `esde/building` (9 systems):

- The building job, from the snapshot to the floor committed: 150 ms in Release (the snapshot of 8 systems 5 ms),
  2.5 s in Debug.
- The `handheld` floor (`room:gb`, `room:pocketlab` half-turned across the hall, `room:pokemini`; 44 items): each room
  in 2 arrays of its own (2.4 MiB), GPU memory peak 252.9 MiB. The lobby (`room:mame` on `ground`): 213.5 MiB. The
  acceptable limit is 512 MiB.
- The benchmark path (`[benchmark]`, `snes-stress`, `--start-room room:snes`) in Release: 1,230 frames, frame p50
  2.70 ms, p99 3.43 ms; the GPU's total p99 1.50 ms.
- G01 to G10 are 0 pixels apart in Release and Debug, with their images unchanged. The goldens' metadata were
  rewritten for the command (`--start-room room:snes`) and the building's plan digest.
