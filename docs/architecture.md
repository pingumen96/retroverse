# Architecture

This document describes the code that exists today. The target architecture is in `specification.md` §15.

## Modules

| Module | CMake target | Responsibility | May depend on |
|---|---|---|---|
| core | `rv_core` | Errors (`rv::Expected`), logging, paths, frame clock, AABB/ray math, sRGB color helpers, file I/O, file change polling (`FileWatchSet`), JSON, command line parsing | glm, spdlog, nlohmann-json, tl-expected |
| geometry | `rv_geometry` | CPU meshes (`MeshData`, 48-byte `Vertex` with glTF tangent space), sockets, procedural mesh builder and procedural prefabs with their collision boxes, uv scale of a surface, also as drawn with a transform (`uvMetersPerUnit`) | core |
| input | `rv_input` | Device-neutral codes, logical actions, action maps (with per-action user overrides), per-frame `ActionState`, input scripts for automated runs (`InputScript`, `InputScriptPlayer`) | core |
| assets | `rv_assets` | Image decoding/encoding, glTF loading, prefab manifests, material presets, shader source loading, procedural test patterns (sized to their surface: `patternSizeForAspect`) | core, geometry, fastgltf, stb |
| scene | `rv_scene` | Camera, first-person controller, lights, render items and batching plan, scene description files, scene geometry assembly (`buildSceneGeometry`: meshes, resolved materials, scene textures and colliders per object), collision (`CollisionWorld`, `buildCollisionWorld`) | core, geometry, input, assets |
| platform | `rv_platform` | SDL3 window, OpenGL context, event pump, dialogs; input: SDL events -> `input::RawInputFrame` (`SdlInputCollector`, code tables in `sdl_input_codes.hpp`), gamepad hot-plug (`GamepadManager`), mouse capture (`MouseCapture`) | core, input, SDL3 |
| render | `rv_render` | OpenGL 4.5 device and state cache, buffers, vertex arrays, shader programs and the hot-reloading `ShaderLibrary`, `GpuMesh`, uniform buffers, framebuffers, textures (`Texture2D`, `Sampler`, `TextureDefaults`, `TextureCache`), GPU materials (`MaterialSet`) | core, geometry, assets, scene, glad |
| devui | `rv_devui` | Dear ImGui developer overlay (developer builds only) | platform, render, imgui |
| app | `rv_app` | Composition root: application host, main loop, input pipeline, FreeRoam walking, scene loading and the lit scene view, screenshots, the sRGB self-check | all of the above |

Three GPU-free sub-targets keep testable policy out of the GPU-side libraries:

| Sub-target | Part of | Content |
|---|---|---|
| `rv_app_cli` | app | Command line options and validation, scene selection (`resolveScenePath`), screenshot file names, exit-code policy, self-check verdicts (`evaluateSrgbCheck`), the scene draw plan of the lit scene view (`planSceneDraws`: materials and picture instances, drawing order) |
| `rv_render_debug_filter` | render | Which OpenGL debug messages are logged, dropped or deduplicated |
| `rv_render_uniforms` | render | std140 uniform block mirrors and binding tables (`uniform_blocks.hpp`), scene lights and environment packed into `LightsUniforms` (`packLights`), materials into `MaterialUniforms` (`makeMaterialUniforms`), the CPU mirror of the display transform (`tonemap.hpp`: exposure + ACES fitted, for the clear color) |

`core`, `geometry`, `input`, `assets`, `scene` and the three sub-targets are **GPU-free**: they never reach SDL3,
glad or ImGui (enforced at configure time by `cmake/LayerRules.cmake`), so they are unit-tested without a window or
a context. `render` receives the OpenGL function loader as a function pointer and never includes SDL. The SDL side of
`platform` (code tables, collector, gamepads through SDL virtual joysticks, mouse capture on a hidden window, the
fullscreen shortcut) is tested by its own executable, `rv_platform_tests` (label `unit`, SDL but no OpenGL), so
`rv_tests` stays SDL-free. Because `rv_render_uniforms` is GPU-free, tests check its std140 mirrors, material flags,
texture units and tonemapping constants against the GLSL sources.

