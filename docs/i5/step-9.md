# I5 step 9: the insertion animation

Roadmap row: I5 step 9. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §11 (the insertion), with §2
(the decoders' stop before a launch, at most 1 s) and ADR-0012 §6 (the launch from the world and its return). Amended
on 2026-10-31 by this step and its review (§11: see "Decisions").

## What was built

- **The state.** `WorldState::AnimatingToConsole` sits between Inspect and Launching (specification §8.3's name).
  Avvia in a room whose hero corner has a console starts it:
  - the held item eases (450 ms, in and out) from the hand into the console's media socket, shrinking to fit it;
  - the eye leads it: it turns to the console in 300 ms, eased out, the feet kept, so the item (held in front of the
    camera) drops into a console already framed;
  - the item is seen seated for 100 ms, then the launch's hand-over begins: 550 ms from Avvia (style §16.2: at most
    650 ms).
- **The framing** (`app::consoleViewPose()`). When the hero CRT's screen faces the eye, the eye aims between the socket
  and the screen, so the game's title card shows above the console; otherwise the socket stands on the lower third's
  line. The eye never looks down more than 30 degrees.
- **The skip.** A deliberate action button skips it (`insertionSkipPressed()`: interact, ui_activate, confirm, menu,
  back, ui_back): the item seats at once, the eye stops where its turn had taken it, the launch follows within the
  frame. Walking, sprinting, looking, the arrows and the D-pad never skip.
- **No insertion** in the arcade, a shared room or a hand-made scene (no console), under `reduceMotion`, or with an
  insertion time of 0: Avvia launches at once, as before.
- **The launch meanwhile.** Avvia's request goes to the catalog worker at once; a ready decision waits for the
  insertion's end (`ProductUi::releaseWorldLaunch()`), a refusal comes back at once (Inspect, the item in the hand, the
  walker as Avvia found it). The screens' slots stop at Avvia, their threads ending during the insertion; the hand-over
  waits only for what remains of the 1 s bound. The hero shows the game's title card meanwhile, undone after the return
  or a refusal.
- **The room holds still** (`WorldSession::launchHolds()`). From Avvia to the return to Inspect a live update's, a
  regeneration's or a retargeted building's result waits, and the editor does not open, so the room the item came from
  is there when it comes back.
- **The return**: Inspect of the same item, in the hand, the walker as Avvia found it; its slot in the plan never moved.
- **The report**: `world.interaction.insertion` (counts, the last one's frames, `seatedFrame` among them, the socket,
  the item's centre, scale and distance per frame); `world.launch` events `inserted` and `insertion-skipped`;
  `video.launches` `stopFrame`, `waitMs`, `handOverFrame`, `cancelled`.

## Decisions

Recorded in ADR-0016 §11's amendment of 2026-10-31:
- **The socket** is the `console_cartridge` prefab's `CartridgeSlot`; no prefab changed. Disc games go into the same
  slot, shrunk to fit: every console room holds the cartridge console.
- **The camera leads; the item follows**, with a short hold on the seated item before "Gioco in corso" dims the view.
- **"Any button" means a deliberate action button.** Keys held from walking or looking (the arrows are bound to both
  walking and the UI's steps) must not end the animation by accident.
- **A skip does not snap the eye**: the return restores Avvia's pose anyway.
- **"Instant under `reduceMotion`" is no frame of it**: snapping the view to the console would be the jump
  `reduceMotion` users asked not to see.
- **The sound fades with the stop, not over the insertion**: the slots stop at Avvia (that is what hides the stop's
  wait), and a stopped slot's sound fades in the mixer's 30 ms pause fade whatever the master fade's length; fading over
  450 ms would mean running the audible decoder into the insertion. Kept as a follow-up.
- **The resolution runs during it**: a refusal comes back at once instead of after the animation.

## Proofs

- Unit (`rv_tests`, `[insertion]`): the transition table on the hand-built shelves (the eye on the console after 18
  frames, the item seated after 27, the hand-over released 6 frames later; the skip by each action button and by no
  walking key, the eye stopped where it was; `reduceMotion`; a refusal during it; no console); a live update delivered
  during the insertion applied only after the return to Inspect; the golden building's sockets and hero screens; the
  framing (the lower third, the title card when the screen faces the eye, the 30 degree clamp); the shrink; the forced
  title card undone (`[screen]`); `PoseAnimator::setEasing()` (`[pose_animator]`); the report's records read back.
- GPU (`test_world_launch.cpp`, the `esde/snes-room` fixture, the fake emulator under `RV_LAUNCH_ALLOWLIST`, hidden,
  lockstep): Avvia on Amber Lantern at 80, seated at 106, released at 112 (0.55 s of frame time), the launch's request
  on 112 and its start on 113, Returned; the recorded socket equal to the shown scene's console socket and inside the
  console's bounds; the hero's decoder stopped at 80 with its thread ended before the hand-over; the title card from
  frame 81; Inspect restored with the walker as Avvia found it. The skip at 86: seated and released on 86, the start on
  87. The card's script (`world_card.json`) and the two-screen case moved past the hold; the arcade launches at once.

## Results

- Debug build without a warning; Debug unit tests (`./scripts/test.ps1`): 2250 of 2250 passed. The Debug GPU cases of
  the launches, the card and the arcade's launch (`-Filter "world launch|world card|returns to the arcade"`): 7 of 7
  passed under `scripts/focus_watch.py` (0 foreground changes, 0 windows shown).
- Release build without a warning; Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`, under
  `scripts/focus_watch.py`): 2427 tests (2250 unit, 177 GPU), all passed but the one skipped as always (the overlay's
  pick, no overlay in Release); G01 to G20 compared and passed, none skipped; 0 foreground changes, no window visible.
- No golden was rewritten: no golden scene launches a game.

## Screenshots

Hidden, frame-limited Debug runs of the GPU cases (`build/i5/step9/`, 1920 x 1080, F12 at the given frames), looked at
against `style.md` and `example.png`:
- `insertion_start_f080.png`: Avvia's frame: Amber Lantern in the hand, the eye just starting its turn.
- `insertion_middle_f093.png`: the eye on the hero corner (the CRT's screen faces it, so the title card "Amber
  Lantern" shows), the cartridge coming down beside the console.
- `insertion_socket_f106.png`: the cartridge standing in the console's slot, the title card above it, no panel yet (the
  hold).
- `skipped_f086.png`: the skip's frame: the item in the slot, the eye where its turn had reached.
- `returned_inspect_f119.png`: after the return, the item in the hand again with its card on Avvia.

The look holds: warm wood and plastics, the console and the CRT framed together, the room behind them.

## Open issues

- The user's trial: the insertion's feel (the eye leading, the hold, the skip's buttons) with a pad and a keyboard,
  and a real launch afterwards; no automated run starts a real emulator.
- From a pose where the CRT turns its back to the eye the title card is not in view (the lower third's framing); the
  report's `video.shown` proves it shows.
- Follow-ups in BACKLOG ("Follow-ups from I5"): a disc console with its own tray; the sound over the insertion.
- The real library was not run: the step touches no catalog, plan or media path.
