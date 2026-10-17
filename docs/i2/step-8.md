# I2 step 8: the media resolver and the media coverage

Roadmap row: I2 step 8 (wave 2, needs 2 and 3). Decisions: [ADR-0008](../decisions/ADR-0008-esde-catalog.md) §5 (media
stored as presence and extension per kind, paths derived), §7 (read-only), §10 (real-library tests);
[ADR-0003](../decisions/ADR-0003-no-integrated-scraper.md) (the coverage report). This page is folded into the shared
documentation at the wave merge.

## What was built

| File | What it does |
|---|---|
| `rv/esde/media_resolver.hpp` (`rv_esde`) | `EsdeMediaResolver`, `catalog::IMediaResolver` over ES-DE's media folders under one media root: `presence()` (the media of every kind an entry has: present, extension as the file spells it), `path()` (the file of a kind, derived without looking at the disk), `listing()` (a system's media folder as listed: its kind folders, whether they have case-sensitive names, the media, shadowed and ignored files, folders that could not be listed, issues), `checkRoot()`, `refresh()`, `stats()`; `mediaExtensions()` (ES-DE 3.4.1's extensions per kind, in its order: `kImageExtensions`, `kVideoExtensions`, `kManualExtensions`), `mediaRelativePath()` (the path below the media root), `MediaFolderState`, the issue codes `kMediaIssue...` |
| `rv/catalog/coverage.hpp` (`rv_catalog`) | `CoveragePopulation` (four populations), `inPopulation()`, `CoverageCounts` (members, unknown, per kind, any; `share()`), `SystemCoverage`, `MediaCoverage`, `CoverageBuilder` (entry by entry, from any source), `measureCoverage()` (a snapshot's entries, their media asked of an `IMediaResolver`), the issue code `media-unknown` |
| `tools/cli/src/cmd_media.cpp` | `retroverse-cli media --system <name> --path <relative path>`, `media --coverage [--system <name>]` (both with `--json` and `--output`), and `media --game <id>`, which answers "not implemented yet (I2 step 11)" until the import gives games their ids |

Tests: `tests/unit/esde/test_media_resolver.cpp`, `test_media_coverage.cpp`, the hidden `test_media_real.cpp`;
`tests/unit/catalog/test_coverage.cpp`; `tests/unit/cli/test_cli_media.cpp`, the hidden `test_cli_media_real.cpp`. New
fixture: `tests/fixtures/esde/media-roots` (an empty media root; the variants a missing one and one on a drive letter
that is not mapped; its README states the expectations). The standard fixture is used read-only.

## ES-DE 3.4.1's media rules, as verified in its source

Read from the tag `v3.4.1` of `gitlab.com/es-de/emulationstation-de`: `es-app/src/FileData.h` (the extension lists,
`getDisplayName()`), `es-app/src/FileData.cpp` (`getMediaDirectory`, `getMediafilePath`, `getVideoPath`,
`getManualPath`, the per-kind getters), `es-core/src/utils/FileSystemUtil.cpp` (`getStem`, `getFileName`, `getParent`,
`exists`).

- **The path.** `<media directory>/<system name>/<kind folder>/<sub-folders>/<display name><extension>`:
  - the media directory is `MediaDirectory` with `~` and `%ESPATH%` replaced and a `/` appended, or
    `<ES-DE home>/downloaded_media/` when empty (step 2's `resolveMediaRoot()`);
  - the system's **name** (es_systems.xml's `<name>`), whatever the ROM folder is called on disk (`GB` holds gb's ROMs,
    `gb` its media);
  - the sub-folders are the ROM's parent path with the system's ROM folder removed (`Utils::String::replace(getParent
    (mPath), mStartPath, "")`), so the ROM tree's sub-folders are mirrored as they are spelled on disk;
  - the display name is `Utils::FileSystem::getStem(mPath)`: the file name without what follows its last dot (the dot
    included), the whole name for a name without a dot, and the **whole name of a directory** (`getStem` tests
    `isDirectory()` first). Never trimmed: `Galactic Disk .atr` gives `Galactic Disk .png`; a file named `.gb` gives the
    media file `.png`. This is step 1's `catalog::mediaStem()`.
- **The kinds and their folders.** `getMediafilePath()` serves `covers`, `backcovers`, `3dboxes`, `physicalmedia`,
  `screenshots`, `titlescreens`, `marquees`, `miximages`, `fanart` and `custom` (`getCustomImagePath()`); `videos` has
  `getVideoPath()` and `manuals` `getManualPath()`. The 12 kinds of specification §4.4, `catalog::MediaKind`.
- **The extensions, in the order ES-DE tries them; the first file that exists wins:**
  - images (every kind but videos and manuals, custom included): `.png`, `.jpg`, `.webp` (`sImageExtensions`);
  - videos: `.mp4`, `.mkv`, `.avi`, `.wmv`, `.mov`, `.webm` (`sVideoExtensions`);
  - manuals: `.pdf` (a local list in `getManualPath()`).
  Nothing else is read: an `.flv` in a videos folder (four in snes on the reference machine) is never shown. The survey
  (esde-data.md section 5) had inferred `.png .jpg` and `.avi .mkv .mov .mp4 .wmv` from the strings of ES-DE.exe; the
  source adds `.webp` and `.webm` and fixes the order.
- **The test.** `Utils::FileSystem::exists()`: `std::filesystem::exists()` on the path's wide form (no `\\?\` prefix).
  On Windows it compares names as the file system does: case-insensitively with the volume's casing table (NTFS,
  exFAT), except in a folder with case-sensitive names. NTFS keeps that flag per folder
  (`FILE_CS_FLAG_CASE_SENSITIVE_DIR`); `fsutil file setCaseSensitiveInfo` sets it, and so does WSL on the folders it
  makes on Windows drives, and folders made inside such a folder get it too. In such a folder only ES-DE's exact
  spelling is found: the system's name, the kind's folder in lower case, the ROM's path as listed, the extension in
  lower case. It follows links, it ignores the hidden attribute, and it is also true for a folder.
- **Never through the gamelist.** ES-DE 3.x writes no media paths into gamelists, and these functions read none: the
  media follow the ROM's path, never its `<name>`.

## Retroverse's choices within the step

**ES-DE's behaviour followed:**
- The system folder, the kind folders and the media names are matched as the file system matches them: the resolver
  lists each kind folder of a system once with `core::walkTree` (links followed, any length, hidden files included) and
  folds every media name with the lookup keys' folding (`core::fileSystemUpperCase`, with a shortcut for ASCII names
  that gives the same result), so an entry's media name is `mediaStem()` of its `LookupKey::key()`. `Collection
  A/Pocket Puzzle (World).gb` finds `COVERS/collection a/POCKET PUZZLE (WORLD).PNG`.
- Folders with case-sensitive names are matched as ES-DE matches them (review finding S8-R1), at no cost in the usual
  case:
  - the media root, each system folder and each kind folder are asked for the flag when listed (one handle with
    `FILE_READ_ATTRIBUTES` per folder, `GetFileInformationByHandleEx(FileCaseSensitiveInfo)`; false on exFAT, FAT32
    and most shares). In a folder that has it, ES-DE's folder is taken only spelled as ES-DE asks for it: `NES` is not
    nes's media folder, nor `Covers` its covers folder (`media-case-sensitive-folder` says so);
  - each media file keeps a 64-bit FNV-1a hash of its name as spelled. An entry that spells its media name alike,
    with ES-DE's extension, is answered from the listing: no folder's case rule can refuse that spelling;
  - otherwise the name matched only through the folding. Then the kind folder's flag and those of the folders below it
    on the way, opened through the entry's spelling and asked once each per listing, decide. Without the flag
    anywhere, the listing answers as before. With it (or for a folder that cannot be opened to tell), each of ES-DE's
    paths (`mediaRelativePath()`, extension by extension in ES-DE's order) is looked up on disk with ES-DE's
    existence test, a link followed, a folder no file, and the first that is there is the media, with the extension
    as ES-DE spells it;
  - so `path()` always names a file the file system finds. `stats()` counts the folders asked (`caseChecks`) and the
    files looked up (`fileChecks`).
  On the reference machine no folder has the flag: 520 folders asked (the root, 44 system folders, 475 kind
  folders), none below a kind folder, no file looked up; the listing time and the memory did not move beyond the
  hash's 8 bytes per file (1.02 s warm, +56 MiB).
- Per kind, the first of ES-DE's extensions for which a file is there; the extension is kept as the file spells it
  (`.PNG`), which reaches the same file on NTFS and exFAT. In a folder with case-sensitive names ES-DE asks for
  `.png`: a lone `Game.PNG` there is not found, a `Game.jpg` beside it is.
- A missing system folder or kind folder is no media; a missing entry (a gamelist entry whose file is gone) still has
  its media (`directory` false: its name loses its last extension, as for a file).
- Zero-byte files count as media (ES-DE's test only asks whether the file exists; two are on the reference machine).

**Deliberate differences, documented in the header:**
- Only files are media. ES-DE's `exists()` is also true for a folder named `Game.png`, which ES-DE then fails to load
  (and it stops looking: a `Game.jpg` beside it is never tried); Retroverse takes the `Game.jpg`.
- Names are matched at any length (`\\?\` listings). ES-DE's plain Win32 paths stop at MAX_PATH unless Windows lets
  long paths through; the longest media path on the reference machine is 252 characters.
- The first version had a third difference: it folded names in folders with case-sensitive names too, so `presence()`
  could report a file ES-DE does not find, and `path()` name a spelling that does not exist. It is gone (see above).
  The listing's counts (`media`, `shadowedFiles`) still compare names folded.

**Availability (ADR-0008 §4, the port's contract):**
- The media root missing, a file, on a drive or share that cannot be reached, or not resolved (`root-unresolved`):
  `checkRoot()`, `presence()` and `listing()` are `Unavailable`; nothing can be known, and the import keeps what it
  stored. `path()` still derives a path when the root is known.
- A system's media folder, or one of its kind folders, that exists but cannot be listed: `presence()` is
  `Unavailable` for every entry of that system (`listing()` gives the folder `MediaFolderState::Unavailable`).
- A folder below a kind folder that cannot be listed (access denied, a link that loops): `presence()` is
  `Unavailable` for the entries whose media would lie below it, the others are answered
  (`MediaKindListing::unlistedFolders`).
- Entries the walk cannot read (a link whose target cannot be opened, a name without a UTF-8 form) are no media, as
  ES-DE cannot open them either (`media-unreadable-entry`).

**Listings in memory.** The root is listed once (its folders found as ES-DE finds them), each system's folder on the
first question about the system, and the answers come from those listings until `refresh()`, which forgets them all
(the root's listing and the folders' case flags too: a system folder made since is seen after it). The exception is
the rare name looked up on disk (above). One import therefore lists each folder once. The listings take about 160
bytes per media file (the folded name, the extension and the spelling's hash, in a sorted vector per kind searched by
binary search): 56 MiB for the reference machine's 364,810 files.

**Issues** (`catalog::Issue::code`, each naming the system and the folder):

| Code | Severity | When |
|---|---|---|
| `media-root-unavailable` | error | the media root is not known, missing or cannot be reached (`media --coverage` reports it) |
| `media-folder-unlisted` | error | a system folder, a kind folder or a folder below one exists but cannot be listed (the first 100 per kind one by one, then a count) |
| `media-ignored-extension` | info | files of an extension ES-DE does not read in a kind's folder, one per extension with the count (`4 file(s) with the extension ".flv" in snes/videos`) |
| `media-shadowed-file` | info | files beside one of the same name whose extension ES-DE tries first (the `.jpg` beside a `.png`), per kind |
| `media-unreadable-entry` | warning | entries of a kind folder the walk could not read, per kind |
| `media-case-sensitive-folder` | info | a system or kind folder has case-sensitive names, or a folder ES-DE would take is spelled otherwise in a folder (the media root included) that has them (`ES-DE does not take "Covers" for its covers folder`) |
| `media-unknown` | warning | (coverage) members whose media could not be known, one per system with the count and the first reason |

## Coverage

`catalog::measureCoverage(snapshot, resolver)` counts, per system and kind, the members of four populations that have a
file of the kind; the shares are over the members whose media are known (`unknown` ones, under a folder that cannot be
read, count in no share):

| Population | Members | Reference machine |
|---|---|---|
| `gamelist-entries` | every `<game>` entry of the gamelists (any state but Folder: its file there or not, outside its folder or not) | 40,870 |
| `gamelist-entries-with-file` | those whose file the scan found | 39,832 |
| `scraped-games` | the games (`EntryState::Game`) with a gamelist entry | 38,986 (38,995 before the snapshot grouped nine scraped disc tracks: see "Integration with step 7") |
| `games` | every game ES-DE shows, scraped or not (disc tracks grouped) | 90,246 (see "Integration with step 7") |

A folder (`<folder>` entry, or a listed folder) is in no population: a folder is no game, even though its own media are
listed (snes' `Hacks` cover). An entry outside its system folder is a member of `gamelist-entries` without media (ES-DE
skips it) and is never asked of the resolver.

**ADR-0003's denominator, checked.** ADR-0003 counts "40,870 `gamelist.xml` entries": every `<game>` entry, the 1,038
whose file is gone included, not the entries whose file exists (39,832) nor the scraped games (38,995, the survey's
"listed games" of esde-data.md section 5). `measureCoverage()` gives all three, and the CLI prints them.

**`retroverse-cli media`.**

```text
retroverse-cli [global options] media --system <name> --path <relative path> [--json] [--output <file>]
retroverse-cli [global options] media --coverage [--system <name>] [--json] [--output <file>]
retroverse-cli [global options] media --game <id>          (I2 step 11; until then: not implemented yet)
```

- The library is the one `<user data>/config/library.json` records (`library create`); its settings and systems are
  read, the dispatcher has checked the user data directory against its folders.
- `--path` takes a ROM path of the system as listed on disk or as a gamelist writes it (`./Sports Vol. 2`, `Collection
  A\Pocket Puzzle (World).gb`, an absolute path inside the ROM folder: `core::resolveRelativePath`). It is taken as
  the disk spells it, element by element as far as the elements are there (`FindFirstFileExW`): ES-DE names media
  after the file it lists, which matters in a media folder with case-sensitive names (`collection a/pocket puzzle
  (world).gb` is `Collection A/Pocket Puzzle (World).gb`). Whether it is a folder (its media keep its whole name), a
  file, or not there (named as a file) is looked up in the ROM folder. It prints the media name and, per kind, the
  file ES-DE shows or `none`; JSON: `system`, `path` (as the disk spells it), `romPath` (`file`, `folder`, `missing`,
  `unknown`), `mediaName`, `mediaRoot`, `media` (12 items: `kind`, `present`, `extension`, `path`).
- `--coverage` prints the media root, the populations' totals (one row each: members, then the share of each kind in
  percent), then per system the gamelist entries whose file exists and the games, then the issues. JSON: `mediaRoot`,
  `source` (where the entries come from: `rom-scan`, see below), `kinds`, `populations` (their descriptions),
  `listed` (`systems` whose media folder was looked up, `files` in those folders, `caseChecks` and `fileChecks` as
  `MediaListingStats` counts them), `total` and `systems[]` (`system`,
  `mediaFolder`: `listed`, `missing`, `unavailable` or null when the root cannot be read, `populations`), each
  population as `entries`, `unknown`, `withAny`, `withKind` (counts, never shares), and `issues`.
- **Where the coverage's entries come from.** The library snapshot the import reads: `esde::EsdeLibrarySource`
  (step 7) through `catalog::ILibrarySource::load()`, with the library.json profile (see "Integration with step 7").
  Until both steps were merged, `cmd_media.cpp` read its entries through a temporary `GamelistSource` (every `<game>`
  entry of the gamelists looked up in its system's ROM folder: no files without a gamelist entry, no disc groups, no
  noload.txt, no MAME BIOS sets, so its `games` were the scraped games); the figures under "Real library" below were
  measured with it.
- Exit codes: 0; 1 when something could not be read (the media root, a media folder, the settings, the systems, a
  gamelist, the ROM folder of `--path`); 2 for invalid options, an unknown system, a path that leads out of its ROM
  folder or is malformed, `--game`, and no library.json; 3 when library.json is invalid or the results cannot be
  written.

## Real library (`scripts/real-library.ps1`, with ES-DE closed)

`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE -RomRoot I:\Giochi\Emulatori
-MediaRoot E:\Programmi\ES-DE_media\downloaded_media`, Release. Each run was made twice: with `-Filter
'[real-library][media]'` (this step's two tests) and with the default filter (every `[real-library]` test, 5 test
cases). That was done for the step (4,145 assertions in all), then again after the review (1,692 assertions in the
step's two tests, 4,724 in all). Every run passed, and the snapshots showed 0 differences among 542,082 entries each
time.

**The media tree** (`esde/media: the real library's media tree`, `test_media_real.cpp`):
- 44 system folders, each spelled as its system; 364,810 files in 6,878 folders, every folder listed (none that could
  not be, no entry that could not be read). No folder has case-sensitive names (the root, the 44 system folders and
  their 475 kind folders were asked).
