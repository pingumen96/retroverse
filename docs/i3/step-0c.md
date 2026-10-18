# I3 step 0c: the 80-byte instance record

Step 0c of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice") widens the record every instanced draw reads from a
`mat4` (64 bytes) to `scene::InstanceData` (80 bytes, ADR-0011 §2): the model matrix, then a `uvec4` of attributes at
attribute location 8 (front layer, spine layer, flags, pick id). It goes through `RenderItem`, `BatchPlan`,
`InstanceBuffer`, `GpuMesh` and the mesh shaders, so that the collection steps (6: the texture arrays read the layers;
8: static instance groups write records; 13a and 13b: the highlighted and hidden flags) only fill it. Every item of the
existing scenes writes zeros beside its matrix: the reference scenes draw the same pixels, batches and draw calls.

## What was built

### `rv_scene`

- `rv/scene/instance_data.hpp` (new, header only): `InstanceAttributes` (`frontLayer`, `spineLayer`, `flags`, `pickId`,
  four `std::uint32_t`, 16 bytes, all zero by default) and `InstanceData` (`glm::mat4 model`, then `attributes`; 80
  bytes), both with defaulted `operator==`, their size and member offsets fixed by `static_assert`; the flag bits in
  `scene::instance_flags`: `kHighlighted` (1), `kHidden` (2), `kUnavailable` (4).
- `RenderItem` gained `InstanceAttributes attributes` after `castsShadow`, zero by default, so every existing
  aggregate initialization (`SceneInstance`, `GpuModel::appendRenderItems`, the tests) is unchanged. Like
  `castsShadow`, the attributes are not part of the batch key: items of one batch differ in their layers and pick ids.
- `BatchPlan::instanceTransforms` (`std::vector<glm::mat4>`) became `BatchPlan::instances`
  (`std::vector<InstanceData>`); the planner writes `{item.model, item.attributes}` for every planned item, the view's
  and the shadow map's alike. Culling, sorting and batching are untouched.

### `rv_render`

- `InstanceBuffer::write` takes spans of `scene::InstanceData` (one list, or several into one region as before);
  `kRecordBytes` (80) sizes the regions, whose capacity stays counted in instances (the run report's
  `instanceCapacity` reads the same). The growth log line reads `... grown to 3 regions of N instances (K KiB)`.
- `GpuMesh::kInstanceStride` is `sizeof(scene::InstanceData)`; `kInstanceAttributesLocation` (8) is new.
  `attachInstanceBuffer` declares the four matrix columns at locations 4 to 7 (offsets 0, 16, 32, 48) and the
  attributes at location 8 as four unsigned integers (offset 64).
- `VertexArray::setUintAttribute` (new) declares an integer attribute with `glVertexArrayAttribIFormat`
  (`GL_UNSIGNED_INT`, never converted to floats); it shares its checks with `setFloatAttribute` (`checkAttribute`).
  `GLDevice` requires the entry point among the 4.5 functions it checks at start.
- `SceneRenderer` writes the records of both plans in its one write per view, as it wrote the matrices.

### Shaders

- `pbr/pbr.vert` (the vertex stage of the lit, unlit and depth programs) reads `layout(location = 8) in uvec4
  aInstance` and hands it on unchanged as `layout(location = 4) flat out uvec4 vInstance`; `common/surface.glsl`
  declares the matching input for the lit and inspection fragment stages. Nothing reads it yet: the depth and unlit
  fragment stages do not declare it, and no output depends on it.
- `common/common.glsl` documents the record's locations and declares the flag bits `RV_INSTANCE_HIGHLIGHTED`,
  `RV_INSTANCE_HIDDEN` and `RV_INSTANCE_UNAVAILABLE` (1, 2, 4), mirrors of `scene::instance_flags`.

### `rv_app`

- `SelfCheckCanvas` (the sRGB and FXAA self-checks) uploads its one instance as a `scene::InstanceData` instead of a
  bare matrix: with the 80-byte stride a 64-byte buffer would leave location 8 reading past its end. This is the only
  file outside `rv_scene`, `rv_render` and the shaders that the step touches; nothing else in the app builds instance
  data.

## Tests

