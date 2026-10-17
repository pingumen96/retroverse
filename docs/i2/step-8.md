# I2 step 8: the media resolver and the media coverage

Roadmap row: I2 step 8 (wave 2, needs 2 and 3). Decisions: [ADR-0008](../decisions/ADR-0008-esde-catalog.md) §5 (media
stored as presence and extension per kind, paths derived), §7 (read-only), §10 (real-library tests);
[ADR-0003](../decisions/ADR-0003-no-integrated-scraper.md) (the coverage report). This page is folded into the shared
documentation at the wave merge.

## What was built

| File | What it does |
|---|---|
| `rv/esde/media_resolver.hpp` (`rv_esde`) | `EsdeMediaResolver`, `catalog::IMediaResolver` over ES-DE's media folders under one media root: `presence()` (the media of every kind an entry has: present, extension as the file spells it), `path()` (the file of a kind, derived without looking at the disk), `listing()` (a system's media folder as listed: its kind folders, the media, shadowed and ignored files, folders that could not be listed, issues), `checkRoot()`, `refresh()`, `stats()`; `mediaExtensions()` (ES-DE 3.4.1's extensions per kind, in its order: `kImageExtensions`, `kVideoExtensions`, `kManualExtensions`), `mediaRelativePath()` (the path below the media root), `MediaFolderState`, the issue codes `kMediaIssue...` |
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
  On Windows it compares names as the file system does (NTFS and exFAT: case-insensitively, with their casing table),
  it follows links, it ignores the hidden attribute, and it is also true for a folder.
- **Never through the gamelist.** ES-DE 3.x writes no media paths into gamelists, and these functions read none: the
  media follow the ROM's path, never its `<name>`.

## Retroverse's choices within the step

**ES-DE's behaviour followed:**
- The system folder, the kind folders and the media names are matched as the file system matches them: the resolver
  lists each kind folder of a system once with `core::walkTree` (links followed, any length, hidden files included) and
  folds every media name with the lookup keys' folding (`core::fileSystemUpperCase`, with a shortcut for ASCII names
  that gives the same result), so an entry's media name is `mediaStem()` of its `LookupKey::key()`. `Collection
  A/Pocket Puzzle (World).gb` finds `COVERS/collection a/POCKET PUZZLE (WORLD).PNG`.
- Per kind, the first of ES-DE's extensions for which a file is there; the extension is kept as the file spells it
  (`.PNG`), which reaches the same file on NTFS and exFAT.
- A missing system folder or kind folder is no media; a missing entry (a gamelist entry whose file is gone) still has
  its media (`directory` false: its name loses its last extension, as for a file).
- Zero-byte files count as media (ES-DE's test only asks whether the file exists; two are on the reference machine).

**Deliberate differences, documented in the header:**
- Only files are media. ES-DE's `exists()` is also true for a folder named `Game.png`, which ES-DE then fails to load
  (and it stops looking: a `Game.jpg` beside it is never tried); Retroverse takes the `Game.jpg`.
- Names are matched at any length (`\\?\` listings). ES-DE's plain Win32 paths stop at MAX_PATH unless Windows lets
  long paths through; the longest media path on the reference machine is 252 characters.
- Of two files whose names differ in case only (a case-sensitive folder, which NTFS allows per folder), the one spelled
  with ES-DE's extension (`.png` before `.PNG`) is taken, then the first in ordinal order; ES-DE would ask for its own
  spelling of the stem too.

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

**Listings in memory.** The root is listed once (its folders found case-insensitively), each system's folder on the
first question about the system, and the answers come from those listings until `refresh()`, which forgets them all
(the root's listing too: a system folder made since is seen after it). One import therefore lists each folder once;
the listings take about 150 bytes per media file (the folded name and the extension, in a sorted vector per kind
searched by binary search): 53 MiB for the reference machine's 364,810 files.

**Issues** (`catalog::Issue::code`, each naming the system and the folder):

| Code | Severity | When |
|---|---|---|
| `media-root-unavailable` | error | the media root is not known, missing or cannot be reached (`media --coverage` reports it) |
| `media-folder-unlisted` | error | a system folder, a kind folder or a folder below one exists but cannot be listed (the first 100 per kind one by one, then a count) |
| `media-ignored-extension` | info | files of an extension ES-DE does not read in a kind's folder, one per extension with the count (`4 file(s) with the extension ".flv" in snes/videos`) |
| `media-shadowed-file` | info | files beside one of the same name whose extension ES-DE tries first (the `.jpg` beside a `.png`), per kind |
| `media-unreadable-entry` | warning | entries of a kind folder the walk could not read, per kind |
| `media-unknown` | warning | (coverage) members whose media could not be known, one per system with the count and the first reason |

## Coverage

`catalog::measureCoverage(snapshot, resolver)` counts, per system and kind, the members of four populations that have a
file of the kind; the shares are over the members whose media are known (`unknown` ones, under a folder that cannot be
read, count in no share):

| Population | Members | Reference machine |
|---|---|---|
| `gamelist-entries` | every `<game>` entry of the gamelists (any state but Folder: its file there or not, outside its folder or not) | 40,870 |
| `gamelist-entries-with-file` | those whose file the scan found | 39,832 |
| `scraped-games` | the games (`EntryState::Game`) with a gamelist entry | 38,995 |
| `games` | every game ES-DE shows, scraped or not (disc tracks grouped) | after step 7 |

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
  A\Pocket Puzzle (World).gb`, an absolute path inside the ROM folder: `core::resolveRelativePath`). Whether it is a
  folder (its media keep its whole name), a file, or not there (named as a file) is looked up in the ROM folder. It
  prints the media name and, per kind, the file ES-DE shows or `none`; JSON: `system`, `path`, `romPath` (`file`,
  `folder`, `missing`, `unknown`), `mediaName`, `mediaRoot`, `media` (12 items: `kind`, `present`, `extension`,
  `path`).
