# I3 step 6: texture arrays in `rv_render`

Roadmap row: I3 step 6 (wave 1, after 0c and 0d; 1c, 2, 3, 4, 5, 7 and 13r ran beside it in their own worktrees).
Decision: [ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md) §4 (texture arrays per size
class), §6 (the uploads under a per-frame budget) and §9 (the budgets). The step fills the stubs step 0a listed for it
(`texture_array_layout` in `rv_render_uniforms`, `texture_array` and `collection_arrays` in `rv_render`), gives the lit
shader and the inspection views the layered base color on a new texture unit, and turns `--no-texture-compression` on.
Nothing in the product draws the collection yet: step 10b's `CollectionInstance` and step 12's loader use what is here.
A test driver, `rv_collection_probe`, draws a wall of test items through it for the GPU smoke tests.

## What was built

| File | What it holds |
|---|---|
| `rv/render/texture_array_layout.hpp`, `src/texture_array_layout.cpp` (`rv_render_uniforms`, GPU-free) | `CollectionClass` (front, spine), `CollectionArrayFormat` (BC1 sRGB, RGBA8 sRGB) and `chooseCollectionArrayFormat`; the classes' sizes, levels and bytes (`layerSize`, `layerLevelCount`, `mipLevelSize`, `layerLevelBytes`, `layerBytes`, `arrayBytes`); `arrayLayersFor` and `planArrayLayers` (sized to the room, 64 at a time, 2,048 per array); `Rgb8`, `toRgb565`, `fromRgb565`; `LayerImage` (one layer's mip chain), `validateLayerImage`, `makeSolidLayer`; `LayerAllocator` (the layers of one array, their owners and the least recently released); `UploadBudget` and `UploadBudgetMeter` |
| `rv/render/texture_array.hpp`, `src/texture_array.cpp` | `Texture2DArray`: immutable `GL_TEXTURE_2D_ARRAY` storage, `uploadLayer` (whole levels: `glCompressedTextureSubImage3D` or `glTextureSubImage3D`), `grow` (a larger array, the GPU copying every layer with `glCopyImageSubData`), `fillLayers` (one layer copied into a range on the GPU); its bytes recorded under `GpuMemoryKind::CollectionArrays`; `internalFormatOf` |
| `rv/render/collection_arrays.hpp`, `src/collection_arrays.cpp` | `CollectionArrays`: the arrays of both classes, `reserve` (create and grow to the room), `acquire` / `release` / `find` (a key's layer), `submit` (a layer image queued), `uploadPending` (the queue under the budget, timed as the CPU section `layerUploads`); `LayerSlot`, `LayerUpload`, `LayerUploadResult` |
| `rv/render/uniform_blocks.hpp` | `material_flags::kLayeredBaseColor` (bit 9) and `kSpineLayer` (bit 10); `CollectionTextureUnit::LayeredBaseColor` = 12, after the ambient occlusion units |
| `rv/render/material_registry.hpp`, `src/material_registry.cpp` | `LayeredBaseColor` (an array and which layer of the record), `MaterialRegistry::addLayered`, `GpuMaterial::layeredBaseColor`; `bind()` binds a layered material's array and the clamped sampler on unit 12, reading the array's id at every bind |
| `src/gl_device.cpp` | `glTextureStorage3D`, `glTextureSubImage3D`, `glCompressedTextureSubImage3D` and `glCopyImageSubData` among the entry points checked at start |
| `assets/shaders/common/material.glsl` | `RV_MATERIAL_LAYERED_BASE_COLOR`, `RV_MATERIAL_SPINE_LAYER`, `uLayeredBaseColorTexture` (`sampler2DArray`, binding 12), `materialInstanceBaseColor(uv, instance)` |
| `assets/shaders/pbr/pbr.frag`, `assets/shaders/debug/inspect.frag` | the base color through `materialInstanceBaseColor(vUv, vInstance)`; nothing else changes for a material without the flag |
| `src/app/src/command_line.cpp`, `rv/app/command_line.hpp` | `--no-texture-compression` accepted (its refusal block removed), its help text |
| `src/app/src/scene_graphics.cpp`, `rv/app/scene_graphics.hpp` | `SceneGraphics::collectionArrayFormat()`, chosen at creation from the device's extensions and the option and logged with the reason |
| `tests/collection_probe/` (new: `main.cpp`, `probe_app.hpp/.cpp`, `probe_layout.hpp`, `CMakeLists.txt`) | `rv_collection_probe.exe`, built with the tests: the GPU smoke tests' driver (below) |
| `tests/smoke/test_collection_arrays.cpp` | the GPU smoke cases `gpu.collection arrays/...` |
| `tests/unit/render/test_texture_array_layout.cpp`, `tests/unit/integration/test_shader_sources.cpp`, `tests/unit/app/test_command_line.cpp` | the unit tests (below) |

### Arrays, formats and sizes

- Two classes, each an array with a full mip chain: fronts 256 x 188 (9 levels) and spines 40 x 256 (9 levels) in BC1
  sRGB (`GL_COMPRESSED_SRGB_S3TC_DXT1_EXT`); RGBA8 sRGB (`GL_SRGB8_ALPHA8`) at half the size, 128 x 94 and 20 x 128
  (8 levels each), when `--no-texture-compression` is given or `GLCapabilities::s3tcSrgb()` is false (either S3TC
  extension missing). The option wins when both apply. `SceneGraphics` logs the choice once:
  `Collection texture arrays: bc1-srgb (256x188 fronts, 40x256 spines): GL_EXT_texture_compression_s3tc and
  GL_EXT_texture_sRGB present` (or `rgba8-srgb (128x94 fronts, 20x128 spines): --no-texture-compression`).
- Bytes per layer, every level: BC1 front 32,280, spine 6,984 (39,264 per item, 38.3 KiB: ADR-0011 §9's 39 KiB); RGBA8
  front 64,012, spine 13,628 (77,640 per item, 1.98 times BC1 at full size). An array of 2,048 layers of each class in
  BC1 takes 80,412,672 bytes (76.7 MiB, within the 80 MiB budget).
- Sizing (`planArrayLayers`): the items of a class (the room's placements and free slots) plus the pending layer,
  rounded up to 64, at most 2,048 per array; past 2,047 items a class gets another array: 2,049 items make {2048, 64},
  890 make {896}. `CollectionArrays::reserve` creates what is missing and grows what is too small, never shrinks.
- Layer 0 of every array is the pending picture, a solid layer of the class's pending color (`Desc::pendingColors`, the
  family's plastic color) uploaded with the array and then copied on the GPU into every other layer the array has or
  gains (`fillLayers`, by doubling runs of `glCopyImageSubData`): a layer read before its picture arrives never shows
  undefined texels. The instance record holds 0 until the item's picture is resident, so an item shows the pending
  picture until then (ADR-0011 §6), and one record write swaps it.
- Growth: `Texture2DArray::grow` creates the larger array (recorded in the ledger first), copies every level of every
  current layer into it on the GPU, then releases the old storage; the object keeps its address, so the materials that
  name it sample the new storage (their bind reads the id). The allocator grows with it; the new layers get the pending
  picture.
- Uploads: `submit` validates the image (format, level count, every level's exact bytes: BC1 levels whose sides are not
  multiples of 4 as whole blocks) and queues it for the key's current layer, with the layer's generation. `uploadPending`
  takes the queue in submission order (the loader of step 12 submits in its priority order) while the budget allows
  (`UploadBudgetMeter`: the first layer of a frame always, then while fewer than 64 went up and the time taken plus the
  expected cost of one more layer, a moving average of the measured ones kept across frames, stays within 1 ms); an
  upload whose layer was given to another key since is dropped. Each upload is one call per level, from client memory
  (no pixel buffer: 32 KiB a layer). A frame with nothing queued costs nothing and records no CPU section; one that
  uploads is timed as `layerUploads` (`core::cpu_section::kLayerUploads`, step 0d). `glGenerateTextureMipmap` is never
  called on an array.

### Layers and their owners (`LayerAllocator`)

Each array has an allocator: a key (the caller's: the item's game id and class, say) acquires a layer and keeps it.
Released layers keep their pictures: the same key acquiring again gets its own layer back, already resident, with no
upload. A new key takes a layer never used first, and only when none is left the **least recently released** one
(its earlier key forgotten, its generation bumped so that an upload still queued for the earlier key is dropped).
Layers in use are never taken. `CollectionArrays::acquire` tries the arrays in order: the key's own layer wherever it
is, then a never-used layer in the first array that has one, then the least recently released layer of the first array
that has one; with every layer in use it answers `OutOfRange` (the caller reserves more first). This is the "LRU of
layers": ADR-0011 keeps every placed item resident (no eviction of a layer in use; the hero tier is step 12's LRU), and
the least-recently-released order only decides which free picture is overwritten when the room changes.

### The layered base color in the shaders

A layered material (`MaterialRegistry::addLayered`) carries `RV_MATERIAL_LAYERED_BASE_COLOR`, plus
`RV_MATERIAL_SPINE_LAYER` for a spine array, and binds its array on unit 12 with the registry's clamped trilinear
anisotropic sampler. `materialInstanceBaseColor(uv, instance)` multiplies `materialBaseColor(uv)` (factor and texture)
by `texture(uLayeredBaseColorTexture, vec3(uv, float(layer)))`, the layer being the record's spine layer
(`vInstance.y`) or front layer (`vInstance.x`); the branch is on a uniform, so the derivatives stay defined, and it comes
before the alpha test like every other texture fetch. The lit shader and the inspection views use it; the depth and
unlit stages do not need the layer (a layered material is opaque). A layer index beyond the array reads its last layer
(GL clamps the layer coordinate). All the fronts of one array are one material, hence one batch; 2,049 items draw their
fronts and spines in four batches (two arrays each).

### `rv_collection_probe` (tests/collection_probe)

The GPU smoke tests need instances whose layers show known pictures before step 10b places cartridges, so a small test
executable on `rv_app`'s host and renderer (`AppHost`, `SceneGraphics`, `SceneInstance`) draws them:
`rv_collection_probe [--probe-scenario chunks|growth] [--probe-view lit|base-color] <retroverse.exe options>` (its own
options are taken out, the rest is parsed by `app::parseCommandLine`, so `--hidden`, `--exit-after-frames`, `--frames`,
`--screenshot`, `--run-report`, `--data-dir` and `--no-texture-compression` work as in `retroverse.exe`; it logs to
`<user data>/logs/collection-probe.log`). A wall of items (`probe_layout.hpp`, shared with the smoke test): a grey shell
box per item with two quads on its face, the front (left) and the spine (right, facing the camera so that both classes
are probed in one picture), framed by a 30-degree camera on +Z, in front of a dark backdrop, lit by a white ambient
alone (sky and ground alike, no light, no ambient occlusion, bloom or vignette). Every layer is a solid color, exact in
BC1 (both endpoints one RGB565 value) and RGBA8 alike, unique among the run's 4,098 pictures (an RGB565 value scrambled
from the picture's number, no channel dark), never the pending magenta (fronts) or cyan (spines). The probe keeps 256
pictures queued so that the budget, not the probe, limits the uploads; a frame-limited run fails (exit 3) when a layer
is still pending at its last frame. Scenarios: `chunks`, 2,049 items (each class in arrays of 2,048 and 64 layers);
`growth`, 63 items in arrays of 64 layers, then from frame 20 (once every layer is resident) 128 more: both arrays grow
to 192 layers.

## Tests

- `tests/unit/render/test_texture_array_layout.cpp` (10 cases): the classes' sizes, levels and bytes (the per-layer
  figures above, 39 KiB per item, 80 MiB for 2,048 layers, RGBA8 about twice); the format choice; `arrayLayersFor` and
  `planArrayLayers` as a table (0, 1, 63, 64, 191, 890, 2,047, 2,048, 2,049, 4,094, 4,095 items); every RGB565 value
  through `fromRgb565` and back, the rounding; solid layers decoded by a BC1 decoder written in the test (the S3TC
  specification's, bit replication) at the corners and center of every level, and their RGBA8 texels; layer images of
  another class, format, level count or an unpadded 47-texel level refused with the level named; the allocator (layer 0
  never given, a key keeps its layer, resident only for its own generation, growth adds never-used layers, never
  shrinks; released layers keep their pictures and come back resident, the least recently released goes first, the
  generation drops an upload of the earlier owner; never-used layers before released ones); the budget meter (the first
  layer always, the time and layer limits, the moving average carried across frames, a zero budget).
- `tests/unit/integration/test_shader_sources.cpp`: the two new `RV_MATERIAL_*` bits equal `material_flags`; a new case
  pins the `sampler2DArray` on `CollectionTextureUnit::LayeredBaseColor`, the body of `materialInstanceBaseColor` (the
  flag, the spine or front component, the array lookup) and its use by `pbr.frag` and `inspect.frag`. The existing
  case "fragment stages sample every texture before the alpha test may discard" covers the new lookup.
- `tests/unit/app/test_command_line.cpp`: `--no-texture-compression` parsed in interactive and frame-limited runs, no
  longer refused, a value refused, listed by `--help` without "Not implemented yet"; the refusal test keeps the two
  options still pending.
- `tests/smoke/test_collection_arrays.cpp` (label `gpu`), each section in BC1 and in RGBA8 (`--no-texture-compression`),
  each run hidden, frame-limited and clean (`requireCleanRun`: exit 0, 0 high or medium GL messages, the accounting):
  - `gpu.collection arrays/2,049 items: ...`: a base color run and a lit run of 240 frames each. In the base color view
    every one of the 4,098 quad centers (projected with the probe's camera) shows its own picture within 2 of 255 per
    channel. In the lit view every pixel equals its picture as the uniform ambient lights it and the display transform
    shows it (`gl::tonemap`, the CPU mirror), the one grey term of the rough dielectric's ambient specular fitted to the
    run, within 3. The last view: 6,148 items, none culled, 6 batches and 6 draw calls (the backdrop, the shells, the
    fronts and the spines in two batches each); `gpuMemory.collectionArrays` exactly the arrays' computed bytes (four
    allocations, peak equal); the `layerUploads` section in at least 4,098 / 64 frames with a median within 1.05 ms.
  - `gpu.collection arrays/growth: ...`: a 20-frame run ends before the growth: the 63 items show their pictures, the
    other 128 cells show none, the arrays hold 2 x 64 layers, the log reads `every layer of the 63 item(s) is resident
    (126 uploaded)`. A 60-frame run: all 191 items show their pictures in the base color view and in the lit one, the
    first 63 copied by the growth and never uploaded again (the log reads `the arrays grown` and `every layer of the 191
    item(s) is resident (382 uploaded)`: 2 x 191 uploads in all); the arrays' bytes are those of 2 x 192 layers and
    their peak the fronts' growth or the spines' (each new array recorded before the old one goes: max(f64 + s64 + f192,
    f192 + s64 + s192)).
- Golden G01 and every other smoke case are unchanged: the reference scenes have no layered material (their flags and
  the unit-12 binding never come into play).

## Results

On the reference machine (RTX 2080 SUPER, driver 617.42), with `RV_LAUNCH_BACKGROUND` set and `RV_LAUNCH_ALLOWLIST`
unset, while the other wave-1 worktrees built and tested (about ten compilers running most of the time): the timings
below are informative, the gate measures on a quiet machine (the roadmap's rule).

- **Builds**: Debug, RelWithDebInfo and Release with 0 warnings.
- **Tests**: `./scripts/test.ps1`: 1,368 unit tests passed. `./scripts/test.ps1 -Preset msvc-x64-debug-gpu`: 1,399
  of 1,400 passed, the 32 GPU cases among them; the one failure, `unit.catalog_db/sessions: a write that meets a busy
  database is tried again, or kept until it is free` (`store.busyRetries() > 0`), is a timing race of I2's session
  store under the machine's load, untouched by this step: it passed in the unit run before and alone after. In Release
  (`msvc-x64-release-gpu`) the two collection cases, the golden, the reference scenes, the self-checks and the
  measurements passed. After the last formatting pass the three builds were redone (0 warnings) and the render,
  shader and command line unit tests and the collection and golden cases of Debug and Release run again (passed).
- **Golden G01**: the Debug run of its JSON's command writes the golden's bytes (SHA-256 `a4aeebd2...f3176`, 0 of
  2,073,600 pixels apart at threshold 0); `gpu.golden/G01` passes in Debug and Release. The log reads `Collection
  texture arrays: bc1-srgb (256x188 fronts, 40x256 spines): GL_EXT_texture_compression_s3tc and GL_EXT_texture_sRGB
  present` and `GL debug output summary: 0 high, 0 medium, 0 low`.
- **Pictures**: every probed quad of every run within the tolerances: in the base color view the largest channel
  difference is 2 of 255 or less (the sRGB decode and encode), in the lit view the fitted ambient specular is 0.016 and
  the largest difference about 1.3 (measured on the RGBA8 screenshots of both scenarios with the same model before it
  became the test).
- **Batches**: 2,049 items draw in 6 batches and 6 draw calls (the backdrop, the shells, two front arrays, two spine
  arrays), 6,148 instances, none culled; the lit pass took 0.41 ms of GPU time at 1920 x 1080 (Release).
- **Memory** (`gpuMemory.collectionArrays`, exactly the computed bytes): 2,049 items, arrays of {2048, 64} layers per
  class: 82,925,568 bytes (79.1 MiB) in BC1, 163,975,680 (156.4 MiB) in RGBA8, four allocations. Growth: 2 x 64 layers
  take 2,512,896 bytes in BC1 (4,968,960 in RGBA8); 2 x 192, 7,538,688 (14,906,880), with a peak of 8,710,656
  (17,259,264) while the fronts' old and new arrays both lived. ADR-0011 §9's 39 KiB per item holds (38.3 KiB), and so
  would the 80 MiB for 2,048 layers (76.7 MiB).
- **Uploads** (the `layerUploads` CPU section, Release, the default 1280 x 720 hidden window): 2,049 items' 4,098 layers
  become resident in 64 to 65 frames in BC1 when the frame holds 64 layers (p50 0.44 ms, p95 0.70 ms, max 1.10 ms in the
  quietest run) and in about 65 to 95 frames in RGBA8, whose layers are twice as large and fill the millisecond first
  (p50 0.81 to 1.00 ms, p95 1.00 ms in the quietest run). Frames past the budget come in bursts while the other
  worktrees load the machine: single frames of 2 to 5 ms, and in some runs one of 20 to 45 ms in either format,
  at random frames (`Frame N: ... layer(s) uploaded in ... ms, over the budget` in the probe's log); a run on a quiet
  moment has none past 1.33 ms. In Debug, where a layer costs about 20 us, every frame is time-limited: about 48
  layers a frame (4,098 layers in 85 to 89 frames in BC1, 119 to 127 in RGBA8), the median at the budget (1.00 to
  1.006 ms). The smoke case checks the median (at most 1.05 ms); the p99 and maximum
  belong to the gate's quiet-machine measurement (and step 12's "no frame over its upload budget").
- **Growth**: the 63 items' 126 layers are resident by frame 2 or 3; the growth runs at frame 20; the 191 items' 382
  layers are resident by frame 24 to 27 in Release.

Screenshots (under `build/msvc-x64-release/smoke/`, written by the GPU cases): `collection_arrays_chunks_bc1-srgb/
base_bc1-srgb.png` and `lit_bc1-srgb.png` (the 46 x 45 wall), `collection_arrays_growth_bc1-srgb/before_bc1-srgb.png`
(63 items, the rest of the wall empty) and `after_bc1-srgb.png` (191 items), and their `rgba8-srgb` twins, reviewed:
every quad a flat color of its own, the shells' grey frames around them, no pending magenta or cyan anywhere.

## Decisions taken within the step

1. **A test executable drives the smoke case.** Nothing in `retroverse.exe` places collection items before step 10b, and
   a scene-file hook for test items would add a format the product does not need and that 10b would remove; a self-check
   reads back inside the run, but the roadmap's checks are screenshots, frame budgets and the run report. So
   `rv_collection_probe` (tests/collection_probe, built with the tests) runs the real host, renderer and arrays with
   `retroverse.exe`'s options. Steps 10b and 12 add the product path; the probe stays as the arrays' focused check.
2. **Two flags, one unit.** `kLayeredBaseColor` marks a layered material and `kSpineLayer` picks the record's spine layer
   (else the front layer): a front material and a spine material differ in one bit, one function serves both. The unit
   is `CollectionTextureUnit::LayeredBaseColor` = 12, a table of its own after the ambient occlusion units (12 was the
   first free unit; `SceneTextureUnit` keeps the lit pass's own textures).
3. **`materialInstanceBaseColor` beside `materialBaseColor`**, not an overload: `materialBaseColor(uv)` stays for the
   depth and unlit stages that have no instance attributes, and the shader tests key functions by name.
4. **The pending picture in every layer.** `CollectionArrays` uploads layer 0 and copies it on the GPU into every other
   layer of a new or grown array, so that an index read before its picture arrives (or a stale one) shows the pending
   label, never undefined memory. It also commits the storage when the array is made rather than during the budgeted
   uploads. It costs one GPU copy of the array at creation, nothing per frame.
5. **The "LRU of layers" is the order in which free pictures are reused.** ADR-0011 keeps every placed item resident (no
   eviction of a layer in use; the hero textures are step 12's LRU). Released layers keep their pictures, so an item
   that comes back (a refresh that drops and restores it, a placement moved) needs no upload; a new key takes never-used
   layers first and then the least recently released. Ties cannot happen (a release clock).
6. **The budget predicts the next layer.** The meter lets a layer go only when the time taken plus a moving average of
   the measured layers fits the millisecond (the first layer of a frame always goes, so that uploads progress whatever a
   layer costs): an upload loop that checked the clock after each layer would overrun by one layer every frame. The
   smoke case bounds the median of the `layerUploads` section; the percentiles above it depend on the machine's load
   (below).
7. **`--no-texture-compression` is decided in `SceneGraphics`**, the one place every application of `rv_app` creates its
   renderer, and exposed as `collectionArrayFormat()`; the collection (10b, 12) reads it there, and so does the probe.
8. **No new `GpuFormat`.** The fallback is `Srgb8Alpha8` and BC1 is `Bc1`, both from step 0d; a separate memory kind is
   not needed either: the arrays record `CollectionArrays`, so `SceneGraphics::finishRun`'s check of the cache's textures
   is unaffected.
9. **CLAUDE.md, the roadmap and the architecture page are not edited here** (see "For the merge").

## Notes for later steps

- Step 10b: create `gl::CollectionArrays` with `SceneGraphics::collectionArrayFormat()`, the family's plastic colors
  as `pendingColors` and the room's placements plus free slots per class; one layered material per array and class
  (`MaterialRegistry::addLayered(key, label, desc, {}, {&arrays.array(cls, index), cls == Spine})`, then `upload`), the
  item's material chosen by the array its `LayerSlot` names. When the room grows, `reserve` at a frame boundary, then add
  the materials of any new array. Keys: anything stable per item and class (the game id).
- Step 12: produce `LayerImage`s of the run's format (`layerSize` / `layerLevelCount` tell the sizes: full size for BC1,
  half for RGBA8) from the `.rvtex` levels; `submit` in priority order; call `uploadPending()` once per frame on the main
  thread and write each completed `LayerUpload`'s layer into the item's record (front or spine). An item whose
  `acquire` answers `resident` needs no submit. `pendingUploads()` and `residentLayers()` feed the run report's
  residency. If the median frame cannot hold its budget on the reference machine, ADR-0011 §6's remedy is a PBO ring.
- Step 8: the static groups keep using `InstanceData::attributes.frontLayer` / `spineLayer`; batches per array (the
  material differs) as ADR-0011 §3 says.
- `rv_collection_probe` can show the arrays at a glance: `build/msvc-x64-debug/bin/rv_collection_probe.exe
  --probe-scenario chunks --exit-after-frames --frames 240 --hidden --screenshot build/screenshots/arrays.png --data-dir
  build/rvdata`.

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Running the app": `--no-texture-compression` keeps the collection's texture arrays in RGBA8 sRGB at half the
    resolution instead of BC1 (also without the S3TC extensions); the log names the choice (`Collection texture
    arrays: ...`). Drop it from the list of options refused until their steps.
  - "Build and test", outputs: `rv_collection_probe.exe` (the GPU smoke tests' driver of the collection's texture
    arrays, `tests/collection_probe`).
  - "Layout and layering", tests: `tests/collection_probe` (`rv_collection_probe`, built with the tests; its
    `probe_layout.hpp` is shared with `tests/smoke/test_collection_arrays.cpp`).
  - "GPU smoke tests": the collection arrays (2,049 items in two arrays per class, arrays grown mid-run, BC1 and the
    RGBA8 fallback) among the cases.
- **docs/roadmap.md**: step 6 done, with the figures above.
- **docs/architecture.md**: `rv_render` holds the texture arrays and the collection's arrays (`Texture2DArray`,
  `CollectionArrays`), `rv_render_uniforms` their layout, allocator and budget; the unit table gains
  `CollectionTextureUnit` (12).
- **BACKLOG.md**: "Texture array for cartridge labels" stays open until the generated room draws its cartridges with
  the arrays (10b, 12); the test room keeps its per-label textures (ADR-0011 §4).
