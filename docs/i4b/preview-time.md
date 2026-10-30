# I4b: the room preview's time

A small step after Gate M3's runs (branch `m3/preview-time`). Gate M3 measured "Rigenera stanza"'s impact preview on
the reference library at 2.65 to 4.10 s in the app and 2.33 to 2.83 s on the pool, against ADR-0015 §9's 2 s
(`docs/testing/gate-m3.md`, "The figures of the runs" and "Follow-ups"). This step brings it within the limit;
nothing a regeneration decides, writes or shows changed.

## Where the time went

A hidden bench on a building of the reference library's size (`app/bench: a room's preview in a building of 90,000
generated entries`, `[.bench]`, Release: 35 rooms, 34,777 placements, 47,452 unplaced) reproduces the real preview
(1.85 to 2.14 s). Laps in the preview of its largest room, warm:

| Part | Before | After |
|---|---|---|
| The plan's file parsed | 0.40 s | not parsed in the app (below) |
| The building's input digest | 0.60 s | 0.12 s |
| The new plan's digest | 0.38 s | 0.11 s |
| The validation | 0.15 s | 0.15 s (unchanged) |
| The previous plan sorted again, the plans' difference | 0.10 s | 0.02 s |

In the app the Gate's logs showed no queueing on the pool (the world's jobs are already its highest priority): the
preview was asked 1.3 s after the "Modifiche" tab (lockstep frames), while the tab's read-ahead of the catalog's
inputs was still running (its SQLite queries 1.0 to 1.6 s, slower beside the floor's cover loads), and the preview's
first dry run read every standing room's data cold (about 0.5 s on the real library).

## What changed

- **The digests' canonical text written straight from the plan and the inputs** (`core::CanonicalTextWriter`;
  `world::detail::writeSortedPlanText()`, `detail::writeBuildingInputText()`): the members in key order, the
  selections' titles and unplaced games (most of the text) without their documents, each room data's members written
  once as nested texts. The text is the document's byte for byte. A selection the reader's check would refuse
  (`detail::checkSelectionForm()`, the update's check since I3 step 9, now shared) or a string that is not UTF-8 goes
  through the document as before, which names the error or makes the same digest. The old document writer keeps its
  JSON pointers lazily (a string per value was most of a large write; the plan's parse re-writes the whole document
  to check it).
- **`sortPlan()` leaves a strictly ordered list as it is** (its only sorted order), and **`planDifference()`** works on
  sorted vectors instead of maps; the same lists in the same order.
- **The plan the app shows** (`app::KnownPlan`): the preview takes it, sorted, instead of parsing the file when it is
  the file's plan: its own digest, written straight from it (`world::sortedPlanDigest()`), is the file's SHA-256 (the
  review below). `readRegenerationWorld()` reads the world apart, so the app's preview job reads it before it waits for
  the inputs; a read that finds the world busy (an action writing it) is tried once more after the inputs.
- **The standing rooms' data read ahead** (`world::standingRoomRequests()`, `app::warmRoomData()`): the tab's read-ahead
  job loads them into the inputs' caching loader right after the inputs; a preview asked meanwhile shares that cache.
  The reading stops once the slot no longer holds those inputs (`app::slotHolds()`: let go of with the directory
  closed, a refresh of the catalog, the session's end). The loader does not merge requests in flight: a preview that
  asks for a room the read-ahead is reading at that moment reads it a second time (each read whole, the cache keeping
  the first; cold, the 44 real rooms take about half a second, some ten milliseconds a room).

Not done, and why:

- **No copy-on-write plan**: copying the whole plan takes about 5 ms; `regenerate()`'s frozen signature stays.
- **No input digest cached across previews**: written directly the digest is about 0.12 s, much of it SHA-256 of
  about 25 MB; a cache keyed by the inputs would save less than its invalidation risks.
- **No own worker for the preview**: it did not queue (above).
- **The validation and the catalog's inputs are as they were**: the validation judges the whole plan, as before; the
  inputs (the snapshot's queries) are the catalog's and are read ahead.

## Figures

The reference library (44 rooms, 19,985 placements), Release, `scripts/real-library.ps1 -Filter
'[real-library][regenerate],[real-library][regenerate-app]'`, four runs, all passed with 0 differences. The first three
had the machine fully loaded by processes of other sessions (CPU at 100%; the test's own snapshot took 1.5 to 4.8 s
against 1.1 to 1.4 s at Gate M3), the last one about a third loaded:

| Case (limit) | Gate M3 | After, loaded (three runs) | After, a third loaded | After the review, loaded |
|---|---|---|---|---|
| room:snes's preview in the app (2 s) | 3.27 and 2.65 s | 1.48, 1.69, 1.52 s | 1.19 s | 0.93 s |
| room:amstradcpc's preview in the app (2 s) | 3.61 and 4.10 s | 1.42, 1.81, 1.55 s | 1.53 s | 1.25 s |
| room:snes's preview on the pool, first | 2.33 and 2.83 s | 1.16, 0.66, 1.03 s | 0.61 s | 0.54 s (rooms read ahead) |
| room:snes's preview on the pool, warm | 1.68 and 1.75 s | 0.64, 0.86, 0.86 s | 0.76 s | 0.52 s |
| The CLI's dry run of room:snes | 1.73 and 1.69 s | 1.09, 1.63, 1.10 s | 0.86 s | 0.74 s |
| A commit or an undo, the room shown again (3 s) | 1.23 to 2.49 s | 1.43 to 2.34 s | 1.20 to 1.73 s | 0.97 to 1.62 s |

In the app the preview's own work (the world read about 0.1 s, the dry run 0.5 to 1.0 s, longer while it shares the
rooms' data still being read ahead) is most of it; the rest is the read-ahead's remainder (0.26 to 1.1 s in these runs:
the test asks 1.3 s after the tab shows). The first run predates the rooms' data read ahead (its dry run 1.0 s), the
second read them inside the inputs' slot (its wait 1.1 s); the last two read them as built.

The bench after this step: 0.91 to 0.99 s parsing the file, 0.55 to 0.61 s with the plan shown.

After the review (one more run, the same filter, passed, 0 differences; the CPU sampled at 100% during the run, other
sessions' processes, 19% just before it): the plan shown is checked against its file in 83 to 87 ms of each pool
preview on the reference library (its read 149 to 154 ms in all), 80 to 83 ms on the bench (its previews 0.55 s with
the plan shown, 0.76 to 0.77 s parsing the file). The test now reads the standing rooms' data ahead (409 ms) before its
pool previews, so its "first" is not cold: not comparable with Gate M3's first.

## Proofs

- The plan goldens (generation, the building and its update, the regenerations' "goldens unchanged" case) are byte for
  byte; none was rewritten.
- `world/regeneration: the digests written straight from the plan and the inputs are the documents'`: every golden
  plan's text written straight from it; every scope of the golden building and of the SNES room's golden plans, the
  catalog unchanged, grown and with a pin, compared with `detail::ScopedDocumentDigests` (the documents' path, as
  before): the input digest, both change-report digests, the plan's text, the impact and the overrides; the building
  generated and updated both ways; unplaced games out of order (the same digest) and a malformed selection (the same
  error).
