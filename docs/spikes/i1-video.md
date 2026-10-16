# I1 spike (c): video on a texture

- Date: 2026-10-16
- Outcome: the pipeline meets the criteria and needs no fallback for its own cost; ADR-0004 adopts it with two
  conditions for I5. Its work does not move with the machine's load: an upload of a 720p frame costs the render thread
  0.8 ms (p99 1.1 to 1.4 ms in every run), a decoder thread about 5% of a core, and no frame was lost to the pixel
  buffer ring, even at one upload per video and render frame. The 60 Hz frame pacing depends on the other programs:
  in the second campaign, at normal priority, every run with the machine at most 46% busy met it and every run from 51%
  on missed it; at above-normal priority 7 of 9 runs at 36 to 70% met every criterion. Two 720p videos met every
  criterion in two of three runs at either priority, the median run included. A third campaign, after the merge of the
  spikes with no other agent running (the machine 25 to 31% busy, a browser and the editor open), confirmed the load as
  the cause: all 12 runs met every criterion, two 720p videos and the three real ones, at either priority (0 to 10
  intervals over 16.7 ms at normal priority, none at above-normal, no frame late or dropped). ADR-0004 makes
  above-normal priority while rendering a condition for I5 (first condition). Read per render frame instead of per
  upload, two 720p uploads due together reach 2.1 to 2.7 ms at p99, so I5 staggers them (second condition). The first
  frame is on screen 7 to 20 ms after the decoder starts, also from a cold disk cache (NFR-06: 2 s). Corrupt, truncated
  and other unusable files fall back to a still picture without a crash, after the frames decoded before the damage.
- Decision: [ADR-0004](../decisions/ADR-0004-video-pipeline.md)

## Goal

The roadmap's row (c) of I1: prove the video pipeline of specification §9.4 (FFmpeg demuxing and decoding with timestamps
on a worker thread, a bounded CPU frame queue, RGB conversion, a texture upload through pixel buffers on the render
thread, presentation driven by timestamps, a render thread that never blocks) within the budgets of §9.5 and §18.1
(NFR-06: the first preview frame within 2 s; NFR-07: 2 decoders in the normal preset). Success criteria: two 720p
videos with the render loop at 60 FPS for 60 s, a p99 frame time of at most 16.7 ms, uploads under 2 ms, fewer than 1%
late frames, a drift within 2%, stable memory, and a truncated or corrupt file falling back without a crash. Fallbacks:
YUV planes converted in the shader, 480p or a single decoder, prebuilt LGPL FFmpeg binaries.

## What was built

- **`rv_media`** (GPU-free, `src/media`, FFmpeg only in its sources): `VideoDecoder` (one `std::jthread` per video that
  opens the file, demuxes and decodes it: `avcodec_send_packet` / `avcodec_receive_frame` with `EAGAIN`, drain at the end
  of the stream, flush on every loop and seek; swscale to RGBA8 scaled to at most `maxHeight` rows with the aspect ratio
  kept and never enlarged; presentation timestamps from `best_effort_timestamp` in seconds, monotonic across loops through
  a loop offset; failures reported as `DecodeFailure`; the file protocol only, UTF-8 paths, non-ASCII names tested; the
  delays from `start()` to the stream being open and to the first frame queued), `FramePool` and `PooledFrame` (queue
  capacity + 2 reusable 64-byte aligned frames), `FrameQueue` (4 frames: the decoder waits, the render thread never
  does; only the consumer removes frames), `PresentationClock` (which frame is on screen at a render time: newest due
  frame, dropped, late, repeated, resync, pause, seek generations), `VideoPlayback` (a decoder and its clock for the
  render thread), `SampleRecorder` (percentiles without allocation), `scaledFrameSize`, and the synthetic fixtures of
  `test_video.hpp` (clips encoded at run time with FFmpeg's native MPEG-4 Part 2 or MJPEG encoders, with the frame
  number in large digits and as a row of 12 black or white blocks that `decodeFrameCounter` reads back, optionally with
  silent PCM audio; truncated, corrupt, empty, text, audio-only and unknown-codec files).
- **Control without waiting** (after the review): the render thread's `pause()`, `seek()` and `stop()` never wait for
  the decoder thread. `seek()` discards the queued frames of the old position on the caller's thread and publishes a new
  generation; the decoder thread only appends to the queue (a frame of an old generation it was about to queue is
  discarded, also while it waits for room), so the clock never reads a frame that another thread removed. The decoder
  decodes from a key frame up to a second before the target and drops the frames before it (see Seeks below); paused, it
  still delivers the frame at the target, which becomes the paused picture. `stop()` hands the thread over and returns;
  `waitForStoppedDecoders()` waits for stopped threads at shutdown. `VideoPlayback::failed()` asks for the fallback
  picture only once the frames decoded before the failure have been shown.
