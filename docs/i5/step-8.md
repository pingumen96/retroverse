# I5 step 8: videos and manuals in the media viewer

Roadmap row: I5 step 8. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §10 (the `videos` page, the
`manuals` page), with §2 (the `direct` class, the viewer's 720 rows), §9 (the viewer's clip at full volume) and §12 (the
viewer's clip in the GPU memory row). Amended on 2026-10-30 by the step's review (§1's name, §2, §4, §10: see
"Review" below).

## What was built

- **The pages** (`rv_ui_model`, `media_viewer_model.hpp`): `media_viewer::kKinds` gains `videos` after the title screen
  and `manuals` last, so `app::viewerPagesOf()` gives them (the clip and the manual a usable extension, as for the other
  kinds). Activate on the videos page pauses the clip and plays it again (`ViewerStep::TogglePause`, once its first frame
  shows; leaving the page drops the pause), on the manuals page asks for the manual (`ViewerStep::OpenManual`, once its
  file is found); on a picture's page it pages on, as before. A page holds the clip's or the manual's state, its reason,
  the manual's file name and what "Apri il manuale" did (`ManualState`: ready, opened, refused, failed; since the review
  opening and unsupported).
- **The document** (`assets/ui/media_viewer.rml`, `.rcss`): the clip as `<img src="rv-texture:media-viewer-video">`,
  fitted to the stage like a picture; "Caricamento del filmato..."; "In pausa" and the hint "A Pausa/Riprendi" (a button
  too); the fallback "Filmato non riproducibile" with a sentence per reason (`media_viewer::kVideoReasons`: the verdicts'
  names, `not-playable`, `graphics-error`, a generic one otherwise, with "Motivo: <reason>"
  for the generic one alone since the review); the manual's page with its file name, "Apri il manuale", "Ricerca del
  manuale...", "Il file del manuale non c'è..." and a sentence per outcome.
- **The clip** (`rv_app`, `screen_service.cpp`): `ScreenService::setViewer()` takes the viewer's request each frame
  (`app::ViewerRequest` in `game_screens.hpp`: open, and the videos page's clip). The clip is a screen of the scheduler:
  `direct` class, at the eye, in view unless the window is minimized, under its own key (`viewer:<opening>:<file>`, so
  it never shares a world screen's 480-row slot). Its slot decodes at `viewerMaxRows` (720) with two decoders, `maxRows`
  with one, into a `gl::VideoTexture` of the viewer's own (allocated with its first frame, gone when the viewer lets the
  clip go); `viewerVideo()` gives its state and a `GL_RGBA8` view of that texture (`gl::createRgba8View`), which the
  panel registers as `rv-texture:media-viewer-video` while the page shows. The page's pause holds the slot on its
  frame. A clip a failure verdict leaves out is never opened (`ScreenVerdicts::screenClip()` since the review: the
  verdict's name); one that fails gives its decoder's reason and a verdict, as on a world screen.
- **The world under it**: while the viewer is open (any page) every bound screen and cabinet counts as out of view:
  their slots pause at once (reason `viewer`), stop after 10 s like a minimized window's, and a slot the viewer's clip
  takes (`needed`) leaves its screen its frame; once the viewer closes they play again (a stopped clip where it stopped).
  The sound is the viewer's clip alone, at full volume (the master and video bus apply; no spatial gain, no duck:
  `audioDucked(..., viewerOpen)`), whatever room holds the eye.
- **The manual** (`rv_launcher`, `manual_open.hpp`): `planManualOpen()` gives explorer.exe of the Windows folder
  (`ISystemProbe::windowsFolder()`) with the manual's plain path as its one argument, run in the Windows folder, no
  startup grace and no window timeout, validated by `validateLaunchRequest()`. `GameScreensPanel` starts it through a
  `Win32ProcessRunner` of its own (no shell; the automation guard first), drops the running process at once
  (`stopWatching()`: no session, nothing waits, Retroverse keeps running) and shows the outcome; a refusal of the guard
  is "refused" (`Apertura bloccata`), a path the plan refuses "unsupported" (since the review), anything else "failed".
- **The panel** (`game_screens_panel.cpp`): the loader thread also finds the clip's file (its `\\?\` path below the
  media root) and the manual's (there or not, never opened); `viewerRequest()` and `setViewerVideo()` carry the clip to
  and from the service (`ProductUi` passes them through; `retroverse_app.cpp` calls them around `ScreenService::update`
  and makes the service, unbound, for a run that has shown no screen yet).
