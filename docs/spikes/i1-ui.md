# I1 spike (d): product UI with RmlUi and the gamepad

Outcome: **RmlUi adopted** (ADR-0005), with one criterion met only in part. The quick catalog mock screen works with the
pad alone (text entry aside: the search field takes typed text until the on-screen keyboard of I7), its focus is always
visible and never jitters, and resizing and text scaling lay it out correctly. The UI pass at 1920 x 1080 costs 0.21 to
0.24 ms of CPU and 0.09 to 0.11 ms of GPU per frame in which the UI does not change (p50; 0.49 ms of CPU when the driver
runs in its slower mode), under the 1 ms budget. Frames in which the UI changes exceed it: a move of the focus while the
D-pad is held costs 1.2 to 1.5 ms of CPU and a single tap, which lays the card out again in the same frame, 3.0 to 4.2 ms
(p50). Built in one session; a second one addressed the review (this revision).

## Question and criteria

From `docs/roadmap.md` (I1, row d): can RmlUi 6.3, on SDL3 and our OpenGL 4.5 context, carry the product screens
(specification §13.1) with a controller first? Success criteria: a quick-catalog mock screen usable with the pad alone,
a stable focus, the UI pass under 1 ms (at 1080p), correct resizing; the approved I1 plan (not in the repository) adds a
reasonable integration effort, "within two sessions", and the fallback: the product UI in Dear ImGui with pad navigation
behind the same view models.

| Criterion | Verdict | Evidence |
|---|---|---|
| A quick-catalog mock screen usable with the pad alone | met, text entry aside | every pad-driven check of the scenario passes in every run (focus, actions, history, filters, the card, the chips row); the search field is reached with the pad, its text is typed on the keyboard (the `keyboard-*` checks, kept out of this verdict) until the on-screen keyboard (I7) |
| Stable, visible focus | met | a stick resting at 0.47 to 0.52 for 2 s moves nothing; one crossing the 0.55 threshold every frame steps at the held cadence (2 steps in 0.47 s), not on every crossing; a near-diagonal stick steps once and never turns; `:focus` styles on every focusable element; the list and the chips row scroll the focus into view |
| UI pass under 1 ms at 1080p | met for quiet frames, **not met for frames with changes** | quiet frames: CPU p50 0.21 to 0.24 ms (fast driver mode, 10 runs), 0.49 ms (slow mode, 1 run; 0.43 to 0.71 ms in 7 earlier runs), GPU p50 0.09 to 0.11 ms; a held D-pad move 1.23 to 1.48 ms CPU p50 (0.78 ms slow mode); a single tap 3.0 to 4.2 ms (1.76 ms slow mode) |
| Correct resizing | met | 1920 x 1080 -> 1280 x 720 -> 1920 x 1080: RmlUi's context and document follow the window's drawable at the display's density, every panel inside the view, the card's buttons whole, the focused game visible, the layout back at 1080p equal to the first to 0.5 px |
| Reasonable integration effort | met, at the limit | two sessions (the spike, then the review fixes); 3 local changes (about 60 lines) to the vendored renderer, everything else on documented RmlUi APIs |

The accessibility settings of specification §12.4, checked by the scenario too: text scale x1.2 makes every text 1.2 times
larger in the same layout (it used to switch to the compact layout and cancel itself, see Pitfalls), high contrast puts
opaque black panels and a yellow focus on screen.

## What was built

- `third_party/rmlui_backends`: the SDL3 platform backend and the OpenGL 3 renderer of RmlUi 6.3, vendored with their
  MIT license and SHA-256 (README), built as `rv_rmlui_backends` with `RMLUI_SDL_VERSION_MAJOR=3` and
  `RMLUI_GL3_CUSTOM_LOADER=<glad/gl.h>`: one OpenGL loader in the process (our glad, loaded by `gl::GLDevice`), no
  duplicate symbols. Local changes: an output target (framebuffer and rectangle), an opt-out of the OpenGL state
  read-back, the sampler reset of units 0 and 1.
