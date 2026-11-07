# Third-party code

Every library Retroverse builds with, from vcpkg or vendored under `third_party/`, with its version, license, how it is
linked and whether a binary distribution must carry its notice. Specification §17.2 and §19.3 ask for a license and
origin record of every component that ships; packaging the notices themselves (an about page or a `THIRD-PARTY` file
next to the executables) is part of I9 (roadmap: installer, portable build, release).

Versions are those of the vcpkg baseline pinned in `vcpkg.json` (`44ccfd88`, ADR-0001, ADR-0007); they move only with
the baseline, which needs a new ADR. Licenses come from each port's `vcpkg.json` and from the notice vcpkg installs in
`build/vcpkg_installed/x64-windows/share/<port>/copyright`; re-read both after a baseline change. Assets have their own
records in `assets/registry/`.

"Notice" says whether a binary distribution must carry the library's copyright and license text: **yes** for MIT, BSD,
Apache-2.0, the FreeType License and the LGPL (which also asks for its full text, the library replaceable by the user,
and an offer of its source: the DLLs keep it replaceable); **no** for public domain, CC0, Zlib, BSL-1.0, the libpng and
bzip2 licenses, which ask nothing of binaries. "DLL" means the import library is linked and the vcpkg applocal step
copies the DLL (Debug builds: the debug variant) next to each executable that loads it.

## vcpkg dependencies (`vcpkg.json`)

| Port | Version | Features | License | Linked as | Used by | Notice |
|---|---|---|---|---|---|---|
| catch2 | 3.16.0 | default | BSL-1.0 | static (`Catch2`, `Catch2Main`) | the test executables only, never shipped | no |
| fastgltf | 0.9.1 | default | MIT | DLL `fastgltf.dll` | `rv_assets` (glTF) | yes |
| glm | 1.0.3 | default | MIT (or the Happy Bunny License) | static `glm.lib` and headers | `rv_core` and up | yes |
| imgui | 1.92.9 | `docking-experimental`, `opengl3-binding`, `sdl3-binding` | MIT | static `imgui.lib` | `rv_devui`: developer builds only, not in Release | yes, if a developer build is distributed |
| lunasvg | 3.5.0 | default | MIT | DLL `lunasvg.dll` (and `plutovg.dll`, below) | `rv_assets`, private to `svg_raster.cpp` (I11 step 2, ADR-0019 §3: the systems' SVG logos read from ES-DE's themes, bounded and refused when they reach past the document) | yes |
| miniaudio | 0.11.25 | default (header only) | Unlicense or MIT-0 | header-only, compiled into `rv_audio` (`miniaudio_impl.cpp`: device I/O only, on WASAPI and miniaudio's null backend) | `rv_audio` (I5 step 5, ADR-0016 §9: the clips' sound) | no (neither license asks for one) |
| nlohmann-json | 3.12.0#2 | default | MIT | header-only | `rv_core` (JSON) | yes |
| pugixml | 1.16 | none | MIT | DLL `pugixml.dll` | `rv_esde`, private (ES-DE's XML, ADR-0008) | yes |
| sdl3 | 3.4.18 | default features off | Zlib; bundled code: BSD-3-Clause (hidapi), MIT or public domain (stb_image), Apache-2.0 (one OpenGL ES header) | DLL `SDL3.dll` | `rv_platform` and the GPU-side modules above it | yes: hidapi's BSD notice (SDL's Zlib license needs none) |
| spdlog | 1.17.0#1 | default (external fmt) | MIT | DLL `spdlog.dll` | `rv_core` (logging) | yes |
| sqlite3 | 3.53.4#2 | `fts5`, `json1` | public domain (`blessing`) | DLL `sqlite3.dll` | `rv_catalog_db`, private (the catalog database, ADR-0008) | no |
| stb | 2026-08-02 | default | MIT or public domain, per file | header-only, compiled into `rv_assets` (each implementation in one source, with internal linkage) | `rv_assets`: images (`stb_image`, `stb_image_write`); since I3 step 5 the collection's covers and labels (ADR-0011 §4 and §5): `stb_image_resize2` (fitting covers, `cover_fit.cpp`), `stb_dxt` (BC1, `bc1.cpp`), `stb_truetype` (spines and generated labels from the bundled font, `text_raster.cpp`; only ever the font files whose SHA-256 match `assets/registry/assets.json`) | no (the public-domain alternative) |
| tl-expected | 1.3.1 | default | CC0-1.0 | header-only | `rv_core` (`rv::Expected`) | no |
| tracy (feature `profiling`) | 0.14.1#1 | `on-demand`, default features off | BSD-3-Clause | DLL (TracyClient, the triplet's linkage; installed only when that preset configures) | `rv_core` with `RV_ENABLE_TRACY`: the `msvc-x64-profile` preset only, never shipped | yes, if a profile build is distributed |
| ffmpeg (feature `video`) | 9.0.2#1 | `avcodec`, `avformat`, `swresample`, `swscale`, `zlib`; default features off | LGPL-2.1-or-later (no GPL component enabled) | DLLs `avcodec-63`, `avformat-63`, `avutil-61`, `swresample-7`, `swscale-10` | `rv_media` (ADR-0004) | yes: the LGPL text and an offer of the source |
| rmlui (feature `ui`) | 6.3 | `freetype` | MIT (the unused `rmlui_debugger.dll` bundles OFL-1.1 fonts) | DLL `rmlui.dll` | `rv_ui` (ADR-0005) | yes |

