# Conventions

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

- C++20 with MSVC `/W4 /WX /permissive- /utf-8`. Fix warnings instead of silencing them.
- Namespaces `rv::<module>`; files `snake_case`; types `PascalCase`; functions and variables `camelCase`;
  private data members `m_name`; constants `kName`; `enum class` values `PascalCase`.
- Errors: return `rv::Expected<T>` / `rv::Status` (`rv/core/expected.hpp`). No exceptions across module
  boundaries. Bad or missing assets never crash the app: log, fall back, continue (spec §19, style §21).
- Logging: `spdlog::info/warn/error` on the default logger configured by `rv::core::initLogging`.
- Units and axes (ADR-0002): meters, +Y up, right-handed, +Z towards the viewer/front of a prefab, CCW front
  faces, glTF UV origin at the image top-left, images never flipped on load.
- Colors are authored as sRGB hex codes (style §3) and converted to linear for shading
  (`rv::core::hexToLinearRgb`). Shading happens in linear space; output through an sRGB framebuffer.
- Text and paths are UTF-8; convert with `rv::core::pathFromUtf8` / `rv::core::pathToUtf8`, which throw on text that
  does not convert: names from a file system or an untrusted file go through `rv/core/utf8.hpp` (`tryPathFromUtf8`,
  `tryPathToUtf8`, `pathToUtf8Lossy` for messages, `utf8ToWide`/`wideToUtf8`, `isValidUtf8`/`findInvalidUtf8`,
  `appendUtf8`; `utf8WithWindows1252Fallback` and `windows1252ToUtf8` for text other programs wrote).
