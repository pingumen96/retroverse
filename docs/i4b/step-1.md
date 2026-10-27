# I4b step 1: overrides written and undone

Roadmap rows: I4b steps 0 (the fixture) and 1. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §2 and §6
(amending ADR-0010 §9). Plan: [plan.md](plan.md).

## What was built

- **The fixture `esde/overflow`** (step 0's remainder, `tests/fixtures/esde/overflow/README.md`): `amiga` (big boxes,
  1,700 titles) and `mame` (60 sets), generated at test time with small portrait covers (about 6 s in Debug). Today's
  generator places 1,561 big boxes and 53 arcade sets; 139 and 7 are unplaced for capacity, what step 7's expansion
  rooms will place.
- **`overrides.json` schema 2** (`rv/world/overrides.hpp`): each pin has a `provenance` (`origin` editor, cli or file,
  and a UTC `time` when known) and a fixture pin an optional `transform` (`xMm`, `zMm`, `yawCdeg` in the room's frame).
  `writeOverrides()` writes the canonical text (formatted, keys and pins sorted), reads it back before returning it,
  and refuses what the reader would refuse. Schema 1 is read lifted (origin file); a schema-1 file holding schema 2's
  keys and a newer schema are refused. `checkPins()` and the validation's `pin-broken` also judge a transform: a
  fixture pinned at a transform is honoured only standing there.
- **`saveOverrides()`** (`world_store.hpp`) under the world lock: atomic, nothing written when the text is there
  already, or when there is no file and no pin (a world without overrides keeps no file).
- **Snapshots and transactions** (`rv/world/world_snapshots.hpp`, new): `commitWorld()` saves a plan and its overrides
  as one explicit action: a snapshot written whole in `backups/snapshots/<time>-<action>.partial/` (the plan, its meta
  sidecar and `overrides.json` as they were, and `snapshot.json`: sequence, kind, action, scope, writer, time, the
  three digests, undone, undoes), then the journal `transaction.json` naming it, then the folder renamed
  `<time>-<action>/`, then the overrides, then the plan (`savePlan()`), then the journal removed and the snapshots
  rotated (below). A failed write restores the snapshot at once. A snapshot is retired (rotation, rollback) by renaming
  it `<name>.removing` first, then removing it. `WorldLock::acquire()` removes the temporary files inside snapshots,
  rolls back an action whose journal it finds (a named snapshot restored byte for byte and retired, then the journal
  removed; a journal naming a `.partial` or `.removing` folder finds the world's files the snapshot's already;
  `WorldLock::rolledBack()` names it), then removes every `.partial` and `.removing` folder. A journal it cannot
  honour fails the lock, the error naming `transaction.json`, the snapshot and the repair by hand (nothing is written
  until repaired); `checkInterruptedAction()` makes the same check without writing.
- **Damaged snapshots**: `scanSnapshots()` lists the folders whose `snapshot.json` cannot be read as damaged beside the
  snapshots; a snapshot missing a file it kept, or holding another, is damaged too (`ValidationFailed`).
- **Undo** (`undoLastAction()`): the newest snapshot of kind `action` not undone, its files checked against their
  digests, restored byte for byte as a transaction of its own behind a snapshot of kind `undo` (naming what it undid,
  never offered); the plan it replaces is first kept in `backups/` as `savePlan()` keeps it; the restored snapshot is
  marked undone. A rollback of an interrupted undo clears that mark. Undo is refused while a damaged folder is there.
  A dry run only names the snapshot.
- **The command line**: `worldplan snapshots [--json]` (reads only; lists the damaged folders; reports a pending
  journal and whether the next lock can roll it back, else the repair by hand) and `worldplan undo [--dry-run]
  [--json]` (exit 1 nothing to undo; 3 a damaged snapshot or folder, a journal that cannot be rolled back). The dry
  run restores nothing, but it takes the world lock like every writer: the lock may create `world.lock` and roll back
  an interrupted action first. `worldplan generate --replace` over a stored plan, the explicit regeneration, now
  saves through `commitWorld()` (action `regenerate`, scope `world`; "Snapshot:" in its summary, JSON `snapshot`), so
  it can be undone; it reads the overrides after taking the lock. When only the overrides change its "Backup:" line
  says so.

## Decisions beyond ADR-0015

- **The meta sidecar travels with the plan** in a snapshot and is restored with it, so that after an undo
  `world-plan.meta.json` describes the plan beside it (it stays informative).
- **Snapshots are ordered by `sequence`** (one more than the highest kept), never by their folder's time, which a clock
  set back would disorder; names only need to be unique (`-2`, `-3` within one millisecond).
- **No stop leaves a snapshot that changes nothing**: the journal is written while the snapshot is still `.partial`
  and the folder named only then, so a stop before the journal leaves a `.partial` folder (removed by the next lock),
  and a stop after it a journal the next lock ends. A rolled-back action leaves no snapshot: its snapshot, equal to the
  state restored, is renamed `.removing` before the journal goes, so a stop in between leaves a journal naming a
  `.removing` folder (the world restored already: the lock only ends the journal). Rotation retires the same way, so
  a removal that fails half-way (a file held open) never leaves a half snapshot listed.
- **The rotation's rule**: after each action at most 20 snapshots stay. Beyond 20 it removes first the snapshots undo
  never offers again (kind `undo`, or undone), lowest `sequence` first, and only then the oldest snapshots of kind
  `action` not undone. So the newest 20 actions not undone are always kept and 20 actions in a row can be undone (the
  undo snapshots then fill the 20).
- **Damaged folders** (a `snapshot.json` that cannot be read) are listed, never skipped: undo is refused while one is
  there (it might walk past the action that folder would undo), and the rotation removes one only once 20 folders are
  newer by name.
- **Undo and the lock's rollback write the plan with `core::writeFileAtomic`, not `savePlan()`**: they restore the
  snapshot's bytes, a plan `savePlan()` wrote before, which a new write would not reproduce (its sidecar, its text).
  Undo first keeps the plan it replaces in `backups/` (`core::backUpFile`, the last 10), so every plan write keeps its
  backup (ADR-0015 §6); the rollback does not (it undoes a write its journal never confirmed, whose `savePlan()` had
  kept the plan before it).
  An undo can bring back face-out titles other than the current plan's: those of the plan before the action undone.
- **Provenance is read leniently**: a schema-2 pin without `provenance` is read (origin `file`, time unknown: a hand
  edit), and a schema-1 pin lifted to schema 2 gets `origin: file` without a `time`; `writeOverrides()` then writes
  them so. Provenance never decides anything (ADR-0015 §2).
- **No commit when nothing changes**: a plan and overrides identical to the folder's take no snapshot.
- **An explicit action rewrites a schema-1 `overrides.json` in schema 2** (it is saved with the plan); undo brings the
  hand-written bytes back.
- **Updates stay outside**: they save the plan alone (its `backups/`), take no snapshot and are not undoable.

## Proofs

- Unit (`tests/unit/world/test_overrides_where_is.cpp`): schema 2's canonical text exactly, its round trips (overrides
  and text), the schema-1 lift, schema 2 written by hand without provenance, refusals (schema 2's keys in schema 1, an
  unknown origin, a bad time, a yaw of 36000, a missing coordinate, schema 0 and 3), writes refused; a transform pin
  honoured where the fixture stands, not honoured and `pin-broken` elsewhere.
