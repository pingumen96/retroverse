# I3 step 11: the hero CRT's video

Step 11 of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice", wave 2) puts a video on the hero CRT, as ADR-0012 §7
decides: one `media::VideoDecoder` (asserted: never two), the screen material's texture override, the 300 ms dwell, the
choice at the start, the fallback chain (screenshot, title screen, cover, the idle pattern), the decoder paused out of
view, stopped and joined before a launch (bounded 1 s) and reopened at its position after the return,
`--video-freeze-pts`, NVIDIA's message 131154 filtered, and the decoder's state in the run report and the Stats panel.
It fills the stubs step 0a listed for it (`screen_policy` in `rv_app_cli`, `screen_service` in `rv_app`) and the run
report's `video` section, and works on the test room's CRT; the room's CRT (10c) and the world's selection (13b) use the
same service later. Golden G01 is unchanged: without a game to show, the CRT keeps its own colour bars.

## What was built

| File | What it holds |
|---|---|
| `rv/app/screen_policy.hpp`, `src/screen_policy.cpp` (`rv_app_cli`, GPU-free) | `ScreenPicture`, `ScreenMedia`, `ScreenGame`, `kScreenVideoExtensions` and `isPlayableVideoFile()`, `stillChain()`, `mediaPath()`, `chooseStartGame()`, `ScreenPhase`, `ScreenPolicy`; `kScreenDwellSeconds` 0.3, `kScreenStopBoundSeconds` 1.0 |
| `src/screen_service.hpp`, `src/screen_service.cpp` (`rv_app`, private) | `HeroScreen` and `findHeroScreen()`, `ScreenServiceOptions`, `ScreenService` |
| `rv/render/material_registry.hpp`, `src/material_registry.cpp` | `GpuMaterial::textureOverrides`, `MaterialRegistry::setTextureOverride()` / `textureOverride()`, `bind()` binding an override in place of the material's own texture |
| `rv/render/debug_message_filter.hpp` | `kNvidiaInformationalIds` gains 131154 |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `VideoRecord` with `VideoFallbackRecord` and `VideoLaunchRecord`, written and read |
| `rv/app/mock_catalog.hpp`, `src/mock_catalog.cpp` | a game's `"media"` (video, screenshot, title screen, cover) and `"lastPlayed"`, `MockCatalog::screenGames()` |
| `src/product_ui.hpp`, `src/product_ui.cpp` | `LaunchHooks` (`starting` before the hand-over, `ended` at the return), `ProductUi::setLaunchHooks()`, `selectedGameId()` |
| `src/retroverse_app.cpp` | the service made for the scene's hero CRT, updated every frame, its launch hooks, its shutdown and report |
| `src/command_line.cpp` | `--video-freeze-pts` accepted (step 0a's refusal removed) |
| `rv/devui/panels.hpp`, `src/panel_stats.cpp` | `StatsPanelModel::videoStatus`, the Stats panel's "Video" section |
| `tests/unit/app/test_screen_policy.cpp` | the start choice, the dwell, the chain, the launch's life, visibility, new games |
| `tests/smoke/test_video.cpp` | `gpu.video: ...`: a clip playing at its rate, paused out of view, frozen frames, the fallbacks, the selection, a launch |

## What the hero CRT plays (`ScreenPolicy`)

- **The target**: before any selection, the start choice (`chooseStartGame`): the most recently played game with a
  video ES-DE would read, else the first game with one in the games' order (the featured order), else nothing (the idle
  pattern). The item focused or inspected (`hover(id, now)`, every frame) becomes the target once it has been so for
  0.3 s of the run's simulated time; a hover that ends changes nothing (the selection keeps looping until another item
  is selected); hovering the target again is no new selection; an id the games do not list is a game without media.
  Each new target bumps `targetGeneration()`.
