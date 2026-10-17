# I2 step A1: the app shell for launching

Step A1 of [I2](../roadmap.md) (milestone M1): `retroverse.exe` gets the product UI and launches games. The quick
catalog of step 6 (`ui::QuickCatalogScreen` over a `ui::CatalogSource`) is shown over the scene below the developer
overlay, opened and closed by the QuickCatalog action; "Avvia" plans the selected game's launch with its emulator
profile (step 5) and starts it through `launcher::LaunchSession` on `platform_win::Win32ProcessRunner` with ADR-0006's
keep policy (`platform::WindowHandover`). While the emulator runs the host renders, updates and presents nothing: it
waits for events, woken by the runner through an SDL user event, and frames resume when the game returns. A
game-session panel ("Gioco in corso") asks the user when the session needs it (UserConfirmReturn, StillRunning, the
user back in Retroverse while the game runs) and tells the outcome (codes whose Italian text lives in
`assets/ui/game_session.rml`). Quitting while a game runs leaves it running. Until the ES-DE catalog reaches the
application (step 13) the catalog is a mock: `--mock-catalog <file.json>`. The run report records the catalog and every
launch.

## What was built

| File | What |
|---|---|
| `src/app/include/rv/app/app_logic.hpp`, `src/app/src/app_host.cpp`, `app_host.hpp` | The host's idle mode: `AppContext::setIdle`/`idle()`, `AppContext::wakeCallback()` (an SDL user event registered per run), `IAppLogic::onIdle`, `AppHost::kIdleWait` (100 ms) |
| `src/app/include/rv/app/input_context.hpp`, `src/input_context.cpp` | GPU-free: `InputContext` (World, Ui), `readsAction`, `actionsFor`, `anyUiActionPressed`, `catalogToggleRequested` |
| `src/app/include/rv/app/back_action.hpp`, `src/back_action.cpp` | Back levels with the product UI: `BackAction::PanelBack`, `CatalogBack`; `BackState::panelOpen`, `catalogOpen`; `toString(BackAction)` |
| `src/app/include/rv/app/mock_catalog.hpp`, `src/mock_catalog.cpp` | GPU-free: the mock catalog (`MockCatalog`, `MockGame`, `MockLaunch`, `parseMockCatalog`, `loadMockCatalog`, `mockCatalogProfiles`) |
| `src/app/include/rv/app/launch_outcome.hpp`, `src/launch_outcome.cpp` | GPU-free: `LaunchMessage` and `launch_outcome::k*` codes and reasons, `messageForOutcome`, `messageForBlocker`, `showsMessage`, `firstToken`, `exitCodeName` |
| `src/app/include/rv/app/launch_flow.hpp`, `src/launch_flow.cpp` | GPU-free: `LaunchFlow`, a launch from Avvia to the return around a `LaunchSession`; `SessionPanel`; `LaunchInfo` |
| `src/app/include/rv/app/game_session_ui.hpp` | GPU-free, header only: the panel document's paths, ids and data model |
| `src/app/include/rv/app/run_report.hpp`, `src/run_report.cpp` | `CatalogRecord`, `LaunchRecord`; `RunReport::catalog`, `RunReport::launches` (schema version 1: keys only added) |
| `src/app/include/rv/app/command_line.hpp`, `src/command_line.cpp` | `--mock-catalog <json>`, `--launch-wait-limit <seconds>`; the help's controls and exit codes |
| `src/app/src/product_ui.hpp`, `.cpp` | `ProductUi` (rv_app, private): RmlUi, the quick catalog, the panel, the launches, the window's hand-over |
| `src/app/src/game_session_panel.hpp`, `.cpp` | `GameSessionPanel` (rv_app, private): the panel's RmlUi document and data model |
| `src/app/src/retroverse_app.cpp`, `retroverse_app.hpp`, `app_input.hpp` | The composition: input contexts, Back levels, the overlay slot composed, the mouse between the layers, idle, the quit hook; `AppInput::releaseAll` |
| `src/app/CMakeLists.txt` | `rv_app_cli` links `rv_input`, `rv_launcher`, `rv_ui_model`; `rv_app` links `rv_launcher`, `rv_platform_win`, `rv_ui`, `rv_devui_model` |
| `assets/ui/game_session.rml`, `.rcss` | The game-session panel: phases, outcome codes and reasons, in Italian |
| `tests/smoke/catalogs/fake_emulator_games.json` | The mock catalog of the smoke tests: 13 fake-emulator profiles, 15 invented games |
| `tests/input_scripts/quick_catalog_launch.json`, `quick_catalog_launch_confirm.json`, `quick_catalog_launch_back.json` | The launch runs' scripts |
| `tests/smoke/test_launch.cpp`, `launch_support.hpp`, `.cpp` | The launch smoke tests (label gpu) and the visible case (label desktop) |
| `tests/smoke/CMakeLists.txt` | `RV_LAUNCH_ALLOWLIST` (ctest `ENVIRONMENT`) on every gpu case, the fake built with them, `desktop.smoke.launch_visible` (with `RV_SMOKE_DESKTOP=1`) |
| `tests/unit/app/test_{input_context,launch_outcome,launch_flow,mock_catalog,game_session_document}.cpp`, `test_back_action.cpp`, `test_run_report.cpp` | Unit tests |

