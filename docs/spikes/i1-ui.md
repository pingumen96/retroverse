# I1 spike (d): product UI with RmlUi and the gamepad

Outcome: **RmlUi adopted** (ADR-0005). The quick catalog mock screen is usable with the pad alone, its focus is always
visible and never jitters, the UI pass costs about 0.2 ms of CPU and 0.1 ms of GPU per frame at 1920 x 1080, and
resizing lays it out again correctly. Frames in which the focus moves cost more (1.3 to 1.5 ms of CPU, p50), which stays
far inside a 60 Hz frame; the measurements and the three fixes that got there are below. Built and measured in one
session.

## Question and criteria

From `docs/roadmap.md` (I1, row d) and plan §4.1: can RmlUi 6.3, on SDL3 and our OpenGL 4.5 context, carry the product
screens (specification §13.1) with a controller first?

| Criterion | Verdict | Evidence |
|---|---|---|
| A quick-catalog mock screen usable with the pad alone | met | 39 scripted checks pass in every run: focus, actions, history, filters, typed search (below) |
| Stable, visible focus | met | a stick resting at 0.47 to 0.52 for 2 s moves nothing; a near-diagonal stick steps once and never turns; `:focus` styles on every focusable element, scrolled into view |
| UI pass under 1 ms at 1080p | met (steady state) | p50 0.19 to 0.29 ms CPU, 0.09 to 0.12 ms GPU; frames with a focus move 1.3 to 1.5 ms CPU p50 |
| Correct resizing | met | 1920 x 1080 -> 1280 x 720 -> 1920 x 1080: the view follows the drawable at the display's density, every panel inside the view, the focused game visible, the layout back at 1080p equal to the first to 0.5 px |
| Reasonable integration effort | met | one session; 3 local changes (about 60 lines) to the vendored renderer, everything else on documented RmlUi APIs |

## What was built

- `third_party/rmlui_backends`: the SDL3 platform backend and the OpenGL 3 renderer of RmlUi 6.3, vendored with their
  MIT license and SHA-256 (README), built as `rv_rmlui_backends` with `RMLUI_SDL_VERSION_MAJOR=3` and
  `RMLUI_GL3_CUSTOM_LOADER=<glad/gl.h>`: one OpenGL loader in the process (our glad, loaded by `gl::GLDevice`), no
  duplicate symbols. Local changes: an output target (framebuffer and rectangle), an opt-out of the OpenGL state
  read-back, the sampler reset of units 0 and 1.