- `rv_ui_model` (GPU-free, `tests/unit/ui`, 41 test cases): `UiCommand` and `UiCommandList` (fixed capacity),
  `NavInput` (raw device input to commands: D-pad, left stick with hysteresis, arrows, Enter, Esc, A, B, X, Y;
  `DirectionRepeater`, `StickDirection`, `NavBindings`, suppression of held directions after `reset()`),
  `FocusRegions` (links between regions, the last focused element of each, entries), `FocusHistory` (Back along the
  path, one entry per region), `QuickCatalogModel` (filters, multi-word search, selection, revisions of the filter, of
  the visible games and of the selection), `TimingSamples` (nearest-rank percentiles in a ring allocated once),
  `ui_assets.hpp` (font faces, the document's ids and classes, the compact layout theme and `compactLayout()`).
- `rv_ui`: `UiSystem` (RmlUi's lifetime; SDL system interface with the application's clock and RmlUi's log to spdlog;
  file interface on the asset root with UTF-8 paths; the renderer, counting its draws; fonts), `UiView` (one RmlUi
  context per output: dp ratio = `SDL_GetWindowDisplayScale` x text scale, the compact theme from the display's
  density alone, event routing, `navigate()` and `activate()` through RmlUi's keys, update, render into the window and
  into screenshots, CPU times per part and GPU time), `QuickCatalogScreen` (data model `catalog`, regions, focus
  history, actions, the card that follows the focus, the fades of the chips row, hot reload of the document).
- `assets/ui`: `quick_catalog.rml` and `.rcss` (original design in the palette of style §3, Italian strings), Atkinson
  Hyperlegible Regular and Bold (SIL OFL 1.1, Braille Institute; `assets/registry`).
- `spikes/ui` (`spike_ui`): the screen over the test room through `app::SceneGraphics` (rv_app unchanged), a scripted
  scenario driving an SDL virtual gamepad (real pads are ignored in scripted runs), every focus change logged, timings
  per frame and phase, heap allocations per frame (`AllocationCounter`), a JSON report (`--report`), screenshots at
  chosen frames (`--screenshot-at <frame>:<png>`, repeatable), `--fps-cap`, `--ui-scale`, `--high-contrast`,
  `--interactive`; ctest case `gpu.spike_ui.scenario` (label `gpu`, a `--smoke` run).

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
with the next frame's poll (background joystick events are allowed for this run: a hidden window never has the focus);
the events of every other pad are dropped, so that a pad in use on the machine cannot move the focus under the checks.

`NavInput` reads the raw device input through its own table (`NavBindings::defaults()`, the defaults of specification
§12.1), not through `rv::input`'s action map, so a user's `bindings.json` does not reach the product UI yet; ADR-0005
records the decision and the plan to build `NavBindings` from UI actions of the action map.

## Method

- Scenario (`spikes/ui/src/scenario.cpp`), 1910 frames at a fixed 1/60 s step: focus down 10 times from the first game
  (`game-g11`), right into the card's actions (`act-play`, history 1; the card shows `g11` in that frame), A (launch of
  `g11`), B (back on `game-g11`, history 0), Y (media of `g11`), X twice (details shown, hidden), the left stick held
  down for 1 s (7 steps: `game-g18`), 2 s of a stick resting at 0.47 to 0.52 (no step), a near-diagonal stick (one
  step, no turn), the D-pad held up for 2.7 s (faster repeat after 1.5 s, past the top of the list into the filters,
  then the search field, history 2), "faro" typed on the keyboard (1 game listed) and erased with Backspace (20), down
  into the filters, right twice, A on "8 bit" (3 games), down into the list (`game-g02`), up into the filters (they
  remember "8 bit"), left twice, A on "Tutte" (20), down (back on `game-g02`), the layout recorded at 1920 x 1080, after
  a resize to 1280 x 720 and after resizing back; then (frames 700 to 922) a stick crossing its engage threshold every
  frame for 0.47 s (2 steps), up into the filters and right to the last chip (scrolled into view, the fades at the
  row's edges where chips hide), "faro" typed again and A on "16 bit", which keeps the same game listed (the highlight
  must move to that chip) and back on "Tutte", the layout at text scale x1, x1.2 and x1 again, high contrast on and
  off. Then the measurement phases: **idle** (frames 930 to 1229, no input), **taps** (1230 to 1589, 20 single D-pad
  taps 0.3 s apart, down and up between two games: each moves the focus and lays the card out again in the same frame)
  and **navigation** (1590 to 1889, the D-pad held down, up, down for 100 frames each: 49 focus moves, the card follows
  once the focus rests). 80 checks run a few frames after the input they verify; a failed check, a check the run ends
  before, or an RmlUi error or warning fails the run (exit code 3).
