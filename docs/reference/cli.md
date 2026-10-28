# retroverse-cli

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

`retroverse-cli [--data-dir <dir> | --portable] [--asset-root <dir>] [--log-level <level>] <command> [options]` is the
ES-DE catalog and, since I3, the rooms' plans from the command line (`tools/cli`, one `src/cmd_<name>.cpp` per command,
`cmd_worldplan_<name>.cpp` per worldplan subcommand; `--help`, `<command> --help`).
Results go to stdout, or atomically to `--output <file>`, as text or with `--json` as one object; logs go to stderr and
`<user data>/logs/retroverse-cli.log`. Pass `--data-dir build/rvdata`. Every command of the table is implemented.

- `discover [--home <dir>] [--install <dir>] [--json]` finds ES-DE as ES-DE finds itself (`--home` is ES-DE's own
  option: the ES-DE home is `<dir>\ES-DE`; `--install` defaults to the uninstall entry) and prints the ES-DE home, `~`,
  the install and version, the settings it uses, the roots, find rules, systems, gamelists and issues, each with its
  source. It reads ES-DE's files and writes nothing but its log; exit 1 on an error issue.
- `library [show]` prints `<user data>/config/library.json` and the roots it gives; `library create [--id <slug>]
  [--home <dir>] [--install <dir>] [--rom-root <dir>] [--media-root <dir>] [--force]` discovers ES-DE and writes it
  atomically, never over an existing file without `--force`; the roots must resolve, from the settings or the options.
  The folder options take a bare drive (`E:`) as its root, as ES-DE does. The catalog's `rv_libraries` is the authority
  on the library's id: `import`, `db rebuild-index` and `identities` (not `identities show`) refuse a library.json that
  names another library (exit 2), as `launch`, `media` and `assign` refuse a `--game` of another library; `assign
  <system>` cannot store a choice for a library the catalog does not hold (exit 3). `library create --id <the catalog's
  library> --force` (with the folder options it was made with) names the catalog's library again, which changes no
  state and no GameId.
- `db migrate | check | info [--json]` on `<user data>/database/retroverse.sqlite`: `migrate` creates it or brings it to
  the program's schema (backed up first when it holds anything, the newest 3 backups kept in `database/backups`);
  `check` (integrity, foreign keys, FTS5, the schema against the program's; exit 1 on a problem) and `info` (versions,
  sizes, `rv_meta`, row counts, backups) write nothing into it. Schema v1 is closed since M1: a later change is a
  migration to a new version. `db rebuild-index [--json]` is an
  import that clears the index tables and writes every row again, every cue sheet and playlist read again; the state
  rows stay (identities, aliases, sessions, choices); it equals a refresh and repairs a damaged index; exit codes as
  `import`'s.
- `import --dry-run [--system <name>] [--json]` prints the library snapshot an import would read
  (`esde::EsdeLibrarySource`, `docs/i2/step-7.md`: the ROM folders listed with ES-DE's rules, the gamelists merged in,
  disc tracks grouped under their sheet, every sheet read): per system its entries by state, disc groups, issues, its
  folder's availability and its gamelist's state, then the totals and "games as ES-DE lists them"; exit 1 on an error
  issue.
- `import [--system <name>] [--accept-removals] [--json]` imports the library library.json records into the catalog
  database (made and migrated when needed; `docs/i2/step-11a.md`, `step-11b.md`): identities, index rows, disc groups,
  media presence, files moved or renamed paired with their identities and what no rule pairs safely as review rows. It
  prints one line per system with rows, changes, issues or an outcome other than applied or source-unavailable (added,
  changed, removed, unchanged, unavailable, kept; a missing system folder that holds no rows is only in the JSON), the
  totals, the identities' changes, each move and review row, the issues and the times; an import that finds nothing
  changed writes no row and no identity, only its record in `rv_imports`. A system with more than half of its present
  files gone is kept as it was (`kept-by-guard`) until `--accept-removals`; a folder that cannot be listed keeps its
  rows as source unavailable; `--system` imports one system. Exit 0; 1 an error issue (a folder that cannot be listed, a
  failed gamelist) or a system kept by the guard; 2 invalid options, no library.json, an unknown system, a library.json
  naming another library; 3 the library or the database cannot be read or written, another import runs (in this or
  another process).
