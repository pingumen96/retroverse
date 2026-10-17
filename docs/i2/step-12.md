# I2 step 12: CLI launches and sessions

Roadmap row: I2 step 12 (wave 3, after 11a, parallel with 11b). Decisions: [ADR-0008](../decisions/ADR-0008-esde-catalog.md)
§5 (state tables), §6 (choices in the database, never in the user's files), §8 (the choice of command, readiness,
"verified"), §9 (play sessions); ADR-0006 (the wait strategies, never a kill). It builds on step 5 (`LaunchSession`, the
runner, the guard, the isolated RetroArch profile), step 10 (`resolveLaunch`), step 11a (GameIds in the catalog) and A1
(`LaunchFlow`). This page is folded into the shared documentation at the wave merge.

Outcome: `retroverse-cli launch --game <id>` launches a game of the catalog, resolved right before the launch, through
`LaunchSession` on `Win32ProcessRunner`, and records every attempt as a play session (`rv_play_sessions`); `--wait`
follows it to its end, `--test` records a launch verification. `assign` stores Retroverse's choice of command or profile
for a system or a game in the catalog database; `sessions` lists the attempts. A process that ends while its session is
open leaves it to the next start, which closes it as interrupted, and only then. Nothing is ever terminated.

## What was built

| File | What it does |
|---|---|
| `src/catalog_db/src/schema_launch.cpp` | The launch area of schema v1: `rv_play_sessions`, `rv_launch_game_choices`, `rv_launch_system_choices`, `rv_launch_verifications` (state tables, STRICT, foreign keys into `rv_*` only, never cascading) |
| `rv/catalog_db/session_store.hpp`, `src/session_store.cpp` | `SessionStore` (sessions, choices, verifications, alias resolution, the busy retry), `BusyRetry`, `kPatientRetry`, `SessionRecorder` (a session write that meets a busy database is kept and tried again), `StoredChoice`, `SystemChoiceRow`, `LaunchVerification`, `SessionFilter`, `ListedSession`, `OwnerAlive` |
| `rv/catalog_db/database.hpp`, `src/database.cpp` | `isBusyError()`: SQLITE_BUSY or SQLITE_LOCKED, told from a UNIQUE conflict (both are `Conflict`) |
| `rv/catalog/play_session.hpp` (step 1's, filled) | `PlaySessionOutcome::Blocked`, `LeftRunning`; `LeftRunningReason`; `PlaySession::choice` (a `launcher::LaunchChoice`, replacing `profile` and `commandLabel`), `choiceSource`, `test`, `closed`, `runTime`, `readiness`, `leftRunning`, `refusedByGuard`, `processId`; `recordOutcome()` takes the owner's reason; `returnedNormally()` |
| `rv/platform_win/win32_process_runner.hpp`, `src/win32_process_runner.cpp` | `ProcessIdentity`, `currentProcessIdentity()`, `ProcessLiveness`, `processLiveness()`, `listedProcess()`: a session owner's id and creation time, and whether it still runs (`rv_platform_win` links `ntdll` for the system's process list) |
| `src/launcher/src/launch_session.cpp` | The command line and working directory are logged at debug level only (ADR-0008 §9); info says what is launched and how it is waited for |
| `tools/cli/src/cmd_launch.cpp` | `launch --game` (`--wait`, `--wait-limit`, `--test`), `launch --dry-run --game`; the path dry run of step 10 kept, with assign's choice for the system |
| `tools/cli/src/cmd_assign.cpp`, `cmd_sessions.cpp` | `assign`, `sessions` |
| `tools/cli/src/session_text.hpp` (new private header, in no source list) | the catalog opened to record launches (`openLaunchCatalog`), the session owner and the recovery at start, `readCatalogGame` (a `--game` checked: an alias resolved, the library, a game or a missing file), sessions as text and JSON |
| `tools/cli/src/launch_text.hpp`, `cmd_profiles.cpp` | `LaunchLibrary::assignedChoices`, `loadAssignedChoices()`, `systemChoiceOrigin()`, `whyText()`; `chosenJson()` gains `origin`; `profiles` shows assign's choices and where each system's choice comes from |
| `tests/support/scoped_environment.hpp` (new, header-only) | `ScopedEnvironmentVariable`, `ScopedLaunchAllowList` (RV_LAUNCH_ALLOWLIST for one scope), `ScopedPathPrefix` |
| `tests/unit/catalog_db/test_session_store.cpp`, `tests/unit/cli/test_cli_sessions.cpp`, `tests/unit/platform_win/test_process_identity.cpp` | the tests (below) |
| `tests/unit/catalog/test_domain.cpp`, `tests/unit/cli/test_cli_launch.cpp` | the domain's new values; launch without `--dry-run` now needs `--game` |
| `tests/unit/CMakeLists.txt` | `rv_tests` depends on `retroverse-cli` (started as a process by the crash-recovery and restart tests) |

