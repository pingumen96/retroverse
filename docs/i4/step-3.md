# I4 step 3: floors of console rooms

Roadmap row: I4 step 3. Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §1, §6, §7, §9 ("Areas" and
"Lights"), §11, §12.

## What was built

- **A floor's scene** (`rv/world/floor_scene.hpp`): `toFloorSceneFile(plan, floor, data)` composes one floor in the
  floor's frame. Every room's `toSceneFile()` output is moved by its placement (a half turn for the south row), its ids
  and light names prefixed with the room's id (`room:snes/fx:hw-w`, `room:snes/room`), its doorway's stop left out
  (`geometry::DoorwayParams::stop`, JSON `"stop": false`; the default keeps the standalone room's scene and digests).
  The derived hall comes after the rooms. The scene's camera stands at the arrival point in front of the elevator,
  facing east, its far plane sized to the floor. `loadFloorSceneData()` reads building.json and each room's
  `loadSceneData()`.
- **The derived hall** (`rv/world/floor_layout.hpp`, `deriveHall()`), integers in millimetres:
  - The interior runs from x 0 (the elevator) to the easternmost room edge, at least 6 m, 3.0 m wide (the lobby 6.0 m)
    and 3.2 m high.
  - Box walls of the wall allowance close it where no room's doorway wall stands: the west and east ends and the gaps
    of each row. They collide by their bounds; the floor and ceiling slabs do not collide.
  - Lights hang on the centre line at x = 3, 9, 15 m ... while below the east end, so a longer hall appends lights and
    moves none. They use the profile's pendant colour, intensity and range, with a lamp box under each.
  - The elevator stand-in: a graphite frame, two closed brushed leaves, a call panel with a lit button. The floor's
    sign hangs above it (building.json's new optional per-floor `sign`; not hashed into the plans' `inputDigest`).
  - The proximity zone (`Hall::elevatorZone`, 1.5 m deep, 2 m wide) and the arrival point (1.2 m east) are records for
    step 7.
  - The lobby has closed entrance doors at its east end.
  - Each room's doorway (`FloorDoorway`) is its template's at its growth step, aligned with the hall by construction.
- **Areas** (scene file `"areas"` and each light's `"area"`; `rv/scene/scene_areas.hpp`):
  - A floor's areas are each room's interior plus its doorway's passage, then the hall's interior (`floorAreas()`).
  - `scene::AreaLighting` follows the eye: the first area holding it, else the last one held.
  - The view packs only that area's lights: its key light (its first caster) shadowed, then its point and spot lights
    nearest the eye up to 16 (`gl::packLights` over a selection). The selection is one pass over the area's lights and
    is repacked only when it changes. `gl::PackedLights::keyLight` is the packed key light's index in the scene's
    lights: the log, the run report, the Lights panel and `requireShadowCasters()`'s note name the eye's area's key
    light by it, never the scene's first caster.
  - The log names every area's key light at load (or why it casts no shadow), not only the start area's.
  - `app::SceneInstance::followEye()` runs before every view. A scene without areas packs once, in list order, as
    before.
  - The run report gains `sceneAreas`: the area at the end, its lights and key light, every change of area.
- **Building validation** (37 rules): `hall-overlap`, `doorway-off-hall` and `floor-unreachable`. The last searches the
  floor's navigation grid (0.1 m cells over the hall and the rooms) from the arrival point. Its obstacles are the hall's
  walls, the elevator, the rooms' walls around their doorways and their fixtures. Each doorway's clear area must be
  reached. These rules run when the context has the building, as in `generateBuilding()`, the update and
  `worldplan check`.
- **CLI**: `worldplan show --floor <id>` summarizes a floor's hall and rooms, and with `--scene <file>` writes the
  floor's scene (JSON `floorScene`, `sceneFloor`). It excludes `--room`. [cli.md](../reference/cli.md).
- **Goldens**:
  - `tests/fixtures/world/golden/building-esde-seed-102938.json`: the building of esde/building's seven systems
    (synthetic selections, pocketlab classified by gb's tag), compared byte for byte in Debug and Release.
  - `tests/fixtures/world/scene/building-esde-seed-102938.floors.json`: its five floors' scene digests, with gen4's
    scene kept whole.
  - No existing plan golden, scene digest or G01-G10 pixel moved.
- **GPU smoke** (`tests/smoke/test_world_floor.cpp`, `gpu.world floor: ...`):
  - `two_areas.json` is rendered from each area and without areas. Only the eye's pool is lit, and it is as the plain
    scene lights it. Each area's pillar is shadowed by that area's key light; without areas the west spot is the only
    key light, so seen from the east the east pillar casts no shadow.
  - The building fixture's gen4 floor is walked with `floor_walk_into_room.json` from the elevator into room:snes. In
    the hall only the hall's lights are packed; in the room its own lights with `room:snes/fx:hero.key-light`. The
    walker passes the doorway; there are no high or medium GL messages.
  - The ground floor's lobby is rendered from the elevator.
- **Walks through the floor's collision** (unit, `world/floor: the walker walks ...`): gen4's north-row room:snes with
  `floor_walk_into_room.json`, and handheld's south-row, half-turned room:pocketlab with
  `floor_walk_into_south_room.json` (the same walk turning right), each past its doorway wall with the areas
  {hall, room}.

## Decisions

- **Hall lights use the pendant style**, with no new profile role, so the lighting profiles stay unchanged. The hall
  reads dim but legible (the styling pass's). Seen from the hall, a room shows its ambient only, and the hall from a
  room likewise (ADR-0013's accepted cost).
- **Room ids as area ids, `hall:<floor>` for the hall.** Object and light ids are `<room id>/<local id>` and
  `hall:<floor>/<part>`.
- **The derived geometry is integers** (mm) until the scene file. The scene's positions are rounded to micrometres
  and its yaws to 1e-4 degrees, as `toSceneFile()` does, so Debug and Release write one text.
- **A doorway opens onto the hall** when it is in the room's south wall and that wall's outer face (the room's
  `maxZ` plus the template's wall) is on the hall's edge. A template wall thicker or thinner than building.json's
  allowance therefore fails `doorway-off-hall`.
- **The area of the eye at the start** is the scene camera's: the nearest area when the camera is in none. The first
  view then follows the walker's eye.

## Deviations and limits

- The floor scene draws no cartridges: the collections are step 5a's (`floorCollectionLayout()` gives each room's
  layout in the floor frame).
- The hall's walls are plain boxes without trim. Their textures restart at every box (uvs in each box's meters).

## The building API (frozen for steps 4 and 5a)

- `rv/world/building.hpp` (steps 1 and 2): `BuildingData` (+ `FloorSpec::sign`), `SystemsData`, `classify()`,
  `roomToFloor()`, `roomYawToFloor()`, `floorFootprint()`, `followDepth()`, `floorRows()`, `placeEnteringRoom()`,
  `hallWidthOf()`.
- `rv/world/floor_layout.hpp`:
  - `FloorRoom` (a room with its template);
  - `deriveHall(floor, rooms, layout) -> Hall` (`interior`, `walls`, `doorways`, `lights`, `elevator`,
    `elevatorZone`, `arrival`, `entrance`, `footprint()`);
  - `floorDoorway()`, `floorRoomWalls()`, `floorAreas()`, `hallAreaId()`, `roomToFloorMatrix(placement)`;
  - the constants `kHallHeightMm`, `kHallLightPitchMm`, `kMinHallLengthMm`, `kArrivalXMm` and the elevator's sizes.
- `rv/world/floor_scene.hpp`:
  - `FloorSceneData`, `loadFloorSceneData(assetRoot, plan, floor, lighting, platformTagsBySystem)`;
  - `toFloorSceneFile()` and `toFloorSceneDescription()` (`FloorSceneBuild`: `sceneFile`, `hall`, `rooms`,
    `undressed`);
  - `roomsOnFloor()`, `floorObjectId()`, `floorCollectionLayout(layout, placement)`.
- `rv/scene/scene_areas.hpp`: `SceneArea`, `areaAt()`, `nearestArea()`, `lightsByArea()`, `AreaLighting`.
  `gl::packLights(lights, selection, environment)`, `gl::PackedLights::keyLight`, `gl::lightName()`.
- `app::SceneInstance::followEye(eye)`, `areaId()`, `areas()`, `areaSelection()`, `keyLight()`. Step 5a also uses
  `areaId()` as the area that holds the eye for picking and the cover loader's view.
- Validation: `rule::kHallOverlap`, `kDoorwayOffHall`, `kFloorUnreachable`, judged with `ValidationContext::building`.

## Follow-ups

- Step 4: shared and arcade rooms stand on the halls unchanged (same template, same doorway).
- Step 5a: the floor in the app. `toFloorSceneDescription()` on the pool; one collection per room through
  `floorCollectionLayout()`; picking per room by `areaId()`; `--start-room` and `--spawn` through
  `roomToFloorMatrix()`.
- The styling pass: hall trim and baseboards, light seen through doorways, a hall light role in the profiles.