## Running it

```powershell
# The smoke tests' catalog, with the fake emulator of the build (tests/smoke/test_launch.cpp writes it the same way):
# replace the profiles' "@FAKE_EMULATOR@" by build\msvc-x64-debug\bin\rv_fake_emulator.exe and create roms\*.gb.
$env:RV_LAUNCH_ALLOWLIST = (Resolve-Path build/msvc-x64-debug/bin/rv_fake_emulator.exe).Path
build/msvc-x64-debug/bin/retroverse.exe --mock-catalog build/a1/catalog.json `
  --input-script tests/input_scripts/quick_catalog_launch.json --exit-after-frames --frames 50 --hidden `
  --fixed-dt 0.016667 --data-dir build/rvdata --run-report build/a1/run.json
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter gpu.launch          # the gpu cases
./scripts/test.ps1 -Preset msvc-x64-debug-desktop -Filter launch_visible  # the visible case, on demand
```

The visible case is the Catch2 test "desktop: a visible launch hands the window over and takes it back", tagged
`[.desktop]` only (neither `launch*` nor `[launch]` matches it), and it skips unless `RV_SMOKE_DESKTOP=1`, which only
its ctest registration `desktop.smoke.launch_visible` sets: no Catch2 filter typed by hand can take over the screen.
On the desktop preset a ctest filter `launch` would also run every `desktop.launch.*` case of `spike_launch`, hence
`launch_visible` (corrected at the wave 2 merge).

Interactively: Tab (gamepad Y) opens the catalog, the arrows or the D-pad move, Enter (A) on a game launches it, Esc
(B) walks back out. A user's `bindings.json` rebinds all of it.

## The mock catalog (`--mock-catalog`)

A JSON file with comments, `schemaVersion` 1 (`rv/app/mock_catalog.hpp` has the format):

