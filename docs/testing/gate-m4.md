# Gate M4: every room has working TVs within the decoder budget

Gate M4 closes iteration I5, screens and media ([ADR-0016](../decisions/ADR-0016-screens-and-media.md)): the decoder
scheduler (2 decoders in Balanced, 1 in Low, priorities, visibility, shared textures, staggered uploads, the priority
boost), playlists, secondary TVs, the arcade's cabinets playing their clips, the clips' sound (miniaudio, spatial, one
dominant source), CRT presets and the picture's aspect, fallbacks and video verdicts, videos and manuals in the media
viewer and the insertion animation. Its milestone is M4: "every room has working TVs within the decoder budget"
(roadmap, I5). What each step built is in [docs/i5/](../i5/). This page says what the gate checks, how to run it, the
figures of its runs on the reference machine, where ADR-0016 and M4's requirements are proven and the follow-ups. How
to try the screens and the sound:
[running-the-app.md](../reference/running-the-app.md#try-the-screens-and-the-sound).

**Status: every M4 row passed, the user's trial pending** (October 31, 2026, reference machine, the machine not
quiet: below). Run 1 (the full gate with the real library) failed one row on the gate's reading of a test's output (a
printed line cut by the next test's log, fixed by 5c60283c; the test had passed); run 2 (`-TimingOnly -RealLibrary
-Soak`) failed one assertion of Gate D's real building case, a race of the ride's record with the hub's showcase
under load that passed in run 1 and again when run alone (BACKLOG). Every I5 row, every golden, every timing row, the
soak, the real library's 0 differences and the focus watch passed in both runs. `-FullVideoScan` was not run at this
gate (I5 step 7's whole-library scan stands: 32,806 of 32,809 clips playable).

## What the gate checks

`scripts/gate.ps1 -Gate M4` runs, under `scripts/focus_watch.py --watch build` (a test process taking the foreground
or showing a window where the user sees it fails the gate), everything Gate D runs ([gate-d.md](gate-d.md)):

- **Gate A's build matrix**: `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release`, a compiler, linker or
  CMake warning failing its row.
- **The unit tests and the GPU smoke tests** of Debug and Release (ctest's `unit` and `gpu` labels, the goldens apart).
- **Every golden**, G01 to G20, in Debug and Release, a row per golden with the verdict its case writes; a golden the
  run does not report, or one that skips, fails its row (as at Gate M3). I5 added G19 (a console room with both screens
  frozen) and G20 (a cabinet playing in Focus); its steps regenerated the goldens where a secondary TV stands, G15, G17
  and G20 on purpose.
- **Gate C's I4 rows and Gate D's I6 rows**, per Debug and Release, counted from the unit and GPU runs (`$i4Areas`,
  `$i6Areas`).
- **The I5 checks**, per Debug and Release, one row per area counted from the unit and GPU runs (`scripts/gate.ps1`'s
  `$i5Areas`; a case counts in one I5 area only). An area (I4's, I6's or I5's) with a skipped GPU case fails, as a
  skipped golden does:

  | Area | Unit cases | GPU cases |
  |---|---|---|
  | Playlists and fallbacks | `app/playlist`, `app/screen_policy` (but the insertion's title card), `app/video_settings` | `gpu.video` (8 cases) |
  | Scheduler | `app/screen_scheduler`, `app/process_priority`, `platform_win/process_priority` | `gpu.screens` (but the aspect's), `gpu.world launch: on the product's video settings, ... both stop before the hand-over ...` |
  | Secondary TVs | `world/secondary TV` | `gpu.gpu.secondary_tv` (4 cases) |
  | Cabinets | `app/cabinets`, `app/screen aspect` | `gpu.cabinets` (10 cases), `gpu.screens: a 16:9 clip on a 4:3 tube ...` |
  | Audio | `audio/*` (the ring, the mixer, the spatial table, the sync, the engine), `media/audio`, `app/audio_policy` | `gpu.audio` (4 cases) |
  | Presets | `assets/crt_presets`, `app/crt_looks` | `gpu.crt presets` (2 cases) |
  | Verdicts | `media/video_verdicts`, `media/video_check`, `cli/media --check-videos`, `app/screen_verdicts` | `gpu.video verdicts` (2 cases) |
  | Viewer | `media viewer: the videos page ...`, `media viewer: one explorer.exe ...`, `launcher/manual_open` | `gpu.media viewer`'s four video and manual cases |
  | Insertion | `app/world_interaction`'s insertion cases, `app/screen_policy: the title card forced for the insertion ...` | `gpu.world launch: Avvia on the card puts the item into the console ...`, `... a button skips the insertion ...` |

- **The benchmark path** in Release (Gate B's: the snes-stress room of 2,000 titles at 1920 x 1080, `--no-vsync`)
  against the acceptable limits, its GPU passes informative, and since this gate **its `two-clips` phase**
  (`tests/input_scripts/bench_snes_room.json`, frames 64 to 115): the pan that holds the hero CRT and the room's
  secondary TV in view, the gate checking from the run report's slot events that both decoder slots played on every
  frame of it, against ADR-0016 §12's row (p50 16.7 ms, p99 33.3 ms).
- **With `-RealLibrary`** (ES-DE closed; the folders from `-EsdeHome`, `-EsdeInstall`, `-RomRoot`, `-MediaRoot` or the
  `RV_REAL_*` variables; generator 1.3's `retroverse-cli.exe` from `-LegacyCli` or `RV_LEGACY_CLI` for Gate D's
  migration rows): one `scripts/real-library.ps1` run, `RV_AUDIO=null`, read-only snapshots before and after with 0
  differences, `RV_LAUNCH_ALLOWLIST` naming the build's fake emulator alone, of

  | Filter | Test | What its rows read |
  |---|---|---|
  | `[real-library][screens]` | `tests/unit/app/test_screens_real.cpp` (new) | every room of the real building that holds games started at its doorway (`--start-room`) for 240 lockstep frames: its hero screen on a clip or its chain's still at the end, never standby once a picture showed, at most 2 decoders open and 2 decoder threads; NFR-06: each clip's first frame within 2 s of its decoder's start |
  | `[real-library][secondary_tv]` | `tests/unit/app/test_secondary_tv_real.cpp` | the busiest floor's largest room with a secondary TV at 1920 x 1080, both slots playing, cold and warm: GPU memory at its peak (512 MiB) |
  | `[real-library][arcade]` | `tests/unit/app/test_arcade_real.cpp` | attract in front of each featured cabinet of area:arcade, the first that played inspected with its clip in the hand |
  | `[real-library][audio]` | `tests/unit/media/test_audio_real.cpp` | a 96 kHz SNES clip played 8 s: its sound against its picture, p95 within 80 ms |
  | `[real-library][videos]` | `tests/unit/cli/test_cli_media_videos_real.cpp` | the SNES clips' verdicts through `retroverse-cli media --check-videos`, checked from nothing (the gate removes the case's kept data directory first): every clip a verdict |
  | `[real-library][building]`, `[real-library][room-real]` | `tests/unit/app/test_building_real.cpp`, `tests/unit/app/test_room_real.cpp` | Gate D's rows: the building planned twice, the unchanged restart, NFR-01 to the lobby, the busiest floor's GPU memory, the rides cold and warm, generator 1.3's building migrated once and re-placed onto the hub, the real SNES room |

  Every limit is written in the script (`$adr16FrameP50Ms`, `$adr16FrameP99Ms`, `$adr16GpuMiB`, `$adr16SyncP95Ms`,
  `$nfr06FirstFrameMs`, `$nfr07Decoders`, `$soakGrowthMiB`), never read from a figure a test prints.
- **`-FullVideoScan`**: the `[videos]` case checks every system's clips instead of the SNES ones (about two hours with
  3 jobs, from nothing).
