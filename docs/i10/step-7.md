# I10 step 7: performance

Roadmap row: I10 step 7. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §9. Built on step 6's game on
the CRT ([step-6.md](step-6.md)).

## What was built

- **The dynamic render scale** (`xr::RenderScale`, `rv_xr_model`): the eyes render at a share of their images' size,
  scaled up into them (`Framebuffer::copyColorTo` filters when the sizes differ). The share goes down by 0.05 while the
  GPU's smoothed time per frame (both eyes: twice the last eye's passes, the shared shadow map once) stays above 85% of
  the display's period, and up again below 65%, within [0.6, 1]; a change waits 36 frames for the next, so that the
  timers' latency and one frame's spike move nothing. While a game shows on the CRT it stays at 0.8 at most (the "in
  game" profile: the GPU shared with the emulator and the capture; the room's decoders already stop for a launch).
  Interactive runs only: frame-limited runs keep 1 and render alike every time.
- **72 and 90 Hz**: xr.json's `refreshRate` (step 3) asks the headset through XR_FB_display_refresh_rate; 0 keeps the
  runtime's own (72 Hz on a Quest through Link unless the Meta app says otherwise).
- **The VR benchmark path** (`tests/smoke/test_world_benchmark.cpp`, `[.bench]`): the snes-stress room of 2,000 titles
  along `tests/input_scripts/bench_snes_room.json` (the desktop's benchmark path), both eyes at 2048 x 2048 (the fake
  runtime at render scale 2: about Link's 2080 x 2096 on a Quest 3S), render scale 1, the fake runtime pacing nothing;
  in Release its frame times against §9 (p50 at most 13.9 ms, p99 at most 20 ms).
- **The run report's `xr` section**: whether the dynamic scale ran, its changes and its lowest, the eyes' smoothed GPU
  time.

## Proof

- **Unit tests** (`test_render_scale.cpp`): the steps down over budget to the minimum, holding in between, back up to
  the maximum; a spike moving nothing; the "in game" bound at once; bad times.
- **The VR benchmark path in Release** (the reference machine, RTX 2080 SUPER): within §9, see "Figures".
- **The step's commit in Release** (a worktree of its own): the unit suite (2,236 tests), the golden and `[xr]` GPU
  tests, the desktop goldens' 35 images identical to step 2's, G21 identical to its Debug-recorded golden.

## Figures

The VR benchmark path in Release on the reference machine (RTX 2080 SUPER, driver 621.01, October 30, 2026; the
verification of the step's commit in a worktree of its own, `build/verify-step7-vrbench.log`), over the "benchmark"
phase's 1,230 frames:

| Measure | p50 | p95 | p99 | Limit (§9, 72 Hz) |
|---|---|---|---|---|
| Frame time | 7.20 ms | 8.27 ms | 8.59 ms | p50 13.9 ms, p99 20 ms |
| Both eyes' GPU (twice the last eye's passes) | 6.01 ms | | 7.35 ms | |
| The session's CPU per frame (xrWaitFrame to xrEndFrame) | | | 7.67 ms | |

One eye's passes at p50: shadow 0.06 ms, depth 0.08 ms, SSAO 0.99 ms, the lit scene 1.07 ms, post 0.82 ms, 3.01 ms in
all. The longest frame (177 ms) is one frame of the whole phase; p99 is untouched by it. Both eyes at Link's size fit
72 Hz with about half the period to spare, and 90 Hz (11.1 ms) at p99 too: the dynamic scale stays at 1 in this room,
and no quality profile of its own is needed.

## Left for later

- A quality profile of its own for VR (fewer bloom levels, the SSAO at a lower resolution): not needed at these
  figures; the dynamic scale takes the load when a room or a captured game asks for more.
- The headset's own measures (missed refreshes on the cable, 90 Hz): the user's session (the run report's `xr`
  section).
