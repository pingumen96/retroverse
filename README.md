# Retroverse

Retroverse turns an [ES-DE](https://es-de.org/) game collection into an explorable 3D world: a warm retro-gaming
museum whose rooms, shelves, cartridges and CRT televisions are generated from the library itself. Pick up a
cartridge, read its card, watch its preview on a CRT and launch the emulator, or skip the walk and use the quick
launcher.

It is a native Windows application written in C++20 with OpenGL 4.5 and SDL3, without a game engine. Retroverse
does not emulate anything and never copies or rewrites the collection: ROMs, `gamelist.xml` metadata and media
stay where ES-DE keeps them.

> **Status:** early development, milestone M0 (native foundations). Nothing user-facing is usable yet.
> See [docs/roadmap.md](docs/roadmap.md).

## Requirements

- Windows 10/11 x64, a GPU with OpenGL 4.5 core support.
- Visual Studio 2026 with the "Desktop development with C++" workload (MSVC 14.5x, bundled CMake and Ninja).
- [vcpkg](https://github.com/microsoft/vcpkg) cloned locally, with `VCPKG_ROOT` pointing at it. The manifest pins
  the baseline in `vcpkg.json`; update the clone if that commit is missing.

## Build

```powershell
./scripts/build.ps1                          # Debug (msvc-x64-debug); first run installs vcpkg packages
./scripts/test.ps1                           # unit tests
./build/msvc-x64-debug/bin/retroverse.exe --help
```

Other presets: `msvc-x64-relwithdebinfo`, `msvc-x64-release`, `msvc-x64-profile` (Tracy). The scripts enter the
Visual Studio developer environment on each call; see [CLAUDE.md](CLAUDE.md) for the full command list.

## Repository layout

| Path | Content |
|---|---|
| `src/` | Engine-free application code, one library per module (see [docs/architecture.md](docs/architecture.md)) |
| `assets/` | Shaders, material presets, prefab manifests, asset registry, reference scenes |
| `tests/` | Catch2 unit tests, test helpers, GPU smoke tests |
| `tools/` | Developer tools (asset preview) |
| `third_party/` | Vendored code not available through vcpkg (generated glad loader) |
| `docs/` | Roadmap, architecture, decision records, testing notes |
| `specification.md`, `style.md`, `example.png` | Product specification, art bible and concept (Italian) |

## License

Retroverse source code is released under the [MIT License](LICENSE). Third-party libraries keep their own licenses.
Retroverse ships no ROMs, BIOS files, cover art or other copyrighted game content.
