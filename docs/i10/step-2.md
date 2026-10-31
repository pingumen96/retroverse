# I10 step 2: the session and the tracked head

Roadmap row: I10 step 2. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §3, §4, §8, §10. Built on
step 1's eyes ([step-1.md](step-1.md)).

## What was built

- **The module `xr`** (`src/xr`):
  - **`rv_xr_model`** is GPU-free, always built, and `rv_forbid_link` keeps it away from the OpenXR loader.
    - `pose.hpp`: poses and eye views. The tracking space is placed on the walker's feet and yaw (`toWorld`, with
      LOCAL's floor offset). It also gives the head between the eyes, the field holding both, and yaw/pitch/roll in
      the camera's conventions.
    - `runtime.hpp`: the `xr::Runtime` interface, the session's lifecycle and frame loop without OpenXR's types.
    - `session.hpp`: `xr::Session`.
      - It begins the session when the runtime is ready and ends it when it stops.
      - It counts the session ended when it exits or is lost, or when a call fails.
      - A frame is `beginFrame()` (wait, begin, locate, acquire), then `endFrame()`. Lost tracking submits no layer.
      - Every frame's timing is kept, and the spike's missed-refresh measure is reused (`frame_stats.hpp`).
    - `fake_runtime.hpp`: `xr::FakeRuntime`.
      - Two eyes 64 mm apart with a Quest 3S's fields (step 1's), 1024 x 1024 each, at the scripted head pose.
      - 72 Hz with no wait.
      - Its session goes ready at once, then focused once begun.
      - A scripted loss, or untracked frames, are available for tests.
    - `swapchain_format.hpp` is the spike's.
  - **`rv_xr`**:
    - The fake runtime's images as GL textures (`gl_images.hpp`).
    - With `RV_ENABLE_XR`, the system's OpenXR runtime (`openxr_runtime.hpp`): the spike's session on the
      `rv::xr::Runtime` interface.
      - XR_KHR_opengl_enable on the app's context, with the optional XR_FB_display_refresh_rate and XR_EXT_local_floor.
      - Space: LOCAL_FLOOR, else STAGE, else LOCAL.
      - One `GL_SRGB8_ALPHA8` swapchain per eye, at the recommendation times the render scale.
      - The ADR's `GraphicsBinding` interface waits for the D3D11 bridge (step 6): OpenGL is the only binding so far.
- **The host** (`AppHost`, `XrSessionDriver`):
  - `--vr` starts the session on the window's context before the logic's `onInit`.
  - While the session runs, the runtime paces the loop:
    - `beginFrame()` before `onUpdate`, which gives `AppContext::xrFrame()`;
    - `endFrame()` after `onRender`;
    - the mirror's swap without vsync;
    - a minimized window does not stop the frames.
  - Without a runtime or a headset the run stays on the desktop and says why. A session that ends sends the run back
    to the desktop, with vsync as asked.
  - The `FramePacer` of the plan became this branch of the loop: one place, with the desktop's path unchanged.
- **The tracked head** (`RetroverseApp::followHead`):
  - Each located frame computes, from the walker's feet and yaw:
    - the eyes' cameras (step 1's eye mode);
    - the head's camera, which takes the view, so culling, covers, labels, the audio listener and picking follow the
      head.
  - The walker keeps walking and turning with the pad and the keyboard (step 3 makes the movement head-relative).
- **The eyes' rendering** (`SceneGraphics::renderFrame` with `XrEyeTargets`):
  - Both eyes render into their own targets, culled against one frustum enclosing them, the second reading the first's
    shadow map, with no vignette (ADR-0017 §4).
  - The ride's fade is a post setting, so it applies to each eye.
  - Each eye is copied verbatim into its swapchain image (`gl::TextureTarget`, a framebuffer over an image someone
    else owns).
  - The window shows the left eye, filled without distortion (`Framebuffer::blitToDefaultFilled`).
  - The screenshots show both eyes side by side at their size.
