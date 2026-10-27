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
  as one explicit action: a snapshot in `backups/snapshots/<time>-<action>/` (made in a `.partial` folder renamed
  whole; the plan, its meta sidecar and `overrides.json` as they were, and `snapshot.json`: sequence, kind, action,
  scope, writer, time, the three digests, undone, undoes), then the journal `transaction.json`, then the overrides,
  then the plan (`savePlan()`), then the journal removed and the snapshots beyond 20 removed. A failed write restores
  the snapshot at once. `WorldLock::acquire()` removes half-made snapshots and the temporary files inside snapshots,
  and rolls back an action whose journal it finds (the snapshot restored byte for byte, then removed with the journal;
  `WorldLock::rolledBack()` names it). A journal it cannot honour fails the lock (nothing is written until repaired).
- **Undo** (`undoLastAction()`): the newest snapshot of kind `action` not undone, its files checked against their
  digests, restored byte for byte as a transaction of its own behind a snapshot of kind `undo` (naming what it undid,
  never offered); the restored snapshot is marked undone. A rollback of an interrupted undo clears that mark. A dry
  run only names the snapshot.
- **The command line**: `worldplan snapshots [--json]` (reads only; reports a pending journal) and `worldplan undo
  [--dry-run] [--json]` (exit 1 nothing to undo). `worldplan generate --replace` over a stored plan, the explicit
  regeneration, now saves through `commitWorld()` (action `regenerate`, scope `world`; "Snapshot:" in its summary,
  JSON `snapshot`), so it can be undone; it reads the overrides after taking the lock.

## Decisions beyond ADR-0015

- **The meta sidecar travels with the plan** in a snapshot and is restored with it, so that after an undo
  `world-plan.meta.json` describes the plan beside it (it stays informative).
- **Snapshots are ordered by `sequence`** (one more than the highest kept), never by their folder's time, which a clock
  set back would disorder; names only need to be unique (`-2`, `-3` within one millisecond).
- **A rolled-back action leaves no snapshot**: its snapshot equals the state restored, so it is removed with the
  journal. A crash after the snapshot but before the journal leaves one snapshot of the state as it stands (undoing it
  changes nothing); a crash before the rename leaves a `.partial` folder, which the next lock removes.
- **Undo snapshots count toward the 20 kept.**
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
  read back; the same commit again writing nothing; refusals writing nothing); an action stopped after its journal,
  between the two writes (the new overrides beside the old plan) and before the journal's removal, each rolled back at
  the next lock to the state before byte for byte, its snapshot gone; a stop before the journal and a `.partial`
  folder; a journal naming no snapshot failing the lock; the rotation (25 commits, the newest 20 kept, sequences 6 to
  25); undo restoring both files and the sidecar byte for byte twice (the second removing `overrides.json`), a dry run
  writing nothing, undo of undo refused, an action after an undo undone; an undo stopped between its writes and before
  its journal's removal rolled back, its target not undone; a damaged snapshot refused; records refused.
- Unit (`tests/unit/esde/test_overflow_fixture.cpp`): the overflow fixture's systems, games and covers.
- CLI (`tests/unit/cli/test_cli_worldplan_undo.cpp`): on the building fixture, undo without a world (exit 1, no folder
  made), a schema-1 `overrides.json` by hand, `generate --replace --seed 4242` behind a snapshot, `snapshots` (text and
  JSON), `undo --dry-run`, `undo --json` (the plan, its sidecar and the hand-written overrides back byte for byte; an
  update then "up to date"), undo of undo refused (exit 1); the overflow fixture's building (53 + 7 and 1,561 + 139).
- No golden and no oracle changed.

## Open issues

- The app does not take snapshots yet (it only saves updates): its editor and regenerations come with steps 3 and 6.
- `checkPins()` and the validation judge transform pins, but nothing applies them before step 2 (an update keeps every
  fixture where it stands; the generator places fixtures by their sockets).
- A newer `overrides.json` schema still makes the app log and run without overrides; the editor's refusal ("edits
  nothing and says so") is step 3's.
