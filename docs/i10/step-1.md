# I10 step 1: views per eye, on the desktop

Roadmap row: I10 step 1. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §4. Desktop only: no
headset and no OpenXR yet (step 2).

## What was built

- **The camera's eye mode** (`rv_scene`, `camera.hpp`):
  - `Camera::setEye(position, quaternion, EyeFov)` puts the camera in eye mode: an orientation with roll and the four
    angles of an eye's field (OpenXR's `XrFovf`). `setPose()` returns it to the yaw/pitch mode.
  - In eye mode, `projection()` is `offAxisProjection()`, which a symmetric field turns into glm's perspective. It
    ignores the aspect ratio and the lens's field but keeps the lens's planes.
  - `forward()`, `right()` and `up()` turn with the quaternion. `yaw()` and `pitch()` describe where the eye looks, and
    `rayFromNdc()` goes through the off-axis field.
  - `orientation()` gives the quaternion in either mode.
  - The yaw/pitch mode computes exactly what it did before.
- **A frustum enclosing several views** (`Frustum::enclosing`, `Frustum::corners`): the first view's planes, each moved
  out until the other views' corners are inside. It is conservative: it may keep a box no eye sees, but never drops one
  that any eye sees.
- **The SSAO of an asymmetric field** (`common/ssao.glsl`): `viewPosition()` adds the projection's shift
  (`uProjection[2][0]`, `[2][1]`) before undoing the scale. The shift is 0 on the desktop, so the desktop's output is
  the same. The samples were already projected with the whole matrix.
- **The shadow map once per frame** (`SceneRenderer::beginFrame()`):
  - A view whose map would match one already drawn this frame reads that map. A match means the same light projection,
    depth bias, items and static groups.
  - It also skips planning its casters and writing their instances.
  - `SceneGraphics` starts a frame before the screenshot, so the window's view now reads the screenshot's map, which is
    the same map.
  - Renderers that never call `beginFrame()` (the asset preview, the tests) draw a map for every view, as before.
- **One frustum for both eyes' culling** (`RenderView::cullFrustum`): when set, the items and the static groups are
  culled against it instead of the camera's frustum, so both eyes draw the same plan.
- **Eight instance-buffer regions** instead of three: up to four views per frame (two eyes and their screenshot) over
  two frames in flight never wait on a fence.
- **The stereo preview** (`--stereo-preview`; `rv_scene`'s `stereo.hpp`, `SceneGraphics`):
  - The window and every screenshot show both eyes side by side, on black.
  - Each eye is an off-axis view of a Quest 3S's field as the step 0 spike measured it (52 degrees outwards, 45
    inwards, 48 up, 50 down), 64 mm from the other.
  - Each eye takes half the image's width, its height in the field's proportions.
  - Both eyes are culled against the frustum enclosing them, and the right eye reads the left eye's shadow map. The
    eyes are copied into the image verbatim (`Framebuffer::copyColorTo(destination, offset)`).
  - Covers, picking and the audio still follow the desktop's camera.

## Proof

- **Unit tests** (`rv_tests`):
  - the off-axis projection's edges land on NDC's, and a symmetric field gives glm's perspective;
  - an eye's basis, view and rays, an invalid eye rejected, a return to the yaw/pitch mode that matches a camera which
    never was an eye;
  - the yaw/pitch quaternion matches the camera's basis;
  - the enclosing frustum holds both eyes' corners, keeps a box only one eye sees, and culls what is behind both;
  - the preview's eyes and layout;
  - the option.
- **GPU smoke tests** (Release): 157 passed.
- **The desktop's image unchanged:**
  - The goldens were rendered with NVIDIA's driver 617.42; this machine now has 621.01, so the golden tests skip
    their comparison.
  - Instead, the 35 images the golden tests render (G01, G02 to G20 and their first frames) were rendered on this
    driver from step 0's code and from step 1's, and compared with `retroverse-screenshot-diff`: identical, largest
    difference 0/255.
- **By eye:** the test room with `--stereo-preview` (below). Each eye sees farther outwards than inwards; the left eye's
  image is shifted right of the right eye's; contact shadows (SSAO) and the lamps' shadows show in both eyes with no
  seam at the edges of the asymmetric field; no GL debug message.

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 5 --hidden --stereo-preview `
  --screenshot build/screenshots/i10/stereo_testroom.png --data-dir build/rvdata
```

## Left for later steps

- Covers' residency from both eyes (`CoverView::of` already takes several frusta), picking from a ray and the audio
  listener from the head: step 2, with the tracked head.
- Planning once for both eyes: they plan the same items against the same frustum, but each plans on its own. Sharing
  the plan and the instance upload is step 7's, if the frame time asks for it.
