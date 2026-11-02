# I7 step 5: the on-screen keyboard

Roadmap row: I7 step 5. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §8, §12, §15. Plan: [plan.md](plan.md).

## What was built

- **The keyboard** (`ui::KeyboardModel`, `ui::KeyboardScreen`, `assets/ui/keyboard.rml`): a grid of ten columns, digits,
  letters, the Italian accented letters, some punctuation titles hold, then Spazio, Cancella and Conferma. One table,
  `keyboardKeys()`, gives what a key shows and what it types; the words of the wide keys are the document's. The D-pad or
  stick moves and wraps, A types, X deletes, B closes, Start confirms. The panel stands where the card stands, so the field
  and the list that narrows stay in view; the card and the catalog's hints hide under it (`body.typing`).
- **Typing** goes through what `ProductUi::typeText` uses (`Context::ProcessTextInput`) into the search field, which keeps
  RmlUi's focus; the selected key is a class, not the focus. Delete is RmlUi's Backspace (`CaretKey::Backspace`), so the
  field's own text stays the only copy. Confirm is Enter in the field (`linebreak`): on to the results.
- **The way in:** Activate on the field with the pad as the device used last; in a headset the ray's click on the field.
- **Back** stays the application's: `catalogBack()` closes the keyboard first, so `BackAction` did not change.
- **The report's** `keyboard`.

## Decisions and departures

- **`Action::Confirm` is read in the UI context now** (`readsAction()`), as `UiCommand::Confirm`, since ADR-0018 §8 gives
  Start to the keyboard. So remapping judges it there: Start for Informazioni is a clash, and Confirm takes no text key (the
  capture rule of `isTextKey()` extended to it). Enter stays shared with UiActivate on purpose (`actionsCompete()`). No other
  screen sees the command: `ProductUi::update` drops it unless the keyboard shows.
- **One press that is both Activate and Confirm** (Enter) is read by the device used last: on the keyboard it confirms, on a
  pad that was given both it types, or that pad could type nothing. Conferma's key is still there.
- **B keeps the text.** The search narrows as it is typed, so nothing is left to cancel; ADR-0018 says only "closes".
- **Beyond ADR-0018:** the ray opens it, which §8 implies but does not say: no keyboard is at hand in a headset. A click beside
  the panel closes it (a transparent element under it takes the click), so a ray never reaches the list underneath, and a
  headset has a way to close it without confirming. Some punctuation (`' - : . , ! ? &`) was added: titles hold it.
- **Lower case only:** the search folds case, and a Shift key would double the grid's states for nothing.
- **The search field is the product's only text field** (`assets/ui`, one `<input>`), as ADR-0018 §8 says; the wizard browses
  paths. The screen hooks are the quick catalog's, so a second field would need its own.
- **X has no repeat:** a press deletes one character, as the commands of `NavInput` go.

## What was checked

The figures of the runs are in the step's report; the cases are `tests/unit/ui/test_keyboard_model.cpp`, the Confirm case of
`tests/unit/app/test_binding_rules.cpp` and `tests/smoke/test_keyboard.cpp` (`[keyboard]`; `RV_SHOTS_DIR` with
`[keyboard_shots]` writes the pictures). M1's manual session can now run its search step on the pad; the session itself is
the user's, at Gate Pad.
