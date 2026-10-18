# The `snes-single` ES-DE fixture

The room of G06 (docs/roadmap.md, I3; style §21's P0 single-ROM room): one cartridge system, ES-DE's `snes` with the
invented full name **Lumen 16 Home System**, one ROM with a gamelist entry and no media at all. The media root exists
and is empty, so that media are known (a plan may be generated, ADR-0010 §3: "unknown media decide nothing") and no
game of the system has a cover: every game is eligible and the one title is placed face-out, the room furnished from
the template alone. `rv::test::Fixture::materialize("esde/snes-single")` generates what `files.json` lists. Nothing
commercial: no real title, company or system name. `tests/unit/esde/test_snes_fixtures.cpp` asserts the counts below.

## Layout

```text
<fixture root>                       ES-DE's `~`
├── ES-DE/settings/es_settings.xml   as snes-room's: ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory {{FIXTURE_ROOT}}\media
├── ES-DE/gamelists/snes/gamelist.xml
├── ES-DE/custom_systems/            empty
├── Programs/ES-DE/                  as snes-room's: ES-DE.exe placeholder, es_systems.xml, es_find_rules.xml, the fake emulator
├── roms/snes/Pocket Lighthouse (USA).sfc    128 KiB of payload, last written 1994-06-11T12:00:00Z
└── media/                           empty: no system folder
```

## Expected counts

| | snes |
|---|---|
| Entries, games | 1, 1 |
| Gamelist entries | 1: "Pocket Lighthouse", rating 0.7, released 1994-06-11, Paper Owl / Harborlight, Avventura, 1 player |
| Favorites, play counts | none |
| Media files | 0 (the media root is listed: `media/snes` is missing, which is no error) |
| Eligible (no game of the system has a cover, so all are) | 1 |