- **`gl::VideoTexture`** (`src/render`, main thread only): an immutable `GL_SRGB8_ALPHA8` texture the size of the video
  refilled from a ring of 3 pixel buffer slots in one buffer mapped with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`:
  `memcpy` into the next slot, `glTextureSubImage2D` from it between two `GL_TIMESTAMP` queries, never waiting for the
  GPU (a busy slot skips the upload). The end timestamp is the completion signal by default (`VideoUploadSync`), fences
  are the fallback and an option. GPU upload times, debug labels, reallocation on a size change.
- **`spikes/video`**: `spike_video` on `app::AppHost` (window, OpenGL context, debug output, frame loop) plays N videos
  in a grid of letterboxed quads drawn with an unlit sRGB-correct shader into an sRGB target blitted to a 1920x1080
  window, measures every frame after a 1 s warm-up and writes a JSON report: frame interval and CPU work per frame, the
  intervals over 16.7 ms, upload CPU time per upload and per render frame (all the uploads of a frame), copy and GPU
  times, per video the clock's presented, late, dropped and repeated frames, resyncs, the decoder thread's CPU time from
  `GetThreadTimes`, the startup delays, the drift of the video clock against the wall clock, decode and conversion
  times, the working set and private bytes every second from `GetProcessMemoryInfo`, the machine's load from
  `GetSystemTimes`, the failure and fallback of each video, the GL debug message counts and the vsync mode. Options:
  `--videos <file>...`, `--generate-fixtures <dir>`, `--error-cases`, `--count`, `--fixture-size`, `--fixture-fps`,
  `--fixture-seconds`, `--fixture-codec`, `--fixture-container`, `--seconds`, `--warmup`, `--max-height`, `--queue`,
  `--decoder-threads`, `--upload-fences`, `--check-frames` (the frame counter of every uploaded frame and of every
  picture in the screenshot), `--texture-self-test`, `--priority`, `--no-vsync`, `--hidden`, `--width`, `--height`,
  `--report`, `--screenshot`, `--data-dir`, `--log-level`, `--list-encoders`. `run_measurements.ps1` runs the series
  below and writes their summary.
- **Tests**: 59 Catch2 cases in `tests/unit/media` (queue and pool, generations, the clock on synthetic frames including
  seeks, the paused picture and a source whose frames vanish mid-update, output sizes, statistics, the decoder on clips
  written at test time: order, timestamps and frame counters, loops, scaling, non-ASCII paths, MP4, MKV and AVI, seeks on
  a full queue and while paused, pause, a stop that does not wait, every failure kind, a Matroska clip whose audio
  outlasts its video, a cut Matroska file, the playback's fallback after the last frame, seeks while the render loop
  updates, the fixtures themselves), and two `gpu` checks registered from `spikes/video` (`check_spike_video.cmake`, in
  Debug with GL debug output): `smoke` plays two 720p clips for 3 s and requires no high-severity GL message, the
  counter of the frame each video uploaded last read back from its picture in the screenshot (and the picture not
  black), the `VideoTexture` self-test (exact readbacks of frames of three sizes, the ring found busy by a burst of
  uploads queued behind GPU work, free again after `glFinish`), and timing bounds when other programs leave the machine
  at most half busy (at most 10 intervals over 16.7 ms in 3 s, upload p99 under 3 ms); `errors` plays the damaged
  files, each falling back for its reason, the good clip showing its last uploaded frame. With an upload made to raise
  `GL_INVALID_OPERATION` both checks fail (GL errors, a black picture, the self-test), where the first version of the
  checks passed.

## Method

- **Machine** (the reference machine of the plan): Intel Core i7-9700K (8 cores, 8 threads, 3.6 GHz), 64 GB, NVIDIA
  GeForce RTX 2080 SUPER with driver 617.42 (OpenGL 4.5 core), a 2560x1440 display at 164.84 Hz, Windows 11 (build
  29671). FFmpeg 9.0.2 from vcpkg (libavformat 63.1.102, libavcodec 63.1.102, libswscale 10.1.102, libavutil 61.1.102),
  LGPL, DLLs. `msvc-x64-release` for every number below; Debug for correctness.
- **Runs**: a 1920x1080 window, hidden (vsync still paces a hidden window on this machine: 164.8 FPS), vsync on unless
  stated, 60 s measured after a 1 s warm-up, 3 runs per series, interleaved across the series by
  `run_measurements.ps1`. "Vsync on" is adaptive vsync (SDL swap interval -1, the report's `vsyncMode`): a frame that
  misses its refresh is presented at once, tearing, instead of waiting for the next one, so a late frame costs its own
  delay and not a whole refresh. The machine was shared with other work during the whole session (other agents, a
  browser using one to three cores): every report records the machine's load over the run (`GetSystemTimes`;
  `Get-Counter` cannot be used, its counter names are localized on this Italian Windows) and its load every second.
  Series marked "above-normal" run the spike at `ABOVE_NORMAL_PRIORITY_CLASS` (decoder threads included), so that the
  other programs (normal priority) cannot preempt its threads; the application itself runs at normal priority unless
  I5 changes it (ADR-0004, point 9). The windows were hidden, so they never had the boost Windows gives the foreground
  window.
- **Campaigns**: the first campaign (02:13 to 02:41, the machine 51 to 98% busy) measured every series with the
  original criteria; after the review, a second campaign (04:35 to 04:58, the machine 33 to 76% busy) re-measured the
  criteria series with the metrics added (intervals over 16.7 ms, uploads per render frame, dropped frames, startup
  delays), a series at the display's rate, a single real video and the error cases. An idle machine was not to be had:
  besides a browser keeping about one core busy, other agents built and ran GPU tests during both campaigns, in bursts
  that filled all 8 cores for seconds (each report's per-second load shows them). A third campaign (05:36 to 05:49,
  after the merge of the I1 spikes) re-measured the two 720p fixtures and the three real videos at both priorities
  with no other agent running: only the browser (about 13% of the machine) and the editor (about 4%) besides the spike,
  the machine 25 to 31% busy, its busiest seconds 39 to 55%. The rows of the first and third campaigns are marked; the
  changes between the campaigns touched neither decoding nor uploading.
- **Fixtures**: generated at run time by `--generate-fixtures` under `build/spike_video/<campaign>/fixtures`
  (`measurements`, `measurements2`, `quiet`; never committed): 10 s clips, 30 fps (165 fps for the ring series),
  1280x720 (and 1920x1080 for the headroom series), MPEG-4 Part 2 at quantizer 3 with 2 B-frames and a key frame every
  30 frames, in MP4 (moov first); each loops six times in a run. The LGPL build has no software H.264 encoder
  (`--list-encoders`: native encoders such as mpeg4, mjpeg, ffv1, prores; Media Foundation and Direct3D 12 hardware
  encoders; no libx264).
- **Real videos**, read in place and read-only (FFmpeg's file protocol opens them with `O_RDONLY`) from
  `E:/Programmi/ES-DE_media/downloaded_media`: `snes/videos/1 US - S/Super Metroid (Japan, USA) (En,Ja).mp4`,
  `snes/videos/1 US - A-E/Donkey Kong Country (USA) (Rev 2).mp4`, `ps2/videos/Gran Turismo 4 (Europe, Australia)
  (En,Fr,De,Es,It).mp4`: all three H.264 640x480 yuv420p at 59.94 fps, about 36 s, 1.6, 2.7 and 1.8 Mb/s. For the
  startup from a cold disk cache, three other SNES videos not opened for at least a month (see Startup). The media
  directory is on a SATA SSD (Samsung 870 QVO).
- **Definitions**: *frame interval*, the time between the starts of two render frames (vsync included: 6.07 ms at
  164.84 Hz); *intervals over 16.7 ms*, those longer than a 60 Hz frame; *frame work*, the CPU time of a frame before
  the swap; *upload*, the CPU time of one `VideoTexture::upload` (copy and GL calls), *upload per render frame* the sum
  of the uploads of one render frame (over the frames that uploaded), *copy* its `memcpy`, *GPU upload* the time between
  its two timestamps; *late*, a frame that was already due at the previous render frame but was not decoded yet (the
  decoder fell behind); *dropped*, a due frame skipped because a newer one was due too (a render stall, or a video faster
  than the display); *repeated*, a render frame that kept a frame past its duration because the queue was empty;
  *resync*, a jump of the media time back to a frame more than 250 ms late; *drift*, (video time shown - wall time) /
  wall time between the first and the last frame shown in the window; *decoder CPU*, the decoder thread's CPU time over
  the window, as a share of one core; *open* and *first frame shown*, the delays from `VideoDecoder::start()` to the
  stream being open and to the first presentation of a frame.
- **How the criteria are read**:
  - *p99 frame time at most 16.7 ms at 60 FPS*: the display runs at 164.84 Hz, not 60 Hz. A stall of the render loop
    makes one long interval at any rate, so at 164.84 Hz a p99 over 2.7 times as many frames tolerates 2.7 times as
    many stalls as at 60 FPS: the first campaign's reading (the p99 at the display's rate) was more lenient, not
    stricter. The criterion is read as at most 1% of the 60 frames per second a 60 FPS loop renders, 36 per 60 s, being
    intervals over 16.7 ms (`framesOver16_7MsWithin1PercentOf60Hz`); it implies a p99 of at most 16.7 ms at the
    display's rate, which the report keeps too.
  - *fewer than 1% late frames*: a frame never shown is not on time either. The criterion counts late and dropped frames
    against every frame that fell due, (late + dropped) / (presented + dropped), for each playing video.
  - *upload under 2 ms*: read per upload, the copy and the GL calls of one frame of one video, as the first campaign
    did; the sum per render frame, which is what the render thread spends, is reported beside it and budgeted in
    ADR-0004.
- **Tables**: the median of the 3 runs, with the range of the runs in parentheses where it matters, or the runs one by
  one where the load differed. The campaigns' `summary.md` adds, per metric, the best value any run reached: the best
  values of one series may come from different runs (the first version of this report called them "the best run").
  "Machine busy" is the share of the whole machine's CPU time used by every program during the run (this one included:
  1 to 4% of it).

## Results

Release, 60 s per run after a 1 s warm-up, 3 runs per series interleaved over 23 minutes (the second campaign, unless
marked).

### The criteria (two 1280x720 30 fps videos)

The runs one by one, in the order they ran (other programs' load varied from run to run).

| Criterion | Threshold | Normal priority: runs 1, 2, 3 | Above-normal priority: runs 1, 2, 3 |
|---|---|---|---|
| Machine busy (busiest second) | - | 51.3 (100), 41.9 (90), 35.9 (69)% | 57.9 (99), 38.8 (74), 36.4 (77)% |
| Intervals over 16.7 ms (the 60 FPS p99) | at most 36 | **39**, 21, 4 | 3, **50**, 0 |
| Longest interval / longest frame work | - | 70.7 / 39.6, 82.4 / 30.5, 21.3 / 3.4 ms | 29.0 / 3.1, 33.5 / 3.1, 8.2 / 2.6 ms |
| p99 frame interval at 164.84 Hz | at most 16.7 ms | 9.20, 7.73, 6.32 ms | 6.29, 6.37, 6.21 ms |
| Upload, CPU, per upload: p50 / p99 | under 2 ms | 0.78 / 1.33, 0.75 / 1.32, 0.82 / 1.30 ms | 0.79 / 1.18, 0.83 / 1.29, 0.83 / 1.25 ms |
| Uploads of a render frame, CPU: p50 / p99 | (see below) | 1.54 / 2.68, 0.75 / 1.42, 1.64 / 2.27 ms | 1.61 / 2.08, 1.67 / 2.31, 1.67 / 2.23 ms |
| Upload, GPU: p50 / p99 | under 2 ms | 0.49 / 0.62, 0.65 / 1.42, 0.52 / 0.65 ms | 0.48 / 1.05, 0.52 / 1.34, 0.51 / 0.64 ms |
| Late or dropped, worst video | under 1% | 0.22, 0.17, 0% (no late frame) | 0, 0, 0% |
| Drift, worst video | within 2% | 0.003% in every run | 0.003% in every run |
| Working set over 60 s | stable | -0.75, -0.87, -0.84 MiB (153.3 to 154.6 MiB at the end) | -0.39, -0.61, -0.65 MiB (153.5 to 153.6 MiB) |
| Every criterion met | - | no, yes, yes | yes, no, yes |

Two runs of three met every criterion at either priority, the median run among them. The pipeline's own work did not
change with the load or the priority: the upload's p99 stayed at 1.18 to 1.33 ms, the GPU copy's median at 0.5 ms, the
decoder threads at 4.0 to 5.1% of a core, and no frame was late in any run. What failed twice is the 60 Hz frame
pacing, with other programs (a browser keeping one core busy, other agents' builds and GPU tests) at their busiest: at
normal priority the frame's own work, 2 ms at p99, took up to 40 ms in the runs at 51% and 42% (the thread was
descheduled in the middle of it); at above-normal priority it never took longer than 3.1 ms, and the 50 long intervals
of the run at 39% (all under 34 ms, no frame dropped) happened outside it, in the buffer swap.

The three real videos (480p60 H.264, below) show the priority's effect under similar loads. At above-normal priority all
three runs met every criterion with the machine 49 to 53% busy and its busiest seconds at 95 to 100%: 34, 1 and 3
intervals over 16.7 ms, the frame's own work at most 2.2 ms. At normal priority the run at 40% met them (5 long
intervals); the runs at 71% and 67% did not: 351 and 314 long intervals, 5.0 and 3.7% of a video's frames dropped by
the stalls, the frame's work taking up to 49 ms. Over the whole campaign, at normal priority every 60 s run with the
machine at most 46% busy met the 60 Hz pacing (1 to 21 long intervals in 6 runs) and every run from 51% on missed it
(39 to 351 in 6 runs); at above-normal priority 7 of 9 runs at 36 to 70% met every criterion, and the frame's own work
never took longer than 6.1 ms. The two above-normal runs that missed the pacing (50 and 46 long intervals) lost the
time outside the frame's work, in the buffer swap, which other programs' bursts delay at either priority.

### The criteria with no other agent running (third campaign)

The same series and the three real videos, after the merge, with only a browser and the editor running beside the spike
(see Campaigns); the runs one by one, in the order they ran.

| Criterion | Threshold | Two 720p30 videos, normal: runs 1, 2, 3 | Above-normal | Three real videos, normal | Above-normal |
|---|---|---|---|---|---|
| Machine busy (busiest second) | - | 31.5 (53.9), 25.2 (39.1), 26.0 (42.6)% | 25.3 (47.7), 25.7 (55.5), 26.7 (42.2)% | 28.3 (51.6), 27.6 (41.6), 26.9 (44.7)% | 28.3 (42.0), 27.7 (43.6), 27.3 (40.2)% |
| Intervals over 16.7 ms (the 60 FPS p99) | at most 36 | 10, 2, 0 | 0, 0, 0 | 0, 0, 0 | 0, 0, 0 |
| Longest interval / longest frame work | - | 32.3 / 32.2, 22.1 / 3.1, 7.2 / 2.9 ms | 7.3 / 2.5, 7.2 / 2.3, 6.6 / 2.6 ms | 7.4 / 1.6, 11.1 / 1.4, 7.6 / 1.3 ms | 6.9 / 1.2, 7.3 / 1.4, 11.8 / 1.3 ms |
| p99 frame interval at 164.84 Hz | at most 16.7 ms | 6.34, 6.21, 6.22 ms | 6.19, 6.18, 6.19 ms | 6.22, 6.22, 6.21 ms | 6.18, 6.20, 6.18 ms |
| Upload, CPU, per upload: p50 / p99 | under 2 ms | 0.83 / 1.30, 0.81 / 1.22, 0.81 / 1.21 ms | 0.80 / 1.21, 0.80 / 1.18, 0.81 / 1.20 ms | 0.21 / 0.37, 0.22 / 0.37, 0.22 / 0.37 ms | 0.21 / 0.36, 0.22 / 0.37, 0.21 / 0.35 ms |
| Uploads of a render frame, CPU: p50 / p99 | (see below) | 1.65 / 2.33, 1.63 / 2.16, 1.62 / 2.17 ms | 1.61 / 2.13, 1.61 / 2.09, 1.62 / 2.10 ms | 0.64 / 0.99, 0.57 / 0.96, 0.64 / 0.99 ms | 0.62 / 0.97, 0.61 / 0.97, 0.63 / 0.96 ms |
| Upload, GPU: p50 / p99 | under 2 ms | 0.51 / 0.65, 0.51 / 0.64, 0.51 / 0.64 ms | 0.51 / 0.64, 0.51 / 0.63, 0.51 / 0.63 ms | 0.28 / 0.36, 0.28 / 0.37, 0.29 / 0.36 ms | 0.28 / 0.37, 0.28 / 0.37, 0.28 / 0.36 ms |
| Late or dropped, worst video | under 1% | 0% in every run | 0% | 0% | 0% |
| Drift, worst video | within 2% | 0.003% in every run | 0.003% | 0.001% | 0.001% |
| Working set over 60 s | stable | +0.79, -0.82, -1.04 MiB (153.3 to 155.8 MiB at the end) | -0.80, -0.61, -0.79 MiB (153.5 to 153.6 MiB) | +3.18, +3.10, +3.04 MiB (134.9 to 135.2 MiB) | +2.84, +2.50, +3.26 MiB (134.8 to 135.6 MiB) |
| Every criterion met | - | yes, yes, yes | yes, yes, yes | yes, yes, yes | yes, yes, yes |

Every run met every criterion, at either priority, and no frame was late, dropped or repeated in any of them. The
pipeline's own numbers are those of the loaded campaigns (upload p99 1.18 to 1.30 ms for 720p and 0.35 to 0.37 ms for
480p, GPU copy medians 0.51 and 0.28 ms, decoder threads 4.7 to 4.9% and 8.0 to 8.2% of a core); what changed is the
frame pacing at normal priority, where only the fixtures' runs had long intervals: the run at 31.5% (busiest second
53.9%, the busiest of the normal-priority runs) had 10, the longest a 32.3 ms frame whose own work took 32.2 ms, 31.4 ms
of it in one upload (0.8 ms at the median: the thread most likely lost the CPU in the middle of it, as in the loaded
runs); the run at 25.2% had 2, although its busiest second (39.1%) was the calmest of the campaign, the longest (22.1
ms) outside the frame's work (at most 3.1 ms). The reports keep the distribution of the per-second load, not its
timeline, so the long intervals cannot be tied to particular bursts. At above-normal priority the longest interval was
11.8 ms and the longest frame work 2.6 ms, with busiest seconds up to 55.5%. The working set of the three real videos
grew by about 3 MiB over the minute, as in the second campaign (Memory). Read per render frame, two 720p uploads due
together still reach 2.09 to 2.33 ms at p99: the second condition of ADR-0004 does not depend on the load. The 30 starts
of this campaign opened their streams in 2.6 to 6.7 ms and showed the first frame 6.7 to 13.0 ms after the start.

### Every series

Medians of the 3 runs (the range of the runs in parentheses where it matters); the rows of the first campaign, at
above-normal priority, lack the metrics added since.

| Series | Machine busy | Intervals over 16.7 ms (36 allowed) | Frame interval p99 | Upload per upload p50 / p99 | Uploads per render frame p50 / p99 | GPU copy p50 / p99 | Late or dropped, worst video | Drift, worst | Decoder thread CPU per video | Working set at the end / growth | Runs meeting every criterion |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2x 720p30 fixtures, normal | 42% (36 to 51) | 21 (4 to 39) | 7.73 ms | 0.78 / 1.32 ms | 1.54 / 2.27 ms | 0.52 / 0.65 ms | 0.17% | 0.003% | 4.6% of a core | 153.3 / -0.84 MiB | 2 of 3 |
| 2x 720p30 fixtures, above-normal | 39% (36 to 58) | 3 (0 to 50) | 6.29 ms | 0.83 / 1.25 ms | 1.67 / 2.23 ms | 0.51 / 1.05 ms | 0% | 0.003% | 5.0% | 153.6 / -0.61 MiB | 2 of 3 |
| 3 real videos (2 SNES, 1 PS2: 480p60 H.264), normal | 67% (40 to 71) | 314 (5 to 351) | 24.83 ms | 0.22 / 0.41 ms | 0.64 / 1.14 ms | 0.26 / 0.82 ms | 3.67% (0.06 to 5.03) | 0.001% | 8.9% | 135.0 / +2.65 MiB | 1 of 3 |
| 3 real videos, above-normal | 51% (49 to 53) | 3 (1 to 34) | 6.26 ms | 0.23 / 0.39 ms | 0.68 / 1.01 ms | 0.26 / 0.82 ms | 0.03% | 0.001% | 8.9% | 135.0 / +2.91 MiB | 3 of 3 |
| 1 real video (Super Metroid), normal | 39% (33 to 51) | 2 (1 to 64) | 6.39 ms | 0.23 / 0.38 ms | 0.23 / 0.38 ms | 0.30 / 0.38 ms | 0% (0 to 1.42) | 0.001% | 6.8% | 96.4 / -0.84 MiB | 2 of 3 |
| 4x 720p30 fixtures, above-normal | 56% (36 to 70) | 4 (0 to 46) | 6.26 ms | 0.88 / 1.24 ms | 3.48 / 4.41 ms | 0.50 / 0.65 ms | 0% | 0.003% | 5.4% | 229.7 / -0.83 MiB | 2 of 3 |
| 2x 720p165 fixtures (one upload per video and render frame), normal | 56% (46 to 76) | 69 (8 to 251) | 14.20 ms | 0.86 / 1.30 ms | 1.73 / 2.45 ms | 0.70 / 1.03 ms | 3.84% (0.78 to 17.8) | 0.002% | 28.3% | 154.4 / -0.20 MiB | 1 of 3 |
| 2x 1080p30 fixtures delivered at 1080 rows (first campaign) | 67% | - | 6.57 ms | 1.76 / 2.37 ms | - | 0.93 / 1.66 ms | 0% late | 0.003% | 10.6% | 246.9 / -0.99 MiB | - |
| 2x 1080p30 fixtures delivered at 720 rows (first campaign) | 66% | - | 6.28 ms | 0.81 / 1.22 ms | - | 0.48 / 1.01 ms | 0% late | 0.003% | 21.7% | 167.7 / +2.77 MiB | - |
| 2x 720p30 fixtures with fences (first campaign) | 67% | - | 6.25 ms | 1.29 / 3.02 ms | - | 0.51 / 1.13 ms | 0% late | 0.003% | 3.6% | 153.5 / -0.70 MiB | - |
| 2x 720p30 fixtures without vsync (first campaign) | 60% | - | 2.66 ms at 777 FPS | 0.82 / 1.30 ms | - | 0.49 / 0.91 ms | 0% late | 0% | 5.2% | 154.1 / -0.37 MiB | - |

Per frame on the decoder thread (medians of the runs): the 720p MPEG-4 fixtures decode in 0.71 to 0.86 ms and convert to
RGBA in 0.54 to 0.92 ms; the real 480p H.264 videos decode in 0.59 to 1.15 ms (Super Metroid alone is the cheapest) and
convert in 0.14 to 0.19 ms; in the first campaign 1080p frames decoded in 1.65 to 1.8 ms and converted in 1.58 ms at
full size, or in 5.43 ms when scaled down to 720 rows (swscale's bilinear filter over 2.25 times the pixels: the decoder
thread's 21.7%). The render thread's copy into the pixel buffer is 0.75 to 0.92 ms for a 720p frame (3.5 MiB), 1.75 ms
for 1080p (7.9 MiB) and 0.21 to 0.23 ms for 480p (1.2 MiB). In every run of both campaigns every decoder allocated
exactly its pool's 6 frames and no playing video reported a decoding error or a damaged frame.

### Uploads per render frame

An upload costs the render thread 0.75 to 0.92 ms for a 720p frame at the median (p99 1.13 to 1.39 ms in every 720p run)
and 0.22 to 0.24 ms for a 480p frame (p99 0.36 to 0.48 ms). Videos started together fall due on the same render
frames, and the render thread then pays the sum: the two 720p fixtures did so in 5 of their 6 runs, on every render
frame that uploaded; in the sixth their clocks started a render frame apart and only 17 of 3578 render frames uploaded
both.

| Videos | Render frames uploading two or more | Uploads of a render frame, p50 / p99 |
|---|---|---|
| 2x 720p30, due together (5 runs) | 100% | 1.54 to 1.67 / 2.08 to 2.68 ms |
| 2x 720p30, a render frame apart (1 run) | 0.5% | 0.75 / 1.42 ms |
| 4x 720p30 (3 runs) | 100% | 3.39 to 3.69 / 4.29 to 4.80 ms |
| 2x 720p165 (3 runs) | 98 to 100% | 1.58 to 1.74 / 2.07 to 2.61 ms |
| 3 real 480p60 videos (6 runs) | 89 to 99.6% | 0.62 to 0.71 / 0.99 to 1.15 ms |

Read per upload, as the criterion is read here, the 2 ms budget holds with a third of it to spare at p99. Read per
render frame, two 720p uploads due together exceed it at p99 in every such run, and four exceed it at the median:
ADR-0004 budgets the sum and has I5 stagger 720p uploads.

### The pixel buffer ring at one upload per render frame

The ring of three slots was never busy in the first campaign, but at 30 or 59.94 fps a slot is reused only after 8 or
more render frames. The `fixtures-2x720p165` series plays 165 fps clips on the 164.84 Hz display (normal priority):
each video uploads on almost every render frame (8,065 to 9,825 uploads per video in a minute, on 98 to 99.6% of the
render frames), so each slot is written again three render frames (18 ms) after the GPU was asked to copy from it, as
many frames as a 60 Hz display would give and less time. In its three runs, with the machine 46 to 76% busy, the ring
was never busy and no presented frame was replaced before its upload (`ringBusy` and `supersededBeforeUpload` 0 for
both videos): three slots are enough at one upload per video and render frame. The frames these runs did not show on
time were the clock's doing: dropped when two fell due on one render frame (165 fps against 164.84 Hz: 0.8% in the
run at 46%), late or dropped during the stalls of the runs at 56% and 76% (3.8 and 17.8%). The `RingBusy` path itself
runs in the `VideoTexture` self-test of the `gpu` check: six uploads within one frame, queued behind about 10 ms of GPU
copies, find the ring busy (three skipped, nothing waited for), and the next upload after `glFinish` goes through.

### Startup (NFR-06)

The delays from `VideoDecoder::start()` (the thread starts, opens the file, reads the streams and opens the decoder) to
the stream being described, to the first frame in the queue, and to the clock's first presentation of it at the next
render frame:

| Start | Stream open | First frame queued | First frame shown |
|---|---|---|---|
| Every video of the second campaign (51 starts, 1 to 4 videos at once, files read before) | 2.4 to 14.0 ms | 6.2 to 19.1 ms | 7.8 to 19.9 ms |
| Every video of the third campaign (30 starts, 2 or 3 videos at once, files read before) | 2.6 to 6.7 ms | 6.1 to 8.8 ms | 6.7 to 13.0 ms |
| Cold disk cache: three SNES videos not opened for a month, one per run | 15.2 to 16.8 ms | 16.8 to 18.4 ms | 18.1 to 20.2 ms |
| The same three again | 5.5 to 5.6 ms | 7.3 ms | 8.2 to 13.5 ms |

The three files (`snes/videos/1 US - A-E/7th Saga, The (USA).mp4`, `Aaahh!!! Real Monsters (USA).mp4`, `ABC Monday
Night Football (USA).mp4`, all H.264 640x480 at 59.94 fps) were read in place, read-only, as the real videos above.
NFR-06 asks for the first preview frame within 2 s of the focus, with the video local and decodable: two orders of
magnitude to spare on the SATA SSD that holds the media here. A USB hard disk that has to spin up, or a network share,
adds its own latency (seconds), which this machine could not show; the decoder's thread absorbs it (the render thread
never waits), and I5 shows the still picture meanwhile.

### Memory

What a decoder costs is the difference between runs with more and fewer decoders, the rest of the process being the
same:

| Decoders | Working set at the end | Private bytes at the end | Per added decoder |
|---|---|---|---|
| 2 against 4 1280x720 fixtures, above-normal (first campaign, medians) | 153.5, 230.4 MiB | 227.2, 334.2 MiB | +38.5 MiB working set, +53.5 MiB private |
| 2 against 4 1280x720 fixtures, above-normal (second campaign, medians) | 153.6, 229.7 MiB | 226.8, 335.1 MiB | +38.1 MiB working set, +54.2 MiB private |
| 1 against 3 real 640x480 H.264 videos, normal (second campaign, medians) | 96.4, 135.0 MiB | 143.8, 194.0 MiB | +19.3 MiB working set, +25.1 MiB private |

The buffers itemized for a 720p decoder add up to 35 MiB (6 pool frames of 3.5 MiB, a 10.5 MiB pinned ring, a 3.5 MiB
texture in video memory) and to about 12 MiB for 480p (7, 3.5 and 1.2 MiB): the rest of the measured cost is FFmpeg's
(the decoder's reference frames and buffers, the demuxer's packets, swscale's) and the driver's. Over a 60 s run the
working set changed by -1.2 to +1.2 MiB with the fixtures and with one real video, and by +2.6 to +3.5 MiB with the
three real videos together (in their first minute: see the longer runs below); the decoders' frame pools never grew (6
allocations each).

Longer runs (first campaign, above-normal priority, the machine 91 to 92% busy):

| Run | Loops per video | Working set: start, end | Private bytes: start, end | Late / late or dropped, worst | Drift, worst |
|---|---|---|---|---|---|
| 3 real videos, 300 s | 8 | 132.3, 137.5 MiB (+5.2) | 213.4, 216.6 MiB (+3.1) | 0.10% / 1.13% | 0.001% |
| 2x 1080p fixtures at 720 rows, 300 s | 30 | 226.6, 227.0 MiB (+0.4) | 322.0, 316.5 MiB (-5.4) | 0.04% / 0.62% | 0.001% |
| 3 real videos, 600 s | 16 | 132.4, 140.4 MiB (+8.0) | 241.2, 244.9 MiB (+3.7) | 0.11% / 0.91% | 0.0002% |

The 1080p fixtures loop 30 times without growth. In the 10-minute run the private bytes rose 2.3 MiB in the first
minute, while FFmpeg's and the driver's pools filled; their minute means were then 243.45, 243.78, 244.06, 244.00,
244.32, 244.56, 244.46, 244.50 and 244.62 MiB (minutes 2 to 10), against swings of 0.7 to 2.2 MiB between the samples
of one minute: about +0.1 MiB per minute, flattening over the last minutes. Nothing grows with the frames or the loops
as far as 10 minutes show, but a slow growth is not excluded: I5's hour-long soak with the scene renderer settles it.
The working set rose more (minute means 133.2 to 139.8 MiB), mostly the spike's own sample storage, committed at the
start and entering the working set as the run fills it (about 0.5 MiB per minute).

### The first campaign, with the machine 63 to 98% busy

Before the review the same series ran while other agents' builds kept the machine 63 to 98% busy. At normal priority
only the run at 63% met the criteria as read then (p99 15.87 ms at the display's rate); the runs at 74% and 80% had
p99 intervals of 65.7 and 19.1 ms and stalls of up to 484 ms, and the run at 74% dropped 11.6% of the frames (207 of
1788 per video), which the late-frame criterion as read then did not count. At above-normal priority the runs at 69%
and 71% met them; in the run at 92% (p99 14.9 ms, the longest stall 741 ms) each video resynced six times and had 11
late and 60 dropped frames: its drift of -4.80% (the video clock 2.9 s behind the wall clock after 60 s) is the sum of
those six resyncs, stalls of the render loop and of the decoders, not the one 741 ms stall. With the three real videos
the run at 98% (normal priority) drifted 13.6% after 18 resyncs per video. The intervals over 16.7 ms were not recorded
then, so those runs cannot be judged by the 60 Hz reading; a p99 above 16.7 ms at the display's rate fails it anyway.

## Error cases

The error series of the second campaign (Release, normal priority, `--error-cases --fixture-size 640x360
--fixture-seconds 3 --seconds 5`): the good clip and the seven damaged files of `FixtureSet::errorCases` in one window.
The run completed (exit code 0) at 161.6 FPS with the machine 58.6% busy; the `gpu` check `errors` asserts the same
verdicts in Debug.

| File | What it is | Verdict | Fallback shown at | Frames queued before |
|---|---|---|---|---|
| `clip_A_640x360_30fps_3s_mpeg4.mp4` | the good clip | plays, loops | - | 184 in 6 s |
| `error_missing.mp4` | no file at that path | `FileNotFound`: "no such file" | first frame | 0 |
| `error_empty.mp4` | 0 bytes | `InvalidFile`: "the file is empty" | first frame | 0 |
| `error_truncated.mp4` | the first half of clip A's bytes (its index is first, so it opens) | `Truncated`: "the stream ends after 1.53 s of the 3.00 s it declares" | 1.54 s | 46 |
| `error_corrupt.mp4` | clip A with 512 random bytes in every 16 KiB from 40% to 80% of the file | `CorruptData`: "3 errors among the last 32 decoding events" (2 damaged frames, 1 decoding error) | 1.34 s | 40 |
| `error_not_a_video.mp4` | a line of text | `InvalidFile`: "Invalid data found when processing input" | first frame | 0 |
| `error_audio_only.wav` | 2 s of silent 16-bit PCM | `NoVideoStream` | first frame | 0 |
| `error_unknown_codec.avi` | a 320x240 MPEG-4 AVI whose codec codes read "RVZZ" | `UnsupportedCodec`: "no decoder for the video codec none (tag RVZZ)" | first frame | 0 |

The fallback now replaces a damaged video once its last good frame has had its time: the truncated clip's 46 frames end
at 1.53 s and the corrupt clip's 40 at 1.33 s; before the review it replaced them as soon as the decoder failed (at
1.37 and 1.17 s), and the frames still queued were never shown. The unit tests check the same kinds, plus a directory
given as a file (`FileNotFound`), a corrupt clip that falls back after showing the frames before the damage while its
clean original plays to the end, a Matroska clip whose silent audio track (4 s) outlasts its 1 s of video, which plays
and loops (the first version declared it truncated: it compared the video with the container's duration), a cut
Matroska file, detected through the per-track `DURATION` tag (Matroska has no stream duration, and its demuxer ends a
cut file without an error), and a stop that returns at once while the decoder waits on a full queue.

## Seeks, pause and stop

The review found that `seek()` cleared the queue on the decoder thread while the render thread's clock was between the
steps of its update (it peeks, pops, then reads the front frame again), which could make it read a frame that was gone;
that a seek was not taken while the decoder waited on a full queue, so a paused playback took it only after resuming,
and first showed frames of the old position; that `pause()` was reported as `Playing` while the decoder waited for
room; and that `stop()` joined the decoder thread on the render thread, which waits for a read of a sleeping disk or a
network share. All four are fixed (ADR-0004, point 3) and covered by tests, among them 40 seeks while a playback
updates without waiting for the decoder, each followed by three frames of the new position and none of an older one.

Writing those tests found an older defect: a seek to a target just before a key frame's presentation time started one
or two frames late. MP4 indexes key frames by decoding time, which runs ahead of the presentation time by the B-frame
reordering delay, so the demuxer positioned on a key frame shown after the target, and the B-frames shown before that
key frame could not be decoded (in a clip with a key frame every 10 frames at 10 fps, a seek to 2.05 s delivered 2.1 s
first, one to 1.05 s delivered 1.2 s). The decoder now seeks a second before its target and drops the frames before
it.

## Upload variants explored

Exploratory runs while building `gl::VideoTexture` (Release, 2 videos 1280x720 30 fps, 12 to 20 s each, one or two runs
per variant on the loaded machine: medians are meaningful, the p99 of these short runs less so):

| Variant | Upload CPU p50 | Upload CPU p99 | Copy p50 | Where the rest goes | GPU copy p50 | Verdict |
|---|---|---|---|---|---|---|
| No pixel buffer: `glTextureSubImage2D` from the frame's memory | 3.62 ms | 12.7 ms (machine 83% busy) | - | the driver copies synchronously | - | 4x slower: PBOs kept |
| PBO, `memcpy`, a fence after each copy (first design) | 1.08 to 1.39 ms | 2.40 to 2.74 ms | 0.61 to 0.64 ms | `glFenceSync`: 0.51 to 0.58 ms on average, the rest of the GL calls 0.03 ms | 0.48 to 0.52 ms | fence stall |
| Same with SSE2 streaming stores instead of `memcpy` | 1.03 to 1.09 ms | 2.5 to 3.0 ms | 0.65 to 0.69 ms | as above | 0.49 to 0.51 ms | no gain: dropped |
| Same with a non-coherent mapping and `glFlushMappedNamedBufferRange` | 1.24 ms | 8.0 ms (77% busy) | 0.68 ms | fence 0.59 ms | 0.50 ms | no gain |
| Same with `GL_CLIENT_STORAGE_BIT` | 1.04 ms | 4.2 ms (73% busy) | 0.69 ms | fence 0.69 ms | 0.59 ms | no gain |
| Same with one texture per pixel buffer (Debug build) | 0.96 ms | 2.80 ms | - | - | 0.71 ms | NVIDIA's warning unchanged (300 of 300 uploads): dropped |
| PBO, `memcpy`, the copy's end timestamp as the completion signal (adopted) | 0.85 to 0.86 ms | 1.31 to 1.44 ms | 0.85 ms | GL calls 0.03 ms; the main thread's CPU time unchanged (6.77% of a core with and without the fence) | 0.51 ms | adopted |

The fence did not cost CPU time: it made the main thread wait, about 0.5 ms per upload, for NVIDIA's driver thread to
process the queued pixel transfer (`glFenceSync` returns a handle, so the threaded driver synchronizes). Without it, the
same work overlaps with the next frame's wait for vsync; the copy itself then reads slightly slower (0.85 ms instead
of 0.62 ms, the driver thread now running beside it), but the upload as a whole is shorter and its p99 halves. The
timestamp queries were already there to time the GPU copy; OpenGL 4.5 §4.3 records a `glQueryCounter` timestamp only
once every earlier command has fully completed, so the end timestamp's availability frees the pixel buffer as safely as
the fence did.

## Screenshots

Written by the runs, under the build tree (not committed): `build/spike_video/measurements/<series>/run<i>.png`,
`build/spike_video/measurements2/<series>/run<i>.png` and `build/spike_video/quiet/<series>/run<i>.png` (the last frame
of every run of the three campaigns),
`build/msvc-x64-debug/spikes/video/smoke/last_frame.png` and `build/msvc-x64-debug/spikes/video/errors/last_frame.png`
(the `gpu` checks, which also read the frame counters back from them). The two clips play side by side with their
frame numbers in sync, letterboxed in green-bordered cells; in the error run the good clip plays, and the truncated and
corrupt ones until their last good frame, then every other cell shows its red-bordered card ("NO VIDEO: FILE NOT
FOUND", "INVALID FILE", "TRUNCATED", "NO VIDEO STREAM", "UNSUPPORTED CODEC", "CORRUPT DATA"). In the real videos'
screenshots Super Metroid and Gran Turismo 4 show their 4:3 pictures in their own colors, and Donkey Kong Country is
black: the last frame of a run falls 25 s into that clip's second pass, where the clip itself is black
(`build/spike_video/dkc/at25.png`, a run stopped at 25 s, is black too, while `build/spike_video/dkc/early.png`, at 4 s,
shows the jungle level; the presentation is the same at the same timestamp in every run).

## Observations

- **At normal priority, other programs decide the frame pacing; the pipeline's own work does not move.** With the
  machine 33 to 76% busy, the normal-priority runs met the 60 Hz pacing up to 46% and missed it from 51% on, and with no
  other agent running (25 to 31%, the third campaign) all six met it, while the upload's p99 stayed at 1.1 to 1.4 ms
  (720p), the GPU copy at about 0.5 ms and the decoder threads at their few percent of a core. Most long intervals fall
  outside the frame's own work, in the buffer swap: the frame's work stayed under 3.5 ms in runs with up to 251 long
  intervals. At above-normal priority the frame's work never took longer than 6.1 ms (four 720p uploads included), and 7
  of 9 runs met every criterion up to 70% load. The decoder's 4 frames of lookahead absorb short stalls; after a longer
  one the clock drops the frames that became due, and after a stall over 250 ms it resyncs instead of slowing the video
  down, so under load the 60 Hz criterion fails first, then the share of late or dropped frames, while the upload and
  decoding times stay where they are.
- **Clips started together fall due together.** Both 720p fixtures (and the three real videos) usually start on the
  same render frame and keep the same rate, so every render frame that uploads uploads all of them: the per-frame sum,
  not one upload, is what the render thread pays (ADR-0004 budgets it).
- **NVIDIA reports every PBO upload** as a medium-severity "Pixel-path performance warning: Pixel transfer is
  synchronized with 3D rendering" (id 131154; the debug filter logs 3 and counts the rest): the copy runs on the 3D
  engine in order with the draws, about 0.5 ms of GPU time per 720p frame. Uploading into a texture no pending draw
  samples did not change it.
- **`glFenceSync` after a PBO copy stalls the render thread** about 0.5 ms with NVIDIA's threaded driver; the copy's end
  timestamp frees the pixel buffer without it (Upload variants explored).
- **The ring is busy only when the GPU is behind.** A small upload's copy completes on the GPU before the CPU has issued
  three more: the first version of the self-test found every slot free in four bursts of six 32x16 uploads within one
  frame, so it now queues about 10 ms of texture copies on the GPU first, and then finds the ring busy in its first
  burst. At the display's rate (165 fps clips) the ring was never busy.
- **A hidden window is paced by vsync** on this machine (164.8 FPS with vsync on, 777 to 825 FPS without), so automated
  runs measure real presentation without taking over the screen; it also never gets the foreground window's boost.
- **FFmpeg conceals damage**: the corrupt copy decodes with broken blocks and few errors returned by
  `avcodec_send_packet`/`avcodec_receive_frame` (4 in 76 frames in the first version, which let it play with broken
  blocks). The damaged frames carry `decode_error_flags` (`FF_DECODE_ERROR_CONCEALMENT_ACTIVE`), which the corruption
  policy now counts: the corrupt copy falls back at its third error (2 damaged frames and 1 decoding error), while the
  real videos report none in any run, so the policy shows no false positive on them. A truncated MP4 whose index comes
  first opens normally; the mov demuxer logs "partial file", returns the end of the file, and the decoder reports
  `Truncated` because the stream ends at 1.53 s of the 3.00 s it declares.
- **Container durations mislead.** MP4 and AVI give each stream its duration; Matroska gives only the container's,
  which covers the longest stream (an audio track outlasting the video), and its demuxer ends a cut file without an
  error. The muxers' per-track `DURATION` tag is what tells a cut Matroska file from a complete one.
- **MP4 places key frames by decoding time**: with B-frames a key frame's decoding time is a few frames ahead of its
  presentation time, so a seek positioned by the target alone lands after it; seeks go a second earlier.
- **The real ES-DE videos are 60 fps** (640x480 H.264 at 59.94 fps), twice the frame rate of the fixtures but a third of
  their pixels.
- **Build**: the configure step printed two CMake dev warnings (`CMP0174`) from vcpkg's `FindFFMPEG.cmake`, called by
  the top-level `find_package(FFMPEG)` of the I1 foundation, and tests added in `spikes/*` were not registered (the
  top-level `CMakeLists.txt` added `spikes/` before `enable_testing()`), so `spikes/video` deferred its `add_test` calls
  to the end of the top-level directory. Both were fixed at the merge of the spikes: the top-level file sets CMP0174 to
  NEW before `project()` (vcpkg's `find_package` override is a macro defined while `project()` loads the toolchain, so
  find modules run under the policies recorded then, those of `cmake_minimum_required(VERSION 3.28)`; a
  `cmake_policy(SET)` after `project()` does not reach them), and the top level enables testing before it adds any
  directory, so `spikes/video` adds its tests directly.

## Reproduce

```powershell
./scripts/build.ps1 -Preset msvc-x64-release
./spikes/video/run_measurements.ps1 -OutDir build/spike_video/measurements2   # the second campaign, summary.md there
./spikes/video/run_measurements.ps1 -Series fixtures-2x720p30, fixtures-2x720p30@above-normal, real-snes-ps2, `
  real-snes-ps2@above-normal -OutDir build/spike_video/quiet   # the third campaign: nothing else heavy running
./spikes/video/run_measurements.ps1 -Series fixtures-2x1080p30-full@above-normal, `
  fixtures-2x1080p30-to720@above-normal, fences-2x720p30@above-normal, novsync-2x720p30@above-normal   # first campaign
build/msvc-x64-release/bin/spike_video.exe --generate-fixtures build/spike_video/fixtures --seconds 60 --hidden `
  --report build/spike_video/report.json --screenshot build/screenshots/video.png --data-dir build/rvdata
build/msvc-x64-release/bin/spike_video.exe --videos <video.mp4> --seconds 3 --hidden `
  --report build/spike_video/startup/one.json --data-dir build/rvdata   # startup of one video: videos[0].startup
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike/video   # the two automated checks
```