## Pulled in by them

| Port | Version | License | Linked as | Needed by | Notice |
|---|---|---|---|---|---|
| plutovg | 1.3.3 | MIT | DLL `plutovg.dll` | lunasvg (its 2D vector rasterizer) | yes |
| fmt | 12.2.0#1 | MIT | DLL `fmt.dll` | spdlog (and `fmt::format` through it) | yes |
| simdjson | 5.0.2 | Apache-2.0 or MIT; parts BSL-1.0 and BSD-3-Clause | DLL `simdjson.dll` | fastgltf | yes |
| freetype | 2.14.3 | FreeType License (FTL) or GPL-2.0-or-later: FTL chosen | DLL `freetype.dll` | rmlui | yes: the FTL's credit in the documentation |
| libpng | 1.6.59 | libpng-2.0 | DLL `libpng16.dll` | freetype | no |
| zlib | 1.3.2#2 | Zlib | DLL `z.dll` | ffmpeg, freetype, libpng | no |
| brotli | 1.2.0 | MIT | DLLs `brotlicommon.dll`, `brotlidec.dll` | freetype | yes |
| bzip2 | 1.0.8#6 | bzip2-1.0.6 | DLL `bz2.dll` | freetype | no |
| robin-hood-hashing | 3.11.5#2 | MIT | header-only, compiled into `rmlui.dll` | rmlui | yes |
| itlib | 1.12.2 | MIT | header-only, compiled into `rmlui.dll` | rmlui | yes |

Build tools that vcpkg installs alongside and that are never linked or shipped: pkgconf 3.0.3#1, ffmpeg-bin2c 9.0.2,
vcpkg-tool-meson 1.9.0#10, vcpkg-cmake, vcpkg-cmake-config, vcpkg-cmake-get-vars, vcpkg-pkgconfig-get-modules.

## Vendored (`third_party/`)

| Directory | Version | License | Linked as | Used by | Notice |
|---|---|---|---|---|---|
| `glad` | loader generated by glad 2.0.8 (OpenGL 4.5 core and six extensions, `GL_EXT_texture_sRGB` added in I3 step 0a for the collection's BC1 sRGB arrays, ADR-0011 §4; `README.md`) | `(WTFPL OR CC0-1.0) AND Apache-2.0` (generated code; the Khronos registry it is generated from); `KHR/khrplatform.h`: the Khronos MIT-style license | static `rv_glad` | `rv_render`, `rv_rmlui_backends` | yes: Apache-2.0 and khrplatform.h's notice |
| `rmlui_backends` | RmlUi 6.3's SDL3 platform and OpenGL 3 renderer backends, with local changes (`README.md`) | MIT (`LICENSE.txt`, RmlUi's) | static `rv_rmlui_backends` | `rv_ui` | yes (RmlUi's notice) |
