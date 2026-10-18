# I3 step 10a: the plan to a scene, GPU-free

Roadmap row: I3 step 10a (wave 2, on 1b and 3; steps 8, 9, 11 and 13r ran beside it in their own worktrees, step 7 was
merged into it while it ran). Decisions: [ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §1 (the
static part as a `SceneDescription`), §4 (lights), §6 (a placement names its slot, the app derives the transform);
[ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md) §1 to §3 (the collection beside the scene's
objects, the 80-byte record, the clusters); [ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §2 (pick
targets and occluders). The step fills the two stubs step 0a listed for it, `scene_builder` and `collection_layout` in
`rv_world`, and adds `--scene` to `retroverse-cli worldplan show`. Nothing in the app reads them yet: step 10b
instantiates the scene and the collection.

## What was built

| File | What it holds |
|---|---|
| `rv/world/scene_builder.hpp`, `src/scene_builder.cpp` | `LightStyle`, `LightingProfile` and `eveningLightingProfile()`; `ScenePrefab`, `ScenePrefabs`, `scenePrefabRefs()`, `loadScenePrefabs()`; `SceneData` and `loadSceneData()`; `UndressedSpan`, `SceneBuild`, `toSceneFile()`, `toSceneDescription()`, `sceneFileText()`; `room_frames::fixtureTransform()`, `yawRotation()`, `roomShell()` |
| `rv/world/collection_layout.hpp`, `src/collection_layout.cpp` | `CollectionTitle` and `collectionTitles()` (from a `RoomSnapshot`), `ItemStance`, `slotPickId()`, `CollectionItem`, `CollectionCluster`, `CollectionGroup`, `SkippedPlacement`, `CollectionLayout`, `collectionLayout()`, `collectionLayoutToJson()` |
| `src/room_parts.hpp` (new, private header, defined in `scene_builder.cpp`) | the slot and level sockets of a built prefab and `dressingProp()`, where a dressed span's prop stands: what the scene and the occluders share |
| `tools/cli/src/cmd_worldplan_show.cpp` (step 3's) | `--scene <file>` |
| `tests/unit/world/test_plan_scene.cpp`, `test_collection_layout.cpp`, `plan_scene_support.hpp`, `tests/unit/cli/test_cli_worldplan_scene.cpp`, `tests/fixtures/world/scene/` | the tests and the golden snapshots (below) |

No CMake list changed: both sources were pre-listed by step 0a, the new header is private, the tests are globbed.

## The mapping

### The static part (`toSceneFile`, `toSceneDescription`)

`toSceneFile(plan, room, data)` writes a scene file (`scene_description.hpp`'s format) and `toSceneDescription` is that
file read back by `scene::parseSceneDescription`: the scene the app instantiates and the file `worldplan show --scene`
writes are one, and the parser's validation guards both. Every number is one division of a plan integer (millimetres /
1000, hundredths of a degree / 100), or a float of the prefabs rounded to micrometres and 1e-4 degrees, so that the text
is the same in Debug and Release.

| Plan | Scene |
|---|---|
| the room (bounds, height) and the template (wall thickness, doorway) | object `room`: `room_shell` of the room's interior at its bounds' centre (a grown room's shell stands south of the origin), the room's height, the template's wall thickness and doorway; the test room's surfaces (oak planks and painted plaster every 2 m, a varnished wainscot to 1 m under a rail, walnut trim) |
| each fixture | object `<fixture local id>` (`fx:hw-w`, `fx:hero.crt`): its pivot, its yaw about +Y, the generator and parameters its manifest resolves to (`assets::resolvePrefab`: the manifest, else the family's generic, else the family's procedural prefab, one warning each), the theme's materials per role; the CRT's screen `MAT_Screen_CRT` with the colour bars, the sign the platform manifest's text on `MAT_Neon_Diffuser`, posters an invented cover picked from the fixture's seed; posters and the rug cast no shadow, the rug does not collide, every other fixture does |
| each dressed span | object `dress:<level>` (`dress:fx:hw-w/l:3`; `.<first slot>` added for a span that does not start at slot 0): the prop's generator with its manifest's parameters and the span's free volume (`width` from the span's first slot's left edge to its last one's right edge, the whole level for a whole level; the Level anchor's `height` and `depth`; `seed` the level's prop seed reduced to [0, 2^31 - 1]), its pivot in the middle of the span on the level's surface, through the Level anchor's frame (a leaning level would lean its prop); a magazine stack's cover an invented one |
| each plan light | a point or spot light (`lightKind`), named by its local id, at its position (a spot aimed at its target), the lighting profile's colour, intensity and range for its role, a spot's outer cone the half angle its span subtends at the target plus the role's margin, at least the role's minimum, at most 80 degrees, the inner cone the role's share of it (0.01 degree steps); the key alone casts shadows, with the profile's strength |
| - | the profile's environment (the test room's: `#221E1A` background, `#E6E2DC` / `#4A4642` ambient at 0.25, exposure 1.5, bloom 0.18 and 0.5, vignette 0.05, ambient occlusion on); the camera 0.8 m inside the doorway at eye height (1.65 m), looking into the room, 6 degrees down |

The collection's items are no scene objects (ADR-0011 §1).

**The evening profile** (`eveningLightingProfile()`, style §8.1's default), from the test room's lights:

| Role | Colour | Intensity | Range | Cones (minimum outer, margin, inner share) |
|---|---|---|---|---|
| key | `#FFE0C0` | 70 | 12 m | 30, 3, 0.55 |
| pendant | `#FFE0C0` | 16 | 10 m | point |
| lamp | `#FFC88A` | 4 | 4 m | point |
| picture | `#FFE0C0` | 8 | 4 m | 25, 10, 0.6 |
| wall-washer | `#FFE0C0` | 10 | 5 m | 32, 8, 0.56 |

### The collection (`collectionLayout`)

One item per placement of the room:

- **Stance**: from the slot the fixture's built prefab has (`Slot_<level>_<index>`): face-out when its volume is the
  media family's face-out slot, spine-out when it is the spine-out one; it must agree with the plan's section (featured
  and band face-out, spine spine-out).
- **Transform**: `fixture * slot socket * offset * geometry::slotItemTransform(stance, item size)`, from the
  cartridge's space (pivot centre) to the room. The offset (ADR-0010 §6, the plan's `insetMm` and `yawCdeg`): pushed in
  along the slot's -Z, turned about the slot's +Y through the item's centre (a display case's leaning +Y for its items).
- **Pick id**: the slot's, `(index of the fixture's socket in the template + 1) << 24 | level << 14 | index` (`hw-w`
  slot 3.12 is `0x0100C00C`): never 0, the same for a slot in every plan of the template version, so that a placement
  keeps its pick id through step 9's incremental updates; the instance record carries it (`attributes()`).
- **Pick target**: the slot frame with the offset, `faceTargetBox` of the slot volume, the item's cluster.
- **Clusters**: a module's level, the whole of any other fixture (a display case, the featured wall shelf); id
  `(socket index + 1) << 10 | level`, 1023 for a whole fixture; bounds the union of the items' boxes.
- **Groups**: one per media family (one in I3: `cartridge_home_16bit`, its item prefab and size), clusters and items
  contiguous, items in (cluster, slot) order.
- **Record**: layers 0 (the pending picture until step 12 assigns them, by `frontLayerKey` `<GameId>/front` and
  `spineLayerKey` `<GameId>/spine`), the flags (`kUnavailable` for a game the titles lack or whose file is not
  available), the pick id. `instance()` is the 80-byte record.
- **Title**: the catalog's (`CollectionTitle`, or `collectionTitles(snapshot)`), for the spine and the hover's label.
- **Occluders** (step 7's notes): the shell's walls (its colliders), for a shelf and the wall shelf their bounds outside
  their levels' free volumes (`Level_<n>` anchors), each opened to the front: the two sides, the back, the base, the
  boards between the levels and the top; for a display case its base under the levels, its sides and its back (front
  and top are glass); the bounds of every other fixture but the rug; the nominal box of every dressing prop.
- **Navigation** (step 7's `SlotNavigation`): a run per wall (north, south, east, west, the fixtures with slots left to
  right as one faces the wall), then one per free-standing fixture (the display cases); levels bottom to top by their
  slots' centre height; slots left to right along the run's right axis; empty and dressed slots `kNoPick`.
- A placement the built prefab cannot hold (a slot it lacks, a slot of another item, a stance against the section: a
  fallback prefab of other levels) becomes no item and is listed in `skipped` with its reason; two placements in one
  slot are refused.

`collectionLayoutToJson()` is the layout as canonical JSON (integers: micrometres, millionths), for the goldens.

### `retroverse-cli worldplan show --scene <file>`

Reads the plan as before, loads the scene data from the asset root (`loadSceneData`: the room's template by id and
version, the platform manifest, the media family, every prefab the plan names), writes the scene file atomically
(`sceneFileText`: two spaces of indentation, keys sorted) after reading it back as a scene, and adds one line to the
summary (`Scene <file>: N objects, M lights, K dressed span(s) left bare`; `"scene"` in `--json`); the spans left bare
are logged. A scene that cannot be built or written exits 3.

## Figures

From `tests/fixtures/world/scene/digests.json` (the same for the three world seeds of a size; the scene's text
differs between seeds through the dressing and the posters, the collection's through the offsets):

| Golden plan | Scene objects | Lights | Bare spans | Items | Clusters | Occluders |
|---|---|---|---|---|---|---|
| 0 titles | 45 | 6 | 4 (the display cases' levels) | 0 | 0 | 81 |
| 1, 15 | 45, 43 | 6 | 4 | 1, 15 | 1, 2 | 81, 79 |
| 50 (`snes-room`'s size) | 42 | 6 | 0 | 50 | 4 | 78 |
| 56, 57 | 41, 40 | 6 | 0 | 56, 57 | 5, 6 | 77, 76 |
| 890 | 32 | 8 | 0 | 890 | 35 | 98 |
| 2,000 | 29 | 10 | 0 | 2,000 | 73 | 145 |
| 8,000 (16 m deep) | 43 | 16 | 0 | 5,615 | 197 | 339 |

Timings, Release, the reference machine with other worktrees building (informative), `[plan_scene][bench]` on the
2,000-title plan, medians of 21 runs: `loadSceneData` 7.5 ms (file reads and 19 prefabs built), `toSceneFile`
0.25 ms, `toSceneDescription` 0.51 ms, `collectionLayout` 14.5 ms (the room shell built for its wall colliders, the
items, occluders and navigation); 15 ms of pure work within step 10b's 1 s.

The rendered rooms (`retroverse.exe --scene`, Debug, 1920 x 1080 from the scene camera; under `build/screenshots/10a/`,
not committed): the `snes-room` fixture's plan (generated by `worldplan generate --seed 102938` after an import, written
by `worldplan show --scene`) and the 890-title golden plan, each 0 high, medium or low GL messages, exit 0. Reviewed:
the shell with its wainscot, the hero corner (CRT on its cabinet with the colour bars, the console, the rug, the
armchair, the lit floor lamp, the sign with the platform's text under its picture light), the two hero-wall modules
either side of the featured wall shelf, the two display cases facing the circulation band, posters on the side walls;
at 890 titles three side-wall modules (w01, w02, e01) with w02's unused lower levels dressed, at 50 titles none and the
hero-wall modules' levels dressed (storage boxes, magazines, bookends). A scratch scene adding a cartridge per item of
the 57-title plan's collection snapshot (transforms decomposed by a script) showed the hero band upright on its board,
the wall shelf's and the display cases' items leaning on their ledges behind the glass, and the spine-out item on its
end with its spine outward, 0 GL messages.

## Tests

- `tests/unit/world/test_plan_scene.cpp` (`[world][plan_scene]`): **golden snapshots**, every golden plan's scene text
  and collection document digested into `digests.json`, the 57-title plan's two documents whole, each computed twice
  (two runs alike) and compared in Debug and Release; **every fixture's prefab built with its parameters** (the object's
  kind and parameters the resolved manifest's, the built bounds the plan's millimetres, the transform its pivot and
  yaw, a material for every role it draws, colliders on every fixture but the rug covering its bounds, the object count
  the shell, the fixtures and the dressed spans but the bare ones, no collection item among the objects); the shell
  sized to the room (a grown room's south of the origin) and the camera in the doorway; the dressing inside its level's
  free volume within 1 mm and only the display cases' spans bare; the lights (one per plan light, the profile's colour,
  intensity and range, spots aimed at their targets with cones covering their spans, the key alone shadowed) and the
  environment; the scene file through a file and `loadSceneDescription`, the sign's text and the CRT's bars; the theme
  dressing every role of every prefab kind; refused inputs (another room, a missing prefab, a manifest of another
  kind, 17 lights).
- `tests/unit/world/test_collection_layout.cpp` (`[world][collection_layout]`): **every placement one item** on every
  golden plan (one per game and slot, nothing skipped, pick ids nonzero, unique, the slot's and the record's, flags and
  titles from the snapshot's titles, layer keys, `find`); pick ids stable across plans; **transforms the prefabs'
  sockets plus offsets within their bounds** (the slot frame equal to the fixture's pivot and yaw times the socket built
  here, times the offset; inset 0 to 4 mm spine-out, yaw within 1.5 degrees face-out; the item's corners inside its
  slot grown only by the offset's sweep); **no item intersects another** (oriented boxes, separating axes, on six plans
  up to 8,000 titles) nor an occluder (0.5 mm for spine-out items standing on boards, 1.7 mm for a face-out turn);
  **clusters and groups with their counts** (one group, clusters partitioning the items in order with the plan's
  placements per module level or fixture, bounds holding their items; 44 featured, 12 band and 834 spine on the
  890-title plan); **the pick index built from the layout** (step 7's `PickIndex::build` on the targets and occluders of
  the 50-, 890- and 2,000-title plans: a ray from 0.6 m through each item's face centre picks it at 0.6 m, a steep ray
  from below through the board under a spine-out item never picks it, a ray from the doorway picks nothing); the
  navigation (every item in the runs, `SlotNavigation::build` accepting them, steps along hw-w); skipped placements and
  refused plans; titles from a snapshot.
- `tests/unit/world/test_plan_scene_bench.cpp` (`[.bench][plan_scene][bench]`): the timings above, the pure part
  checked against 100 ms in Release.
- `tests/unit/cli/test_cli_worldplan_scene.cpp` (`[cli][worldplan]`): `worldplan show <golden plan> --scene` writes
  `toSceneFile`'s text, which loads as a scene; `--json` names it; an unwritable path exits 3; the `snes-room` fixture
  imported, its plan generated and shown as a scene (50 placements, no side-wall module, the hero-wall modules dressed).

Results on the reference machine (RTX 2080 SUPER, driver 617.42), with other worktrees building at the same time:
`msvc-x64-debug` and `msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` passed 1,562 of 1,562 unit tests in
Debug (907 s) and in Release, the step's 19 test cases among them (and its hidden benchmark; 1,543 before the step); the golden scenes written in
Debug are reproduced byte for byte in Release; `rv_smoke_tests "golden*"` (G01) passes in Debug, 0 pixels apart (the
step changes no rendering code). `RV_LAUNCH_ALLOWLIST` not set.

## Decisions taken within the step

1. **The scene is its file read back.** `toSceneDescription` parses `toSceneFile`'s document rather than filling
   `SceneObject`s itself: the app and `worldplan show --scene` see one scene, the scene format's validation (ranges,
   cones, light limit) guards the generated room as it guards the reference scenes, and no scene writer was needed in
   `rv_scene` (which steps 7 and 8 also touch). Parsing 40 objects costs well under a millisecond (Figures).
2. **The lighting profile is code for now.** `LightingProfile` and `eveningLightingProfile()` carry colour, intensity,
   range and cone rules per light role and the environment, taken from the test room's lights. Step 16 owns the two
   profiles and their data (`world.json` chooses one): it can load a profile from `assets/world` into the same struct
   and pass it in `SceneData::lighting`; nothing here reads the profile from the plan, so switching moves nothing.
   Cones derive from each light's span and distance (the plan has no cone), so a wall-washer over two modules widens
   by itself. The theme's materials per role (`themeMaterials`) are code too, the test room's.
3. **Pick ids are the slot's, not the item's index.** `(socket index + 1) << 24 | level << 14 | index` is fixed by the
   template version, so a placement keeps its pick id when step 9 adds placements (an index into the placements would
   shift), the static group's records of the items standing stay valid (step 15b's "moves none"), and a pick id names
   its slot when read back from a GPU or a log. 254 sockets, 1,024 levels and 16,384 slots per level fit (the template
   has 45 sockets, 7 levels, 33 slots).
4. **Clusters: a module's level, any other fixture whole** (ADR-0011 §3's "a shelf level of a module, a display case";
   the featured wall shelf, 12 items, is one cluster too). Ids follow the template's socket order, like the pick ids.
5. **Occluders come from the prefabs' level sockets, by kind**, not from the meshes (one mesh per material role merges
   every board of a shelf) nor from the walking colliders (a module's collider holds its slots): a shelf's and the wall
   shelf's bounds minus their levels' free volumes opened to the front (sides, back, base, boards, top: the wall
   shelf's lips are left open, harmless); a display case's base, sides and back (its glass front and top open, its
   fabric risers and panels too: they stand behind or below the items); every other fixture's bounds but the rug; each
   dressing prop's nominal box; the shell's walls. A fixture kind with slots and no rule gets no occluder rather than a
   box that would hide its own items.
6. **A face-out turn sweeps into the backing.** ADR-0010 §6's yaw of up to 1.5 degrees, applied about the slot's +Y
   through the item's centre, moves one back corner of a leaning face-out item up to 1.64 mm into what it leans on (the
   wall shelf's back board, the display case's base under its tilted level). The plan stores the yaw, the scene must
   honour it, the overlap is inside the board and hidden behind the item, and pushing the item forward instead would
   put its front corner into the lip; the tests bound it (1.7 mm for face-out items, 0.5 mm for spine-out ones, which
   stand on their boards).
7. **The display cases' levels stay bare when dressed.** Their free volume is 22 mm deep (the leaning level from its
   panel to its lip) and the dressing props take 50 mm at least (`ShelfDressingParams`); a prop clamped to 50 mm would
   cut through the glass and the panel. Such a span is left out of the scene and reported (`SceneBuild::undressed`,
   logged by `worldplan show --scene`): four spans on a room of 0 titles, none from 44 titles on. Step 16 decides what
   an empty display level shows (a face-out sleeve, a riser card).
8. **Every fixture collides but the rug** ("colliders cover every fixture"), the posters and the sign included (thin
   boxes on the walls, out of the walker's way): the scene format's per-prefab collider boxes are the app's, as in the
   test room; the collection's items are never colliders (ADR-0011 §1).
9. **Items are not checked against the prefab by the plan's numbers alone**: the stance comes from the slot volume the
   built prefab has, and a placement whose slot the prefab lacks or holds another item (a fallback prefab with other
   levels) is skipped and reported instead of drawn at a wrong size; step 10c decides how the room reports it.
10. **The sign stays the poster frame the plan measured** (0.5 x 0.7 m, step 1a's note): the scene gives it the
    platform's text on the neon diffuser; a sign of its own size needs a manifest the template names, and changes the
    plan's bounds (step 16).
11. **Determinism**: every scene number is one division of a plan integer or a prefab float rounded to micrometres or
    1e-4 degrees, and the collection's canonical document holds integers only; the goldens are written in Debug and
    compared in Release.

## Notes for later steps

- **10b**: load `SceneData` with `loadSceneData(assetRoot, plan, room)` on the pool (it reads files) and build
  `toSceneDescription` and `collectionLayout` there too; give the description to `SceneInstance` as any scene, and the
  layout to `app::CollectionInstance`: per group, the item prefab (`group.item`, resolved through `data.prefabs`; the
  cartridge's parts `plastic_shell`, `label`, `spine` with `themeMaterials("cartridge")`), each item's `instance()`
  record in item order (layers 0 until step 12), the clusters' ranges and bounds for culling. The pick index is
  `PickIndex::build(layout.pickTargets(), layout.occluders)`, the D-pad's `SlotNavigation::build(layout.navigation)`.
  `InteractionRoom` (step 7) maps a pick id to the item (`layout.find`, linear: index it if called per frame) and its
  GameId, face-out from `item.stance`, its section from the cluster or the fixture. A run report field could carry the
  layout's `skipped` count and the scene's `undressed`.
- **10b, the camera**: the scene's camera stands 0.8 m inside the doorway at eye height looking north; the walker's
  spawn in the room can be the scene camera, as for the test room.
- **10c**: a manifest removed falls back in `loadScenePrefabs` (one warning, the family's prefab); a fallback across
  kinds is refused (ValidationFailed) since the plan's slots would not fit. Placements the fallback prefab cannot hold
  are in `CollectionLayout::skipped`.
- **12**: the layer keys are `<GameId>/front` and `<GameId>/spine` (`CollectionArrays::acquire`); write a layer into
  `scene::InstanceAttributes::frontLayer` / `spineLayer` of the item's record; `item.title` is the spine's text and
  `kUnavailable` is already in `flags`.
- **13a**: the pick target's frame is the slot frame with the placement's offset (`item.slotFrame`), the pull-out moves
  along its +Z; `item.transform` is the cartridge's static pose. Clusters ids are not cluster indices: use
  `layout.clusters` to find a cluster's range.
- **16**: tune `eveningLightingProfile()` (and add the second profile) and `themeMaterials`; the display cases' empty
  levels; the sign's size; the poster titles are invented ones picked from the fixture's seed.

## For the merge

- **Order**: the step touches only its two stubs (`scene_builder`, `collection_layout`), one new private header
  (`src/world/src/room_parts.hpp`, no CMake change), step 3's `cmd_worldplan_show.cpp` (the `--scene` option, its help
  and one helper) and new test files and fixtures; step 7 is merged in. No shared header changes.
- **CLAUDE.md** (not edited by the step), "retroverse-cli": `worldplan show [<plan>] ... [--scene <file>]` also writes
  the plan's room as a scene file (the shell, fixtures, dressing, lights; the collection's items are not part of it),
  which `retroverse.exe --scene <file>` renders. "Layout and layering", `rv_world`: `scene_builder.hpp`
  (`toSceneFile`, `toSceneDescription`, `loadSceneData`, the evening lighting profile, the theme's materials) and
  `collection_layout.hpp` (`collectionLayout`: items with their transforms, pick ids, layer keys and flags, clusters,
  groups, occluders, navigation) since I3 step 10a. "Visual verification" or the tests' paragraph: golden scenes in
  `tests/fixtures/world/scene` (digests of every golden plan's scene and collection), rewritten only with
  `RV_UPDATE_GOLDEN=1`; `RV_WORLD_SCENE_OUT=<folder>` makes `rv_tests "cli/worldplan show --scene: the snes-room*"`
  leave the snes-room fixture's plan and scene file there.
- **docs/architecture.md**: the `world` row: the plan's static part as a scene and the collection's layout (10a) are
  filled.
- **docs/roadmap.md**: step 10a done, with the figures above.
