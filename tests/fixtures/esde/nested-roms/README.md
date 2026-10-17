# The `nested-roms` ES-DE fixture

ROM folders laid out the ways the ROM scan (I2 step 7) must follow ES-DE through: namesakes in sub-folders, deep
nesting, folders named with dots, directories that are games (ES-DE lists them and never looks inside), files without an
extension where a system configures `.`, and gamelist entries that name folders, games and nothing. Invented titles;
the layout of the `standard` fixture (`ES-DE/` the ES-DE home, `Programs/ES-DE/` the install folder, `roms/` the ROM
root); payloads generated from `files.json`. Defaults for every setting.

## Systems

| System | Extensions |
|---|---|
| nes | `.nes .NES .zip .ZIP` |
| ps3 | `.iso .ISO .ps3 .PS3` |
| xbox360 | `.iso .ISO .xex .XEX .` (`.` matches files without an extension, and names that end with a dot) |

## ROM tree and gamelists

| Path under `roms/` | State | Gamelist | What it exercises |
|---|---|---|---|
| `nes/Hero Quest (USA).nes` | game | no | three byte-identical namesakes (size, seed, time) in three folders |
| `nes/Shelf A/Hero Quest (USA).nes` | game | no | the second namesake, titled from its stem like the first |
| `nes/Shelf B/Hero Quest (USA).nes` | game | yes ("Hero Quest") | the third, the only one with metadata |
| `nes/Shelf A` | folder | `<folder>` | a folder entry |
| `nes/Shelf B` | no entry | a `<game>` entry | a `<game>` entry naming a folder ES-DE enters: skipped (`gamelist-kind-mismatch`) |
| `nes/L1/L2/L3/L4/L5/L6/Deep Diver (World).nes` | game | yes | six levels of sub-folders |
| `nes/L1/L2` | folder | `<folder>`, `<hidden>` | a folder entry deep in the tree, with no game directly inside; hidden, which counts only with `ShowHiddenGames` off |
| `./L1/L2/Lost Level (USA).nes` | missing file | yes | a missing file deep in the tree |
| `nes/Vol. 1.5/Point Five (Europe).NES` | game | no | a folder whose name holds dots (its extension `.5` is none of nes', so it is entered) |
| `nes/Collection.zip` | game (a directory) | yes ("Collection Box") | a directory named with a configured extension: one game, size 0, never entered |
| `nes/Collection.zip/Inside One (USA).nes`, `Inside Two (USA).nes` | no entry | `Inside One` only | never listed; the gamelist entry is skipped (`gamelist-entry-inside-game`) |
| `./Collection.zip` (`<folder>`) | | `<folder>` | a `<folder>` entry naming a game directory: skipped (`gamelist-kind-mismatch`) |
| `nes/Empty Shelf` | folder | `<folder>` | an empty folder with a folder entry: kept (ES-DE would not show a folder without games) |
| `./Gone Shelf` | | `<folder>` | a folder entry naming no folder: skipped (`gamelist-folder-not-found`) |
| `nes/.nes` | no entry | no | a file whose stem is empty: ES-DE skips it "as it has no filename" (`rom-name-empty`) |
| `nes/Readme.txt` | no entry | no | an extension nes does not configure |
| `ps3/Space Odyssey (Europe).ps3` | game (a directory) | no | a game directory holding a PS3 tree; titled "Space Odyssey (Europe)" |
| `ps3/Puzzle Box (USA).iso` | game | no | a plain file beside it |
| `ps3/Tools/Helper.ps3` | game (a directory) | no | a game directory inside an ordinary folder; titled "Helper" |
| `xbox360/default` | game | no | a file without an extension, with `.` configured |
| `xbox360/Ring World` | no entry | no | a directory without an extension: never a game, even with `.` configured; entered |
| `xbox360/Ring World/Ring World.iso`, `content` | game | no | its two games |
| `xbox360/notes.txt` | no entry | no | an extension xbox360 does not configure |
| `extra/Linked Game (USA).nes` | | | outside the ROM root: reached only through the junction a test makes |

## Expected counts

| | nes | ps3 | xbox360 |
|---|---|---|---|
| Entries | 10 | 3 | 3 |
| Games | 6 | 3 | 3 |
| Missing files | 1 | 0 | 0 |
| Folders | 3 | 0 | 0 |
| Gamelist | read | absent | absent |

Issues: `gamelist-duplicate-path` (the gamelist names `./Collection.zip` as a game and as a folder: both kept, the scan
uses the one that matches the disk), `rom-name-empty` (`nes/.nes`), `gamelist-kind-mismatch` twice (`./Shelf B`, the
`<folder>` `./Collection.zip`), `gamelist-entry-inside-game` (`./Collection.zip/Inside One (USA).nes`),
`gamelist-folder-not-found` (`./Gone Shelf`).

## Situations the tests make

- `xbox360/Odd Name.`, a name that ends with a dot, written through its `\\?\` form (the materializer's paths would
  drop the dot, as Windows' plain paths do): its extension is `.`, so it is one more xbox360 game, titled "Odd Name".
- `nes` removed: the system folder is missing; the gamelist's entries are kept, source unavailable, one per key, with
  the state the gamelist alone tells (`./Shelf B` names no configured extension: not a game).
- A junction `roms/nes/Loop` leading back to `roms/nes`: not followed, as ES-DE skips a recursive link (`rom-link-loop`,
  a warning); no unavailable folder, nothing else changes. A `<game>` entry the test adds for
  `./Loop/Hero Quest (USA).nes` names nothing listed: a missing file.
- A junction `roms/nes/Linked` leading to `extra`: followed, `Linked/Linked Game (USA).nes` is one more game.
- `roms/nes/L1/L2` made unlistable: an unavailable folder; Deep Diver and Lost Level are then games "source
  unavailable" (from the gamelist alone), never missing; the `L1/L2` folder entry stays a folder (its parent lists it).
- `ShowHiddenGames` off: ES-DE deletes the folder `L1/L2`, whose `<folder>` entry is `<hidden>`, with everything below
  it. `L1/L2` and Deep Diver are not games (`ShowHiddenGames`), Lost Level stays a missing file; nes then has 10
  entries, 5 games, 1 missing file, 2 not games, 2 folders, and ES-DE lists 5 games. So it is too with `L1/L2`
  unlistable (Deep Diver and Lost Level, from the gamelist alone, are not games) and with `nes` removed. The entry
  spelled `./l1/l2` matches the folder by key (the folder is not a game), but ES-DE applies only the spelling on disk:
  Deep Diver stays a game.