- **The report**: `video.viewer` (`clips`, `decodersOpened`, `framesUploaded`, `pauses`, `knownBad`, `failures`, `game`,
  `state`, `reason`, `width`, `height`, `rows`, `maxGpuBytes`, `gpuBytes` since the review, `openFrames`,
  `worldFramesWhileOpen`), the viewer in
  `video.screens` (kind `media-viewer`), `screens.videoFrames` and `screens.videoBox`, and the screens' events `video`
  and `manual`.

## Decisions beyond ADR-0016

- **The plain path for explorer.exe.** The media root's paths are `\\?\` paths; explorer.exe gets the drive or UNC path
  they stand for, and the plan refuses what it would misread: a relative or device path, another extension than `.pdf`,
  a path past `MAX_PATH`, an element ending with a dot or a space (Windows would name another file); since the review an
  element whose stem is a reserved device name, a colon past the drive's and any comma (explorer.exe splits its argument
  there; the argument is always quoted, but a quoted comma cannot be proved safe without opening a window). `&`, `%`,
  `^`, `!`, `'`, `;`, `$( )` and non-ASCII names stay in the one argument (the unit table, read back by
  `CommandLineToArgvW`).
- **No session for the manual.** explorer.exe hands the file to the shell and exits at once, often with exit code 1, so
  it is not followed: a process started is "opened". The PDF program is the shell's, outside Retroverse's job.
- **Which page asks for the clip.** Only the videos page: the clip opens when it shows and goes when it is left (coming
  back starts it from its beginning); the world pauses as soon as the viewer opens, on any page.
- **The viewer's volume** is 1, not a kind's `volume` from video.json (ADR-0016 §1: the viewer is not configured per
  kind); video.json's `enabled: false` turns the world's clips off but not the viewer's, which the user asked for (the
  keys that reach the viewer: ADR-0016 §4 as amended on 2026-10-30).
- **The reasons.** A verdict's name or the decoder's failure with dashes (as the report and the verdicts name them),
  `not-playable` for an extension ES-DE does not play, `graphics-error` for a texture or view that cannot be made, and
  `file-not-found` when the loader cannot place the clip below the media root.

## Proofs

