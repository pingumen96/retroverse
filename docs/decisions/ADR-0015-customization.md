# ADR-0015: Customization: the editor, overrides, explicit regeneration, expansion rooms

- Status: accepted (proposed and accepted on 2026-10-27: the user asked to proceed with I4b and was not available for
  questions; the decisions taken on the user's behalf are listed in `docs/i4b/plan.md`, "Risks and open questions",
  to be revisited there)
- Date: 2026-10-27
- Amends:
  - ADR-0010 §3 and §10: the face-out set also changes through the editor, not only through a regeneration;
  - ADR-0010 §9: `overrides.json` is written by Retroverse (the editor, the regenerations, the CLI), no longer only
    read; snapshots of the plan and the overrides join `backups/`;
  - ADR-0012 §1: a new world state, `Edit`, with its input context;
  - ADR-0013 §3: a system moves between rooms or floors through "Rigenera piano" and "Rigenera mondo";
  - ADR-0013 §8: expansion rooms are built, with the rules below (generator 1.5).
- Relates to: specification §6 (`PlacementOverride`), §7.3 step 12, §7.6, §13.1, §16.1, §16.3, §21.3, SC-09, M3,
  FR-009, FR-025; style §13.4 ("pinned"), §18 ("modifiche manuali su posizioni e decorazioni") and the warning against
  a complete room editor; ADR-0005 (product UI in RmlUi)

## Context

I4 and I6 built the building and its look; M3 still asks for a world "customizable without manual per-game work"
(specification §22). What the code has:

- **Pins exist but are never applied.** `overrides.json` (`<user data>/worlds/<library id>/`, ADR-0010 §9) holds game
  pins (a game in a slot) and fixture pins (a fixture in its socket, optionally its prefab). The generator and the
  update protect them and report them (`orphanedPins`, `needsConfirmation`); nothing writes the file.
- **Regeneration is all or nothing.** `worldplan generate --replace` regenerates the whole building after a plan
  backup; the app has no regeneration and no preview. The face-out set, a room's family or template version
  (`room-kept`) and a system's room (sticky, ADR-0013 §3) can change only that way.
- **Backups are plans only.** `backups/` keeps the last 10 plans before each write; there is no undo.
- **Capacity.** A room grows to 16 m; beyond, titles are `unplaced` with reason `capacity`. The ids `room:<system>:<n>`
  and the field `expands` are reserved (ADR-0013 §8). The reference library has no capacity entry (I6 step 4: amiga's
  1,055 big boxes in a room of 1,561), but the arcade room holds 53 titles at most (I6 step 6) and
  `placement.include: all` overflows c64 and amiga.

The specification asks that manual changes survive every update (SC-09, FR-009, FR-025, §21.3: "no pin changes after
a single ROM addition with capacity available", "a regeneration never deletes overrides without an explicit action"),
that "Rigenera stanza" and "Rigenera mondo" show an impact summary first (§7.6), and that destructive regenerations
back up first (§16.3). Style §18 allows manual changes of positions and decorations; its last chapter warns against a
complete room editor.

## Decision

### 1. The plan is the truth; overrides are the user's claims

- **The plan** stays what the app draws: an edit changes the plan itself (a placement's slot, a fixture's transform).
  The plan's schema stays 2.
- **The overrides** record what the user decided, so that a regeneration can honour it. An update moves nothing that
  stands (ADR-0010 §10), so an override matters at a regeneration and to the update's few moving rules (growth, the
  successor migration).
- **Every manual change is pinned** (style §13.4): a game the editor moves or swaps is pinned in its new slot, a
  fixture it moves is pinned at its new transform. The user may also pin a game or fixture in place, and unpin.

### 2. `overrides.json`, schema 2

- **Where.** `<user data>/worlds/<library id>/overrides.json`, beside the plan, never under `<user data>/config`
  (configuration stays read-only to Retroverse). Written atomically under the world lock (§6), like the plan.
