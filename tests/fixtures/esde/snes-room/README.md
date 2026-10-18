# The `snes-room` ES-DE fixture

The room of G02 and G10 (docs/roadmap.md, I3): one cartridge system, ES-DE's `snes` with an invented full name, 50
invented titles with generated covers, cartridge pictures and clips, the damaged media the fallbacks need, two titles
released in two regions, a game without a cover, two hacks without media, and the ten games G10 adds after a first
plan. `rv::test::Fixture::materialize("esde/snes-room")` copies it under the build tree and generates what `files.json`
lists (I3 step 1c, `tests/support/fixture_payloads.hpp`): the ROM payloads with their times, every picture (an original
procedural picture with the title: abstract geometry and the title in the built-in bitmap font,
`assets::makeCoverPlaceholder`) and every clip (`media::test_video`: a moving pattern with the frame number burned in).
Nothing commercial: no real title, company, system name, logo or cover; no media of the user's. The tests in
`tests/unit/esde/test_snes_fixtures.cpp` assert the counts below.

## Layout

```text
<fixture root>                       ES-DE's `~`
├── ES-DE/settings/es_settings.xml   ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory {{FIXTURE_ROOT}}\media
├── ES-DE/gamelists/snes/gamelist.xml
├── ES-DE/custom_systems/            empty
├── Programs/ES-DE/                  the install folder (%ESPATH%): a 4 KiB ES-DE.exe placeholder, es_systems.xml,
│                                    es_find_rules.xml, Emulators/FakeEmulator/fake-emulator.exe (rv_fake_emulator.exe)
├── roms/snes/                       55 ROM files (64 to 512 KiB of payload each, fixed last write times)
├── media/snes/<kind>/               the generated media
└── additions/                       G10's ten games, laid over the root by Fixture::applyOverlay("additions")
    ├── ES-DE/gamelists/snes/gamelist.xml    the gamelist above with their ten entries appended
    ├── roms/snes/                           their ten ROM files
    └── media/snes/covers/                   their ten covers
```

The system (`es_systems.xml`): `snes`, full name **Lumen 16 Home System** (invented: no trademarked name in any image a
golden takes), extensions `.sfc .SFC .smc .SMC .zip .ZIP`, platform and theme `snes`, one command, "Fake Emulator":
`%EMULATOR_FAKE-EMULATOR% -- %ROM%`, found by its static path `%ESPATH%\Emulators\FakeEmulator\fake-emulator.exe`. The
copy finds the DLLs it needs only through PATH (see `esde/standard`). Settings: ShowHiddenFiles and ShowHiddenGames on,
ParseGamelistOnly and LegacyGamelistFileLocation off, no scraper credentials.

## The games

