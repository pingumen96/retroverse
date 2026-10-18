# I3 step 0d: measurement for the budgets

Step 0d of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice", and its "Budgets" table; ADR-0011 §9) gives the later
steps the measurements their budgets are checked with: frame-time percentiles over the run and per named phase, CPU
timers around named main-thread sections, and the renderer's own accounting of the GPU memory it allocates, all in the
run report (and the GPU memory in the overlay's Stats panel). It fills the stubs step 0a pre-listed (`cpu_timers`,
`gpu_memory`, `frame_timing`) and the run report's `phases` and `gpuMemory`; nothing changes what a frame draws (golden
G01 unchanged).

## What was built

### Frame times and phases (`rv_app_cli`, `rv/app/frame_timing.hpp`)

- `app::FrameTiming`: the host (`AppHost::runMainLoop`) records every frame it presents, with its wall-clock time from
  the end of the previous frame's buffer swap to the end of this one's (`steady_clock`; the measured time, also under
  `--fixed-dt`, which only replaces the simulation's step). The first frame has no previous one; the idle wait of a
  launch and the minimized wait of an interactive run are no frame time (the next frame is measured from the end of
  the wait). The first `kFrameTimeWarmUpFrames` (30) frames are left out of every percentile.
- Phases: `openPhase(name)` / `closePhase(name)` by the code that runs them, through `AppContext::frameTiming()`. A frame
  belongs to a phase when the phase is open when the frame ends, or was opened and closed during it. Openings nest by
  name (two animations at once keep `animation` open until both close); a phase opened again later adds to the same
  record. The names the budgets share are constants: `kPhaseRefresh` ("refresh"), `kPhaseCoverLoad` ("coverLoad"),
  `kPhaseAnimation` ("animation"), `kPhaseBenchmark` ("benchmark").
- `--input-script` steps take `"phase": "<name>"` (`rv/input/input_script.hpp`, `InputScript::phasesAt`,
  `input::isValidPhaseName`: an ASCII letter, then letters, digits, `_` or `-`, at most 64 bytes): the phase is open on
  every frame the step covers, with or without actions. `RetroverseApp` opens and closes them at the start of each
  frame's update and logs each change (`Frame 40: phase 'benchmark' opened by the input script`). Steps of one phase
  that meet or overlap keep it open throughout. `tests/input_scripts/measurement_phases.json` holds two phases (one half
  within the warm-up).

### CPU sections (`rv_core`, `rv/core/cpu_timers.hpp`)

- `core::CpuTimers`: named sections, each accumulating its time per frame (any number of runs per frame: `add()`, or a
  `Scope` on `steady_clock`); `endFrame()` turns each section that ran into one sample, the others record nothing, so a
  section's percentiles describe the frames it ran in (their count is reported). Time spent before the first frame
  belongs to the first; what runs after the last `endFrame()` is not recorded. Every frame counts, the warm-up included
  (an upload burst at the start is exactly what the upload budgets are about). Samples are kept for the run (8 bytes per
  section and frame). `core::summarizeDurations` (nearest rank) is the one percentile function; `app::summarizeTimes`
  and `app::TimeSummary` (now an alias of `core::DurationPercentiles`) use it.
- `core::mainThreadCpuTimers()`, closed by the host after every presented frame, and `core::CpuSectionScope(name)` (main
  thread only; elsewhere it logs an error and times nothing). The names: `core::cpu_section::kCollectionPlanning`
  (step 8), `kLayerUploads` (6, 12), `kVideoUpload` (11), `kProductUi` and `kSceneSubmit`.
- Timed now: `productUi` around `ProductUi::update`, `renderToWindow` and `renderTo` (every frame, the catalog open or
  not), and `sceneSubmit` around `gl::SceneRenderer::render` in `SceneGraphics` (every view of the frame: the window's
  and the screenshots'), the renderer's CPU side, of which the collection's planning will be a part.

### GPU memory (`rv_render_uniforms`, `rv/render/gpu_memory.hpp`)

- `gl::GpuMemoryKind`: `ProceduralSurfaces`, `CollectionArrays` (step 6), `HeroTextures` (12), `Video`, `Targets`,
  `SceneTextures` (every other texture: the scene's files, patterns and model images, the defaults), `Buffers`
  (vertices, indices, instances, uniform blocks, debug lines). Report keys `proceduralSurfaces`, ..., `buffers`.
- `gl::GpuFormat` and `textureLevelBytes` / `textureBytes(format, size, levels, layers)`: RGBA8, sRGB8_A8, RGBA16F, R8,
  RG16F, D24S8, D32F (bytes per texel) and BC1 (8 bytes per 4 x 4 block, each side rounded up, every level at least one
  block), each level halving down to 1 x 1. `mipChainBytes` (RGBA8) agrees, by test.
- `gl::GpuMemoryLedger` (bytes, peak and live allocations per kind; the total and its peak, the highest sum) and
  `gl::GpuMemoryAllocation` (RAII, move-only, `reset`, `setKind`). The process's ledger, `gl::gpuMemoryLedger()`, is
  trivially destructible (an allocation released during static destruction never touches a destroyed object).
- Recorded by their owners in `rv_render`: `Texture2D` (`TextureDesc::memoryKind`, default `SceneTextures`; the texture
  cache gives its `procedural:` keys `ProceduralSurfaces`), `Framebuffer` (`Targets`, `framebufferBytes(desc)`: every
  color attachment and the depth/stencil buffer; a resize records the new attachments before releasing the old, so the
  peak shows both), `ShadowMap` (`Targets`, D32F), `Buffer` (`Buffer::create(..., memoryKind = Buffers)`), `VideoTexture`
  (`Video`: its texture and its pixel buffer ring). Not counted: the vendored Dear ImGui and RmlUi backends' own
  textures and buffers, what the driver adds, the window's default framebuffer.
- `SceneGraphics::finishRun` checks that the ledger's procedural surfaces and scene textures equal what the texture cache
  and the defaults hold (each texture's `estimatedBytes`, from its format, size and levels): a frame-limited run fails
  otherwise (exit 3), an interactive one logs the error. Every smoke case of `retroverse.exe` and the asset preview
  checks it so.
- The Stats panel of the developer overlay has a "GPU memory" table: every kind's bytes now and its peak, and the total,
  under short labels that fit the panel's column (`gl::displayName`: Surfaces, Arrays, Hero, Video, Targets, Textures,
  Buffers; the report's key and the live allocations in each label's tooltip).

### The run report (`rv/app/run_report.hpp`)

The host fills the four records after the main loop (`AppHost::recordMeasurements`, on every way out of the run; the
GPU memory is taken when the main loop ends, before the logic releases its resources, or when the report is written for
a run that ended before) and logs them once for frame-limited runs (`Frame times of the run ...`, `Phase '...'`, `CPU
section '...'`, `GPU memory (the renderer's accounting): ...`). Required keys like 0a's sections (`schemaVersion` stays
1):

```json
"frameTimes": {"warmUpFrames": 30, "count": 70, "p50Ms": 6.06, "p95Ms": 6.76, "p99Ms": 8.58, "maxMs": 8.58},
"phases": [
  {"name": "coverLoad", "opened": 1, "open": false, "frames": 40, "count": 20, "p50Ms": 6.04, ...},
  {"name": "benchmark", "opened": 1, "open": false, "frames": 40, "count": 40, "p50Ms": 6.06, ...}
],
"cpuSections": [
  {"name": "productUi", "count": 100, "p50Ms": 0.02, "p95Ms": 0.03, "p99Ms": 0.08, "maxMs": 0.09},
  {"name": "sceneSubmit", "count": 100, "p50Ms": 2.68, "p95Ms": 3.46, "p99Ms": 5.63, "maxMs": 6.29}
],
"gpuMemory": {"totalBytes": 161369500, "peakTotalBytes": 161369500, "kinds": {
  "proceduralSurfaces": {"bytes": 83886044, "peakBytes": 83886044, "allocations": 23}, ...,
  "targets": {"bytes": 50568896, ...}, "sceneTextures": {"bytes": 25800008, ...}, "buffers": {"bytes": 1114552, ...}}}
```

`count` is the frames timed (for a section, the frames it ran in); with a count of 0 the percentiles are left out (and
not required when read). Phases are in the order they first opened, sections by name. `gpuMemory.kinds` holds every
kind this build knows (required; a kind of a newer writer is ignored).

## Measured (the reference machine, Debug, 1280 x 720 hidden window, vsync adaptive)

`retroverse.exe --scene m0_testroom --spawn 0,0,3.2,0,-6 --exit-after-frames --frames 100 --fixed-dt 0.016667 --hidden
--input-script tests/input_scripts/measurement_phases.json`: frame p50 6.1 ms, p99 8.6 ms over 70 frames; `sceneSubmit`
p50 2.7 ms (a Debug build's CPU side); `productUi` 0.02 ms with the catalog closed. GPU memory: procedural surfaces
80.0 MiB (23 maps; 83,886,044 bytes, 36 under 80 MiB since the chains stop at 1 x 1), scene textures 24.6 MiB (106: the
cache's 102 and the 4 defaults), so the room's textures take 104.6 MiB, the figure the texture cache logs and the
roadmap's "about 105 MiB"; render targets 48.2 MiB (the window's view targets at 1280 x 720 and the 16 MiB shadow map; a
`--screenshot` adds the screenshot targets at its size), buffers 1.1 MiB. Budgets are measured in Release with
`--no-vsync` (the roadmap's "how measured").

## Tests

- `tests/unit/core/test_cpu_timers.cpp`: nearest-rank percentiles; a section's frame as the sum of its runs, frames
  without a run, values ignored, the sections by name; scopes timed on the clock, `stop()` once, a moved-from scope; the
  main thread's timers and the shared names.
- `tests/unit/render/test_gpu_memory.cpp`: bytes per format, BC1 blocks and layers, agreement with `mipChainBytes`; the
  procedural surfaces' 83,886,044 bytes; the kinds' names; the ledger's bytes, peaks (the highest sum) and counts,
  never below zero; allocations moved, replaced (both in the peak) and moved to another kind.
- `tests/unit/app/test_frame_timing.cpp`: the warm-up left out, unknown and invalid times; a phase's frames (open at the
  end of a frame, or opened and closed within it); nesting, reopening, still open at the end, a phase within the warm-up.
- `tests/unit/app/test_run_report.cpp`: every report holds the sections (the minimal one: no frame time, no phase, no
  section, nothing allocated); the records read back field for field; the JSON layout; a missing or mistyped section,
  a missing percentile or kind refused with its JSON pointer, a newer writer's kind ignored; the ledger and the timers
  turned into records.
- `tests/unit/input/test_input_script.cpp`: the `phase` field, `phasesAt`, the names refused.
- `tests/smoke/test_measurements.cpp` (`gpu.measurements/...`): the test room for 100 frames with
  `measurement_phases.json`: 70 frames timed with ordered percentiles; both phases with their frames (40 each) and
  timed frames (20 within the warm-up's end, 40); `productUi` and `sceneSubmit` over the 100 frames, the later steps'
  sections absent; the procedural surfaces exactly their computed bytes and maps, no collection arrays, hero textures
  or video, targets, scene textures and buffers present, the textures within 256 MiB; then a 30-frame run without a
  script: no phase, no frame time, the same procedural surfaces.
- `requireCleanRun` (every smoke case) now checks the 0d sections instead of their emptiness: the frame count past the
  warm-up, phases and sections within the run's frames with ordered percentiles, the kinds adding up to the total,
  peaks at least the bytes, targets for a run that drew a view.

Results on the reference machine (RTX 2080 SUPER, driver 617.42) with `RV_LAUNCH_BACKGROUND` set (and
`RV_LAUNCH_ALLOWLIST` unset): Debug, RelWithDebInfo and Release built with 0 warnings; `./scripts/test.ps1` 1,288 unit
tests passed; `./scripts/test.ps1 -Preset msvc-x64-debug-gpu` 1,318 tests (unit and GPU) passed, `gpu.measurements/...`
among them; in Release the smoke cases of the scenes, the self-checks, the golden and the measurements passed. Golden
G01 0 of 2,073,600 pixels apart (largest difference 0/255) in Debug and Release. In Release the measurement case's run
reads frame p50 6.06 ms, p99 6.64 ms (vsync), `sceneSubmit` p50 0.16 ms, `productUi` p50 0.004 ms, and the same GPU
memory to the byte as in Debug.

## Decisions taken within the step

1. **Frame time = swap to swap**, measured by the host after `window.swap()`, not `FrameClock`'s delta (which is taken
   before the update and so belongs to the previous frame). A frame's time is everything the main thread did for it:
   events, update, render, the swap (vsync included: budgets are measured with `--no-vsync`).
2. **The warm-up applies to every percentile of frame times, phases included**, so a phase opened at the start (a cover
   load) does not report the shader compiles and the procedural surfaces; its `frames` still counts every frame, so a
   reader sees how many were left out. CPU sections keep every frame (their work is the spike the budgets limit).
3. **Phases aggregate by name** (every animation in one record, as the budget "p99 over the animation phases" reads)
   and nest; the record says how often a phase was opened and whether it was still open.
4. **One percentile rule** (nearest rank, `core::summarizeDurations`) for the catalog's pages and frames, the frame
   times, the phases and the sections; `app::TimeSummary` is its type.
5. **The process's ledger is a global** (like the logger): every GPU object of `rv_render` records itself without the
   allocation sites being handed a ledger; the RAII token keeps it exact through moves and resizes. Tests use their
   own ledgers.
6. **Kinds beyond the row's list**: `SceneTextures` and `Buffers`, so that the total is everything the renderer
   allocates and "all textures" is the total without `Buffers`. The video's ring buffer counts as `Video` (its budget
   covers "the video's texture and ring").
7. **The accounting checks itself**: a frame-limited run fails when the ledger's textures differ from what the cache and
   the defaults hold, so the acceptance "gpuMemory of the test room equals the sum of its textures' sizes" holds in
   every smoke run, not just one case.
8. **`sceneSubmit`** is a section the row did not list: the renderer's CPU side, the frame's largest main-thread cost
   today and the frame that step 8's planning will sit in.
9. **CLAUDE.md is not edited here** (see "For the merge").

## Notes for later steps

- Open and close a phase where the work starts and ends: `context.frameTiming().openPhase(kPhaseRefresh)` when the
  import job starts (13r, 15b), `closePhase` when its `CatalogChanges` arrive; `kPhaseCoverLoad` from the plan shown to
  the room's last layer resident (12); `kPhaseAnimation` per pickup or inspection animation (13a); 17b's
  `bench_snes_room.json` marks its path with `"phase": "benchmark"`. A refresh opened by the worker must be opened on
  the main thread (FrameTiming is main-thread only).
- Time a main-thread section with `const core::CpuSectionScope timing(core::cpu_section::kLayerUploads);` around the
  calls (6 and 12: the uploads under the frame budget; 11: `VideoTexture::upload`; 8: the static groups' planning and
  culling, inside `sceneSubmit`). A section is reported only once it runs.
- Step 6: `Texture2DArray` records `GpuMemoryAllocation(GpuMemoryKind::CollectionArrays, textureBytes(GpuFormat::Bc1
  (or Rgba8/Srgb8Alpha8 for the fallback), size, levels, layers))`; a growth by GPU copy records the new array before
  the old one is released (the peak shows both, as it happens). Step 12's hero textures pass `TextureDesc::memoryKind =
  HeroTextures`. A new storage format adds a `GpuFormat` value and a line in `textureLevelBytes` with a test.
- `SceneGraphics::finishRun`'s check counts the cache and the defaults against `ProceduralSurfaces` + `SceneTextures`:
  a texture created outside the cache must take its own kind (hero textures, arrays), or the frame-limited run fails.
- The overlay's Stats panel shows the ledger; CPU sections and phases are only in the report and the log (a panel for
  them would be a BACKLOG item if a step needs it live).

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Running the app", `--run-report`: the report also holds the frame times (`frameTimes`: p50, p95, p99, max of the
    frames past the first 30, swap to swap), the phases (`phases`: named spans opened by the code or an input script's
    `"phase"`, each with its frames and frame times), the main thread's CPU sections (`cpuSections`: `productUi`,
    `sceneSubmit`, later `collectionPlanning`, `layerUploads`, `videoUpload`; p50, p95, p99, max over the frames each
    ran in) and the renderer's GPU memory per kind (`gpuMemory`: procedural surfaces, collection arrays, hero
    textures, video, targets, scene textures, buffers; now and peak); frame-limited runs log them at the end, and fail
    (exit 3) when the accounting misses a texture of the cache.
  - "Running the app", `--input-script`: a step may name a `"phase"` (`{"fromFrame": 40, "toFrame": 99, "phase":
    "benchmark"}`), open on its frames.
  - "Running the app", developer overlay: the Stats panel lists the GPU memory per kind (now and peak).
  - "Conventions", core helpers: `cpu_timers.hpp` (`CpuTimers`, `mainThreadCpuTimers`, `CpuSectionScope`,
    `summarizeDurations`); the GPU memory accounting in `rv/render/gpu_memory.hpp` (`GpuMemoryLedger`,
    `gpuMemoryLedger()`, `GpuMemoryAllocation`, `textureBytes`).
  - The 0a sentence "the run report holds the I3 sections ... empty until their steps" drops `phases` and `gpuMemory`.
- **docs/architecture.md**: `rv_render_uniforms` holds the GPU memory accounting; `rv_core` the CPU timers; `rv_app_cli`
  the frame timing (one line each in the module table, if the merge wants them there).
- **docs/roadmap.md**: step 0d done (edited here).
