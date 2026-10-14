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
| 3 | SDL3 window, OpenGL 4.5 core context, `GLDevice` with debug output, app host loop, `--smoke` | |
| 4 | Shaders with includes and hot reload, GPU meshes, framebuffer, offscreen `--screenshot` | |
| 5 | Input action mapping (keyboard, mouse, gamepad hot-plug) and first-person camera | |
| 6 | Textures, images, material presets, `PBR_Base` and `Unlit_Emissive` shaders, sRGB pipeline | |
| 7 | glTF loader (fastgltf) with sockets, prefab manifests | |
| 8 | Scene renderer with batching and instancing, debug drawing, scene description files | |
| 9 | Dear ImGui developer overlay | |
| 10 | M0 test room (procedural shell, shelf, cartridges, CRT, TV cabinet) -> **M0 Done** | |
| 11 | Shadow map for the key light | |
| 12 | Capsule vs AABB collision | |
| 13 | `retroverse-asset-preview` tool | |
| 14 | GPU smoke test (offscreen render assertions) | |
| 15 | Build matrix, docs, golden screenshot -> **Gate A (preliminary)** | |
