# I3 step 17b: the benchmark path and the real room

Roadmap row: I3 step 17b (after 15b and 16, beside 17a). The budgets of the roadmap's "Budgets" table that this step
first measures: the frame time on the benchmark path (p99 at most 16.7 ms, NFR-03's 60 FPS; p50 at most 8 ms) and the
GPU time per pass (shadow map 0.4 ms, depth pre-pass 0.4 ms, ambient occlusion 0.5 ms, scene 2.0 ms, post-processing
0.6 ms, outline 0.1 ms, their sum 5 ms), in Release at 1920 x 1080 with `--no-vsync` on the `snes-stress` room (2,000
titles); and the real SNES room (890) checked end to end through `scripts/real-library.ps1`. Nothing changes what a
frame draws (golden G01 untouched).

## What was built

| Where | What |
|---|---|
| `tests/input_scripts/bench_snes_room.json` (new) | The benchmark path: from the room's scene camera (inside the doorway, no `--spawn`, so that it plays in any room of the template), a 1,230-frame `benchmark` phase (frames 30 to 1259, all past the run's warm-up) that pans the side walls from the doorway, walks 5 m in along the circulation band, turns to the west wall's spine-out modules and walks 2.5 m along them, turns to the east wall, then to the hero wall and steps towards it, turns round to see the whole room from the hero end and walks back to the doorway; 10 frames after the phase, so that it is closed when the run ends. Run with `--frames 1270 --fixed-dt 0.016667`: the same path on every machine, the frames timed by the wall clock. |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `GpuPassRecord` (a pass's name and percentiles) and `gpuPasses` in `frameTimes` and in every phase: the GPU time of `shadow`, `depth`, `ssao`, `scene`, `post`, `outline` and their per-frame `total`, over the same frames as the frame times (past the warm-up), each pass only over the frames that reported it (the outline only while an item is hovered). Written only when there is one; reports without the key read as before (`schemaVersion` stays 1). |
| `rv/app/frame_timing.hpp`, `src/frame_timing.cpp` | `FrameTiming::recordGpuPasses(const gl::FrameStats&)`: the frame's last view's timer query results (a frame or two late, as `FrameStats` documents), taken by the next `recordFrame` for the run and its open phases; negative or infinite values dropped; `kGpuPassNames`. |
| `src/scene_graphics.cpp` (hook) | `renderFrame` passes the renderer's statistics to `frameTiming().recordGpuPasses` beside the stats log (frames that render no view add no GPU time). |
| `src/app_host.cpp` | The frame-limited run's measurement log lines add `; GPU <pass> p50 ... ms p99 ... ms, ...` to the run's and each phase's frame times. |
| `tests/smoke/test_world_benchmark.cpp` (new, hidden `[.bench][gpu][world][benchmark]`) | The snes-stress fixture imported, a run without a lockstep that fills the derived-texture cache (every layer resident, required), then the benchmark path; the phase's frames, every pass present, the figures printed (`WARN`); in Release the budgets checked (frame p99 and p50; every pass and the total at their p99). `build/msvc-x64-release/bin/rv_smoke_tests.exe "[benchmark]"`. |
| `tests/unit/app/test_room_real.cpp` (new, hidden `[.real-library][real-library][room-real]`) | The real SNES room under `build/<preset>/real-library/room` (a new catalog, a fresh plan of template version 2): `cold` (no plan, an empty cache), `again` (the plan deleted, generated a second time: the same digest and the same file SHA-256; warm), a second import then `restart` (the stored plan reconciled: `unchanged`, rebuild `no-rebuild`, 0 moved, 0 added, 0 removed; warm), the benchmark path, and four lockstep screenshots (doorway, a side wall's spine-out shelf at 1 m, the hero band at 1.2 m, a band item inspected with `world_inspect.json` up to frame 61, held with its card). Figures printed as `Real snes room 17b: ...`. |
| `tests/unit/app/test_frame_timing.cpp`, `test_run_report.cpp` | The GPU passes follow the run's and the phases' frames (warm-up, frames without a view, invalid times, the total); `gpuPasses` round-trips and is left out where empty. |

## Results

The reference machine (RTX 2080 SUPER, driver 617.42 as `4.5.0 NVIDIA 617.42`), 2026-10-19, at commit 99bc343 plus this
step. **Not a quiet machine**: the CPU load stood at 60 to 100% throughout (other sessions' builds and a long-running
recursive `grep`, and another worktree's GPU smoke tests sharing the GPU during the snes-stress benchmark). The figures
below are informative; the gate (step 18) measures on a quiet machine under `scripts/focus_watch.py`.

- **Build**: `msvc-x64-release` with 0 warnings (the first attempt ran out of compiler heap under the load; the
  incremental rerun built). Unit tests `[frame_timing],[run_report]`: 20 cases passed.
- **snes-stress benchmark** (`rv_smoke_tests "[benchmark]"`, Release): the path ran, the phase closed with 1,230 frames.
  The budget checks failed on one pass only (post-processing at its p99, below).
- **The real library**: `./scripts/real-library.ps1 ... -Filter '[real-library][room-real]'` (Release, ES-DE closed,
  `RV_LAUNCH_ALLOWLIST` naming the fake emulator alone): run 1 failed 3 checks, all from one cause, the cold run's
  6,000 frames ending before the cold covers were in under the load (681 of 890 resident; the next run then stored the
  remaining 408 entries): the frame limits were raised (20,000 cold, 8,000 warm); run 2 passed. **0 differences among
  542,120 entries** both times (the ES-DE home and install hashed, the ROM and media roots listed).

## Figures

### The benchmark path (1920 x 1080, `--no-vsync`, `--fixed-dt 0.016667`, the `benchmark` phase: 1,230 frames)

| | Budget | snes-stress (2,000), warm cache | real SNES room (890), run 1 | real SNES room, run 2 |
|---|---|---|---|---|
| Frame p50 | 8 ms | **2.98 ms** | **2.98 ms** | **3.52 ms** |
| Frame p95 / p99 / max | p99 16.7 ms | 4.98 / **8.35** / 17.25 ms | 4.57 / **9.43** / 19.50 ms | 16.12 / **44.78** / 182.5 ms (missed) |
| GPU shadow map p50 / p99 | 0.4 ms | 0.045 / 0.055 ms | 0.045 / 0.061 ms | 0.047 / 0.054 ms |
| GPU depth pre-pass | 0.4 ms | 0.044 / 0.083 ms | 0.037 / 0.068 ms | 0.037 / 0.057 ms |
| GPU ambient occlusion | 0.5 ms | 0.393 / 0.427 ms | 0.401 / 0.489 ms | 0.403 / 0.932 ms |
| GPU scene | 2.0 ms | 0.361 / 0.422 ms | 0.357 / 0.440 ms | 0.356 / 0.774 ms |
| GPU post-processing (FXAA) | 0.6 ms | 0.374 / **0.882** ms (p95 0.649) | 0.376 / 0.475 ms | 0.380 / **1.115** ms |
| GPU outline | 0.1 ms | not drawn on the path (no item hovered from the band's middle) | | |
| GPU total | 5 ms | 1.229 / 2.064 ms | 1.214 / 1.530 ms | 1.239 / 2.116 ms |
| Collection arrays / GPU memory peak | 80 MiB / 256 MiB | 76.7 / 251.8 MiB | 33.6 / 214.9 MiB | 33.6 / 214.9 MiB |

- The last view of the snes-stress path (at the doorway, facing south): 7 draw calls, 73 clusters all culled, the shadow
  map 24 draw calls; the walker ended at (-0.45, 0, 1.95) in both rooms.
- In the snes-stress run, the layers came from the cache during the phase's first seconds (4,000 hits, 0 misses, every
  layer resident 4.5 s after the room showed; 0.26 to 0.38 s on the quieter machine of step 10b). The earlier attempt whose preparing run filled only the first view's layers
  (a 2-frame lockstep) derived covers during the phase: p95 24 ms, p99 35 ms; the case now requires a full cache first.

### The real SNES room (Release; run 2 unless stated)

| | Budget | Figure |
|---|---|---|
| Placements | | 890 (44 featured, 12 band, 834 spine); 6,907 unplaced (645 duplicate titles, 6,262 without a cover); 32 scene objects, 14 lights (8 in step 10b; the evening profile's accents since step 16), 35 clusters |
| The plan generated twice | one SHA-256 | `58ddeaf7...d558d9` both times, the plan file's SHA-256 the same (it is the digest) |
| Imports (the snes system, 7,835 entries) | | the first 1.1 to 1.6 s; the second 0.35 to 1.1 s writing nothing (0 identities, 0 entries) |
| The restart after the second import | `NoRebuild`, 0 moves | outcome `unchanged`, change `updated`, rebuild `no-rebuild`, plan not written; reconciliation `textures`: 890 kept, 0 moved, 0 flags changed, 0 added, 0 removed |
| Every layer resident after the room showed, cold | 10 s | **20.4 s** (missed; 1,780 misses and stored; 7.1 s quiet in step 12); run 1 had not finished at 17 s |
| The same, warm (plan generated again / restart) | 1.0 s | **1.27 s** / **1.00 s** (1,780 hits, 0 misses); run 1: 12.3 s and 7.3 s |
| The room's job, cold / again / restart | | 1,050 / 439 / 213 ms (156 / 107 ms quiet in step 10b) |
| The snapshot's end to the commit, cold / again / restart | | 1,105 / 580 / 644 ms |
| The room shown after the process started, cold / again / restart | | 4.5 / 2.8 / 7.0 s (1.3 s quiet in step 10b) |
| Frame p99 over the `coverLoad` phase, cold / warm | 33.3 ms | 25.5 / 16.8 and 23.2 ms (p50 3.2 to 3.5 ms) |
| Layer uploads per frame (`layerUploads`) | 1 ms | p99 0.10 to 0.77 ms, max 0.77 ms |

### Screenshots (not committed: the user's covers)

Under `build/msvc-x64-release/real-library/room/` (1920 x 1080, lockstep): `doorway.png` (the hero wall's two modules
with the band and the featured shelf, the sign, the CRT on its cabinet with the armchair and the rug, the side walls'
modules with a poster, a plant, a slim lamp and a narrow cabinet, both display cases' covers), `shelf_spines_100.png`
(a west-wall spine-out module at 1 m: the titles on the spines in the cover's colour, one pulled out with its hover
label), `shelf_band_120.png` (the hero band's face-out covers at 1.2 m, the first hovered), `inspect.png` (Breath of
Fire II held, its card beside it: system, year, developer, genre, players, "Altre versioni", Avvia / Vedi media /
Informazioni / Indietro). Against style §26.2: the room reads as the concept's (warm wood, the sign and the CRT the most
saturated, the upper walls dark), the spines readable at 1 m, the covers clear. Seen: the CRT shows black in the
8-frame shots (the clip has not started yet); one band cover shows a wide tan border around a small picture (the
scraped image itself is framed so; the fit keeps it whole).

## Budgets missed and likely causes

1. **Post-processing p99 0.88 ms (snes-stress) and 1.12 ms (real room, run 2) against 0.6 ms**, its p50 0.37 to 0.38 ms
   in every run and its p99 0.48 ms in the real room's run 1. The pass is a fixed per-pixel cost (bloom chain, tonemap,
   FXAA at 1920 x 1080) that does not depend on the room, so the tail is not the room's: the likely cause is the GPU
   shared with another worktree's GPU smoke tests during those runs (the ambient occlusion and the scene passes, also
   per-pixel, show the same tail in run 2: p99 0.93 and 0.77 ms). To confirm on a quiet machine at the gate; if it
   stays, the bloom chain's mip passes are the first suspect (style §4.3's sober bloom).