- Times per frame: CPU of the commands, `Context::Update` with the screen's after-update work, the render (split into
  the renderer's `BeginFrame`, `Context::Render`, `EndFrame`), the draws, and the GPU time of the render
  (`GL_TIME_ELAPSED`, read frames later: a sample in each frame in which the latest result changed, so a result equal to
  the previous one, or followed by another within one frame, is not recorded: 2 to 8% of the idle frames have no
  sample, which leaves the percentiles of a series that steady practically unchanged). Frames are **quiet** when the UI
  did not change, and **with changes** when the focus moved or the card was laid out again (in the frame that lays it
  out: the card settling after an update counts for the next frame). Percentiles by nearest rank: the quiet frames of
  the three measurement phases, and the frames with changes of the taps and of the held D-pad.
- Heap allocations per frame (`spikes/ui/src/allocation_counter.cpp`, Release and Debug): the import tables of
  `spike_ui.exe` (our code and the vendored backends) and `rmlui.dll` point the CRT's allocation functions (malloc,
  calloc, realloc and the debug and aligned forms; `operator new` calls them) at counting functions while the run
  lasts; the main thread is counted between the start of the commands and the end of the update, and around the
  render. The driver's, SDL's and FreeType's allocations are not seen.
- Release (`msvc-x64-release`, no GL debug context), hidden 1920 x 1080 window on the reference machine (RTX 2080
  SUPER, driver 617.42, display at 125%: dp ratio 1.25), `--no-vsync --fixed-dt 0.016667`, the code of this revision.
  Primary measurement: `--fps-cap 60` (60 Hz pacing, as a vsync display), two sets of six runs; second measurement:
  uncapped, four runs. The machine's CPU load was sampled every second during each run (`Get-Counter
  '\Processore(_Total)\% Tempo processore'`, the Italian name of `\Processor(_Total)\% Processor Time`). The video and
  launch spikes of I1 and their builds and tests ran on the same machine and GPU throughout: mean loads of 37 to 93%,
  peaks of 67 to 100%; the launch spike switched the display mode during one paced run (7 display scale changes, left
  out: `displayScaleChanges` in the report). The tables give the minimum, median and maximum over the runs, by driver
  mode (below).
- Driver modes. The CPU cost of the renderer's calls falls in two clusters that a run keeps from start to end: **fast**,
  `BeginFrame` about 0.002 ms, `Context::Render` 0.15 ms, `EndFrame` 0.0015 ms at p50, and **slow**, about 0.02, 0.35
  and 0.05 ms. 1 of the 16 runs of this revision ran slow, and 7 of the 37 Release runs of the first revision (their
  reports re-read for this review). The application does not choose it; it matches NVIDIA's threaded optimization
  ("Auto" by default) deciding per process whether OpenGL calls run on a driver thread (fast calls, slow read-backs and
  object creation) or on the calling thread. That explanation is a hypothesis: the spike cannot set the driver's profile,
  and forcing the setting either way in the NVIDIA control panel was not tried. The mode of a run shows in its report
  (`timings.idle.beginFrameCpuMs`).

Commands (PowerShell, worktree root):

```powershell
./scripts/build.ps1 -Preset msvc-x64-release
build/msvc-x64-release/bin/spike_ui.exe --exit-after-frames --frames 1910 --hidden --no-vsync --fixed-dt 0.016667 `
  --fps-cap 60 --data-dir build/measure/data --report build/measure/report.json
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike_ui    # the scenario as a ctest case (--smoke)
```

## Results

### UI pass (Release, 1920 x 1080, dp ratio 1.25, 60 Hz pacing, milliseconds)

