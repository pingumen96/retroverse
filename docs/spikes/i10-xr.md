# I10 step 0 spike: an OpenXR session on Retroverse's OpenGL context

Decides ADR-0017 §2 (the graphics binding) and gives §9's limits their first figures. The questions:

1. Does each runtime the user plays on accept the app's GL 4.5 context through `XR_KHR_opengl_enable`? The runtimes:
   Meta Horizon Link (cable and Air Link) and SteamVR.
2. What does each offer? The swapchain formats (`GL_SRGB8_ALPHA8`?), the reference spaces (LOCAL_FLOOR, STAGE), the
   refresh rates (72/80/90/120 Hz through `XR_FB_display_refresh_rate`), the recommended size per eye and the fields.
3. With a trivial scene, how does frame pacing look? The display period, the refreshes missed, and the CPU and GPU
   time per frame. That is the loop's own cost, before the renderer is in it (step 7 measures that).
4. Is the stereo picture right? The room stays put while the head moves, the cube floats at a believable depth, the
   floor is at the feet (LOCAL_FLOOR or STAGE), the eyes' distance matches the headset's IPD setting.

## The spike

`spikes/xr` (`spike_xr.exe`, built by every preset with `RV_ENABLE_XR`):
- **The context:** an SDL window with the app's GL 4.5 core context (`rv_platform`), its HDC and HGLRC handed to
  `xrCreateSession`.
- **The swapchains:** one per eye at the recommended size times `--render-scale`.
- **The scene:** a 6 x 6 m room ray-cast in one fullscreen triangle per eye from the eye's inverse view-projection.
  The floor's ring marks the reference space's origin, its arrow points to -Z, and the green wall is in front. A yellow
  line runs round the walls at 1.60 m, and a cube turns 1.2 m ahead at 1.3 m.
- **The mirror:** the left eye in the window.

It is GPU-free where it can be (`spike_xr_core`: the off-axis projection, the frame statistics, the format choice),
tested by `spike_xr_tests` (label `unit`).

```powershell
# What the active runtime and the headset offer (no session; no headset needed for the runtime's part)
build/msvc-x64-release/bin/spike_xr.exe --list --data-dir build/spike_xr --report build/spike_xr/list-meta.json

# 30 s in the headset on the active runtime (Meta's): start Link on the headset first
build/msvc-x64-release/bin/spike_xr.exe --seconds 30 --data-dir build/spike_xr --report build/spike_xr/meta-72.json

# Another refresh rate, another render scale
build/msvc-x64-release/bin/spike_xr.exe --refresh-rate 90 --report build/spike_xr/meta-90.json --data-dir build/spike_xr
build/msvc-x64-release/bin/spike_xr.exe --render-scale 1.3 --report build/spike_xr/meta-72-x1.3.json --data-dir build/spike_xr

# SteamVR without changing the system's active runtime (its manifest, from SteamVR's install folder)
build/msvc-x64-release/bin/spike_xr.exe --runtime-json "<SteamVR>/steamxr_win64.json" --report build/spike_xr/steamvr.json --data-dir build/spike_xr
```

**Options:**
- `--space auto|local-floor|stage|local`;
- `--wait-system-s` (how long to wait for the headset, 15 s);
- `--frames`;
- `--warmup-s` (the first 2 s reported apart);
- `--max-missed` (0.01);
- `--no-mirror`, `--hidden`;
- `--source-revision`.

Esc or closing the window ends the session; so does quitting it from the headset's menu.

**Exit codes:**
- 0: the criteria were met (frames rendered, at most 1% of the refreshes missed after the warm-up);
- 1: they were not;
- 2: initialization failed. The report's `result.reason` says why: no runtime, no headset, no OpenGL;
- 3: a runtime failure.

**The report:**
- the runtime: its name, version, extensions and enabled ones;
- the system: its name, eyes, blend modes and OpenGL requirements;
- the session: the spaces, formats, eye size, images and refresh rates;
- the event timeline;
- `frames` and `warmUp`: the display period and refresh rate, the missed refreshes, how far apart consecutive
  predicted display times were (`gapsInPeriods`), CPU and GPU percentiles. A missed refresh is one that no new frame
  reached: the refreshes the run spans less the intervals between its frames. Counting each interval on its own took
  the jitter of Meta's predictions over Link (a long interval, then a short one) for misses;
- `runtimeCounters`: the runtime's own counters when it offers `XR_META_performance_metrics` (the compositor's dropped
  frames, its view of the app's CPU and GPU time), sampled every 0.5 s over the measured frames;
- the eyes at the start (poses and fields in degrees), the median eye distance, the head's range, and poses every
  0.5 s.

The spike's few direct GL calls are its exception to the module rule; the product adds the helpers to `rv_render` in
step 2. These calls are: an eye's framebuffer bound, the draw call, the framebuffers wrapping the swapchain textures,
and the mirror's blit.

## Results

To be measured in the headset (Release, the reference machine, Quest 3S over the Link cable, then Air Link; SteamVR
if installed: on 2026-10-29 only Meta's runtime is registered on the machine).

**First runs (2026-10-29, Meta Horizon Link 1.208, Quest 3S):**
- **The active runtime:** the registry's `ActiveRuntime` named a manifest under `C:\Program Files\Oculus` that no
  longer existed (the app had moved to another drive), so the loader found no runtime. Meta Horizon Link's settings
  (General, OpenXR runtime) make it active again; `--runtime-json` with the installed manifest works meanwhile. The
  VR mode's log has to say this plainly (step 2).
- **OpenGL:** accepted. The session reached FOCUSED with `GL_SRGB8_ALPHA8` swapchains (3 images per eye) at the
  recommended 2080 x 2096 per eye, in LOCAL_FLOOR (STAGE and LOCAL offered too); OpenGL 4.0 to 4.6.
- **The fields:** asymmetric, 52° outwards, 45° inwards, 48° up, 50° down per eye; the eyes 68 mm apart.
- **The refresh rate:** the runtime offered 72 Hz only, so asking for 90 Hz returned
  `XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB`. Over Link the rates come from Meta Horizon Link's graphics settings
  for the headset.
- **Pacing:** CPU p99 2.2 to 3.9 ms, GPU 0.25 ms per frame (both eyes). The reports counted 25 missed refreshes
  (1.2%), but the 28 measured seconds at 72 Hz hold 2016 refreshes and the runs delivered 2018 and 2017 frames:
  the old count read the predictions' jitter as misses. It was replaced (above); the runs are to be repeated.

| Run | Runtime | OpenGL accepted | Format | Space | Eye size | Refresh | Missed | CPU p99 | GPU p99 |
|---|---|---|---|---|---|---|---|---|---|
| | | | | | | | | | |

**Decision for ADR-0017 §2:** pending these runs.
