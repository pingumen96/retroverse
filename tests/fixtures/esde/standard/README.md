# The `standard` ES-DE fixture

A small, original ES-DE installation (invented titles, companies and emulators; ES-DE's own system names and file
formats) shared by the I2 steps that read ES-DE: configuration, gamelists, the ROM scan, media, launch translation and
the import. `rv::test::Fixture::materialize("esde/standard")` copies it under the build tree, writes the fixture's
absolute path where `{{FIXTURE_ROOT}}` stands and generates what `files.json` lists. The expectations below are what the
later steps assert. Steps that need other situations add fixture folders of their own beside this one, so that steps
running in parallel never edit the same files; this one only grows additively (new files, and the counts below updated
with them).

## Layout

```text
<fixture root>                       the user's home folder of the fixture (%USERPROFILE%): ES-DE's `~`
├── ES-DE/                           the ES-DE home, at its default place under the home folder
│   ├── settings/es_settings.xml
│   ├── gamelists/gb/gamelist.xml
│   ├── gamelists/snes/gamelist.xml
│   ├── gamelists/psx/gamelist.xml
│   └── custom_systems/              empty, as on the reference machine
├── Emulators/HomeEmulator/
│   └── home-emulator.exe            a copy of rv_fake_emulator.exe, found only through `~`
├── Programs/ES-DE/                  the install folder (%ESPATH%)
│   ├── ES-DE.exe                    a 4 KiB placeholder, never run
│   ├── resources/systems/windows/es_systems.xml
│   ├── resources/systems/windows/es_find_rules.xml
│   └── Emulators/FakeEmulator/
│       ├── fake-emulator.exe        a copy of rv_fake_emulator.exe
│       └── cores/                   fake_gb_libretro.dll, fake_psx_libretro.dll (placeholders)
├── roms/                            ROMDirectory
└── media/                           MediaDirectory
```

The copies of the fake emulator find the DLLs they need (SDL3, fmt) only through PATH: a test that starts one puts
`rv::test::testProgramsDir()` on the PATH it passes.

## Settings (`ES-DE/settings/es_settings.xml`)

Sibling elements and no root element, sorted by type then name, as ES-DE writes them. `ROMDirectory` is
`{{FIXTURE_ROOT}}\roms` (backslashes) and `MediaDirectory` `{{FIXTURE_ROOT}}/media` (forward slashes), the mix of the
reference machine. `ShowHiddenFiles` true, `ShowHiddenGames` true, `ParseGamelistOnly` false,
`LegacyGamelistFileLocation` false, `AlternativeEmulatorPerGame` true. `ScraperUsernameScreenScraper` holds
`fixture-scraper-user-Q7x4` and `ScraperPasswordScreenScraper` holds `fixture-scraper-password-K9w2`: neither value may
ever appear in a log, the database, the CLI's output or any file Retroverse writes.

Variants are made from the materialized copy, never from a copy of this folder: `Fixture::replaceText` turns a setting
off, for example `fixture.replaceText("ES-DE/settings/es_settings.xml", R"(name="ShowHiddenGames" value="true")",
R"(name="ShowHiddenGames" value="false")")`. Their expected counts are under "Variants" below.

## Systems (`es_systems.xml`) and find rules (`es_find_rules.xml`)

| System | Full name | Extensions | Commands (label: what it exercises) |
|---|---|---|---|
| gb | Nintendo Game Boy | `.gb .GB .gbc .GBC .zip .ZIP` | Fake Emulator: `%EMULATOR_FAKE-EMULATOR%`, `%CORE_FAKE-EMULATOR%` (core present), `%ROM%`; Fake Emulator (Game Folder): `%STARTDIR%=%GAMEDIR%`; Missing Emulator (Standalone): an emulator no rule finds; Home Emulator: an emulator found only under `~` |
| snes | Nintendo SNES (Super Nintendo) | `.sfc .SFC .smc .SMC .zip .ZIP` | Fake Emulator; Fake Emulator (Hidden Console): `%HIDEWINDOW%`, `%STARTDIR%=%EMUDIR%`; Shortcut or script: `%EMULATOR_OS-SHELL%` (cmd.exe, refused by name), `%ESCAPESPECIALS%` |
| psx | Sony PlayStation | `.bin .BIN .chd .CHD .cue CUE .m3u .M3U` | Fake Emulator: core present; Fake Emulator (Inject): `%INJECT%=%BASENAME%.commands`; Fake Emulator (Missing Core): a core that is not there |

Platforms: gb `gb`; snes `snes, satellaview` (a comma list); psx `psx`. Themes equal the names. psx carries the pcfx
quirk of ES-DE's own file: `CUE` without its dot, which matches no file (`.CUE` files are no games).