Cross-module contracts written by independent work streams (prefab parameters and sockets, material names, scene
files, manifests, shader sources and uniform blocks) are pinned by `tests/unit/integration`, which builds the real
geometry, resolves the real materials and preprocesses the real shaders.

## Rendering path (M0)

- **Scene:** `--scene <name|file>` (default `m0_testroom`) is loaded with `scene::loadSceneDescription`;
  `scene::buildSceneGeometry` builds every prefab or primitive, resolves each part's material (library name, inline
  material, magenta fallback; placeholder cubes for unavailable geometry) and attaches the texture the scene binds to
  the part's role. The application's `SceneView` uploads one `GpuMesh` per part and draws every part instance
  separately (batching and instancing arrive with step 8) in the order of the GPU-free plan `app::planSceneDraws`
  (`rv/app/scene_draw_plan.hpp`, unit-tested): opaque materials, then alpha-masked ones, each sorted by shader,
  material and part, then alpha-blended ones in scene order (blending on, depth writes off, no depth sorting yet).
- **Materials:** one GPU material per material of the scene, and one more per object, role and pattern size with a
  scene texture, which becomes that instance's base color texture (cover patterns on cartridge labels and posters,
  color bars on the CRT screen; patterns are generated at the aspect ratio of their surface as drawn,
  `geometry::uvMetersPerUnit` with the instance transform, so a poster scaled unevenly is not stretched). Scene
  textures are single pictures, sampled clamped to their edges; textures named by materials repeat. `MaterialSet`
  keeps every `MaterialUniforms` block in one uniform buffer (offsets aligned to `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`)
  and binds per material the block range, five textures (base color and emissive sRGB; metallic-roughness, normal and
  occlusion linear; neutral `TextureDefaults` in empty slots) and sampler objects (trilinear, 8x anisotropy clamped to
  the device, the wrap mode of the texture). Material flags tell the shaders which slots hold a texture and the alpha
  mode; fragment stages sample every texture before an alpha-mask discard (derivatives are undefined after it).
  Shader families (`scene::ShaderKind`): `Pbr` (`pbr/pbr.vert` + `pbr/pbr.frag`) and `Unlit` (`pbr/pbr.vert` +
  `unlit/unlit_emissive.frag`: base color * texture * emissive strength).
- **Textures:** `TextureCache` uploads each source once (asset files, `pattern:<name>:<title>:<seed>@WxH` patterns,
  decoded model images; keys include the color space and the wrap mode) as immutable `GL_RGBA8` / `GL_SRGB8_ALPHA8`
  textures with full mip chains. A missing or broken source is logged once and drawn as the checker (style §21).
- **Lighting:** `gl::packLights` turns the scene lights (contract of `scene::Light`) and environment into
  `LightsUniforms`: up to 8 point/spot lights (inverse square with a smooth window reaching 0 at the range, smoothstep
  spot cones) and one directional light, the hemispherical sky/ground ambient and the exposure. `pbr.frag`: GGX NDF,
  height-correlated Smith visibility, Schlick Fresnel, Lambert diffuse, metallic workflow, normal maps in the glTF
  tangent space (ADR-0002), occlusion, emissive; the ambient lights the diffuse lobe from the hemisphere around the
  normal and the specular lobe from the hemisphere around the reflection (analytic environment BRDF), so that metals
  reflect the room. Lit and unlit surfaces share exposure + ACES fitted (Hill) tonemapping (`common/tonemap.glsl`);
  the environment's clear color is background radiance and goes through the same transform on the CPU
  (`gl::tonemap`), so the void beyond a doorway sits on the scale of the lit room.
- **Frames:** every view is rendered into an offscreen `Framebuffer` (`GL_SRGB8_ALPHA8` color + depth24/stencil8)
  with `GL_FRAMEBUFFER_SRGB` enabled, so shaders output linear values and the target stores sRGB. The window frame
  uses a target of the window's size, copied to the default framebuffer with a raw blit (`GL_FRAMEBUFFER_SRGB`
  disabled), so the window shows exactly the stored bytes whatever its own encoding. `--self-check srgb` proves the
  encoding: an unlit linear 0.5 grey quad (the unlit program built with `RV_UNLIT_LINEAR_OUTPUT`, so without
  tonemapping) drawn into such a target must read back as 188 +- 2.
