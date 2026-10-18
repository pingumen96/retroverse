# I3 step 1a: the world domain

Roadmap row: I3 step 1a (main, after 0a and 0b; 0c, 0d and 1b ran beside it in their own worktrees). Decisions:
[ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §2 (inputs and outputs), §3 (who stands where:
the types only), §4 (the template), §7 (ids), §8 (determinism and the canonical form). The step fills the stubs step 0a
listed for 1a: the WorldPlan with its ids and canonical JSON, WorldConfig, RoomSelection, RoomInput, the result and
report types, the data files under `assets/world` with their readers, and the room's bulk snapshot port in
`rv_catalog`. Nothing generates, selects or validates yet (steps 2, 3, 4): this step fixes the data every later step
reads and writes, so that the wave-1 worktrees share one model.

## What was built

| File | What it holds |
|---|---|
| `rv/world/ids.hpp`, `src/ids.cpp` | `RoomId`, `FixtureId` (a socket, optionally a part), `LevelId`, `SlotId`: validated, formatted and parsed, ordered by their parts; `PrefabRef` (manifest id and kind); `isValidSocketName`, `isValidDataId`, `isValidPrefabKind`, `isValidRuleName`; `detail::enumFromString` and `detail::parseDecimal` |
| `rv/world/world_plan.hpp`, `src/world_plan.cpp` | `WorldPlan` (versions, library, seed, preset, `inputDigest`, rooms, fixtures with their slot levels, placements, dressed levels, unplaced games), `GeneratorVersion`, the enumerations `FixtureRole`, `Section`, `UnplacedReason`, `sortPlan()`, `isSha256Hex()`; `kPlanSchemaVersion` = 1 |
| `rv/world/plan_json.hpp`, `src/plan_json.cpp` | `planToJson()`, `writePlan()` (the canonical text), `planDigest()`, `readPlan()` |
| `rv/world/world_config.hpp`, `src/world_config.cpp` | `WorldConfig` (preset, seed, decoration density, growth reserve, passage widths, include, reduceMotion), `worldSeed()`, `checkWorldConfig()`, the preset reader, `WorldConfigOverrides` (world.json), `loadWorldConfig()`, the canonical form |
| `rv/world/room_input.hpp` | `RoomInput`: the library and the system's `catalog::RoomSnapshot` (header only) |
| `rv/world/room_selection.hpp`, `src/room_selection.cpp` | `RoomSelection` (eligibility, the titles in priority order, the unplaced games), `RoomTitle`, `Eligibility`, `priorityLess()` (ADR-0010 §3's order), the canonical form |
| `rv/world/generation.hpp` | `GenerationResult`, `ChangeReport`, `ValidationReport`, `ValidationFinding`, `GenerationOutcome`, `RebuildLevel`, `Severity` (types only) |
| `rv/world/room_template.hpp`, `src/room_template.cpp` | `RoomTemplate`, `TemplateSocket`, `HeroPart`, `Doorway`, `DecorationChoice`, `Footprint`, `Wall`, `SocketKind`; `interiorFootprint()`, `socketFootprint()`, `doorwayClearance()`; the reader and its geometric checks |
| `rv/world/platform_manifest.hpp`, `src/platform_manifest.cpp` | `PlatformManifest` and its reader, `isSrgbHexColor()` |
| `rv/world/media_family.hpp`, `src/media_family.cpp` | `MediaFamily`, `SlotSpec`, `MediaFamilyRegistry` and its reader; `checkItemManifest()` (the family against its item's manifest) |
| `src/data_reader.hpp`, `src/data_reader.cpp` (new, private, in the source list) | the strict reading every document shares: `DocumentReader` (the first problem, its line), `ObjectReader` (keys checked, typed reads), `parseDataJson()` (comments, a key twice refused), `lineOfPointer()`, `findDuplicateKey()`, `requireStrictOrder()` |
| `src/plan_parts.hpp` (new, private header) | the unplaced games' JSON, shared by the plan and the selection (defined in `plan_json.cpp`) |
| `rv/catalog/room_entry.hpp` (`rv_catalog`, 0a's stub) | `catalog::RoomEntry`, `catalog::RoomSnapshot` (with `mediaKnown` and its reason) |
| `rv/catalog/catalog_query.hpp` | `ICatalogQuery::roomEntries(system)`, virtual with a default that answers `Unsupported` |
| `assets/world/templates/console_room_9x10.json`, `platforms/snes.json`, `media_families.json`, `presets/retro_museum_cozy.json` | the data (below), replacing 0a's placeholders |
| `tests/unit/world/test_world_ids.cpp`, `test_plan_json.cpp`, `test_world_config.cpp`, `test_world_data.cpp`, `test_room_selection.cpp` | the tests (below) |

The other 1a stubs keep including their header only: `generation.cpp` (the types are header-only; step 3 adds
`generate`), `seeds`, `title_key` and the rest belong to later steps.

## The data model

Units and frame (ADR-0010 §8): integers only. Lengths in millimetres (`...Mm`), angles in hundredths of a degree
(`...Cdeg`), ratios in permille (`...Permille`); seeds as unsigned 64-bit integers. A room's frame is ADR-0002's: +X
east, +Y up, north = -Z, the origin on the floor at the centre of the template's **base** room. Yaws run from 0 to
35999 about +Y, right-handed (glm::rotate's sense, counter-clockwise seen from above), applied to a prefab whose front
faces +Z: 0 faces south, 9000 east, 18000 north, 27000 west. The same units are used in the data files and world.json,
so the generator's arithmetic is integer from the file to the plan.

### Identifiers

| Id | Text | Notes |
|---|---|---|
| `RoomId` | `room:snes` | `room:` and a valid `catalog::SystemName` (which holds no `/` or `:`) |
| `FixtureId` | `room:snes/fx:w03`, `room:snes/fx:hero.crt` | a template socket, `.` and a part when a socket holds several fixtures |
| `LevelId` | `room:snes/fx:w03/l:2` | a fixture's level of slots, 0 the lowest; `localText()` is ADR-0010 §7's `fx:w03/l:2` for its prop seed |
| `SlotId` | `room:snes/fx:w03/s:3.12` | level and index as the prefab's `Slot_<level>_<index>` socket |

Socket and part names: 1 to 32 of `a-z`, `0-9`, `-`, the first a letter. Levels 0 to 999, indices 0 to 9999, decimal
without leading zeros. Ids order by their parts (room, socket, part, level, index), numbers as numbers (`s:2.9` before
`s:2.10`), and a socket's only fixture before its parts.

### The WorldPlan

```text
WorldPlan   schemaVersion 1, generatorVersion "major.minor", randomAlgorithm, titleKeyRule, catalogSortKeyVersion,
            library, seed (the world seed), preset, inputDigest (64 hex)
  rooms       id, template + templateVersion, interior bounds (minX, maxX, minZ, maxZ), heightMm, growthSteps, seed
              (the room seed), mediaFamily, themePack
  fixtures    id, role (hero-module | side-module | display | hero-corner | decoration), prefab {manifest, kind},
              pivot x/y/z, yaw, bounds width/height/depth, levels [{level, slots, section}]
  placements  slot, game, insetMm, yawCdeg
  dressing    level, firstSlot, count, prop {manifest, kind}
  unplaced    game, room, reason (duplicate-title | no-cover | unavailable | capacity), representative
              (duplicate-title only)
```

- **Slots are the fixtures' levels**: each fixture records its levels with their slot counts and their section
  (`featured`, `band`, `spine`), copied from its prefab's slot sockets, rather than one entry per slot (a room of 2,000
  titles has some 4,000 slots). A slot exists when its level is listed and its index is below the level's count; the
  section says whether its title stands face-out.
- **A placement names its slot** and carries the small seeded offset of ADR-0010 §6 (`insetMm` for spine-out, a yaw for
  face-out), so that the app needs no seed to draw it; the transform comes from the prefab's slot.
- **Dressed levels are spans** (`firstSlot`, `count`) of one level: a level wholly dressed, or the tail of a display
  level that the featured titles do not fill, or one slot freed by an alias (ADR-0010 §10). Several spans of one level
  are allowed; the validation keeps placements out of them.
- **The plan digest is not a field**: it is the SHA-256 of the canonical text (`planDigest()`), recorded by the meta
  sidecar of step 9.
- Room bounds are stored, not derived: a grown room's `maxZMm` is the base depth's half plus its growth, and
  `growthSteps` says how many steps it took.

The canonical text (`plan_json.hpp`'s header has the whole document): `core::writeCanonicalJson` with the lists sorted
by `sortPlan()`: rooms and fixtures by id, levels by level, placements by slot then game, dressing by level then first
slot, unplaced by game (then room, reason, representative). `writePlan()` sorts a copy, so the order in memory never
matters, and checks what it wrote with the reader, so a plan it writes is always one it reads. `readPlan()` takes
`core::parseCanonicalJson`'s strict reading (a hand-edited text is refused, not normalized), refuses a newer
`schemaVersion` first (`Unsupported`, whatever else the plan holds), then every field's form: types, ranges, ids,
enumerations, versions, digests, the order, no key twice. It does not check how the lists refer to each other (a
placement's fixture, a slot within its level, two games in one slot, a game both placed and unplaced): that is step 4's
validation, which must be able to read the plans it judges.

### WorldConfig, the preset and world.json

`WorldConfig`: `preset` (`retro_museum_cozy`), `seed` (optional; `worldSeed()` gives `deriveSeed("world", library)`
without one, `0xa1fac9cfdf9c5474` for `esde`), `decorationDensityPermille` 400, `growthReservePermille` 150,
`mainPassageWidthMm` 1400, `minClearWidthMm` 1200, `include` (`covered` | `all`), `reduceMotion` (ADR-0012 §3; how the
room is shown, never read by the generator). `checkWorldConfig()`: permilles 0 to 1000, the minimum clear width from 600
mm to the main passage's width, which is at most 5 m.

`loadWorldConfig(assetRoot, userConfigDir)` reads the default preset, then `<user data>/config/world.json`: every key
optional, the same names plus `preset`, `seed` (null for the default) and `reduceMotion`; a named preset is loaded first
and the user's keys go over it. A missing world.json is no issue; a file that cannot be read or parsed, a value out of
range, a preset that does not load or a combination `checkWorldConfig()` refuses leaves the file out whole, with one
issue (`LoadedWorldConfig::issues`, its file and line), as the other user files are. Only the default preset failing
to load is an error.

### RoomSelection, RoomInput and the snapshot

- `catalog::RoomSnapshot`: the system, `sortKeyVersion`, `mediaKnown` and `mediaUnknownReason`, the entries by GameId.
  `catalog::RoomEntry`: GameId, lookup key (with the exact relative path, whose last element is the file name), whether
  it is a folder, title, sort key, year, rating, ES-DE's favourite, hidden flag and play count, Retroverse's session
  count and last session, entry state, availability, media presence per kind. `ICatalogQuery::roomEntries(system)`
  returns it; the default answers `Unsupported` so that the existing sources compile unchanged, and step 2 implements it
  in `SqliteCatalogQuery` (`room_query.cpp`).
- `RoomInput`: the library and the snapshot.
- `RoomSelection`: the room, the eligibility rule applied (`covered`, `all`, `no-cover-in-system`), the title-key rule
  and the sort-key version, the titles (`RoomTitle`: the representative's GameId, its title key and sort key, favourite,
  play count, rating) **in priority order**, and the unplaced games as a plan lists them. `priorityLess()` is the order:
  favourites first, then the higher play count, the higher rating (rated before unrated), the sort key, the GameId.
  Its canonical form (with the titles in priority order, which the reader checks) is what step 3's `inputDigest` hashes;
  it holds sort and title keys, so it is never written into a plan or under user data.

### The result types

`GenerationResult` = the plan, a `ChangeReport` (outcome `generated` | `updated` | `requires-explicit-regeneration`,
`RebuildLevel` `no-rebuild` | `local-reflow` | `room-expansion` | `full`, the games added and newly unplaced, how many
placements were kept, the fixtures added, orphaned pins and the changes that need a confirmation, the digests before
and after) and a `ValidationReport` (findings with a rule name, `hard` or `soft`, a subject id and a message;
`passed()` when no hard one; the LayoutScore in permille when computed).

### The data files

Every file is JSON with comments, every key required unless the header says otherwise, unknown keys refused,
`schemaVersion` 1, the file's id equal to its name (`loadRoomTemplate`, `loadPreset`, `loadPlatformManifest`).

- **`templates/console_room_9x10.json`** (`room_template.hpp` documents the format). A socket is a floor rectangle: its
  anchor is the middle of its back edge, its yaw turns its local frame (+Z out of the back edge, +X to the right as one
  faces it), it reaches `widthMm` along local X and `depthMm` along local Z; sockets are axis-aligned, and a wall
  socket faces into the room with its back edge at most 100 mm from the wall (30 mm here, clear of the wainscot's rail).
  The room, 9.0 x 10.0 x 3.2 m:

  | Sockets | Kind | Where |
  |---|---|---|
  | `hw-w`, `hw-e` | hero-module (0.9 m) | the hero wall, either side of the hero corner and the featured shelf |
  | `hero` | hero-corner (2.4 x 3.0 m) | the hero wall: TV cabinet, CRT, the platform's console, the sign, the rug, the armchair, a floor lamp |
  | `fs01` | display (the featured wall shelf) | the hero wall, beside the hero corner |
  | `dc01`, `dc02` | display (low display cases) | at the circulation band's edges, facing it, 50 mm clear |
  | `w01`..`w07`, `e01`..`e07` | side-wall (0.9 m, 1 m apart) | the west and east walls, north to south |
  | `door-w`, `door-e` | door-reserved (1.8 m) | the middle of each side wall, between the 4th and 5th side-wall sockets |
  | `dec-nw`, `dec-ne`, `dec-sw`, `dec-se` | decoration | the hero wall's corners; the south wall either side of the doorway |
  | `w08`..`w13`, `e08`..`e13` | side-wall, growth steps 1 to 3 | two per side wall per 2 m step |

  The base room has the **16 collection sockets** (2 hero-wall, 14 side-wall), the **2 door-reserved** sockets and
  **3 display** sockets; 26 side-wall sockets at 16 m. `collectionOrder` (the 28 collection sockets: `hw-w`, `hw-e`, then
  `w01`, `e01`, `w02`, `e02`, ... north to south, the growth steps' at the end), `heroBand` (`hw-w`, `hw-e`),
  `displayOrder` (`fs01`, `dc01`, `dc02`). The doorway: 1.6 m in the middle of the south wall, 2.3 m high, 1.6 x 1.5 m
  clear inside it. The room grows south only, in 2.0 m steps to 16.0 m: a step moves the south wall, its sockets and
  the doorway by 2 m and adds its side-wall sockets; nothing else moves. Module prefabs (`modules.hero`,
  `modules.side`), each display socket's prefab, the hero corner's parts (in its local frame; the console's prefab is
  null: the platform's), `decoration` (poster at 1.55 m, floor lamp, sideboard) and `shelfDressing` (storage box,
  magazine stack, bookends). The reader checks the geometry: every socket inside the room at its growth step, none
  overlapping another or the doorway's clearance in the base room or after any step, every part inside the hero
  corner, exactly one hero corner with a `crt` and the `console`, the orders naming exactly their sockets.
- **`platforms/snes.json`**: `system`, `mediaFamily` (`cartridge_home_16bit`), `themePack` (`classic_16bit_warm`),
  `plastic` (cartridge `#9A9A96`, console `#D6CBAE`, accent `#7D6E9E`: the base materials' aged grey, aged ivory and
  violet accent, sRGB hex, uppercase), `console` (`hardware.console.cartridge.generic`, `console_cartridge`), `sign`
  (`16-BIT CORNER`, the test room's; 1 to 32 printable ASCII characters). Nothing branded.
- **`media_families.json`**: `cartridge_home_16bit`: the item `media.cartridge.home16.generic` (`cartridge`), 125 x 85 x 20
  mm; spine-out slot 20 x 125 x 85 mm, face-out 125 x 85 x 20 mm, both with a 4 mm gap (a 2.4 cm spine pitch), 30 mm
  margins and 20 mm headroom. The reader requires the slots to be the item in each orientation;
  `checkItemManifest()` checks the item against its manifest's `runtime.scaleMeters` (rounded to millimetres once) and
  generator.
- **`presets/retro_museum_cozy.json`**: the values above, `name` "RetroMuseumCozy".

### Errors

Every reader reports the first problem as an `Error`: context `<file>:<line>`, message `<JSON pointer>: <problem>`
(`console_room_9x10.json:57`, `/sockets/7/kind: expected one of hero-module, ...`). Syntax errors come from nlohmann
(`ParseError`, the line from its byte offset, the column in the message); semantic problems are `ValidationFailed`
with the line of the value the pointer names (or of the object a missing key belongs to), found by a small scanner of
the text (`lineOfPointer()`), which also refuses a key written twice in one object (nlohmann keeps the last silently).
A newer plan schema is `Unsupported`; a writer refusing a value is `InvalidArgument`.

## Decisions taken within the step

1. **One unit system from the files to the plan.** The data files and world.json are written in millimetres,
   hundredths of a degree and permille, as the plan is, and the readers refuse fractions (`0.15`, `1.0`). The cost is a
   less natural world.json (`"growthReservePermille": 250`); the gain is that no float enters the generator, so Debug
   and Release cannot round apart (ADR-0010 §5, §8).
2. **Slots as levels, not entries**: the plan lists each fixture's levels (slot count and section) instead of every
   slot; a slot is valid when its level is listed and its index is in range. Half the size of a per-slot list and it
   keeps the prefab's slot math in one place; the validation compares the levels with the prefab (step 4).
3. **Hero corner parts as fixtures with a part name** (`fx:hero.crt`): every prefab in the room is one fixture with one
   id, and ADR-0010 §7's `fx:hero` names the socket they share. Decoration in a socket can use parts the same way
   (`fx:w05.poster`).
4. **The room's frame is the base room's**: growth extends south and the origin stays, so a placement's coordinates never
   change when the room grows (ADR-0010 §4: growth moves nothing but the south wall). Rooms store their bounds.
5. **Placement offsets are stored**, not re-derived from the seed when drawing: the plan describes the room fully, a
   golden plan shows them, and the derivation is the generator's alone.
6. **`representative` only on `duplicate-title`**, absent otherwise (the reference room's 6,907 unplaced lines stay
   short); a representative on another reason is refused rather than dropped.
7. **The reader checks form and order, the validation checks references.** A plan with two games in one slot reads;
   `worldplan check` (step 4) is where it fails, with the rule's name. Equal sort keys are refused by the reader, since
   they make the canonical order ambiguous.
8. **The writer verifies with the reader**: `writePlan()` and `writeRoomSelection()` read back the document they made,
   so that a plan is never written that cannot be read (the cost is one more pass over a few thousand placements).
9. **A broken world.json is left out whole**, not key by key, with one issue: a partly applied file would make a world
   that matches neither the file nor the preset.
10. **`ICatalogQuery::roomEntries` has a default** answering `Unsupported`, so that `SqliteCatalogQuery` and the tests'
    in-memory queries compile unchanged until step 2 implements it ("types only").
11. **A strict reader of hand-written files of our own** (`data_reader`): unknown keys, duplicate keys, fractions,
    wrong types and ranges are refused with their line, instead of reusing `rv_assets`'s private `json_util` (which
    reports JSON paths without lines and reads numbers as floats).
12. **Prefabs the data names before they exist.** The template names the manifests step 1b writes (the modules, the
    featured wall shelf, the display case, the three dressing props) and an armchair manifest no step writes yet; a
    test accepts a named manifest that is missing only when it is in its pending list, so 1b's manifests can land in
    any order (and must have the kind the template names).
13. **Lights are not in the plan yet.** The template carries only the limit (16); ADR-0010 §4's lights (the key spot,
    the pendant, the lamps, the wall-washers) are placed by step 3, which adds them to schema 1 with the fields it
    needs (the schema stays open until Gate B).

## Tests

- `test_world_ids.cpp` (`[world][ids]`): socket names, data ids, prefab kinds and rule names, accepted and refused; every
  id formatted and parsed back (a system name with spaces and an accented letter included); 38 malformed texts and 7
  malformed parts refused (prefixes, empty and uppercase parts, leading zeros, ranges, separators); the order by parts.
- `test_plan_json.cpp` (`[world][plan_json]`): a plan with every kind of field written as a hand-written golden text
  (keys by code point, one element per line, the final line feed), the same text from reversed lists; read back as the
  sorted plan and written again byte for byte; an empty plan; 2,000 placements in a scrambled order round-trip; schema 2
  refused as `Unsupported` (also with unknown content), schema 0 refused; non-canonical texts (white space, `\/`, no
  final line feed, `3.0`) refused; 31 malformed fields refused, each with its message and the exact line; lists out of
  order, a placement twice, a fixture twice refused with their line, two games in one slot read; the writer refusing an
  empty id, a fixture twice, a stray or missing representative, a malformed digest, a newer schema; versions and
  enumerations as text.
- `test_world_config.cpp` (`[world][world_config]`): the shipped preset equal to ADR-0010 §2's values; the default seed
  `0xa1fac9cfdf9c5474`; world.json absent, overriding a few keys, naming another preset; eleven broken world.json files
  (a fraction, an unknown key, a syntax error, a key twice, a bad enumeration, a negative seed, a passage narrower than
  the clear width, a missing preset, not an object, lines counted through block and line comments and after a byte
  order mark) each left out with one issue and its line; a broken preset, a preset
  named after another file, a missing default preset; the canonical form round-trips, refuses values out of range.
- `test_world_data.cpp` (`[world][data]`): every file under `assets/world` read by its reader (4 files); no `.glb` or
  `.gltf` in any of them; the template's 16 collection sockets, 2 door-reserved and 3 display sockets, the hero band and
  the hero corner; the socket order (hero-wall modules, then north to south alternating west and east); the growth (3
  steps, 26 side-wall sockets at 16 m, the south wall's sockets and the doorway moving, the rest fixed) and footprints;
  the platform manifest and the media family (2.4 cm spine pitch), the family's item against its manifest (and two
  mismatches); every prefab the data names has a manifest of the named kind or is pending; 21 malformed templates
  refused with their message and line (a fraction, an unknown key, a bad kind, an id twice, a yaw not facing the room,
  a back edge off its wall, an overlap, the doorway's clearance entered, an overlap after growth, a socket leaving the
  grown room, a growth step past the limit, a prefab where none belongs, the console with a prefab, the growth order, a
  socket missing from the order, a wrong kind in the band, a display twice, a depth not reached by whole steps, 17
  lights, a model path as a manifest, a hero corner without parts), a syntax error and a key twice with their line; ids
  that are not their file's name; malformed platform manifests and families.
- `test_room_selection.cpp` (`[world][room_selection]`): the priority order; the canonical text as a golden, read back
  and written again; titles out of priority order, a game twice, an unplaced game of another room, a rating above 10 and
  an unknown eligibility refused; the result types' enumerations and `ValidationReport::passed()`.

Results on the reference machine, with other builds running at the same time: `msvc-x64-debug` and `msvc-x64-release`
built with 0 warnings; `./scripts/test.ps1` passed 1,339 unit tests in Debug and 1,339 in Release (243 s), 33 more than
step 0b's 1,306 (the 33 new test cases of `tests/unit/world`, 3,048 assertions in Debug).

## Notes for later steps

- **1b**: the template names `furniture.shelf.hero_module.generic` and `furniture.shelf.side_module.generic` (kind
  `shelf`), `furniture.wall_shelf.featured.generic` (`wall_shelf`), `furniture.display_case.generic` (`display_case`),
  `decor.storage_box.generic` (`storage_box`), `decor.magazine_stack.generic` (`magazine_stack`) and
  `decor.bookends.generic` (`bookends`). If 1b chose other ids or kinds, change the template (a data edit) rather than
  the manifests; `test_world_data.cpp`'s pending list then drops the names that exist. The sockets give the modules
  0.9 x 0.4 m, the featured shelf 1.1 x 0.4 m and the display cases 1.2 x 0.5 m (width along the front); a prefab that
  does not fit needs its socket changed in the template.
- **2**: fill `ICatalogQuery::roomEntries` in `SqliteCatalogQuery` (`room_query.cpp`), entries by GameId; `selectRoom`
  returns a `RoomSelection` with the titles sorted by `priorityLess()` (the reader refuses another order) and the
  unplaced games of the room; the title-key rule's name (`title-key/1` is the form `isValidRuleName` accepts) goes into
  `RoomSelection::titleKeyRule` and the plan.
- **3**: read the template with `loadRoomTemplate`, the platform with `loadPlatformManifest`, the family with
  `loadMediaFamilies` (check `checkItemManifest` against the loaded manifest), the configuration with `loadWorldConfig`
  (log its issues). A fixture's pivot is its prefab's origin: for a socket, the anchor plus half the prefab's depth
  along the socket's front, at the socket's yaw; the hero corner's parts are in its local frame (their `yMm` assumes the
  TV cabinet's default 0.55 m height). Decoration must fit the socket it goes to (the sideboard is 1.2 m wide: only the
  south wall's 1.4 m sockets take it). The plan's `inputDigest` should hash a canonical document of the selection, the
  configuration without `reduceMotion`, and the data files' versions (or digests). Lights go into schema 1 as a new
  top-level list. Use `kPlanSchemaVersion`, a `GeneratorVersion` constant of the generator's own, and
  `core::kRandomAlgorithm`.
- **4**: `readPlan` leaves every cross-reference to the validation: fixtures in a listed room, placements and dressing in
  listed fixtures and levels, indices below the level's count, one game per slot, one entry per game across placements
  and unplaced, no placement inside a dressed span, `representative` placed. `Footprint` and `socketFootprint` are the
  template's; fixtures need the same from their pivot, yaw and bounds.
- **9**: the plan file is `writePlan()`'s text; `readPlan()` refuses a hand-edited one (`ValidationFailed`) and a newer
  one (`Unsupported`); the digest for the meta sidecar is `sha256Hex` of the text.
- **10a**: the armchair manifest (`furniture.armchair.generic`, kind `armchair`) does not exist yet; write it, or let the
  family fallback of 1b resolve it. The sign uses the poster frame's manifest with its default size; a sign of its own
  size needs a manifest or parameters.

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Layout and layering", the paragraph on `rv_world` (or a new bullet): `rv_world` holds the world's domain since I3
    step 1a: ids (`room:snes/fx:w03/s:3.12`), the WorldPlan and its canonical JSON (`plan_json.hpp`: `writePlan`,
    `readPlan`, `planDigest`; a newer schema refused, a hand-edited plan refused), WorldConfig with the preset and
    `<user data>/config/world.json` (integers: millimetres, permille; a broken file left out whole with an issue),
    RoomSelection, the result types, and the readers of `assets/world` (the template, the platform manifests, the media
    families, the presets; errors name `<file>:<line>` and a JSON pointer).
  - "Configuration files": add `assets/world/presets/<id>.json` under `world.json` (per key; `preset` names another
    preset, `seed` a seed; read and never written).
- **docs/architecture.md**: the `world` row: the domain of step 1a is filled (ids, plan, canonical JSON, configuration,
  selection types, data files and their readers); `rv_catalog`'s row: `RoomEntry`, `RoomSnapshot` and
  `ICatalogQuery::roomEntries` (filled in step 2).
- **docs/roadmap.md**: step 1a done.
- Test counts: 1,339 unit tests in Debug and Release (step 0b's 1,306 plus 33); with 0c, 0d and 1b merged the counts
  add up.
- If step 1b merged with other manifest ids or kinds, align `console_room_9x10.json` and the pending list of
  `tests/unit/world/test_world_data.cpp`.