- `media --system <name> --path <relative path>` names per kind the media file ES-DE shows for a ROM path (taken as
  the disk spells it), or `none`; `media --coverage [--system <name>]` gives the share of each kind over four
  populations (every `<game>` entry, those whose file exists, the scraped games, every game) of the library snapshot
  that `import --dry-run` reads (`tools/cli/src/library_snapshot.hpp`); exit 1 when the media root, a media or ROM
  folder or a gamelist could not be read. `media --game <id>` gives a game's media as the last import stored them, each
  file's path derived under the media root of library.json's library; nothing on disk is looked at (exit 1 an unknown
  game, 2 a game of another library, 3 no catalog).
- `systems`, `games [--system <name>] [--state <list>] [--availability <list>] [--include-unavailable] [--sort
  title|system|release-date] [--offset <n>] [--limit <n>]`, `search <text>` (the options of `games`; text that starts
  with `-` after `--`) and `show <gameId>` read the catalog database through a reader (`SqliteCatalogQuery`) and never
  create, migrate or write it: exit 3 without a database or with another schema version, 2 for an unknown `--system`,
  1 for `show` of an unknown game. `show` prints the media paths (as `media --game`), no readiness yet.
- `identities` (`docs/i2/step-11b.md`): `list --system <name>` (GameId, state `present`, `missing` or `unseen`, quick
  hash `current`, `stale`, `none` or `folder`, last path), `show <gameId>` (file facts, quick hash, aliases, history,
  review rows; an alias prints what it stands for; needs no library.json), `reviews [--system <name>] [--all]` (open
  rows with the GameId each candidate has now), `resolve <review> --with <gameId> [--force]` (the candidate becomes an
  alias of the vanished identity, which takes its path, its state rows following; `--force` for an identity that is none
  of the candidates: the merge cannot be undone) or `resolve <review> --keep-separate`, and `hash [--system <name>]
  [--limit <files>]` (the quick-hash pass: resumable, committed in batches of 1,000 files or 10 s, Ctrl+C ends it and
  the batches already committed stay; `--limit` bounds a run; a progress line every 2 s on stderr). `list`, `show` and
  `reviews` write nothing; `resolve` and `hash` take the import lock. Exit 1 an unknown identity or review row, a row
  resolved already or whose file is back, files `hash` could not read; 2 usage errors, another library, a system without
  identities, an identity that cannot be paired with; 3 no catalog, another import runs.
- `profiles [--system <name>]` lists the emulator profiles, `assign`'s and emulators.json's system choices and, for
  every configured system, the command or profile its games launch with, where that choice comes from and why, its
  readiness, the program and core with the find rule that found them, the attempts behind "not found", and each command
  and profile with its own readiness (exit 1 with `--system` when that system is not ready). `launch --dry-run --system
  <name> --path <relative path>` and `launch --dry-run --game <id>` show the request a game would start (argv, working
  folder, environment, command line, the automation guard's verdict, the database's choices read only) and start nothing
  (exit 0 ready, 1 not). Both look at this machine (`Win32SystemProbe`).