2. **Frame p99 44.8 ms in the real room's run 2** (9.4 ms in run 1 on the same path and room, 8.4 ms on snes-stress):
   CPU load from other processes (the GPU total stays at p99 2.1 ms in that run); to be measured at the gate.
3. **Cold residency 20.4 s for 890 covers against 10 s** (7.1 s on a quieter machine in step 12, 19 s for 2,000 here
   against 15 s): the cover derivation runs on the image worker pool, which competed with the machine's other work; the
   warm budget was met in run 2 (1.00 s at the restart, 1.27 s after a regeneration) and missed in run 1 (7.3 s), for
   the same reason. To be measured at the gate.

## Decisions taken within the step

1. **GPU passes in the run report, per phase**: the budgets name the benchmark phase, and the stats log's average over
   the run cannot give a percentile; the samples ride on `FrameTiming`, so every phase (a refresh, a cover load, an
   animation) gets them too. Each pass is summarised over the frames that reported it; the per-frame total sums what
   each frame reported.
2. **The path starts at the scene camera**, not a `--spawn`: the same script measures the stress room, the real room
   and any later room of the template; walls and furniture stop the walker where a room is shorter.
3. **The benchmark runs with a full cache, as a restart finds it**: a preparing run without a lockstep fills it (a
   lockstep run waits only for the layers in view). The warm layers still arrive during the phase's first frames, as at
   a real restart.
