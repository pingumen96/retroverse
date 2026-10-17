# The `media-roots` ES-DE fixture

A media root with nothing in it, and media roots that cannot be read (I2 step 8): what `esde::EsdeMediaResolver`,
`catalog::measureCoverage` and `retroverse-cli media` make of them. ES-DE finds media by convention under
`MediaDirectory` (ADR-0008 §5); an empty media root means that no game has media, which is no error, while a media root
that is missing or whose drive cannot be reached means that nothing can be known about any media (`Unavailable`): the
import then keeps the media it stored. The tests make the variants from the materialized copy with
`Fixture::replaceText`.

## Layout

```text
<fixture root>                              the user's home folder: ES-DE's ~
├── ES-DE/settings/es_settings.xml          ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory {{FIXTURE_ROOT}}\media
├── ES-DE/gamelists/nes/gamelist.xml        two <game> entries
├── ES-DE/custom_systems/                   empty
├── Programs/ES-DE/                         the install folder: ES-DE.exe (a placeholder), es_systems.xml, find rules
├── roms/nes/                               Rocket Ranger (USA).nes, Unscraped Hop (World).nes
├── roms/gb/                                Pocket Pal (World).gb (gb has no gamelist)
└── media/                                  the media root: an empty folder
```

Systems: gb (`.gb .GB .zip .ZIP`) and nes (`.nes .NES .zip .ZIP`), each with one command of an invented emulator that
no find rule finds (nothing is launched here). The nes gamelist names `./Rocket Ranger (USA).nes` (its file is there)
and `./Lost Lagoon (Europe).nes` (its file is gone: a missing file); `Unscraped Hop (World).nes` and gb's `Pocket Pal
(World).gb` have no gamelist entry.

## Expectations

| Settings | Media root | Every entry's media | `media --coverage` | `media --system nes --path "Rocket Ranger (USA).nes"` |
|---|---|---|---|---|
| as committed | `media`, empty | none of the 12 kinds (no system folder: no media) | exit 0; nes: 2 gamelist entries, 1 with its file, 1 scraped game, 2 games (Rocket Ranger, Unscraped Hop); gb: 1 game (Pocket Pal); every share 0.0% | exit 0, 0 of 12 kinds |
| MediaDirectory `{{FIXTURE_ROOT}}\unplugged\media` | missing | `Unavailable` | exit 1: `media-root-unavailable` (error), every entry unknown, no share | exit 1 |
| MediaDirectory on a drive letter that is not mapped (chosen by the test) | unreachable | `Unavailable` | exit 1, as above | exit 1 |
