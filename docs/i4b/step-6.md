# I4b step 6: regeneration and undo in the app

Roadmap row: I4b step 6. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §3, §5, §6, §9 (§5 and §6
amended on 2026-10-29 by this step and reworded by its review, below). Plan: [plan.md](plan.md), risks 7, 8 and 10.

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
  level): computing while the dry run runs ("Calcolo di cosa cambierebbe...", Annulla), then the scope's name, the
  counts (rooms regenerated, new and removed, moved; games that change place; titles entering and leaving the display
  cases, games shown anew and no more, fixtures new, removed and moved: a line only when it is not zero, but the first
  two),
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
  world's configuration and data, one caching room-data loader: `RegenerationInputs`), `previewRegeneration()` (the
  world read with `world::readWorldUnlocked()`, nothing written, not even `world.lock`; the scope's selections taken as
  `worldplan regenerate` takes them; the frozen `regenerate()`'s dry run; a preview whose plan would break a hard rule
  is `invalid-plan`; `previewContentOf()` the panel's content), `commitRegeneration()` (under the world lock: the
  preview's own result when the plan's digest and the overrides read again are those it read and the overrides are
  kept, else the scope regenerated again with the overrides kept, the drop refused: the review below; `commitWorld()`
  action `regenerate`, scope the room, `floor:<id>` or `world`), `previewUndo()` / `commitUndo()` (the snapshot undo
  restores next, by `nextUndo()`; under the lock the undo is made only when it is still that snapshot, else
  `changed`), and `keepRoomItems()` (each system's room items written again from the snapshot for the new plan, a
  game placed anew with its title and cover, since the review only the systems of the rooms that changed, and the
  building stamp removed: the next start runs the building job).
- **In the app** (`src/app/src/world_regenerate.cpp`, `world_regenerate_state.hpp`): one job at a time on the pool. The
  catalog's inputs are kept in a slot per catalog refresh, read ahead while the directory shows "Modifiche" and waited
  for (or taken) by the preview. The commit's job also builds what shows of the plan stored after it: since the
  review the rooms it changed alone (`rebuildRoomsAlone()`, `roomStaticPart()`), replaced in place on the main thread
  (`SceneInstance::replaceObjects()`, their collections applied), when the floor's other rooms and its hall stand as
  they did; else the floor shown (as a ride's destination), applied through the live update's path (`planLiveUpdate()`:
  room by room when the floor's static part stands, else the floor committed again with the walker kept). Then a
  walker whose area the scope covers (`regenerationCovers()`: a room's chain, a floor's rooms and hall, every room and
  hall) stands at its doorway, at the elevator when its room is gone (a floor's system moved into the shared room) or
  when it stood in a hall. While a preview shows or a commit
  runs, rides, the editor and live updates wait (`regenerating()`), lockstep runs wait for its jobs between frames.
- **The update's needs-confirmation** (`world_notice.rml`'s `needs-confirmation`): when a building result or a live
  update reports `needsConfirmation` (a growth a pinned fixture refused, a pinned room not migrated), the notice names
  the room for 12 s and confirm (Enter, the pad's Start) in free roam opens its "Rigenera stanza" preview: the first
  room of the change report's `needsConfirmationRooms` that stands (`confirmationRoomOf()`, since the review; the
  sentences were parsed before). The `regeneration` notice (a room kept until a regeneration) now points at
  "Rigenera stanza" or "Rigenera mondo" in the directory instead of the CLI.
- **The run report's `world.regenerations`**: one record per preview asked (kind, scope, via, the preview's frame and
  time from the request, its job's, the inputs taken by it, the counts, the second confirmations shown, the choice, the
  commit: written, reused, snapshot, digest, its job's time, the room shown again from the choice, the path (`room`,
  `in-place`, `floor`, `none`), the walker).

## Decisions beyond ADR-0015

- **A third tab** for the explicit actions, not more rows above the floors: "Modifica stanza" stays above the floors
  (step 3) and is also the tab's first row. The floors list keeps its place and its scripts.