- `rv_ui_model` (GPU-free, `tests/unit/ui`, 39 test cases): `UiCommand` and `UiCommandList` (fixed capacity),
  `NavInput` (raw device input to commands: D-pad, left stick with hysteresis, arrows, Enter, Esc, A, B, X, Y;
  `DirectionRepeater`, `StickDirection`, `NavBindings`, suppression of held directions after `reset()`),
  `FocusRegions` (links between regions, the last focused element of each, entries), `FocusHistory` (Back along the
  path, one entry per region), `QuickCatalogModel` (filters, multi-word search, selection, revisions),
  `TimingSamples` (nearest-rank percentiles in a ring allocated once), `ui_assets.hpp` (font faces, the document's ids).
- `rv_ui`: `UiSystem` (RmlUi's lifetime; SDL system interface with the application's clock and RmlUi's log to spdlog;
  file interface on the asset root with UTF-8 paths; the renderer, counting its draws; fonts), `UiView` (one RmlUi
  context per output: dp ratio = `SDL_GetWindowDisplayScale` x text scale, event routing, `navigate()` and `activate()`
  through RmlUi's keys, update, render into the window and into screenshots, CPU times per part and GPU time),
  `QuickCatalogScreen` (data model `catalog`, regions, focus history, actions, hot reload of the document).
- `assets/ui`: `quick_catalog.rml` and `.rcss` (original design in the palette of style §3, Italian strings), Atkinson
  Hyperlegible Regular and Bold (SIL OFL 1.1, Braille Institute; `assets/registry`).
- `spikes/ui` (`spike_ui`): the screen over the test room through `app::SceneGraphics` (rv_app unchanged), a scripted
  scenario driving an SDL virtual gamepad, every focus change logged, timings per frame and phase, a JSON report
  (`--report`), screenshots at chosen frames (`--screenshot-at <frame>:<png>`, repeatable), `--fps-cap`,
  `--ui-scale`, `--high-contrast`, `--interactive`; ctest case `gpu.spike_ui.scenario` (label `gpu`).

### How input reaches the screen

```text
SDL event --> UiView::handleEvent: mouse, wheel, text, IME, keys that are not commands --> RmlUi context
          \-> platform::SdlInputCollector (pads by hot-plug, keys by scancode) --> input::RawInputFrame
              --> ui::NavInput (stick hysteresis, repeat) --> UiCommandList
              --> QuickCatalogScreen::apply: Up/Down/Left/Right -> UiView::navigate (RmlUi's arrow key: nav-* spatial
                  navigation inside the region's scroll container); nothing in that direction -> FocusRegions link ->
                  the target region's last focused element or entry
                  Activate -> a game: launch; else RmlUi's Enter (click, line break)
                  Back -> FocusHistory, else close;  Media, Info -> the selected game
```

The scripted runs use the same path: the scenario presses the buttons of an SDL virtual joystick, whose events arrive
with the next frame's poll (background joystick events are allowed for this run: a hidden window never has the focus).

## Method

- Scenario (`spikes/ui/src/scenario.cpp`), 1320 frames at a fixed 1/60 s step: focus down 10 times from the first game
  (`game-g11`), right into the card's actions (`act-play`, history 1), A (launch of `g11`), B (back on `game-g11`,
  history 0), Y (media of `g11`), X twice (details shown, hidden), the left stick held down for 1 s (7 steps:
  `game-g18`), 2 s of a stick resting at 0.47 to 0.52 (no step), a near-diagonal stick (one step, no turn), the D-pad
  held up for 2.7 s (faster repeat after 1.5 s, past the top of the list into the filters, then the search field,
  history 2), "faro" typed on the keyboard (1 game listed) and erased with Backspace (20), down into the filters, right
  twice, A on "8 bit" (3 games), down into the list (`game-g02`), up into the filters (they remember "8 bit"), left
  twice, A on "Tutte" (20), down (back on `game-g02`), the layout recorded at 1920 x 1080, after a resize to 1280 x 720
  and after resizing back, then the measurement phases: **idle** (frames 700 to 999, no input) and **navigation**
  (1000 to 1299, the D-pad held down, up, down for 100 frames each: 47 focus moves). Checks run a few frames after the
  input they verify; a failed check fails the run (exit code 3).
- Times per frame: CPU of the commands, `Context::Update`, the render (split into the renderer's `BeginFrame`,
  `Context::Render`, `EndFrame`), the draws, and the GPU time of the render (`GL_TIME_ELAPSED`, read frames later; a
  sample per frame in which a new result arrived). Percentiles by nearest rank over the steady phases (idle and
  navigation), also split into the frames in which the focus moved and the others.
- Release (`msvc-x64-release`, no GL debug context), hidden 1920 x 1080 window on the reference machine (RTX 2080
  SUPER, driver 617.42, display at 125%: dp ratio 1.25), `--no-vsync --fixed-dt 0.016667`, with `--fps-cap 60` (60 Hz
  pacing, as a vsync display) as the primary measurement and uncapped as a second one. Three runs per set; the machine's
  CPU load sampled every second (`Get-Counter '\Processore(_Total)\% Tempo processore'`: the Italian name of
  `\Processor(_Total)\% Processor Time`). Other agents were building and running the video and launch spikes on the same
  machine: the loads below are high, and one of them changed the display mode during set 1 (run 2 laid out at dp
  ratio 1.0, 98 draws instead of 86).
- Heap allocations of the UI pass counted in a Debug run with the debug CRT's allocation hook (main thread, our code and
  `rmlui.dll`; not the driver).