- `games`: the catalog's entries in order (`ui::CatalogEntry` fields: `id`, `title` required; `platform`, `year`,
  `developer`, `publisher`, `genre`, `players`, `description`, `state` optional), each with an optional `launch`:
  `profile` (an emulator profile's id), `rom` (absolute, or relative to the catalog file's folder), `system` (the ES-DE
  system name), `variables` (the launch's `{var:NAME}` values).
- `profiles`: optional emulator profiles in step 5's format, over the shipped and the user's profiles
  (`<asset root>/launcher/emulators.json`, `<user data>/config/emulators.json`): a profile with the same id replaces
  theirs.
- Unknown fields are errors at every level; ids must be catalog ids (`ui::isValidEntryId`) and unique. A catalog that
  cannot be loaded fails the initialization (exit code 2); a broken profile is logged and left out (its games then say
  "not configured, unknown profile").

The games are an `ui::InMemoryCatalogSource`; the catalog's screen is created the first time the catalog opens (a
screen created hidden never lays its pool of rows out: below). Runs without `--mock-catalog` have no product UI at
all: QuickCatalog logs once that there is no catalog, and nothing else changes (golden G01's runs).

## Input contexts and Back

`InputContext` (`rv/app/input_context.hpp`) decides who reads a frame's actions, from the state the frame began in:

| Context | When | Reads |
|---|---|---|
| World | the catalog and the panel are closed | every action but the UI ones (walking, looking, Back, QuickCatalog opens the catalog) |
| Ui | the catalog or the panel is open | the UI actions (`ui_*`, through `ui::NavInput`) and QuickCatalog (closes the catalog) |

ToggleDevUI, ReloadShaders and Screenshot are read in both. `actionsFor()` releases the actions a context does not
read, so the walker stands still under the catalog (FreeRoam gets the World view of the frame, zeros in Ui) and the
catalog never scrolls while the player walks. The frame that opens the catalog gives it no command (its Y press is no
"Vedi media"); `NavInput::reset()` on opening keeps a stick still held from walking from scrolling it.

QuickCatalog toggles: in World it opens the catalog; in Ui it closes it unless a UI action was pressed in the same
frame (`catalogToggleRequested`). With the default bindings Tab opens and closes the catalog, while the pad's Y, which
is also `ui_media`, opens it and is "Vedi media" inside it; B and Esc leave it through Back. A user binding of
QuickCatalog to a button no UI action uses toggles like Tab.

Events go to the developer overlay first, then to the product UI while it has the input (`UiView::handleEvent`: the
mouse, text, keys that are not UI commands), then to the game; a click the UI did not use never captures the mouse
while the catalog is open. The mouse moves between first-person look and the layers over the scene with one
`devui::CaptureHandover` (`syncCaptureLayers`): the first layer to open (the overlay, the catalog) frees a captured
mouse, the last to close captures it again when it freed it. The product UI always needs the cursor: opening it while
the overlay shows frees a mouse that a click into the scene captured again meanwhile (given back when the last layer
closes), so the catalog never opens under a hidden, captured cursor.

Back (`backAction`, `rv/app/back_action.hpp`) leaves one level at a time: the overlay's field (ignored), the overlay's
popup, the game-session panel (`PanelBack`: back to the game while a session runs, the outcome dismissed after it), the
catalog (`CatalogBack`: the screen's Back, along its focus history, then it closes), the captured mouse, the overlay,
quit. In World Back is the `back` action; in Ui the screen's Back command (`ui_back`) asks the same levels.

## Launching: from Avvia to the return

`LaunchFlow` (`rv/app/launch_flow.hpp`, GPU-free) drives one launch around a `LaunchSession`; `ProductUi` gives it the
requests and turns its state into the host's idle mode, the panel and the window's hand-over.

1. **Avvia** (A or Enter on a game, the button, a double click): the game's launch target is planned with its profile
   (`launcher::planLaunch` on `platform_win::Win32SystemProbe`). A blocker, a game without a target or an unknown
   profile is refused at once (`LaunchFlow::refuse`: the outcome panel, a "Blocked" record). A ready request is
   pending: the frame that asked shows the panel's "Gioco in corso" notice and is presented, then the host idles.
2. **The start**, on the host's first idle wake (`IAppLogic::onIdle`): visible windows prepare the hand-over
   (`WindowHandover::prepare`, `HandoverPolicy::Keep`); the session starts with the host's wake callback and
   `beforeResume` = `handOver()`. A start that fails (validation, the runner's error, the automation guard's refusal)
   ends at once with its outcome.
3. **The wait** (ExternalEmulatorRunning, specification §15.5): the host waits for events, at most `kIdleWait` (100 ms)
   at a time; every wake polls the session (`LaunchSession::poll`, its timeouts included). No frame is rendered: the
   window keeps the "Gioco in corso" frame behind the emulator. Input events that would act once frames resume
   (presses, motion, text) are dropped; releases and window events are delivered, and `AppInput::releaseAll()` releases
   whatever the wait dropped when it ends (the release of the button that launched among them).
4. **Questions** stop the wait so that the panel can render: StillRunning (no window for the profile's window timeout:
   "Torna a Retroverse" or "Continua ad aspettare"), AwaitingConfirmation (UserConfirmReturn: "Sono tornato" or "Torna
   al gioco"), and the user back in Retroverse while the game runs (the window kept the focus for
   `LaunchFlow::kComeBackDelay`, 1.5 s: the emulator's window closing hands the focus back 0.2 to 0.35 s before its
   process ends, which must not flash the panel; "Torna a Retroverse" or "Torna al gioco"). Returning ends the session
   with `LaunchSession::confirmReturn` (the emulator left running when it runs). A window the game shows again ends
   StillRunning's question the same way, and the window losing the focus takes back the "playing" question (the user
   went back to the game without answering); the session's own questions (StillRunning, the confirmation) stay until
   answered. The session is followed every frame while a question shows.
   - **Only while it can be seen**: a question renders while Retroverse's window has the focus. Without it (the game in
     front: a UserConfirmReturn launcher exits at once and the confirmation is asked while the user plays), its frame
     is presented once, the frame the window keeps behind the game, and the host idles again until the focus comes back
     (`LaunchFlow::hostIdle`; ADR-0006: no GPU work while the emulator runs).
   - **Back to the game** ("Torna al gioco", "Continua ad aspettare", or Back on the panel: `LaunchFlow::backToGame`)
     renders one frame with the notice and waits again. From a window without the focus nothing more is needed (the
     question comes back with the user: a focus gain, then the come-back delay). From the focused window the user is
     still in front of Retroverse, whose presses the wait drops, so it never waits there without a way back: for
     "Torna al gioco" (Playing, Confirm) the window is minimized (`ProductUi`: `SDL_MinimizeWindow`), so that Windows
     activates the next window, the game the user came from, and `WindowHandover::restore()` takes it back from the
     taskbar at the return; should the focus stay anyway, the question comes back after the come-back delay. A press
     in the focused window (a key, a click, a pad button: `LaunchFlow::userPressed`, the press itself still dropped)
     brings the question back at once; that is also how "Continua ad aspettare" ends, since StillRunning's game shows
     no window to go back to (no minimizing, no timer).
