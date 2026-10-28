# I4b step 5: the regeneration in rv_world and `worldplan regenerate`

Roadmap row: I4b step 5. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §3, §5, §8, §9. Plan:
[plan.md](plan.md).

## What was built

- **`world::regenerate()`** (`rv/world/regeneration.hpp`, `src/world/src/regeneration.cpp`, new): a room (its chain),
  a floor or the world of a stored plan generated afresh as a fresh building generates it, a pure function of its
  inputs. A **room** keeps its id, kind, floor, x, yaw, sections and seed; its data is a new room's of its kind on its
  floor (the template's current version, the family's last successor), so a `room-kept` room comes back current and
  its face-out titles and representatives are chosen again. A **floor**'s systems (the sections of its rooms, and the
  selections' systems no room holds whose classification names it) enter it afresh (`enterBuilding()` with the other
  floors' rooms standing): own or shared rooms by today's counts, the arcade room on the ground floor, the rooms placed
  again from the rows' start; a standing system stays on the floor whatever its classification says now. The
  **world** is `generateBuilding()`'s rules over the same detail functions. Rooms outside the scope stand as they are.
- **Honouring the overrides** (ADR-0015 §3's four rules): the private `pinned_room.cpp` generates a room around its
  pins: a pinned game takes its title's place when another version represents it now (the old representative
  unplaced as its duplicate, which the update's rule then keeps); the side-wall sockets of pinned slots and pinned
  modules always take their module (the room grown to a growth step's socket when one holds them); the layout
  (`fixture_layout.cpp`, `LayoutPins`) stands the pinned fixtures first, at their transform or in their socket with
  their prefab (a decoration's as it stood), decorates a pinned decoration's socket whatever the density draws, gives a
  pinned decoration's side-wall socket no module, and skips a socket whose new fixture would overlap a moved pinned
  fixture (the module to the next socket); the wall-washers light a moved module where it stands. The allocation
  (`allocation.cpp`, `AllocationInput::reserved`) places the pinned games in their slots first; the face-out choice and
  the alphabetical fill skip those slots and games (a face-out level fills its other slots from the left, its free
  ones dressed; a spine-out level's quota counts its pinned games). Without pins every path is the old one.
- **Conflicts** (`OverrideConflict`, by pin): `room-gone`, `slot-gone`, `game-gone`, `invalid`. A pinned fixture
  named by a hard finding of the new plan (as its subject or in its message; else every pinned fixture of a room a
  finding names) is `invalid`, and the scope is generated again without it. Conflicts stay in the overrides (the
  validation judges the honoured and orphaned pins, as the update does); `dropOverrides` removes the scope's pins, all
  of them.
- **The impact** (`RegenerationImpact`, `planDifference()`): rooms regenerated, added, removed and moved; games that
  change slot; titles entering and leaving face-out; games newly placed and unplaced; fixtures added, removed and
  moved; pins kept, conflicts, overrides dropped. The change report is the same difference (Generated, Full or
  NoRebuild).
- **The inputDigest** is what an update of the same selections computes over the new plan (each selection with the
  data of the room it stands in: the scope's fresh data, the other rooms' recorded data; the honoured pins XORed in),
  so the update after a regeneration of an unchanged catalog writes nothing, and the regeneration of a world without
  overrides equals its fresh plan byte for byte.
- **`worldplan regenerate (--room <id> | --floor <id> | --world) [--drop-overrides] [--dry-run]`**
  (`tools/cli/src/cmd_worldplan_regenerate.cpp`, `world_regeneration_text.hpp`, `docs/reference/cli.md`): the impact,
  then the commit through `commitWorld()` (action `regenerate`, scope the room, `floor:<id>` or `world`), undoable.
  **`generate --replace`** over a stored plan is now the world's regeneration: it stops failing `pin-broken` after
  edits (step 2's open issue); the conflicts are named on stderr.

### The frozen signature

```cpp
struct RegenerationScope { RegenerationScopeKind kind; RoomId room; std::string floor; /* ofRoom, ofFloor, ofWorld */ };
struct RegenerationInput { RegenerationScope scope; bool dropOverrides; catalog::LibraryId library;
                           std::vector<RoomSelection> selections; std::map<SystemName, std::vector<std::string>> platformTags;
                           WorldConfig config; WorldOverrides overrides; WorldPlan previous; std::string previousDigest; };
using RoomDataLoader = std::function<Expected<WorldData>(const RoomDataRequest&)>;
struct Regeneration { GenerationResult result; WorldOverrides overrides; RegenerationImpact impact; bool changed; };
Expected<Regeneration> regenerate(const RegenerationInput& input, const BuildingData& building,
                                  const SystemsData& systems, const RoomDataLoader& load);
RegenerationImpact planDifference(const WorldPlan& before, const WorldPlan& after);
RoomDataLoader assetRootLoader(const std::filesystem::path& assetRoot);
```

Step 6 runs `regenerate()` on the pool twice: a dry run for the preview (`impact`, `changed`), then the consent's
(`dropOverrides` for "Rigenera e scarta le personalizzazioni"), saved with `commitWorld(lock, {"regenerate",
scope.toString()}, result, overrides)`. Step 7 adds expansion chains beneath the room scope (today a room with
expansions is refused, `Unsupported`).

## Decisions beyond ADR-0015

- **The data comes through a loader** (`RoomDataLoader`, the request `loadWorldData()` takes): the regeneration knows
  which rooms it needs (a new room's data on a given floor, a standing room's recorded data for the digest and the
  validation), the app and the CLI cache it as they read it.
- **A room or floor needs the world's identity**: a plan of another seed, preset, random algorithm, title-key rule or
  generator major is regenerated only whole (`Unsupported`, the CLI's exit 1 "needs --world"), as the update refuses
  it. A room or floor scope needs a selection of each of its systems (`NotFound`).
- **A floor keeps its systems.** ADR-0015 §5 says systems classified onto another floor stay where they are; an arcade
  system standing on another floor than the ground floor is decided there as any other system. The floor also takes
  the selections' systems no room holds that its classification names (they would enter at the next update anyway).
- **Pinned fixtures' roles come from the previous plan** (a pin names a fixture id, not what it is): a decoration
  keeps its prefab as it stood unless the pin names one; a module or display takes the template's unless it does. A
  fixed fixture (the hero corner, a hero-wall module) is honoured only as the fresh layout stands it.
- **A pinned module or a pinned slot's socket grows the room** to its growth step; a pinned decoration of a growth
  step the room no longer takes is `slot-gone`.
- **A south-wall poster follows its wall** in a regeneration too (as the step-2 amendment does in an update): its pin's
  transform is rewritten in the result's overrides, so a room that shrinks keeps its poster on the wall rather than
  making it `invalid`.
- **A face-out level holding a pinned game** fills from the left (not centred around it); a spine-out level holding a
  pinned game is never dressed.
- **The dry run takes no lock** (unlike `undo` and `edit`): it reads the plan and the overrides, and refuses (exit 3)
  while an interrupted action's journal waits, so that "a dry run writes nothing" holds for `world.lock` too.
- **The floor's acceptable preview time** is the world's 15 s (ADR-0015 §9 names a room and the world only).

## Proofs

- Unit (`tests/unit/world/test_world_regeneration.cpp`, 10 cases): every golden plan of the SNES room (0, 15, 57,
  890 and 2,000 titles, the three golden seeds) and the golden building fixture's every room, floor and the world regenerated
  equal to the golden text, nothing in the impact (no golden rewritten); new favourites entering face-out, the room
  equal to a fresh plan of the selection, the update after it writing nothing; a room kept for its template version
  regenerated to the current one; a floor's sharing decided again both ways (megadrive into its own room, snes into
  the shared room), equal to the fresh building, a system reclassified onto gen3 staying on gen4 with the floor and
  moved by the world; six game pins and two fixture pins (a move, two swaps, one onto a display fixture, a pin in
  place, a moved display case, a decoration pinned) honoured in every scope, the overrides unchanged, the update after
  writing nothing, twice one SHA-256, and dropped with the consent giving the fresh plan back; the pin winning the
  representative; `game-gone`, `slot-gone`, `room-gone` and `invalid` (a display case moved into a grown room's south
  part, the room shrunk: `fixture-outside-room`), each kept in the overrides; a south-wall poster following its wall
  as the room shrinks; a property test of 3 × 40 random edits then a changed catalog (no override dropped, every pin
  honoured or a conflict, the impact the plans' difference, the update after writing nothing, the drop giving the fresh
  plan); the refusals. Every case checks the impact against a difference computed apart.
- CLI (`tests/unit/cli/test_cli_worldplan_regenerate.cpp`): on the building fixture, no plan (exit 1), usage errors,
  an unknown room or floor (exit 1), another seed (exit 1); a dry run writing nothing (the world's folder byte for
  byte, no lock file) with its JSON impact; a room regenerated behind a snapshot with its pin kept, the update up to
  date, a second run writing nothing, undone to the edited plan byte for byte; a floor; the world with
  `--drop-overrides` giving the generated plan back; `generate --replace` after a move honouring the pin (exit 0,
  undoable), then `regenerate --world` finding nothing to change. `test_cli_worldplan_edit` no longer expects
  `pin-broken` from `--replace`.
- Real library (`tests/unit/cli/test_cli_worldplan_regenerate_real.cpp`, `[real-library][regenerate]`): the whole
  library imported, its building generated, then dry runs of room:snes, the busiest floor and the world: exit 0,
  nothing written, nothing to change, the preview within ADR-0015 §9's limits.
- No plan golden, scene golden, oracle or legacy fixture changed.

## Measurements

The real library's dry runs in Release (`./scripts/real-library.ps1 -Filter '[real-library][regenerate]'`, ES-DE
closed, 0 differences among 542,120 entries; 44 rooms on 9 floors, 19,985 placements; the library imported in 26 s,
the building generated in 3.6 s). The preview is the rooms' data and the regeneration (impact and validation
included), the time step 6's job takes on the pool; the catalog's selections, which the app holds already, apart:

| Dry run | Preview | Of which | Selections apart | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|
| room:snes (890 placed) | 1,342 ms | the data of every standing room (the digest, the validation) and the room | 1,361 ms | 2 s |
| floor computer (10 rooms, 5,314 placed) | 1,913 ms | the floor's systems' data and rooms, the other floors' data | 1,121 ms | 15 s (a floor: the world's) |
| the world (44 rooms, 19,985 placed) | 1,713 ms | data 474 ms, regeneration 1,239 ms | 998 ms | 15 s |

Every dry run changed nothing (the catalog unchanged since the generation). The room's preview is within its row but
its largest part is the building's rest (the standing rooms' data for the digest and the validation of every floor),
not the room: the app's cached data (step 6) will take that part out.

## Open issues

- `worldplan check` does not measure overrides yet (ADR-0015 §8: honoured, orphaned, conflicting); the conflicts'
  reasons now exist for it.
- Expansion chains (step 7): the room scope refuses a room with expansions; the floor and world scopes will split a
  chain's titles once generator 1.5 makes them.
- An `invalid` pin found by a finding that does not name it costs every pinned fixture of its room (the fallback is
  conservative); none of the tests needs it.
- The room preview's 1.34 s of 2 s is mostly the rest of the building (above); if step 6 measures it over the row on
  a slower machine, the validation can be narrowed to the scope's floors.
- The app does not regenerate yet (step 6: the directory's actions, `regenerate_preview.rml`, the job on the pool).
