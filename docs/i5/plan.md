# I5 plan: M4, screens and media

This file holds the I5 section, ready to paste into `docs/roadmap.md` above the I6 section, the row for the iteration
table, how the steps sit beside I4b's, and the decisions made on the user's behalf. The decisions themselves are
[ADR-0016](../decisions/ADR-0016-screens-and-media.md).

## Iteration table row (replaces the I5 row)

| I5 | M4 screens and media (ADR-0016): the decoder scheduler (2 decoders in Balanced, 1 in Low, priorities, visibility, shared textures, staggered uploads, the priority boost), playlists, secondary TVs, arcade cabinets playing clips, clip audio (miniaudio, spatial, one dominant source), CRT presets and the picture's aspect, fallbacks and video verdicts, videos and manuals in the media viewer, the insertion animation | Gate M4: every room has working TVs within the decoder budget, M4 Done |

## Section for the roadmap

```markdown
## I5: M4, screens and media

[ADR-0016](decisions/ADR-0016-screens-and-media.md), decided on October 27, 2026 on the user's "procedi" (its choices
are listed in `docs/i5/plan.md`, "Risks and open questions"):
- **The decoder scheduler:** 2 decoders in Balanced, 1 in Low, priority classes, visibility, shared textures,
  staggered uploads, the process priority boost.
- **Playlists:** the hero screen's playlist, and secondary TVs as a theme pack's furniture.
- **The arcade's cabinets** play their own clips.
- **The clips' sound:** miniaudio, spatial, one dominant source, never audible in tests.
- **The picture:** CRT presets and the picture's aspect.
- **Fallbacks** ending at a generated title card, and video verdicts.
- **The media viewer:** videos and manuals.
- **The insertion animation.**

The media manager and its downloads stay out (ADR-0003).

Every step shows what it adds on screen or through the speakers, and is reviewed for correctness, for stability and
the ADR, and for the look. In tests, clips are generated at test time: tone tracks at 44.1, 48 and 96 kHz,
portrait and 16:9 clips, and the damaged kinds of SC-08. Goldens freeze every screen with `--video-freeze-pts`, and
every test runs with `RV_AUDIO=null`.

| # | Step | Needs | Proof | Status |
|---|---|---|---|---|
| 0 | ADR-0016, this plan | | The files exist | done |
| 1 | Playlists and fallbacks on the hero screen (one decoder still): `app::Playlist` (shuffle, sequential, favourites, recent, explicit), the hero's start choice then its playlist, a selection that interrupts it and gives it back after its clip, `video.json` read (defaults and the user's overrides), the generated title card at the chain's end, the `standby` pattern, the previous clip's last frame held between clips | 0 | Unit: each mode's order as a table, no repeat within a cycle, the selection's hold, unknown explicit ids skipped. GPU, at `--fixed-dt` with a 2 s `previewDuration`, three clips in the room: the playlist's games in the unit table's order; no frame of a switch shows standby or bars (probes); a game without media shows its card. G01 to G18 0 pixels apart | done ([step-1.md](step-1.md)) |
| 2 | The decoder scheduler: `app::ScreenScheduler` (classes, sharing, visibility pause then stop after 10 s, hysteresis, the late-frame drop to one decoder), `ScreenService` on several screens and slots, NFR-07's assertion on the budget, uploads at most 4 MiB a frame, the above-normal priority while rendering, `--max-decoders`; on a smoke scene of four CRTs | 1 | Unit: the scheduler's tables, never more threads than the budget, a slot reopened only after its thread ended. GPU (`screens_four`): 2 decoders for 3 clips on 4 screens, two of them one texture; a screen turned away paused within a frame and stopped after 10 s; `--max-decoders 1` gives one; two 720p clips due together never upload more than 4 MiB in a frame; the report's priority above normal while rendering and normal during the fake emulator's game; G01 unchanged | done ([step-2.md](step-2.md)) |
| 3 | Secondary TVs: the `secondary_tv` piece (a sideboard and a small CRT) in the packs' furniture, at most one per room (two past 12 m), none in the arcade or the hub; the `ambient` class with `proximityMeters` | 2 (beside 4, 5, 7) | Plans and plan goldens byte-identical; scene digests rewritten; GPU: walking from the doorway, the secondary takes the second slot within its proximity and in view while the hero keeps the first, and with one decoder it shows its still; the goldens where it stands regenerated on purpose, and new G19 (a console room, both screens frozen at 1.0 s); real library: the busiest floor with both slots playing at most 512 MiB, 0 differences | done ([step-3.md](step-3.md)): the real busiest floor 465.5 MiB with both slots playing |
| 4 | Arcade cabinets playing clips: the focused or inspected cabinet's screen plays on the dynamic path (pooled video materials, its static instance hidden), attract on the nearest cabinet facing the eye within 3 m, the hero cabinet never following the selection; the picture rectangle in `MaterialUniforms` (letterbox and pillarbox, never stretched) | 2 (beside 3, 5, 7) | Unit: the attract choice, the rectangle's table (4:3, 16:9, portrait on 4:3 and 3:4). GPU: the focused cabinet's screen shows the clip's frame counter, then its cover again when Back; a 16:9 clip on a CRT shows black bars (probes); G08a and every 4:3 screen 0 pixels apart; new G20 (a cabinet playing in Focus, frozen); real arcade room: clips on the cabinets, 0 differences | done ([step-4.md](step-4.md)) |
| 5 | The clips' sound: `rv_audio` (miniaudio from vcpkg, the null and offline mixer, `--audio`, `--mute`, `RV_AUDIO`), audio decoded on the decoder's thread and resampled by swresample, the sync to the video's media time, one dominant source with a cross-fade, spatial gain and pan, ducking under the card, the directory and the quick catalog, `toggle_mute` (F8), the device stopped around a launch; the test presets and `real-library.ps1` set `RV_AUDIO=null` | 2 (beside 3, 4, 7) | Unit: 96, 48 and 44.1 kHz tones resampled (frequency within 0.5 Hz, level within 0.1 dB, no clipping), the gain and pan table, the sync's correction and resync, the ring's bounds, an offline mix equal bit for bit over two runs. GPU (offline mix): a clip's beeps within 20 ms of its flashes; of two screens only the audible one in the mix (the other's tone below -40 dB by Goertzel); walking away lowers the level as the table says; every smoke case asserts `audio.device` null or off. Real library: a 96 kHz SNES clip's sound within 80 ms of its picture; the user hears the real room | done ([step-5.md](step-5.md)); the user's listening pending |
| 6 | CRT presets: `crt_presets.json` (neutral, consumer, arcade, monitor), chosen by `crtEffect`, the room, the pack's `screenPreset` and the screen kind; the arcade's and the optical-era lounge's presets | 4 | `consumer` equal to today's values: every golden but the lounge's 0 pixels apart; each preset on a test scene, reviewed, with probes (neutral: no row modulation; arcade: deeper than consumer); G17 regenerated on purpose | done ([step-6.md](step-6.md)): G15, G17 and G20 regenerated on purpose, every other golden 0 pixels apart |
| 7 | Missing media: the video verdicts (`cache/media/videos.json`), clips known to fail never opened, `retroverse-cli media --check-videos`; the library-wide check of ADR-0004's thresholds; a `[.soak]` hour with the renderer (informative) | 2 (beside 3, 4, 5) | Unit: the verdicts' keys, a change of size or time checked again, a byte-identical round trip; the CLI on each damaged fixture kind gives its verdict. GPU: a known-bad clip is opened 0 times, its screen on its still from the first frame. Real library (`real-library.ps1`): the SNES clips' verdicts per kind, each failure looked at in the step note, the whole library with `-FullVideoScan`, 0 differences; the soak's private bytes flat | done ([step-7.md](step-7.md)); the soak flat since the review (15 minutes, +0.7 MiB; the hour at Gate M4) |
| 8 | Videos and manuals in the media viewer: the `videos` page with sound (`direct`, 720 rows, a `GL_RGBA8` view as `rv-texture:media-viewer-video`), the world paused under it; the `manuals` page opening the PDF through `rv_launcher` | 5 | Unit: the pages with videos and manuals; the manual's plan `explorer.exe` plus one argument (hostile names stay one argument); the guard refusing it under the allowlist. GPU: frames advance on the viewer's page (counter read from the screenshot); the world's decoders paused while it is open; the offline mix holds the viewer's clip only; a damaged clip shows the page's fallback with its reason | |
| 9 | The insertion animation: `AnimatingToConsole` (450 ms to the console's media socket, skippable, instant under `reduceMotion`), the hand-over after both it and the decoders' stop | 2 (after I4b's interaction changes) | Unit: the transition table (skip, `reduceMotion`, a refused launch back to Inspect with the item in hand, none in the arcade or without a console). GPU with the fake emulator: the item at the socket within 1 cm at 450 ms; a skip launches within a frame; the hand-over after the decoders stopped; back in Inspect with the item on its shelf, the session verified | |
| 10 | Gate M4: `scripts/gate.ps1 -Gate M4` (and `-RealLibrary`), `docs/testing/gate-m4.md`, documentation | 1-9 | The gate passes; the soak's growth explained and bounded (`rv_smoke_tests "[soak]"` in Release, its `memory.csv` in `gate-m4.md`); the user tries the screens and the sound and declares M4 done | |

Order: 1, 2, then 3, 4, 5 and 7 (parallel worktrees: each owns its files), then 6 after 4, 8 after 5, 9, 10.

### Gate M4

`scripts/gate.ps1 -Gate M4` runs everything Gate D runs, under `scripts/focus_watch.py`:
- the build matrix (no warning);
- the unit tests and the GPU smoke tests in Debug and Release;
- every golden in both, G19 and G20 added;
- a row per I5 area counted from those runs: playlists, the scheduler, secondary TVs, cabinets, audio, presets,
  verdicts, the viewer, the insertion;
- the benchmark path in Release with two clips playing, against the acceptable rows (ADR-0016 §12).

It prints the summary table (logs in `build/gate-m4`) and exits 1 when a step failed.

`-RealLibrary` adds, with ES-DE closed and `RV_AUDIO=null`, one `scripts/real-library.ps1` run with snapshots before
and after and 0 differences:
- **every collection room of the real building** started with `--start-room` for a few seconds: its hero screen shows
  a clip or its chain's still, never standby while the room has media, with at most 2 decoders;
- the busiest floor's GPU memory with both slots playing;
- the arcade's cabinets playing;
- a 96 kHz clip's sync;
- the SNES clips' verdicts;
- Gate D's real rows.

`-FullVideoScan` adds the whole library's verdicts.

`docs/testing/gate-m4.md` holds:
- the figures;
- where ADR-0016 and M4's requirements are proven: FR-015 to FR-017, FR-030, NFR-06, NFR-07, SC-03, SC-08, spec §9;
- the follow-ups;
- what to try: a room's playlist and a selection, a secondary TV, the arcade, the sound walking away, mute, the
  viewer's video and a manual, a launch with its animation.
```

