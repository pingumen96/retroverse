# ADR-0011: Collection rendering and cover textures

- Status: accepted
- Date: 2026-10-18
- Relates to: ADR-0001 (the OpenGL 4.5 core baseline stays: the S3TC formats are an optional extension with a tested
  fallback), ADR-0004 (point 9: the render loop keeps its pacing only on a machine at most half busy, so the image
  workers run below normal priority), ADR-0008 §5 and §7 (media found as ES-DE finds them, only ever read)

## Context

The M2 room draws one cartridge per title of the system (ADR-0010): 890 on the reference SNES collection, 2,000 on the
stress fixture that I3 measures with (the plan's risk 3: "first measurement already in I3 with a 2k fixture"), each with
its own cover on its front label and its own title on its spine. Specification §8.2 and style §12.2-12.3 ask for derived
textures kept in Retroverse's cache with the originals untouched, a readable generated label where the image is missing,
covers never stretched (a spine is generated from the title, not squeezed from the cover), several resolutions with the
better one replacing the thumbnail near the object, and no high-resolution load for what cannot be seen; specification
§14.4 and §18.4 ask for texture arrays or atlases, an LRU GPU cache, instancing and images requested by visibility and
distance; SC-12 that exploring never starts thousands of texture uploads at once.

What the renderer has today (I0 to I2):

- The instance record is a `mat4` (64 bytes, locations 4 to 7) and the material is part of the batch key: each of the
  test room's 96 labels has a material and a `Texture2D` of its own, 96 of its 129 draw calls (BACKLOG "Texture array
  for cartridge labels"); the GPU smoke test bounds the room at 160.
- `scene::BatchPlanner` culls and sorts every item every frame, twice when the key light casts (the view and the shadow
  map), and copies the matrices twice: fine for hundreds of items, not for several thousand parts that never move.
- `gl::TextureCache` decodes and uploads synchronously on the main thread, only from the asset root, with no eviction;
  `Texture2D` generates its mips with the driver.
- The generated glad already has `GL_EXT_texture_compression_s3tc` (its README's command line: the RGB and RGBA DXT1,
  DXT3 and DXT5 enums) but not `GL_EXT_texture_sRGB`, which defines `GL_COMPRESSED_SRGB_S3TC_DXT1_EXT`; vcpkg's `stb`
  already ships `stb_image_resize2.h`, `stb_dxt.h` (BC1 and BC3 encoders) and `stb_truetype.h`; the bundled Atkinson
  Hyperlegible font (OFL) is in `assets/ui/fonts`.

The survey of the reference media (2026-10-18): the 1,535 SNES covers are opaque PNGs (alpha unused), 680 x 497 for
91.5% (aspect 1.368) and 476 x 864 portrait (0.551) for Japanese and BS-X ones, 815 MiB on disk, about 7.5 ms to decode
and 7.5 ms to downscale each on one core. At 256 px on the long side with mips, every cover takes 246.5 KiB in RGBA8 and
30.8 KiB in BC1. A cartridge 3.2 m away covers about 50 x 30 px at 1920 x 1080; a spine 2.0 cm wide covers 25 px at 1 m.

## Decision

### 1. A collection layer beside the scene's objects

The room's static part (shell, shelves, display cases, furniture, lights) goes through `SceneDescription` and
`app::SceneInstance` as every scene does. The cartridges do not: `app::CollectionInstance` holds them, one entry per
placement, with the media family's shared meshes (the shell, the front label quad, the spine quad: one mesh each per
family, never per game), the placement's transform, its GameId, its texture layers and its flags. A cartridge is never a
collider (the shelves are) and never a `SceneObject` with textures, which would make a material and a texture per game.

### 2. The instance record: 80 bytes

`scene::InstanceData` = the model matrix and a `uvec4` read at location 8 with `glVertexArrayAttribIFormat`: `x` the
front layer, `y` the spine layer, `z` flags (highlighted; hidden, while the item is pulled out or inspected and drawn by
the dynamic path instead; unavailable, drawn dimmed), `w` the pick id. `RenderItem`, `BatchPlan`, `InstanceBuffer` and
`GpuMesh::kInstanceStride` carry it for every item; items that are not collection items write zeros, so the reference
scenes draw the same pixels (golden G01 stays 0 pixels apart). The depth and shadow programs ignore the extra attribute.
An SSBO indexed by `gl_BaseInstance + gl_InstanceID` would need GL 4.6 or `ARB_shader_draw_parameters`, and packing the
layers into the matrix's last row breaks `pbr.vert`'s normal matrix: both rejected.

