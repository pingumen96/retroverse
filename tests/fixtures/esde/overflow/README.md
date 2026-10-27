# The `overflow` ES-DE fixture

The rooms past their capacity of I4b (ADR-0015 §7, `docs/roadmap.md`): two systems with more titles than their rooms
hold at their template's growth limit, so that generator 1.5's expansion rooms (I4b step 7) have something to place.
Generated when the fixture is materialized (`tests/support/fixture_collection.hpp`'s collections, one per system):
ROM payloads, gamelists and small covers. Nothing commercial: titles, companies and descriptions come from the
generator's word lists, the covers are the original procedural picture with the title
(`tests/support/fixture_payloads.hpp`). `tests/unit/esde/test_overflow_fixture.cpp` and
`tests/unit/cli/test_cli_worldplan_undo.cpp` assert the counts below. It materializes in about 6 s in Debug.

## Layout

```text
<fixture root>                         ES-DE's `~`
├── ES-DE/settings/es_settings.xml     as building's: ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory ...\media
├── ES-DE/gamelists/<system>/          generated
├── ES-DE/custom_systems/              empty
├── Programs/ES-DE/                    the install: ES-DE.exe placeholder, es_systems.xml, es_find_rules.xml, the fake
│                                      emulator, resources/MAME (invented one-entry lists: no set of the fixture)
├── roms/<system>/                     the ROM files
└── media/<system>/covers/             the generated covers
```

Every system runs the fake emulator (`%EMULATOR_FAKE-EMULATOR% -- %ROM%`, found by its static path, as building's).

## Systems

| System | Full name (invented) | Platform | ROMs | Floor | Media family | Seed | Titles | Covers |
|---|---|---|---|---|---|---|---|---|
| `amiga` | Lumen Desk Computer | amiga | `.adf` | computer | big_box_v2 | 5200 | 1,700 | every one, 160 x 220 |
| `mame` | Arcade Board Library | arcade | `.zip` | ground | arcade_cabinet | 5100 | 60 | every one, 140 x 200 |

No second releases and no clips; the covers are small (portrait, as the big boxes' and the flyers') to keep the
fixture quick to make.

## Expected counts

The library snapshot: amiga 1,700 entries, 1,700 games, 1,700 covers; mame 60 entries, 60 games, 60 covers.

The building `retroverse-cli worldplan generate --seed 102938` makes of it until generator 1.5: the ground floor with
the arcade room `area:arcade` (mame's section: 53 sets placed, 7 unplaced for capacity) and the computer floor with
`room:amiga` (1,700 titles, over 30: its own room; 1,561 placed, the room at its growth limit, 139 unplaced for
capacity). I4b step 7's expansion rooms place them all (`room:amiga:2`, `area:arcade:2`).