- **The picture** of the target: its video once a frame of it shows (`videoShowing()`), else the still of the chain that
  is ready (`stillReady()`), else the idle pattern. `wantedStill()` is the still to load: screenshot, title screen,
  cover, those the game has, in that order; `stillFailed()` moves to the next. A video whose file ES-DE would not read
  (`isPlayableVideoFile`: `.mp4 .mkv .avi .wmv .mov .webm`, esde's `kVideoExtensions`, kept equal by a test) is never
  asked for; one whose decoder fails (`videoFailed()`) is not asked for again until the target changes.
- **The phase**: `launchStarting(position)` stops the decoder (`wantsDecoder()` false) and keeps the clip's position;
  `launchEnded()` (the return, a failed start) lets it run again and `takeResumePosition()` hands the position, once,
  to the reopened decoder; `quit()` keeps it stopped for good (quitting during a game finds it stopped).
- **Visibility**: `setVisible(false)` pauses the decoder (`decoderPaused()`), the picture kept.

## The screen service (`ScreenService`)

- **The hero screen** (`findHeroScreen`): the scene's first `crt` prefab object whose `screen` role shows a picture
  (the test room's `crt_tv` with its colour bars), its material the picture instance `SceneInstance` registers as
  `"<id>/screen@<w>x<h>"` (a `MAT_Screen_CRT` with a base colour texture), its bounds from the object's first instance.
  Scenes without one (the model viewer) have no service and an empty `video` section.
- **One decoder**: `media::VideoDecoder` at most 480 rows (ADR-0012 §7: one 480p decoder), looping, through
  `media::VideoPlayback` on the run's simulated time (so `--fixed-dt` runs advance the clip with the shader time). A
  clip replacing another opens only once the other's thread has ended (the stopped decoder is kept, polled each frame
  without waiting), so no two decoder threads ever run; a process-wide count asserts it (a second decoder is refused
  and logged in Release). `maxDecoderThreads` in the report is the most decoder threads alive at once, checked to be at
  most 1 by every smoke run.
- **Uploads**: the frames go into one `gl::VideoTexture`, created at the first frame and kept for the run (launches
  included), each `upload` inside the CPU section `videoUpload` (the creation outside it); a busy pixel buffer ring
  keeps the frame for the next frame (`ringBusy`).
- **Stills**: read shared (`core::readFileShared`), decoded and fitted to the screen's 4:3 at 640 x 480
  (`assets::fitCover`: cropped within 8% or banded) on a worker of its own (`core::WorkerPool`, one thread, below normal
  priority), uploaded on the main thread as a `Texture2D` of kind `HeroTextures` with mipmaps, clamped. Runs with
  `--fixed-dt` wait for a still being loaded (at most 5 s), so scripted runs show the same picture on the same frame. A
  new target cancels the load in flight.
- **The override**: whichever picture is shown is set as the base colour texture of the screen's material
  (`MaterialRegistry::setTextureOverride`); the idle pattern clears it (the material's own colour bars), and the
  service clears it when it goes. A `VideoTexture` that reallocates gets a new id, which the next frame sets.
- **Out of view**: the screen's bounds against the camera's frustum; outside it the playback pauses (`pauses`), the
  picture kept.
- **Launches**: `ProductUi`'s `LaunchHooks`: `starting` on the host's wake that starts a pending launch, before the
  window's hand-over and the process; `ended` once the flow says the launch returned (the return, or a start that
  failed at once). Before the launch the decoder stops and its thread is waited for at most 1 s (a thread that does not
  end in time is logged and the launch goes on), the position (the last frame's timestamp modulo the clip's duration)
  kept; after it the decoder opens again and seeks there. A launch refused before it starts (a blocked game) never
  stopped the decoder. When Retroverse quits, the service's `shutdown()` stops the decoder (it stays stopped when a game
  still runs), waits for every decoder thread (`media::waitForStoppedDecoders()`), ends its worker, and the report
  records `joinedAtExit`.
- **`--video-freeze-pts <s>`** (frame-limited runs): every clip opens paused, seeks to the time and holds that frame;
  the frame that first shows it waits for it (at most 5 s), so two runs give the same screenshot byte for byte.
- **Logs** name the game, never a path (the user's folders): the video's file at debug level only.

## Selection and media until the room

The world's focus and inspection (13a, 13b) do not exist yet, nor the room's games with their media (10b, 10c). Until
then:

- **The selection** the CRT follows is the quick catalog's selected game while the catalog is open
  (`ProductUi::selectedGameId()`), through the same dwell. Step 13b replaces it with the world's focus and inspection
  (or keeps the catalog's as a second source).
- **The games** come from the mock catalog (`--mock-catalog`): a game's `"media"` (paths relative to the catalog file,
  like its ROM) and `"lastPlayed"` (UTC ISO-8601). The ES-DE catalog gives the service no games yet, so its runs show the
  idle pattern; step 10c gives it the placed games' media paths (step 2's snapshot) and their last sessions.

## The run report's `video` section

Empty (`{}`) without a hero CRT; otherwise:

