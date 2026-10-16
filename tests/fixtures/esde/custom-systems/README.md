# The `custom-systems` ES-DE fixture

The user's `custom_systems` beside ES-DE's bundled configuration (I2 step 2), and the rules and quirks ES-DE 3.4.1
applies while it reads them (SystemData::loadConfig, FindRules::loadFindRules). The tests make the variants from the
materialized copy with `Fixture::replaceText`.

## Layout

```text
<fixture root>
├── ES-DE/settings/es_settings.xml           ROMDirectory "{{FIXTURE_ROOT}}\roms"; fake scraper credentials
├── ES-DE/custom_systems/es_systems.xml      gb (replaces the bundled one), homebrew (new)
├── ES-DE/custom_systems/es_find_rules.xml   see below
├── Programs/ES-DE/resources/systems/windows/es_systems.xml     the bundled systems and their quirks
├── Programs/ES-DE/resources/systems/windows/es_find_rules.xml  FAKE-EMULATOR, OS-SHELL, cores RETROARCH, ODD-CORE
└── roms/{gb,gb-custom,homebrew,nes,pcfx,dirgames}/
```

## Systems

ES-DE reads the custom file first; the first definition of a name is the system, the later ones are fallbacks ES-DE
tries when the earlier ones find no game (`SystemsConfiguration::fallbacks`).

| System | Definition | What it exercises |
|---|---|---|
| gb | custom: "Game Boy (custom)", `%ROMPATH%\gb-custom`, `.gb .gbx`, two commands | replaces the bundled gb (fallback 1, issue `system-replaced`) |
| homebrew | custom | a new system |
| nes | bundled | a full name split by a line feed ("NintendoEntertainment System": ES-DE removes line feeds), `<systemsortname>`, extensions split on blanks, a tab, a line feed and commas, an upper-case platform list (`nes`, `famicom`), no `<theme>` (the name) |
| nes (again) | bundled | a repeated name (fallback 2, issue `system-repeated`) |
| dirgames | bundled | `.` (files without extension), one unlabelled command, an empty platform (issue `platform-missing`), an empty `<theme>` |
| pcfx | bundled | `CUE` without its dot, kept (issue `extension-without-dot`) |
| androidgames | bundled | `PLACEHOLDER %ROM%`, flagged as a placeholder |
| unlabelled | bundled | a labelled command after an unlabelled first one: ES-DE keeps the first only (issue `command-label-missing`) |
| duplabels | bundled | a repeated label (ignored, issue `command-label-duplicate`), then an unlabelled command among labelled ones: ES-DE stops there (`command-label-missing`): commands "Same" and "Other" |
| nofullname, (no name) | bundled | skipped (issue `system-skipped`) |
| bad:name | bundled | no valid folder name: skipped (issue `system-name-invalid`) |

Effective systems: androidgames, dirgames, duplabels, gb (custom), homebrew, nes, pcfx, unlabelled. Variants:
`<!-- LOAD-EXCLUSIVE -->` in the custom file replaced by `<loadExclusive/>` leaves only gb and homebrew (the bundled
file is not read, even broken); `<!-- BUNDLED-LOAD-EXCLUSIVE -->` in the bundled file replaced the same way is ignored
(issue `load-exclusive-ignored`).

## Find rules

| Name | Kind | Definition | What it exercises |
|---|---|---|---|
| FAKE-EMULATOR | emulator | custom: `~\Custom Emulators\fake-emulator.exe` | replaces the bundled one (issue `find-rule-replaced`) |
| CUSTOM-EMULATOR | emulator | custom: winregistryvalue (with a `\|` suffix), systempath, staticpath (a wildcard; a `\|` command), winregistrypath | rules kept in document order, typed |
| CUSTOM-EMULATOR (again) | emulator | custom | the first definition counts (issue `find-rule-repeated`) |
| (no name) | emulator | custom | skipped (`find-rule-skipped`) |
| ODD-RULES | emulator | custom: a corepath rule, a rule without type, an androidpackage rule (all skipped), a staticpath entry with spaces around it (kept as written) | |
| OS-SHELL | emulator | bundled | |
| RETROARCH | core | custom `~\Custom Cores` | replaces the bundled `%EMUPATH%\cores` |
| ODD-CORE | core | bundled, a staticpath rule (skipped: cores have corepath rules only) | |

With the custom file broken, it is skipped (issue `find-rules-unreadable`) and the bundled rules apply.
