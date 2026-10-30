# I5: the memory margin

A small step before Gate M3/M4 (branch `m3/memory-margin`). I5 step 3's review measured the busiest real floor
(`computer`, both screens playing) at **505.8 MiB** in a 1920 x 1080 window, 6.2 MiB under the acceptable 512 MiB
(`docs/roadmap.md`, "Acceptable performance"; `docs/i5/step-3.md`, "Measurement"). The render targets took 88.5 MiB
of it, and `--screenshot` added a second full set. This step takes 28.3 MiB out of the 1080p targets and drops the
screenshot's second set; the shading, the Balanced profile's features and the Low profile are unchanged.

## What was trimmed

- **The plan of the targets** (`rv_render_uniforms`, `view_target_plan.hpp`, GPU-free, unit-tested): each image of
  an output's `gl::ViewTargets`, with its format, its size, the first and last pass of a frame it lives through
  (`gl::ViewPass`), and the image whose storage it takes over. `gl::viewTargetsBytes()` gives the bytes they allocate.
  `ViewTargets::create()` takes its formats, sizes and the depth's flags from the plan, and in debug builds asserts
  that its framebuffers record exactly `viewTargetsBytes()`. An image that takes another's storage is a texture view of
  it (`Framebuffer::createAlias`), which allocates and records nothing.
- **The output has no depth buffer of its own** (7.9 MiB at 1080p). The debug lines draw into
  `ViewTargets::outputWithSceneDepth()`, the output's color over the HDR target's depth (views, no storage). No pixel
  changes.
- **The emitted light in `GL_R11F_G11F_B10F`** (7.9 MiB), and **the tonemapped image written over it** (7.9 MiB).
  The post pass reads the emission with `texelFetch` only at the texel it writes, after `glTextureBarrier()`: the one
  feedback the GL 4.5 spec defines. `post/post.frag` says so beside its read.
- **The bloom in `GL_R11F_G11F_B10F`** (2.6 MiB), and **its first level in the half-resolution occlusion's storage**
  (2.0 MiB): the occlusion's blur is over before the bloom starts.
- **A screenshot at the window's size is drawn through the window's targets** (`SceneGraphics::captureScreenshot()`).
  A screenshot of another size draws through targets made for it alone, with the window's let go of first. One set is
  held at any time.
- **The bind sites** (`ssao_pass.cpp`, `post_processor.cpp`) point to `viewTargetPlan()` for the lifetimes. Reading
  the raw occlusion after its blur, or the emission after the post pass, is forbidden. Their texture units are unbound
  once the pass is over, so no later shader can sample a target it draws into.

Not done: post-processing intermediates at reduced resolution (they change the look's sharpness), the shadow map
(16 MiB, its precision), a 16-bit radiance (`GL_R11F_G11F_B10F` there would band the image).

**The in-place write is validated on the reference GPU only** (NVIDIA RTX 2080 SUPER, driver 621.01): the views of
`GL_SRGB8_ALPHA8` over `GL_R11F_G11F_B10F` storage, read and written in one pass. The FXAA self-check now checks it:
with the bloom on, the image written over the emission must equal, byte for byte, the one written into the output
without anti-aliasing. If another driver fails it, the fallback is one line of `view_target_plan.cpp`: Tonemapped's
storage not shared (no `storageOf`, 7.9 MiB back at 1080p).

## Figures

The renderer's own accounting (`gpuMemory.kinds.targets` at its peak: the view targets and the key light's 16 MiB
shadow map), `m0_testroom`, Release, driver 621.01:

| Run | Before | After |
|---|---|---|
| 1920 x 1080 window | 88.5 MiB | 60.2 MiB |
| 1920 x 1080 window, `--screenshot` at 1920 x 1080 | 161.0 MiB | 60.2 MiB |

The busiest real floor at 1080p with both screens playing should therefore hold about **477.5 MiB** (505.8 - 28.3),
the same with a screenshot. Gate M3 measures it again on a quiet machine.

## The pixels

The output's depth, the views and the screenshot's targets change no pixel. The two formats do, slightly, and FXAA
turns a few of those 1/255 steps into an edge decided the other way. On the test room's 1080p screenshot, 73 pixels
differ by more than 6/255 (at most 38/255, 0.004%, on the edges of the neon sign and the lamp), against the 0.5% the
goldens allow. Without the formats the step would save 7.9 MiB at 1080p, short of the 30 it set out to regain.

**Goldens.** G01 is not rewritten: it passes within its tolerance (`docs/testing/reference-hardware.md`). G10 compares
itself with G02's golden, which needs G02 rendered the same way, so the room goldens G02 to G20 are rewritten on purpose
(`RV_UPDATE_GOLDEN=1`, Debug, driver 621.01; their PNGs are equal in Release). Each was first compared with its old PNG:
36 (G07) to 1,417 (G15, the arcade's lit cabinets) pixels beyond 6/255, all on the edges of emitters and their glow,
none above 0.07%.

## Proofs

- Unit (`tests/unit/render/test_view_target_plan.cpp`): every image once, in order, within its passes; every image
  that takes storage lives after its owner (or in place in its last pass), at its size and view class; the bytes at
  720p, 1080p and an odd size. `tests/unit/app/test_self_check.cpp`: the in-place comparison.
- GPU (`tests/smoke/test_measurements.cpp`, `gpu.measurements/the render targets at 1080p ...`, at `--fixed-dt`):
  the run report records the window's drawable size (`window`) and the targets each screenshot used
  (`screenshot.targets`: `window` or `own`). The expected bytes follow the reported drawable, so a scaled display
  still tests the window's path. A window's peak is its set and the shadow map, exactly. A screenshot of its size from
  a small window uses its own set, and the window's set is let go of meanwhile. The same screenshot from the window uses
  the window's targets, with no extra bytes, and is equal to the other byte for byte.
- `gpu.self-check/fxaa`: the in-place write, as above.

**Results** (after the review's fixes). Debug (`./scripts/build.ps1`, no warning; `./scripts/test.ps1`): 2216 of 2216
unit tests passed. Release (`./scripts/build.ps1 -Preset msvc-x64-release`; `./scripts/test.ps1 -Preset
msvc-x64-release-gpu`): 2384 tests (2216 unit, 168 GPU) passed, one skipped by design (the overlay's pick). G01 to G20
ran, none skipped: G02 to G20 0/255 from their PNGs, G01 73 pixels beyond 6/255 as before. The 18 golden cases also pass in Debug, where the
targets' asserts run (`-Preset msvc-x64-debug-gpu -Filter 'gpu\.golden/'`). The reports show a
1920 x 1080 drawable, `own` from the small window and `window` from the 1080p one, and the FXAA self-check's in-place
image 0 of 4,096 pixels apart.

## The room preview's time

Secondary goal: I4b step 6 measured the real `room:amstradcpc`'s regeneration preview at 1.6 s quiet and 3.2 s under
load (limit 2 s). A hidden bench (`tests/unit/app/test_world_regenerate.cpp`, `app/bench: a large room's preview, its
read and its regeneration`, `[.bench]`, by hand in Release) previews `esde/snes-stress`'s `room:snes` (2,000 titles in
one room) in 59 to 75 ms. A large room alone previews in about a tenth of a second, so the real library's time comes
from the world around the room. Nothing was changed: the faster digests are in BACKLOG ("Measurement"), and Gate M3
measures the real preview again on a quiet machine.