- Media per kind, equal to the survey's file counts (esde-data.md section 5): covers 36,388, backcovers 31,081, 3dboxes
  36,250, physicalmedia 32,569, screenshots 39,691, titlescreens 38,235, marquees 35,705, miximages 39,674, fanart
  19,636, manuals 22,768, videos 32,809, custom none. No name has both a `.png` and a `.jpg` (0 shadowed files); the four
  `.flv` of snes/videos are ignored (`media-ignored-extension`).
- The listing took 1.19 s and 1.63 s for the step, and 1.02 s after the review, with the case flags of 520 folders
  asked. All three were **warm**: the snapshot the script takes just before had listed the same tree (the survey
  measured 5.8 s cold and 1.2 to 1.3 s warm with Python). The listings cost 53 MiB of private bytes, 56 MiB with the
  spelling's hash: about 160 bytes per media file. Informative: the gate times the import on a quiet machine.

**The coverage** (`cli/media: the real library's media coverage ...`, `test_cli_media_real.cpp`: `library create` with
the four folders into a temporary user data directory, then `media --coverage --json`). It took 2.5 and 3.0 s for the
step and 2.07 s after the review, warm: the gamelists read, the ROM folders listed down the gamelists' folders and the
media of 45 systems listed. Of the entries, none needed a folder below a kind folder asked for its case rule, and no
media file was looked up on disk (`fileChecks` 0, checked). Every count equals an independent oracle's: a Python count
over the survey's read-only listings of 2026-10-16 (ElementTree over the gamelists, `os.scandir` listings of both
trees, its own media-name rule, ES-DE's extensions, `str.upper()` folding), run once on those machine-local listings,
its figures frozen in the test:

