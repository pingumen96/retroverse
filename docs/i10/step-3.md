# I10 step 3: movement and comfort

Roadmap row: I10 step 3. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §5, §8. Built on step 2's
session and tracked head ([step-2.md](step-2.md)).

## What was built

- **`xr::Locomotion`** (`rv_xr_model`, `locomotion.hpp`): the walker's movement in VR, GPU-free.
  - The tracking space is anchored in the world (an origin on the floor, a turn, a floor offset). The walker's
    capsule follows the head over the floor through `CollisionWorld::moveCapsule`.
  - **The stick** walks along the head's heading at the walker's speeds (sprint included), carrying the anchor.
  - **Turns** turn the anchor about the head, which stays where it is: a snap of 30 degrees each time the right stick
    passes 0.75 (again once it went back under 0.35), or smooth at 90 degrees per second; the mouse turns in both.
  - **The head leaning beyond the capsule** (over a table, into a wall) fades the view from 12 cm, black at 30 cm, and
    further than that is pushed back: the anchor moves so that the head stays 30 cm beyond the capsule.
  - **The teleport**: the right stick up past 0.7 aims a ballistic arc (8 m/s) from 35 cm below the head along its
    gaze; it lands on the walker's floor where the capsule fits, and stops on the first collider standing above the
    floor (walls, furniture). Let go below 0.35 over a landing, the head lands above it, the view blinking (black, back
    in 0.15 s).
  - **The comfort vignette** closes in while the stick walks or turns smoothly (0.2 s to close or open), as far as
    `comfort.vignette` says; off by default.
  - **Placements and recentering**: a walker moved by something else (the start, a ride's destination, the world's
    ease, the overlay's "go to") puts the head above its feet facing its yaw. The runtime's recentering (a
    ReferenceSpaceChangePending event, passed on with the next located frame as `Frame::spaceChanged`) keeps the head
    where it was in the world.
  - **The posture**: standing takes the tracked height; seated makes it up to the walker's eye height (1.65 m) at the
    start and at each recentering.
- **`xr.json`** (`assets/config/xr.json`, `xr_settings.hpp`): `display.mode` ("vr" starts every run in VR, as `--vr`
  does), `renderScale` (`--xr-render-scale` overrides it), `refreshRate` (0: the runtime's own), `posture`,
  `comfort.turn`, `snapDegrees`, `smoothDegreesPerSecond`, `vignette`. The user's `<user data>/config/xr.json` overrides
  it key by key, and a bad one is left out whole, as video.json is.
- **The app** (`XrMovement`, `XrProps` in `src/app/src/xr_movement.cpp`):
  - In a located VR frame the walker's actions of the frame (the same filtered actions the desktop walker gets: none
    during an ease or a ride) go to the movement, and the walker is written back: its feet the capsule's, its yaw and
    pitch the head's. The world's interaction, the rides and the report see the walker where the user stands.
  - The eyes and the head's camera come from the movement's anchor.
  - The movement's fade joins the ride's; the comfort vignette is the post pass's `tunnel`, centred where each eye
    looks straight ahead (an off-axis eye's centre is not its image's).
  - The arc is drawn as glowing beads, cyan with a ring on the floor where it lands, amber where it cannot.
  - Every transition of the world is instant in VR (reduceMotion forced, ADR-0017 §5).
- **The run report's `xr` section** adds the posture, the turn mode, the snap turns, the teleports, the frames pushed
  back and the farthest lean.

## Proof

- **Unit tests** (`rv_tests`, `[xr]`):
  - the placement, the snap and smooth turns about the head, the stick along the head's heading into a wall;
  - a lean into a wall: no fade within the capsule's reach, fading at 15 cm, pushed back past 30 cm, back in view;
  - the seated posture and a recentering that keeps the head in place, LOCAL's floor offset;
  - the arc: its landing where the parabola meets the floor, ending on a wall, on a table's side, no room beside a
    chest; aiming, the jump, the blink; a blocked arc never jumps;
  - the comfort vignette; bad input;
  - xr.json's defaults, the user's file merged, a bad one left out;
  - the session passing a recentering to the next located frame once;
  - the post block's new members (`tunnel`, `tunnelCenter`) against the shader's.
- **GPU smoke tests** (`tests/smoke/test_xr.cpp`):
  - **A scripted walk** (`tests/input_scripts/xr_walk.json`, the test room from `--spawn 0,0,3.2,0,0`): the head turned
    west, the stick walks it 1.25 m west; one snap turn of 30 degrees leaves the head in place; the teleport aimed 30
    degrees down jumps about 1.73 m along the heading. Every frame submitted with both eyes.
  - **xr.json seated** (in the case's data directory): a head tracked 1.1 m up stands at 1.65 m, the smooth turn and
    the vignette on.
  - Step 2's cases unchanged, G21 identical.
- **By eye:** the arc while aiming (frame 56 of the walk): beads down to the ring, in both eyes.

```powershell
build/msvc-x64-debug/bin/retroverse.exe --vr --xr-runtime fake --scene m0_testroom --spawn 0,0,3.2,0,0 --hidden `
  --exit-after-frames --frames 56 --fixed-dt 0.016667 --input-script tests/input_scripts/xr_walk.json `
  --screenshot build/screenshots/i10/xr_arc.png --data-dir build/rvdata
```

## By hand, in the headset

```powershell
build/msvc-x64-release/bin/retroverse.exe --vr --data-dir build/rvdata
```

- **Walk:** the left stick along where the head looks; the right stick left and right turns by 30 degrees; up aims the
  arc, let go to land on the ring.
- **Lean** into a wall or over a shelf: the view fades, and does not pass through.
- **The building:** the elevator (E, or the pad's A, facing it), the directory (the pad's View/Back), a teleport from
  it: the head lands at the room's doorway.
- **Seated:** `{"posture": "seated"}` in `build/rvdata/config/xr.json`, then hold the headset's menu button to
  recenter.

## Left for later steps

- The movement relative to a hand (ADR-0017 §5's option) and the arc from the hand: step 4, with the controllers.
- Focus still moves the walker (instantly): step 4 keeps it in place.
- A keyboard or pad recentering of Retroverse's own: the headset's recentering covers it.