Fast driver mode, 10 runs (minimum, median, maximum over the runs of each run's value):

| Frames | CPU p50 | CPU p99 | CPU max | GPU p50 | GPU p99 |
|---|---|---|---|---|---|
| Quiet (891 per run) | 0.206, 0.219, 0.243 | 0.433, 0.625, 1.145 | | 0.090, 0.110, 0.113 | 0.342, 0.793, 0.816 |
| Single tap, card laid out (20 per run) | 3.01, 3.37, 4.20 | | 4.08, 5.12, 523 | | |
| Held D-pad move (49 per run) | 1.23, 1.29, 30.3 | 3.27, 6.29, 750 | | | |

Slow driver mode, 1 run (load 59%): quiet CPU p50 0.493, p99 1.508, GPU p50 0.090; a tap 1.760 (max 3.795), a held move
0.776 (p99 2.276).

Per part, quiet frames at p50, fast mode: commands under 0.001 ms, update 0.043 to 0.055 ms, render 0.15 ms (`BeginFrame`
0.002, `Context::Render` 0.147 to 0.182, `EndFrame` 0.002), 87 draws. A tap: update 0.80 to 0.99 ms, render 1.94 to
3.10 ms (fast mode); 0.87 and 0.80 ms in the slow one. Uncapped (fast mode, 4 runs, loads 51 to 64%, frame p50 3.1 to
4.5 ms, GPU-bound): quiet CPU p50 0.183 to 0.206, a tap 2.89 to 3.09, a held move 3.72 to 5.70 (the driver blocks the
CPU in whichever call finds its queue full, and a move creates buffers). Debug, for reference (one run): quiet CPU p50
1.93 ms, a tap 18.9 ms, GPU 0.09 ms.

- Quiet frames: the 1 ms budget holds with room to spare in both driver modes, CPU and GPU together 0.3 to 0.6 ms.
- Frames with changes exceed it. RmlUi re-evaluates the bindings and styles of what changed and builds new geometry
  (new OpenGL buffers per compiled geometry). A held move (the `selected` and `:focus` styles of two rows, the bound
  games array) costs 1.2 to 1.5 ms. A tap also refills the card: its text is laid out again (update 0.8 to 1.0 ms) and
  its glyphs rebuilt into new buffers (render 1.9 to 3.1 ms), 3.0 to 4.2 ms in all. The slow driver mode halves the
  cost of a change (buffer creation runs on the calling thread instead of waiting for the driver's) while it doubles
  that of a quiet frame. Either way a change stays within a 60 Hz frame (the test room's whole frame takes about 2.9 ms
  at 1080p, CLAUDE.md), and changes come with input, a few times per second at most.
- GPU: 0.09 to 0.11 ms for the 1080p layer (MSAA 2x clear, the draws, resolve, composite), independent of changes.
- Outliers, under the other spikes' load: in run 1 of the first set a held move took 30 ms at p50 and 750 ms at
  worst, a tap up to 523 ms (load 63%, peaks of 100%, the video spike decoding on the same GPU); in the first revision's
  set 2 run 3, frames with a focus move took 17.4 ms at p50 and the CPU p99 of the steady phases was 86.8 ms (431 ms at
  worst). Neither reproduced in the other runs; both are kept in the minimum, median and maximum above.

### Three fixes measured along the way (first revision, single exploratory runs, 60 Hz pacing)

| Change | Before | After |
|---|---|---|
| No OpenGL state read-back in the renderer's `BeginFrame` (local change 2) | render CPU p50 0.94 ms, of which `BeginFrame` 0.64 ms (35 `glGet*`, each a round trip to NVIDIA's driver thread) | `BeginFrame` 0.002 ms, UI CPU p50 0.23 ms |
| No color transitions in the style sheet | navigation CPU p50 0.94 ms (60 ms `background-color`/`border-color` transitions re-mesh their elements every animated frame) | 0.23 ms |
| The card follows the selection once the focus rests (0.15 s) while it runs through the list | focus-moved CPU p50 3.7 ms (update 0.9, render 2.7: the card's text laid out again) | 1.3 ms; a single tap still lays the card out in its own frame (3.0 to 4.2 ms above) |

Pacing matters for the measurement itself: uncapped, the GPU-bound driver blocks the CPU in whichever OpenGL call finds
its queue full, sometimes one of the UI's; and pacing before the swap left the UI's commands unflushed during the wait,
which put 12 ms bubbles into the GPU timer's p99. Frames are paced at their start.

### Allocations (per frame, main thread: Retroverse's code + RmlUi; update + render)

| Frames | Release | Debug |
|---|---|---|
| Quiet (every one) | 0 + 0, and RmlUi 0 + 3 | 0 + 0, and RmlUi 5 + 554 |
| A held move (p50) | 0 + 3, and RmlUi 116 + 19 | 15 + 3, and RmlUi 1,111 + 634 |
| A tap, the card laid out again | 0 + 20, and RmlUi 2,625 + 354 | 22 + 20, and RmlUi 10,532 + 2,045 |

The same counts in every run. In Release RmlUi allocates 3 times per quiet frame, in `Context::Render`; the Debug counts
are inflated by the debug iterators' container proxies (one per `std::vector` or `std::string` made, in RmlUi and in our
code alike). Retroverse's code allocates nothing in Release in any measured frame, the vendored renderer one
`CompiledGeometryData` per new geometry (the 3 and 20 of the render column); the fixed-capacity command list, the
reserved records and the logging after the measured part keep the spike's own work out of it.

### Checks

Every run (the 16 Release runs, the Debug runs): 80 checks passed (none skipped, none left unrun), RmlUi's log clean (0
errors, 0 warnings), GL debug output clean in Debug (0 high, 0 medium, 0 low; the ctest case is a `--smoke` run, which
fails on a high-severity message). The scenario is deterministic: 140 focus changes at the same frames in every run.
Two of the checks added for the review were also run against the code before their fix (reverted for the test), and
failed as they should: the card at frame 112 still showed "Corsa sul Lungomare" with Avvia focused under "La Torre
degli Orologi", and after A on "16 bit" the highlight stayed on "Tutte". The check of the last chip found the chips row
that never scrolled (pitfall 8) on its first run.