Every fake command passes its ES-DE arguments after `--`, which the fake emulator echoes and never interprets. The
gamelist of snes selects "Fake Emulator (Hidden Console)" with `<alternativeEmulator>` (the snapshot's
`SystemInfo::alternativeEmulator` of snes; empty for gb and psx); the gb game Café Racer selects "Fake Emulator (Game
Folder)" with `<altemulator>`. Find rules: `FAKE-EMULATOR` has an App Paths rule and a PATH rule that find nothing on a
normal machine, then static paths `~\AppData\Roaming\FakeEmulator\fake-emulator.exe` (absent),
`%ESPATH%\Emulators\FakeEmulator\fake-emulator.exe` (the copy) and `%ESPATH%\..\Emulators\...` (absent); its core rule
is `%EMUPATH%\cores`. `OS-SHELL` is `cmd.exe` on PATH. `MISSING-EMULATOR` is never found. `HOME-EMULATOR` has one
static path, `~\Emulators\HomeEmulator\home-emulator.exe`: `~` is ES-DE's home folder, the fixture root (the parent of
the ES-DE home, `LibraryProfile::esdeHome`), where the copy is; read as the ES-DE home itself, `~` finds nothing.

Launch expectations: gb's default command is ready, and so is Café Racer's alternative; gb's Home Emulator command is
ready (found under `~`); snes games use the hidden console command (ready); psx's default is ready; with the inject
command, Solo Disc requires launcher configuration (its `Solo Disc (USA).commands` is usable) while Twin Tracks drops
the inject element (no such file); the missing core gives "core not found", the missing emulator "emulator not found",
the shell command "requires launcher configuration".

## ROM tree (`roms/`) and gamelists

Generated payloads (`files.json`), with fixed sizes, seeds and last write times; the committed text files
(`Notes.txt`, the legacy gamelist, the two cue sheets, the inject file) get the time of the copy.