Commands (PowerShell, worktree root):

```powershell
./scripts/build.ps1 -Preset msvc-x64-release
build/msvc-x64-release/bin/spike_ui.exe --exit-after-frames --frames 1320 --hidden --no-vsync --fixed-dt 0.016667 `
  --fps-cap 60 --data-dir build/measure/data --report build/measure/report.json
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike_ui    # the scenario as a ctest case
```

## Results

### UI pass, final code (Release, 1920 x 1080, dp ratio 1.25, milliseconds)

Median of three runs (best run in brackets), steady phases, 60 Hz pacing. Set 3 ran the committed code; sets 1 and 2
ran the revision before it, which differed only in how the list refreshes after a filter or a search (outside the
steady phases):

| Set (CPU load during the runs) | CPU p50 | CPU p99 | GPU p50 | GPU p99 | Focus moved: CPU p50 | Focus still: CPU p50, p99 |
|---|---|---|---|---|---|---|
| Set 1 (97%, 85%, 77%) | 0.287 (0.219) | 3.989 (3.524) | 0.098 (0.088) | 0.793 (0.747) | 1.461 (1.273) | 0.274 (0.216), 1.709 (0.379) |
| Set 2 (53%, 95%, 86%) | 0.242 (0.191) | 3.312 (2.565) | 0.115 (0.095) | 0.803 (0.727) | 1.368 (0.778) | 0.236 (0.187), 2.107 (0.836) |
| Set 3 (53%, 64%, 57%) | 0.205 (0.203) | 2.797 (2.400) | 0.111 (0.111) | 0.791 (0.790) | 1.297 (1.231) | 0.200 (0.199), 0.873 (0.812) |

Uncapped (set 2, loads 61%, 79%, 98%; frame p50 about 3 ms, GPU-bound): CPU p50 0.206 (0.187), p99 3.793 (1.032), GPU
p50 0.092 (0.092), p99 0.440 (0.104); focus moved CPU p50 1.110 (0.752).

The best run of set 2 (53% load), per part at p50: commands 0.001 ms, update 0.04 ms, render 0.15 ms (`BeginFrame`
0.002, `Context::Render` 0.14, `EndFrame` 0.002), 86 draws. Debug, for reference (one run): CPU p50 2.1 ms, GPU
0.09 ms.

- Steady state: the 1 ms budget holds with room to spare, CPU and GPU together about 0.3 ms.
- A focus move (47 in 600 frames) costs 1.3 to 1.5 ms of CPU at p50: RmlUi re-evaluates the bindings and styles of the
  changed elements and rebuilds their geometry (new OpenGL buffers per changed element). The CPU p99 of 2.4 to 4 ms is
  set by these frames and by the machine's load: the frames in which nothing moved stay at 0.4 to 1.2 ms at p99 in the
  five runs below 80% load, 1.7 to 16 ms in the four more loaded ones.
- GPU: 0.09 to 0.12 ms for the 1080p layer (MSAA 2x clear, the draws, resolve, composite). A GPU p50 of 0.3 to 0.67 ms
  in two runs of set 2 coincides with the video spike decoding and uploading on the same GPU.

### Three fixes measured along the way (single exploratory runs, 60 Hz pacing)

| Change | Before | After |
|---|---|---|
| No OpenGL state read-back in the renderer's `BeginFrame` (local change 2) | render CPU p50 0.94 ms, of which `BeginFrame` 0.64 ms (35 `glGet*`, each a round trip to NVIDIA's driver thread) | `BeginFrame` 0.002 ms, UI CPU p50 0.23 ms |
| No color transitions in the style sheet | navigation CPU p50 0.94 ms (60 ms `background-color`/`border-color` transitions re-mesh their elements every animated frame) | 0.23 ms |
| The card follows the selection once the focus rests (0.15 s) while it runs through the list | focus-moved CPU p50 3.7 ms (update 0.9, render 2.7: the card's text laid out again) | 1.3 ms; the card costs its 2.5 ms once, when the focus rests |

Pacing matters for the measurement itself: uncapped, the GPU-bound driver blocks the CPU in whichever OpenGL call finds
its queue full, sometimes one of the UI's (one run measured 2.5 ms of "render" that was waiting); and pacing before the
swap left the UI's commands unflushed during the wait, which put 12 ms bubbles into the GPU timer's p99. Frames are
paced at their start.

### Allocations (Debug, debug CRT hook)

| Frames | Update | Render |
|---|---|---|
| Nothing changed (p50, p99) | 5, 5 | 540, 540 |
| Focus moved (p50) | 1,126 | 623 |
| Card laid out again (max) | 10,514 | 2,243 |

All of it inside RmlUi (`Context::Update`, `Context::Render`): our code allocates nothing per frame in steady state
(fixed-capacity command list, reserved records, no formatting in the measured part; focus changes are logged after it).

### Checks

Every run (the three capped sets, the uncapped one, Debug): 39 checks passed, RmlUi's log clean (0 errors,
0 warnings), GL debug output clean in Debug (0 high, 0 medium, 0 low). The scenario is deterministic: 99 focus changes
at the same frames with `--fixed-dt 0.016667` or the default 1/60 s.

### Screenshots

Written by a Debug build (kept out of the repository, 1920 x 1080), the scenario's run and an interactive one:

```powershell
$s = "build/spikes/ui/screenshots"
build/msvc-x64-debug/bin/spike_ui.exe --exit-after-frames --frames 1320 --hidden --no-vsync `
  --data-dir build/spikes/ui/doc_data --report build/spikes/ui/doc_report.json `
  --screenshot-at "29:$s/01_start.png" --screenshot-at "112:$s/02_down10.png" --screenshot-at "120:$s/03_actions.png" `
  --screenshot-at "130:$s/04_launched.png" --screenshot-at "158:$s/05_info_media.png" `
  --screenshot-at "568:$s/06_search.png" --screenshot-at "600:$s/07_filtered.png" `
  --screenshot-at "682:$s/08_resized_1280x720.png" --screenshot-at "696:$s/09_resized_back.png"
build/msvc-x64-debug/bin/spike_ui.exe --interactive --high-contrast --ui-scale 1.2 --exit-after-frames --frames 30 `
  --hidden --data-dir build/spikes/ui/doc_data --screenshot-at "29:$s/10_high_contrast_scale_1.2.png"