```json
"video": {"bound": true, "screen": "crt_tv", "gameId": "g000000000000b001", "picture": "video", "phase": "running",
  "decoderState": "playing", "frozenFrameShown": false, "selections": 0, "targets": 1, "decodersStarted": 1,
  "maxDecoderThreads": 1, "framesUploaded": 75, "ringBusy": 0, "pauses": 0, "firstFrameMs": 16.9,
  "clipFramesUploaded": 75, "firstPtsSeconds": 0.0, "lastPtsSeconds": 2.47, "clipUploadSeconds": 2.47,
  "videoBytes": 4915200, "stillBytes": 0, "joinedAtExit": true,
  "fallbacks": [{"gameId": "...", "picture": "video", "reason": "corrupt-data"}],
  "launches": [{"decoderRan": true, "stopMs": 1.3, "threadsRunning": 0, "positionSeconds": 0.567, "reopened": true,
                "reopenedAtSeconds": 0.567}]}
```

`picture` is `idle-pattern`, `video`, `screenshot`, `title-screen` or `cover`; `phase` `running`, `launched` (quit
during a game) or `quit`; `decoderState` the decoder's (`playing`, `paused`, ...) or `none`; a fallback's `reason` is
the decoder's failure hyphenated (`file-not-found`, `invalid-file`, `corrupt-data`, `truncated`, `unsupported-codec`,
`no-video-stream`, `decoder-error`), `not-playable` (never opened), or an error code (`not-found`, `parse-error`,
`unsupported`) for a still. `firstFrameMs` is the wall-clock time from the last clip's decoder start (its selection after
the dwell, the start choice, the reopening after a launch) to its first frame uploaded. `requireCleanRun` checks, in
every smoke run, at most one decoder thread, `joinedAtExit`, and `videoBytes` equal to `gpuMemory`'s video.

## Tests

- `tests/unit/app/test_screen_policy.cpp`: the playable extensions (ES-DE's list); the start choice (most recently
  played with a video, first with one, none, ties); the dwell under the fixed step (the 19th frame selects, the 18th
  does not), stepping every 5 frames never switching, a hover that ends keeping the selection, a hover broken and
  restarted, the start choice selected without a restart, an unlisted id; the chain (a still while the video opens, the
  video once shown, a failed video back to the still, each failed still to the next, the idle pattern last, a file ES-DE
  would not read never asked for, a new target's own chain); the launch (stopped, the position resumed once, a launch
  without a frame shown, quitting keeps it stopped); out of view; new games replacing the start choice, not a
  selection.
- `tests/unit/app/test_mock_catalog.cpp`: media and last play read, relative and absolute paths, `screenGames()`,
  unknown media kinds, wrong types, a bad time.
