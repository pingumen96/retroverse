# I12 step 6: light and atmosphere

ADR-0021 §4 (as amended by this step); roadmap I12 step 6. The rooms' light stood still: no air in the beams, a neon
that never wavered, a CRT whose glow kept one colour whatever it showed. Now dust drifts in the key light's cone and
under the pendant, the lounge and the arcade show their spots' cones in the air, the hero sign dips for a moment now
and then, and the CRT's spill light takes the colour and brightness of the clip it plays.

## What was built

- **The rooms' air as data.** A pack's `atmosphere` block (`dust`, `lightCones`, `coneStrength`, `neonFlicker`,
  `crtBreathing`; every key optional) becomes the profile's `RoomAtmosphere` through `themedLighting()`, and the scene
  builders write it into the scene: a room's scene into its `environment`, a floor's into each room's area (the hall
  and the scene itself have none). Every pack has dust; the optical-era lounge and the arcade add the light cones; the
  hub's showcase room keeps a clear air (`dust` 0, no flicker, no breathing). The test room declares none, so G01 and
  G21 draw nothing new. The scene goldens and digests move by that block alone.
- **The emitters, GPU-free** (`scene/atmosphere.hpp`, `deriveAtmosphere()`): for every light whose area's atmosphere
  (else the scene's) is set, a dust cone under the spot named `*key-light` (320 motes at dust 1, from the light to 2 cm
  above its area's floor), a dust pool under `fx:room.pendant` (160 motes in a 0.9 m cylinder down to 0.8 m above the
  floor), and with `lightCones` a light cone under the key light, the washers and the display cases' and wall shelf's
  spots (ending 5 cm above the floor). Each mote's place is a function of its index, its emitter's seed and the time
  (`motePosition()`, mirrored formula for formula by the shader): it drifts down 1.5 to 4 mm a second, wraps with a
  fade, and wobbles within 3 cm. `SceneInstance` keeps the scene's emitters and hands the views those of the eye's
  area, each with its light as it stands now.
- **The atmosphere pass** (`render/atmosphere_pass.hpp`, uniform block 6, `atmosphere_settings.hpp`): after the lit
  pass's blended batches and before the post-processing, additive into the radiance alone (no emission: dust never
  blooms of itself, no alpha), depth-tested against the pre-pass without writes, from an empty vertex array: the cones
  as one instanced draw of 24 x 8 quads, the motes as one instanced strip of four vertices. A mote is a soft speck of
  2.5 mm at least 2 px wide (dimmer by the area it gained, so it never flickers between pixels), lit by its own light
  (its falloff and cone) and scattering forwards (Henyey-Greenstein, g 0.6): it glows between the eye and its lamp
  and barely shows with the lamp behind the eye. A cone scatters what the air there receives over the chord the eye
  looks through, `radiance x tan(half angle) / (2 pi d)` times its strength, thickest where the eye looks through most
  of it, faded in from the apex and out towards its end, never above 0.15. The emitters outside the view are culled
  by their bounds; at most 16 per view. Its GPU time is a pass of its own (`atmosphere` in the run report's passes,
  the frame timing, the overlay's stats, VR's render scale).
- **The neon's flicker** (`kEmissiveFlicker`, `MaterialUniforms`' former padding: seed, rate, depth): the unlit shader
  dims the emission by `materialFlicker(uTime)`, one stutter of 80 to 120 ms in every slot of 8 s, at a place the
  seed and the slot choose, never before 4 s (so a frame-limited run's screenshots never catch one); its CPU mirror
  `gl::emissiveFlickerFactor` is what the unit tests check. `RoomAtmosphere` sets it on the hero signs' pictures of
  the rooms that ask (a seed per room), steady (rate 0) under reduceMotion.
- **The CRT's glow** (`app/crt_glow.hpp`, `RoomAtmosphere`): the screen service keeps the mean linear colour of the
  last frame it uploaded for each slot (`media::averageLinearColour`, a 16 x 12 grid), and the hero's room's
  `accent:crt_glow` light takes the picture's hue (65 %) and follows its luminance (0.35 to 1.6 times its own
  intensity), eased with a 0.6 s time constant, at once on a clip's first frame (a frozen clip's view is constant),
  written at most ten times a second through `SceneInstance::repackLights()` (a repack that leaves the area's
  selection and environment ease alone). Back to its own colour when the clip stops, the hero moves to another room or
  reduceMotion is on.
- **reduceMotion and VR.** Under reduceMotion the motes stand still, the signs are steady and the glow keeps its own
  colour; the cones stay. VR's fast profile halves the motes and drops the cones. The frame's time reaches the shaders
  wrapped hourly.
- **The report.** `lastView.atmosphere` (motes, cones, frozen) and its GPU time; the catalog record's
  `flickeringSigns`, `crtGlows`, `crtGlowWrites`.

## Decisions and deviations

- The average colour is taken on the main thread when the frame is uploaded (192 samples of the RGBA frame the
  decoder made), not on the decoder thread as ADR-0021 §4 planned: the cost is a few microseconds, and it keeps the
  media module untouched but for one free function.
- The light cones end above the floor instead of fading against the scene's depth: the depth they would read is the
  target they are drawn into. Where a cone meets a wall or a piece of furniture its edge is cut by the depth test; at
  their strength it reads as a soft shaft.
- A still (a cover on the CRT, the standby) does not move the glow: only a clip's frames do. The neon tubes of the
  arcade's lamp and the signs' own lights do not dip, only the hero signs' lettering (and its bloom).

## Measured

**Tests:** the unit suite 2667 of 2667; the GPU suite 2940 with the known failures only (the two process_runner cases under the launch allowlist, the two xr cases a filter matches nothing for, and the pillarboxed capture's crop, which passed alone); `world: the stored room shows at the first frame ...` now runs under reduceMotion, since its two pictures may differ on the CRT alone and the motes drift between them; the GPU case `world look: the room's air` (the snes-room's 480 motes, its sign flickering and
its glow, the motes still under reduceMotion, the lounge's cones).

**The goldens:** G02, G15, G17 and G20 regenerated (the motes in the key light and the pendant's pool, the lounge's and the arcade's cones, the glow following the frozen clip); the other sixteen compare within the tolerance, G01 and G21 exactly (the test room has no atmosphere).

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 2.80 ms | 3.34 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 2.88 ms | 3.56 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 7.08 ms | 8.09 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.49 ms | 5.31 ms | informative |

The atmosphere pass's own GPU time on the benchmark path: p50 0.001 ms, p99 0.002 ms (the same per eye in VR).

## Follow-ups (BACKLOG)

- Light cones that fade against the scene's depth (a copy of the pre-pass depth, or a depth-only target).
- A still's colour for the glow, and the sign's lights dipping with its lettering.
