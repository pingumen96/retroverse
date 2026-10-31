# I10 step 4: controllers

Roadmap row: I10 step 4. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §5 (amending ADR-0012's
Focus and Inspect in VR). Built on step 3's movement ([step-3.md](step-3.md)).

## What was built

- **The controllers as a gamepad** (`rv_xr_model`, `controllers.hpp`): each controller's inputs drive a gamepad's
  sticks, buttons and triggers (`xr::PadState`). `xr::VirtualPad` merges them into the frame's device input, with
  pressed and released edges of their own, so the action map's gamepad bindings (and a user's `bindings.json`) drive
  the VR mode exactly as a pad does; the physical pad keeps working beside them.
- **The pointing hand** (`xr::Pointer`): the hand whose trigger went down last points (the right one at first), the
  other one takes over when it loses tracking. Its aim pose's ray, composed with the tracking space, is the frame's
  pick ray in place of the reticle; its trigger is interact (with a 0.6 / 0.4 hysteresis).
- **`xr_bindings.json`** (`assets/config/xr_bindings.json`, `xr_bindings.hpp`): the bindings suggested for the Touch,
  Index, Vive, Windows Mixed Reality, Pico 4 (with `XR_BD_controller_interaction`, when the runtime has it) and simple
  controllers. The paths are checked against their actions' kinds; a user's file replaces a profile or adds one.
  The Touch layout:

  | Input | Gamepad | In the room |
  |---|---|---|
  | Left stick, its click | left stick, View | walk; the directory |
  | Right stick | right stick | snap turn; up aims the teleport |
  | Either trigger | (select) | interact with that hand's ray |
  | A, B | South, East | interact; back |
  | Y, X | North, West | the quick catalog ("Vedi media" in Inspect); "Informazioni" |
  | Left menu | Start | "Avvia" (play now) |
  | Grips | shoulders | the catalog's filters, the editor's turns |

- **The runtime** (`xr::Runtime::syncControllers`, `vibrate`):
  - OpenXR: one action set, `retroverse`, with the gamepad's actions, each hand's select, grip and aim poses and
    vibration; the profiles' suggested bindings; xrSyncActions while focused; the poses located at the display time;
    xrApplyHapticFeedback. The hands' interaction profiles follow XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED into
    the session's events.
  - The fake runtime: the input script's `leftHand` and `rightHand` steps (posed like `headPose`), its vibrations
    counted.
- **The room in VR** (`XrHands` and `XrProps` in `src/app/src/xr_movement.cpp`):
  - The right controller aims the teleport's arc from its own pose (step 3's arc came from the head);
    `comfort.moveRelativeTo: "hand"` makes the left stick walk along the left controller.
  - **Focus without moving** (`InteractionSettings::keepWalker`): in VR an interact on a spine-out item, and the
    D-pad's steps, pull the item out where it is; the walker stays. "Mostra nella stanza" still takes the user there.
  - **Inspect in the hand**: the inspected item is held 12 cm in front of the pointing controller's grip, its axes the
    hand's (the wrist turns it, the stick still turns it on top); with no controller, in front of the eyes as on the
    desktop.
  - **Vibration** when the ray comes onto another item (0.35 for 20 ms).
  - **The controllers drawn**: a generic handle with a glowing band (no vendor shape or mark), and the pointing hand's
    laser to the first collider or the floor (at most 4 m), cyan while it rests on an item.
- **The run report's `xr` section** adds the profiles the runtime took, each hand's profile at the end, the frames a
  controller was tracked and those whose pick came from a hand, the vibrations asked for.

## Proof

- **Unit tests** (`rv_tests`):
  - the virtual pad's edges and its sticks against the physical pad's; the pointing hand, its hysteresis, the hand
    taking over, the ray in the world; the laser's reach to a wall and to the floor;
  - the arc from the right controller and the walk along the left one;
  - the shipped bindings (every profile selects, points, holds, vibrates, goes back and has a menu; all but the simple
    controller walk and turn), the paths refused for their kinds, a user's profile replacing one;
  - the input script's hands; the session passing the controllers to a focused frame and the vibrations on;
  - Focus and its D-pad steps leaving the walker in place with `keepWalker`; the item held in a hand, turned by the
    wrist and the inspection's yaw, lifted along the straight line.
- **GPU smoke test** (`tests/smoke/test_xr_room.cpp`): the snes-room fixture's room in the fake headset, the right
  controller pointing at the band item fx:hw-w/s:5.4 from the hip. Interact inspects it and it is held 12 cm in front
  of that controller (the report's held centre within 1 cm); the controller vibrated; the head never moved.
- **By eye:** the screenshot of that run, both eyes: the controller, its laser and the cartridge in the hand.

## By hand, in the headset

```powershell
build/msvc-x64-release/bin/retroverse.exe --vr --data-dir build/rvdata
```

- Point at a cover with either controller (the laser turns cyan, the controller ticks), pull the trigger: the
  cartridge comes into that hand; turn the wrist to see its sides; B puts it back.
- A spine-out item: the trigger pulls it out where it is, without moving.
- Y opens the quick catalog, the left stick's click the directory (both still shown on the monitor only: step 5).

## Left for later steps

- The card beside the item, the catalog and the directory as panels in the headset, the ray on them: step 5.
- Haptics on presses and on the card's buttons: with the panels.