- `--coverage` prints the media root, the populations' totals (one row each: members, then the share of each kind in
  percent), then per system the gamelist entries whose file exists and the games, then the issues. JSON: `mediaRoot`,
  `source` (where the entries come from: `gamelists`, see below), `kinds`, `populations` (their descriptions),
  `listed` (`systems` whose media folder was looked up, `files` in those folders), `total` and `systems[]` (`system`,
  `mediaFolder`: `listed`, `missing`, `unavailable` or null when the root cannot be read, `populations`), each
  population as `entries`, `unknown`, `withAny`, `withKind` (counts, never shares), and `issues`.
- **Where the coverage's entries come from until step 7 is merged.** The ROM scan and the library snapshot
  (`esde::EsdeLibrarySource`) are step 7's, in the same wave. `cmd_media.cpp` reads the entries through
  `catalog::ILibrarySource`, implemented there by `GamelistSource`: every `<game>` entry of the gamelists looked up in
  its system's ROM folder (listed with `core::walkTree` down the folders the gamelist names only), with the states the
  gamelist and the listing tell: Game, NotAGame for an extension the system does not configure (compared with its
  case) and for what ShowHiddenFiles and ShowHiddenGames exclude, MissingFile, OutsideSystemFolder, SourceUnavailable.
  It has no files without a gamelist entry, no disc groups, no noload.txt and no MAME BIOS sets, so its `games` are the
  scraped games and the text says so. The three gamelist populations are exact with it (the real-library test checks
  them against the oracle).
- Exit codes: 0; 1 when something could not be read (the media root, a media folder, the settings, the systems, a
  gamelist, the ROM folder of `--path`); 2 for invalid options, an unknown system, a path that leads out of its ROM
  folder or is malformed, `--game`, and no library.json; 3 when library.json is invalid or the results cannot be
  written.

## Real library (`scripts/real-library.ps1`, with ES-DE closed)

