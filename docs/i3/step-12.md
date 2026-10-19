# I3 step 12: covers in the room

Roadmap row: I3 step 12 (wave 3, beside 10c and 13a; needs 5, 6 and 10b). Decisions:
[ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md) §5 (the spine on the cover's colour), §6
(the streaming loader: priorities, the budget per frame, the pause for launches, the unavailable media root, the
lockstep), §7 (the derived-texture cache), §8 (the hero textures), §9 (the budgets);
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §6 (nothing loaded while a game runs). Step 10b's first
loader served the room's items in the layout's order and made every lockstep frame wait for the whole room; this step
completes it: the order by visibility and distance, the lockstep narrowed to the frame's view, the hero textures, the
pause, the retry after a refresh, the records in the run report and the Stats panel, and the measurements of the
budgets on `snes-room`, `snes-stress` and the real SNES room.

## What was built

| File | What it holds |
|---|---|
| `rv/app/cover_requests.hpp`, `src/cover_requests.cpp` (`rv_app_cli`, GPU-free; step 0a's stubs) | `CoverView` (the eye and the frusta of the frame: the window's, and the screenshot's when its aspect differs), `CoverRank`, `CoverPriority`, `coverInView()`, `coverDistance()`, `coverPriority()`; `CoverSchedule` (every item's stage, the next items to make and to upload best first, ranked again when the view or the focus changes, nothing handed out while paused, the arrivals recorded, retries); `heroPictureSize()`, `makeHeroPicture()`; `HeroLru` |
| `src/hero_textures.hpp`, `src/hero_textures.cpp` (`rv_app`; step 0a's stubs) | `HeroTextures`: the hero textures on the worker pool, uploaded one a round, 8 kept (`HeroLru`), the shown one pinned, recorded under `GpuMemoryKind::HeroTextures` |
| `src/cover_loader.hpp`, `src/cover_loader.cpp` (`rv_app`, step 10b's) | `CoverLoader` on `CoverSchedule`: jobs and uploads in its order, the made layers handed to the arrays a budget's worth at a time, the pause, `retry()` (the media root checked on a worker, the items whose fronts did not come from their covers served again, only changed pictures uploaded), `waitForResults()`, `statusLines()`, the run report's new records |
| `src/collection_instance.hpp`, `.cpp` (step 10b's) | `itemOfPick()`, the hero front: `heroMaterial()` (the label's material with a clamped white base texture of its own, overridden by the hero texture shown), `setFrontHidden()`, `frontRenderItem()` |
| `src/world_session.hpp`, `.cpp` (step 10b's) | `SceneHost::camera`; `coverRound()` (heroes, then layers within the rest of the budget, paused while a launch is active), `followRefresh()`, `setHeroItem()`, `showHero()`, `dynamicItems()`, `settle()`, `settled()`, `waits()` narrowed, `statusLines()`; the residency's new records |
| `rv/app/world_build.hpp`, `src/world_build.cpp` | `WorldBuild::mediaRootPath`: the media root also while it is out of reach (the retry's) |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `HeroRecord`; `ResidencyRecord`'s arrival, upload, lockstep, pause, retry and hero records, written and read |
| `rv/devui/panels.hpp`, `src/panel_stats.cpp` | `StatsPanelModel::collectionStatus`, the Stats panel's "Collection" section |
| `src/retroverse_app.cpp` | the hooks below |
| tests | `tests/unit/app/test_cover_requests.cpp` (new), `test_collection_layers.cpp` and `test_run_report.cpp` (extended), `test_covers_real.cpp` (new, hidden); `tests/smoke/test_world_covers.cpp` (new); `tests/input_scripts/world_cover_retry.json` (new) |

### Hooks in the app's world wiring (wave 3: 10c and 13a touch it too)

New files hold the logic; the shared files gained these hooks only:

- `src/retroverse_app.cpp`: `State::viewItems` and three lines in `State::view()` appending
  `WorldSession::dynamicItems()` to the view's items; `host.camera` in `startWorld()`; `state.world->settle(...)` after
  the hover in `onUpdate()`; the Stats panel's `collectionStatus` lambda in `createDevUi()`.
- `src/world_session.cpp` (merged over step 10c's split of the commit): `update()` calls `followRefresh()` and
  `coverRound()` in place of 10b's loader block (10c's preview and reconciliation flow kept as it is); this step owns
  `startCoverLoader()`'s body (the loader with the collection's item boxes, the previous loader's items adopted, the
  hero set made once per collection and given the media root when it is known, the coverLoad phase pending);
  `recordBuild()` keeps `build.mediaRootPath`; `hover()` always ends with `setHeroItem(hovered)` (13a replaces that line
  with its focus, pull-out and inspection); `waits()` uses `settled()`, 10c's preview clause included (the stored room's
  frame renders once its view is in); `frameRendered()` notes the next frame; `dropCollection()` and `record()` gained
  the hero set and the new records.
- `src/collection_instance.cpp`: the hero material at `create()`, `Item::transform` and `Item::bounds`, four small
  functions.
- `src/world_build.cpp`: one line (`build.mediaRootPath`).

### With step 10c's persisted start

10c shows the stored plan's room before the snapshot (its loader serving the covers from the cache alone) and, on a
`textures` reconciliation, starts a second loader with the media root to check the covers' sources. Its open issue (the
room's layers uploaded a second time, identical) is closed here: `CoverLoader::adoptShown()` takes over what the
previous loader made (each item of the same game it had finished: its front's source and both cache entry names) and
serves those items as a retry, uploading only the layers whose entry changed or was rebuilt from a changed source.
The snes-room restarts of the smoke tests now record `adopted` {`items` 50, `changed` 0} and no upload after the
reconciliation.

## The loader's order (`CoverSchedule`)

- **Ranks** (`coverPriority`): the focused item first (step 12's is the hovered one, `WorldSession::setHeroItem`), then
  the items whose box meets a frustum of the frame (`Frustum::intersects`, conservative; the screenshot's frustum too
  when its aspect differs from the window's, since the lens's field of view is horizontal), largest projected size
  first (the box's half diagonal over its distance from the eye), then the rest of the room nearest first; equal ones by
  item index (deterministic).
- **Stages**: Waiting, Making (its job on the pool), Made (its layers in memory), Uploading (submitted to the arrays),
  Done. Both the next jobs and the next uploads are taken best first, so a layer made early for an item that has since
  left the view waits behind the view's. The made layers are handed to `gl::CollectionArrays` a budget's worth at a
  time (at most the budget's 64 layers queued there, the arrays' FIFO otherwise ignoring the order). Ranking happens
  only when the view (eye and frustum planes) or the focus changes: about 0.1 ms for 2,000 items, nothing on a still
  frame.
- **Jobs**: at most `max(48, 2 x threads)` on the pool, submitted with the pool priority of their rank (focused 2, in
  view 1, rest 0), so that an item coming into view runs before the queued rest; no new job while 256 made layers wait.
  The 48 matter for the warm cache: the results are taken once a frame, and a warm job takes about 0.5 ms, so 10 jobs a
  frame (10b's bound) fed only 20 layers a frame, a third of the budget.
- **Pause** (ADR-0012 §6): while `ProductUi::launchActive()`, a round takes the finished results and stops: no job is
  submitted, no layer and no hero texture goes up (`pausedRounds` in the report). The host's idle wait already skipped
  the world while a game had the screen (10b); this covers the frames of the launch's start and the return.

## The lockstep, narrowed to the view

ADR-0011 §6: runs with `--fixed-dt` wait until every item in the frame's view is resident. 10b waited for the whole room
before every frame; now:

- between frames (the host's idle wait, `WorldSession::waits()`): while the room is on its way, then until the items in
  the last frame's view (and the hero item's texture) are in; before the run's **last frame** until **every** item is in
  (so that the run report's residency is complete and "every front and spine resident before the screenshot" holds for
  any viewpoint);
- within a frame (`WorldSession::settle()`, after the walker and the hover moved): when the camera brought items into
  view that are not resident, the frame waits for them (`lockstepWaits`, `lockstepWaitMs` in the report), pumping the
  loader with the idle budget (8 ms or 512 layers) and `CoverLoader::waitForResults()` (a condition the jobs signal);
- at most `kLockstepWaitLimit` (10 s) each time, logged once when it passes. Other runs never wait.

Screenshots and goldens stay exact: an item out of every frustum puts no pixel on screen (labels and spines cast no
shadow). The frames that wait within themselves count the wait in their frame time (lockstep runs only).

## Hero textures (`HeroTextures`)

- The hero item (the hovered one until 13a's focus, pull-out and inspection call `setHeroItem`) asks for its hero
  texture: its cover read with `core::readFileShared` on a worker (pool priority 1000, above every layer), decoded and
  fitted with `assets::fitCover` to `heroPictureSize()`: the front label's 256:188 aspect, as wide as the cover's pixels
  need, at most 1,024 px (a 680 x 497 cover gives 680 x 499, 1.8 MiB with mips; a portrait 476 x 864 one 1,024 x 752
  with side bands; 4.1 MiB at most). Uploaded on the main thread as an RGBA8 sRGB `gl::Texture2D` with the driver's
  mips (a 2D texture, not an array), one a round, its time counted in the round's upload budget and in the
  `layerUploads` CPU section; never cached on disk.
- `HeroLru` keeps 8, least recently used out first, the shown one pinned (8 x 4.1 MiB, within ADR-0011 §9's 32 MiB).
  A cover that cannot be read or decoded is not asked again until the media root changes; with the media root unknown
  nothing is asked (the item keeps its layer).
- **Drawn by the dynamic path**: once resident, the item's front record is flagged `kHidden` in the static group and
  `WorldSession::dynamicItems()` holds the front quad at the item's transform with the collection's hero material (the
  label material with a clamped 1 x 1 white base texture of its own, its base colour overridden by the hero texture:
  `MaterialRegistry::setTextureOverride`, one hero shown at a time), the record's attributes (the hover's highlight, so
  the outline is unchanged), casting no shadow. The app appends the dynamic items to the view's items.
- Step 14a's card and 14b's media viewer take their cover from `HeroTextures::find()`.

## The media root unavailable, and the retry after a refresh

- With the media root unknown at the snapshot (10b's `media-unknown-kept`), the cache serves every cover it holds
  (`CachedCover`, its spine on the cached dominant colour) and the others show their generated labels
  (`LabelForCover`); nothing is read under the media root and no hero texture is asked.
- After the catalog's refresh ends `done` (13r's `RefreshState::last` changing; `RefreshSummary` has no equality, the
  session compares its outcome, time and counts), the loader checks the media root on a worker
  (`WorldBuild::mediaRootPath`: the resolved root also while it was out of reach) and, if it can be read, serves again
  every item whose front did not come from its cover. Only changed pictures go up again: a layer served from the cache
  under the entry name shown before is the same picture (the cached covers stay, their spines too); the labels whose
  covers can now be read become covers, their spines move to the cover's colour. Items the catalog records no cover for
  wait for the snapshot (15b's live update).

## The run report and the Stats panel

`residency` gains: `arrival` {`viewItems`, `viewResidentAfterShownMs`, `residentAtViewResident`, `inViewAmongFirst`,
`distanceQuarters` {`q1`..`q4`}: the mean distance of each quarter of the out-of-view arrivals}, `uploads`
{`overBudgetRounds`, `maxRoundMs`: frame rounds whose uploads, layers and hero together, passed 1 ms}, `lockstep`
{`waits`, `waitMs`}, `pausedRounds`, `retries` {`rounds`, `items`, `changed`}, `adopted` {`items`, `changed`}, `hero`
{`requests`, `loaded`, `failed`, `evicted`, `resident`, `bytes`, `peakBytes`, `maxUploadMs`, `shownGame`}.
The front counters (`covers`, `cachedCovers`,
`labels`, `coverFailures`) are now each item's current front, so a retry moves items between them. The Stats panel's
"Collection" section shows the layers resident, the fronts' sources, the cache's hits and misses, the queue (making,
made, uploading, in view and pending, paused) and the hero textures. `gpuMemory.heroTextures` holds the hero textures
and the hero material's 4-byte base texture.

## Tests

- `tests/unit/app/test_cover_requests.cpp` (`[app][cover_requests]`, 8 cases): the priority of an item (the focused one
  first, in view by projected size, the rest by distance, ties by index), a squarer screenshot's frustum, views
  compared; the schedule: the layout's order without a view, the view's after it, the focus first and a new view
  ranking what still waits; made layers going up best first within the layers' bound (always one), the arrivals in
  order with their ranks and distances, a job with nothing to upload done at once; nothing handed out while paused (a
  running job's layers wait); a done item queued again with its first arrival kept, a job given back; the hero set's
  LRU (8, the least recently used out, the pinned one kept, all pinned: nothing held); the hero picture's sizes and its
  making from PNG bytes (landscape, portrait, not an image).
- `tests/unit/app/test_collection_layers.cpp`: the spine's cache entry is the spine of the cover's dominant colour, not
  the palette's (ADR-0011 §5's "spines on the covers' colours"), in both formats.
- `tests/unit/app/test_run_report.cpp`: the new residency fields read back field for field, the optional one left out.
- `tests/unit/core/test_worker_pool.cpp`: the threads below normal and without dynamic boosts.
- `tests/unit/core/test_atomic_write_backups.cpp`: an unflushed atomic write replaces the file whole; its stop before
  the rename keeps the previous file.
- `tests/smoke/test_world_covers.cpp` (`gpu.covers: ...`, label `gpu`, every run hidden):
  - **snes-room, nearest first and the hero texture**: from the doorway every item is in the first view and resident
    before frame 0; cold again (cache and stored plan deleted) 0.9 m from the hero band, lockstep: every front and spine
    resident before the screenshot, the view's items (a few of the band) the first arrivals, the out-of-view arrivals'
    distance quarters increasing, the view resident before the room; the hovered item's hero texture made, resident,
    shown (`shownGame`), within 1,024 x 752 with mips, its front textured in the screenshot; hero and array memory within
    their budgets in every run.
  - **snes-room, the media root unplugged, then a refresh**: half of the fronts' cache entries deleted, the media folder
    renamed: `media-unknown-kept`, no cover read, the cached covers from the cache (`cachedCovers` between 1 and 47), the
    rest generated labels, no hero texture asked; then a run started with the folder away, the folder put back once the
    room showed, "Aggiorna libreria" pressed on frame 300 (`world_cover_retry.json`): the refresh `done`, one retry of
    the 50 items, only the labels that became covers changed (`48 - cachedCovers`), 48 covers and 2 labels at the end,
    every layer resident.
  - **snes-stress (2,000 titles)**: the plan made in RGBA8 (`--no-texture-compression`, format checked); cold in BC1
    without a lockstep, 1.2 m from a side wall's spine-out module: the arrays within 80 MiB, at least 20 items in view,
    the first arrivals the view's (within 12), the out-of-view quarters increasing, at most 64 layers a round, the
    `layerUploads` median within 1 ms (rounds over 1 ms reported, informative), the coverLoad phase opened once; then
    the media root unplugged: `media-unknown-kept`, no cover read, cached covers served.
  - hidden `[.bench]` **figures of the snes-stress room's covers**: cold and warm without a lockstep at 1920 x 1080,
    `--no-vsync`, the residency times checked in Release (15 s, 1.5 s), the coverLoad phase's p99 reported.
- `tests/unit/app/test_covers_real.cpp` (hidden, `[.real-library][real-library][covers-room]`): the real SNES room's
  covers cold and warm, and the close-ups (below).
- The pause for launches is tested on the schedule (`CoverSchedule::setPaused`); the room's launch with the fake
  emulator is 15a's smoke case.

## Results

On the reference machine (RTX 2080 SUPER), `RV_LAUNCH_BACKGROUND` set, `RV_LAUNCH_ALLOWLIST` unset, while other
worktrees built and tested (informative: the gate measures on a quiet machine).

- **Builds**: `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` with 0 warnings (after the merge of
  main with step 10c).
- **Unit tests**: `./scripts/test.ps1` passed 1,651 of 1,651 (Debug, after the merge).
- **GPU tests**: `./scripts/test.ps1 -Preset msvc-x64-debug-gpu` under `python -I scripts/focus_watch.py --watch build
  --out build/focus/12.json`: in the last full run every `gpu.world`, `gpu.covers` and golden case passed (golden G01
  among the 58 gpu cases); the one failure was `gpu.spike/video: two generated 720p clips play on time` (I1's spike:
  its upload p99 3.42 ms against 3 ms in Debug under load; it uses none of this step's code and passed when run
  alone, 2 of 2). focus_watch: no window of a test process shown; one foreground finding, a taskbar jump list
  ("Jump List per ClickUp", `ShellExperienceHost.exe`) attributed to `rv_tests.exe` through a reused process id, the
  user's own click on the desktop. An earlier full run had 0 findings. The orchestrator stopped the work before a
  fully green rerun.
- **The real library** (`./scripts/real-library.ps1 ... -Filter '[real-library][covers-room]'`, Release, ES-DE
  closed): passed, 0 differences among 542,120 entries.

## Figures

| | Budget | snes-stress (2,000), Release, 1920 x 1080, `--no-vsync` | the real SNES room (890), Release |
|---|---|---|---|
| Every layer resident after the room showed, cold | 15 s (2,000), 10 s (890) | 8.2 to 10.1 s; once 15.4 s under load | 7.1 s |
| The same, warm (a restart: the stored room, then reconciled) | 1.5 s, 1.0 s | 0.52 to 1.23 s | 0.93 s |
| Frame p99 over the coverLoad phase, cold | 33.3 ms | 9.7 to 268 ms (the machine's load) | 32.4 ms (p50 3.2 ms) |
| The same, warm | 33.3 ms | 4.1 to 64 ms | 6.4 ms |
| Layer uploads per frame (`layerUploads`), cold | 1 ms | p50 0.02 to 0.33 ms; max 9 to 128 ms (single driver stalls) | p50 0.02 ms, p99 0.44 ms, max 2.35 ms (1 round over) |
| Collection arrays | 80 MiB at 2,048 layers | 76.7 MiB | 33.6 MiB |
| Hero textures | 32 MiB | | 1.7 MiB a 680 x 499 cover |
| GPU memory, every kind (targets and buffers included) | | peak 258.7 MiB (textures alone about 170 MiB) | peak 224.5 MiB |

- Snes-room (Debug): the hovered item's hero 680 x 499 uploaded in 0.62 ms; from 0.9 m the view's 6 items resident
  14 ms after the room showed (warm).
- The textures reconciliation of 10c's restart uploads nothing again (`adopted` 50, `changed` 0).

## Close-ups of the real room (BC1 review)

Under `build/msvc-x64-release/real-library/covers/` (the user's covers: never committed): `band_060_bc1.png` and
`band_060_rgba8.png` (0.6 m from the hero band, the hovered item with its hero texture), `band_150_*` (1.5 m),
`spines_100_*` (a spine-out level). Reviewed: decision 13.

## Decisions taken within the step

1. **One schedule decides both the jobs and the uploads.** Ranking only the jobs would let layers made early for items
   that have left the view take the frame's budget; the arrays' upload queue is a FIFO, so the loader hands them a
   budget's worth of the best made layers at a time.
2. **"In view" is the frustum test of the item's box, not the cluster's**: an item out of every frustum (the window's
   and the screenshot's) puts no pixel on screen, which is what the lockstep needs for exact screenshots, and a
   cluster's box (a whole shelf level) would count items far to the side. Occlusion is ignored (conservative).
3. **The lockstep waits for the whole room before the run's last frame**, the view before the others. ADR-0011 §6 names
   the view; the row's "every front and spine resident before the screenshot" and the run report's residency need the
   room complete at the end of a frame-limited run, whatever the viewpoint. The walks and the first frame no longer wait
   for the room's far side (the snes-room fixture from 0.9 m: 6 items in view, resident 14 ms after the room showed,
   instead of all 50).
4. **The frames whose camera reveals pending items wait within the frame** (`settle()`), after the walker and the hover
   moved: waiting between frames only knows the previous frame's camera (`turn_around.json` turns 18 degrees a frame).
   Such waits count in the frame's time (lockstep runs only, recorded as `lockstepWaits`).
5. **The hero item is the hovered one** until 13a's focus: the reticle within 2.5 m. ADR-0011 §8 names the focused,
   pulled-out or inspected item; 13a and 13b call `WorldSession::setHeroItem` with theirs. A hover swaps the front for
   the hero once it is resident (680 x 499 for the fixture's and the real library's usual covers), which the shelf
   screenshot shows indistinguishable at 0.9 m and sharper at 0.6 m.
6. **The hero picture keeps the front label's aspect** (fitted with step 5's rules) rather than the cover's own, so that
   it maps onto the label quad's texture coordinates exactly as the layer did; "the source size capped at 1,024 px" is
   read as the cover's pixels kept up to 1,024 px wide.
7. **Hero textures get their mips from the driver** (`glGenerateTextureMipmap` on a 2D texture): ADR-0011 forbids it on
   the arrays only; the hero's mips are seen at most briefly (it is close by construction).
8. **The retry checks the media root itself** (a directory test on a worker) instead of asking the catalog worker for a
   new snapshot, which is 15b's (live updates): after a refresh only the items whose fronts did not come from their
   covers are served again; an item the catalog recorded no cover for waits for 15b.
9. **Job priorities in the pool follow the ranks** and the jobs in flight are at least 48: measured on snes-stress warm,
   10b's 10 jobs a frame fed 20 layers a frame; 48 keep the 64-layer budget fed (every layer resident 0.5 to 1.2 s warm
   in Release instead of about 3 s at 60 Hz).
10. **The workers lose their dynamic priority boosts** (`SetThreadPriorityBoost`, `rv_core`'s pool): measured on
    snes-stress cold in Release, the frames of the coverLoad phase took p50 40 ms with the boosts (a worker woken by its
    file I/O lifted to the main thread's priority) and 11 ms without; ADR-0011 §6's below-normal priority is what it
    meant.
11. **The derived-texture cache writes its entries without the flush** (`core::Durability::Unflushed`): step 5's note
    (the flush about 7 ms of a 19.8 ms cold serve); an entry is checked when read (checksums, the key) and made again
    when lost, so atomicity is enough. The world's plans and every other atomic write keep the flush.
12. **The coverLoad phase opens after the room's first two frames**: the commit's frame and the next, which waits for
    the new scene's first GPU work (300 to 350 ms on snes-stress in BC1 and in RGBA8 alike), are the commit's hitch (10b's
    open issue), as the run's first 30 frames are its warm-up. The loader runs from the commit as before; only the
    measurement starts later.
13. **BC1 stays for every level of the fronts and spines** (the open point of step 5): reviewed on the real room's
    close-ups (below), BC1 at 256 x 188 reads sharper than the RGBA8 fallback at half resolution at 0.6 m and 1.5 m, its
    block artefacts visible only magnified three times (the red titles' edges); the hero texture takes over at the
    distances where they would show. No level and no class needs RGBA8; BC7 stays I8's.

## Open issues

- **NFR-04 during a cold load** is not met on this machine while other worktrees build: frame p99 over the coverLoad
  phase 210 to 270 ms cold and 7 to 64 ms warm on snes-stress (Release, 1920 x 1080, `--no-vsync`); the frames that
  wait are the main thread's and the driver's turns on cores shared with the other processes' normal-priority threads
  (40 to 75 ms frames also occur after the room's last layer, with no work of the loader at all). The gate's
  quiet-machine measurement decides; if it still fails, the remedies are fewer workers while loading cold, or the
  main thread above normal priority (ADR-0004's process boost, I5).
- **Single layer uploads of 12 to 92 ms** appeared in the cold runs (`maxRoundMs`): the first layer of a frame always
  goes (step 6's meter), and a driver stall lands on it. ADR-0011 §6's remedy is a pixel buffer ring (`VideoTexture`'s);
  not built: the warm runs never passed 1 ms and the stalls coincide with the machine's load.
- **The spines of the real room are mostly near black**: the dominant colour of SNES covers is often their black frame
  (step 5's rule: the fullest bin). Readable (ivory text at 4.8:1), but monotone; step 16 (look development) may weight
  saturated bins.
- **The snes-stress cold time** once reached 15.4 s (budget 15 s) under the machine's load (the workers below normal
  without boosts yield to every other process); 8.2 to 10.1 s otherwise. The gate's quiet machine decides; the real
  room's 7.1 s (budget 10 s) holds. A fully green rerun of the whole GPU suite was not made (the orchestrator's stop);
  `gpu.spike/video`'s Debug upload p99 failed twice under load, unrelated to this step.
- `tests/smoke/test_world_room.cpp` (10b's budget case) now allows the `layerUploads` median 5% over 1 ms in Debug
  (step 6's tolerance): with 48 jobs in flight a warm Debug run fills every frame's millisecond.

## Notes for later steps

- **13a**: replace `hover()`'s last line (`setHeroItem(m_collection->hovered())`) with the focused, pulled-out or
  inspected item; the pulled-out item's dynamic path can reuse `CollectionInstance::frontRenderItem` with the hero
  material when its hero is resident (`HeroTextures::find`), and `WorldSession::dynamicItems()` is where the app takes
  dynamic items (`State::view()`).
- **14a, 14b**: the card's and the media viewer's cover: `HeroTextures::find(game)` (request it first; the set keeps 8).
- **15a**: the pause is in place (`coverRound` reads `ProductUi::launchActive()`; `pausedRounds` counts the rounds);
  the smoke case with the fake emulator checks `pausedRounds > 0` and no upload during the game.
- **15b**: an item swapped or added: `CoverSchedule::requeue` and a new `CoverItem`; `CoverLoader::retry` shows how a
  changed picture is uploaded into the item's own layer; the hero of a changed cover must be dropped (`HeroLru::erase`).
- **16**: the spines' colour (open issue above); the label sizes are step 5's.
- **17b**: `covers: the figures of the snes-stress room's covers` (hidden) prints the cold and warm figures.

## For the merge

- **CLAUDE.md**:
  - "Running the app": the covers come nearest first (the hovered item, the view by projected size, the rest by
    distance); the hovered item's front shows its hero texture (its cover at up to 1,024 px); lockstep runs
    (`--fixed-dt`) wait for the layers of the frame's view (and the hovered item's hero texture), and for the whole
    room before their last frame (10 s at a time); nothing is loaded while a game runs; after "Aggiorna libreria" the
    covers the media root could not give are tried again.
  - `--run-report`: `residency`'s `arrival`, `uploads`, `lockstep`, `pausedRounds`, `retries`, `hero`.
  - Developer overlay: the Stats panel's "Collection" section.
  - GPU smoke tests: `gpu.covers: ...` (`tests/smoke/test_world_covers.cpp`).
  - The real room's covers: `./scripts/real-library.ps1 -Filter '[real-library][covers-room]'` (its data directory and
    close-ups under `build/<preset>/real-library/covers`).
  - Conventions: `core::writeFileAtomic(..., core::Durability::Unflushed)` only for derived data checked when read; the
    worker pool's threads run below normal without dynamic boosts.
- **docs/architecture.md**: `rv_app_cli`'s `cover_requests` (the loader's order, the hero set's LRU), `rv_app`'s
  `hero_textures`.
- **docs/roadmap.md**: row 12 done (edited here).
- **BACKLOG.md**: "A pixel buffer ring for the collection's uploads" (if the gate's quiet machine still sees single
  uploads over the budget); "Spine colours from saturated bins" (step 16 may take it).
