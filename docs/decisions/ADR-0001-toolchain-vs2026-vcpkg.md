# ADR-0001: Toolchain, dependency management and core libraries

- Status: accepted
- Date: 2026-10-14

## Context

The specification (§3.1, §17.2) asks for C++20, OpenGL, CMake + Ninja + vcpkg with pinned versions and MSVC x64 as
the primary Windows toolchain. The development machine has Visual Studio 2026 (18.10, MSVC 14.51) and no
Visual Studio 2022. Its vcpkg clone was 15 months old and its tool predated Visual Studio 2026 support.

## Decision

- **Compiler:** MSVC 14.51 (Visual Studio 2026), C++20, `/W4 /WX /permissive- /utf-8 /Zc:preprocessor`.
- **Build:** CMake >= 3.28 (VS-bundled 4.3.1 in practice) with Ninja, presets `msvc-x64-{debug,relwithdebinfo,
  release,profile}`. `scripts/dev.ps1` enters the VS developer environment for every command, compacting the
  machine PATH first because its length breaks `VsDevCmd.bat` (cmd.exe 8191-character line limit).
- **Dependencies:** vcpkg manifest mode, `builtin-baseline` `44ccfd88b3eca6929d6d16398c7b19945c29a5f6`
  (2026-10-13), triplet `x64-windows` (shared libraries; compatible with LGPL FFmpeg later),
  `VCPKG_INSTALLED_DIR=build/vcpkg_installed` shared by all presets of a checkout.
- **Libraries in M0:** SDL3, glm, spdlog, fastgltf, stb, nlohmann-json, tl-expected, Dear ImGui (SDL3 + OpenGL3
  backends, docking), Catch2 3. Optional manifest features: `profiling` (Tracy), `video` (FFmpeg), `ui` (RmlUi).
- **OpenGL loader:** glad2 generated for GL 4.5 core and vendored in `third_party/glad` (the vcpkg `glad` port is
  glad 1 with a compatibility-profile default and a Python build step).
- **Errors:** `rv::Expected<T>` = `tl::expected<T, rv::Error>` (C++23 `std::expected` API, available in C++20).
- **OpenGL baseline:** 4.5 core only, DSA everywhere; no 3.3 fallback (spec §24 open decision 2, closed).
- **Tests:** Catch2 v3, discovered by CTest at test time, labels `unit` and `gpu`.

## Consequences

- Updating the vcpkg baseline or the compiler invalidates the binary cache: do it deliberately, with a new ADR.
- A machine without Visual Studio 2026 must adapt `scripts/dev.ps1` (it uses vswhere and picks the latest VS).
- Module target names are shorter than in the specification's suggested layout (`rv_platform` for
  `platform/sdl`, `rv_render` for `rendering/gl`); the mapping is documented in `docs/architecture.md`.
