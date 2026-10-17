# I2 step 9: queries and search

The catalog's lists and search of ADR-0008 §5 on the catalog database: the search area of schema v1 (an FTS5 index over
`idx_games` and `idx_systems`, kept in step by triggers, and the order index of the lists), `SqliteCatalogQuery`
(rv_catalog's `ICatalogQuery` with its filters, search, total orders, materialized lists, rows fetched by ids, and the
catalog's systems and a game's details), `retroverse-cli systems`, `games`, `search` and `show`, a catalog database
generator in `tests/support`, and the benchmarks. NFR-02 holds with a wide margin (p95 of indexed queries 6.8 ms on
10,000 games, Release, on a loaded machine), and a UI page of 30 rows fetched by ids takes 0.1 ms at p95 on 100,000
games (ADR-0008's budget is 5 ms).

## What was built

| File | What |
|---|---|
| `src/catalog_db/src/schema_search.cpp` | the search area of schema v1: the view `idx_game_search_content`, the FTS5 table `idx_game_search`, six triggers, the order index `idx_games_sort` |
| `src/catalog_db/include/rv/catalog_db/catalog_query.hpp`, `src/catalog_query.cpp` | `SqliteCatalogQuery` (`run`, `fetch`, `searchPlan`, `searchWords`, `systems`, `details`), `QueryOptions`, `SearchPlan`, `SystemSummary`, `GameDetails` |
| `tools/cli/src/catalog_text.hpp` (new private header), `cmd_games.cpp` | what the four commands share (the catalog opened for reading, results, rows as JSON), and `games` |
| `tools/cli/src/cmd_search.cpp`, `cmd_systems.cpp`, `cmd_show.cpp` | `search`, `systems`, `show` |
| `tests/support/catalog_generator.{hpp,cpp}` (new) | the generated catalogs (`generateCatalogDatabase()`, `generatedEntry()`, `generatedWords()`) |
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
  `_delete` and `_update` on `idx_games` (the update one only when an indexed column changed: the store's upsert sets
  every column, and an import writes most rows unchanged), and `idx_systems_search_insert`, `_delete` and `_update` on
  `idx_systems` (a system's full name is indexed with each of its games: a changed full name re-indexes the system's
  games; a system written after its games gives them its full name; a removed or renamed one takes it away). Writes are
  upserts, never `REPLACE` (step 4), so updates reach the update triggers. A full `rebuild` from the view is a repair
  (tested to give the same index); `db check` runs FTS5's integrity-check with rank 1 (step 4), which compares the
  index with the view: every test of the triggers ends with it.
- **`idx_games_sort`** on `idx_games (sort_key, state, availability, library, system, release_date)`: the total order
  (sort key, GameId: the rowid ends every index entry) of the lists, covering their filters and the columns of the
  other orders, so that a list is read in order from the index alone.

On 100,000 generated entries (the benchmark below) the order index takes 1,588 pages of 4 KiB (6.2 MiB) and the search
index 5,648 (22 MiB) of the database's 24,606 (96 MiB).

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
  implies `--include-unavailable`); `systems` (names, any library). The enumeration names are written into the SQL
  (constants, never user text: SQLite compares a literal list faster than a bound one); system names are bound, as a
  JSON array read by `json_each`.
- **Search** (`searchPlan()`): text split into words by FTS5's own tokenizer (`searchWords()`: the text inserted into an
  FTS5 table of the same tokenizer on the private in-memory database, its tokens read back in order through an
  `fts5vocab` table of instances), one quoted string per word (a `"` doubled) joined with AND, the last one a prefix:
  `"super" AND "pix"*`. User text never reaches FTS5 as syntax: quotes, `*`, `-`, `:`, parentheses, NEAR, OR, NOT, AND,
  `^`, `+` are separators or words. Blank text, and text without a word (punctuation only), matches every entry; text of
  more than 64 words is refused (`InvalidArgument`) rather than made into an FTS5 query of as many terms. One character
  (one code point once ASCII white space is trimmed) is a filter without FTS5, which has no index for one-character
  prefixes: the entries whose sort key starts with it, lower-cased as sort keys are (`sort_key >= p AND sort_key < p ||
  0xFF` on the order index: case ignored, diacritics not, so `é` finds "Éclair" and `e` does not).