### 3. Static instance groups

The collection's items are planned once, not per frame: a `gl::StaticInstanceGroup` takes pre-sorted batches (per media
family part and texture array chunk) and their instances, uploads them once into a buffer of its own (rewritten only
when the room changes: a placement added, an item lifted or put back), and keeps one bounding box per cluster (a shelf
level of a module, a display case). Each view, the shadow map's included, culls the clusters and draws
`firstInstance`/`count` ranges with `glDrawElementsInstancedBaseInstance`; the dynamic path (the inspected item, the
pulled-out one, animations) stays as it is. Cartridge shells cast shadows (a row reads as one solid block); labels and
spines do not (coplanar quads). LOD for far rows is not needed at the measured budgets (2,000 cartridges of about 130
triangles are under 1 M triangles a pass) and stays a hook for I8.

### 4. Texture arrays per size class

- Two classes, each an immutable `GL_TEXTURE_2D_ARRAY` with a full mip chain, sRGB: **front** 256 x 188 (the cover's
  1.368 aspect) and **spine** 40 x 256 (the 2.0 x 12.5 cm spine). Layer 0 of each is the pending picture (a neutral
  label in the family's plastic colour). An array holds at most 2,048 layers (OpenGL 4.5's guaranteed minimum); a room
  with more items gets a second array of the class, a second material and one more batch.
- **Sized to the room**: immutable storage fixes the layer count at creation, so an array is created with the room's
  placements and free slots plus one (layer 0), rounded up to a multiple of 64 and at most 2,048, not with 2,048 layers
  for every room; a room that outgrows it (a module added, the room grown) gets a larger array at a frame boundary, the
  resident layers copied into it on the GPU (`glCopyImageSubData`, core since 4.3) before the old one is deleted.
- **Format: BC1 sRGB** (`GL_COMPRESSED_SRGB_S3TC_DXT1_EXT`), encoded on the workers with `stb_dxt`: covers are opaque,
  and a 256 x 188 layer takes 32 KiB with its mips instead of 257 KiB, so **every placed item stays resident** and the
  shelf tier needs no LRU (2,048 items: 64 MiB for the fronts, 14 MiB for the spines). Mip levels whose sides are not
  multiples of 4 (94, 47, 5, ...) are encoded as whole blocks with their edge pixels repeated and uploaded whole, as
  S3TC requires. glad keeps `GL_EXT_texture_compression_s3tc` and gains `GL_EXT_texture_sRGB` by its README's procedure
  (no hand edit, no new dependency; `docs/third-party.md` updated); should glad2 not emit that extension's enums for the
  core profile, `rv_render` declares the one value it needs from the Khronos registry, glad untouched. Both extensions
  are checked at run time (`GL_EXTENSIONS`); without them, or with `--no-texture-compression` (the option the fallback
  is tested with), the arrays are RGBA8 at half the resolution (128 x 94 and 20 x 128), twice the memory of BC1 at full
  resolution. BC7 needs an encoder the project does not have; it is not adopted.
- The lit shader samples the layer through a `sampler2DArray` on a new texture unit (the unit table in
  `uniform_blocks.hpp`, shared by C++ and GLSL) when the material has the `RV_MATERIAL_LAYERED_BASE_COLOR` flag: all the
  fronts of a family are one batch, all its spines another, whatever the number of games.
- The test room keeps its per-label 2D textures and its 160 draw-call bound (its golden must not move); the generated
  room is where the arrays are measured. Moving the test room onto them is I8's (BACKLOG).

### 5. What a face shows (style §12.2, §21)

- **Front label**: the `covers` image fitted to 256 x 188 without stretching: scaled to cover the label and cropped when
  that crops at most 8% of either side, else scaled to fit and the remaining bands filled with the mean colour of the
  image's edge (the portrait covers get side bands). `physicalmedia` (a photograph of the whole cartridge with a
  transparent background, in two regional shapes) does not match a label and is not used on it; `3dboxes` (a rendered
  box in perspective) is no flat texture either. Both are shown in the media viewer (ADR-0012).
