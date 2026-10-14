# Architecture

This document describes the code that exists today. The target architecture is in `specification.md` §15.

## Modules

| Module | CMake target | Responsibility | May depend on |
|---|---|---|---|
| core | `rv_core` | Errors (`rv::Expected`), logging, paths, frame clock, AABB/ray math, sRGB color helpers, file I/O, JSON, command line parsing | glm, spdlog, nlohmann-json, tl-expected |
| geometry | `rv_geometry` | CPU meshes (`MeshData`, 48-byte `Vertex`), sockets, procedural mesh builder and procedural prefabs | core |
| input | `rv_input` | Device-neutral codes, logical actions, action maps, per-frame `ActionState` | core |
| assets | `rv_assets` | Image decoding/encoding, glTF loading, prefab manifests, material presets, shader source loading | core, geometry, fastgltf, stb |
| scene | `rv_scene` | Camera, first-person controller, lights, render items and batching plan, scene description files, collision | core, geometry, input, assets |
| platform | `rv_platform` | SDL3 window, OpenGL context, event pump, gamepads, SDL -> input translation | core, input, SDL3 |
| render | `rv_render` | OpenGL 4.5 device, shaders, buffers, textures, framebuffers, scene renderer, debug drawing | core, geometry, assets, scene, glad |
| devui | `rv_devui` | Dear ImGui developer overlay (developer builds only) | platform, render, imgui |
| app | `rv_app` | Composition root: application host, main loop, scenes, screenshots | all of the above |

Two GPU-free sub-targets keep testable policy out of the GPU-side libraries:

| Sub-target | Part of | Content |
|---|---|---|
| `rv_app_cli` | app | Command line options and validation, exit-code policy |
| `rv_render_debug_filter` | render | Which OpenGL debug messages are logged, dropped or deduplicated |

`core`, `geometry`, `input`, `assets`, `scene` and the two sub-targets are **GPU-free**: they never reach SDL3,
glad or ImGui (enforced at configure time by `cmake/LayerRules.cmake`), so they are unit-tested without a window or
a context. `render` receives the OpenGL function loader as a function pointer and never includes SDL.

Cross-module contracts written by independent work streams (prefab parameters and sockets, material names, scene
files, manifests) are pinned by `tests/unit/integration`, which builds the real geometry and resolves the real
materials for every reference scene and manifest.

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
  Logs go to `<user data>/logs/retroverse.log`.
