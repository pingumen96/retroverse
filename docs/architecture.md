# Architecture

This document describes the code that exists today. The target architecture is in `specification.md` §15.

## Modules

| Module | CMake target | Responsibility | May depend on |
|---|---|---|---|
| core | `rv_core` | Errors (`rv::Expected`), logging (`FailureStreak`: which failures of an operation retried every frame to log; the in-memory recent lines with a message count and `parseLogLine` for the overlay), paths, frame clock, AABB/ray math, sRGB color helpers, file I/O, file change polling (`FileWatchSet`), JSON, command line parsing | glm, spdlog, nlohmann-json, tl-expected |
| geometry | `rv_geometry` | CPU meshes (`MeshData`, 48-byte `Vertex` with glTF tangent space), sockets, procedural mesh builder and procedural prefabs with their collision boxes, uv scale of a surface, also as drawn with a transform (`uvMetersPerUnit`) | core |
| input | `rv_input` | Device-neutral codes, logical actions, action maps (with per-action user overrides), per-frame `ActionState`, input scripts for automated runs (`InputScript`, `InputScriptPlayer`) | core |
| assets | `rv_assets` | Image decoding/encoding, glTF loading, prefab manifests, material presets, shader source loading, procedural test patterns (sized to their surface: `patternSizeForAspect`) | core, geometry, fastgltf, stb |
| scene | `rv_scene` | Camera, first-person controller and viewpoints (`frontViewpoint`: where a walker stands to look at an object, framed in a part of the view, `ViewArea`), lights, render items, draw passes and the batching plan (`BatchPlanner`: frustum culling, opaque/mask/blend passes, blend back to front), scene description files, scene content assembly (`buildSceneGeometry`: meshes shared by objects of the same shape, resolved materials, scene textures, glTF models through a `ModelLoader`, sockets, bounds and colliders per object), debug lines (`DebugLines`, `addSceneBoundsLines`, `addSceneSocketLines`), collision (`CollisionWorld`, `buildCollisionWorld`) | core, geometry, input, assets |
| platform | `rv_platform` | SDL3 window, OpenGL context, event pump, dialogs; input: SDL events -> `input::RawInputFrame` (`SdlInputCollector`, code tables in `sdl_input_codes.hpp`), gamepad hot-plug (`GamepadManager`), mouse capture (`MouseCapture`) | core, input, SDL3 |
| render | `rv_render` | OpenGL 4.5 device and state cache, buffers, vertex arrays, shader programs and the hot-reloading `ShaderLibrary` (with the build state and last error of every program), `GpuMesh`, uniform buffers, framebuffers (`Framebuffer`, window blit, `clearDefault`), textures (`Texture2D`, `Sampler`, `TextureDefaults`, `TextureCache`), registries (`MeshRegistry`, `MaterialRegistry` with live factor edits; GPU bytes of both), glTF models on the GPU (`GpuModel`), the batched instanced `SceneRenderer` (`InstanceBuffer`, `GpuTimer`, wireframe), the render targets of an output (`ViewTargets`: HDR scene target, bloom chain, sRGB output), bloom and the post pass (`PostProcessor`), debug lines (`DebugDraw`) | core, geometry, assets, scene, glad |
| devui | `rv_devui` | Dear ImGui developer overlay (developer builds only, `RV_DEV_UI`): `DevUi` (ImGui context, SDL3 and OpenGL3 backends, event routing, popups closed by Back, dock space and its see-through scene area, rendering into the window or a screenshot) and its panels (`panels.hpp`: Stats, Camera, Lights, Materials, Rendering, Log, Scene) | core, input, scene, platform, render, imgui |
| app | `rv_app` | Composition root: application host, main loop, input pipeline, FreeRoam walking, scene loading and instantiation (`SceneInstance`), the model viewer (`--model`), screenshots, the sRGB self-check, the developer overlay's panels wired to the app state (developer builds) | all of the above |

Four GPU-free sub-targets keep testable policy out of the GPU-side libraries:

