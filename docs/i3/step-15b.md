# I3 step 15b: live updates of the room after a refresh

Roadmap row: I3 step 15b (wave 4, after 9, 10c, 12 and 13r; step 13a runs beside it and touches the app's world wiring:
this step keeps to new files and the hooks listed below). Decisions:
[ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §10 (the incremental rules: every placement kept, a
new title into the spine-out section, `NoRebuild`/`LocalReflow`/`RoomExpansion`), §11 (a plan that cannot be shown
never replaces the one shown), §12 ("after an import (step 13r) it does the same": the persisted plan reconciled with
the snapshot, the difference applied); [ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md) §6
(layers keyed by item, the arrays grown by a GPU copy); [ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §6
(a refresh during a game: the world applies its result after the return). After a refresh of the catalog ends (`done`:
"Aggiorna libreria" or `--refresh-library`), the room's job runs again while the room shows (the snapshot, `world::update`,
the validation, the plan saved under the world's lock) and its result is applied at a frame boundary with as little
rebuilt as the change needs; new titles appear in their slots with their covers made by the cover loader, nothing else
moves. Golden G01 (`--scene m0_testroom`) is unchanged.

## What was built

| File | What |
|---|---|
| `rv/app/world_live_plan.hpp`, `src/world_live_plan.cpp` (`rv_app_catalog`, new, GPU-free) | `ShownRoom` (what the session keeps of the room shown), `shownRoomOf()`, `LiveUpdateKind`, `LiveUpdatePlan`, `planLiveUpdate()`: the decision below |
| `src/world_live_update.hpp`, `.cpp` (`rv_app`, step 0a's stubs) | `WorldSession::requestLiveUpdate`, `startLiveFlow`, `applyLive`, `applyItems`, `keepShown`, `releaseHero` |
| `src/collection_instance.hpp`, `.cpp` | `applyLayout()` (the items applied in place), `CollectionUpdate`; `takePictures()` and `create(..., pictures)` (`CollectionPictures`: a rebuilt room keeps the arrays and the pictures in them); the item loop moved into `setItems()`, the meshes and materials kept as members |
| `src/cover_loader.hpp`, `.cpp` | `adoptShown()` matches the items by game (was: by index) |
| `src/hero_textures.hpp`, `.cpp` | `forget()`: a hero texture dropped when its cover changed (a picture on its way discarded) |
| `rv/app/world_build.hpp`, `src/world_build.cpp` | `WorldBuild::sceneDigest` (SHA-256 of the static part's scene file); `WorldFlow::Options::preview` (false: the snapshot at once) |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `WorldLiveUpdateRecord`, `WorldRecord::liveUpdates` (`world.liveUpdates`, optional when read); `reconcile.kind` may be `items` |
| tests | `tests/unit/app/test_world_live_plan.cpp` (new), `test_run_report.cpp` (extended); `tests/smoke/test_world_live_update.cpp` (new, `[world_live]`) |

## The decision (`planLiveUpdate`)

The build of the room's job compared with the room shown (`ShownRoom`: the plan's digest, the scene file's digest, the
collection's layout, the cover requests, whether the media root was known, the hero CRT's games):

| The build | `kind` | What happens |
|---|---|---|
| the same plan, collection, scene and cover requests | `unchanged` | nothing (the plan was not written: `Unchanged`) |
| the same plan, collection and scene; other cover requests (a cover added, replaced, gone; a title), or the media root known now | `textures` | ADR-0010's `NoRebuild`: a new cover loader adopts every item (only the changed pictures are uploaded again), the hero textures of the changed covers forgotten; when only the media root became known, the loader stays (step 12's retry after the refresh serves the items again) |
| the same static part (scene digest) and media family; items added, freed or with other flags (a game unavailable now keeps its slot, dimmed) | `items` | `CollectionInstance::applyLayout`: the items that stood keep their pick ids, transforms and resident layers; the new ones acquire layers (the arrays grow by a GPU copy past their headroom, an added array gets its layered material); only the collection's static group is made again; the pick index rebuilt; a new loader adopts the items that stood; the scene, the walker and the CRT untouched |
| another static part (a dressed level taken, a side-wall module, a growth), another family, or an item elsewhere | `rebuild` | the room committed again with the walker where it stands (10c's `showSceneKeepingWalker`), **its arrays and pictures kept** (`takePictures`) and the loader's work adopted: only the new or changed pictures go up |
| no plan, no scene (the stored plan refused under the lock, an error), or the test room | `kept` | the room shown stays (logged; ADR-0010 §11) |

An item never moves in `items`: the incremental rules never give one, and a build that would is a `rebuild`. The start's
reconciliation (10c) uses the same decision for its `rebuild` case, so a catalog changed between two runs is applied in
place as well (`reconcile.kind` `items`).

## Flow

- `followRefresh()` (step 12's hook) sees a refresh end `done` and asks for a live update (`requestLiveUpdate`). Once no
  flow runs and the persisted start's reconciliation is done, `startLiveFlow` starts a `WorldFlow` without its preview:
  the snapshot on the catalog worker, `buildWorld` on the pool, the plan saved when it changed.
- Its result is taken at a frame boundary (`applyLive`), never while a launched game runs (ADR-0012 §6: applied after
  the return). Lockstep runs wait for it like any flow (the refresh itself is not waited for, as in 13r).
- The room shown is kept as a `ShownRoom` by `showRoom` and the reconciliation, so each later update compares with what
  the screen shows.
- A refresh while the shell or the test room shows (the media root came back, the first snes games) commits the room as
  at the start.

## Tests and results (Debug)

- Unit (`[world_live]`, 6 cases, 121 assertions): every kind on hand-made rooms; on the snes-room fixture imported: an
  unchanged refresh is `unchanged` (nothing written); the first title added takes a dressed level (`dressingRemoved` 1,
  another scene digest: `rebuild`, 50 kept, 0 moved); the next one takes a free slot of that level (the same scene
  digest: `items`, 51 kept, 1 added); a ROM deleted keeps every placement byte for byte (`items`, 1 with other flags).
  `[run_report]` reads `liveUpdates` back field for field, and a report without the key as none.
- GPU smoke (`[world_live]`, 2 cases, 490 assertions):
  - a ROM added and `--refresh-library`: the refresh imports it before the room's snapshot (the room's job waits behind it
    on the catalog worker), the reconciliation places exactly one new cartridge (`rebuild`: its level was dressed),
    kept 50, moved 0, the plan saved with the 50 earlier placements byte-identical and their items at the same pick ids
    and transforms; the 50 pictures adopted, none uploaded again; the refresh's own live update `unchanged`;
  - a ROM added while the room shows, "Aggiorna libreria" pressed by an input script: the refresh ends at frame 22, the
    live update applied at frame 23 (`items`, 6.2 ms on the main thread in Debug), 1 added, 51 kept, 0 moved, the plan
    written (52 placements, the 51 earlier byte-identical, their items unmoved), 52 fronts and spines resident, 51
    adopted with 0 changed, 2 cache entries stored (the new front and spine); the final screenshot equal to a fresh
    start's of the same catalog outside the hero CRT, the walker's pose equal.
- Golden G01 (`gpu.golden/G01`) passes. The `[world]` smoke cases (16 without the hidden `[.bench]` one) pass in Debug
  but for step 12's "covers: the snes-stress room ...", whose unpaced Debug runs of 2,000 titles missed their timing
  (the room still generating after 900 frames, or 31 of 40 fronts arrived) while two other worktrees ran their GPU
  suites on the machine; the case makes no refresh, so no live update runs in it. Step 10c's corrupt-clip CRT case
  failed once under the same load and passed when run again; the first run also failed step 12's unplugged-media
  case (the retry's counters), fixed by decision 5.

## Decisions taken within the step

1. **The static part is compared by the scene file's digest**, not by the change report: a new placement alone is
   `LocalReflow` in step 9's report, but only dressing, fixtures and the room's size change the scene file (it names no
   placement), so the digest says exactly when the scene can stay.
2. **`items` rebuilds the collection's static group, not its records one by one**: the group is pre-sorted and culled by
   clusters whose bounds grow with their items; rebuilding it from the layout (resident layers named from the start)
   costs a few milliseconds and keeps one code path with the first commit.
3. **A rebuilt room keeps its pictures**: the arrays pass to the new collection (same family and format), the layers of
   the games gone released, and the new loader adopts the old one's finished items by game. 10c's open issue (a rebuild
   at start shows the covers pending again) is closed with it.
4. **Every refresh that ends `done` runs the room's job again**, even when the start's snapshot may already have followed
   it (an `unchanged` update costs a snapshot and a job, writes nothing, changes nothing on screen): simpler than
   tracking which snapshot saw which commit.
5. **Step 12's retry stays the media root's path back**: a `textures` update whose cover requests are unchanged (the
   media root reachable again) keeps the loader that `followRefresh()` already asked to retry, so its records
   (`retryRounds`, `retriedItems`) stay those of step 12's smoke case.
6. **A live update never takes the room away**: a build with nothing to show leaves the room as it is (logged), unlike
   the start, which shows the shell.

## Open issues

- Not measured here (no Release run in this step): the roadmap's frame p99 over the `refresh` phase in Release, and the
  10-game import (G10's) while the app runs; 17a's G10 and the gate measure them.
- The arrays' growth past their headroom (64-layer rounding) and an array added (past 2,048 items) are covered by step
  6's unit tests and the code path, not by a smoke case (the fixture has 50 to 52 titles).
- A side-wall module added or a growth step commits the whole room again (pictures kept, walker kept); rebuilding only
  the scene's objects while keeping the collection would need the scene host to keep the collection's registry entries.

## For the merge

- **Hooks in shared files** (13a's rebase): `src/world_session.cpp`: `update()` takes a live flow's result with
  `applyLive()` (`m_liveFlow`; held while `product.launchActive()`) and calls `startLiveFlow(product)` after
  `followRefresh()`; `followRefresh()` calls `requestLiveUpdate()` on a `done` refresh; `showRoom()` takes the shown
  collection's pictures and loader when the walker is kept, and ends with `keepShown(build)`; `dropCollection()` resets
  `m_shown`; `countPlan` left the anonymous namespace (declared in `world_session.hpp`). `src/world_session.hpp`: the
  members and functions of the live update (after `setNotice`, after `m_previewAwaitingFrame`). `src/world_start.cpp`:
  the `rebuild` reconciliation tries `applyItems()` first; the `textures` one calls `keepShown()`.
  `releaseHero()` resets `m_heroShown`, `m_heroItem` and `m_dynamicItems`: if 13a renames them or adds pulled-out state,
  release that there too. `src/collection_instance.cpp`: `create()`'s item loop is now `setItems()` (13a's per-record
  flags belong there).
- **CLAUDE.md**, "Running the app": after "Aggiorna libreria" or `--refresh-library` the room follows the refreshed
  catalog live (the room's job again, the plan saved; new titles placed into their slots with their covers, nothing else
  moved; a dressed level, module or growth commits the room again with the walker and the pictures kept; never during a
  game); `--run-report`: `world.liveUpdates`, `reconcile.kind` `items`. GPU smoke tests: `gpu.world: ...` of
  `tests/smoke/test_world_live_update.cpp` (`[world_live]`). "Layout and layering": `rv_app_catalog`'s `world_live_plan`.
- **docs/architecture.md**: the app's world: the live update after a refresh (the flow without its preview, the decision,
  the collection applied in place).
- **docs/roadmap.md**: row 15b (edited here).
