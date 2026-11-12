# I12 step 8: life

ADR-0021 §6; roadmap I12 step 8. Until now nothing in a room moved but the clips and, since step 6, the dust. Now a
ceiling fan turns over the 90s collector's room and a lava lamp breathes on its crate, a wall clock in the study tells
the time, the media racks' activity LEDs blink, and a secondary TV shows a moment of static between two clips. All of
it stands still under `world.reduceMotion` and is the same at every run under `--fixed-dt`.

## What was built

- **Moving parts as scene data** (`scene/object_motion.hpp`): a scene object's `motion` (`spin`, `clock-hour`,
  `clock-minute`, `clock-second`; a pivot and an axis in its own frame; a spin's turns a second) turns its drawn items
  about the pivot each frame. A spin follows the run's simulated seconds, a clock's hands the time of day (clockwise,
  the second hand stepping on whole seconds). Only an object that neither collides nor casts a shadow, and is not a
  grid, may move (the parser refuses the rest). `SceneInstance` keeps each moving item's described model and moves it
  by `motionDelta()`; the pre-pass and every pass follow, nothing in the shaders changes.
- **The 90s room's ceiling fan** (a pack's `roomDressing.fan`, `console_90s_collector.json`): over the plan's room
  pendant, a canopy at the ceiling (a beam's underside over it), a rod, the motor with a glowing light kit, and four
  pitched oak blades 0.3 m under the lowest ceiling or beam they pass under, each spinning at 54 turns a minute. Left
  out in a room under 2.9 m.
- **The lava lamp** (a new prefab, `lava_lamp`: a base cone, a tinted bottle with a pool of wax and three blobs, a cap)
  stands on the 90s set's crate (`furniture.heroDressing`, a prop); the magazines moved to the floor beside it. Its wax
  (`MAT_Lava_Wax`) breathes: one slow swell every 12.5 s.
- **The study's wall clock** (`roomDressing.clock`, `computer_studio.json`): over the featured wall shelf, in the
  picture band (the gallery leaves that shelf's frame out): a walnut case, an ivory dial, twelve marks, three hands
  turning about the dial's centre, a hub. Its time is the local time, `--clock HH:MM[:SS]` when given, and 10:10 in a
  frame-limited run (going on with the run's time), so a golden with a clock is the same at every run.
- **Blinking and breathing** (`gl::FlickerMode`: step 6's neon dips gain `Blink` and `Pulse`, as flags
  `RV_MATERIAL_EMISSIVE_BLINK` and `_PULSE` beside the flicker's, the CPU mirror `emissiveFlickerFactor` formula for
  formula): every pack's media rack names `MAT_Led_Activity_Red` or `_Green`, whose LEDs blink in eighths of a second,
  on in about 60 % of them; the speakers' power LEDs stay steady. As the neon's, nothing varies in a run's first 4 s.
- **Static between clips** (`ScreenPolicy::showsStatic()`, `ScreenPlaySettings::staticBetweenClips`, a secondary TV's
  only): when the playlist goes from a clip that showed to the next game, the screen shows snow
  (`RV_MATERIAL_CRT_STATIC`: the picture's 320 x 240 cells hashed with the frame's 24th of a second, streaked by row,
  through the tube) for at least 0.3 s, then until the next clip's first frame, at most 1.5 s. Never on the hero, never
  for a screen's first game or a selection, never while a launch holds the screen; under reduceMotion the snow stands
  still.
- **`RoomLife`** (`app/src/room_life.cpp`, its rules GPU-free in `app/room_life.hpp`): each frame turns the scene's
  moving parts (under reduceMotion the fans stand still and the second hands at twelve, while the hours and minutes
  keep the time) and sets the living materials' blink and breath (steady under reduceMotion). The run report's
  catalog: `spinningParts`, `clockHands`, `blinkingMaterials`, `breathingMaterials`, `partMoveFrames`, `clockTime`; its
  video: `staticShows`, `staticFrames`.

## Departures from the plan

- **No `kSpinY` instance flag.** The parts turn on the CPU (a few items per room, one matrix product each), so the
  shadow map and the pre-pass follow without a shader change, and the same mechanism carries the fan and the clock's
  hands (the plan had a shader flag for one and CPU transforms for the other).
- **The fan and the clock are dressing primitives, the lava lamp is the only new prefab** (step 1's way for the
  shell's details): the plan's `ceiling_fan`, `wall_clock` and `power_strip` prefabs and their manifests were not
  needed. The power strip was left out: behind the TV cabinet it is never seen; the racks' LEDs carry the blinking.
  The consoles get no LED: the procedural consoles have no light part and the content models are the user's.
- **Where they stand.** The lava lamp is on the 90s set's crate, not on the secondary TV's sideboard (the TV fills its
  top). The clock hangs over the featured wall shelf, the study's only free wall in the picture band (the desk stands
  in the hero corner).
- **The clock keeps the time under reduceMotion** (only its second hand stands still): a clock frozen at a wrong time
  reads as broken, and its hours and minutes never visibly move.
- **The static is a rule of the policy over the picture, not a `ScreenPicture` value**: the stills, the held frame
  and the report's picture timeline stay as they were; the static only covers them.
- **Wax that breathes, not blobs that rise**: the blobs stand inside the bottle; their glow swells and fades.

## Measured

**Tests:** the unit suite 2686/2686; the GPU suite all but the four known cases (the two `process_runner` cases that
need their own executables in `RV_LAUNCH_ALLOWLIST`, the two `gpu.xr` launch cases that match no test in this build);
the GPU cases `world look: the rooms' life` (the fan's frames 30 and 31 differ and are the same picture under
reduceMotion, the study's three hands at 10:10:00) and `gpu.secondary_tv: between two clips the secondary shows static`
(at least once, at least 0.3 s each, never the hero).

**The goldens:** G12 regenerated on purpose (the 90s room: the fan over the aisle, the lava lamp on the crate); the
other screenshot goldens unchanged (no golden shows the study, the static never shows in one). The scene goldens
`plan-57` and `gen4` changed only in the media rack's LED material (`MAT_Led_Activity_Green`), with their digests and
floors file.

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 2.78 ms | 7.22 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 3.01 ms | 3.83 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 6.63 ms | 7.46 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.29 ms | 6.71 ms | informative |

The desktop p99 rose from step 7's 3.15 ms to 7.22 ms while its p95 stayed at 3.15 ms: a tail of slow frames (the
run's max is 234.6 ms), not a steady cost (the life's CPU work is a few matrix products a frame); it stays far inside
the acceptable 33.3 ms, and the gate's run measures it again.

## Follow-ups (BACKLOG)

- The lava lamp's blobs rising and falling (a part's slow travel along the bottle, as the motions turn).
- An LED per console model (a content manifest's light), and the arcade cabinets' coin-door lights.