## Acceptable performance rows to add

To be added to the roadmap's table with the section (ADR-0016 §12):

| Measure | Acceptable |
|---|---|
| Frame time with two clips playing (the benchmark path past a secondary TV) | p50 at most 16.7 ms, p99 at most 33.3 ms |
| GPU memory with the screens playing (the busiest floor, two slots; the viewer's clip) | all textures at most 512 MiB |
| A clip's sound against its picture | within 80 ms at p95 |

## Beside I4b

I4b (customization) changes `rv_world`, `rv_app` and `rv_ui`:
- the overrides and pins editor;
- regeneration with an impact preview;
- backup and undo;
- expansion rooms.

The I5 steps sort into three groups by how much they share with it.

**Free to run beside I4b at any time.** These keep to new files and a few hooks:
- step 1: `playlist.hpp/.cpp`, `screen_policy`, `video_settings`;
- step 2: `screen_scheduler`, `screen_service`, the priority in `rv_platform_win`;
- step 5: `rv_audio`, `rv_media`'s audio, `vcpkg.json`, `cmake/LayerRules.cmake`;
- step 6: `rv_render`'s CRT block, `assets/materials`;
- step 7: `cmd_media.cpp`, the verdict cache.

**Merge after I4b's matching step:**
- **Step 3**: `src/world/src/scene_builder.cpp`, `room_look.hpp/.cpp`, `assets/world/themes/*.json`, the floor scene
  digests and the room goldens. I4b's expansion rooms build scenes and rewrite plan goldens there.
- **Step 4**: `collection_instance.cpp`, the dynamic path in `world_session.cpp`. I4b's editor likely previews moves
  and pins there.
- **Step 8**: `product_ui.hpp/.cpp`. I4b's editor and regeneration screens are added there.
- **Step 9**: `world_interaction.hpp/.cpp`, `world_inspect`, the card's actions in `assets/ui/game_card.rml`. I4b's
  pin action and editor state are likely there.

**Files both iterations touch.** These are merged by hand, the second to merge rebasing:
- `src/app/src/world_session.{hpp,cpp}` and `retroverse_app.cpp` (wiring);
- `run_report.{hpp,cpp}` (new sections);
- `command_line.{hpp,cpp}` (new options and `--help`);
- `assets/config/default_bindings.json` and the action table in `rv_input`: `toggle_mute` against the editor's actions;
- `tools/cli/src/commands.hpp` and `command.cpp` (the command table);
- the CMake source lists of `rv_app`, `rv_ui` and `rv_world`;
- `tests/unit/CMakeLists.txt` and `tests/smoke/CMakeLists.txt`;
- `scripts/gate.ps1`: Gate M3 and Gate M4 side by side; whichever lands second adds its branch;
- the goldens in `assets/style_tests/screenshots`: a golden both regenerate is regenerated again by the second;
- the docs: `docs/roadmap.md`, `CLAUDE.md`, `docs/reference/running-the-app.md`, `layout.md`, `docs/architecture.md`,
  `BACKLOG.md`.

The ADR numbers are I4b's ADR-0015 and this one's 0016.

## Risks and open questions

Each of these was decided on the user's behalf. The ADR records the decision; here are the reasons and the fallbacks.

**Decided on the user's behalf**
- **The insertion animation is included** (step 9, small). It hides the decoders' stop before a launch. It is
  skippable and instant under `reduceMotion`. It is the first thing to drop if I5 runs long.
- **Manuals open in Windows' default PDF program** through `rv_launcher`. In-app pages would need pdfium or mupdf,
  neither of which is at the pinned vcpkg baseline (poppler is GPL), so they wait for a new ADR.
- **World screens decode at 480 rows** in both profiles, the viewer at 720. The real clips are 480p, and this keeps
  the busiest floor near 495 MiB of 512.
- **The render clock stays the master; audio follows it.** This keeps `--fixed-dt` runs deterministic and the proven
  video pacing. The offline mixer makes the sound testable.
- **One audible screen at a time,** and no interaction or ambient sounds until I8: they need original sounds with
  provenance.
- **Secondary TVs are a pack's look** (no plan change, no migration): at most one per room, two in a room grown past
  12 m.
- **In the arcade the selected cabinet plays its own clip;** the hero cabinet keeps the playlist.
- **After a selection, the hero returns to its playlist** once the selected clip has played (SC-03), where ADR-0012
  §7 kept it looping.
- **The config file is `video.json`,** after the specification's `[video]` section, kept apart from `world.json`,
  which I4b may change. The mute state is the run's; nothing is written.
- **The decoder budget is set by `video.json`'s `maxDecoders`** until I8's graphics profiles exist. At most 2: no
  High profile with more.
- **No prefetch of the next clip,** no quick catalog video, no hub screen video, no LCD screen.
- **The colour bars stay only in the test room,** so G01 is unchanged; the building's screens get the `standby`
  pattern and the title card.
- **ExplicitPlaylist is data only** (GameIds in `video.json`): editing it in the app belongs to I4b's editor or I7's
  settings.

**Risks**
- **GPU memory near the limit.** Two slots and the viewer's add about 24 MiB to the busiest floor's 471.8 MiB (at 720p;
  505.8 MiB at 1080p, step 3's review). The memory margin ([memory-margin.md](memory-margin.md)) took 28.3 MiB out of
  the 1080p render targets (88.5 to 60.2 MiB) and a screenshot's own set (161.0 to 60.2 MiB at the window's size): about
  477.5 MiB expected at 1080p, measured again at the gate. The next remedy is per-floor residency of the procedural
  surfaces (BACKLOG), not lower video quality.
- **Late frames under load.** The late-frame drop to one decoder and the priority boost are the answers. If Gate M4's
  frame rows still fail, the contingencies are zero-copy conversion into the PBO, then YUV planes (ADR-0004).
- **Badly interleaved or variable frame rate clips** may starve the audio ring. Silence is padded and the video never
  waits. Step 7's scan lists such files.
- **miniaudio's header under `/W4 /WX`.** Its headers come from vcpkg as external includes, as stb's do. If its
  implementation file still warns, it is wrapped like stb's implementation files, never by lowering warnings in our
  code.
- **Sound on the user's machine.** `RV_AUDIO=null` in the presets, null by default in frame-limited runs, and an
  assertion in every smoke case. Operating-system audio sessions are not watched (a possible extension of
  `focus_watch.py`).
- **The emulator's audio after a launch.** The device stops before the hand-over. Exclusive-mode emulators were not
  checked by hand at step 5 (no automated run may open the device or start a real emulator): the check moves to the
  user's listening (`docs/i5/step-5.md`, "What the user should listen for").
- **Above-normal priority while rendering** could compete with the user's other programs. It is dropped while
  minimized and during games, and the report records it.
- **Shared files with I4b** (above). The steps that collide wait for I4b's matching merges.
