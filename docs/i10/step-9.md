# I10 step 9: 120 Hz

Asked by the user on October 30, 2026, after step 7's figures: a Quest 3S's highest refresh rate, 120 Hz, an 8.3 ms
frame. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §9, amended with a 120 Hz row (below). Built
on step 7 ([step-7.md](step-7.md)); made and measured without the headset, with the fake runtime.

## Where the time went

Step 7's VR benchmark path in Release spent the frame on the GPU: the session's CPU per frame was 0.4 ms at p50 (the
update and the submission of both eyes), the frame 7.2 ms at p50 and 8.6 ms at p99, most of it waiting for the GPU
(both eyes about 6 ms, one eye's ambient occlusion 1.0 ms and post-processing 0.8 ms of its 3 ms). At 120 Hz the GPU is
what has to shrink; neither a D3D11 nor a Vulkan bridge would change it.

## What was built

- **The refresh rate asked for** (`xr::chooseRefreshRate`): the highest rate the headset offers that is not above the
  one asked for (XR_FB_display_refresh_rate), from `--xr-refresh-rate <Hz>` (new, over xr.json) or xr.json's
  `refreshRate`. The OpenXR runtime logs a rate it had to change. The fake runtime offers a Quest 3S's 72, 80, 90 and
  120 Hz and switches its display period with a RefreshRateChanged event, so that the session, the dynamic scale and
  the report run at 120 Hz as on the headset.
- **xr.json's quality** (`"quality": "auto" | "full" | "fast"`): fast leaves out the ambient occlusion and the
  anti-aliasing in the eyes (the lens and the panel's resolution hide FXAA's work; the occlusion is the most costly
  pass for its share of the picture); auto takes it from 110 Hz on (120 Hz), full below. The run report's `xr.quality`
  says which the eyes had.
- **The dynamic render scale** (step 7) follows the display's period: at 120 Hz it lowers the eyes when their GPU time
  stays above 85% of 8.3 ms.
- **The VR benchmark path at 120 Hz** (`tests/smoke/test_world_benchmark.cpp`, `[.bench]`): step 7's path with
  `--xr-refresh-rate 120`, both eyes at 2048 x 2048, render scale 1, the fast profile; Gate VR runs it with step 7's.

## Figures

Both VR benchmark paths in Release on the reference machine (RTX 2080 SUPER, driver 621.01, October 30, 2026; the
verification of the step's commit in a worktree of its own, `build/verify-step9-vrbench.log`), each over the
"benchmark" phase's 1,230 frames, both eyes at 2048 x 2048:

| Run | Frame p50 | p95 | p99 | Both eyes' GPU p50 / p99 | One eye's SSAO / post at p50 | Limit |
|---|---|---|---|---|---|---|
| 72 Hz, full (step 7's) | 7.12 ms | 8.29 ms | 9.62 ms | 5.84 / 7.59 ms | 0.95 / 0.80 ms | p50 13.9 ms, p99 20 ms |
| 120 Hz, fast | 4.81 ms | 5.34 ms | 6.44 ms | 3.39 / 4.61 ms | 0.06 / 0.55 ms | p50 8.3 ms, p99 11.1 ms |

The fast profile takes an eye from 2.92 ms to 1.70 ms of GPU at p50 (the occlusion's pass left out, the post-processing
without FXAA), and at 120 Hz the frame keeps about 40% of the period for the compositor and Link's encoder even at p99.
Each run has one long frame (about 300 ms, once in the 1,230; p99 untouched): a hitch of the room's path to look into
in the headset, where the runtime's reprojection keeps the head's view moving through it.

## ADR-0017 §9's 120 Hz row

| Measure | Acceptable |
|---|---|
| Frame time in VR at 120 Hz, a populated room, both eyes at the runtime's recommended size, render scale 1, the fast profile | p50 at most 8.3 ms, p99 at most 11.1 ms (never below 90 Hz's period); in the headset at most 1% of the refreshes missed |

## By hand, in the headset

- The Meta Horizon Link app: the headset's graphics preferences, refresh rate 120 Hz (it may sit among the
  experimental features); without it Link offers 90 Hz at most and Retroverse asks for 90 (logged).
- Then `--xr-refresh-rate 120`, or `"refreshRate": 120` in `<user data>/config/xr.json`; the run report's `xr` section
  gives the rate the session ran at (`refreshRate`, `refreshRates`), the missed refreshes, the profile and the dynamic
  scale's lowest.
- On Air Link the encoder and the network decide much of what reaches the eyes; the cable is the measure.

## Not verified without the headset

- That Meta's runtime switches to 120 Hz when asked (the call is XR_FB_display_refresh_rate's, made before the session
  begins); its compositor's and its encoder's share of the GPU at 120 Hz; ASW halving the rate when frames are late.
