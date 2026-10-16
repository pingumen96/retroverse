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
  - `gamelistPath(esdeHome, system)`: `<ES-DE home>/gamelists/<system>/gamelist.xml`, the only place gamelists are
    read from.
  - The issue codes it reports (`kGamelistIssue...`, below) and `kMaxGamelistBytes` (64 MiB).
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
for (const rv::esde::GamelistEntry& entry : gamelist->entries) {
    // entry.outsideSystemFolder(): CatalogEntry{state OutsideSystemFolder, key empty, gamelistPath = entry.path}.
    // Otherwise match entry.key.key() with the key of a listed file (makeLookupKey of its relative path): the file's
    // relative path is the one listed on disk, which may differ from entry.key.relativePath() in case only.
}
snapshot.issues.insert(snapshot.issues.end(), gamelist->issues.begin(), gamelist->issues.end());
```

A `GamelistEntry` fills the gamelist side of a `catalog::CatalogEntry`: `gamelistPath` = `path`, `metadata`, `raw`,
`flags`, `altEmulator`; the title is `catalog::defaultTitle(metadata, relativePath)` for a game (its `<name>`, else the
file's stem). `line` is there for the issues step 7 reports about an entry (a `<folder>` entry that names no folder,
an extension the system does not configure). `readGamelist` neither sleeps nor retries: the caller chooses the delay
and may read several systems' gamelists in parallel (no shared state; spdlog only at debug level).

## Issues

Every issue names the system (`Issue::system`), the gamelist file (`Issue::file`) and, in its reason, the line of the
element concerned; values and paths are quoted with control characters as `\xHH` and cut after 120 bytes.

| Code | Severity | When | What becomes of it |
|---|---|---|---|
| `gamelist-entry-without-path` | warning | a `<game>` or `<folder>` without `<path>` text (missing, empty, blanks) | skipped, as ES-DE skips it |
| `gamelist-path-malformed` | warning | `core::resolveRelativePath` refuses the path as malformed (control characters, `..` even inside the folder, `:` in a name, `C:a.gb`, `/a.gb`, `\\?\`, the folder itself) | skipped |
| `gamelist-path-outside-folder` | warning | the path leads out of the ROM folder (`./../snes/x.sfc`, an absolute path elsewhere, another drive or share) | kept without a key: `EntryState::OutsideSystemFolder` |
| `gamelist-duplicate-path` | warning | a later entry names the same file (same lookup key; outside the folder, the same path as written) | the earlier one is dropped (see the decisions) |
| `gamelist-value-unreadable` | info | a normalized value that does not parse (`<rating>4/5</rating>`) | kept as written in `raw.originalForms`, no value (a boolean false, a count 0); one issue per element name with the count, the first value and its line |
| `gamelist-ignored-element` | info | what ES-DE ignores: a modelled element repeated in an entry, other elements inside `<gameList>`, top-level elements besides the first `<gameList>` and `<alternativeEmulator>`, an `<alternativeEmulator>` without a label, other elements inside it | dropped; one issue per kind and element name with the count and the first line |
| `gamelist-read-leniently` | warning | the loader read bytes that are not UTF-8 as Windows-1252, or references to no character as U+FFFD | read as ES-DE reads it; one issue per kind, the loader's message |

Order: the file's lenient reading first, then the others in document order (a counted issue at its first element).
Errors are not issues: a gamelist that cannot be read at all is the caller's `gamelist-failed` after its retry.

## Decisions taken within the step

1. **The last entry of a file counts, not the first** (the brief said "the first entry wins"). ES-DE 3.4.1's
   `GamelistFileParser::parseGamelist` applies every entry in turn: `findOrCreateFile` returns the file's existing
   `FileData` and `MetaDataList::createFromXML` overwrites its metadata, so the last entry's metadata is what ES-DE
   shows; and `updateGamelist`, when it saves a game's new play count or edited metadata, removes the first node whose
   path matches and appends the updated node at the end of `<gameList>`. With a duplicate, the first entry is the stale
   one: keeping it would show older metadata than ES-DE after every game played. The earlier entries are reported
   (`gamelist-duplicate-path`). The real gamelists hold no duplicate (exactly or case-insensitively), so the choice is
   about parity, not about data on the reference machine.
2. **Duplicates by lookup key, across kinds, in document order.** Two paths that differ in case only name one file on
   NTFS and exFAT: one entry, the last (ES-DE would show two entries, the one spelled like the file with its own
   entry's metadata; ADR-0008 §3 merges them). A `<game>` and a `<folder>` with the same path are duplicates too:
   ES-DE reads all `<game>` entries before the `<folder>` ones and skips the one whose tag does not match the file's
   type; the parser keeps one entry per key in document order and step 7 checks the kind against the disk as it does
   for every entry. Entries outside the folder have no key and are compared by their path as written (the snapshot
   keeps one per system and gamelist path).
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
   duplicates) stay one issue per entry: each is an entry skipped, superseded or kept outside its folder.
7. **The alternative emulator as written**: the first top-level `<alternativeEmulator>`, before or after `<gameList>`,
   its first `<label>`; an empty label means none. It is not checked against the system's commands (step 10 does, and
   reproduces ES-DE's warning for a stale label). Inside `<gameList>` it is an ignored element.
8. **Failures for the caller to retry**: an empty or blank file, one without `<gameList>` (also `<GameList>`: names
   compare with their case, as in ES-DE), one cut short anywhere or padded with NUL characters, one nested deeper than
   the loader's 64 levels are `ParseError`; a missing file is `NotFound` (absent); the loader's other errors pass
   through. A `<gameList>` without entries is a valid, empty gamelist.
9. **Legacy gamelists**: `gamelistPath()` never points into a ROM folder and nothing else names a gamelist file, so a
   `gamelist.xml` inside a ROM folder (`LegacyGamelistFileLocation` on, which Retroverse does not support: ADR-0008 §3)
   is never opened. The standard fixture's test holds `roms/gb/gamelist.xml` locked without sharing while it reads gb's
   gamelist.
10. **The XML loader gained `XmlDocument::line()`** (above): an additive change to step 0b's private loader, the only
    file outside this step's stubs that the step touches.

## Verification

- `rv_tests "esde/gamelist*"`: the standard fixture's three gamelists (11 `<game>` and 3 `<folder>` entries, every
  case of its README: snes' alternative emulator, the folder entries, the missing file kept as an entry, the unknown
  `<region>`, the non-ASCII Café Racer, `&amp;` and the double-escaped `&amp;amp;`, Café Racer's `<altemulator>`, the
  case-only Delta Drift whose key is its file's, the entry outside gb's folder, the lines of the entries) and the legacy
  gamelist never opened; generated gamelists: paths (relative forms, absolute inside with another case, outside, `..`,
  other drives and shares, malformed forms, empty paths), duplicates (exact, case-only, across kinds, outside the
  folder), failures (missing, empty, blank, BOM only, no `<gameList>`, every truncation of a gamelist, NUL padding and
  holes, the size cap, a folder, a relative ROM folder), an empty `<gameList>`, the alternative emulator's forms, the
  values of real gamelists (year-only dates, `2-3000`, `8+`, ratings in tenths and in hundredths, trailing spaces, line
  feeds inside values, blank lines, empty elements) and unreadable ones, unknown, repeated and stray elements, CDATA,
  encodings (UTF-8 BOM with CRLF, Windows-1252 bytes, UTF-16 with characters beyond the BMP, character references), a
  4 MiB field at the cap and one byte over it, control characters in text and paths, nesting at and beyond the
  loader's limit, messages that quote long paths.
- Real library (`./scripts/real-library.ps1` with the ES-DE home, install, ROM root and media root of the reference
  machine, Release, ES-DE closed): the 45 gamelists read with 40,870 `<game>` and 155 `<folder>` entries, 6 with an
  alternative emulator, 0 failures, 0 entries outside their folder, 0 issues (no duplicate, no malformed path, every
  value parses), every `<game>` and `<folder>` element of the XML outline an entry; read in 0.63 s (34 MiB in the file
  cache: the snapshot had just hashed them; informative, the gate times the import). The snapshots before and after:
  0 differences among 542,082 entries.
- Debug and Release build without a warning; the unit tests (848) pass in both.

## For the shared documentation

- CLAUDE.md, the core and catalog helpers list: `rv/esde/gamelist.hpp` (`readGamelist`, `gamelistPath`; NotFound means
  no gamelist, any other error is retried; issue codes `gamelist-*`; the last entry of a file counts).
- Roadmap, step 3: done, with the census above.
- Architecture, `rv_esde`: the gamelist reader on the private XML loader, which counts lines per block.