| Kind | Every `<game>` entry (40,870) | Their file exists (39,832) | Scraped games (38,995) |
|---|---|---|---|
| covers | 37,151 (90.90%) | 36,549 (91.76%) | 35,719 (91.60%) |
| backcovers | 31,768 (77.73%) | 31,442 (78.94%) | 30,730 (78.80%) |
| 3dboxes | 37,012 (90.56%) | 36,502 (91.64%) | 35,672 (91.48%) |
| physicalmedia | 33,302 (81.48%) | 33,003 (82.86%) | 32,223 (82.63%) |
| screenshots | 40,359 (98.75%) | 39,413 (98.95%) | 38,581 (98.94%) |
| titlescreens | 38,842 (95.04%) | 38,058 (95.55%) | 37,314 (95.69%) |
| marquees | 36,468 (89.23%) | 36,003 (90.39%) | 35,175 (90.20%) |
| miximages | 40,357 (98.74%) | 39,411 (98.94%) | 38,579 (98.93%) |
| fanart | 20,002 (48.94%) | 19,780 (49.66%) | 19,436 (49.84%) |
| manuals | 23,110 (56.55%) | 22,803 (57.25%) | 22,453 (57.58%) |
| videos | 33,592 (82.19%) | 33,249 (83.47%) | 32,424 (83.15%) |
| custom | 0 | 0 | 0 |
| any kind | 40,561 (99.24%) | 39,563 (99.32%) | 38,729 (99.32%) |

