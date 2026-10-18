# I3 step 3: fresh generation

Roadmap row: I3 step 3 (wave 1, on 1a and 1b; steps 2 and 4 merged on main while it ran and are merged into it).
Decisions: [ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §3 (the display rule chosen by the
user), §4 (the template, socket and fixture order, growth, lights), §5 (capacity), §6 (allocation), §7 (seeds), §8
(determinism). The step fills the stubs step 0a listed for it: `world::generate` and the data it reads, capacity from
the prefabs' slot sockets, the local seeds, the fixture layout and the allocation, and `retroverse-cli worldplan
generate` and `show`. It adds the plan's lights to schema 1 (as step 1a's decision 13 planned).

## What was built

| File | What it holds |
|---|---|
| `rv/world/capacity.hpp`, `src/capacity.cpp` | `PrefabInfo` (a prefab as the generator reads it: bounds, slot levels, a lamp's light source, in millimetres rounded once), `PrefabCatalog`, `prefabsToRead()`, `readPrefabInfo()` (through `assets::resolvePrefab`, so a missing manifest falls back to its family), `loadPrefabCatalog()`, `prefabCatalogToJson()`; `LevelFacing` (face-out, spine-out or other, from the slot volume against the media family's item); `CollectionCapacity` (D, B, 2 Sh, S, side-wall sockets per growth step), `collectionCapacity()`, `reserveFor()`, `sideModulesFor()`, `CollectionCounts`, `countCollection()` |
| `rv/world/seeds.hpp`, `src/seeds.cpp` | `roomSeed`, `fixtureSeed`, `propSeed` (a socket's decoration, a dressed level), `placementSeed`, as the oracle's chain spells them |
| `rv/world/generation.hpp`, `src/generation.cpp` | `kGeneratorVersion` (1.0), `kDefaultRoomTemplate`, `WorldData` and `loadWorldData()` (template, platform manifest, media family checked against its item's manifest, every prefab measured), `GenerationInput`, `generate()`; the input digest; the validation of the result (step 4's `validatePlan`) |
| `src/fixture_layout.hpp`, `src/fixture_layout.cpp` (private) | `layOutRoom()`: the room at its growth, the hero corner's parts, the modules, the display fixtures, the lights, the decoration |
| `src/allocation.hpp`, `src/allocation.cpp` (private) | `allocate()`: band order, quotas, the titles of each section in reading order, the placements' offsets, the dressing |
| `rv/world/world_plan.hpp`, `src/world_plan.cpp`, `rv/world/plan_json.hpp`, `src/plan_json.cpp` (1a's) | `LightRole`, `LightKind`, `lightKind()`, `PlanLight`, `WorldPlan::lights`, sorted by id; written and read as `"lights"` |
| `tools/cli/src/cmd_worldplan_generate.cpp`, `cmd_worldplan_show.cpp`, `world_plan_text.hpp` (new, header only) | the two subcommands and what they share (the plan's default place, the library, the summary as text and JSON) |
| `tools/cli/src/cmd_worldplan_check.cpp` (4's) | the hook step 4 asked for: the plan's lights counted per room into `ValidationContext::lights` |
| `tests/unit/world/test_world_capacity.cpp`, `test_world_generation.cpp`, `world_generation_support.hpp`, `tests/unit/cli/test_cli_worldplan.cpp`, `tests/fixtures/world/golden/` | the tests and the 25 golden plans (below) |

`layout_score` stayed step 4's (it computes the LayoutScore inside `validatePlan`, which `generate` now runs).

## The algorithm

`generate(input, data)` is a pure function of the selection, the configuration, the library id and the world's data
(template, platform manifest, media family, measured prefabs):

1. **Capacity** (`collectionCapacity`): D is the face-out slots of the `displayOrder` sockets' prefabs, B those of the
   hero-wall module times the `heroBand` sockets, 2 Sh the spine-out slots of every hero-module socket, S the side-wall
   module's spine-out slots; a level's facing is read from its first slot's volume against the media family's item
   (`slotSizeFor`'s rule of step 1b), never from the shelf's layout formula.
2. **Counts** (`countCollection`): R = ceil(T x reserve); F = min(T, D + B), the first D featured, the next B in the band;
   e = max(0, ceil((T - F + R - 2 Sh) / S)); the room grows by the fewest steps whose side-wall sockets hold e; when even
   the template's limit does not, the largest T' that fits (bisection: e never shrinks as T grows) is placed and the
   titles T' + 1 .. T, last in priority order, are unplaced with reason `capacity`.
3. **Layout** (`layOutRoom`), in ADR-0010 §4's order: the room's bounds at its growth; the hero corner's parts (the
   platform's console for the `console` part) at their template positions; the modules in `collectionOrder` (the two
   hero-wall modules, then the first e side-wall sockets that exist at the growth); the display fixtures; the lights;
   the decoration. A prefab stands with its back on its socket's back edge, centred across the socket's width
   (`pivot = anchor + right x -(minX + maxX) / 2 + front x -minZ`), lifted by the decoration's mount height; a module or
   display prefab that does not fit its socket is an error, decoration that does not fit is not chosen there.
   - **Decoration**: every side-wall socket no module needs, in fill order, then round(density x 4) of the decoration
     sockets (2 of 4 at 400 permille), those whose prop seeds are lowest; each takes one of the template's choices that
     fits it, drawn from its prop seed; a lamp only while the light limit allows its light.
   - **Lights** (each plan light is one scene light, so the renderer's limit counts plan lights): the key spot over the
     middle of the hero wall's collection (2.3 m out, aimed at the wall's foot, 0.5 m high), the pendant over the middle
     of the base room, the hero corner's lamp at its prefab's `LightSource`, the picture light on the sign, then
     wall-washers: the modules of each wall form runs of adjacent sockets (at most 200 mm apart; a door-reserved socket
     breaks a run), cut from the end that fills first into pieces of g modules, g the smallest from 2 whose pieces the
     remaining lights hold (one per run in fill order when even that is too many); last, decoration lamps. Positions and
     targets only: colour, intensity, range and cones are the lighting profile's (10a, 16).
4. **Allocation** (`allocate`): within each section the titles stand in (catalog sort key, title key, GameId) order,
   as one reads a page: a fixture's top level first (highest slots, then the highest level number), left to right
   (slot 0 first). The featured titles fill the display fixtures in `displayOrder`, the band's the hero band's modules
   in `heroBand` order, each level whole before the next; the free tail of the last face-out level and the face-out
   levels no title needs are dressed. The spine-out titles fill the modules one after the other: in each module the
   spine-out levels in band order (the level whose slots' centre is nearest the template's eye height, 1.3 m, then
   alternately above and below, above first), each to its quota floor(slots x 1000 / (1000 + reserve permille)) (28 of
   33), every quota of the section rising by one while they cannot hold the section's titles; the used levels keep
   their last slots free (the reserve), the unused ones are dressed. A dressed level's prop is
   `shelfDressing[Random(propSeed(level)).below(3)]`; a placement's offset `Random(placementSeed)`: spine-out pushed in
   by 0 to 4 mm, face-out turned by -1.5 to +1.5 degrees.
5. **The plan**: versions (`generatorVersion` 1.0, `splitmix64/1`, the selection's title-key rule and sort-key version),
   the world seed (`worldSeed(config, library)`), the preset, `inputDigest` (the SHA-256 of the canonical document of
   the configuration without `reduceMotion`, the world seed, the selection's canonical document, the template's id and
   version, what the plan takes from the platform manifest and the media family, and every prefab as measured), the
   selection's unplaced games plus the `capacity` ones, sorted (`sortPlan`). The change report is a fresh plan's
   (Generated, Full, every placement and fixture added, the digest after); the validation report is `validatePlan`'s
   against the template, the configuration, the selection, the prefabs' slots as measured and the plan's lights.

## Figures

Capacity, from the shipped manifests (asserted by `test_world_capacity.cpp`): D 44, B 12, 2 Sh 396, S 231; side-wall
sockets 14, 18, 22, 26 at 10, 12, 14, 16 m; quota 28 of 33.

| Titles | Face-out (featured + band) | Spine-out | Reserve | Side-wall modules | Growth | Unplaced (capacity) |
|---|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 1 + 0 | 0 | 1 | 0 | 0 | 0 |
| 15 | 15 + 0 | 0 | 3 | 0 | 0 | 0 |
| 50 (G02, `snes-room`'s size) | 44 + 6 | 0 | 8 | 0 | 0 | 0 |
| 56 (D + B) | 44 + 12 | 0 | 9 | 0 | 0 | 0 |
| 57 | 44 + 12 | 1 | 9 | 0 | 0 | 0 |
| 890 (the reference library) | 44 + 12 | 834 | 134 | 3 | 0 | 0 |
| 2,000 (`snes-stress`) | 44 + 12 | 1,944 | 300 | 8 (exactly full with the reserve) | 0 | 0 |
| 3,205 (the 10 m room's capacity) | 44 + 12 | 3,149 | 481 | 14 | 0 | 0 |
| 3,206 | 44 + 12 | 3,150 | 481 | 15 | 1 | 0 |
| 5,615 (the 16 m room's capacity) | 44 + 12 | 5,559 | 843 | 26 | 3 | 0 |
| 8,000 | 44 + 12 | 5,559 | 843 | 26 | 3 | 2,385 |

At 890 titles the hero-wall modules hold 6 x 28 spine-out titles each, w01 and e01 7 x 28, w02 106 (levels 4, 5 and 3
at 28, level 6 at 22; levels 0 to 2 dressed); the other 11 side-wall sockets hold decoration; 8 lights (key, pendant,
lamp, picture, wall-washers over hw-w, hw-e, w01-w02 and e01). With the shipped data no plan of the golden sizes has a
soft finding of the validation (LayoutScore reported), and every plan of the 1,000 random sizes passes its hard rules.

Timings (Release, the reference machine with other worktrees building, so informative): `generate` of 2,000 titles
median 67 ms over 21 runs with its validation (budget 200 ms; 38 to 69 ms before step 4's validation was merged in),
of which the input digest about 25 ms and the plan's canonical text (`planDigest`, which `writePlan` verifies with its
reader) about 15 ms, the layout and allocation under 10 ms; `writePlan` alone 13.5 ms; `loadWorldData` 8 to 13 ms. In Debug `generate` of 2,000 titles takes about 0.8 s (the
JSON documents), which is why the property suite checks 60 sizes there and 1,000 in Release. The golden plans weigh
1.8 MB together (the 8,000-title plan 0.7 MB).

## Decisions taken within the step

1. **Lights are plan data, one plan light per scene light.** `PlanLight` records role, position, target and span; the
   lighting profile gives colour, intensity, range and cones when the room is drawn, so switching profiles moves
   nothing (step 16) and the renderer's 16 count plan lights. Ids are FixtureIds of what they serve
   (`fx:hero.key-light`, `fx:w01.washer`, `fx:w05.light`, `fx:room.pendant`, "room" being the room itself, never a
   socket). The reader refuses a point light with a target or a span.
2. **The plan records the prefab the data names**, measured through `assets::resolvePrefab`: a missing manifest falls
   back to its family (logged once) and the room falls back alike when drawn; `PrefabInfo::resolvedManifest` says which
   was used, and the measures enter the input digest.
3. **Facing from the slot volume**: a level whose slot is neither the item face-out nor spine-out (`LevelFacing::Other`)
   holds no title and is dressed; a face-out level of a module outside the band is dressed too.
4. **Band order starts above** the nearest level and then alternates (ADR-0010 §5 "alternately above and below";
   step 1b's note: 4, 6, 3, 2, 1, 0 on the hero-wall module; 4, 5, 3, 6, 2, 1, 0 on the side-wall module). A tie of
   distances keeps the lower level.
5. **Reading order is by height**, top first: the display case's levels step up from the front, so its back level reads
   first, as a shelf's top one does.
6. **A small library's reserve is its dressed levels.** With no spine-out title every spine-out level is dressed (no
   bare board); dressing gives way to new titles (ADR-0010 §5, §10), which is where step 9 puts them.
7. **Decoration density** applies to the template's decoration sockets only (2 of 4 at 400 permille: what step 4's soft
   rule expects); every unneeded side-wall socket is decorated whatever the density (ADR-0010 §3: never sparse).
8. **Lamps' lights are turned in quarter turns** (the floor lamp's bulb stands on its axis): the plan's arithmetic stays
   integer, no sine of a part's yaw.
9. **`generate` validates its plan** with the slots it measured and returns the report; `worldplan generate` writes
   nothing when a hard rule fails. `worldplan check` (and the tests) judge with `manifestPrefabSlots`, and the property
   test proves both find the same.
10. **The input digest names the template by id and version**, not its bytes: a template whose sockets change is a new
    `version` (ADR-0010 §8's `templateVersion`); a comment edit changes no plan.
11. **`worldplan generate` takes the catalog's selection by default** (step 2's `roomEntries`, `checkRoomMedia`,
    `selectRoom`, as the app will), exit 1 while the media are unknown; `--selection` reads a canonical selection file
    instead (the tests, the goldens' selections). It never writes over an existing plan (`--replace` is step 9's);
    `--plan` names another file.
12. **Golden plans are generated selections**, not `snes-room`'s (step 1c builds it in parallel): invented syllable
    titles from `makeSelection(T, 20261018)`, world seeds 102938, 7 and 2^64 - 1.

## Tests

- `test_world_capacity.cpp` (`[world][capacity]`, `[world][seeds]`): D, B, Sh, S and the side-wall sockets from the
  manifests; the hero-wall module's levels, facings, size and slot centres; the floor lamp's light source; the counts
  at 13 sizes (the table above); the fewest modules and the least growth for every 7th size up to 5,615; the local
  seeds equal to the oracle chain's goldens.
- `test_world_generation.cpp` (`[world][generation]`): the 25 golden plans byte for byte, each generated twice and read
  back (`RV_UPDATE_GOLDEN=1` rewrites them; Debug wrote them, Release reproduces them); every plan of the golden sizes
  validated with `manifestPrefabSlots` and the plan's lights, no hard finding, the soft ones reported (none), equal to
  the generator's own report; the room at 0, 50, 890, 2,000 and 8,000 titles (module counts, per-module and per-level
  counts at 890, growth to 16 m and the side-wall modules unmoved by it); the property suite (1,000 sizes in Release,
  60 in Debug, mostly up to 700, a quarter up to 3,300, a twentieth past the limit; random selections and seeds): face-out
  exactly the first min(T, D + B) by priority, the featured the first D; every title placed once or unplaced for
  capacity, the last by priority; the fewest side-wall modules; every module before the last holding spine-out titles
  at one quota on every spine-out level, none after it; no bare level, titles from slot 0 without gaps, dressing only
  after them; every side-wall socket of the room a module or decoration, nothing in door-reserved or ungrown sockets;
  no two fixtures of different sockets overlapping, each inside its socket, clear of the doorway; levels and slot counts
  the prefabs'; lights at most 16, one key; no hard finding. The result's reports and versions, the input digest
  following the inputs, the default seed the library's, `reduceMotion` changing nothing; refused inputs (another room,
  titles out of order, a game both title and unplaced, a configuration out of range, a module wider than its socket).
  `[.bench]`: 2,000 titles, the budget checked in Release.
- `test_plan_json.cpp` (1a's): lights written by id, read back, a point light's aim and span refused, roles round-trip.
- `test_cli_worldplan.cpp` (`[cli][worldplan]`): generate from a selection file into the world folder equal to
  `generate`'s text, never over an existing plan, `--plan`, `--json`, world.json's seed; usage errors (seeds, library,
  no library.json), a system without a platform manifest, a broken selection; the catalog's selection of the imported
  `esde/standard` fixture (no catalog: 3; an unknown system: 1; another library: 2; the media folder renamed away: 1 and
  nothing written; then a plan, validated, all face-out); show's summary, `--placements`, `--json`, a missing plan (1),
  a hand-edited or newer plan (3).
- `test_cli_commands.cpp`: `generate` and `show` left the list of stubs.

Results on the reference machine, with other worktrees building, after merging main (steps 2, 4 and 5):
`msvc-x64-debug` and `msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` ran 1,463 unit tests in Release,
all passed (1,113 s), and 1,463 in Debug (1,574 s), all passed but `unit.app/catalog_worker: a session write that
meets a busy database ...`, an I2 timing test that failed once under the load and passed when run again
(`-Filter "catalog_worker: a session write"`). The golden plans written in Debug are reproduced byte for byte in
Release.

## Notes for later steps

- **4**: the generator's plans pass every hard rule and, with the shipped data, no soft one. Fixture boxes: the
  generator places a prefab by its own bounds (`PrefabInfo`), which agree with step 4's per-kind origins for the shipped
  prefabs (posters at the middle of their back, the rest on their floor centre). Hero corner parts overlap each other by
  design (the CRT on the cabinet, the armchair on the rug); step 4's floor-covering rule and part handling accept them.
- **9**: new titles go to the spine-out section by fill order: modules in `collectionOrder`, levels in band order
  (`allocation.cpp`'s `bandOrder`), the first free slot after the alphabetical predecessor; the reserve of a used level
  is its free tail, of a small library its dressed levels. A new side-wall module takes the next side-wall socket in
  `collectionOrder` (its decoration removed); growth adds the step's sockets. The face-out set is the plan's `featured`
  and `band` placements. Re-run `layOutRoom`'s light rules after a module is added (the wall-washers' runs change) and
  keep every other light. `inputDigest` changes with any input; compare versions, seed and preset for
  `RequiresExplicitRegeneration`, not the digest.
- **10a**: an item's transform is the fixture's pivot and yaw times the prefab's `Slot_<level>_<index>` socket times
  `slotItemTransform(facing, item)` times the placement's offset (`insetMm` along the slot's -Z, `yawCdeg` about its +Y).
  A plan light becomes one scene light: `lightKind(role)` gives point or spot; the key alone casts shadows; spans give
  the cones (`spanMm` at the distance from position to target). Dressing props are sized to the level's `Level_<n>`
  anchor (step 1b), the display case's levels included.
- **16**: with the shipped data the side-wall sockets (0.9 x 0.4 m) take only the poster: the floor lamp is 0.44 m
  across and the sideboard 1.2 m wide, so every unneeded side-wall socket holds a poster and no decoration lamp is lit.
  Decoration that fits (a narrower lamp manifest, a bench, a plant) belongs in the template's `decoration`. The light
  positions are constants at the top of `fixture_layout.cpp`, taken from the test room's; they may move into the
  template's data.
- **10b / 17b**: generation of 2,000 titles is within its budget in Release; the input digest and the plan's canonical
  text are most of its time (nlohmann documents), worth a look only if 10b's 1 s commit budget is tight.

## For the merge

- **Order**: steps 2 and 4 are merged into this branch already (one merge commit, no conflict). This step edits step
  1a's `world_plan.hpp`, `world_plan.cpp`, `plan_json.hpp`, `plan_json.cpp` and `test_plan_json.cpp` (the lights; the
  sample text gains `"lights":[]` on its existing line) and step 4's `cmd_worldplan_check.cpp` (the light count) and
  `validation.hpp` (one comment). `tests/unit/cli/test_cli_commands.cpp`: the stub list keeps `update` only.
- **CLAUDE.md** (not edited by the step), "retroverse-cli": `worldplan generate --system <system> [--selection <file>]
  [--seed <n>] [--library <id>] [--plan <file>] [--json]` generates a fresh plan from the catalog (or a selection file),
  validates it and writes `<user data>/worlds/<library id>/world-plan.json` (never over an existing plan; exit 1 while
  the media are unknown); `worldplan show [<plan>] [--library <id>] [--placements] [--json]` summarizes a plan.
  "Layout and layering", `rv_world`: `generation.hpp` (`generate`, `loadWorldData`), `capacity.hpp` (capacity from the
  prefabs' slot sockets), `seeds.hpp` since I3 step 3; the plan holds the room's lights. "Visual verification" or the
  tests' paragraph: golden plans in `tests/fixtures/world/golden`, rewritten only with `RV_UPDATE_GOLDEN=1`.
- **docs/architecture.md**: the `world` row gains the generator (capacity, layout, allocation, seeds) and the plan's
  lights.
- **docs/roadmap.md**: step 3 done. Its row's `worldplan generate --system snes --seed 102938` on `snes-room` waits for
  step 1c's fixture: the test does the same on `esde/standard` (imported, all face-out); with `snes-room` merged, a test
  in `test_cli_worldplan.cpp`'s pattern checks its 50 titles all face-out and no side-wall module.
- Test counts: 1,463 unit tests with steps 2, 4 and 5 merged (the benches hidden).
- **ADR-0010**: no change needed; §4's lights are placed as written ("one per two modules ... then spread evenly"), and
  §5's quota and band order as written (decision 4 above reads "alternately above and below" as above first).
