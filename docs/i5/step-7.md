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
  reads the last second and holds its last frame to ADR-0004's truncation rule against the video stream's own
  duration: without it a file cut short past its middle, its index intact, passed the check and failed on a screen.
  A clip up to 7 s is decoded whole (the decoder's own first-pass rule applies).
- **Which clips the CLI checks.** The files of ES-DE's video extensions in each system's videos folder, as listed (not
  the catalog's games: no import is needed, and a clip ES-DE would show for no game is checked too). The app derives a
  clip's path from the catalog; both are compared folded.
- **Which clips the app judges.** A failure: what the decoder reports once its frames are shown, but `file-not-found`
  (no file, no verdict) and a texture that could not be made (the GPU's, not the clip's). Playable: the first pass of a
  clip opened at its start (not resumed at a position, not frozen) ended with no error and no damaged frame; a clip
  the playlist leaves before its end gives none. A playable verdict already in the file is not written again.
- **When the app looks at a file.** Only a failure verdict's file is looked at (its size and time, on the main thread,
  once per clip and run): the other clips are opened as before. The verdicts are read off the main thread; an
  interactive run's first room may open a known-bad clip once more if the file is read after the room is bound (it is
  then left out at the next frame).
- **The keys.** `ScreenMedia::videoKey`, the clip's path under the media root as the room's items name it (`snes/videos/
  X.mp4`); a mock catalog's clips have none, so the test room and the mock catalogs' cases never read or write
  verdicts.
- **Two writers.** A named mutex (`Local\RetroverseVideoVerdicts-<hash of the file's folded path>`) around read, merge
  and write; the app waits for it at most 2 s (a write that fails is counted, `writeFailures`), the CLI 10 s.
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

## Results

- Debug unit tests: 2091 of 2091 passed (after the fix above). Release unit and GPU (`msvc-x64-release-gpu`): 2224,
  one skipped by design (133 GPU cases, the 2 verdict cases included). G01 to G18 0 pixels apart: no golden changed
  (the test room and the mock catalogs have no verdicts).
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
  2 open, 4 fallbacks of the cut clip, joined at exit): **the private bytes are not flat**: 277 MiB (minutes 5 to 10)
  to 387 MiB (the last 5), about +110 MiB an hour, steadily; the case's 32 MiB check fails. Bisected with 6 and 10
  minute runs of the same scene (`build/i5/step7/leak`): playing without switching clips grows about 5 MiB in 400 s,
  as does a run without clips (its stills and cards switching): the run report's per-frame samples (frame time, GPU
  passes, CPU sections: about 75 bytes a frame, by design); switching clips every 4 s adds about 21 MiB in 400 s (some
  150 KiB per decoder opened), with the sound on or `audio.mode` off alike, and with one decoder at half the rate. The
  same switching in a Debug build stays flat, and `rv_media` alone in Release (900 decoders started, stopped, seeked,
  through `VideoPlayback`) grows 1 MiB: the growth is the app's, in Release, per clip opened. Not found in this step
  (an open issue below).

## Open issues

- **Private bytes grow about 150 KiB per clip opened in a Release app** (the soak, above). At the default
  `previewDuration` (30 s) on two screens that is some 30 to 40 MiB an hour. Next: an allocation trace of a Release
  run (the CRT's or ETW's heap tracing) around `ScreenService::openSlot`/`releaseDecoder`, the stills' textures and
  the material overrides; and bound the run report's per-frame samples (a reservoir or a histogram, as the mixer's
  sync errors) for interactive runs.
- The soak runs uncapped (no frame limiter; a hidden window does not wait for vsync), heavier than a user's hour.
- An interactive run whose verdicts are read after its first room is bound may open a known-bad clip once (then left
  out at the next frame); lockstep runs wait for them.
- Two writers race only within the lock's wait: the app gives up after 2 s (`writeFailures`) and its verdicts of that
  batch are lost for the run (given again at the next failure).
- Clips that ES-DE would not use (a `.mkv` beside an `.mp4` of the same name) are checked too.

## Notes for later steps

- Step 8 (the viewer's video page): `ScreenMedia::videoKey` and `media::readVideoVerdicts`/`VideoVerdicts::findCurrent`
  give a clip's verdict; the page's fallback names its `verdictName`.
- I7's media coverage report can read `cache/media/videos.json` as it is (canonical, documented in
  `video_verdicts.hpp`).
- Gate M4's `-FullVideoScan` forwards to `real-library.ps1 -FullVideoScan`; the scan resumes in
  `build/<preset>/real-library/videos` (delete it to check every clip again: about two hours with 3 jobs).
