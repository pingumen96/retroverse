# ADR-0008: The ES-DE catalog: what is a game, identities, storage, configuration and launch translation

- Status: accepted
- Date: 2026-10-16
- Amended: 2026-10-16, at I2 step 0a: the system probe's port lives in `rv_core`, not in `rv_catalog` (§2)
- Amended: 2026-10-16, at I2 step 0b: §1 names the executables that declare `longPathAware`; §6 names the install
  folder and links in the data-directory rule; §7 says how ES-DE's XML files are decoded

## Context

M1 (specification §22) reads an existing ES-DE collection without moving anything, indexes it for search, and launches
a selected title through an external emulator and back. I1 built the launch mechanics (ADR-0006) and the product UI
(ADR-0005); ADR-0003 keeps ES-DE as the only scraper. A read-only survey of the reference machine, then an adversarial
review of this decision against it, found:

- ES-DE 3.4.1 with its home in `%USERPROFILE%\ES-DE`, its install found through the uninstall registry key, a ROM root
  on an external exFAT drive and a media root elsewhere. `es_settings.xml` has 169 sibling elements and no root
  element, and it holds the ScreenScraper user name and password in plain text.
- 45 gamelists with 40,870 `<game>` and 155 `<folder>` entries; 6 of them hold `<alternativeEmulator>` beside
  `<gameList>`. 1,038 entries name a file that no longer exists (recompressed, moved or deleted); 837 more name
  existing ps2 `.cue` files, an extension ps2 does not configure. ES-DE rewrites a gamelist in place after every game
  played from it.
- ES-DE itself shows 103,067 games: every file whose extension `es_systems.xml` configures for the system (compared
  case-sensitively, both cases being listed), in every sub-folder, hidden files included here; 64,072 have no gamelist
  entry, and psx shows 15,339 entries for 2,518 discs because every `.bin` track of a `.cue` is listed. MAME BIOS and
  device sets are skipped. Files are byte-alike in bulk: 56,084 games share their system, size and modification time
  with another game, and 7,881 their system, size and file name.
- Media follow `<media>/<system>/<kind>/<path relative to the system's ROM folder, last extension removed>.<ext>`,
  sub-folders mirrored with their case; 11 kinds are present, 364,810 files. Three ROM paths reach 260 characters or
  more. Listing the ROM tree takes 10 s cold and 0.7 s warm.
- `es_systems.xml` uses 15 tokens; `%EMULATOR_X%` and `%CORE_X%` are resolved through `es_find_rules.xml`. RetroArch is
  found only through App Paths here. Three commands were edited by hand to absolute executables on a drive that no
  longer exists; the ROM tree also holds 490 emulator binaries, some named like find-rule entries (DuckStation, Mesen,
  mGBA, snes9x, Dolphin).
- ES-DE logged five launches on this machine (RetroArch with bsnes, mesen, mednafen_psx and gambatte); they are frozen
  as machine-local test data, since ES-DE rotates its log.

The specification proposes pugixml, SQLite and toml++ (§3.1), TOML configuration files (§16.1), and an illustrative
schema (§6.3); the original plan added xxhash, utf8proc and CLI11.

## Decision

### 1. Libraries

SQLite (3.53.4, vcpkg features `fts5` and `json1`) and pugixml (1.16, always with `parse_fragment`, never XPath, which
throws). Not toml++: configuration stays JSON like every other configuration file of the project (a departure from
specification §3.1 and §16.1). Not CLI11: `retroverse-cli` dispatches subcommands on `rv::core::ArgParser`. Not xxhash or
utf8proc: identities hash with SHA-256 implemented in `rv_core`, paths fold case the way the Windows file system does,
and search folds diacritics in FTS5's `unicode61` tokenizer. The executables that handle library paths declare
`longPathAware`: `retroverse`, `retroverse-cli`, the test programs (`rv_tests`, `rv_platform_tests`, `rv_smoke_tests`)
and the fake emulator (`rv_fake_emulator`); file-system walks and the reads of ES-DE's files use `\\?\` paths, which
reach past MAX_PATH whatever the system's long path setting.

