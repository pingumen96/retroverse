# I3 step 10c: the persisted start, the room's CRT and the object fallbacks

Roadmap row: I3 step 10c (wave 3, after 9, 10b and 11; steps 12 and 13a run beside it and touch the app's world wiring:
this step keeps to new files and the hooks listed below, and merges first). Decisions:
[ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §3 (unknown media decide nothing), §10 (the
incremental rules), §11 (a plan that breaks a hard rule is never shown), §12 ("at start the app reads the persisted plan
on the pool and shows the room at once, then reconciles it with the catalog's snapshot ... and applies the difference");
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §7 (the hero CRT: the start choice, the fallback chain, one
decoder); style §21 and §26.3 (object fallbacks, "an asset lost on purpose"). With a plan stored, `retroverse.exe` now
shows the stored plan's room at its first frame, before it even asks the catalog worker for the room's snapshot, its
covers and spines from the derived-texture cache by the room's items the last run kept; once a frame has shown it, the
snapshot's result reconciles it: an unchanged catalog moves, rebuilds and writes nothing, a changed one (a game added)
commits the room again around the items that stood, with the walker where it stands. The room's hero CRT plays the ES-DE
catalog's clips (the start choice: the most recently played placed game with a video, else the first featured one; the
quick catalog's selection after its dwell; the fallback chain of step 11). A manifest missing from the asset root falls
back to its family's prefab with one warning per run, the room built and valid. Golden G01 (`--scene m0_testroom`) is
unchanged.

## What was built

| File | What it holds |
|---|---|
| `rv/app/room_items.hpp`, `src/room_items.cpp` (`rv_app_cli`, new) | `RoomItemText`, `RoomItems`, `roomItemsOf()`, `collectionTitlesOf()`, `roomItemsFile()`, `roomItemsToJson()`, `parseRoomItems()`, `readRoomItems()`, `writeRoomItems()`: the room's items kept for the next start |
| `rv/app/world_reconcile.hpp`, `src/world_reconcile.cpp` (`rv_app_catalog`, new) | `LayoutDifference`, `compareLayouts()`, `Reconciliation`, `ReconcilePlan`, `planReconciliation()`: what the snapshot's result does to the stored room shown |
| `rv/app/room_screen.hpp`, `src/room_screen.cpp` (`rv_app_catalog`, new) | `roomScreenGames()`: the hero CRT's games of the room, in the featured order |
| `src/world_start.hpp`, `src/world_start.cpp` (`rv_app`, step 0a's stubs) | `WorldSession::commitPreview()` and `WorldSession::reconcile()` |
| `rv/app/world_build.hpp`, `src/world_build.cpp` | `previewWorld()`; `WorldPhase::Previewing`, `WorldFlow::takePreview()`, the snapshot asked once the preview is taken; `WorldBuild::preview`, `screenGames`, `objectFallbacks`, `itemsWritten`, `itemsError`, `itemsRead`; `ObjectFallback`; `WorldFlowTimes::previewMs`, `previewJobMs`; `buildScene()` taking the titles and the covers' text from either source |
| `src/world_session.hpp`, `src/world_session.cpp` | the preview taken and its result held for a frame; `commit()` split into `recordBuild()`, `showRoom()`, `startCoverLoader()`, `finishCommit()`; `SceneHost::showSceneKeepingWalker`, `setScreenGames`; the lockstep's first frame on the preview |
| `src/retroverse_app.cpp` | the two new `SceneHost` hooks; `replaceScene(..., keepWalker)` |
| `rv/app/screen_policy.hpp`, `src/screen_policy.cpp` | `ScreenGame::startCandidate`, honoured by `chooseStartGame()` |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `WorldPreviewRecord`, `WorldReconcileRecord`, `ObjectFallbackRecord`; `WorldRecord::preview`, `reconcile`, `itemsWritten`, `screenGames`, `objectFallbacks` |
| `rv/world/scene_builder.hpp`, `src/scene_builder.cpp` | `ScenePrefab::fallbackReason` |
| `rv/assets/prefab_resolver.hpp`, `src/prefab_resolver.cpp` | a fallback warned once per process (asset root, id, replacement, reason), at debug level after |
| tests | `tests/unit/app/test_world_start.cpp` (new), `test_world_build.cpp`, `test_run_report.cpp`, `test_screen_policy.cpp`, `tests/unit/assets/test_prefab_resolver.cpp`; `tests/smoke/test_world_start.cpp` (new), `smoke_support.hpp/.cpp` |

## The persisted start

```text
main thread                                       worker pool                      catalog worker
-----------------------------------------------   ------------------------------   -------------------------------
the catalog Ready, games of snes ------------->   readWorldPlan
a plan stored ------------------------------->    previewWorld: the stored plan's
                                                  scene and collection, the
                                                  room's items' text from
                                                  <cache>/world/<library>/
takePreview -> WorldSession::commitPreview:
the room shown (covers and spines from the
cache alone), no CRT games
next update: requestRoomSnapshot ---------------------------------------------->   the room's snapshot
the snapshot taken ------------------------->     buildWorld (as 10b), then the
                                                  CRT's games and the room's
                                                  items written if they changed
after a frame has shown the preview:
WorldSession::reconcile (planReconciliation)
```

- **The preview** (`previewWorld`) is the plan as stored (outcome Kept, nothing written, no change report): its scene,
  its collection's layout made with the titles of the room's items (a placed game they do not know: no title,
  available, so that its instance flags match the reconciled room's), and the cover requests of the items they know,
  served from the derived-texture cache alone (no media root: whether it can be reached is the snapshot's to say;
  `CachedCover` entries are served by name as with an unplugged media root). A missing or unreadable items file leaves
  the items on the pending picture (logged when unreadable). A preview whose scene cannot be made (another template
  version in the asset root, say) shows nothing and the flow asks for the snapshot at once.
- **The snapshot is asked for only once the preview has been taken** (the update after `takePreview()`): the stored room
  always shows before the snapshot answers, by construction, and the catalog worker's first jobs are the quick
  catalog's. The cost on a warm start is the preview's job before the snapshot's (a few tens of milliseconds in Release).
- **The result waits for a frame**: the session takes the build only after a frame has rendered the preview
  (`m_previewAwaitingFrame`, cleared in `frameRendered()`). Lockstep runs (`--fixed-dt`) wait before frame 0 for the
  preview and its layers only, render it, then wait for the reconciliation and its layers: frame 0 is the stored room
  whole, frame 1 the reconciled room, deterministically.
- **The reconciliation** (`planReconciliation`, world_reconcile.hpp):

  | The build against the room shown | What happens | `world.reconcile.kind` |
  |---|---|---|
  | the same plan digest and the same collection (every item's game, pick id, transform and flags) | nothing moves or is rebuilt; the cover loader starts again on the same collection with the build's cover requests when they differ from the preview's or the media root is known (the covers' sources checked: a cover changed on disk is seen), the hero CRT gets its games, the notice is the build's | `textures` |
  | another plan, or another collection (games added or removed, an availability changed) | the room committed again (`SceneHost::showSceneKeepingWalker`): the scene, the collection, the pick index, the loader, the CRT's games; the walker where it stands; the items that stood keep their pick ids and transforms (the incremental rules move none) | `rebuild` |
  | no plan (the plan refused under the world's lock, the world's configuration unreadable, ...) | the empty room's shell with the build's notice, as when no plan was stored | `shell` |

  The record compares the two collections by game (`kept`, `moved`, `flagsChanged`, `added`, `removed`); `moved` is 0 by
  the incremental rules, which `requireCleanRun` checks in every smoke run.
- **The room's items** (`room_items.hpp`): `<user data>/cache/world/<library id>/room-<system>.json`, canonical JSON of
  each placed game's title, title of its file name, year, cover path (relative to the media root) and availability, and
  the system's full name. Written by the room's job after its scene, atomically, only when its text changes (an
  unchanged catalog writes nothing); never needed (a cache: deleting it costs one start's covers at the first frame). It
  holds no absolute path; the plan file stays free of titles and paths (ADR-0010's consequence: attachable to a bug
  report).

## The room's hero CRT

- **The games** (`roomScreenGames`): every snapshot entry in state Game with a video, a screenshot, a title screen or a
  cover, its paths under the media root as the snapshot derived them (`esde::roomMediaPaths`, no folder listed; opened
  through `core::extendedLengthPathBelow`, which the decoder and the stills both read), its last play the snapshot's
  `lastSession` (Retroverse's sessions: ES-DE's `<lastplayed>` is not in the snapshot). The placed games come first in
  the featured order (the display fixtures' titles, the hero band's, the spine-out ones, each by `world::priorityLess`
  with a title's data gathered from its versions as the selection gathers them) and are the start candidates; the
  others follow by GameId with `startCandidate` false, played only when selected. None while the media are unknown or
  without a media root.
- **The start choice** (ADR-0012 §7): the most recently played placed game with a video, else the first featured one with
  one (`chooseStartGame` skips the games that are no start candidates). On snes-room: Amber Lantern (a favourite, 42
  plays, the first by priority).
- **The selection** stays the quick catalog's (step 11's bridge) until 13a and 13b give the world's: any game of the
  system with media plays when selected, placed or not.
- **When**: the preview has no games (the CRT shows its colour bars); the reconciled room's games are set through
  `SceneHost::setScreenGames` (`ScreenService::setGames`), after the scene is shown for a commit or a rebuild, at once for
  a `textures` reconciliation. The fallback chain, the dwell, the pause out of view, the launches are step 11's,
  unchanged.

## Object fallbacks in the room

The room's prefabs were already resolved through `assets::resolvePrefab` (step 1b) by the generator's capacity and by
the scene's data, so a missing manifest fell back to its family's prefab; but one run resolved it two to three times (the
capacity, the scene, the preview), each logging its warning. The resolver now warns once per process for the same asset
root, id, replacement and reason, and logs the repeats at debug level. The scene data records each prefab's
`fallbackReason`; the build lists the fallbacks (`WorldBuild::objectFallbacks`) and the run report says them
(`world.objectFallbacks`: prefab, source, reason). With `furniture.shelf.hero_module.generic` removed from a scratch copy
of the asset root, the hero wall's sockets get the side-wall module (the family's generic, same footprint, no hero
band): the 44 display slots take the first 44 titles, the other 6 stand spine-out in the hero wall, 0 hard findings,
every item drawn, one warning per run (the first run's generation and the restart's preview and reconciliation alike).

## The run report

`world` gains:

```json
"preview": {"frame": 0, "planDigest": "b24a...", "items": 50, "itemsKnown": 50, "committedAtMs": 1183.4,
            "buildMs": 10.9, "commitMs": 57.6, "frontsResident": 50, "spinesResident": 50},
"reconcile": {"kind": "textures", "frame": 1, "kept": 50, "moved": 0, "flagsChanged": 0, "added": 0, "removed": 0,
              "coversReloaded": true},
"itemsWritten": false, "screenGames": 53,
"objectFallbacks": [{"prefab": "furniture.shelf.hero_module.generic", "source": "family generic",
                     "reason": "the manifest is missing"}]
```

`preview` and `reconcile` are left out without a stored plan shown; `frontsResident` and `spinesResident` are the layers
resident when the preview's frame was rendered (absent when the run ended before). `firstFrame` is the preview's frame;
`committedAtMs`, `commitMs` and `snapshotToCommitMs` are the reconciliation's commit. A run that ends before the
reconciliation reports the preview's outcome (`kept`, its reason "the stored plan, shown before the catalog's
snapshot").

## Tests

- `tests/unit/app/test_world_start.cpp` (`[app][world_start]`): the room's items round trip as canonical JSON, by
  GameId, the titles of the preview's layout (unknown games untitled and available), written once and not again when
  unchanged, read back, rewritten when a title changes; a file of another shape refused with its JSON pointer (not an
  object, a newer schema, a bad GameId, games out of order or twice, a missing availability, a title that is no string);
  `compareLayouts` (kept, moved by transform or pick id, flags changed, added, removed); `planReconciliation` (textures
  with and without the covers served again, a title changed, rebuild for another plan or an availability changed, the
  shell without a plan or a scene).
- `tests/unit/app/test_world_build.cpp` (`[app][world_build]`, snes-room imported): the flow makes the preview before it
  asks for the snapshot (one request in all until it is taken), the preview equal to the room (the layout's canonical
  JSON, the cover requests), the snapshot's result reconciled as `textures` with 50 kept; the room's items written by a
  first plan, not by an unchanged one, no absolute path in them; a preview without the items (no cover request, no item
  flagged) and with a broken file; a game added: the preview's 50 items, the update's 51, `rebuild` with 50 kept and 1
  added; `roomScreenGames`: 53 games, the 50 placed first and the only start candidates, Amber Lantern the start choice
  with its four media on disk, Thistle Trellis (no cover) a selectable non-candidate, the most recently played placed
  game chosen over a more recent unplaced one, nothing with the media unknown; a manifest removed from a scratch asset
  root: the room generated with the side-wall module (0 band, 6 spine), its fallback recorded, one warning for the
  generation, the preview and the restart's update together.
- `tests/unit/app/test_run_report.cpp`, `test_screen_policy.cpp`, `tests/unit/assets/test_prefab_resolver.cpp`: the new
  records read back (absent optionals, a missing key and an incomplete reconciliation pointing at their keys);
  `startCandidate` in the start choice and a non-candidate played when selected; a fallback met again in one process not
  warned again, another asset root warned again.
- `tests/smoke/test_world_start.cpp` (`gpu.world: ...`, `[world_start]`, snes-room imported, every run hidden and
  lockstep):
  - **the persisted start**: a first run generates (no preview, the items written, 53 CRT games); a one-frame run shows
    the stored room at frame 0 with all 50 fronts and spines resident and the CRT's colour bars, nothing reconciled; a
    40-frame run: the preview at frame 0, the reconciliation at frame 1 (`textures`, 50 kept, 0 moved, the covers'
    sources checked), the snapshot answered after the preview's commit, the outcome `unchanged`, nothing written (the plan
    and the items byte for byte), the CRT playing Amber Lantern's clip (the start choice, one decoder); the one-frame and
    40-frame screenshots differ only around the CRT (pixels moved by more than 6/255 outside its projected box and bloom
    margin at most 1% of those inside);
  - **a game added** (one of G10's, imported between runs): the preview's 50 items at frame 0, `rebuild` at frame 1 (50
    kept, 0 moved, 1 added), 51 items, the plan and items written; a walk on frames 0 to 5 ends where the same walk ends
    on the unchanged catalog (the walker kept across the rebuild);
  - **the room's CRT and the quick catalog**: a search selects Lilac Loom (corrupt clip: `corrupt-data`, then its
    cover), Nutmeg Nook (empty: `invalid-file`), Opal Oriel (unsupported codec), Ember Kettle Expedition (its clip deleted
    after the import: `file-not-found`), each falling to its cover, and Juniper Jamboree, whose clip plays; never two
    decoder threads;
  - **the object fallback**: a scratch copy of the build's assets without the hero-wall module's manifest
    (`--asset-root`): the room built twice (generated, then restarted through the preview), 50 items, 0 band and 6
    spine, the fallback in the report, exactly one warning in each run's log.
- `requireCleanRun` (smoke_support): a preview is the room's first frame; a reconciliation comes after it and moves
  nothing; an overload for runs given `--asset-root`.

## Results

On the reference machine (RTX 2080 SUPER), `RV_LAUNCH_BACKGROUND` set and `RV_LAUNCH_ALLOWLIST` unset, while the other
worktrees of wave 3 built and tested:

- **Builds**: `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` with 0 warnings.
- **GPU tests**: `./scripts/test.ps1 -Preset msvc-x64-debug-gpu` under `python -I scripts/focus_watch.py --watch build
  --out build/focus/10c.json`: 1,696 of 1,697 passed (1,642 unit, 55 gpu; golden G01 among them), 0 findings (no
  foreground change, no visible window of a test process). The one failure, `gpu.world: a game added between two
  starts ...`, waited past the lockstep's 10 s for the stored room: its preview job took 15.9 s and the room's job
  14.4 s (300 ms to 1.6 s in the quiet runs) while other worktrees' compilers held every core and the pool's threads run
  below normal priority, so frame 0 went on without the room. Run again alone under `focus_watch`
  (`build/focus/10c-rerun.json`): passed, 0 findings. 10b's `requireRoomShown` (`firstFrame == 0`) has the same
  exposure to a starved pool.
- **Unit tests**: `./scripts/test.ps1` under `focus_watch` (`build/focus/10c-unit.json`): 1,642 of 1,642 passed, 0
  findings.
- **Release**: `build/msvc-x64-release/bin/rv_smoke_tests.exe "[world],golden*"` under `focus_watch`: 13 of 13 passed
  (the 4 `[world_start]` cases, 10b's 7 `gpu.world` cases, G01), 0 findings.
- **Golden G01** (`--scene m0_testroom`): 0 of 2,073,600 pixels apart (largest difference 0/255) in Debug and in
  Release.

## Figures

Release, snes-room (50 titles), lockstep runs of the smoke cases, from their run reports (informative: the machine was
shared with the wave's other worktrees; the gate measures quietly).

| | First start (no plan) | Restart, catalog unchanged | Restart, a game added |
|---|---|---|---|
| The preview's job on the pool (plan, scene data, scene, layout, items) | | 10.0 to 11.6 ms | 10.3 ms |
| The preview's commit on the main thread | | 54.6 to 62.7 ms | 58.8 ms |
| The stored room shown, from the process's creation | | 1.13 to 1.31 s | 1.31 s |
| Its layers resident after it showed (warm cache, from the cache alone) | | 8.9 ms | |
| The snapshot answered after the preview's commit (asked once it showed) | | 2.6 to 2.8 ms | 2.6 ms |
| The room's job on the pool | 29.3 ms (a first plan) | 20.2 to 21.5 ms | 34.0 ms (the update) |
| The reconciliation's commit on the main thread | 62.0 ms (the commit) | 0.17 to 0.34 ms (`textures`) | 53.0 ms (`rebuild`) |
| From the snapshot's answer to the commit | 91.6 ms | 20.8 to 21.9 ms | 87.4 ms |
| Every layer resident after the room first showed | 188 ms (cold) | 30.3 to 33.3 ms (served again, sources checked) | 21.0 ms after the rebuild |

- In Debug on quieter moments: the preview's job 0.43 to 1.6 s, its commit about 0.6 s (the scene's meshes), the
  `textures` reconciliation's commit 1.2 to 1.5 ms.
- The hero CRT's first clip frame came 22 ms after the decoder's start on a first start and 190 to 235 ms after it on a
  restart (lockstep: the reconciliation's covers are served and uploaded while the frames wait), within NFR-06's 2 s.
- The room's items file of snes-room: 50 items, about 9 KiB.

## Screenshots

Under the build tree, not committed (the fixture's invented titles and generated covers):

- `build/msvc-x64-debug/smoke/world_start/unchanged/preview.png`: frame 0 of a start with the plan stored, the stored
  room whole: the display cases, the wall shelf and the hero band with their covers, the dressing, the CRT's colour
  bars.
- `build/msvc-x64-debug/smoke/world_start/unchanged/reconciled.png`: frame 39 of the same start, reconciled: the same
  room, the CRT playing Amber Lantern's generated clip (its frame counter burned in).
- `build/msvc-x64-debug/smoke/world_start/object_fallback/generated.png`, `restarted.png`: the room with the hero-wall
  module's manifest missing: the left hero-wall module is the side-wall module, its band level given to a storage box row
  and six spine-out cartridges; every other fixture as in `preview.png`.

## Decisions taken within the step

1. **The snapshot is asked for after the preview is shown**, not in parallel with it: "shown before the snapshot answers"
   holds by construction (the smoke case checks the timestamps), the catalog worker's first jobs stay the quick
   catalog's, and the reconciliation, which cannot be committed before the preview anyway, starts a preview job later
   (some 20 to 50 ms in Release).
2. **The room's items in the cache** (`cache/world/<library>/room-<system>.json`), not in the plan or the world folder:
   the preview needs each item's title and cover path to find its cache entries without the snapshot; the plan must hold
   neither (ADR-0010), and the world folder's files are ADR-0010 §9's. A cache costs nothing when lost (pending pictures
   at the first frame).
3. **The preview's covers come from the cache alone** (no media root), and a `textures` reconciliation serves them again
   with the media root when it is known: a cover replaced on disk under the same name is seen at the next start's
   reconciliation (its entry is rebuilt by size and time), at the cost of uploading the room's layers twice at start
   (identical pictures, within the per-frame budget; step 12 can skip the uploads whose entry is unchanged).
4. **A rebuild commits the whole room again** (scene, collection, loader) with the walker kept: the incremental update's
   finer application (items added without a rebuild, textures swapped, `NoRebuild`/`LocalReflow` honoured) is step 15b's
   live update; at start a rebuild is rare (a catalog changed between two runs) and lockstep frames never see it half
   done. Interactive runs see the preview's covers pending again for the few frames the warm cache takes.
5. **Unknown games are available in the preview's layout**: without the room's items every item would be flagged
   unavailable and every start without them would rebuild; flags follow the snapshot at the reconciliation.
6. **The CRT's games are every game of the system with media, the placed ones the start candidates**
   (`ScreenGame::startCandidate`): ADR-0012 §7's start choice is among placed games, while the quick catalog's selection
   (the bridge until 13a/13b) can be any game; a non-candidate is played only when selected.
7. **The featured order is the sections' order then the room's priority** (favourites, play count, rating, sort key),
   computed from the snapshot as the selection computes it, so it holds for any plan shown (a kept one included).
8. **Fallback warnings once per process** in the resolver itself (keyed by asset root, id, replacement and reason): the
   room resolves its prefabs in several jobs, and the live updates of 15b would resolve them again; the first warning
   is the one style §26.3 asks for, the repeats stay in the log at debug level.

## Hooks in shared files (for 12 and 13a's rebases)

- `src/world_session.hpp/.cpp`: `SceneHost::showSceneKeepingWalker` and `setScreenGames`; `update()` takes the preview
  (`commitPreview`) and holds the result while `m_previewAwaitingFrame`, then `reconcile()` or `commit()`; `commit()` now
  calls `recordBuild()`, `showRoom(build, keepWalker)` and `finishCommit()`; the loader is made by
  `startCoverLoader(covers, mediaRoot, library)` (step 12's loader construction moves there); `frameRendered()` records
  the preview's residency; `waits()` lets the preview's frame render; `countPlan()` resets its counts.
- `src/retroverse_app.cpp`: `startWorld()` sets the two new hooks; `replaceScene()` takes `keepWalker`.
- `rv/app/world_build.hpp`, `src/world_build.cpp`: the `Previewing` phase and `takePreview()`; `buildScene()`'s
  parameters; `previewWorld()`; `buildWorld()` writes the room's items and makes the CRT's games after the scene.
- `rv/app/run_report.hpp`, `src/run_report.cpp`: the `world` record's new fields (after `hoveredGame`).
- `tests/smoke/smoke_support.hpp/.cpp`: the `requireCleanRun(run, assetRoot)` overload and the world record's two new
  invariants.

## Open issues

- A `textures` reconciliation uploads the room's layers a second time (identical pictures): step 12's loader can compare
  each served entry with the one resident and skip it.
- A rebuild at start shows the preview's covers pending again for a few frames in interactive runs (15b's live update
  applies additions without a rebuild).
- ES-DE's `<lastplayed>` is not in the room's snapshot, so the start choice knows only Retroverse's sessions; a user who
  never launched from Retroverse sees the first featured title's clip. Adding it to `RoomEntry` is a catalog change
  (BACKLOG).
- A run that ends before the reconciliation reports the preview's outcome (`kept`); `world.reconcile` absent says so.

## Notes for later steps

- **12**: the loader is made in `WorldSession::startCoverLoader()`; the preview's loader serves the room's items from
  the cache alone (no media root) and the `textures` reconciliation starts a second one with the media root. The lockstep
  waits for the preview's layers before frame 0, then for the reconciliation's.
- **13a**: the session keeps the preview's layout until the reconciliation (`m_shownPreview`); after a `textures`
  reconciliation the collection, pick index and layout shown are the preview's (equal to the build's by construction).
- **13b**: the hero CRT's games are set; pass the focused or inspected item's GameId to `ScreenService::update()` (step
  11's note).
- **15b**: `planReconciliation` is the start's version of the live update's decision; `showSceneKeepingWalker` keeps the
  walker for a rebuild; `LayoutDifference` measures "moves none".
- **17a**: G08a/G08b on the room are possible now (the start choice's clip, Lilac Loom's corrupt clip from the quick
  catalog), though the roadmap's goldens use a fixture CRT.

## For the merge

- **CLAUDE.md**:
  - "Running the app", the room: with a plan stored, the stored plan's room shows at the first frame (its covers and
    spines from the cache by the room's items, `<user data>/cache/world/<library id>/room-<system>.json`) before the
    snapshot is asked for, then is reconciled with it (unchanged: nothing moved, rebuilt or written; changed: committed
    again with the walker kept); lockstep runs show the stored room at frame 0 and the reconciled one from frame 1. The
    room's hero CRT plays the ES-DE catalog's clips (the start choice: the most recently played placed game with a video,
    else the first featured one; the quick catalog's selection after its dwell; the fallback chain). A manifest missing
    from the asset root falls back to its family's prefab, warned once per run.
  - "Running the app", `--run-report`: `world.preview`, `world.reconcile`, `world.itemsWritten`, `world.screenGames`,
    `world.objectFallbacks`.
  - GPU smoke tests: `gpu.world: ...` of `tests/smoke/test_world_start.cpp` (`[world_start]`).
  - "Layout and layering": `rv_app_cli`'s `room_items`; `rv_app_catalog`'s `world_reconcile` and `room_screen`;
    `rv_app`'s `world_start`.
- **docs/architecture.md**: the app's world: the persisted start (the preview job, the snapshot after it, the
  reconciliation) in the threading diagram.
- **docs/roadmap.md**: row 10c done (edited here).
- **BACKLOG.md**: "ES-DE's lastplayed in the room's snapshot" (the start choice); "Skip identical layer uploads at a
  textures reconciliation" (step 12 may take it).
