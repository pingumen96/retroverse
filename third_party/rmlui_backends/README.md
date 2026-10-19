# RmlUi backends (vendored)

The SDL3 platform backend and the OpenGL 3 renderer of [RmlUi](https://github.com/mikke89/RmlUi), copied from the
`Backends` directory of tag **6.3** (commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413`), the version vcpkg installs
(`build/vcpkg_installed/x64-windows/share/rmlui`, port `rmlui` 6.3 with the `freetype` feature). RmlUi's packages ship
the library but not its backends, which upstream intends to be copied into applications. Used by `rv_ui` only
(`src/ui`, ADR-0005), built as the static library `rv_rmlui_backends` (`CMakeLists.txt`, added by
`src/ui/CMakeLists.txt`).

| File | Upstream SHA-256 (LF line endings) | State |
|---|---|---|
| `RmlUi_Platform_SDL.h` | `4819581a4dcb1c7bfad84732f048b07462c8a27f4556f4c35d886928e1c038be` | unchanged |
| `RmlUi_Platform_SDL.cpp` | `9cf82af58a085c88363e9cce9b764dbe1177ea2330170b2dfb48d4f09d375e9d` | unchanged |
| `RmlUi_Renderer_GL3.h` | `a482f40ff59ca0f72638714b29c23ecebdbffc4f3aedd27d7cd43b889bb7ab98` | local changes below |
| `RmlUi_Renderer_GL3.cpp` | `8f51d9dafc81ace13bcdf56a120f61cdf2f217bed97a890772d9758feb9857dd` | local changes below |
| `LICENSE.txt` | `64c049f65ec810b805457dc1f24745002203f919ea5a16b6fcd7ca5f45ea298e` | unchanged |

## Build configuration

- `RMLUI_SDL_VERSION_MAJOR=3` (public): the SDL3 code paths of the platform backend.
- `RMLUI_GL3_CUSTOM_LOADER=<glad/gl.h>` (private): the renderer includes our glad 2 loader (`third_party/glad`,
  OpenGL 4.5 core, already loaded by `rv::gl::GLDevice`) instead of compiling its own glad (`RmlUi_Include_GL3.h`, not
  vendored). There is one OpenGL loader in the process and no duplicate `glad_gl*` symbols; `RmlGL3::Initialize` only
  reports success. The renderer's shaders stay `#version 330`, valid in the 4.5 core context.
- MSVC: `/W4 /WX` like the project, with `/wd4100` (the `out_message` parameter of `RmlGL3::Initialize`, unused with a
  custom loader) instead of a source change. `RMLUI_NUM_MSAA_SAMPLES` keeps its default, 2.

## Local changes

All in `RmlUi_Renderer_GL3.*`, each marked `Retroverse local change`:

1. **Output target.** `SetOutput(framebuffer, x, y, width, height)` names the framebuffer `EndFrame()` composites the
   frame into, where upstream always draws into the default framebuffer, and the rectangle it covers there, scaled
   bilinearly; the defaults (framebuffer 0, rectangle 0 x 0: 1:1 at the viewport offset) are upstream's behavior.
   `rv::ui::UiView` draws into the window and, scaled uniformly and centered like the developer overlay, into the
   screenshot targets of `app::SceneGraphics`, whatever their size.
2. **Optional state backup.** `SetStateBackup(false)` skips reading back the OpenGL state in `BeginFrame()` (about 35
   `glGet*` and `glIsEnabled` calls) and leaves that state at OpenGL's initial values in `EndFrame()` instead of
   restoring what was read (scissor, stencil and depth tests, culling and blending off; color and stencil writes on;
   stencil function and operations, blend function and equation, clear values at their initial values; texture unit 0
   active). On NVIDIA's threaded driver the read-back took 0.6 to 0.67 ms of CPU per frame (median, Release), more than
   the rest of the UI pass; Retroverse keeps its own `gl::RenderStateCache` and invalidates it after the UI. Enabled by
   default, as upstream.
3. **Sampler objects.** `BeginFrame()` unbinds the sampler objects of texture units 0 and 1 (`glBindSampler(unit, 0)`).
   The renderer samples its textures (font atlases, layers) with their own parameters and no mipmaps; the samplers
   Retroverse's material registry leaves bound (trilinear) made them incomplete, and the whole UI read as opaque black.
4. **Host textures (`rv-texture:`).** `SetExternalTexture(key, texture, size)` registers an OpenGL texture the host
   owns under a key; an image source `rv-texture:<key>` (an `<img src>`, an image decorator) loads as a handle of the
   key's slot, `RenderGeometry()` binds the texture the slot names when it draws (nothing is drawn while the slot is
   empty) and `ReleaseTexture()` never deletes it. RmlUi thus never reads a picture from a file: the product UI shows
   the app's textures (I3 step 14a: the world's card shows the inspected game's hero texture, through an RGBA8 view of
   its sRGB storage, since the UI composites sRGB-encoded values). A slot's handle stays valid while the host replaces
   or empties its texture, so a document keeps its `<img>` across games.

Upstream issues worth raising: an output framebuffer for `EndFrame()`, an opt-out of the state backup, the sampler
reset and textures registered by the host would make every local change unnecessary.

## License

MIT, see `LICENSE.txt`:

```text
MIT License

Copyright (c) 2008-2014 CodePoint Ltd, Shift Technology Ltd, and contributors
Copyright (c) 2019-2026 The RmlUi Team, and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Updating

Copy the four files from the `Backends` directory of the RmlUi tag that matches the vcpkg port, convert them to LF,
re-apply the local changes (search for `Retroverse local change` in the old copy), update the table above and rebuild
with `/W4 /WX`.
