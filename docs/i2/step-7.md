# I2 step 7: the ROM scan and the library snapshot

Roadmap row: I2 step 7 (wave 2, needs 2 and 3). Decision: [ADR-0008](../decisions/ADR-0008-esde-catalog.md) §3, §4,
§7, §10. `rv::esde::EsdeLibrarySource` implements `catalog::ILibrarySource::load`: ES-DE's configuration, every
system's ROM folder listed with ES-DE's discovery rules, its gamelist merged in, the disc groups, into a
`catalog::LibrarySnapshot` whose entries carry their states. `retroverse-cli import --dry-run` prints it. This page is
folded into the shared documentation at the wave merge.

## What was built

| File | What it does |
|---|---|
| `rv/esde/mame_sets.hpp` | `loadMameLists()` / `readMameListFiles()`: `resources/MAME/mamenames.xml`, `mamebioses.xml`, `mamedevices.xml` where ES-DE finds them (`bundledResource()`), read as ES-DE's MameNames reads them; `MameSets` (`isBios`, `isDevice`, `realName`, exact lookups); `isArcadePlatform()`; `esdeRemoveParenthesis()` (ES-DE's `Utils::String::removeParenthesis`); `mameName()` (the name of an unscraped arcade game) |
| `rv/esde/rom_scanner.hpp` | `scanRomFolder()`: one system's ROM folder walked (`core::walkTree`, links followed unless they loop) into `ScannedItem`s with ES-DE's verdict (`ScanVerdict`: game, excluded with a `ScanExclusion`, empty name, extension not configured, folder) and the facts the gamelist rules need (`above`, `parentHidden`, `extensionConfigured`, `arcadeAsset`); `RomFolderListing` (items by key, unavailable folders, `noLoadAtRoot`, `systemFolderHidden`); `esdeExtension()`, `esdeStem()`, `esdeDefaultName()` |
| `rv/esde/disc_groups.hpp` | `discSheetKind()`, `decodeSheet()` (UTF-8, else Windows-1252), `cueSheetFiles()`, `m3uFiles()` (RetroArch's `<path>|<label>` lines give their path), `resolveSheetReference()`, `readDiscSheet()`; `groupDiscs()`: heads and their references into `catalog::DiscGroup`s without cycles, each member's head, the members-first order |
| `rv/esde/library_source.hpp` | `EsdeLibrarySource` (`EsdeLibrarySourceOptions`: the gamelist retry delay, a system filter; `lastStatistics()`: counts and times of the last load) |
| `tools/cli/src/cmd_import.cpp` | `retroverse-cli import --dry-run [--system <name>] [--json] [--output <file>]`; without `--dry-run`: "not implemented yet (I2 step 11)", exit 2 |
| `scripts/esde_census.py` | the independent oracle (below) |

Also, outside the step's stubs: `settings.hpp`/`settings.cpp` (step 2) read one more key, `MAMENameStripExtraInfo`
(`EsdeSettings::mameNameStripExtraInfo`, ES-DE's default true), and `test_esde_settings.cpp` lists it in the whitelist;
step 1's `rv/catalog/disc_group.hpp` gained `DiscGroup::references` (what a sheet says, which the cache keeps: below),
with the comments of `CachedDiscGroup` and `LibraryLoadOptions::discGroups` to match.

Tests: `tests/unit/esde/test_library_source.cpp` (the standard fixture, its variants, read-only, the system filter,
cancellation, the disc group cache, the gamelist retry, system fallbacks on `custom-systems`),
`test_rom_scan_fixtures.cpp` (the five new fixtures), `test_disc_groups.cpp`, `test_mame_sets.cpp`, the hidden
`test_library_source_real.cpp` (`[.real-library]`), `tests/unit/cli/test_cli_import.cpp`; helpers in
`tests/unit/esde/snapshot_test_support.{hpp,cpp}`. Fixtures (each with a README stating its expectations):
`tests/fixtures/esde/nested-roms`, `multi-disc`, `arcade`, `noload`, `long-path`; `standard` and `custom-systems` are
used read-only (variants made on the materialized copy).

## ES-DE 3.4.1's behaviour, as verified in its source

Read from the tag `v3.4.1` (`es-app/src/SystemData.cpp`, `GamelistFileParser.cpp`, `FileData.cpp`, `FileData.h`,
`es-core/src/MameNames.h`, `MameNames.cpp`, `utils/FileSystemUtil.cpp`, `utils/StringUtil.cpp`, `Settings.cpp`).

**Names.** `getExtension()` is the name from its last `.`, the dot included, and `.` when there is none (`name.` gives
`.` too); `getStem()` cuts a file's name at its last `.` and keeps a folder's whole name. `isHidden()` is the hidden
attribute or a leading dot.

**`SystemData::loadConfig`, per `<system>`:** the folder must exist and be a folder; with `ShowHiddenFiles` off a
hidden system folder skips the system; `populateFolder()`, then the gamelist; the system is kept when its root has
children and, with `ShowHiddenGames` off, not only hidden games. A name already kept is skipped, so a later definition
is used only when the earlier ones found no game.

**`SystemData::populateFolder`, per entry of a folder (`getDirContent`, sorted):**
1. In the system folder, a `noload.txt` found in the listing (exactly that name) makes `populateFolder` return false:
   neither the folder nor the gamelist is read, the system does not load.
2. Self-referencing links are skipped; with `ShowHiddenFiles` off a hidden file or folder is skipped (nothing below a
   hidden folder is seen). A recursive link to a folder (step 4) is skipped with a warning ("as it's a recursive
   symlink"), and the system loads.
3. A configured extension (verbatim, `std::find`), unless it is a directory whose extension is `.`: a GAME
   `FileData`. A name left empty is skipped "as it has no filename"; a directory's name loses its extension's length;
   an arcade asset is discarded (a directory that is one, or whose name is empty, falls through to the folder rules).
4. Other directories: a recursive link is skipped; a folder holding `noload.txt` (`Utils::FileSystem::exists`, so any
   case) becomes a FOLDER marked noload, not entered; otherwise it is populated and kept when it holds games.
5. `flatten.txt` in the system folder merges sub-folders into the root (presentation; ignored here, ADR-0008 §3).

**`GamelistFileParser::parseGamelist`, every `<game>` before every `<folder>`:** the path resolved; with
`ParseGamelistOnly` off an entry whose file does not exist is skipped ("does not exist"); with `ShowHiddenFiles` off an
entry whose file or parent folder is hidden is skipped (only those two); `findOrCreateFile()` walks the tree from the
root: at a node marked noload it stops (the entry is skipped), a missing folder is created (so an entry two levels below
a hidden folder shows), a missing `<folder>` is skipped, a missing game is checked against the extensions ("unsupported
extension") and created unless it is an arcade asset; a "Game/folder mismatch" is skipped; the metadata of a later
entry replaces an earlier one's; with `ShowHiddenGames` off a `<hidden>` entry is removed: for a `<folder>` entry the
folder with everything below it (`~FileData` deletes its children), so that a later entry of the folder finds nothing.
The tree holds the names as listed, so a `<folder>` entry spelled in another case finds no folder and is skipped. With
`ParseGamelistOnly` on,
`populateFolder` is not called at all: ES-DE lists what its gamelists name (noload.txt is never looked for).

**MAME (`FileData::isArcadeAsset`, `MameNames`).** In a system whose platforms include `arcade` or `neogeo` (and not
`ignore`, which replaces all the others), a file whose stem is in `mamebioses.xml` or `mamedevices.xml` (exact
`std::find`) is no game. The default name of a game there is `MameNames::getCleanName(removeParenthesis(stem))`: the
`<realname>` of that set (an `unordered_map`, the last of a repeated name; the set's own name when the real name is
missing or empty), then, with `MAMENameStripExtraInfo` (default true), `removeParenthesis` of it. The lists are read
names first, then BIOS sets, then devices, and a list missing or unparseable ends the reading (a missing
`mamenames.xml` leaves no BIOS or device set). Texts are never trimmed.

## What a game is (the rules of `EsdeLibrarySource`)

For every listed file or folder, and every gamelist entry, in this order (the first that applies):

| Listed item | With a gamelist entry of its kind | Without one |
|---|---|---|
| system folder hidden, `ShowHiddenFiles` off | not a game (`ShowHiddenFiles`) | not a game (`ShowHiddenFiles`) |
| `ParseGamelistOnly` on | the rules below, noload.txt ignored | not a game (`ParseGamelistOnly`) |
| `noload.txt` in the system folder | not a game (noload) | not a game (noload) |
| the file or its folder hidden (`ShowHiddenFiles` off) | not a game (`ShowHiddenFiles`) | |
| a folder above it not entered | noload when the outermost such folder holds noload.txt; a hidden folder further up does not count (ES-DE creates the folders) | not a game, that folder's reason (`ShowHiddenFiles` or noload) |
| the item itself hidden | | not a game (`ShowHiddenFiles`) |
| extension not configured | not a game (extension not configured) | no entry |
| ES-DE's name empty | (a game: the gamelist creates it) | no entry, `rom-name-empty` |
| a MAME BIOS or device set | not a game (BIOS or device set) | not a game (BIOS or device set) |
| `<hidden>`, `ShowHiddenGames` off | not a game (`ShowHiddenGames`) | |
| below a folder whose `<folder>` entry ES-DE applies with `<hidden>`, `ShowHiddenGames` off | not a game (`ShowHiddenGames`) | not a game (`ShowHiddenGames`) |
| otherwise | game, with the entry's metadata | game, titled `esdeDefaultName()` |

A folder ES-DE enters is an entry only with a `<folder>` entry: a Folder, or not a game for the same reasons (hidden,
noload, `<hidden>`). An entry of the other kind than the item (a `<game>` naming a folder ES-DE enters, a `<folder>`
naming a file or a game directory) is skipped with `gamelist-kind-mismatch`.

A hidden folder hides its tree, as ES-DE deletes it with everything below: with `ShowHiddenGames` off, a `<folder>`
entry with `<hidden>` that ES-DE applies (spelled as the folder is on disk; not skipped for a hidden file or parent with
`ShowHiddenFiles` off; not at or below a noload folder unless `ParseGamelistOnly`; not in a system ES-DE does not load)
makes every Game and Folder below the folder not a game (`ShowHiddenGames`); entries excluded for another reason keep
it, missing files stay missing (ES-DE skips their entries before, as files that do not exist). Below a folder that
cannot be listed, or in a system folder that is not available, the gamelist alone tells: a `<hidden>` `<folder>` entry
(not dotted with `ShowHiddenFiles` off) hides its tree likewise. A `<folder>` entry spelled in another case still gives
the folder its metadata, matched by key (the folder is then not a game), but hides nothing below it, as in ES-DE.

Gamelist entries that name nothing listed, one key at a time: outside the system folder → `OutsideSystemFolder`; below
a folder that could not be listed → `SourceUnavailable` with what the gamelist alone tells (the name's extension, a BIOS
name, a leading dot, `<hidden>`); inside a game directory → skipped, `gamelist-entry-inside-game`; otherwise a `<game>`
→ `MissingFile` (the last entry of the key; its extension does not matter: ES-DE's existence check comes first), a
`<folder>` → skipped, `gamelist-folder-not-found`. A system folder that is missing or cannot be listed keeps every
gamelist entry as `SourceUnavailable`, one per key (the last `<game>` entry, else the last `<folder>`).

Titles: the gamelist's `<name>` (`displayText()`), else `esdeDefaultName()`: the stem (`titleFromFileStem()`), a game
directory's name without its extension, a folder's whole name, an arcade game's MAME name.

Systems: every system of the configuration is in the snapshot. For a name with several definitions (a custom system and
the bundled one), the first whose folder gives a game (games or disc members on disk, not what a gamelist alone tells of
below a folder that cannot be listed) is used, as ES-DE loads it (`system-fallback-used`); the first when none does.

## Disc groups

- Heads: the entries in state Game that are files named `.cue` or `.m3u` (any case; `CUE` without its dot is no
  extension, so such a file is no game and never a head).
- References: a cue sheet's FILE names, a playlist's lines but comments (of a line `<path>|<label>`, RetroArch's disc
  label, the path before the first `|`, trimmed); resolved against the sheet's folder (`..` allowed within the system
  folder, `\` or `/`, an absolute path inside the system folder), matched with the key's case folding (the file spelled
  like the reference first); each key once, kept in the group as `DiscGroup::references` whether or not it names
  something there or a game. A reference that names no file or folder of the system is `disc-reference-missing`
  (reported on every load, read or cached, while the file is not there), one out of the system folder or malformed
  `disc-reference-invalid` (reported when the sheet is read, and not kept).
- Members: picked from the references on every load: the referenced entries in state Game (before grouping), the head
  excluded, each once. Not games (a hidden track with `ShowHiddenFiles` off), missing files and files whose extension is
  not configured are not members, until they are.
- Chains and cycles: groups per head; heads walked in key order, depth first; a reference back to a head on the path is
  dropped (`disc-group-cycle`), so a cycle keeps its first head as a game. A member two heads reference is in both
  groups and shown under the first head in key order (`CatalogEntry::discHead`).
- Metadata: a head without a gamelist entry takes the metadata (and so the title) of its first member that has some,
  members first along a chain; its raw values and gamelist path stay its own (empty).
- ES-DE's play data: every head adds to its flags those of the members shown under it (`CatalogEntry::discHead`),
  members first along a chain: play counts and play times summed, the latest last played time, a favourite if any
  member is. ES-DE lists each track as a game, so a game launched from a track has its play data on the track's entry
  (on the reference library 8 members: 7 psx tracks, among them `Final Fantasy IX (Italy) (Disc 1).bin` played twice,
  and saturn's Sega Rally track, played 3 times). The members keep their own (a cue sheet in a playlist with its
  tracks' added): a group's play data is its head's alone, never to be summed with its members' again. `kidGame`,
  `completed`, `broken` and `hidden` stay the head's own.
- Reading: sheets up to 1 MiB, decoded as UTF-8 with a Windows-1252 fallback, read by 8 readers at once (on the
  reference machine's external hard drive four times faster per file than one by one: 3.7 ms against 16.5 ms per cold
  file, measured on ps2's cue sheets). The cache (`LibraryLoadOptions::discGroups`) keeps what a sheet says, its
  references, never the members they gave: a head whose key, size and time match its cached group is not read, and its
  cached references give the members the listing gives now (a track missing or hidden when the sheet was read joins
  the group once it is there, without the sheet being read again); a head that cannot be read keeps its cached
  references (`disc-sheet-unreadable`, "the group read before is kept"), without them its files stay what the listing
  makes them; a head below a folder that cannot be listed keeps its cached references without an issue (the folder's
  own issue says why). A head that gives no member makes no group, so it is not cached and is read on every load.

## Availability

`SystemInfo::availability` comes from `systemInfos()` (`checkFolder`), then from the walk: a folder that went away or
cannot be listed after all becomes `SourceUnavailable` (`FolderMissing`, `NotListable`). A folder below the system
folder that cannot be listed (the walker's Error entry after a Directory entry: access denied, an I/O error, a link
whose target cannot be opened) is a `catalog::UnavailableFolder` (`NotListable`; `FolderMissing` when it vanished),
with `rom-folder-unavailable`; its entries are `SourceUnavailable`, never missing. A link that loops (the walker's
`Unsupported` Error entry) is not one: it is not followed, as ES-DE skips a recursive link with a warning
(`rom-link-loop`, a warning, so the dry run does not fail on it); what it leads to is listed by its own path, and a
gamelist entry naming a path through it names nothing listed (a missing file, reconciled as any other).

## Issue codes

| Code | Severity | Reported by |
|---|---|---|
| `rom-folder-unavailable` | error | the scan: a folder below the system folder that cannot be listed |
| `rom-link-loop` | warning | the scan: a link to a folder that leads back to a folder above it, not followed |
| `rom-entry-unreadable` | warning | the scan: a name without UTF-8 form, a file link whose target cannot be opened |
| `rom-name-unsupported` | warning | the scan: a name the catalog cannot key |
| `rom-name-empty` | warning | the scan: a would-be game ES-DE skips "as it has no filename" |
| `mame-list-missing`, `mame-list-unreadable` | warning | the MAME lists |
| `disc-sheet-unreadable`, `disc-reference-missing`, `disc-reference-invalid`, `disc-group-cycle` | warning | the disc groups |
| `system-folder-unavailable` | error | a system folder that exists but cannot be reached or listed |
| `system-folder-missing` | warning | a missing system folder whose gamelist names entries |
| `gamelist-failed` | error | a gamelist that still fails after the delay (`GamelistState::Failed`) |
| `gamelist-kind-mismatch`, `gamelist-folder-not-found`, `gamelist-entry-inside-game` | warning | the merge |
| `gamelist-location-unsupported` | warning | `LegacyGamelistFileLocation` on |
| `system-not-loaded` | info | a hidden system folder (`ShowHiddenFiles` off) or a `noload.txt` in it |
| `system-fallback-used` | info | a later definition of a system name was used |

Plus those of the parts: settings, roots, systems (`extension-without-dot`, ...), gamelists (`gamelist-*`, step 3).
Issues of one code are listed up to 100 per system, the others counted into one more issue of the code.

## CLI

```text
retroverse-cli [global options] import --dry-run [--system <name>] [--json] [--output <file>]
```

Reads the library `<user data>/config/library.json` records (`retroverse-cli library create` writes it) and prints a
table: per system its entries, games, missing files, not games, disc members, folders, entries outside the system
folder, entries source unavailable, disc groups and issues, with its folder's availability and its gamelist's state
(systems with entries, issues or a folder that cannot be read); the totals, "games as ES-DE lists them" (the games and
disc members on disk; `esdeGames` in JSON), the not-a-game reasons, the folders that cannot be listed, the issues and the
time. `--json` gives every system of the configuration
(`systems`, `totals`, `unavailableFolders`, `issues`, `statistics`). Every sheet that is a game is read (no database to
take cached groups from: step 11 passes them). It writes nothing but its log file (the test compares the fixture's
tree and the user data directory before and after). Exit codes: 0; 1 when an error issue was met; 2 for invalid
options, without `--dry-run` ("not implemented yet (I2 step 11)"), without library.json and for an unknown system; 3
when library.json or the library cannot be read, or `--output` cannot be written.

## The census (`scripts/esde_census.py`)

```text
python -I scripts/esde_census.py --esde-home <ES-DE home> [--esde-install <folder>] [--rom-root <folder>]
                                 [--system <name>] [--json] [--output <file>]
```

Standard library only, read-only, no machine paths inside: a port of the survey's discovery emulation (`scan_tree.py`,
`rom_analysis.py`, `rom_details.py`, which reproduced ES-DE's own "Total game count") with the rules above and the disc
grouping, written apart from the C++. It prints the table the hidden test prints: per system ES-DE's count (games and
disc members on disk, plus ES-DE's second entries for gamelist paths that differ from their file in case only),
entries, games, missing, not games, members, folders, outside, groups, unavailable folders, then the totals and the
not-a-game reasons.

On the reference machine its table and the test's are byte for byte the same (195 lines, checked again after the
review fixes). It also agrees with the C++ on every fixture of this step and their variants: after the review, 16
copies made as the tests make them (nested-roms as committed, with `ShowHiddenGames` off, with that and the folder entry
spelled in another case, with that and `nes` removed, with the two junctions and an entry through the loop; multi-disc,
and with a hidden track and `ShowHiddenFiles` off; arcade, and with `MAMENameStripExtraInfo` off; noload, and with
`ShowHiddenFiles` off, and with `ParseGamelistOnly` on; long-path; standard, and with `ShowHiddenGames` off, and with
`ShowHiddenFiles` off) gave the same tables through the census and through the hidden test pointed at them.

## Real library (`scripts/real-library.ps1`, ES-DE closed)

`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE -RomRoot I:\Giochi\Emulatori
-MediaRoot E:\Programmi\ES-DE_media\downloaded_media` (Release): the `[real-library]` tests passed and the snapshots
showed 0 differences among 542,082 entries. The figures:

- 103,067 games as ES-DE lists them; Retroverse: 105,130 entries, 90,246 games, 12,821 disc members (psx 12,820, saturn
  1), 1,038 missing files, 870 not games (837 ps2 `.cue` entries whose extension ps2 does not configure, 33 MAME BIOS
  and device sets), 155 folders, 0 outside their folder, 2,513 disc groups, 0 unavailable folders;
- psx: 2,519 games (2,518 cue sheets and one other file), 12,820 members, 2 missing, 2,512 groups (six cue sheets name
  only tracks that are not there); ps2: 3,593 games;
- 195 systems configured, 191 with a ROM folder, 48 with entries;
- issues: 12 `disc-reference-missing` (psx cue sheets naming tracks that are not there) and pcfx's
  `extension-without-dot`.

Timings (Release, informative: other worktrees were building meanwhile):

| Run | Cache | Total | Walk | Gamelists | Sheets |
|---|---|---|---|---|---|
| the hidden test, sheets one by one (an earlier build) | listing warm (the snapshot had just walked the tree), sheets cold | 22.2 s | 0.9 s | 0.6 s | 19.9 s (2,520) |
| `import --dry-run`, 8 readers | warm | 3.1 s | 1.3 s | 0.7 s | 0.06 s |
| the hidden test, 8 readers (every `[real-library]` test, twice: the second while other worktrees built) | warm | 3.0 s, 4.8 s | 1.3 s, 1.6 s | 0.7 s, 1.3 s | 0.04 s, 0.18 s |
| after the review fixes: the hidden test (through `real-library.ps1`, 0 differences) | listing warm, sheets mostly warm | 2.86 s | 1.07 s | 0.60 s | 0.26 s |
| after the review fixes: `import --dry-run` (exit 0) | warm | 1.67 s | 0.66 s | 0.43 s | 0.03 s |

The sheets dominate a cold first load on the external drive. With 8 readers they were not measured cold: once read
they stayed in the cache of the 64 GB machine for the rest of the session. From the cold ps2 measurement (4.5 times
faster per file with 8 readers), a cold first load should take about 4 to 5 s of sheets instead of 20; the gate
measures it on a quiet machine, and step 11's cache skips the sheets on every later import. The census takes 49 s cold
(its walk and its sheets one by one, in Python) and 6.5 s warm (5.7 s after the review fixes).

## Decisions taken within the step

1. **ES-DE's checks and their order, not a simplification of them**, so that both frontends agree: the exact
   `noload.txt` in the system folder against any case below it; the gamelist's hidden check on the file and its parent
   only; `ParseGamelistOnly` ignoring noload.txt; an empty ES-DE name skipping a file unless the gamelist names it; a
   BIOS directory entered as a folder. Each is in a fixture.
2. **Excluded files stay entries** with the reason, whether or not a gamelist names them (the standard README's
   variants keep 23 entries). Files whose extension is not configured are entries only when a gamelist names them.
3. **A `<folder>` entry for an existing folder without games is a Folder** (gc's `GraphicMods` folder and mame's
   `./roms` on the reference machine), although ES-DE would not show it: the folder is there, its metadata is kept.
   `ParseGamelistOnly` folders likewise.
4. **Excluded `<folder>` entries are NotAGame** with the reason (hidden, noload, `<hidden>`), like files.
5. **Members only from games**: a hidden track, a missing file or an unconfigured extension is not a member while it
   is so; a sheet that is not a game (ps2's `.cue`) heads nothing.
6. **Under an unavailable folder**: entries from the gamelist alone, one per key, with the name-based rules; cached
   groups of their heads kept; no MissingFile.
7. **System filter**: `EsdeLibrarySourceOptions::systems` makes a partial snapshot, for the dry run's `--system`; never
   for an import.
8. **The gamelist retry** waits `gamelistRetryDelay` (1 s by default, 0 in the tests), cancellable, then reads once
   more; a gamelist without a ROM folder (an unresolved path) is Failed when it exists, not read.
9. **MAMENameStripExtraInfo** joined the settings whitelist (step 2's reader), so arcade names follow the user's setting.
10. **Sheets in parallel**, 8 readers, results taken in the heads' order (deterministic); sheets larger than 1 MiB are
    unreadable.
11. **The cache keeps what a sheet says** (`DiscGroup::references`), never the members it gave: membership is a
    function of the sheet and of the files on disk now, not of the import history. A step-1 domain change, made here
    because the cache is this step's (review S7-2, F1).
12. **A hidden folder hides its tree** with `ShowHiddenGames` off, as ES-DE deletes it; only the entry spelled as the
    folder is applies (ES-DE's tree holds the names on disk), although a case variant still gives the folder its
    metadata (review S7-1). Not modelled: of two `<folder>` entries of one path, the first hidden one deletes the folder
    in ES-DE and the later one finds nothing, but step 3's reader keeps only the last entry of a kind and path (ES-DE
    never writes two).
13. **A link that loops is a warning** (`rom-link-loop`), as ES-DE skips it, not an unavailable folder: it never fails
    the dry run nor keeps rows out of reconciliation; an entry through it is a missing file, which ES-DE would list (a
    second entry of a file listed by its own path) (review S7-4, F3).
14. **A head carries its members' ES-DE play data** (play counts and times summed, the latest last played time, any
    favourite), members first along a chain; the members keep their own, and a group's play data is its head's alone
    (review F2). The queries and the UI (steps 9, 13) then need no aggregation over members.
15. **RetroArch's disc labels**: a playlist line `<path>|<label>` names its path (review S7-3).

## For the later steps

- **Step 11 (import):** pass `LibraryLoadOptions::discGroups` from the stored groups with each head's size and time,
  and store each group's `references` with it: they are what the next load picks the members from (a stored group
  without them gives no member). `idx_disc_members` holds members by GameId; the references include files that are not
  there (no identity), so they need their own rows by relative path (for example `idx_disc_references (head, position,
  relative_path)`, schema v1 being open). `SystemInfo::gamelist == Failed` keeps a system's rows; `UnavailableFolder`s
  and `SourceUnavailable` entries keep theirs; `EsdeLoadStatistics` for the log. A head's ES-DE flags already carry its
  members' play data: seed or count a group's ES-DE history from its head only. `import` without `--dry-run` is the
  stub in `cmd_import.cpp`.
- **Step 9 (queries):** "last played", "most played" and favourites over the visible games (heads, not members) are
  right as stored: a head's flags hold its members' play data.
- **Step 8 (media):** media follow the listed relative paths (`CatalogEntry::key.relativePath()`); a head shows its own
  media, members theirs.
- **Step 10 (launch):** the head is what launches; `CatalogEntry::altEmulator` as before.

## For the merge

- `src/esde/include/rv/esde/settings.hpp`, `src/esde/src/settings.cpp` and `tests/unit/esde/test_esde_settings.cpp`
  (step 2's): the whitelist gained `MAMENameStripExtraInfo`. `discover` does not print it yet (its eight values).
- Shared documentation: CLAUDE.md's module notes (`rv/esde/library_source.hpp`, the census, `import --dry-run`); the
  roadmap's step 7 row (done, with the figures above); architecture's `rv_esde` (the scan, the disc groups, the MAME
  lists, the library source).
- `src/catalog/include/rv/catalog/disc_group.hpp` (step 1's): `DiscGroup::references`, and the comments of
  `CachedDiscGroup` and of `LibraryLoadOptions::discGroups` (`library_source.hpp`): the cache gives references, the
  members are picked again. Nothing else uses the field yet; aggregate initializers without it still compile.
- Suggested ADR-0008 §3 amendment: the rules above that the ADR states loosely (the exact system-level `noload.txt`,
  the gamelist's hidden check, `ParseGamelistOnly` and noload, empty names, folders without games kept, members only
  from games, a shared member's head), and those of the review: a hidden `<folder>` entry hides the folder's tree, a
  link that loops is skipped with a warning, the disc group cache keeps references, a head carries its members' ES-DE
  play data, RetroArch's disc labels.

## Review findings (after the step was committed)

| Finding | Verdict | What changed |
|---|---|---|
| S7-1: a hidden `<folder>` entry hides only the folder, not its games (ShowHiddenGames off) | confirmed in ES-DE's source (`~FileData` deletes the children) | the tree below is not a game (rule above), in the C++ and the census; nested-roms' `L1/L2` entry is `<hidden>`, a test covers four variants |
| S7-2, F1: the cache stores the members a sheet gave, not what it says | confirmed (the old test asserted it) | `DiscGroup::references`, members picked again on every load; tests of a cached group with fewer members and of a track that comes back; missing references reported on every load |
| S7-3: `path\|label` lines of an .m3u are taken whole | confirmed | `m3uFiles()` cuts at the first `\|`; unit test, multi-disc's `Labels (USA).m3u` |
| S7-4, F3: a link that loops is an error and an unavailable folder | confirmed (ES-DE warns and loads the system) | `rom-link-loop` warning, no unavailable folder; an entry through the loop is a missing file; the census likewise |
| F2: ES-DE play data on a member is hidden | confirmed (8 members on the reference library) | a head adds its members' play data to its flags; multi-disc carries play data on a head, a track, a chain and a shared track |