`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE -RomRoot I:\Giochi\Emulatori
-MediaRoot E:\Programmi\ES-DE_media\downloaded_media`, Release, run twice: with `-Filter '[real-library][media]'` (this
step's two tests) and with the default filter (every `[real-library]` test, 5 test cases, 4,145 assertions). Both
passed; the snapshots showed 0 differences among 542,082 entries each time.

**The media tree** (`esde/media: the real library's media tree`, `test_media_real.cpp`):
- 44 system folders, each spelled as its system; 364,810 files in 6,878 folders, every folder listed (none that could
  not be, no entry that could not be read).
- Media per kind, equal to the survey's file counts (esde-data.md section 5): covers 36,388, backcovers 31,081, 3dboxes
  36,250, physicalmedia 32,569, screenshots 39,691, titlescreens 38,235, marquees 35,705, miximages 39,674, fanart
  19,636, manuals 22,768, videos 32,809, custom none. No name has both a `.png` and a `.jpg` (0 shadowed files); the four
  `.flv` of snes/videos are ignored (`media-ignored-extension`).
- The listing took 1.19 s and 1.63 s (the two runs), **warm**: the snapshot the script takes just before had listed the
  same tree (the survey measured 5.8 s cold and 1.2 to 1.3 s warm with Python). The listings cost 53 MiB of private
  bytes, about 150 bytes per media file. Informative: the gate times the import on a quiet machine.

**The coverage** (`cli/media: the real library's media coverage ...`, `test_cli_media_real.cpp`: `library create` with
the four folders into a temporary user data directory, then `media --coverage --json`; 2.5 and 3.0 s, warm, the
gamelists read, the ROM folders listed down the gamelists' folders and the media of 45 systems listed). Every count
equals an independent oracle's (a Python count over the survey's read-only listings of 2026-10-16: ElementTree over the
gamelists, `os.scandir` listings of both trees, its own media-name rule, ES-DE's extensions, `str.upper()` folding;
run once on those machine-local listings, its figures are frozen in the test):

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

**ADR-0003's table**, over the same 40,870 entries (its denominator: every `<game>` entry, those whose file is gone
included):

| ADR-0003's row | ADR-0003 | Measured | Difference (in entries, about: ADR-0003 rounds to 0.1 point) |
|---|---|---|---|
| Screenshot | 98.5% | 98.75% | +0.25 point (+102 entries) |
| Cover | 90.6% | 90.90% | +0.30 point (+123) |
| Marquee | 89.3% | 89.23% | -0.07 point |
| Video | 82.4% | 82.19% | -0.21 point (-85) |
| Physical media image | 81.2% | 81.48% | +0.28 point (+116) |
| Back cover | 77.8% | 77.73% | -0.07 point |
| Manual | 56.6% | 56.55% | -0.05 point |

Three rows are reproduced within 0.1 point; four differ by 0.21 to 0.30 point, and the test allows those four
differences (with 0.02 to 0.05 point to spare) and 0.1 point for the three others. Why they differ:
- The measured figures are not in doubt: the oracle, written independently of the C++ code, gives the same counts
  entry for entry, on all three populations and per system against the survey's own table.
- ADR-0003 (2026-10-15, a day before the survey) does not record its method beyond "matching every entry to
  downloaded_media with ES-DE's naming rule". The data have not changed since: the media tree was last written on
  2026-09-04 and the gamelists on 2026-09-06 (ES-DE has not run since), and the snapshots match the survey's counts.
- No other population or rule gives its rows either: its differences run both ways (cover +123 entries, video -85), so
  neither adding nor removing entries explains them, and the only extension that ES-DE does not read (`.flv`) accounts
  for 4 entries. Over the 41,025 entries `<folder>` ones included (with their media) the shares are 98.47, 90.65,
  88.97, 81.95, 81.23, 77.52 and 56.37%: screenshot, cover and physical media within 0.1 point of ADR-0003, the four
  others 0.2 to 0.45 point off. Counting files per kind instead of entries gives 97.1, 89.0, 87.4, 80.3, 79.7, 76.1 and
  55.7%.
- "Description and metadata" (90.5%) is no media kind: 37,128 of the 40,870 entries have a `<desc>` (90.84%).
So ADR-0003's figures are close approximations whose computation is lost; the figures above are the reproducible ones
(see "Suggested amendments").

## Verification

