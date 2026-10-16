# ADR-0004: Video pipeline: FFmpeg decoding on worker threads, RGBA frames uploaded through persistently mapped PBOs

- Status: accepted
- Date: 2026-10-16
- Spike: I1 (c), report in [docs/spikes/i1-video.md](../spikes/i1-video.md)

## Context

Specification §9.4 mandates the video pipeline of the TV screens: FFmpeg demuxing, decoding with timestamps
(`avcodec_send_packet` / `avcodec_receive_frame`, `EAGAIN`, end of stream, seek and drain), a bounded frame queue on a
worker thread, RGB conversion (or YUV planes), a texture upload through pixel buffers on the render thread, an emissive
material, presentation driven by timestamps, and a render thread that never blocks on disk, file opening or decoding.
§9.5 sets the budgets (2 decoders in the normal profile, 1 in the low one, 480p or 720p previews, a still picture when a
screen is not visible or when the codec is missing, the file is corrupt or the budget is exhausted) and §15.3 forbids
OpenGL calls from workers. The roadmap's spike (c) had to show, on the reference machine, two 720p videos at 60 FPS for
60 s with a p99 frame time of at most 16.7 ms, uploads under 2 ms, fewer than 1% late frames, a drift within 2% and
stable memory, and a fallback without a crash for truncated or corrupt files; otherwise one of the fallbacks (YUV planes
converted in the shader, 480p or a single decoder, prebuilt LGPL FFmpeg binaries) had to be taken.

## Decision

The pipeline built in I1 is adopted for I5. Its own cost needs none of the fallbacks; the spike leaves I5 two conditions,
both measured. First, at normal priority the render loop kept a 60 Hz frame pacing only while other programs left the
machine at most about half busy; at above-normal priority it held in most runs up to 70% (point 9). Second, the uploads
of the videos due on the same render frame add up: two 720p uploads reach 2.1 to 2.7 ms at p99 (budgets).

1. **FFmpeg from vcpkg** (9.0.2: avformat, avcodec, swscale, swresample, avutil; LGPL components only, DLLs), software
   decoding. FFmpeg types stay behind `rv_media`'s sources; no public header includes them.
