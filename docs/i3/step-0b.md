# I3 step 0b: primitives in `rv_core`

Roadmap row: I3 step 0b (main, after 0a; 0c and 0d ran beside it in their own worktrees). Decisions:
[ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md) §7 (seeds and the random generator), §8 (determinism
and the canonical form), §9 (atomic writes and the last 10 plans), §12 (where generation runs);
[ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md) §6 (the worker pool). The step fills the
five `rv_core` stubs step 0a listed for it and makes `writeFileAtomic` flush before its rename. Nothing uses them yet:
steps 1a (the plan's reader and writer), 3 (seeds, generation), 9 (the plan's store, backups), 10b and 12 (the pool)
build on them. Only `rv_core` and its tests changed; no CMake list was touched.

## What was built

| File | What it does |
|---|---|
| `rv/core/seed.hpp`, `src/seed.cpp` (0a's stubs) | `kSeedTupleTag` (`rv-seed/1`), `SeedPart` (text, or an integer as its decimal digits), `deriveSeed(span)` and the variadic `deriveSeed(parts...)`, the `SeedInteger` concept |
| `rv/core/random.hpp`, `src/random.cpp` (0a's stubs) | `kRandomAlgorithm` (`splitmix64/1`), `Random`: `next()`, `below()`, `between()`, `unit()`, `shuffle()`, `state()` |
| `rv/core/canonical_json.hpp`, `src/canonical_json.cpp` (0a's stubs) | `writeCanonicalJson()`, `canonicalJsonDigest()`, `parseCanonicalJson()` |
| `rv/core/worker_pool.hpp`, `src/worker_pool.cpp` (0a's stubs) | `WorkerPool` (`WorkerPoolOptions`, `submit`, `cancel`, `waitIdle`, `shutdown`, `kShutdownBound`, `kHighestPriority`), `JobGeneration`, `JobContext`, `JobOptions`, `Job`, `defaultWorkerThreads()` |
| `rv/core/backups.hpp`, `src/backups.cpp` (0a's stubs) | `backUpFile()` (`BackupOptions`, `BackupReport`), `listBackupsOf()` |
| `rv/core/file_io.hpp`, `src/file_io.cpp` | `writeFileAtomic()` on Win32: the temporary file flushed (`FlushFileBuffers`) before a write-through rename (`MoveFileExW`); `detail::writeFileAtomicStoppingAt()`, the seam of the interrupted-write tests |
| `scripts/world_seed_oracle.py` | the independent oracle: golden seeds, sequences, distributions and canonical texts from Python's hashlib and json (`--check <file>...`) |
| `tests/unit/core/test_seed_random.cpp`, `test_canonical_json.cpp`, `test_worker_pool.cpp`, `test_atomic_write_backups.cpp` | the tests (below) |

Typical use (steps 3, 9 and 12 will do this):

```cpp
const std::uint64_t world = rv::core::deriveSeed("world", libraryId);           // ADR-0010 §7
const std::uint64_t room = rv::core::deriveSeed(world, "room", "room:snes");    // a parent seed enters as a number
rv::core::Random random(rv::core::deriveSeed(room, "prop", "fx:w03/l:2"));
const auto choice = random.below(dressings.size());                             // never std::uniform_int_distribution

auto text = rv::core::writeCanonicalJson(planJson);                              // the plan file's bytes
const std::string digest = rv::core::sha256Hex(*text);                           // the plan digest
rv::core::backUpFile(planPath, planPath.parent_path() / "backups");              // the last 10 kept
rv::core::writeTextFileAtomic(planPath, *text);                                  // flushed, then renamed

rv::core::WorkerPool pool;                                                       // min(6, hardware threads - 3)
rv::core::JobGeneration covers;
pool.submit([](const rv::core::JobContext& job) { /* ... if (job.cancelled()) return; ... */ },
            {.priority = projectedSize, .generation = &covers});
pool.cancel(covers);                                                             // a new room: drop the old jobs
```

## The definitions, exactly

`scripts/world_seed_oracle.py` holds the same definitions in its docstring and computes every golden value with
Python's own `hashlib`, `json` and integers; the C++ code shares nothing with it but the text below.

### Seeds

`deriveSeed(parts...)` is SHA-256 (`rv/core/sha256.hpp`, ADR-0008 §1) over the UTF-8 bytes of the tuple
(`rv-seed/1`, parts...), the elements separated by one NUL byte each (none after the last), the digest's first 8 bytes
read big-endian. An integer part is its decimal ASCII digits with `-` before a negative one, so `deriveSeed(7)` and
`deriveSeed("7")` are one seed and a parent seed enters its children's tuples as its decimal digits
(`deriveSeed(worldSeed, "room", roomId)`). No part: the tag alone; an empty part is an element of its own. Integer types
are every `std::integral` but `bool`, `char`, `wchar_t`, `char8_t`, `char16_t` and `char32_t` (a stray `'a'` or `true`
does not compile); `signed char` and `unsigned char` are `std::int8_t` and `std::uint8_t` and count as numbers.

The world's default seed of the reference library id `esde` is `deriveSeed("world", "esde")` = `0xa1fac9cfdf9c5474`.

### `Random` (`splitmix64/1`)

- `next()`: the state advances by `0x9E3779B97F4A7C15` (mod 2^64); the output is the advanced state `z` mixed by
  `z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9`, `z = (z ^ (z >> 27)) * 0x94D049BB133111EB`, `z ^ (z >> 31)` (Vigna's
  `splitmix64.c`: from a state of 0 the first output is `0xE220A8397B1DCDAF`, checked).
- `below(n)`, `n >= 1`: outputs `x` are drawn until `x >= (2^64 - n) mod n`, the result is `x mod n` (unbiased; at least
  one output, `n = 1` included). `below(0)` is 0 and draws nothing.
- `between(lo, hi)`, signed 64-bit, both included: `lo + below(hi - lo + 1)` in two's complement; for the whole range
  (`hi - lo + 1 = 2^64`) `lo + next()`. `lo` when `hi < lo`, nothing drawn.
- `unit()`: `(next() >> 11) * 2^-53`, in [0, 1), exact in IEEE double arithmetic.
- `shuffle(items)`: Fisher-Yates from the last item, for `i = n - 1` down to 1 items `i` and `below(i + 1)` swapped.
- `state()`: the seed advanced once per output drawn (the goldens check it after each case, so a distribution that
  draws one output too many or too few is caught).

Any change of these is a new algorithm id (`kRandomAlgorithm`, recorded in the plan as `randomAlgorithm`).

### Canonical JSON

`writeCanonicalJson(value)`:

- object members sorted by their keys' code points, which is the order of their UTF-8 bytes (RFC 8785 sorts by UTF-16
  code units: the two differ only above U+FFFF, where U+FFFF sorts before U+10000 here, after it there);
- integers in decimal, `-` before a negative one, no fraction, no exponent; a floating-point number (`1.0` included) is
  refused, as are binary and discarded values and nesting deeper than 64 levels (`InvalidArgument`, the value's JSON
  pointer as context);
- strings in UTF-8 (malformed UTF-8 refused) with exactly `"`, `\` and U+0000 to U+001F escaped: `\b \t \n \f \r`, the
  others `\u00XX` in lowercase hexadecimal; `/`, DEL and everything above U+007F as they are (RFC 8785's escaping);
- arrays in the order given: what a document defines as sorted, its writer sorts;
- no white space between tokens, except that a top-level array (the document when it is an array, else a member of the
  document's object) that is not empty holds one element per line: `[`, LF, the elements separated by `,` and LF, LF,
  `]`; an empty one is `[]`, an array inside a top-level array stays on its element's line;
- the text ends with one LF.

A plan therefore reads `{"fixtures":[`, one fixture per line, `],"placements":[`, one placement per line, `],...}` and
diffs line by line. `canonicalJsonDigest(value)` is the SHA-256 of that text in lowercase hex (the plan digest).
`parseCanonicalJson(text, context)` reads strict JSON (no comments) and refuses (`ValidationFailed`, naming the byte
where it departs) any text that is not exactly the canonical text of what it holds: white space, key order, a key
twice, `-0`, `1.0`, `"A"`, `"\/"`, a byte order mark, a missing or second final LF.

### `writeFileAtomic`

The data goes to `<name>.tmp-<process id>-<n>` beside the destination (`CreateFileW`, no sharing), is flushed with
`FlushFileBuffers`, and only then renamed over the destination with `MoveFileExW(MOVEFILE_REPLACE_EXISTING |
MOVEFILE_WRITE_THROUGH)` (NTFS journals the rename itself; write-through only matters for a move done as a copy,
which a temporary file beside its destination never needs); on any error the temporary file is deleted. A crash or a power loss at any point leaves either
the previous file or the new one, whole; one before the rename may leave the temporary file behind. Errors now come from
`fileErrorCode()` of the failing call: access denied (a destination held open without delete sharing, read-only, or a
folder) stays `IoError`, an unreachable drive becomes `Unavailable`; the messages changed with them. Before, the write
used `std::ofstream` and `std::filesystem::rename`: the data reached the operating system's cache, not the disk, before
the rename, and the temporary name `<name>.tmp-<n>` could collide between the app and `retroverse-cli`. Every caller
(the CLI's `library create`, the image and GLB writers, the test fixtures) works as before; only the error codes and
messages of a failed write changed. The flush costs a few milliseconds per file on an SSD: no caller writes files in a
loop that a frame waits for.

### Backups

`backUpFile(file, folder, options)` copies `file` (read whole: plans are small) into `folder` as
`<stem>-<YYYYMMDDTHHMMSS.mmmZ>[-<n>]<extension>` (UTC; `n` from 2 when two backups share a millisecond, at most 1,000),
written with `writeFileAtomic`, then removes the backups of that name beyond the newest `keep` (default 10), ordered by
the time and counter their names hold; the backup just written is always kept, even when a clock set back dates it the
oldest. Only names of exactly that form count (another file's backups, near misses, temporary files are left alone).
`listBackupsOf(file, folder)` lists them newest first. The name form is the catalog database's
(`catalog_db::listBackups()`), whose own copy (`VACUUM INTO`, `-v<version>`) was left as it is.

### `WorkerPool`

- Threads: `WorkerPoolOptions::threads`, 0 meaning `defaultWorkerThreads()` = `min(6, hardware threads - 3)`, at least 1
  (5 on the reference machine's 8; 0 hardware threads, unknown, gives 1). Each thread sets
  `THREAD_PRIORITY_BELOW_NORMAL` (unless `belowNormalPriority` is false) and the description `<name> <n>`.
- Bounded: the queue holds `capacity` jobs (default 4,096; stale ones count until dropped); a full queue refuses with
  `Unavailable`, an empty job with `InvalidArgument`, a shut-down pool with `Cancelled`.
- Priorities: a signed 64-bit `JobOptions::priority`, higher first, equal ones in submission order;
  `WorkerPool::kHighestPriority` is the world's generation job's.
- Cancellation by generation: a `JobGeneration` is a counter shared by its copies; a job submitted with
  `.generation = &g` belongs to `g.current()`. `g.advance()` makes every earlier job stale: a queued one is dropped when
  its turn comes, a running one sees `JobContext::cancelled()`; `pool.cancel(g)` also removes the queued ones at once
  (freeing capacity) and returns how many. `JobContext::generation()` lets a result carry its generation, so the main
  thread drops one that arrives late.
- Shutdown: `shutdown(bound)` (the destructor uses `kShutdownBound`, 2 s) refuses new jobs, drops the queued ones
  without running them, sets `cancelled()` for the running ones and joins the threads as they end; a thread still in a
  job at the bound is detached and logged, its state kept alive by the thread itself, and `shutdown` returns false.
- A job's exception is caught on its thread and logged (`Worker pool '<name>': a job failed: ...`); the pool goes on.

## Decisions taken within the step

1. **Integers in seed tuples are decimal text**, so one rule covers ids and parent seeds and the oracle needs no type
   tags; the cost, `deriveSeed(7) == deriveSeed("7")`, is harmless because every tuple names its kind (`"room"`,
   `"prop"`, ...). Character and boolean types are kept out at compile time.
2. **No runtime check for NUL inside a text part.** Parts are identifiers validated where they are read (step 1a's
   readers); a Debug-only assert would crash a developer build on a malformed asset, which style §21 forbids. The header
   states the precondition.
3. **`between()` over the whole range is `lo + next()`**, the same formula as every other range, rather than the output
   reinterpreted; the first oracle draft disagreed with the C++ code here and the test caught it.
4. **`below(0)` returns 0 and draws nothing; `between()` with `hi < lo` returns `lo`.** Total functions with documented
   results rather than undefined behaviour, both pinned by goldens.
5. **`shuffle()` is part of `splitmix64/1`.** `std::shuffle` is implementation-defined; the generator will need to
   permute (decoration choices), and a defined shuffle avoids the temptation.
6. **The file ends with LF and top-level arrays break per element.** ADR-0010 §8's "one element per line in the
   top-level arrays" read literally: the document's own arrays and its object's array members; nested arrays stay on
   one line, so the line structure is stable whatever a placement holds.
7. **`parseCanonicalJson` refuses non-canonical texts** instead of normalizing them: a plan edited by hand (or written
   by another tool) has a digest that is not the digest of its content's canonical form, and steps 4 and 9 must notice
   (`worldplan check`, the incremental update's "previous plan").
8. **Nesting is bounded at 64 levels** in the writer: it is recursive, and a hand-edited plan nested thousands deep
   must give an error, not a stack overflow (nlohmann 3.12 parses and destroys such a value without recursion).
9. **The pool refuses when full instead of blocking**: the main thread submits, and must never wait on a queue. Its
   callers (the cover loader) keep their own list of what to request and submit as room frees up.
10. **Generations are objects, not numbers in the pool**: each client owns its `JobGeneration` (the room's covers, the
    world's plans), so cancelling one kind never touches another's jobs and the pool needs no registry of kinds.
11. **No pause in the pool yet.** ADR-0011 §6's "while a launched game runs the pool takes no new job" is step 12's
    "pause hook for launches"; it is a flag in the queue's wait and belongs with its caller.
12. **The interrupted-write seam is `detail::writeFileAtomicStoppingAt()`**, a stop point in the real function rather
    than a hook or a child process killed mid-write: the test runs the same code path the product does up to the stop.
13. **The backup's names follow the catalog database's backups**, so the user data folder has one convention; the
    migrator's copy was not refactored onto `backUpFile()` (it copies through SQLite and holds a version in the name).

## Tests

- `test_seed_random.cpp` (`[core][seed]`, `[core][random]`): 18 golden seeds (no part, empty parts, separators keeping
  `("ab", "c")` and `("a", "bc")` apart, the ADR's local seeds, integers at the 64-bit limits, composed and decomposed
  é, a character outside the BMP), all distinct; the tuple's bytes built by hand; integers equal to their digits for
  every integer type, characters and booleans refused at compile time; the chain world, room, fixture, prop, place
  (5 goldens); `next()` for 5 seeds (0, 1, all ones, the gamma, the world seed), 6 outputs each, and the published
  first output of seed 0; `below()` for 9 bounds (1 to 2^64 - 1, two that reject about half the outputs) with the
  state after; `between()` for 8 ranges (empty, negative, the whole range, an inverted one); `unit()` as exact doubles
  (hexadecimal literals); `shuffle()` of 0, 1, 2 and 10 items for every seed; `below(6)` uniform over 60,000 draws
  within 3%, and both halves of a bound just above 2^63 alike.
- `test_canonical_json.cpp` (`[core][canonical_json]`): 17 golden documents (scalars, the 64-bit limits, empty and
  nested arrays and objects, a plan-like document, keys beyond ASCII in code point order, every escape, `\u007f`,
  U+2028, a surrogate pair) with their canonical text and digest; the round trip of every golden and of a document
  built in code byte-identical; floating-point numbers, binary values and malformed UTF-8 refused with their pointer
  (RFC 6901 escapes); every control character's escape; U+FFFF before U+10000; 64 levels written, 65 refused, a text
  nested 100,000 deep refused without a crash; 15 non-canonical or malformed texts refused, with the byte where one
  departs.
- `test_worker_pool.cpp` (`[core][worker_pool]`): the thread count table and the default pool's count; priorities and
  submission order on one thread behind a gate (8 jobs, `kHighestPriority` first); 4,000 jobs on 4 threads each run
  once, never on the caller's thread; cancellation by generation (queued jobs dropped at once, a new generation's job
  runs, a generation advanced without `cancel()` drops its jobs when their turn comes, copies share the counter); a
  running job sees its cancellation and its generation; the bound (`Unavailable` when full, room freed by `cancel()`,
  an empty job refused); a shutdown with two cooperative running jobs and 1,000 queued ends within 2 s with none of the
  queued run, then refuses jobs (`Cancelled`); a job ignoring its cancellation is detached after a 100 ms bound,
  logged, and ends by itself after the pool is gone; threads at `THREAD_PRIORITY_BELOW_NORMAL` (and normal when asked);
  exceptions (a `std::exception` and an `int`) logged and the next job run.
- `test_atomic_write_backups.cpp` (`[core][file_io]`, `[core][backups]`): a write stopped after half its bytes and one
  stopped before the rename leave the previous plan, their temporary files named with the process id, and a later
  write succeeds; a destination held open without delete sharing (and one that is a folder) refuses the replace,
  keeps the previous file and leaves no temporary file; 12 backups with the default `keep` leave the 10 newest, each
  backup removing the oldest; three backups in one millisecond (`-2`, `-3`), a clock set back an hour (the new backup
  kept, the two oldest by name removed), near-miss and foreign names left alone; a missing file (`NotFound`, no folder
  created), `keep = 0` taken as 1.
- `python -I scripts/world_seed_oracle.py --check tests/unit/core/test_seed_random.cpp
  tests/unit/core/test_canonical_json.cpp`: 87 of 87 golden lines found.

Results on the reference machine (RTX 2080 SUPER, 8 hardware threads), with other builds running at the same time:
`msvc-x64-debug` and `msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` passed 1,306 unit tests in Debug
(347 s) and 1,306 in Release (204 s), 33 more than step 0a's 1,273; the oracle check found 87 of 87 lines.

A first run of the pool's tests while a Release build saturated every core showed what below-normal priority means: a
pool constructed and destroyed at once waited its whole 2 s bound and detached a thread that had not been scheduled
yet. Windows runs a below-normal thread only when no normal one is ready, and lifts a starved thread only after about
4 s. The tests therefore run their pools at normal priority (`testPool()`), except the one that checks the priority
itself, which waits up to 30 s; the product keeps below-normal threads, as ADR-0011 §6 decides.

## Notes for later steps

- **1a** (the plan's reader and writer, `plan_json.hpp`): write the plan with `writeCanonicalJson`, read it with
  `parseCanonicalJson` (a hand-edited plan is refused, not normalized), and sort placements and fixtures by id before
  writing: the writer keeps array order. Numbers in the plan must be `std::int64_t` or `std::uint64_t` in the JSON
  value; a `double` that happens to be whole is still refused. Identifiers read from templates and manifests must be
  validated (no NUL, well-formed UTF-8) before they become seed parts.
- **3** (`seeds.hpp`): build the local seeds with the variadic `deriveSeed(parent, "room", roomId)`; a parent seed is
  passed as the integer, which enters as its decimal digits. Record `core::kRandomAlgorithm` as `randomAlgorithm`.
  Draw with `Random::below`/`between`/`unit`/`shuffle` only.
- **9** (`world_store.hpp`): `backUpFile(planPath, worldFolder / "backups")` before each `writeTextFileAtomic`, under
  the world lock (two backups racing in one millisecond could otherwise pick one name). A crash before a rename leaves
  `world-plan.json.tmp-<pid>-<n>`; the store may delete such files in its folder when it takes the lock.
- **10b, 12**: one `WorkerPool` for the app (default options); the world's job at `kHighestPriority` with its own
  `JobGeneration`, the cover loader's jobs under another, advanced (`cancel`) when the room or plan changes. Results
  carry `JobContext::generation()` and the main thread drops those of an older generation. A full queue returns
  `Unavailable`: keep the request list on the main thread and top the queue up each frame. The pause for launches
  (ADR-0011 §6, step 12) is not in the pool yet: add it in `worker_pool.cpp` (a paused flag in the workers' wait).
- **13r, quitting**: the pool's destructor waits 2 s at most; jobs that read slow drives should check `cancelled()`
  between reads so the bound is met. Below-normal threads also wait for every normal-priority thread: while the main
  thread renders unthrottled (`--no-vsync`) on a machine whose other cores are busy, the pool may make no progress,
  and step 12's cold-cache budget is measured with that in mind (the main thread waits in vsync or in the lockstep).

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Conventions", core helpers of I2: add the I3 ones: `seed.hpp` (`deriveSeed`: SHA-256 of the NUL-separated tuple
    `rv-seed/1`, parts..., integers as decimal digits, the first 8 bytes big-endian), `random.hpp` (`Random`,
    SplitMix64 with its own `below`/`between`/`unit`/`shuffle`, `kRandomAlgorithm` `splitmix64/1`; never the standard
    distributions), `canonical_json.hpp` (`writeCanonicalJson`, `canonicalJsonDigest`, `parseCanonicalJson`: keys by
    code point, integers only, top-level arrays one element per line, a final LF), `worker_pool.hpp` (`WorkerPool`:
    `min(6, hardware threads - 3)` threads below normal priority, bounded, priorities, `JobGeneration` cancellation,
    2 s shutdown bound), `backups.hpp` (`backUpFile`, the newest 10 by default, names as the database's backups);
    `file_io.hpp`'s `writeFileAtomic` flushes before a write-through rename.
  - "Determinism": golden seeds, sequences and canonical texts come from `scripts/world_seed_oracle.py` (Python's
    hashlib and json), and `python -I scripts/world_seed_oracle.py --check tests/unit/core/test_seed_random.cpp
    tests/unit/core/test_canonical_json.cpp` must pass (beside the GameId and quick-hash oracles).
- **docs/architecture.md**: the `core` row gains seeds, the random generator, canonical JSON, the worker pool and
  rotating backups.
- **docs/roadmap.md**: step 0b done.
- Test counts: 1,306 unit tests in Debug and Release (step 0a's 1,273 plus 33).
