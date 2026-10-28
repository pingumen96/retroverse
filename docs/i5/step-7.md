# I5 step 7: missing media

Roadmap row: I5 step 7. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §8 ("Video verdicts", "media
--check-videos"), settling ADR-0004's corruption and truncation thresholds on the whole library and its open "hour-long
soak".

## What was built

- **The verdicts** (`rv_media`, `video_verdicts.hpp`): `<user data>/cache/media/videos.json`, canonical JSON
  (`core::writeCanonicalJson`: integers only, one verdict per line), one verdict per clip keyed by its path under the
  media root (compared folded, as the file system compares names) and its file's size and last write time as its
  folder lists them. Each holds `playable` or the decoder's failure (`invalid-file`, `no-video-stream`,
  `unsupported-codec`, `corrupt-data`, `truncated`, `decoder-error`), the duration, coded size, audio rate and codec,
  the decode errors and damaged frames counted, and its source (`check` or `play`). `updateVideoVerdicts()` merges a
  writer's verdicts into the file as it is (read, merge, `writeTextFileAtomic`) under a named mutex of the file's path,
  so the app and the CLI never drop each other's verdicts.
- **The check of a clip** (`rv_media`, `video_check.hpp`): `checkVideo()` runs the screens' own `VideoDecoder` (120
  rows, no loop, no sound) over a sample: the first 2 s, 1 s from the middle and the last second; a clip that declares
  at most 7 s is decoded whole. The sample's last frame is held to the decoder's truncation rule (90% of the video
  stream's own duration, less 0.5 s; `StreamInfo::streamDurationSeconds` is new, the rule's constants now public), so a
  file cut short whose index is intact fails without being decoded whole. Each clip within 60 s; cancellable.
- **`retroverse-cli media --check-videos [--system <name>] [--jobs N]`** (`cmd_media_videos.cpp`): every file of ES-DE's
  video extensions in the systems' videos folders (found as `EsdeMediaResolver` finds them) without a verdict for its
  file as it is now, checked on `--jobs` threads (2 by default) with the process at below-normal priority; verdicts
  merged every 25 clips or 20 s, so an interrupted run resumes; verdicts of clips no longer in a folder listed whole
  removed. The report (text or `--json`): per system the clips, checked now, known and unchecked, and each verdict's
  count; the failures with the decoder's reason; the "near misses" (playable with errors or damaged frames under the
  thresholds); the clips whose check did not finish. Nothing is written under the media root.
- **The screens** (`rv_app`, `screen_verdicts.cpp`): the service reads the file on a thread of its own when it starts
  (a `--fixed-dt` run waits for it). A game whose clip has a failure verdict for its file as it is now loses its clip
  (`ScreenMedia::video` and the new `videoKey` cleared) before any policy sees it: never opened, never in a playlist,
  its screens on its stills from their first picture. A clip that fails on a screen (but a missing file), or plays
  through once from its start with no decode error or damaged frame, gives a verdict the thread merges into the file.
  The run report's `video.verdicts` (`loaded`, `entries`, `knownBad`, `given`, `written`, `writeFailures`).
- **Fixtures**: `tests/fixtures/esde/video-verdicts` (eight invented SNES games: one clip of each damaged kind, two
  playable, a screenshot each, Ember Quill a favourite); the fixtures' clip payloads gain `no-video` (a `.wav`'s bytes
  under a clip's name) and `not-a-video` (text).
- **`scripts/real-library.ps1 -FullVideoScan`** sets `RV_REAL_FULL_VIDEO_SCAN=1`: `[real-library][videos]` then checks
  every system's clips instead of the SNES ones.
- **The `[.soak]` hour** (`tests/smoke/test_soak.cpp`, informative).

## Decisions beyond ADR-0016

