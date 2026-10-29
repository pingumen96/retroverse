# ADR-0017: VR through OpenXR: a headset on the PC, any runtime, the game on the room's CRT

- Status: accepted (the user's approval of the I10 plan on 2026-10-29, the choices below made with the user in
  planning); §2's graphics binding and §9's limits to be confirmed by the step 0 spike
  ([spikes/i10-xr.md](../spikes/i10-xr.md))
- Date: 2026-10-29
- Amends: specification §2.2 (VR is no longer out of scope), §22 M6 and FR-037 (VR moves from M6 to iteration I10);
  ADR-0001 (a new dependency, `openxr-loader`, at the same baseline); ADR-0006 (a launch from VR keeps the session and
  shows the game, §7); ADR-0012 (Focus and Inspect without camera motion in VR, §5); ADR-0011's budgets (a VR row,
  §9); the roadmap (iteration I10) and the BACKLOG as recorded under "Amendments"
- Relates to: ADR-0002 (units and axes, which OpenXR shares), ADR-0005 (the product UI, navigated with the pad),
  ADR-0016 (the screens' textures, which the captured game joins); style §12.4's comfort options and `reduceMotion`

## Context

The user wants to use Retroverse with a Meta Quest 3S connected to the Windows PC, and wants the work to serve other
headsets later. Until now VR was out of v1 (specification §2.2, M6 "VR in un'architettura dedicata", FR-037 P2).

What the code offers and lacks (the exploration of 2026-10-29):

- **Rendering:** `gl::RenderView` and `gl::ViewTargets` are already one view into its own offscreen targets, blitted
  to the window at the end. The camera, though, is yaw and pitch with a symmetric projection and takes no external
  matrix. The SSAO reconstructs positions from a symmetric projection. The shadow map is drawn again for every view,
  and the instance buffer has three regions.
- **The frame loop:** `SDL_GL_SwapWindow` paces it, and while a game runs the host renders nothing.
- **Input:** it already goes through device-free actions (`input::Action`, `ActionMap`, input scripts), with no pose
  or ray. Picking has one entry point, `pickRay()`.
- **The UI:** RmlUi and ImGui draw in screen space, but both can already draw into any framebuffer (`renderTo`).
- **The platform:** it exposes the HWND only; the GL handles (HDC, HGLRC) that a Win32 OpenGL binding needs come from
  `wglGetCurrentDC` / `wglGetCurrentContext`.
- **The library:** the pinned vcpkg baseline has `openxr-loader` 1.1.63 (Apache-2.0 AND MIT AND BSL-1.0). The machine's
  active runtime is Meta Horizon Link's.

Choices made with the user: an iteration now, beside I4b and I5, in its own worktree. Quest Link and Air Link (Meta's
runtime) and SteamVR. Seated or standing play with stick, snap turn and teleport; room-scale movement adds to it but
is not required. Touch controllers with a ray, the Xbox pad as today. The launched game shown on the room's CRT and
played with the physical pad. `--vr` or a setting, back to the desktop when no headset is found, the left eye mirrored
in the window. 72 Hz guaranteed, 90 Hz where it fits, on the reference machine.

## Decision

### 1. OpenXR only

Retroverse talks to headsets through OpenXR 1.0 (with the extensions it checks for), through the Khronos loader:

- no vendor SDK (Meta's OVR, OpenVR);
- no vendor extension that the VR mode cannot do without;
- the runtime chosen by the system (Meta Horizon Link, SteamVR, Virtual Desktop's, WMR's), or for a measurement by
  `XR_RUNTIME_JSON`.

Optional extensions, each checked for and recorded:
- `XR_FB_display_refresh_rate` (72/90/120 Hz);
- `XR_EXT_local_floor`;
- later passthrough or hand tracking (BACKLOG).

### 2. The graphics binding

- **First binding:** `XR_KHR_opengl_enable` on the app's GL 4.5 context, with swapchains of `GL_SRGB8_ALPHA8` (else
  `GL_RGBA8`), one per eye.
- **Second binding (step 6):** a D3D11 bridge, swapchains of D3D11 textures that GL renders into through
  `WGL_NV_DX_interop2`. It serves the runtimes that refuse OpenGL, and reuses the interop the game's capture needs
  (§7).
- **Shape in the code:** an `xr::GraphicsBinding` interface with these two implementations.
- **Open:** the spike confirms the first binding on Meta's runtime and on SteamVR. If either refuses OpenGL, the
  bridge moves to step 2.

### 3. The module `xr`

A new module between `render` and `app`:

- **`rv_xr_model`, GPU-free:** poses and fields, the tracking space composed with the world, off-axis projections,
  locomotion and comfort, the XR bindings' JSON, ray against quad. It is marked with
  `rv_forbid_link(... EXTRA ${RV_OPENXR_LIBRARIES})`.
- **`rv_xr`:** the loader and `rv_render`. It holds:
  - `xr::Runtime` with `OpenXrRuntime`, and `FakeRuntime` for tests (two asymmetric eyes, scripted poses, swapchains
    that are offscreen GL textures);
  - the session's state machine;
  - the swapchains and layers.
- **The host:** `AppHost` gets a `FramePacer`.
  - `DesktopPacer` keeps the swap as today.
  - `XrPacer` paces frames with `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` at the predicted display time.
- **Losing the session** (LOSS_PENDING, instance loss, the headset unplugged) logs and returns to the desktop mode.

### 4. Rendering per eye

- **The camera:** `scene::Camera` gains an eye mode: an orientation quaternion and four field angles.
- **The desktop's output stays identical:** no golden moves.
- **Changes in `rv_render`:**
  - the SSAO reconstructs positions with an off-axis projection;
  - the shadow map is drawn once per frame, not per view;
  - the instance buffer gets enough regions for two eyes, the mirror and a screenshot;
  - culling uses a frustum enclosing both eyes;
  - covers get both eyes' frusta (`CoverView::of` already takes several).
- **In VR:** the vignette is off, and a fade (the ride's) applies per eye.

### 5. Movement and interaction

- **Anchoring:** the tracking space is anchored to the walker (feet and yaw).
- **The head:**
  - its height comes from tracking (LOCAL_FLOOR, else STAGE, else LOCAL with a seated height);
  - its offset in the room goes through `CollisionWorld::moveCapsule`;
  - a head inside a wall fades and is pushed back.
- **Locomotion:**
  - stick movement relative to the head (an option: to the hand);
  - snap turn of 30° (an option: smooth);
  - teleport with an arc;
  - recentering.
- **Comfort:** `reduceMotion` is forced for every transition, plus an optional comfort vignette.
- **The pad:** keeps the current actions, with the gaze taken from the head.
- **Touch controllers:**
  - their action sets map onto the existing `input::Action`s;
  - two new channels: the hands' poses and the pointing ray;
  - the ray feeds `pickRay()`;
  - suggested bindings ship for the Touch, Index, Vive, WMR, Pico and simple controllers
    (`assets/config/xr_bindings.json`).
- **Focus and Inspect (amends ADR-0012):** in VR, Focus never moves the walker. The item is outlined and its card shows
  beside it. Inspect holds the item in the hand, turned by the wrist.
- **Controller models:** generic, original low-poly models, no vendor asset or mark.

### 6. The UI in VR

- **Panels:** the RmlUi documents (and the devui in developer builds) draw through `renderTo` into their own
  swapchains, shown as quad layers placed in the world:
  - the quick catalog and the directory in front of the user;
  - the card beside its item;
  - labels and the elevator's prompt as small quads.
- **Input:** a ray hitting a quad becomes RmlUi mouse input at its UV; the pad navigates as today.
- **Fallback:** without quad layers, the same texture is drawn on a quad in the scene.

### 7. The game on the room's CRT (amends ADR-0006)

- **The launch:** a launch from VR keeps the session running and the scene rendered. There is no hand-over and no idle
  mode.
- **The picture:** Windows.Graphics.Capture captures the emulator's window into a D3D11 texture (in `platform_win`).
  The interop (in `render`) makes that texture a GL texture, which overrides the hero CRT's picture the way a clip's
  `VideoTexture` does (ADR-0016). An option shows it on a large quad in front of the user instead.
- **During the game:**
  - the scene uses an "in game" profile: lower render scale, the room's decoders stopped;
  - the emulator is played with the physical pad, which it reads without focus (RetroArch configured not to pause when
    inactive);
  - emulators in exclusive fullscreen cannot be captured: the documented setting is borderless.
- **The return:** the game's end, or a Touch command that stops showing it (the game keeps running: Retroverse never
  ends an emulator).
- **Not embedded emulation:** the emulator stays an external process and Retroverse only shows its window, so
  specification §2.2's "riproduzione dei videogiochi all'interno del televisore virtuale" stays out of scope.

### 8. Configuration and activation

- **Activation:** `--vr` or `display.mode` in `<user data>/config/xr.json` (shipped defaults in
  `assets/config/xr.json`).
- **Settings:** comfort, render scale, quality profile, refresh rate, seated height.
- **Without a headset:** with no runtime or no headset the app starts on the desktop and logs why.
- **The window:** shows the left eye, a blit with no extra rendering.
- **The run report:** gains an `xr` section (runtime, system, refresh rate, missed refreshes, poses).

### 9. Acceptable performance (added to the roadmap's table)

| Measure | Acceptable |
|---|---|
| Frame time in VR, a populated room, both eyes at the runtime's recommended size, render scale 1.0 | p50 at most 13.9 ms, p99 at most 20 ms; at most 1% of the refreshes missed (72 Hz); 90 Hz informative |
| Frame time in VR with a game captured on the CRT | the same limits with the "in game" profile |

The VR quality profile (reduced SSAO, fewer bloom levels, FXAA, a dynamic render scale within bounds) is tuned in
step 7. Correctness and the focus rules are never relaxed.

### 10. Tests

- **Unit tests:** cover the model.
- **GPU smoke tests:** run `retroverse.exe --vr --xr-runtime fake` hidden, with input scripts that carry head and hand
  poses. They check the report's `xr` section and one stereo golden of the test room.
- **The real headset:** the `[.xr]` cases are hidden and run only by hand, like `[.desktop]`.
- **The capture:** a `desktop` test, because the captured window must be visible.
- **Launches:** any launch uses `RV_LAUNCH_ALLOWLIST`; RetroArch runs only with an isolated configuration under
  `build/`.

## Amendments (to the documents)

- **Specification:**
  - §2.2: "VR" leaves the out-of-scope list, pointing here;
  - §22 M6: "VR in un'architettura dedicata" becomes "VR: ADR-0017, iterazione I10";
  - FR-037: "Specifica separata: ADR-0017 (I10)".
- **Roadmap:**
  - iteration I10 with its steps and gate;
  - VR leaves "Deferred beyond v1";
  - §9's rows join "Acceptable performance".
- **BACKLOG:** VR leaves "Deferred beyond v1"; parked under I10's follow-ups:
  - hand tracking;
  - passthrough and mixed reality;
  - the Touch controllers as a virtual pad for the emulator (a third-party kernel driver);
  - a Linux runtime.
- **ADR-0001:** the vcpkg feature `xr` (`openxr-loader`) joins the manifest; the baseline is unchanged.

## Consequences

- **The desktop:** keeps its look and its limits. Every change to it in I10 is proven by the existing goldens and
  gates, unchanged.
- **New hardware:** costs a JSON binding, not code, as long as its runtime speaks OpenXR.
- **I10 runs beside I4b and I5:**
  - its logic lives in new files (`xr_session_driver`, `xr_interaction`), with small hooks in `retroverse_app.cpp`;
  - the branch is rebased often.

### Open risks

- **OpenGL on the runtimes:** OpenGL on Meta's runtime and on SteamVR (the spike); the bridge is the answer if one
  refuses.
- **One GPU for three jobs:** the emulator, the capture and two eyes share the GPU. At 72 Hz in game a stronger "in
  game" profile may be needed (the room alone, or a cinema).
- **Capture limits:** exclusive fullscreen emulators cannot be captured. The capture's yellow border is removed with
  `IsBorderRequired = false` (Windows 11).
- **Air Link:** adds latency the app does not control; the limits are measured on the cable.
