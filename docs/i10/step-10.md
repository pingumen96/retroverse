# I10 step 10: moving while the game plays

Asked by the user on October 30, 2026, after the first session in the headset (Link, a SNES game in RetroArch on the
CRT): the user should walk up to the screen while the game runs. The same session found two faults of step 6, fixed
here. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §7. Built on step 9 ([step-9.md](step-9.md)).

## What was built

- **The controllers walk while a game runs** (`retroverse_app.cpp`, `AppInput::controllersAlone`): the pad and the
  keyboard still play the game and Retroverse still reads none of them, but each VR frame's controllers alone go
  through the same bindings (an input system of their own, its held state apart from the devices'), and their walk,
  turns, sprint and teleport aim move the walker as in free roam (`walkOf`). Their buttons and triggers press nothing
  while the game runs, so no panel opens and no item is taken. The emulator never sees the controllers: OpenXR gives
  their input to the headset's application alone. A controller put down sleeps (the headset switches to hand tracking);
  picking it up wakes it. At the game's end the walker stays where the user walked (`WorldInteraction::finishReturn`
  with `keepWalker`), not where Avvia was pressed as on the desktop.
- **The inspected item set aside** (`WorldHover::setHeldAside`): Avvia is pressed on an inspected item, which in VR
  stays in the pointing hand; with the controllers put down for the pad it fell back in front of the eyes, over the
  CRT. While the game runs it stands at its pulled-out pose in its fixture and the view is not dimmed; the inspection
  comes back with the game's end, as before.
- **The game's window takes the foreground** (`platform_win::giveForeground`): on the desktop Retroverse's window
  steps back and the emulator, allowed by `AllowSetForegroundWindow`, comes in front; in VR Retroverse renders on, and
  the session's RetroArch did not read the pad. Once the emulator's window shows, it is given the foreground (this
  thread's input attached to the foreground's for the call, as the window hand-over takes Retroverse's back), never in
  hidden or background runs; every change of the foreground while the game runs is logged ("VR: the foreground is
  ...").
- **Input scripts** (`leftStick`, `rightStick`: [x, y] in [-1, 1]) push the fake runtime's thumbsticks, and the run
  report's `xr.playingWalkFrames` counts the frames a game ran in which the controllers walked, turned or aimed.

## Proof

- **Unit tests**: the scripted thumbsticks (values, ranges, errors); the report's new field (round trip).
- **GPU smoke test** (`tests/smoke/test_xr_room.cpp`, the launch case): while the fake emulator's game runs, the left
  controller's stick pulled back walks the head more than 20 cm back from the shelf (`playingWalkFrames` above 0), and
  the return keeps it there; the capture as before.
- **Release**: the 2,239 unit tests (the return's fix after them: the interaction's, the launch's, the script's and the
  report's again); the GPU golden, `[xr]` and launch cases, 44 of them.

## By hand, in the headset

```powershell
build/msvc-x64-release/bin/retroverse.exe --vr --data-dir build/vr-trial --start-room room:snes
```

- Inspect a cartridge, Avvia: the cartridge goes back to its shelf, the game shows on the CRT, the Xbox pad plays it.
- Pick up a Touch controller: its left stick walks up to the CRT, the right one turns and aims the teleport.
- The log's "VR: the game's window takes the foreground" and "VR: the foreground is the game's window" lines.

## Left for later

- Whether the foreground was the whole cause of the pad's silence is the next session's (the log's foreground lines
  say who held it).