- **The undo has a preview** (ADR-0015 §6 as amended): nothing brings an undone action back, so the panel first names
  it; it is made under the lock only when the snapshot restored is still the one named.
- **The commit reuses the preview's result** when nothing changed since it read the world and the overrides are kept;
  when another writer changed the plan or the overrides meanwhile, Rigenera regenerates the scope again under the lock
  with the overrides kept (I4b step 5's note), and the drop is refused (the review, below).
- **The floor shown follows the stored plan**: the rooms a commit changed rebuilt alone in place, step 4's path, when
  the floor's other rooms and its hall stand as they did (the review); else through the live update's path.
- **The walker of a removed room** stands at the floor's elevator, its arrival point; so does a walker in the hall of
  a floor whose rooms were placed again (the review).
- **The items files and the building stamp**: written again and removed after every commit and undo that changed
  something, so that a game a regeneration placed anew shows its cover on every floor and the next start does not take
  the stamp for the plan.
- **The inputs read ahead** when "Modifiche" shows (and when the needs-confirmation notice shows, since the review),
  kept until the catalog is refreshed: the building snapshot and the selections are most of a cold preview on the
  reference library (step 5's figures).
- **The notice opens the first room's preview**: the first room of the update's `needsConfirmationRooms` that stands.
- **The editor's menu keeps its own "Annulla ultima azione"** (the draft's), never the snapshot undo (ADR-0015 §6 as
  amended): an undo of a stored action from inside a session would pull the plan from under its draft.

## Measurements

Release, the reference machine, the GPU cases' runs of the review (lockstep: from the request, on the main thread's
frame, to the frame that shows it; the job is the pool's share), the building fixture (7 rooms):

| What | Scope | Job | Shown | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|
| The preview | room:snes (40 titles, one pin) | 90 to 110 ms | 91 to 110 ms | 2 s |
| The preview | handheld floor (3 rooms decided again) | 129 to 148 ms | 130 to 149 ms | 15 s |
| The preview | the world (7 rooms) | 101 ms (waiting for the inputs read ahead) | 101 ms | 15 s |
| The undo's preview | | under 1 ms | 1 ms | |
| Rigenera's commit, the room shown again | room:snes | 53 ms (the room alone 17 ms) | 73 ms (path `room`) | 3 s |
| Scarta e rigenera, the room shown again | room:snes | 90 ms (the room alone 22 ms) | 117 ms (`room`) | 3 s |
| Annulla la modifica, the room shown again | room:snes | 64 ms (the room alone 19 ms) | 88 ms (`room`) | 3 s |
| Rigenera piano, the floor shown again | handheld (2 rooms gone, 1 added) | 81 ms | 297 ms (`floor`) | 3 s |

Before the review the room was shown again 150 to 197 ms after the choice, the floor committed again each time. Debug
is about fifteen times slower (the room's preview 1.6 s, the room back 1.2 s after Rigenera). The reference library's
commits are measured below.

## Proofs

- Unit (`tests/unit/ui/test_regenerate_preview.cpp`, 7 cases): names and ids; computing with Annulla alone, Back a
  cancel; the three choices, the drop's second confirmation with Indietro focused above "Scarta e rigenera" (pressed
  twice: back to the preview), Back from it the preview again, the confirmed drop committing with no button and Back
  ignored; Rigenera alone, nothing to change ("Chiudi"), only the drop (Annulla focused); an undo ("Indietro"
  focused), a failure, a late answer ignored, clicks on buttons not shown; the first input frame's Activate ignored
  after a content, the hints' device; the document's ids (each once), bindings, events, every key and its column.
  `test_directory.cpp`, 3 cases: the "Modifiche" tab (the three tabs by the shoulders and the tab bar, its rows with
  and without "Modifica stanza", each command, "Rigenera stanza" in a hall doing nothing, Back closing), its selection
  following its action, and its document (the one-line descriptions, the floors by name, the chevron).
  `test_run_report.cpp`: `world.regenerations` round trip.
- Unit (`tests/unit/app/test_world_regenerate.cpp`, 7 cases, the imported `esde/snes-room` planned by the building job,
  a game moved into a free slot by `worldplan edit move`): the inputs; the room's preview writing nothing (the world's
  folder byte for byte) with its counts (a pin kept, the drop's count); Rigenera committing the preview's own result
  behind a snapshot (its plan the file's, nothing read back), the pinned game in its slot, the items written again and
  the stamp removed; the undo's preview (the regeneration, its room, its local time) and commit restoring the edited
  plan's SHA-256 and `overrides.json` byte for byte; the next undo the CLI's edit, a stale undo refused (`changed`,
  nothing written); a world changed since the preview regenerated again (both pins kept); a commit with nothing to
  change writing nothing (no items, the stamp kept); the drop refused over `overrides.json` changed alone since the
  preview; the drop giving the building job's first plan back; the failures (no plan, nothing to undo, a journal: busy,
  a room the plan lacks, unknown media); `regenerationCovers()` (halls of a regenerated floor or of the world);
  `confirmationRoomOf()` on the generator's own refusal (a south-wall fixture pinned, a growth refused: "socket ..."
  without the room, the room beside it); a late job answer refused by the mailbox; `roomsChangedAlone()`. World:
  `needsConfirmationRooms` in `test_world_update.cpp` and `test_world_migration.cpp`; `roomContentDigests(plan, rooms)`
  in `test_plan_json.cpp`.
