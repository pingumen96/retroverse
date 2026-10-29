# I4b step 6: regeneration and undo in the app

Roadmap row: I4b step 6. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §3, §5, §6, §9 (§5 and §6
amended on 2026-10-29 by this step). Plan: [plan.md](plan.md), risks 7, 8 and 10.

## What was built

- **The directory's "Modifiche" tab** (`ui::DirectoryModel`, `directory.rml`): a third tab beside "Piani" and
  "Preferiti e recenti" (the shoulders walk the three round; the tab bar's Left and Right stop at its ends), its rows
  "Modifica stanza" (while a room can be edited: step 3's command), "Rigenera stanza" (the room of the plan that holds
  the walker, `DirectoryBuilding::regenerableRoom`; in a hall the row says "Entra in una stanza per rigenerarla" and
  does nothing), "Rigenera piano" (the walker's floor), "Rigenera mondo" and "Annulla ultima modifica", each a command
  (`RegenerateRoom`, `RegenerateFloor`, `RegenerateWorld`, `Undo`) that closes the panel. A command refused for now (a
  floor on its way, a ride, a launch, another regeneration) puts the panel back with the notice `change-unavailable`.
- **The preview** (`ui::RegeneratePreviewModel` in `rv_ui_model`, `ui::RegeneratePreviewScreen` in `rv_ui`,
  `assets/ui/regenerate_preview.rml` and `.rcss`, hosted by `ProductUi` as a panel: the UI's input, Back's PanelBack
  level): computing while the dry run runs ("Calcolo di cosa cambierebbe...", Annulla), then the scope's name, the counts
  (rooms regenerated, new and removed, moved; games that change place; titles entering and leaving the display cases,
  games shown anew and no more, fixtures new, removed and moved: a line only when it is not zero, but the first two),
  the customizations kept and those a regeneration cannot honour (each the game's title or the fixture's role and its
  reason, the first five listed, "e altre N"), and ADR-0015 §5's three choices: **Rigenera** (only when something would
  change), **Rigenera e scarta le personalizzazioni** (only when the scope has customizations) and **Annulla**
  ("Chiudi" when there is nothing to do). The drop opens the second confirmation ("Scartare N personalizzazioni?",
  **Scarta e rigenera** and **Indietro**, Indietro focused: a button pressed twice drops nothing; Back returns to the
  preview). Committing shows no button; a failure says why (an action in progress, the world changed, unknown media, a
  plan that would break a rule, nothing to undo, a damaged snapshot) with Chiudi.
- **The undo's preview**: "Annulla ultima modifica" names the action it would undo (the saved edits or the
  regeneration, its room, floor or the building, its local time, read from the snapshots without the lock) with
  **Annulla la modifica** and **Indietro**, and says that it cannot be redone.
- **The jobs** (`rv/app/world_regenerate.hpp`, `world_regenerate_job.cpp` in `rv_app_catalog`, GPU-free):
  `takeRegenerationInputs()` (the catalog's building snapshot on the world reader and every system's selection, the
  world's configuration and data, one caching room-data loader: `RegenerationInputs`), `previewRegeneration()` (the world
  read with `world::readWorldUnlocked()`, nothing written, not even `world.lock`; the scope's selections taken as
  `worldplan regenerate` takes them; the frozen `regenerate()`'s dry run; a preview whose plan would break a hard rule is
  `invalid-plan`; `previewContentOf()` the panel's content), `commitRegeneration()` (under the world lock: the preview's
  own result when the plan's digest and the overrides read again are those it read and the overrides are kept, else the
  scope regenerated again with the consent's choice; `commitWorld()` action `regenerate`, scope the room, `floor:<id>` or
  `world`), `previewUndo()` / `commitUndo()` (the snapshot undo restores next, by `nextUndo()`; under the lock the undo
  is made only when it is still that snapshot, else `changed`), and `keepRoomItems()` (each system's room items written
  again from the snapshot for the new plan, a game placed anew with its title and cover, and the building stamp removed:
  the next start runs the building job).
