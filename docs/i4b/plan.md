# I4b plan: M3 part 2, customization

The I4b section below is ready to paste into `docs/roadmap.md` (before the I6 section, as the current iteration; its
links are relative to `docs/`),
followed by the iteration table's new row, the acceptable rows, the "Deferred" line, the steps that can run beside
I5, and the decisions taken on the user's behalf.

---

## I4b: M3 part 2, customization

[ADR-0015](decisions/ADR-0015-customization.md), planned on October 27, 2026 after the user asked to proceed with
I4b and then I5 (decisions taken on the user's behalf: [i4b/plan.md](i4b/plan.md), "Risks and open questions"):
- **the editor**: games moved, swapped and pinned, furniture moved on the room's grid, written to `overrides.json`;
- **"Rigenera stanza / piano / mondo"** with an impact preview and explicit consent to drop customizations;
- **snapshots and undo** of every explicit action;
- **linked expansion rooms** for titles past a room's capacity (generator 1.5).

It closes M3: "a heterogeneous collection generated automatically, navigable and customizable without manual
per-game work". The look does not change: no golden is expected to move, and none is added.

| # | Step | Needs | Proof | Status |
|---|---|---|---|---|
| 0 | ADR-0015, this plan, the BACKLOG's I4b items moved here; a fixture `esde/overflow` (a big-box system of about 1,700 titles, an arcade of 60, generated at test time) | | The files exist | |
| 1 | Overrides written and undone: `overrides.json` schema 2 (provenance, the schema-1 lift), `saveOverrides` under the world lock, paired snapshots in `backups/snapshots/` (the last 20), the journal and its rollback, `undo`; `worldplan snapshots` and `worldplan undo` | 0 | Unit: round trips, the lift, a crash injected between the two writes rolled back at the next lock, the rotation, undo restoring both files byte for byte, undo of undo refused; the CLI's `undo` on a fixture world | |
| 2 | Edits in `rv_world`: `applyEdit` (move, swap, pin, unpin, move and turn a fixture) on a plan and its overrides, validated by the hard rules, dressing and wall-washers placed again; updates honouring moved fixtures (sockets they overlap skipped, never moved by growth); the overrides in `inputDigest`; `worldplan edit` (with `--script`) | 1 | Unit and property: each edit and each refusal by rule; an edit then an update of an unchanged catalog writes nothing; an update with additions keeps every pin and moved fixture (§21.3); a world without overrides keeps its digests; an edited room through `worldplan show --scene` and `retroverse.exe --scene`, judged by eye | done ([step-2.md](step-2.md)): `applyEdit()` / `applyEdits()` (frozen) with their own rules and the floor's hard rules, every manual change pinned; the honoured pins XORed into `inputDigest`; updates keep moved fixtures (overlapped sockets skipped, never moved by growth, wall-washers over modules where they stand); `worldplan edit` (`--script`, `--dry-run`) behind a snapshot |
| 3 | The editor in the app, games: the state `Edit` and its context, "Modifica stanza" in the directory and "Sposta" on the card, empty slots and the held item, move, swap, pin and unpin, `edit_bar.rml`, the draft through the live update's path, Salva / Annulla ultima azione / Esci senza salvare, updates deferred during a session | 2 | Unit: the interaction's transitions and contexts, the draft's history, a save replayed over a changed plan; GPU `gpu.world_edit`: a scripted session moves a cartridge onto a display case and swaps two, saves, restarts and finds them there; a discarded session leaves the plan's SHA-256 unchanged; screenshots of the bar | |
| 4 | The editor, furniture: fixtures picked, moved on the 100 mm grid and turned, the outline and the bar naming the broken rule, a lamp's light and the wall-washers following | 3 | Unit: drops refused in a doorway, a door-reserved socket, over another fixture, closing the 1.2 m passage, cutting a module off; GPU: a poster and a side module moved and saved, a refused drop, the room walked after; screenshots | |
| 5 | Regeneration in `rv_world`: room chain, floor and world, honouring overrides (pinned fixtures first, pinned games reserved, the pin winning the representative), conflicts with reasons, the impact summary, keep or drop; `worldplan regenerate`, `generate --replace` as `--world` | 2 (beside 3, 4) | Unit and property: a regeneration never drops an override without `--drop-overrides`; every honoured pin stands; the face-out set chosen again by priority; a `room-kept` room regenerated to the current template; a floor's sharing decided again; a dry run writes nothing; the impact equals the plans' difference; twice one SHA-256; the real library's dry runs (a room, a floor, the world) through `real-library.ps1`, 0 differences | |
| 6 | Regeneration and undo in the app: the directory's "Rigenera stanza / piano / mondo" and "Annulla ultima modifica", `regenerate_preview.rml` (the counts, the conflicts, the three choices and the second confirmation), the job on the pool, the floor reconciled, the walker at the room's doorway; the update's `needsConfirmation` notice opening the preview | 5, 3 | Unit: the preview's model and commands; GPU: a room regenerated with its pins kept, then undone (the plan's SHA-256 back); a regeneration dropping overrides only after the second confirmation; the preview within 2 s; screenshots of the preview | |
| 7 | Expansion rooms in `rv_world` (generator 1.5): chains, ids and `expands`, entry on the floor, one fill order per chain, the fresh split in equal alphabetical shares, update-created expansions without face-out titles, the migration of capacity entries, chains in regeneration and validation, a full floor | 5 | Unit and property on `esde/overflow`: no title over capacity while the floor has room; a 1.4 plan with capacity entries migrated, every other room byte for byte in its elements; an expansion's head's freed slots used first; twice one SHA-256; the real library's update through `real-library.ps1`: no capacity entry before or after, every room's elements byte for byte | |
| 8 | Expansion rooms in the app: the portal and neon signs with their number, the directory's chains with title ranges, teleport, "Mostra nella stanza" and where-is into an expansion, its CRT playing its games, the editor within a chain | 7, 6 | GPU on `esde/overflow`: a walk from the hall into `room:<system>:2`, a cover inspected; the arcade's 60 titles all placed; a game moved from the head to its expansion; screenshots of the hall's portals and the directory | |
| 9 | Gate M3: `scripts/gate.ps1 -Gate M3` (and `-RealLibrary`), `docs/testing/gate-m3.md` | 4, 8 | The gate passes; the user tries the editor, a regeneration and an undo, and declares M3 done | |

Order: 0, 1, 2, then 3 and 4 beside 5 and 7, then 6, 8, 9. Steps 1, 2, 5 and 7 change `rv_world`, `tools/cli` and
their unit tests only; steps 3, 4, 6 and 8 change `rv_app`, `rv_ui`, `rv_input` and `assets/ui`. `applyEdit`'s and
the regeneration's signatures are frozen at the end of steps 2 and 5.

### Gate M3

`scripts/gate.ps1 -Gate M3` runs Gate A's build matrix (no warning), the unit tests and the GPU smoke tests in Debug
and Release, every golden in both (G01 to G18, none rewritten), a row per I4b area counted from those runs (the
overrides and snapshots, the edits, the editor's games and furniture, the regenerations and their preview, undo, the
expansion rooms in the world and in the app), the benchmark path in Release and ADR-0015 §9's rows against the
acceptable limits, prints the summary table (logs in `build/gate-m3`) and exits 1 when a step failed; all of it under
`scripts/focus_watch.py`. `-RealLibrary` adds, with ES-DE closed, one `scripts/real-library.ps1` run (snapshots before
and after, 0 differences): the building updated by generator 1.5 (no capacity entry, every room's elements byte for
byte), dry runs of "Rigenera stanza" on the SNES room, of a floor and of the world (their impact counts, nothing
written, the preview's time), a CLI edit session on the real plan in the build's data directory (a move, a swap, a
pin, a fixture moved), an update moving nothing, an undo restoring the plan's SHA-256, and Gate C's rows (the lobby
within 5 s, the busiest floor within 512 MiB, the rides). `docs/testing/gate-m3.md` holds the figures, where ADR-0015
and M3 part 2's requirements (SC-09, FR-009, FR-025, §7.6, §16.3, §21.3) are proven, the follow-ups and how to try
the editor. Then the user tries it and declares M3 done.

---

## The iteration table's row

Replace the I4b row with:

| I4b | M3 part 2, customization (ADR-0015): the editor (games moved, swapped and pinned, furniture moved on the room's grid) written to `overrides.json`, "Rigenera stanza / piano / mondo" with an impact preview and consent, snapshots and undo, linked expansion rooms past a room's capacity (generator 1.5) | Gate M3: M3 Done (customizable without manual per-game work) |

And append to the "Done:" paragraph: "I4b, customization ([ADR-0015](decisions/ADR-0015-customization.md)), planned on
October 27, 2026, I5 planned beside it."

## The acceptable rows (added to "Acceptable performance")

| Measure | Acceptable |
|---|---|
| An edit shown (a move, a swap, a fixture moved) | 0.25 s, the frames within the 50 ms p99 row |
| The impact preview of a regeneration | a room 2 s, the world 15 s on the reference library |
| A save, a regeneration's commit or an undo | the room shown again within 3 s warm |

## "Deferred"

Replace the I4b part of the I4 bullet with: "**I4b** (since planned by ADR-0015, above)". New deferrals are in
ADR-0015's "Deferred" and go to `BACKLOG.md` at the gate.

## Beside I5

I5 (screens and media: the decoder scheduler, audio, the CRT shader, playlists, fallbacks) changes `rv_media`,
`rv_render` and the app's screens. The worlds' steps of I4b (1, 2, 5, 7) touch neither and can run in their own
worktree at any time. The app steps (3, 4, 6, 8) share files with I5 and are merged one at a time, each rebased on
I5's latest merge:

| Shared file or area | I4b's change | I5's likely change | How to merge |
|---|---|---|---|
| `src/app/src/world_session.cpp`, `retroverse_app.cpp`, `product_ui.cpp/.hpp` | the editor's draft, the preview's job, the deferrals | the decoder scheduler, audio, secondary TVs | small hunks; I4b keeps its code in new files (`world_edit*.cpp`, `world_regenerate*.cpp`) and calls them |
| `src/app/src/run_report.cpp` | `world.edits`, `world.regenerations` | the decoders' and audio's records | new records in their own functions; append, never reorder |
| `src/input/src/action.cpp`, the action map | `edit_pin`, `edit_rotate_left`, `edit_rotate_right` | perhaps volume or CRT preset actions | append at the end of the enum; the second to land rebases |
| `assets/ui/game_card.rml/.rcss` | "Sposta" | videos and manuals in the media viewer, perhaps on the card | one button each; RML merged by hand |
| `assets/ui/directory.rml`, new `edit_bar.rml`, `regenerate_preview.rml` | I4b's | none expected | |
| `src/app/src/screen_room.cpp`, `room_screen.cpp`, `game_screens*.cpp` | none (step 8 only checks that an expansion's CRT plays its games) | I5's | I4b does not touch them |
| `tests/smoke/CMakeLists.txt`, `scripts/gate.ps1` | `gpu.world_edit*`, Gate M3 | I5's cases and gate | new case files; one gate block each |
| `src/app/CMakeLists.txt`, `src/ui/CMakeLists.txt` | new sources | new sources | append; trivial conflicts |
| Docs: `docs/roadmap.md`, `BACKLOG.md`, `CLAUDE.md`, `docs/reference/running-the-app.md`, `cli.md`, `layout.md`, `docs/architecture.md` | I4b's sections | I5's | each iteration edits its own sections; `CLAUDE.md` once per gate |

---

## Risks and open questions

Decisions taken on the user's behalf on October 27, 2026, to be revisited:

1. **Overrides beside the plan.** `overrides.json` stays in `<user data>/worlds/<library id>/` (ADR-0010 §9), written
   by Retroverse under the world lock; `<user data>/config` stays read-only. Schema 2 adds provenance and fixture
   transforms; schema 1 is lifted.
2. **Every manual change is pinned** (style §13.4). Unpinning lets a later regeneration move the item again.
3. **The plan is the truth.** Edits change the plan; overrides only record intent for regenerations. The plan's
   schema stays 2; a placement carries no "pinned" flag.
4. **A light editor.** Games move only within their room chain (no cross-system moves, no hiding, no adding or
   removing items). Furniture: side modules, display fixtures and decoration, on a 100 mm grid, 90-degree turns (15
   for decoration), wall decoration along walls only; the hero corner, the hero-wall modules, walls and doorways stay
   fixed; no prefab choice in the app.
5. **Entering the editor** through the directory ("Modifica stanza") and the card ("Sposta"); no dedicated key. New
   actions: `edit_pin` (P, pad west), `edit_rotate_left/right` (Q and R, shoulders). The menu is Start or Enter.
6. **A draft saved as a replay of operations**, so a plan changed meanwhile is edited safely or refused with reasons.
   Live updates wait for the session's end.
7. **Regeneration scopes.** A room means its chain. A floor re-decides own and shared rooms and re-places its rooms
   (rooms on that floor move); systems change floors only with "Rigenera mondo". A walker in a regenerated room is
   put at its doorway.
8. **Consent.** "Rigenera" keeps every override (conflicts kept and reported); dropping needs "Rigenera e scarta le
   personalizzazioni" and a second confirmation. A conflicting pin is never removed silently.
9. **The pin wins** over the representative rule and the face-out priority.
10. **Snapshots** in `backups/snapshots/` (the last 20), paired plan and overrides, a journal for the two writes. Undo
    walks back through explicit actions only; no redo; incremental updates are not undoable.
11. **Expansion rooms** enter their floor like any room (the shorter row's east end), reached from the hall, signed
    with their head's sign and a number; no interior door to the head. A fresh split is into equal alphabetical
    shares; an update-created expansion has no face-out titles until its chain is regenerated.
12. **Generator 1.5 is minor**: the migration places capacity entries only. The reference library has none (I6 step
    4), so the real library proves only that nothing moves; expansions are proven on `esde/overflow`. The arcade's
    53-title limit is the likeliest real case.
13. **"Riorganizza scaffale"** (specification §7.6) is not a separate action; "Rigenera stanza" covers it. If the user
    wants a lighter action that only closes a shelf's gaps, it is a small step after I4b.
14. **No new golden.** The editor's bar and the preview are judged by screenshots; the goldens stay G01 to G18.

Risks:
- **Validation of moved furniture.** A free move can produce layouts the template never made (washers aimed at
  nothing, a module facing a wall). The hard rules are the judge; soft findings (the LayoutScore, the hero TV's line
  of sight) are shown, not enforced. If rooms end up looking wrong, restrict modules to their wall in a follow-up.
- **Growth and moved fixtures.** A fixture moved near the south wall stays put while the wall moves past it; the nav
  grid must still pass, or the growth is refused and offered as a regeneration.
- **Two writers.** The app and the CLI share the world lock; a long app session over a CLI edit relies on the replay.
- **Merge pressure with I5** in `world_session.cpp`, the run report and the input actions (table above).
- **Floor regeneration moves rooms**, which the walker and the cover cache notice; the reconcile path handles moved
  rooms since I6 step 5, but a GPU case must cover it.