- Core helpers of I2, one header each in `rv/core/`: `sha256.hpp` (SHA-256, streaming and one-shot, hex);
  `case_fold.hpp` (`fileSystemUpperCase`: the file-system casing of lookup keys and path comparisons, code unit by code
  unit as NTFS and exFAT compare names, never ASCII-only folding or Unicode normalization; `invariantLowerCase`, the
  same table's lower case for the catalog's sort keys; `casingTableVersion`);
  `time.hpp` (UTC ISO-8601: `formatUtcIso8601`, `utcNow`, `parseUtcIso8601`); `relative_path.hpp`
  (`resolveRelativePath`: a path read from a file, a gamelist's for one, relative to its folder; `OutOfRange` when it
  lies outside the folder, `InvalidArgument` when malformed); `tree_walk.hpp` (`walkTree`/`listTree`: big trees through
  `\\?\` paths, sorted, links followed without loops, errors as entries, a folder that cannot be listed `Unavailable`);
  `file_io.hpp` (`readFileShared` for files another program rewrites, `extendedLengthPath` for folders the user names,
  `extendedLengthPathBelow` to reopen a walked name exactly: a plain path loses trailing dots and spaces;
  `SharedFileReader`, positioned reads of a file opened with full sharing, its facts from the handle; `listedFileFacts`,
  a file's size and time as a listing gives them, which is what the import records);
  `system_error.hpp` (Windows errors as text and as `ErrorCode`: a missing drive is `Unavailable`, a locked file
  `Conflict`; `ErrorCode::Cancelled` is an operation stopped on request); `paths.hpp` (`Paths::databaseDir`,
  `pathIsWithin` (lexical), `checkUserDataDirectory`: the data-directory guard, links and junctions resolved);
  `system_probe.hpp` (the `ISystemProbe` port with `appPath`, `findUninstallEntry`, `matchesWildcard`;
  `platform_win::Win32SystemProbe` asks Windows, `test::FakeSystemProbe` maps).
- Core helpers of I3: `seed.hpp` (`deriveSeed`: SHA-256 of the NUL-separated tuple `rv-seed/1`, parts..., integers as
  decimal digits, the first 8 bytes big-endian), `random.hpp` (`Random`, SplitMix64 with its own `below`, `between`,
  `unit`, `shuffle`, `kRandomAlgorithm` `splitmix64/1`; never the standard distributions or `std::shuffle`),
  `canonical_json.hpp` (`writeCanonicalJson`, `canonicalJsonDigest`, `parseCanonicalJson`, which refuses any text that
  is not canonical: keys by code point, integers only, top-level arrays one element per line, a final LF),
  `worker_pool.hpp` (`WorkerPool`: `min(6, hardware threads - 3)` threads below normal priority without dynamic
  boosts, bounded (a full queue refuses, never blocks), priorities, `JobGeneration` cancellation, a 2 s shutdown
  bound), `backups.hpp` (`backUpFile`, the newest 10 by default, named as the database's backups), `cpu_timers.hpp`
  (`CpuTimers`, `mainThreadCpuTimers`, `CpuSectionScope`, `summarizeDurations`, the one nearest-rank percentile rule);
  `file_io.hpp`'s `writeFileAtomic` flushes before a write-through rename (`core::Durability::Unflushed` only for
  derived data checked when read, such as the derived-texture cache); `log.hpp`'s `logLinesSince` reads the in-memory
  lines and their count under one lock.
- GPU memory (`rv/render/gpu_memory.hpp`: `GpuMemoryLedger`, `gpuMemoryLedger()`, `GpuMemoryAllocation`,
  `textureBytes`): every texture and buffer of `rv_render` records its bytes under a `GpuMemoryKind`. A texture created
  outside the texture cache records a kind of its own (`TextureDesc::memoryKind = HeroTextures` for the hero CRT's
  stills; the texture arrays and the video texture record theirs): frame-limited runs fail (exit 3) when the ledger's
  procedural surfaces and scene textures differ from what the cache and the defaults hold. A new storage format adds a
  `GpuFormat` and its line in `textureLevelBytes`.
- The collection's layers (`rv_assets`, I3 step 5): fronts 256 x 188 and spines 40 x 256 in BC1 sRGB (RGBA8 at half
  the resolution as the fallback), mips in linear light, spines and generated labels drawn with stb_truetype from the
  bundled fonts, loaded only when their SHA-256 matches `assets/registry/assets.json`; the derived-texture cache under
  `<user data>/cache/textures/<library id>` (`.rvtex` entries named without the source, found while it is out of
  reach, invalidated by its size and time, trimmed to 512 MiB). Raise `kCoverPipelineVersion` (`rvtex.hpp`) with any
  change of a layer's pixels. Since I4 step 2 (ADR-0013 §4) a front is made for its family's label quad
  (`assets::LabelAspect`, `world::MediaFamily::coverAspect()`): the cover fitted to that shape and the generated label
  laid out at it, then resampled once into the 256 x 188 layer, the shape in lowest terms the last element of the front
  keys; the layer's own shape (the 16-bit cartridges, `coverFit: "layer"`) runs I3's code and keys unchanged.
- Media families (`assets/world/media_families.json`, `rv/world/media_family.hpp`, ADR-0013 §4): ids are permanent (a
  new definition takes a new id). A family names its item manifest (the `cartridge` prefab at its size, its label quad
  proven by `checkItemManifest()`) and optionally its fixtures (hero-wall and side-wall modules, display case, featured
  wall shelf at its slot sizes); `withFamilyFixtures()` stands them in the template's sockets wherever a room's data is
  read, so the plan records them by ref. A fixture's levels must face the family exactly (`LevelFacing`), as
  `tests/unit/world/test_world_families.cpp` checks for every family.
- World plans (ADR-0010, ADR-0013 §5): a plan is `writePlan()`'s canonical text, in schema 2 (the building), and is
  only ever written through `world::savePlan()` under the world lock (a hard rule broken is never saved); never edit
  one by hand (it is then refused). The one exception (I4b step 1, ADR-0015 §6): undo and the lock's rollback of an
  interrupted action restore a snapshot's plan byte for byte with `core::writeFileAtomic` under the world lock, a plan
  `savePlan()` wrote before; undo first keeps the plan it replaces in `backups/` (`core::backUpFile`) as `savePlan()`
  does. A schema-1 plan is read and lifted (`world::lift`), never written. Placements are never moved by an update;
  only `worldplan generate --replace` chooses the face-out titles again, and an undo can bring back the earlier plan's
  (the plan before the action it undoes, byte for byte). The frozen oracles
  `tests/fixtures/world/v1/` are never rewritten; the goldens of `tests/fixtures/world/golden/` only with
  `RV_UPDATE_GOLDEN=1`.
- Catalog identities (ADR-0008 §4, `rv_catalog`): a GameId is derived once from (`rv-game/1`, library, system, lookup
  key) and then persisted; never derive it again for a stored identity. The golden GameIds of
  `tests/unit/catalog/test_identity.cpp` come from `scripts/game_id_oracle.py` (Python's hashlib): a change of the
  derivation changes both, and `python -I scripts/game_id_oracle.py --check tests/unit/catalog/test_identity.cpp`
  must pass. Likewise the golden quick hashes (rule 1, `rv/catalog/fingerprint.hpp`: SHA-256 of a file up to 128 KiB,
  else of its first and last 64 KiB) of `tests/unit/catalog/test_fingerprint.cpp` come from
  `scripts/quick_hash_oracle.py`, and this must pass:
  `python -I scripts/quick_hash_oracle.py --check tests/unit/catalog/test_fingerprint.cpp`. Reconciliation
  (`rv/catalog/reconciliation.hpp`, ADR-0008 §4) pairs an equal quick hash only with an equal file name or time
  (ADR-0009); anything it cannot pair safely becomes a review row, never a merge.
- ES-DE files (`rv_esde`, ADR-0008; ES-DE 3.4.1's behaviour as verified in its source is in `docs/i2/step-2.md` and
  `step-3.md` (configuration, gamelists), `step-7.md` (the ROM scan, disc groups, MAME lists), `step-8.md` (media) and
  `step-10.md` (launch commands and find rules)): follow ES-DE, and report every deliberate difference as a
  `catalog::Issue` or in the header. `~` is ES-DE's home folder (the parent of the ES-DE home) and every `~` of a text
  is replaced. The settings reader extracts only its nine whitelisted keys (`esde::settingKeys()`): never read, log or
  return the scraper credentials or the UI mode's passkey. `rv/esde/gamelist.hpp`: `readGamelist` (NotFound means no
  gamelist; any other error is retried by the caller), `gamelistPath`, `entryForListed`; issue codes `gamelist-*`, those
  about single entries listed up to 100 per code; of the entries of one kind and path the last counts, entries of one
  file that differ in kind or case are all kept for the scan to choose. The user's paths are logged at debug level only:
  parse files under the user's folders with `core::readFileShared` and `detail::parseXml`, as `readGamelist` does
  (`detail::loadXmlFile` still logs the loader's warnings with the path at warning level, BACKLOG). The library snapshot
  comes from `rv/esde/library_source.hpp` (`EsdeLibrarySource`; `scripts/esde_census.py` must agree with it), media from
  `rv/esde/media_resolver.hpp` (`EsdeMediaResolver`: ES-DE 3.4.1's paths and extensions in its order,
  `mediaRelativePath`; a media root out of reach or a media folder that cannot be listed is `Unavailable`, never "no
  media"; open a media file through `core::extendedLengthPathBelow`, since names may end with a dot or a space), the
  coverage from `rv/catalog/coverage.hpp` (`measureCoverage` over four populations), launch requests from
  `rv/esde/launch_translation.hpp` (`EsdeCommandTranslator`) through `catalog::resolveLaunch`
  (`rv/catalog/launch_resolution.hpp`, the readiness of a system or a game).
- Catalog database (`rv_catalog_db`, `docs/i2/step-4.md`): only `src/catalog_db/src/database.cpp` includes `sqlite3.h`;
  other code uses `Connection`, `Statement` (kept statements reset after each use, `ScopedReset`) and `Transaction`.
  One writer per process on one thread (`Database::writer()`), read-only readers on other threads
  (`Database::openReader()`). Schema v1 is closed since M1: a change of the schema is a migration to a new version
  (`AreaStep{2, ...}`) in its area's source (`schema_identity.cpp`, `schema_index.cpp`, `schema_search.cpp`, `schema_launch.cpp`), STRICT,
  foreign keys only into `rv_*` (never into `idx_*`) and never cascading; writes are upserts, never `REPLACE`; tests
  keep databases under their temporary folder. A state table that names a game refers to `rv_game_identities (id)`
  through a single-column foreign key: `putAlias()` finds those columns in the schema and re-points their rows when an
  identity becomes an alias (a table with one row per game keeps the moved identity's), which a composite key would
  escape. Since I2 step 11 the identity area also holds the import records (`rv_imports`), the quick hash read and
  written with the identity (`StoredIdentity::quickHash`; the pass writes it alone with `putQuickHash()`) and the
  history, alias and review rows; the index area the disc references (`idx_disc_references`); the launch area (step 12)
  `rv_play_sessions`, `rv_launch_game_choices`, `rv_launch_system_choices` and `rv_launch_verifications`. Whatever
  writes identities, aliases or index rows holds the import lock (`ICatalogStore::lockImport()`, never
  waiting: `Conflict`): the import from before it reads the library (it reads the store before its write transaction,
  which holds the writes alone), the quick-hash pass for its run, a review's resolution. Session and choice writes take
  none and touch neither; another process's bulk import can hold the write lock for longer than the 5 s busy timeout,
  so they go through `SessionStore` (`BusyRetry`, `SessionRecorder`, `isBusyError()`). Unchanged rows and identities
  are not written: `rv_game_identities.last_seen` is when an import last wrote the identity with its file present, not
  the last import that saw it, which `rv_imports` records (the newest `kKeptImportRecords`, 100, per library, each
  holding only the systems with something to say). Bulk writes call `suspendSearchIndex(writer)` once their transaction
  has begun and `resumeSearchIndex(writer)` before its commit (`rv/catalog_db/search_index.hpp`; a commit in between
  leaves the index behind, which `db check` reports); the import's store does it through `prepareWrites()`
  (`SearchIndexPolicy::Automatic`: a first import, or rows written at least a fifth of the index). Queries go through
  `SqliteCatalogQuery` (`catalog_query.hpp`, step 9), one instance per thread on its own reader; user text never reaches
  FTS5 as syntax.
- OpenGL calls only in `rv_render`/`rv_devui`/`rv_ui`/`rv_app` (and the I1 spike executables), only on the main
  thread, DSA style (GL 4.5; the vendored RmlUi GL3 renderer in `third_party/rmlui_backends` excepted).
- Determinism: no `rand()`, no iteration over unordered containers into outputs, explicit seeds (`core::deriveSeed`,
  `core::Random`); the world's arithmetic is integer, so Debug and Release write the same plans. The golden seeds,
  sequences and canonical texts come from `scripts/world_seed_oracle.py` (Python's hashlib and json) and the title
  keys and version choice of `tests/unit/world/test_title_key.cpp` from `scripts/title_key_oracle.py`; a rule change
  changes both, and these must pass: `python -I scripts/world_seed_oracle.py --check
  tests/unit/core/test_seed_random.cpp tests/unit/core/test_canonical_json.cpp` and
  `python -I scripts/title_key_oracle.py --check tests/unit/world/test_title_key.cpp`.
- Product code starts processes only through `rv_launcher` and `rv_platform_win` (`Win32ProcessRunner`): argv, never a
  shell string, no batch files; the product never terminates an emulator. An emulator profile's argument template
  gives one argument per element, never split again (`rv/launcher/arg_template.hpp`); `{env:NAME}` takes only the
  allow-listed folder variables, since command lines are logged. (Test harnesses start their own children:
  `tests/support/child_process.hpp`, `rv_fake_emulator`.)
- Product UI strings live in the RML documents under `assets/ui` (Italian until localization tables exist); sizes in dp,
  bundled fonts only, no color transitions (RmlUi rebuilds the geometry of every animated frame).
- Language: identifiers, comments, documentation and commit messages in English.

