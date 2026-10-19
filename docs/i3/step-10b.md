# I3 step 10b: the room in the app

Roadmap row: I3 step 10b (after the wave-2 merge: it integrates steps 2, 3, 5, 6, 7, 8, 9, 10a, 11 and 13r in the app,
alone; wave 3 starts from it). Decisions: [ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §3 (no
first plan from unknown media), §9 (the world's files), §10 (the incremental update), §11 (a plan that breaks a hard
rule is never saved or shown), §12 (where it runs, the commit at a frame boundary, the lockstep);
[ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md) §1 to §7 (the collection beside the scene,
the static groups, the arrays, the loader, the cache); [ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §2,
§3 (picking, the hover outline). Without `--scene`, `--model` or `--mock-catalog`, `retroverse.exe` now opens the
generated room of its ES-DE catalog's SNES games: the snapshot and the media paths on the catalog worker, the plan read,
generated (a first plan) or updated (the incremental rules) and saved on the worker pool, the result committed at a frame
boundary: the room's static part through `SceneDescription`/`SceneInstance`, its cartridges on a static instance group
with their pick ids, their covers and spines made through the derived-texture cache and uploaded into the collection's
arrays within the upload budget (the pending picture until each arrives), the walker at the room's doorway camera, the
hover outline on the item the reticle points at. While the room is made the empty room shows (the WorldGenerating
state) with a notice; without a catalog or without SNES games the test room shows, with its notice; a room that cannot
be made now (the media root out of reach before a first plan, a plan that cannot be used) leaves the empty room and its
notice. The test room stays available with `--scene` (golden G01 unchanged).

## What was built

| File | What it holds |
|---|---|
| `rv/app/room_snapshot.hpp`, `src/room_snapshot.cpp` (`rv_app_catalog`, was a stub) | `RoomSnapshotRequest`, `RoomSnapshotResult`, `takeRoomSnapshot()` (the catalog worker's job), `coverPathOf()` |
| `rv/app/catalog_worker.hpp`, `src/catalog_worker.cpp` | `CatalogWorker::roomSnapshot()`: a `RoomJob` (the latest request only, never served during a refresh), `CatalogResults::room` |
| `rv/app/app_catalog.hpp`, `src/app_catalog.cpp` | `AppCatalog`'s room part (`roomCatalogState`, `roomLibrary`, `hasGamesOf`, `requestRoomSnapshot`, `takeRoomSnapshot`, `workerEnded`), implemented by `EsdeAppCatalog`; every list's wait recorded (`CatalogRecord::lists`) |
| `rv/app/world_build.hpp`, `src/world_build.cpp` (`rv_app_catalog`, new) | `WorldPaths`, `readWorldPlan()` (the first job), `WorldBuildInput`, `WorldBuild`, `WorldOutcome`, `buildWorld()` (the second job), `WorldResult`, `WorldPhase`, `WorldFlowTimes`, `WorldFlow` (the main thread's side), `kRoomSystem` |
| `rv/app/collection_layers.hpp`, `src/collection_layers.cpp` (`rv_app_cli`, new) | `CoverItem`, `FrontSource`, `ItemLayers`, `textureFormatOf()`, `toLayerImage()`, `serveItemLayers()`: one item's front and spine through the cache |
| `rv/app/world_notice_ui.hpp` (`rv_app_cli`, new, header only) | the notice document's paths, ids, data model and keys |
| `src/world_session.hpp`, `src/world_session.cpp` (`rv_app`, were stubs) | `WorldSession`: the flow, the commit (the scene through the application's `SceneHost`, the collection, the pick index, the loader), the notice and its timing, the hover, the lockstep wait, the run report's records |
| `src/collection_instance.hpp`, `src/collection_instance.cpp` (`rv_app`, were stubs) | `CollectionInstance`: the family's meshes, the shell's material and one layered material per array and class, the arrays, every item's layers, the static group, the records written when a layer arrives, the hover's highlight |
| `src/cover_loader.hpp`, `src/cover_loader.cpp` (`rv_app`, step 12's stubs, a first form) | `CoverLoader`: the items' layers on the worker pool (the fonts loaded once, the cache), submitted and uploaded within the budget, the cache trimmed once the room's layers are in |
| `src/world_notice_panel.hpp`, `src/world_notice_panel.cpp` (`rv_app`, new) | `WorldNoticeContent`, `WorldNoticePanel`: the notice's RmlUi document, shown without focus or input |
| `assets/ui/world_notice.rml`, `.rcss` (new) | the notice's sentences (Italian), a band at the bottom of the view |
| `src/product_ui.hpp`, `.cpp` | `setWorldNotice()` (shown while neither the catalog nor the panel shows), `catalog()`, `launchActive()` |
| `src/retroverse_app.cpp` | room mode; the shell, or the test room at once without library.json or database; `startWorld()` (every procedural surface made, the pool, the session); `replaceScene()`; the world's update, hover, lockstep idle and records; `createScreen()` |
| `src/free_roam.hpp`, `.cpp` | `FreeRoam::resetScene()` |
| `rv/world/scene_builder.hpp`, `src/scene_builder.cpp` (step 10a's) | `shellSceneDescription()`: the template's empty room with its pendant |
| `rv/devui/panels.hpp`, `src/panel_lights.cpp` | `LightsPanelModel::lights` asked through a function (the scene can be replaced) |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `WorldRecord`, `ResidencyRecord` (filled), `CatalogRecord::lists` |
| `src/app/CMakeLists.txt` | `collection_layers.cpp` (`rv_app_cli`), `world_build.cpp` (`rv_app_catalog`), `world_notice_panel.cpp` (`rv_app`) |
| tests | `tests/unit/app/test_world_build.cpp`, `test_collection_layers.cpp`, `test_world_notice_document.cpp`, `test_world_real.cpp` (hidden), `test_run_report.cpp`, `catalog_test_support.hpp` (any fixture); `tests/unit/world/test_shell_scene.cpp`; `tests/unit/integration/test_room_walk.cpp`; `tests/smoke/test_world_room.cpp`, `smoke_support.cpp`; `test_catalog.cpp`, `test_library_refresh.cpp`, `test_launch.cpp` adapted; `tests/input_scripts/world_catalog_search.json` |

## Threading

```text
main thread (every frame, and every idle wake   worker pool (5 threads, below      catalog worker
of a lockstep run, after the catalog's poll)    normal: the world's jobs at the    (one thread)
                                                highest priority, the covers')
------------------------------------------      -------------------------------    ----------------------------
onInit: the empty room (shell) shows, every
procedural surface made, the pool started
the catalog opened Ready, with snes games -->   readWorldPlan: world-plan.json
                                                and its digest (no lock)
requestRoomSnapshot(the plan's placed games) <-
  ------------------------------------------------------------------------------>  takeRoomSnapshot: the bulk
                                                                                    query (reader), a fresh media
                                                                                    resolver's root check, the
                                                                                    media paths, the aliases of
                                                                                    placed games it lacks (store)
the snapshot taken (poll) ------------------->  buildWorld: the world lock, the
                                                plan read again under it, the
                                                selection, generate or update,
                                                the validation, savePlan, the
                                                scene data and toSceneFile, the
                                                collection's layout, the items'
                                                cover requests
takeResult -> WorldSession::commit (a frame
boundary): SceneInstance of the scene (the
registries cleared), FreeRoam reset to the
doorway camera, the hero CRT's service, the
CollectionInstance (arrays, materials, static
group), the pick index, the CoverLoader  ---->  serveItemLayers per item (fonts
                                                once, the derived-texture cache):
                                                the cover or the generated label,
                                                the spine -> LayerImages
every frame: the finished layers submitted <-
to the arrays, uploadPending (1 ms or 64
layers), each upload naming its layer in the
item's record; the hover's ray, the outline   once every layer is in: the cache
                                                trimmed (512 MiB, the room's
                                                entries kept)
```

- **The main thread never waits** but in lockstep runs (`--fixed-dt`): the host then idles before the first frame and
  between frames while the room or a layer is on its way (`WorldSession::waits()`), at most `kLockstepWaitLimit` (10 s)
  at a time, its idle wakes moving the flow, committing and uploading (8 ms or 512 layers a wake, no frame waiting for
  it); so the first frame of a lockstep run shows the room whole and its screenshots never catch a half-loaded shelf
  (ADR-0010 §12, ADR-0011 §6). Nothing is committed or uploaded while a launch starts or a game has the screen.
- **Jobs hold what they need**: inputs copied, results posted into a mailbox the flow or the loader shares
  (`std::shared_ptr`), the host woken (`AppContext::wakeCallback`). A flow or loader that goes cancels its jobs
  (`WorkerPool::cancel` of its `JobGeneration`); the pool is shut down at the end of the run (2 s bound) before the
  session, which goes before the graphics.
- **The catalog worker runs the database part only** (ADR-0010 §12): the room's job is queued like a list, behind a
  refresh and never during one (the aliases read the writer's store); the quick catalog's lists are answered meanwhile
  (the smoke case measures them).
- **The scene is replaced at a frame boundary** (`RetroverseApp::State::replaceScene`): the hero CRT's service stops,
  the registries are cleared, the `SceneInstance` is made again in the same storage (the overlay's panels keep
  referring to it; the Lights panel asks for the lights each frame), the walker is reset to the new scene's camera (or
  `--spawn`) and the camera takes its pose in the same frame, the CRT's service starts on the new scene's CRT, the debug
  lines are made again.

## What shows, and its notice

| Situation | Scene | Notice (`assets/ui/world_notice.rml`) | `world.state`, `outcome` |
|---|---|---|---|
| The room is being made | the empty room (`shellSceneDescription`: the template's base room, its surfaces, its pendant, the doorway camera) | `generating`, while it shows | `generating` |
| No library.json or no database (checked before the first frame) | the test room from the start | `no-catalog` (by the catalog's state), 12 s | `test-room` |
| The catalog did not open (data directory, mismatch, unavailable) | the shell, then the test room | `no-catalog`, 12 s | `test-room` |
| The catalog has no snes games | the shell, then the test room | `no-games`, 12 s | `test-room` |
| A first plan (generated, saved), an updated plan (saved), the plan unchanged (nothing written) | the room | none | `room`, `generated` / `updated` / `unchanged` |
| The media root out of reach, a plan stored | the room as it was, covers from the cache alone | `media-unknown-kept`, 12 s | `room`, `kept` |
| Another seed, preset, template or generator major version | the room as it was | `regeneration`, 12 s | `room`, `kept` |
| An update that breaks a hard rule or fails, the world's data unreadable | the room as it was (the findings and the error logged) | none | `room`, `kept` |
| A plan of a newer generator version | the room as it was, never written | `refused`, 12 s | `room`, `kept` |
| The media root out of reach, no plan | the shell | `media-unknown`, while it shows | `shell`, `no-plan` |
| The stored plan refused (edited by hand, newer) | the shell | `refused`, while it shows | `shell`, `no-plan` |
| A first plan that breaks a hard rule, an error | the shell | `failed`, while it shows | `shell`, `no-plan` |

The notice shows over the room in the window and in screenshots, never while the quick catalog or the game-session panel
shows, and takes no input. The test room by `--scene` shows no notice.

## The run report

- `world` (`{}` in runs that ask for no room): `state`, `notice`, `reason`, `catalogState`, `room`, `library`, `outcome`,
  `changeOutcome`, `rebuild`, `planDigest`, `planWritten`, `saveError`, `placements` {`total`, `featured`, `band`,
  `spine`}, `unplaced` {`total`, `duplicateTitle`, `noCover`, `unavailable`, `capacity`}, `added`, `kept`,
  `hardFindings`, `softFindings`, `objects`, `lights`, `undressed`, `items`, `clusters`, `skipped`, `staticRecords`,
  `pickTargets`, `times` {`snapshotAtMs` and `committedAtMs` from the process's creation, `snapshotMs` on the worker,
  `buildMs` on the pool, `commitMs` on the main thread, `snapshotToCommitMs`}, `firstFrame`, `hoverChanges`,
  `hoveredPickId`, `hoveredGame`.
- `residency` (`{}` without a room): `format`, `arrayBytes`, `items`, `frontsResident`, `spinesResident`, `pending`,
  `fronts` {`covers`, `cachedCovers`, `labels`, `coverFailures`, `failed`}, `cache` (assets::CacheStats), `times`
  {`shownAtMs`, `allResidentAtMs`, `residentAfterShownMs`}, `allResidentFrame`, `uploadRounds`, `maxLayersPerRound`.
- `catalog.lists`: every list the worker answered, from the request to the answer.
- The `coverLoad` phase is opened when the room is committed and closed when every layer is in.

## Tests

- `tests/unit/app/test_world_build.cpp` (`[app][world_build]`, on the snes-room fixture imported): the catalog
  worker's job (55 entries, 53 with media paths, 52 covers named `snes/covers/...`, the media root resolved, no alias for
  a game that is none, NotFound for a system the catalog lacks, the media unknown with the folder renamed and the paths
  derived all the same); a first plan generated and saved, equal to `world::generate` of the same selection (the CLI's
  `worldplan generate`), its scene, its 50 items and their cover requests in item order, then unchanged at a second
  start; unknown media: no first plan and no file, then a stored plan kept as it is with its covers asked of the cache
  alone; a plan edited by hand refused and never overwritten, another seed in world.json kept with the regeneration
  notice; the flow: the test room without a catalog (no-library) or without snes games, nothing asked; the stored plan
  read, the snapshot asked with its 50 placed games at the second start, the build taken once; a worker that ends before
  it answers.
- `tests/unit/app/test_collection_layers.cpp` (`[app][collection_layers]`): a cover through the cache in both formats
  (built, a hit with the same levels, served by its name without a media root, the spine's entry the same), a cover gone
  without an entry (the label), no cover and a cover that is no image (the same label), nothing generated without fonts;
  every layer valid for the arrays (`gl::validateLayerImage`).
- `tests/unit/app/test_world_notice_document.cpp`: the document's ids, data model, a sentence for every notice and catalog
  state.
- `tests/unit/app/test_run_report.cpp`: the world and residency records read back field for field, the optional values
  left out, a missing key and a bad count pointing at their keys.
- `tests/unit/world/test_shell_scene.cpp`: the shell is the room of a plan of the base size (its shell, pendant,
  environment and camera).
- `tests/unit/integration/test_room_walk.cpp` (the row's "a scripted walk reaches every shelf section and never enters a
  fixture"): in the rooms of the 50- and 890-title golden plans, the walker from the doorway camera along a path found on
  a 0.1 m grid of the floor to 0.9 m in front of each fixture holding items (the display cases, the wall shelf, the hero
  wall's and the side walls' modules), at 60 Hz with the collision on: every section reached, the reticle there picking
  an item of the section, the capsule never in a collider.
- `tests/smoke/test_world_room.cpp` (`gpu.world: ...`, label `gpu`, every run hidden), on the snes-room fixture imported
  with retroverse-cli:
  - **the room appears**: a lockstep run from the doorway (the plan generated and written, its digest the file's;
    44 featured, 6 band, 0 spine, 5 unplaced: 2 duplicates, 3 without a cover; 50 items, 150 records, 50 pick targets;
    the room in frame 0; 48 covers, 2 generated labels; every layer resident within 10 s of the room showing; one
    static group of 3 batches); a second lockstep run close to the hero wall's band (`--spawn` 0.9 m in front of its
    first item): the plan unchanged and not written, every layer from the cache, the band's fronts textured (luma spread
    at their projected centres), the reticle's item hovered and outlined (pixels exactly #F4EFE6 around it), no outline
    from the doorway;
  - **a ROM added between two runs** (one of G10's additions, its ROM and cover, imported again): `updated`, 1 added,
    50 kept, the new title spine-out, every earlier placement byte-identical and every item's pick id and transform
    unchanged (the layouts compared), only the new item's cover and spine made;
  - `--no-texture-compression`: the room in RGBA8;
  - **a media root out of reach** (renamed before the first run): no plan, no file, the shell with `media-unknown`;
  - **the budget**: a run without `--fixed-dt` after a first one: every layer resident, at most 64 a frame, the
    `layerUploads` median within 1 ms;
  - **lists while the room is made**: the quick catalog opened and searched from frame 1 without a lockstep, every list
    within 100 ms;
  - **the test room**: without library.json (from the start, `no-catalog`, `no-library`, nothing written) and with a
    catalog of other systems (`esde/multi-disc`: the shell, then the test room, `no-games`);
  - a hidden `[.bench]` case on `snes-stress` (2,000 titles), cold and warm, printing the figures below and checking the
    commit within 1 s of the snapshot, the planning within 0.3 ms (Release), the arrays within 80 MiB and 3 batches.
- `tests/unit/app/test_world_real.cpp` (hidden, `[.real-library][real-library][world]`): the real SNES room (below).
- Adapted: `gpu.catalog` and `gpu.refresh` run on the test room with `--scene m0_testroom` (the standard fixture has snes
  games, which would now open the room; the catalog's behaviour is the same in either); `gpu.launch`'s closed-catalog
  case passes `--scene m0_testroom` too (without it the ES-DE catalog's run asks for the room and shows the test room's
  notice). `requireCleanRun` accepts bound world and residency records (a state, no hard finding, the arrays' bytes
  within their kind's peak).

## Results

On the reference machine (RTX 2080 SUPER, driver 617.42), `RV_LAUNCH_BACKGROUND` set and `RV_LAUNCH_ALLOWLIST` unset:

- **Builds**: `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` with 0 warnings.
- **Unit tests**: `./scripts/test.ps1` passed 1,632 of 1,632 in Debug (682 s), wrapped in `python -I
  scripts/focus_watch.py --watch build --out build/focus/10b-unit.json`: 0 findings (no foreground change, no visible
  window of a test process).
- **GPU tests**: `./scripts/test.ps1 -Preset msvc-x64-debug-gpu` passed 1,683 of 1,683 (1,632 unit, 51 gpu: the 7
  `gpu.world` cases, the adapted `gpu.catalog`, `gpu.refresh` and `gpu.launch` cases, golden G01 among them; 1,224 s),
  wrapped in `python -I scripts/focus_watch.py --watch build --out build/focus/10b.json`: 0 findings.
- **Release**: the `gpu.world: ...` cases and the hidden `snes-stress` bench passed
  (`build/msvc-x64-release/bin/rv_smoke_tests.exe "[world]"`; figures below).
- **Golden G01** (`--scene m0_testroom`): 0 of 2,073,600 pixels apart in Debug and in Release (the golden's bytes).
- **The real library**: `./scripts/real-library.ps1 -EsdeHome ... -Filter '[real-library][world]'` (Release, ES-DE
  closed): the test passed, 0 differences among 542,120 entries before and after.
- After those runs the review added two failure-path guards (an empty scene in place of one that cannot be made; no
  loader without a cache root): the three presets rebuilt with 0 warnings, the `gpu.world` cases passed again in Debug
  and Release, and the world-build, collection-layer and run-report unit tests in Debug.

## Figures

The reference machine (RTX 2080 SUPER, driver 617.42), 2026-10-19, nothing else running but this worktree's builds
(informative: the gate measures on a quiet machine). Times of the room's runs from their run reports.

| | snes-room (50 titles), Debug | snes-room, Release | snes-stress (2,000), Release | the real SNES room (890), Release |
|---|---|---|---|---|
| The snapshot on the catalog worker (query, media root, paths, aliases) | 5.6 to 6.9 ms | 0.6 to 1.4 ms | 8.7 to 10.2 ms | 34 to 35 ms |
| The room's job on the pool, cold (a first plan: generate, validate, save, scene, layout) / warm (unchanged) | 299 to 320 / 268 to 276 ms | 24 to 27 / 17 to 19 ms | 91 / 67 to 71 ms | 156 / 107 ms |
| The commit on the main thread (the scene instance, the collection, the pick index) | 336 to 339 ms | 50 to 59 ms | 43 to 51 ms | 49 to 51 ms |
| From the snapshot's end to the commit (row 10b: within 1 s for 2,000 titles) | 610 to 660 ms | 69 to 85 ms | 114 to 140 ms | 159 to 205 ms |
| The room shown, from the process's creation (lockstep) | 3.2 to 3.5 s | 1.1 to 1.3 s | | 1.33 s |
| Every layer resident after the room showed, cold (ADR-0011 §9: 10 s for 890, 15 s for 2,000) | 0.65 s | 0.18 to 0.25 s | 8.2 s | 7.5 s |
| The same, warm (1.0 s for 890, 1.5 s for 2,000) | 21 to 69 ms | 6 to 20 ms | 0.26 to 0.38 s | 0.10 s |
| Collection arrays (BC1; ADR-0011 §9: 34 MiB for 890, 80 MiB at 2,048 layers) | 2.4 MiB | 2.4 MiB | 76.7 MiB | 33.6 MiB |
| Collection planning per frame (CPU section, row 10b: 0.3 ms) | p50 0.007 to 0.015 ms, p99 0.07 ms | p50 0.001 to 0.003 ms | p50 0.003 ms, p99 0.007 ms | |
| Uploads per frame without a lockstep (CPU section `layerUploads`, the budget 1 ms) | | p50 0.07 ms, max 0.09 ms (20 layers a frame, warm) | | |
| Quick catalog lists while the room is made | | 0.27 / 0.54 ms (2 lists) | | |

- The real room: 890 placements (44 featured, 12 band, 834 spine: ADR-0010's figures), 6,907 unplaced (645 duplicate
  titles, 6,262 without a cover), 32 scene objects, 8 lights, 35 clusters; cold, 890 covers made and 1,780 entries
  stored; warm, 1,780 hits; at the doorway (1920 x 1080, `--no-vsync`, 600 frames) frame p50 2.66 ms, p95 3.11 ms, p99
  3.30 ms (the max, 463 ms, the last frame's screenshot written), 49 draw calls of which 3 the static group's (2,731
  instances, 35 clusters none culled); `./scripts/real-library.ps1 -Filter '[real-library][world]'`: 0 differences among
  542,120 entries (the ES-DE home and install hashed, the ROM and media roots listed).
- The 2,000-title room: 2,000 items in 73 clusters, 6,000 records (480,000 bytes) in 3 batches, one array of 2,048 layers
  per class; frames p50 2.7 ms, p99 3.4 to 3.7 ms in the 300-frame bench (`--no-vsync`, 1920 x 1080).
- The commit's 50 ms in Release is the scene instance's meshes (the shelves, the display cases, the furniture); in Debug,
  before every procedural surface was made at the start, the commit took 1.1 s (the surfaces the room adds generated at
  the commit: decision 5).

## Screenshots

Under the build tree, not committed (they show the fixture's invented titles and the generated covers; the real room's
only the real library's own covers, under `build/msvc-x64-release/real-library/world/`):

- `build/msvc-x64-debug/smoke/world/room/doorway.png`: the snes-room fixture's room from the doorway: the hero corner
  (the CRT with its colour bars, the console, the rug, the armchair, the lamp), the sign, the two hero-wall modules with
  the band and their dressed levels, the featured wall shelf, the two display cases with their face-out cartridges, the
  posters. Reviewed: every cartridge stands in its slot, the covers read as pictures, nothing floats or intersects.
- `build/msvc-x64-debug/smoke/world/room/shelf.png`: 0.9 m from the hero-wall module's band: six face-out cartridges
  with their covers and titles (the generated label of the long title shows its two lines and ellipsis), the first one
  outlined in ivory, the dressing (magazine stacks, storage boxes) on the other levels.
- `build/msvc-x64-debug/smoke/world/media_unknown/shell.png`: the empty room under its pendant with the
  `media-unknown` notice.
- `build/screenshots/10b/stress_doorway.png`, `stress_spines.png` (Release, the `snes-stress` room of the bench, its
  media folder gone after the test: the plan kept, every cover from the cache, the `media-unknown-kept` notice): five
  side-wall modules besides the hero wall's, dense spine-out rows, the spines' titles legible at 1 m.

## Decisions taken within the step

1. **The empty room while the room is made, the test room at once without a catalog.** The scene is decided before the
   first frame only when it can be without the worker: without library.json or the catalog's database the test room
   shows from the start (no shell flashing first); otherwise the empty room of the template (`shellSceneDescription`,
   GPU-free, the room of a plan holding nothing but its pendant) shows until the flow ends, and is replaced at a frame
   boundary by the room, by the test room, or stays with its notice. Runs with `--scene` never ask for a room.
2. **The test room is the fallback when there is no room to make, the shell when there is one that cannot be shown.**
   No catalog or no snes games: the test room (the prompt's "fallback when no catalog ... can be made"); the media root
   out of reach before a first plan, a refused plan, a first plan that breaks a hard rule: the empty room and the quick
   catalog, with the notice saying why and what to do (ADR-0010 §3 and §11: "the empty room shell and the quick
   catalog").
3. **The stored plan is read first, then the snapshot asked with its placed games**, so that the worker can ask the
   catalog for the aliases of the placed games its snapshot lacks (ADR-0008 §4, step 9's `UpdateInput::aliases`) in the
   same job; the room's job reads the plan again under the world lock before it decides anything (a writer reads what
   it replaces under the lock). Step 10c shows that first read's room before the snapshot answers.
4. **A first plan is generated only from known media; a stored plan is kept as it is while they are unknown**, its
   covers from the cache alone (no read under a media root known to be out of reach, so an unplugged network drive never
   stalls a worker). A plan that cannot be saved (the world lock held by another program) is shown all the same and
   saved at the next start.
5. **Every procedural surface is made at the start of a run that asks for a room** (as the test room's start makes
   them), so that the commit builds and uploads meshes only: 339 ms on the main thread in Debug instead of 1.1 s, 50 ms
   in Release.
6. **The arrays are sized to the placements** (rounded up to 64 by `planArrayLayers`), not to every slot of the room:
   2,000 items take one array of 2,048 layers per class (76.7 MiB, ADR-0011 §9's 80 MiB); sizing them to every slot
   (2,368 layers on `snes-stress`) took 86 MiB and a second array per class. A placement added later takes a layer of
   the headroom or grows the arrays by a GPU copy (`CollectionArrays::reserve`, step 15b).
7. **A record names its layer once the layer is uploaded** (ADR-0011 §6: layer 0, the pending picture, until then): one
   16-byte write of the record that shows it (the front's or the spine's), so that a released layer reused by another
   key can never show a stale picture.
8. **The cover loader's first form serves the items in the layout's order**, a bounded number of jobs at a time (twice
   the pool's threads, and no new job while 256 made layers wait for their upload); step 12 orders them by visibility and
   distance, adds the hero textures, the pause for launches and the residency's view-based lockstep.
9. **The lockstep waits for every item of the room** (not only those in view, step 12's rule) before the first frame and
   between frames, uploading in its idle wakes with a larger budget (8 ms or 512 layers); the layers uploaded while the
   host waits count towards the next frame's `layerUploads` section (`core::CpuTimers`' rule), so the per-frame budget is
   measured on runs without `--fixed-dt` (the smoke case's).
10. **The hover uses the reticle only** (`Camera::rayFromNdc({0, 0})`) while the world has the input, through
    `world::PickIndex::sample` and `world::HoverTracker`, and flags the hovered item's three records highlighted; the
    rest of the interaction (the cursor with the overlay open, the pull-out, Focus) is step 13a's.
11. **The notice is an RmlUi document of the product UI** (`world_notice.rml`, its sentences in the document, the code
    naming keys), drawn over the scene like the quick catalog and never taking the focus; it is hidden while the
    catalog or the game-session panel shows, and over a usable room (the test room, a kept plan) after 12 s of the
    run's time.
12. **The Lights panel asks for the lights each frame**: the overlay's panels outlive the scene instance, which the
    app now replaces; the other panels' references stay valid (the new instance takes the old one's place in the same
    `std::optional`).
13. **`CatalogRecord::lists`** records every list's wait (from the request to the answer), so that "lists answered
    within 100 ms while the room generates" is measured on any run.

## Open issues

- The hero CRT of the room has no games yet (the ES-DE catalog gives the screen service none until step 10c): it shows
  its colour bars.
- After an import whose `ImportStatistics::mediaUnknown` was not 0 the stored presence may be stale (step 2's note): the
  database does not record it, so a later start cannot tell; the app's in-process refresh (15b) can carry it.
- The commit builds the static part's meshes on the main thread (50 ms in Release for the reference rooms, a frame's
  hitch); `scene::buildSceneGeometry` could run on the pool if a later room needs it (the material library's
  missing-name log would need a lock).

## Notes for later steps

- **10c**: `WorldFlow` already reads the stored plan first (`readWorldPlan`, phase `ReadingPlan`): show it at that
  point (a `buildWorld` without a snapshot: the plan as stored, its scene, its layout, the covers from the cache) and
  reconcile when the snapshot's build arrives (an `Unchanged` outcome moves nothing: commit only the textures). Give the
  room's CRT its games from `WorldBuild::covers`' entries and the snapshot's media paths (`RoomSnapshotResult`); the room
  CRT is `fx:hero.crt` (`findHeroScreen` finds it).
- **12**: the loader is `src/cover_loader.cpp`: replace the layout's order with `cover_requests` (visibility, distance),
  narrow the lockstep to the items in view, add the pause hook for launches (no job and no upload while a game runs:
  `ProductUi::launchActive()`), the hero textures and the Stats panel's residency; `ResidencyRecord` and the
  `coverLoad` phase exist.
- **13a**: `WorldSession::hover` is the reticle's hover; the outline already follows `CollectionInstance::setHovered`.
  Replace it with `WorldInteraction` (its `InteractionRoom` over the layout: `layout.find`, `layout.findGame`, the
  sections from the items) and the cursor's ray while the overlay frees the mouse; the pulled-out item's records take
  `kHidden` (`CollectionInstance` keeps every record's attributes).
- **15b**: the session commits a whole room; an update's change report (`WorldBuild::changes`) says what to rebuild;
  `CollectionArrays::reserve` and new layered materials for a grown array; the shell → room swap is
  `RetroverseApp::State::replaceScene`.
- **17b**: the hidden `gpu.world: the figures of the snes-stress room` prints the room's figures; the benchmark path can
  start from it.

## For the merge

- **CLAUDE.md**:
  - "Running the app": without `--scene`, `--model` or `--mock-catalog`, `retroverse.exe` opens the generated room of
    its ES-DE catalog's snes games (the plan under `<user data>/worlds/<library id>/`, generated or updated and saved;
    the covers and spines through the derived-texture cache under `<user data>/cache/textures/<library id>`); the empty
    room while it is made; the test room with a notice when there is no catalog (no library.json or database) or no snes
    games; the empty room with a notice when the media root is out of reach before a first plan or the plan cannot be
    used. `--scene m0_testroom` shows the test room as before (golden G01's command has it). The walker starts at the
    room's doorway camera (or `--spawn`); the item the reticle points at within 2.5 m is outlined. Lockstep runs
    (`--fixed-dt`) wait before the first frame and between frames for the room and every layer (10 s at a time).
  - "Running the app", `--run-report`: the `world` and `residency` sections (above) and `catalog.lists`.
  - "Visual verification": the room's screenshots: `build/msvc-x64-debug/bin/retroverse.exe --exit-after-frames
    --frames 30 --fixed-dt 0.016667 --hidden --data-dir <a data directory with the snes-room fixture imported>
    --screenshot build/screenshots/room.png` (the doorway); `gpu.world: ...` leaves them under
    `build/<preset>/smoke/world/`.
  - GPU smoke tests: `gpu.world: ...` (`tests/smoke/test_world_room.cpp`); the catalog and refresh cases on the test
    room by `--scene`.
  - "Layout and layering": `rv_app_catalog`'s `room_snapshot` and `world_build` (the room up to its commit, GPU-free),
    `rv_app_cli`'s `collection_layers` and `world_notice_ui.hpp`; `rv_app`'s `world_session`, `collection_instance`,
    `cover_loader` (step 12 completes it), `world_notice_panel`; `rv_app` uses `rv_world` and the worker pool.
  - The real room: `./scripts/real-library.ps1 -Filter '[real-library][world]'` (its data directory under
    `build/<preset>/real-library/world`).
- **docs/architecture.md**: the app's world (the threading diagram above); `rv_app_catalog` holds the room's flow.
- **docs/roadmap.md**: row 10b done (edited here).
- **BACKLOG.md**: "The room's static part built on the pool" (the open issue above); "The import's unknown media
  recorded" (step 2's note).
