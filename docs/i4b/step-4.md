# I4b step 4: the editor in the app, furniture

Roadmap row: I4b step 4. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §3, §4 (with its I4b step 3
amendment), §9. Plan: [plan.md](plan.md).

## What was built

- **Furniture as targets** (`rv/app/world_edit_room.hpp`): the edited room's movable fixtures (`world::
  editableFixtures()`, new in rv_world: side-wall modules, display fixtures and decoration, each with its box as the
  validation judges it, `world::fixtureBox()`, and the refusal of one a kept room holds) are targeted by the reticle
  where no game or slot is (`EditRoom::fixtureAt()`: the nearest box within 4 m); the floor point under the reticle and
  the wall point (`floorAim()`, `wallAim()`, in the room's frame) are what a held fixture follows; `fixtureMove()` is the
  move it is drawn with.
- **The interaction's Moving mode** (`EditInteraction`, GPU-free): interact on a fixture picks it up, held where the
  reticle took it (its offset from the floor or wall point kept, so it never jumps); it follows the reticle on the
  room's 100 mm grid through its home (`world::snapFixture()` on the draft: the session's placer), a poster along the
  walls only; `edit_rotate_left` and `edit_rotate_right` (new actions, appended after `edit_pin`: Q and R, the pad's
  shoulders, which the Edit context did not use) turn it a quarter turn, decoration 15 degrees, a poster never (its
  wall turns it); the D-pad and the arrows nudge it one grid step along the room's axis nearest the view's (forward,
  right; a poster along its wall) until the player looks or walks; each new place is a **probe**; interact drops it (a
  move-fixture operation; refused, it stays in the hand), where it stood puts it back, as Back does; `edit_pin` pins or
  unpins a targeted fixture in place; a fixture of a kept room is refused at once (`fixture-in-kept-room`).
- **Probes** (`world_edit.cpp`): each new place of a held fixture, and each slot or game a held game rests on, is
  judged on the pool by `applyEdit()` on a copy of the draft (its own job box beside the session's job, the latest
  asked waiting for the one in flight; lockstep runs wait for it); the verdict colours the held fixture's outline (the
  hover's warm white where it may drop, the bar's red, `#C8645A`, where a rule refuses it: `RenderView::outline` set by
  `WorldSession::applyInspection()`) and the bar names the rule; a drop where the last probe judged, on the same draft,
  shows the probe's edited world instead of applying the operation again.
- **The held fixture drawn where it would stand**: its scene items (its own objects, its pack's pieces, its dressed
  spans, the picture hung over it: `SceneInstance::itemsOf()` through the host) left out of the view and drawn moved,
  its games hidden in their slots and drawn moved, all outlined.
- **The room's static part rebuilt alone** (ADR-0015 §4 as amended): an edit whose static part changed (a fixture moved,
  dressing giving way where a game lands: `LocalReflow`) no longer builds the floor again. The job makes the room's part
  of the floor's scene (`world::roomFloorScene()`, new: `toFloorSceneFile()`'s per-room part, which it now calls, so no
  scene golden moved; `app::roomStaticPart()` moves it into the scene's frame and reads it back) and lays its collection
  out again; the main thread replaces the room's objects and lights in place (`SceneInstance::replaceObjects()`: an object
  equal to the one it replaces keeps its render items, colliders and bounds, the others are built and registered as a
  scene's are; the render items, the collision world, the object bounds, the debug lines and the lights follow in the
  description's order, the eye's area choosing its lights again; `FreeRoam::setCollision()`, the walker where it
  stands), then applies the room's collection in place (its occluders and pick targets follow the furniture). Wall-washers
  and a lamp's light come with the room's lights (rv_world places them again at the edit). Step 3's moves into a dressed
  slot take the same path. The run report's `path` is `room` for it; `world.edits` counts the `probes`. While a session
  shows its drafts, the plan's derived state (the plan state and its where-is index, the card's versions, the launch's
  placements) waits for its end (`keepShown()`, `endEdit()`).