- **Orders**, each total: Title (sort key, GameId); System (the system's full name folded as sort keys are, then its
  name, then Title's order; games of a system without a row sort as an empty full name); ReleaseDate (oldest first,
  undated last, then Title's order). A list is read in Title's order from `idx_games_sort`; System and ReleaseDate are
  made from it by a stable sort in C++ (ranks of the systems, the release date column).
- **How a list is read**. Without a search, the order index is read whole (with the one-character filter, the range
  of the sort key). With a search, FTS5 gives the matching rowids (`SELECT rowid ... MATCH`, no row content read); up to
  `QueryOptions::lookupLimit` (5,000) of them are looked up one by one (`json_each` of the ids joined to `idx_games`,
  sorted); beyond it, the list without the search is read and narrowed to the matches (binary search over them).
- **The kept list**: the last list read without a search is kept with the snapshot's `PRAGMA data_version` and reused
  while no other connection has committed: the same query costs a copy, and a search with the same filters and order
  narrows it in memory, which is what typing in the quick catalog does once its list is open. A commit (an import)
  changes `data_version`, and the next query reads the list again (tested). The connection must therefore not write
  (a reader cannot).
- **`fetch(ids)`**: one statement, `json_each` of the ids driving the join in their order (`ORDER BY j.key`): one row
  per id that has one, in the ids' order, an id listed twice twice; text fields as `displayText()` shows them, the
  description as written. `positionOf()` and `pageOf()` of query.hpp do the rest (a position over 81,829 ids: 0.03 ms).
- **`systems()`**: every system with its counts (`games`: Game and Available; `unavailable`: Game or MissingFile not
  Available; `entries`: every indexed entry), in SortOrder::System's order then by library, systems known only from
  their entries listed by name; one read.
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
- `games` and `search`: one line per game, `<gameId>  <system>  <title>[ (<year>)][ [<state>]][ [<availability>]]`,
  then `N games` (`N entries` with `--state`), or `<first> to <last> of N games` for a page, or `None from position
  <n>: the list holds N games`. `--state` and `--availability` take comma-separated names; `--availability` with an
  unavailable one implies `--include-unavailable`. JSON: `query` (text, `search`: everything, first-character or words,
  the words, systems, states, availabilities, includeUnavailable, sort), `count`, `offset`, `limit` (null for all),
  `games` (id, system, systemFullName, title, state, availability, releaseDate `YYYY-MM-DD` or null, year, developer,
  publisher, genre, players `1-4`/`8+` or null, rating 0 to 1 or null, favorite, description). A search text that
  starts with `-` follows `--` (`search -- -x`).
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
catalog in one import transaction, entries in ascending GameId order (step 4: much faster than key order); without
`keepEntries` the entries are made again as they are written (100,000 kept would hold about 200 MB). Deterministic for
the options, GameIds included (`stableGameId()` of each entry's key, as a first import gives them). It holds eleven
systems (ES-DE's names and full names, `fictif` "Système Fictif (Éditions Pixel)" and `archive`, whose folder is
missing: its games are SourceUnavailable); invented titles of one to five words from French, Spanish, German, Swedish
and Japanese-sounding words with their accents (5% of them decomposed, NFD), "The X of Y" with sort titles; works
drawn among two thirds as many as entries, so that most titles recur (namesakes within a system and across
systems); every indexed state
(Game 87%, MissingFile 4%, NotAGame 3% with each reason, DiscMember 4% under psx, Folder 2%); unscraped files (15%)
titled from their names; companies with accents, genres, release dates (10% undated), players, ratings, favorites,
covers and videos, original forms of the normalized fields; descriptions holding `quixotry`, which nothing searched
holds. `generatedWords()` is the oracle's folding of generated text (the generator's accented letters, ASCII words).

## Benchmarks

`build/msvc-x64-release/bin/rv_tests.exe "[bench]"` (hidden tag; Release). Taken while other worktrees built and ran
their tests (the processor near 100%): informative, the gate measures on a quiet machine.

| Measure | Result |
|---|---|
| NFR-02: 828 indexed queries on 10,000 games (68 searches: words, prefixes of 1 to 5 characters, two and three words, years, companies, system names; each in Title order, System order, ReleaseDate with the unavailable, and with a filter of two systems) | p50 0.9 ms, p95 6.8 ms, max 9.7 ms (limit 100 ms) |
| UI page on 100,000 entries: 30 rows fetched by ids, 503 pages spread over the list | p50 0.10 ms, p95 0.13 ms, max 0.23 ms; the first page of a new reader 0.7 ms (limit 5 ms) |
| Every game (81,829 ids) read after a commit: Title, System, ReleaseDate | 47, 86, 90 ms (median) |
| The same, kept | 0.03 to 0.07 ms |
| One letter, no FTS5: `s` (9,999 ids), `p` in System order (5,888) | 6.7 ms, 5.7 ms |
| Two letters after a commit (default limit): `su` (14,004), `pi` (20,823), `ch` (4,428), `19` (44,628), `an` (10,081) | 60, 124, 61, 60, 62 ms |
| The same narrowing the kept list | 17, 10, 5.5, 25, 7.4 ms |
| Longer searches after a commit: `pixel` (17,275), `drago` (2,234), `super pi` (2,728), `neon samurai` (55), `super m` (3,978) | 69, 97, 51, 3.9, 131 ms |
| The same narrowing the kept list | 27, 7.4, 6.9, 4.6, 37 ms |
| `systems()` with the counts | 196 ms |
| Position of the last of 81,829 ids (`positionOf`) | 0.03 ms |

Rows looked up against the list read and narrowed, after a commit (median ms; the crossover chose the default limit of
5,000): `ch` (4,428 matches) 37 against 54, `drago` (2,234) 38 against 72, `super pi` (2,728) 21 against 56, `su`
(14,004) 84 against 87, `an` (10,081) 139 against 84, `pixel` (17,275) 191 against 113, `19` (44,628) 169 against 116.
`super m` costs most: FTS5 has no index for the one-character prefix `m`* that ends it and merges every term that
starts with m (ADR-0008 keeps prefix indexes 2 and 3). An earlier version that read every list again on every query
(SQLite's default page cache, bound lists of states) took 72 to 270 ms for the same searches on the same machine.

<!-- the write-cost line is filled below -->

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
   after others (`super m`) stays an FTS5 prefix query (correct, slower: above).
4. **Text without a word matches every entry**, as blank text does (`!!`, `""`): ignoring a stray punctuation mark beats
   an empty list.
5. **Availabilities**: the listed ones narrow what `includeUnavailable` lets through, as `query.hpp` documents it and
   step 1's in-memory query reads it; the CLI's `--availability` naming an unavailable one implies
   `--include-unavailable`, so that asking for missing files shows them.
6. **The order index is covering and lives in the search area**: a list is read from the index alone, in order; the
   System and ReleaseDate orders are stable sorts of Title's in C++ rather than SQL sorts of the whole list.
7. **The kept list** per query object, validated by `PRAGMA data_version` (exactly "another connection committed"),
   so that typing in a search narrows the open list in memory; one list kept (the last read without a search), about
   650 KB for 82,000 games.
8. **The lookup limit** (5,000 rows matching, from the benchmark's crossover) only matters when no list is kept (the
   first search after an import): a few rows are looked up, many make the list be read and kept.
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

## Tests

`tests/unit/catalog_db/test_catalog_query.cpp` (label `unit`, tags `[catalog_db][search]`, `[catalog_db][query]`):

- the search area's objects (the FTS5 options, the view, the triggers, the order index) and `db check` passing;
- **the triggers**: an upsert that changes a row and one that keeps it, a removal, a system's full name changed, a
  system written after its games in one import, a system deleted and one renamed by hand, a `rebuild`: each followed by
  searches and by `db check` (FTS5's integrity-check with rank 1);
- **diacritics and case**: `eclair`, `ECLAIR`, `éclair` and its NFD form find "Éclair", "Éclair Nocturne" written
  decomposed and "ECLAIR DE LUNE"; `chateau` and `CHÂ` find "Château Fort";
- **words**: in any order, every one required, the last a prefix (`pix` and the prefix indexes of 2 and 3, `pixel qu`,
  not `pix quest`), each searched column (title, sort title, developer, publisher, genre, year, system full name), not
  descriptions;
- **one character**: `p`, `P`, ` p ` (trimmed), `é`, `É`, `e` (not "Éclair"), `"`;
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
- **details and systems**: a full entry read back equal to what the store wrote, its system, counts and order;
- **a generated catalog of 3,000 entries against an oracle**: 43 searches × 3 orders × 2 availabilities, 48 filter
  combinations, and both ways of reading a search's list (lookup limit 0 and unlimited), each equal to the oracle's
  list; the systems' counts; `db check`.

`tests/unit/cli/test_cli_catalog.cpp` (`[cli][catalog_db]`): `systems` as text and JSON; `games` (a system's list with
its namesakes in GameId order, the unavailable, other states, orders, pages, JSON fields, usage errors, an unknown
system); `search` (words, prefixes, diacritics, system names, filters, one character, FTS5 syntax, `--`, JSON);
`show` (text lines, JSON, an unscraped entry and a missing file, an unknown game, malformed ids); no database (exit 3,
none made), a database of version 0, `--output`, `--help`. The command table tests (`test_cli_commands.cpp`,
`tests/cmake/test_cli_process.cmake`) cover the four commands' unknown options as for every command.

`tests/unit/catalog_db/test_catalog_query_bench.cpp`: the benchmarks above (`[.bench]`, hidden).

## Notes for the steps that build on it

- **Step 11** (import): the search area follows the store's writes by itself (upserts, removals, systems at any point
  of the import). Its cost on the writes is measured above. `db rebuild-index` can end with `INSERT INTO
  idx_game_search (idx_game_search) VALUES ('rebuild')` if it rewrites the index tables by other means than the store.
- **Step 13** (the app): one `SqliteCatalogQuery` on the worker (`run`, `systems` for the chips) and one on the main
  thread for `fetch` (each with its own reader); the worker's instance keeps the open list, so typing narrows it in
  memory; after an import, `run` again (the kept list is dropped by `data_version`) and restore the selection with
  `positionOf`. `CatalogRow` gives what a card shows; readiness comes from step 10.
- **Step 10**: `show` can print the readiness of the game's command; **step 8/11**: its media paths
  (`IMediaResolver`).

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
  without FTS5, triggers and upserts, the materialized list in one read, the page budget); decisions 1, 2 and 7 add
  detail and may be recorded in its §5 at the merge: "the index keeps no positions (`detail=none`); the words come from
  the tokenizer through an `fts5vocab` table; a query keeps the last list it read and reuses it while
  `PRAGMA data_version` is unchanged".
- **query.hpp**'s comment on `availabilities` could say outright that an unavailable availability needs
  `includeUnavailable` (decision 5).
