# ADR-0005: Product UI with RmlUi, navigated with the gamepad

- Status: accepted
- Date: 2026-10-16

## Context

Specification §13.1 gives the product screens to RmlUi and keeps Dear ImGui for developer tools; the screens must read
on a TV and work with a controller first (§12.1 controls, §12.4 text scaling and high contrast, §13.2 the quick
catalog, §5.3 kiosk use; style §16.3 contextual panel, §17.3 typography, §3 palette). The plan made the choice
conditional on spike (d) of iteration I1 (`docs/roadmap.md`): a quick-catalog mock screen usable with the pad alone,
with a stable and visible focus, the UI pass under 1 ms at 1080p, correct resizing and a reasonable integration effort
(plan §4.1: within two sessions). Fallback: the product UI in Dear ImGui with pad navigation behind the same C++ view
models.

The spike built the first version of the product UI and measured it (`docs/spikes/i1-ui.md`): `rv_ui_model` (GPU-free
navigation, focus and view model), `rv_ui` (RmlUi on SDL3 and OpenGL), the quick catalog document
(`assets/ui/quick_catalog.rml`, `.rcss`) and `spike_ui`, which drives the screen over the test room with a scripted
virtual gamepad.

## Decision

RmlUi is the product UI toolkit. Every criterion was met; the fallback is not taken.

- **Library:** RmlUi 6.3 from vcpkg (feature `ui` of the manifest, FreeType font engine, linked as `rmlui.dll`). Its
  SDL3 platform backend and OpenGL 3 renderer are vendored in `third_party/rmlui_backends` (README: source, tag, MIT
  license, SHA-256) and built on our glad loader through `RMLUI_GL3_CUSTOM_LOADER`: one OpenGL loader, no duplicate
  symbols. Three local changes to the renderer, each a candidate upstream option: an output target (framebuffer and
  rectangle, for screenshots of any size), an opt-out of its OpenGL state read-back (0.6 ms of CPU per frame on NVIDIA's
  threaded driver; Retroverse invalidates its own state cache instead) and a reset of the samplers the host leaves
  on texture units 0 and 1 (they made RmlUi's textures incomplete: an all-black UI).
- **Structure:** `ui::UiSystem` owns RmlUi's global state (system interface with the application's clock and the log
  routed to spdlog, a file interface on the asset root with UTF-8 paths, the renderer, the bundled fonts); one
  `ui::UiView` (an RmlUi context) per output, sized to its drawable with dp ratio = display scale x text scale; screens
  such as `ui::QuickCatalogScreen` bind GPU-free view models (`ui::QuickCatalogModel`) via RmlUi data models. Screens
  never hold domain state, so a different toolkit could bind the same models.
- **Input:** keys bound to UI commands (arrows, Enter, Esc) and every gamepad event reach the UI as `ui::UiCommand`s
  from `ui::NavInput` (D-pad, left stick with hysteresis 0.55 / 0.35 and a 1.25 axis-switch ratio, repeat after 0.4 s
  every 0.1 s, every 0.05 s after 1.5 s; A Activate, B Back, Y Media, X Info), the same path for pad and keyboard.
  Mouse, wheel, text input and the other keys go to the context through the SDL backend. Focus moves use RmlUi's
  spatial navigation (`nav: auto`) confined to regions that are scroll containers, `ui::FocusRegions` links between
  regions (with the last focused element of each) and `ui::FocusHistory` for Back.
- **Rendering:** the UI is drawn after the scene into the window with `GL_FRAMEBUFFER_SRGB` off (RmlUi's colors are
  sRGB and blend in sRGB like a browser), then `gl::RenderStateCache::invalidate()`; into screenshots scaled uniformly
  like the developer overlay. CPU and GPU times are measured per view.
- **Look and assets:** documents and style sheets in `assets/ui` (hot reload in developer builds), Atkinson Hyperlegible
  (SIL OFL 1.1, `assets/registry`) and no system font, sizes in dp, a compact layout below 760 dp of height (media
  query), a high-contrast class, Italian strings in the documents. No color transitions on focus (below).

## Consequences

- Performance at 1920 x 1080, dp ratio 1.25, over the test room (Release, 60 Hz pacing, RTX 2080 SUPER, medians of
  three sets of three runs on a machine loaded by other builds; `docs/spikes/i1-ui.md`): the UI pass takes 0.2 to
  0.3 ms of CPU and 0.1 ms of GPU per frame (p50); a frame in which the focus moves takes about 1.3 to 1.5 ms of CPU
  (p50), and laying the selected game's card out again about 2.5 ms more, once, when the focus rests (an exploratory
  run). The 1 ms budget holds for the steady state and is exceeded, briefly, by input-driven frames: still far inside a
  16.7 ms frame.
- RmlUi is retained-mode: quiet frames are cheap, changes cost. Its 6.3 renderer rebuilds an element's geometry (new
  OpenGL buffers) for every change, so color transitions re-mesh on every animated frame (0.9 ms of CPU per frame while
  the focus moved, against 0.2 ms without them): product style sheets avoid color and size animations; transforms are
  the cheap way to animate. RmlUi also allocates on the heap every frame (Debug count: about 5 allocations in the update
  and 540 in the render of the quick catalog when nothing changes); our own UI code allocates nothing per frame.
- A catalog of 40,000 games needs a virtualized list (a window of rows bound to a slice of the model) and deferred work
  while the focus runs (the card already follows only once the focus rests); `QuickCatalogModel` is ready for it.
- Text entry with the pad needs an on-screen keyboard (I7); localization moves the documents' strings into a table
  bound like the rest (`assets/ui/locale`); covers and videos can be shown by mapping an image source scheme to our GL
  textures in the render interface (I3, I5).
- `rv_ui` is a GPU-side module next to `rv_devui`: it calls OpenGL through the vendored renderer and `UiView`, on the
  main thread, and SDL through the platform backend.
- Dear ImGui stays developer-only. Upgrading RmlUi means re-vendoring the backends and re-applying the local changes
  (README).
