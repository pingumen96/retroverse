# I5 step 2: the decoder scheduler

Roadmap row: I5 step 2. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §2 (amending ADR-0004
points 4 and 9, ADR-0012 §7, ADR-0013 §9).

## What was built

- **`app::ScreenScheduler`** (`rv/app/screen_scheduler.hpp`, GPU-free in `rv_app_cli`). Each frame it takes the
  screens' requests (clip, class, distance, in view) and the slots' feedback (thread running, presented, late and
  dropped frames) and returns commands (open, stop, pause, resume) and each screen's slot.
  - Requests for one clip share a slot. Clips rank by being in view, then class (`direct`, `focus`, `room`,
    `ambient`), then the nearest screen, then the smallest screen id.
  - A clip out of view keeps its slot paused (paused on the frame its last screen leaves the view) and stops 10 s
    after that frame, or sooner when a request in view needs the slot. Out of view it takes only a free slot, never
    one another clip holds.
  - A slot changes hands at most once a second unless `focus` or `direct` asks. A clip no screen wants stops at once.
  - `ambient` requests count within `proximityMeters` and leave 0.5 m beyond it.
  - Late plus dropped frames over 5% of a slot's frames over 4 s of play drop the budget to 1 for 30 s; the
    lowest-ranked clip stops (`budget`).
  - A stopped slot drains: it opens again only once the service reports its thread ended.
