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

The pipeline built in I1 is adopted for I5 as it is. None of the fallbacks is needed.

1. **FFmpeg from vcpkg** (9.0.2: avformat, avcodec, swscale, swresample, avutil; LGPL components only, DLLs), software
   decoding. FFmpeg types stay behind `rv_media`'s sources; no public header includes them.
2. **One decoder thread per video** (`rv::media::VideoDecoder`, a `std::jthread`): it opens the file (the render thread
   never waits for the disk), demuxes and decodes it with `thread_count = 1`, so a decoder costs at most one core and its
   CPU time is its thread's (spec §9.5 budgets decoders). Files are opened read-only through FFmpeg's file protocol
   only (`protocol_whitelist=file`: a file's content cannot make FFmpeg open anything else), UTF-8 paths with the
   `file:` prefix (non-ASCII names work), an interrupt callback ends blocking I/O on `stop()`. The end of the stream is
   drained, then the clip loops: a seek to the start, `avcodec_flush_buffers`, and a loop offset so that timestamps keep
   growing. `seek()` flushes, discards the queued frames, decodes from the key frame before the target and drops what
   precedes it, under a new generation number. No exception leaves the thread: every failure ends it with a
   `DecodeFailure` the render thread reads.
3. **RGBA8 conversion on the decoder thread** (swscale), scaled to at most 720 rows (480 in the low profile: one
   setting), the aspect ratio kept (non-square pixels shrink the side they stretch), never enlarged, with the stream's
   matrix (BT.709 from 720 rows up and BT.601 below when unspecified) to full-range RGB. Keeping YUV 4:2:0 planes for a
   shader conversion is not adopted: the conversion costs the decoder thread under a millisecond per 720p frame (median
   0.82 to 0.87 ms) and the RGBA upload stays under its budget (p99 1.35 ms), while YUV planes would need three textures
   or a packed layout, a colour matrix per stream in the material, and a different screen shader. It remains the first
   optimization if 1080p previews or more decoders are ever wanted: a full 1080p RGBA upload has a p99 of 2.37 ms
   (BACKLOG).
4. **Bounded memory, no allocation per frame**: frames come from a `FramePool` of queue capacity + 2 reusable 64-byte
   aligned RGBA buffers and travel through a `FrameQueue` of 4 frames (the decoder waits while it is full; the render
   thread never waits for the decoder). Presentation timestamps are `best_effort_timestamp` times the stream time base in
   seconds, measured from the stream start, plus the loop offset. FFmpeg's demuxer still allocates its packet buffers;
   memory levels off after the first minute: in a 10-minute run of the three real videos (16 loops each) private bytes
   grew 2.3 MiB in the first minute and 1.0 MiB in the last 8, within the run's fluctuations, and the 1080p fixtures
   loop 30 times in 5 minutes without growth.
5. **Presentation by timestamp** (`rv::media::PresentationClock`, through `rv::media::VideoPlayback` on the render
   thread): the media time starts at the first frame shown, when it is shown, and advances with the render loop's clock
   (stopped while paused); each frame shows the newest due frame (2 ms tolerance for render jitter), skips older due ones
   (dropped), keeps the current one when nothing newer is due (repeated, when the queue starved), and jumps to a frame more
   than 250 ms late instead of racing to catch up (resync). A frame is late when it was already due at the previous
   render frame but not decoded yet: the decoder fell behind. Frames of an older seek generation are thrown away.
6. **Upload through persistently mapped PBOs** (`gl::VideoTexture`, main thread only): one immutable `GL_SRGB8_ALPHA8`
   texture per video (sRGB-correct: it is sampled as linear and written to sRGB targets), a ring of 3 pixel buffer slots
   in one buffer mapped with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`, a `memcpy` of the frame into the next slot
   and `glTextureSubImage2D` from it between two `GL_TIMESTAMP` queries. A slot is written again only once the GPU is
   done with it, tested without waiting; when the next slot is busy the upload is skipped and retried at the next render
   frame (it never happened in the 28 measured runs). Size changes reallocate the texture. Debug labels on every object.
7. **The end timestamp is the completion signal, not a fence.** OpenGL 4.5 §4.3 records a `glQueryCounter` timestamp
   once every earlier command has fully completed, so the availability of the copy's end timestamp tells as much as a
   fence would. A `glFenceSync` created right after the copy made NVIDIA's threaded driver wait for its server thread,
   about 0.5 ms per 720p upload: with fences the upload's p99 was 2.9 to 11.5 ms in the three campaign runs (median 3.02
   ms, above the 2 ms budget) against 1.32 to 1.37 ms without (median 1.35 ms). Fences stay as the fallback for contexts
   without timestamps (`GL_QUERY_COUNTER_BITS` 0) and as an option (`gl::VideoUploadSync::Fence`,
   `spike_video --upload-fences`).
8. **Failures fall back to a still picture** (spec §9.5, SC-08): the decoder reports `FileNotFound`, `InvalidFile` (empty,
   unknown container, damaged header), `NoVideoStream` (audio only or cover art only), `UnsupportedCodec`, `CorruptData`,
   `Truncated` or `DecoderError`, and the consumer shows the screenshot or cover instead (the spike shows a generated card
   naming the reason). Frames decoded before a failure mid-stream are shown first. Corruption: 16 errors in a row, or,
   once a clean frame was decoded, 3 errors among the last 32 decoding events, where FFmpeg's damaged frames
   (`decode_error_flags`, `AV_FRAME_FLAG_CORRUPT`: concealed blocks) count as errors, because FFmpeg conceals damage and
   broken pictures are worse than the fallback. Truncation: the demuxer runs out of file mid-stream, or the first pass
   ends before 90% of the declared duration minus 0.5 s.

### Budgets for I5 (decoder scheduler, spec §9.5, NFR-07)

Measured on the reference machine (i7-9700K, RTX 2080 SUPER), Release, per decoder:

| Content | Decoder thread CPU | Upload on the render thread, per frame | GPU copy, per frame | Memory |
|---|---|---|---|---|
| 1280x720 30 fps (MPEG-4 Part 2 fixtures) | 4.9 to 5.7% of a core (decode 0.82 to 0.86 ms, RGBA conversion 0.82 to 0.87 ms per frame) | p50 0.87 ms, p99 1.35 ms | p50 0.50 ms, p99 1.06 ms | about 35 MiB: 6 frames of 3.5 MiB, a 10.5 MiB pinned ring, a 3.5 MiB texture |
| 640x480 59.94 fps H.264 (real ES-DE videos) | 8.8 to 8.9% (decode 1.18 to 1.20 ms, conversion 0.19 ms) | p50 0.22 ms, p99 0.43 ms | p50 0.24 ms, p99 0.91 ms | about 12 MiB: 6 frames of 1.2 MiB, a 3.5 MiB ring, a 1.2 MiB texture |
| 1920x1080 30 fps delivered at 720 rows | 21.7% (the downscale costs 5.4 ms per frame) | p50 0.81 ms, p99 1.22 ms | p50 0.48 ms, p99 1.01 ms | as 720p, plus FFmpeg's 1080p buffers |

(Medians of three 60 s runs at above-normal priority; the per-run numbers are in the spike report.)

- Keep **2 decoders** in the normal profile and **1** in the low one, previews at **720 rows** at most (480 in the low
  profile): four 720p decoders met every criterion in their median run (p99 frame interval 6.94 ms, upload p99 1.37
  ms), so the budget leaves room for the scene; full 1080p frames do not fit the upload budget (p99 2.37 ms).
- A screen that is not visible or not a priority shows its last frame or its still picture and its decoder is paused or
  stopped (`VideoPlayback::pause`, `VideoDecoder::stop`): a paused decoder blocks on a condition variable (no CPU by
  design, not measured) and keeps the memory of its pool.
- Several screens showing the same video share one `VideoTexture`.
- Memory per 720p decoder: 6 pool frames of 3.5 MiB (21 MiB), a 10.5 MiB pinned pixel buffer ring and a 3.5 MiB
  texture; per 480p decoder about 7 + 3.5 + 1.2 MiB.

## Consequences

- `rv_media` (GPU-free) and `gl::VideoTexture` are the first versions of the I5 video service; I5 adds the scheduler
  (visibility, priority, budget), the screen material binding, audio and playlists on top of `VideoPlayback`.
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
  a larger API. Not needed at the measured cost.
- **Busy machines**: on a machine saturated by other programs the render loop is preempted (the measured p99 frame time
  grows with the machine's load, not with the videos); a frontend usually runs alone, but I5's scheduler should drop to
  one decoder when frames are late.
- **Edge content**: variable frame rate files, files whose first frames reference missing pictures (counted only after
  the first clean frame), 4:3 SNES captures with non-square pixels, very long files; the corruption thresholds may need
  tuning on the whole library (I5: a scan of every video of the collection with the decoder's verdicts).
