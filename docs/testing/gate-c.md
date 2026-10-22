# Gate C: the building

Gate C closes iteration I4, the first part of milestone M3 (specification §22): the whole ES-DE library in one
building, walkable floor by floor, with a functional look ([ADR-0013](../decisions/ADR-0013-the-building.md)). Every
system of the library has a room, one room per console, the systems under 30 distinct titles share their floor's room
and the arcade systems the arcade room; the floors follow the console generations, with a handheld floor and a computer
floor, joined by an elevator whose directory also lists the favourites and the recent games; the ground floor's lobby is
the hub. One floor is resident at a time. What each I4 step built and measured is in [docs/i4/](../i4/). This page says
what the gate checks, how to run it, what step 8 changed, the figures of its runs on the reference machine, where
ADR-0013's decisions and M3 part 1's requirements are proven, the follow-ups, and how to try the building.

**Status: passed** (October 22, 2026, reference machine: `./scripts/gate.ps1 -Gate C`, then `./scripts/gate.ps1
-Gate C -RealLibrary`, below). Every correctness check passed and every timing row is within the acceptable limits.
Next the user walks the building ("Try the building", below) and declares I4 done.

## What the gate checks

`scripts/gate.ps1 -Gate C` runs everything Gate A runs ([gate-a.md](gate-a.md): the build matrix of Debug,
RelWithDebInfo and Release without a warning, the unit tests and the GPU checks of Debug and Release) and adds:

- **Every golden**, in Debug and Release: G01, the SNES room's G02, G02s, G06, G07, G08a, G08b and G10
  ([gate-b.md](gate-b.md)) and the building's three (`tests/smoke/test_golden_room.cpp`, below): G11 the lobby, G12
  the jewel-case room, G13 a shared room; one row per golden and preset with its case's verdict (`comparison.txt`: a
  pixel differs when a channel moves by more than 6/255, at most 0.5% may differ), and a failed row for any of the
  eleven that the run does not report (a case renamed, untagged or not discovered).