| Sub-target | Part of | Content |
|---|---|---|
| `rv_app_cli` | app | Command line options and validation, scene selection (`resolveScenePath`), screenshot file names, exit-code policy, the Back action (`backAction` over a `BackState`: close the overlay's topmost popup, else release the mouse, else close the overlay, else quit), when a run needs the overlay and which frames draw it into screenshots (`overlayRequired`, `overlayCaptureSize`), the debug line kinds shown (`DebugLineSelection`, `composeDebugLines`), self-check verdicts (`evaluateSrgbCheck`), the scene content plan (`planSceneContent`: distinct meshes, materials and picture instances, render items, model instances), the model viewer scene (`makeModelViewerScene`: framing, three-point light rig, background; `modelViewerGrid`; the model summary `describeModel`), the frame statistics log (`FrameStatsLog`) |
| `rv_devui_model` | devui | Built in every configuration: the frame time history of the Stats panel (`FrameTimeHistory`), the overlay's input routing (`routeOverlayEvent`) and mouse capture hand-over (`CaptureHandover`), the Log panel's copy of the log (`LogView`), where the overlay lands in a screenshot (`fitOverlayCapture`) |
| `rv_render_debug_filter` | render | Which OpenGL debug messages are logged, dropped or deduplicated |
| `rv_render_uniforms` | render | std140 uniform block mirrors and binding tables (`uniform_blocks.hpp`), scene lights and ambient packed into `LightsUniforms` (`packLights`), materials into `MaterialUniforms` (`makeMaterialUniforms`), the post-processing of a view (`post_settings.hpp`: `PostSettings` from the scene environment, `postSettingsFor`; its `PostUniforms` block, `makePostUniforms`; the bloom strength per level, `bloomMix`, the radius as the upsampling scatter, `bloomScatter`, the threshold and knee, the bloom level sizes, `bloomLevelSizes`), the CPU mirror of the display transform (`tonemap.hpp`: exposure + ACES fitted, which tests use to predict displayed colors), the renderer's frame statistics (`FrameStats`, header only), live material factor edits (`MaterialFactors`, `withFactors`) |

`core`, `geometry`, `input`, `assets`, `scene` and the four sub-targets are **GPU-free**: they never reach SDL3,
glad or ImGui (enforced at configure time by `cmake/LayerRules.cmake`), so they are unit-tested without a window or
a context. `render` receives the OpenGL function loader as a function pointer and never includes SDL. The SDL side of
`platform` (code tables, collector, gamepads through SDL virtual joysticks, mouse capture on a hidden window, the
fullscreen shortcut) is tested by its own executable, `rv_platform_tests` (label `unit`, SDL but no OpenGL), so
`rv_tests` stays SDL-free. Because `rv_render_uniforms` is GPU-free, tests check its std140 mirrors, material flags,
texture units, tonemapping constants and bloom filter weights against the GLSL sources.

Cross-module contracts written by independent work streams (prefab parameters and sockets, material names, scene
files, manifests, shader sources and uniform blocks) are pinned by `tests/unit/integration`, which builds the real
geometry, resolves the real materials and preprocesses the real shaders.

## Rendering path (M0)

- **Scene:** `--scene <name|file>` (default `m0_testroom`) is loaded with `scene::loadSceneDescription`; `--model
  <file>` instead builds the model viewer scene (`app::makeModelViewerScene`: the model at the origin, a camera framing
  its bounds from the front right and above, a three-point light rig sized to it, a neutral environment whose dark
  mid-grey background, shown as about sRGB 61, sets off black plastic as well as ivory).
  `app::SceneInstance` (`rv/app/scene_instance.hpp`) turns the description into drawable content, the reusable step
  between a scene and the renderer: `scene::buildSceneGeometry` builds every prefab or primitive once per kind and
  parameters (the 24 cartridges of the test room share one shell mesh and one label mesh, `ScenePart::meshKey`),
  loads every glTF model once per path (`scene::ModelLoader`: `assets::loadGltf` under the asset root, or the viewer's
  file), resolves each part's material (library name, inline material, magenta fallback; magenta placeholder cubes
  for unavailable geometry: builder errors, models that cannot be loaded or have nothing to draw) and attaches the
  texture the scene binds to the part's role. The GPU-free plan `app::planSceneContent` (unit-tested) lists the
  distinct meshes, the materials and one render item per part and object instance; `SceneInstance` registers them in
  the `gl::MeshRegistry` and `gl::MaterialRegistry` (deduplicated by key), uploads every distinct model as a
  `gl::GpuModel` (meshes `<path>#mesh<i>`, materials `<path>#material<i>`, images through the `TextureCache` as
  `<path>#image<i>`, the draw items with their node transforms, the sockets) and adds one render item per model
  primitive and model instance. It also packs the lights, builds the collision world and the debug lines (bounds of
  every object instance, axes of every socket), and keeps the environment (background radiance, ambient,
  post-processing) that the overlay edits and every view reads.
