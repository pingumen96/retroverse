# ADR-0013: The building: floors by generation, a room per console, the elevator

- Status: accepted
- Date: 2026-10-20 (reviewed the same day by four adversarial readers: stability, feasibility, runtime, scope)
- Amends: ADR-0010 (the plan's schema 2, ids, compatibility per room), ADR-0011 (arrays per room on a resident floor,
  covers fitted to their family's label, the cache's bound), ADR-0012 (the hero CRT of the walker's room, "Mostra nella
  stanza" across floors); specification §7.3, §7.4, §13.2, §14.4, §21.3 and style §5.2, §5.6 as recorded under
  "Amendments"
- Relates to: ADR-0008 (the catalog), ADR-0004 (one decoder until I5)

## Context

I3 ends with one generated SNES room per library. M3 asks for "a heterogeneous collection generated automatically,
navigable and customisable" (specification §22): a hub, a connected room graph, several systems and media models,
streaming, fallbacks for unmapped systems. The documents picture a flat hub with portals (style §5.2).

The user decided (2026-10-20) a **building**:
- one room per console;
- **one floor per console generation**, with an **elevator** between floors;
- the **ground floor** is the hub: a lobby and the **arcade game room**;
- a **dedicated handheld floor**, and the computers on their own floor;
- systems with few titles **share one room per floor**;
- a **functional look** for now, a styling pass later;
- performance **acceptable**, not maximal.

**The reference library.** A read-only census taken on 2026-10-20 (`build/i4-survey`):
- 48 systems with games.
- 19,547 distinct titles with a cover, by the room selection's own rule (`title-key/1`; the I3 SNES figure, 890, exact).
- 58,328 distinct titles counting every game.
- The arcade sets are barely scraped (mame: 36 titles with a cover out of 6,862 games).
- Scraping is uneven: n64 has 2 covered titles out of 1,889 games (860 distinct titles), pcengine 1 of 1,538.
- Grouped as below, the busiest floors are the computers' (about 5,300 placeable titles) and the handhelds' (about
  4,500).

**What the code offers.**
- **Rooms.** The plan already holds `rooms[]`. Validation, pins, where-is and `toSceneFile` already loop over rooms.
  The app names its room once, as `kRoomSystem = "snes"`.
- **Loading.** `dropCollection` plus `showScene` already unloads a room and loads another.
- **Rendering.** The renderer draws any number of static groups and takes its lights per view
  (`RenderView::lights`), but `SceneInstance` packs at most 16 per scene once, and walls do not stop light.
- **Ids that collide.** Pick ids are unique per room only. The collection's registry keys (`collection/<family>/...`)
  are unique per media family, so two rooms of one family collide silently.
- **Doorways.** The template has one doorway in its south wall, which moves with growth, and two door-reserved side
  sockets that never move.

## Decision

### 1. Floors, separate scenes

The building is a list of **floors**. Each floor is its own scene and the unit of residency (§9). Floors are never
stacked. The floors are data, `assets/world/building.json`, listed here in elevator order:

| Order | Floor | Holds |
|---|---|---|
| 0 | `ground` | the lobby and the arcade game room |
| 1-6 | `gen2` to `gen7` | home consoles by generation: generations 1 and 2 on `gen2`, 7 to 9 on `gen7` |
| 7 | `handheld` | handhelds of every generation |
| 8 | `computer` | computers, then the kind `other` (engines, ports, PC shortcuts, fantasy consoles, mobile) |

- A floor exists in a plan when it holds a room. The ground floor always exists.
- Floor names shown to the user live in the RML documents (Italian). Signs in the 3D scene use generic texts from the
  data, never a trade name.

### 2. Systems: classification data

`assets/world/systems.json` classifies every system of ES-DE 3.4.1's `es_systems.xml`:
- `kind`: `home`, `handheld`, `arcade`, `computer`, `other` or `excluded`;
- `generation` 1 to 9, for home consoles and handhelds;
- `family`, the media family (§4);
- `order`, the entry order (§7);
- `sign`, a generic text.

Its `defaults` per kind give the theme pack and the console prefab (none for `arcade`, `computer` and `other`).

- **Unknown systems.** A system the table does not name (a user's custom system) takes the entry of the first known
  system sharing one of its ES-DE platform tags. Failing that, it is `other` with the family `generic_case`.
- **Excluded systems.** `excluded` systems (`desktop`, `emulators`, `kodi`) get no room, and their games stay in the
  quick catalog.
- **Platform manifests.** `assets/world/platforms/<system>.json` becomes an optional override (plastic colours,
  console, sign). `snes.json` stays as it is.

### 3. Which room a system gets (sticky)

- **Placement.** ADR-0010 §3 is unchanged: a room places one item per distinct title among the games with a cover, or
  every game when the system has no cover at all. A system with no placeable title gets no room yet (style §21).
- **Own room or shared room.** When a system first enters the plan, it gets its **own console room** if it has at
  least `sharedRoomMinTitles` (preset and `world.json` key, default 30) **distinct titles over all its eligible games,
  with or without a cover**. This is `selectRoom` under `include: all`, so the count measures the collection, not the
  scraping. Otherwise it gets a **section of its floor's shared room**. On the reference library this gives:
  - own rooms to n64 (860), vic20 (885), pcengine (732), saturn (230), amigacd32 (179), ngpc (157) and dos (56); n64's
    room starts with 2 cartridges and fills in place as covers arrive;
  - sections of shared rooms to wii, ps3, psp, pokemini, pcfx, pc, channelf, segacd, windows and neogeo.
- **Arcade.** Every `arcade` system gets a section of the arcade game room.
- **Sticky.** The plan records each system's floor, room and section order, together with the room's family and theme
  pack. They change only through an explicit regeneration (I4b): neither the system's growth nor a change of the data
  (kind, generation, family, theme) moves them.

### 4. Media families: data

A family is `media_families.json`'s entry (ADR-0010 §2). In schema 1 of that file it gains:

- **`item`**: its own item manifest, the `cartridge` prefab at the family's size with explicit chamfer, label margin,
  label inset and spine label. Its parameters span cases and boxes (2 to 50 cm, 0.4 to 20 cm deep). A unit test builds
  every family's item.
- **`fixtures`**: optional manifests for the hero module, the side module, the display case and the featured wall
  shelf at its slot size, hand-computed, each kind keeping its own gaps, margins, boards and levels.
  - Absent, the template's refs serve. This is the case of `cartridge_home_16bit`, so the SNES room's fixtures, plans
    and goldens do not change.
  - A unit test builds every family's fixtures and checks that their levels face the family exactly.

The families added in I4 are functional, and the rare formats map to the nearest one:

| Family | Media |
|---|---|
| `cartridge_home_8bit` | 2nd and 3rd generation home cartridges |
| `cartridge_home_64bit` | 5th generation cartridges |
| `cartridge_handheld` | handheld cartridges |
| `case_handheld` | DS, 3DS, PSP, Vita, Switch |
| `disc_case_cd` | CD jewel cases |
| `disc_case_dvd` | DVD and Blu-ray keep cases |
| `big_box` | 16- and 32-bit computers |
| `generic_case` | everything else, the shared rooms and the arcade room |

- **Ids are permanent.** A family id is never removed or redefined; a new definition takes a new id. Template
  versions likewise: a changed template is a new version file, and the old one stays while a plan may name it.
- **Spine-out stays as I3 has it**, the item on its end with its top face out (ADR-0012 §3). Portrait cases therefore
  stand with their height in the shelf's depth until a side-face facing exists, which is the styling pass's. The
  capacity per family follows from its fixtures. Step 2 measures it on the reference library.
- **Covers.** A cover is fitted to its family's **label quad** (the item's front less its label margin), with ADR-0011
  §5's rules, then resampled once into the unchanged 256 x 188 front layer. The same holds for the generated front
  labels and the hero pictures.
  - `cartridge_home_16bit` keeps today's fit as a stated exception, so no SNES pixel moves.
  - The front cache keys gain the family id. `kCoverPipelineVersion` is not raised, so warm caches stay warm.
  - Spines stay 40 x 256.

### 5. The plan: schema 2

The plan stays one canonical file per library (ADR-0010 §8, §9). Schema 2 adds:

- **`floors`**: id and order.
- **Per room:**
  - `kind`: `console`, `shared`, `arcade`;
  - `floor`;
  - `sections`, its systems in order (one for a console room);
  - its **placement** on the floor: x and z in millimetres, yaw in half turns (0 or 1);
  - its `generatorVersion`; the plan-level one is the highest;
  - `expands`, reserved for expansion rooms (§8).
- **No halls in the plan.** The halls and the lobby are not stored. They are derived from the floor's rooms when a
  floor's scene is built, and validated on that derived geometry (§6, §12).
- **Ids.**
  - A console room keeps `room:<system>`, so the SNES room's id, seed and bytes do not change.
  - An expansion is `room:<system>:<n>`, unambiguous because `:` is not allowed in a system name.
  - The shared rooms are `area:shared:<floor>` and the arcade room is `area:arcade`.
  - Seeds derive from the id's text as today. `scripts/world_seed_oracle.py --check` covers the new shapes.
- **The lift.** Reading schema 1 is a pure function, `lift(v1 plan, building.json, systems.json)`, that gives a
  complete, valid schema-2 building:
  - the v1 room becomes a `console` room of its system, on its floor, at the north row's first position;
  - the ground floor exists;
  - nothing else is added: the building update adds the other systems.
  - **The invariant**, checked against frozen v1 oracles (`tests/fixtures/world/v1/`, never rewritten): after the lift
    and a 1.3 update with the same selection, these equal the v1 file's:
    - the room's id, template and version, bounds, height, growth steps, seed, media family and theme pack;
    - every fixture, placement, dressing, light and unplaced element.
  - A plan is written in schema 2 only.
  - ADR-0010 §10's "same schemaVersion" reads "a schema this program reads (1, lifted; 2)". The digest "before" an
    update is always the stored file's SHA-256.
- **Generator 1.3** is a minor version because every update keeps every stored placement, ADR-0010 §10's test. Fresh
  generation does change for systems under the shared threshold. The room-level generation API stays beneath the
  building, so the I3 room suites keep testing a console room of any size. The tests of one-ROM rooms (G06) set
  `sharedRoomMinTitles` to 0 in their `world.json`.
- **Digests.**
  - One plan-level `inputDigest` stays, as provenance only. It hashes the configuration, the generator version, the
    library, the seed, `building.json`, the plan's `systems.json` rows and platform tags, and per room its selections,
    template, family, platform fields and measured prefabs.
  - Plan equality is the file's SHA-256.
  - A room's **content digest**, the SHA-256 of its canonical sub-document (its record and every element of it), is
    computed, never stored, and serves the app's per-room reconcile (§9).
- **Compatibility per room.** The world-level checks stay global: seed, preset, the generator's major version, the
  random algorithm and the title-key rule. A room is **kept**, its contents untouched, when:
  - its template version is no longer the asset root's current one;
  - its fixtures' levels are no longer their prefabs';
  - its generator major is another.

  The recorded family and theme pack are authoritative, so a data change never freezes a room (§3). A kept room's games
  that it does not hold are `unplaced` with the new reason **`room-kept`**. The building still saves; the outcome is
  Updated with a list of `{room, reason}`.

### 6. Rooms and halls

- **Collection rooms.** Console rooms, shared rooms and the arcade room use `console_room_9x10` version 2, whose file
  stays byte-identical in I4 (a test pins its version and sockets).
  - **Shared and arcade rooms.** The kinds `shared` and `arcade` skip the hero corner's `console` role, which is a
    code rule by kind; validation requires no console there. Their family is `generic_case`.
  - **Order.** A shared or arcade room orders its collection by (recorded section index, sort key, title key, GameId).
    A new system's first title therefore follows the previous section's last. Systems mixing at a section's edges is
    accepted.
  - **The arcade room** keeps the template's CRT. A cabinet prefab is the styling pass's (or I6's).
- **The hall of a floor** is derived at scene build from its rooms:
  - **Geometry.** A corridor 3.0 m wide and 3.2 m high, of box primitives that collide by their bounds, running east
    from the elevator at its west end. It has an opening onto each room's doorway, a sign by the elevator, and lights
    at a fixed 6 m pitch from the west end, so that a longer hall appends lights and moves none.
  - **The lobby.** The ground floor's hall is 6 m wide. It holds the elevator, a sign and the arcade room's doorway.
    The east end has closed entrance doors (decoration).
  - **The elevator** is a stand-in: a door frame and a call panel of primitives.
- **Doorways.** A room's doorway stop is dropped on a floor (`DoorwayParams::stop`, default true). The standalone
  `worldplan show --scene` room keeps it.
- **Object ids.** Objects of a floor scene are named with the room's id as prefix.

### 7. Floor layout

- **Rows.** Rooms stand in two rows along the hall: the north row with its doorways facing south, the south row turned
  by half a turn.
- **Entry order.** Several systems entering the plan at once take places in the order (floor order, systems.json
  `order`, system name).
- **x, fixed at entry.** A room's x is set when it enters the plan: at the current east end of the shorter row,
  measured in millimetres, north on a tie. It never changes.
- **z, follows depth.** A room's z follows its current depth, so that its doorway wall always abuts the hall. When a
  room grows it moves away from the hall as a whole; its contents keep their place in its own frame.
- **A full floor.** A floor holds at most 12 rooms per row. A new system on a full floor joins that floor's shared
  room if it has one. Otherwise its games are `unplaced` with reason `capacity`, naming the room they would have had.
- **Frames.**
  - rv_world offers each room's floor transform. The floor-scene builder and the collection layouts emit floor-frame
    data.
  - `--spawn`, input-script poses and the run report's walker poses are in the frame of `--start-room`'s room when it
    is given, else in the floor's frame. The report adds each resident room's placement.
  - A growth, a live update or a launch's return keeps the walker, and a Focus or Inspect pose, **in the frame of the
    room that holds it**, moved with that room. Items are compared, and `moved` counted, in their room's frame.

### 8. Expansion rooms

- **Schema 2 reserves them**: the ids `room:<system>:<n>` and `area:...:<n>`, and the `expands` field.
- **They are implemented in I4 only if step 2's per-family capacity census shows a reference system that overflows a
  room grown to its limit.** Until then `unplaced: capacity` stays, and they move to I4b.
- **The rules, when built:**
  - **One fill order per chain.** A chain (the head room, then each expansion) has one fill order. The predecessor rule
    runs over the whole chain, so slots freed in the head are reused first.
  - **Fresh generation** splits a system alphabetically. Each room chooses its face-out titles among its own share.
  - **Updates.** An expansion created by an update has no face-out titles. The titles a plan holds as capacity-unplaced
    enter the chain as additions.

### 9. The app: one floor resident

**Flows.** The flow is split in two.

- A **building job** reads the catalog and plans the building, producing data only:
  - It reads every system's snapshot within one read transaction, on its own read-only connection, as a pool job, so
    that the catalog worker keeps answering lists.
  - It runs the selections, merged by system name, and the update.
  - It saves the plan.
  - It writes, only when changed, one items file per system (`cache/world/<library>/room-<system>.json`: cover paths,
    fallback titles, years, and the media paths the CRT needs). This is the only per-item source of a floor that is not
    resident.
- A **floor-build job**, keyed by (floor, plan digest), builds a floor from the plan and the items files without a
  catalog round trip. There is at most one; the latest destination wins.
- Every floor shown, whether at the start, after a ride or for a rebuild, comes from a floor-build job.
- A building result that arrives while another floor is resident reconciles that floor room by room, by content
  digest.

**Residency.**
- Resident: the floor's static scene (the hall and every room), and one `CollectionInstance` per collection room, with
  every `collection/...` registry key and picture set carrying the room id.
- One cover loader serves the floor. "The view" is the items of the area that holds the eye, plus those of rooms whose
  doorway is in the frustum; the rest follow by distance.
- Picking uses a pick index per room and queries the room that holds the eye.

**Areas.** An area is a room, extended through its doorway passage, or a floor's hall. "The area that holds the eye" is
the one it is in, else the last one held.
- **Lights.** Each frame the renderer packs the 16 lights of that area nearest the eye, with that area's key light
  shadowed. Other areas get the ambient light only, which also keeps light from leaking through walls.
- **Light limits.** Rooms keep ADR-0010's 16. The hall has no limit, since its lights are chosen by distance.

**The hero CRT** is the CRT of the room that holds the eye, with one decoder.
- **Rebinding.** The rebind never blocks the render loop: the clip stops as a clip replacement does, the previous
  screen's override clears, and the new CRT is bound with that room's games after ADR-0012's 300 ms dwell (also a
  hysteresis at doorways).
- **What it plays.** The shared room's CRT plays its sections' games. The quick catalog's selection plays only when the
  game is in the eye's room.

**Memory.**
- About 39 KiB of arrays per item (BC1) plus about 180 MiB of other textures: the busiest reference floor totals about
  390 MiB, within the acceptable 512 MiB. A floor beyond that, or the RGBA8 fallback on a large floor, is logged and
  accepted.
- The derived-texture cache's bound rises from 512 MiB to 2 GiB, about 2.5 times the reference building, and protects
  the resident floor's entries as today.

**Start.**
- The app starts in the lobby in front of the elevator.
- **Persisted start.** With a stored plan, the start floor is previewed at the first frame from the plan and the items
  files, then reconciled. A lifted schema-1 plan has a ground floor, so its lobby shows at once. While no plan exists
  the empty lobby shell is shown.
- **`--start-room <room id>`** starts at that room's doorway camera (the tests, the goldens, the benchmark path). A
  room the plan lacks exits 2.

### 10. The elevator and the directory

- **One panel, the directory** (RmlUi, pad-navigable).
  - It lists the plan's floors, and on each floor its rooms with their game counts.
  - It also offers a tab of favourites and recently played games (Retroverse's sessions and ES-DE's `lastplayed`,
    converted to UTC; one catalog-worker query). That tab uses the quick catalog's card with Avvia and "Mostra nella
    stanza".
- **Opening it.** In front of the elevator (a proximity zone with a prompt) interact opens it at the floors list. Anywhere
  in free roam, M or Select opens it. It has its own input context and Back level.
- **Choosing a room teleports the walker to its doorway**, riding first when the room is on another floor.
- **The ride.**
  1. The view fades out in 250 ms (instant under `reduceMotion`).
  2. The destination floor is built on the pool, and the resident floor is dropped only once it is ready. A failure
     leaves the walker where it was, with a notice.
  3. The floor is swapped at a frame boundary behind the fade.
  4. The walker stands at the destination's elevator, facing the hall.
  5. The view fades in when the eye's area is resident, or after 10 s; under lockstep the waits replace the timeout.
- **Restrictions.**
  - No ride starts in Focus, Inspect, a launch or while a floor applies. "Mostra nella stanza" leaves Focus first.
  - A building result or a refresh that ends mid-ride applies after it, as during a game.
  - Input is suspended during the ride.
- **Run report.** The phase `elevator`, and the records `world.floors` and `world.rides`.
- **Same floor.** "Mostra nella stanza" and a room chosen on the same floor teleport to the room's doorway, then ease to
  the Focus pose.

### 11. Validation (adds to ADR-0010 §11)

The building rules hold on the plan and the derived halls:
- the rooms of a floor overlap neither one another nor the hall;
- every doorway opens onto its hall;
- every room is reachable from the elevator on the floor's navigation grid;
- every placeable game is placed or unplaced exactly once in the building;
- every system is in one room or one section;
- each room has at most 16 lights;
- the ground floor exists.

The per-room rules apply to collection rooms, except the console rule for the `shared` and `arcade` kinds.

### 12. Command line

- **`worldplan generate | update`** plan the whole building.
  - `--system <name>` (repeatable) restricts the building to those systems, only with `--plan <file>` or when no plan
    is stored. These are the tests' small buildings.
  - `--replace` regenerates the whole building after a backup. Per-room regeneration is I4b's.
- **`worldplan show [--floor <id>] [--scene <file>]`** writes one floor's static part, halls included.
- **`where-is`** names the floor.

### 13. Acceptable performance (added to the roadmap's table)

- **A ride** fades in within 3 s warm and 10 s cold, the frames behind the fade exempt.
- **The room that holds the walker** is resident within 30 s cold and 3 s warm.
- **The rest of a floor's covers** are informative.

## Amendments to the documents

- **Specification §7.3, §21.3.** "Every door connected to the hub" reads "every room reachable from its floor's
  elevator, and every floor from the ground floor".
- **Specification §7.4.** One room per console. The systems under 30 distinct titles share their floor's room, and the
  arcade systems share the arcade game room.
- **Specification §13.2.** The world starts in the lobby, not in the last room visited.
- **Specification §14.4.** The streaming unit is the floor.
- **Style §5.2.** The hub is the ground floor's lobby: the elevator, the directory and the arcade room. Its golden is
  the lobby.
- **Style §5.6.** The size bands become: a section of the shared room under 30 titles, else a room.
- **Roadmap I6.** The faithful families, side-face spines and the arcade cabinets stay I6's. I4 brings functional
  families forward.

## Consequences

**Gains**
- Every system of a library gets a place in a world that can be walked, and the busiest reference floor fits the
  acceptable memory.
- A stored SNES room survives: it is lifted, keeps its id, seed, template and placements, and has its floor built
  around it.
- A new system or floor appears without moving anything, and a floor visited again loads warm.

**Costs**
- The plan goldens are rewritten in each step that changes the format or the building. Each rewrite is checked against
  the frozen v1 oracles.
- Every GPU world test names its room with `--start-room`.
- A new runtime state (the ride), the flow split in two, and per-area light packing.
- Seen from the hall, a room is lit by the ambient light only, and the hall likewise from a room, until the styling
  pass.
- Cases and boxes stand with their height in the shelf's depth.

**Deferred**
- **I4b:** the editor and pins written to `overrides.json`; "Rigenera stanza / piano / mondo" with an impact preview,
  backup and undo (the only way to move a system between rooms or floors); expansion rooms if no reference system
  needs them in I4.
- **The styling pass:** theme packs per floor, the lobby's 3D displays, the arcade cabinet, stairs, side-face spines,
  L-shaped templates and layout candidates, light seen through doorways, a background warm-up of the whole building's
  covers.
- **I5:** video on more screens.
