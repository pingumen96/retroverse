# ADR-0005: Product UI with RmlUi, navigated with the gamepad

- Status: accepted
- Date: 2026-10-16
- Amended: 2026-10-17, at I2 step 6: the Input paragraph says that `NavInput` reads the UI actions of the action map
  (`ui_activate` among them; the stick read whole, so the hysteresis thresholds stay fractions of its travel;
  `NavBindings` gone); the Consequences record the virtualized quick catalog and the cost of a held direction that
  scrolls it (about a tap), within the accepted deviation
- Amended: 2026-10-17, at I2 step 13 (folded at step 14, Gate M1): the Consequences record what the quick catalog's card
  shows of a game of the ES-DE catalog (its launch readiness and its first blocker, never cut through a line) and that
  M1's manual session searches with the keyboard (the on-screen keyboard stays in I7)

## Context

Specification §13.1 gives the product screens to RmlUi and keeps Dear ImGui for developer tools; the screens must read
on a TV and work with a controller first (§12.1 controls, §12.4 text scaling and high contrast, §13.2 the quick
catalog, §5.3 kiosk use; style §16.3 contextual panel, §17.3 typography, §3 palette). The plan made the choice
conditional on spike (d) of iteration I1 (`docs/roadmap.md`): a quick-catalog mock screen usable with the pad alone,
with a stable focus, the UI pass under 1 ms (at 1080p) and correct resizing, built with a reasonable integration effort
(the approved I1 plan, which is not in the repository: "within two sessions"). Fallback: the product UI in Dear ImGui
with pad navigation behind the same C++ view models.

The spike built the first version of the product UI and measured it (`docs/spikes/i1-ui.md`): `rv_ui_model` (GPU-free
navigation, focus and view model), `rv_ui` (RmlUi on SDL3 and OpenGL), the quick catalog document
(`assets/ui/quick_catalog.rml`, `.rcss`) and `spike_ui`, which drives the screen over the test room with a scripted
virtual gamepad.

## Decision

RmlUi is the product UI toolkit; the fallback is not taken. Four criteria were met, the fifth in part:

- Met: the screen is usable with the pad alone (text entry aside: the search field takes typed text until the on-screen
  keyboard, I7), its focus is visible and stable, resizing and text scaling lay it out correctly, and the integration
  took two sessions (the spike and its review fixes).
