# Golden scenes and collection layouts of the golden plans

What `tests/unit/world/test_plan_scene.cpp` compares (I3 step 10a), in Debug and Release alike:

- `digests.json` (canonical JSON, `core::writeCanonicalJson`): for every golden plan of `../golden`, the SHA-256 of its
  room's scene file (`world::toSceneFile`, written as `world::sceneFileText` writes it: keys sorted, two spaces of
  indentation) and of its collection layout's canonical document (`world::collectionLayoutToJson`: lengths in
  micrometres, matrix elements in millionths, both rounded to integers), with the counts of objects, lights, bare
  dressed spans, items, clusters and occluders.
- `plan-57-seed-102938.scene.json` and `plan-57-seed-102938.collection.json`: the same two documents whole for one plan
  (44 featured titles, 12 in the hero band, one spine-out title; dressing of every kind), for review.

- `building-esde-seed-102938.floors.json` (I4 step 3, `tests/unit/world/test_floor_scene.cpp`): for every floor of the
  golden building `../golden/building-esde-seed-102938.json`, the SHA-256 of its floor scene (`world::toFloorSceneFile`,
  written as `world::sceneFileText` writes it) with the counts of objects, lights, areas, rooms, the hall's lights and
  walls and bare dressed spans; `building-esde-seed-102938.gen4.scene.json` that floor's scene whole (the SNES room on
  its floor, the hall, the elevator), for review. `$env:RV_UPDATE_GOLDEN = '1'; build/msvc-x64-debug/bin/rv_tests.exe
  "world/floor: golden*"` rewrites them.

The layouts take invented titles (`tests/unit/world/plan_scene_support.hpp`: "Title <n>" in the games' order, every
seventh game unavailable, every thirteenth missing). Nothing here comes from a real library.

Rewrite them only when a change of the conversion, the prefabs, the theme or the lighting profile alters the scenes on
purpose: `$env:RV_UPDATE_GOLDEN = '1'; build/msvc-x64-debug/bin/rv_tests.exe "world/plan_scene: golden*"`, review the
difference, and say in the commit why they moved.