- `tests/unit/scene/test_batch_planner.cpp`: the existing cases on `BatchPlan::instances`; new: the record's size,
  offsets and zero defaults (`RenderItem{}` included) and the flag values; every planned instance, of the view and of
  the shadow map, carries its own item's matrix and attributes, and items of one key with different attributes still
  make one batch (opaque in submission order, blended back to front).
- `tests/unit/integration/test_shader_sources.cpp`: `pbr.vert`'s inputs are the vertex at 0 to 3, `mat4 aModel` at 4
  and `uvec4 aInstance` at 8, matching `InstanceData`'s offsets; the flat varying `vInstance` at location 4 is written
  from `aInstance` and declared by `common/surface.glsl`; the `RV_INSTANCE_*` bits equal `scene::instance_flags`.
- `tests/unit/integration/test_reference_scenes_batching.cpp`, `tests/unit/scene/test_reference_scenes.cpp`: the
  renamed plan field (the counts they assert are unchanged).
- `tests/smoke/test_reference_scenes.cpp`: one comment (the capacity counts instances). The GPU smoke tests are
  otherwise unchanged and pass (the reference scenes' bounds, the self-checks through `SelfCheckCanvas`, golden G01).
  No GPU test sets non-zero attributes: no scene or item can yet (the collection steps add them); the manual check
  below proved the path once.

## Measurements

On the reference machine (RTX 2080 SUPER, driver 617.42), with the other I3 main steps building and testing in their
worktrees at the same time (about 20% CPU load), so the timings are informative (the roadmap: the gate measures on a
quiet machine).

- **Golden G01**: the Debug run of its JSON's command writes the golden's bytes (SHA-256 `a4aeebd2...f3176`, 0 of
  2,073,600 pixels apart at threshold 0); `gpu.golden/G01` passes in Debug and Release.
- **The reference scenes**, Release before (main at `d167433`) and after, `--smoke --frames 5`, from their run reports:
  every last-view figure equal (items, culled, batches, draw calls, instances, triangles, shadow and depth draw calls,
  instance capacity), and the 1920 x 1080 screenshots byte-identical.

  | Scene | Batches | Draw calls | Instances | Shadow draw calls / instances | Instance capacity |
  |---|---|---|---|---|---|
  | `m0_testroom` | 129 | 129 | 225 | 101 / 179 | 1,024 |
  | `material_test` | 26 | 26 | 26 | 0 / 0 | 1,024 |
  | `instancing_stress` | 4 | 4 | 1,001 | 3 / 1,000 | 2,048 |
  | `instancing_growth` | 2 | 2 | 37 | 0 / 0 | 1,024 |

- **`instancing_stress` frame time**, Release, `--no-vsync --hidden --exit-after-frames --frames 600` (the default
  1280 x 720 window), the mean frame time of `Frame stats of the run`, before and after interleaved (before-after,
  after-before, before-after), the median of three runs each. Four series of three:

  | Series | Before (ms) | After (ms) | Median before | Median after | Change |
  |---|---|---|---|---|---|
  | 1 | 1.11, 1.21, 1.29 | 1.19, 1.36, 1.30 | 1.21 | 1.30 | +7.4% |
  | 2 | 1.12, 1.14, 1.11 | 1.19, 1.14, 1.11 | 1.12 | 1.14 | +1.8% |
  | 3 | 1.20, 1.11, 1.11 | 1.10, 1.07, 1.07 | 1.11 | 1.07 | -3.6% |
  | 4 | 1.08, 1.09, 1.07 | 1.08, 1.09, 1.08 | 1.08 | 1.08 | 0.0% |

  The first series ran while another worktree's tests were running and spread over 1.11 to 1.36 ms within each
  build; ten interleaved rounds right after it gave medians of 1.145 ms before and 1.135 ms after (-0.9%). The later
  series are within the row's 5%. The extra 16 bytes per instance are about 31 KiB a frame for this scene's 2,001 instances.
- **The attributes reach the shaders exactly**: a scratch build (not committed) gave every planned instance of the test
  room the attributes (index + 1, 7, 0x5A, 0xC0FFEE00 | index & 0xFF), and a scratch copy of `pbr.frag` (under
  `--asset-root`) painted green where `vInstance` held exactly those values and red elsewhere: every lit surface came
  out green (the only red pixels, 930, were the CRT's unlit red bar), 0 GL messages. A float format at location 8 would
  have failed this.
- **Tests**: Debug, `./scripts/test.ps1 -Preset msvc-x64-debug-gpu`: 1,275 unit and 29 GPU cases passed; Release,
  `./scripts/test.ps1 -Preset msvc-x64-release-gpu`: 1,275 unit and 27 GPU cases passed. `RV_LAUNCH_BACKGROUND` set,
  `RV_LAUNCH_ALLOWLIST` not set.
- **Builds**: Debug, RelWithDebInfo and Release with 0 warnings.

## Decisions taken within the step

1. **`RenderItem` keeps `model` and adds `attributes`**, rather than holding an `InstanceData`: the matrix is read by
   name in the app, the overlay, the asset preview and the tests (bounds, culling, picking the test room's objects),
   and an extra defaulted member leaves every aggregate initialization as it was. The planner composes the record.
2. **`BatchPlan::instanceTransforms` renamed `instances`**: it no longer holds transforms; the rename is confined to
   `rv_scene`, `rv_render` and their tests.
3. **The attributes travel to the fragment stages now** (`vInstance`, flat), so that step 6 reads its layers and the
   flag steps their bits without touching the vertex stage again. An unread varying costs nothing measurable (see the
   frame times) and changes no pixel.
4. **The flag bits are fixed here** (highlighted 1, hidden 2, unavailable 4, ADR-0011 §2's order), in C++ and GLSL with
   a test keeping them equal; what each flag does (the outline, skipping a hidden instance, dimming) belongs to the
   steps that use them (8, 13a, 13b, 12), not to this one.
5. **`VertexArray::setUintAttribute` instead of a format parameter** on `setFloatAttribute`: the integer path is a
   different GL call (`glVertexArrayAttribIFormat`), and a `uvec4` input fed through the float path is undefined (the
   values arrive converted to floats); a separate name makes the choice visible at the call site.
6. **Capacity stays in instances**: `kMaxRegionCapacity`, `regionCapacity()` and the run report's `instanceCapacity`
   count records, not bytes, so the smoke tests' capacity checks and `kInitialInstanceCapacity` (1024) are unchanged;
   the buffer is 25% larger in bytes (the growth log's KiB).

## Notes for later steps

- A collection item sets `RenderItem::attributes` (or, in a static group, writes `InstanceData` directly); everything
  else leaves it zero. Layer 0 of each array is the pending picture (ADR-0011 §4), so a zero record on a layered
  material shows the pending label, never garbage.
- Fragment stages read the attributes through `vInstance` (`common/surface.glsl`; a stage that does not include it
  declares `layout(location = 4) flat in uvec4 vInstance;` itself). The depth and unlit stages do not declare it today.
- `scene::instance_flags` and the `RV_INSTANCE_*` constants change together (the shader source test fails otherwise).
- A buffer attached with `GpuMesh::attachInstanceBuffer` must hold a whole 80-byte record per instance drawn: a static
  instance group (step 8) uploads `InstanceData` as it is.

## For the merge

- **docs/architecture.md** (not edited by the step, which runs beside 0b and 0d):
  - "Frame pipeline (M0)", the diagram: `--InstanceBuffer--> one write of both plans' matrices` becomes `one write of
    both plans' instance records (80 bytes: matrix and attributes)`.
  - "Rendering path": "It copies every instance matrix of both plans" becomes "It copies the instance record of every
    item of both plans (`scene::InstanceData`: the model matrix and its attributes, front layer, spine layer, flags and
    pick id; zero for items that are not collection items)"; "Mesh shaders read the instance matrix at vertex attribute
    locations 4..7 (divisor 1)" becomes "Mesh shaders read the matrix at vertex attribute locations 4..7 and the
    attributes as a `uvec4` at location 8 (divisor 1), handed to the fragment stages as the flat `vInstance`".
  - The `scene` row may name `InstanceData` beside the render items and the batching plan.
- **docs/roadmap.md**: step 0c done, with the figures above.
- **CLAUDE.md**: nothing to change (the growth log line it quotes still starts `Instance buffer
  'SceneRenderer.Instances' grown to`).