- **Renderer:** `gl::SceneRenderer` draws a `gl::RenderView` (camera, render items, lights, background radiance,
  post-processing settings, time, optional debug lines) through the `gl::ViewTargets` of an output: the scene pass
  into its HDR target, then the post-processing into its sRGB output (below). Per view it writes `FrameUniforms` and,
  when they changed, `LightsUniforms`;
  plans the items with `scene::BatchPlanner` (frustum culling against the registered mesh bounds; opaque batches, then
  alpha-masked ones, each sorted by shader, material and mesh; then alpha-blended items back to front by the distance
  to their world bounds, consecutive ones with the same key merged; the renderer keeps the planner and the plan, so
  planning allocates nothing once they have grown to the largest view); copies every instance matrix of the plan into
  the `gl::InstanceBuffer` (persistently mapped, coherent, three regions used in turn, each guarded by a fence placed
  after the draws that read it, grown and re-attached to the meshes when a view needs more) and issues one
  `glDrawElementsInstancedBaseInstance` per batch, binding the program and the material (uniform block range,
  textures, samplers, blend, depth writes, culling) only when they change. A view whose instances cannot be written is
  skipped; `core::FailureStreak` logs the failure once per cause and the recovery with the number of skipped views.
  Mesh shaders read the instance matrix at vertex attribute locations 4..7 (divisor 1). `gl::GpuTimer`s (rings of
  `GL_TIME_ELAPSED` queries read frames later, never stalling) time the scene pass and the post-processing of each
  view. `gl::FrameStats` (items, culled items, batches, draw calls, instances, triangles, shader and material switches,
  debug lines, GPU time of the scene pass and of the post-processing) are kept per view;
  `app::FrameStatsLog` logs them with the frame times once per second in developer builds (`RV_DEV_UI`) and for the
  whole run at the end of frame-limited runs (frames that render no view, a minimized window, count their time only).
  The test room draws its 71 parts in 47 batches (23 meshes uploaded); `instancing_stress` draws 1001 items in 4;
  `instancing_growth` turned around draws about 2000 instances, more than the 1024 per region the instance buffer
  starts with, so it grows mid-run.
- **Debug lines:** `--debug-draw` adds `gl::DebugDraw` over the post-processed output, depth-tested against the scene
  (its depth is copied from the HDR target first): `scene::DebugLines` (a world grid with its +Y marker drawn first, so
  the floor axes show where they overlap it; every object instance's bounds as an oriented box; the axes of every
  socket: shelf slots, the CRT screen surface, model anchors; the overlay's Rendering panel toggles each kind,
  `app::composeDebugLines`, and the program and buffer are created when lines are first shown) uploaded when the
  selection changes and drawn as `GL_LINES` with `debug/debug_lines.{vert,frag}` (linear colors encoded by the sRGB
  output, neither lit, tonemapped nor bloomed; depth-tested without depth writes, each vertex pulled towards the eye by
  0.2% of its distance so that lines lying on surfaces do not flicker). Socket axes are markers: a second draw shows
  their parts hidden by surfaces, dimmed (the complementary depth test), so a socket under a surface, such as the CRT's
  screen plane under its bulged glass, still shows its origin.