```

| File | Frame | Shows |
|---|---|---|
| `01_start.png` | 29 | the first game focused, its card |
| `02_down10.png` | 112 | after ten downs: "La Torre degli Orologi" focused at the bottom of the scrolled list |
| `03_actions.png` | 120 | "Avvia" focused (tungsten), the game kept as the selected row |
| `04_launched.png` | 130 | the status line of the launch |
| `05_info_media.png` | 158 | Informazioni: the whole description, publisher and year; the status line of Vedi media |
| `06_search.png` | 568 | the search field focused, caret shown |
| `07_filtered.png` | 600 | "8 bit" active: three games, the selection moved to the first |
| `08_resized_1280x720.png` | 682 | 1280 x 720 at dp ratio 1.25 (the compact layout below 760 dp), scaled into the screenshot |
| `09_resized_back.png` | 696 | back at 1920 x 1080, the same layout as before |
| `10_high_contrast_scale_1.2.png` | 29 | `--high-contrast --ui-scale 1.2`: black panels, yellow focus, larger text |

## Effort

One session. About 2,800 lines in `src/ui` (model 1,100, GPU side 1,700), 880 lines of unit tests, 2,100 lines of
spike, 470 lines of RML and RCSS, 60 lines of local changes to the backends. Everything else is RmlUi's documented API
(data models, event listeners, `nav-*` properties, media queries, `dp` units).

## Pitfalls

1. **Sampler objects.** Our material registry leaves trilinear sampler objects on texture units 0 to 4; RmlUi samples
   its textures (no mipmaps) on unit 0 with their own parameters, so they were incomplete and the whole UI read as
   opaque black. The renderer now unbinds the samplers of units 0 and 1 (local change 3).
2. **State read-back.** Upstream's `BeginFrame` queries 35 states to restore them afterwards: 0.6 ms of CPU on NVIDIA's
   threaded driver, more than the rest of the pass. Retroverse already invalidates its state cache after third-party
   renderers; the renderer leaves the state at OpenGL's initial values instead (local change 2).
3. **Output target.** Upstream composites into the default framebuffer only; screenshots need another target and a
   uniform scale (local change 1).
4. **No user-agent style sheet.** The body must be sized (`width: 100%; height: 100%`) or percentage layouts collapse
   to nothing; `div` and `p` are inline until declared block.
5. **`data-for` and shrinking arrays.** Views on the repeated element itself are updated in the same pass as the
   `data-for` view (same depth, unordered), so when the list shrinks RmlUi evaluates bindings of removed rows and logs
   "Data array index out of bounds". The bindings sit one level below the repeated element.
6. **Focus events bubble down the chain.** RmlUi sends `focus` to every element of the new focus chain, ancestors
   included, before it updates `GetFocusElement()`; the screen only takes elements with `tab-index: auto`.
7. **Spatial navigation stops at scroll containers.** It searches only inside the nearest scroll container of the
   focused element. Used as a feature: each region is a scroll container (`overflow: hidden` or `auto`), the links
   between regions are the screen's.
8. **Clicks focus text.** Every element takes the focus on a click unless `focus: none`; text and decorations are
   `focus: none`, so a click lands on the game, chip or button around it.
9. **Color transitions re-mesh.** RmlUi 6.3 bakes colors into geometry: a color animation rebuilds its elements' meshes
   every frame. The product style sheets use instant focus changes.
10. **Per-frame allocations inside RmlUi.** About 540 small allocations per idle frame in `Context::Render` and 5 in
    `Context::Update` (Debug count); harmless at 0.2 ms per frame, not traced to their call sites yet.
11. **Background joystick events.** `SdlPlatform` drops pad events while no Retroverse window has the focus, which a
    hidden automated run never has: the spike allows them for its scripted pad only.
12. **A display that changes under the run.** Other programs switching display modes changed the window's display scale
    and size mid-run; the resize comparison is skipped (not failed) when the drawable or the dp ratio cannot match.
13. **Test registration.** The top-level `CMakeLists.txt` adds `spikes/` before `enable_testing()`, so `add_test` there
    was silently ignored; `spikes/ui` enables testing for itself and for `spikes/` (to be fixed at the top level).

## Follow-ups

- A virtualized list for collections of thousands of games (a window of rows bound to a slice of the model) and the
  card's settle delay tuned on a TV (0.15 s now).
- An on-screen keyboard for text entry with the pad (I7); the hint bar from the active bindings (BACKLOG "Hints that
  name keys").
- Localization: the documents' Italian strings into `assets/ui/locale` tables bound through the data model.
- Covers and video frames in documents: an image source scheme (`rv-texture:<key>`) mapped to `gl::TextureCache`
  textures by the render interface.
- Profile the per-change cost (Tracy: `RMLUI_TRACY_PROFILING` is off in the vcpkg build) and RmlUi's per-frame
  allocations; propose the three renderer options upstream.
- A pooled vertex buffer in the renderer instead of new buffers per compiled geometry, if focus moves must get under
  1 ms.