- **Generated front label** for a game without a cover: the title, the system's name and the year (catalog data,
  specification §19.4), set in the bundled font on a colour of the theme's label palette chosen from the placement's
  seed, with the family's frame and stripes: readable, never an imitation of a real layout. The fixtures behind
  committed goldens give their system an invented full name, so that no image under `assets/` shows a trademarked name
  (CLAUDE.md, style §17.2).
- **Spine**: always generated. The title in one line, or two when it is long (split at `" : "` or `" - "` first, then
  between words; an ellipsis when it still does not fit at the minimum size), reading top to bottom (the convention of
  the US and UK spines the collection mostly holds), in the bundled font on the cover's dominant colour darkened or
  lightened until the text's contrast is at least 4.5:1, or on the generated label's colour without a cover.
- **Text**: rasterized with `stb_truetype` from the bundled Atkinson Hyperlegible at twice the layer's resolution and
  reduced, so that small glyphs keep their shapes; a character the font lacks is drawn as the font's replacement glyph,
  and a title of nothing but such characters uses the catalog's cleaned file-name title. `stb_truetype` is not hardened
  against malformed fonts: it only ever loads the bundled file whose SHA-256 matches its entry in
  `assets/registry/assets.json`, never a system or user font. This replaces the 5 x 7 bitmap font for collection items
  (the test room keeps it).

### 6. The streaming loader

- A `core::WorkerPool` (bounded, priorities, cancellation by generation, joined at shutdown within a bound) of
  `min(6, hardware threads - 3)` threads, at least 1 (5 on the reference machine's 8 cores, leaving the main thread,
  the catalog worker and the video decoder theirs), at below-normal thread priority (ADR-0004 point 9), decodes and
  prepares images: read with `core::readFileShared` (the media are only read, never written; paths logged at debug level
  only), decoded with stb, fitted with `stb_image_resize2`, the mip chain computed in linear light (a box filter on
  linear values, encoded back to sRGB), BC1 encoded, the dominant colour taken; spines and generated labels are drawn on
  the same workers. The world's generation job (ADR-0010 §12) runs on the same pool at the highest priority.
- **Paths**: the room's media paths (cover, screenshot, title screen, video and the media viewer's kinds) are derived on
  the catalog worker in one job per snapshot from the stored presence and extension (`esde::mediaRelativePath`, as
  ES-DE finds them, ADR-0008 §5), never by listing media folders per item, and handed to the loader with the plan.
- Priorities: the focused or inspected item first (also its hero texture, point 8), then the items of the visible
  clusters by their projected size, then the rest of the room by distance from the walker. A new room or plan bumps the
  generation and drops what is queued for the old one. While a launched game runs the pool takes no new job (the
  running ones finish) and nothing is uploaded; it resumes after the return (ADR-0012 §6).
