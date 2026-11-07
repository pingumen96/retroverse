# I11 step 1: the identity reader

ADR-0019 §2, roadmap I11 step 1. The systems' identity is read from ES-DE's themes and Retroverse's `identity.json`.
`retroverse-cli identity show` prints it with its sources. Nothing in the app shows it yet: that is steps 2 to 6.

## What was built

- **`catalog::SystemIdentity`** (`rv/catalog/system_identity.hpp`, domain, plain data):
  - the name, maker, year, release date, kind of hardware and description, each an `IdentityText` with its
    `IdentitySource` (`override`, `theme`, `active-theme`, `full-name`, `none`) and origin;
  - the colours, the logo (`IdentityLogo`: file, format, source) and the issues.
- **`rv/esde/themes.hpp`:**
  - `isThemeId()`: 1 to 64 characters of `[A-Za-z0-9._-]`, no leading dot. It is checked before an es_systems.xml
    `<theme>` names a file.
  - `themeFolders()`: `<ES-DE home>/themes`, `<install>/themes`. `findTheme()`.
  - `themeMetadataFile()` and `readThemeMetadata()`: linear-es-de's `system/metadata/<id>.xml`, at most 256 KiB,
    through the module's XML loader.
    - Values have their whitespace collapsed and are left out when longer than 256 bytes or holding a control character.
    - The year must be four digits.
    - The description is the `it_IT` translation when there is one; a long one is cut at its last whole sentence within
      4,096 bytes.
    - Colours are six hex digits, given as `#RRGGBB`.
  - `findThemeLogo()`: the layouts `system/logos/<id>`, `_inc/systems/logos/<id>`, `<id>/images/logo`, each as .svg,
    then .png, then .jpg, with a size limit (SVG 1 MiB, PNG and JPEG 8 MiB).
- **`rv/esde/identity.hpp`:**
  - `readIdentityFile()`: `identity.json`, schema 1, strict: `theme`, `themeRoots`, `activeTheme` and per-system
    overrides of every field.
  - `identitySources()`: the theme folders found.
  - `resolveSystemIdentities()`: the order of ADR-0019 §2.
- **`es_settings.xml`'s `Theme`** joins the whitelist (`EsdeSettings::theme`, `activeThemeName()`).
- **`retroverse-cli identity show [--system <name>] [--json]`:** over the catalog's systems.
- **The fixture `esde/theme`:** invented systems' metadata, original SVG logos, a generated PNG and an SVG past the
  limit.
- **Tests:**
  - `tests/unit/esde/test_esde_identity.cpp`;
  - the settings test's whitelist;
  - `tests/unit/cli/test_cli_identity.cpp` (the standard fixture imported, `identity.json` pointing at the theme
    fixture);
  - the hidden `[real-library][identity]` case.

## Decisions and departures

- **Where the identity is resolved.** The domain type is `catalog::SystemIdentity` and the resolution is in `rv_esde`,
  not `app::SystemIdentity` as ADR-0019 §2 says. The CLI and the app need the same resolution. The scene builder
  (`rv_world`, step 4) must read the type without depending on `rv_esde`. And `identity.json` sits beside
  `library.json`, which `rv_esde` already reads. Building it on the catalog worker comes with its first consumer,
  step 3.
- **Fields are never mixed from two themes.** The first metadata theme that has a system's file gives all its fields,
  and the next themes are not read for the missing ones: a name from one theme beside a description from another
  could disagree. The logo is searched separately, in the metadata themes, then the active theme.
- **A theme's translated values other than the description are not taken.** linear-es-de translates
  `systemHardwareType` ("Konsole", "Console"); the product is Italian, so the Italian description is taken, and the
  other values stay as the theme's default variables write them.
- **The systems come from the catalog.** `identity show` reads them as `systems` does, so it needs an import. That
  matches what the app will show.
- **`identity.json` errors.** The CLI stops (exit 3), so that a typo is seen. The app (step 3) will log it and take
  the defaults, as with the other files of `config`.

## Figures

`./scripts/real-library.ps1 -Preset msvc-x64-debug -Filter '[real-library][identity]'` (2026-11-07, ES-DE closed, the
whole library imported into `build/msvc-x64-debug/real-library/identity`):

- **Coverage:** the catalog's 195 systems (every system ES-DE configures, not only the 45 with games) all take their
  name from linear-es-de, and each has an SVG logo, an Italian description and colours. None falls back to ES-DE's
  full name.
- **Time:** `identity show` takes 0.68 s in Debug, the catalog read included.
- **Read-only:** 0 differences among 542,121 entries of the ES-DE home, the install and the roots.

## Follow-ups

- **Other metadata layouts** (BACKLOG, "Parked at I11's planning"). A system without a linear-es-de file shows its
  full name.
- **WebP logos** (BACKLOG).