- `rv_tests "[esde][media],[catalog][coverage],[esde][coverage],[cli][media]"` (14 test cases, 1,460 assertions):
  - the extension lists and their order; the media paths (dotted names, folders, trailing spaces, an empty media name);
  - the standard fixture: every entry of its README's media table, the 12 kinds of Alpha Quest, png before jpg, the
    ignored flv, the folder covers (Hacks, Sports Vol. 2 with its whole name), the namesake without media, the missing
    file's cover, the listings and their issues, the derived paths, path()'s refusals;
  - trees written at test time: system, kind and media names in another case (`GB/COVERS/collection a/...PNG`), every
    image and video extension in ES-DE's order (`.webp` and `.webm` included), manuals in `.PDF`, a folder named like
    a media file, files without an extension, non-ASCII names (é and É), punctuation, a hidden file, a media path past
    MAX_PATH, refresh() after files and a system folder appear;
  - media roots: empty, without kind folders, missing, a file, an unmapped drive letter, unresolved; folders that
    cannot be listed (`ListingDenied`) below a kind folder, a kind folder, a system folder;
  - the coverage domain on a scripted resolver (populations, counts, unknown members, what is asked), the standard
    fixture's 23 entries over the four populations, the media-roots fixture's empty and missing roots;
  - `retroverse-cli media` as text and JSON on both fixtures, its usage errors, `--game`, `--output`, the exit codes,
    and the fixture's files unchanged after it.
- Debug and Release build without a warning; the unit tests (1,027) pass in both.
- The real library: see above.

## For the merge

- **Step 7.** In `tools/cli/src/cmd_media.cpp`, `showCoverage()` makes a `GamelistSource`: replace it with
  `esde::EsdeLibrarySource` (the same `catalog::ILibrarySource::load()` call) and delete `GamelistSource`, `ListedFile`,
  `extensionOf()` and `parentsOf()`. Then: the JSON's `source` becomes the snapshot's (say `rom-scan`), the text's
  "Entries" line and the games table's title lose their "until the ROM scan" notes, and `tests/unit/cli/
  test_cli_media.cpp` expects the games of the snapshot: 15 for the standard fixture (gb 8, snes 5, psx 2, covers 4,
  videos 5: `tests/unit/esde/test_media_coverage.cpp` already checks them on a snapshot built by hand), 3 for the
  media-roots fixture; the media-roots README's last paragraph goes. `test_cli_media_real.cpp` can then check the
  games population over the real library (about 90,000 games).
- **Step 11.** The import asks `presence()` for every entry with a key and stores the presence and extension per kind
  (ADR-0008 §5), keeping what it stored when `presence()` is `Unavailable`; `media --game <id>` reads them back and
  derives the paths with `path()`. One `EsdeMediaResolver` per import, `refresh()` (or a new one) at the next.
- **Step 13.** The app opens media through `core::extendedLengthPathBelow(media root, mediaRelativePath(...))` (names
  that end with a dot or a space).
- **Shared documentation.** CLAUDE.md (the helpers list): `rv/esde/media_resolver.hpp` and `rv/catalog/coverage.hpp`
  as above; the roadmap's step 8 row: done, with the figures above; architecture, `rv_esde`: the media resolver on
  `core::walkTree`, and `rv_catalog`: the coverage service. The standard fixture's README lists the video extensions
  ES-DE reads as `.avi .mkv .mov .mp4 .wmv`: ES-DE 3.4.1 reads `.mp4 .mkv .avi .wmv .mov .webm` (and images `.png .jpg
  .webp`), a parenthesis to correct when the README is next edited.

## Suggested amendments

- **ADR-0008 §5** could name the extension lists and their order as verified (images `.png .jpg .webp`, videos `.mp4
  .mkv .avi .wmv .mov .webm`, manuals `.pdf`) and the availability rule (a missing or unreachable media root, or a media
  folder that cannot be listed, is unknown, never "no media").
- **ADR-0003's table** cannot be reproduced to 0.1 point for four of its seven media rows (see above); its method is not
  recorded. The figures measured here, which an independent count of the survey's listings gives entry for entry, could
  replace it, with the denominator named (every `<game>` entry, 40,870).
