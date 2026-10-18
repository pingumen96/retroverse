# I3 step 4: the WorldPlan's validation

Roadmap row: I3 step 4 (wave 1, needs 1a; steps 1b, 1c, 2, 3, 5, 6, 7 and 13r ran beside it in their own worktrees).
Decisions: [ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §11 (validation: the hard and soft
rules), §3 and §4 (the template, the display rule, the doorway and the door-reserved sockets), §9 (pins), with style
§6.2 (the space kept free in front of an interactive shelf), §6.3 (decoration density) and §13.5 (the LayoutScore). The
step fills the stubs step 0a listed for it: `validation.hpp`, `nav_grid.hpp`, the private `layout_score` and
`cmd_worldplan_check.cpp`. Step 3's generator does not exist yet: every rule is tested on plans built by hand, and
"every generated plan is valid" is step 9's (the generator's output checked by this validation).

## What was built

| File | What it holds |
|---|---|
| `rv/world/validation.hpp`, `src/validation.cpp` | `validatePlan(plan, context)`, the 29 rules (`rule::k...`, `validationRules()`, `findValidationRule()`), `ValidationContext` (templates, configuration, selections, prefab slots, light counts, pins), `PrefabSlotSource` and `manifestPrefabSlots(assetRoot)`, `RoomLights`, `GamePin`, `FixturePin`, the geometry constants |
| `rv/world/nav_grid.hpp`, `src/nav_grid.cpp` | `NavGrid` (0.1 m cells, obstacles and walls dilated by a clearance, Euclidean, integers) and `NavReach` (breadth-first search through edge neighbours, `reachesNear`) |
| `src/layout_score.hpp`, `src/layout_score.cpp` (private) | `detail::LayoutScoreTerms`, style §13.5's weights and penalties, `layoutScorePermille()` |
| `tools/cli/src/cmd_worldplan_check.cpp` | `retroverse-cli worldplan check <plan.json> [--selection <file>] [--json] [--output <file>]` |
| `tests/unit/world/world_test_plans.hpp`, `.cpp` (new) | hand-built plans of the shipped template: `validRoomPlan()`, `grownRoomPlan(steps)`, `selectionOf()`, `prefabSlotsOf()`, `validContextFor()` |
| `tests/unit/world/test_validation.cpp`, `test_nav_grid.cpp`, `test_validation_bench.cpp`, `tests/unit/cli/test_cli_worldplan_check.cpp` (new) | the tests (below) |
| `tests/unit/cli/test_cli_commands.cpp` | `check` left the list of worldplan stubs |

No shared file changed: the source lists already held every file, and `world_plan.hpp`, `plan_json.*`,
`generation.hpp` and the CLI's table are untouched.

## What a plan is judged against

`ValidationContext`:

- **templates**: the templates of the plan's rooms (by id). A room whose template is missing or of another version than
  the one it records fails `room-template`, and none of its geometry is judged.
- **config**: `WorldConfig` (`minClearWidthMm` 1200, `mainPassageWidthMm` 1400, `decorationDensityPermille` 400).
- **selections** (optional): a room's `RoomSelection`; every game it names (titles and unplaced) needs one entry.
- **prefabSlots** (optional): the slot levels of a prefab; `manifestPrefabSlots(assetRoot)` reads
  `<assetRoot>/manifests/prefabs/<id>.json`, requires its `runtime.generator` to be the plan's kind, builds it with its
  `runtime.params` (`geometry::buildPrefab`) and counts its `Slot_<level>_<index>` sockets per level (indices from 0
  without a gap); a model prefab's slots are not read.
- **lights** (optional): the point and spot lights per room. Lights are not in schema 1 yet (step 1a's decision 13:
  step 3 adds them), so the caller counts them; a room without a count is not judged.
- **gamePins**, **fixturePins** (optional): `overrides.json`'s pins once step 9 reads them.

`validatePlan()` never fails: what it cannot judge is a finding. The report lists the findings rule by rule in the
table's order, each rule's sorted by subject then message (a finding repeated is listed once), and
`layoutScorePermille`, always computed.

