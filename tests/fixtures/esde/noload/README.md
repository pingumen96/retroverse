# The `noload` ES-DE fixture

What keeps ES-DE from listing files it would otherwise show, as the ROM scan (I2 step 7) reports it (ADR-0008 §3): a
`noload.txt` in a sub-folder or in the system folder, hidden folders with `ShowHiddenFiles` off, and
`ParseGamelistOnly`. Such files stay entries, "not a game" with the reason. The layout of the `standard` fixture; ROM
payloads generated from `files.json`; default settings as committed (the tests turn settings off with
`Fixture::replaceText`).

ES-DE 3.4.1's own rules, followed here:
- a folder holding `noload.txt` (`Utils::FileSystem::exists`, so any case) is not entered: nothing below it is a game,
  even what its gamelist names (ES-DE stops at the folder it marked noload);
- in the system folder, ES-DE looks for `noload.txt` in the folder's listing, spelled exactly so: then it does not load
  the system at all; `NOLOAD.TXT` there does nothing;
- a hidden folder (here a leading dot) with `ShowHiddenFiles` off is not entered, but a gamelist entry is skipped only
  when its file or the folder holding it is hidden: ES-DE creates the folders above that it skipped, so an entry two
  levels below a hidden folder shows;
- with `ParseGamelistOnly` on ES-DE lists only what its gamelists name, and neither scans nor looks for `noload.txt`.

## ROM tree and gamelists

| Path under `roms/` | Gamelist | As committed | `ShowHiddenFiles` off | `ParseGamelistOnly` on |
|---|---|---|---|---|
| `gba/Keep (USA).gba` | no | game | game | not a game (ParseGamelistOnly) |
| `gba/Skip Me` (holds `noload.txt`) | `<folder>` | not a game (noload) | not a game (noload) | folder |
| `gba/Skip Me/Hidden Away (USA).gba` | yes | not a game (noload) | not a game (noload) | game |
| `gba/Skip Me/Deeper/Lost (USA).gba` | no | not a game (noload) | not a game (noload) | not a game (ParseGamelistOnly) |
| `gba/Upper/Shouting (USA).gba` (`Upper` holds `NOLOAD.TXT`) | no | not a game (noload) | not a game (noload) | not a game (ParseGamelistOnly) |
| `gba/.secret/Sneaky (USA).gba` | no | game | not a game (ShowHiddenFiles) | not a game (ParseGamelistOnly) |
| `gba/.secret/Sneaky Two (USA).gba` | yes | game | not a game (ShowHiddenFiles: its folder is hidden) | game |
| `gba/.secret/Inner/Found (USA).gba` | yes | game | game (only its grandparent is hidden) | game |
| `gba/.secret/Inner/Unlisted (USA).gba` | no | game | not a game (ShowHiddenFiles) | not a game (ParseGamelistOnly) |
| `n64/Sky Pilot (USA).z64` (`n64` holds `noload.txt`) | yes | not a game (noload) | not a game (noload) | game |
| `n64/Tiny Wings (Japan).n64` | no | not a game (noload) | not a game (noload) | not a game (ParseGamelistOnly) |
| `pcengine/Bounce Kid (Japan).pce` (`pcengine` holds `NOLOAD.TXT`) | no | game | game | not a game (ParseGamelistOnly) |

The `noload.txt` files themselves are no entries (`.txt` is no extension of these systems).

| Settings | gba games, not games, folders | n64 games, not games | pcengine games |
|---|---|---|---|
| as committed | 5, 4, 0 | 0, 2 | 1 |
| `ShowHiddenFiles` off | 2, 7, 0 | 0, 2 | 1 |
| `ParseGamelistOnly` on | 3, 5, 1 | 1, 1 | 0 (1 not a game) |

Entries: gba 9, n64 2, pcengine 1, whatever the settings. Issues as committed and with `ShowHiddenFiles` off:
`system-not-loaded` for n64 (its `noload.txt`); none with `ParseGamelistOnly` on.