2. **One decoder thread per video** (`rv::media::VideoDecoder`, a `std::jthread`): it opens the file (the render thread
   never waits for the disk), demuxes and decodes it with `thread_count = 1`, so a decoder costs at most one core and its
   CPU time is its thread's (spec §9.5 budgets decoders). Files are opened read-only through FFmpeg's file protocol
   only (`protocol_whitelist=file`: a file's content cannot make FFmpeg open anything else), UTF-8 paths with the
   `file:` prefix (non-ASCII names work), an interrupt callback ends FFmpeg's I/O between reads on `stop()`. The end of
   the stream is drained, then the clip loops: a seek to the start, `avcodec_flush_buffers`, and a loop offset so that
   timestamps keep growing. No exception leaves the thread: every failure ends it with a `DecodeFailure` the render
   thread reads.
3. **The owner never waits for the decoder thread.** The render thread (the queue's only consumer) calls `pause()`,
   `resume()`, `seek()` and `stop()`, and none of them waits:
   - `seek()` publishes a new generation and discards the queued frames of the old position at once, on the caller's
     thread: the decoder thread only ever appends to the queue, so the presentation clock never sees a frame vanish
     between the steps of its update. A decoder waiting for room discards the frame it holds; the thread then decodes
     from a key frame up to a second before the target (MP4 indexes key frames by decoding time: without this pre-roll
     the B-frames shown just before a key frame were lost) and drops the frames before the target. While paused it
     still delivers the frame at the target, which becomes the paused picture.
   - `pause()` is reported by `state()` at once, also while the thread waits for room.
   - `stop()` and the destructor ask the thread to end and return: the thread finishes the read or the frame it is busy
     with on its own (a read from a sleeping disk or a network share can take seconds, spec §9.4), and
     `waitForStoppedDecoders()` waits for those threads once, at application shutdown.
4. **RGBA8 conversion on the decoder thread** (swscale), scaled to at most 720 rows (480 in the low profile: one
   setting), the aspect ratio kept (non-square pixels shrink the side they stretch), never enlarged, with the stream's
   matrix (BT.709 from 720 rows up and BT.601 below when unspecified) to full-range RGB. Keeping YUV 4:2:0 planes for a
   shader conversion is not adopted: the conversion costs the decoder thread under a millisecond per 720p frame (run
   medians 0.54 to 0.92 ms) and an RGBA upload stays under its budget, while YUV planes would need three textures or a
   packed layout, a colour matrix per stream in the material, and a different screen shader. It remains the first
   optimization if 1080p previews, more decoders or a lower upload cost per render frame are wanted: YUV 4:2:0 is 1.5
   bytes per pixel against 4.
5. **Bounded memory, no allocation per frame**: frames come from a `FramePool` of queue capacity + 2 reusable 64-byte
   aligned RGBA buffers and travel through a `FrameQueue` of 4 frames (the decoder waits while it is full; the render
   thread never waits for the decoder). Presentation timestamps are `best_effort_timestamp` times the stream time base in
   seconds, measured from the stream start, plus the loop offset. FFmpeg's demuxer still allocates its packet buffers.
   The pools never grow; over 10 minutes the private bytes rose 2.3 MiB in the first minute, then by about 0.1 MiB per
   minute, flattening (the last three minute means within 0.2 MiB), against swings of 0.7 to 2.2 MiB within a minute:
   no growth with frames or loops showed, and I5's hour-long soak settles the rest.
6. **Presentation by timestamp** (`rv::media::PresentationClock`, through `rv::media::VideoPlayback` on the render
   thread): the media time starts at the first frame shown, when it is shown, and advances with the render loop's clock
   (stopped while paused); each frame shows the newest due frame (2 ms tolerance for render jitter), skips older due ones
   (dropped), keeps the current one when nothing newer is due (repeated, when the queue starved), and jumps to a frame more
   than 250 ms late instead of racing to catch up (resync). A frame is late when it was already due at the previous
   render frame but not decoded yet: the decoder fell behind. Frames of an older seek generation are never shown; while
   paused, the first frame of a clip or of a seek becomes the paused picture.
7. **Upload through persistently mapped PBOs** (`gl::VideoTexture`, main thread only): one immutable `GL_SRGB8_ALPHA8`
   texture per video (sRGB-correct: it is sampled as linear and written to sRGB targets), a ring of 3 pixel buffer slots
   in one buffer mapped with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`, a `memcpy` of the frame into the next slot
   and `glTextureSubImage2D` from it between two `GL_TIMESTAMP` queries. A slot is written again only once the GPU is
   done with it, tested without waiting; when the next slot is busy the upload is skipped and retried at the next render
   frame (`VideoUpload::RingBusy`, exercised by the spike's self-test). Three slots suffice at one upload per video and
   render frame: with 165 fps clips on the 164.84 Hz display, each slot reused every third render frame (18 ms), the
   ring was never busy and no frame was superseded before its upload in three 60 s runs (8,065 to 9,825 uploads per
   video each). Size changes reallocate the texture (the ring only when it grows). Debug labels on every object.
8. **The end timestamp is the completion signal, not a fence.** OpenGL 4.5 §4.3 records a `glQueryCounter` timestamp
   once every earlier command has fully completed, so the availability of the copy's end timestamp tells as much as a
   fence would. A `glFenceSync` created right after the copy made NVIDIA's threaded driver wait for its server thread,
   about 0.5 ms per 720p upload: with fences the upload's p99 was 2.9 to 11.5 ms in the three runs of the first campaign
   (median 3.02 ms, above the 2 ms budget) against 1.32 to 1.37 ms without (median 1.35 ms). Fences stay as the fallback
   for contexts without timestamps (`GL_QUERY_COUNTER_BITS` 0) and as an option (`gl::VideoUploadSync::Fence`,
   `spike_video --upload-fences`).
9. **Above-normal priority while the 3D view renders.** I5 sets the process to `ABOVE_NORMAL_PRIORITY_CLASS` while it
   renders (decoder threads included, the configuration measured) and back to `NORMAL_PRIORITY_CLASS` while an
   emulator runs or the window is minimized; child processes start at normal priority anyway. The pipeline's own cost
   is the same at both priorities; the frame pacing is not. In the second campaign every normal-priority run of 60 s
   with the machine at most 46% busy met the 60 Hz pacing (1 to 21 intervals over 16.7 ms, 36 allowed) and every one
   from 51% on missed it (39 to 351, up to 5% of a video's frames dropped, the frame's own work, at most 2.7 ms at p99,
   taking up to 49 ms). At above-normal priority the frame's work never took longer than 6.1 ms and 7 of 9 runs at 36 to 70%
   met every criterion; the two that missed the pacing (50 and 46 long intervals) lost the time in the buffer swap,
   which bursts of other programs delay at either priority. A boost of the render thread alone (`SetThreadPriority`,
   MMCSS) and the boost Windows gives a foreground window (the spike's windows were hidden) were not measured; I5 may
   measure them as alternatives.
10. **Failures fall back to a still picture** (spec §9.5, SC-08): the decoder reports `FileNotFound`, `InvalidFile`
    (empty, unknown container, damaged header), `NoVideoStream` (audio only or cover art only), `UnsupportedCodec`,
    `CorruptData`, `Truncated` or `DecoderError`, and the consumer shows the screenshot or cover instead (the spike shows
    a generated card naming the reason). `VideoPlayback::failed()` turns true only once the frames decoded before the
    failure have been shown and the last one has had its time: the truncated fixture falls back at 1.54 s, after its
    1.53 s of frames (before the review, at 1.37 s, with frames still queued). Corruption: 16 errors in a row, or, once
    a clean frame was decoded, 3 errors among the last 32 decoding events, where FFmpeg's damaged frames
    (`decode_error_flags`, `AV_FRAME_FLAG_CORRUPT`: concealed blocks) count as errors, because FFmpeg conceals damage and
    broken pictures are worse than the fallback. Truncation: the demuxer runs out of file mid-stream, or the first pass
    ends before 90%, less 0.5 s, of the video stream's own duration (the stream's, or the `DURATION` tag Matroska writes
    per track); never the container's duration, which covers an audio track longer than the video.

### Budgets for I5 (decoder scheduler, spec §9.5, NFR-06, NFR-07)

Measured on the reference machine (i7-9700K, RTX 2080 SUPER), Release, per decoder: the ranges of the run medians of the
second campaign at either priority (these costs do not depend on it), the 1080p row from the first campaign; the
per-run numbers and the machine's load are in the spike report.

| Content | Decoder thread CPU | Upload on the render thread, per frame of a video: p50 / p99 | GPU copy: p50 / p99 | Memory, measured per added decoder (itemized buffers) |
|---|---|---|---|---|
| 1280x720 30 fps (MPEG-4 Part 2 fixtures) | 4.0 to 5.6% of a core (decode 0.71 to 0.86 ms, RGBA conversion 0.54 to 0.92 ms per frame) | 0.75 to 0.92 / 1.18 to 1.37 ms | 0.47 to 0.65 / 0.62 to 1.42 ms | +38 MiB working set, +54 MiB private bytes (35 MiB: 6 pool frames of 3.5 MiB, a 10.5 MiB pinned ring, a 3.5 MiB texture) |
| 640x480 59.94 fps H.264 (real ES-DE videos) | 6.7 to 9.1% (decode 0.59 to 1.15 ms, conversion 0.14 to 0.19 ms) | 0.22 to 0.24 / 0.36 to 0.48 ms | 0.25 to 0.38 / 0.56 to 1.17 ms | +19 MiB working set, +25 MiB private bytes (about 12 MiB: 7 + 3.5 + 1.2 MiB) |
| 1920x1080 30 fps delivered at 720 rows | 21.7% (the downscale costs 5.4 ms per frame) | 0.81 / 1.22 ms | 0.48 / 1.01 ms | as 720p, plus FFmpeg's 1080p buffers |

- **Upload per render frame**: the uploads of the videos due on the same render frame add up, and videos started
  together fall due together. Two 720p videos due together cost 1.54 to 1.67 ms at the median and 2.08 to 2.68 ms at
  p99 per render frame, four 3.39 to 3.69 and 4.29 to 4.80 ms, the three real 480p videos 0.62 to 0.71 and 0.99 to 1.15
  ms. I5 keeps the render thread's upload work of a frame under 2 ms by uploading at most one 720p frame per render
  frame when two fall due together: the other stays pending one render frame, which `VideoPlayback` supports (a
  pending frame is uploaded at the next update, or replaced by a newer one), and each frame then costs what the table
  says. The spike did not implement the staggering; one 720p run in which the two clocks happened to start a render
  frame apart (17 of 3578 render frames uploading both) shows its effect: a per-frame p99 of 1.42 ms. Two 60 fps 720p
  videos on a 60 Hz display cannot be staggered without dropping frames; they need the YUV planes of point 4, or 480p.
- Keep **2 decoders** in the normal profile and **1** in the low one, previews at **720 rows** at most (480 in the low
  profile): four 720p decoders at above-normal priority met every criterion in two of three runs (at 36% and 70% load),
  their uploads unchanged per frame of a video but 3.4 to 3.7 ms per render frame; full 1080p frames do not fit the
  upload budget (p99 2.37 ms per upload).
- **Startup** (NFR-06, the first preview frame within 2 s): the first frame was shown 7.8 to 19.9 ms after
  `VideoDecoder::start()` in all 51 starts of the second campaign, and 18 to 20 ms from a cold disk cache (three
  videos not opened for a month, on the SATA SSD that holds the media). Disks that spin up and network shares were not
  measured; the decoder thread absorbs their latency and the screen shows its still picture meanwhile.
- A screen that is not visible or not a priority shows its last frame or its still picture and its decoder is paused or
  stopped (`VideoPlayback::pause`, `VideoDecoder::stop`): a paused decoder blocks on a condition variable (no CPU by
  design, not measured) and keeps the memory of its pool; a stopped one frees it when its thread ends.
- Several screens showing the same video share one `VideoTexture`.
- **Memory per decoder**: budget the measured figures of the table, not the itemized buffers: the rest is FFmpeg's
  (reference frames, packets, swscale) and the driver's. Prefetching the next clip of a playlist (spec §9.5) costs a
  decoder's worth.

## Consequences

- `rv_media` (GPU-free) and `gl::VideoTexture` are the first versions of the I5 video service; I5 adds the scheduler
  (visibility, priority, budget, upload staggering), the screen material binding, audio, playlists and the process
  priority of point 9 on top of `VideoPlayback`, and calls `waitForStoppedDecoders()` at shutdown.
- The NVIDIA driver reports every PBO upload as a medium-severity "Pixel-path performance warning: Pixel transfer is
  synchronized with 3D rendering" (id 131154): the copy runs on the 3D engine (about 0.5 ms of GPU time per 720p frame),
  not on a copy engine. A texture per pixel buffer did not change it. Asynchronous copies would need a second OpenGL
  context on its own thread; not worth it at this cost. The debug message filter may drop this id in I5 (it is expected).
- Real ES-DE videos are H.264 640x480 at 59.94 fps (the three measured), which the LGPL build decodes; AV1 has no
  software decoder in this build (no libdav1d) and falls back to the still picture as `UnsupportedCodec`.

### Open risks

- **Hardware decoding** (D3D11VA, DXVA2, Media Foundation are compiled into the vcpkg FFmpeg): OpenGL interop
  (`WGL_NV_DX_interop2`) was not measured; deferred beyond v1 as decided (BACKLOG). Software decoding of 720p costs a
  few percent of a core.
- **Audio** (spec §9.7, I5): swresample is linked but the spike decodes video only; audio adds a second stream per
  decoder, a clock master question (audio clock vs render clock) and miniaudio.
- **Zero-copy upload**: the decoder could convert straight into the mapped pixel buffers, saving the render thread's
  copy (about 0.8 ms per 720p frame); it needs pixel buffers handed to the decoder and released by fence or timestamp,
  a larger API. The first remedy if staggering is not enough.
- **Busy machines**: point 9 keeps programs at normal priority from interrupting the frame's work, but their bursts
  still delay the buffer swap (the two above-normal runs that missed the pacing), and programs at higher priority still
  preempt the render loop; the criteria were not measured on an idle machine (none was to be had during the spike).
  I5's scheduler should drop to one decoder when frames are late.
- **Edge content**: variable frame rate files, files whose first frames reference missing pictures (counted only after
  the first clean frame), 4:3 SNES captures with non-square pixels, very long files, cold reads from USB or network
  disks; the corruption thresholds may need tuning on the whole library (I5: a scan of every video of the collection
  with the decoder's verdicts).