Per system, the scraped games and their covers, screenshots, videos and marquees are esde-data.md section 5's table for
all 45 systems with a gamelist, but one cell: snes has 1,462 games with a video, not 1,466, since the survey counted
the four `.flv` files that ES-DE never reads. trs-80 has a gamelist and no media folder (`missing`).

**ADR-0003's table.** ADR-0003 states its shares over the 40,870 `gamelist.xml` entries: every `<game>` entry, those
whose file is gone included. Over them, as written:

| ADR-0003's row | As written | Measured | Difference | With the `<folder>` entries' media: over 40,870 | over 41,025 |
|---|---|---|---|---|---|
| Screenshot | 98.5% | 98.75% | +0.25 point (+102 entries) | 98.85% | 98.47% |
| Cover | 90.6% | 90.90% | +0.30 point (+123) | 90.99% | 90.65% |
| Marquee | 89.3% | 89.23% | -0.07 point | 89.31% | 88.97% |
| Video | 82.4% | 82.19% | -0.21 point (-85) | 82.25% | 81.94% |
| Physical media image | 81.2% | 81.48% | +0.28 point (+116) | 81.54% | 81.23% |
| Back cover | 77.8% | 77.73% | -0.07 point | 77.81% | 77.52% |
| Manual | 56.6% | 56.55% | -0.05 point | 56.58% | 56.37% |

