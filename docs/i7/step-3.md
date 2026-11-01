# I7 step 3: controls

Roadmap row: I7 step 3. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §7, §12, §15. Plan: [plan.md](plan.md).

## What was built

- **The hints** (`input::KeyLabels`, `input::bindingLabel()`, `ui::HintBar`, `ui::HintBarView`, `assets/ui/key_labels.json`):
  every document's hints come from the active map and the last device. A document declares a hint in one line and links
  `hint_bar.rcss` after its own sheet; `wizard.rml` needs nothing else, and no screen does anything:

  ```html
  <span class="hint"><span class="pad" hint-action="ui_activate"></span>Avvia</span>
  <span class="hint"><span class="pad" hint-action="ui_filter_previous ui_filter_next"></span>Gruppo</span>
  <span class="hint-prose">(<span hint-action="quick_catalog"></span>)</span> <!-- inside a sentence -->
  ```

  `HintBarView` finds every `hint-action` of the view's documents, those loaded later too, and gives it the name, a face
  button's colour (which so follows the button), or `hint-wide`. A hint with nothing bound on the device hides. The
  elevator's prompt and the world notices' "(Tab)" and "Premi Invio" read the same table.
- **Remapping** (`app::binding_rules.hpp`, the Controlli group): 21 actions in "Nel mondo" and "Nei menu". A capture reads
  the raw input (`app::CapturePress`), a clash is judged per context (`actionsCompete()`, over `readsAction()`) and offers
  a swap; a key or pad button goes to its device's slot alone. In `settings.json`'s `bindings` the whole list of a changed
  action, removed again when it equals the layer below. The frame's actions are cleared while a capture waits, so nothing
  else sees the press, and `AppInput::setBindings()` resolves the held input through the new map at once: without that
  the very key a capture took would press the row it was captured for.
- **A swap** involves every holder in the context, and is refused when the action has nothing to give back and a holder
  would lose its last key or pad button (`Locked`), or when what goes back is another action's where the holder is read
  (`Reserved`): it never strips a device, nor moves the clash.
- **No lock-out** (`keepUnlocked()`, in the loader and after every edit): UiActivate, UiBack and Menu get the shipped key or
  button back whatever a file says. The arrows and the D-pad are not bindable, so the screen can always be walked.
  `--safe-bindings` leaves the section out, and Controlli then shows only "Ripristina predefiniti".
- **The hotplug** (`app::ControllerWatch`, `ui::ControllerNoticeScreen`): "Controller scollegato" when the last pad goes,
  until one is back or 8 s pass; the last pad gone, the keyboard is the device used last, so no hint names it.
  `rv_platform_tests` proves a virtual pad unplugged and back during a UI screen and across a game (FR-029), and a smoke
  case the notice on a game's return, the pad going through the script's `"raw"`, not an SDL event.
- **Input scripts** gained `"raw"` (the devices, through the bindings) and `"device"`. The pad's hints are thereby tested
  for the first time; step 1's scripted actions never set the last device.

## Decisions and departures

- **`default_bindings.json` changed**, though the plan's table says it stays. Step 1's gap could only close with a binding,
  and ADR-0018 §7 asks a key of Menu, which had none: F2, F3 (filters and group), F4 (Informazioni, Ripristina), F6 (Vedi
  media), F10 (Menu). Function keys, because letters belong to the text fields.
- **The notice is a document of its own,** not a state of `world_notice.rml`, which hides under every panel: a pad lost in
  a menu is when it matters most.
- **A UI action takes no text key** (`isTextKey()`): a letter bound to UiActivate would type and press at once.
- **Touch controllers follow the pad's map.** ADR-0018 §12 says they keep `xr_bindings.json`; they do, but what comes out
  of it is a virtual pad through the same `ActionMap`, so remapping the pad in a headset moves them too. Separating them
  means resolving them before the section; parked in the BACKLOG, since no hint would then be right in a headset either.
- **`menu` and `confirm` share Start, `quick_catalog` and `ui_media` share Y on purpose,** in one context: the shipped
  sharings count as no clash (`actionsCompete()`).
- **G07's script** sets the pad as the last device at frame 60: the card's hints were always the pad's letters there, and
  on the keyboard they would now read F6, F4, Esc. Its picture holds them, so it stays 0 pixels apart.
- **`ui::SettingsModel::setDevice`, the preview's and the edit bar's `device`** went: the hints no longer need them.

## What was checked

The figures of the runs are in the step's report; the cases are `tests/unit/input/test_binding_labels.cpp`,
`tests/unit/app/test_binding_rules.cpp`, `tests/unit/ui/test_hint_bar.cpp`, the capture cases of
`tests/unit/ui/test_settings_model.cpp` and `tests/unit/input/test_input_script.cpp`, `tests/platform/test_gamepad_manager.cpp`
and `tests/smoke/test_controls.cpp` (`[controls]`). Pictures: `rv_smoke_tests "[controls_shots]"` with `RV_SHOTS_DIR`.
