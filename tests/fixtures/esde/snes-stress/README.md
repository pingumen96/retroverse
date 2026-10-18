# The `snes-stress` ES-DE fixture

The room the I3 budgets are measured on (docs/roadmap.md, I3, "Budgets"), the room of G02s and of the benchmark path:
one cartridge system, ES-DE's `snes` with the invented full name **Lumen 16 Home System**, 2,000 invented titles, 300
of them with a second release, every ROM file with a gamelist entry and a generated cover, four with a clip. Too large to
list file by file, the collection is generated when the fixture is materialized from the one `collections` item of
`files.json` (`tests/support/fixture_collection.hpp`, seed 2000): file names, gamelist, covers and clips are the same on
every machine and every run. Nothing commercial: titles, companies and descriptions come from word lists of ordinary
words (amber, lantern, orchard, thimble, ...) and the covers are the original procedural picture with the title
(`tests/support/fixture_payloads.hpp`). `tests/unit/esde/test_snes_fixtures.cpp` asserts the counts below, and its
hidden `[.bench]` case that the fixture is materialized within 10 s in Release (8.6 s on a fully loaded reference
machine, docs/i3/step-1c.md; the pictures and clips are generated on up to 8 threads, the covers through the quick PNG
writer of `fixture_payloads.hpp`).

## Layout

```text
<fixture root>                       ES-DE's `~`
├── ES-DE/settings/es_settings.xml   as snes-room's
├── ES-DE/gamelists/snes/gamelist.xml    generated: 2,300 entries
├── ES-DE/custom_systems/            empty
├── Programs/ES-DE/                  as snes-room's: ES-DE.exe placeholder, es_systems.xml, es_find_rules.xml, the fake emulator
├── roms/snes/                       2,300 ROM files of 4 KiB, "<title> <tags>.sfc"
└── media/snes/covers/               2,300 covers
    media/snes/videos/               4 clips
```

## The rules (`fixture_collection.hpp`)

- **Titles**: 2,000, each title key once (the title lower-cased): "Adjective Noun" (40%), "Adjective Noun Tail"
  (30%), "Noun: The Adjective Tail" (12%; the file name writes " - " for ": "), "Adjective Noun II/III/IV" (10%) and
  "The Adjective Noun and the Adjective Noun" (8%, long), drawn with `core::Random` (splitmix64/1) from the seed.
- **Regions** of title i (from 0): "(World)" when i % 20 = 7, "(Europe)" when i % 20 = 13, "(Japan)" when i % 40 = 19,
  else "(USA)".
- **Second releases**: the 300 titles whose index i has i % 20 in {1, 4, 9}: a "(Europe)" release beside the "(USA)"
  one for 1 and 4 (200), and for 9 a "(USA) (Rev 1)" beside the plain "(USA)" (100), where No-Intro's highest revision
  is the representative and the plain file the duplicate (ADR-0010 §3).
- **Gamelist**: every file has an entry: `<name>` (the title), a description, rating (0 to 1 in steps of 0.05),
  release date (1990 to 1998), developer, publisher, genre (in Italian, as the reference library's), players; a
  favorite every 25th title (80), a play count and last-played time on about a quarter of the titles (the
  representatives only).
- **Covers**: 680 x 497 PNG; a JPEG (`.jpg`) when i % 10 = 3 (200 titles); a portrait PNG of 497 x 680 when i % 50 = 11
  (40 titles). Both releases of a title share the picture.
- **Clips**: the first 4 titles have a 4:3 clip, `.mp4`, 320 x 240, 30 fps, 4 s, MPEG-4 Part 2.

## Expected counts

| | snes |
|---|---|
| Entries, games (all with a gamelist entry) | 2,300, 2,300 |
| Titles (eligible title keys) | 2,000: room selection places 2,000 (ADR-0010 §3: 56 face-out, 1,944 spine-out) |
| Duplicates (`duplicate-title`) | 300: 200 "(Europe)" beside "(USA)", 100 "(USA)" beside "(USA) (Rev 1)" |
| Files named "(Europe)" | 300: the 200 second releases and the 100 titles released in Europe alone |
| Favorites | 80 |
| Covers | 2,300 (2,100 PNG, 200 JPEG; 60 portrait: 40 titles, 20 of them with a second release), every one decodes |
| Clips | 4, all playable |