- Unit (`tests/unit/world/test_world_snapshots.cpp`): `saveOverrides()`; a commit (the snapshot's files and record,
  read back; the same commit again writing nothing; refusals writing nothing); an action stopped after its journal
  (the snapshot not yet named), after naming it, between the two writes (the new overrides beside the old plan) and
  before the journal's removal, each rolled back at the next lock to the state before byte for byte, no snapshot
  folder left; a stop before the journal, a half-removed `.removing` folder, a rollback stopped after retiring its
  snapshot; a journal naming no snapshot and an unreadable one failing the lock with the repair named, and
  `checkInterruptedAction()` finding the same; the rotation (25 commits, the newest 20 kept, sequences 6 to 25); 20
  commits undone one after the other, a 21st undo finding nothing; undo restoring both files and the sidecar byte for
  byte twice (the second removing `overrides.json`), the plan replaced kept in `backups/`, a dry run writing nothing,
  undo of undo refused, an action after an undo undone; an undo stopped at each point rolled back, its target not
  undone and no other folder left; a snapshot holding another file or missing one refused as damaged; a damaged
  folder listed, refusing undo (a dry run too) and rotated out once 20 folders are newer; records refused.
- Unit (`tests/unit/esde/test_overflow_fixture.cpp`): the overflow fixture's systems, games and covers.
- CLI (`tests/unit/cli/test_cli_worldplan_undo.cpp`): on the building fixture, undo without a world (exit 1, no folder
  made), a schema-1 `overrides.json` by hand, `generate --replace --seed 4242` behind a snapshot, `snapshots` (text and
  JSON), a snapshot missing a file and a damaged folder (listed; undo exit 3), `undo --dry-run`, `undo --json` (the
  plan, its sidecar and the hand-written overrides back byte for byte, the plan replaced in `planBackup`; an update
  then "up to date"), undo of undo refused (exit 1), a regeneration rewriting only the overrides, a journal naming no
  snapshot (`snapshots` naming the repair, undo exit 3); the overflow fixture's building (53 + 7 and 1,561 + 139).
- No golden and no oracle changed.

## Open issues

- The app does not take snapshots yet (it only saves updates): its editor and regenerations come with steps 3 and 6.
- `checkPins()` and the validation judge transform pins, but nothing applies them before step 2 (an update keeps every
  fixture where it stands; the generator places fixtures by their sockets).
- A newer `overrides.json` schema still makes the app log and run without overrides; the editor's refusal ("edits
  nothing and says so") is step 3's.
