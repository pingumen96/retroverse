# The `multi-disc` ES-DE fixture

Cue sheets and playlists as the ROM scan (I2 step 7) groups them into disc groups (ADR-0008 §3): a file a `.cue` or
`.m3u` references is a disc member of it when that sheet is itself a game of the same system. Chains (a playlist naming
cue sheets that name tracks), a cycle, a sheet that cannot be read, names matched with the key's case folding, a
reference through `..\`, a track two sheets share, a reference to nothing, a head whose metadata is a member's,
RetroArch's disc labels in a playlist, and the ps2 case where `.cue` is not a configured extension. Invented titles; the
layout of the `standard` fixture; tracks and the oversized sheet generated from `files.json`; default settings.

The test writes one more sheet before it loads, in Windows-1252 (hostile encodings are generated at test time, ADR-0008
§10): `roms/psx/Café Noir (France).cue`, whose `FILE "Café Noir (France).bin" BINARY` holds the byte E9 for `é`. The
counts below include it.

## Systems

| System | Extensions |
|---|---|
| psx | `.bin .BIN .chd .CHD .cue .CUE .m3u .M3U` |
| ps2 | `.bin .BIN .chd .CHD .iso .ISO` (no `.cue`, as on the reference machine) |

## psx

| Head | Kind | References | Members |
|---|---|---|---|
| `Twin Peak (Europe).cue` (gamelist) | cue | its two tracks | `Twin Peak (Europe) (Track 1).bin`, `(Track 2).bin` |
| `Saga (USA).m3u` (gamelist) | m3u | the two disc sheets (comments and a blank line around them) | `Saga (USA) (Disc 1).cue`, `(Disc 2).cue` |
| `Saga (USA) (Disc 1).cue`, `(Disc 2).cue` | cue | their track | `Saga (USA) (Disc 1).bin`, `(Disc 2).bin`: members of members (head: the cue) |
| `Loop A (World).m3u` | m3u | `Loop B (World).m3u` | `Loop B (World).m3u` |
| `Loop B (World).m3u` | m3u | `Loop A (World).m3u` (dropped: a cycle, `disc-group-cycle`), `Loop B (World).bin` | `Loop B (World).bin` |
| `Huge Sheet (USA).cue` | cue | none: 1,100,000 bytes, larger than a sheet is read (`disc-sheet-unreadable`) | none: `Huge Sheet (USA).bin` stays a game |
| `Café Noir (France).cue` (written by the test) | cue | `Café Noir (France).bin`, read as Windows-1252 | `Café Noir (France).bin` |
| `Hollow Vale (Italy) (Disc 1).cue` (no gamelist entry) | cue | its track | `Hollow Vale (Italy) (Disc 1).bin`, which has the gamelist entry: the cue shows its metadata ("Hollow Vale") |
| `Quiet Storm (USA).cue` | cue | `quiet storm (usa).bin` (another case) | `Quiet Storm (USA).bin` |
| `Discs/Echo (Japan).cue` | cue | `..\Tracks\Echo (Japan).bin` | `Tracks/Echo (Japan).bin` |
| `Shared A (USA).cue` | cue | `Shared (USA) (Track 1).bin`, `Shared A (USA) (Track 2).bin` | both; Track 1's head is Shared A (the first in key order) |
| `Shared B (USA).cue` | cue | `Shared (USA) (Track 1).bin` | the same Track 1 |
| `Broken Link (USA).cue` | cue | `Broken Link (USA).bin`, which is not there (`disc-reference-missing`) | none |
| `Plain Words (USA).cue` | cue | `FILE Plain Words (USA).bin BINARY`, unquoted | `Plain Words (USA).bin` |
| `Rift (USA).m3u` (no gamelist entry) | m3u | `Rift (USA).cue` | `Rift (USA).cue`, itself the head of `Rift (USA).bin`, which has the gamelist entry: the cue shows "Rift", then the playlist |
| `Labels (USA).m3u` | m3u | `Labels (USA) (Disc 1).chd\|Disc One`, `Labels (USA) (Disc 2).chd \| Disc Two` (RetroArch's disc labels) | `Labels (USA) (Disc 1).chd`, `(Disc 2).chd` |

Also `Solo (USA).chd`, a game alone, and `./Lost Saga (USA).m3u`, a gamelist entry whose file is gone.

ES-DE's play data, as ES-DE records it on the files it lists (every track is a game there): a head adds that of the
members shown under it to its own. `Twin Peak (Europe).cue` (2 plays, 600 s, last played 2026-01-01) and its Track 1
(a favourite, 3 plays, 900 s, last played 2026-03-01) give the cue sheet a favourite, 5 plays, 1,500 s, last played
2026-03-01; `Rift (USA).bin` (1 play, last played 2025-05-05) gives it to its cue sheet and from there to the playlist;
`Shared (USA) (Track 1).bin` (4 plays, no name) gives its plays to `Shared A (USA).cue`, the head it is shown under, and
none to `Shared B (USA).cue`. The members keep their own.

| | psx | ps2 |
|---|---|---|
| Entries | 36 | 3 |
| Games | 16 | 2 |
| Disc members | 19 | 0 |
| Missing files | 1 | 0 |
| Not a game: extension not configured | 0 | 1 (`Moon Race (Europe).cue`, a gamelist entry) |
| Disc groups | 16 | 0 |

ES-DE itself lists the 35 psx files that exist (games and members alike) and 2 ps2 files (`Moon Race (Europe).bin`,
`Star Hop (USA).iso`).

## The cache (LibraryLoadOptions::discGroups)

A test hands the snapshot's groups back as the cache of the next load: unchanged heads are not read again. With a
cached Twin Peak group whose recorded size differs and the sheet held open without sharing, the sheet cannot be read and
the cached group stands (`disc-sheet-unreadable`, "the group read before is kept").