- **Options:** `--vr`, `--xr-runtime openxr|fake`, `--xr-runtime-json <manifest>`, `--xr-render-scale <0.25..2>`,
  `--xr-fake-lose-after <frames>`. Input scripts gain `headPose` steps.
- **The run report's `xr` section:**
  - the runtime, the headset, the space, the eyes' size and format;
  - the frames waited for and submitted, the refresh rate and the missed refreshes, the CPU time per frame;
  - the session's events and the end reason;
  - the frame the run went back to the desktop;
  - the head in the world on the first 600 frames.

## Proof

- **Unit tests** (`rv_tests`):
  - the poses' composition and angles;
  - the session over the fake runtime: begin, frames at its pace, eyes in place, lost tracking, an exit request, a lost
    session, a failed call;
  - the frame statistics (the spike's cases);
  - the options;
  - the head poses of input scripts;
  - the report's `xr` section, field for field.
- **GPU smoke tests** (`tests/smoke/test_xr.cpp`, `[xr]`):
  - **The test room with `tests/input_scripts/xr_look.json`:** 40 frames all submitted with both eyes at 72 Hz, none
    missed. The head's yaw is the walker's plus the script's, and its height comes from tracking. The screenshot holds
    both eyes, 2048 x 1024. No GL debug message.
  - **A session lost after 10 frames:** the run goes on on the desktop from frame 10 to its 20th.
  - **A missing runtime manifest:** the run stays on the desktop. The report says `XR_ERROR_RUNTIME_UNAVAILABLE`, and
    the system's runtime is never asked.
  - **Golden G21** (`assets/style_tests/screenshots/g21_xr_testroom_golden.png`): both eyes in the test room from the
    scene camera's place. It was recorded on driver 621.01 and compared on that driver only, like the others.
- **By eye:** G21 and the look's last frame (the head tilted 10 degrees and crouched to 1.2 m). Shadows and SSAO show
  in both eyes, and each eye sees farther outwards.
- **The desktop:** unchanged. The goldens' 35 images, rendered in Release, are identical to step 1's (largest
  difference 0/255), and `--vr` absent leaves every path as it was.
- **The suites:** the unit tests (2208) in Debug; the golden and `[xr]` GPU tests (22) in Release, where G21 matches
  the Debug-recorded golden exactly.

```powershell
build/msvc-x64-debug/bin/retroverse.exe --vr --xr-runtime fake --scene m0_testroom --hidden --exit-after-frames `
  --frames 40 --fixed-dt 0.016667 --input-script tests/input_scripts/xr_look.json `
  --screenshot build/screenshots/i10/xr_look.png --run-report build/screenshots/i10/xr_look.json --data-dir build/rvdata
```

## By hand, in the headset

Meta Horizon Link running and the headset in Link (cable or Air Link):

```powershell
build/msvc-x64-release/bin/retroverse.exe --vr --data-dir build/rvdata
```

- **Where to look:** in the lobby, then the SNES room (`--start-room room:snes` on a data directory whose library has
  it).
- **What should hold:**
  - the room at its true scale, the floor under the feet;
  - turning and leaning move the view with no lag or swim;
  - both eyes agree (no double image);
  - the window mirrors the left eye.
- **The pad:** walks and turns as on the desktop.
- **Leaving:** quitting Link sends the run back to the desktop.

## Left for later steps

- The movement relative to the head, snap turn, teleport, the head's offset through the collision: step 3.
- Controllers and their ray (picking still casts from the head): step 4. The UI in the headset: step 5, until then the
  product UI shows in the mirror only.
- A launched game: the host idles and the headset shows the runtime's frame until the game ends; step 6 keeps the
  session rendering.
- `xr.json` (render scale, refresh rate, comfort): with step 3's comfort settings. Until then the command line carries
  the render scale, and the runtime's default refresh rate stands.
- The runtime's performance counters (XR_META_performance_metrics, the spike's): step 7.
- An RGBA8 swapchain (a runtime without sRGB) would get display-encoded bytes. Meta's and SteamVR's offer sRGB; the
  D3D11 bridge (step 6) handles the rest.