- Uploads happen on the main thread under a **budget per frame** (1 ms, or 64 layers, whichever comes first) with
  `glCompressedTextureSubImage3D` per level; a layer of 32 KiB needs no pixel buffer at that size (a PBO ring like
  `VideoTexture`'s is the remedy if the budget is not met). `glGenerateTextureMipmap` is never called on an array.
- Until its layers arrive an item shows layer 0; the swap is one instance-record write in its static group.
- **An unavailable media root** (unplugged, unreachable: the catalog's media unknown) or a cover that cannot be read
  never blocks the room: the loader serves what the cache holds (point 7, found without the source), an item the cache
  cannot serve shows its generated front label (a title is better than a blank one), and the loader tries again after
  the catalog's next refresh. Nothing is ever written under the media root.
- Runs with a fixed frame time (`--fixed-dt`) wait between frames until the layers of every item in the view are
  resident, in the host's one lockstep (with the catalog and the plan, ADR-0010 §12), at most 10 s between two frames,
  so that screenshots and goldens never catch a half-loaded shelf; every other run never waits. The run report records
  the layers resident and pending, the cache's hits and misses, when the plan was shown and when the last layer of the
  room became resident; the overlay's Stats panel shows the same (specification §18.4's dashboard).

### 7. The derived-texture cache

`<user data>/cache/textures/<library id>/<class>/<2 hex>/<sha256>.rvtex`, the name the SHA-256 of (the pipeline's
version, the class, the media kind, the media file's path relative to the media root as the catalog derives it), so
that the cache never names the user's files and an entry is found without touching the source. A file holds a header
(magic, version, class, format, width, height, mip count, dominant colour, the source's size and modification time) and
the levels; written atomically by the workers. **Invalidation**: when the source can be read, its size and time are
compared with the header's and a different source is decoded again and its entry replaced; when it cannot (the media
root unavailable) the entry is used as it is; a file that fails its checks is deleted and rebuilt; a new pipeline
version names new entries. Spines and generated labels are cached by (pipeline version, class, text, colour, font
file's SHA-256). **Size**: the cache is derived and can be deleted at any time; it is kept under 512 MiB: the loader
sets an entry's modification time when it first serves it in a run (Windows does not keep reliable access times), and
at start the workers remove the oldest entries beyond the bound, never one the shown plan uses. Nothing is ever written
under the media root (ADR-0008 §7); the cache is under the guarded user data directory, so tests and
`scripts/real-library.ps1` keep it under their `--data-dir` in `build/`.

### 8. Hero textures

The focused, pulled-out or inspected item swaps its front for a **hero texture**: the cover at its source size capped at
1,024 px on the long side, RGBA8 sRGB with mips, a `Texture2D` in a least-recently-used set of 8 (at most 32 MiB), drawn
by the dynamic path; also the back cover and the other images the media viewer shows. It is loaded on demand (never
cached on disk) and replaces the layer when ready; while the media root is unavailable the item keeps its layer.

### 9. Budgets (1920 x 1080, Release, the reference machine's RTX 2080 SUPER)

| Item | Budget |
|---|---|
| Collection arrays (fronts + spines, BC1 with mips) | 39 KiB per layer: 34 MiB for the reference room's 890 items (layers for its free slots extra), at most 80 MiB for 2,048 layers |
| Hero textures | at most 32 MiB (8 at 1,024 px) |
| All textures of the room, the 80 MiB of procedural surfaces included | at most 256 MiB |
| Uploads of collection layers | at most 1 ms of the main thread per frame |
| CPU planning of the collection (static groups, cluster culling, every view) | at most 0.3 ms per frame for 2,000 items |
| Cold cache (decode, fit, mips, BC1): one cover | 25 ms median on one worker |
| All covers of the room resident, warm cache | 1.0 s after the plan is shown (890 items), 1.5 s (2,000) |
| All covers of the room resident, cold cache | 10 s (890), 15 s (2,000), the room interactive throughout with a frame p99 of at most 33.3 ms |

How they are measured: memory by the renderer's own accounting of what it allocates (bytes per texture from its format,
size, layers and levels, `gpuMemory` in the run report and the Stats panel), not by the driver; the main thread's
upload and planning times by CPU timers around those calls, reported per run as p50, p99 and max; the cold cover time by
a hidden `[.bench]` case in Release, as I2 measured its queries; the residency times from the run report's timestamps
on the `snes-stress` fixture and, through `scripts/real-library.ps1`, on the reference library.

## Consequences

- The fronts and spines of a family are two batches whatever the size of the collection; the BACKLOG item "Texture array
  for cartridge labels" closes for the generated room.
- Every cover of a room stays on the GPU at shelf resolution, which keeps the policy simple (no eviction on the shelf
  tier) up to the 2,048 items an array holds; the hero tier is the only LRU. Rooms of several thousand covers (I4) keep
  the same scheme with more arrays, and the Low profile (I8) can halve the classes.
- A second start reads a few tens of MiB from the cache instead of decoding 815 MiB of PNG, and a room whose media root
  is unplugged still shows the covers it showed last time.
- BC1 can band smooth gradients on covers at 256 px; the label step compares the encoded layers with RGBA8 ones and
  checks the spines' text contrast after encoding. If it fails there, BC3 or RGBA8 spines are the fallback.
- The 80-byte record touches every renderer, planner and instancing test once (I3 step 0c) and costs 16 bytes per
  instance more for the reference scenes.
- No new dependency and no change to ADR-0001's baseline: glad, a vendored generated file, gains one extension by its
  README's procedure (this ADR is the decision CLAUDE.md asks for), and the S3TC path is optional with a tested
  fallback. `stb_image_resize2.h`, `stb_dxt.h` and `stb_truetype.h` belong to the `stb` port the manifest already pins
  (MIT or public domain per file, no notice needed); `docs/third-party.md` names their use in `rv_assets`. The font's
  registry entry names its new use; the SIL OFL 1.1 allows rasterizing it, and the textures are derived data, never
  distributed as a font.
