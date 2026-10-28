# I4b step 2: edits in rv_world and `worldplan edit`

Roadmap row: I4b step 2. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §1 to §4 and §8. Plan:
[plan.md](plan.md).

## What was built

- **The edits** (`rv/world/world_edit.hpp`, new): `move` (a placed game into an empty slot of its room chain, a
  face-out slot too), `swap`, `pin` / `unpin` a game in place, `move-fixture` (a side-wall module, display fixture or
  decoration moved and turned), `pin-fixture` / `unpin-fixture`. Each is applied to a copy of the plan and the
  overrides; it is refused (the world untouched, `EditResult::refused` naming the rule) by the edit's own rules
  (`editRules()`: `game-unknown`, `slot-taken`, `slot-pinned`, `other-room`, `same-game`, `fixture-unknown`,
  `fixture-fixed`, `fixture-in-kept-room`, `off-grid`, `turn-step`, `wall-mount`; a slot the plan lacks by the
  validation's `slot-unknown`) or by a hard finding of `validatePlan()` on the floor it edits that it caused, one the
  floor did not have before it (its rooms, their templates, kept rooms judged as kept, the building's halls). Every
  manual change is pinned: a game moved or swapped in its new slot, a fixture where it was put; a fixture put back
  home has its pin removed (a pinned prefab stays pinned in its socket); provenance is the caller's.
- **What moves with what.** A dressed span gives way where a game lands (split around its slot); a face-out slot a game
  leaves is dressed with one slot of its level's prop, a spine-out one stays free (as an update frees slots). A game's
  offset is drawn again in its new slot as a fresh plan draws it. A fixture's games and dressing follow it (they name
  it); a lamp's light is placed again at its bulb; a moved side-wall module has the room's wall-washers placed again
  over the modules where they stand (`detail::modulesOnWalls()`: a socket made where a moved module stands, every
  module judged where it stands, pinned or not).
- **Snapping** (`fixtureHome()`, `snapFixture()`): a movable fixture's home (where a fresh layout stands it, the origin
  of its grid) and the place nearest a wanted one that the edit's own rules accept (the yaw to its turn step, x and z
  to the grid through its home; a poster onto the nearest wall, its back as far off it as at home, along it on the grid
  within the wall's length), so that step 4's app snaps without copying layout code; `applyEdit()` still judges the
  place.