- **Materials:** one GPU material per material of the scene, and one more per object, role and pattern size with a
  scene texture, which becomes that instance's base color texture (cover patterns on cartridge labels and posters,
  color bars on the CRT screen; patterns are generated at the aspect ratio of their surface as drawn,
  `geometry::uvMetersPerUnit` with the instance transform, so a poster scaled unevenly is not stretched). Scene
  textures are single pictures, sampled clamped to their edges; textures named by materials and model images repeat.
  `MaterialRegistry` keeps every `MaterialUniforms` block in one uniform buffer (offsets aligned to
  `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`, dynamic storage: the overlay's live edits, `setFactors`, rewrite one block with
  `gl::withFactors`) and binds per material the block range, five textures (base color and emissive sRGB;
  metallic-roughness, normal and occlusion linear; neutral `TextureDefaults` in empty slots) and sampler objects
  (trilinear, 8x anisotropy clamped to the device, the wrap mode of the texture). Material flags tell the shaders which
  slots hold a texture and the alpha mode; fragment stages sample every texture before an alpha-mask discard
  (derivatives are undefined after it).
  Shader families (`scene::ShaderKind`): `Pbr` (`pbr/pbr.vert` + `pbr/pbr.frag`) and `Unlit` (`pbr/pbr.vert` +
  `unlit/unlit_emissive.frag`: base color * texture * emissive strength). Both write linear scene radiance, neither
  exposed nor tonemapped: the post pass does that once for the whole view.
- **Textures:** `TextureCache` uploads each source once (asset files, `pattern:<name>:<title>:<seed>@WxH` patterns,
  decoded model images; keys include the color space and the wrap mode) as immutable `GL_RGBA8` / `GL_SRGB8_ALPHA8`
  textures with full mip chains. A missing or broken source is logged once and drawn as the checker (style §21).
- **Lighting:** `gl::packLights` turns the scene lights (contract of `scene::Light`) and environment into
  `LightsUniforms`: up to 8 point/spot lights (inverse square with a smooth window reaching 0 at the range, smoothstep
  spot cones) and one directional light, and the hemispherical sky/ground ambient. `pbr.frag`: GGX NDF,
  height-correlated Smith visibility, Schlick Fresnel, Lambert diffuse, metallic workflow, normal maps in the glTF
  tangent space (ADR-0002), occlusion, emissive; the ambient lights the diffuse lobe from the hemisphere around the
  normal and the specular lobe from the hemisphere around the reflection (analytic environment BRDF), so that metals
  reflect the room. The environment's clear color is background radiance, cleared into the HDR target and
  post-processed like the surfaces, so the void beyond a doorway sits on the scale of the lit room.
- **Post-processing (M0 step 10, style §4.3, spec §14.2):** `gl::PostProcessor` takes the HDR scene of a
  `gl::ViewTargets` to its sRGB output with the `gl::PostSettings` of the view (`gl::postSettingsFor` the scene's
  environment: exposure, `bloomStrength`, `bloomRadius`, `vignette`, and one 8-bit step of dither noise), in passes
  that draw one fullscreen triangle each without a vertex buffer (`post/fullscreen.vert`, an empty vertex array, the
  sources on texture units 5 and 6, `gl::PostTextureUnit`, with a bilinear sampler clamped to the edges; parameters in
  `PostUniforms` at binding 3, `common/post.glsl`). Bloom (Jimenez 2014), when its strength is above 0: the light of
  the scene above a soft threshold (exposed brightness 1, knee 0.5, `bloomExcess`: lit walls and wood stay out,
  screens, lamp shades, neon and highlights bloom) is downsampled into 6 levels of half, a quarter, ... of the view
  with the 13-tap filter, the first level with the Karis average against fireflies (`RV_BLOOM_KARIS`); then, from
  the smallest level up, the 3 x 3 tent upsampling of each level is mixed into the next larger one by the
  fixed-function blend (constant alpha `bloomScatter(bloomRadius)`). Filters and blend keep the energy of the image.
  The post pass (`post/post.frag`) replaces the share `bloomMix(bloomStrength)` of the light above the threshold by
  its bloom (each of the 6 levels takes the strength, so 0.06 moves about 31% of that light: the usual strengths of
  0.04 to 0.08 of chains that sum their levels give the same soft glow, but energy is moved, never added), applies the
  vignette, the exposure and the ACES fitted curve (`common/tonemap.glsl`, mirrored on the CPU by `gl::tonemap`), and
  dithers the result with interleaved gradient noise in its sRGB encoding against banding. A program that never built
  falls back: without the bloom programs the view is composited without bloom, without the post program the HDR scene
  is copied to the output as it is (clamped, neither exposed nor tonemapped). At 1920x1080 the post-processing takes
  about 0.2 ms on the reference GPU.
