# I3 step 1b: the collection's prefabs, their manifests and the object fallbacks

Step 1b of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice") builds the prefabs whose slot counts the world generator
reads (ADR-0010 §3-§5) and the fallbacks of style §21 for objects: the cartridge's spine, the hero-wall and side-wall
modules as parameters of `shelf`, the display case, the featured wall shelf, three shelf dressing props, the manifests
of all of them, and the prefab families with `assets::resolvePrefab`. Nothing reads them in the app yet: steps 3 and 10a
place them, step 10c takes the fallbacks into the room. The test room is untouched (golden G01 0 pixels apart).

## The figures

Counted from the shipped manifests built with their parameters (`geometry::slotLevels` over the prefab's sockets), as
the generator will count them; asserted by `tests/unit/assets/test_collection_manifests.cpp`.

| Figure | Fixture (manifest) | Slots | Value |
|---|---|---|---|
| `S` | side-wall module (`furniture.shelf.side_module.generic`) | 7 spine-out levels x 33 | **231** |
| `Sh` | hero-wall module (`furniture.shelf.hero_module.generic`) | 6 spine-out levels x 33 | **198** |
| `B` | the hero band: one face-out level of each hero-wall module | 2 x 6 | **12** |
| `D` | featured wall shelf (`furniture.wall_shelf.featured.generic`) and two display cases (`furniture.display_case.generic`) | 12 + 2 x 16 | **44** |

They equal ADR-0010's draft figures (`B` 12, `Sh` about 198, `S` about 231, `D` 44), so the ADR's consequences stand:
with the reserve of 0.15 the 9 x 10 m room (14 side-wall sockets) holds `floor((D + B + 2 Sh + 14 S) / 1.15)` = 3,205
titles and the 16 m room (26 sockets) 5,615; 890 titles take `e` = 3 side-wall modules (572 spine-out titles and reserve
beyond the hero-wall modules), 2,000 take 8 (exactly 1,848 / 231), 56 or fewer none. One detail of ADR-0010 §5 differs:
the modules are 1.775 m tall, not about 1.9 m (below).

The slots (ADR-0010 §5): spine-out 2.0 x 12.5 x 8.5 cm (`slotSizeFor(SlotFacing::SpineOut, {0.125, 0.085, 0.020})`),
2.4 cm apart (a 4 mm gap), the first 3 cm from the module's side; face-out 12.5 x 8.5 x 2.0 cm: 0.6 cm apart in the hero
band, 1.2 cm on the wall shelf and in the display cases.

## What was built

### `rv_geometry`

- **The cartridge's spine** (`CartridgeParams::spineLabel`, off by default, and `spineLabelSize`, 13.5 x 86.4 mm by
  default: the 40 x 256 aspect of ADR-0011's spine layer): the role `spine`, a quad on the top face's flat part (inside
  the chamfers, behind the front pocket), 0.5 mm above the shell, whose top face is lowered by those 0.5 mm so that the
  bounds stay 12.5 x 8.5 x 2.0 cm; the socket `Spine` (Other). Off, the cartridge's geometry is the test room's,
  unchanged (the top border of the front's pocket is computed by the same operations).
- **How items stand in slots**: `SlotFacing` (`FaceOut`, `SpineOut`), `slotSizeFor` and `slotItemTransform`
  (prefabs.hpp). Spine-out a cartridge stands on its end, its top face (the spine) out, its +X end up and its front to
  the right as one faces the shelf; an item's transform in a fixture is
  `socket.transform * slotItemTransform(facing, itemSize)`. The spine quad's uv (0, 0) is its top-left corner in that
  view: a spine layer is drawn upright.
- **Slot and level sockets**: every fixture names its slots `Slot_<level>_<index>` (ShelfSlot, bounds = the slot volume
  in the socket's frame, +Z to the viewer) and has one Anchor `Level_<level>` per level, its frame on the level's
  surface and its bounds the volume free above it (where dressing stands). `parseSlotId`, `parseLevelId` and
  `slotLevels` (the slots of each level: count, slot volume, height) are in socket.hpp.
- **`shelf`** gained `slotFrontInset` (null: centered, as before; the modules set their items 1 cm behind the boards'
  front edge) and display levels (`displayLevels` with `displaySlotSize`, `displaySlotGap`, `displaySlotMargin`: another
  item size on chosen levels, checked by the same rules). The modules:
  - side-wall (and the family's generic) module: 0.9 x 1.775 x 0.24 m, a closed 0.35 m base (the plinth), 7 levels with
    a 20 cm pitch (17.5 cm free above 12.5 cm cartridges), boards' tops at 0.375 to 1.575 m (slot centres 0.44 to
    1.64 m), slots 1 cm behind the front edge;
  - hero-wall module: the same with level 5 (board at 1.375 m, the face-out cartridges' centres at 1.42 m, just below a
    standing eye) given to the hero band, 6 face-out slots.
- **`display_case`** (`DisplayCaseParams`): 1.2 x 0.95 x 0.5 m, a wooden base of 0.55 m with two doors and knobs, a
  vitrine with a glass front and top (one double-sided sheet each, role `glass`) between wooden sides, a back panel and
  two top rails; two levels stepped from the front, each a fabric riser with a ledge, a lip and a panel tilted back by
  30 degrees, 8 face-out slots each; the slots' frames lean with the levels. Roles `wood_primary`, `wood_secondary`,
  `metal_trim`, `fabric`, `glass`.
- **`wall_shelf`** (`WallShelfParams`), the featured wall shelf: 0.9 x 0.36 x 0.08 m hung with its bottom at 1.1 m
  (pivot on the floor below it), a back board between two cheeks under a cap, two picture ledges with lips, 6 face-out
  slots each leaning back 12 degrees against the back board (the lowest edge on the ledge, the top back edge on the
  board).
- **Shelf dressing**, three kinds sharing `ShelfDressingParams` (the level's free volume and a seed, varied through
  `core::Random`): `storage_box` (a row of lidded cardboard boxes with label cards; roles `cardboard`, `label`),
  `magazine_stack` (one to four stacks of 5 to 12 magazines, each turned up to 3 degrees, a cover on top; `paper`,
  `cover`), `bookends` (books between two block bookends, two bindings; `metal`, `book`, `book_alt`). Each stays inside
  the volume it is given and rests on y = 0.
- `prefabKinds()` lists the five new kinds; the JSON factory reads their parameters (`slotFrontInset` a number or null,
  `displayLevels` an array of integers).

### Manifests and fallbacks (`assets/manifests`, `rv_assets`)

- New manifests, under the ids step 1a's template names (`console_room_9x10.json`): `furniture.shelf.side_module.generic`
  and `furniture.shelf.hero_module.generic` (kind `shelf`), `furniture.wall_shelf.featured.generic`,
  `furniture.display_case.generic`, `decor.storage_box.generic`, `decor.magazine_stack.generic`,
  `decor.bookends.generic`, and the hero corner's `furniture.armchair.generic` (the test room's armchair, which had no
  manifest); `test_world_data.cpp` now requires every manifest the world's data names. The side-wall module is the
  generic of the family `furniture.shelf`, so that a hero-wall module whose manifest is missing falls back to a
  module that fits its 0.9 m socket; `furniture.shelf_modular.generic` (the default 1.2 m shelf) is unchanged, in a
  family of its own. `media.cartridge.home16.generic` builds the spine (`{"spineLabel": true}`, socket `spine`). The
  hero-wall module names the side-wall module as `fallbackPrefab`. `kProceduralPrefabKinds` lists the five new kinds
  and `armchair`: every kind `geometry::prefabKinds()` builds.
- **Families** (`assets/manifests/families.json`, `rv/assets/prefab_resolver.hpp`): each family is a dotted id prefix
  with its generic manifest (tier B) and a procedural prefab (tier C: the generic's generator and parameters, kept equal
  by a test). `resolvePrefab(assetRoot, families, id)` returns the requested manifest when it is usable (it loads, names
  its id, validates without errors, builds, names only existing sockets; a model manifest: its file exists), else its
  family's generic manifest, else its family's procedural prefab, with the built prefab, the generator and parameters
  (for callers that size a prefab, the dressing) and the reason; each fallback logs exactly one warning. Sixteen
  families cover every shipped manifest (style §26.3: a family fallback for every category).

### Asset preview

The new kinds get the test room's materials per role (`prefabRoleMaterials`; the magazines a cover picture); slot and
level volumes are drawn as boxes (slots green, levels teal, without names); the Statistics panel lists the slots by
volume ("2.0 x 12.5 x 8.5 cm: 198 in 6 levels"; `AssetReport::slots`); a socket's tooltip in the Model panel adds its
size.

## Screenshots (reviewed; under `build/screenshots/1b/`, not committed)

- The asset preview, `--panel Statistics`, 1920 x 1080: `side_wall.png` (furniture.shelf.side_module.generic: 90.0 x
  177.5 x 24.0 cm, "2.0 x 12.5 x 8.5 cm: 231 in 7 levels"), `hero_wall.png` (furniture.shelf.hero_module.generic: 198
  in 6 levels and 12.5 x 8.5 x 2.0 cm: 6 in 1 level), `armchair.png` (furniture.armchair.generic), `display_case.png`
  (120.0 x 95.0 x 50.0 cm, 16 in 2 levels), `wall_shelf.png` (90.0 x 36.0 x 8.0 cm, 12 in 2 levels),
  `cartridge_spine.png` and `cartridge_spine_uv.png` (the media family's cartridge: the spine on the top face, 12.5 x
  8.5 x 2.0 cm), `storage_box.png`, `magazine_stack.png`, `bookends.png`. 0 GL messages; each manifest reports only the
  placeholder hash's warning.
- A scratch scene that fills the fixtures (`build/scenes/b1_collection.json`, written by `build/scenes/make_b1_scene.py`
  with the slot formulas: 485 cartridges spine-out and face-out, the dressing on unused levels), rendered by
  `retroverse.exe --scene`: `collection_overview.png`, `collection_spines_1m.png` (`--spawn 0.2,0,-1.3,0,-12`: dense
  spine-out rows, every spine upright on the cartridge's top), `collection_hero_band.png` (the band of 6 face-out
  cartridges between spine-out levels), `collection_display_case.png` (both levels seen through the glass from 0.6 m),
  `collection_wall_shelf.png`. 0 high or medium GL messages.

Against style.md and example.png: dark walnut and varnished oak (`#493024`, `#76513A` family), bordeaux fabric in the
cases (`#683D42`), graphite bookends; the rhythm the concept shows (tall shelves dense with spines, a low glass case in
front, a face-out band, boxes and magazines on spare boards) without any brand or trademarked name. The display glass
(`MAT_Glass_Display`, blended, double-sided) still lays a light haze over the items under the key light; its tuning
belongs to step 16's look development.

## Tests

- `tests/unit/geometry/test_collection_prefabs.cpp`: the spine (size, aspect, position, the shell lowered, the label
  unchanged, the uv's orientation standing spine-out); `slotSizeFor` and `slotItemTransform` (rigid, filling the slot
  volume); slot and level ids, `slotLevels`; the modules' slots (counts, sizes, positions, front inset, free headroom)
  and Level anchors; the display case (2 x 8 slots leaning 30 degrees inside the vitrine, every item seen through the
  glass from 0.6 m and nothing else, standing on its ledge and leaning on its panel, the levels' volumes holding their
  slots, a taller variant); the wall shelf (items resting on the ledge against the back board, behind the lip, under
  the next ledge); dressing over 180 parameter sets per kind (inside the volume, on y = 0, valid meshes, no coplanar
  overlaps for level-sized props, the same parameters the same geometry, four seeds four variants, a prop sized to a
  module's Level anchor inside it).
- `tests/unit/geometry/test_prefabs.cpp`: the kind list, roles, bounds, sockets, JSON errors (the new keys and kinds),
  z-fighting variants, typed defaults equal to JSON defaults, determinism, colliders for the new kinds; shelf slots and
  Level anchors by the layout formula.
- `tests/unit/assets/test_collection_manifests.cpp`: `D`, `B`, `Sh`, `S` from the shipped manifests, the manifests'
  `scaleMeters` against the built bounds, the capacity and module counts of ADR-0010 §3, §5; the dressing manifests
  sized to a level.
- `tests/unit/assets/test_prefab_resolver.cpp`: the families (every manifest in one, every generic present, tier C equal
  to the generic, prefix rules), malformed registries refused with their JSON pointer; every shipped manifest resolved
  as itself without a log line; **a manifest removed from a scratch copy of the asset root** falls back to its family's
  generic with exactly one warning (the hero-wall module to the side-wall module, same footprint), the generic removed
  too to the procedural prefab with one warning; broken JSON, out-of-range parameters, another id, missing sockets and
  a missing license fall back alike; ids without a family or malformed are errors.
- `tests/unit/assets/test_prefab_manifest.cpp`: the shipped manifests (now 17 for 15 kinds, every kind the geometry
  builds).
- `tests/unit/world/test_world_data.cpp` (step 1a's): the pending list emptied, every manifest the template names exists
  with the template's kind.
- `tests/unit/preview`: slot and level volumes in the overlay (shelf, display case, wall shelf), the report's slots, the
  new kinds' materials (the existing "every prefab kind looks as in the test room" case covers them).

Results (reference machine, RTX 2080 SUPER, with step 1a merged): Debug and Release built with no warning;
`./scripts/test.ps1 -Preset msvc-x64-debug-gpu` ran 1,371 unit tests and 30 GPU cases: all passed but
`unit.catalog_db/sessions: a write that meets a busy database is tried again...`, a timing test of I2 that failed once
while other worktrees built and passed when run again (`-Filter catalog_db/sessions`, 9 of 9); golden G01 0 of
2,073,600 pixels apart. Before the merge of step 1a, the same preset passed 1,350 of 1,350 (1,321 unit, 29 GPU).

## Decisions taken within the step

1. **Display levels and a front inset as parameters of `shelf`** rather than a new kind: the hero-wall and side-wall
   modules are one prefab with two manifests, as the row asks; both new keys default to the old behaviour, so the test
   room's shelves are unchanged.
2. **The spine is optional on the cartridge** (`spineLabel`, off by default): the test room's 96 cartridges keep their
   geometry, materials and draw calls (G01); the media family's manifest turns it on. The shell gives up its top 0.5 mm
   instead of the label floating above the bounds.
3. **Spine-out stance**: +X up, the front to the right, so that a title reading top to bottom (ADR-0011 §5, the US and
   UK convention) has its glyphs' tops towards the front cover, as on a real spine; the layer itself is upright.
4. **One slot naming for every fixture** (`Slot_<level>_<index>`) and **Level anchors** (`Level_<level>`): the generator
   reads capacity, slot volumes (face-out or spine-out by comparing with `slotSizeFor`) and the dressing's volume from
   sockets alone, never from the shelf's layout formula.
5. **The side-wall module is its family's generic**, and families carry a procedural tier C equal to their generic, so
   a fallback always fits the socket its variant was planned for; the hero-wall module's fallback loses only the band.
   The three dressing kinds are three families (step 1a's ids share no prefix but `decor`): a missing magazine stack
   becomes the procedural magazine stack, not storage boxes.
6. **The resolver in `rv_assets`** (`prefab_resolver.hpp`, the one new source file outside `rv_geometry`): manifests
   live there and `rv_world` depends on it. It builds procedural prefabs to judge them, so a broken parameter set falls
   back like a broken file. It logs one warning per fallback and nothing for errors (the caller reports those).
7. **Glass as single sheets**: a blended double-sided box laid two hazy layers over the items.
8. **Dressing sized by the caller**: the manifests carry no size; the generator merges the level's `width`, `height`,
   `depth` (the Level anchor's bounds) and `seed` (its propSeed reduced to [0, 2^31 - 1]) into the parameters.

## Notes for later steps

- **Step 3** (capacity, fill order, dressing): build each fixture's manifest through `assets::resolvePrefab`, then
  `geometry::slotLevels(prefab.sockets)`; a level is face-out when its slot volume equals `slotSizeFor(FaceOut, item)`.
  The hero band is level 5 of the hero-wall module; ADR-0010 §5's band order starts at the spine-out level whose slot
  centres are nearest 1.3 m: level 4 (1.24 m), then 6 (1.64 m) and 3, 2, 1, 0 below. Dress a level with the kind chosen
  from its propSeed, sized to `socketSize(Level_<n>)`. The display case's and the wall shelf's Level anchors lean with
  their levels (the case's) or stand on the ledge (the wall shelf's): whether a face-out level without titles is dressed
  there is the step's (and 16's) choice.
- **Step 1a / the template**: the ids are the template's. The sockets hold the prefabs: modules 0.9 x 0.24 m in
  0.9 x 0.4 m sockets, the featured wall shelf 0.9 x 0.08 m (hung at 1.1 m) in its 1.1 x 0.4 m socket, the display
  cases 1.2 x 0.5 m exactly. The media family's spine-out slot (20 x 125 x 85 mm, 4 mm gap, 30 mm margin) is the
  modules'; its face-out gap and margin (4 and 30 mm) are the media family's defaults, while each display fixture keeps
  its own spacing (the hero band 6 and 25 mm, the cases 12 and 30 mm, the wall shelf 12 and 20 mm): the slots a
  generator reads are the prefabs' sockets. The modules keep 50 mm free above a spine-out cartridge (the family's
  headroom is 20 mm).
- **Step 5**: the spine layer is drawn upright (40 x 256, top up); a title reading top to bottom is the text turned a
  quarter clockwise. The label's 13.5 x 86.4 mm keep that aspect exactly.
- **Step 10a**: an item's transform = fixture * `socket.transform` * `slotItemTransform(facing, cartridge size)` * the
  placement's offset (spine-out: pushed in along the slot's -Z by 0 to 4 mm; face-out: a yaw about the slot's +Y, which
  leans with the display slots). Glass casts no shadow (blended); the display case's collider is its bounds.
- **Step 10c**: `resolvePrefab`'s `source` and `fallbackReason` say what happened; the one warning is already logged.
  A model manifest's file is only checked for existence: when its glTF then fails to load, resolve the family
  (`families.familyOf`) and use its generic as `resolvePrefab` would.
- **Step 16**: the display glass's haze, the fabric colour inside the cases, the dressing's materials
  (`MAT_Cardboard_Box`, `MAT_Paper_Print`, `MAT_Fabric_Sofa` and `MAT_Plastic_Black` books, `MAT_Metal_Graphite`
  bookends in the preview).

## For the merge

- **CLAUDE.md** (not edited by the step): "Asset preview": the Statistics panel lists the slots by volume, slot and
  level volumes are drawn with the sockets; "Layout and layering" or a new line: prefab families in
  `assets/manifests/families.json`, `assets::resolvePrefab` (style §21's object fallbacks, one warning each), the
  collection's prefabs (`display_case`, `wall_shelf`, `storage_box`, `magazine_stack`, `bookends`, the cartridge's
  `spineLabel`, the shelf's display levels and front inset, `Level_<n>` anchors).
- **docs/architecture.md**: the `geometry` row (the collection's prefabs, slot and level sockets, `SlotFacing`) and the
  `assets` row (prefab families and `resolvePrefab`); the asset preview section (slots in Statistics).
- **docs/roadmap.md**: step 1b done, with the figures `D` 44, `B` 12, `Sh` 198, `S` 231.
- **ADR-0010** needs no change: its figures stand; §5's "7 levels between 0.35 and 1.9 m" reads, built, "7 levels on
  boards from 0.375 to 1.575 m in a 1.775 m module", which this page records.