### Geometry

Millimetres in the room's frame. A fixture is the axis-aligned box of its prefab's bounds turned by its yaw about its
pivot; the pivot is the prefab's origin as `rv/geometry/prefabs.hpp` places it by kind: the middle of the footprint at
the bottom for most kinds, the middle of the back for `poster_frame` (the box reaches `depthMm` out along its front and
is centred on `yMm`), the centre for `cartridge`. Boxes are kept in half millimetres, so that a fixture of odd size
centred on its pivot keeps exact edges and two touching boxes never overlap by rounding. A turned box (the armchair at
210 degrees, the console at 352) is rounded outwards from sines and cosines of a Taylor polynomial (basic arithmetic
only: every build computes the same boxes; exact at quarter turns). A **floor covering** (on the floor, at most 20 mm
high: the rug, which has no collider in its prefab) overlaps nothing, blocks no cell and hides nothing, but may not lie
in the doorway's clear area nor leave the room.

### Navigation

`NavGrid` over the room's interior: cells of 100 mm, a cell blocked when its centre lies closer than the clearance to
an obstacle's rectangle (Euclidean, so corners are rounded) or to a wall, distances compared squared in integers. Every
fixture but floor coverings is an obstacle, wall-mounted ones included. The clearance is half the minimum clear width
(600 mm): a free cell is where a body 1.2 m wide can stand. `NavReach` searches from the cell at the centre of the
doorway's clear area through edge neighbours (never a diagonal between two obstacles touching at a corner).

