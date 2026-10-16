# I2 step 3: gamelists

ES-DE's `gamelist.xml` files read into the catalog's values (ADR-0008 §3, §5, §7). Step 7 (the ROM scan and the
library snapshot) merges what this step reads with the files it lists; nothing here looks at the disk beyond the
gamelist file itself, and nothing builds a snapshot.

## What was built

- `rv/esde/gamelist.hpp` (`rv_esde`, `src/esde/src/gamelist.cpp`):
  - `readGamelist(file, system, romFolder, maxBytes = kMaxGamelistBytes)` reads one system's gamelist into a
    `Gamelist`: the top-level `<alternativeEmulator>` label, the `<game>` and `<folder>` entries and the issues.
  - `GamelistEntry`: the kind (`GamelistEntryKind::Game`, `Folder`), the `<path>` as written, the path resolved
    against the ROM folder with its lookup key (`catalog::LookupKey`; empty for an entry outside the system folder,
    `outsideSystemFolder()`), `catalog::GameMetadata`, `catalog::RawEsdeValues`, `catalog::EsdeFlags`, the
    `<altemulator>` label and the entry's line in the file.
  - `entryForListed(sameKey, listed, kind)`: of the entries of one lookup key, the one that applies to a file or folder
    the ROM scan lists, as ES-DE applies them (decision 2).
  - `gamelistPath(esdeHome, system)`: `<ES-DE home>/gamelists/<system>/gamelist.xml`, the only place gamelists are
    read from.
  - The issue codes it reports (`kGamelistIssue...`, below), `kMaxGamelistBytes` (64 MiB) and the bounds on what one
    file can make (decision 11): `kGamelistMaxPathUtf16Units` (32,767), `kGamelistIssuesListedPerCode` (100),
    `kGamelistNamesCountedPerKind` (16), `kGamelistUnknownElementsPerEntry` (256).
- The XML loader (`src/esde/src/xml.hpp`, private): `XmlDocument::line(node)`, the line where a node starts. The
  text's line feeds are counted per 1 KiB block when it is parsed, so a line costs at most a 1 KiB count whatever the
  node's offset (`locate()` counted from the start of the text on every call: quadratic over a gamelist's entries).
  `locate()` uses the same count; its output is unchanged.
- Tests: `tests/unit/esde/test_gamelist.cpp` (the standard fixture and gamelists generated at test time) and
  `tests/unit/esde/test_gamelist_real.cpp` (hidden, `[.real-library]`: the census of the real gamelists).

## How step 7 uses it

```cpp
const std::filesystem::path file = rv::esde::gamelistPath(profile.esdeHome, system.name);
auto gamelist = rv::esde::readGamelist(file, system.name, system.romFolder);
if (!gamelist) {
    // NotFound: the system has no gamelist (GamelistState::Absent, its files unscraped).
    // Anything else (ParseError, IoError, Conflict, Unavailable, OutOfRange, InvalidArgument): read again after a
    // delay, then GamelistState::Failed with an issue; the import keeps the system's previous rows (ADR-0008 §7).
}
system.alternativeEmulator = gamelist->alternativeEmulator;
// The entries by key, each key's in the order of gamelist->entries: several when they differ in kind or in case.
std::unordered_map<std::string, std::vector<const rv::esde::GamelistEntry*>> byKey;
for (const rv::esde::GamelistEntry& entry : gamelist->entries) {
    if (entry.outsideSystemFolder()) {
        // CatalogEntry{state OutsideSystemFolder, key empty, gamelistPath = entry.path}.
    } else {
        byKey[entry.key.key()].push_back(&entry);
    }
}
for (const auto& listed : listing) { // a file or folder: its key (makeLookupKey of its relative path on disk), and
                                     // Game or Folder as ES-DE types it
    const auto found = byKey.find(listed.key.key());
    const rv::esde::GamelistEntry* entry =
        found == byKey.end() ? nullptr : rv::esde::entryForListed(found->second, listed.key, listed.kind);
    // nullptr: no metadata. entry->kind != listed.kind: skipped with an issue, as ES-DE skips a "Game/folder
    // mismatch". Otherwise the file takes the entry's values; its relative path is the one listed on disk.
}
// A key that no listed file has: its <game> entries name a missing file (the last of them), a <folder> entry a folder
// that is not there (an issue, nothing kept).
snapshot.issues.insert(snapshot.issues.end(), gamelist->issues.begin(), gamelist->issues.end());
```