### 2. Modules

All GPU-free and checked by `rv_forbid_link`, which gains per-target forbidden lists (`rv_catalog` may not reach
SQLite, pugixml or Windows-only targets):

- `rv_catalog`: the domain (libraries, systems, games, entry states, media kinds, disc groups, change sets, play
  sessions), one header per port (`ILibrarySource`, `ICatalogStore`, `ICatalogQuery`, `IMediaResolver`), and the
  services that only see ports (import and reconciliation, launch resolution).
- `rv_esde`: the ES-DE adapter (discovery, settings, systems, find rules, gamelists, ROM scan, disc groups, media,
  command translation); pugixml stays private.
- `rv_catalog_db`: the SQLite store and its queries; SQLite stays private.
- `rv_platform_win`: the Windows side that needs no SDL: the process runner moved out of `rv_platform`, and the system
  probe (registry values and App Paths in HKCU then HKLM, the uninstall key, the executable search of point 8, file and
  wildcard checks). The probe's port, `ISystemProbe`, lives in `rv_core` (`rv/core/system_probe.hpp`): `rv_launcher`
  finds emulator executables through it and sits below `rv_catalog`, and `rv_esde` uses it too.
- `tools/cli`: `retroverse-cli` and its GPU-free core. Results go to stdout or `--output` as UTF-8, logs only to stderr
  and the log file.

`rv_app` composes them for the quick catalog.

### 3. What a game is

Every entry of the snapshot has a state: **game**; **missing file** (a gamelist entry whose file is gone); **not a game
for ES-DE** (the file exists but its extension is not configured, a `noload.txt` excludes it, it is a BIOS or device
set, or an ES-DE setting excludes it, with the reason kept); **disc member**; **folder**; **outside the system folder**.
Only a missing file is "unavailable", and only it takes part in reconciliation. Games are what ES-DE would show, so both
frontends agree:

- files whose extension is in the system's list, used verbatim and compared case-sensitively (the list split on blanks
  and commas, `.` meaning "no extension", upstream quirks such as pcfx's `CUE` left as they are);
- in every sub-folder; a directory whose name carries a configured extension is a game, other directories are entered;
  symbolic links and junctions are followed unless they loop (volume serial and file id);
- hidden files and folders (the attribute or a leading dot) as `ShowHiddenFiles` says; `ParseGamelistOnly` and
  `noload.txt` honoured; `flatten.txt` only changes ES-DE's presentation and is ignored;
- MAME BIOS and device sets skipped for the arcade and neogeo platforms, from ES-DE's `resources/MAME` lists, and
  unscraped arcade zips titled from `mamenames.xml`; other unscraped files titled from their cleaned file name;
- gamelist metadata matched to files by path; gamelists inside ROM folders ignored (`LegacyGamelistFileLocation` off).

Deliberate differences from ES-DE: a missing file stays as an unavailable game with its metadata (FR-033), hidden by
default; files that a `.cue` or `.m3u` references are disc members of it, kept and identifiable but hidden, when that
`.cue` or `.m3u` is itself a game of the same system (psx: 12,820 members here; ps2 none, `.cue` not being a ps2
extension); chains (`.m3u` to `.cue` to tracks) and cycles are resolved, names are matched with the key's case folding,
cue sheets are read as UTF-8 with a Windows-1252 fallback, membership is cached by key, size and modification time and
kept when a cue sheet cannot be read, and a head without its own gamelist entry shows the metadata of a member that has
one; a gamelist path that differs from the file only in case matches it (ES-DE would show two entries).

### 4. Identity

- **Library**: a fixed slug chosen once (default `esde`) and stored in the database, which is the authority;
  `library.json` names it, and a mismatch is refused until an explicit re-association.
- **Lookup key**: the path relative to the system's ROM folder, `/` separators, no `./`, upper-cased with the Windows
  file-system casing table (`LCMapStringEx`, `LOCALE_NAME_INVARIANT`, file-system casing) and never Unicode-normalized,
  as NTFS and exFAT compare names. Identities match on the exact stored relative path first, then on the key; the
  casing table's NLS version is stored and keys are re-computed in place when it changes. Two names that fold to one
  key keep distinct identities, disambiguated deterministically and reported as an import issue.