- **The bar** (`edit_bar.rml`): the mode `moving`, a fixture targeted ("Mobile", "Vetrina", "Poster", "Lampada",
  "Tappeto", "Decorazione"), held ("Sposti: un mobile"), the place's verdict ("Si può posare qui.", "Verifica del
  posto...", "Qui non si può posare." with the rule's sentence), a sentence for each rule a fixture's move can break
  (another fixture, the doorway, a door-reserved wall, outside the room, a module or the TV cut off from the 1.2 m
  passage, the room's entrance, the floor, off its wall, the grid, the turn step, fixed furniture, a kept room, the
  room's lights) and the keys (Q R / LB RB "Ruota", "Sposta di 10 cm").
- **Dressed free slots from afar** (step 3's note): a held game's ray passes the dressing props that give way where it
  lands (`EditRoom::heldIndex()`: the occluders but the dressing's, `CollectionLayout::dressingOccluders`, outside the
  canonical document).

## Decisions beyond ADR-0015

- **Fixtures are targeted where no game or slot is**, by the reticle only (not by the D-pad's steps): a shelf full of
  games is taken by its frame, side, top or plinth. Their reach is 4 m (games keep 2.5 m).
- **Held where it was taken**: the offset between the pivot and the reticle's floor (or wall) point at the pick-up is
  kept, so the fixture does not jump to the reticle.
- **D-pad nudges** of one grid step along the room's axis nearest the view's, so that the pad and the keyboard place
  furniture exactly (and scripted runs drive it); the reticle takes it back once the player looks or walks.
- **`edit_pin` on a fixture** pins or unpins it in place (`pin-fixture`, `unpin-fixture`), as on a game.
- **The verdict while held is a probe**: `applyEdit()` on a copy of the draft for each new place, on the pool, the
  outline's colour the last known verdict of that place (warm white where it stood). A drop is applied whatever the
  verdict (a refusal names the rule, the fixture stays in the hand, outlined red).
- **A held game's slot is probed too, and a drop takes its probe's world**: the same `applyEdit()` with the same
  operation on the same draft, done ahead while the player aims (on the real library's floor its validation takes 150
  to 180 ms). A drop before its probe answers applies the operation itself, as in step 3.
- **The plan's derived state waits for the session's end**: made again at each edit it cost 117 ms on the real
  library's main thread; the directory, the quick catalog, rides and launches, which read it, wait during a session.
- **The scene's digest of a rebuilt room** is synthetic (`edit-scene:<session>:<n>`), the base's again after an undo
  to the base or a discard, and stays so after Salva: the next live update after a refresh of the catalog commits the
  floor once (walker kept). Building the saved floor to record its exact digest took 1.6 s on the real computer floor,
  over Salva's 3 s with the save itself (`BACKLOG.md`: per-room digests).
- **Replaced objects' meshes and materials stay registered** until the floor is committed again (the registries only
  grow; equal keys share): a session moving furniture adds a few.

## Measurements

Release, reference machine, lockstep runs (from the request to the frame that shows it; the job is the pool's share:
with a drop where its probe judged, the room's static part and its collection laid out again). Runs while another build
loaded the machine took up to 0.42 s for a move and 3.4 s for a save; the figures below are the unloaded runs'.

| Edit | Room | Path | Job | Shown | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|---|
| A poster slid, a cabinet moved | building's area:arcade (ground floor, the hub) | the room's static part alone | 6 to 7 ms | 15 to 21 ms | 0.25 s |
| A move into a dressed slot (step 3's) | building's room:snes (gen4) | the same (was the floor: 85 to 88 ms) | 7.5 ms | 15 to 18 ms | 0.25 s |
| A module moved | overflow's amiga room, 1,561 big boxes (computer floor) | the same | 15 to 21 ms | 62 to 85 ms | 0.25 s |
| A move into a dressed slot | the real library's area:shared:computer (computer floor, 5,314 placed, 19,985 in the building) | the same (was the floor: 2,046 ms) | 32 ms | 153 ms | 0.25 s |
| A module moved | the real library's room:amstradcpc, 1,646 placed (computer floor) | the same | 39 ms | 159 ms | 0.25 s |

The room's objects are replaced on the main thread in 0.5 to 1.6 ms (`SceneInstance::replaceObjects()`: one object
built, the room's others kept); the rest of the main thread's share is the room's collection applied in place and the
cover loader started again (step 3's). Before the probe's world was reused and the plan's state deferred, the real
library's rows were 398 ms (the dressed slot) and 495 ms (the module): `applyEdit()` 150 to 180 ms on the pool,
the plan's derived state 117 ms on the main thread. A drop before its probe answers still applies the operation (on the
real floor about 0.15 s more). Salva: 66 ms in the arcade room (the room back 109 ms after it), 131 to 220 ms in the
amiga room (215 to 307 ms), 2.74 s on the real computer floor (the room back 2.80 s: reading, snapshotting and writing
a plan of 19,985 placements, the pool shared with the cover loader); acceptable 3 s warm. Esci senza salvare's room back:
125 ms on the real floor. Debug is about ten times slower (the arcade's moves 143 to 146 ms, its save 671 ms).

## Proofs

- Unit (`tests/unit/app/test_world_edit_furniture.cpp`, 7 cases): the room's movable fixtures as targets (never the
  hero corner nor the hero-wall modules; their boxes the validation's), `fixtureAt()`, the floor and wall points,
  a south-row room's frame, `fixtureMove()` (a quarter turn about the pivot, in a turned frame too); the interaction:
  picked up where no game is, held where taken, turned left and right, nudged along the view's axes (facing north,
  facing east), following the reticle again once the player looks (on the grid through its home), a probe per new
  place and its verdict (a verdict about another place says nothing), dropped (refused: still in the hand), put back
  (Back, or interact where it stood), the menu letting go of it, the reticle off it targeting nothing, `edit_pin` on a
  fixture; decoration turned by 15 degrees, a poster never turned and nudged along its wall only; a kept room's
  fixtures refused at once (`fixture-in-kept-room`); drops through the draft refused in the doorway's clear area
  (`doorway-clearance`), a door-reserved socket (`door-reserved`), over another display case (`footprints-overlap`),
  closing the 1.2 m passage (a target more than 1.2 m from the case cut off: `unreachable`), cutting a module off (a
  case right in front of it: `unreachable` naming it), an accepted drop kept and pinned; a room's static part rebuilt
  alone equal, object for object, to the room's part of the floor's scene of the edited plan (the case moved, the shell
  as it was); a held game's ray reaching a dressed free slot the browsing ray cannot (the dressing occludes it); a held
  game's move onto a free slot and swap with another game judged ahead, once per slot.
  `test_action.cpp`, `test_action_map.cpp`, `test_input_system.cpp`: the two actions appended, their bindings;
  `test_run_report.cpp`: `probes` and the path `room`; `test_edit_bar_document.cpp`: the mode `moving` and a sentence
  for every rule (24).
- GPU (`tests/smoke/test_world_edit_furniture.cpp`): `gpu.world_edit: a poster and a side-wall module moved and saved,
  a refused drop, the room walked after` on the building fixture's arcade room (room:snes has no side-wall module):
  the test plans where the walker stands and looks (a poster and a cabinet seen where no game is), the poster slid
  along its wall and dropped (path `room`), the cabinet nudged to a place it may drop, further to one a rule refuses
  (dropped: refused, the bar's sentence, path `none`), back and dropped (path `room`), Salva; the stored plan holds both
  where they were put with two fixture pins (origin editor, a transform each), every placement as it was; a start on
  the saved plan writes nothing, and its walker, walking from where it stood into the cabinet, stops against it where
  it stands now (0.38 m from its box's corner in Release: the capsule's 0.3 m, the cabinet's colliders a little inside
  its box). Step 3's GPU cases now see the path `room` for
  their moves into a dressed slot. Hidden: `[.bench][furniture]` (the amiga room above),
  `[.real-library][edit][furniture]` (below).
- No golden changed: the Release GPU run compared G01 to G18 (all pass); the floor and plan scene goldens are unchanged
  (`toFloorSceneFile()` calls `roomFloorScene()`).

The real library's rows ran through `./scripts/real-library.ps1 -Measure` (ES-DE closed, 0 differences among 542,120
entries, four runs) with the hidden cases `[real-library][edit]`: step 3's move into a dressed slot, and a side-wall
module of the computer floor's busiest room that has one moved a few grid steps and saved, each library imported into
a data directory under `build/msvc-x64-release/real-library/`.

## Screenshots

`build/i4b/step4/`: `holding.png` (a cabinet held where it may drop, outlined warm white, its game's screen moving with
it, the bar "Sposti: un mobile · Si può posare qui."), `refused.png` (the cabinet over its neighbour, outlined red, the
bar "Qui non si può posare." and "Lì c'è già un altro mobile."), `saved.png` (the room just after Salva, the cabinet
out from the wall, lit as before), `walked.png` (the walker stopped against the cabinet), `bench.png` (the amiga room's
module moved out).

## Open issues

- A held fixture's lamp light and the wall-washers move only at the drop (the room's static part rebuilt); while it is
  held its light stays where it stood (`BACKLOG.md`).
- Fixtures are targeted by the reticle only; the D-pad's steps reach games and slots (`BACKLOG.md`).
- After a session whose static part changed, the floor shown keeps a synthetic scene digest: the next live update after
  a refresh commits the floor once (2 s on the real computer floor, the walker kept); per-room digests would make it a
  room's work (`BACKLOG.md`).
- Salva on the real computer floor takes 2.7 s of the 3 s row: the plan's read, snapshot and write (19,985
  placements), the pool shared with the cover loader.
- Replaced objects' meshes and materials stay registered until the floor is committed again (`BACKLOG.md`).
- A fixture dropped onto the walker leaves the walker inside it until its next step pushes it out.
- The outline is the hover's 2 px line: on a dark cabinet the red reads mostly through the bar's sentence.

## For step 6

- **A regenerated room shown alone.** "Rigenera stanza" keeps a room's id, x and yaw; when its depth (so its place on
  the floor and the hall) is unchanged too, its static part can be shown through the same path
  (`roomStaticPart()` on the floor shown, the host's `replaceObjects()`, then the room's collection in place) instead
  of committing the floor; otherwise commit the floor. An undo that restores one room likewise.
- **The scene digest after it** is synthetic (as after Salva): the next live update commits the floor once; a
  regeneration that commits the floor anyway has its exact one.
- **The plan's state**: `keepShown()` defers the plan's derived state while an edit session runs; a regeneration's
  preview or commit outside a session makes it as before.
- `world_edit.cpp`'s `displayInto()` is the place: `LocalReflow` rooms on the floor shown, each one's scene data kept by
  the session's floor (`templateOf()` keeps only the edited room's).

## For step 8

- **The editor within a chain.** A move from a head to its expansion names both rooms (`EditResult::rooms`): the
  session's floor (`templateOf()`) must keep the scene data of every room of the chain, or `roomStaticPart()` fails and
  the floor is built again; the targets (`EditRoom`) are one room's today.
- An expansion's fixtures are its own room's (`editableFixtures()` takes one room).
