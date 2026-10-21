# The building property suite's data

Test-only world data of I4 step 6a's NFR-08 property suite (`tests/unit/world/test_world_building_suite.cpp`), laid over
a copy of the shipped `assets/world` and `assets/manifests` in a temporary folder; nothing here is shipped.

- `templates/suite_room_small.json`: `console_room_9x10` v2 cut down to the hero wall, one side-wall socket and one
  growth step that adds one more, so that a room fills, grows and reaches its capacity within a few hundred titles.
  The prefabs are the shipped ones; the families' fixtures stand in its sockets as in the shipped template.
- `building.json`: the shipped floors with at most two rooms per row, so that a floor fills within a few systems.
- `systems.json`: the shipped defaults and fallback with twelve of the shipped rows as they are (mame, fbneo, nes,
  snes, sfc, megadrive, genesis, neogeo, gb, atarilynx, pokemini, amiga) and kodi, excluded. The suite also invents
  `pocketlab` (gb's row by its platform tag) and `zzcustom` (no tag matches: the fallback, the computer floor).

The suite asserts the update's invariants, not counts; it needs no change when the shipped data changes, unless a
shipped prefab or family stops fitting the template's sockets.