- **GameId**: assigned the first time a key is seen, then persisted. SHA-256 over the UTF-8, NUL-separated tuple
  (`rv-game/1`, library id, ES-DE system name, key); the first 8 bytes big-endian with the top bit cleared, 0 excluded.
  On a collision the tuple gains a counter (1, 2, ...) until the value is free, keys being processed in (system, key)
  ordinal order. Text form: `g` and 16 hexadecimal digits, a valid UI entry id. A new drive letter or ROM root keeps every
  ID. Golden ID tables are built on an empty database and checked by an independent implementation.
- **Fingerprints**: every import stores each file's size and modification time from the listing (free). A quick hash
  (SHA-256 of the first and last 64 KiB) is computed progressively by a resumable, cancellable pass that commits in
  batches, and is stored with the size and time it belongs to; a first import reads no ROM bytes.
- **Reconciliation**: an identity whose file was present at the last import and is gone now, whatever the gamelist
  says, is a candidate, within its library and system. It pairs with a new file whose quick hash equals its stored one;
  without a stored hash, only with a unique new file of equal size, time and file name. Ties go to the equal file name,
  then the longest common path suffix; groups of identical copies pair in key order. Anything else becomes a review row,
  never a silent merge. The moved identity keeps its ID, history and sessions; a gamelist entry left at the old path gets
  a new ID. A file moved back onto a key that has an identity keeps the identity with history and records the other as
  an alias; resolving a review does the same.