- **The rows of a verdict.** "Its duration, size and audio rate": the video stream's duration in milliseconds (the
  canonical form holds integers only), its coded width and height, the first audio stream's rate; also the codec, the
  decode errors and damaged frames (for the thresholds) and the source. The file's "time" is its last write time in
  FILETIME ticks, as `core::walkTree` and `core::listedFileFacts` give it (the directory entry's).
- **The sample's end.** ADR-0016 names the first 2 s, 1 s from the middle and "the streams' durations". The check also
  reads the last second, and a stream that ends after either seek has its last frame held to ADR-0004's truncation
  rule against the video stream's own duration: without it a file cut short, its index intact, passed the check and
  failed on a screen (cut past its middle until this step's end, cut before it until the review). A clip up to 7 s is
  decoded whole (the decoder's own first-pass rule applies).
- **Which clips the CLI checks.** The files of ES-DE's video extensions in each system's videos folder, as listed (not
  the catalog's games: no import is needed, and a clip ES-DE would show for no game is checked too). The app derives a
  clip's path from the catalog; both are compared folded.
- **Which clips the app judges.** A failure: what the decoder reports once its frames are shown, but `file-not-found`
  (no file, no verdict), `read-error` (the system could not open, read or seek the file: since the review) and a
  texture that could not be made (the GPU's, not the clip's). Playable: the first pass of a
  clip opened at its start (not resumed at a position, not frozen) ended with no error and no damaged frame; a clip
  the playlist leaves before its end gives none. A playable verdict already in the file is not written again.
- **When the app looks at a file.** Only a failure verdict's file is looked at (its size and time, once per clip and
  run, on the verdicts' thread since the review: the clip is left out meanwhile, and comes back if its file changed):
  the other clips are opened as before. The verdicts are read off the main thread; an interactive run's first room may
  open a known-bad clip once more if the file is read after the room is bound (it is then left out at the next
  frame).
- **The keys.** `ScreenMedia::videoKey`, the clip's path under the media root as the room's items name it (`snes/videos/
  X.mp4`); a mock catalog's clips have none, so the test room and the mock catalogs' cases never read or write
  verdicts.
- **Two writers.** A named mutex (`Local\RetroverseVideoVerdicts-<hash of the file's folded path>`) around read, merge
  and write, which every read takes too since the review; the app waits for it at most 2 s (a write that fails is
  counted, `writeFailures`), the CLI 10 s (a merge that fails is tried again at the next).
- **The scan's log.** While it runs the default logger is raised from info to warn: a scan opens thousands of decoders,
  each an open and an end line (named after its file, so that `--log-level debug` names a clip that troubles one).
  Progress goes to stderr every 30 s.
- **The soak's frame rate.** The app has no frame limiter and a hidden window does not wait for vsync: the soak renders
  as fast as it presents (about 150 frames a second in Debug at 960 x 540), which loads the machine more than a user's
  hour does and makes it the stronger test.

## Proofs

- Unit:
  - `tests/unit/media/test_video_verdicts.cpp` (6 cases): the verdicts' names; keys folded (another case finds the
    clip, another name none), a change of size or of time is checked again, a new verdict of a path spelled otherwise
    replaces the old one, malformed paths and `file-not-found` refused; **the canonical text read back byte for byte**
    (and its exact text); texts other than the canonical form refused (another version, verdict, source, a negative
    size, a fraction, an unknown member, two verdicts of one path, white space); two writers merging into one file, a
    removal, an unreadable file replaced, a malformed change writing nothing.
  - `tests/unit/media/test_video_check.cpp` (4): each damaged kind's verdict (empty and text `invalid-file`, a sound
    file `no-video-stream`, an unknown codec `unsupported-codec`, a cut `truncated`, damage `corrupt-data`, a missing
    file `file-not-found`), a 20 s clip checked at its head, middle and end (42 of its 200 frames, two seeks), a 20 s
    clip cut at 70% failing as truncated, cancelled and timed-out checks without a verdict.
  - `tests/unit/cli/test_cli_media_videos.cpp` (3): **the CLI on the video-verdicts fixture gives each damaged kind its
    verdict** (2 playable, 2 `invalid-file`, 1 each of `no-video-stream`, `unsupported-codec`, `corrupt-data`,
    `truncated`) in the file and the report, text and JSON, the media tree byte for byte unchanged; a second run checks
    nothing and leaves the file's bytes; a clip replaced (another size) and one touched (another time) checked again; a
    clip gone loses its verdict; the usage errors.
  - `test_run_report.cpp`: the `verdicts` section's round trip.
- GPU (`tests/smoke/test_video_verdicts.cpp`, `[video_verdicts]`, 2 cases, `esde/video-verdicts` with one clip kept):
  - an unsupported clip, the room's only one: the first run starts 1 decoder, falls back to the screenshot and writes
    the verdict (`unsupported-codec`, source `play`, the file's facts); **the second run starts 0 decoders** and no
    decoder thread, records no video fallback, lists the game in `knownBad`, and **its screen shows the game's
    screenshot as its first picture**, never a video nor its pattern (`idleFramesWithGame` 0; screenshot in
    `build/<preset>/smoke/video_verdicts/known_bad/second.png`); the file's time changed, the third run opens it again;
  - a 3 s clip played 4 s: `playable` given and written (duration 3000 ms, `mpeg4`); a second run gives nothing.
- Real library (`[real-library][videos]`, `-FullVideoScan`): below.
- Soak: below.

## A fix the scan found: frames padded past their last row

The first full scan crashed `rv_tests.exe` with a heap corruption (0xC0000374) after about 10,000 clips; a rerun with
one job and `--log-level debug` (through `real-library.ps1 -Measure`) stopped on the same real 140 x 96 clip, and a
generated 140 x 96 clip does the same: converted unscaled, swscale's SIMD converters write whole blocks of pixels past
the end of a row that is no whole number of them, and the last row's overrun went past the frame's allocation. The
screens were exposed too: 7,121 of the library's 32,809 clips have a width that is no multiple of 8, and a clip at most
480 rows tall plays at its own size. `VideoFrame` now allocates 256 bytes (64 pixels) past its last row (a row's
overrun lands in the next row, written after it); `test_video_decoder.cpp` decodes widths of 140, 142, 150 and 186
unscaled and two clips scaled to odd widths (it crashed before the fix, in Release).

## Review

Two reviews found no blocker; the fixes (ADR-0016 §8's amendment of the review records the final behaviour, and the
ADR-0004 entries under its amendments):
- **Readers take the writers' lock.** A rename over a file another program holds open fails, whatever that program
  shares, so the app's startup read and the CLI's first read could make a writer's replace fail; `readVideoVerdicts`
  now reads under the named mutex (and shares everything). A file that cannot be read (held, a device error, too large,
  the lock not had) is never replaced: only a text that is not the verdicts' is (`isUnreadableText`). The CLI stops at
  once when the file cannot be read, and keeps a batch whose merge failed for its next merge instead of ending the
  scan. `test_video_verdicts.cpp`: 150 reads against 150 writes from another thread, none failing; a file held without
  sharing, one too large and a folder in its place, each refused and left as it was.
- **A fault of the system is no verdict.** The decoder reports `ReadError` (new) when the system fails an open, a read
  or a seek (`EACCES`, `EIO`, `EBUSY`, ...: the file protocol's errno, or the error the I/O context recorded), never
  `invalid-file`, `truncated`, a decode error or `decoder-error`; a failed read no longer reads as a file cut short.
  Neither the app nor the CLI records a verdict for it (`test_video_check.cpp`: a clip another program holds without
  sharing gives `read-error` and no verdict, then checks playable once released). `retroverse-cli media --check-videos
  --recheck-failures` checks the failures again; deleting the file forgets every verdict, and an FFmpeg upgrade raises
  the verdicts' version (`video_verdicts.hpp`).
- **A clip cut before its middle** passed the check: its middle's seek found nothing to read and ended the stream with
  the truncation rule off (a pass after a seek). The rule now holds every stream that ends after a seek
  (`test_video_check.cpp`: a 20 s clip cut at 30% fails as `truncated`, its last frame before 9 s).
- **The app's verdicts** keep the failures whole and, of the playable clips, their keys and files' facts (not the whole
  document); a playable verdict is written unless the file holds one for the clip's file as it is now (`findCurrent`'s
  rule, on the thread); a failure's file is looked at on the thread, not the main one (above). `video.fallbacks` keeps
  its first 512 entries, as the other timelines.
- **The leak: a thread per clip.** `tests/platform/test_thread_soak.cpp` (`rv_platform_tests "[soak]"`, Release, 1,500
  threads each): a thread started and joined costs nothing without a GL context or with an idle one, and 36 to 73 KiB
  when the main thread presents frames meanwhile (a debug context alike: about 73 KiB each in one run); the same work on
  one thread that lives on, 128 bytes a job. In the app the memory went to a private region outside every Win32 heap (a
  census of a run's regions: 11.6 MiB in 8 minutes, while the CRT heap's small blocks held steady and its large ones
  were the decoders' frames and the per-frame samples). The decoders now run on worker threads the process keeps
  (`video_decoder.cpp`, `DecoderWorkers`): an idle worker takes the next decoder, a new one is started only when none is
  idle; `stop()` never waits, `threadRunning()` and `waitForStoppedDecoders()` follow the decoder's run, so the
  scheduler's budget, the stop before a launch, the slots' reopening once their thread ended and the minimized stop are
  unchanged.
- **The per-frame samples** (`core::DurationRecorder`): exact for the first 65,536 samples of a series (6 to 7 minutes
  at 165 frames a second), then a histogram of logarithmic buckets 1% wide (about 17 KiB); a percentile is its
  bucket's upper bound, never below the exact value and within 1% of it, the count and the maximum exact
  (`test_cpu_timers.cpp`). The frame times, GPU passes and CPU sections use it.
- **The soak** samples handles and threads beside the private bytes (`memory.csv`; handles flat between 463 and 477,
  threads 12 to 27 in every run of the review). Before and after (Release, the soak's scene and settings,
  `build/i5/step7/fixleak`): before, 263.8 MiB (minutes 1 to 3) to 272.8 MiB (the last 2 of 6.7 minutes), +9.0 MiB with
  133 decoders; the workers alone, +8.2 MiB in 13 minutes (the samples of 130,000 frames); both fixes, 258.0 MiB to
  258.7 MiB in 15 minutes with 294 decoders and 150,000 frames, every sample between 255.2 and 267.9 MiB.
- Optional and not done: a marker in the frames' padding (the decoder's case still proves the widths that crashed).

## Results

- Debug unit tests: 2091 of 2091 passed (after the fix above). Release unit and GPU (`msvc-x64-release-gpu`): 2224,
  one skipped by design (133 GPU cases, the 2 verdict cases included). G01 to G18 0 pixels apart: no golden changed
  (the test room and the mock catalogs have no verdicts).
- After the review: Debug unit tests 2095 of 2095; Release unit and GPU 2228, one skipped by design (133 GPU cases);
  no golden changed. The SNES clips checked again from scratch on the decoders' workers (`[real-library][videos]`,
  with the 96 kHz sync case `[real-library][audio]`): 1,470 checked in 475 s (beside a Debug test run), **1,470
  playable**, the sync case within its limits, 0 differences.
- **SNES** (`real-library.ps1 -Filter '[real-library][videos]'`, Release, 2 jobs): 1,470 clips checked in 364 s, **1,470
  playable**, no failure of any kind, no near miss (no clip with a decode error or a damaged frame), none unfinished;
  1,377 are H.264 640 x 480, the others H.264 from 256 x 224 to 584 x 446; sound at 48 kHz (976), 96 kHz (465), 44.1
  kHz (25) and 24 kHz (4); 12.8 to 115 s long, 35.8 s the median. 0 differences among 542,120 entries.
- **The whole library** (`-FullVideoScan`, 3 jobs; the first run crashed as above after 5,370 verdicts, which the
  second run kept and resumed from): **32,809 clips in 44 systems, 32,806 playable, 2 `invalid-file`, 1
  `corrupt-data`**, no `no-video-stream`, `unsupported-codec`, `truncated` or `decoder-error`, no near miss, none
  unfinished; 27,439 checked in 6,822 s (0.25 s a clip); 32,661 H.264, 72 HEVC, 56 MPEG-4 Part 2, 17 VP9, one PNG
  stream, all decoded. 0 differences among 542,120 entries (both runs).
  - The two `invalid-file` clips are not videos: a file of 0 bytes (a Game Boy clip), and one of 53 bytes FFmpeg
    finds no container in (an Apple II clip; a scraper's error text, probably).
  - The `corrupt-data` clip (an Amstrad CPC one, H.264 640 x 480, 30.7 s, 96 kHz sound) holds a run of NAL units whose
    sizes read 0 ("Invalid NAL unit size (0 > 547)"): 3 decode errors among 32 events, the threshold, and its picture
    would break up there. A real fault, not a false positive.
  - **ADR-0004's thresholds stand**: no playable clip of the library counted a single decode error or damaged frame,
    so no clip sits near either threshold; the truncation rule found nothing (no clip cut short).
- **The soak** (Release, an hour, 960 x 540, about 165 frames a second: 592,829 frames, 1,102 decoders started, at most
  2 open, 4 fallbacks of the cut clip, joined at exit): **the private bytes were not flat**: 277 MiB (minutes 5 to
  10) to 387 MiB (the last 5), about +110 MiB an hour, steadily; the case's 32 MiB check failed. Two causes, found and
  fixed in the review (below). The run report's per-frame samples (the frame's time, 6 GPU passes and 3 CPU sections:
  about 80 bytes a frame, some 45 MiB of the hour at 165 frames a second) and the clips: about 60 to 80 KiB per clip
  opened net of the samples (the hour's 110 MiB less about 45 MiB over its 1,102 decoders; a 400 s run of the review,
  15 MiB less about 4.5 MiB over its 133 decoders), 100 to 115 KiB gross. Bisected with 6 and 10 minute runs of the
  same scene (`build/i5/step7/leak`): playing without switching clips grew no more than the samples, as did a run
  without clips (its stills and cards switching); the sound on or `audio.mode` off alike, and one decoder at half the
  rate. The same switching in a Debug build grew far less, and `rv_media` alone in Release (900 decoders started,
  stopped, seeked, through `VideoPlayback`) grew 1 MiB: both without a GL context presenting frames, or presenting
  fewer of them.

## Open issues

- The soak runs uncapped (no frame limiter; a hidden window does not wait for vsync), heavier than a user's hour.
- An interactive run whose verdicts are read after its first room is bound may open a known-bad clip once (then left
  out at the next frame); lockstep runs wait for them.
- Two writers race only within the lock's wait: the app gives up after 2 s (`writeFailures`) and its verdicts of that
  batch are lost for the run (given again at the next failure); the CLI keeps them for its next merge.
- Clips that ES-DE would not use (a `.mkv` beside an `.mp4` of the same name) are checked too.

## Notes for later steps

- Step 8 (the viewer's video page): `ScreenMedia::videoKey` and `media::readVideoVerdicts`/`VideoVerdicts::findCurrent`
  give a clip's verdict; the page's fallback names its `verdictName`.
- I7's media coverage report can read `cache/media/videos.json` as it is (canonical, documented in
  `video_verdicts.hpp`).
- Gate M4's `-FullVideoScan` forwards to `real-library.ps1 -FullVideoScan`; the scan resumes in
  `build/<preset>/real-library/videos` (delete it to check every clip again: about two hours with 3 jobs).