4. **The budgets per pass are checked at their p99** in the hidden bench case (strict; the table names no percentile).
   Step 18 may settle on another percentile for the gate.

## For the merge

- **CLAUDE.md**: "Running the app": the run report's `frameTimes` and `phases` carry `gpuPasses` (`shadow`, `depth`,
  `ssao`, `scene`, `post`, `outline`, `total`: p50, p95, p99, max over the frames past the warm-up). "Visual
  verification" or the GPU smoke tests: the benchmark, `rv_smoke_tests "[benchmark]"` in Release (hidden), and the path
  `tests/input_scripts/bench_snes_room.json` (`--frames 1270 --fixed-dt 0.016667 --no-vsync`, 1920 x 1080). The
  real-library tests: `[room-real]` (the real room's plan twice, the restart after a second import, cold and warm
  covers, the benchmark path, screenshots under `build/<preset>/real-library/room`).
- **docs/architecture.md**: the run report's measurements now include the GPU passes (`FrameTiming::recordGpuPasses`).
- **docs/roadmap.md**: row 17b done with the figures above (the missed budgets to be measured at the gate on a quiet
  machine).
- **Step 18 (Gate B)**: run `[benchmark]` and `[real-library][room-real]` on a quiet machine; if post-processing's p99
  stays above 0.6 ms there, look at the bloom chain. The real room's frame limits (20,000 cold, 8,000 warm) leave room
  for a slow cold load; the runs end by their frame count, so on a quiet machine they last about 60 s and 25 s.
- **BACKLOG.md**: "An option ending a frame-limited run once every layer is resident" (the real room's runs wait out
  their frame count).