- GPU (`tests/smoke/test_world_regenerate.cpp`, 5 cases, the building fixture's room:snes with a game moved by the CLI;
  the walker walks into the room, M, the shoulders to "Modifiche"): `gpu.world_regenerate: a room regenerated with its
  pin kept, then undone to the plan's SHA-256` (the directory, the preview: nothing written, its counts, its time; then
  Rigenera: written behind a snapshot, `reused`, the room rebuilt alone (path `room`), the pin kept, the walker
  teleported to the doorway; the preview again offering only the drop, Annulla focused; then "Annulla ultima
  modifica": its preview names the room, Indietro focused, the commit gives back the edited plan's SHA-256 and
  `overrides.json`, path `room`); `... "Rigenera e scarta le personalizzazioni" drops the room's pins only after the
  second confirmation` (a run that stops at the confirmation writes nothing; Back, the drop again, "Scarta e rigenera":
  no pin left, the plan the fixture's first, path `room`); `... the world's preview, then Annulla, writes nothing`;
  `... a floor regenerated decides its rooms again and moves them; the walker of a room gone stands at the elevator`
  (the handheld floor planned with a room per system, a fixture of each room that goes pinned, then world.json's
  default threshold: the crowded preview with its two `room-gone` conflicts photographed; pocketlab and pokemini into
  `area:shared:handheld`, gb's room kept and placed again; the walker, in pocketlab's room, at the elevator; the floor
  committed again, path `floor`); `... "Rigenera piano" from the floor's hall puts the walker at the elevator` (the
  eye in `hall:handheld` at the choice).
- Real library (`tests/unit/app/test_world_regenerate_real.cpp`, `[real-library][regenerate-app]`): below.
- No golden changed and none added: no golden shows the directory or a panel of this step, G07's card is unchanged.

## Real library

`./scripts/real-library.ps1 -Filter '[real-library][regenerate-app]'` in Release, the review's run (ES-DE closed; 0
differences): the library imported in 12.4 s, its building generated in 3.1 s (44 rooms on 9 floors, 19,985 placed).
The app's jobs, nothing written (the world's folder byte for byte), the catalog unchanged since the generation
(nothing to change: no game moved, placed or unplaced, no fixture, no room moved):