5. **The return**: the session's end (`LaunchSession` outcome) becomes a `LaunchMessage`; the outcome panel shows it
   unless it is a plain return or a return the user chose with the game left running (`showsMessage`). Visible windows
   come back: `WindowHandover::restore()`, then `update()` every frame until restored, in fullscreen and in the
   foreground (the time it took is `LaunchRecord::foregroundMs`), except when processes of the game's tree still run
   and nobody said the game was over (`LaunchFlow::returnTakesForeground`: `processes-remained`, the wait limit): the
   window then stays where it is, behind the game or minimized, and its message waits for the user, since raising it
   (the input attachment included) would cover a game that may still run. Hidden windows (`--hidden`, the gpu tests)
   have no hand-over at all: nothing is visible to hand over or bring back, and the foreground remedies must never
   raise a window nobody can see.
6. **Quitting** while a game runs (the window closed, a quit request; Back cannot quit then: the panel or the catalog
   takes it): `IAppLogic::onShutdown` is the quit hook. `LaunchFlow::quit` ends the session with `confirmReturn` (never
   a kill: `LaunchSession` stops following the processes), recorded as `left-running`/`quit` with `quitDuringGame`; a
   launch still pending never starts. The run exits cleanly (an `--exit-after-frames` run with 0).
7. **The wait limit** of frame-limited runs (`--smoke`, `--exit-after-frames`): a session still running
   `--launch-wait-limit` seconds after its start (default 120, at most 3600; refused without a frame limit) ends like
   a quit, with `confirmReturn` (`LaunchFlow::setWaitLimit`), recorded as `left-running`/`wait-limit` with
   `waitLimitReached`, and the run goes on to its `--frames` and writes its report: an automated run never waits
   forever for a program that does not end, and nothing is ever terminated to get there. Interactive runs have no
   limit.

The focus the flow follows is Retroverse's window's input focus (SDL focus events; the window's flag when the product
UI starts). A hidden window counts as focused, always: the input script stands for the user in front of it, so the gpu
runs ask their questions deterministically and a Back on a question comes back after the come-back delay.

One launch at a time: Avvia during a launch does nothing (the panel has the input anyway).

### The host's idle mode

`AppContext::setIdle(true)` (from any callback) makes the host skip update, render and swap from the next loop: it
calls `IAppLogic::onIdle` right after the frame that set it was presented, then after every wait (any event, the wake
event, or `AppHost::kIdleWait`). The wake event is an SDL user event registered per run (`SDL_RegisterEvents`); the
host keeps it to itself. Frames do not advance while idle: `frameIndex()` stays, an `--input-script`'s frames wait, a
frame-limited run renders its whole `--frames` whatever the wait (the budget is not consumed), and the frame after the
wait gets a short delta (the clock ticks once when the wait ends, so the wait is no frame time: the walker does not
jump, the frame statistics do not count it). Quit requests and window close still end the run during the wait.

## Outcome messages

`LaunchMessage{code, reason, detail}` (`rv/app/launch_outcome.hpp`); the Italian text is in
`assets/ui/game_session.rml`, chosen by `data-if` on the codes: each says what happened and what to do (check the
emulator's path in its profile, that the drive or folder of the games is connected, retry and then check the emulator or
its core). The detail is data: a file name, a token, an exit code or an NTSTATUS name sit inside the sentence; the
runner's and the plan's own words (`start-failed`, `invalid-request`, `program-refused`, `core-missing`), English and
often with a path, show apart on a smaller "Dettagli:" line (`.details`, broken anywhere).