- **`-Soak`**: `rv_smoke_tests "[soak]"` in Release, last (`tests/smoke/test_soak.cpp`: `tests/smoke/scenes/
  screens_four.json`'s four CRTs playing generated clips of a mock catalog for an hour in real time, each playlist
  moving on every 4 s, one clip cut short failing each time it plays; the process's private bytes, handles and threads
  sampled every 10 s into `memory.csv`): rows for the private bytes' growth after the first 5 minutes (the mean of the
  last 5 minutes against minutes 5 to 10, below 32 MiB, the case's own rule), their least-squares slope from minute
  10, the handles, threads and decoders; `memory.csv` copied to `build/gate-m4/soak-memory.csv`.
- **`-TimingOnly`**: the Release build, the benchmark path and, with `-RealLibrary`, the real library's run with its
  correctness rows: a quiet-machine rerun. **The gate's verdict is a full run plus a `-TimingOnly -RealLibrary` run and
  the soak on a quiet machine.**

The gate sets `RV_LAUNCH_BACKGROUND=1` and `RV_AUDIO=null` for everything it runs (as Gate M3 does, it leaves
`RV_LAUNCH_ALLOWLIST` to the GPU cases' ctest `ENVIRONMENT` and to `scripts/real-library.ps1`). Every step logs into
`build/gate-m4` (the build and test logs, the JUnit and Catch2 reports, `results.json`, `focus_watch.json`,
`real-library.log` and `real-library/`, `soak.log` and `soak-memory.csv`).

## How to run it

```powershell
./scripts/gate.ps1 -Gate M4                                    # fixtures only
./scripts/gate.ps1 -Gate M4 -RealLibrary -LegacyCli <generator 1.3's retroverse-cli.exe>   # ES-DE closed
./scripts/gate.ps1 -Gate M4 -TimingOnly -RealLibrary -Soak     # the timing parts and the soak hour, a quiet machine
./scripts/gate.ps1 -Gate M4 -RealLibrary -FullVideoScan        # every clip's verdict (hours)
```

Close ES-DE first (`Get-Process ES-DE` must find nothing). The gate exits 0 when no step failed, 1 otherwise, 2 when it
cannot start. Generator 1.3's CLI is built from I6 step 2's commit as [i6/step-4.md](../i6/step-4.md) says.

## The figures of the runs

October 31, 2026, on the reference machine ([reference-hardware.md](reference-hardware.md): Intel Core i7-9700K, 8
cores, 64 GB, RTX 2080 SUPER with OpenGL 4.5.0 NVIDIA 621.01, Windows 11 Home 10.0.29671), branch `i5/step-10` (run 1
at 585a2604, run 2 at 5c60283c: the same gate and product code, one test's output flushed). ES-DE 3.4.1 closed, 45
gamelists; the catalog of 105,130 entries, 90,246 games; the building of 44 rooms on 9 floors, 19,985 placements
(nothing derived from the titles is written here). Logs in `build/gate-m4-run1` and `build/gate-m4-run2` (each run's
`gate.log`, `results.json`, `focus_watch.json`, `real-library/tests.log`; run 2's `soak-memory.csv` and `cpu-load.txt`).

**The machine was not quiet.** Before run 1 the CPU stood at 5 to 9%; another project's Debug build (8 compilers)
ran after it, and run 2 started once it had stopped, with a browser, editors and another project's tools running
beside it: sampled every minute (`cpu-load.txt`, our own processes included), the CPU's median was 100% during run 2's
real-library step (its own lockstep runs and the 2-job clip check among them) and 53% during the soak (its own
uncapped rendering at about 165 frames a second among them). Every timing row passed under that load; the correctness
rows are not affected by it.

### Run 1: `./scripts/gate.ps1 -Gate M4 -RealLibrary -LegacyCli <generator 1.3's CLI>`

128 steps, 13,035 s: **FAILED**, 1 row, on the gate's reading of a test's output alone (below); every check of every
test passed.

| Step | Result | Limit | Details |
|---|---|---|---|
| Build `msvc-x64-debug`, `-relwithdebinfo`, `-release` | passed | 0 warnings | 0 warnings each (RelWithDebInfo configured and built from nothing, 1,223 s) |
| Unit tests Debug / Release | passed | | 2,250 passed each (1,786 s / 487 s) |
| GPU smoke tests Debug / Release | passed | | 161 passed (4,644 s) / 158 passed, 1 skipped (the overlay's cursor case, as in every Release run; it lies in no area) |
| Goldens G01, G02, G02s, G06, G07, G08a, G08b, G10 to G20, Debug and Release | passed | 0.5%, none skipped | 36 comparisons: G01 73 of 2,073,600 pixels beyond 6/255 (0.004%, the memory margin's known difference), every other golden 0 pixels (largest difference 0/255) |
| I4 rows, Debug / Release | passed | | world building 76, floors 42, shared rooms 9, update suite 8, directory 41, elevator 21, floor app 26, start and live 27 passed each |
| I6 rows, Debug / Release | passed | | light 35, halls 15, theme packs 17, spines 14, era furniture 10, hub 23, arcade and handhelds 8 passed each |
| I5 playlists | passed | | 41 passed each (33 unit, 8 GPU) |
| I5 scheduler | passed | | 26 passed each (19 unit, 7 GPU) |
| I5 secondary TVs | passed | | 10 passed each (6 unit, 4 GPU) |
| I5 cabinets | passed | | 20 passed each (9 unit, 11 GPU) |
| I5 audio | passed | | 40 passed each (36 unit, 4 GPU) |
| I5 presets | passed | | 19 passed each (17 unit, 2 GPU) |
| I5 verdicts | passed | | 22 passed each (20 unit, 2 GPU) |
| I5 viewer | passed | | 10 passed each (6 unit, 4 GPU) |
| I5 insertion | passed | | 9 passed each (7 unit, 2 GPU) |
| Benchmark path, snes-stress (2,000 titles), frame times | passed | p99 <= 33.3 ms, p50 <= 16.7 ms | p50 3.19 ms, p95 4.06 ms, **p99 6.18 ms**, max 275.7 ms over 1,230 frames |
| Benchmark path, GPU per pass | info | | p50/p99 ms: shadow 0.047/0.053, depth 0.049/0.088, SSAO 0.416/0.942, scene 0.467/1.000, post 0.378/0.881, total 1.364/2.163 |
| Benchmark path, GPU memory | passed | all <= 512 MiB | collection arrays 76.7 MiB (BC1, 2,000 items), all textures at most 240.0 MiB |
| **Benchmark path, two clips playing** | passed | p99 <= 33.3 ms, p50 <= 16.7 ms | the `two-clips` phase, frames 64 to 115: p50 3.61 ms, p95 4.64 ms, **p99 5.22 ms**; both slots playing on 52 of 52 frames (the hero CRT and the secondary TV `room:snes/fx:w05`); over the path 4 decoders started, at most 2 open |
| Real library: snapshots (NFR-10) | passed | 0 differences | **0 differences among 542,121 entries** |
| Real library: the seven filters' tests, Release | passed | | 1,777 assertions in 8 test cases, all passed |
| **Real rooms: the hero screens** | **FAILED** | a clip or a still, never standby; <= 2 decoders | "43 collection rooms of the 44": the last room's line (room:nes) was cut in two by the next test's log output, so the gate's pattern missed it; the test itself checked all 44 and passed (its summary: 41 on a clip, 3 on a title card, 0 on standby). Fixed by 5c60283c (each line flushed whole); run 2 reads 44 of 44 |
| NFR-06 on the real rooms | passed | <= 2,000 ms | the hero's first clip frame at most **870 ms** after its decoder started (room:fds), median 26 ms, over 40 rooms |
| Real busiest floor, both screens playing | passed | all <= 512 MiB | room:amstradcpc on the computer floor (5,314 items), 1920 x 1080, two decoders: **477.5 MiB** at its peak cold, warm and with a screenshot; frame p99 7.92 and 7.75 ms |
| Real arcade: the cabinets playing | passed | clips on the cabinets | area:arcade, 36 cabinets; attract played on **6 of the 6** featured cabinets; the first inspected, its clip in the hand from frame 40 (interact at 40) |
| Real 96 kHz clip: sound against picture | passed | p95 <= 80 ms | AAC 96 kHz stereo, H.264 640 x 480, 8 s played: p50 14.3 ms, **p95 29.5 ms**, max 31.3 ms over 420 frames; 0 resyncs |
| Real videos: the SNES clips | passed | every clip a verdict | **1,470 clips checked from nothing in 632 s, 1,470 playable**, no failure of any kind |
| Real videos: every system's clips | off | | not run (`-FullVideoScan`; I5 step 7: 32,806 of 32,809 playable) |
| Gate D's real rows: the building planned twice, the unchanged restart | passed | one SHA-256; skipped, <= 3 s | one digest over 9 runs (jobs 3.58 and 3.34 s); skipped by its stamp in 0.52 and 0.63 s |
| NFR-01 to the lobby | passed | < 5,000 ms | the stored lobby **3,401 and 3,722 ms**, the quick catalog's first page 2,163 and 2,499 ms |
| Real building: the busiest floor's GPU memory (1280 x 720) | passed | <= 512 MiB | the computer floor: 451.7 MiB cold and warm |
| Real building: rides cold / warm, the room after them | passed | 10 s / 3 s; 30 s / 3 s | the view back 2.50 s (`timed-out`, built at 1.00 s) / **2.49 s** (`arrived`); the room whole 15.96 s / 2.24 s |
| Real building: generator 1.3 migrated once; re-placed onto the hub | passed | once, 0 over capacity | 25 rooms migrated, 19 others byte for byte, 0 over capacity; area:arcade re-placed in the same update |
| Real SNES room: Gate B's rows | passed | | the plan twice one SHA-256; the restart 0 moves; the room 2,142 ms; covers cold 8.6 s, warm 0.41 and 0.55 s; cover-load p99 24.1 ms at most; its benchmark path p99 4.62 ms; the refresh p99 6.46 ms, 0 moved; NFR-01 with the room 1,965 ms |
| Focus watch (whole gate) | passed | no finding | 382 foreground changes, **0 to test processes**; 264 windows shown, **0 visible of test processes**, 2 hidden or off-screen (the fake emulator's) |

None of the known load-sensitive Debug cases (the floor-app cases' 10 s lockstep wait, `app/world_flow`'s 60 s, the
cabinets' attract first frame) failed in this run.

### Run 2: `./scripts/gate.ps1 -Gate M4 -TimingOnly -RealLibrary -Soak -LegacyCli <generator 1.3's CLI>`

41 steps, 6,148 s: **FAILED**, 1 row: one assertion of Gate D's real building case, a race on a loaded machine
(below); every timing row and every other correctness row passed.

| Step | Result | Limit | Run 2 |
|---|---|---|---|
| Build `msvc-x64-release` | passed | 0 warnings | 0 warnings |
| Benchmark path, frame times / GPU memory | passed | p99 <= 33.3 ms / 512 MiB | p50 2.89 ms, **p99 4.26 ms**, max 304.3 ms; 240.0 MiB |
| **Benchmark path, two clips playing** | passed | p99 <= 33.3 ms, p50 <= 16.7 ms | p50 3.00 ms, p95 3.93 ms, **p99 6.22 ms** over the 52 frames, both slots playing on all 52 |
| Real library: snapshots (NFR-10) | passed | 0 differences | **0 differences among 542,121 entries** |
| Real library: the seven filters' tests | **FAILED** | | 7 of 8 test cases passed; the building's case failed one assertion (below) |
| **Real rooms: the hero screens** | passed | a clip or a still, never standby; <= 2 decoders | **44 collection rooms of the 44** (37 console, 6 shared, the arcade), each started at its doorway for 4 s: **41 on a clip, 3 on a title card** (area:shared:gen5, room:switch, room:trs-80: no clip, the chain's card), **0 on standby**, never standby once a picture showed; at most **2 decoders open and 2 threads** in any run |
| NFR-06 on the real rooms | passed | <= 2,000 ms | the first clip frame at most **626 ms** after its decoder started (room:atari7800), median 22 ms, over 41 rooms |
| Real busiest floor, both screens playing | passed | <= 512 MiB | **477.5 MiB** cold, warm and with a screenshot (procedural surfaces 84.9, collection arrays 210.9, scene textures 106.0, targets 60.2, buffers 6.5, hero textures 3.1 MiB); frame p99 7.69 and 6.90 ms |
| Real arcade: the cabinets playing | passed | clips on the cabinets | attract on **6 of 6** featured cabinets; the inspected one's clip in the hand from frame 40 |
| Real 96 kHz clip: sound against picture | passed | p95 <= 80 ms | p50 14.2 ms, **p95 29.2 ms**, max 31.3 ms; 0 resyncs |
| Real videos: the SNES clips | passed | every clip a verdict | 1,470 checked from nothing in 578 s, **1,470 playable** |
| Gate D's real rows | passed | as run 1 | planned twice (jobs 3.38 and 2.77 s); restart skipped in 0.52 and 0.56 s; **NFR-01 to the lobby 3,021 and 3,517 ms**, the first page 1,848 and 2,189 ms; the busiest floor 451.7 MiB; rides cold 2.50 s (`timed-out`) and warm **2.63 s**, the room 14.74 s and 2.11 s; generator 1.3 migrated once, re-placed onto the hub; the SNES room (the room 2,878 ms, covers cold 9.7 s, warm 0.18 and 0.49 s, its benchmark p99 5.28 ms, the refresh p99 7.64 ms, 0 moved, NFR-01 with the room 3,466 ms) |
| **Soak: the private bytes** | passed | < 32 MiB after the first 5 minutes | 3,590 s, 350 samples: **257.7 MiB (minutes 5 to 10) to 260.9 MiB (the last 5), +3.3 MiB**; peak 264.3 MiB; the slope from minute 10 +5.5 MiB an hour |
| Soak: handles, threads and decoders | passed | <= 2 decoders | after minute 5: handles 436 to 444, threads 12 to 16; 1,097 decoders started, at most 2 open, 4 fallbacks (the cut clip), 592,316 frames |
| Focus watch (whole gate) | passed | no finding | 642 foreground changes, **0 to test processes**; 281 windows shown, **0 visible of test processes** |

**The failed assertion** (`tests/unit/app/test_building_real.cpp`, line 567, the cold ride's return): the collection
arrays when the view came back to the ground floor were 2.4 MiB, against the 4.8 MiB a direct start of the lobby
holds. The ride's log shows why: the hub's showcase (its favourites' and recent games' proxies, 12 items in an array
of their own) was built 51 ms after the ride arrived (frame 26,427), so the record taken on arrival did not count it;
the run's end, checked by the next assertion, held the 4.8 MiB. In run 1 and in every earlier gate the showcase was
built before the arrival. It is a race of the record with the showcase's job under load, not a fault of the screens:
nothing of I5 touches the rides or the showcase. Run again alone right after (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, the
same load; logs in `build/m4-real-building`), the case passed whole (609 assertions in its 2 test cases, the cold
ride's return 4.8 MiB, 0 differences among 542,121 entries). The follow-up (below) is to take the ride's record once
the showcase is in, or to have the ground floor's arrival wait for it.

### The soak

`rv_smoke_tests "[soak]"` in Release, an hour (`tests/smoke/test_soak.cpp`), at 960 x 540 with no frame limiter
(about 165 frames a second: 592,316 frames), the four CRTs of `screens_four` switching clips every 4 s (1,097 decoders
started, two at most), the cut clip failing at each turn. `memory.csv` (`build/gate-m4-run2/soak-memory.csv`), the
private bytes' mean per 5 minutes:

| Minutes | 0-5 | 5-10 | 10-15 | 15-20 | 20-25 | 25-30 | 30-35 | 35-40 | 40-45 | 45-50 | 50-55 | 55-60 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Private bytes (MiB) | 250.4 | 257.7 | 256.2 | 257.5 | 258.1 | 258.7 | 259.9 | 258.6 | 259.6 | 260.2 | 260.1 | 261.0 |

Every sample after minute 5 lies between 244.7 and 264.3 MiB; handles 436 to 444, threads 12 to 16 (27 at most
during the start).

**The growth explained.** The first minutes fill what the run keeps by design: the first clips' frames and rings, the
procedural surfaces, and the run report's per-frame samples, exact for their first 65,536 frames (about 6.6 minutes at
165 frames a second) and a histogram of fixed size after them (`core::DurationRecorder`, I5 step 7's review); the
report's timelines (the slots' events, the pictures shown and played, the fallbacks) keep their first 512 entries
each. From minute 10 the means rise by 4.8 MiB in 50 minutes, a least-squares slope of 5.5 MiB an hour from minute 10
and 3.8 MiB an hour from minute 30: a slope that decreases, with samples that swing by 5 to 15 MiB as the decoders'
frame buffers come and go. Before I5 step 7's review the same soak grew about 110 MiB an hour: 60 to 80 KiB per clip
opened (a thread per clip, each costing the graphics driver 36 to 73 KiB) and 80 bytes a frame of samples. Here what is
left is at most 4.8 MiB over the about 920 decoders of minutes 10 to 60, about 5 KiB per clip, less than a tenth of
the old leak and of the order of the allocator's and the driver's fragmentation: no per-clip or per-frame growth of
the old kind remains. Telling the last few MiB apart (heap fragmentation from the decoders' frame buffers, the
driver) would take a census of the process's regions, as step 7's review made; it was not made here.

**The growth bounded.** The case's rule, the mean of the last 5 minutes against minutes 5 to 10, holds with +3.3 MiB
against 32 MiB. Taken as linear at its steepest (5.5 MiB an hour), a user's eight-hour day would add about 45 MiB to
about 260 MiB, and the soak is the heavier load: it renders uncapped at about 165 frames a second and switches clips
every 4 s, where the product's playlists move every 30 s.

## Where ADR-0016's decisions are proven

| ADR-0016 | Decision | Where it is proven |
|---|---|---|
| §1 | Screens and their kinds (hero, secondary, cabinet, the media viewer's) | `app/crt_looks: the hero is the first screen that binds, by its role ...`, `app/crt_looks: a scene's screens and their rooms`; the real rooms' hero screens, each its room's own (gate) |
| §2 | The scheduler: the budget, slots, priority classes, sharing, visibility, stability, late frames, launches, uploads, the priority boost | `app/screen_scheduler: ...` (17 cases, the budget over seeded runs), `gpu.screens: 3 clips on 4 screens play on 2 decoders, two screens sharing one texture`, `... a screen turned away pauses within a frame, stops after 10 s ...`, `... --max-decoders 1 ...`, `... two 720p clips due together never upload more than 4 MiB in a frame`, `... the process runs above normal while rendering ...`, `... minimized ...`, `gpu.world launch: on the product's video settings ... both stop before the hand-over ...`; every real room within 2 decoders and threads (gate) |
| §3 | Playlists: the hero's start choice then its playlist, a selection that interrupts it and gives it back, the held frame between clips | `app/playlist: ...` (8 cases), `app/screen_policy: ...` (19 cases), `gpu.video: the hero screen plays its playlist, holding the last frame between clips, never its pattern`, `gpu.video: a selection switches the clip after its dwell ...` |
| §4 | `video.json`: shipped defaults, the user's overrides, per kind, room and screen | `app/video_settings: ...` (5 cases) |
| §5 | Secondary TVs, a theme pack's furniture; the `ambient` class and `proximityMeters` | `world/secondary TV: ...` (6 cases), `gpu.gpu.secondary_tv: ...` (4 cases), G19; the real busiest floor with both slots (gate) |
| §6 | The arcade's cabinets: focus, inspect, attract, the hero cabinet's kept frame | `app/cabinets: ...` (6 cases), `gpu.cabinets: ...` (10 cases), G20; the real arcade (gate) |
| §7 | The picture's aspect and the CRT presets | `app/screen aspect: ...` (3 cases), `gpu.screens: a 16:9 clip on a 4:3 tube shows black bars ...`, `assets/crt_presets: ...` (7 cases), `app/crt_looks: ...`, `gpu.crt presets: ...` (2 cases), G15, G17, G20 |
| §8 | Fallbacks ending at the title card, the standby pattern, video verdicts, `media --check-videos` | `gpu.video: missing, empty, corrupt, truncated and unsupported files fall to the next picture`, `gpu.video: a game without media shows its title card ...`, `media/video_verdicts: ...`, `media/video_check: ...`, `cli/media --check-videos: ...`, `gpu.video verdicts: ...` (2 cases); the real rooms never on standby, the SNES clips' verdicts (gate); the whole library's (I5 step 7: 32,806 of 32,809 playable) |
| §9 | Audio: the null and offline mixer, one dominant source, spatial gain and pan, the sync to the picture, ducking, mute, the device around a launch | `audio/mixer: ...` (13 cases), `audio/spatial: ...`, `audio/sync: ...`, `audio/ring: ...`, `audio/engine: ...`, `media/audio: ...` (5 cases), `app/audio_policy: ...` (4 cases), `gpu.audio: ...` (4 cases); the real 96 kHz clip (gate); the user's listening (below) |
| §10 | The media viewer's clip (720 rows, `direct`, the world paused) and manual (Windows' PDF program through `rv_launcher`) | `media viewer: the videos page pauses its clip ...`, `media viewer: one explorer.exe at a time ...`, `launcher/manual_open: ...` (4 cases), `gpu.media viewer`'s four video and manual cases |
| §11 | The insertion animation: 450 ms into the console's slot, skippable, instant under `reduceMotion`, the hand-over after the decoders' stop | `app/world_interaction: Avvia in a room with a console eases the item into it in 450 ms ...` and its five other insertion cases, `gpu.world launch: Avvia on the card puts the item into the console ...`, `gpu.world launch: a button skips the insertion ...` |
| §12 | The acceptable rows: two clips playing, GPU memory with the screens playing, a clip's sound against its picture | the benchmark path's `two-clips` phase, the real busiest floor with both slots, the real 96 kHz clip (gate); the soak's memory (gate, `-Soak`) |

## Where M4's requirements are proven

| Requirement | Where it is proven |
|---|---|
| FR-015 (a 3D TV with local video: the MP4 decoded and shown as a texture) | `gpu.video: the hero CRT plays a generated clip at its rate, its memory accounted`, `gpu.video: --video-freeze-pts holds one frame, the same in two runs`; the real rooms' hero screens on their clips (gate) |
| FR-016 (video on the focus: the selected game's preview or a fallback) | `app/screen_policy: a selection becomes the target after 300 ms ...`, `gpu.video: a selection switches the clip after its dwell, stepping faster does not, never two decoders`, `gpu.cabinets: the inspected cabinet plays its clip in the hand ...`, `gpu.video: missing, empty, corrupt ...` |
| FR-017 (the decoder budget kept with screens out of view) | `app/screen_scheduler: never more decoders than the budget nor threads than slots, over seeded runs`, `... a slot out of view pauses at once and stops after 10 s`, `gpu.screens: a screen turned away pauses within a frame, stops after 10 s ...`, `gpu.screens: --max-decoders 1 ...`; the real rooms and the soak within 2 (gate) |
| FR-030 (spatial video volume, independent levels and mute) | `audio/spatial: the gain table ...`, `audio/spatial: the pan from the azimuth ...`, `app/audio_policy: ...`, `gpu.audio: walking away lowers the level as the gain table says`, `gpu.audio: F8 mutes and sounds again; the quick catalog ducks the sound`, `app/video_settings: the shipped defaults hold every key ...` (`audio.master`, `video`, `effects`, `ambience`, `muted`) |
| NFR-06 (from the focus to the preview's first frame within 2 s) | the real rooms' first frames (gate, `NFR-06 on the real rooms`); `gpu.video: a selection switches the clip after its dwell ...` (the dwell of 300 ms, then the decoder) |
| NFR-07 (at most 2 decoders at once in the normal profile, a runtime assertion) | the scheduler's assertion on the budget (`ScreenService`, Debug), `app/screen_scheduler: ... over seeded runs`, every smoke run's report checked for its decoders, `gpu.screens: 3 clips on 4 screens play on 2 decoders ...`; the real rooms, the real busiest floor, the soak (gate) |
| SC-03 (a CRT's playlist interrupted by a focused cartridge, its clip or a fallback shown, no crash on an unsupported file) | `app/screen_policy: a selection interrupts the playlist and gives it back after its clip`, `gpu.video: a game without media shows its title card, and the playlist comes back after it`, `gpu.video: missing, empty, corrupt, truncated and unsupported files fall to the next picture` |
| SC-08 (a missing, empty or corrupt MP4 marked unplayable, a screenshot or cover shown, the 3D going on; a new scan possible) | `gpu.video: missing, empty, corrupt, truncated and unsupported files fall to the next picture`, `gpu.video verdicts: a clip that failed is never opened again, its screen on its still from the start`, `cli/media --check-videos: each damaged kind of clip gets its verdict`, `cli/media --check-videos: resumes, checks a changed clip again ...` (`--recheck-failures`); the real SNES verdicts (gate) |
| Specification §9 | §9.1: every real room that holds games has an active display (gate, `Real rooms: the hero screens`), the secondary CRT (§5), the arcade monitor (§6), the "monitor moderno" as the optical-era lounge's `monitor` preset (G17); §9.2: the playlist modes (`app/playlist: each mode's order, cycle after cycle`) and the per-display parameters (`video.json`, `app/video_settings`); §9.3: the playlist on entering, the focus, the fallbacks (no "Scarica anteprima": ADR-0003), the frame kept out of view (`gpu.video: out of the view the decoder pauses, the picture kept`), every decoder and the sound stopped for a launch and resumed after it (`gpu.world launch: on the product's video settings ...`, `audio/engine: the launch's fade ...`, `gpu.video: the decoder is stopped and joined before a launch and reopened at its position ...`), one audible screen (`gpu.audio: of two screens only the audible one is in the mix ...`), the priority classes (§2); §9.4: I1's and I3's pipeline (`media/video_decoder`, `media/video_playback`), the frames padded past their last row (I5 step 7); §9.5: 2 and 1 decoders, 480 and 720 rows, stills out of view, the stop after 10 s, no prefetch (ADR-0016), shared textures (`gpu.screens: 3 clips on 4 screens ...`), stills when the budget is spent (`gpu.screens: --max-decoders 1 ...`); §9.6: the four presets and `crtEffect` (`gpu.crt presets: ...`); §9.7: the clips' stereo through FFmpeg and miniaudio, the distance, ducking, mute, the master and video levels (§9); interaction sounds and ambience are I8's (ADR-0016's amendment) |

## Follow-ups

Gathered in `BACKLOG.md`, "Follow-ups from I5":

- **The ride's record against the hub's showcase** (run 2's one failed assertion, above): take the record once the
  showcase is in, or have the ground floor's arrival wait for it.
- **Rooms without clips** (area:shared:gen5, room:switch, room:trs-80) end on the title card of their first game;
  preferring a game with a still file would show a picture where the room has one.
- **The soak's last few MiB** (a slope of 5.5 MiB an hour falling to 3.8) are not itemised; a census of the process's
  regions would tell the allocator from the driver.
- **The two-clips phase is tied to frames** of the benchmark path; the gate checks it against the slot events.
- **The Debug cases close to their time limits under load** (the floor-app cases' 10 s lockstep wait,
  `app/world_flow`'s 60 s, `gpu.cabinets: attract ...`'s first frame): none failed in run 1; they stay in BACKLOG.
- **`RV_LAUNCH_ALLOWLIST` for the unit run** (Gate M3's), unchanged: the gate leaves it to the GPU cases and the real
  library's run.
- From the steps' notes, unchanged: the user's listening (I5 step 5: the sound in sync, the 96 kHz clips' pitch, the
  emulator's own sound after a launch, also in exclusive mode), the arcade's horizontal clips letterboxed on the
  cabinets' portrait screens (step 4), the rooms whose decoration drew no sideboard for a secondary TV (step 3), the
  late-frame drop proven by the unit tables only (step 2), the sound over the insertion (step 9), a disc console's own
  tray (step 9); ADR-0016's "Deferred" (in-app manual pages, the quick catalog's video, interaction sounds and
  ambience, phosphor persistence, LCD and projection screens, hardware decoding, more than 2 decoders, the hub's
  screens playing).