- Unit (`rv_tests`):
  - `[manual_open]`: explorer.exe of the Windows folder with one argument for eight names (hostile ones included),
    each read back as one argument by `CommandLineToArgvW`; the `\\?\` forms given back; twelve paths refused, a long
    one, a Windows folder without explorer.exe; under `RV_LAUNCH_ALLOWLIST` naming the fake emulator the guard's
    verdict is `not listed`, `checkLaunchGuard` and `Win32ProcessRunner::start` refuse it (no process: the test stops
    before the start if the guard would allow it).
  - `[media_viewer]`: the pages' order with the clip and the manual, Activate's steps on each page, the states, the
    document's ids, sources, events and a sentence for every reason and outcome. `[game_screens]`: the clip and the
    manual among the pages. `[audio_policy]`: no duck under the viewer.
- GPU (`tests/smoke/test_media_viewer_video.cpp`, `esde/snes-room` with Amber Lantern's clip 1280 x 720 with a 1000 Hz
  tone and a manual, Cobalt Compass's clip with a 440 Hz tone on the hero's playlist):
  - frames advance on the page: the clip's counter read from F12 screenshots in the report's `videoBox` at frames 230,
    290, 330, 350 and 390: 22, 52 (1 s later: 30 frames), 56, 56 (paused at 300), 72 (playing again from 360);
  - the clip at 720 rows into its own 1280 x 720 texture; the world paused: the hero's slot paused (`viewer`) at 166,
    the room's secondary TV's slot taken (`needed`) at 185 for the clip, no frame of the world uploaded while the viewer
    was open (`worldFramesWhileOpen` 0), the hero resumed and the secondary reopened where it stopped (2.73 s) at the
    close (420);
  - the offline mix: the hero's 440 Hz heard before (0.078 amplitude), under the viewer its 1000 Hz at 0.49999 (the
    clip's 0.5: 0.0 dB, no duck over the quick catalog, no spatial loss) and the 440 Hz at 3.8e-11 (far below -40 dB),
    silence while paused;
  - "Apri il manuale": its file found, `refused` by the guard (the test requires `RV_LAUNCH_ALLOWLIST` to name the fake
    emulator and not explorer.exe before it runs);
  - a damaged clip (Opal Oriel's unknown codec, the world's clips off): the first run's page `failed
    unsupported-codec` (one decoder, a verdict given), the next run's `verdict unsupported-codec` (0 decoders).
  - `test_media_viewer.cpp`'s cases follow the new pages (Amber Lantern's clip at 7 of 9, Bramble Burrow Rally's at 4).
- GPU memory: the viewer's 720p clip holds 14.06 MiB at most (its texture and three-buffer ring, `maxGpuBytes`
  14,745,600 bytes), allocated only while it plays; with the hero and the secondary TV slots the video peak was 24.6 MiB
  and the fixture room's whole GPU peak 272.9 MiB at 1920 x 1080. On the busiest real floor (465.5 MiB with both slots
  playing at step 3) the viewer adds those 14.1 MiB while the world's slots keep theirs: about 480 MiB, within 512 MiB
  (computed, not measured on the real library).

## Results

- Debug unit tests (`./scripts/test.ps1`): 2217 of 2217 passed (the build clean, no warning).
- Debug GPU, the four `media viewer` cases (`./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter "media viewer"`):
  all passed.
- Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`, the Release build without a warning): 2386
  tests (2217 unit, 169 GPU), all passed; 16 skipped: the 15 goldens still recording driver 617.42 (the machine runs
  621.01; their re-baselining is another branch's) and, as always, the overlay's pick. G15, G17 and G20 compared and
  passed. No golden was rewritten: the viewer appears in none.
- The figures above are the Debug run's; the Release run passed the same checks.

## Screenshots

Hidden, frame-limited Debug runs of the GPU cases (`build/i5/step8/`, 1920 x 1080, the window 1280 x 720):
- `videos_playing_f230.png`, `videos_playing_f290.png`: "Filmato, 7 di 10", the 16:9 clip filling the stage's height
  without stretching, its counter row and burned-in "A 0052" readable, the hints "A Pausa".
- `videos_paused_f330.png`, `videos_paused_f350.png`: the same frame (0056) twice, the amber "In pausa" badge at the
  stage's corner, the hint "A Riprendi"; `videos_playing_again_f390.png` the clip moving on (0072).
- `manuals_refused_f412.png`: "Manuale, 10 di 10", "Manuale del gioco", the file name, the green "Apri il manuale" and
  "Apertura bloccata: in questa esecuzione automatica Retroverse non avvia altri programmi."
- `damaged_decoder_failure.png`, `damaged_known_verdict.png`: "Opal Oriel, Filmato, 2 di 2", "Filmato non
  riproducibile", "Il filmato usa un formato video che Retroverse non sa decodificare." and "Motivo:
  unsupported-codec", no pause hint.

## Review

Fixed after the step's review (ADR-0016 amended on 2026-10-30: §1's name, §2, §4, §10):
- **The world's clips under a long-open viewer** stopped after 10 s (`viewer`) lost their positions and reopened from
  their start: `stopSlot()` keeps the position for `viewer` as for `minimized`. GPU: a viewer open 12.2 s on a cover
  page; the hero's slot paused (`viewer`) at 166, stopped (`viewer`) at 766, reopened at 900 at 1.733 s.