### Screenshots

Written by a Debug build (kept out of the repository, 1920 x 1080), the scenario's run:

```powershell
$s = "build/spikes/ui/screenshots"
build/msvc-x64-debug/bin/spike_ui.exe --smoke --frames 1910 --hidden --no-vsync `
  --data-dir build/spikes/ui/doc_data --report build/spikes/ui/doc_report.json `
  --screenshot-at "29:$s/01_start.png" --screenshot-at "108:$s/02_down10.png" --screenshot-at "112:$s/03_actions.png" `
  --screenshot-at "130:$s/04_launched.png" --screenshot-at "158:$s/05_info_media.png" `
  --screenshot-at "568:$s/06_search.png" --screenshot-at "600:$s/07_filtered.png" `
  --screenshot-at "682:$s/08_resized_1280x720.png" --screenshot-at "696:$s/09_resized_back.png" `
  --screenshot-at "806:$s/10_last_chip.png" --screenshot-at "908:$s/11_text_scale_1.2.png" `
  --screenshot-at "918:$s/12_high_contrast.png"
```

| File | Frame | Shows |
|---|---|---|
| `01_start.png` | 29 | the first game focused, its card |
| `02_down10.png` | 108 | after ten downs 8 frames apart: "La Torre degli Orologi" focused at the bottom of the scrolled list; the card still shows "Corsa sul Lungomare", the game of the first move: it follows once the focus rests for 0.15 s |
| `03_actions.png` | 112 | "Avvia" focused (tungsten), the card filled with "La Torre degli Orologi" as the focus left the list |
| `04_launched.png` | 130 | the status line of the launch |
| `05_info_media.png` | 158 | Informazioni: the whole description, publisher and year; the status line of Vedi media |
| `06_search.png` | 568 | the search field focused, caret shown |
| `07_filtered.png` | 600 | "8 bit" active: three games, the selection moved to the first |
| `08_resized_1280x720.png` | 682 | 1280 x 720 at dp ratio 1.25 (576 dp of display: the compact theme), scaled into the screenshot; the chips row fades where "32 bit" is cut |
| `09_resized_back.png` | 696 | back at 1920 x 1080, the same layout as before |
| `10_last_chip.png` | 806 | "Portatile", the last chip, focused and scrolled into view; the row fades where "Tutte" is cut |
| `11_text_scale_1.2.png` | 908 | text scale x1.2: every text 1.2 times larger in the regular layout, three description lines, "Informazioni" wrapped to a second row of buttons |
| `12_high_contrast.png` | 918 | high contrast: opaque black panels, the focused game in yellow |

## Effort

Two sessions: the spike, then the fixes of its review. About 2,900 lines in `src/ui` (model 1,250, GPU side 1,680),
930 lines of unit tests, 2,900 lines of spike, 520 lines of RML and RCSS, 60 lines of local changes to the backends.
Everything else is RmlUi's documented API (data models, event listeners, `nav-*` properties, media queries and themes,
`dp` units).

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
   focused element. Used as a feature: each region is a scroll container, the links between regions are the screen's.
8. **`ScrollIntoView` passes over `overflow: hidden`.** RmlUi scrolls only `auto` and `scroll` containers to bring an
   element into view, so the chips row, a hidden region, kept its focused last chip clipped (the first revision never
   walked that far). The regions that must follow the focus are `overflow-x: auto` (the list `overflow-y: auto`) with
   no visible scroll bar; the card's buttons, a hidden region, wrap to a second row instead of overflowing.
9. **Text scale and dp media queries.** The dp ratio includes the text scale, so a dp height query sees 1920 x 1080 at
   125% with text scale 1.2 as 720 dp tall and swaps in the compact layout, whose sizes are about 0.8 times the regular
   ones: text scaling cancelled itself. `UiView` activates an RmlUi theme from the display's density alone
   (`@media (theme: compact)`).
10. **Clicks focus text.** Every element takes the focus on a click unless `focus: none`; text and decorations are
    `focus: none`, so a click lands on the game, chip or button around it.
11. **Color transitions re-mesh.** RmlUi 6.3 bakes colors into geometry: a color animation rebuilds its elements' meshes
    every frame. The product style sheets use instant focus changes.
12. **Deferred work must settle before it is acted on.** The card skips quick moves; the focus entering the card's
    buttons (Avvia launches the selected game) or an action fills it at once, or Avvia would show one game and launch
    another for up to 0.15 s.
13. **Per-frame allocations inside RmlUi.** 3 per quiet frame in Release, in `Context::Render`, not traced to their
    call sites; the 554 of a Debug frame are mostly the debug iterators' proxies.
14. **Two driver modes.** The same binary runs its OpenGL calls about 2.3 times slower on the CPU in some runs (1 in 15
    here, 7 in 37 before), cheaper for buffer creation; compare runs of the same mode (`BeginFrame` at p50).
15. **Background joystick events.** `SdlPlatform` drops pad events while no Retroverse window has the focus, which a
    hidden automated run never has: the spike allows them, and hears its virtual pad only.
16. **A display that changes under the run.** Other programs switching display modes changed the window's display scale
    and size mid-run; the layout and text-scale comparisons are skipped (not failed) when the drawable or the display
    scale differ, and measurement runs with a change are left out (`displayScaleChanges`).
17. **Test registration.** The top-level `CMakeLists.txt` adds `spikes/` before `enable_testing()`, so `add_test` there
    is silently ignored; `spikes/ui` enables testing for itself and, through `CMAKE_TESTING_ENABLED` in the parent
    scope, for `spikes/` and the spike directories added after it (launch) but not before it (video). Fixed at the
    merge of the spikes: the top level enables testing before it adds any directory, and `spikes/ui` no longer enables
    it for itself.

## Follow-ups

- The cost of a change, to get frames with changes under 1 ms: new geometry into a pooled vertex buffer in the renderer
  instead of new OpenGL buffers per compiled geometry (the fast driver mode pays for every creation), the card in a
  document of its own (a change of layout formats the whole document from its root, `ElementDocument::UpdateLayout`),
  the selected row marked by a class set on two elements instead of re-evaluating the bound games array; then measure
  again with Tracy (`RMLUI_TRACY_PROFILING` is off in the vcpkg build). Trace RmlUi's 3 allocations per frame.
- A virtualized list for collections of thousands of games (a window of rows bound to a slice of the model) and the
  card's settle delay tuned on a TV (0.15 s now).
- UI actions in the action map: `NavBindings` built from `ui_*` actions of `assets/config/default_bindings.json`, so
  that user bindings reach the product UI (ADR-0005).
- An on-screen keyboard for text entry with the pad (I7); the hint bar from the active bindings (BACKLOG "Hints that
  name keys").
- Localization: the documents' Italian strings into `assets/ui/locale` tables bound through the data model.
- Covers and video frames in documents: an image source scheme (`rv-texture:<key>`) mapped to `gl::TextureCache`
  textures by the render interface.
- Move `devui::fitOverlayCapture`, which `rv_ui` links `rv_devui_model` for, to a module below both overlays
  (`rv_core` or `rv_render`); a result count in `gl::GpuTimer`, so that GPU samples are recorded per result instead of
  per changed value. (Done at the merge of the I1 spikes: testing enabled before `add_subdirectory(spikes)`, pitfall
  17.)
- Propose the three renderer options upstream.
