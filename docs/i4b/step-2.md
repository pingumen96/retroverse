# I4b step 2: edits in rv_world and `worldplan edit`

Roadmap row: I4b step 2. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §1 to §4 and §8. Plan:
[plan.md](plan.md).

## What was built

- **The edits** (`rv/world/world_edit.hpp`, new): `move` (a placed game into an empty slot of its room chain, a
  face-out slot too), `swap`, `pin` / `unpin` a game in place, `move-fixture` (a side-wall module, display fixture or
  decoration moved and turned), `pin-fixture` / `unpin-fixture`. Each is applied to a copy of the plan and the
  overrides; it is refused (the world untouched, `EditResult::refused` naming the rule) by the edit's own rules
  (`editRules()`: `game-unknown`, `slot-taken`, `slot-pinned`, `other-room`, `same-game`, `fixture-unknown`,
  `fixture-fixed`, `room-kept`, `off-grid`, `turn-step`, `wall-mount`; a slot the plan lacks by the validation's
  `slot-unknown`) or by any hard finding of `validatePlan()` on the floor it edits (its rooms, their templates, kept
  rooms judged as kept, the building's halls). Every manual change is pinned: a game moved or swapped in its new slot, a
  fixture where it was put (in its socket, with no transform, when put back home); provenance is the caller's.
- **What moves with what.** A dressed span gives way where a game lands (split around its slot); a face-out slot a game
  leaves is dressed with one slot of its level's prop, a spine-out one stays free (as an update frees slots). A game's
  offset is drawn again in its new slot as a fresh plan draws it. A fixture's games and dressing follow it (they name
  it); a lamp's light is placed again at its bulb; a moved side-wall module has the room's wall-washers placed again
  over the modules where they stand (`detail::modulesOnWalls()`: a socket made where a moved module stands).
- **Updates honouring moved fixtures** (`incremental.cpp`): a fixture pinned at a transform is never moved by growth
  (nor its light; the wall-washers then placed again); a socket whose new module, or growth decoration, would overlap a
  fixture of another socket is skipped (the module to the next socket, reported in `needsConfirmation`); a growth is
  refused only for a fixture pinned in its south-wall socket, as before.
- **The overrides in `inputDigest`**: the SHA-256 of the canonical form of the pins the plan honours (provenance left
  out) is XORed into the inputs' digest by `generateBuilding()`, the update and every edit; no honoured pin, no term.
- **`worldplan edit`** (`tools/cli/src/cmd_worldplan_edit.cpp`, `docs/reference/cli.md`): one operation, or
  `--script <file.json>` (several as one session, all or nothing); `--dry-run`, `--json`; under the world lock, the
  catalog never read, saved through `commitWorld()` (action `edit`) so `worldplan undo` restores it; a refusal exits 1
  naming the rule.

### The frozen signature

```cpp
struct EditedWorld { WorldPlan plan; WorldOverrides overrides; };
struct EditData { WorldConfig config; BuildingData building; std::map<RoomId, std::shared_ptr<const WorldData>> rooms; };
Expected<EditResult> applyEdit(EditedWorld& world, const EditOperation& operation, const EditData& data,
                               const PinProvenance& provenance);
Expected<EditSession> applyEdits(EditedWorld& world, std::span<const EditOperation> operations, const EditData& data,
                                 const PinProvenance& provenance);
```

`EditOperation` (kind, game, slot, with, fixture, xMm, zMm, optional yawCdeg; factories per kind), `EditResult`
(refused, changed, rebuild, rooms, fixturesMoved, floor, validation, summary), `editRooms()` (the rooms whose data a
session needs) and `editedResult()` (the `GenerationResult` `commitWorld()` saves) complete it. Step 3's draft keeps
its operations and replays them with `applyEdits()` on the stored plan at save; `EditResult::rebuild` and `rooms`
drive the live update's room-by-room path. The script's format (`parseEditScript()` / `writeEditScript()`) can carry a
draft.

## Decisions beyond ADR-0015