- Met in part: the UI pass stays under 1 ms in the frames in which the UI does not change (CPU p50 0.21 to 0.24 ms, 0.49
  ms in the slower of the driver's two modes; GPU p50 0.09 to 0.11 ms), but not in the frames that change it: a focus
  move with the D-pad held costs 1.2 to 1.5 ms of CPU (p50) and a single tap, which lays the card out again in the same
  frame, 3.0 to 4.2 ms (`docs/spikes/i1-ui.md`). The deviation is accepted: the budget was set for the cost of showing
  the UI in every frame, which holds with room to spare; a change costs at most a quarter of a 60 Hz frame, comes only
  with input, and the work to get it under 1 ms is known and contained in the renderer and the screen (Consequences).
  The Dear ImGui fallback was not built, so there is no measured comparison; its immediate mode would lay the whole
  screen out in every frame instead, and lose the style sheets and data bindings the product screens rely on.

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
- **Input:** keys bound to UI actions (arrows, Enter, Esc, Page Up/Down, Home, End) and every gamepad event reach the
  UI as `ui::UiCommand`s from `ui::NavInput` (D-pad, left stick with hysteresis 0.55 / 0.35 and a 1.25 axis-switch
  ratio, repeat after 0.4 s every 0.1 s, every 0.05 s after 1.5 s; A Activate, B Back, Y Media, X Info), the same path
  for pad and keyboard. Mouse, wheel, text input and the other keys go to the context through the SDL backend. Focus
  moves use RmlUi's spatial navigation (`nav: auto`) confined to regions that are scroll containers, `ui::FocusRegions`
  links between regions (with the last focused element of each) and `ui::FocusHistory` for Back. The spike's
  `NavInput` read the raw device input through a binding table of its own (`ui::NavBindings`), so the user's
  `bindings.json` did not reach the product UI. This ADR planned eight UI actions (`ui_up`, `ui_down`, `ui_left`,
  `ui_right`, `ui_activate`, `ui_back`, `ui_media`, `ui_info`) in `assets/config/default_bindings.json`, with
  `NavBindings` built from the action map. Since I2 step 6 `NavInput` reads the UI actions of the action map (`ui_up`
  ... `ui_filter_next`, `assets/config/default_bindings.json`, the user's `bindings.json` over them) from
  `input::ActionState`, keeping the stick hysteresis (the stick read whole by `ui_nav_x`/`ui_nav_y`, without a dead
  zone: 0.55 / 0.35 of its travel, unchanged) and the repeat, with a release threshold for the triggers that page;
  `NavBindings` is gone, since reading `ActionState` made that table unnecessary; the user's bindings and
  `--input-script` reach the product UI. The activate action is `ui_activate`, after `ui::UiCommand::Activate`. Beyond
  the eight planned actions, the defaults page with the triggers and Page Up/Down, jump to the first and last game with
  the stick clicks and Home/End, and change the filter with the shoulders: a list of 100,000 games needs jumps on the
  pad.
- **Rendering:** the UI is drawn after the scene into the window with `GL_FRAMEBUFFER_SRGB` off (RmlUi's colors are
  sRGB and blend in sRGB like a browser), then `gl::RenderStateCache::invalidate()`; into screenshots scaled uniformly
  like the developer overlay. CPU and GPU times are measured per view.
- **Look and assets:** documents and style sheets in `assets/ui` (hot reload in developer builds), Atkinson Hyperlegible
  (SIL OFL 1.1, `assets/registry`) and no system font, sizes in dp, a compact layout for views under 760 dp of the
  display's density tall (an RmlUi theme that `UiView` activates, `@media (theme: compact)`, so that the text scale
  never switches layouts: a dp media query would cancel text scaling), a high-contrast class, Italian strings in the
  documents. No color transitions on focus (below). Regions that must follow the focus are scrollable (`overflow:
  auto`), since RmlUi never scrolls a hidden one into view.

## Consequences

- Performance at 1920 x 1080, dp ratio 1.25, over the test room (Release, 60 Hz pacing, RTX 2080 SUPER, driver 617.42;
  16 runs on a machine loaded by the other I1 spikes; `docs/spikes/i1-ui.md`): quiet frames cost 0.21 to 0.24 ms of CPU
  and 0.09 to 0.11 ms of GPU (p50). The driver runs a process's OpenGL calls in one of two modes; in the slower one
  (1 run in 16 here, 7 in 37 before) a quiet frame costs 0.49 ms of CPU, 0.43 to 0.71 ms in the earlier runs: the
  conservative figure, still under the budget. Frames with changes: 1.2 to 1.5 ms per focus move with the D-pad held,
  3.0 to 4.2 ms per single tap (the card laid out again), at p50; under the other spikes' load single frames reached
  tens and once hundreds of milliseconds. Over the virtualized list of I2 step 6 (100,000 games, `docs/i2/step-6.md`;
  informative figures on a loaded machine) quiet frames cost 0.13 to 0.15 ms of CPU and a single tap what it cost,
  while a held direction that scrolls the list costs about a tap, 2.8 to 3.0 ms at p50: RmlUi 6.3 formats the whole
  document and generates every text's mesh again for the entering row's text; documents of their own for the list and
  the card and the pooled vertex buffer are the levers. The accepted deviation holds: a change stays under a quarter of
  a 60 Hz frame and comes only with input.
- RmlUi is retained-mode: quiet frames are cheap, changes cost, and a change of layout (a new text) formats the whole
  document again from its root (`ElementDocument::UpdateLayout`). Its 6.3 renderer builds new OpenGL buffers for every
  compiled geometry, so color transitions re-mesh on every animated frame (0.9 ms of CPU per frame while the focus
  moved, against 0.2 ms without them): product style sheets avoid color and size animations; transforms are the cheap
  way to animate. To bring changes under 1 ms: new geometry into a pooled vertex buffer in the vendored renderer, the
  card and the list in documents of their own, the selected row marked without re-evaluating the bound list (done in
  I2 step 6, with a class on the two rows concerned). Work deferred while the focus runs (the card) is done at once
  whenever something acts on it.
- Allocations: in Release RmlUi allocates 3 times per quiet frame, 2,600 and 350 more (update, render) when a tap lays
  the card out; Retroverse's code allocates nothing in any measured frame, the vendored renderer one object per new
  geometry. Counted in Release by patching the import tables of `spike_ui.exe` and `rmlui.dll`; the Debug counts (554
  per quiet frame) are inflated by the debug iterators' proxies.
- A catalog of 40,000 games needs a virtualized list and deferred work while the focus runs (the card follows only
  once the focus rests). Done in I2 step 6: `QuickCatalogModel` is a window over a paged `ui::CatalogSource`, whose
  queries may answer later from the catalog worker, and the screen binds a pool of as many rows as fit, in which a game
  keeps its row while the window holds it, so a scroll step binds one row.
- The card of a game of the ES-DE catalog (I2 step 13, `docs/i2/step-13.md`) shows its launch readiness as one Italian
  line and its first blocker as an Italian sentence of one or two lines (one per blocker code,
  `ui::quick_catalog::kBlockerCodes`); the blocker's own words are left to the game-session panel that Avvia opens, so
  that the card keeps its description and metadata rows. The card's text is never cut through a line: its title,
  readiness, blocker sentence, description (whole lines) and actions keep their height, and the metadata rows give way
  first, by whole rows; the run report's `catalog.cardSqueezed` names any block the layout still cuts, and the smoke
  cases require it empty.
- Text entry with the pad needs an on-screen keyboard (I7): until then the quick catalog's search takes typed text, and
  M1's manual pad-only session (`docs/testing/gate-m1.md`) uses the keyboard for its one search step (a decision of the
  Gate M1 step; BACKLOG "Text entry with the pad"); localization moves the documents' strings into a table
  bound like the rest (`assets/ui/locale`); covers and videos can be shown by mapping an image source scheme to our GL
  textures in the render interface (I3, I5).
- `rv_ui` is a GPU-side module next to `rv_devui`: it calls OpenGL through the vendored renderer and `UiView`, on the
  main thread, and SDL through the platform backend. It links `rv_devui_model` (GPU-free, always built) for
  `devui::fitOverlayCapture` until that helper moves below both overlays.
- Dear ImGui stays developer-only. Upgrading RmlUi means re-vendoring the backends and re-applying the local changes
  (README).