- `tests/unit/app/test_run_report.cpp`: a bound video record read back field for field, absent optionals, a missing
  field and a bad launch entry refused with their pointers; the unbound record stays `{}` (step 0a's test).
- `tests/unit/app/test_command_line.cpp`: `--video-freeze-pts` accepted (0, 1.0, 86400), validated as before, no longer
  refused; `tests/unit/render/test_debug_message_filter.cpp`: the NVIDIA ids with 131154.
- `tests/smoke/test_video.cpp` (`gpu.video: ...`, test room, fixed step, clips written by `media::writeTestVideo` into
  the case directory: 640 x 480, 30 fps, MPEG-4):
  - a clip plays: the start choice, one decoder, the first frame within 1.7 s (2 s less the dwell), about 30 frames per
    simulated second with their timestamps as far apart as their uploads, `videoBytes` within 16 MiB and equal to
    `gpuMemory`'s video, the `videoUpload` section over every upload (its figures as a warning: informative);
  - out of view: turned around (`turn_around.json`), the decoder paused once, the picture kept;
  - `--video-freeze-pts 1.0` twice: one frame uploaded, at 1.0 s, held (the decoder paused), byte-identical
    screenshots; 2.5 s gives another picture whose changes lie on the CRT (at most 1% of the changed pixels outside the
    CRT's projected box and its glow); a catalog without media keeps the colour bars, the video's changes on the CRT
    only;
  - fallbacks, one run each: a missing clip to the cover, an empty one to the screenshot, a corrupt one past a missing
    screenshot to the title screen, a truncated one (after its frames) past a text named `.png` and a truncated PNG to
    the cover, an unsupported codec without stills to the idle pattern, an `.flv` (never opened, selected in the
    catalog) to the cover; each fallback recorded with its reason (compared as a set: the stills fail on the worker
    while the decoder opens), the failed decoder gone, the still a hero texture of the accounting; the frame times are
    printed as a warning (informative: the lockstep of `--fixed-dt` waits for the stills, and the budget is measured
    without it, below);
  - the selection: C played last is the start choice, the catalog opening on A selects it after the dwell, B after its
    own, C hovered 4 frames changes nothing: three targets, two selections, three decoders started, never two at once,
    B's first frame within 1.7 s;
  - a launch (the fake emulator, hidden, `RV_LAUNCH_BACKGROUND` set, the allow-list naming the fake): the decoder
    stopped and joined before the hand-over (no thread left, within 1 s), its position about 0.57 s, reopened there
    after the return (the reopened clip's first timestamp at that position), two decoders started, never two at once.

Results on the reference machine (RTX 2080 SUPER) with `RV_LAUNCH_BACKGROUND` set and `RV_LAUNCH_ALLOWLIST` unset (the
GPU cases set it to the fake emulator themselves): Debug, RelWithDebInfo and Release built with 0 warnings;
`./scripts/test.ps1 -Preset msvc-x64-debug-gpu` 1,590 tests passed (1,552 unit, 38 GPU, the 6 `gpu.video` cases among
them); in Release the `gpu.video` cases and `gpu.golden/G01` passed. Golden G01 0 of 2,073,600 pixels apart (largest
difference 0/255) in Debug and Release. A Debug `./scripts/test.ps1` run before failed two busy-database timing tests
(`unit.app/catalog_worker: a session write that meets a busy database ...` and `unit.catalog_db/sessions: a write that
meets a busy database ...`, step 1c saw the first under load too), which passed when run again alone and in the GPU
preset's run; nothing of this step is involved.

## Measured

Informative: the machine was busy with the other worktrees of the wave (CPU load 68% and more while measuring), so the
percentiles above p95 carry other programs' preemption; the gate measures on a quiet machine. Release, the test room,
1920 x 1080, `--no-vsync`, hidden, a 640 x 480 30 fps MPEG-4 clip of the mock catalog:

| | Frame p50 / p95 / p99 | `videoUpload` p50 / p95 / p99 / max | First frame |
|---|---|---|---|
| 4,000 frames with the clip (388 uploads) | 2.97 / 4.60 / 9.51 ms | 0.18 / 0.26 / 0.58 / 3.39 ms | 21.9 ms |
| 600 frames with the clip, 5 runs | p50 3.0 to 3.8 ms | p50 0.17 to 0.19 ms | 15 to 70 ms |
| 600 frames without (the colour bars), 5 runs | p50 2.9 to 5.2 ms | | |

- **Upload**: p50 0.18 ms, p99 0.58 ms against the budget's 0.5 ms (ADR-0004's spike measured 0.36 to 0.48 ms for
  480p on a quiet machine); the maximum, 3.4 ms, is one of the first uploads, which touch the persistently mapped ring's
  pages for the first time. The texture's creation is outside the section. Debug's run of `gpu.video: ... plays ...`
  on a quieter moment: p50 0.35 ms, p99 0.58 ms over 75 uploads.
- **Frame**: the decoder's own cost is on its thread; with the clip the frame's p50 is within the noise of the runs
  without it (2.9 to 3.8 ms).
- **First frame** (NFR-06, from the decoder's start): 6.6 to 70 ms in every run (the busiest machine moments the
  highest), against 2 s less the 300 ms dwell.
- **Stop before a launch**: the decoder's thread ended 1.1 to 9.9 ms after the stop (bound 1 s); reopened at 0.567 s,
  its first frame 7 ms later.
- **Fallbacks without `--fixed-dt`** (300 frames, Release): the truncated clip (past a text named `.png` and a truncated
  PNG, to the cover) frame max 9.0 and 51.5 ms in two runs, the corrupt one (past a missing screenshot, to the title
  screen) 8.7 and 18.1 ms; p99 7.7 to 15 ms but for the one run at 43 ms, all under load. The main thread's share of a
  fallback is a still's upload (640 x 480 with mipmaps); decoding and fitting are on the worker.
- **GPU memory**: the video's texture and ring 4,915,200 bytes (4.7 MiB: a 1.2 MiB texture and a 3.5 MiB ring), equal
  in the report's `video.videoBytes` and `gpuMemory.kinds.video`; a still 1,638,352 bytes with its mipmaps (heroTextures).

## Decisions taken within the step

1. **The quick catalog's selection drives the CRT until 13b**, through the same dwell: the only selection the app has
   before the world's focus, and what the roadmap's "the first frame within 2 s of the selection" can be measured on.
2. **Media through the mock catalog** (`"media"`, `"lastPlayed"`): the test room has no WorldPlan and the ES-DE catalog's
   media paths reach the app with the room (10b, 10c); no new command-line option was needed (step 0a settled them).
3. **Never two decoder threads**, not only two decoder objects: a replaced clip waits (without blocking the frame) for
   the previous thread to end before its own starts; between them the screen shows the new game's still or the idle
   pattern for a frame or two.
4. **The clip's clock is the run's simulated time**, like the shader time, so a fixed-step run plays its clip at the
   run's pace; interactive runs' simulated time is the measured one.
5. **Stills on a worker of the service's own** (one thread, below normal), fitted to 640 x 480 and recorded as hero
   textures; step 12's hero textures (8, least recently used) may take them over. Lockstep runs wait for them.
6. **One 480-row decoder** (ADR-0012 §7), the SNES clips' size: the texture and its ring take 4.7 MiB (budget 16 MiB).
7. **Launch hooks in `ProductUi`** (`starting` on the wake that starts a pending launch, `ended` when the flow returns):
   the decision (and a refusal) comes before the hand-over, so a refused launch never stops the decoder and has nothing
   to restart; a start that fails at once returns through `ended` and reopens it.
8. **131154 in the NVIDIA list**, dropped whatever its severity on NVIDIA only, like the informational ids: every PBO
   upload raises it (ADR-0004), and a run showing the video must stay clean.
9. **Failure names hyphenated** in the report, like the error codes, so that one vocabulary reads the fallbacks.
10. **The `video` section stays `{}` without a hero CRT** (the model viewer, the asset preview), so step 0a's minimal
    report is unchanged; a bound record requires its fields.

## Open issues

- The selection bridge (the quick catalog's) is for 13b to keep or replace; the ES-DE catalog gives the service no
  games until 10c (the idle pattern meanwhile).
- Between two clips without stills, the colour bars show for the frame or two the previous decoder's thread takes to
  end; keeping the previous clip's last frame instead would hide it (I5's scheduler, or 13b if it shows).
- The budgets (upload p99 0.5 ms, no frame over 33.3 ms while falling back) were measured only on a loaded machine
  (upload p99 0.58 ms, fallback frames up to 51 ms in one run of four); the smoke cases print them as warnings and the
  gate measures them quietly. If the upload's p99 stays above 0.5 ms there, ADR-0004's zero-copy upload or YUV planes
  are the remedies.
- The decoder's audio, the scheduler (two decoders, priorities, playlists), upload staggering and the process priority
  stay I5's (ADR-0004, ADR-0012 §7).

## Notes for later steps

- **10c**: make the room's games (`ScreenGame`: the GameId text, the media paths from the snapshot, the last session's
  time) and call `ScreenService::setGames()`; `findHeroScreen` finds the room's CRT if it is a `crt` prefab object with
  a picture on its screen (or bind the room's own `HeroScreen`).
- **13b**: pass the focused or inspected item's GameId to `ScreenService::update()` (the `hovered` argument), in place
  of or beside the quick catalog's.
- **15a**: the world's launch goes through `ProductUi`'s launch, so the hooks apply; if it does not, call
  `launchStarting()` before the hand-over and `launchEnded()` after the return or a refusal.
- **17a**: G08a is `--video-freeze-pts 1.0` on a fixture clip (byte-identical between runs, checked here); G08b is the
  corrupt clip's fallback (a lockstep run shows the still on a fixed frame).

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Running the app": `--video-freeze-pts <seconds>` (frame-limited runs) is implemented: the hero CRT's clip opens
    paused at that time and holds the frame; the 0a sentence listing it as refused drops it.
  - "Running the app", `--run-report`: the `video` section (the hero CRT: the game and picture shown, the one decoder,
    the clip's first frame and uploads, the fallbacks with their reasons, the launches' stops and reopenings; `{}`
    without a CRT) and the CPU section `videoUpload`.
  - "Running the app", `--mock-catalog`: a game's `"media"` (video, screenshot, titlescreen, cover) and `"lastPlayed"`
    feed the hero CRT; without them (and with the ES-DE catalog until I3 step 10c) the CRT shows its colour bars.
  - "Running the app": the hero CRT plays the selected game's clip (the quick catalog's selection after 300 ms; the start
    choice: the most recently played game with a video), falls back to its screenshot, title screen, cover, then the
    colour bars; pauses out of view; stops before a launch and resumes after it. The developer overlay's Stats panel has
    a "Video" section. NVIDIA's message 131154 (every video upload) is dropped like the informational ids.
  - GPU smoke tests: `gpu.video: ...` (`tests/smoke/test_video.cpp`).
- **docs/architecture.md**: `rv_app` uses `rv_media` (the hero CRT's screen service); `rv_app_cli` holds the screen
  policy.
- **docs/roadmap.md**: step 11 done.