- **The digest's join is an XOR.** ADR-0015 §2 joins the honoured overrides to `inputDigest`; joined as a document the
  edit could not compute it without the catalog's selections. XORing the overrides' SHA-256 into the inputs' digest
  lets an edit swap the old term for the new one, so an edit then an update of an unchanged catalog writes nothing,
  and a world without overrides keeps every digest (the term is empty). A plan written before step 2 beside honoured
  hand-written pins has no term yet: its first update writes the plan once, its digest alone changed
  (`test_cli_worldplan_undo` adapted to say so).
- **The grid runs through a fixture's home**: x and z move in 100 mm steps from where its socket stands it, so a
  fixture slid along its wall stays flush with it. A hung decoration (a decoration above the floor: a poster) keeps its
  back on a wall's face (0 to 100 mm), faces into the room, and only its coordinate along the wall is on the grid.
- **Turns** are checked as absolute yaws: multiples of 9000 cdeg for modules and display fixtures, 1500 for decoration.
- **A refusal names the first hard finding about the moved fixture**, else the first hard finding (a fixture in a
  doorway is `doorway-clearance`, not the floor rule that runs earlier).
- **Wall-washers are placed again only when a side-wall module moves**, by the update's rule over the room's lights;
  a room without moved modules is lit exactly as before. A module unpinned after a move is lit from its socket again.
- **Pins only** (pin, unpin) change the plan's `inputDigest` alone; a pin in place over an unhonoured hand-written pin
  of the same game replaces it; a slot pinned to another game is refused, never overwritten.
- **Fixtures of a kept room** (an older template) are not moved; its games are.

## Proofs

- Unit (`tests/unit/world/test_world_edit.cpp`, 11 cases): a move pinned, the update of the same selection equal byte
  for byte, moved back and unpinned the plan as generated; face-out dressing split and a left face-out slot dressed;
  swap, pin, unpin, an orphaned pin dropped; every edit refusal and `slot-unknown`, the world unchanged; games kept
  within their chain (`other-room`), missing data an error; the digest: no overrides the inputs' digest exactly, an
  edit's pin equal to a generation honouring the same pin, provenance and unhonoured pins left out; fixtures moved and
  turned 15 degrees (the lamp's light following), put back home and unpinned to the plan as generated; refusals
  `fixture-unknown`, `fixture-fixed`, `off-grid`, `turn-step`, `room-kept`, `fixture-outside-room`,
  `footprints-overlap`, `doorway-clearance`, `door-reserved`, `wall-mount`; an update with 2,500 titles added keeping
  every pin and moved fixture (§21.3), the overlapped socket skipped and reported; growth leaving a moved south-wall
  fixture where it stands, a socket-pinned one still refusing it; a property test of 3 × 40 random edits (each refusal
  leaving the world unchanged, each applied edit valid and pinned, the update then writing nothing); the script format.
- CLI (`tests/unit/cli/test_cli_worldplan_edit.cpp`): on the building fixture, no plan (exit 1); a move behind a
  snapshot (origin `cli`) and the update "up to date"; `slot-taken` (exit 1, nothing written, JSON `refused`); a dry
  run; a script of two operations as one snapshot, undone to the move's state; `off-grid`; pin and unpin of a display
  case; usage errors (exit 2).
- No plan golden, scene golden, oracle or legacy fixture changed; the goldens hold no pin, so their digests are
  untouched.
- By eye: `worldplan edit --script build/i4b/step2/edits.json` on the golden SNES room of 890 titles (a display case
  turned towards the doorway, a side module stood out in the room, a poster slid along its wall, a floor lamp moved
  and turned 15 degrees), `worldplan check` passed, `worldplan show --scene` and `retroverse.exe --scene` before and
  after: `build/i4b/step2/before.png`, `after.png` (the module's dressing and the lamp's light follow; nothing
  intersects).

## Open issues

- `worldplan generate --replace` still applies no pin (step 5): with a moved fixture's or a moved game's pin it fails
  `pin-broken` and writes nothing until the regeneration honours pins.
- An update's new module next to a moved fixture can still close a passage; the update's validation then refuses the
  plan (exit 3), and the remedy is step 5's "Rigenera stanza" (the plan's risk "growth and moved fixtures").
- `worldplan check` does not measure overrides yet (ADR-0015 §8; with step 5's conflicts).
- The app reads the new digests as any update's; its editor (draft, replay on save) is step 3's.