- **Updates honouring moved fixtures** (`incremental.cpp`): a fixture standing elsewhere than its home (moved, pinned
  or not: `detail::standsMoved()`) or pinned at a transform is never moved by growth (nor its light; the wall-washers
  then placed again), and no module replaces it in its socket; but a decoration hung on the south wall follows the
  wall (else it would hang 2 m in front of it in mid-air), its pin's transform rewritten (`GenerationResult::
  overrides`, which `worldplan update` and the app save with the plan through `commitWorld()`, action `update`); a
  socket whose new module, or growth decoration, would overlap a fixture of another socket is skipped (the module to
  the next socket, reported in `needsConfirmation`); a growth is refused only for a fixture pinned in its south-wall
  socket, as before.
- **A new hard rule** `decoration-off-wall` (38 rules): a decoration hung above the floor stands within 100 mm of a
  wall's interior face, so that no plan with a poster in mid-air is ever written; kept rooms are not judged by it. It
  judges the fixture's box (`detail::nearestWallOf()`), whatever its yaw, because the scene's drawn era variants
  (`drawnFixtures()`) are boxes turned 0; the edit's `wall-mount` and the growth share the stricter
  `detail::wallMountOf()` / `hangsOnWall()` (the back on the wall the yaw faces away from).
- **The overrides in `inputDigest`**: the SHA-256 of the canonical form of the pins the plan honours (provenance left
  out) is XORed into the inputs' digest by `generateBuilding()`, the update and every edit; no honoured pin, no term.
- **`worldplan edit`** (`tools/cli/src/cmd_worldplan_edit.cpp`, `docs/reference/cli.md`): one operation, or
  `--script <file.json>` (several as one session, all or nothing); `--dry-run`, `--json`; under the world lock, the
  catalog never read, saved through `commitWorld()` (action `edit`) so `worldplan undo` restores it; a refusal exits 1
  naming the rule. `worldplan generate --replace` over pinned edits fails `pin-broken` with a hint (unpin, or step 5's
  regeneration).

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
  hand-written pins has no term yet: its first update or edit writes the plan once, its digest alone changed
  (`test_cli_worldplan_undo` adapted to say so).
- **The grid runs through a fixture's home**: x and z move in 100 mm steps from where its socket stands it, so a
  fixture slid along its wall stays flush with it. A hung decoration (a decoration above the floor: a poster) keeps its
  back on a wall's face (0 to 100 mm), faces into the room, and only its coordinate along the wall is on the grid.
- **Turns** are checked as absolute yaws: multiples of 9000 cdeg for modules and display fixtures, 1500 for decoration.
- **A refusal names the first hard finding about the moved fixture**, else the first hard finding (a fixture in a
  doorway is `doorway-clearance`, not the floor rule that runs earlier).
- **Wall-washers are placed again only when a side-wall module moves**, by the update's rule over the room's lights;
  a room without moved modules is lit exactly as before. "Moved" follows where a fixture stands, not its pin: a module
  unpinned after a move is lit where it stands, and growth leaves it there.
- **A fixture moved back home is unpinned.** ADR-0015 §3 refuses a growth that would move a fixture pinned in its
  south-wall socket; pinning a fixture dropped back home in its socket would block its room's growth for good after a
  move undone by hand. Letting growth move socket-pinned south-wall fixtures would contradict §3 (a new ADR), so the
  move back home removes the pin instead: at home a fixture stands where the layout stands it and there is nothing to
  remember (style §13.4's "every manual change pinned" holds for every place but the generator's own). A pinned prefab
  stays pinned, in its socket; an explicit `pin-fixture` at home still pins in the socket and still refuses the growth.
- **A poster on the south wall follows it.** ADR-0015 §3 keeps a moved fixture at its transform when the room grows;
  for a decoration hung on the south wall that would leave it in mid-air, which the new `decoration-off-wall` rule
  refuses. It moves with its wall instead, its pin's transform rewritten; a floor-standing fixture stays where it was
  put. The update then changes the overrides, so it saves both behind a snapshot (action `update`, undoable) instead of
  the plan alone.
- **A refusal is about what the edit caused.** The floor's hard findings before the edit (computed once per floor in a
  session, then each applied edit's) are subtracted: a finding already there (a plan written by hand) blocks nothing,
  one whose message changes with the edit counts as caused.
- **Pins only** (pin, unpin) change the plan's `inputDigest` alone; a pin in place over an unhonoured hand-written pin
  of the same game replaces it; a slot pinned to another game is refused, never overwritten.
- **Fixtures of a kept room** (an older template) are not moved (`fixture-in-kept-room`, named apart from the
  validation's soft `room-kept`); its games are.

## Proofs

- Unit (`tests/unit/world/test_world_edit.cpp`, 17 cases): a move pinned, the update of the same selection equal byte
  for byte, moved back and unpinned the plan as generated; face-out dressing split and a left face-out slot dressed;
  swap, pin, unpin, an orphaned pin dropped; every edit refusal and `slot-unknown`, the world unchanged; games kept
  within their chain (`other-room`), missing data an error; the digest: no overrides the inputs' digest exactly, an
  edit's pin equal to a generation honouring the same pin, provenance and unhonoured pins left out; fixtures moved and
  turned 15 degrees (the lamp's light following), put back home and so unpinned, the plan as generated; refusals
  `fixture-unknown`, `fixture-fixed`, `off-grid`, `turn-step`, `fixture-in-kept-room`, `fixture-outside-room`,
  `footprints-overlap`, `doorway-clearance`, `door-reserved`, `wall-mount`; an update with 2,500 titles added keeping
  every pin and moved fixture (§21.3), the overlapped socket skipped and reported; growth leaving a moved south-wall
  fixture where it stands, a socket-pinned one still refusing it; a property test of 3 × 40 random edits (each refusal
  leaving the world unchanged, each applied edit valid and pinned unless back home, the update then writing nothing);
  the script format. After the review: a fixture moved then unpinned still held by growth; the wall-washers equal with
  w03 pinned or unpinned after w01 moves, none over w03's empty socket; a south-wall fixture moved back home unpinned
  and moving with its wall again; `fixtureHome()` and `snapFixture()` (grid, turn step, a poster onto the nearest
  wall, every snapped place passing the edit's own rules); a poster slid along `dec-sw`/`dec-se`, and one moved onto
  the south wall from the west wall, following the wall through growth with its pin rewritten (the next update writing
  nothing), one left in mid-air found `decoration-off-wall`; a plan with an unrelated hard finding still edited (a
  move, a session), an edit's own `doorway-clearance` still refused. `test_validation`: `decoration-off-wall` at
  100/101 mm. CLI: `generate --replace` after a move exits 3 with the unpin hint, nothing written.
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

## Measure in step 3

- **The preview's cost.** Each `applyEdit()` copies the plan and the overrides and validates the edited floor (twice
  when that floor already has a hard finding: before and after); the editor previews a drag with it. ADR-0015 §9 asks
  for 0.25 s on the real plan: measure it on the real library's largest floor in step 3 before relying on it per
  frame, and keep the per-session floor cache (`applyEdits()`) in mind.

## Open issues

- `worldplan generate --replace` still applies no pin (step 5): with a moved fixture's or a moved game's pin it fails
  `pin-broken` and writes nothing until the regeneration honours pins (stderr names the unpin commands).
- The update's rewritten pins are saved by `worldplan update` and the app; no CLI test grows a fixture world with a
  pinned south-wall poster yet (the world tests prove the update's result).
- An update's new module next to a moved fixture can still close a passage; the update's validation then refuses the
  plan (exit 3), and the remedy is step 5's "Rigenera stanza" (the plan's risk "growth and moved fixtures").
- `worldplan check` does not measure overrides yet (ADR-0015 §8; with step 5's conflicts).
- The app reads the new digests as any update's; its editor (draft, replay on save) is step 3's.
