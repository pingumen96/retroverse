# I3 step 9: persistence and the incremental update

Roadmap row: I3 step 9 (wave 2, needs 3 and 4; steps 8, 10a and 11 ran beside it in their own worktrees, and 1c, 7 and
13r of wave 1 were still open when it started). Decisions: [ADR-0010](../decisions/ADR-0010-world-generator-and-world-plan.md)
§3 (the stability of the display), §9 (persistence: the files under `worlds/<library id>/`, backups, the world lock,
where-is), §10 (the incremental rules), §11 (a plan that breaks a hard rule is never saved). The step fills the stubs
step 0a listed for it: `world_store`, `incremental`, `overrides` and `where_is` in `rv_world`, and `worldplan update` and
`where-is` in `retroverse-cli`; it gives `worldplan generate` the world lock and `--replace`, and checks every generated
and updated plan with step 4's validation.

## What was built

| File | What it holds |
|---|---|
| `rv/world/world_store.hpp`, `src/world_store.cpp` | `WorldFiles` (`worldFiles()`: `world-plan.json`, `world-plan.meta.json`, `overrides.json`, `backups/`, `world.lock`), `WorldLock` (LockFileEx, a wait, the temporary files of interrupted writes removed), `readStoredPlan()` (the plan and the file's digest), `PlanMeta` with `writePlanMeta()` and `readPlanMeta()`, `savePlan()` (backup, atomic write, sidecar; nothing written when unchanged) |
| `rv/world/incremental.hpp`, `src/incremental.cpp` | `GameAlias`, `UpdateInput`, `world::update()`: ADR-0010 §10's rules (below) |
| `rv/world/overrides.hpp`, `src/overrides.cpp` | `GamePin` and `FixturePin` (moved here from `validation.hpp`), `WorldOverrides`, `parseOverrides()`, `loadOverrides()`, `checkPins()` (`PinCheck`: orphaned, unhonoured, the pins to judge), `pinText()` |
| `rv/world/where_is.hpp`, `src/where_is.cpp` | `PlanIndex`: GameId to `GameLocation` (slot, fixture role and prefab, section; or the reason, the representative and where it stands), slot to GameId |
| `rv/world/generation.hpp`, `src/generation.cpp` | `GenerationInput::overrides`; `generationInputDigest()` (step 3's input digest, public); `ChangeReport`'s `fixturesRemoved`, `fixturesMoved`, `dressingRemoved`, `slotsFreed`, `regenerationReason`; a fresh plan's pins reported and judged |
| `src/generation_parts.hpp` (new, private) | what `generate` and `update` share: `checkGenerationInput()`, `catalogSlots()`, `validationContext()`, `uncheckedInputDigest()` |
| `src/plan_parts.hpp`, `src/plan_json.cpp`, `src/room_selection.cpp` (1a's and 2's, additions) | `sortedPlanDigest()` and `roomSelectionDocument()`: the canonical documents without the writers' check by the reader (decision 13) |
| `src/fixture_layout.hpp/.cpp`, `src/allocation.hpp/.cpp` (step 3's, refactored) | the rules the update repeats, exposed without changing a fresh plan: `sideModuleIn()`, `decorationIn()`, `wallWasherLights()`, `sectionOf()`, `bandOrder()`, `dressingProp()`, `setPlacementOffset()` |
| `tools/cli/src/cmd_worldplan_update.cpp`, `cmd_where_is.cpp` (0a's stubs), `cmd_worldplan_generate.cpp` (step 3's), `world_selection.hpp` (new, header only), `world_plan_text.hpp` | `worldplan update`, `where-is`, `generate --replace` under the world lock; the selection from a file or the catalog, the catalog's aliases, the change report as text and JSON, shared by generate and update |
| `tests/unit/world/test_world_update.cpp`, `test_world_store.cpp`, `test_overrides_where_is.cpp`, `world_update_support.hpp`, `tests/unit/cli/test_cli_worldplan_update.cpp`, `tests/fixtures/world/golden/update-*.json` | the tests (below) and three golden update sequences |
| `tests/unit/cli/test_cli_commands.cpp` | no worldplan subcommand or I3 command is a stub any more |

## Persistence

`<user data>/worlds/<library id>/`:

- `world-plan.json`: exactly `writePlan()`'s canonical text. `readStoredPlan()` returns the plan and the SHA-256 of the
  file (its digest); a text that is not the canonical text of a plan (edited by hand) is `ValidationFailed`, a newer
  schema `Unsupported` (step 1a's reader).
- `world-plan.meta.json`: formatted JSON, never hashed nor read to decide anything: `writtenAt` (UTC ISO-8601),
  `writer` (the subcommand and `core::versionBanner()`), `durationUs`, `planDigest`, the change report's summary
  (`outcome`, `rebuild`, `placements`, `added`, `kept`, `unplaced`, `fixturesAdded`, `fixturesRemoved`, `orphanedPins`,
  `needsConfirmation`, `digestBefore`), `schemaVersion` 1.
- `backups/`: before a plan is replaced, `core::backUpFile()` copies it there (`world-plan-<YYYYMMDDTHHMMSS.mmmZ>.json`),
  the newest 10 kept.
- `world.lock`: the world lock, an empty file whose first byte is locked exclusively with `LockFileEx`, as the catalog
  database's process lock is (opened with read and write sharing, never delete sharing). `WorldLock::acquire()` creates
  the folder and the file, tries again every 10 ms until its wait (2 s by default, `kWorldLockWait`) and answers
  `Conflict` ("another program writes this world ...") when another holder keeps it; the operating system releases the
  lock of a process that ends, a crash included. Once it holds the lock it removes the temporary files of interrupted
  writes (`<name>.tmp-<digits>-<digits>`) in the folder and in `backups/`, and nothing else.
- `savePlan(lock, result)` refuses a `RequiresExplicitRegeneration` result (`InvalidArgument`: nothing to save) and a
  plan that breaks a hard rule (`ValidationFailed`, ADR-0010 §11); writes nothing when the folder holds that text already
  (no backup, the sidecar untouched); else backs the old plan up, writes the new text with `core::writeTextFileAtomic`
  (flushed, then renamed) and then the sidecar. A sidecar that cannot be written leaves the plan saved and is reported
  (`SaveReport::metaError`). An interruption at any point leaves the previous plan or the new one, whole.
- Reading needs no lock; writers take it before they read the plan they will replace (`worldplan update`) or decide
  anything from the folder (`worldplan generate`). The data-directory guard runs before (retroverse-cli's dispatcher; the
  app's start, step 10b).
- `overrides.json` (`overrides.hpp`): JSON with comments, `schemaVersion` 1, optional `gamePins` (`game`, `slot`) and
  `fixturePins` (`fixture`, optional `prefab`); a game, a slot or a fixture pinned twice refused with its line, a newer
  schema `Unsupported`, a missing file no pin. Read, never written (I4's editor writes it).

## The incremental rules, exactly

`world::update(UpdateInput, WorldData)`: the library, the room's selection now, the configuration, the overrides, the
previous plan with its file's digest, and the aliases the catalog records for placed games its snapshot lacks.

1. **Refused**: a previous plan of a newer schema or generator version (`Unsupported`: a newer Retroverse wrote it), of
   another library, or not holding exactly the selection's room (`InvalidArgument`), and the inputs `generate` refuses.
2. **RequiresExplicitRegeneration**, the previous plan returned unchanged with `regenerationReason`: another schema,
   generator major version, world seed (`worldSeed(config, library)`), preset, template id or version, random algorithm,
   title-key rule, room seed, media family or theme pack; or a collection fixture whose levels (level, slots, section)
   are no longer its prefab's as measured now (a manifest edited without a template version). An older generator minor
   version and another catalog sort-key version are updated (the plan then records this program's).
3. **Every placement is kept** byte for byte, also a game's that is unavailable, without its cover, hidden or gone from
   the snapshot: a slot is never given to another game and free slots are never compacted.
4. **Aliases** (in GameId order): an alias whose identity has no placement hands it its slot (slot and offset kept: a
   texture swap, `NoRebuild` for it alone), else its slot is freed: a spine-out slot becomes reserve, a face-out one is
   dressed (a one-slot span with the level's prop), so the face-out set loses it until a regeneration.
5. **Representatives are chosen once**. A selection title is already represented when it is placed, when one of its
   versions (the selection's duplicates naming it) is placed (the lowest GameId of them), or when the previous plan
   listed it or a version as a duplicate of a placed game that is no title of the selection now (the representative's
   file gone missing, hidden, gone from the snapshot). Such a title is unplaced `duplicate-title` of the placed game, and
   its versions point at that game too. A title the previous plan left unplaced for `capacity` stays so until a
   regeneration (a fresh plan's titles past the limit would otherwise take the reserve at the first start).
6. **New titles** (every other title) are placed in the order (sort key, title key, GameId), each in the spine-out
   section only (the face-out set changes only with a regeneration), the section's fill order being the modules in the
   template's collection order, each module's spine-out levels in band order (step 3's), slots from 0:
   1. the first slot no title takes and no dressing covers after its predecessor's slot (the spine-out title just before
      it in that order; titles without keys in the selection, gone or unavailable, are not predecessors), else the first
      such slot of the section;
   2. with none, the first dressed spine-out slot in the same order (after the predecessor first), its level's dressing
      removed whole (`dressingRemoved`, `LocalReflow`);
   3. with none, a side-wall module in the next side-wall socket of the collection order that holds none at the room's
      growth (the decoration standing there and a lamp's light removed; the module placed by step 3's rule, every level
      dressed by its prop seed until titles take them; the wall-washers placed again over the modules as they stand,
      within the lights the others leave);
   4. with no socket left, one growth step (`RoomExpansion`): the room's bounds, every fixture of a south-wall socket and
      its lights moved south by the step, the step's side-wall sockets decorated as a fresh room's unneeded sockets are;
   5. at the template's limit the title is unplaced `capacity`.
   A new placement's offset comes from its placement seed, as in a fresh plan.
7. **Pins** (overrides.hpp): a side-wall socket whose standing fixture is pinned never takes a module (the module goes
   to the next socket), a growth that would move a pinned south-wall fixture is not taken (its titles are unplaced for
   capacity); each is a `needsConfirmation` entry, as are the pins the plan does not honour (never applied in I3); pins
   naming what no longer exists are `orphanedPins`. The validation judges the honoured and orphaned pins (a pin the
   previous plan already did not honour leaves the plan updatable); a fresh plan's validation judges every pin, so
   `generate --replace` never saves a plan that breaks one.
8. **The plan** records `kGeneratorVersion`, the selection's sort-key version and `generationInputDigest()` of the
   inputs read; an update of an unchanged catalog gives the previous plan byte for byte. The unplaced list is the
   selection's games that have no placement (duplicates re-pointed), the represented titles, the capacity ones.
9. **The change report**: `added` (new placements and slots handed over, sorted), `kept` (previous placements standing
   unchanged), `unplaced` (games newly listed), `fixturesAdded`/`Removed`/`Moved` against the previous plan (a socket
   whose decoration a module replaced is in both added and removed), `dressingRemoved`, `slotsFreed`, the pins, the
   rebuild level (`RoomExpansion` with a growth, `LocalReflow` when a placement, a freed slot, dressing, a fixture or a
   light changed, else `NoRebuild`), `digestBefore` and `digestAfter`.

`kGeneratorVersion` stays 1.0: the update's rules come with the first release of the generator (schema 1 is open until
Gate B), and fresh plans are byte-identical to step 3's (every golden plan reproduced).

## retroverse-cli

- `worldplan update --system <system> [--selection <file>] [--previous <plan>] [--seed <n>] [--library <id>]
  [--plan <file>] [--diff] [--json] [--output <file>]`: the selection from the catalog as `generate` makes it (with the
  aliases of placed games the snapshot lacks, `ICatalogStore::alias`) or from a file; the world's plan read and saved
  again under the world lock (`--previous` reads another, `--plan` writes elsewhere without backup or lock). Text: the
  heading ("Updated the plan of room:snes: ..." or "... is up to date: ..."), the outcome and rebuild, the counts, the
  pins, the digests; `--diff` adds every list (added, newly unplaced, fixtures added, removed and moved, dressing
  removed, slots freed, pins). `--json`: `plan`, `written`, `changes`, `summary` (`planSummaryJson`). Exit 0 updated or
  up to date; 1 nothing written (no plan, a refused plan: edited by hand or newer, an explicit regeneration needed with
  its reason, unknown media: "no update while the media are unknown", a system the catalog lacks); 2 usage; 3 the data,
  catalog, selection or overrides unreadable, a hard rule broken, the lock held by another program, a write failed.
- `worldplan generate` takes the world lock before it looks at the folder, reads the world's `overrides.json`, and
  saves through `savePlan()`; `--replace` regenerates over an existing plan after its backup ("Regenerated the plan of
  ...", "Backup: the previous plan kept as ..."). Without it, an existing plan still exits 1, now naming both
  `worldplan update` and `--replace`; `--replace` and `--plan` exclude each other.
- `where-is <gameId> [--plan <file> | --library <id>] [--json] [--output <file>]`: the world's plan (or the file) read,
  indexed and asked. Placed: room, slot (level, index), fixture (role, manifest), section and face-out or spine-out;
  unplaced: the reason, the representative and where it stands. `--json`: `game`, `room`, `plan`, `digest`, `placed`,
  `place`, `reason`, `representative`, `representativePlace`. Exit 0 answered; 1 no plan, or a game the plan does not
  hold; 2 usage (no GameId, a malformed one); 3 a plan that cannot be read or is refused.

## Tests

- `test_world_update.cpp` (`[world][update]`):
  - an unchanged catalog (0, 1, 15, 50, 56, 57, 890 and 2,000 titles): the plan byte-identical, `NoRebuild`, the
    digests equal;
  - NFR-08's property suite: 20 seeds with 15, 50, 890 and 2,000 titles in Release (2 seeds, 15 to 890 titles in Debug),
    each a fresh plan of a selection with up to 8 titles held back, then 12 random steps (5 in Debug): a held or new
    title arrives (a quarter of them favourites), a version of a placed title, a game without a cover, a title leaves the
    catalog or goes missing, a title becomes the favourite and most played. After each: every earlier placement
    byte-identical, the face-out set unchanged, the new placements spine-out and the report's, nothing that stood moved,
    the plan valid and read back as itself with the report's digest; a game added gives exactly one entry more (a
    placement or an unplaced one); a title arriving lands on its predecessor's level after it when that level had a free
    slot; leaving, going missing and priority changes move nothing;
  - across growth (3 seeds in Release, 1 in Debug): 3,200 titles (14 side-wall modules), then 5 batches of 120: the
    growth step taken once, `RoomExpansion`, the room 2 m deeper, only south-wall fixtures moved, one module more;
  - the order of the full section: 890 titles' reserve filled (no dressing touched, no fixture), then a dressed level,
    then every dressed slot, then a fourth module at e02 in place of its poster (the wall-washers placed again); 3,205
    titles' modules full, then a growth step with a module at w08 and decoration in the step's three other sockets;
    5,615 titles (16 m) full, then one unplaced for `capacity`, still so at the next update;
  - new titles spine-out while display slots are free (15 titles: the favourite on hw-w's eye-level level, the next one
    beside it); a new favourite moves nothing, a regeneration features it; G10 (10 titles added to G02's 50: the 10
    spine-out, the 50 face-out unchanged, golden);
  - missing, gone and uncovered games keep their slots; a better version arriving later is a duplicate of the placed
    one (its versions re-pointed); a duplicate whose placed representative went missing stays its duplicate; aliases
    handing a slot over and freeing a face-out slot (dressed); pins kept, two orphans reported, the module refused on
    pinned e02 and placed on w03, a pinned south-wall poster refusing the growth (3 titles unplaced for capacity);
  - RequiresExplicitRegeneration for another seed, preset, template version, generator major and changed prefab slots,
    the plan and digest kept; a newer generator or schema `Unsupported`; another library or no room refused;
  - every plan of 1,000 seeds at 15, 50, 890 and 2,000 titles valid on the manifests' slots (16 in Debug);
  - golden update sequences: `update-50-plus-10`, `update-890-plus-5-minus-3`, `update-3205-full-plus-20` (seed
    102938), written in Debug and reproduced in Release;
  - `[.bench]`: 2,000 titles plus one updated, `savePlan` into a world folder, `PlanIndex` of the result.
- `test_world_store.cpp` (`[world][store]`): the folder's names; a plan saved (sidecar read back, writer, time,
  duration, digests), saved again unchanged (nothing written, no backup), replaced after a backup; 13 saves leave the
  10 newest backups; writes interrupted after half their bytes and before the rename leave the previous plan readable,
  the next lock removes the temporary file (and a backup's, not another file's); a sidecar that cannot be written; the
  lock refused to a second holder after its wait, kept across a move, free once released; a missing plan, a hand-edited
  one (a line feed, a space), a newer schema refused; a kept or invalid result never saved.
- `test_overrides_where_is.cpp` (`[world][overrides]`, `[world][where_is]`): overrides read and sorted, 9 malformed
  files refused with their lines, a newer schema, a missing and a broken file; pins measured against a plan (honoured,
  unhonoured, orphaned) as the validation judges them; every placed and unplaced game of an 890-title plan answered,
  representatives with their slots, slots to games, a featured game's fixture.
- `test_cli_worldplan_update.cpp` (`[cli][worldplan]`): update of the world's plan (`--diff` text, the plan equal to
  `world::update`'s, every placement kept, the backup, the sidecar), then nothing to change (`--json`, nothing written);
  no plan, another seed, a hand-edited and a newer plan (exit 1, the plan untouched); `--previous` with `--plan`; usage;
  `generate --replace` after an update that featured nothing: the favourite face-out, two backups in order, the sidecar;
  the world lock held by the test refusing `retroverse-cli.exe` as a second process (exit 3 after the 2 s wait, nothing
  written), then generate and update by it; where-is (text, JSON, a duplicate with its representative's slot, a game
  not in the plan, usage, a refused plan); the catalog's selection of `esde/standard` imported: up to date, then the
  media root renamed away (exit 1, "no update while the media are unknown", the plan untouched), where-is for every
  placed game.

Results on the reference machine, with other worktrees building and testing at the same time: `msvc-x64-debug` and
`msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` passed 1,503 unit tests in Debug (1,285 s) and 1,503 in
Release (1,696 s, run beside the Debug build), the 28 new test cases of the step among them (the bench hidden). The
golden plans of step 3 are reproduced byte for byte after the refactoring; the update goldens written in Debug are
reproduced in Release.

## Figures

Release, the reference machine, other worktrees busy (informative: the gate measures on a quiet machine):

| Measure | Figure | Budget |
|---|---|---|
| `update` of 2,000 titles plus one (`[.bench]`, median of 21) | 38 to 52 ms over three runs (48.6 ms with the machine loaded) | 50 ms |
| of which the input digest and the plan's digest | about 10 and 7 ms (without the readers' checks; with them, as `generate` pays: 19 to 26 and 13 to 16 ms) | |
| of which the validation | about 4 ms | |
| `savePlan` of that plan into a world folder (the backup, the plan and the sidecar, each flushed) | 35 to 38 ms median | 50 ms |
| `PlanIndex` of that plan | 1.9 ms | well under 10 ms (ADR-0010 §9) |
| `generate` of 2,000 titles (step 3's bench, for comparison) | 56 to 61 ms | 200 ms |
| the update suite in Release | NFR-08's suite 73 s, the 1,000 plans 51 s, the rest under 5 s each | |

In Debug the property suite takes 9 to 16 s, the full-section test about 50 s, growth 17 s, the 16 plans' validity 6
to 9 s.

Before the readers' checks were left out of the update's digests, the update took 67 to 90 ms: the selection's
canonical document read back by its reader cost 14 ms of the input digest's 20 to 25, and the plan's check by its
reader half of its digest's 15 (decision 13).

## Decisions taken within the step

1. **The update takes a selection, not a snapshot.** `world::update` reads `selectRoom()`'s output, as `generate`
   does: one place decides eligibility, title keys and representatives, and the update reconciles them with what the
   plan holds (rule 5). Unknown media therefore update nothing: there is no selection (the CLI exits 1, the app keeps
   its plan, step 10c).
2. **A removed title keeps its slot.** ADR-0010 §10 keeps every placement, also of games that are unavailable; a game
   gone from the catalog's snapshot entirely (its gamelist entry and file gone) is kept alike: its identity may come
   back (ADR-0008's identities keep GameIds across moves), and only an alias frees a slot. The room then shows a slot
   whose game the catalog does not list (10b/12 draw it as a generated label or leave it empty: their choice).
3. **Capacity titles stay unplaced** until a regeneration (rule 5): otherwise the first update after a fresh plan at the
   template's limit would fill the reserve the plan had just kept with the titles it had just left out.
4. **Representatives across updates** come from the selection's duplicates and from the previous plan's duplicates
   (rule 5): the plan holds no title keys (ADR-0010 §8), so "the same title" is what the selection says now or what the
   plan said before, never a key the plan would have to store.
5. **A dressed level is taken whole**: its dressing removed when its first slot is taken, its other slots then reserve,
   so a level never holds titles and a prop at once (a face-out level's tail excepted, as in a fresh plan).
6. **The update's modules, decoration, dressing, lights and offsets are the fresh layout's own functions**
   (`fixture_layout`, `allocation`), refactored out of step 3's builder without changing a fresh plan, so that a module
   added by an update stands where a fresh plan of that size would put it.
7. **`inputDigest` names the last inputs read**: a plan updated from S to S' records S''s digest, as a fresh plan of S'
   does; the history is the meta sidecar's and the backups'. An unchanged catalog thus gives a byte-identical plan, which
   the app's start (10c) reconciles with 0 moves and no write.
8. **Pins are protected, never applied, in I3** (overrides.hpp): nothing writes them before I4's editor, and applying one
   would move a game or a fixture, which the ADR leaves to the editor's confirmation. A fresh plan judges every pin; an
   update only the ones it honours or that are orphaned (so a stale pin never blocks updates).
9. **Newer plans are refused, other versions regenerated**: a newer schema or generator version is `Unsupported` (exit
   1, nothing written), another major, seed, preset or template `RequiresExplicitRegeneration`; changed prefab slots are
   one too (a manifest edited without bumping the template version would otherwise leave placements in slots that no
   longer exist, which the validation would only report).
10. **The world lock waits 2 s**, then refuses (`Conflict`, exit 3): a writer never blocks the app's frame for long, and
    two writers of one world at once are a user's mistake to report, not to queue.
11. **`savePlan()` skips an unchanged text**: an update with nothing to change makes no backup, so the 10 backups keep
    the plans that differed.
12. **`GamePin` and `FixturePin` moved to `overrides.hpp`**, which `validation.hpp` now includes: `generation.hpp` needed
    `WorldOverrides`, and `validation.hpp` includes `generation.hpp` (the types are unchanged).
13. **The update's digests skip the writers' checks by the reader** (`plan_parts.hpp`: `roomSelectionDocument()`,
    `sortedPlanDigest()`; `generation_parts.hpp`: `uncheckedInputDigest()`): the same documents and digests, a third of
    the 50 ms budget saved. The update checks its selection's form itself first (`checkSelectionForm()`: what
    `readRoomSelection()` would refuse: titles out of priority order or twice, a stray representative, ...), the plan is
    validated, and `savePlan()` writes it through `writePlan()`, which keeps the check. `generate` is unchanged.

## Notes for later steps

- **10b, 10c**: read the plan with `readStoredPlan()` on the pool, show it, then on the snapshot `selectRoom()` and
  `update()` with `previousDigest` and the aliases of placed games the snapshot lacks (`ICatalogStore::alias`); commit the
  result at a frame boundary and, when `changes.digestAfter != digestBefore`, save it with `savePlan()` under
  `WorldLock::acquire(worldFiles(userDataRoot, library))` on the pool (a `Conflict` is logged and retried at the next
  reconcile). `RequiresExplicitRegeneration` keeps the previous plan and shows the notice with `regenerationReason`.
  `PlanIndex` is the where-is of the plan shown (15a's "Mostra nella stanza").
- **15b**: the change report says what to rebuild: `added` and `slotsFreed` (items), `dressingRemoved`,
  `fixturesAdded`/`Removed`/`Moved` (the static part), `RoomExpansion` (the shell); a slot handed over is in `added`
  with `NoRebuild` (a texture swap).
- **17b**: `worldplan update` after a second import of the real library must report `NoRebuild` and 0 added (the plan
  byte-identical, "is up to date").
- **`worldplan check`** (step 4's) still judges no pins: it could read `<user data>/worlds/<plan's library>/overrides.json`
  (`loadOverrides`, `checkPins`) into `ValidationContext::gamePins`/`fixturePins` in a few lines.

## For the merge

- **Order**: this step edits step 3's `generation.hpp`/`.cpp`, `fixture_layout.hpp`/`.cpp`, `allocation.hpp`/`.cpp`,
  `cmd_worldplan_generate.cpp`, `world_plan_text.hpp` (`defaultPlanPath` through `worldFiles`), step 1a's
  `plan_parts.hpp` and `plan_json.cpp` and step 2's `room_selection.cpp` (one function each, added), step 4's
  `validation.hpp` (the pin types moved out, an include added), the golden README, and
  `tests/unit/cli/test_cli_commands.cpp` (no stub left). 10a (`worldplan show --scene`) may touch `world_plan_text.hpp`
  too: keep both changes. No CMake list changed (the stubs were listed; the tests are globbed).
- **CLAUDE.md** (not edited by the step), "retroverse-cli": `worldplan update --system <system> [--selection <file>]
  [--previous <plan>] [--seed <n>] [--library <id>] [--plan <file>] [--diff] [--json]` updates the world's plan by
  ADR-0010 §10's rules under the world lock (the previous plan in `backups/`, the last 10; exit 1 when it needs
  `worldplan generate --replace`, is refused or the media are unknown); `worldplan generate --replace` regenerates over
  it after a backup; `where-is <gameId> [--plan <file> | --library <id>] [--json]`. "Layout and layering", `rv_world`:
  `world_store.hpp` (the world's files, the world lock, `savePlan`), `incremental.hpp` (`update`), `overrides.hpp`
  (pins, protected, never applied), `where_is.hpp` (`PlanIndex`) since I3 step 9. Tests: golden update sequences in
  `tests/fixtures/world/golden/update-*.json`.
- **docs/architecture.md**: the `world` row gains persistence, the incremental update, pins and where-is.
- **docs/roadmap.md**: step 9 done.