| Code | Reason | Detail | When |
|---|---|---|---|
| `returned` | | the exit code | a normal end, or a return the user confirmed with nothing left running (no message; a UserConfirmReturn launcher's quick exit is no hint then) |
| `quick-exit` | | the exit code | a clean exit within the startup grace (DirectProcess), with ADR-0006's hint: the profile must wait for the process tree (`"wait": "tree"`) |
| `processes-remained` | | the exit code | the started program ended on its own while processes of its tree still ran (a wrapper whose emulator is its child): the game may still run, with the same hint; the window is not raised |
| `left-running` | `confirmed`, `quit`, `wait-limit` | | the user returned while it ran (no message), Retroverse quit while it ran, a frame-limited run stopped waiting |
| `crashed` | | `0xC0000005 STATUS_ACCESS_VIOLATION` | an NTSTATUS exception |
| `failed-to-start` | `start-failed`, `loader-error`, `early-exit`, `invalid-request` | the error (without the launch's name), the NTSTATUS name, the exit code (`3`; an NTSTATUS by its name) | `FailureReason` |
| `not-configured` | `template`, `program-refused`, `invalid-request`, `unknown-profile`, `no-launch` | the token (`{var:core}`), the plan's message, the profile id | the plan's configuration blockers, the mock's unknown profile or missing target |
| `emulator-missing` | | the profile's name | `ExecutableNotFound` |
| `core-missing` | | the plan's message | `RequiredFileMissing` |
| `rom-missing` | | the ROM's file name | `RomMissing` |

## The game-session panel

`assets/ui/game_session.rml` (data model `session`: `phase`, `title`, `code`, `reason`, `detail`, `timeout`; events
`return_home`, `resume`, `dismiss`), a modal document over the catalog in the same RmlUi context. Phases
(`toString(SessionPanel)`): `launching` ("Gioco in corso", no choice), `playing`, `still-running`, `confirm` (two
choices), `outcome` (OK). Shown, it takes the focus on its first choice; the directions move among the buttons (RmlUi's
spatial navigation, confined to their row), Activate presses the focused one; the choices are applied after the
frame's commands, never inside RmlUi's event dispatch. Hidden, it drops its modal state first, so that RmlUi gives the
focus back to the catalog's element.

## Run report

`RunReport::catalog` (`CatalogRecord`): `source` ("mock"), `file`, `games`, `opened`, `open` at the end,
`selectedPosition`, `selectedId`. `RunReport::launches` (`LaunchRecord`, written only when the run launched something):
`gameId`, `profile`, `state` (the session's final state, `Blocked`, `Pending`), `outcome`, `reason`, `detail`,
`exitCode`, `strategy`, `processId`, `quickExit`, `processesRemained`, `confirmedByUser` (the user's own confirmation:
false when quitting or the wait limit ended the session), `wasStillRunning`, `quitDuringGame`, `waitLimitReached`,
`frames` {`request`, `start`, `end`, `withPanel`, `whileRunning`}, `idleWakes`, `times` {`startMs`,
`runMs`, `returnMs`, `foregroundMs`}, `panels`. A plain return reads `frames.start == frames.end` (the frame after the
request; no frame between) and `whileRunning` 0; `whileRunning` counts only the frame after a choice to go back to the
game.

## Decisions taken within the step

1. **The panel and its screen live in rv_app** (`GameSessionPanel`, private to `rv_app`, on RmlUi through `rv_ui`'s
   public link), its GPU-free contract in `rv_app_cli` (`game_session_ui.hpp`), because this wave's brief gives rv_app
   and the assets to this step; it can move to `rv_ui` beside `QuickCatalogScreen` later.
2. **The catalog's screen is created on first open**, not hidden at start: `QuickCatalogScreen` lays its pool of rows
   out and records its focus only while shown (its `afterUpdate` returns while hidden), so a screen created hidden
   showed the focus on its first row without knowing it, and Avvia did nothing.
3. **QuickCatalog toggles unless a UI action shares the press** (Tab toggles, the pad's Y opens and is "Vedi media"
   inside): reading QuickCatalog in the UI context would close the catalog on every "Vedi media" with the default
   bindings.
4. **The frame that asks for a launch is presented first**, the session starts at the host's next wake: no frame is
   rendered after the emulator starts, and the frame the window keeps behind it says "Gioco in corso".
5. **No hand-over for hidden windows** (the brief: the remedies must not raise a fullscreen window in gpu tests): the
   whole `WindowHandover` is skipped, not only its remedies, since a hidden window has nothing to hand over.
6. **The come-back delay** (1.5 s of focus before asking "Gioco in corso"), so that the focus handed back while an
   emulator shuts down never flashes the panel.
7. **The quit hook is `onShutdown`**: every way out of the run (window close, quit request, an error) passes there,
   and `LaunchSession::confirmReturn` ends the session as left running; no confirmation is asked, since nothing is
   terminated (specification §11.5 asks for one before terminating an emulator).
8. **Frame-limited runs bound the wait** (`--launch-wait-limit`, 120 s by default, below the smoke harness's 240 s):
   a run whose launched program never ends still reaches its frames and writes its report, with the game left running
   (`left-running`/`wait-limit`), instead of hanging until a harness terminates it without a report.
9. **Failure details are data**: the runner's error, the NTSTATUS name, the token; the session's sentence loses the
   launch's name (the panel names the game itself).
10. **The automation guard in every gpu case** (ctest `ENVIRONMENT` lists the fake alone): a smoke run that launched
    anything else would be refused. The launch tests also set it themselves when run outside ctest
    (`ensureLaunchAllowList`), so the guard is always exercised; one case launches a copy of the fake the list does
    not hold and expects the refusal.
11. **A hidden window counts as focused**: it never gets focus events, yet the input script acts as the user in front
    of it; counting it unfocused would idle every question after one frame and leave a Back on a question waiting for
    a focus gain that never comes.
12. **"Torna al gioco" from the focused window minimizes it**: the game, the window the user came from, is next in the
    z-order and takes the foreground; finding the tree's window and raising it would need the runner's window list,
    which `IRunningProcess` does not expose. `WindowHandover::restore()` already takes a minimized window back (the
    spike's Minimize policy). The come-back delay and a press bring the question back when the focus stays anyway.
13. **No raise over processes left in the tree**: a return with processes still running that nobody confirmed
    (`processes-remained`, the wait limit) leaves the window where it is; a false positive (a helper about to exit)
    costs the user a click on Retroverse, a wrong raise would cover their game.

## Tests

- **Unit** (`rv_tests "[app]"`, 106 test cases): Back levels with the panel and the catalog; input contexts through the
  default bindings (Enter, the down arrow, B, Tab, Y; the global actions); outcome messages for every session end and
  blocker; the mock catalog (fields, relative ROMs, errors with their JSON pointer, profiles merged, the smoke fixture
  valid, `--mock-catalog`); `LaunchFlow` on a scripted runner (the wait, one launch at a time, every ending, the
  guard's refusal, StillRunning asked and answered, a window shown again, UserConfirmReturn, the user back after the
  delay, a question presented once without the focus, back to the game from the focused window (the hand-over asked
  for, the question back after the delay or at a press, "Continua ad aspettare" waiting for a press), processes left in
  the tree, a confirmed quick exit, the wait limit, quitting, the records); `--launch-wait-limit`; the run report's new records (round trip, absent keys, errors); the panel document's
  ids, bindings and a text for every phase, code and reason.
- **GPU smoke** (`gpu.launch: ...`, hidden, `RV_LAUNCH_ALLOWLIST` = the fake): a return (Returned, frames 9 to 9, no
  frame while it ran, 2.5 s run, 50 frames rendered, the fake's argv with the ROM as one argument in its folder); nine
  failure paths, each leaving the app interactive (the outcome panel dismissed by Back, the catalog closed by Back, the
  walker moved), with their exact details: quick exit (`0`), error exit (`3`), crash, the guard's refusal, emulator
  missing, ROM missing, a template variable missing, no launch target, a wrapper leaving its child running
  (`processes-remained`); the panel's questions (StillRunning answered "Torna a Retroverse": left running and the
  fake ending by itself; UserConfirmReturn answered "Sono tornato", also after its program's quick exit: a plain
  return); Back on the confirmation (frame 14: the notice, the wait, the question back after the come-back delay,
  answered on frame 20: panels `confirm` twice, one frame while the game had the screen); the wait limit (a fake that
  hangs, `--launch-wait-limit 2`: `left-running`/`wait-limit`, the run's 50 frames and its report, the fake ending by
  its own exit timeout with 124); closing the window while the game runs (WM_CLOSE
  from the test: the run exits 0 after 9 frames, the launch recorded left running/quit, the fake still running, then
  ending by its exit timeout with 124); a closed catalog draws nothing (the same frames with and without
  `--mock-catalog`, 0 pixels apart).
- **Desktop** (`desktop.smoke.launch_visible`, on demand): Retroverse in borderless fullscreen launches the fake with a
  fullscreen window (500 frames, about 3 s at 165 Hz); the run records the window back in the foreground after the exit
  (on the reference machine: frames 9 to 9, no panel, back in the foreground 7 ms after the fake's exit).
- Golden G01 runs without a catalog: unchanged (0 pixels apart in Debug and Release).
- **Gate A** (`./scripts/gate.ps1 -Gate A`, on the reference machine, with other worktrees building): the three builds
  without a warning, 1,041 unit tests in Debug and Release, 22 GPU checks in Debug and 20 in Release (the launch cases
  among them, Release rendering from its deployed assets), golden G01 0 pixels apart in both. Launch cases in Debug:
  the return 17 s, the eight failures 96 s, the panel's questions 18 s, quitting 24 s (the fake's own 12 s exit
  timeout included), the closed catalog 10 s; in Release 5, 16, 12, 14 and 5 s.
- **After the review fixes** (`./scripts/test.ps1 -Preset msvc-x64-debug-gpu` and `msvc-x64-release-gpu`, both builds
  without a warning): 1,047 unit tests in both, 25 GPU checks in Debug and 23 in Release, all passed, golden G01
  among them; the seven launch cases in Debug: Back on the question 9 s, the closed catalog 11 s, the wait limit 16 s
  (the fake's 12 s exit timeout included), the return 7 s, quitting 16 s, the nine failures 40 s, the questions 21 s;
  in Release 6, 4, 13, 4, 13, 17 and 14 s. The visible case was not rerun (it takes over the screen); run directly
  without its ctest registration, it skips (exit code 77).

## For the merge

- **CLAUDE.md**: "Running the app": `--mock-catalog`, `--launch-wait-limit` (frame-limited runs, default 120 s), the
  quick catalog's controls (Tab / Y, Back levels), the idle wait while an emulator runs, the launch records of
  `--run-report`; the GPU smoke tests list the launch cases (and `RV_LAUNCH_ALLOWLIST` on every gpu case), the desktop
  case (run only through ctest: it skips without `RV_SMOKE_DESKTOP=1`); layout: `rv_app` links `rv_ui`, `rv_launcher`,
  `rv_platform_win` (the "`rv_app` does not use `rv_media`, `rv_launcher` or `rv_ui` yet" sentence changes:
  `rv_media` alone); safety rules: automated runs that launch set `RV_LAUNCH_ALLOWLIST` (step 5's note).
- **docs/architecture.md**: the app layer's product UI (`ProductUi`, `GameSessionPanel`), `LaunchFlow`, the host's idle
  mode and wake event, input contexts, the overlay slot composed (product UI below the developer overlay).
- **docs/roadmap.md**: step A1 done.
- **ADR-0006**: the Consequences' "the application drives a LaunchSession in its ExternalEmulatorRunning state ... with
  the keep policy, and shows quickExit and processesRemained" is done (A1: `quick-exit`, `processes-remained`); "Torna
  al gioco" from the focused window minimizes it for that leg (decision 12); the open risk "While the emulator shuts down
  ... the kept window shows its last frame: render a returning state" is answered by the kept "Gioco in corso" frame.
- **BACKLOG.md**: close "Game running screen" (the panel) and the A1 parts of the I1 follow-ups; candidates:
  - `QuickCatalogScreen::show()` after `hide()` relies on RmlUi's focus event to restore its focus record, which RmlUi
    does not send when its focus never left the element; `ProductUi` moves RmlUi's focus to the context's root before
    `show()` (fix in rv_ui: record the focus explicitly in `show()`);
  - hot reload of `game_session.rml` in developer builds (the catalog's document reloads, the panel's not yet);
  - `LaunchSession` logs the command line at info level (ADR-0008 §9 says debug: step 12);
  - the panel's strings with the game title in the RML (localization tables, I7);
  - "Vedi media" and "Informazioni" do nothing in the app yet (I3, I5).