- `launch --game <id> [--wait [--wait-limit <seconds>]] [--test] [--json]` (`docs/i2/step-12.md`) resolves a game of the
  catalog right before the launch (its file looked for on disk now, the database's choices) and starts it through
  `LaunchSession` on `Win32ProcessRunner`, so the automation guard applies: automated runs set `RV_LAUNCH_ALLOWLIST`.
  Every attempt is a play session (`rv_play_sessions`): a game not ready is recorded `blocked` and not started; without
  `--wait` the launch is followed through its startup grace (2 s) and half a second more, then a game still running is
  recorded `left-running` (`not-waited`) and the command returns 0 with the game running on; `--wait` follows it to its
  end as its wait strategy says (Ctrl+C stops waiting, `--wait-limit` 1 to 86400 s; nothing is ever terminated);
  `--test` waits and records a launch verification when the game returned normally, else exits 1 (every normal return
  records one too). Exit 0 returned or left running; 1 blocked, failed to start, crashed, a quick exit, a failed
  `--test`, an unknown game; 2 invalid options, a game of another library or that is not a game; 3 ES-DE's
  configuration or the catalog cannot be read (an import makes it), a session that could not be recorded (printed).
- `assign <system> [<label|profile>] [--command <label> | --profile <id> | --clear] [--json]` and `assign --game <id>
  ...` store Retroverse's choice of an ES-DE command label or a profile serving the system in the catalog database,
  never in the user's files; without a value they show the stored choice. The choice of command, most specific first:
  the game's `assign`, its `<altemulator>`, the system's `assign` before emulators.json's `"systemChoices"` (`--clear`
  gives that back), the gamelist's `<alternativeEmulator>`, the first command; a stored choice that disappears blocks
  the launch, never skipped. Choices survive restarts and `db rebuild-index`. Exit 1 an unknown game; 2 invalid options,
  an unknown system, a value the system does not have; 3 the catalog cannot be read or written.
- `sessions [--game <id>] [--limit <n>] [--json]` lists the attempts, newest first (50; 0 for all), after closing as
  interrupted the open sessions whose owner process is gone (`launch --game` does it at its start too; a session another
  live process follows stays open). `exitKind` is null when the session has no exit code (an attempt that never
  started, a start that failed, an open or interrupted session). Exit 1 a `--game` the catalog does not know; 2 invalid
  options; 3 the catalog cannot be read. A GameId that became an alias stands for its game in `launch`, `assign` and
  `sessions`: the JSON of `launch` and `sessions` gives the identity as `game` and the alias as `requestedGame`,
  `assign`'s as `game.id` and `game.requested`; the text says "(for <alias>, an alias)".