- **The viewer's clip with no room's screens bound** (the lobby, a hall, the quick catalog before any room: the service
  made unbound) kept decoding and sounding after its page was left or the viewer closed: `update()`'s unbound path
  returned before the scheduler. It now runs the scheduler while a slot is open. GPU, the lobby: the clip opened at its
  page, stopped (`unwanted`) when the page was left (180) and when the viewer closed (260), opened again from its start
  in between (no `fromSeconds`), `video.viewer.gpuBytes` and `video.videoBytes` 0 at the end; the mix 0.499986 while it
  plays, 0 after leaving its page and after the close.
- **A clip with a failure verdict outside the world's screens** was opened (the verdicts knew only the clips the world
  screened): `ScreenVerdicts::screenClip()` answers for one clip from the failure verdicts and its file's facts (its
  file looked at on the verdicts' thread as `screened()` does, the clip not asked for meanwhile), never resetting the
  world's pending change. `ScreenVerdicts` moved to `rv_app_catalog` (its header public) for its unit test. GPU: Opal
  Oriel's damaged clip from the lobby: `verdict unsupported-codec`, 0 decoders.
- **The manual's plan** also refuses an element whose stem is a reserved device name (any case and extension, `COM0`
  and `LPT0` and the superscript forms included), a colon past the drive's, and any comma; explorer.exe's argument is
  always quoted (`LaunchRequest::quoteEveryArgument`, `launcher::appendAlwaysQuotedArgument`). A refused path is
  `ManualState::Unsupported` with its own sentence ("Retroverse non apre questo manuale: il percorso del file contiene
  una virgola, due punti o un nome che Windows leggerebbe in un altro modo."), event `unsupported <code>`.
- **One explorer.exe a press**: the page is `Opening` while a start runs and ignores Activate then and for
  `media_viewer::kManualReopenSeconds` (2 s, the panel's monotonic clock) after the manual opened.
- **The document**: "In pausa" at the clip's corner (`#media-video-box`); "Motivo: <code>" only beside the generic
  sentence; the fallbacks and the manual's page one width (760 dp, at most the stage's); the empty viewer "Nessun
  contenuto".
- **Tests**: the videos case checks `video.viewer.gpuBytes` 0 after the close and the secondary TV's slot stopped
  `needed`; `media_viewer.json` has its Right at the last page again (frame 106, no page event: the unit test's "no
  wrap" covers the model, this the app). New unit cases: `[screen_verdicts]` (4), the manual's debounce
  (`[media_viewer]`), the always-quoted arguments (`[command_line]`, `CommandLineToArgvW` over 3,000 random lists in
  both modes), the device names, colons and commas (`[manual_open]`: 18 more paths refused, 3 names close to device names
  allowed, every argument quoted).

Results after the review (main merged: the goldens re-baselined to driver 621.01): Debug unit 2223 of 2223 passed;
Debug GPU, the six `media viewer` cases, passed; Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`)
2394 tests (2223 unit, 171 GPU), all passed, G01 to G20 compared and passed, 1 skipped (the overlay's pick, as
always); both builds without a warning.

Screenshots again (`build/i5/step8/`): `videos_paused_f330.png` (the badge at the clip's bottom right corner),
`manuals_refused_f412.png` and `damaged_known_verdict.png` (the two panels one width; the unsupported codec's sentence
without "Motivo"), `empty_viewer.png` (Pocket Lighthouse of `esde/snes-single`, no media: "Nessun contenuto").

## Open issues

- A real manual opened by hand (the user's PDF program, where its window opens over Retroverse's) and a real clip in
  the viewer are the user's to try; no automated run may open either. The real library was not run for this step.
- In-app manual pages still need a permissive PDF renderer and an ADR (BACKLOG).