- **Availability**: tracked per system folder and per directory. A folder that is missing or cannot be listed keeps its
  rows, marked "source unavailable" with the reason, and stays out of reconciliation (SC-06). A mass disappearance (an
  empty ROM root, or more than half of a system's present files gone) changes nothing and is reported, unless the user
  accepts the removals.

### 5. Storage

- One SQLite database, `<user data>/database/retroverse.sqlite`, WAL, foreign keys, `synchronous=FULL`,
  `journal_size_limit`, a checkpoint after each import.
- **Index tables** (`idx_*`) are a function of the ES-DE snapshot and the identity table and can be rebuilt at any time
  (`db rebuild-index`, tested to give the same index as a refresh); **state tables** (`rv_*`) hold what only Retroverse
  knows: libraries, identities with their last path, history, aliases and fingerprints, review rows, per-game launch
  choices, launch verifications, play sessions. Foreign keys point only from `idx_*` to `rv_*`, with no cascade out of
  the index. Game rows use the GameId as `INTEGER PRIMARY KEY`; user-visible ids of other tables use `AUTOINCREMENT`.
- **Concurrency**: one writer connection per process, used from one thread (the catalog worker in the app, the main
  thread of the CLI); the app's main thread reads through its own read-only connection and never waits on a lock;
  statements are reset after each page; an import lock keeps two imports (also from two processes) from overlapping;
  `sqlite3_interrupt` at shutdown.
- **Migrations**: `PRAGMA user_version`, forward-only, each in one transaction, under a process-level lock that every
  Retroverse process holds while the database is open; before each, a uniquely named backup (the last 3 kept, free space
  checked); table rebuilds follow SQLite's documented procedure. A database newer than the program is refused.
- **Search**: an FTS5 table with external content (`unicode61 remove_diacritics 2`, prefix indexes 2 and 3) over the
  title, sort title, developer, publisher, genre, year and the system's full name; descriptions are not searched in M1.
  The input is split into words with the same tokenizer and becomes one quoted token per word, joined with AND, the last
  one as a prefix; one-character inputs filter without FTS. The index follows the rows through triggers and upserts,
  never `REPLACE`; a full rebuild is a repair, and `db check` runs FTS5's integrity check.
- **Paging**: a query (filter, search, sort) materializes its ordered GameId list in one read; the list's size is the
  count, a page is fetched by ids, and an id's position can be asked. The total order is (sort key, GameId), the sort key
  computed at import by a documented, versioned folding. The list is rebuilt when an import completes and the selection
  restored by GameId. A page fetch on the UI thread stays under 5 ms in Release with 100,000 games; queries run on the
  worker otherwise.
- **Size**: media are stored per game and kind as presence and extension (paths are derived); the raw ES-DE values kept
  are the unknown elements and the original forms of normalized fields.

### 6. Configuration

JSON with comments under `<user data>/config`: `library.json` (the library's id and adapter, the ES-DE home and install
folders, optional ROM and media root overrides) and `emulators.json` (the user's emulator profiles and per-system
choices); shipped defaults under `assets/launcher/`. Choices made through the CLI (`assign`) and per-game choices are
stored in the database, never written into the user's commented files. The user data directory may not lie inside the
ES-DE home, its install folder, the ROM root or the media root, compared as the file system compares names and again
with links, junctions, subst and mapped drives resolved (`rv::core::checkUserDataDirectory`).

### 7. Read-only towards ES-DE

ADR-0003 and NFR-10: the adapter opens ES-DE's files for reading only, with full sharing, and never writes under the
ES-DE home, the install, the ROM root or the media root (reading cue sheets may update last-access times, which is
accepted); `<favorite>`, `<playcount>` and `<lastplayed>` are read, never updated. ES-DE's XML files are decoded as its
pugixml decodes them (UTF-8, UTF-16 or UTF-32 by their byte order mark or first character, a declared ISO-8859-1), and
what it reads leniently is read too, as UTF-8 and reported: bytes that are not UTF-8 as Windows-1252 characters,
character references to no character as U+FFFD. The settings reader extracts only the keys it uses and never reads,
logs or copies the scraper credentials. A gamelist that fails to parse (it is cut short or padded with NUL characters
while ES-DE writes it, among others), is empty, or has no `<gameList>` element is retried after a delay and then keeps
that system's previous rows; on a first import that system is marked "metadata pending" rather than imported
unscraped.

### 8. Launch translation

- **Splitting**: a command is split into argv elements the way `CommandLineToArgvW` splits a command line, before any
  token is expanded, and expanded values are never split again. This gives ES-DE's argv for every command in use here;
  where ES-DE's own argv breaks (48 of 851 upstream templates with unquoted `%BASENAME%` and names containing spaces),
  Retroverse's stays correct.
- **The program** is decided on the unexpanded template: it must be exactly one `%EMULATOR_X%` element or a literal
  absolute path with no token other than `~` and `%ESPATH%`. Shells and launchers are refused by file name even when a
  find rule resolves them (cmd, powershell, pwsh, wscript, cscript, mshta, rundll32, explorer, conhost, wt, wsl, bash,
  msiexec, regsvr32), and the program must pass `validateLaunchRequest`'s checks (a PE `.exe` or `.com`).
- **Tokens**: `%ROM%` (built as ES-DE builds it: `ROMDirectory` as configured, the system's `<path>` with `%ROMPATH%`
  expanded, the relative path as listed on disk), `%ROMRAW%`, `%ROMPATH%`, `%BASENAME%`, `%FILENAME%`, `%GAMEDIR%`,
  `%GAMEDIRRAW%`, `%EMUDIR%`, `%ESPATH%`, `%STARTDIR%=` (the working directory), `%HIDEWINDOW%` (a console of its own
  hidden, `CREATE_NO_WINDOW`, where ES-DE hides the window: the same for the console programs that use it), and
  `%INJECT%=`: the element, or its glued prefix's value, is dropped when the file (relative to the game's folder, with
  `%BASENAME%` expanded) is missing, empty or over 4,096 bytes, as ES-DE then injects nothing; a usable file makes the
  game "requires launcher configuration" (conservative). Anything else (`%RUNINBACKGROUND%`, `%PRECOMMAND_X%`,
  `%RPCS3_GAMEID%` and other unknown `%X%` text, placeholder commands, a static path with `|`) makes the game "requires
  launcher configuration" with the token named; nothing runs.
