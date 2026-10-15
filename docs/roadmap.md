# Roadmap

Derived from `specification.md` §22 (milestones M0-M6) and `style.md` §27 (visual phases A-E), ordered by
technical risk. Each iteration ends with a verifiable gate; each work session delivers one verifiable step.

| Iteration | Scope | Gate |
|---|---|---|
| **I0** | M0 native foundations + style Phase A look-development and tooling | M0 Done (app starts, controllable camera, stable rendering, no engine) + Gate A (empty but credible room) |
| I1 | Risk spikes: FFmpeg decode to GL texture via PBO, RmlUi with gamepad navigation, emulator launch/return with fullscreen restore | Success criteria met or fallback recorded in an ADR |
| I2 | M1 ES-DE catalog (discovery, gamelist/media/es_systems parsing, SQLite + FTS5, stable IDs) + Windows launcher + `retroverse-cli` | M1 Done: reads a real ES-DE collection without moving ROMs, launches an emulator and returns |
| I3 | M2 SNES vertical slice: single-room generator, instanced cartridges with real covers, inspect, play, CRT video, incremental update | Gate B: already looks like the concept with one room; deterministic WorldPlan snapshot |
| I4 | M3 multi-room world: hub, room graph, PS1 room, theme packs, streaming, overrides/pins, map and teleport | Gate C: different rooms, same world |
| I5 | M4 screens and media: decoder scheduler, audio, CRT shader, playlists, local media manager | Every room has working TVs within the decoder budget |
| I6 | Arcade and handheld rooms | Gate D |
| I7 | M5 part 1: wizard, emulator management, settings, statistics, file watcher, scraper framework | 30-minute controller-only session |
| I8 | Performance, Blender hero assets, license audit, golden screenshots, fault injection | Gate E + style §28 DoD |
| I9 | Installer, portable build, release | M5 Done |

Deferred beyond v1: ScreenScraper live scraping (needs developer credentials), gamelist write-back, legacy
EmulationStation/RetroBat/Batocera adapters, VR, hardware video decoding, Linux, an OpenGL 3.3 fallback.

## I0 / M0 step checklist

| # | Step | Status |
|---|---|---|
| 0 | Toolchain: vcpkg updated to the pinned baseline, glad2 generated | done |
| 1 | Repository skeleton, CMake presets, vcpkg manifest, layering rules, scripts, docs | done |
| 2 | `rv_core` (errors, logging, paths, frame clock, AABB, math/color, file I/O, JSON, CLI) + test infrastructure | done |
| 3 | SDL3 window, OpenGL 4.5 core context, `GLDevice` with debug output, app host loop, `--smoke` | done |
| 4 | Shaders with includes and hot reload, GPU meshes, framebuffer, offscreen `--screenshot` | done: `ShaderLibrary` (hot reload, `core::FileWatchSet`), `GpuMesh`, `UniformBuffer` + `FrameUniforms`, `Framebuffer`, `--scene` / `--screenshot` / `--screenshot-size`, F5 / F12, scenes drawn unlit in flat material colors (`scene::buildSceneGeometry`) |
| 5 | Input action mapping (keyboard, mouse, gamepad hot-plug) and first-person camera | done: `platform::SdlInputCollector` (keys by scancode, captured mouse motion, wheel, gamepad buttons/axes with +Y up), `GamepadManager` (hot-plug, most recently used pad), `MouseCapture` (click to capture, Esc or focus loss releases), bindings from `default_bindings.json` + per-action user overrides, FreeRoam walker with capsule collision against the scene (`scene::buildCollisionWorld`: room walls around the doorway and a stop in its outer face, so the walker stays in the room; furniture bounds; exact footprints for objects turned about +Y), Back/F5/F12 through the action map (Alt+Enter consumed by the host), `--input-script` + `--fixed-dt` (simulation and shader time) with the final pose logged, `rv_platform_tests` (virtual-joystick hot-plug) |
| 6 | Textures, images, material presets, `PBR_Base` and `Unlit_Emissive` shaders, sRGB pipeline | done: `Texture2D` (immutable storage, full mip chain, sRGB/linear RGBA8, anisotropy from the caps), `Sampler`, `TextureDefaults`, `TextureCache` (asset files, `pattern:` specs sized to their surface, model images; checker + one warning for missing or broken sources), `MaterialSet` (std140 `MaterialUniforms` at binding 2, texture flags, opaque/mask, blend drawn last unsorted), `LightsUniforms` at binding 1 (`packLights`: 8 point/spot + 1 directional, hemispheric ambient, exposure), shaders `pbr/pbr.{vert,frag}` (GGX, height-correlated Smith, Schlick, Lambert, normal maps in the glTF tangent space, occlusion, emissive) and `unlit/unlit_emissive.frag`, ACES fitted tonemapping, scene textures on labels/posters/CRT screen (pictures clamped, patterns sized to their surface as drawn; GPU-free draw plan `app::planSceneDraws`), clear color exposed and tonemapped (`gl::tonemap`), `--self-check srgb` (linear 0.5 -> 188), reference scenes tuned (test room: warm-white wall washers and fills that keep the neutrals neutral, amber practical lamp lit through its shade openings, entrance light, dark matte ceiling; `turn_around.json` viewpoint) |
| 7 | glTF loader (fastgltf) with sockets, prefab manifests | CPU part done |
| 8 | Scene renderer with batching and instancing, debug drawing, scene description files | CPU part done (batch planner, scene files) |
| 9 | Dear ImGui developer overlay | |
| 10 | M0 test room (procedural shell, shelf, cartridges, CRT, TV cabinet) -> **M0 Done** | CPU part done (prefab kit, `m0_testroom.json`) |
| 11 | Shadow map for the key light | |
| 12 | Capsule vs AABB collision | CPU part done (collision world, boxes turned about +Y included); prefabs declare colliders and the app walks with them (step 5); `--spawn` pending |
| 13 | `retroverse-asset-preview` tool | |
| 14 | GPU smoke test (offscreen render assertions) | |
| 15 | Build matrix, docs, golden screenshot -> **Gate A (preliminary)** | |
