# I4 step 6b: the persisted start, live updates and the CRT of the eye's room

Roadmap row: I4 step 6b. Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §5, §7, §9, §13; ADR-0012 §7.

## What was built

- **New systems from the app.** The building job's update gives `updateBuilding()` every catalog system that gets a
  room and has a placeable title, as `worldplan update` does (step 6a): a system without a room enters with its own
  `loadWorldData()` data; the others are left out with a log line (those without a room, excluded, in one line per
  job); entered, waiting and held systems are logged. The building fixture's `additions/` put megadrive in gen4's new
  shared room, and the CLI finds the app's plan up to date.
- **The items files hold the CRT's data** (`RoomItemScreen`: a placed game's clip, screenshot and title screen relative
  to the media root, its last play, its title's priority gathered from its versions). Every collection room of a floor
  gets its CRT games and content digest in `buildFloor()`, from the items alone; the preview reads them under the
  library's media root when it can be read (its covers still from the cache alone), each game's last play the
  catalog's sessions now (`catalog_db::lastPlaySessions()`), so a CRT starts on the game the building's result chooses
  also after a game was played since the items were written. A room's CRT plays its placed games only (ADR-0013 §9).
  The items files' schema is 2 (schema 1 still read, without the CRTs' data).
- **Room by room** (`planLiveUpdate()`'s `RoomUpdate`, used by the persisted start's reconcile and by the live update):
  each room by its content digest (`world::roomContentDigests()`), its items and its covers. At the persisted start
  every room is reconciled as `textures`: its covers served again from the cache to check their sources (the media
  root known now), nothing uploaded over a picture that showed. In a live update an unchanged room keeps its
  collection and pictures (the new cover loader takes its items over as they show: `adopted.resident`, nothing served
  or uploaded again); a room whose items changed has them applied in place, the others untouched; a static change of
  the floor commits it again, every room's pictures kept, the walker moved with the area that holds it
  (`walkerMove()`: the room's scene placement, or the floor's frame for the hall). Other floors are only saved. The
  report gives each room's part (`reconcile.rooms`, `liveUpdates[].rooms`), the `walker` kept, and each room's
  `uploads` and `reuploads`.
- **The hero CRT of the eye's room** (`ScreenRoomTracker`, rv_app_cli): bound after the eye stayed 0.3 s in a room
  (also the doorway's hysteresis), kept in a hall, never while a game runs; from the lobby the service is made on the
  first room the eye stays in (the run's first screen). One `ScreenService` for the run: `bind()`
  stops the clip as a clip replacement does (its decoder's thread ends on its own, never joined on the render loop),
  clears the previous screen's override, restarts the choice on the new room's games; any scene shown in place of
  another (a floor committed again, the shell or the test room after a floor) unbinds without a join and binds the new
  scene's CRT, if any; `shutdown()` only at the run's end. The quick catalog's selection plays only when its game
  stands in the eye's room. Report: `video.binds` (the binds after the run's first screen, with `clipStopped`),
  `video.waitsBeforeExit` (the waits for decoder threads before the run's end: 0), `world.screenRoom`, the
  `screenBind` phase (60 frames from the bind).
- **An unchanged start plans nothing.** The job writes a building stamp (`building-stamp.json` beside the items:
  digests of the plan, of the catalog's stamp read with the snapshots, of the world's inputs). At the next start, before
  any snapshot, it compares (`unchangedBuilding()`).
- **The room snapshot reads through its system's index** (`room_query.cpp`: `CROSS JOIN`): SQLite's planner had scanned
  the whole of `idx_games` and `idx_games_system` for every system, about 70 ms a system whatever it held.

## Decisions

- **The skip rule.** The job plans nothing only when all hold: the stored plan's file digest is the stamp's (no other
  writer since); the catalog's stamp is the same (`catalog_db::catalogStamp()`: the library's newest `rv_imports` id,
  which every committed import, refresh or index rebuild adds, also one that changed nothing; the count and latest start
  of the play sessions that count; the identities' aliases and history; the schema and sort-key versions); the world's
  inputs are the same (`worldInputsDigest()`: the program, its version banner and its executable's name, size and last
  write time, so that another build of the same version never takes the stamp; the generator's, items' and stamp's
  versions; every file of the asset root's `world/` and `manifests/`, `world.json`, `library.json`, `overrides.json`,
  the media root's path); the media root can be read now; every room's items file is there. Anything else runs the
  whole job; a live update after a refresh always does. The stamp is written only for a plan saved or unchanged from
  known media with every items file kept and nothing left to settle: a room kept until a regeneration, a room held, a
  system waiting to enter one or a notice writes no stamp and removes the one there, so their notice and warnings come
  at every start. The stamp keeps the plan's soft findings for the skipped start's report.
- **The CRT plays placed games only**: the snes-room fixture's CRT lists its 50, no longer 53.
- **Re-uploads count pictures uploaded over a layer the item showed**: a cold room's labels replaced by covers at the
  reconcile count (gen5's first show), an unchanged room's never.

## Measurements (Release, reference machine)

- **The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, 0 differences
  among the snapshots' 542,120 entries; 192 systems, 105,130 entries, 44 rooms on 9 floors, 19,985 placements):
  - the unchanged start's building job: **0.5 s** with the stamp (0.45 s of it the lobby's floor), against 16 to 18 s
    before this step (the snapshot alone 13 to 15 s, the planning about 3 s); the whole job when something changed:
    2.9 s (the snapshot 1.1 s, the update 1.4 s);
  - NFR-01: the stored lobby shown 2.3 to 2.7 s after the process's creation, the quick catalog's first page 1.4 to
    1.6 s (limit 5 s);
  - the computer floor (10 rooms, 5,314 items, its largest room 1,646): GPU memory at its peak 407.3 MiB (arrays
    210.9 MiB, BC1), cold and warm (limit 512 MiB); cold, its floor's build took 13 s on a pool busy with the covers;
  - `worldplan update` on the unchanged library: 4.3 s (14.3 s in step 6a).
- **90,000 generated entries** in 11 systems (`rv_tests "[world_build][bench]"`): the unchanged start's job 3.4 s
  whole before the snapshot's index fix, 2.8 s after it (the snapshot 1.1 s, then 0.6 s), 58 ms by the stamp.
- The CRT's rebind (`gpu.world floor app: the hero CRT rebound on a floor that runs freely ...`: the walker backs from
  the first room's playing clip into the second room, whose CRT is bound): 0.07 to 0.08 ms on the main thread, the
  clip stopped with its decoder's thread still ending (never waited for: `waitsBeforeExit` 0), the 60 frames after it
  p99 3.9 to 9.8 ms over two runs (limit 50 ms). The lockstep walk's rebind stops the first room's clip, paused out of
  view, likewise.

## Deviations and limits

- No room of the building fixture grows with `additions/` (each takes a dressed level), so the GPU cases keep the
  walker, who walked before the refresh, where the same walk ends without the additions, in the start room's frame or
  the hall's; a room's move is covered by the unit test of `walkerMove()`.
- A floor committed again, or items applied in place, ends a Focus or Inspect (the item back in its slot, as I3's live
  update did); the walker keeps the pose it had, moved with its area.
- A system that loses every entry keeps its last items file, so the stamp never matches until its room is updated: the
  whole job runs at each start meanwhile.
- A skipped start reports the plan's soft findings from the stamp but no change report (nothing ran).
- A rebuild of the executable (even one that changes nothing) runs the whole job once at the next start.

## Follow-ups

- **Step 7:** the ride rebuilds a floor through the same machinery (pictures kept per room, the walker at the
  destination's elevator); the CRT binds the destination's room once the eye stays in it.