- `core/canonical_json: the text writer ...`: the oracle's goldens and 2,000 seeded random documents written both ways;
  the writer's refusals (keys out of order or twice, misplaced values, unbalanced documents, malformed UTF-8, depth).
- `app/world_regenerate: a preview takes the plan shown when its file is that plan, in any order; ...`: the same
  preview, plan and impact from the file, the plan shown, the plan shown with its lists reversed, another plan's
  digest (the file read), the world read apart, the rooms' data read ahead (and stopped).
- `app/world_regenerate: a preview never takes the plan shown when it draws the stored lobby's rooms on the hub` (the
  review): below.
- `core/canonical_json: the text writer refuses ...`: a value given through `value()` counts its levels from the
  writer's; a top-level array given through it is written on one line.

## The review

- **The plan shown is not always the plan as stored (high).** `buildFloor()` draws a stored building's ground-floor
  rooms that stand at I4's lobby on the hub (`world::placeOnHub()`, nothing written) and keeps the stored digest, and
  the editor's `asShown()` does the same after a save; the app keeps that plan (`keepPlan()`) and gave it to the
  preview as `KnownPlan` under the stored digest. The preview then ran on the rooms as drawn, and a commit, which in
  Release reuses the preview's own result, would have written their re-placement: against ADR-0015 §5 and §6 and "an
  update never moves a placement". A Debug-only comparison with the parsed file hid it (it logged an error and took
  the file, which no test read). Fixed by proving the plan instead of trusting the digest it was kept with: the
  preview takes the plan shown only when its own canonical text (sorted, written straight from it:
  `world::sortedPlanDigest()`) has the file's SHA-256, so the plan taken is the file's byte for byte; otherwise the
  file is parsed (logged). Capturing the stored plan where it is drawn was weighed and left: the plan is drawn in
  five places (`buildFloor()`, `rebuildRoomsAlone()`, a commit's floor rebuilt alone, the editor's two `asShown()`)
  and handed on through floor templates and edit sessions with their digests changed, so one place missed would
  bring the fault back silently, and keeping the plan as stored beside the plan shown would hold the whole plan twice
  for the session. The check holds nothing between previews: its sorted copy lives for the preview, as the parsed
  plan did; its time is in the figures above. The Debug comparison is dropped: the digest check is the same proof, in
  every build. Its test builds the `KnownPlan` as the app does (the floor of a stored plan whose ground-floor rooms
  stand at the lobby, `previewWorld()`) and asserts that the previews are the file's: the ground-floor room's (plan,
  regeneration, impact, panel) and `room:snes`'s refusal (invalid-plan on the file's plan, the lobby's room inside the
  hub, where the plan as drawn gives a preview a commit could write).
- **The read-ahead stops** when its slot no longer holds the inputs (`RegenerationInputsSlot::retired`, set when a
  refresh of the catalog makes a new slot and at the session's end; the slot emptied by `releaseIdleInputs()`).
- **A busy read** of the world, now made before the wait for the inputs, is tried once more after it.
- **`CanonicalTextWriter::value()`** counts the nested value's levels from the writer's (the 64 levels of the
  document's path); a top-level array given through it is written on one line (documented: such an array goes
  through `beginArray()`).
- **The two digest paths**: each document builder (`documentJson()` and `roomJson()` of the plan, `titleJson()` and
  `documentJson()` of a selection, `buildingInputDocument()`) names its direct writer to mirror, and back.
  `buildingInputDocument()` and `writeBuildingInputText()` are file-local to `generation.cpp` now: a test comparing
  their texts would need the private `BuildingDigestInput` made public, and the comparison test already compares
  both paths' digests (the SHA-256 of the texts) on every world fixture.