| Path under `roms/` | State | Gamelist | What it exercises |
|---|---|---|---|
| `gb/Tiny Tennis (Europe).gb` | game | full metadata | every field: rating 0.7, release date, players `1-2`, favorite, playcount 3, playtime 1825, lastplayed, a blank line and `&amp;` in the description |
| `gb/Collection A` | folder | `<folder>` | a folder entry (path, name, lastplayed) |
| `gb/Collection A/Pocket Puzzle (World).gb` | game | yes | a year-only date (`19900101T000000`), players `1`, the unknown element `<region>` |
| `gb/Collection B/Pocket Puzzle (World).gb` | game | no | a namesake in another sub-folder, byte-identical to the one in Collection A (size, seed and time) |
| `gb/Café Racer (Europe).gb` | game | yes | a non-ASCII name and title, a trailing space in `<publisher>`, a comma inside a genre, players `2-16`, `<altemulator>` |
| `gb/.Dot Hero (World).gb` | game | no | a leading-dot name (a game: ShowHiddenFiles) |
| `gb/Hidden Gem (World).gb` | game | no | the hidden attribute (a game: ShowHiddenFiles) |
| `gb/LOUD CART (USA).GB` | game | no | an upper-case extension variant the system lists |
| `gb/Collection A/Deep/Bottom Dweller (Japan).gbc` | game | no | two levels of sub-folders; 192 KiB, more than the quick hash's two 64 KiB chunks |
| `gb/Notes.txt` | no entry | no | an extension gb does not configure |
| `gb/gamelist.xml` | no entry | no | a legacy gamelist inside the ROM folder (an older scraper's format) that names Tiny Tennis "Legacy Tennis": never read (`LegacyGamelistFileLocation` false), and `.xml` is no gb extension |
| `./Gone Fishing (USA).gb` (gb gamelist) | missing file | yes | a gamelist entry whose file is gone (its cover is still there) |
| `./../snes/Alpha Quest (USA).sfc` (gb gamelist, "Wrong Way") | outside the system folder | yes | a path that leads out of gb's folder: in the snapshot and its issues, never indexed |
| `snes/Alpha Quest (USA).sfc` | game | full metadata | rating `1`, players `8+`, kidgame, a trailing space in `<developer>`, a line feed ending `<publisher>`, a double-escaped `&amp;amp;` in the description; media of all 12 kinds |
| `snes/Beta Run (Europe).SMC` | game | no | an upper-case extension variant |
| `snes/Mixed Case (USA).Sfc` | not a game: extension not configured | yes | `.Sfc` is neither `.sfc` nor `.SFC`: ES-DE compares extensions with their case |
| `snes/Hacks` | folder | `<folder>` | a folder entry with a description and a cover |
| `snes/Hacks/Gamma Hack (Hack).sfc` | game | yes | players `2-3000`, rating 0.3 |
| `snes/Delta Drift (USA).sfc` | game | yes, as `./delta drift (usa).sfc` | a gamelist path that differs from the file's in case only: it matches the file (ADR-0008 §3), whose relative path stays as listed on disk; ES-DE shows a second entry for it |
| `snes/Sports Vol. 2` | folder | `<folder>`, named "Sports, Volume 2" | a folder whose name holds a dot: its cover is `Sports Vol. 2.png`, the whole name |
| `snes/Sports Vol. 2/Ice Lanes (USA).sfc` | game | no | a game in a folder whose name holds a dot |
| `snes/manual.pdf` | no entry | no | an extension snes does not configure |
| `psx/Twin Tracks (Europe).cue` | game, disc group head | full metadata | a cue sheet naming two tracks; players `1-4`, playtime 33552, lastplayed `20260906T190454` |
| `psx/Twin Tracks (Europe) (Track 1).bin` | disc member | no | a track of the cue sheet (70,560 bytes, 30 sectors of 2,352) |
| `psx/Twin Tracks (Europe) (Track 2).bin` | disc member | no | the second track (35,280 bytes) |
| `psx/Solo Disc (USA).chd` | game | yes | players `24`; `<hidden>` and `<completed>` true, `<broken>` written false (a game while ShowHiddenGames is true) |
| `psx/Solo Disc (USA).commands` | no entry | no | the inject file of "Fake Emulator (Inject)" for Solo Disc |
| `psx/Loud Disc (Japan).CUE` | no entry | no | the `CUE` quirk: not a game, so never read as a cue sheet (its track does not exist) |

## Expected counts

| | gb | snes | psx | All |
|---|---|---|---|---|
| Entries | 11 | 8 | 4 | 23 |
| Games | 8 | 5 | 2 | 15 |
| Missing files | 1 | 0 | 0 | 1 |
| Not a game: extension not configured | 0 | 1 | 0 | 1 |
| Not a game: other reasons (noload.txt, BIOS or device set, setting) | 0 | 0 | 0 | 0 |
| Disc members | 0 | 0 | 2 | 2 |
| Folders | 1 | 2 | 0 | 3 |
| Outside the system folder | 1 | 0 | 0 | 1 |
| Indexed entries (every entry but those outside their system folder) | 10 | 8 | 4 | 22 |
| Games without a gamelist entry (titled from their file stem) | 5 | 2 | 0 | 7 |
| Gamelist entries (`<game>` + `<folder>`) | 5 + 1 | 4 + 2 | 2 + 0 | 11 + 3 |
| Disc groups | 0 | 0 | 1 (cue, 2 members) | 1 |
| Files that are no entry | 2 | 1 | 2 | 5 |

ES-DE itself shows 18 games: the 15 above, the two tracks of Twin Tracks, and a second Delta Drift for its gamelist
entry, since ES-DE matches gamelist paths with their case. Titles of the games without a gamelist entry: "Pocket Puzzle
(World)" (Collection B), ".Dot Hero (World)", "Hidden Gem (World)", "LOUD CART (USA)", "Bottom Dweller (Japan)", "Beta
Run (Europe)", "Ice Lanes (USA)".

### Variants

Made with `Fixture::replaceText` on the materialized settings. The entries stay the same 23: what a setting excludes
becomes "not a game, excluded by a setting" with the setting's name (`CatalogEntry::excludingSetting`).

| Settings | Games (gb, snes, psx) | Not a game: excluded by a setting | Games without a gamelist entry | ES-DE shows |
|---|---|---|---|---|
| as committed | 8, 5, 2 | 0 | 7 | 18 |
| `ShowHiddenFiles` false | 6, 5, 2 | 2: gb's .Dot Hero and Hidden Gem, `ShowHiddenFiles` | 5 | 16 |
| `ShowHiddenGames` false | 8, 5, 1 | 1: psx's Solo Disc, `ShowHiddenGames` | 7 | 17 |
| both false | 6, 5, 1 | 3 | 5 | 15 |

## Media (`media/`)

25 files (`files.json`), random bytes rather than images or videos. What each entry has, by kind and extension:

| System | Entry | Media |
|---|---|---|
| gb | Tiny Tennis | covers `.png` (a `.jpg` beside it: png comes before jpg), screenshots `.jpg`, videos `.mp4` |
| gb | Pocket Puzzle in Collection A | covers `.png`, in `covers/Collection A/`; its namesake in Collection B has none |
| gb | Gone Fishing (missing file) | covers `.png` |
| gb | Café Racer | videos `.avi` |
| gb | LOUD CART | videos `.mkv` |
| snes | Alpha Quest | all 12 kinds: covers `.png`, backcovers `.png`, 3dboxes `.png`, physicalmedia `.png`, screenshots `.jpg`, titlescreens `.jpg`, marquees `.png`, miximages `.png`, fanart `.jpg`, manuals `.pdf`, videos `.mov`, custom `.png` |
| snes | Beta Run | none: its `.flv` is no video ES-DE reads (`.avi .mkv .mov .mp4 .wmv`) |
| snes | Hacks (folder) | covers `.png` |
| snes | Sports Vol. 2 (folder) | covers `.png`, named with the whole folder name (`Sports Vol. 2.png`, never `Sports Vol.png`) |
| psx | Twin Tracks | covers `.png`, videos `.wmv`; the tracks have none (their stems differ) |

No other entry has media.
