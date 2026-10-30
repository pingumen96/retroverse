# I5: the memory margin

A small step before Gate M3/M4 (branch `m3/memory-margin`). I5 step 3's review measured the busiest real floor
(`computer`, both screens playing) at **505.8 MiB** in a 1920 x 1080 window, within the acceptable 512 MiB by 6.2 MiB
(`docs/roadmap.md`, "Acceptable performance"; `docs/i5/step-3.md`, "Measurement"): the render targets took 88.5 MiB
there against 48.2 at 1280 x 720, and `--screenshot` added a second full set (161.0 MiB of targets in all). BACKLOG's
"Measurement" put the remedy in the targets. This step takes 28.3 MiB out of the 1080p targets and the screenshot's
whole second set; the shading, the Balanced profile's features and the Low profile are unchanged.

## What was trimmed

- **The plan of the targets** (`rv_render_uniforms`, `view_target_plan.hpp`, GPU-free, unit-tested): each image of an
  output's `gl::ViewTargets`, its format, its size (full, half, a bloom level), the first and last pass of a frame it
  lives through (`gl::ViewPass`: depth pre-pass, occlusion, blur, lit pass, bloom, post, anti-aliasing, outline, debug
  lines, present) and the image whose storage it takes over; `gl::viewTargetsBytes()` the storage they allocate.
  `ViewTargets::create()` allocates from it. An image that takes another's storage is a texture view of it
  (`Framebuffer::createAlias`: `glTextureView` of a color attachment, in the same view class, and optionally of a depth
  texture; it allocates and records nothing). The unit tests check that two images on one storage never live through
  the same pass, but for the in-place hand-over below, and that they share size and texel size.
- **The output has no depth buffer of its own** (7.9 MiB at 1080p). Only the debug lines tested against it, after a
  copy of the scene's depth; they now draw into `ViewTargets::outputWithSceneDepth()`, the output's color over the HDR
  target's depth/stencil (views, no storage), and write no depth. The outline's edge, the overlays and the screenshot's
  UI (RmlUi draws through its own layers) never used it. Pixel-identical.
- **The emitted light in `GL_R11F_G11F_B10F`** (7.9 MiB) and **the tonemapped image in its storage** (7.9 MiB). The
  emission (`SceneOutput::Emission`) only feeds the bloom's first downsample and the post pass's `bloomSource`; the
  post pass reads it with `texelFetch` at the very texel it writes, so the tonemapped image (`GL_SRGB8_ALPHA8`, the
  same 32-bit view class) is written over it in place, after `glTextureBarrier()` (the one feedback the GL 4.5 spec
  defines: a single read and write of a texel by its own fragment). `post/post.frag` says so beside its read.
- **The bloom in `GL_R11F_G11F_B10F`** (2.6 MiB) and **its first level in the half-resolution occlusion's storage**
  (2.0 MiB): the occlusion's blur is over before the bloom starts (`GL_RG16F`, the same size and view class).
  `Framebuffer::readLinearPixels()` reads the R11F_G11F_B10F levels for the bloom self-check.
- **A screenshot at the window's size draws through the window's targets** (72.5 MiB at 1080p; `SceneGraphics`
  `captureScreenshot()`): the window's view draws them anew right after. A screenshot of another size draws through
  targets made for it alone, the window's let go of first and made again by the window's view: one set is held at any
  time, so a 1080p screenshot from a 720p window peaks at the 1080p set (and F12 no longer keeps a second set for the
  rest of the run).

