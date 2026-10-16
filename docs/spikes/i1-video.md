# I1 spike (c): video on a texture

- Date: 2026-10-16
- Outcome: the success criteria are met by the pipeline, and no fallback is needed. On the reference machine, shared
  with other work during the measurements, every criterion held in the median run at above-normal priority (p99 frame
  interval 8.46 ms, upload p99 1.35 ms, 0% late frames, drift 0.003%, no memory growth) and in the least-loaded run at
  normal priority (15.87 ms at 63% machine load). Runs in which other programs kept the machine 74 to 98% busy exceeded
  the 16.7 ms p99 because the render loop was descheduled (stalls up to 0.86 s); the presentation then degrades as
  designed: frames skipped, a resync after each stall longer than 250 ms (drift up to 14% in the runs at 84 to 98%
  load), late frames above 1% in a single run (4.0% at 98% load). Corrupt, truncated and other unusable files fall back
  to a still picture without a crash.
- Decision: [ADR-0004](../decisions/ADR-0004-video-pipeline.md)

## Goal

The roadmap's row (c) of I1: prove the video pipeline of specification §9.4 (FFmpeg demuxing and decoding with timestamps
on a worker thread, a bounded CPU frame queue, RGB conversion, a texture upload through pixel buffers on the render
thread, presentation driven by timestamps, a render thread that never blocks) within the budgets of §9.5 and §18.1
(NFR-06, NFR-07). Success criteria: two 720p videos with the render loop at 60 FPS for 60 s, a p99 frame time of at
most 16.7 ms, uploads under 2 ms, fewer than 1% late frames, a drift within 2%, stable memory, and a truncated or corrupt
file falling back without a crash. Fallbacks: YUV planes converted in the shader, 480p or a single decoder, prebuilt LGPL
FFmpeg binaries.

## What was built

- **`rv_media`** (GPU-free, `src/media`, FFmpeg only in its sources): `VideoDecoder` (one `std::jthread` per video that
  opens the file, demuxes and decodes it: `avcodec_send_packet` / `avcodec_receive_frame` with `EAGAIN`, drain at the end
  of the stream, flush on every loop and seek; swscale to RGBA8 scaled to at most `maxHeight` rows with the aspect ratio
  kept and never enlarged; presentation timestamps from `best_effort_timestamp` in seconds, monotonic across loops through
  a loop offset; seek generations; pause, resume, prompt stop with FFmpeg's I/O interrupted; failures reported as
  `DecodeFailure`; the file protocol only, UTF-8 paths, non-ASCII names tested), `FramePool` and `PooledFrame`
  (queue capacity + 2 reusable 64-byte aligned frames), `FrameQueue` (4 frames: the decoder waits, the render thread
  never does), `PresentationClock` (which frame is on screen at a render time: newest due frame, dropped, late,
  repeated, resync, pause, generations), `VideoPlayback` (a decoder and its clock for the render thread),
  `SampleRecorder` (percentiles without allocation), `scaledFrameSize`, and the synthetic fixtures of `test_video.hpp`
  (clips encoded at run time with FFmpeg's native MPEG-4 Part 2 or MJPEG encoders, with the frame number in large digits
  and as a row of 12 black or white blocks that `decodeFrameCounter` reads back; truncated, corrupt, empty, text,
  audio-only and unknown-codec files).
