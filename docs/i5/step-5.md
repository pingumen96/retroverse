# I5 step 5: the clips' sound

Roadmap row: I5 step 5. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §9 (and §12's row "a clip's
sound against its picture within 80 ms at p95").

## What was built

- **`rv_audio`** (`src/audio`, GPU-free, on `rv_core` alone; `RV_AUDIO_FORBIDDEN_LINKS` in `cmake/LayerRules.cmake`
  keeps FFmpeg, `rv_media`, RmlUi, SQLite and pugixml out, and `rv_media` may not reach it). miniaudio 0.11.25 from
  vcpkg (the port added to `vcpkg.json`, the baseline unchanged), compiled once in `miniaudio_impl.cpp` with device
  I/O only (WASAPI and miniaudio's null backend), its header external.
  - `AudioRing`: a lock-free single-writer, single-reader ring of 0.5 s of 48 kHz stereo float, timestamped per chunk
    (media time, seek generation); the writer never waits (what does not fit is dropped and counted).
  - `spatial.hpp`: the gain table (1 within 1.5 m, 1.5/d, a linear fade over the last 2 m before `proximityMeters`),
    the pan (sin of the azimuth, at most 0.6) and the equal-power gains; `kDuckDecibels` -9.
  - `AudioSync`: nothing within 20 ms, a ratio up to 1% off to 120 ms, a resync beyond.
  - `Mixer`: one source per decoder slot, reading its ring at the picture's media time; at ratio 1 the samples are
    copied exactly, within the correction band read by linear interpolation; the 300 ms gain slope (the cross-fade),
    the 30 ms pause fade, master, duck, mute and the launch's silence; the sum clamped; deterministic.
  - `AudioEngine`: `device` (miniaudio's shared-mode default endpoint, rendered on its thread), `null` (pulled by the
    main thread, the elapsed time each frame: dt x 48000 under `--fixed-dt`), the capture of the null mix as a WAV;
    suspend and resume around a launch. `wav_file.hpp` and `analysis.hpp` (Goertzel, zero-crossing frequency, onsets)
    for the tests.
- **`rv_media`**: `AudioSink` (`audio_sink.hpp`); `DecoderSettings::audioSink`, `audioSampleRate`, `audioEnabled`;
  `VideoDecoder::setAudioEnabled()`. The clip's first audio stream is decoded on the decoder's thread, resampled by
  swresample to 48 kHz float (mono kept mono and copied to both channels at its level, more than two channels
  down-mixed to stereo by swresample), timestamped on the video's timeline (from the video stream's start, plus the
  loops' offset; the resampler's delay taken off), flushed at seeks, loops and re-enables; frames before a seek's
  target dropped. Disabled, the audio packets are discarded undecoded. `StreamInfo` names the audio stream.
  `writeTestVideo` writes PCM tones and beeps whose frames flash (44.1, 48, 96 kHz, mono or stereo).
- **`rv_app`**: `audio_policy.hpp` (GPU-free: the output, `chooseAudible`, `spatialSound`, `azimuthOf`,
  `audioDucked`). `ScreenService` gives each slot a mixer source and each opened clip a ring (`RingSink`), picks the
  audible screen, sets each source's gain, pan and clock every frame, and keeps a decoder's sound decoded only while it
  is heard, about to be, or fading. `RetroverseApp` makes the engine, mutes on F8, ducks under the card (Inspect), the
  directory and the quick catalog, pulls the null mix, suspends the output around a launch and writes the report's
  `audio` section (appended last). `--audio device|null|off`, `--mute`, `--audio-capture <wav>` (appended options),
  `RV_AUDIO`, `toggle_mute` (the last action, F8 in `default_bindings.json`, a global action).
- **No test plays sound**: `RV_AUDIO=null` in every test preset, in `scripts/real-library.ps1` and in
  `scripts/gate.ps1`'s Catch runs; `runApp` and `StartedApp::finish` check every report's `audio.device` is `null` or
  `off`.

## Decisions beyond ADR-0016

- **The drift correction is the mixer's**, not swresample's: swresample converts the rate on the decoder's thread,
  the source reads at 1 +- up to 1% by linear interpolation (exact copies at ratio 1). The correction grows with the
  error, full from 30 ms; once started it goes on until the error is within 5 ms (hysteresis), so a steady drift is
  held between 5 and 20 ms.
- **The ring drops, it never blocks**: a decoder never waits for its sound's reader; an overflow is counted
  (`audio.sync.overflowFrames`) and healed by the next resync.
- **A source's gain rises only once it sounds**, so a screen's fade-in starts with its first sample, not its request.
  A newly audible slot decodes its sound from where its demuxer stands (just ahead of the picture), padded with
  silence up to it.
- **The pan's centre is unity**, so a centred source passes bit for bit, and no channel exceeds unity: the louder
  side is 1, the quieter one follows the equal-power ratio (review: the gains scaled by sqrt 2 reached 1.345 at a pan
  of 0.6 and clipped a source near full scale).
- **"Visible"** is the scheduler's (in the frustum) and the eye in the bound room: in a hall the last room's screens
  are silent (`WorldSession::eyeInScreenRoom`).
- **`RV_AUDIO`** (trimmed, any case) forces `null` or `off` only, never `device`; other values are ignored with a
  warning. `video.json`'s `audio.mode`
  `off` builds no engine. A device that does not open falls back to the null output (reason `device-failed`). The
  model viewer reports `off`.
- **The null mix covers the elapsed time** (from the last pull to the frame's time), so the capture's frame n is the
  run's time `captureStartSeconds + n / 48000` whatever the steps.
- **Lockstep runs wait** (at most 100 ms) for the heard clip's sound to be decoded 40 ms past its picture, unless the
  decoder waits on its frame queue, so that a test's mix does not depend on the decoder thread's pace.
- **Mute fades in 10 ms, the duck in 100 ms** ("at once" without a click). Device latency is not compensated: the
  picture's own presentation latency is of the same order.
- **The effects and ambience buses** are read from `video.json` but carry nothing (I8); the clips' gain is
  `master x video x the screen's volume x the spatial gain x the duck`.

## Proofs

- Unit (`tests/unit/audio`, 19 cases): the ring's bounds (0.5 s at 48 kHz, a second written keeps half and counts the
  rest), chunk times, generations, a concurrent writer; the gain table (15 rows) and the pan table, equal power,
  the centre exactly unity; the sync's decisions and hysteresis; the mixer copying samples exactly in sync, a picture
  0.5% fast held within 22 ms by the correction alone, 80 ms behind caught up without a resync, a jump of 300 ms
  dropped and of -200 ms padded, a seek's generation, the cross-fade, the pause fade, the -9 dB duck, the mute, an
  empty ring; **an offline mix equal bit for bit over two runs** (two sources, pans, a change of audible source, a
  duck, a jump, a drift); the null output's frames (dt x rate, carried) and capture; the device path on miniaudio's
  null backend (callbacks render, suspend stops them, resume restarts). `tests/unit/media/test_audio_decode.cpp`
  (4): **96, 48 and 44.1 kHz tones, mono and stereo, resampled to 48 kHz: frequency within 0.5 Hz of 997 Hz, level
  within 0.1 dB, the same in both channels, no clipping** (0.98 full scale stays below 1.0, its level within 0.1 dB);
  beeps at their flashes' times within 1 ms; disabled, packets discarded, enabled mid-clip from there; loops and a
  seek's generation. `test_audio_policy.cpp` (3): the output table (`RV_AUDIO` over `--audio` over frame-limited),
  the audible screen's table, the azimuth. Command line, run report round trip (`audio` written last), the action
  table.
- GPU (`tests/smoke/test_audio.cpp`, `[audio]`, 4 cases on `screens_four`, the offline mix): **the beeps within
  20 ms of their flashes** (each flash's first frame from `audio.presented`, each beep's onset in the capture);
  **of two screens only the audible one in the mix** (the hero's 440 Hz; crt_c's 1000 Hz 96 kHz clip below -40 dB by
  Goertzel), and turned 40 degrees the other way round; **walking away**, the report's gains equal the table at its
  distances and the mix's level matches them and their ratio within 0.5 dB; F8 mutes and sounds again, the quick
  catalog ducks by 9 dB for its 90 frames. **Every smoke run** (`runApp`, `StartedApp::finish`) checks its report's
  `audio.device` is `null` or `off`, and that a retroverse.exe run that exits 0 has one.
- Real library (`scripts/real-library.ps1 -Filter '[real-library][audio]'`, Release, ES-DE closed): passed, 0
  differences among 542,120 entries.

## Review

Two reviews found no blocker; the fixes (ADR-0016 §9's amendment of the review records the final behaviour):
- **Loops**: a clip's sound is trimmed at its picture's end (the last finished pass's end, else the video stream's
  declared duration) and the audio decoder and swresample are drained at each pass's end, so a sound longer than its
  picture no longer steps back 0.5 s at every loop, and the last frame is no longer lost
  (`test_audio_decode.cpp`: a sound 1.5 s and 0.6 s long on a 1 s picture, four loops, each pass's sound starting at
  its picture's start within 1 ms). A mono clip's stereo span is made after its up-mix (it reached past the vector).
- **The mixer never allocates while rendering** (the device's thread, under its mutex): the sync's errors go into a
  fixed histogram (10 us bins to 200 ms and one beyond; the percentiles a bin's upper edge, never above the exact
  largest), the scratch is sized for 8192 frames and a longer block is rendered in pieces.
- **The pan** is capped at unity per channel (above) and ramped per sample (at most 1 in 50 ms; a silent source takes
  its pan at once). **Samples that are not finite** are written to the ring as 0 and the output turns anything not
  finite into silence. **A new ring** on a source that still sounds waits for the old clip's 30 ms fade on its own
  clock instead of cutting it.
- **The audible screen** keeps the sound until a screen of its class is 0.5 m nearer (the ambient requests' margin);
  a better class, or the screen no longer qualifying, changes it at once. **`RV_AUDIO`** is trimmed and case-folded,
  and an ignored value is logged once.
- **The device** is retried when miniaudio reports it stopped (at once) or when it does not start again after a launch
  (every 2 s), never while suspended; the report's `audio` section counts `deviceFailures`, `deviceLosses` and
  `deviceRecoveries` (proven on miniaudio's null backend with injected failures and a simulated loss). **Before a
  launch** the fade begins before the screens' decoders stop (`AudioEngine::beginSuspend`), and `suspend()` waits only
  for what remains of it, not 70 ms after them.
- **No test can open the device**: `runCommand` and `StartedApp` set `RV_AUDIO=null` for every app they start, whatever
  the caller's environment. `unit.cmake/layer_rules` proves `rv_audio`'s rules (FFmpeg, `rv_media`, SDL3, RmlUi).
- **`rv_audio_model`** (`audio_output.hpp`, `spatial.hpp`, no miniaudio) is what `rv_app_cli` links; `audio_policy.hpp`
  no longer includes the engine.
- Docs: `docs/architecture.md`'s `audio` row, `AudioSink` in `media`'s, `audio_policy` in `app`'s; the exclusive-mode
  check of `i5/plan.md` moves to the user's listening.

## Results

- Debug unit tests: 2049 of 2049 passed. Release unit and GPU (`msvc-x64-release-gpu`): 2180 of 2180 (one skipped by
  design; 131 GPU cases, the 4 audio cases included). G01 to G18 0 pixels apart (largest difference 0/255).
- Real library: the 96 kHz case passed through `real-library.ps1`, 0 differences.
- After the review: Debug unit tests 2059 of 2059 passed (`tests/unit/audio` 27 cases, `test_audio_decode.cpp` 5,
  `test_audio_policy.cpp` 4). Release unit and GPU: 2190, one skipped by design; the walking-away case first failed on
  the old pan law's level and passes since its expectation includes the capped pan (the 4 `gpu.audio` cases rerun:
  4 of 4). G01 to G18 passed unchanged. The real-library case was not rerun: the decoder's changes act at a loop's end
  and the case plays 8 s of a 38 s clip.

## Measurements

- **Sync, real library**: the first 96 kHz SNES clip in name order (AAC stereo 96 kHz, H.264 640 x 480 at 30 fps,
  38 s; the third clip probed of 1470) played 8 s at 60 Hz: the sound against the frame shown **p50 14.8 ms, p95
  29.4 ms, max 31.3 ms** (acceptable: 80 ms at p95), most of it the 33 ms a 30 fps frame stays up; the mixer's own error
  p95 0.004 ms, no resync, no correction, no underrun, no ring overflow, no decode error.
- **Sync, test clip** (Release and Debug, lockstep): beeps 0.01 to 0.07 ms from their flashes; the mixer's error p95
  0.02 ms, no resync.
- **Two screens**: the other's tone at -119 dB (facing) and -221 dB (turned): its decoder discards its sound.
- **Walking away** (proximity 7.5 m): 4.36 m gain 0.344 heard 0.344; 5.56 m 0.262 heard 0.258 (-0.14 dB, mid-walk);
  6.65 m 0.096 heard 0.096. After the review's pan cap (both channels' RMS, the screen off the centre): 4.36 m at pan
  -0.49 expected 0.2643 heard 0.2643 (-2.28 dB from the gain alone); 6.65 m at pan -0.32 expected 0.0790 heard 0.0790.
- **Duck**: -9.00 dB.
- Frame time and GPU memory: no row changes (the mix is a few microseconds a frame; no texture).

## Open issues

- **The device path is proven on miniaudio's null backend only**: no automated run opens WASAPI (by design). The
  user's listening is the proof of the real endpoint.
- **Exclusive-mode emulators** after the device stops (i5/plan.md's risk) were not tried: that needs a real emulator
  (RetroArch with an isolated configuration) and the user's ears.
- Device latency is not compensated (a WASAPI shared period, about 10 ms); if the user hears the sound late, the
  stamp can lead by the device's reported latency.
- A screen that becomes audible starts from where its demuxer stands, so its first ~100 ms are silence before its
  fade-in (the cross-fade still takes 300 ms).
- Operating-system audio sessions are not watched by `focus_watch.py` (a possible extension); the tests' guarantee
  is `RV_AUDIO=null` and the report's assertion.
- The ring drops on overflow: a clip whose sound is muxed far ahead of its picture (over 0.5 s) would lose sound and
  resync; none seen (the real clip: 0 frames dropped). Step 7's library-wide scan can count such files.

## What the user should listen for

Run interactively (the default output is the device; `--mute` or F8 for silence), e.g. `retroverse.exe --start-room
room:snes`:
- the hero CRT's clip sounds, its sound on its picture (lips and hits on time), the 96 kHz clips at the right pitch;
- walking away the sound falls, beyond `proximityMeters` (12 m) it is gone; it pans with the head;
- turning so the CRT leaves the view: its sound fades out in 30 ms (it pauses);
- opening the card (inspecting an item), the directory (M) or the quick catalog (Tab): the sound drops by 9 dB;
- F8 mutes at once and sounds again;
- launching a game: the sound fades out before the emulator starts, the emulator's own sound plays normally (also an
  emulator in exclusive mode), and the clip's sound returns after the game;
- in a hall between rooms, silence.