- **In the app** (`src/app/src/world_regenerate.cpp`, `world_regenerate_state.hpp`): one job at a time on the pool. The
  catalog's inputs are kept in a slot per catalog refresh, read ahead while the directory shows "Modifiche" and waited
  for (or taken) by the preview. The commit's job also builds the floor shown from the plan stored after it (as a
  ride's destination); the main thread applies it through the live update's path (`planLiveUpdate()`: room by room when
  the floor's static part stands, else the floor committed again with the walker kept), then a walker whose room the
  scope covers (`regenerationCovers()`: a room's chain, a floor's rooms, every room) stands at its doorway, at the
  elevator when its room is gone (a floor's system moved into the shared room). While a preview shows or a commit
  runs, rides, the editor and live updates wait (`regenerating()`), lockstep runs wait for its jobs between frames.
- **The update's needs-confirmation** (`world_notice.rml`'s `needs-confirmation`): when a building result or a live
  update reports `needsConfirmation` (a growth a pinned fixture refused, a pinned room not migrated), the notice names it
  for 12 s and confirm (Enter, the pad's Start) in free roam opens the preview of "Rigenera stanza" of the first room its
  sentences name (`confirmationRoomOf()`). The `regeneration` notice (a room kept until a regeneration) now points at
  "Rigenera stanza" or "Rigenera mondo" in the directory instead of the CLI.
- **The run report's `world.regenerations`**: one record per preview asked (kind, scope, via, the preview's frame and
  time from the request, its job's, the inputs taken by it, the counts, the second confirmations shown, the choice, the
  commit: written, reused, snapshot, digest, its job's time, the room shown again from the choice, the path, the walker).

## Decisions beyond ADR-0015

- **A third tab** for the explicit actions, not more rows above the floors: "Modifica stanza" stays above the floors
  (step 3) and is also the tab's first row. The floors list keeps its place and its scripts.
- **The undo has a preview** (ADR-0015 §6 as amended): nothing brings an undone action back, so the panel first names
  it; it is made under the lock only when the snapshot restored is still the one named.
- **The commit reuses the preview's result** when nothing changed since it read the world and the overrides are kept;
  otherwise (the drop, another writer meanwhile) it regenerates the scope again under the lock with the consent's
  choice rather than failing (I4b step 5's note): the consent is to the scope and the choice.
- **The floor shown follows the stored plan through the live update's path**, which commits the floor again whenever
  a room's static part changed, as a regeneration's fixtures and dressing almost always do. Step 4's room-alone path is
  a follow-up (`BACKLOG.md`): within the acceptable 3 s on the fixtures and, by the figures below, on the reference
  library's floors.
- **The walker of a removed room** stands at the floor's elevator, its arrival point.
- **The items files and the building stamp**: written again and removed after every commit and undo, so that a game a
  regeneration placed anew shows its cover on every floor and the next start does not take the stamp for the plan.
- **The inputs read ahead** when "Modifiche" shows, kept until the catalog is refreshed: the building snapshot and the
  selections are most of a cold preview on the reference library (step 5's figures).
- **The notice opens the first room's preview**: an update's needsConfirmation sentences name rooms in their slot and
  fixture ids; the first that stands in the plan is offered.
- **The editor's menu keeps its own "Annulla ultima azione"** (the draft's), never the snapshot undo (ADR-0015 §6 as
  amended): an undo of a stored action from inside a session would pull the plan from under its draft.

## Measurements

Release, the reference machine, the GPU cases' runs (lockstep: from the request, on the main thread's frame, to the
frame that shows it; the job is the pool's share), the building fixture (7 rooms):

| What | Scope | Job | Shown | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|
| The preview | room:snes (40 titles, one pin) | 86 to 92 ms (the regeneration 81 to 85 ms) | 86 to 92 ms | 2 s |
| The preview | handheld floor (3 rooms decided again) | 133 ms | 133 ms | 15 s |
| The preview | the world (7 rooms) | 97 ms (waiting for the inputs read ahead; the regeneration 17 ms) | 97 ms | 15 s |
| The undo's preview | | 0.4 ms | 0.7 ms | |
| Rigenera's commit, the room shown again | room:snes | 91 ms | 162 ms (the floor committed again) | 3 s |
| Scarta e rigenera, the room shown again | room:snes | 98 ms | 167 ms | 3 s |
| Annulla la modifica, the room shown again | room:snes | 113 ms | 197 ms | 3 s |
| Rigenera piano, the floor shown again | handheld (2 rooms gone, 1 added) | 110 ms | 307 ms | 3 s |

Debug is about fifteen times slower (the room's preview 1.6 s, the room back 1.5 s after Rigenera). On the reference
library the commit is the step-4 Salva's figures plus the floor's build (step 3: 1.4 s on the computer floor) and its
commit on the main thread: the room-alone path is the follow-up that keeps a room's regeneration well within 3 s
there (`BACKLOG.md`); the real library's run is dry (below) and does not measure it.

## Proofs

- Unit (`tests/unit/ui/test_regenerate_preview.cpp`, 6 cases): names and ids; computing with Annulla alone, Back a
  cancel; the three choices, the drop's second confirmation with Indietro focused (pressed twice: back to the preview),
  Back from it the preview again, the confirmed drop committing with no button and Back ignored; Rigenera alone, nothing
  to change ("Chiudi"), only the drop; an undo, a failure, a late answer ignored, clicks on buttons not shown; the
  document's ids (each once), bindings, events and every key. `test_directory.cpp`, 2 cases: the "Modifiche" tab (the
  three tabs by the shoulders and the tab bar, its rows with and without "Modifica stanza", each command, "Rigenera
  stanza" in a hall doing nothing, Back closing) and its document. `test_run_report.cpp`: `world.regenerations` round
  trip.
- Unit (`tests/unit/app/test_world_regenerate.cpp`, 4 cases, the imported `esde/snes-room` planned by the building job,
  a game moved into a free slot by `worldplan edit move`): the inputs; the room's preview writing nothing (the world's
  folder byte for byte) with its counts (a pin kept, the drop's count); Rigenera committing the preview's own result
  behind a snapshot, the pinned game in its slot, the items written again and the stamp removed; the undo's preview
  (the regeneration, its room, its local time) and commit restoring the edited plan's SHA-256 and `overrides.json` byte
  for byte; the next undo the CLI's edit, a stale undo refused (`changed`, nothing written); a world changed since the
  preview regenerated again (both pins kept); the drop giving the building job's first plan back; the failures (no
  plan, nothing to undo, a journal: busy, a room the plan lacks, unknown media); `regenerationCovers()` and
  `confirmationRoomOf()`.
- GPU (`tests/smoke/test_world_regenerate.cpp`, 4 cases, the building fixture's room:snes with a game moved by the CLI;
  the walker walks into the room, M, the shoulders to "Modifiche"): `gpu.world_regenerate: a room regenerated with its
  pin kept, then undone to the plan's SHA-256` (the directory, the preview: nothing written, its counts, its time; then
  Rigenera: written behind a snapshot, `reused`, the pin kept, the walker teleported to the doorway; then "Annulla
  ultima modifica": its preview names the room, the commit gives back the edited plan's SHA-256 and `overrides.json`);
  `... "Rigenera e scarta le personalizzazioni" drops the room's pins only after the second confirmation` (a run that
  stops at the confirmation writes nothing; Back, the drop again, "Scarta e rigenera": no pin left, the plan the
  fixture's first); `... the world's preview, then Annulla, writes nothing`; `... a floor regenerated decides its rooms
  again and moves them; the walker of a room gone stands at the elevator` (the handheld floor planned with a room per
  system, then world.json's default threshold: pocketlab and pokemini into `area:shared:handheld`, gb's room kept and
  placed again; the walker, in pocketlab's room, at the elevator; the floor committed again).
- Real library (`tests/unit/app/test_world_regenerate_real.cpp`, `[real-library][regenerate-app]`): below.
- No golden changed and none added: no golden shows the directory or a panel of this step, G07's card is unchanged.

## Real library

`./scripts/real-library.ps1 -Filter '[real-library][regenerate-app]'` in Release (ES-DE closed; 0 differences among
542,120 entries): the library imported in 25.6 s, its building generated in 4.3 s (44 rooms on 9 floors, 19,985
placed). The app's jobs, nothing written (the world's folder byte for byte), the catalog unchanged since the generation
(nothing to change: no game moved, placed or unplaced, no fixture, no room moved):

| Preview | Read | Regeneration | Shown | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|
| The catalog's inputs (snapshot 968 ms, selections 252 ms) | | | 1,228 ms (read ahead by "Modifiche") | |
| room:snes (890 placed), the first | 412 ms | 1,384 ms | 1,806 ms; 3,034 ms if it takes the inputs itself | 2 s |
| floor computer (10 rooms, 5,314 placed) | 404 ms | 1,272 ms | 1,686 ms | 15 s |
| the world (44 rooms, 19,985 placed) | 393 ms | 1,015 ms | 1,877 ms | 15 s |
| room:snes again (its rooms' data cached) | 382 ms | 918 ms | 1,310 ms | 2 s |

A room's preview is within its 2 s once the inputs are ready, which is why "Modifiche" reads them ahead (the user's
move from the tab to a row and A takes longer than their 1.2 s on the pool in practice; a preview asked sooner waits for
them, and the record says so: `inputsTaken`). Its read (the plan of 19,985 placements read and parsed) and the data
of every standing room (the digest, the validation) are most of it, as step 5 found.

## Screenshots

`build/i4b/step6/` (the Release GPU cases' runs, 1920 x 1080, hidden, lockstep): `directory.png` (the "Modifiche" tab,
"Rigenera stanza" focused, its line naming the room), `preview_room.png` (room:snes: 1 room, 33 games moving, the pin
kept, the three choices, Rigenera focused), `preview_world.png` (the world: 7 rooms), `confirm.png` (the second
confirmation, "Scartare 1 personalizzazione?", Indietro focused), `regenerated.png` (the room just after Rigenera, the
walker at its doorway), `preview_undo.png` ("Annulla ultima modifica": la rigenerazione, Lumen 16 Home System, its
time), `undone.png` (the room after the undo), `dropped.png` (after Scarta e rigenera), `floor.png` (the handheld hall
after Rigenera piano, the walker at the elevator, gb's room and the shared room signed).

## Open issues

- The floor shown after a commit is committed again whenever a room's static part changed (a regeneration's fixtures
  and dressing almost always do); step 4's room-alone path would make it a room's cost (`BACKLOG.md`).
- The notice offers the first room its sentences name; the preview lists five conflicts and counts the others
  (`BACKLOG.md`).
- No GPU case drives the needs-confirmation notice (making an update refuse a growth needs a pinned south-wall fixture
  and enough new titles); its parsing and the scope's rule are unit-tested, the notice's document by
  `test_world_notice_document.cpp`.
- A room's preview on the reference library is 1.8 s of its 2 s with the inputs ready (above); reading the stored plan
  once per session instead of once per preview (0.4 s) would leave room.

## For step 8

- The directory's "Modifiche" names the walker's room by its id when the floors do not list it (an expansion: step 8
  groups chains); a room scope already covers its chain (`regenerationCovers()`, `regenerate()`).
- The preview's room title is its head's system's name; step 8's numbered rooms may want "(2)" there.