**The roadmap's criterion, "within 0.1 point of ADR-0003", is not met against the table as written:** three rows are
reproduced within 0.1 point, four differ by 0.21 to 0.30 point. The step's first version of the hidden test allowed
those four rows tolerances fitted to the measured values, which hid the gap (review finding F1). Why the rows differ:
- The measured figures are not in doubt. The oracle, written independently of the C++ code, gives the same counts
  entry for entry, on all three populations and per system against the survey's own table. A reviewer's spot checks
  of ES-DE's existence test agree.
- ADR-0003 mixes populations. Count the 155 `<folder>` entries and their media in the numerator (the last two
  columns). Then marquee, back cover and manual fit a denominator of 40,870 (within 0.02 point), while screenshot,
  cover and physical media fit one of 41,025 (within 0.05 point). Video fits neither (82.25 or 81.94%; 82.26% with the
  four `.flv` counted). No single population gives all seven rows.
- Its per-system figures are those of every `<game>` entry, as written: Atari 800 has 47.23% covers and 14.82% videos
  over its 3,172 entries (ADR-0003: 47% and 15%), Commodore 64 52.87% and 11.66% over 1,012 (53% and 12%). Over the
  scraped games Atari 800 would read 49.5% and 15.5%.
- ADR-0003 (2026-10-15, a day before the survey) records no method beyond "matching every entry to downloaded_media
  with ES-DE's naming rule". The data have not changed since: the media tree was last written on 2026-09-04 and the
  gamelists on 2026-09-06 (ES-DE has not run since), and the snapshots match the survey's counts.
- Counting files per kind instead of entries gives 97.1, 89.0, 87.4, 80.3, 79.7, 76.0 and 55.7%: no row either.
- "Description and metadata" (90.5%) is no media kind: 37,128 of the 40,870 entries have a `<desc>` (90.84%).