Targets: every hero-wall and side-wall module, every display fixture and the hero TV (the hero corner's `crt` part).
A target's **interaction point** is the middle of its front face moved 800 mm out along its front; it is reachable
when a reached cell's centre lies within 400 mm of it, so the place the walker stands is 0.4 to 1.2 m in front of the
fixture (style §6.2: 1.1 to 1.5 m kept free in front of an interactive shelf). A second grid at half the main passage
width (700 mm) gives `passage-narrow`. The TV is in **line of sight** when the segment from the clear area's centre at
1.65 m (the walker's eye) to the middle of the CRT's front face crosses no other fixture's box (floor coverings
excepted); a touching segment does not cross.

## The rules

Hard rules (a plan that breaks one is never saved or shown):

| Rule | Subject | What fails it |
|---|---|---|
| `room-unknown` | fixture, game | a fixture or an unplaced entry in a room the plan does not list |
| `room-template` | room | the room's template is not known, or is now another version |
| `room-size` | room | the interior or height is not the template's at the room's growth step, or more steps than the template allows |
| `fixture-socket` | fixture | no such socket in the template, a socket of a growth step the room did not take, a role the socket's kind does not take (hero module: hero-module; side module: side-wall; display: display; hero corner: hero-corner, with one of the template's parts; decoration: side-wall or decoration), a part on a module or display, the fixture listed twice |
| `fixture-levels` | fixture | a section its role does not hold (featured on displays, band or spine on hero-wall modules, band only in the template's hero band, spine on side-wall modules), levels on a fixture without slots, a level twice, a level with no slots |
| `prefab-slots` | fixture | a collection fixture's levels differ from its prefab's slot sockets (with a prefab source) |
| `fixture-outside-room` | fixture | its box leaves the interior or rises above the ceiling |
| `door-reserved` | fixture | it stands in a door-reserved socket, or its floor enters one |
| `doorway-clearance` | fixture | its floor enters the doorway's clear area |
| `footprints-overlap` | the first fixture | two fixtures' boxes overlap (floor coverings excepted) |
| `lights-over-limit` | room | more point and spot lights than the template's `lightLimit` (with a count) |
| `slot-unknown` | slot | its fixture is not in the plan, its level is not listed, its index is past the level |
| `slot-shared` | slot | two games in one slot |
| `dressing-unknown` | level | a dressed span on a fixture or level the plan does not hold, past the level, or of no slot |
| `dressing-overlap` | level | two dressed spans of a level overlap |
| `slot-dressed` | slot | a placement inside a dressed span |
| `game-twice` | game | more than one placement or unplaced entry |
| `game-missing` | game | a game of the room's selection has no entry (with a selection) |
| `representative-unknown` | game | a duplicate title whose representative is itself, none, or a game neither placed nor listed for another reason |
| `pin-broken` | game, fixture | a pinned game not in its slot (both still in the plan), a pinned fixture missing or of another prefab |
| `hero-tv-missing` | room | no hero TV, or more than one |
| `doorway-blocked` | room | the clear area's centre is not a free cell; the targets are then not listed one by one |
| `unreachable` | target | no reached cell within 400 mm of its interaction point |

Soft rules (reported, the plan kept):

| Rule | Subject | What it reports |
|---|---|---|
| `prefab-unresolved` | fixture | a collection fixture's prefab whose slots cannot be read (missing manifest, another kind, a model) |
| `pin-orphaned` | game, fixture | a pin naming a slot or a game that the plan no longer holds, or a socket the template no longer has |
| `tv-line-of-sight` | the TV | the fixtures that hide it from the doorway |
| `passage-narrow` | target, room | reachable through 1.2 m but not through the main passage (1.4 m); the room when the doorway leaves no main passage |
| `socket-bare` | socket | a hero-module or side-wall socket with neither a module nor decoration (ADR-0010 §3: never empty) |
| `decoration-density` | room | the share of decoration sockets in use falls outside the band of style §6.3 that holds the configured density (0-249, 250-599, 600-849, 850-1000 permille) |

The **LayoutScore** (style §13.5, permille, chooses nothing in I3): navigation clearance (the targets reachable through
the main passage) 240, hero visibility (the TV in line of sight) 190, collection capacity (placements among placements
and `capacity` entries) 170, visual balance (side-wall modules west against east) 150, screen visibility (the hero TV
alone in I3) 100, theme match (one theme pack per room: 1000) 80, decoration variation (distinct decoration prefabs per
decoration fixture) 70; minus 100 per overlapping pair, 200 per door-reserved, doorway-clearance or doorway-blocked
finding, 20 per pair of adjacent levels of one fixture dressed with the same single prop; clamped to 0..1000. The
hand-built room scores 951 (its ten decorations come in three prefabs).

## `retroverse-cli worldplan check`

`retroverse-cli [global options] worldplan check <plan.json> [--selection <file>] [--json] [--output <file>]` reads the
plan (`readPlan`: a hand-edited text or a newer schema is not a plan this program reads), the templates its rooms name
from the asset root, the prefabs' slots (`manifestPrefabSlots`), the configuration (the user's, the default preset or
the one `world.json` names with `world.json` over it, when it is of the plan's preset; else the plan's preset alone)
and, with `--selection`, a room selection's canonical JSON. Text: the plan, its digest (SHA-256 of the file), each room
(template, size, growth, fixtures), the counts, whether a selection was checked, the LayoutScore, `Findings: <n> hard,
<n> soft`, each finding as `  <severity> <rule> <subject>: <message>` (100 per rule, then a count), `Result: passed` or
`FAILED`. `--json`: `plan`, `readable`, `error`, `digest`, `selection`, `rooms`, `placements`, `dressing`, `unplaced`,
`passed`, `hardCount`, `softCount`, `layoutScorePermille`, `findings` (all of them). Exit codes: 0 no hard rule fails
(soft findings allowed); 1 a hard rule fails, or the file is not a plan this program reads (`Not a plan this program
reads: <reason>`, `"readable": false`); 2 usage error (no plan, an unknown option, a selection that cannot be read or is
not a selection); 3 the plan file, the configuration or the results cannot be read or written. It writes nothing but
its results and the log; lights and pins are not judged by the command until the plan holds lights (step 3) and
`overrides.json` is read (step 9).

## Tests