## The launch area of schema v1

| Table | Holds |
|---|---|
| `rv_play_sessions` | every launch attempt (`PlaySession`), its id AUTOINCREMENT (shown to users): the game (its identity, an alias resolved when written), the choice (`choice_kind` `command` or `profile`, `choice`: an ES-DE label, `''` for a single unlabelled command, or a profile id) and its source (`ChoiceSource`), `test`; UTC times `started_at`, `ended_at` (NULL while running and for interrupted sessions), `closed_at` (when a later start closed an interrupted one); `run_ms` (monotonic: from the moment the process ran to its authoritative exit, or to the moment its owner stopped following it); the outcome, `readiness` (blocked), `left_running` (`LeftRunningReason`), the launcher's `failure`, `exit_code`, `exit_kind` (nullable since the integration of steps 11b and 12, [step-11b-12i.md](step-11b-12i.md): present with `exit_code` only, never for a blocked or guard-refused attempt nor for the failures `invalid request` and `start failed`, always for a crash), `quick_exit`, `confirmed_by_user`, `processes_remained`, `refused_by_guard`, `message`; `process_id` (the started process); the owner `owner_pid` and `owner_created` (FILETIME ticks). CHECKs tie the outcome to its fields (an end time unless running or interrupted, a readiness only when blocked, a reason only when left running). Indexed by (game, id) and on the open sessions |
| `rv_launch_game_choices` | Retroverse's choice for a game (`assign --game`): its GameId as INTEGER PRIMARY KEY, kind, value, `chosen_at` |
| `rv_launch_system_choices` | Retroverse's choice for a system (`assign <system>`), by (library, system) |
| `rv_launch_verifications` | per (library, system, kind, value) the latest session that returned normally with that choice: the game, the session, whether it was a launch test, the program that ran, `verified_at` |

