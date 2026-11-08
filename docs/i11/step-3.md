# I11 step 3: real names and logos in the UI

ADR-0019 §2 and §3; roadmap I11 step 3. The systems' identity read by step 1 reaches the product UI. Wherever a system's
name shows, it is now the identity's name ("Super Nintendo" from linear-es-de, or identity.json's), and the directory
shows each console room's logo, rasterized by step 2's code.

## What was built

- **The identity on the catalog worker** (`catalog_worker.cpp`, `resolveIdentities()`):
  - When the catalog opens and after each refresh, beside the launch library, the worker reads
    `<user data>/config/identity.json`. One that cannot be read is logged and its defaults are taken.
  - It resolves the identity of each system with games: the themes through `esde::identitySources()`, the active one
    from the library's ES-DE settings.
  - A system's shown name is its identity's when identity.json or a theme gives one, else ES-DE's `<fullname>`, else its
    name.
- **`CatalogSystem`:**
  - `fullName` is now that shown name, which the chips, the rows, the card and every `systemFullName()` caller take:
    the directory, the world's notices, the edit bar, the regenerate preview.
  - `esdeName` keeps ES-DE's `<fullname>`.
  - `logo` holds the logo file.
- **The worker's other answers** carry the shown name too: favourites and recent games (the directory, the hub's
  showcase), statistics (games and systems), media coverage and the library screen's held systems and reviews.
- **The emulators screen** shows ES-DE's name as the detail when it differs ("Lumen 16 · in ES-DE Nintendo SNES (Super
  Nintendo)").
- **`SystemLogos`** (`src/app/src/system_logos.cpp`):
  - Each system's logo file is rasterized on the worker pool (`assets::loadLogo` into a 512 x 128 box) and its alpha
    premultiplied, as RmlUi composites.
  - Logos are uploaded on the main thread, one a frame, with mips, under the new GPU memory kind `uiTextures`.
  - Each is registered with RmlUi as `rv-texture:logo-<system>`.
  - A logo that cannot be read is logged and left out: the row shows the name alone.
- **The directory:**
  - A console room's row shows its logo between its number and its name, in a 120 x 40 box (`directory.rml`,
    `.row-logo`), once the logo is registered (`ProductUi::systemLogoSource()`).
  - The building's rows are made again when a logo arrives.
  - The VR panel is the same UI, so the logos show in the headset too.
- **The run report's catalog section:**
  - `identityNames`: the systems whose name differs from ES-DE's;
  - `logoFiles`: the systems with a logo file;
  - `logosShown`: the logos registered.

## Tests

- **Unit** (`test_catalog_worker.cpp`, `[identity]`): the standard fixture with an identity.json pointing at the
  invented theme fixture.
  - snes is "Lumen 16" in its `CatalogSystem`, its chip, its rows and `systemFullName()`, with ES-DE's name kept and its
    logo file found.
  - psx, which the theme lacks, keeps "Sony PlayStation" and has no logo.
- **GPU** (`test_world_elevator.cpp`, `[identity]`): the building fixture with that identity.json, the directory opened
  on snes' floor.
  - The run report counts the names and logos, every logo file registered.
  - The screenshot shows "Lumen 16" with the fixture's logo, checked by eye.
- **Unchanged without a theme:** the fixtures' ES-DE installs hold no linear-es-de and none of their data directories
  has an identity.json, so every golden and every other GPU case runs as before.

## Departures

- **Logos in the console rooms' rows only.** A shared or arcade room lists several systems by name. The elevator's
  panel names no system: it only says which key to press.
- **The identity is resolved for the systems with games only**, the ones the UI shows, on every opening and refresh. On
  the reference library that is 48 systems; step 1 measured all 195 in 0.68 s in Debug, the catalog read included.
- **No logo cache.** As step 2 decided: a few milliseconds each on the pool.
- **A new GPU memory kind, `uiTextures`.** The scene textures' kind must equal what the texture cache holds (the
  run's accounting check), and the logos are not the cache's.