- `test_validation.cpp` (`[world][validation]`): the rule table (29 names, hard before soft); `validRoomPlan()` passes
  with no finding at all, LayoutScore 951, the same report with its lists reversed and without the optional inputs;
  the 16 m room with 26 side-wall modules passes. For every hard rule a plan changed from the valid one that breaks it
  and no other hard rule (the row's list: overlapping fixtures, a shelf in a door-reserved socket, a blocked doorway, an
  unreachable shelf, 17 lights, a game neither placed nor unplaced, two games in a slot; and every other rule:
  unknown rooms, templates and sizes, sockets, levels, prefab slots, outside the room, a fixture entering a
  door-reserved socket, a rug in the doorway, the doorway blocked alone by a wider clear width, slot ids, dressing,
  games twice, representatives, pins, the hero TV missing and walled in), each with its subject and message; touching
  boxes and the turned armchair's box to the millimetre; for every soft rule a plan that reports it and still passes;
  the findings' order; the prefab slots of the shipped manifests (the test room's shelf, a cabinet without slots, a
  wrong kind, a missing manifest, a malformed id).
- `test_nav_grid.cpp` (`[world][nav_grid]`): cells, centres, the cell of a point on an edge, an empty area; walls and
  obstacles at the clearance (a centre exactly at it is free, corners rounded), obstacles outside the area, a clearance
  of 0; the search through a gap that a wider clearance closes, `reachesNear`, no diagonal step between obstacles
  touching at a corner, nothing reached from a blocked or outside cell.
- `test_validation_bench.cpp` (`[.bench]`, Release): the grid of the 9 x 16 m room built and searched, median of 21,
  within 20 ms; `validatePlan` of that plan (40 fixtures, 2,000 placements) reported and bounded at 100 ms.
- `test_cli_worldplan_check.cpp` (`[cli][worldplan]`): a valid plan (exit 0; text and JSON, the digest, `--output`),
  a plan with two games in a slot (exit 1, the finding line), a selection with a game the plan lacks (exit 1) and the
  plan's own selection (exit 0), a hand-edited text and a newer schema (exit 1, `Not a plan this program reads`), a
  missing plan (exit 3), usage errors (exit 2), `--help`; nothing under `worlds/`. Its plan names unshipped prefab
  manifests for the collection fixtures, so its 11 soft findings do not depend on step 1b's prefabs.

Results on the reference machine (other worktrees building and testing at the same time): `msvc-x64-debug` and
`msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` passed 1,375 unit tests in Debug and 1,375 in Release
(the 19 new test cases among them; the first Release run failed once in `catalog_db/import: db rebuild-index reads
every sheet again`, which passed alone and in a second full run, as did a load-sensitive `app/catalog_worker` case of
an earlier Debug run). The Release bench: the grid of the 9 x 16 m room (90 x 160 cells, 39 obstacles, 8,606 cells
reached) built and searched in 0.57 ms (median of 21), `validatePlan` of that plan in 4.5 ms (Debug: 4.5 ms and 74 ms).

## Decisions taken within the step

1. **Optional inputs, each named.** The validation judges what it is given: without a selection no game is missing,
   without a prefab source the levels are not compared with the prefabs, without a light count lights are not judged,
   without pins nothing is pinned. The plan alone carries enough for every geometric rule, so `worldplan check` works on
   any plan file; steps 3, 9 and 10b pass what they hold.
2. **Lights and pins as inputs, not plan fields.** Lights enter schema 1 with step 3 and pins with step 9; adding either
   to `world_plan.hpp` here would edit a file both steps edit. `RoomLights` and `GamePin`/`FixturePin` are the
   validation's own types for the merge to feed.
3. **The interaction point** is ADR-0010 §11's "interaction point" made concrete: 800 mm in front of the front face's
   middle, reached within 400 mm. A cell must lie 600 mm from every obstacle, so the reached place is 0.6 to 1.2 m in
   front of a shelf, as style §6.2 asks; the radius tolerates the neighbours of a module 1 m apart.
