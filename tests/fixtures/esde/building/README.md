# The `building` ES-DE fixture

The building of I4 (ADR-0013, docs/roadmap.md): a few hundred invented games over the ground floor and four floors,
each system as I4 meets it. Nine systems under ES-DE's names with invented full names, generated when the fixture is
materialized (`tests/support/fixture_collection.hpp`'s collections, one per system, and a few files of `files.json`):
ROM payloads, gamelists, covers of each family's shapes and a few clips. Nothing commercial: titles, companies and
descriptions come from the generator's word lists, the covers are the original procedural picture with the title
(`tests/support/fixture_payloads.hpp`). `tests/unit/esde/test_building_fixture.cpp` and
`tests/unit/cli/test_cli_worldplan_building.cpp` assert the counts below. It materializes in about 3 s in Debug.

## Layout

```text
<fixture root>                         ES-DE's `~`
├── ES-DE/settings/es_settings.xml     as snes-room's: ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory ...\media
├── ES-DE/gamelists/<system>/          generated, one per system but kodi
├── ES-DE/custom_systems/es_systems.xml  pocketlab, the custom system
├── Programs/ES-DE/                    the install: ES-DE.exe placeholder, es_systems.xml, es_find_rules.xml, the fake
│                                      emulator, resources/MAME (invented one-entry lists: no set of the fixture)
├── roms/<system>/                     the ROM files; roms/psx/*.m3u the two multi-disc playlists (committed)
├── media/<system>/covers/, videos/    the generated media
└── additions/                         laid over the root by Fixture::applyOverlay("additions")
```

Every system runs the fake emulator (`%EMULATOR_FAKE-EMULATOR% -- %ROM%`, found by its static path, as snes-room's).

## Systems

| System | Full name (invented) | Platform | ROMs | Floor | Media family | What it is for |
|---|---|---|---|---|---|---|
| `mame` | Arcade Board Library | arcade | `.zip` | ground | generic_case | arcade: 25 sets, 15 with a cover (portrait flyers), 10 without |
| `snes` | Lumen 16 Home System | snes | `.sfc` | gen4 | cartridge_home_16bit | cartridges: 40 titles, 2 second releases |
| `megadrive` | Lumen Drive Home System | megadrive | `.md` | gen4 | cartridge_home_16bit | a new system: its folder comes with `additions/` |
| `psx` | Orbit Disc System | psx | `.chd`, `.m3u` | gen5 | disc_case_cd | jewel cases: 38 titles, square and portrait covers, and two multi-disc games |
| `gb` | Pocket Lumen | gb | `.gb` | handheld | cartridge_handheld | handheld cartridges: 30 titles, 2 second releases |
| `pokemini` | Pico Pocket | pokemini | `.min` | handheld | cartridge_handheld | a small system: 8 titles (under 30; `additions/` takes it past) |
| `pocketlab` | Pocket Lab Homebrew | gb | `.gb` | handheld | cartridge_handheld | a custom system (`ES-DE/custom_systems`) with a name systems.json does not know: gb's row by its platform tag |
| `amiga` | Lumen Desk Computer | amiga | `.adf` | computer | big_box | no media at all: every game placed with a generated label |
| `kodi` | Media Center Shortcuts | pcwindows | `.lnk` | none | none | excluded: 2 shortcuts, no gamelist, no room |

## The collections (`files.json`)

The generator's rules (`fixture_collection.hpp`): invented titles, their gamelist entries, a cover per title (JPEG when
the title's index i has i % 10 == 3), the first `videos` titles with a 4:3 clip; I4 step 2's keys give the covers'
sizes in turn (`coverSizes`), the titles with a cover (`covers`, the first ones) and the first title whose files an
overlay writes (`from`).

| System | Seed | Titles | Second releases | Covers | Clips |
|---|---|---|---|---|---|
| mame | 3100 | 25 | 0 | the first 15, 476 x 680 | 1 |
| snes | 3200 | 40 | 2 (Europe) | 680 x 497 (index 11 portrait) | 2 |
| psx | 3300 | 38 | 0 | 600 x 600, 600 x 600, 476 x 680 in turn | 2 |
| gb | 3400 | 30 | 2 (Europe) | 600 x 610, 680 x 497 in turn | 1 |
| pokemini | 3500 | 8 | 0 | 600 x 600 | 0 |
| amiga | 3600 | 20 | 0 | none | 0 |
| pocketlab | 3700 | 6 | 0 | 680 x 497 | 0 |

The two multi-disc games of psx (`files.json`'s files): `Twin Harbor Saga (USA).m3u` (two discs, a 476 x 680 cover)
and `Starfall Requiem (Europe).m3u` (three discs, a 600 x 600 cover), their discs `.chd` payloads, no gamelist entry:
ES-DE shows the playlist under its file name and hides the discs as its members.

## `additions/`

| What | How |
|---|---|
| megadrive, 12 titles with covers | a collection of seed 3800 under `additions/` |
| snes: 5 titles more | snes's collection with 45 titles from 40: its whole gamelist, the new titles' files |
| psx: 5 titles more | psx's with 43 titles from 38 |
| pokemini: 24 titles more, 32 in all | pokemini's with 32 titles from 8 |

## Expected counts

The library snapshot (entries, games, disc members; games with a cover; clips):

| System | As committed | With `additions/` |
|---|---|---|
| mame | 25 entries, 25 games; 15 covers, 1 clip | the same |
| snes | 42, 42; 42 covers (41 landscape, 1 portrait), 2 clips | 47, 47; 47 covers |
| megadrive | none (no folder) | 12, 12; 12 covers |
| psx | 45 entries, 40 games, 5 disc members (2 groups); 40 covers (27 square, 13 portrait), 2 clips | 50, 45, 5; 45 covers |
| gb | 32, 32; 32 covers (16 portrait 600 x 610, 16 landscape), 1 clip | the same |
| pokemini | 8, 8; 8 covers (square) | 32, 32; 32 covers |
| pocketlab | 6, 6; 6 covers | the same |
| amiga | 20, 20; no media | the same |
| kodi | 2, 2; no media | the same |

The building `retroverse-cli worldplan generate` makes of it (I4 step 4, ADR-0013 §3: an arcade system takes a section
of the arcade room, a system under 30 distinct titles over all its games a section of its floor's shared room, both of
the generic case without a console): the floors ground, gen4, gen5, handheld and computer; kodi left out (excluded);
the rooms and their placements: area:arcade 15 (ground, mame, its 10 sets without a cover `no-cover`),
area:shared:computer 20 (amiga's 20 titles, generated labels), area:shared:handheld 14 (the sections pocketlab 6, by
gb's row entering first, and pokemini 8), room:gb 30 (handheld, cartridge_handheld), room:psx 40 (gen5, disc_case_cd),
room:snes 40 (gen4, cartridge_home_16bit). After `additions/` and a new import, an update of snes, psx and pokemini
(the shared room with both its sections) places 34 titles and moves nothing, pokemini past 30 titles staying in its
section; an update of the whole building then enters megadrive (I4 step 6a): its 12 titles, under 30, open gen4's
shared room, `area:shared:gen4`, in the south row, and nothing that stands moves. (Until step 4 every system had its
own console room: room:amiga, room:mame, room:pocketlab and room:pokemini.)