| Preview | Read | Regeneration | Shown | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|
| The catalog's inputs (snapshot 879 ms, selections 251 ms) | | | 1,138 ms (read ahead by "Modifiche") | |
| room:snes (890 placed), the first | 410 ms | 1,373 ms | 1,794 ms; 2,932 ms if it takes the inputs itself | 2 s |
| floor computer (10 rooms, 5,314 placed) | 395 ms | 1,215 ms | 1,620 ms | 15 s |
| the world (44 rooms, 19,985 placed) | 394 ms | 1,060 ms | 1,932 ms | 15 s |
| room:snes again (its rooms' data cached) | 392 ms | 949 ms | 1,353 ms | 2 s |

A room's preview is within its 2 s once the inputs are ready, which is why "Modifiche" (and the needs-confirmation
notice) reads them ahead (the user's move from the tab to a row and A takes longer than their 1.1 s on the pool in
practice; a preview asked sooner waits for them, and the record says so: `inputsTaken`). Its read (the plan of 19,985
placements read and parsed) and the data of every standing room (the digest, the validation) are most of it, as step 5
found. The inputs hold 224 MiB of private bytes (46 MiB before them, in the test's process: the snapshot's 105,130
entries of 192 systems and their selections): CPU memory, which the roadmap's 512 MiB row does not count (it counts
GPU memory, every texture, where the busiest floor takes 472 MiB: I6 step 6), so no acceptable limit is passed. They
are let go of once the directory closes with nothing pending (item 16 of "Review").

Then, since the review, the app's commits in that world (retroverse.exe of the build, hidden, lockstep, warm: the
building shown once first): a game of room:snes and one of room:amstradcpc (the computer floor's busiest room that
takes a move, 1,646 placed) moved by the CLI, then in the app:

| Commit | The commit's job | The room shown again after the choice | Path | Acceptable |
|---|---|---|---|---|
| Rigenera stanza, room:snes | 955 ms (write 720, the room alone 168) | 1,081 ms | `room` | 3 s |
| Annulla ultima modifica, room:snes | 986 ms (undo 356, the room alone 172) | 1,107 ms | `room` | 3 s |
| Rigenera stanza, room:amstradcpc | 1,111 ms (write 732, the room alone 298) | 1,309 ms | `room` | 3 s |
| Annulla ultima modifica, room:amstradcpc | 1,228 ms (undo 350, the room alone 347) | 1,442 ms | `room` | 3 s |
| Rigenera piano, gen4 | 1,097 ms (write 809, the rooms alone 201) | 1,258 ms | `room` | 3 s |

"Rigenera piano" of gen4 changed room:snes alone (its pin), so its rooms were rebuilt alone too; a floor whose rooms
move is built whole (the fixtures' 297 ms above; on the reference library the floor's build and commit, about 2 s by
step 3's figures, are not measured here). The first measurement of the review, before the costs below were cut, gave
2,696 and 2,886 ms after Rigenera and 3,249 and 3,515 ms after the undo, over the 3 s: the plan parsed under the lock
(0.4 s), every room's content digest three times (the whole plan's documents), every system's items files (0.4 s), and
the undo's commit waiting for the catalog's inputs the tab had asked for less than a second earlier (lockstep frames).

After the follow-up (the inputs let go of while the directory is closed), the same test also opens the room's preview
in the app a few seconds after the tab showed, as a player would, and checks that it found the inputs read ahead
(`inputsTaken` false) within 2 s. Its run on 2026-10-29 shared the machine with the other worktree's build and test
suite (the library imported in 40.7 s instead of 12.4, the read-only snapshots 50 to 60 s instead of 10): every figure
was about 1.5 to 2 times the quiet run's, the unchanged in-process previews included, so the 2 s checks failed on
time alone (0 differences):

| What, under the other worktree's load | Measured | The same, quiet (above) |
|---|---|---|
| The catalog's inputs | 2,215 ms | 1,138 ms |
| room:snes's preview again, in the test's process (warm) | 2,375 ms | 1,353 ms |
| room:snes's preview in the app, after the tab's read-ahead (inputs ready) | 2,413 ms | not measured |
| room:amstradcpc's preview in the app, after the tab's read-ahead (inputs ready) | 3,160 ms | not measured |
| Rigenera stanza / its undo, room:snes, the room shown again | 1,475 / 1,355 ms | 1,081 / 1,107 ms |
| Rigenera stanza / its undo, room:amstradcpc | 1,821 / 1,989 ms | 1,309 / 1,442 ms |
| Rigenera piano, gen4 | 1,447 ms | 1,258 ms |