- **`gl::VideoTexture`** (`src/render`, main thread only): an immutable `GL_SRGB8_ALPHA8` texture the size of the video
  refilled from a ring of 3 pixel buffer slots in one buffer mapped with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`:
  `memcpy` into the next slot, `glTextureSubImage2D` from it between two `GL_TIMESTAMP` queries, never waiting for the
  GPU (a busy slot skips the upload). The end timestamp is the completion signal by default (`VideoUploadSync`), fences
  are the fallback and an option. GPU upload times, debug labels, reallocation on a size change.
- **`spikes/video`**: `spike_video` on `app::AppHost` (window, OpenGL context, debug output, frame loop) plays N videos
  in a grid of letterboxed quads drawn with an unlit sRGB-correct shader into an sRGB target blitted to a 1920x1080
  window, measures every frame after a 1 s warm-up and writes a JSON report (frame interval and CPU work per frame,
  upload CPU, copy and GPU times, per video the clock's presented, late, dropped and repeated frames, the decoder
  thread's CPU time from `GetThreadTimes`, the drift of the video clock against the wall clock, decode and conversion
  times, the working set and private bytes every second from `GetProcessMemoryInfo`, the machine's load from
  `GetSystemTimes`, the failure and fallback of each video, the GL debug message counts). Options: `--videos <file>...`,
  `--generate-fixtures <dir>`,
  `--error-cases`, `--count`, `--fixture-size`, `--fixture-fps`, `--fixture-seconds`, `--fixture-codec`,
  `--fixture-container`, `--seconds`, `--warmup`, `--max-height`, `--queue`, `--decoder-threads`, `--upload-fences`,
  `--priority`, `--no-vsync`, `--hidden`, `--width`, `--height`, `--report`, `--screenshot`, `--data-dir`,
  `--log-level`, `--list-encoders`. `run_measurements.ps1` runs the series below and writes their summary.
- **Tests**: 46 Catch2 cases in `tests/unit/media` (queue and pool, the clock on synthetic frames, output sizes,
  statistics, the decoder on clips written at test time: order, timestamps and frame counters, loops, scaling, non-ASCII
  paths, MP4, MKV and AVI, seeks, pause, prompt stop, every failure kind, the fixtures themselves), and two `gpu` checks
  registered from `spikes/video` (`check_spike_video.cmake`: two 720p clips for 3 s, hidden, with robust thresholds
  and timing thresholds only when other programs leave the machine mostly idle; the damaged files, each falling back
  for its reason).

## Method

- **Machine** (the reference machine of the plan): Intel Core i7-9700K (8 cores, 8 threads, 3.6 GHz), 64 GB, NVIDIA
  GeForce RTX 2080 SUPER with driver 617.42 (OpenGL 4.5 core), a 2560x1440 display at 164.84 Hz, Windows 11 (build
  29671). FFmpeg 9.0.2 from vcpkg (libavformat 63.1.102, libavcodec 63.1.102, libswscale 10.1.102, libavutil 61.1.102),
  LGPL, DLLs. `msvc-x64-release` for every number below; Debug for correctness.
- **Runs**: a 1920x1080 window, hidden (vsync still paces a hidden window on this machine: 164.8 FPS), vsync on unless
  stated, 60 s measured after a 1 s warm-up, 3 runs per series, interleaved across the series by
  `run_measurements.ps1`. The machine was shared with other work during the whole session (other agents' builds, a
  browser using up to three cores, an installer): every report records the machine's load over the run
  (`GetSystemTimes`; `Get-Counter` cannot be used, its counter names are localized on this Italian Windows). Series
  marked "above-normal" run the spike at `ABOVE_NORMAL_PRIORITY_CLASS` (decoder threads included), which removes most of
  the preemption by the other programs (normal priority), so that the pipeline's own cost shows.
- **Fixtures**: generated at run time by `--generate-fixtures` under `build/spike_video/measurements/fixtures` (never
  committed): 10 s clips, 30 fps, 1280x720 (and 1920x1080 for the headroom series), MPEG-4 Part 2 at quantizer 3 with 2
  B-frames and a key frame every 30 frames, in MP4 (moov first); each loops six times in a run. The LGPL build has no
  software H.264 encoder (`--list-encoders`: native encoders such as mpeg4, mjpeg, ffv1, prores; Media Foundation and
  Direct3D 12 hardware encoders; no libx264).
- **Real videos**, read in place and read-only (FFmpeg's file protocol opens them with `O_RDONLY`) from
  `E:/Programmi/ES-DE_media/downloaded_media`: `snes/videos/1 US - S/Super Metroid (Japan, USA) (En,Ja).mp4`,
  `snes/videos/1 US - A-E/Donkey Kong Country (USA) (Rev 2).mp4`, `ps2/videos/Gran Turismo 4 (Europe, Australia)
  (En,Fr,De,Es,It).mp4`: all three H.264 640x480 yuv420p at 59.94 fps, about 36 s, 1.6, 2.7 and 1.8 Mb/s.
- **Definitions**: *frame interval*, the time between the starts of two render frames (vsync included: 6.07 ms at
  164.84 Hz); *frame work*, the CPU time of a frame before the swap; *upload*, the CPU time of `VideoTexture::upload`
  (copy and GL calls), *copy* its `memcpy`, *GPU upload* the time between its two timestamps; *late*, a frame that was
  already due at the previous render frame but was not decoded yet (the decoder fell behind); *dropped*, a due frame
  skipped because a newer one was due too; *repeated*, a render frame that kept a frame past its duration because the
  queue was empty; *drift*, (video time shown - wall time) / wall time between the first and the last frame shown in the
  window; *decoder CPU*, the decoder thread's CPU time over the window, as a share of one core. The roadmap's "at 60
  FPS" is read as "no frame takes longer than a 60 Hz frame": the display runs at 164.84 Hz, so the render loop runs 2.7
  times as many frames as at 60 Hz and the p99 frame interval must stay at or under 16.7 ms.

## Results

Release, 60 s per run after a 1 s warm-up, 3 runs per series interleaved over about 30 minutes; the median of the 3 runs
first, the best run in parentheses. "Machine busy" is the share of the whole machine's CPU time used by every program
during the run (this one included: 1 to 3% of it).

### The criteria (two 1280x720 30 fps videos)

| Criterion | Threshold | Normal priority | Above-normal priority |
|---|---|---|---|
| Machine busy during the runs | - | 74% (63%; runs 74, 63, 80%) | 71% (69%; runs 69, 71, 92%) |
| p99 frame interval | at most 16.7 ms | 19.13 ms (15.87 ms) | 8.46 ms (6.19 ms) |
| Upload, CPU, p99 | under 2 ms | 1.88 ms (1.64 ms) | 1.35 ms (1.32 ms) |
| Upload, GPU, p99 | under 2 ms | 1.32 ms (0.91 ms) | 1.06 ms (0.95 ms) |
| Late frames, worst video | under 1% | 0% (0%; worst run 0.06%) | 0% (0%; worst run 0.66%) |
| Drift, worst video | within 2% | 0.003% (0.003%; worst run 0.70%) | 0.003% (0.003%; worst run 4.80%) |
| Working set over 60 s | stable | -0.70 MiB (-0.79 MiB), 153.5 MiB at the end | -0.23 MiB (-0.86 MiB), 153.5 MiB |
| Runs meeting every criterion | - | 1 of 3 (the run at 63%) | 2 of 3 |

The pipeline itself meets every criterion: the frame interval's median is 6.07 ms (the display's 164.84 Hz) in every
run, the CPU work of a frame has a p99 of 2.2 to 2.4 ms, and both runs of this series at above-normal priority below
92% machine load, and its normal-priority run at 63%, meet all of them. What breaks the p99 frame interval is
preemption by the other programs: at normal priority with the machine 74% or 80% busy, the render loop was descheduled
for up to 484 ms; at above-normal priority with the machine 92% busy, once for 741 ms. The presentation degrades as
designed in those runs: the clock skips the frames that became due during a stall (dropped: 414 frames for two videos
in the 74% run), resyncs after a stall longer than 250 ms (drift 0.70%, and 4.80% after the 741 ms one), and late frames
stay under 1% in every run of both series.

### Every series

| Series | Machine busy | Frame interval p99 | Upload CPU p50 / p99 | GPU copy p50 / p99 | Late, worst video | Drift, worst video | Decoder thread CPU per video | Working set at the end / growth |
|---|---|---|---|---|---|---|---|---|
| 2x 720p30 fixtures, normal | 74% | 19.13 (15.87) ms | 0.87 / 1.88 ms | 0.54 / 1.32 ms | 0% | 0.003% | 4.9% of a core | 153.5 / -0.70 MiB |
| 2x 720p30 fixtures, above-normal | 71% | 8.46 (6.19) ms | 0.87 / 1.35 ms | 0.50 / 1.06 ms | 0% | 0.003% | 5.2% | 153.5 / -0.23 MiB |
| 3 real videos (2 SNES, 1 PS2: 480p60 H.264), normal | 70% | 29.68 (14.38) ms | 0.22 / 0.45 ms | 0.25 / 0.95 ms | 0.38% | 0.007% | 8.9% | 135.3 / +3.77 MiB |
| 3 real videos, above-normal | 66% | 6.77 (6.19) ms | 0.22 / 0.43 ms | 0.24 / 0.91 ms | 0% | 0.001% | 8.8% | 135.3 / +2.60 MiB |
| 4x 720p30 fixtures, above-normal | 66% | 6.94 (6.20) ms | 0.92 / 1.37 ms | 0.47 / 0.97 ms | 0% | 0.003% | 5.7% | 230.4 / -0.38 MiB |
| 2x 1080p30 fixtures delivered at 1080 rows, above-normal | 67% | 6.57 (6.20) ms | 1.76 / 2.37 ms | 0.93 / 1.66 ms | 0% | 0.003% | 10.6% | 246.9 / -0.99 MiB |
| 2x 1080p30 fixtures delivered at 720 rows, above-normal | 66% | 6.28 (6.22) ms | 0.81 / 1.22 ms | 0.48 / 1.01 ms | 0% | 0.003% | 21.7% | 167.7 / +2.77 MiB |
| 2x 720p30 fixtures with fences, above-normal | 67% | 6.25 (6.20) ms | 1.29 / 3.02 ms | 0.51 / 1.13 ms | 0% | 0.003% | 3.6% | 153.5 / -0.70 MiB |
| 2x 720p30 fixtures without vsync, above-normal | 60% | 2.66 (2.22) ms at 777 (825) FPS | 0.82 / 1.30 ms | 0.49 / 0.91 ms | 0% | 0% | 5.2% | 154.1 / -0.37 MiB |

Per frame on the decoder thread (medians of the runs): the 720p MPEG-4 fixtures decode in 0.82 to 0.86 ms and convert
to RGBA in 0.82 to 0.87 ms (0.93 ms with four decoders); the real 480p H.264 videos decode in 1.18 to 1.20 ms and
convert in 0.19 ms; 1080p frames decode in 1.65 to 1.8 ms and convert in 1.58 ms at full size, or in 5.43 ms when scaled
down to 720 rows (swscale's bilinear filter over 2.25 times the pixels: the decoder thread's 21.7%). The render thread's
copy into the pixel buffer is 0.82 to 0.86 ms for a 720p frame (3.5 MiB), 1.75 ms for 1080p (7.9 MiB) and 0.21 to 0.22
ms for 480p (1.2 MiB). Across the 28 runs the pixel
buffer ring was never busy, no presented frame was superseded before its upload, every decoder allocated exactly its
pool's 6 frames, and no playing video (the real ones included) reported a decoding error or a damaged frame.

- **Headroom**: four 720p decoders meet every criterion in the median run (the frame's CPU work rises to a p99 of
  4.3 ms). Full 1080p frames do not fit the upload budget (p99 2.37 ms): previews stay at 720 rows, as spec §9.5 plans.
  Without vsync the render loop runs at 777 FPS (median; 706 to 825) with both videos on time.
- **Memory**: over 60 s the working set changes by -1.0 to +1.3 MiB with the 720p and full 1080p fixtures, by +2.6 to
  +2.8 MiB with the 1080p fixtures scaled to 720 rows and by +1.9 to +4.0 MiB with the real videos (whose 36 s clips loop
  once in a run); the decoders' frame pools never grow (6 allocations each). Whether the growth levels off is checked by
  the longer runs of "Memory over 5 and 10 minutes" below: it does.

### Memory over 5 and 10 minutes

Two longer runs at above-normal priority, in parallel (separate processes, the machine 92% busy):

| Run | Loops per video | Working set: start, end, min, max | Private bytes: start, end | Late, worst | Drift, worst |
|---|---|---|---|---|---|
| 3 real videos, 300 s | 8 | 132.3, 137.5, 131.1, 137.5 MiB (+5.2) | 213.4, 216.6 MiB (+3.1) | 0.10% | 0.001% |
| 2x 1080p fixtures at 720 rows, 300 s | 30 | 226.6, 227.0, 224.9, 227.0 MiB (+0.4) | 322.0, 316.5 MiB (-5.4) | 0.04% | 0.001% |

The 1080p fixtures loop 30 times without any growth, so the decoder's loop (drain, seek, flush) and the upload path
keep nothing. With the real H.264 videos the working set grows more slowly than in the first minute (+5.2 MiB in 5
minutes against +1.9 to +4.0 MiB in one); about 2.4 MiB of that is the spike's own statistics, whose sample storage is
reserved (committed) at the start and enters the working set as the run fills it. Private bytes, which that storage does
not change, grew by 3.1 MiB.

A 10-minute run on the real videos then gave the trend (16 loops of each, above-normal priority, the machine 91% busy,
p99 frame interval 6.33 ms, late frames 0.10 to 0.11%, drift 0.0002%, the report's per-second series): private bytes
241.2 MiB at the start, 243.4 after 1 minute, 243.8 after 2, 244.3 after 5 and 244.9 after 10, so +2.3 MiB while
FFmpeg's and the driver's pools fill during the first minute and +1.0 MiB over the last 8 minutes, within the run's
fluctuations of about 0.4 MiB; the working set went from 132.4 to 140.4 MiB, the private growth plus about 4.4 MiB of
statistics. The memory is stable: nothing grows with the frames or the loops (the decoders' pools never grew). An
hour-long soak with the scene renderer belongs to I5's checks.

## Error cases

The error series of the campaign (Release, normal priority, `--error-cases --fixture-size 640x360 --fixture-seconds 3
--seconds 5`): the good clip and the seven damaged files of `FixtureSet::errorCases` in one window. The run completed
(exit code 0) at 161 FPS (p99 15.5 ms with the machine 66% busy); the `gpu` check `errors` asserts the same verdicts in
Debug and Release.

| File | What it is | Verdict | Fallback shown at | Frames decoded before |
|---|---|---|---|---|
| `clip_A_640x360_30fps_3s_mpeg4.mp4` | the good clip | plays, loops | - | 185 in 6 s |
| `error_missing.mp4` | no file at that path | `FileNotFound`: "no such file" | first frame | 0 |
| `error_empty.mp4` | 0 bytes | `InvalidFile`: "the file is empty" | first frame | 0 |
| `error_truncated.mp4` | the first half of clip A's bytes (its index is first, so it opens) | `Truncated`: "the stream ends after 1.53 s of the 3.00 s it declares" | 1.37 s | 46 |
| `error_corrupt.mp4` | clip A with 512 random bytes in every 16 KiB from 40% to 80% of the file | `CorruptData`: "3 errors among the last 32 decoding events" (2 damaged frames, 1 decoding error) | 1.18 s | 41 |
| `error_not_a_video.mp4` | a line of text | `InvalidFile`: "Invalid data found when processing input" | first frame | 0 |
| `error_audio_only.wav` | 2 s of silent 16-bit PCM | `NoVideoStream` | first frame | 0 |
| `error_unknown_codec.avi` | a 320x240 MPEG-4 AVI whose codec codes read "RVZZ" | `UnsupportedCodec`: "no decoder for the video codec none (tag RVZZ)" | first frame | 0 |

The unit tests check the same kinds, plus a directory given as a file (`FileNotFound`), a corrupt clip that falls back
after showing the frames before the damage while its clean original plays to the end, and a stop that ends a decoder
waiting on a full queue at once.

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

Written by the runs, under the build tree (not committed): `build/spike_video/measurements/<series>/run<i>.png` (the
last frame of every run), `build/msvc-x64-debug/spikes/video/smoke/last_frame.png` and
`build/msvc-x64-debug/spikes/video/errors/last_frame.png` (the `gpu` checks). The two clips play side by side with their
frame numbers in sync, letterboxed in green-bordered cells; in the error run the good clip and the corrupt one play
until the corruption is reached, every other cell shows its red-bordered card ("NO VIDEO: FILE NOT FOUND", "INVALID
FILE", "TRUNCATED", "NO VIDEO STREAM", "UNSUPPORTED CODEC", "CORRUPT DATA"). In the real videos' screenshots Super
Metroid and Gran Turismo 4 show their 4:3 pictures in their own colors, and Donkey Kong Country is black: the last frame
of a run falls 25 s into that clip's second pass, where the clip itself is black (`build/spike_video/dkc/at25.png`, a run
stopped at 25 s, is black too, while `build/spike_video/dkc/early.png`, at 4 s, shows the jungle level; the presentation
is the same at the same timestamp in every run).

## Observations

- **The machine's load, not the videos, decides the p99.** The p99 frame interval follows the other programs' load
  (normal priority: 15.9 ms at 63%, 19.1 ms at 80%, 65.7 ms at 74% with a 484 ms stall; above-normal: 6.2 ms at 69%,
  8.5 ms at 71%, 14.9 ms at 92%), while the median frame interval is the display's 6.07 ms in every vsync run and the
  frame's own CPU work keeps a p99 of 2.2 to 2.4 ms for two 720p videos. The decoder's 4 frames of lookahead absorb short
  stalls; after a longer one the clock skips the frames that became due (dropped) and resyncs instead of slowing the
  video down, so late frames stay rare (above 1% in one run of 28, at 98% load) and drift appears only after resyncs.
- **NVIDIA reports every PBO upload** as a medium-severity "Pixel-path performance warning: Pixel transfer is
  synchronized with 3D rendering" (id 131154; the debug filter logs 3 and counts the rest): the copy runs on the 3D
  engine in order with the draws, about 0.5 ms of GPU time per 720p frame. Uploading into a texture no pending draw
  samples did not change it.
- **`glFenceSync` after a PBO copy stalls the render thread** about 0.5 ms with NVIDIA's threaded driver; the copy's end
  timestamp frees the pixel buffer without it (Upload variants explored).
- **A hidden window is paced by vsync** on this machine (164.8 FPS with vsync on, 777 to 825 FPS without), so
  automated runs measure real presentation without taking over the screen.
- **FFmpeg conceals damage**: the corrupt copy decodes with broken blocks and few errors returned by
  `avcodec_send_packet`/`avcodec_receive_frame` (4 in 76 frames in the first version, which let it play with broken
  blocks). The damaged frames carry `decode_error_flags` (`FF_DECODE_ERROR_CONCEALMENT_ACTIVE`), which the corruption
  policy now counts: the corrupt copy falls back at its third error (2 damaged frames and 1 decoding error), while the
  three real videos report none in any run, so the policy shows no false positive on them. A truncated MP4 whose index
  comes first opens normally; the mov demuxer logs "partial file", returns the end of the file, and the decoder reports
  `Truncated` because the stream ends at 1.53 s of the 3.00 s it declares.
- **The real ES-DE videos are 60 fps** (640x480 H.264 at 59.94 fps), twice the frame rate of the fixtures but a third of
  their pixels.
- **Build**: the configure step prints two CMake dev warnings (`CMP0174`) from vcpkg's `FindFFMPEG.cmake`, called by the
  top-level `find_package(FFMPEG)` of the I1 foundation (already there before this spike). Tests added in `spikes/*` are
  not registered by the top-level `CMakeLists.txt` as it stands (it adds `spikes/` before `enable_testing()`), so
  `spikes/video` defers its `add_test` calls to the end of the top-level directory.

## Reproduce

```powershell
./scripts/build.ps1 -Preset msvc-x64-release
./spikes/video/run_measurements.ps1                      # every series, 3 x 60 s, summary in build/spike_video/measurements
build/msvc-x64-release/bin/spike_video.exe --generate-fixtures build/spike_video/fixtures --seconds 60 --hidden `
  --report build/spike_video/report.json --screenshot build/screenshots/video.png --data-dir build/rvdata
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike/video   # the two automated checks
```