- **Screenshots:** `--screenshot` (last frame of a frame-limited run) and F12 render the same view into a second
  target at `--screenshot-size` (default 1920x1080, independent of the window), read it back through an `RGBA8`
  texture view (raw sRGB bytes on every driver), flip the rows to top-first (ADR-0002) and write an RGB PNG (the
  window is opaque, so the alpha channel is dropped).
- **Uniforms:** binding points are listed once in `rv/render/uniform_blocks.hpp` (0 frame, 1 lights, 2 material,
  3+ reserved) with the std140 mirrors `FrameUniforms` (view, projection, viewProjection, cameraPosition, time:
  `common/common.glsl`), `LightsUniforms` (`common/lights.glsl`) and `MaterialUniforms` (`common/material.glsl`).
  The per-draw `uModel` is a plain uniform.
- **Shaders:** `gl::ShaderLibrary` assembles GLSL from `<asset root>/shaders` (`#include`, defines, `#line`) and
  maps compiler messages to `<file>:<line>`. F5 rebuilds every program; developer builds (`RV_DEV_ASSET_ROOT`) also
  poll the files of every program, includes included, every 500 ms (after a failed assembly: the files it read and
  the one it could not read, so creating a missing include rebuilds the program). A failed rebuild keeps the
  previous program.

## Input path (M0)

One pass per frame on the main thread, in `RetroverseApp` (`src/app/src/app_input.*`, `free_roam.*`):

- **Events:** the host consumes the Alt+Enter press (`platform::isFullscreenToggle`, borderless fullscreen), so it
  never reaches the bindings (Enter is Confirm). In the logic, `platform::MouseCapture` sees every other SDL event
  first: a click into the focused window captures the mouse (SDL relative mode) and is consumed, focus loss releases
  it. Interactive runs also capture at startup once the window has focus; frame-limited runs never do, so unattended
  runs neither take the user's mouse nor pick up its motion. Every other event goes to `platform::SdlInputCollector`:
  keys by scancode (layout-independent WASD), mouse buttons, relative motion while captured, wheel, gamepad buttons
  and axes (SDL ranges to [-1, 1], +Y up, triggers [0, 1]) of the pads `platform::GamepadManager` opened on hot-plug;
  the most recently used pad is reported. Focus loss releases everything held. Joystick events never arrive in the
  background (`SdlPlatform` hint).
- **Actions:** the collector's `RawInputFrame` is resolved by `input::InputSystem` with the bindings of
  `<asset root>/config/default_bindings.json` (`ActionMap::defaults()` with a warning when unusable), overridden per
  action by `<user data>/config/bindings.json` (`ActionMap::applyOverrides`). `--input-script` then replaces the
  actions of the frames it covers (`input::InputScriptPlayer`, edges kept consistent).
- **Use:** Back releases the captured mouse or quits, ReloadShaders and Screenshot are handled by the app, ToggleDevUI
  is reserved for the developer overlay. `FreeRoam` drives `scene::FirstPersonController` with the actions and the
  frame delta (`--fixed-dt` replaces it for reproducible runs; the sum of the deltas is also the shader time): the
  walker starts at the scene camera (feet = camera position - eye height) and collides with
  `scene::buildCollisionWorld()` of the scene geometry, built from the colliders each prefab declares (for
  `room_shell` one box per wall part around the doorway and a stop in the doorway's outer face, so the walker can
  step into the opening but not leave the room; the bounds for furniture; none for `rug`; objects with
  `"collides": false` have none). Objects turned about +Y collide with their exact footprints, tilted ones with their
  world boxes. Frame-limited runs log the final feet position, yaw and pitch, the simulated time, the distance walked
  and the nearest collider with its clearance.

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
  Logs go to `<user data>/logs/retroverse.log`, F12 screenshots to `<user data>/screenshots`.