The app's warm preview of room:snes costs what the warm dry run costs under the same load (2,413 against 2,375 ms):
it waits for no input, so the read-ahead keeps it warm after the release. room:amstradcpc (1,646 placed) is about 1.3
times room:snes's under the same load, which quiet would be about 2 s: its row is to measure again on a quiet machine
before the gate. The commits stay within their 3 s even under that load.

## Screenshots

`build/i4b/step6/` (the Release GPU cases' runs of the review, 1920 x 1080, hidden, lockstep): `directory.png` (the
"Modifiche" tab, End then Up three times: "Rigenera stanza" focused, the five rows whole with their one-line
descriptions, the undo's chevron), `preview_room.png` (room:snes: 1 room, 33 games moving, the pin kept, the three
choices, Rigenera focused, the keys' hints), `preview_drop_only.png` (the room again: nothing to change, the drop and
Annulla, Annulla focused), `confirm.png` (the second confirmation, "Scartare 1 personalizzazione?", Indietro focused
above "Scarta e rigenera"), `preview_world.png` (the world: 7 rooms), `preview_floor.png` (the handheld floor: rooms
new and gone, the fixtures, the body scrolled to its top with the conflicts below it, the choices and hints whole),
`regenerated.png` (the room just after Rigenera, rebuilt alone, the walker at its doorway), `preview_undo.png`
("Annulla ultima modifica": "Viene annullata: La rigenerazione", Lumen 16 Home System, "Del ... L'annullamento è
definitivo.", Indietro focused), `undone.png` (the room after the undo), `dropped.png` (after Scarta e rigenera),
`floor.png` (the handheld hall after Rigenera piano, the walker at the elevator, gb's room and the shared room signed).
`preview_drop_only.png` and `confirm.png` together show that a double-click on "Rigenera e scarta le
personalizzazioni" cannot land on "Scarta e rigenera": the drop's row (y 740 to 815 at 1080) is Indietro's in the
confirmation (738 to 820), "Scarta e rigenera" below it; the product UI also ignores a click within 0.5 s of the
buttons changing.

## Open issues

- A floor's or the world's regeneration that places rooms again builds the floor on the pool and commits it whole
  (`BACKLOG.md`); not measured on the reference library.
- The notice offers the first room of the update's; the preview lists five conflicts and counts the others, and a
  crowded preview's body scrolls by the mouse wheel only (`BACKLOG.md`).
- No GPU case drives the needs-confirmation notice (making an update refuse a growth needs a pinned south-wall fixture
  and enough new titles); the room it offers is unit-tested on the generator's own refusal, the notice's document by
  `test_world_notice_document.cpp`.
- A room's preview on the reference library is 1.8 s of its 2 s with the inputs ready (above); reading the stored plan
  once per session instead of once per preview (0.4 s) would leave room.

## For step 8

- The directory's "Modifiche" names the walker's room by its id when the floors do not list it (an expansion: step 8
  groups chains); a room scope already covers its chain (`regenerationCovers()`, `regenerate()`).
- The preview's room title is its head's system's name; step 8's numbered rooms may want "(2)" there.

## Review

The step's review (2026-10-29) found, and this step fixed:

1. **The needs-confirmation notice never appeared for a head room's refusal** ("socket w01" names no room, so
   `confirmationRoomOf()` found none). The change report now carries `needsConfirmationRooms` beside the sentences,
   filled where they are made (`incremental.cpp`, `checkPins()`); the notice takes the first that stands, names it
   ("Una stanza resta com'è per non spostare ciò che hai fissato." and the room), keeps the offer while it shows, waits
   for the build's own notice to hide, offers an identical refusal again at the next update, and reads the inputs ahead.
   The unit test uses the generator's own refusal. Seeding the inputs from the building job's snapshot is deferred
   (`BACKLOG.md`).
2. **A late answer could overwrite a newer one.** The jobs post through `JobMailbox`, which refuses a cancelled job's
   answer once the next job was asked (unit-tested); a job that throws answers `failed`; a commit not answering within
   120 s, or a panel committing with no commit, ends with a failure the panel can close.
3. **"Rigenera e scarta" could drop customizations the user never saw.** The drop is refused (`changed`, nothing
   written) when the plan or the overrides differ from the preview's (unit: `overrides.json` changed alone); Rigenera
   with the overrides kept still regenerates again. ADR-0015 §5's amendment says so.
4. **Focus defaults.** The undo's preview focuses "Indietro", a preview offering only the drop "Annulla"; Activate is
   ignored for the input frame after a content arrives; the second confirmation puts "Indietro" above "Scarta e
   rigenera" with the panel's bottom still, so a double-click's second press lands on Indietro (screenshots above), and
   a click within 0.5 s of the buttons changing is ignored.
5. **The view follows the stored plan whatever shows**: `adoptPlan()` (rides, where-is, the directory) and a live
   update when the floor cannot be shown; "written" kept apart from a plan that cannot be read back; the stamp removed
   whenever the undo wrote; `commitUndo()` without a plan followed by a live update.
6. **The Modifiche selection follows its action** across `setBuilding()` (unit-tested).
7. **A walker in a hall** goes to the elevator after its floor's or the world's regeneration (unit and GPU).
8. **The undo waits for the catalog's inputs**, so the items files are always written again.
9. **"Room back within 3 s warm" is now measured on the real library** (the tables above): the redundant read-back
   dropped (the regeneration's own plan, the commit's digest), the rooms a commit changed shown alone through step 4's
   path (`rebuildRoomsAlone()`, `roomStaticPart()`, `replaceObjects()`; an undo too), the plan parsed under the lock
   only when needed, the floor's rooms' digests alone, the items of the changed rooms' systems alone. The earlier claim
   ("within the acceptable 3 s ... on the reference library's floors") was unmeasured and is replaced by the figures.
10. **The preview's panel is a column**: its body scrolls, the choices and hints never clipped (`preview_floor.png`).
11. **"Modifica stanza" half scrolled off**: the rows fit the list (one-line descriptions, tighter rows), a focused
    first row scrolls the list to its top.
12. **Hints** "A Scegli · B Annulla/Indietro/Chiudi" with the pad's or the keys' glyphs, as the editor's bar.
13. **The undo's icon** is a back chevron.
14. **Descriptions** of at most 40 characters ("Riallestisce Console a 16 bit", "Disponibile fuori
    dall'ascensore", ...).
15. **Italian wording**: "Viene annullata: La rigenerazione", "Del ... L'annullamento è definitivo.", "una per volta",
    "va al piano che gli spetta", "un edificio nuovo", the drop's "vengono eliminate", the `invalid` reason,
    "riallestire" throughout, "(M o Select, scheda Modifiche)", "giochi e arredi" above the floors.
16. **Memory**: the panel's preview, inputs and undo's preview let go of when the panel ends or a commit ends. The
    slot's inputs are 224 MiB of private bytes on the reference library (the test's process: 46 MiB before): CPU
    memory, not the GPU memory the roadmap's 512 MiB row counts (every texture; the busiest floor's 472 MiB is the
    renderer's `gpuMemory`), so the two do not add up against that row and the step passes no limit. Held for the
    whole session they were too much to keep idle: since the follow-up the slot is emptied (`releaseIdleInputs()`) once
    the directory is closed with no directory command waiting, no notice offering a preview, and no preview, commit or
    undo asked or running (a job or a preview that took them keeps them through its `shared_ptr`); "Modifiche" and
    the needs-confirmation notice read them ahead again, so a preview opened from them stays warm. The run report
    counts the releases (`world.regenerationInputsReleased`). Unit: the slot emptied when not wanted, kept while
    wanted, a held preview unaffected, the next one making them again; GPU: the directory closed then reopened, one
    release, the world's preview finding its inputs read ahead (`inputsTaken` false).

Nits: the in-place path records the build and makes the showcase again as `applyLive()` does; a commit that changes
nothing writes no items, keeps the stamp and moves no walker; the smoke test asserts the path exactly; the note's long
lines; the CMake comment's wrap; the roadmap's I4b rows in line with the plan; `RegenerationInputs`' loader behind a
mutex (a cancelled job may still run beside the next one); the backlog's follow-ups.

No golden changed and none added.