Roles: **placed** are the 50 titles a first plan places (G02: all face-out, ADR-0010 §3); **duplicate** another release
of a placed title (room selection's `duplicate-title`, its USA release the representative); **no-cover** and **hack**
are games without a cover (`no-cover`); **addition** are G10's, under `additions/` until laid over the root. Every game
but the hacks has a gamelist entry: its `<name>` (the title as written below, without the tags), an invented
description, rating, release date (1990 to 1998), developer, publisher, genre and players. Covers are 680 x 497 PNG
unless the table says otherwise; cartridge pictures (`physicalmedia`) 600 x 520 PNG with a transparent background;
clips 4:3, 640 x 480, 30 fps, 4 s, MPEG-4 Part 2 in the container their extension names, unless the table says
otherwise (Ember Kettle Expedition's is 320 x 240, Juniper Jamboree's 6 s, Fennel Ferry's Motion JPEG).

| # | File under `roms/snes/` | Role | Priority data | Media |
|---|---|---|---|---|
| 1 | `Amber Lantern (USA).sfc` | placed | favorite, playcount 42 | covers .png, backcovers .png, 3dboxes .png 560x700, physicalmedia .png, screenshots .jpg 512x448, titlescreens .jpg 512x448, marquees .png 800x300, fanart .jpg 1280x720, miximages .png 640x480, videos .mp4 |
| 2 | `Bramble Burrow Rally (USA).sfc` | placed | playcount 17 | covers .png, physicalmedia .png, screenshots .jpg 512x448 (truncated), videos .mp4 |
| 3 | `Cobalt Compass (USA).sfc` | placed | favorite, playcount 8 | covers .png, physicalmedia .png, videos .mp4 |
| 4 | `Dappled Dovecote (World).sfc` | placed | playcount 23 | covers .png, physicalmedia .png, screenshots .jpg 512x448, videos .mp4 |
| 5 | `Ember Kettle Expedition (USA).sfc` | placed | playcount 5 | covers .png, physicalmedia .png, videos .mp4 320x240 |
| 6 | `Fennel Ferry (USA).sfc` | placed | playcount 11 | covers .jpg 640x468, physicalmedia .png, videos .mkv (Motion JPEG) |
| 7 | `Gilded Gazebo (USA).sfc` | placed | favorite, playcount 3 | covers .png, physicalmedia .png, videos .avi |
| 8 | `Hazel Hedgerow (USA) (Rev 1).sfc` | placed | - | covers .png, physicalmedia .png, videos .mp4 |
| 9 | `Indigo Inkwell (World).sfc` | placed | playcount 2 | covers .png, physicalmedia .png |
| 10 | `Juniper Jamboree (USA).sfc` | placed | playcount 31 | covers .png, physicalmedia .png, videos .mp4 6 s |
| 11 | `Lilac Loom (USA).sfc` | placed | playcount 1 | covers .png, videos .mp4 (**corrupt**: G08b's clip) |
| 12 | `Mossy Meadow Mystery (USA).sfc` | placed | favorite | covers .png, videos .mp4 (**truncated**) |
| 13 | `Nutmeg Nook (USA).sfc` | placed | playcount 14 | covers .png, videos .mp4 (**empty**) |
| 14 | `Opal Oriel (Europe).sfc` | placed | - | covers .png, videos .avi (**unsupported codec**) |
| 15 | `Pewter Parasol (USA).sfc` | placed | playcount 6 | covers .png; **the missing clip**: no video |
| 16 | `Quilted Quarry (World).sfc` | placed | - | covers .png |
| 17 | `Russet Rookery (USA).sfc` | placed | playcount 9 | covers .png |
| 18 | `Saffron Satchel (USA).sfc` | placed | - | covers .png |
| 19 | `Tawny Toboggan (USA).sfc` | placed | favorite, playcount 2 | covers .png |
| 20 | `Umber Umbrella (USA).sfc` | placed | playcount 4 | covers .png 497x680 (**the portrait cover**) |
| 21 | `Velvet Vineyard (USA).smc` | placed | - | covers .png 512x512 (square) |
| 22 | `Willow Windmill (USA).sfc` | placed | playcount 19 | covers .png 1360x994 (large) |
| 23 | `Woolly Abacus (Japan).sfc` | placed | - | covers .png |
| 24 | `Yarrow Bellows (USA).sfc` | placed | - | covers .jpg |
| 25 | `Marigold Carousel (World).sfc` | placed | playcount 7 | covers .jpg |
| 26 | `Clover Dumpling (USA).sfc` | placed | - | covers .png |
| 27 | `Sorrel Easel (USA).sfc` | placed | playcount 1 | covers .png |
| 28 | `Briny Fiddle Regatta (USA).sfc` | placed | - | covers .png |
| 29 | `Copper Hammock (USA).sfc` | placed | - | covers .jpg |
| 30 | `Kiln Keeper - The Dappled Almanac (USA).sfc` | placed | favorite, playcount 12 | covers .png; named "Kiln Keeper: The Dappled Almanac" |
| 31 | `Pinwheel Parade (USA).sfc` | placed | - | covers .png |
| 32 | `Marmalade Igloo (World).sfc` | placed | playcount 3 | covers .png |
| 33 | `The Lantern of Saffron Hollow (USA).sfc` | placed | - | covers .png |
| 34 | `Teapot Tumble (USA).sfc` | placed | playcount 10 | covers .jpg |
| 35 | `Acorn Errands (USA).sfc` | placed | - | covers .png |
| 36 | `Garret Ghost Picnic (USA).sfc` | placed | - | covers .png |
| 37 | `Nutmeg Nook II (USA).sfc` | placed | playcount 2 | covers .png |
| 38 | `Quarry Quartet (USA).sfc` | placed | - | covers .png |
| 39 | `Trellis Tales (World).sfc` | placed | - | covers .png |
| 40 | `Satchel Scouts (USA).sfc` | placed | - | covers .jpg |
| 41 | `Orchard Oddities (USA).sfc` | placed | playcount 4 | covers .png |
| 42 | `Dovecote Derby (USA).sfc` | placed | - | covers .png |
| 43 | `Inkwell Islands (USA).sfc` | placed | - | covers .png |
| 44 | `Bellows Brigade (USA).sfc` | placed | playcount 1 | covers .png (**truncated**: decodeImage ParseError) |
| 45 | `Compass Caper (USA).sfc` | placed | - | covers .png |
| 46 | `Loom Lullaby (USA).sfc` | placed | - | covers .png |
| 47 | `Parasol Patrol (USA).sfc` | placed | - | covers .png (**not an image**: a line of text; decodeImage Unsupported) |
| 48 | `Windmill Whirl (USA).sfc` | placed | - | covers .png |
| 49 | `The Copper Kettle and the Juniper Thimble Expedition of the Long Valley (USA).sfc` | placed | - | covers .png (a long title: two lines and an ellipsis on a spine) |
| 50 | `Rookery Rumble (USA).sfc` | placed | playcount 1 | covers .png |
| 51 | `Cobalt Compass (Europe).sfc` | duplicate | - | covers .jpg |
| 52 | `Gilded Gazebo (Europe).sfc` | duplicate | - | covers .jpg |
| 53 | `Thistle Trellis (USA).sfc` | no-cover | playcount 5 | screenshots .jpg 512x448, videos .mp4 (media, but no cover) |
| 54 | `Amber Lantern Night Edition (Hack).sfc` | hack | no gamelist entry | none |
| 55 | `Fennel Ferry (USA) [h1].sfc` | hack | no gamelist entry | none |
| 56 | `Zinnia Zephyr (USA).sfc` | addition | - | covers .png |
| 57 | `Kestrel Kite Festival (USA).sfc` | addition | - | covers .png |
| 58 | `Gazebo Gardeners (USA).sfc` | addition | - | covers .png |
| 59 | `Hammock Heist (USA).sfc` | addition | - | covers .png |
| 60 | `Easel Escapade (USA).sfc` | addition | - | covers .png |
| 61 | `Dumpling Drift (USA).sfc` | addition | - | covers .png |
| 62 | `Abacus Avenue (USA).sfc` | addition | - | covers .png |
| 63 | `Carousel Comet (USA).sfc` | addition | - | covers .png |
| 64 | `Igloo Interlude (USA).sfc` | addition | - | covers .png |
| 65 | `Toboggan Tangle (USA).sfc` | addition | - | covers .png |

The picture's seed is 100 plus the row's index from 0 (Amber Lantern 100, Rookery Rumble 149, Zinnia Zephyr 155): every
kind of one game shows the same pattern and title. The clip's seed is 300 plus that index. "The missing clip" is a game
with a cover and no video file; a test that needs a clip the catalog has recorded but that is gone deletes a valid one
after the import.

## Expected counts

| | As committed | With `additions/` laid over the root |
|---|---|---|
| Entries (all games) | 55 | 65 |
| Gamelist entries | 53 | 63 |
| Favorites / with a play count | 6 / 26 | 6 / 26 |
| Games with a cover (eligible, ADR-0010 §3) | 52 | 62 |
| Titles among them (one cartridge per title) | 50 | 60 |
| `duplicate-title` | 2 (the two Europe releases) | 2 |
| `no-cover` | 3 (Thistle Trellis, the two hacks) | 3 |
| Covers: decode / truncated / not an image | 50 / 1 / 1 | 60 / 1 / 1 |
| Covers: PNG / JPEG; portrait | 44 / 8; 1 | 54 / 8; 1 |

Media files per kind folder (as committed; the additions add 10 covers): covers 52, physicalmedia 10, screenshots 4 (one
truncated), videos 14 (10 that play: 8 `.mp4`, one `.mkv`, one `.avi`; and the corrupt, truncated and empty `.mp4` and
the unsupported-codec `.avi`), backcovers, 3dboxes, titlescreens, marquees, fanart and miximages 1 each (Amber Lantern),
manuals and custom none. What the decoder makes of the damaged clips (`media::DecodeFailure`): Lilac Loom
`CorruptData`, Mossy Meadow Mystery `Truncated`, Nutmeg Nook `InvalidFile`, Opal Oriel `UnsupportedCodec`.