- **Frames:** every view is rendered through a `gl::ViewTargets`, the targets of one output, all of its size: the HDR
  scene target (`GL_RGBA16F` color + depth24/stencil8), the 6 bloom levels (`GL_RGBA16F`, `gl::bloomLevelSizes`) and
  the output (`GL_SRGB8_ALPHA8` color + depth24/stencil8), which the post pass writes with `GL_FRAMEBUFFER_SRGB`
  enabled, so shaders output linear values and the target stores sRGB. The window and the screenshots each keep their
  own `ViewTargets`, reallocated only when their size changes (a complete new set first, so a failure keeps the old
  one). The window frame's output is copied to the default framebuffer with a raw blit (`GL_FRAMEBUFFER_SRGB`
  disabled), so the window shows exactly the stored bytes whatever its own encoding. The developer overlay is drawn
  last, into the default framebuffer, with `GL_FRAMEBUFFER_SRGB` disabled (Dear ImGui's colors are sRGB already); the
  backend changes GL state behind the `RenderStateCache`, which is invalidated afterwards. A frame whose window targets
  cannot be allocated clears the window to black (`Framebuffer::clearDefault`) and still draws the overlay over it, so
  its log shows why. `RenderView::wireframe` draws the scene pass with `glPolygonMode(GL_LINE)`. `--self-check srgb`
  proves the output path: an unlit linear 0.5 grey quad drawn with the scene's unlit program into the HDR target of
  8x8 `ViewTargets`, taken by the `PostProcessor` to the output with bloom, vignette and dithering off, exposure 1 and
  the post program built with `RV_POST_LINEAR_OUTPUT` (a clamp instead of the ACES curve), must read back as
  188 +- 2.
- **Screenshots:** `--screenshot` (last frame of a frame-limited run) and F12 render the same view through a second
  `ViewTargets` at `--screenshot-size` (default 1920x1080, independent of the window; the bloom levels scale with it,
  so the glow covers the same share of the image), read the output back through an `RGBA8` texture view
  (raw sRGB bytes on every driver), flip the rows to top-first (ADR-0002) and write an RGB PNG (the window is opaque, so
  the alpha channel is dropped). They show the scene alone; with `--screenshot-include-ui` the open overlay is drawn
  over them too, from the frame's draw data scaled uniformly from the window to fit the screenshot and centered
  (`devui::fitOverlayCapture`: another aspect than the window's leaves bands of scene beside it instead of stretching
  the panels; that frame's glyphs are rasterized for the screenshot's density, so the text stays sharp).
- **Uniforms:** binding points are listed once in `rv/render/uniform_blocks.hpp` (0 frame, 1 lights, 2 material,
  3 post, 4+ reserved) with the std140 mirrors `FrameUniforms` (view, projection, viewProjection, cameraPosition, time:
  `common/common.glsl`), `LightsUniforms` (`common/lights.glsl`), `MaterialUniforms` (`common/material.glsl`) and
  `PostUniforms` (exposure, bloom mix, threshold and knee, vignette, dither amplitude: `common/post.glsl`).
  Model matrices are per-instance vertex attributes (locations 4..7), not uniforms.
- **Shaders:** `gl::ShaderLibrary` assembles GLSL from `<asset root>/shaders` (`#include`, defines, `#line`) and
  maps compiler messages to `<file>:<line>`. F5 rebuilds every program; developer builds (`RV_DEV_ASSET_ROOT`) also
  poll the files of every program, includes included, every 500 ms (after a failed assembly: the files it read and
  the one it could not read, so creating a missing include rebuilds the program). A failed rebuild keeps the
  previous program.

## Input path (M0)

One pass per frame on the main thread, in `RetroverseApp` (`src/app/src/app_input.*`, `free_roam.*`):

- **Events:** the host consumes the Alt+Enter press (`platform::isFullscreenToggle`, borderless fullscreen), so it never
  reaches the bindings (Enter is Confirm). In developer builds the overlay sees every other event first
  (`devui::routeOverlayEvent`) and keeps what it uses: mouse events over a panel or while a widget is dragged, key
  presses while a text field or a dragged value has the keyboard (so Esc there never quits), text input; releases always
  reach the game, so nothing stays held, and while first-person look holds the mouse every mouse event is the game's.
  Then `platform::MouseCapture` sees the event: a click into the focused window captures the mouse (SDL relative mode)
  and is consumed, focus loss releases it. Interactive runs also capture at startup once the window has focus;
  frame-limited runs never do, so unattended runs neither take the user's mouse nor pick up its motion. Every other
  event goes to `platform::SdlInputCollector`: keys by scancode (layout-independent WASD), mouse buttons, relative
  motion while captured, wheel, gamepad buttons and axes (SDL ranges to [-1, 1], +Y up, triggers [0, 1]) of the pads
  `platform::GamepadManager` opened on hot-plug; the most recently used pad is reported. Focus loss releases everything
  held. Joystick events never arrive in the background (`SdlPlatform` hint).
- **Actions:** the collector's `RawInputFrame` is resolved by `input::InputSystem` with the bindings of
  `<asset root>/config/default_bindings.json` (`ActionMap::defaults()` with a warning when unusable), overridden per
  action by `<user data>/config/bindings.json` (`ActionMap::applyOverrides`). `--input-script` then replaces the
  actions of the frames it covers (`input::InputScriptPlayer`, edges kept consistent).
- **Use:** Back leaves one level (`app::backAction`: ignored while the overlay has the keyboard, else closes the
  overlay's topmost popup, else releases the captured mouse, else closes the overlay, else quits), ToggleDevUI shows
  or hides the overlay (`devui::CaptureHandover`: opening it frees a captured mouse, closing it captures the mouse
  again), ReloadShaders and Screenshot are handled by the app.
  `FreeRoam` drives `scene::FirstPersonController` with the actions and the frame delta (`--fixed-dt` replaces it for
  reproducible runs; the sum of the deltas is also the shader time): the walker starts at the scene camera (feet =
  camera position - eye height) and collides with the collision world of the `SceneInstance`
  (`scene::buildCollisionWorld()` of the scene content), built from the colliders each object declares (for `room_shell`
  one box per wall part around the doorway and a stop in the doorway's outer face, so the walker can step into the
  opening but not leave the room; the bounds for furniture; the `ColliderProxy` nodes of glTF models, else their bounds;
  none for `rug`; objects with `"collides": false` have none). Objects turned about +Y collide with their exact
  footprints, tilted ones with their world boxes. Frame-limited runs log the final feet position, yaw and pitch, the
  simulated time, the distance walked and the nearest collider with its clearance.

## Developer overlay (M0 step 9)

`devui::DevUi` (`rv/devui/dev_ui.hpp`, developer builds) owns the Dear ImGui context (1.92, docking branch) with the
SDL3 platform backend (gamepads left to the game; the SDL hints the backend sets for itself are restored, so the game's
input behaves the same) and the OpenGL3 renderer backend (GLSL `#version 450`, its own GL loader). The app creates it
after the scene and adds the panels (`rv/devui/panels.hpp`), thin views that hold references to the state they show and
edit in place:

- Stats: `devui::FrameTimeHistory` (the last 240 wall-clock frame times, recorded every frame), the renderer's
  `gl::FrameStats` (the GPU times of the scene pass and of the post-processing first), mesh, material and texture counts
  and bytes from the registries and the `TextureCache`.
- Camera: the walker (`scene::FirstPersonController` of `FreeRoam`: feet, yaw and pitch teleport it; speeds), the view
  camera's lens (FOV), the input settings (mouse sensitivity, invert Y), the app's collision flag (noclip), and a reset
  to the scene camera.
- Lights: `SceneInstance::sceneLights()` and `environment()` (background radiance and ambient), edited in place and
  repacked (`applyLighting`: `gl::packLights`); colors are edited as sRGB and stored linear.
- Materials: `gl::MaterialRegistry` (`setFactors`: base color, metallic, roughness, emissive strength; never saved; a
  revert goes back to `GpuMaterial::source`).
- Rendering: the app's debug line selection and wireframe flag, the window's vsync, the post-processing of
  `SceneInstance::environment()` (exposure, bloom strength and radius, vignette; read by every view, so edits show at
  once), `ShaderLibrary` (reload, build state and last error of every program).
- Log: `devui::LogView`, a ring of the recent log lines (time, level, message) refreshed only when
  `core::loggedMessageCount()` changes.
- Scene: the objects of the scene description (id, then kind and source on a line of their own, so a narrow column
  shows them whole); "go to" places the walker with `scene::frontViewpoint` in front of the object's world bounds
  (all its instances), facing its front (+Z) and framing it in the area the panels leave see-through
  (`DevUi::sceneViewArea`, the dock space's central node, as a `scene::ViewArea` in normalized device coordinates),
  or at the center of an object larger than 4 m along both x and z (a room); long furniture is looked at.

Each frame the app records the frame time and, while the overlay is visible, builds its frame in `onUpdate` before
the walker moves, so edits apply to the frame being rendered; `onRender` draws it after the window blit. While hidden
no ImGui frame is built. The default layout (Stats and Camera on the left, Lights/Materials/Scene tabs and Rendering on
the right, Log under the scene, a see-through central node) is built with the dock builder when no saved layout
exists; its side columns are at least 18 and 22 font sizes wide (at most 30% and 40% of the width they split), so
the panels stay readable as fonts and sizes follow `SDL_GetWindowDisplayScale`. Interactive runs load and save the
layout in `<user data>/config/imgui.ini`; frame-limited runs always use the default layout and save nothing, so their
screenshots never depend on earlier runs. Panels reserve exactly their widest label beside the widgets
(`widgets::pushLabelWidth`) and put vectors of three values on a line of their own (`widgets::labelAbove`).
Keyboard navigation stays off (it would take the game's keys), so Dear ImGui never closes a popup on Escape: Back
does (`DevUi::closePopup`, one level at a time), and hiding the overlay closes every popup. A frame-limited run that
asks for the overlay (`app::overlayRequired`) fails with exit code 3 when it cannot start. Release builds compile
neither `rv_devui` nor ImGui.

## Threading (M0)

Single main thread: event pump, input, simulation, OpenGL calls, presentation. Asset loading is synchronous at
startup. Worker threads (catalog, media decoding, video) arrive in later iterations and must hand GPU work to the
main thread through a command queue (spec §15.3). `rv::core::assertMainThread` guards main-thread-only code.

## Data conventions

See `docs/decisions/ADR-0002-coordinates-units-uv.md`: meters, +Y up, right-handed, +Z front, CCW front faces,
glTF UV convention, linear-space shading with sRGB textures and framebuffer. Tangent space as in glTF:
`bitangent = cross(normal, tangent.xyz) * tangent.w` points towards decreasing v (the top of the image), the "+Y up"
of normal maps; `geometry::computeTangents` and the procedural builders generate tangents that way (checked with
synthetic normal maps in M0 step 6: `tests/unit/geometry/test_mesh_data.cpp`).

## Runtime directories

- Asset root: `--asset-root`, `RV_ASSET_ROOT`, `<source>/assets` (developer builds), `<exe>/assets`.
- User data: `--data-dir`, `<exe>/RetroverseData` with `--portable`, `%LOCALAPPDATA%\Retroverse` otherwise.
  Logs go to `<user data>/logs/retroverse.log`, F12 screenshots to `<user data>/screenshots`, the developer
  overlay's layout to `<user data>/config/imgui.ini`.
