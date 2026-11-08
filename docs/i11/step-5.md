# I11 step 5: the history plaque

ADR-0019 §6; roadmap I11 step 5. A console room now has a brass plaque on the wall beside its hero corner, when its
system's identity has a description. The plaque tells the system's story: its logo, its maker, its year, its type and
its description. Pointed at, it shows its name. Interact opens its card with the text in full, on the desktop and in
the headset.

## What was built

- **The identity's text on the main thread:**
  - `CatalogSystem` carries `maker`, `year`, `type` and `description`, from `catalog::SystemIdentity`'s
    `systemManufacturer`, `systemReleaseYear`, `systemHardwareType` and `systemDescription`. The description is in
    Italian when the theme has it.
  - `SignSystem` hands them on.
- **The plate** (`assets::makeHistoryPlaque()`, GPU-free, deterministic):
  - **The brass:** #B08C52, lighter at the top, with a fixed brushing and an engraved border.
  - **The title:** the logo's silhouette, engraved and centred. A system without a logo gets its name in bold.
  - **The details:** "maker · year · type", the fields that exist.
  - **The description:** under a rule, in Atkinson Hyperlegible, wrapped at the spaces (`assets::wrapText()`). It is cut
    to the plate's height, with an ellipsis.
  - **Size:** 768 x 548 pixels for a plate 42 x 30 cm.
- **Where it stands** (`app::plaqueSites()`, `plaqueObject()`, GPU-free):
  - **Placement:** from the room's hero sign (`<room>/fx:hero.sign`), 0.955 m to its right and 1.15 m under it, on the
    same wall. The plaque's centre is at 1.35 m, between the TV cabinet and the featured shelf, inside the hero corner's
    own width.
  - **The object:** a poster frame, `plaque:<room>`, 42 x 30 cm, framed in `MAT_Metal_Brass`, without a shadow or a
    collision. Its picture is a paper print with a generic pattern, so that it has a material of its own.
  - Shared and arcade rooms have no plaque.
- **In the scene** (`WorldPlaques`, at runtime):
  - **Placement:** the plaques of the scene shown are added through `SceneInstance::replaceObjects()` under the prefix
    `plaque:`, which no room's rebuild touches. They are found again when the scene changes or the catalog's systems do.
    While the room editor holds render items by index, nothing is added or taken away.
  - **The plate:** drawn on the worker pool and uploaded under `signTextures`. It goes on the picture as a texture
    override, as step 4's signs do.
  - **A lockstep run** waits for its plates.
  - **Without a plaque to hang,** nothing is added: the scene stays as the plan built it.
- **Pointing and reading:**
  - The frame's pick ray finds the plaque in free roam, when nothing of the room is hovered, within 2.5 m. This is the
    reticle, the cursor, or in VR the controller's ray (`plaqueHit()`).
  - A plaque pointed at shows the system's name in the hover's label above it. In VR the controller vibrates when the
    ray comes onto it.
  - Interact opens **the plaque's card** (`assets/ui/room_plaque.rml`, `PlaqueCardPanel`):
    - "Targa della stanza", the logo, the name, then Produttore, Anno and Tipo ("n.d." when empty) and the full
      description, in a region Up and Down scroll.
    - It is a panel level of the product UI: Back closes it, and activate does too.
    - In the headset it is the UI's panel.
- **The run report's catalog section:** `plaques`, `plaquesDrawn`, `plaqueCards`.

## Tests

- **Unit:**
  - `test_history_plaque.cpp`: the details' line; the wrapping (widths, order, the ellipsis on the last line, a word
    too wide, runs of spaces); the plate's brass and its engraved title, details and description; a logo in the name's
    place; the same pixels twice.
  - `test_history_plaques.cpp`:
    - the sites from the hero signs (a room turned round, a shared room, a pack's further piece);
    - the plaque object, the ray's hits and misses, the box;
    - the text, none without a description;
    - the card document's ids, bindings and words.
- **GPU:**
  - **Desktop** (`test_golden_room.cpp`, `[identity]`): the snes-room fixture with the invented theme. The walker
    stands in front of the plaque: the plate up close with its label ("Lumen 16"), then interact opens the card. The
    report counts 1 plaque drawn and 1 card. Checked by eye.
  - **VR** (`test_xr_room.cpp`, the fake runtime): the right controller's ray on the plaque vibrates it, and interact
    opens its card on the headset's panel.
  - **G22 rewritten:** the plaque stands beside the console. With the colours off, against G02 only the box from the
    sign down to the plaque differs.
- **The real library, by hand:**
  - The snes floor's 3 console rooms hang their plaques, with a peak of 285 MiB.
  - The Super Nintendo plaque shows the engraved logo, "Nintendo · 1992 · Console" and the Italian description, cut
    after seven lines.
  - Its card shows the whole text and scrolls.
  - The screenshots stay under `build/`.
- **G02 to G21** are unchanged: no fixture of theirs has a theme, so no plaque is added and no object is replaced.

## Departures

- **The plate is drawn like the signs, not through `renderTo`.** ADR-0019 §6 draws `room_plaque.rml` into a texture
  through the UI. The plate holds only the theme's text, no sentence of Retroverse's own, so it is drawn GPU-free with
  the bundled faces, deterministic and testable at its pixels. `room_plaque.rml` is the card that shows the text in
  full, with the UI's words: Produttore, Anno, Tipo.
- **No stand of its own.** The ADR has the plaque "on its own small stand beside the hero corner". It hangs on the hero
  corner's wall instead, where the template leaves 85 cm free between the cabinet and the featured shelf. The floor
  beside the cabinet is the armchair's and the lamp's.
- **Focus and Inspect are the label and the card.** A plaque is no catalog item: WorldInteraction's Focus and Inspect
  lift games. Pointing at a plaque labels it, and interact opens its card, as the hub's showcase opens its card.
- **The hardware type as the theme writes it** ("Console"). The theme localizes only the description; a translation
  of the types waits for a theme that needs one.