A `GamelistEntry` fills the gamelist side of a `catalog::CatalogEntry`: `gamelistPath` = `path`, `metadata`, `raw`,
`flags`, `altEmulator`; the title is `catalog::defaultTitle(metadata, relativePath)` for a game (its `<name>`, else the
file's stem). `line` is there for the issues step 7 reports about an entry (a `<folder>` entry that names no folder,
an extension the system does not configure). The entries of one key that the scan does not use are already reported
(`gamelist-duplicate-path`). `readGamelist` neither sleeps nor retries: the caller chooses the delay and may read
several systems' gamelists in parallel (no shared state; spdlog only at debug level, decision 13).

## Issues

Every issue names the system (`Issue::system`), the gamelist file (`Issue::file`) and, in its reason, the line of the
element concerned; values, paths and element names are quoted with control characters as `\xHH` and cut after 120
bytes.

| Code | Severity | When | What becomes of it |
|---|---|---|---|
| `gamelist-entry-without-path` | warning | a `<game>` or `<folder>` without `<path>` text (missing, empty, blanks) | skipped, as ES-DE skips it |
| `gamelist-path-malformed` | warning | `core::resolveRelativePath` refuses the path as malformed (control characters, `..` even inside the folder, `:` in a name, `C:a.gb`, `/a.gb`, `\\?\`, the folder itself), or it is longer than 32,767 UTF-16 code units (checked before it is resolved) | skipped |
| `gamelist-path-outside-folder` | warning | the path leads out of the ROM folder (`./../snes/x.sfc`, an absolute path elsewhere, another drive or share) | kept without a key: `EntryState::OutsideSystemFolder` |
| `gamelist-duplicate-path` | warning | a later entry of the same kind and relative path (outside the folder: the same path as written); or an entry of the same key that differs from an earlier one in kind or in case | the later one replaces the earlier one in its place; entries that differ in kind or case are all kept for the scan to choose (decision 2) |
| `gamelist-value-unreadable` | info | a normalized value that does not parse (`<rating>4/5</rating>`) | kept as written in `raw.originalForms`, no value (a boolean false, a count 0); one issue per element name with the count, the first value and its line |
| `gamelist-ignored-element` | info | what ES-DE ignores: a modelled element repeated in an entry, other elements inside `<gameList>`, top-level elements besides the first `<gameList>` and `<alternativeEmulator>`, an `<alternativeEmulator>` without a label, other elements inside it; unknown elements of an entry beyond the first 256 | dropped; one issue per kind and element name with the count and the first line, the names of a kind beyond the first 16 counted together |
| `gamelist-read-leniently` | warning | the loader read bytes that are not UTF-8 as Windows-1252, or references to no character as U+FFFD | read as ES-DE reads it; one issue per kind, the loader's message |

The four codes about single entries (`entry-without-path`, `path-malformed`, `path-outside-folder`, `duplicate-path`)
are listed up to 100 times each; the others of the code are counted into one more issue of it, "N more issue(s) of
this kind, not listed one by one after the first 100; the last at line L". Entries are kept or skipped all the same: an
entry outside its folder whose issue is not listed is in `entries`. Order: the file's lenient reading first, then the
others in document order (a counted issue at its first element), then those counts. Errors are not issues: a gamelist
that cannot be read at all is the caller's `gamelist-failed` after its retry.

## Decisions taken within the step

1. **The last entry of a file counts, not the first** (the brief said "the first entry wins"). ES-DE 3.4.1's
   `GamelistFileParser::parseGamelist` applies every entry in turn: `findOrCreateFile` returns the file's existing
   `FileData` and `MetaDataList::createFromXML` overwrites its metadata, so the last entry's metadata is what ES-DE
   shows; and `updateGamelist`, when it saves a game's new play count or edited metadata, removes the first node whose
   path matches and appends the updated node at the end of `<gameList>`. With a duplicate, the first entry is the stale
   one: keeping it would show older metadata than ES-DE after every game played. The earlier entries are reported
   (`gamelist-duplicate-path`). The real gamelists hold no duplicate (exactly or case-insensitively), so the choice is
   about parity, not about data on the reference machine.
2. **One entry per kind and relative path; the entries of one file that differ in kind or in case are all kept.**
   ES-DE applies an entry only to the file spelled exactly like its path (`findOrCreateFile` looks each element up by
   its exact name; another spelling makes a second, phantom entry for a `<game>` and nothing for a `<folder>`), and only
   when its tag matches the file's type (it reads every `<game>` before any `<folder>` and skips a "Game/folder
   mismatch"). The parser therefore merges only entries of the same kind and relative path (written alike or not:
   `./a.gb`, `.\a.gb`, an absolute path inside the folder), the last one taking the place of the first, and keeps every
   entry that shares a lookup key with another but differs from it in kind or in case: only the disk tells which one
   ES-DE applies, and the scan knows the disk. `entryForListed()` chooses for a listed file: the entry of its kind
   spelled exactly like it (the one ES-DE applies); else the last one of its kind (a case-only difference matches the
   file, ADR-0008 §3, where ES-DE would show a second entry); else the last one, of the other kind, which the scan
   skips with an issue as ES-DE skips it. On a case-sensitive NTFS directory, two files that differ in case each find
   their own entry. Entries outside the folder have no key and are compared by their path as written, whatever their
   kind (ES-DE skips them all; the snapshot keeps one per system and gamelist path). The first version kept one entry
   per key across kinds: a `<folder>` entry followed by a `<game>` entry of the same directory lost the folder's
   metadata, which ES-DE shows (review finding F1).
3. **Values as ES-DE reads them.** An element's text is its first text or CDATA child (`pugi::xml_node::text()`),
   decoded, never trimmed (`displayText()` cleans it where it is shown); an element without text, blanks included
   (pugixml drops whitespace-only text), reads as a missing one, its original form kept as `""`; of a modelled element
   repeated in an entry the first counts; attributes and child elements of unknown elements are not kept (step 1's
   `RawElement`). Booleans are true only for `true`, unreadable counts are 0, as ES-DE reads them.
4. **What is modelled is step 1's list** (`catalog::isModelledGamelistElement`): ES-DE's own `<controller>`,
   `<collectionsortname>`, `<nogamecount>`, `<nomultiscrape>`, `<hidemetadata>` and `<folderlink>` are unknown elements,
   kept with their text. Folders are read with the same fields as games: ES-DE 3.4's folder metadata has no
   `sortname`, `collectionsortname`, `nogamecount`, `playcount`, `playtime` or `altemulator` (none occur in a real
   `<folder>`), but the catalog models a folder entry like a game's.
5. **Paths through `core::resolveRelativePath` (step 0b), with its deliberate differences from ES-DE**: a path without
   `./` is relative to the ROM folder (ES-DE leaves it relative to its working directory, so it finds nothing and skips
   the entry); an absolute path inside the folder compares as the file system compares names (ES-DE compares bytes);
   `..` is refused even inside the folder (ES-DE makes a folder named `..`); `~` is a name (ES-DE expands it in no
   gamelist path either). All 41,025 real paths are `./`-relative with forward slashes.
6. **Unreadable values and ignored elements are counted per element name**, one issue each: a scraper that writes
   another format for one field would otherwise flood the import's issues with one per game. Per-entry problems (paths,
   duplicates) stay one issue per entry, up to 100 per code (decision 11): each is an entry skipped, superseded or kept
   outside its folder.
7. **The alternative emulator as written**: the first top-level `<alternativeEmulator>`, before or after `<gameList>`,
   its first `<label>`; an empty label means none. It is not checked against the system's commands (step 10 does, and
   reproduces ES-DE's warning for a stale label). Inside `<gameList>` it is an ignored element.
8. **Failures for the caller to retry**: an empty or blank file, one without `<gameList>` (also `<GameList>`: names
   compare with their case, as in ES-DE), one cut short anywhere or padded with NUL characters, one nested deeper than
   the loader's 64 levels are `ParseError`; a missing file is `NotFound` (absent); the loader's other errors pass
   through, and running out of memory is `OutOfRange` (decision 11). A `<gameList>` without entries is a valid, empty
   gamelist.
9. **Legacy gamelists**: `gamelistPath()` never points into a ROM folder and nothing else names a gamelist file, so a
   `gamelist.xml` inside a ROM folder (`LegacyGamelistFileLocation` on, which Retroverse does not support: ADR-0008 §3)
   is never opened. The standard fixture's test holds `roms/gb/gamelist.xml` locked without sharing while it reads gb's
   gamelist.
10. **The XML loader gained `XmlDocument::line()`** (above): an additive change to step 0b's private loader, the only
    file outside this step's stubs that the step touches.
11. **What one file can make is bounded** (review finding S3-R1). Within the 64 MiB cap a broken gamelist can hold
    millions of entries, and the first version made an issue of each (about 500 bytes apiece, 85 times the file:
    5.8 GiB for 67 MB of `<game/>`), kept every superseded entry until the end and then copied the entries again, and
    counted every distinct stray element name apart. Now issues about single entries are listed up to
    `kGamelistIssuesListedPerCode` (100) per code and counted beyond; the ignored elements of a kind that can bear any
    name are counted apart for its first `kGamelistNamesCountedPerKind` (16) names, then together; an entry superseded
    by a later one is replaced by it in its place as it is read, and document order is restored at the end by an
    in-place permutation, only when a superseded entry was not the last one kept; unknown elements are kept up to
    `kGamelistUnknownElementsPerEntry` (256) per entry (real gamelists hold none, legacy Skraper ones a few dozen),
    further ones counted as ignored. What does not become an entry now costs no more than its XML tree (pugixml: 64
    bytes per node); what does is the caller's data. Running out of memory anyway (a machine with little free memory)
    is `OutOfRange`, "out of memory while reading the gamelist", never an exception through the module boundary (as the
    asset loaders do). Measured in Release with the reviewer's probe, one process per file (peak commit charge):

    | File | Before | After |
    |---|---|---|
    | 30 MiB of `<game/>` | 4.4 s, 2.6 GiB, 4,493,891 issues | 1.8 s, 475 MiB, 101 issues |
    | 67,000,000 bytes of `<game/>` | 11.6 s, 5.8 GiB | 3.6 s, 1,025 MiB (the XML tree), 101 issues |
    | 30 MiB of distinct stray elements | 5.7 s, 2.6 GiB | 0.9 to 1.3 s, 336 MiB, 17 issues |
    | 30 MiB of one entry repeated | 9.3 s, 1.7 GiB | 8.0 s, 272 MiB, 1 entry, 101 issues |
    | 30 MiB of one entry's unknown elements | 1.36 GiB | 1.5 s, 750 MiB, 256 kept |
    | 30 MiB of distinct paths outside the folder | 8.5 s, 1.7 GiB | 7.6 s, 1,092 MiB: 876,899 entries kept |
    | 30 MiB of minimal valid entries | 6.5 to 6.8 s, 1,131 MiB | 6.1 to 6.6 s, 1,102 MiB: 809,445 entries |
    | realistic 30 MiB gamelist | 0.6 s, 180 MiB | 0.5 s, 168 MiB: 34,525 entries |

    The last three are entries the caller asked for, about 1.4 KB per minimal entry (the domain's values and the
    vector's growth); a real gamelist costs about five times its size.
12. **A path longer than any path Windows can name is refused before it is resolved** (review finding S3-R2).
    `core::resolveRelativePath` folds every element of a path and keeps them all, about 48 bytes per byte: one 30 MiB
    `<path>` cost 5 s and 1.4 GiB and gave an entry whose key could never match a file. A path of more than 32,767
    UTF-16 code units (`kGamelistMaxPathUtf16Units`, the longest Windows names in its `\\?\` form) names nothing, and
    ES-DE skips its entry (`exists()` fails), so it is `gamelist-path-malformed` and skipped: 0.7 s and 91 MiB for the
    same file. The count is of UTF-16 code units, not bytes (a character beyond U+FFFF counts two). Names longer than
    255 code units are not refused: the bound on the whole path is what bounds the cost, and such an entry is, for the
    scan, a file that is not there.
13. **What was read leniently is logged at debug level only** (review findings S3-R3, F2). `readGamelist` reads the
    file with `core::readFileShared` and parses it with `detail::parseXml` rather than through `detail::loadXmlFile`,
    which logs the loader's warnings at warning level with the file's path, `C:\Users\<name>\ES-DE\gamelists\...`,
    against `catalog::Issue::file` ("the user's own path: never logged above debug level by the product"). The
    warnings reach the caller as `gamelist-read-leniently` issues, as before, and the log has them at debug level.

## Review

The committed step had two reviews; every finding was verified (the memory figures reproduced on a 10 MB file: 764 MiB
for `<game/>`, 613 MiB for one entry repeated) and fixed:

- S3-R1 (medium, unbounded issues and duplicates): decision 11. The suggested fix released a superseded entry and
  compacted the vector at the end; the entries kept their slots until then, so a file of one entry repeated would
  still have held a slot per repetition. The superseding entry takes the earlier one's place instead, which keeps one
  slot per kind and path. The optional cap on unknown elements per entry is in (256).
- S3-R2 (low, huge paths resolved): decision 12. The optional refusal of names over 255 code units is not done (above).
- S3-R3 and F2 (low, the same finding: the lenient reading logged with the user's path at warning level): decision 13.
- F1 (low, duplicates across kinds and spellings decided by document order): decision 2 and `entryForListed()`.

## Verification

- `rv_tests "esde/gamelist*"`: the standard fixture's three gamelists (11 `<game>` and 3 `<folder>` entries, every
  case of its README: snes' alternative emulator, the folder entries, the missing file kept as an entry, the unknown
  `<region>`, the non-ASCII Café Racer, `&amp;` and the double-escaped `&amp;amp;`, Café Racer's `<altemulator>`, the
  case-only Delta Drift whose key is its file's, the entry outside gb's folder, the lines of the entries) and the legacy
  gamelist never opened; generated gamelists: paths (relative forms, absolute inside with another case, outside, `..`,
  other drives and shares, malformed forms, empty paths, paths at and just over 32,767 UTF-16 code units in ASCII, in
  three-byte characters and in characters beyond U+FFFF, the deepest path), duplicates (exact, written otherwise,
  case-only, across kinds, outside the folder, interleaved so that document order must be restored) and the entry a
  listed file or folder takes (`entryForListed`: exact spelling, case-only, either kind, a kind mismatch, no entry),
  failures (missing, empty, blank, BOM only, no `<gameList>`, every truncation of a gamelist, NUL padding and holes,
  the size cap, a folder, a relative ROM folder), an empty `<gameList>`, the alternative emulator's forms, the values
  of real gamelists (year-only dates, `2-3000`, `8+`, ratings in tenths and in hundredths, trailing spaces, line feeds
  inside values, blank lines, empty elements) and unreadable ones, unknown, repeated and stray elements, CDATA,
  encodings (UTF-8 BOM with CRLF, Windows-1252 bytes, UTF-16 with characters beyond the BMP, character references), a
  4 MiB field at the cap and one byte over it, floods kept bounded (150,000 entries without a path: 101 issues; one
  entry 300 times: 1 entry and 101 issues; 150 entries outside the folder: all kept, 101 issues; 40 stray names of
  each kind: 16 apart and the rest together; 300 unknown elements in one entry: 256 kept), control characters in text
  and paths, nesting at and beyond the loader's limit, messages that quote long paths, and the log of a leniently read
  file (its path only at debug level).
- Real library (`./scripts/real-library.ps1` with the ES-DE home, install, ROM root and media root of the reference
  machine, Release, ES-DE closed): the 45 gamelists read with 40,870 `<game>` and 155 `<folder>` entries, 6 with an
  alternative emulator, 0 failures, 0 entries outside their folder, 0 issues (no duplicate, no malformed path, every
  value parses), every `<game>` and `<folder>` element of the XML outline an entry; read in 0.47 to 0.63 s over two
  runs (the files in the cache: the snapshot had just hashed them; informative, the gate times the import). The snapshots before and after:
  0 differences among 542,082 entries.
- Debug and Release build without a warning; the unit tests (852) pass in both.

## For the shared documentation

- CLAUDE.md, the core and catalog helpers list: `rv/esde/gamelist.hpp` (`readGamelist`, `gamelistPath`,
  `entryForListed`; NotFound means no gamelist, any other error is retried; issue codes `gamelist-*`, those about
  single entries listed up to 100 per code; of the entries of one kind and path the last counts, entries of one file
  that differ in kind or case are all kept for the scan to choose).
- Roadmap, step 3: done, with the census above.
- Architecture, `rv_esde`: the gamelist reader on the private XML loader, which counts lines per block.
- `detail::loadXmlFile` (step 0b) still logs the loader's warnings at warning level with the file's path; product code
  that reads files under the user's folders (ES-DE's home, its install) should parse through `core::readFileShared` and
  `detail::parseXml` as `readGamelist` does, or the loader should log at debug level: a one-line change in 0b's file,
  left to the merge because other steps of the wave may call it.