- **`ScreenService` on several screens and slots** (`screen_service.hpp/.cpp`).
  - `findScreens()` binds every `crt` with a picture: the hero first (the first `crt`, or the arcade's hero cabinet),
    the others as secondaries (in a scene with areas, those in the hero's area). Each screen has its own
    `ScreenPolicy`, stills, held picture and texture override.
  - A slot owns a decoder at `video.json`'s `maxRows` and a `gl::VideoTexture`; screens of one clip show one texture.
  - One render frame uploads at most 4 MiB of video, the longest pending frame first; the first upload always goes.
  - NFR-07: open decoders never above the budget, live decoders never above the slots (asserted, logged once, reported
    as `maxOpenDecoders` and `maxDecoderThreads`).
  - Launch: every slot stops and is joined within 1 s in all. Each screen's policy keeps its clip's position, and the
    clips reopen there after the return.
- **`ScreenPolicy`**:
  - `startChoice` (false for secondaries: their playlist first);
  - `setNoSlot()` (the stills and the card stand for a clip without a slot);
  - `videoStopped()` (the still again after a slot stopped without a failure).
- **`video.json`**: `rooms.<room id>.screens.<screen id>` takes a kind's keys for one screen. `maxDecoders`, `maxRows`
  and `proximityMeters` are now used.
- **`--max-decoders 1|2`**: the budget over `video.json`'s `maxDecoders`.
- **The process priority**:
  - `rv_platform_win`'s `setProcessPriority` / `currentProcessPriority`;
  - `app::processPriorityFor` decides. Above normal while the view renders, normal while minimized, from the launch
    hook (before the emulator's process) until the first frame after the return, and at the exit.
  - The report's `processPriority.changes` records each change, read back from the system.
- **The report**:
  - `video` gains `maxDecoders`, `maxOpenDecoders`, `handOvers`, `maxUploadBytes`, `deferredUploads`,
    `oversizeUploads`, `screens` (each screen's slot, picture and texture), `slotEvents` and `budgetDrops`;
  - the hero's fields keep their meaning.
- **`tests/smoke/scenes/screens_four.json`**: four CRTs on TV cabinets, the room `screens_four`.

## Decisions beyond ADR-0016

- **A screen switching clips waits for its own slot.** It takes the slot it showed before, even while that slot
  drains, rather than a free one. A single screen keeps one decoder thread and one texture, as in step 1 (the
  test room's runs still report `maxDecoderThreads` 1). A free slot opens at once for a clip none of whose screens had
  one.
- **Positions are kept.** A clip stopped while still wanted (`out-of-view`, `needed`, `budget`) reopens where it
  stopped, as after a launch. The screen shows its still meanwhile; the reopened first frame replaces it.
- **Per-screen keys in `video.json`.** The smoke scene needs screens of one kind playing different games. A user may
  pin a screen too.
- **Secondaries are any further `crt` in the room** until step 3's `secondary_tv` piece. No shipped scene or floor has
  one, so no golden changes.
- **Minimized counts as out of view.** The slots pause, and they stop after 10 s like a screen turned away. This frees
  the decoders and their CPU, where ADR-0016 says only "pause".
- **Visibility is the frustum alone.** The service only holds the screens of the room bound (the eye's, or the last
  one's in a hall), which stands for "its area holds the eye"; I4 step 6b's hall behaviour is kept.
- **A clip out of view may open, paused, in a free slot** within its 10 s. ADR-0016 only says a screen leaving the
  view pauses. The hero's selection made while its CRT is out of view (an item inspected close up) opened paused with
  step 1's one decoder, and the world launch case expects it; it never takes a slot another clip holds.
- **The late-frame window** needs 4 s of unpaused play and at least 30 frames. Paused time clears it.
- **The upload budget's oldest-first order** is by the render frame a slot's frame began waiting, not by timestamp. A
  superseded frame keeps its slot's place.

## Proofs

- Unit:
  - `test_screen_scheduler.cpp` (12 cases): the four-CRT table (2 slots, a and b sharing, c without); one decoder; the
    pause on the turn and the stop 10 s later; a request in view taking a paused slot, opened only after its
    thread's 7 more updates; the hand-over rule under swapping distances, and `focus` at once; the hysteresis walk; the
    late-frame drop at 4 to 8 s and the budget back after 30 s; `stopAll`; a screen switching clips on its own slot; a clip out of view opening paused in a free slot only;
  - 40 seeded runs of 3000 frames: open decoders never above the budget, threads never above the slots, every open
    after its slot's thread ended;
  - `test_screen_policy.cpp`: secondaries' playlists and the card without a slot. `test_video_settings.cpp`:
    per-screen keys. `test_command_line.cpp`: `--max-decoders`. `test_run_report.cpp`: the round trip.
    `test_process_priority.cpp` (app and platform_win).
- GPU (`tests/smoke/test_screens.cpp`, `[screens]`, 5 cases):
  - 3 clips on 4 screens: 2 decoders started, crt_a and crt_b on slot 0 with one texture id, crt_c on slot 1, crt_d on
    its cover;
  - turned 40 degrees at frame 60: crt_c's slot paused at frame 60, stopped `out-of-view` 10.0 s later, never taken by
    crt_d; turned back at frame 720: reopened on that frame at its position (the counter reads `U 0053` 50 frames
    later);
  - `--max-decoders 1`: one decoder, crt_c and crt_d on their covers;
  - two 1280 x 720 clips at 60 fps with `maxRows` 720: `maxUploadBytes` 3,686,400 (one frame), every other upload
    put off a frame, none oversize;
  - the fake emulator's game: priority changes above-normal (frame 0, rendering), normal (launch, frame 37, the
    game's start frame), above-normal (rendering, frame 37 again: no frame renders while the game runs, and the log
    shows the game's 2.5 s between the two), normal (exit), all applied.
- The step 1 video cases pass unchanged (`maxDecoderThreads` 1 on the single screen). The clean-run checks now hold
  the decoders to the run's budget.

## Results

- Debug unit tests: 1999 of 1999 passed. Release unit and GPU (`msvc-x64-release-gpu`): 2125 of 2125 (one skipped by
  design; 126 GPU cases). G01 to G18 0 pixels apart (the 16 goldens, largest difference 0/255).
- The first Release run caught `world launch: Avvia on the card ...`: the inspected item's clip, its CRT out of view,
  no longer opened (a clip out of view never took a slot), so no decoder ran at the launch. Fixed as above.
- Screenshots (hidden runs, `build/i5/step2/`): `four_screens_two_decoders.png` (crt_a and crt_b on one frame `T 0059`,
  crt_c on its own clip, crt_d on its cover), `four_screens_one_decoder.png`, `turned_before.png`, `turned_away.png`,
  `turned_back_frame721.png` (crt_c on its cover until its first frame), `turned_back_frame770.png` (crt_c's clip
  going on from where it stopped), `four_screens_release_1080p.png`, `screens_four_empty.png` (the scene's standby
  without a catalog).

## Measurements

Against the acceptable rows only (Release, the reference machine, `build/i5/step2/perf`):
- **Frames, two clips playing.** The four-CRT room at 1920 x 1080, `--no-vsync`, 900 frames, two slots open (three
  screens playing, one on its cover): p50 2.95 ms, p99 3.44 ms (acceptable 16.7 / 33.3). `videoUpload` p50 0.41 ms.
- **GPU memory.** 223 MiB in all, the two slots' textures and rings 9.4 MiB (acceptable 512). The busiest floor with
  both slots playing is step 3's measure, when a room first has two screens.
- **Uploads.** Two 720p clips at 60 fps in Debug: 149 uploads over 150 frames, at most 3,686,400 bytes in a frame,
  frame p99 6.2 ms.

The real library was not run: nothing of the catalog, the import or the rooms' plans changed, and every real room
still has one screen.

## Open issues

- The late-frame drop is proven by the unit tables only: no GPU case forces late frames.
- The hero's report figures (`firstFrameMs`, `clipFramesUploaded`, ...) are its last slot's. A slot reopened for another
  screen's clip resets them (only with several screens).
- `slotEvents` keeps its first 512 entries. A long run with playlists on four screens fills it.