- **Find rules**: in ES-DE's order: App Paths (HKCU, then HKLM when the HKCU key is absent), registry values (the value
  plus the suffix appended verbatim), the executable search, static paths (`~` is the ES-DE home, `%ESPATH%` the install
  folder, a wildcard only in the last component with matches taken in sorted order), and the core path
  (`%EMUPATH%\cores`); `custom_systems/es_find_rules.xml` is read first and its emulators replace the bundled ones. The
  executable search looks in the ES-DE install folder, System32, the Windows folder and the absolute entries of PATH,
  never in Retroverse's folder or the current directory (the ROM tree here holds emulators named like find-rule entries).
  `%ESPATH%` with an unknown install folder makes the game "requires launcher configuration".
- **Working directory** without `%STARTDIR%`: the emulator's folder (ADR-0006). ES-DE leaves emulators in its own
  current directory (its install folder); the difference keeps emulators launched by Retroverse from writing there.
- **Choice of command**, most specific first: Retroverse's choice for the game, the game's `<altemulator>` (only with
  `AlternativeEmulatorPerGame`), Retroverse's choice for the system, the gamelist's `<alternativeEmulator>`, the first
  `<command>`. Choices are stored as a system and an ES-DE command label, or as a Retroverse profile id, and translated
  again from the current `es_systems.xml` at every refresh and launch; a stored label that no longer exists makes the
  game "requires launcher configuration" with the label named, while ES-DE's own stale labels fall back to the first
  command with ES-DE's warning, as ES-DE does.
- **Readiness**, the first that applies: source unavailable, ROM missing, requires launcher configuration, emulator not
  found, core not found, ready. It is computed at refresh and checked again before each launch. "Ready" does not wait for
  a launch test (a departure from specification §11.3): the ES-DE commands are what the user already plays with;
  "verified" (a clean return without a quick exit, or `retroverse-cli launch --test`) is shown.
- **Automation guard**: when `RV_LAUNCH_ALLOWLIST` is set (gate scripts, test harnesses, agents), only the listed
  executables may start, RetroArch only with `--appendconfig`, and DuckStation never; unit-tested.

### 9. Play sessions

Every launch attempt is recorded with UTC wall-clock times, its outcome (including failures to start and quick exits)
and its owner (process id and creation time); at start, only sessions whose owner is gone are closed as interrupted.
Command lines, which carry user paths, are logged at debug level only. Emulators keep Windows' default error mode, so a
crash dialog holds the session until the user returns (ADR-0006's window timeout and confirmation, never a kill).

### 10. Verification

Fixtures are original and minimal (invented titles), materialized under the build tree; hostile encodings and oversized
files are generated at test time. Tests on the real library carry the hidden Catch2 tag `[.real-library]`, take their
paths from environment variables, keep databases under `build/`, and run through `scripts/real-library.ps1`, which
refuses to run while ES-DE runs and compares read-only snapshots (files and directories) of the ES-DE home, the ROM root
and the media root before and after. Independent oracles check the code: a Python census of ES-DE's discovery
(`scripts/`), `hashlib` for the golden GameIds, and the frozen ES-DE launch lines (machine-local, git-ignored under
`tests/real_library/local/`), compared with `retroverse-cli launch --dry-run` through `CommandLineToArgvW`. Timings taken
while other work runs are informative; NFR-01, NFR-02, import times and UI costs are measured at the gate, in Release, on
a quiet machine. ES-DE 3.4.1 is the certified version.

## Consequences

- The catalog matches what the user sees in ES-DE (about 90,000 games here once disc tracks are grouped, unscraped
  files included); the quick catalog is virtualized and paged, and search runs in the index.
- Two new DLLs (`sqlite3.dll`, `pugixml.dll`) ship next to the executables; pugixml's MIT notice is recorded with the
  third-party records now and packaged in I9. SQLite and pugixml parse untrusted input and move only with the baseline
  (ADR-0001).
- Moves and renames keep their IDs only once the progressive hashing pass has seen the files, or when size, time and
  name single them out; the rest become review rows.
- Commands that need a shell, a precommand, a background launcher or an inject file cannot run until the user
  configures a profile (SC-10); the I7 emulator screens build on the same readiness states.
- The specification's TOML files become JSON files; its illustrative schema becomes the index and state tables above.
