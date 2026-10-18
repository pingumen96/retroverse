# Golden scenes and collection layouts of the golden plans

What `tests/unit/world/test_plan_scene.cpp` compares (I3 step 10a), in Debug and Release alike:

- `digests.json` (canonical JSON, `core::writeCanonicalJson`): for every golden plan of `../golden`, the SHA-256 of its
  room's scene file (`world::toSceneFile`, written as `world::sceneFileText` writes it: keys sorted, two spaces of
  indentation) and of its collection layout's canonical document (`world::collectionLayoutToJson`: lengths in
  micrometres, matrix elements in millionths, both rounded to integers), with the counts of objects, lights, bare
  dressed spans, items, clusters and occluders.
- `plan-57-seed-102938.scene.json` and `plan-57-seed-102938.collection.json`: the same two documents whole for one plan
  (44 featured titles, 12 in the hero band, one spine-out title; dressing of every kind), for review.

The layouts take invented titles (`tests/unit/world/plan_scene_support.hpp`: "Title <n>" in the games' order, every
seventh game unavailable, every thirteenth missing). Nothing here comes from a real library.

Rewrite them only when a change of the conversion, the prefabs, the theme or the lighting profile alters the scenes on
purpose: `$env:RV_UPDATE_GOLDEN = '1'; build/msvc-x64-debug/bin/rv_tests.exe "world/plan_scene: golden*"`, review the
difference, and say in the commit why they moved.