Every table that names a game refers to `rv_game_identities (id)`, never to `idx_*`, never cascading: `db rebuild-index`
leaves them alone (tested), and step 11b's re-pointing of an alias's rows (UPDATE OR IGNORE, then delete what is left,
found from the schema's foreign keys) moves them with nothing else to do: a game's sessions all move, its choice moves
unless the moved identity has its own (tested with that procedure). Enumerations are stored as their `toString()` names
(the launcher's `FailureReason` and `ExitKind` included, which read "start failed", "loader error").

## Sessions (ADR-0008 §9)

- **What is recorded, and when.** `launch --game` records the attempt when it begins (`running`, the owner this
  process, `started_at` now) and again when it ends. A game that is not ready right before the launch is not started:
  the attempt is recorded at once as `blocked` with its readiness and its blockers' codes and messages. Outcomes:
  `returned` (with `quick_exit` for a clean exit within the startup grace, `processes_remained` for a wrapper whose
  children still run), `failed-to-start` (the launcher's failure: invalid request, start failed, loader error, early
  exit; `refused_by_guard` when the automation guard refused it, decided by its verdict on the request), `crashed`,
  `blocked`, `left-running`, `interrupted`. The choice recorded is the one used, or the one that was blocked.
- **Left running.** When the owner ends a session with `LaunchSession::confirmReturn()` while the game still runs, the
  session is `left-running` with its reason: `not-waited` (a launch without `--wait`), `wait-limit`, `quit` (Ctrl+C,
  the app closed), `confirmed` (the user said they were back). `confirmed_by_user` holds only for the user's own word. A
  session that awaited the user's confirmation (`UserConfirmReturn`) and was stopped for another reason is left running
  whatever its tree says: its end is that confirmation, which never came. Nothing is ever terminated.
- **Times.** UTC wall-clock text (`core::formatUtcIso8601`); the end's wall-clock time is the start's plus the
  monotonic time to the authoritative exit (the system's exit time of the process, `SessionTimes::exited`) or to the
  owner's return, so a late observation (a busy database, a slow poll) does not move it.
- **Owners and interrupted sessions.** The owner is the recording process's id and creation time
  (`platform_win::currentProcessIdentity()`). At the start of every command that records sessions (`launch --game`,
  `sessions`; the app at step 13) `SessionStore::closeInterrupted()` closes as `interrupted` (with `closed_at`, no end
  time) the open sessions whose owner is gone (`platform_win::processLiveness()`: no process has the id, the one that
  has it was created at another time, or it exited), and only those: a session another live retroverse-cli or app
  follows stays open. A process with the owner's id that this user may not open (access denied: a service, another
  user's process, typically one that took the id after a reboot) is compared through the system's process list
  (`listedProcess()`: `NtQuerySystemInformation(SystemProcessInformation)` gives every process's id and creation time
  without any access right): another creation time is gone, the owner's own running (an elevated owner). Only when that
  list cannot be read either is the owner taken as running (`Unknown`).
- **A busy database.** Session writes are short write transactions on the process's writer and never touch identities
  or index rows (no import lock). Another process's import can hold SQLite's write lock longer than the writer's busy
  timeout (5 to 9 s for a bulk import on the reference library, step 11a): `SessionStore` tries a write that meets a busy
  database (`isBusyError()`) again for as long as its `BusyRetry` says, and `SessionRecorder` keeps a write it could not
  make yet, the latest state winning. `launch --game` opens the database with a 1 s busy timeout: the first write gets
  one attempt (a launch never waits longer for the database), a pending write is tried again every 2 s while the game
  runs, and the end is written with `kPatientRetry` (2 minutes). A session that still cannot be written is printed with
  the error, and the command exits 3: nothing is lost silently.
- **Aliases.** A GameId that became an alias (step 11b) stands for the identity it names in every write and read of the
  store (`launch --game`, `assign --game`, `sessions --game` take it too), so a game aliased while it ran keeps its
  history.

## The choice and its ranking

ADR-0008 §8's order, most specific first: Retroverse's choice for the game (`assign --game`), the game's `<altemulator>`
(with `AlternativeEmulatorPerGame`), Retroverse's choice for the system, the gamelist's `<alternativeEmulator>`, the
first command. **Retroverse's choice for the system is the catalog database's (`assign <system>`) when there is one,
else emulators.json's `"systemChoices"`**: `assign` is an explicit action of the user, the latest one, while
emulators.json is a configuration file that may have been written long before; `profiles` shows both, which one is in
effect (`"origin": "assign"` or `"emulators.json"`, the text "(assign's choice ranks first)"), and `assign <system>
--clear` gives emulators.json's back. Both are the same `ChoiceSource::RetroverseSystem`: `rv_catalog` is unchanged, the
callers fill `LaunchChoices::system` with the first of the two (`launch_text.hpp`'s `systemChoices()`). A stored choice
is honoured or blocks, never skipped: a label or profile that disappears later makes the launch "requires launcher
configuration" (`choice-stale`, `profile-unknown`), naming it, until another one is assigned or the choice cleared.

## `retroverse-cli`

```text
retroverse-cli [global options] launch --game <id> [--wait [--wait-limit <seconds>]] [--test] [--json] [--output <file>]
retroverse-cli [global options] launch --dry-run --game <id> [--json] [--output <file>]
retroverse-cli [global options] launch --dry-run --system <name> --path <relative path> [--json] [--output <file>]
retroverse-cli [global options] assign <system> [<label|profile>] [--command <label> | --profile <id> | --clear] [--json]
retroverse-cli [global options] assign --game <id> [<label|profile>] [--command <label> | --profile <id> | --clear] [--json]
retroverse-cli [global options] sessions [--game <id>] [--limit <n>] [--json] [--output <file>]
```

**`launch --game <id>`** (the GameId `games`, `search` and `show` print; an alias stands for its game): the game must be
of the library library.json records and a game or a missing file (a disc member, a folder or a file that is not a game
for ES-DE is refused, exit 2). Right before the launch it is resolved with step 10's `resolveLaunch()`: the system as the
library configures it now, the game's file looked for on disk now (`esde::listedPath`), its `<altemulator>` and its
system's `<alternativeEmulator>` as the last import read them, the choices from the database. Ready, it starts through
`LaunchSession` on `Win32ProcessRunner`, which applies the automation guard:

- **without `--wait`** the launch is followed through its startup grace (2 s) and half a second more, so that a failure
  to start, a crash or a quick exit is recorded as such; a game still running then is recorded as `left-running`
  (`not-waited`) and the command returns 0, the game running on;
- **`--wait`** follows the session to its end as its wait strategy says (ADR-0006: the started process; the process tree
  for `"wait": "tree"`; the user's confirmation for `"wait": "confirm"`, which Ctrl+C gives, the command telling so on
  stderr), then records the outcome. Ctrl+C while the game runs stops waiting (`left-running`, `quit`); `--wait-limit
  <seconds>` (1 to 86400) stops waiting after that long (`wait-limit`). A window timeout (StillRunning) is only noted on
  stderr: the wait goes on;
- **`--test`** waits, and when the game returned normally (a clean exit, code 0, that was no quick exit, nothing left
  running: `catalog::returnedNormally()`) records a launch verification for the choice and the system; it exits 1
  otherwise. Every launch that returned normally records one too (ADR-0008 §8: "verified" is a clean return without a
  quick exit, or a launch test).

Text: the resolution's lines as the dry run prints them (game, title, ROM, launches with and why, readiness, program,
core, blockers), then `Started` (time, process), `Outcome`, `Message`, `Ran for`, `Session` (its id, or `NOT RECORDED`
with the error), `Verified`; sessions closed as interrupted at this start come first. JSON: the dry run's keys (`game`,
`requestedGame`, `system`, `path`, `listedPath`, `directory`, `availability`, `romPath`, `title`,
`alternativeEmulator`, `chosen` with `origin`, `command`, `readiness`, `ready`, `program`, `core`, `blockers`,
`attempts`, `notes`, `request`, `commandLine`, `guard`), then `started`, `waited`, `test`, `session` (below),
`sessionRecorded`, `sessionError`, `verified`, `verificationError`, `interrupted` (sessions). Exit codes: 0 started and
returned (or left running: not waited for, the wait limit, Ctrl+C), for `--test` only verified; 1 not ready (blocked,
recorded), failed to start, crashed, a quick exit, a `--test` that did not return normally, a `--game` the catalog does
not have; 2 invalid options, a `--game` of another library or that is not a game; 3 ES-DE's configuration or the catalog
cannot be read (no catalog yet: an import makes it), a session that could not be recorded.

**`launch --dry-run`** is step 10's, unchanged in its options and output, with `--game` besides `--system`/`--path`.
Both show Retroverse's choices from the database (read only: never made, migrated or written): `--game` the game's and
the system's, `--path` the system's (a per-game choice needs the game's id: `--dry-run --game`). `chosen.origin` says
where a Retroverse choice came from.

**`assign`** stores Retroverse's choice for a system or a game in the catalog database, never in the user's files. A
value is checked against what exists: an ES-DE command label of the system (compared exactly) or the id of a profile
serving it (its `"systems"`); a value that is both is refused until `--command` or `--profile` says which; a profile that
exists but does not serve the system is refused, naming that. Without a value it shows the stored choice; `--clear`
removes it. It prints the choice, what it replaces, emulators.json's choice for the system and its rank, and the
choice's readiness for the system (what does not depend on a game). JSON: `library`, `system`, `game` (`id`,
`requested`, `title`, `path`, or null), `action` (`show`, `set`, `clear`), `choice` and `previous` (`kind`, `value`,
`chosenAt`, or null), `changed`, `emulatorsJsonChoice`, `readiness`, `blockers`, `commands`, `profiles`. Exit codes: 0;
1 a `--game` the catalog does not have; 2 invalid options, an unknown system, a value the system does not have, a `--game`
of another library or that is not a game; 3 the catalog cannot be read or written (no catalog yet, or a library it does
not hold: an import records it).

**`sessions`** lists the attempts, newest first, after closing the sessions left open by owners that are gone: each with
its id, start, how long the game ran, its system, title and path, GameId, the choice (and "launch test"), the outcome in
words and, for outcomes other than returned, the message. `--game` lists one game's (an alias stands for its game),
`--limit` caps the list (50; 0 for all). JSON: `game`, `limit`, `sessions` (each session's keys: `id`, `game`, `choice`
{`kind`, `value`, `source`}, `test`, `startedAt`, `endedAt`, `closedAt`, `durationMs`, `outcome`, `outcomeText`,
`readiness`, `leftRunning`, `failure`, `exitCode`, `exitKind`, `exitDescription`, `quickExit`, `confirmedByUser`,
`processesRemained`, `refusedByGuard`, `message`, `processId`, `owner` {`processId`, `creationTime`}; and `library`,
`system`, `path`, `title`), `interrupted` (the sessions closed now). Since step 11b-12i `exitKind` is null when the
session has no exit code, and `requestedGame` follows `game`: with `--game <alias>`, `game` is the identity the alias
stands for and `requestedGame` the alias (both null without `--game`), and the text says "Sessions of <identity>
(for <alias>, an alias)."; `assign --game <alias>`'s text line "For" ends "(for <alias>, an alias)" too. Exit codes:
0; 1 a `--game` the catalog does not know; 2 invalid options; 3 the catalog cannot be read.

**`profiles`** shows `Assigned` (the database's system choices) before emulators.json's, marks an emulators.json choice
that assign's outranks, and says for each system where its choice comes from (JSON: `assignedChoices`, `chosen.origin`).

## Logging

`LaunchSession` logs "Launching <label> (<wait strategy>)" at info and the working directory and command line at debug
only (ADR-0008 §9, A1's note); `launch` logs the request's command line at debug only, the GameId, the choice, the
readiness and the session's outcome at info. No test of the app, the smoke tests or the spikes read the line that
changed (the launch spike reports its command line from `Win32StartInfo`).

## Tests

`build/msvc-x64-debug/bin/rv_tests.exe "[sessions]"` (18 test cases), with `[catalog][domain]`:

- `tests/unit/catalog_db/test_session_store.cpp`: the launch area's tables and their rules (AUTOINCREMENT ids, foreign
  keys into the identities, a game choice's GameId as INTEGER PRIMARY KEY); every outcome written and read back
  unchanged (an exit code past 2^31, text outside ASCII, a run time, every reason); what the store refuses (fields that
  contradict the outcome, a stored id, text that is not UTF-8, a game that is neither an identity nor an alias: NotFound,
  an unknown id to update); only the open sessions whose owner is gone closed as interrupted (an id reused with another
  creation time is gone; finished sessions untouched; the owners asked outside any transaction); the listing (newest
  first, by game, limited, a game without an index row listed without a title); choices per game and per system (replace,
  clear, a library the catalog does not hold), an alias standing for its game in choices, sessions and listings;
  verifications (the latest per library, system and choice); a write that meets a busy database (another holder's BEGIN
  IMMEDIATE): one attempt gives a busy Conflict, the recorder keeps it pending, `finish()` with too little patience
  gives the error back, a write tried for long enough gets in after the commit; `isBusyError()` against UNIQUE conflicts;
  the state rows kept by `clearIndex()` (db rebuild-index) and re-pointed as step 11b re-points an alias's rows, with no
  foreign key violation.
- `tests/unit/platform_win/test_process_identity.cpp`: this process runs; the same id with another creation time is gone;
  the fake emulator running, then exited (its handle open, then closed): gone. The system's process list agrees with
  `GetProcessTimes` (this process, the fake; an exited process kept by its handle is listed without threads or not at
  all); a running process this user may not open (found at test time; skipped in an elevated run, where there is none)
  with another creation time, a 2022 one among them, is gone, with its own creation time running.
- `tests/unit/cli/test_cli_sessions.cpp` (the standard fixture imported; fake emulator profiles in the user's
  emulators.json; `RV_LAUNCH_ALLOWLIST` set to the fakes each test starts with `rv::test::ScopedLaunchAllowList`, and put
  back after):
  - every outcome with its session row: returned (2.6 s, verified), a quick exit (exit 1 and the hint), an error exit
    within the grace (failed to start, early exit, 3), a loader error (the fake exits 0xC0000135: the real loader's
    missing-DLL dialog would hold a test), a crash (0xC0000005), a program Windows cannot load (a program stub:
    CreateProcessW fails, failed to start, start failed), the guard's refusal (the fixture's unlisted copy: nothing
    created, `refusedByGuard`), an executable that is not there (blocked, emulator not found), a missing file (blocked,
    ROM missing, with the first command); the listing newest first, `--game`, `--limit`, the text; the verifications of
    the launches that returned normally only; a launch test verified, and one that is not (exit 1). The startup grace
    (2 s from the moment the process runs, its DLLs loaded within it) separates a quick exit from a return and an early
    exit from a return with an error code; with another worktree's build holding the processor at 100%, the Debug fake
    needed 2.0 to 2.6 s to start and exit, so the test judges those two by the session's own run time (the time the
    grace is compared with), launching again up to four times to get a run within it, and warns when it never does;
  - each wait strategy followed to its end: a wrapper waited for as a process tree (returned after the tree's 2.6 s, no
    quick exit, verified), a launcher stub awaiting the user's confirmation not waited for or given up on (left running);
  - hostile and long-path arguments byte-identical in the fake's argv (`--print-args`): a ROM past MAX_PATH named with
    `%`, `^`, `&`, `'`, parentheses, brackets and Unicode, and literal arguments with quotes, a trailing backslash after a
    space, `%PATH%`, `^`, `&`, `|`, `<`, `>`, Unicode, an empty argument and literal braces, through a profile (equal to
    the dry run's argv too) and through an ES-DE command (the fixture's Home Emulator command with `--print-args`, the
    copy finding its DLLs through PATH);
  - nothing killed: `--wait --wait-limit 2` and a launch not waited for both leave their 6 s game running, which then
    ends by itself with exit code 0;
  - crash recovery with two retroverse-cli processes waiting for 6 s games: one killed mid-game (its game runs on; the
    next start, `sessions`, closes its session as interrupted, and only it), the other still running (its session left
    open, then recorded as returned when it ends normally);
  - assign checked against what exists (labels, profiles, profiles that do not serve the system, an unknown system, an
    unknown game, a disc member, a value both a label and a profile), ranking before emulators.json's choice (`profiles`
    shows the origin), surviving `db rebuild-index` and a restart (a second retroverse-cli process), cleared back to
    emulators.json's; a label that disappears blocks the launch (choice-stale) and the attempt is recorded; the user's
    emulators.json and library.json unchanged;
  - a session recorded while another process holds the database's write lock (two processes: the test process holds
    a BEGIN IMMEDIATE as a bulk import's write transaction holds it, while a retroverse-cli process runs `launch --game
    --wait`): the launch starts after one 1 s attempt (the log's "stays pending"), the game ends while the lock is still
    held (since step 11b-12i until the game's process has ended and 3 s more, since a fixed wait was too short on a
    loaded machine), the CLI keeps trying its session's end rather than exiting, and records it once the holder
    commits ("written at attempt N (the database was busy)"; `returned`, `sessionRecorded`, never exit 3). The import
    itself is not the holder: on the standard fixture it commits in milliseconds;
  - `launch --dry-run --game`: the game's choices, a missing file blocked (exit 1), a folder refused, an unknown game;
    the database file unchanged byte for byte, no session.
- `tests/unit/catalog/test_domain.cpp`: the new names; `recordOutcome()` with the owner's reasons (left running, the
  user's confirmation, a wrapper's processes not the owner's doing); `returnedNormally()`.
- `tests/unit/cli/test_cli_launch.cpp`: launch without `--dry-run` needs `--game`; option combinations refused.

Results (the reference machine, another worktree building throughout, so timings are informative): `msvc-x64-debug` and
`msvc-x64-release` build without a warning; `./scripts/test.ps1` passes every unit test in Debug (1,208, 254 s) and in
Release (1,208, 155 s). The step's CLI tests take 57 s in Debug and 44 s in Release, most of it the fake emulator's
runs (2.6 s and 6 s games, the startup grace waited for); the session store's tests about 2.5 s. A1's GPU launch cases
(`./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter gpu.launch`, 7 cases) pass with the command line logged at
debug level; no test of the app, the smoke tests or the spikes reads that line.

## RetroArch by hand, isolated

Run once, by hand, on the reference machine (2026-10-17, ES-DE and RetroArch closed, another worktree building), with
the Release `retroverse-cli` of this step, everything Retroverse wrote under `build/step12-real` of the worktree:

1. **Snapshots before** (`scripts/snapshot_tree.py snapshot before.json --hash <RetroArch folder> --hash <ES-DE home>
   --listing <gb's ROM folder>`): RetroArch's folder with its configuration, playlists,
   saves, states and logs hashed (48,524 files, 1,419 folders), the ES-DE home hashed (14,676 files), Mole Mania's ROM
   folder listed (8,629 files, 54 folders): 74,261 entries, taken before the library was even read.
2. **The library** (`library create`: the ES-DE home, the install, ROMDirectory and MediaDirectory found as ES-DE finds
   them) and **the import** (`import`, 34.5 s with the machine loaded and the drive cold: step 11a's figures, 105,130
   rows and identities, 13 issues, 195 systems).
3. **The isolated profile**: the append configuration written first, with the content `spikes/launch` writes for its
   isolated runs (`retroArchAppendConfig()`: nothing saved on RetroArch's own initiative, every directory and file it
   could write moved into `build/step12-real/launch-scratch`, fullscreen, muted, offline) and its scratch folders
   (`retroArchScratchDirectories()`); a user `emulators.json` in the data directory holding step 5's
   `retroarch-isolated` profile with the spike's arguments: `-L {var:core} --appendconfig=<scratch>\retroverse-isolated.cfg
   --max-frames=240 --sram-mode=noload-nosave --log-file=<scratch>\logs\retroarch.log --verbose {rom}`, gambatte for gb.
4. **`assign gb retroarch-isolated`**: stored, ready for the system. Mole Mania is
   `g1aeae0b244ed5501` (Mole Mania (USA, Europe) (SGB Enhanced), of the two USA entries of the gamelist the first; no
   `<altemulator>`, so the system's choice applies). `launch --dry-run --game g1aeae0b244ed5501` with
   `RV_LAUNCH_ALLOWLIST=<RetroArch folder>\retroarch.exe`: ready, "Retroverse's choice for the system
   (assign)", RetroArch found through App Paths, the core in `%EMUPATH%\cores`, the guard "allowed".
5. **`launch --game g1aeae0b244ed5501 --wait --wait-limit 120 --json`** with the same allow-list: RetroArch started
   (process 32204), ran the game fullscreen for 240 frames ("Content ran for a total of: 00 hours, 00 minutes, 04
   seconds" in its own log) and exited by itself; the CLI returned after 14.1 s, exit 0. Session 1: `returned`, exit
   code 0, no quick exit, nothing left running, 13.1 s from the moment the process ran to its exit, choice `profile
   retroarch-isolated` (`retroverse-system`), owner the CLI's process; verified (`rv_launch_verifications`: gb,
   retroarch-isolated). `sessions` lists it.
6. **RetroArch wrote only into the scratch folder**: its log, and the core options `config\Gambatte\Gambatte.opt`; its
   log says SRAM was neither loaded nor saved.
7. **Snapshots after**, compared: **0 differences among 74,261 entries**.

## Decisions taken within the step

1. **assign's choice for a system ranks before emulators.json's** (the user's latest explicit choice), both being
   Retroverse's choice for the system; `profiles` shows which one is in effect and where it comes from.
2. **A blocked game is a recorded attempt** (`blocked`, with its readiness), and so is a refusal by the guard
   (`failed-to-start`, `refused_by_guard`, decided by the guard's verdict on the request since `FailureReason` stays
   step 5's).
3. **Without `--wait` the launch is followed through its startup grace**, then recorded as `left-running`
   (`not-waited`): failures to start, crashes and quick exits are recorded as such, and a game that runs is never
   presented as interrupted.
4. **Ctrl+C during `--wait` stops waiting** (a console control handler for the wait only): `left-running` (`quit`), or
   the user's confirmation when the session asks for it (`UserConfirmReturn`). A CLI ended otherwise (killed, its
   console closed) leaves its session open to the next start.
5. **A session stopped while it awaited the user's confirmation is left running**, whatever the tree says.
6. **Verifications are state** (the latest per library, system and choice), recorded by every launch that returned
   normally and by `--test`, which reports whether it got one.
7. **The first session write gets one attempt with a 1 s busy timeout**, a pending one is retried every 2 s while the game
   runs, the end gets 2 minutes (`kPatientRetry`): a busy database never holds a game back, and a session is never lost
   silently (exit 3 with the session printed when it could not be written).
8. **An alias stands for its game** in the store and the CLI (`resolveGame()`, the inserts resolve it), so step 11b's
   aliases need nothing more of this step.
9. **`launch --game` takes the game's `<altemulator>` and its system's `<alternativeEmulator>` from the catalog** (the
   last import's), and its file, the systems and commands from disk now; the path dry run keeps reading the gamelist.
10. **`sessions` writes** (the recovery at its start): it opens the writer, as `launch` does.
11. **A loader error is tested through the fake's exit code**: a real missing DLL makes Windows show its loader dialog to
    a process with the default error mode, which would hold a test.

## Notes for step 13 (the app)

- Open a `SessionStore` on the catalog worker's writer; at the app's start call `closeInterrupted()` with
  `platform_win::processLiveness()` (as `session_text.hpp`'s `recoverInterruptedSessions()` does), then record every
  launch of `LaunchFlow` with a `SessionRecorder`: `begin()` when the session starts (a blocked game recorded at once as
  `blocked`), `update()` and `flush()` as it runs, `finish()` at its end (the worker may keep a pending write; the main
  thread never waits on the database). Owner: `currentProcessIdentity()`.
- Map A1's endings with `recordOutcome(session, outcome, ended, reason)`: the user's "Torna a Retroverse" / "Sono
  tornato" is the user's confirmation (`None`, or `Confirmed`), quitting while a game runs is `Quit`, the frame-limited
  wait limit `WaitLimit`; `returnedNormally()` records a verification (`recordVerification()`), "verified" on the card is
  a row of `verifications()` for the system and the choice in effect.
- Resolve the choices as `launch --game` does: `gameChoice()`, then `systemChoice()` before emulators.json's
  (`launch_text.hpp`'s `systemChoices()` shows the order).
- The command line is logged at debug only now: the app's run reports keep what they record.

## Notes for step 11b

Answered by the integration of steps 11b and 12 ([step-11b-12i.md](step-11b-12i.md)): the re-pointing is tested on the
real tables.

- The launch tables refer to `rv_game_identities (id)` through `game` columns (`rv_play_sessions`,
  `rv_launch_game_choices` with the GameId as its primary key, `rv_launch_verifications`); `rv_launch_verifications.session`
  refers to `rv_play_sessions (id)`, which is never deleted. The re-pointing procedure of the brief (UPDATE OR IGNORE then
  delete what is left) is tested on them in `test_session_store.cpp`.
- `SessionStore::resolveGame()` and the store's alias resolution read `rv_identity_aliases (alias, game)`.

## For the merge

- **CLAUDE.md**, the retroverse-cli section: `launch --game` (`--wait`, `--wait-limit`, `--test`, Ctrl+C), `launch
  --dry-run --game`, `assign`, `sessions` with their exit codes; automated runs that launch set `RV_LAUNCH_ALLOWLIST` (the
  CLI's launches go through the runner); "Catalog database": the launch area (`rv_play_sessions`,
  `rv_launch_game_choices`, `rv_launch_system_choices`, `rv_launch_verifications`), `SessionStore` and `SessionRecorder`
  (writes through the writer, `BusyRetry`, `isBusyError()`), sessions closed as interrupted only when their owner is gone;
  assign's choice ranks before emulators.json's; tests: `tests/support/scoped_environment.hpp`; `rv_tests` depends on
  `retroverse-cli`.
- **docs/architecture.md**: `rv_catalog_db` gains the session store; `rv_platform_win` the process identity
  (`processLiveness()`); `tools/cli`'s launch, assign and sessions (no stubs left but `identities`, step 11b).
- **docs/i2/step-4.md**'s table: the launch area's tables.
- **docs/roadmap.md**: step 12 done, with the RetroArch check.
- **ADR-0008**, suggested amendments: §6 "a choice for a system made with `retroverse-cli assign` ranks before
  emulators.json's for the same system (the user's latest explicit choice)"; §9 "an attempt blocked before the launch,
  a refusal by the automation guard and a game left running (its owner stopped following it: not waited for, a wait
  limit, quit, the user's return) are recorded with their own outcomes; a session write that meets a busy database is
  tried again, never lost silently"; §8 "a launch verification is recorded by every launch that returns normally (a
  clean exit that is no quick exit, nothing left running) and by `retroverse-cli launch --test`".
- **BACKLOG.md** candidates: the CLI's Ctrl+C is not tested (it would need a console shared with the test); `profiles`
  could show "verified" per command and profile (the verifications are there); `launch --dry-run --path` cannot show a
  per-game choice (the identity is not looked up by path); a pending session write is lost when the process ends before
  the database is free (2 minutes of patience at the end).
