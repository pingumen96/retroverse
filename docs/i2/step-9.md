# I2 step 9: queries and search

The catalog's lists and search of ADR-0008 §5 on the catalog database: the search area of schema v1 (an FTS5 index over
`idx_games` and `idx_systems`, kept in step by triggers, and the order index of the lists), `SqliteCatalogQuery`
(rv_catalog's `ICatalogQuery` with its filters, search, total orders, materialized lists, rows fetched by ids, and the
catalog's systems and a game's details), the search index suspended for bulk writes (`search_index.hpp`),
`retroverse-cli systems`, `games`, `search` and `show`, a catalog database generator in `tests/support`, and the
benchmarks. NFR-02 holds with a wide margin (p95 of indexed queries 4.4 to 4.6 ms on 10,000 games, Release, on a loaded
machine), and a UI page of 30 rows fetched by ids takes 0.12 to 0.16 ms at p95 on 100,000 games, 0.66 to 0.83 ms when
each page is fetched for the first time after a commit (ADR-0008's budget is 5 ms).

## What was built

| File | What |
|---|---|
| `src/catalog_db/src/schema_search.cpp` | the search area of schema v1: the view `idx_game_search_content`, the FTS5 table `idx_game_search`, six triggers (each its own statement, created by the migration's `apply` step and by `resumeSearchIndex()` from the same text), the order index `idx_games_sort`; the functions of `search_index.hpp` |
| `src/catalog_db/include/rv/catalog_db/search_index.hpp` (new) | `suspendSearchIndex()`, `resumeSearchIndex()`, `searchIndexFollowsWrites()`: bulk writes without the triggers, the index rebuilt before the commit |
| `src/catalog_db/include/rv/catalog_db/catalog_query.hpp`, `src/catalog_query.cpp` | `SqliteCatalogQuery` (`run`, `fetch`, `searchPlan`, `searchWords`, `systems`, `hasSystem`, `details`), `QueryOptions`, `SearchPlan`, `SystemSummary`, `GameDetails` |
| `tools/cli/src/catalog_text.hpp` (new private header), `cmd_games.cpp` | what the four commands share (the catalog opened for reading, results, rows as JSON), and `games` |
| `tools/cli/src/cmd_search.cpp`, `cmd_systems.cpp`, `cmd_show.cpp` | `search`, `systems`, `show` |
| `tests/support/catalog_generator.{hpp,cpp}` (new) | the generated catalogs (`generateCatalogDatabase()`, `generatedEntry()`, `generatedWords()`, `generatedComposed()`) |
| `tests/support/CMakeLists.txt` | `rv_test_support` links `rv_catalog` and `rv_catalog_db` for the generator |
| `tests/unit/catalog_db/test_catalog_query.cpp`, `test_catalog_query_bench.cpp`, `tests/unit/cli/test_cli_catalog.cpp` | the tests and the benchmarks (below) |
| `tests/unit/catalog_db/test_migrator.cpp` | two of step 4's tests adapted to a schema that holds an FTS5 table (decision 12) |

Typical use (the app's catalog worker, step 13):

```cpp
auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
auto query = rv::catalog_db::SqliteCatalogQuery::open(database->openReader().value()).value();
rv::catalog::CatalogQuery request;
request.text = "pixel qu";                                   // what the user typed
request.systems = {rv::catalog::SystemName::parse("snes").value()};
const std::vector<rv::catalog::GameId> list = query->run(request).value();   // the whole ordered list, one read
const auto rows = query->fetch(rv::catalog::pageOf(list, 0, 30)).value();   // a page, by ids
```

## The search area of the schema

`searchArea()` adds to schema v1 (step 4's notes: v1 stays open until the M1 gate), after the identity and index areas:

- **`idx_game_search_content`**, a view: one row per `idx_games` row (`id`, the GameId), its `title`, `sort_name`,
  `developer`, `publisher`, `genre` as written, `release_date / 10000 AS year` (NULL when undated) and the full name of
  its system (`idx_systems.full_name` through a LEFT JOIN: NULL while the system has no row). The FTS5 table's external
  content: `idx_games` holds neither the year nor the full name (step 4's note).
- **`idx_game_search`**, FTS5 with external content over the view (`content_rowid = 'id'`): `tokenize = 'unicode61
  remove_diacritics 2'` (case folded, diacritics removed, also from letters that carry several; decomposed text indexed
  as composed text), `prefix = '2 3'`, `detail = none` (the index records which rows hold a token, not where: every
  search word is one token matched anywhere in the row), `columnsize = 0` (nothing is ranked by relevance; lists are
  ordered by sort key). Descriptions are not indexed (ADR-0008 §5).
- **Triggers**, because FTS5 removes a row's tokens only when given the values it indexed: `idx_games_search_insert`,
  `_delete` and `_update` on `idx_games` (the update one writes to the index only when an indexed column changed: the
  store's upsert sets every column, so the trigger runs for every row written again, and its WHEN clause leaves the
  index alone for the unchanged ones), and `idx_systems_search_insert`, `_delete` and `_update` on `idx_systems` (a
  system's full name is indexed with each of its games: a changed full name re-indexes the system's games; a system
  written after its games gives them its full name; a removed or renamed one takes it away). Writes are upserts, never
  `REPLACE` (step 4), so updates reach the update triggers. A full `rebuild` from the view is a repair (tested: the same
  answers after it, `db check` passing) and what ends a bulk write (below); `db check` runs FTS5's integrity-check with
  rank 1 (step 4), which compares the index with the view: every step of the triggers' test ends with it. Each trigger
  is its own statement in `schema_search.cpp`, created by the migration's `apply` step and by `resumeSearchIndex()`, so
  that the schema holds the same text either way (`db check` compares it with the program's).
- **`idx_games_sort`** on `idx_games (sort_key, id, state, availability, library, system, release_date)`: its first
  two columns are the total order (sort key, GameId) of the lists, so that a list, or the sort key range of a first
  character, is read in order from the index alone, without a sort (tested with `EXPLAIN QUERY PLAN`: no temporary
  B-tree for any list form, nor for the systems' counts); the columns after them cover the filters and the other
  orders. The GameId must be named: SQLite's implicit rowid at the end of every entry orders ties only after every
  column before it, which made every list read sort its ties of the sort key again (the review's finding: every game
  read whole took 47 ms in the step's first benchmark, 31 ms now).

On 100,000 generated entries (the benchmark below) the order index takes 1,812 pages of 4 KiB (7.1 MiB) and the search
index 5,651 (22 MiB) of the database's 24,833 (97 MiB).

## Bulk writes: the search index suspended (`search_index.hpp`)

FTS5 holds the terms of the rows written in a transaction in memory and writes them out as a new segment whenever a
statement opens a savepoint, and every statement that writes an index row does (its triggers may fail half-way; the
store's per-entry `SAVEPOINT` adds nothing). The store writes rows one statement each, so with the triggers every row
written in an import becomes a segment of its own, which FTS5's automerge then merges again and again: the generator's
100,000 entries written row by row flush 100,000 leaf pages (FTS5's write counter, which the write benchmark reads from
the structure record), against 5,632 for the same index rebuilt once. That is what the search area cost the writes
(16.3 s against 8.8 s without the triggers, below), not the tokenizing: FTS5's `rebuild` tokenizes all 100,000 entries
from the view in 1.1 to 1.7 s. (The step's first notes concluded the opposite from `hashsize`, which only bounds what is
held between flushes and never comes into play when every statement flushes.)

- `suspendSearchIndex(writer)`, inside the write transaction (an import's: `beginImport()` begins it at once), drops
  the six search triggers; the rows written after it are not indexed.
- `resumeSearchIndex(writer)`, before the commit, rebuilds the index from `idx_games` and `idx_systems` (FTS5's
  `rebuild`) and creates the triggers again from the migration's text. With or without a suspension it is also the
  repair.
- Both refuse to run outside a transaction (`InvalidArgument`): DDL is transactional in SQLite, so a transaction rolled
  back keeps the triggers and the index as they were, and readers read the last commit throughout (tested). A
  transaction committed between the two leaves the index behind and the triggers missing: `db check` reports it (the
  schema differs from the program's, FTS5's integrity check fails) and a resume in a later transaction repairs it
  (tested). `searchIndexFollowsWrites()` says whether every trigger is in place.
- Statements the store prepared before are prepared again by SQLite (`sqlite3_prepare_v3`) when they next run.

Measured on the generator's 100,000 entries (write benchmark): 9.6 s suspended and rebuilt against 16.3 s row by row
(8.8 s without the triggers at all); an index made either way is the same (tested: every term with its rows, `db
check`). For an import that writes most rows again unchanged, row by row is cheaper (6.0 s against 7.4 s: the WHEN
clauses skip the index, the rebuild tokenizes everything): suspending pays off when many rows change, a first import or
`db rebuild-index` above all. The generator suspends by default (`CatalogGeneratorOptions::suspendSearchIndex`), so the
benchmarks' and tests' catalogs are made faster and their search index has the shape a rebuild gives (few large
segments).

## Queries (`SqliteCatalogQuery`)

`SqliteCatalogQuery::open(Connection, QueryOptions)` takes the connection it reads (a reader of
`Database::openReader()`, as the app's worker and the CLI use it) and refuses a schema version other than the
program's (`Conflict`, "migrate it first"). A private in-memory database splits search text into words. One instance is
used from one thread at a time; the app's main thread and its worker each have their own (each with its reader).

- **One read**: `run()` reads one snapshot, a single statement or several in one read transaction (`BEGIN` deferred;
  none of its own when the caller's connection is inside a transaction), and leaves no read transaction open: on a
  reader it never waits on the writer, and sees the last commit (tested during an import's open transaction).
- **Filters** (`CatalogQuery`): `states` (none: Game and MissingFile); availabilities: Available only, every one with
  `includeUnavailable`, narrowed to `availabilities` when listed (so an unavailable availability is let through only
  with `includeUnavailable`, as query.hpp and step 1's in-memory query read the port; the CLI's `--availability`
  implies `--include-unavailable`, and so does `--state missing-file`: a missing file is never available);
  `systems` (names, any library). The enumeration names are written into the SQL
  (constants, never user text: SQLite compares a literal list faster than a bound one); system names are bound, as a
  JSON array read by `json_each`.
- **Search** (`searchPlan()`): text split into words by FTS5's own tokenizer (`searchWords()`: the text inserted into an
  FTS5 table of the same tokenizer on the private in-memory database, its tokens read back in order through an
  `fts5vocab` table of instances), one quoted string per word (a `"` doubled) joined with AND, the last one a prefix:
  `"super" AND "pix"*`. User text never reaches FTS5 as syntax: quotes, `*`, `-`, `:`, parentheses, NEAR, OR, NOT, AND,
  `^`, `+` are separators or words. Blank text, and text without a word (punctuation only), matches every entry; text of
  more than 64 words is refused (`InvalidArgument`) rather than made into an FTS5 query of as many terms.
- **Normalization**: the text is composed (Unicode NFC, Windows' `NormalizeString`; ASCII text as it is) before
  anything else, so that the form it was typed in never matters: "é" typed as "e" + U+0301 is one character. (The FTS5
  words were alike already: unicode61 folds both forms to the same token.)
- **One character** (one code point once composed and ASCII white space is trimmed) is a filter without FTS5, which has
  no index for one-character prefixes: the entries whose sort key starts with that character, lower-cased as sort keys
  are, the key read as composed text (case ignored, diacritics not: `é` finds "Éclair" and `e` does not). Sort keys are
  not normalized (step 1's `sortKeyOf`, version 1), and a title a gamelist holds decomposed has a key that starts with
  "e" + U+0301: so `SearchPlan::forms` holds the character and its canonical decomposition when it has one, each form's
  range is read in turn on the order index (`sort_key >= f AND sort_key < f || 0xFF`; the forms ascend and neither
  begins the other, so the ranges follow one another in Title's order), and a key whose character after the form is
  not ASCII is composed up to its next ASCII character (rare) and kept only when its first character is still the one
  typed: under `e`, "e" + U+0301 + "clair" is "éclair" and left out; "e" + U+0332 (a low line, which composes with
  nothing) stays. Tested with titles written both ways, typed both ways, and against the generated catalog's oracle,
  whose decomposed titles (5%) it composes back with the generator's own table.
- **Orders**, each total: Title (sort key, GameId); System (the system's full name folded as sort keys are, then its
  name, then Title's order; games of a system without a row sort as an empty full name); ReleaseDate (oldest first,
  undated last, then Title's order). A list is read in Title's order from `idx_games_sort` without a sort in SQLite;
  System and ReleaseDate are made from it by a stable sort in C++ (ranks of the systems, the release date column).
- **How a list is read**. Without a search, the order index is read whole (with the one-character filter, the ranges
  of the sort key). With a search, FTS5 gives the matching rowids (`SELECT rowid ... MATCH`, no row content read); when
  it matches up to `QueryOptions::lookupLimit` (5,000) rows, every row it matches, of any state, availability and
  system, before the filters, they are looked up one by one (`json_each` of the ids joined to `idx_games`, sorted);
  beyond it, the list without the search is read, kept and narrowed to the matches (binary search over them).
- **The kept list**: on a reader, the last list read without a search is kept with the snapshot's `PRAGMA
  data_version` and reused while no other connection has committed: the same query costs a copy, and a search with the
  same filters and order narrows it in memory, which is what typing in the quick catalog does once its list is open. A
  commit (an import) changes `data_version`, and the next query reads the list again (tested). `data_version` does not
  change with a connection's own writes, committed or not, so a query object on a connection of another role (the
  writer, whose reads see its own writes) keeps nothing and reads every list again (tested: writes on the same
  connection, inside and outside a transaction, a rollback).
- **`fetch(ids)`**: one statement, `json_each` of the ids driving the join in their order (`ORDER BY j.key`): one row
  per id that has one, in the ids' order, an id listed twice twice; text fields as `displayText()` shows them, the
  description as written. `positionOf()` and `pageOf()` of query.hpp do the rest (a position over 81,829 ids: 0.03 ms).
- **`systems()`**: every system with its counts (`games`: Game and Available; `unavailable`: Game or MissingFile not
  Available; `entries`: every indexed entry), in SortOrder::System's order then by library, systems known only from
  their entries listed by name; one read. The entries are counted in C++ from a read of the order index's columns, in
  its order: a `GROUP BY` of library, system, state and availability made SQLite sort every entry of the index first
  (23 ms against 110 ms on 100,000 entries).
- **`hasSystem(name)`**: whether the catalog knows a system of that name (an `idx_systems` row, or entries of it in any
  library), one statement: `idx_systems` is small, `idx_games` is looked up through `idx_games_system` per library.
  The CLI checks `--system` with it instead of counting every system.
- **`details(id)`**: everything the catalog holds about a game, in one read: the `IndexedEntry` exactly as the store
  wrote it (every field, the media, the raw ES-DE values; tested equal), its library and its system's row.
- **The page cache** of the query connection is 32 MiB (`PRAGMA cache_size`), which holds the order and search indexes
  of a large catalog; SQLite's default 2 MiB made every list read whole cycle through it.

## `retroverse-cli systems`, `games`, `search`, `show`

```
retroverse-cli [--data-dir <dir> | --portable] systems [--json] [--output <file>]
retroverse-cli [--data-dir <dir> | --portable] games [--system <name>] [--state <states>] [--availability <list>]
        [--include-unavailable] [--sort title|system|release-date] [--offset <n>] [--limit <n>]
        [--json] [--output <file>]
retroverse-cli [--data-dir <dir> | --portable] search <text> [the options of games]
retroverse-cli [--data-dir <dir> | --portable] show <gameId> [--json] [--output <file>]
```

They read `<user data>/database/retroverse.sqlite` through `SqliteCatalogQuery` on a reader: the database is never
made, migrated or switched to WAL by them (nothing is written into it). Each has `--help`.

- `systems`: one line per system, `<name>  <full name>  <n> games  <n> unavailable  <n> entries`, with
  `[source-unavailable: <reason>]` or `[gamelist <state>]` when it applies (the library prefixed, `esde:gb`, when the
  catalog holds several libraries), then `N systems: N games, N unavailable, N entries`. JSON: `systems` (library, name,
  fullName, platforms, theme, romFolderConfigured, romFolder, availability, unavailableReason, gamelist,
  alternativeEmulator, games, unavailable, entries), `count`, `games`, `unavailable`, `entries`.
- `games` and `search`: one line per game, `<gameId>  <system>  <title>[ (<year>)][ [<state>]][ [<availability>]]`, then
  `N games` (`N entries` with `--state`), or `<first> to <last> of N games` for a page, or `None from position <n>: the
  list holds N games`. `--state` and `--availability` take comma-separated names; `--availability` with an unavailable
  one implies `--include-unavailable`, and so does `--state` with `missing-file` (a missing file is never available:
  without it, `games --state missing-file` listed nothing). JSON: `query` (text, `search`: everything, first-character
  or words, the words, systems, states, availabilities, includeUnavailable, sort), `count`, `offset`, `limit` (null for
  all), `games` (id, system, systemFullName, title, state, availability, releaseDate `YYYY-MM-DD` or null, year,
  developer, publisher, genre, players `1-4`/`8+` or null, rating 0 to 1 or null, favorite, description). A search text
  that starts with `-` follows `--` (`search -- -x`).
- `show <gameId>`: the game's id, title, system and library, state (with its reason and the excluding setting),
  availability (with its reason), relative path and path (the system's ROM folder joined to it), file (size, last
  write time in UTC, folder, hidden) or "not seen by the last import", disc head, gamelist entry or "none (unscraped)",
  sort key and title, release date, developer, publisher, genre, players, rating, flags, play count, time and last
  played, alternative emulator, media kinds with their extensions, the unknown gamelist elements, the description.
  JSON: the same fields, the metadata and flags as objects, `raw` with `unknown` and `original` as [name, text] pairs.
  Launch readiness (step 10) and media paths (step 8 and 11) are not shown yet.
- Exit codes: 0 ok (a list that matches nothing too: `0 games`); 1 `show` of a GameId the catalog does not have; 2 usage
  errors (an unknown option, state, availability or order, a malformed GameId, a negative count, a search of more than
  64 words) and a `--system` the catalog does not have (named on stderr); 3 the catalog cannot be read: no database
  ("'retroverse-cli import' makes it"), a schema version that is not the program's ("Run 'retroverse-cli db migrate'
  first."), a damaged file.

## The database generator (`tests/support/catalog_generator.hpp`)

`generateCatalogDatabase(file, {entries, seed}, keepEntries)` makes and migrates the database and writes a generated
catalog in one import transaction, entries in ascending GameId order (step 4: much faster than key order), the search
index suspended and rebuilt before the commit (`suspendSearchIndex`, on by default; off, the triggers index the rows
one by one); without
`keepEntries` the entries are made again as they are written (100,000 kept would hold about 200 MB). Deterministic for
the options, GameIds included (`stableGameId()` of each entry's key, as a first import gives them). It holds eleven
systems (ES-DE's names and full names, `fictif` "Système Fictif (Éditions Pixel)" and `archive`, whose folder is
missing: its games are SourceUnavailable); invented titles of one to five words from French, Spanish, German, Swedish
and Japanese-sounding words with their accents (5% of them decomposed, NFD), "The X of Y" with sort titles; works drawn
among two thirds as many as entries, so that most titles recur (namesakes within a system and across systems); every
indexed state (Game 87%, MissingFile 4%, NotAGame 3% with each reason, DiscMember 4% under psx, Folder 2%); unscraped
files (15%) titled from their names; companies with accents, genres, release dates (10% undated), players, ratings,
favorites, covers and videos, original forms of the normalized fields; descriptions holding `quixotry`, which nothing
searched holds. `generatedWords()` is the oracle's folding of generated text (the generator's accented letters, ASCII
words); `generatedComposed()` composes the generator's decomposed letters back with its own table (the oracle of the
one-character filter).

## Benchmarks

`build/msvc-x64-release/bin/rv_tests.exe "[bench]"` (hidden tag; Release). Taken while other worktrees built and ran
their tests: informative, the gate measures on a quiet machine. The figures are those of the runs after the review's
fixes (the order index keyed by (sort key, GameId), the generator's catalogs written with the search index suspended);
under heavier load a run took up to 2.5 times as long (NFR-02's p95 12 ms once).

| Measure | Result |
|---|---|
| NFR-02: 828 indexed queries on 10,000 games (69 searches: words, prefixes of 1 to 5 characters, two and three words, years, companies, system names; each in Title order, System order, ReleaseDate with the unavailable, and with a filter of two systems) | p50 0.74 to 0.83 ms, p95 4.4 to 4.6 ms, max 6.9 to 14 ms (limit 100 ms) |
| UI page on 100,000 entries: 30 rows fetched by ids, 503 pages spread over the list, fetched again | p50 0.10 to 0.11 ms, p95 0.12 to 0.16 ms, max 0.23 to 0.60 ms; the first page of a new reader 0.68 ms (limit 5 ms) |
| UI page, 200 pages each fetched for the first time right after a commit of the writer (the reader's page cache dropped, as after an import) | p50 0.55 to 0.58 ms, p95 0.66 to 0.83 ms, max 4.8 to 4.9 ms (limit 5 ms, checked) |
| Every game (81,829 ids) read after a commit: Title, System, ReleaseDate | 29 to 31, 41 to 48, 37 to 41 ms (median) |
| The same, kept | 0.03 ms |
| One system (psx, 14,016 ids) read after a commit | 29 to 33 ms (the order index read whole, filtered) |
| One letter, no FTS5: `s` (9,999 ids), `p` in System order (5,888) | 3.6 to 4.3 ms, 2.9 to 3.3 ms |
| Two letters after a commit (default limit), with the rows FTS5 matches: `su` (15,752 matches, 14,004 listed), `pi` (25,112; 20,823), `ch` (5,401; 4,428), `19` (55,499; 44,628), `an` (12,355; 10,081) | 36 to 40, 36 to 37, 32 to 34, 41 to 42, 40 to 50 ms |
| The same narrowing the kept list | 7.2 to 7.6, 8.4, 5.5, 13 to 27, 7.6 to 9.4 ms |
| Longer searches after a commit: `pixel` (20,745; 17,275), `drago` (2,735; 2,234), `super pi` (3,085; 2,728), `neon samurai` (65; 55), `super m` (4,492; 3,978) | 49 to 52, 19 to 29, 19, 3.5 to 4.0, 55 to 59 ms |
| The same narrowing the kept list | 24 to 46, 6.7 to 7.1, 6.3 to 7.1, 4.2 to 4.9, 33 to 34 ms |
| `systems()` with the counts | 22 to 23 ms |
| Position of the last of 81,829 ids (`positionOf`) | 0.04 ms |

**The lookup limit.** Rows looked up against the list read and narrowed, after a commit (median ms of the quieter
run), labelled with the rows FTS5 matches, which is what `QueryOptions::lookupLimit` is compared with: `neon samurai`
(65 matches) 3.4 against 38, `drago` (2,735) 18 against 36, `super pi` (3,085) 18 against 34, `super m` (4,492) 57
against 65, `ch` (5,401) 31 against 36, `an` (12,355) 59 against 39, `dr` (14,229) 60 against 40, `su` (15,752) 74
against 39, `ro` (17,199) 63 against 39, `pixel` (20,745) 89 against 56, `pi` (25,112) 82 against 36, `ga` (34,951) 90
against 39, `19` (55,499) 123 against 43. A lookup costs about 5 µs a match, a list read whole about 31 ms (plus the
narrowing): the crossover lies near 6,000 to 7,000 matches on 100,000 entries. The default stays at 5,000, a little
below it, because the narrowing path also keeps the list, so that the next keystrokes narrow it in 5 to 8 ms instead
of looking their matches up again. The limit does not weigh the filters (a system filter keeps few of the matches, and
its list still reads the whole order index) nor the catalog's size (a list of 40,000 entries reads in about 12 ms, its
crossover is lower); both paths stay far within NFR-02 either way. `super m` costs most when narrowed too: FTS5 has no
index for the one-character prefix `m`* that ends it and merges every term that starts with m (ADR-0008 keeps prefix
indexes 2 and 3). An earlier version that read every list again on every query (SQLite's default page cache, the states
and availabilities bound as lists) took 72 to 270 ms for the same searches on the same machine, and 80 ms for every
game; the step's first version, before the order index named the GameId, took 47 ms for every game.

**What the search area costs the writes** (the third benchmark: the generator writing 100,000 entries in one import
transaction; the leaf pages FTS5 flushed and the segments it holds read from its structure record), in the quieter run
(the other in brackets):

| Written | Time | Leaf pages flushed, segments |
|---|---|---|
| with the search area, its triggers row by row | 16.3 s (26.0) | 100,000, 10 |
| with the search area, suspended and rebuilt before the commit | 9.6 s (11.3) | 5,632, 11 |
| without the search triggers (the order index only) | 8.8 s (8.5) | none |
| without the order index (the triggers only) | 10.8 s (12.8) | 100,000, 10 |
| without either | 4.9 s (4.9) | none |
| the same entries again, unchanged, row by row | 6.0 s (5.8) | 100,000, 10 |
| the same entries again, unchanged, suspended and rebuilt | 7.4 s (8.4) | 5,632, 11 |
| a rebuild of the search index alone (`resumeSearchIndex()`) | 1.1 s (1.7) | |

(The entries' generation, twice, 0.8 to 1.1 s.) Row by row, every row is flushed alone (one leaf each, 100,000 in all):
the triggers cost about 75 µs a row, all of it the flushes and FTS5's merges of the segments they make (above, "Bulk
writes"), where the rebuild tokenizes the whole catalog in 1.1 to 1.7 s. Written again unchanged, the rows still cost:
the store's upsert sets every column, so the UPDATE trigger runs for each row (its WHEN clause writes nothing to the
index) and every `idx_games_sort` entry is deleted and written again; the review measured an UPDATE of every row to its
own values at 1.04 s without the search area, 1.89 s with its triggers, 1.98 s with the order index too (WAL 2.7 MB
against 10 MB). Step 4's timed test of 10,000 rows in Debug went from 3.1 to 4.8 s with the search area (it writes row
by row).

## Decisions taken within the step

1. **`detail=none` and `columnsize=0`** for the search index: every search word is one token matched anywhere in the
   row (no phrases, no column filters: user text is never FTS5 syntax), and lists are ordered by sort key, not ranked;
   the index keeps rowids only. Phrase queries of several tokens are refused by FTS5 with `detail=none`, and the queries
   never make one (next decision).
2. **Words split by FTS5's own tokenizer through `fts5vocab`** rather than `fts5_api`'s `xTokenize`, which needs the
   raw `sqlite3*` (database.cpp alone holds it, step 4): the same tokenizer with the same options, on a private
   in-memory FTS5 table whose tokens are read back by position. The tests prove the identity on hostile and generated
   text (the words equal the terms the index holds for that text, each word is one token), and a check of every Unicode
   code point with SQLite's unicode61 showed its folding idempotent (a folded token is one token, itself), so a word
   always quotes as one token.
3. **One character filters by the sort key's first character** (case folded as sort keys are, diacritics not), on the
   order index: the natural first keystroke of a title, and a range read without FTS5. A last word of one character
   after others (`super m`) stays an FTS5 prefix query (correct, slower: above). The character is compared as composed
   text on both sides: the search text is composed (NFC) before it is classified, and the sort keys, which `sortKeyOf`
   (version 1) does not normalize, are read in the ranges of both forms of the character and composed where a mark
   follows it (the review's finding: before, a title written decomposed was found by `e` and not by `é`, and `é`
   typed decomposed was a two-character FTS5 search). Normalizing the sort keys themselves (NFC in `sortKeyOf`, a new
   `kSortKeyVersion`) belongs to the sort key's owner, steps 1 and 4, with BACKLOG's "sort keys that fold diacritics"
   (version 2, which would fold `é` to `e` and make the forms moot); the normalization is a private helper of
   catalog_query.cpp (`NormalizeString`, kernel32) until then, and moves to `rv/core/utf8.hpp` when the sort keys need
   it.
4. **Text without a word matches every entry**, as blank text does (`!!`, `""`): ignoring a stray punctuation mark beats
   an empty list.
5. **Availabilities**: the listed ones narrow what `includeUnavailable` lets through, as `query.hpp` documents it and
   step 1's in-memory query reads it; the CLI's `--availability` naming an unavailable one implies
   `--include-unavailable`, so that asking for missing files shows them, and `--state missing-file` does too
   (MissingFile entries are always Missing: entry.hpp). The CLI implies it whatever other states are listed: `--state
   game,missing-file` also lists the games of unreadable folders, which `CatalogQuery`'s filters, a product of states
   and availabilities, cannot leave out while letting missing files through.
6. **The order index is covering, keyed (sort key, GameId), and lives in the search area**: a list is read from the
   index alone, in order, without a sort (an `EXPLAIN QUERY PLAN` test keeps it so); the System and ReleaseDate orders
   are stable sorts of Title's in C++ rather than SQL sorts of the whole list; the systems' counts are made in C++ from
   one read of the index rather than by a `GROUP BY`, which sorted it.
7. **The kept list** per query object on a reader, validated by `PRAGMA data_version` (exactly "another connection
   committed"), so that typing in a search narrows the open list in memory; one list kept (the last read without a
   search), about 650 KB for 82,000 games. On a connection of another role nothing is kept (`data_version` misses the
   connection's own writes): refusing such connections in `open()` would have cost the reads of an import's own
   connection, which see its writes, and keeping the lists there would gain little.
8. **The lookup limit** (5,000 rows FTS5 matches, of any state, availability and system, a little below the
   benchmark's crossover of 6,000 to 7,000 on 100,000 entries, in favour of the narrowing path, which keeps the list for
   the next keystrokes) only matters when no list is kept (the first search after an import): a few rows are looked up,
   many make the list be read and kept. It weighs neither the filters nor the catalog's size (above); both paths stay
   far within NFR-02.
9. **Enumeration names written into the SQL** as literal lists (constants of `[a-z-]`, never user text); system names
   and ids bound as JSON arrays.
10. **The query connection's page cache is 32 MiB**, as the writer's.
11. **`details()` has its own row reader** of every `idx_games` column (the store reads index rows per system, for the
    import, and its reader is private to catalog_store.cpp); a test checks it against what the store wrote.
12. **Two of step 4's tests adapted** to a schema that holds an FTS5 table: `PRAGMA integrity_check` connects every
    virtual table when it is prepared, so with the FTS5 shadow tables' root pages overwritten it fails whole (one
    problem, the damage met) instead of listing a problem per B-tree; the test now damages the ordinary tables' roots
    (a problem each, as before) and, in a copy, the shadow tables' (a failed integrity and fts5 check). The busy test's
    message names whichever FTS5 table is checked first (now the schema's own).
13. **The read commands open the database as found** (not made, not migrated, not switched to WAL), exit 3 with the
    remedy when it is missing or of another schema version; an unknown `--system` is a usage error (2), named, rather
    than an empty list; `show` of an unknown game exits 1.
14. **`rv_test_support` links `rv_catalog` and `rv_catalog_db`** for the generator (the brief puts it in
    tests/support).
15. **Bulk writes suspend the search index** (`search_index.hpp`) rather than keeping it in step row by row: the
    triggers dropped inside the write transaction and created again from the migration's text after a `rebuild`,
    because FTS5 flushes a segment at every statement savepoint (above). Dropping the triggers was chosen over the two
    alternatives the review named: a deferral flag in `rv_meta` tested by every trigger's WHEN clause (a lookup per row
    written, and a flag committed by mistake would silently stop the index for good, where missing triggers fail `db
    check`'s schema comparison), and a queue of pending changes applied at the commit (the old indexed values of every
    row changed kept until then: more machinery than an import needs, whose bulk cases rewrite most of the catalog
    anyway). When an import suspends is step 11's decision (its import service sees only `IImportTransaction`: the
    store's import transaction can suspend when it begins a first import, or after a threshold of rows written, and
    resume in `commit()`); the step provides and tests the mechanism and the generator uses it.
16. **`hasSystem()`** for the CLI's check of `--system`, a lookup per library, rather than the counts of every system.

## Tests

`tests/unit/catalog_db/test_catalog_query.cpp` (label `unit`, tags `[catalog_db][search]`, `[catalog_db][query]`):

- the search area's objects (the FTS5 options, the view, the triggers, the order index) and `db check` passing;
- **the order index's plans**: `EXPLAIN QUERY PLAN` of the list statements (each order's columns, the systems filter, a
  first character's range) and of the systems' counts: the covering order index, no temporary B-tree;
- **bulk writes**: an import with the search index suspended (a row changed, one added, one removed, a full name
  changed, a system added; a reader meanwhile reads the last commit) and resumed before the commit gives the searches
  of the new state and passes `db check`; refused outside a transaction; a rollback keeps the triggers and the index; a
  commit while suspended fails `db check` and a later resume repairs it; a generated catalog of 800 entries written
  both ways holds the same index (every term with its rows);
- **the triggers**: an upsert that changes a row and one that keeps it, a removal, a system's full name changed, a
  system written after its games in one import, a system deleted and one renamed by hand, a `rebuild`: each followed by
  searches and by `db check` (FTS5's integrity-check with rank 1);
- **diacritics and case**: `eclair`, `ECLAIR`, `éclair` and its NFD form find "Éclair", "Éclair Nocturne" written
  decomposed and "ECLAIR DE LUNE"; `chateau` and `CHÂ` find "Château Fort";
- **words**: in any order, every one required, the last a prefix (`pix` and the prefix indexes of 2 and 3, `pixel qu`,
  not `pix quest`), each searched column (title, sort title, developer, publisher, genre, year, system full name), not
  descriptions;
- **one character**: `p`, `P`, ` p ` (trimmed), `"`; `é` and `É`, typed composed or decomposed, find "Éclair" written
  composed, "Éclair Nocturne" written decomposed and a decomposed "É" followed by a second acute accent, in every order
  and with a system filter (`SearchPlan::forms` both forms, ascending); `e` and `E` find "Echo" and "e" + U+0332 "cho
  Line" (a mark that composes with nothing), not the titles whose decomposed first character is "é";
- **FTS5 syntax inert**: 41 hostile inputs (quotes, `*`, `-`, `:` column filters, `{title}:`, parentheses, NEAR, OR,
  NOT, AND, `^`, `+`, backslashes, a control character), each running without error and equal to its words alone;
  text that is not UTF-8 refused;
- **the tokenizer**: the words of hostile, NFD, symbol, CJK, emoji and generated titles equal the terms the index holds
  for them (an `fts5vocab` of the index), each word one token, the generator's oracle in agreement;
- **filters**: every state, the availabilities with and without `includeUnavailable`, systems with and without a
  search, OutsideSystemFolder matching nothing;
- **orders**: ties of the sort key (namesakes in one system and across systems, a sort title folding to the same key)
  ordered by GameId; System by the folded full name, then the name, an orphan system first; ReleaseDate;
- **positions and pages**: `positionOf` of every id, `pageOf` past the middle, `fetch` of every field, missing and
  repeated ids, the largest GameId;
- **a reader during a writer's transaction**: searches, filters, orders, systems and details of the last commit, at
  once (the reader's busy timeout 0), no read transaction left open; after the commit, the new state, also from lists
  kept before it;
- the schema version and connection refusals; a connection inside its caller's transaction;
- **queries on a writer** (a second `Database`'s writer moved into the query): writes on the same connection,
  committed at once and inside its own transaction, then rolled back, each seen by the next list and search (a lookup
  limit of 0, so that every search narrows the list a reader would keep);
- **details and systems**: a full entry read back equal to what the store wrote, its system, counts and order;
  `hasSystem` of systems with a row, known from their entries only, unknown, and a row without entries;
- **a generated catalog of 3,000 entries against an oracle**: 47 searches (one-character ones among them: `é` typed
  both ways, `e`, `a`, `o`, the oracle composing the sort keys back with the generator's table; the catalog holds sort
  keys whose first character is decomposed) × 3 orders × 2 availabilities, 48 filter combinations, and both ways of
  reading a search's list (lookup limit 0 and unlimited), each equal to the oracle's list; the systems' counts; `db
  check`.

`tests/unit/cli/test_cli_catalog.cpp` (`[cli][catalog_db]`): `systems` as text and JSON; `games` (a system's list with
its namesakes in GameId order, the unavailable, other states, `--state missing-file` with and without a search, orders,
pages, JSON fields, usage errors, an unknown system); `search` (words, prefixes, diacritics, system names, filters, one
character, FTS5 syntax, `--`, JSON); `show` (text lines, JSON, an unscraped entry and a missing file, an unknown game,
malformed ids); no database (exit 3, none made), a database of version 0, `--output`, `--help`. The command table tests
(`test_cli_commands.cpp`, `tests/cmake/test_cli_process.cmake`) cover the four commands' unknown options as for every
command.

`tests/unit/catalog_db/test_catalog_query_bench.cpp`: the benchmarks above (`[.bench]`, hidden).

After the review's fixes, `./scripts/test.ps1` passes the 1,035 unit tests in `msvc-x64-debug` and in
`msvc-x64-release`, both built without a warning; the benchmarks pass in Release.

## Notes for the steps that build on it

- **Step 11** (import): the search area follows the store's writes by itself (upserts, removals, systems at any point
  of the import), at about 75 µs a row written, which is FTS5 flushing a segment per statement (above). For a bulk
  write, a first import and `db rebuild-index` above all, call `suspendSearchIndex(writer)` once the import transaction
  has begun and `resumeSearchIndex(writer)` before its commit (both inside it; the store's import transaction is the
  natural owner: on a first import, or past a threshold of rows changed, since the rebuild tokenizes the whole catalog,
  1.1 to 1.7 s for 100,000 entries, and loses to the triggers when most rows are written unchanged: 7.4 s against 6.0
  s). Skip the upsert of an entry equal to its stored row (the store reads them per system already): an unchanged row
  written again still runs the UPDATE trigger and rewrites its `idx_games_sort` entry (the review's measurement: 1.04 s
  for 100,000 rows without the search area, 1.98 s with it). Write entries in ascending GameId order (step 4): FTS5
  also appends its rowids then.
- **Step 13** (the app): one `SqliteCatalogQuery` on the worker (`run`, `systems` for the chips) and one on the main
  thread for `fetch` (each with its own reader); the worker's instance keeps the open list, so typing narrows it in
  memory; after an import, `run` again (the kept list is dropped by `data_version`) and restore the selection with
  `positionOf`. `CatalogRow` gives what a card shows; readiness comes from step 10.
- **Step 10**: `show` can print the readiness of the game's command; **step 8/11**: its media paths
  (`IMediaResolver`).

## Review findings

Two reviews of the committed step reported ten findings (one twice); each was verified, none was rejected:

| Finding | Resolution |
|---|---|
| The order index ended with the rowid, so every list read sorted its ties again (both reviews) | keyed `(sort_key, id, ...)`; a plan test; lists 1.5 times faster (decision 6) |
| A first character depended on the normalization form of the text and of the titles | search text composed first; both forms of the character read, keys composed where a mark follows (decision 3); the sort keys themselves stay steps 1 and 4's (BACKLOG) |
| `--state missing-file` listed nothing without `--include-unavailable` | implied, as by `--availability` (decision 5) |
| A kept list on a connection that writes went stale (`data_version` misses its own writes) | lists kept on readers only (decision 7) |
| FTS5 flushes a segment per row written; the notes blamed the tokenizing | measured (100,000 leaf pages flushed for 100,000 rows); `search_index.hpp` suspends the triggers for bulk writes and rebuilds before the commit (decision 15); the notes corrected; when to suspend is step 11's |
| `systems()` sorted the whole order index for its `GROUP BY`; the CLI counted every system to check `--system` | counted in C++ (110 to 23 ms); `hasSystem()` (decision 16) |
| Rows written again unchanged still run the UPDATE trigger and rewrite the order index | the notes corrected; step 11 to skip unchanged upserts (notes for step 11) |
| The lookup limit is compared with FTS5's raw matches, the crossover was labelled by list sizes | `QueryOptions` documents it; the benchmark labels each search with its matches; the default kept at 5,000 with its reasons (decision 8) |
| The UI page benchmark timed only pages already in the reader's cache | 200 pages each first fetched after a commit, their p95 checked against 5 ms (0.66 to 0.83 ms) |

## For the merge

- **docs/roadmap.md**: step 9 done (NFR-02 on 10,000 games, the UI page on 100,000 in Release, diacritics, FTS5 syntax
  inert).
- **CLAUDE.md**: under the build and test commands, the benchmarks (`build/msvc-x64-release/bin/rv_tests.exe
  "[bench]"`, hidden, Release); a `retroverse-cli` paragraph may list `systems`, `games`, `search`, `show` with their
  main options; `tests/support` holds the catalog generator.
- **docs/architecture.md**: `rv_catalog_db`'s queries (`SqliteCatalogQuery`, the search area, the kept list),
  `rv_test_support`'s new links.
- **ADR-0008 §5** is met as written (FTS5 with `unicode61 remove_diacritics 2` and prefix indexes 2 and 3 over the
  listed columns, the words split by the same tokenizer, quoted, joined with AND, the last a prefix, one character
  without FTS5, triggers and upserts, the materialized list in one read, the page budget); decisions 1, 2, 7 and 15 add
  detail and may be recorded in its §5 at the merge: "the index keeps no positions (`detail=none`); the words come from
  the tokenizer through an `fts5vocab` table; search text is composed (NFC) first; a query on a reader keeps the last
  list it read and reuses it while `PRAGMA data_version` is unchanged; bulk writes suspend the search triggers and
  rebuild the index before their commit".
- **BACKLOG.md**: under "Sort keys that fold diacritics", add "and are composed (NFC): the one-character filter reads
  both forms of a character meanwhile (I2 step 9)".
- **query.hpp**'s comment on `availabilities` could say outright that an unavailable availability needs
  `includeUnavailable` (decision 5).