- `worldplan` (I3, ADR-0010, `docs/i3/step-3.md`, `step-4.md`, `step-9.md`, `step-10a.md`; the building since I4 step
  1, ADR-0013, `docs/i4/step-1.md`): the building's plan, the canonical JSON `<user data>/worlds/<library id>/
  world-plan.json` in schema 2 (its floors and rooms; beside it `world-plan.meta.json`, `overrides.json`'s pins, the
  last 10 plans in `backups/`, the paired snapshots of the explicit actions in `backups/snapshots/` and the world lock
  `world.lock`, waited for 2 s; since I4b step 1, `docs/i4b/step-1.md`, `overrides.json` is schema 2, schema 1 read
  lifted, and a lock that finds the journal `transaction.json` of an interrupted action rolls it back first). A plan
  of schema 1 (I3's one room) is read lifted into the building (`world::lift`: the room on its system's floor at the
  north row's first position, the ground floor) by every subcommand and `where-is`; the first update writes it in
  schema 2.
  `worldplan generate [--system <system>]... [--selection <file>]... [--seed <n>] [--library <id>] [--replace | --plan
  <file>] [--json]` generates a fresh building from the catalog's selections (or canonical selection files, one per
  system, which name the building's systems), validates it and saves it under the world lock, never over an existing
  plan unless `--replace` (the explicit regeneration of the whole building, which chooses the face-out titles again,
  after a backup); `--seed` defaults to `world.json`'s, else the library's seed. Without `--system` the building holds
  every system of the catalog that gets a room (`assets/world/systems.json`: not excluded) and has a placeable title,
  every other system left out with a note on stderr ("<system> left out of the building: <why>": no room (excluded), no
  entries in the catalog, no placeable title, or no data yet, the asset root not giving its data) and in the summary
  ("Left out:", JSON `leftOut` {`system`, `reason`}); `--system` (repeatable) restricts it, only with `--plan` or when
  no plan is stored (else exit 2). Selection files given with `--system` name the same systems, and with `--replace`
  over a stored plan they must hold every system of the stored building (else exit 2: `--replace` never drops a room).
  Since I4b step 1 `--replace` over a stored plan is an explicit action: the plan and the overrides (rewritten in
  schema 2) are saved together behind a snapshot ("Snapshot: <name> ('worldplan undo' restores it)", JSON `snapshot`,
  else null), which `worldplan undo` restores; when only the overrides change, "Backup:" says the plan is unchanged.
  Since I4b step 5 (`docs/i4b/step-5.md`) `--replace` over a stored plan this program reads is `worldplan regenerate
  --world` (below): the overrides honoured, those it cannot honour kept in `overrides.json` and named on stderr
  ("not honoured, kept in overrides.json: <pin>: <reason>: <why>"); a plan it cannot read is replaced by a fresh one.
  A fresh generation (no stored plan) still applies no pin: one it does not honour breaks `pin-broken` and exits 3,
  nothing written, with a hint on stderr (`worldplan edit unpin --game <id>` or `unpin-fixture --fixture <id>`).
  Since I4 step 4 (`docs/i4/step-4.md`) every arcade system takes a section of the arcade room `area:arcade` on the
  ground floor, a system with fewer distinct titles over all its games (with a cover or not) than `world.json`'s
  `sharedRoomMinTitles` (default 30) a section of its floor's shared room `area:shared:<floor>`, every other system its
  own console room; the room and section are recorded and sticky.
  `worldplan update [--system <system>]... [--selection <file>]... [--previous <plan>] [--seed <n>] [--library <id>]
  [--plan <file>] [--diff] [--json]` applies ADR-0010 §10's rules to every room of the plan and enters every other
  system of the catalog that gets a room and has a placeable title (each of the others left out with a note on stderr,
  "retroverse-cli worldplan update: <system> left out of the building: <why>", JSON `leftOut` {`system`, `reason`}: no
  room (excluded), no entries in the catalog, no placeable title, or no data yet, the asset root not giving its data),
  or works on the systems named or of the selection files only (the others stay as they are; `--system` given with files
  names the same systems, else exit 2; a system of a shared or arcade room updates the whole room with every section's
  selection, and selection files must give every section, else exit 2; an update of some rooms that changes nothing
  keeps the plan's `inputDigest`, so it writes nothing): every placement kept, new titles spine-out next to their
  alphabetical predecessor (in a shared or arcade room, within their section: its collection is ordered by section, sort
  key, title key and GameId), a system grown past the threshold staying in its section, then a dressed level, a new
  module in place of decoration, growth (the room moves away from its hall, its x kept), `capacity`; the face-out set
  unchanged. A room of an older template version, of prefabs whose levels changed or of another generator major is kept
  as it is, its unplaced entries too, the games of its selection it neither places nor lists unplaced as `room-kept`
  ("Room kept:", JSON `changes.keptRooms`); a shared room some of whose sections have no selection is held ("Room
  held:", JSON `changes.heldRooms`). Since I4 step 6a (`docs/i4/step-6a.md`) a system that no room holds enters the
  building as in a fresh one, nothing that stands moving: its own room at the east end of the shorter row of its floor
  (a floor that held no room appears), or a section appended to its floor's shared room or to the arcade room (opened
  when absent); on a full floor it joins the shared room, else its games are unplaced for capacity, naming the room it
  would have had (when it enters again, those naming its console room give way to its entry; those naming a shared or
  arcade room that found no place, only for its selection's games). A new system taking a section of a standing shared
  or arcade room (an arcade system, one under the threshold, or a console system its full floor sends to the shared
  room) needs that room's sections' selections: named from the catalog, it has them read too (the update runs again with
  them), so it never waits; from selection files without them it waits ("Waiting:", JSON `changes.systemsWaiting`), the
  room held. The report says "Entered: <system> in <room> (a new room | a new section)" (JSON `changes.systemsEntered`,
  `changes.roomsAdded`). Since I6 step 4 (`docs/i6/step-4.md`, ADR-0014 §2) a room of a family that has a successor
  in rooms of its kind (I4's jewel cases, keep cases, handheld cases, big boxes, and the generic cases of console and
  shared rooms, standing on their end; the arcade room keeps its generic case until I6 step 6) is migrated once,
  given every section's selection and named by no pin: regenerated with the successor (standing on its side), its id,
  floor, x, sections and seed kept, its face-out titles chosen again ("Room migrated: <room>: <from> -> <to>", with ",
  <n> unplaced for capacity" when its new room holds fewer; JSON `changes.migratedRooms` {`room`, `from`, `to`,
  `unplacedForCapacity`}, rebuild `full`); a pinned room's refusal is among the confirmations. Since I6 step 5
  (`docs/i6/step-5.md`, ADR-0014 §3) a ground floor room standing at I4's lobby is re-placed once onto the hub's wall,
  its contents unchanged ("Room re-placed: <room>: onto the hub, x <x> z <z> (was x <x> z <z>)"; JSON
  `changes.replacedRooms` {`room`, `from`, `to`: {`xMm`, `zMm`, `yawHalfTurns`}}, rebuild `full`). Since I4b step 2 a
  fixture the user moved stays where it stands when its room grows, pinned or not, but for a poster hung on the south
  wall, which follows its wall: when it is pinned, its pin's transform follows it and `update` saves the plan and
  `overrides.json` together behind a snapshot (action `update`), which `worldplan undo` restores (with `--plan` only
  the plan is written, with a warning). It prints the change report. Both exit 0 written or up to date; 1 nothing
  written (a plan exists without `--replace`; no plan to update, one edited by hand or newer, one that needs `generate
  --replace`: another seed, preset or generator major version; the media unknown; no such system in the catalog); 2
  usage; 3 the data, catalog, selections or overrides unreadable (a system named that gets no room, an excluded one,
  included), a hard rule broken, the world locked by another program, a failed write.
  `worldplan show [<plan>] [--library <id>] [--placements] [--floor <id>] [--scene <file> [--room <id>]] [--json]`
  summarizes a plan: its floors (order, rooms), each room with its kind, floor, sections, row and place on the floor
  and generator, and the theme pack it is drawn with (resolved on its floor, I6 step 3: "<pack> (recorded <old> until
  the next update)" when the plan records another; JSON `floors`, `rooms[].kind`, `floor`, `sections`, `placement`, `generatorVersion`,
  `fileSchemaVersion`, `lifted`); exit 1 no plan, 2 usage (`--scene` on a plan of several rooms without `--room` or
  `--floor`, a `--room` or `--floor` the plan lacks, `--room` with `--floor`), 3 a plan refused or a scene not
  written; `--scene` also writes the static part of one room (shell, fixtures, dressing, lights; no collection items)
  as a scene file that `retroverse.exe --scene <file>` renders: the plan's only room, or the one `--room` names
  ("Scene <file> of <room>: ...", JSON `sceneRoom`), its sign and plastics classified with the catalog's platform tags
  of its system when the catalog holds the plan's library (a custom system's room as `generate` classified it;
  without a catalog, systems.json's fallback). `--floor <id>` (I4 step 3, `docs/i4/step-3.md`) also summarizes that
  floor: its hall derived from its rooms (length, width, height, its lights every 6 m, the elevator at its west end)
  and each room with its row and its doorway onto the hall ("Floor <id>: hall ...", JSON `floorScene` {`floor`,
  `lobby`, `hall`, `rooms`}: `lobby` true for the ground floor's hall, the hub since I6 step 5); with `--scene` it
  writes the whole floor instead of a room ("Scene <file> of floor
  <id>: ...", JSON `sceneFloor`): every room moved onto the floor by its placement (its ids prefixed with the room's,
  its doorway's stop left out), the hall's walls, floor, ceiling, lights and lamps, the elevator stand-in with the
  floor's sign (building.json), the lobby's entrance doors on the ground floor, the hall's dressing (I6 step 2,
  `docs/i6/step-2.md`: the floor's theme pack's `hall` materials (I6 step 3), trim on every wall, a portal with the
  room's flat and blade signs at each doorway, beams every 3 m and over the doorways, sconces washing their wall and prints in turn, benches
  with a plant beside them, the runner), the areas whose lights the app packs
  (each room's interior with its doorway's passage, the hall), a camera in front of the elevator facing east.
  `worldplan check <plan.json> [--selection <file>] [--json]` validates a plan against its templates, the prefabs' slots,
  the world's configuration and the building's data (37 rules, `rv/world/validation.hpp`: the ground floor, the rooms'
  floors, no two rooms of a floor overlapping, every system in one room; since I4 step 3 each floor's derived hall: no
  room overlapping it, every doorway in its room's doorway wall on the hall's edge, every room walked to from the
  elevator on the floor's navigation grid; a room of an older template version or
  another generator major judged as kept; the plan's lights counted, no pins judged: exit 1 a hard rule broken or not
  a plan this program reads, soft findings and the LayoutScore reported; 2 usage; 3 unreadable). Every subcommand
  takes `--output <file>`.
  `worldplan snapshots [--library <id>] [--json]` (I4b step 1, ADR-0015 §6) lists the world's snapshots newest first:
  each folder's name, its action and scope, when and by whom it was taken, the plan's and the overrides' digests
  (shortened; "none" for a file it did not hold), "undone" or, for an undo's own snapshot, the snapshot it undid; then
  the folders whose `snapshot.json` cannot be read, "damaged: <why>"; then "Next undo:" ("refused while a snapshot is
  damaged" when one is) and, when a journal waits for the next lock, "Interrupted:" with the snapshot it names, or,
  when the lock cannot roll it back, why and the repair by hand (JSON `snapshots` with `name`, `sequence`, `kind`
  `action` | `undo`, `action`, `scope`, `writer`, `takenAt`, `planDigest`, `metaDigest`, `overridesDigest` (null when
  absent), `undone`, `undoes`; `damaged` {`name`, `error`}; `nextUndo` (null while a folder is damaged), `kept` 20,
  `interruptedAction`, `interruptedSnapshot`, `rollbackBlocked` (the repair, or null)). It reads only: exit 0 (none
  included), 2 usage.
  `worldplan undo [--library <id>] [--dry-run] [--json]` restores the newest snapshot of an action not undone yet,
  byte for byte (the plan, its meta sidecar and `overrides.json`; a file the snapshot did not hold removed), under the
  world lock, behind a snapshot of kind `undo` of the state it replaced, which undo never offers (no redo): "Undid
  <action> of <scope>: the snapshot <name> taken <time> by <writer>", the digests, "Kept:", "Plan backup:" (the plan
  it replaced, kept in `backups/` as every plan write keeps it) (JSON `dryRun`, `restored`, `kept`, `planBackup`,
  `snapshotsRemoved`, `rolledBack`). `--dry-run` names it and restores nothing; it still takes the world lock, which
  may create `world.lock` and roll back an interrupted action first, as for every writer. Exit 0 undone; 1 nothing
  to undo (no world, no snapshot, or only undone ones and undos: "the last change was an undo, which is never undone
  (no redo)"); 2 usage; 3 the lock held by another program or a journal it cannot roll back (the message names
  `transaction.json`, the snapshot and the repair by hand), a damaged snapshot (its files missing or not those its
  digests name) or any damaged folder (nothing written), a failed read or write.
  `worldplan edit <operation> [its options] | --script <file.json> [--library <id>] [--dry-run] [--json]` (I4b step 2,
  ADR-0015 §4, §8, `docs/i4b/step-2.md`) applies the editor's operations (`rv/world/world_edit.hpp`) to the world's
  plan and overrides, read under the world lock: `move --game <id> --slot <slot>` (a placed game into an empty slot of
  its room chain, a face-out one too: a dressed span gives way, a face-out slot left is dressed), `swap --game <id>
  --with <id>`, `pin | unpin --game <id>`, `move-fixture --fixture <id> --x-mm <n> --z-mm <n> [--yaw-cdeg <n>]` (a
  side-wall module, display fixture or decoration: its pivot on the room's 100 mm grid through its place in its socket,
  quarter turns, decoration 15 degrees, a hung decoration along the walls only; the hero corner and the hero-wall
  modules are fixed), `pin-fixture | unpin-fixture --fixture <id>`; `--script` a session of several, all or nothing
  (`{"schemaVersion": 1, "operations": [{"op": "move", "game": ..., "slot": ...}, {"op": "move-fixture", "fixture":
  ..., "xMm": ..., "zMm": ..., "yawCdeg": ...}, ...]}`, JSON with comments). Every manual change is pinned (origin
  `cli`); the session is validated by the hard rules on the floors it edits and saved through `commitWorld()` behind a
  snapshot (action `edit`, scope the room), which `worldplan undo` restores; the catalog is not read. `--dry-run`
  applies and validates the operations and writes nothing; it still takes the world lock, which may create
  `world.lock` and roll back an interrupted action first, as for every writer. A move back home removes the fixture's
  pin; an operation is refused only for a hard finding it causes (one its floor did not have before). It prints each
  operation ("Done:" or "Unchanged:"), the pins, the plan's digests and "Snapshot:" (JSON `operations` {`op`,
  `operation`, `changed`, `summary`, `refused`}, `changed`, `written`, `scope`, `snapshot`, `rebuild`, `digestBefore`,
  `digestAfter`, `fixturesMoved`, `gamePins`, `fixturePins`, `refused`). Since step 2 the pins a plan honours join its
  inputDigest (`generate`, `update` and `edit` alike), so an update of an unchanged catalog after an edit writes
  nothing; a world without overrides keeps its digests. Exit 0 edited, nothing to change, or a dry run that passes;
  1 an operation refused ("operation <n> (<operation>) refused by <rule>: <subject>: <why>; nothing written", the
  rule an edit rule such as `slot-taken`, `other-room`, `off-grid`, `turn-step`, `wall-mount`, `fixture-fixed`,
  `fixture-in-kept-room`, or a validation rule such as `footprints-overlap`, `doorway-clearance`, `door-reserved`,
  `decoration-off-wall`, `unreachable`), no plan, a plan
  refused; 2 usage (an unknown operation, an option it does not take or lacks, a malformed id or number, a script that
  cannot be read); 3 the data or overrides unreadable, the lock held by another program, a failed write.
  `worldplan regenerate (--room <id> | --floor <id> | --world) [--drop-overrides] [--dry-run] [--selection <file>]...
  [--seed <n>] [--library <id>] [--diff] [--json]` (I4b step 5, ADR-0015 §3, §5, §8, `docs/i4b/step-5.md`) regenerates a
  room (its chain), a floor or the whole building of the world's plan through `world::regenerate()`
  (`rv/world/regeneration.hpp`): a room afresh where it stands (the template's current version, the family's last
  successor, the face-out titles and representatives chosen again; its id, kind, floor, x, yaw, sections and seed kept);
  a floor's systems decided again (own or shared rooms by today's counts, the rooms placed again from the rows' start, a
  system on the floor staying there); the world as `generate` makes it. The scope's overrides are honoured (pinned
  fixtures first, the sockets of pinned slots taking their module, pinned games reserved, a pinned game its title's
  representative); a pin it cannot honour is a conflict (`room-gone`, `slot-gone`, `game-gone`, `invalid`), kept in
  `overrides.json` with its reason recorded on the pin (`"conflict"`; an update then reports it as orphaned, not as
  needing a confirmation) and reported, unless `--drop-overrides` (the consent) drops the scope's overrides. The
  selections are the catalog's as `update` reads them (every system of the catalog that gets a room; the scope's systems
  required: exit 1 without one) or selection files (`--world`'s must hold every system of the stored building, else exit
  2). It prints the impact: "Rooms: <n> regenerated, added, removed, moved", "Games: <n> change slot, enter face-out,
  leave it, newly placed, newly unplaced", "Fixtures:", "Pins: <n> kept, conflicting, dropped", a "Conflict:" line each
  ("<pin>: <reason>: <why>") and "Dropped:" lines, the validation, the digests, "Snapshot:" and "Time:" (the selections,
  the data and the regeneration); `--diff` lists every item (JSON `scope`, `library`, `plan`, `dryRun`, `dropOverrides`,
  `changed`, `written`, `snapshot`, `impact` {`counts`, `roomsRegenerated`, `roomsAdded`, `roomsRemoved`, `roomsMoved`,
  `gamesMoved` {`game`, `from`, `to`}, `faceOutEntering`, `faceOutLeaving`, `newlyPlaced`, `newlyUnplaced`,
  `fixturesAdded`, `fixturesRemoved`, `fixturesMoved`, `pinsKept`, `conflicts` {`pin`, `game`, `fixture`, `reason`,
  `message`}, `overridesDropped`}, `validation`, `digestBefore`, `digestAfter`, `gamePins`, `fixturePins`, `timings`
  {`selectionsMs`, `dataMs`, `regenerationMs`, `totalMs`}, `leftOut`). Without `--dry-run` the plan and the overrides
  are read under the world lock and saved together through `commitWorld()` behind a snapshot (action `regenerate`, scope
  the room's id, `floor:<id>` or `world`), which `worldplan undo` restores; nothing changed, nothing is written.
  `--dry-run` is the preview: it reads the plan and the overrides without the lock (`world::readWorldUnlocked()`: with
  full sharing, so a writer's commit never fails because of it, and checked again once both are read) and writes
  nothing, not even `world.lock`; a journal there exits 3 ("an action is in progress or was interrupted ... Nothing
  previewed"), and so does a world that changed during two readings (busy). A room or floor of a plan of another seed,
  preset or generator major needs `--world` (exit 1). Exit 0 regenerated, nothing to change, or a dry run; 1 nothing
  written: no plan, no such room or floor, a scope's system without a selection, a plan needing `--world`, unknown
  media; 2 usage; 3 the data, catalog, selections, plan or overrides unreadable, a hard rule broken (nothing written), a
  dry run finding a journal or a busy world, the lock held, a failed write.
- `where-is <gameId> [--plan <file> | --library <id>] [--json]` tells where a game stands in the building's plan
  (floor, room, fixture, slot, face-out or spine-out; JSON `floor`) or why it is not placed, with the version that
  stands for it; exit 1 no plan or a game the plan does not hold, 2 usage, 3 a plan refused.
- `import`, `db rebuild-index`, `media`, `profiles`, `launch`, `assign` and `identities` (but `identities show`) read
  the library `library.json` records (exit 2 without one: `library create` first); so do `worldplan` and `where-is`
  for the world's own plan unless `--library` names it (always with the catalog's selection).
- Nothing is written, the log file included, until the user data directory has passed the guard against the library's
  folders (the ES-DE home, the install, the ROM and media roots, the ROM folder of a system outside the ROM root):
  `discover` and `library` check the folders they find, every other command those of `library.json`.