- **The I4 checks**, per Debug and Release, one row per area, counted from the unit and GPU runs (a case counts in one
  area only; `scripts/gate.ps1`'s `$i4Areas`):

  | Area | Unit cases | GPU cases |
  |---|---|---|
  | World building | `world/building`, `world/building plan`, `world/lift`, `world/ids`, `world/families`, `assets/families`, `esde/building_fixture`, `cli/worldplan*`, `cli/where-is`, `world/where-is` | `gpu.world families` |
  | Floors | `world/floor`, `scene/areas`, `render/light_packing` | `gpu.world floor` |
  | Shared rooms | `world/shared rooms`, `app/room_screen: a shared room ...` | `gpu.world floor app: the arcade room and a floor's shared room ...` |
  | Update suite | `world/building update`, `world/building suite` (the building's NFR-08 property suite) | |
  | Directory | `ui/directory`, `app/directory_data`, `catalog/recent_games`, `catalog_db/recent_query` | `gpu.directory` |
  | Elevator | `app/elevator_ride`, `app/elevator_prompt_document`, `app/world_build: a ride's floor ...`, `app/run_report: the floors, the rides ...` | `gpu.world elevator` (9 cases) |
  | Floor app | `app/world_build` (but the ride's floor and the unchanged start), `app/room_snapshot`, `app/room_items`, `app/cover_requests` of a floor | `gpu.world floor app`: two rooms of one family, the walk between rooms, the lobby start |
  | Start and live | `app/world_flow`, `app/world_live_plan`, `app/world_reconcile`, `app/screen_room`, `app/world_build: an unchanged start ...`, `app/room_screen: the room ...` | `gpu.world floor app`: the stored floor reconciled room by room, a system entering the resident floor, the hero CRT rebound, the lobby into the arcade room, the quick catalog's selection in its room only |

- **The benchmark path** in Release (`rv_smoke_tests "[benchmark]"`, the snes-stress room at 1920 x 1080 with
  `--no-vsync`): its frame times and GPU memory read from its run report against the acceptable limits
  ([roadmap](../roadmap.md), "Acceptable performance"), its GPU passes reported; `-Benchmarks` also runs every hidden
  benchmark as informative rows.
- **The focus watch**: the whole gate runs under `python -I scripts/focus_watch.py --watch build`, as Gates M1 and B
  do (a foreground change to a test process, a visible window of one, or a system error dialog fails the gate).
- **With `-RealLibrary`**, on the real ES-DE library (its folders from `-EsdeHome`, `-EsdeInstall`, `-RomRoot`,
  `-MediaRoot` or the `RV_REAL_*` variables; refused while ES-DE runs): one `scripts/real-library.ps1 -Filter
  '[real-library][building],[real-library][room-real]'` run (read-only snapshots of the ES-DE home and install,
  hashed, and of the ROM and media roots, listed, before and after: **0 differences** required; every process with
  `RV_LAUNCH_BACKGROUND=1` and `RV_LAUNCH_ALLOWLIST` naming the build's fake emulator alone). The building's test
  (`tests/unit/app/test_building_real.cpp`) imports the whole library into a new catalog under
  `build/msvc-x64-release/real-library/building` and runs `retroverse.exe` (Release, hidden) on it: `first` (no plan:
  the building planned), `again` (the plans' folder removed: planned again from nothing), `second` and `third` (the
  user's start, no lockstep, the quick catalog opened on frame 1), `whole` (the building stamp removed: the whole job on
  the unchanged library), `busiest-cold` and `busiest-warm` (a start in the largest room of the busiest floor, the
  one with the most placements in the plan), `ride-cold` and `ride-warm` (the derived covers emptied, then a ride from
  the lobby through the elevator and the directory to that room and back with Map, as the user rides). The gate reads their reports into rows against the
  acceptable limits: the building planned twice with one plan (the test compares the files' SHA-256), an unchanged
  restart's building job skipped by its stamp within 3 s, NFR-01 to the lobby (5 s), the busiest floor's GPU memory
  (512 MiB), the rides (the view back within 10 s cold and 3 s warm) and the room that holds the walker after them
  (30 s cold, 3 s warm). The I3 SNES room's test (`[room-real]`, [gate-b.md](gate-b.md)) runs in the same pair of
  snapshots, its rows as Gate B's.

Every step logs into `build/gate-c` (the build and test logs, the JUnit and Catch2 XML reports, `results.json`,
`focus_watch.json`, `real-library.log` and `real-library/` with the snapshots and `tests.log`).

## How to run it

```powershell
./scripts/gate.ps1 -Gate C                     # fixtures only
./scripts/gate.ps1 -Gate C -RealLibrary        # also the real library (ES-DE closed; the RV_REAL_* variables or
                                               # -EsdeHome, -EsdeInstall, -RomRoot, -MediaRoot)
./scripts/gate.ps1 -Gate C -Clean              # every target rebuilt first
```

Close ES-DE first (`Get-Process ES-DE` must find nothing). The gate exits 0 when no step failed (a skipped step, no
OpenGL 4.5 context or another GPU than the goldens', is reported, not failed), 1 otherwise, 2 when it cannot start.

## The figures of the runs

October 22, 2026, on the reference machine ([reference-hardware.md](reference-hardware.md): Intel Core i7-9700K, 8
cores, 64 GB, RTX 2080 SUPER with OpenGL 4.5.0 NVIDIA 617.42, Windows 11 Home 10.0.29671), at commit e3ae3bf (I4
step 8 on branch `i4/step-8`). Not a quiet machine: no other build or test of the project ran, but the user's desktop
programs did (VS Code, Chrome, Edge and other agents' tools on the same CPU and GPU). The timing figures are measured
against the acceptable limits only (roadmap, "Acceptable performance"); the correctness checks are not affected.

**Run 1**, `./scripts/gate.ps1 -Gate C` (51 steps, 5,036 s): **PASSED**, 0 failed, 0 skipped (logs in
`build/gate-c-run1`).

| Step | Result | Limit | Details |
|---|---|---|---|
| Build `msvc-x64-debug`, `-relwithdebinfo`, `-release` | passed | 0 warnings | 0 warnings each |
| Unit tests Debug / Release | passed | | 1,863 passed each (1,238 s / 398 s) |
| GPU smoke tests Debug / Release | passed | | 96 / 93 passed (Release: 1 skipped, the overlay's cursor case, as in every Release run) |
| Goldens G01, G02, G02s, G06, G07, G08a, G08b, G10, G11, G12, G13, Debug and Release | passed | 0.5% | **0 of 2,073,600 pixels differ** by more than 6/255 in each of the 22 comparisons (largest difference 0/255) |
| I4 world building, Debug / Release | passed | | 60 passed each (57 unit, 3 GPU) |
| I4 floors | passed | | 21 passed each (19 unit, 2 GPU) |
| I4 shared rooms | passed | | 9 passed each (8 unit, 1 GPU) |
| I4 update suite | passed | | 8 passed each (8 unit: the building's NFR-08 suite, the golden update sequence, ...) |
| I4 directory | passed | | 35 passed each (34 unit, 1 GPU) |
| I4 elevator | passed | | 20 passed each (11 unit, 9 GPU) |
| I4 floor app | passed | | 25 passed each (22 unit, 3 GPU) |
| I4 start and live | passed | | 26 passed each (21 unit, 5 GPU) |
| Benchmark path, snes-stress (2,000 titles), frame times | passed | p99 <= 33.3 ms, p50 <= 16.7 ms | p50 **3.27 ms**, p95 3.57 ms, **p99 3.89 ms** (257 FPS at p99), max 4.73 ms over 1,230 frames |
| Benchmark path, GPU per pass | info | | p50/p99 ms: shadow 0.048/0.052, depth 0.048/0.088, SSAO 0.430/0.466, scene 0.407/0.481, post 0.397/0.438, total 1.319/1.499 (the outline not drawn on the path) |
| Benchmark path, GPU memory | passed | all <= 512 MiB | collection arrays 76.7 MiB (BC1, 2,000 items), all textures at most 252.2 MiB |
| Focus watch (whole gate) | passed | no finding | 109 foreground changes, 0 to test processes; 100 windows shown, 0 visible of test processes, 2 hidden or off-screen (the fake emulator's) |

The building fixture's rides in the GPU cases (`gpu.world elevator: ...`, lockstep, Release): the view back 212 to
449 ms after the request, the covers in view of the destination room (40 in room:psx, 30 in room:gb) resident at 143
and 362 ms, the room whole at the same frame.

**Run 2**, `./scripts/gate.ps1 -Gate C -RealLibrary` (72 steps, 5,197 s): FAILED on one step, the real library's tests:
the building's test passed, the SNES room's (`[room-real]`) failed two checks that predate the building's update. Since
I4 step 6b its `refresh` run's building job enters every other system of the library after the refresh (the catalog
held snes alone): the floor shown (gen4) then holds 1,671 items, not the plan's 19,985 placements (`world.items ==
world.placements`), and the plan file changes (its SHA-256 compared with the first plan's). A test defect, fixed at
this step (`tests/unit/app/test_room_real.cpp`): every placement of the floor shown is drawn (that floor's count), and
the SNES room's content digest (ADR-0013 §5) is unchanged by the refresh, its reconciliation moving nothing (run 2:
`rebuild`, 890 kept, 0 moved, 781 added in the gen4 rooms that entered). Every other row of run 2 passed, among them 0
differences among the snapshots' 542,120 entries; its logs are in `build/gate-c-run2`.

**Run 3**, `./scripts/gate.ps1 -Gate C -RealLibrary` at commit 27b9ced, after the fix (72 steps, 5,133 s): **PASSED**,
0 failed, 0 skipped. Its fixture rows are run 1's (the same counts, every golden 0 pixels apart in Debug and Release;
the benchmark path's p50 3.29 ms, p99 4.81 ms, 208 FPS at p99; 252.2 MiB at most); the focus watch saw 0 foreground
changes and 2 windows shown, both hidden or off-screen (the fake emulator's). The real library's rows, ES-DE closed
(ES-DE 3.4.1, 45 gamelists; the catalog of 105,130 entries, 90,246 games; the building of 44 rooms on 9 floors,
19,985 placements; logs in `build/gate-c`):

| Step | Result | Limit | Details |
|---|---|---|---|
| Real library: snapshots (NFR-10) | passed | 0 differences | **0 differences among 542,120 entries** (the ES-DE home and install hashed, the ROM and media roots listed) |
| Real library: `[building]` and `[room-real]` tests, Release | passed | | 322 assertions in 2 test cases |
| Real building: planned twice | passed | one SHA-256 | the first start and `again` (the plans' folder removed) generated one plan: the same digest over the nine runs and the same file SHA-256 (the test compares them; never written here); the jobs 3.11 s and 3.02 s (the snapshot of every system 0.99 s and 0.93 s, the planning 1.5 s) |
| Real building: unchanged restart | passed | skipped, <= 3 s | the building job skipped by its stamp in **0.45 s** at both starts, nothing written; the whole job on the unchanged library, its stamp removed: 2.68 s |
| NFR-01 to the lobby | passed | < 5,000 ms | the stored lobby shown **2,066 ms** and 1,976 ms after the process started (its preview built in 0.4 s), the quick catalog's first page 1,190 ms and 1,061 ms (90,246 games) |
| Real building: the busiest floor's GPU memory | passed | <= 512 MiB | the computer floor (10 rooms, 5,314 items, the most placements of the 9 floors): at most **407.3 MiB** cold and warm (collection arrays 210.9 MiB, BC1) |
| Real building: rides cold | passed | view back <= 10 s | the derived covers emptied, the lobby to the computer floor's largest room (1,646 items) through the elevator: the view back **2.50 s** after the request (`timed-out`: the floor built at 0.68 s, 150 of the room's 1,646 covers in view resident by then, 1,496 still coming; 9.51 s in I4 step 7b), 407.3 MiB; back to the lobby's elevator 1.33 s |
| Real building: the room after the cold ride | passed | <= 30 s | the room whole **16.65 s** after the request |
| Real building: rides warm | passed | view back <= 3 s | the same ride warm: the view back **1.80 s** (`arrived`: the floor built at 0.68 s, the covers in view resident at 1.55 s), 407.3 MiB; back 0.82 s |
| Real building: the room after the warm ride | passed | <= 3 s | the room whole 1.55 s after the request |
| Real room: the plan generated twice | passed | one SHA-256 | 890 placements (44 featured, 12 band, 834 spine), 6,907 unplaced; 18 lights, 35 clusters; one plan digest in the cold, again and restart runs |
| Real room: restart after a second import | passed | NoRebuild, 0 moves | unchanged, `no-rebuild`, the plan not written; the reconciliation `textures`: 890 kept, 0 moved |
| Real room: time to the room | passed | restart <= 5 s | shown 1,282 ms (restart), 1,775 ms (cold, the plan generated), 1,416 ms (again) after the process started |
| Real room: covers resident, cold / warm | passed | 30 s / 3 s | **7.88 s** cold (1,780 layers derived and stored; 23.7 s at Gate B on a busier machine), 0.43 s and 0.54 s warm |
| Real room: frames while covers load | passed | p99 <= 50 ms | coverLoad p99 7.55 ms cold, 6.25 and 15.92 ms warm |
| Real room: benchmark path | passed | p99 <= 33.3 ms, p50 <= 16.7 ms; 512 MiB | p50 3.34 ms, p99 **4.04 ms** (248 FPS at p99), max 4.96 ms; GPU total p50 1.35 ms, p99 1.52 ms; arrays 33.6 MiB, all textures at most 215.2 MiB |
| Real room: refresh of the library | passed | p99 <= 50 ms, 0 moves | the whole library imported while the room showed (105,130 entries, 97,295 added) in 14.8 s; the refresh phase 4,137 frames, p50 3.43 ms, **p99 5.03 ms**, max 333 ms; the room's live update `unchanged`, 1,671 kept, 0 moved; the SNES room's content digest unchanged |
| NFR-01 with the room | passed | < 5,000 ms | the quick catalog's first page **1,099 ms** after the process started (90,246 games), the stored room shown at 1,234 ms (the floor rebuilt at 21.0 s with the gen4 rooms that the refresh's building brought) |

The rides on the real library meet their rows with room to spare; cold, the view now comes back by the 3 s timeout
rather than with the room's covers in view: from the largest room's doorway every one of its 1,646 items meets the
view's frustum (the cover loader's view tests frusta, not occlusion), so the covers in view are the whole room, which
cold takes about 16 s to derive. The view is back at 2.5 s with the nearest and largest covers first, the rest arriving
while the walker looks (BACKLOG "The ride's covers in view").

## What step 8 changed

- **The ride's view comes back sooner** (decided at this step; ADR-0013 §10 and §13 amended on 2026-10-21). On the
  real library a cold ride to the computer floor's largest room stayed black 9.5 s (I4 step 7b) waiting for every
  cover of the room. The view now fades back in once the covers **in view** of the room that holds the eye are
  resident (`CoverSchedule::groupView()`: the cover loader's view items of that room's group, which it ranks first), or
  3 s after the request at the latest, whichever comes first (`RideSettings::viewTimeoutSeconds`; the wait ends 0.75 s
  before, the fade-in and a margin, so that the view is back by 3 s); the rest of the room, and of the floor, streams
  in afterwards. Under lockstep the frames wait for the covers in view instead, as before. The ride's record gains
  `viewItems` and `viewPending` (the covers in view at the view back) and `roomResidentMs` (the destination room whole,
  measured from the request: the acceptable row of the room that holds the walker). The acceptable rows stand (the
  view back within 3 s warm and 10 s cold); in practice a ride is now back within about 3 s.
- **The gate** (`scripts/gate.ps1 -Gate C`, above), a thin extension of Gate B's script; on the way, `-Benchmarks`
  now reaches the gate's inner run (the outer run did not pass it on), and the real room's row no longer prints its
  plan digest (it derives from the user's titles).
- **The real building's test** plans the building a second time from nothing (`again`) and checks the room that
  holds the walker after each ride; **the real SNES room's test** checks its refresh for a plan that holds the other
  rooms too (run 2, below).
- **Three goldens** of the building, below.

## The goldens of the building

Recorded like G02 to G10 (`tests/smoke/test_golden_room.cpp`; each with its JSON: the commands of the runs, the GPU
and driver, the commit, the SHA-256 of the image, the fixture's tree, the plan shown and `assets/world`), from
`esde/building` under the shipped threshold (no `world.json`), the app's first run planning the building and filling
the cache, the golden's run showing the stored plan at its first frame, lockstep at 1920 x 1080, the hero CRT's clip
held at 1.0 s (`--video-freeze-pts 1`). 0 pixels apart in Debug and Release on the reference machine; skipped on
another GPU or driver. Each was looked at:

| Golden | View | What it shows | Verdict |
|---|---|---|---|
| G11 `g11_lobby_golden.png` | no `--start-room`: the floor scene's camera at the elevator's arrival point, facing east | The 6 m wide lobby with its oak floor, plain dark walls and one ceiling light in view; on the north side the arcade game room's open doorway, its floor lamp lit inside; the closed entrance doors (two wooden leaves) at the east end. No CRT is bound in the hall. | Functional, nothing broken: the hub reads as a hall with a way into the arcade room and the way out; dim (the hall's pendant style at a 6 m pitch), as I4's functional look accepts (BACKLOG "The halls"). |
| G12 `g12_jewel_case_room_golden.png` | `--start-room room:psx`: the room's doorway camera on gen5 | The jewel-case room under its "CD HOME" sign: the 40 titles all face-out (fewer than the display slots and the hero band hold) as CD jewel cases with their square and portrait covers fitted to the square label, in the two lit display cases, the featured wall shelf and the hero band; the hero modules' other levels dressed (storage boxes, magazines); the CRT playing the clip held at 1.0 s with the console beside it; armchair, rug, lamps, posters, plant. | Functional and coherent with the SNES room's look (I3's template and theme); every cover in place, nothing interpenetrates. |
| G13 `g13_shared_room_golden.png` | `--start-room area:shared:handheld`: the room's doorway camera on the handheld floor | The handheld floor's shared room under its "MORE GAMES" sign: pocketlab's 6 and pokemini's 8 titles as generic cases, face-out with their covers, ten on the featured wall shelf right of the sign and four on the back tier of the left display case (`fx:dc01`, level 1, slots 1 to 4: the plan's 14 placements); the display cases' other spans carry the template's riser cards (I3 step 16: a card as wide as the empty slots, with an invented cover: "PIXEL PILOTS" on the left case's front tier, "TURBO TIDES" and "CRYSTAL CANYON" on the right case, which holds no game); no console beside the CRT (a shared room has none), which shows its colour bars (no section has a clip). | Functional: a shared room reads as a room of its own, its 14 titles in place, nothing interpenetrates. Among so few titles the riser cards' invented covers can be taken for games (BACKLOG "Shared and arcade rooms", I6). |

## Where ADR-0013's decisions are proven

| ADR-0013 | Decision | Where it is proven |
|---|---|---|
| §1 | Floors as separate scenes, `building.json` in elevator order, the ground floor always | `world/building: building.json and systems.json as shipped`, `world/floor: ...`, `gpu.world floor: ...`, G11; the real building's floors |
| §2 | Every ES-DE system classified, the platform-tag fallback, `excluded` | `world/building: systems.json classifies every system of ES-DE 3.4.1`, `world/building: classify() by the table, the platform tags, the fallback`, `esde/building_fixture: ...` (pocketlab by gb's tag, kodi excluded) |
| §3 | Own room or shared section by distinct titles, arcade sections, sticky | `world/shared rooms: the count rule ...`, `world/shared rooms: a system crossing the threshold moves nothing ...`, `world/building plan: a stored room's recorded family and theme pack are ...`, G13 |
| §4 | Media families with their items and fixtures, covers fitted to the label quad | `world/families: ...`, `assets/families: ...`, `app/cover_requests: a family's hero picture ...`, `gpu.world families: ...`, G12 |
| §5 | Schema 2, the lift, ids, digests, compatibility per room | `world/plan_json: ...`, `world/lift: every v1 oracle lifted and updated by 1.3 with its selection`, `world/ids: the room ids' four forms ...`, `world/update: a room of another template version or prefab levels is kept ...`, `app/world_build: a stored schema-1 plan is lifted ...`; the real building planned twice with one SHA-256 (gate) |
| §6 | Collection rooms on one template, the derived hall, the lobby, the elevator stand-in, open doorways | `world/floor: the hall is derived from its rooms ...`, `world/floor: a longer hall appends lights and moves none`, `world/floor: each room's scene moved onto the floor, its doorway open ...`, `gpu.world floor: a walk from the elevator along the hall into a room ...`, G11, G13 |
| §7 | Two rows, x fixed at entry, z following depth, full floors, frames | `world/building: rooms enter the shorter row at its east end ...`, `world/building: a growing room keeps its x ...`, `world/building: full rows take no room ...`, `app/world_build: the start room's frame ...`, `app/world_live_plan: the walker kept in the frame of the area that holds it ...` |
| §8 | Expansion rooms reserved, built only if a reference system overflows | `world/ids`, `world/plan_json: shared, arcade and expansion rooms round-trip byte for byte`; I4 step 2's census: none overflows (`docs/i4/step-2.md`), so they are I4b's |
| §9 | The building job and the floor-build job, one floor resident, the cover loader's view, picking per room, areas and lights, the hero CRT of the eye's room, memory, the start | `app/world_build: ...`, `app/room_snapshot: the building's snapshot ...`, `app/cover_requests: on a floor the view ...`, `scene/areas: ...`, `render/light_packing: ...`, `app/screen_room: ...`, `gpu.world floor app: ...` (every case), `gpu.world floor: areas: ...`; the real building's NFR-01 to the lobby, unchanged restart and busiest floor (gate) |
| §10 | The directory, the elevator's prompt, the ride, teleports, "Mostra nella stanza" across floors (amended at step 8: the view back once its covers in view are resident, or 3 s) | `ui/directory: ...`, `app/directory_data: ...`, `app/elevator_ride: ...`, `app/elevator_prompt_document: ...`, `catalog_db/recent_query: ...`, `gpu.directory: ...`, `gpu.world elevator: ...` (nine cases); the real rides (gate) |
| §11 | The building's validation rules | `world/validation: the building's rules`, `world/building suite: NFR-08 over random sequences of a building's updates` (no hard finding after any step) |
| §12 | `worldplan generate`, `update`, `show --floor --scene`, `where-is` with the floor | `cli/worldplan: a building of selection files, its floors, where-is with the floor`, `cli/worldplan: the building fixture's whole building, then its additions`, `cli/worldplan: a stored building is regenerated whole ...`, `cli/where-is: ...` |
| §13 | The acceptable rows of a ride and of the room that holds the walker | the real rides and the room after them (gate); `gpu.world elevator: a ride from the ground floor to gen4 and back ...` in Release |

## Where M3 part 1's requirements are proven

M3 (specification §22): "Room graph connesso, più sistemi, più modelli di media, postazioni e console. Hub, porte,
collisioni, streaming stanza, override e editor leggero. Preset tematici e fallback per sistemi non mappati." I4 takes
the building and leaves the overrides, the editor and the regeneration to I4b (ADR-0013, "Deferred"); its gate asks
for "the whole library in one building, walkable floor by floor, functional look" (roadmap, I4).

| Requirement | Where it is proven |
|---|---|
| A connected room graph (spec §7.3, §21.3 as amended: every room reachable from its floor's elevator, every floor from the ground floor) | `world/validation: the building's rules` (`floor-unreachable` on the floor's navigation grid, `doorway-off-hall`, `hall-overlap`), `world/floor: the walker walks from the elevator along the hall into a room`, `gpu.world elevator: ...` (floors reached through the directory) |
| More systems, one room per console, small systems sharing (spec §7.4 as amended) | `world/building plan: several systems on their floors ...`, `world/shared rooms: arcade systems in the arcade room, small systems in their ...`, the real building (every system with a placeable title in a room) |
| More media models | the families of ADR-0013 §4 (`world/families: ...`, `gpu.world families: ...`), G12 |
| Stations and consoles | each console room's console on its hero corner (I3's template), none in shared and arcade rooms (`world/validation`'s console rule by kind), G12 against G13 |
| A hub, doors, collisions | the lobby (G11, `gpu.world floor app: without --start-room the run starts in the ground floor's lobby ...`), the open doorways and the hall's walls that collide (`world/floor: the hall is derived from its rooms ...`, `gpu.world floor: a walk from the elevator ...`) |
| Room streaming (spec §14.4 as amended: the floor is the unit) | one floor resident, given back on a ride (`gpu.world elevator: a ride ...`: each floor's GPU memory after a ride a direct run's), the busiest real floor within 512 MiB (gate) |
| Fallbacks for unmapped systems, theme presets | `world/building: classify() by the table, the platform tags, the fallback` (a custom system by its platform tag, else `other` with `generic_case`), `esde/building_fixture` (pocketlab), the families' theme packs (`world/room_look: ...`) |
| The world starts in the lobby (spec §13.2 as amended) | `gpu.world floor app: without --start-room ...`; NFR-01 to the lobby on the real library (gate) |
| FR-005, FR-007, FR-008 for the building | `world/building plan: the golden building of five systems`, `world/floor: golden digests of every floor's scene, two runs alike`, `world/building update: the golden update sequence ...` (Debug equal to Release); the real building planned twice with one SHA-256 (gate) |
| FR-009, NFR-08 for the building | `world/building suite: NFR-08 over random sequences of a building's updates`, `world/building update: ...`, `gpu.world floor app: a system entering the resident floor ...` (nothing moved) |
| FR-013 across the building | "Mostra nella stanza" across rooms and floors (`app/directory_data: Mostra nella stanza across rooms and floors`, `gpu.world elevator: Mostra nella stanza of a game on another floor ...`), `where-is` with the floor |
| NFR-01 | the stored lobby and the quick catalog's first page within 5 s on the real library (gate) |
| NFR-10 | the real library's snapshots before and after, 0 differences (gate) |

## Follow-ups

Gathered in `BACKLOG.md`, "Follow-ups from I4": expansion rooms and per-room regeneration (I4b); `worldplan check` and
old template versions for kept rooms; a system waiting for a held room; a floor's build behind its cold covers; the
building stamp's misses; the ride's covers in view (a frustum holds a whole room from its doorway); a background
warm-up of the building's covers; the favourites query's scan; the halls' look,
light through doorways, the families' faces and the shared and arcade rooms' styling (I6); GPU cases of a room that
grows and of the ride's refusals.

## Try the building

On the reference machine or any Windows 10/11 PC with an OpenGL 4.5 GPU, with ES-DE closed. Retroverse only reads the
library: everything it writes (the catalog, the building's plan, the derived covers, the logs) goes to the data
directory you choose, which may not lie inside ES-DE's folders (the programs refuse one that does).

```powershell
./scripts/build.ps1 -Preset msvc-x64-release                                       # once
$data = 'D:\RetroverseData'                                                         # any folder outside ES-DE's
./build/msvc-x64-release/bin/retroverse-cli.exe --data-dir $data library create     # finds ES-DE as ES-DE finds itself
# or: ... library create --home <dir> --install <ES-DE's install folder>, where <dir> holds ES-DE's home folder ES-DE
#     (as ES-DE's own --home takes it: C:\Users\me for C:\Users\me\ES-DE)
./build/msvc-x64-release/bin/retroverse-cli.exe --data-dir $data import             # the catalog (again to refresh)
./build/msvc-x64-release/bin/retroverse.exe --data-dir $data --fullscreen           # the building, from the lobby
./build/msvc-x64-release/bin/retroverse-cli.exe --data-dir $data worldplan show     # its floors and rooms as text
./build/msvc-x64-release/bin/retroverse-cli.exe --data-dir $data where-is <GameId>  # where a game stands
```

The first start plans the building (a few seconds) and shows the lobby; the covers are derived while you walk (a
floor's the first time you visit it), and later starts show the stored lobby at once. `--start-room room:snes` (or
`area:arcade`, `area:shared:handheld`, any room id `worldplan show` lists) starts at that room's doorway. Games launch
with ES-DE's own commands from a PowerShell where `RV_LAUNCH_ALLOWLIST` is not set.

Controls (keyboard and mouse, or a pad):

- **Walking:** WASD or the left stick (Left Shift sprints), the mouse or the right stick to look; a click captures the
  mouse.
- **The elevator:** facing its doors in a floor's hall a prompt shows; E (pad A) opens the directory on the floors.
  Choose a floor (its hall's row rides to its elevator) or one of its rooms (rides there and stands you at its
  doorway): the view fades out, the floor changes, and the view comes back within a few seconds at most, the last
  covers arriving while you look.
- **The directory anywhere:** M (pad Back/Select) in free roam; its second tab, "Preferiti e recenti" (the shoulders,
  or Left and Right on the tabs), lists your ES-DE favourites and recently played games, each with its card: Avvia and
  "Mostra nella stanza", which rides to its floor and walks you to the item.
- **The items:** point at one to read its title; E, a click or A takes it (Focus: the D-pad steps along the shelf),
  again inspects it with its card (Avvia, Vedi media, Informazioni); the room's CRT plays the clip of the item you hold.
- **The quick catalog:** Tab (pad Y) searches the whole library; "Mostra nella stanza" on a game's card goes there,
  across floors.
- **Back:** Esc (pad B) leaves one level (a card, the directory, Inspect, Focus, then the mouse; then it quits).