The reproducible table is the measured column, over every `<game>` entry. The merge amends ADR-0003 with it (see "For
the merge"). The hidden test now checks every row to 0.1 point against those amended figures (`kAdr0003`), prints the
figures as written beside them, and checks the per-system figures as written (whole percents of each system's
`<game>` entries). The exact oracle counts above pin the figures anyway. Until the amendment the criterion stays open;
with it, it is met.

## Review findings addressed

- **S8-R1 (folders with case-sensitive names).** Fixed rather than documented. The header's claim that the resolver
  "compares names as the file system does" was false for these folders: `presence()` reported media ES-DE never shows,
  and `path()` combined the entry's spelling with the file's extension into a name that does not exist. It held in the
  reviewer's scratch library (`covers` made case-sensitive with `fsutil`, holding `case game.png`, for the ROM `Case
  Game.nes`). Now `media --system nes --path "Case Game.nes"` there shows no cover, as ES-DE does. See "ES-DE's
  behaviour followed" for the rule and its cost (none on the reference machine). New test: `esde/media: folders with
  case-sensitive names, where ES-DE finds only its own spelling` (skipped where the flag cannot be set). It covers a
  kind folder with the flag (an entry spelled otherwise, an upper-case extension beside a `.jpg`, two names that
  differ in case only, a sub-folder that inherits the flag), a sub-folder with the flag below a kind folder without
  it, and a media root and a system folder with it (`NES`, `Covers` not taken). Every `path()` it derives exists, and
  the folders asked and the files looked up are counted exactly. `media --path` now takes the ROM path as the disk
  spells it (ES-DE names media after the file it lists).
- **F1 (ADR-0003's table).** Confirmed: no single population reproduces it (the reviewer's analysis, re-derived here
  from the survey's listings). The test's fitted tolerances are gone (see above), the amendment is in "For the merge",
  and the back-cover share counted per file reads 76.0% (31,081 / 40,870 = 76.05%), not 76.1%.

## Verification

- `rv_tests "[esde][media],[catalog][coverage],[esde][coverage],[cli][media]"` (16 test cases run, the 2 hidden ones
  skipped; 1,613 assertions):
  - the extension lists and their order; the media paths (dotted names, folders, trailing spaces, an empty media name);
  - the standard fixture: every entry of its README's media table, the 12 kinds of Alpha Quest, png before jpg, the
    ignored flv, the folder covers (Hacks, Sports Vol. 2 with its whole name), the namesake without media, the missing
    file's cover, the listings and their issues, the derived paths, path()'s refusals;
  - trees written at test time: system, kind and media names in another case (`GB/COVERS/collection a/...PNG`), every
    image and video extension in ES-DE's order (`.webp` and `.webm` included), manuals in `.PDF`, a folder named like
    a media file, files without an extension, non-ASCII names (é and É), punctuation, a hidden file, a media path past
    MAX_PATH, refresh() after files and a system folder appear; nothing looked up on disk without case-sensitive
    folders;
  - folders with case-sensitive names (above);
  - media roots: empty, without kind folders, missing, a file, an unmapped drive letter, unresolved; folders that
    cannot be listed (`ListingDenied`) below a kind folder, a kind folder, a system folder;
  - the coverage domain on a scripted resolver (populations, counts, unknown members, what is asked), the standard
    fixture's 23 entries over the four populations, the media-roots fixture's empty and missing roots;
  - `retroverse-cli media` as text and JSON on both fixtures, its usage errors, `--game`, `--output`, the exit codes,
    a `--path` in another case taken as the disk spells it, and the fixture's files unchanged after it.
- Debug and Release build without a warning; the unit tests (1,028) pass in both.
- The real library: see above.

## Integration with step 7

Made once both steps were on main (I2 step 7-8i).

**What changed.**
- `tools/cli/src/cmd_media.cpp`: `media --coverage` measures the library snapshot the import reads,
  `esde::EsdeLibrarySource` through `catalog::ILibrarySource::load()` with the profile library.json records (its root
  overrides; the source reads ES-DE's settings and systems itself), only the system of `--system`
  (`EsdeLibrarySourceOptions::systems`). The temporary `GamelistSource`, `ListedFile`, `extensionOf()` and
  `parentsOf()` are gone. Reading library.json and loading the snapshot are shared with `import --dry-run` through
  `tools/cli/src/library_snapshot.hpp` (`readRecordedLibrary()`, `readLibrarySnapshot()`), a private header that no
  source list names, like `launch_text.hpp`; both commands keep their messages and exit codes.
- The output: the JSON's `source` is `rom-scan`; the text's `Entries` line gives the snapshot's entries (`23 in the
  library snapshot (the ROM folders listed with ES-DE's rules, the gamelists merged in, disc tracks grouped under their
  sheet)`); the games table is "The games, per system"; the `games` description ends "disc tracks grouped under their
  sheet". The snapshot's warnings and errors (ROM folders, gamelists, disc groups) are among the issues, and an error
  makes the exit code 1, as before.
- The populations are `coverage.hpp`'s four, unchanged. `games` is now every game of the snapshot, scraped or not
  (`EntryState::Game`; disc members are in no population). `scraped-games` loses the scraped disc tracks the snapshot
  groups under their sheet (below). The two populations of `<game>` entries are what they were, entry for entry.
- `media --system --path` is unchanged: it takes the ROM path as the disk spells it (`spelledOnDisk()`). The snapshot
  would not give that more simply: it holds entries only (a path ES-DE does not list, such as a gone file without a
  gamelist entry, still gets its media names), and loading it for one path walks the whole system folder. The
  snapshot's keys keep the disk's spelling too (`EsdeLibrarySource` keys an entry by the listed file, also when its
  gamelist entry spells it in another case), so in a folder with case-sensitive names the coverage asks for the
  spelling ES-DE asks for.
- Cost: the coverage now walks every ROM folder whole and reads the cue sheets and playlists: 3.6 s on the reference
  machine (Release, warm; 5.2 s within the run of every real-library test), against 2.1 s through the gamelists.

**Tests.** `test_cli_media.cpp`: the standard fixture's coverage has the snapshot's 15 games (gb 8, snes 5, psx 2) with
covers 4, videos 5 and any kind 6, as `test_media_coverage.cpp` counts them on a snapshot built by hand, with their text
rows, and the gamelist populations as before (11, 9, 8); the media-roots fixture has 3 games (gb 1, nes 2) in two
systems (gb, whose game has no gamelist entry, is now in the report, its media folder missing), all unknown when the
root is unplugged. The media-roots README gives the games in its table instead of the paragraph about the temporary
source. `test_cli_media_real.cpp` checks the games population in total and for each of the 48 systems, and the scraped
games as step 8's figures less the scraped tracks.

**The real library** (`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE
-RomRoot I:\Giochi\Emulatori -MediaRoot E:\Programmi\ES-DE_media\downloaded_media`, Release, ES-DE closed): with
`-Filter '[real-library][media]'` 2 test cases, 2,110 assertions; with the default filter 8 test cases (6 passed, the
two launch tests skipped without `-RetroArchExe`), 226,413 assertions; 0 differences among 542,082 entries both times.
`media --coverage` exits 0 and lists 48 systems: the 45 with a gamelist, and pc (21 games), pcfx (12) and switch (229),
which have neither a gamelist nor a media folder (`missing`, as trs-80's); 364,810 files in their media folders, 520
folders asked for their case rule, no media file looked up on disk.

| Kind | Every game (90,246) | Scraped games (38,986) |
|---|---|---|
| covers | 36,010 (39.90%) | 35,711 (91.60%) |
| backcovers | 30,869 (34.21%) | 30,723 (78.81%) |
| 3dboxes | 35,960 (39.85%) | 35,664 (91.48%) |
| physicalmedia | 32,397 (35.90%) | 32,215 (82.63%) |
| screenshots | 39,237 (43.48%) | 38,573 (98.94%) |
| titlescreens | 37,888 (41.98%) | 37,306 (95.69%) |
| marquees | 35,397 (39.22%) | 35,167 (90.20%) |
| miximages | 39,221 (43.46%) | 38,571 (98.94%) |
| fanart | 19,482 (21.59%) | 19,430 (49.84%) |
| manuals | 22,618 (25.06%) | 22,450 (57.58%) |
| videos | 32,581 (36.10%) | 32,416 (83.15%) |
| custom | 0 | 0 |
| any kind | 39,404 (43.66%) | 38,721 (99.32%) |

- **Every game**: step 7's 90,246 (its 12,821 disc members, 1,038 missing files, 870 entries that are no game and 155
  folders are in no games population). Each share is under half the scraped games' since 51,260 games have no
  gamelist entry (c64 12,014, mame 6,786, snes 6,237, gb 4,159, megadrive 3,615, nes 2,947, ...). Of those, 683 have
  media all the same, named after their file (atari800 537, ps2 92, c64 30, 14 other systems 24 together).
- **Scraped games**: 38,986, nine fewer than the 38,995 measured with `GamelistSource`, which took every track for a
  game as ES-DE does. The snapshot makes nine scraped tracks disc members of their sheet: eight psx `.bin` tracks
  (FIFA Football 2003, Final Fantasy IX disc 1, International Track & Field, Metal Slug X, This Is Football 2 in two
  versions, Wild Arms, The X-Files disc 1; each with a cover, a screenshot, a video and a marquee, 80 media in all) and
  saturn's Sega Rally Championship track 01 (no media). Per system, the survey's table less those: psx 2,225 scraped
  games (covers, screenshots 2,220, videos 2,215, marquees 2,219), saturn 7.
- **The `<game>` entries**: unchanged, 40,870 and 39,832 with their file, every count as the oracle gives it, so
  ADR-0003's amended table is met as before (every row within 0.01 point).

**The oracle of the games population.** Independent of the C++ and run once, on 2026-10-17, on the reference library
(read-only, ES-DE closed), its figures frozen in `test_cli_media_real.cpp` (`kGames`, `kGameRows`, `kScrapedTracks`):
the entries and their states from `scripts/esde_census.py` (step 7's oracle of `EsdeLibrarySource`, whose table
matches the C++'s byte for byte), its `System` class driven as its `census()` drives it, so that each system's chosen
definition keeps its entries; the media from an `os.scandir` listing of the media root made for the purpose, ES-DE's
rule applied in Python (the ROM path below the system folder, a file's last extension removed and a folder's name
whole, the extensions ES-DE reads per kind, only files, names compared with the census's case folding: no media folder
has case-sensitive names); the populations as `coverage.hpp` defines them. It reproduced step 8's figures of both
`<game>` entry populations count for count, step 7's states (90,246 games, 12,821 members, 1,038 missing, 870 not a
game, 155 folders) and the 364,810 media files.

**Left open.** With the snapshot, a `<folder>` entry that ES-DE excludes (`ShowHiddenGames` off and `<hidden>`,
`ShowHiddenFiles` off and a hidden folder, a noload.txt) is `NotAGame` with a gamelist path, so `inPopulation()` counts
it among the `<game>` entries (`gamelist-entries`, and `gamelist-entries-with-file` when listed); `GamelistSource` read
no `<folder>` entries. `CatalogEntry` does not say which kind of gamelist entry named it, and an excluded game
directory (a `<game>` entry) looks alike. There is none on the reference library (all 155 `<folder>` entries are
folders) nor in the fixtures the coverage tests read; the noload fixture has one as committed (`gba/Skip Me`, which
holds a noload.txt), nested-roms one with `ShowHiddenGames` off (`nes/L1/L2`). A fix needs that kind in the snapshot
(step 1's domain): a candidate for BACKLOG.

## For the merge

- **ADR-0003.** Amend its context table with the reproducible figures, the population named: "On the reference
  collection (45 systems, 40,870 `<game>` entries of `gamelist.xml`, those whose file is gone included; `<folder>`
  entries left out), matching every entry to `downloaded_media` with ES-DE 3.4.1's naming rule and extensions
  (I2 step 8, `retroverse-cli media --coverage`) gives: Screenshot 98.75%, Cover 90.90%, Marquee 89.23%, Video
  82.19%, Physical media image 81.48%, Back cover 77.73%, Manual 56.55%". Keep "Description and metadata" at 90.8%
  (37,128 entries with a `<desc>`), or drop it as no media kind. Note that the figures of 2026-10-15 (98.5, 90.6,
  89.3, 82.4, 81.2, 77.8, 56.6) mixed populations. The per-system sentence stands as written (Atari 800 47% covers,
  15% videos; Commodore 64 53% and 12%: over every `<game>` entry). `test_cli_media_real.cpp`'s `kAdr0003` holds the
  amended figures; a different amendment changes them there.
- **Roadmap, step 8's row.** Done; the criterion "real coverage over 40,870 entries within 0.1 point of ADR-0003" is
  met against the amended table (every row within 0.01 point), with the figures above.
- **Step 7.** Done (I2 step 7-8i, "Integration with step 7" above): `showCoverage()` reads the snapshot of
  `esde::EsdeLibrarySource`; `GamelistSource`, `ListedFile`, `extensionOf()` and `parentsOf()` are deleted; the JSON's
  `source` is `rom-scan`, the text has lost its "until the ROM scan" notes; `test_cli_media.cpp` expects the snapshot's
  games (standard 15, media-roots 3), the media-roots README's last paragraph is gone, and `test_cli_media_real.cpp`
  checks the games population over the real library (90,246 games) besides the gamelist populations. The snapshot's
  keys keep the disk's spelling of the files.
- **Step 11.** The import asks `presence()` for every entry with a key and stores the presence and extension per kind
  (ADR-0008 §5), keeping what it stored when `presence()` is `Unavailable`; `media --game <id>` reads them back and
  derives the paths with `path()`. One `EsdeMediaResolver` per import, `refresh()` (or a new one) at the next.
- **Step 13.** The app opens media through `core::extendedLengthPathBelow(media root, mediaRelativePath(...))` (names
  that end with a dot or a space).
- **Shared documentation.** CLAUDE.md (the helpers list): `rv/esde/media_resolver.hpp` and `rv/catalog/coverage.hpp`
  as above; architecture, `rv_esde`: the media resolver on `core::walkTree`, and `rv_catalog`: the coverage service.
  The standard fixture's README lists the video extensions ES-DE reads as `.avi .mkv .mov .mp4 .wmv`: ES-DE 3.4.1
  reads `.mp4 .mkv .avi .wmv .mov .webm` (and images `.png .jpg .webp`), a parenthesis to correct when the README is
  next edited. S8-R1 is fixed and needs no BACKLOG entry. After the integration with step 7: CLAUDE.md's retroverse-cli
  section can say that `media --coverage` measures the library snapshot of the ROM scan (as `import --dry-run` reads
  it, `library_snapshot.hpp`); BACKLOG can take the `<folder>` entries that `gamelist-entries` counts ("Left open"
  above).

## Suggested amendments

- **ADR-0008 §5** could name the extension lists and their order as verified (images `.png .jpg .webp`, videos `.mp4
  .mkv .avi .wmv .mov .webm`, manuals `.pdf`), the availability rule (a missing or unreachable media root, or a media
  folder that cannot be listed, is unknown, never "no media"), and the case rule: media names are matched with the
  lookup keys' folding, except below a folder with case-sensitive names, where ES-DE's own spelling decides.