4. **Prefab pivots by kind.** The plan records a prefab's origin; the box around it depends on where the origin lies in
   the prefab (`poster_frame` hangs from the middle of its back, `cartridge` is centred). The validation keeps a
   two-entry table of the kinds whose origin is not on the floor (`pivotOf`); a new kind of that sort adds itself there.
5. **Floor coverings by height** (on the floor, at most 20 mm): the rug lies under the armchair and is walked over, as
   its prefab says (no collider); no kind name is needed.
6. **3D boxes for overlaps, floors for door areas.** Stacked parts of the hero corner (the CRT and the console on the
   cabinet) touch and do not overlap; the door-reserved sockets and the doorway's clear area are floor areas, and
   nothing may stand over them at any height.
7. **`doorway-blocked` hides `unreachable`.** A blocked doorway reaches nothing; listing every target would bury the
   one finding that matters. The LayoutScore still counts them unreached.
8. **Two soft rules beyond the ADR's list**: `socket-bare` (ADR-0010 §3's "never an empty module or socket") and
   `prefab-unresolved`; `pin-orphaned` is §10's orphaned pin. The decoration density's soft rule reads style §6.3's bands,
   since the ADR gives the density (0.4) without a count of sockets; step 3 may place decoration sockets differently and
   then this rule only reports.
9. **A text that is not a plan exits 1**, not 3: the check ran and found the file unusable (a hand edit, a newer schema),
   as `db check` treats a damaged database. A file that cannot be read at all exits 3.
10. **The configuration of the plan's preset**: the user's `world.json` applies only when it selects the plan's preset;
    otherwise the plan's preset alone gives the passages, so a plan is judged with the widths it was made for.

## Notes for later steps

- **3**: the generator's output can be checked at once: `validatePlan(plan, {templates: {template}, config,
  selections: {selection}, prefabSlots: manifestPrefabSlots(assetRoot) or the generator's own slot table, lights:
  {{room, plan's light count}}})`. A hung prefab (`poster_frame`) is recorded at the middle of its back (its origin),
  standing ones at the middle of their footprint; the hero corner's parts at their template positions pass. The
  decoration density's rule expects 1 or 2 of the template's four decoration sockets in use at 400 permille.
  `capacity.hpp`'s slot counts and `manifestPrefabSlots` should agree (or one call the other).
- **9**: every generated plan and the golden plans validated; pins from `overrides.json` into `gamePins` and
  `fixturePins`; a plan whose validation fails is never written (ADR-0010 §11).
- **10b**: the app logs every finding of a failed plan and keeps the previous plan or the empty shell.
- When step 3 adds lights to the plan, `cmd_worldplan_check.cpp` counts them per room into `ValidationContext::lights`
  (a few lines), and its usage text drops the sentence about lights.

## For the merge

- **Order**: steps 3 and 9 edit nothing of this step; step 3's lights need one hook in `cmd_worldplan_check.cpp` (above).
  `tests/unit/cli/test_cli_commands.cpp`'s list of worldplan stubs: this step removed `check` (array of 3); step 3
  removes `generate` and `show`, step 9 `update`: adjacent lines, resolve by keeping the remaining stubs.
- **CLAUDE.md** (not edited by the step), "retroverse-cli": `worldplan check <plan.json> [--selection <file>] [--json]`
  validates a plan (ADR-0010 §11: the hard rules exit 1, the soft ones are reported, the LayoutScore as a diagnostic)
  against its rooms' templates and the prefabs' slots under the asset root and the world's configuration; a text that
  is not a plan exits 1, a missing file 3. "Layout and layering", `rv_world`: `validation.hpp` (`validatePlan`, the 29
  rules) and `nav_grid.hpp` (the 0.1 m grid) since I3 step 4.
- **docs/architecture.md**: the `world` row gains the validation and the navigation grid.
- **docs/roadmap.md**: step 4 done.
- Test counts: 1,375 unit tests in Debug and Release, 19 more than before the step (the bench is hidden).