- **What.**
  - `gamePins`: `{game, slot, provenance}`; a game pinned once, a slot holding one pin (as schema 1).
  - `fixturePins`: `{fixture, transform?, prefab?, provenance}`; `transform` is `{xMm, zMm, yawCdeg}` in the room's
    frame (the height stays the prefab's), absent for a fixture pinned in its socket.
  - `provenance`: `{origin, time}`; `origin` is `editor`, `cli` or `file` (a schema-1 pin, or a hand edit), `time` UTC.
- **Reading.** Schema 1 is read and lifted (`origin: file`); schema 2 is the only one written; a newer schema is
  refused, and the app then edits nothing and says so. Provenance never decides anything and is left out of every
  digest.
- **Digest.** The canonical form of the honoured overrides (provenance left out) joins a plan's `inputDigest` when
  there is any, so that a plan of a world without overrides keeps its digest byte for byte.

### 3. How the generator honours overrides

- **Updates** (ADR-0010 §10, unchanged): every placement is kept, pinned or not. A socket that a pinned fixture holds,
  or that a moved fixture's footprint overlaps, never takes a new module (the next socket does). A user-moved fixture
  stays at its transform when the room grows. A growth that would move a fixture pinned in its south-wall socket is
  refused, its titles unplaced for capacity and the refusal in `needsConfirmation`; the app's notice then offers
  "Rigenera stanza" (§5). A room a pin names is not migrated to a family's successor (ADR-0014 §2, as today).
- **Regenerations** (§5) apply the overrides of their scope, then generate around them:
  1. pinned fixtures first: each at its pinned transform or socket, with its pinned prefab, before the layout fills
     the other sockets;
  2. the sockets holding a pinned game's slot always take their module, whatever the count needs;
  3. pinned games are reserved in their slots before allocation; the face-out choice and the alphabetical fill skip
     those slots and those games;
  4. a pinned game stays its title's representative even when a better-ranked version exists (the pin wins).
- **Conflicts** are pins a regeneration cannot honour, each with its reason: `room-gone` (the room is not in the new
  plan: a system moved to a shared room), `slot-gone` (the slot's fixture or level is not in the new layout),
  `game-gone` (the game is not in the room's selection), `invalid` (a moved fixture breaks a hard rule of the new
  layout). A conflict is **never dropped silently**: it stays in `overrides.json`, reported as orphaned at every update
  and shown by the editor, until the user drops it (§5's consent, or unpinning it in the editor).

*Amended on 2026-10-28 (I4b step 2's review):* a fixture counts as user-moved whenever it stands away from its socket's
pose, pinned or not. A hung decoration whose back is on the south wall moves with that wall when the room grows; its
pin's transform is rewritten and the update saves the plan and `overrides.json` together (§6, action `update`). A
fixture moved back to its own socket loses its pin instead of being pinned there, so it does not refuse growth; only
an explicit pin in a south-wall socket does. A hung decoration off every wall is a hard finding
(`decoration-off-wall`).

*Amended on 2026-10-28 (I4b step 5's review):* a hung decoration whose back is on the south wall follows that wall in
a regeneration too, the room growing or shrinking, its pin's transform rewritten in the overrides saved with the plan
(so a room that shrinks keeps its poster on the wall instead of making it `invalid`). A regeneration records each
conflict on its pin in `overrides.json` (`"conflict": "<reason>"`, an optional key of schema 2, left out of every
digest) and clears it on the pins it honours; a pin the plan does not honour that records a conflict is reported as
orphaned at every update and never in `needsConfirmation`, since the regeneration that the app's notice would offer
cannot honour it either. The editor's next pin of the same game or fixture replaces the pin and its record. The growth
a regeneration gives a room comes from its pinned slots, modules and display fixtures, then a pinned decoration is
judged against it; the side-wall sockets that pins take or block grow the room further, within its template's limit,
until the modules the count needs stand.

### 4. The editor (amends ADR-0012 §1)

A light editor, in the product UI (RmlUi, Italian strings in `assets/ui`), pad and keyboard alike. No Dear ImGui.

- **Entering.** From the directory, "Modifica stanza" while the walker is in a collection room; from the game card in
  Inspect, "Sposta" (the item already held). Never during a ride, a launch, a floor being applied or a regeneration.
  The scope is the resident floor's collection rooms; the hub, halls and the favourites' showcase are not editable.
- **The state `Edit`** has its own input context. The walker walks and looks as in FreeRoam; the reticle picks games,
  empty slots and movable fixtures; a bar at the bottom (`edit_bar.rml`) names the selection, the actions and their
  glyphs, the pending changes and the validation's verdict.
- **Games** (within the room chain that holds them: their system's room and its expansions, §7):
  - `interact` picks a game up (held as in Inspect, smaller); the reticle or the D-pad (Focus's steps, empty slots
    included) chooses a slot; `interact` drops it into an empty slot (**move**) or onto another game (**swap**: the
    other takes the first's slot). A face-out slot is allowed: this is the in-app face-out choice. A dressed level's
    dressing gives way. Back puts the game back.
  - `edit_pin` (P, the pad's west button) pins or unpins the game under the reticle in place.
  - No move to another system's room, no hiding a game, no adding or removing items.
- **Furniture**: side-wall modules, display fixtures and decoration. The hero corner, the hero-wall modules, walls and
  doorways are fixed.
  - `interact` picks a fixture up; it follows the reticle's floor point on the room's 100 mm grid; `edit_rotate_left`
    and `edit_rotate_right` (Q and R, the shoulder buttons) turn it by 90 degrees (decoration by 15). Wall decoration
    slides along the walls only. Its games move with it.
  - It drops only where the hard rules hold (ADR-0010 §11, ADR-0013 §11): no overlap, nothing in a doorway's
    clearance or a door-reserved socket, inside the room, every module, display fixture and the hero TV reachable,
    1.2 m clear. While held it is outlined warm white when it may drop, red otherwise, and the bar names the rule.
  - The room's wall-washers are placed again after a fixture move (lights are derived, never pinned); a decoration
    lamp's light moves with it.
- **The draft.** Each edit is an operation applied to an in-memory draft of the plan and the overrides, validated at
  once and shown through the live update's room-by-room path (a placement move a texture swap, a fixture move the
  room's static part rebuilt). The menu (Start, Enter) offers **Salva**, **Annulla ultima azione** (the session's own
  undo), **Esci senza salvare** and **Continua**; Back with pending edits asks the same.
- **Saving** replays the session's operations onto the stored plan under the world lock, behind a snapshot (§6). If
  the stored plan changed meanwhile (the CLI, an update) and an operation no longer validates, nothing is written and
  the session stays open with the reasons. A building result or a refresh that ends during a session applies after it,
  as during a game.
- **The command line** offers the same operations (§8), so tests and scripts edit without a window.

*Amended on 2026-10-28 (I4b step 3's review):* a session edits the collection room that holds the walker when it begins
("Modifica stanza", or "Sposta" on the card of a game of that room); another room needs a new session. A game's move or
swap is shown by laying the room's collection out again in place. A move into a slot that a dressed level covers makes
the dressing give way by building the resident floor again from the draft, until the room's static part can be rebuilt
alone (I4b step 4, which also needs it for fixture moves); until then such an edit may exceed §9's 0.25 s on a large
floor (2.0 s from the request to the frame that shows it on the reference library's computer floor, 5,314
placements: the floor's job 1.4 s), and step 4 brings it within. The floor's live update does not start during a
session: a refresh that ends during one starts it when the session ends.

### 5. Explicit regeneration with an impact preview

- **Scopes.**
  - **Rigenera stanza**: a room chain (§7) generated afresh as a fresh building generates it, honouring the overrides
    of its rooms: the face-out set and the representatives chosen again, the template's current version (the way out
    of `room-kept`), the family's last successor; its id, kind, floor, x, yaw, sections and seed kept, its z
    following its depth, expansions added or removed as its titles need.
  - **Rigenera piano**: every room of a floor, and the floor's rooms decided again: own or shared rooms by
    `sharedRoomMinTitles` on today's counts, the rooms placed again in entry order from the row start. Systems
    classified onto another floor stay where they are.

    *Amended on 2026-10-28 (I4b step 5's review):* an arcade system standing on another floor than the ground floor
    (its classification changed since it entered) is decided there like any other system, an own room or a section
    of the floor's shared room by `sharedRoomMinTitles`; only "Rigenera mondo" moves it into the arcade room
    (ADR-0013 §3 as amended).
  - **Rigenera mondo**: the whole building generated afresh (`generateBuilding`), honouring every override; systems
    move to the floors their classification names. `worldplan generate --replace` becomes this scope.
- **The preview** runs the regeneration as a dry run on the pool and compares the plans: rooms regenerated, added,
  removed and moved; games that change slot; titles entering and leaving face-out; games newly placed and newly
  unplaced; fixtures added, removed and moved; pins kept; overrides conflicting, each with its reason (§3). It is
  shown in `regenerate_preview.rml` before anything is written.
- **Consent.** Three choices: **Rigenera** (the overrides kept: honoured ones applied, conflicting ones kept and
  reported); **Rigenera e scarta le personalizzazioni** (the scope's overrides dropped, behind a second confirmation
  naming how many); **Annulla**. Nothing is regenerated without one of the first two.
- **After.** The result is saved behind a snapshot (§6) and the resident floor reconciled room by room; a walker in a
  regenerated room stands at its doorway, facing in.

### 6. Snapshots, transactions and undo

- **Snapshots.** Before every explicit action (an editor save, a regeneration, an undo, a CLI edit), the plan and the
  overrides as they are go into `backups/snapshots/<UTC time>-<action>/` with `snapshot.json` (the action, its scope,
  the writer, the digests). The last 20 are kept. The plan-only backups of every plan write (ADR-0010 §9, the last 10)
  stay as they are.
- **Transactions.** The two files change together: under the world lock, the snapshot first, then a journal
  `transaction.json` naming it, then `overrides.json`, then the plan, then the journal removed. A lock that finds a
  journal (a crash in between) restores the snapshot and logs it, so the plan and the overrides never disagree.
- **Undo.** "Annulla ultima modifica" (the directory, the editor's menu, `worldplan undo`) restores the newest
  snapshot not yet undone, marks it undone, and keeps the state it replaced as a snapshot of kind `undo`, which undo
  never offers: undo walks back up to the kept snapshots, and redo is not offered. The catalog's changes since then
  come back through the next update as usual. Incremental updates are not undoable actions (their plan backups stay).

### 7. Expansion rooms (amends ADR-0013 §8; generator 1.5)

- **When.** A room at its template's growth limit with titles it holds no slot for gets an expansion, instead of
  unplacing them for capacity. A room and its expansions are a **chain**; its head is the room as I4 made it.
- **Ids and record.** `room:<system>:<n>`, `area:shared:<floor>:<n>`, `area:arcade:<n>`, n from 2, the next number
  after the chain's highest, never reused. An expansion's `expands` names the head; it has the head's kind, floor,
  sections, family and template. Its seed derives from its own id.
- **Where and how it is reached.** It enters its head's floor as any new room does (ADR-0013 §7: the east end of the
  shorter row, its x fixed), so it need not stand beside its head. It is reached from the hall (the hub on the ground
  floor) through its own doorway, like every room. Its portal and neon sign show the head's sign with its number. On a
  full floor the titles stay unplaced for capacity, naming the head.
- **Fill.** One fill order per chain (head, then expansions by number); the predecessor rule runs over the chain, so
  slots freed in the head are used first.
- **Fresh generation** (and "Rigenera stanza") splits the chain's titles into the fewest rooms that hold them with the
  reserve, in equal alphabetical shares, each room sized to its share and choosing its own face-out titles from it by
  priority.
- **Updates.** An expansion an update creates has no face-out titles (its display fixtures and band dressed, ADR-0010
  §10: new titles enter spine-out) until the chain is regenerated.
- **Migration.** Generator 1.5 is a minor version: at the first update, the titles a stored plan holds as unplaced for
  capacity in a room at its limit enter its chain as additions. Nothing else moves; the change report's `roomsAdded`
  and the app's notice name the new rooms.
- **Validation.** An expansion's head exists, on the same floor, of the same kind, family and template; the chain is
  one room for "every system is in one room or one section".
- **In the app.** The directory lists a chain under its system, each room with its title range ("A – K"), from the
  items files; teleport, "Mostra nella stanza" and where-is reach any room of a chain; an expansion's CRT plays its
  own games (ADR-0013 §9, unchanged). A door between a head and an expansion that stand side by side is not built.

### 8. Command line

- `worldplan edit move --game <id> --slot <slot>`, `swap --game <id> --with <id>`, `pin | unpin --game <id>`,
  `move-fixture --fixture <id> --x-mm <n> --z-mm <n> [--yaw-cdeg <n>]`, `pin-fixture | unpin-fixture --fixture <id>`,
  and `edit --script <file.json>` (several operations as one session, one snapshot); `--dry-run`, `--json`. A refused
  operation exits 1 naming the rule.
- `worldplan regenerate (--room <id> | --floor <id> | --world) [--drop-overrides] [--dry-run] [--json]`: the impact
  summary, then the regeneration unless `--dry-run`. `--drop-overrides` is the CLI's consent.
- `worldplan snapshots [--json]` and `worldplan undo [--dry-run]`.
- `worldplan check` also measures the overrides against the plan (honoured, orphaned, conflicting).

### 9. Acceptable performance (added to the roadmap's table)

- **An edit** (a move, a swap, a fixture moved) is shown within 0.25 s, the frames within the existing p99 of 50 ms.
- **The impact preview** is shown within 2 s for a room and 15 s for the world on the reference library.

*Amended on 2026-10-28 (I4b step 5's review):* a floor's impact preview is shown within 15 s, as the world's (the
roadmap's row names it).
- **A save, a regeneration's commit or an undo**: the room shown again within 3 s warm.

## Amendments

- **ADR-0010 §3, §10:** "The face-out set changes only with an explicit regeneration" reads "only with an explicit
  regeneration or the editor".
- **ADR-0010 §9:** `overrides.json` is written by Retroverse under the world lock (schema 2, §2); `backups/snapshots/`
  holds paired snapshots (§6).
- **ADR-0012 §1:** the state `Edit` (§4) joins FreeRoam, Focus, Inspect and Launching; the card in Inspect gains
  "Sposta".
- **ADR-0013 §3:** a system's room and floor change only through "Rigenera piano" or "Rigenera mondo".
- **ADR-0013 §8:** built as §7 above. A fresh generation splits in equal shares; an expansion stands where its floor
  places it, reached from the hall.
- **ADR-0013 §12:** per-room and per-floor regeneration exist (§8 above).

## Consequences

**Gains**
- SC-09 holds end to end: a shelf moved, a favourite put on a display case, games added, nothing overwritten without
  consent.
- The face-out set, `room-kept` rooms and a system's room can be refreshed from the app, with a preview.
- Every explicit action can be undone; a crash never leaves the plan and the overrides out of step.
- No title is unplaced for capacity while its floor has room for an expansion.

**Costs**
- A new world state and input context, two new RML documents and three new input actions.
- A fixture moved by the user makes the room's layout no longer the template's; validation is the only judge.
- An update-created expansion shows no face-out titles until its chain is regenerated.
- An expansion may stand far from its head on a long floor.

**Deferred**
- "Riorganizza scaffale" as its own action ("Rigenera stanza" covers it).
- Redo; undo of incremental updates.
- Moving games between systems' rooms, hiding games, adding or removing furniture, choosing a decoration's prefab in
  the app (the file's `prefab` is honoured).
- Moving the hero corner and the hero-wall modules.
- An interior door between a head and an adjacent expansion (the door-reserved sockets allow it later).
