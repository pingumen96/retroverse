# Architecture

This document describes the code that exists today. The target architecture is in `specification.md` §15.

## Modules

| Module | CMake target | Responsibility | May depend on |
|---|---|---|---|
| core | `rv_core` | Errors (`rv::Expected`), logging, paths, frame clock, AABB/ray math, sRGB color helpers, file I/O, file change polling (`FileWatchSet`), JSON, command line parsing | glm, spdlog, nlohmann-json, tl-expected |
| geometry | `rv_geometry` | CPU meshes (`MeshData`, 48-byte `Vertex`), sockets, procedural mesh builder and procedural prefabs | core |
| input | `rv_input` | Device-neutral codes, logical actions, action maps, per-frame `ActionState` | core |
| assets | `rv_assets` | Image decoding/encoding, glTF loading, prefab manifests, material presets, shader source loading | core, geometry, fastgltf, stb |
| scene | `rv_scene` | Camera, first-person controller, lights, render items and batching plan, scene description files, scene geometry assembly (`buildSceneGeometry`: meshes and resolved materials per object), collision | core, geometry, input, assets |
| platform | `rv_platform` | SDL3 window, OpenGL context, event pump, gamepads, SDL -> input translation | core, input, SDL3 |
| render | `rv_render` | OpenGL 4.5 device and state cache, buffers, vertex arrays, shader programs and the hot-reloading `ShaderLibrary`, `GpuMesh`, uniform buffers, framebuffers | core, geometry, assets, scene, glad |
| devui | `rv_devui` | Dear ImGui developer overlay (developer builds only) | platform, render, imgui |
| app | `rv_app` | Composition root: application host, main loop, scene loading and the unlit scene view, screenshots | all of the above |

Two GPU-free sub-targets keep testable policy out of the GPU-side libraries:

| Sub-target | Part of | Content |
|---|---|---|
| `rv_app_cli` | app | Command line options and validation, scene selection (`resolveScenePath`), screenshot file names, exit-code policy |
| `rv_render_debug_filter` | render | Which OpenGL debug messages are logged, dropped or deduplicated |

`core`, `geometry`, `input`, `assets`, `scene` and the two sub-targets are **GPU-free**: they never reach SDL3,
glad or ImGui (enforced at configure time by `cmake/LayerRules.cmake`), so they are unit-tested without a window or
a context. `render` receives the OpenGL function loader as a function pointer and never includes SDL. The header
`rv/render/uniform_blocks.hpp` (uniform binding table and std140 mirrors) is GPU-free too, so tests check it against
the GLSL sources.

Cross-module contracts written by independent work streams (prefab parameters and sockets, material names, scene
files, manifests, shader sources and uniform blocks) are pinned by `tests/unit/integration`, which builds the real
geometry, resolves the real materials and preprocesses the real shaders.

## Rendering path (M0)

- **Scene:** `--scene <name|file>` (default `m0_testroom`) is loaded with `scene::loadSceneDescription`;
  `scene::buildSceneGeometry` builds every prefab or primitive and resolves each part's material (library name,
  inline material, magenta fallback; placeholder cubes for unavailable geometry). The app uploads one `GpuMesh` per
  part and, for now, draws every part instance separately with the unlit shader in its flat base color; batching,
  textures and lighting replace this path in later steps.
- **Frames:** every view is rendered into an offscreen `Framebuffer` (`GL_SRGB8_ALPHA8` color + depth24/stencil8)
  with `GL_FRAMEBUFFER_SRGB` enabled, so shaders output linear values and the target stores sRGB. The window frame
  uses a target of the window's size, copied to the default framebuffer with a raw blit (`GL_FRAMEBUFFER_SRGB`
  disabled), so the window shows exactly the stored bytes whatever its own encoding.
- **Screenshots:** `--screenshot` (last frame of a frame-limited run) and F12 render the same view into a second
  target at `--screenshot-size` (default 1920x1080, independent of the window), read it back through an `RGBA8`
  texture view (raw sRGB bytes on every driver), flip the rows to top-first (ADR-0002) and write an RGB PNG (the
  window is opaque, so the alpha channel is dropped).
- **Uniforms:** binding points are listed once in `rv/render/uniform_blocks.hpp` (0 frame, 1 lights, 2 material,
  3+ reserved); `FrameUniforms` (view, projection, viewProjection, cameraPosition, time, exposure) mirrors the std140
  block in `assets/shaders/common/common.glsl`. Per-draw values (`uModel`, `uBaseColor`) are plain uniforms.
- **Shaders:** `gl::ShaderLibrary` assembles GLSL from `<asset root>/shaders` (`#include`, defines, `#line`) and
  maps compiler messages to `<file>:<line>`. F5 rebuilds every program; developer builds (`RV_DEV_ASSET_ROOT`) also
  poll the files of every program, includes included, every 500 ms (after a failed assembly: the files it read and
  the one it could not read, so creating a missing include rebuilds the program). A failed rebuild keeps the
  previous program.

## Threading (M0)

Single main thread: event pump, input, simulation, OpenGL calls, presentation. Asset loading is synchronous at
startup. Worker threads (catalog, media decoding, video) arrive in later iterations and must hand GPU work to the
main thread through a command queue (spec §15.3). `rv::core::assertMainThread` guards main-thread-only code.

## Data conventions

See `docs/decisions/ADR-0002-coordinates-units-uv.md`: meters, +Y up, right-handed, +Z front, CCW front faces,
glTF UV convention, linear-space shading with sRGB textures and framebuffer.

## Runtime directories

- Asset root: `--asset-root`, `RV_ASSET_ROOT`, `<source>/assets` (developer builds), `<exe>/assets`.
- User data: `--data-dir`, `<exe>/RetroverseData` with `--portable`, `%LOCALAPPDATA%\Retroverse` otherwise.
  Logs go to `<user data>/logs/retroverse.log`, F12 screenshots to `<user data>/screenshots`.
