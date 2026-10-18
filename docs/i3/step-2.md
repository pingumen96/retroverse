# I3 step 2: room selection

Roadmap row: I3 step 2 (wave 1, after 1a; 1b, 1c, 3, 4, 5, 6, 7 and 13r ran beside it in their own worktrees).
Decisions: [ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §2 (the RoomInput), §3 (which games the
room holds: eligibility, unknown media, one cartridge per title, the title key, the representative, the unplaced
reasons, the priority), §12 (where it runs), and [ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md)
§6 (the media paths derived on the catalog worker). The step fills the stubs step 0a listed for it: the room's bulk
snapshot query in `rv_catalog_db`, the title keys and version tags in `rv_world`, and `world::selectRoom`; and it adds,
next to `esde::mediaRelativePath` (step 0a's decision 2), the derivation of a snapshot's media paths and the check that
says whether its media are known. Nothing is generated yet (step 3): the selection is the generator's input.

## What was built

| File | What it holds |
|---|---|
| `src/catalog_db/src/room_query.cpp` (0a's skeleton), `rv/catalog_db/catalog_query.hpp` | `SqliteCatalogQuery::roomEntries(system)`: the `ICatalogQuery` port of step 1a on the catalog database, one read |
| `rv/esde/media_resolver.hpp`, `src/esde/src/media_resolver.cpp` | `esde::RoomMediaPaths`, `esde::roomMediaPaths(snapshot)`, `esde::checkRoomMedia(snapshot, resolver)`; the stored extension's check shared with `EsdeMediaResolver::path()` |
| `rv/world/title_key.hpp`, `src/world/src/title_key.cpp` (0a's stubs) | `kTitleKeyRule` (`title-key/1`), `titleKey()`, `RegionRank`, `VersionTags`, `readVersionTags()`, `fileNameOf()`, `versionTagsLess()` |
| `rv/world/room_selection.hpp`, `src/world/src/room_selection.cpp` | `world::selectRoom(RoomInput, WorldConfig)` beside 1a's types |
| `scripts/title_key_oracle.py` (new) | the independent oracle of the title keys, the tags and the version choice (`--check <file>`) |
| `tests/unit/world/test_title_key.cpp`, `test_select_room.cpp`, `selection_golden.hpp` (new) | the oracle's tables, the rules, the golden helper |
| `tests/unit/catalog_db/test_room_query.cpp`, `test_room_query_bench.cpp`, `test_room_selection_real.cpp` (new) | the query, the media, the standard fixture's golden, the benchmark, the real collection |
| `tests/fixtures/world/selection/table.json`, `standard.json`, `standard_all.json` (new) | golden selections (canonical text, `world::writeRoomSelection()`) |

On the catalog worker the three parts run in this order (step 10b wires them):

```cpp
auto snapshot = query.roomEntries(snes);              // rv_catalog_db: one read, the media left unknown
rv::esde::checkRoomMedia(*snapshot, mediaResolver);   // rv_esde: the media root checked (its own listing only)
auto paths = rv::esde::roomMediaPaths(*snapshot);     // rv_esde: every media path, no folder listed
// then, on the world's pool job:
auto selection = rv::world::selectRoom(rv::world::RoomInput{library, *snapshot}, config);
```

## The rules, exactly

### The snapshot (`SqliteCatalogQuery::roomEntries`)

- One read on the caller's connection (a deferred read transaction, none of its own inside one already, as every read
  of `catalog_query.cpp`): `hasSystem()`, then four statements on one snapshot: rv_meta's `sort_key_version`, the
  system's `idx_games` rows by GameId, their `idx_media` rows, their `rv_play_sessions` aggregated. `rv_libraries`
  drives each join, so `idx_games` is read through `idx_games_system (library, system)` per library, never scanned;
  media and sessions are matched to the entries by binary search. The statements are prepared per call (the query
  object's statement cache is private to `catalog_query.cpp`; a few tens of microseconds against milliseconds).
- Every indexed entry of the system, of every library the catalog holds (an import keeps one), whatever its state;
  `directory` is the file facts' kind, else a Folder entry (the import's rule for a media name); `year` the release
  date's; rating, favourite, hidden and play count as ES-DE wrote them; the media as stored (presence and extension).
- **Sessions**: Retroverse's sessions that started the game, every outcome but `failed-to-start` and `blocked`, launch
  tests (`retroverse-cli launch --test`) left out; `lastSession` the latest start (ISO-8601 text of fixed width, so
  `max()` is the latest).
- **Media: unknown.** The database keeps the presence its imports found, not whether the media root can be reached
  now, so the query answers `mediaKnown = false`, the reason "the media root has not been checked: the catalog's
  database cannot tell whether it can be reached". `esde::checkRoomMedia()` then sets it from
  `EsdeMediaResolver::checkRoot()`: true when the root is resolved, there and listed (one listing of the root's own
  folder, cached by the resolver until `refresh()`), else false with the root's reason ("the media root does not
  exist", ...). The system's media folders are never listed.
- Errors: NotFound for a system the catalog does not know (`hasSystem()`); a system it knows without entries gives an
  empty snapshot; ParseError for a row it cannot read back.

### The media paths (`esde::roomMediaPaths`)

Per entry that has media, in the snapshot's order: per kind, `mediaRelativePath(system, key, directory, kind,
extension)` ("snes/covers/Alpha Quest (USA).png", a folder's whole name: "snes/covers/Sports Vol. 2.png"), derived from
the stored presence and extension, never by listing a folder. Derived whether or not the media are known: the derived
texture cache of step 5 names its entries after these paths and serves them while the root is out of reach (ADR-0011
§7). A stored extension that is no file extension gives no path for its kind (the check `EsdeMediaResolver::path()`
applies, now one function).

### The title key (`title-key/1`)

Of the display title (`RoomEntry::title`: the gamelist's name, else the catalog's title of the file name):

1. every group in parentheses or brackets removed: from an opening `(` (or `[`) not inside an earlier group, scanned
   from the left, to the `)` (or `]`) that balances it, only characters of its own kind counted (what the group holds
   goes with it); an opening character nothing balances, and a closing one outside a group, stay as text;
2. ASCII white space trimmed and every run of it made one space;
3. the separators `" : "`, `" - "` and `" – "` (U+2013 between spaces) replaced, from the left, by `": "` (`": "`
   stays): "Quest - Part : Two" reads "Quest: Part: Two";
4. folded by the catalog's sort key (`catalog::sortKeyOf`, version 1: white space again, the invariant lower case);
   the version goes into the selection as `catalogSortKeyVersion`.

A title made only of groups ("(Untitled)") takes its whole title through step 4. Nothing else is folded: accents,
digits and other punctuation count ("Quest II" and "Quest 2" stay two titles; "Spider-Man" keeps its hyphen).

### The version tags and the representative

Read from the file's name (the last element of the relative path), never from the title:

- **Region tag**: the first group in parentheses that is made only of No-Intro region names separated by `", "` (45
  names, as written: "USA", "Europe", "Japan", "World", "Hong Kong", "Latin America", ...) or is a GoodTools country
  code (`1 4 A B C E F FC FN G GR HK I J K NL S SW U UK W`, or two or more distinct letters of `ABCEFGIJKSUW`
  combined: "JU", "UE", "JUE"). Rank 0 when it names USA or World (No-Intro "USA" or "World"; GoodTools U or W alone or
  combined, and 4, USA and Brazil), 1 when it names Europe (and neither), 2 otherwise and without a region tag. Only
  those spellings: "(USA,Europe)" without its space is no region tag, "(UK)" is England (rank 2), "(Unl)", "(PD)" and
  "(Unk)" name no country, a tag in brackets is no region tag.
- **Status tag**: a group in parentheses one of whose items (`", "` between them) starts with the word Beta, Proto,
  Demo, Sample or Pirate ("(Beta 2)", "(Demo, Kiosk)"; "(Kiosk Demo)" and "(Betamax)" are not), or a GoodTools mark
  `[h...]`, `[b...]`, `[a...]`: the letter alone or followed by anything but a lower-case letter ("[h1C]", "[hI]",
  "[b1]", "[a]"; "[hack]", "[!]", "[T+Eng]" are not).
- **Revision**: the highest N of the items `Rev N` (1 to 9 decimal digits) of the groups in parentheses; none is 0, and
  so is any other spelling ("(Rev A)", "(Rev1)", GoodTools' "(V1.1)").
- **Representative** of a title key: the first of its eligible versions by region rank, then without a status tag
  before with one, then the highest revision (`versionTagsLess()`), then the lookup key (`LookupKey`'s order: the key
  of the whole relative path, folder included, then the exact path, ordinal), then the GameId.

### `world::selectRoom`

1. **Unknown media decide nothing**: while `snapshot.mediaKnown` is false, there is no selection, whatever the
   configuration: `Unavailable`, "the media of the system are not known (<reason>): its room decides nothing from
   them", context the system.
2. **The games**: the entries in state Game or MissingFile that ES-DE does not hide. NotAGame, DiscMember and Folder
   entries are no games; a hidden game is left out as ES-DE leaves it out; none of them is listed.
3. A MissingFile entry is unplaced, `unavailable`. A Game whose folder could not be listed (`SourceUnavailable`) is not
   known to be gone and is selected like the others.
4. **Eligibility**: `include: "all"` makes every other game eligible (`all`); else a game with a cover is eligible and
   one without is unplaced, `no-cover` (`covered`); when no game has a cover (MissingFile games' covers counted, hidden
   games' not), every game is eligible (`no-cover-in-system`, style §21's P0 room; golden G06's single ROM).
5. **One title per title key** of the eligible games; its representative as above; the other versions unplaced,
   `duplicate-title`, with the representative's GameId.
6. **Priority data of a title**: its eligible versions' together: a favourite when one of them is, the play counts
   (ES-DE's plus Retroverse's sessions) summed, the highest rating; its sort key is the representative's. The titles in
   `priorityLess()` order (step 1a: favourites, play count, rating, sort key, GameId), the unplaced games sorted as a
   plan sorts them. A cover's shape (a portrait cover in the table) plays no part.
7. Errors besides 1: InvalidArgument for a snapshot without a system or with a GameId twice; a title that is not UTF-8
   (`titleKey()`), naming the game.

## The real SNES collection

`./scripts/real-library.ps1 -Filter '[real-library][room_selection]'` (Release, ES-DE closed), 2026-10-18; the system
imported alone into a new database under `build/msvc-x64-release/real-library/room-selection`. Snapshots of the ES-DE
home and install (hashed), the ROM and media roots (listings), 542,120 entries before and after: **0 differences**.

| | Figure |
|---|---|
| Entries | 7,835: 7,797 games, 38 folders; no missing file, hidden game, disc member or file that is no game |
| Games with a cover | 1,535 (media known: the root checked, no system folder listed) |
| **Titles chosen** | **890**: 760 rank 0 (733 "USA", 21 "World", 3 "Japan, USA", 3 "USA, Europe"), **57 Europe**, 73 other (53 "Japan", 6 "France", 3 GoodTools "J", 2 "Korea", 5 without a tag, one each "BS", "Brazil, Mexico", "China", "Hong Kong") |
| Unplaced | 645 `duplicate-title` (1,535 - 890), 6,262 `no-cover`, 0 `unavailable` |
| Media paths derived | 1,553 covers (the 1,535 games' and 18 folders'), each found on disk by its path |
| Times (Release, other work running) | snapshot 29.6 to 34.2 ms, media paths 5.2 to 6.3 ms, selection 12.1 to 19.0 ms |

Against the survey (890 distinct display titles among the 1,535 covered games; 727 USA once deduplicated, 57 Europe
only, 106 other):

- **The 890 titles match exactly.** The title key joins no two display titles of this collection (the test prints
  every key that would); the survey's grouping by the exact display title and the title key give the same groups.
- **No-cover**: the survey's 6,237 games without any media plus the 25 gamelist entries that have other media but no
  cover (1,560 - 1,535) are the 6,262 `no-cover`.
- **Regions**: the test regroups the covered games the survey's way (by display title, the USA release preferred, then
  Europe, then the rest) and gets 740 USA, 57 Europe only, 93 other when "USA" is any tag naming USA ("(Japan, USA)",
  "(USA, Europe)", GoodTools U). Our 760 / 57 / 73 differ from that by exactly two things: ADR-0010 ranks World with
  USA, so the 21 titles whose best release is "(World)" are rank 0 here and "other" there (740 + 21); and in one title
  the "(World)" release and the "(USA)" release tie at rank 0 without status or revision, and the lookup key chooses
  the World one, which lies in a sub-folder whose name sorts before the USA file's name; the survey counts it other (- 1 USA, + 1 other: 760 =
  740 + 21 - 1, 73 = 93 - 21 + 1). Europe agrees (57).
- **The survey's 727 / 106** cannot be reproduced exactly from the file names: read strictly (only the tag "(USA)"
  exactly is USA, only "(Europe)" Europe) the regrouping gives 734 / 61 / 95, and read broadly 740 / 57 / 93. The
  survey's script was not kept; its 13 fewer USA titles than the broad reading are titles whose USA release carries a
  combined or GoodTools tag, which it evidently did not read as USA. The choice of titles is the same either way; only
  which release represents a title can differ, and the ADR's rule (rank 0 for any tag naming USA or World) decides.

## Benchmark

`build/msvc-x64-release/bin/rv_tests.exe "[room_query][bench]"`: a generated catalog of 20,000 entries; its largest
system, psx with 3,835 entries (more than the budget's 2,000), read 20 times after a warming read: **median 20.2 ms**,
slowest 58.3 ms (other builds running); `selectRoom` on it (1,512 titles) median 10.3 ms. Budget: 50 ms for 2,000
games; the real collection's 7,835 entries took 30 to 34 ms. In Debug the same read takes about 300 ms (informative).

## Decisions taken within the step

1. **The query leaves the media unknown; `rv_esde` knows them.** The database cannot tell whether the media root can
   be reached now (schema v1 is closed, and an import that could not know an entry's media keeps what it stored), and
   `rv_catalog_db` must not reach `rv_esde`. So `roomEntries()` answers `mediaKnown = false` with a reason, and
   `esde::checkRoomMedia()` decides from the root, on the same worker: the safe default is that a caller who forgets
   the check gets no selection, never a room decided from stale or absent media.
2. **The media paths live beside `mediaRelativePath` in `media_resolver`** (0a's decision 2), not in a new file, so no
   source list changes; they are derived even while the media are unknown (the texture cache needs the names).
3. **A folder that could not be listed does not make a game unavailable**: only a MissingFile entry is `unavailable`
   (ADR-0010: "a missing file at its first consideration"). An unplugged ROM drive marks every game SourceUnavailable;
   reading that as unavailable would make an empty first room.
4. **Hidden games are left out entirely**, not listed as unplaced (ADR-0010's eligibility excludes them; no reason
   fits). Their covers do not count for "no cover in the system".
5. **A title's priority comes from all its eligible versions**: a favourite or plays on the Europe release count for
   the title the USA release represents. Only eligible versions count (an uncovered hack's plays do not).
6. **The separators are made into `": "`**, and a title made only of groups keeps its whole title as its key.
7. **Exact spellings only for the tags**: the region names, the GoodTools codes, the status words and `Rev N` as the
   naming conventions write them; anything else is no tag (documented in `title_key.hpp`, tested by the oracle).
8. **The lookup key tie-break is the whole key**, folder included, as ADR-0010 says ("the lookup key in ordinal
   order"); the tags are read from the file's name alone.
9. **Golden selections** under `tests/fixtures/world/selection/`, compared byte for byte and rewritten only with
   `RV_UPDATE_GOLDEN=1` (the test then fails, so a rewrite never passes unnoticed), as the roadmap asks of golden
   plans.
10. **The oracle covers the title key, the tags and the version choice** (`scripts/title_key_oracle.py`, Python, its
    own scanner): the 40 file names of the roadmap's table plus four entries of other states, 28 title keys and 52
    file names; the priority order is checked by hand in the C++ test and by the golden.

## Tests

- `test_title_key.cpp` (`[world][title_key]`): the oracle's tables (`python -I scripts/title_key_oracle.py --check
  tests/unit/world/test_title_key.cpp`: 168 of 168 lines): title keys (groups nested, unbalanced, of both kinds,
  separators, white space, accents), version tags (No-Intro and GoodTools regions, combined codes, status words and
  marks, revisions), and the 44-row table's keys, tags and outcomes against `selectRoom`; the table's selection in
  priority order (the favourite through its Europe release, plays through its Japanese release, sessions, ratings,
  then sort keys) and its golden `table.json`, read back equal.
- `test_select_room.cpp` (`[world][select_room]`): covered by default; `include: "all"`; the small-library rule (a
  single ROM without media, several uncovered games, a missing file's cover counting, a hidden game's not, an empty
  system); unknown media giving no selection with the reason, whatever the configuration; the games considered (hidden,
  missing, source unavailable, no games, disc members); a title's priority from its versions; the ties (key, exact
  path, the folder ignored by the tags); the selection independent of the snapshot's order; snapshots refused.
- `test_room_query.cpp` (`[catalog_db][room_query]`): a generated catalog of 3,000 entries (every state, media,
  favourites) with sessions of every outcome and a launch test: every system's snapshot equal to what the store wrote,
  sessions counted as the rule says; an unknown system, a known empty one, a read during an import's open transaction
  (the last commit, whole), no read transaction left open; the standard fixture: the media known from the root (no
  system listed), Alpha Quest's twelve paths and the folders' covers, every path a file of the fixture, then the media
  root renamed away: unknown with its reason, the same paths, no selection; malformed stored extensions; the standard
  fixture's SNES selection (`standard.json`: one title, four `no-cover`) and with `include: "all"` (`standard_all.json`),
  every snapshot entry equal to `details()`.
- `test_room_query_bench.cpp` (`[.bench]`) and `test_room_selection_real.cpp` (`[.real-library]`): above.

Results on the reference machine, with other worktrees building at the same time: `msvc-x64-debug` and
`msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` ran 1,374 unit tests in Debug and 1,374 in Release (the
18 new test cases of the step among them, all passing); in Release all passed, in Debug one test of the app's catalog
worker that waits on a busy database (`a session write that meets a busy database stays pending ...`) failed once
under the load and passed when run again alone, unrelated to the step.

## Notes for later steps

- **3**: the selection is `selectRoom(RoomInput, WorldConfig)`; hash `writeRoomSelection()`'s text into the plan's
  `inputDigest`; `plan.titleKeyRule = selection.titleKeyRule` (`kTitleKeyRule`) and `plan.catalogSortKeyVersion =
  selection.catalogSortKeyVersion`; copy `selection.unplaced` into the plan and add `capacity` for the titles that do
  not fit. `worldplan generate` runs the same three calls as the app (`roomEntries`, `checkRoomMedia`, `selectRoom`) and
  exits 1 with the `Unavailable` error's message when the media are unknown.
- **1c / the merge**: `esde/snes-room` (USA and Europe releases, a portrait cover, a game without a cover) deserves a
  golden selection of its own beside `standard.json`: `checkSelectionGolden("snes-room", ...)` in
  `test_room_query.cpp`'s pattern, written once with `RV_UPDATE_GOLDEN=1` and reviewed. Its invented names must use
  the exact tag spellings above to be ranked (a "(USA)" tag, not "[USA]").
- **10b**: the catalog worker's job is `roomEntries` → `checkRoomMedia` → `roomMediaPaths`. `checkRoomMedia` uses the
  resolver's cached root check: refresh the resolver (or use a fresh one) before each snapshot, so that an unplugged
  drive is seen. After an import whose `ImportStatistics::mediaUnknown` was not 0 the stored presence may be stale for
  the entries it could not ask (they kept what they had, or nothing on a first import): the worker should then keep
  the snapshot's media unknown until an import knows them all, or a first room could see "no cover in the system".
- **9**: the selection is for a first generation; the incremental update decides per game, but with the same rules
  (`titleKey()`, `readVersionTags()`, `versionTagsLess()`, the eligibility), and a representative is chosen once
  (ADR-0010: a better version arriving later is `duplicate-title`).
- **14b** (Informazioni: other versions' titles and regions): `readVersionTags()` gives each version's region tag as
  written (`VersionTags::regionTag`).

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Layout and layering", `rv_world`: since I3 step 2 `world::selectRoom` (the room selection of ADR-0010 §3: games,
    eligibility, title keys `title-key/1`, representatives by region rank, status and revision, priority) and
    `rv/world/title_key.hpp`; `rv_catalog_db`: `SqliteCatalogQuery::roomEntries` (the room's bulk snapshot, media left
    unknown); `rv_esde`: `esde::checkRoomMedia` and `esde::roomMediaPaths` (in `media_resolver.hpp`).
  - Conventions, beside the GameId oracle: "Title keys and version choice (`rv/world/title_key.hpp`): the tables of
    `tests/unit/world/test_title_key.cpp` come from `scripts/title_key_oracle.py`; a rule change changes both, and
    `python -I scripts/title_key_oracle.py --check tests/unit/world/test_title_key.cpp` must pass."
  - Tests: golden selections in `tests/fixtures/world/selection/` (`tests/unit/world/selection_golden.hpp`),
    rewritten only with `RV_UPDATE_GOLDEN=1`; `./scripts/real-library.ps1 -Filter '[real-library][room_selection]'`
    prints the real SNES selection.
- **docs/architecture.md**: the `world` row (selection and title keys), `catalog_db` (the room query), `esde` (the
  room's media paths and check).
- **docs/roadmap.md**: step 2 done; the real figures (890 titles: 760 USA or World, 57 Europe, 73 other) beside the
  survey's in the I3 introduction if wanted.
- Shared files touched besides the row's: `rv/esde/media_resolver.hpp` and `media_resolver.cpp` (additions at the end,
  one helper shared by `path()`), `rv/catalog_db/catalog_query.hpp` (one declaration and the header's first sentence);
  no CMake source list changed (the tests are globbed).