Not done: post-processing intermediates at reduced resolution (they change the look's sharpness), the shadow map
(16 MiB, its precision), a 16-bit radiance (`GL_R11F_G11F_B10F` there would band the image).

## Before and after

The renderer's own accounting (`gpuMemory.kinds.targets`, its peak), `m0_testroom`, Release, driver 621.01; the
targets do not depend on the scene, only on the window and the screenshot: the window's view targets and the key
light's 16 MiB shadow map. The `building` fixture's console room (`room:snes` of `gpu.secondary_tv`'s data, both screens
in view; its clips stayed in the test's temporary folder) holds the same targets after the step: 143.6 MiB of GPU
memory in all at 720p, 168.2 at 1080p with or without a 1080p screenshot.

| Run | Before | After | Saved |
|---|---|---|---|
| 1280 x 720 window | 48.2 MiB | 35.6 MiB | 12.6 |
| 1920 x 1080 window | 88.5 MiB | 60.2 MiB | 28.3 |
| 1280 x 720 window, `--screenshot` at 1920 x 1080 | 120.7 MiB | 60.2 MiB | 60.5 |
| 1920 x 1080 window, `--screenshot` at 1920 x 1080 | 161.0 MiB | 60.2 MiB | 100.8 |

The 1080p view targets by image (MiB; the shadow map apart):

| Image | Before | After |
|---|---|---|
| Radiance (`GL_RGBA16F`) | 15.8 | 15.8 |
| Emission | 15.8 (`GL_RGBA16F`) | 7.9 (`GL_R11F_G11F_B10F`) |
| Scene depth/stencil | 7.9 | 7.9 |
| Occlusion, half (`GL_RG16F`) and full (`GL_R8`) | 4.0 | 4.0 (the first bloom level in the half's storage) |
| Bloom, 6 levels | 5.3 (`GL_RGBA16F`) | 0.7 (`GL_R11F_G11F_B10F`, levels 2 to 6) |
| Tonemapped | 7.9 | 0 (in the emission's storage) |
| Output color | 7.9 | 7.9 |
| Output depth/stencil | 7.9 | 0 |
| All | 72.5 | 44.2 |

The real busiest floor at 1080p with both screens playing should therefore hold about **477.5 MiB** (505.8 - 28.3),
and 477.5 with a screenshot too (578.3 before); Gate M3 measures it again on a quiet machine.

## The pixels

The output's depth, the aliases and the screenshot's targets change no pixel (the test room's 1080p screenshot 0 of
2,073,600 pixels apart with the formats unchanged; a screenshot through the window's targets equals one through its
own, the new GPU case). The two formats do, slightly, and FXAA turns a few of those 1/255 steps into an edge decided
the other way: on the test room's 1080p screenshot the emission alone changes 5,933 pixels (5,651 by 1/255, 24 by
more than 6/255, at most 44/255, on the neon sign's and the lamp's edges), emission and bloom together 32,174 (29,286
by 1/255, 73 by more than 6/255, at most 38/255): 0.004% beyond the goldens' tolerance, which allows 0.5%. Without
them the step would save 7.9 MiB at 1080p, short of the 30 it set out to regain.

**Goldens.** G01 is not rewritten: it passes within its tolerance (73 pixels beyond 6/255, 0.004%). G10 checks that
no pixel outside its ten added items differs from G02's golden by more than 6/255, which needs G02 rendered the same
way, so the room goldens G02 to G20 are rewritten on purpose (`RV_UPDATE_GOLDEN=1`, Debug, driver 621.01; their PNGs
equal in Release), each compared with `retroverse-screenshot-diff` against its old PNG first: G02 68 pixels beyond
6/255 (largest 42/255), G02s 151 (51), G06 59 (34), G07 36 (22), G08a 165 (68), G08b 221 (39), G10 70 (42), G11 250
(44), G12 83 (48), G13 120 (27), G14 217 (52), G15 1,417 (60, the arcade's lit cabinets), G16 95 (48), G17 114 (41),
G18 206 (60), G19 124 (56), G20 462 (37): all on the edges of emitters and their glow, none above 0.07%. Their
metadata name the commit before the rewrite; plans, scenes and digests are untouched.

## Proofs

- Unit (`tests/unit/render/test_view_target_plan.cpp`, 4 cases): every image once, in order, within its passes; the
  bloom and occlusion sizes; every image taking storage lives through it after its owner (or in place in its last
  pass), at its size and view class; the view classes; the bytes at 720p and 1080p (19.6 and 44.2 MiB) and at an odd
  size.
- GPU (`tests/smoke/test_measurements.cpp`, `gpu.measurements/the render targets at 1080p ...`): a 1080p screenshot
  from a 64 x 64 window peaks at exactly the 1080p set and the shadow map, whatever the window's pixels; a 1080p window
  holds at most 64 MiB of targets and the same with a screenshot of its size; that screenshot equals the other byte for
  byte.

**Results.** Debug (`./scripts/build.ps1`, no warning; `./scripts/test.ps1`): 2215 of 2215 unit tests passed. Release
(`./scripts/build.ps1 -Preset msvc-x64-release`; `./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2383 tests (2215
unit, 168 GPU), all passed but one skipped by design (the overlay's pick, as always); one GPU case
(`gpu.world elevator: the ride's fade ...`) timed out under the shared machine's load and passed on its rerun. G01
passes within its tolerance (73 pixels beyond 6/255) and G02 to G20 compare 0/255 in Release and in Debug
(`-Preset msvc-x64-debug-gpu -Filter 'gpu\.golden/'`, where three cases' `firstFrame` check failed under load and
passed on their rerun). Plans and plan goldens byte-identical (nothing of the world changed).

## The room preview's time

Secondary goal: I4b step 6 measured the real `room:amstradcpc`'s regeneration preview at 1.6 s quiet and 3.2 s under
load (limit 2 s). Without the real library, a hidden bench (`tests/unit/app/test_world_regenerate.cpp`, `app/bench: a
large room's preview, its read and its regeneration`, `[.bench]`, by hand in Release) imports `esde/snes-stress` (2,000
titles, 300 of them twice, generated at test time), plans it (2,000 placed in one room) and previews `room:snes` three
times: 75, 59 and 59 ms in Release on a quieter machine (read 14 ms, regeneration 45 to 60). With temporary timers
(not committed), on the loaded machine (the Debug goldens running beside it):

| Part of the preview | ms |
|---|---|
| Read: the world's files | 2 to 9 |
| Read: the plan parsed (`readPlan`) | 10 to 15 |
| `generateScope` (the room's chain generated) | 4 to 16 (the first: its data read) |
| `buildingInputDigest`: the JSON document | 15 to 16 |
| `buildingInputDigest`: canonical text (322 KB) and SHA-256 | 4 to 7 |
| `validatePlan` | 5 |
| `sortedPlanDigest` (the new plan's): document, text (220 KB), SHA-256 | 9 to 16 |
| `planDifference`, the pins, the change report, the rest | about 5 |
| The whole preview | 55 to 165 (82 to 112 typical) |

No single hot spot: the two digests (their JSON documents built with nlohmann::json, then written canonically and
hashed) are about a third, the plan's parse a sixth, the generation and the validation the rest. A large single room
previews in about a tenth of a second; the real library's 1.6 s comes from the world's size around the room (44 rooms
and 19,985 placements parsed, every standing room's data read, the input digest over every system's selection, the
whole plan's digest and validation), which these parts scale with. A faster digest would write the canonical text
straight from the plan instead of building a document first: an algorithmic change of the digests' code, outputs
byte-identical, left to the backlog rather than taken here. Nothing was changed; Gate M3 measures the real preview
again on a quiet machine.
