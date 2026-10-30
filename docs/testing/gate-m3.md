# Gate M3: customizable without manual per-game work

Gate M3 closes iteration I4b, customization ([ADR-0015](../decisions/ADR-0015-customization.md)): the editor (games
moved, swapped and pinned, furniture moved on the room's grid) written to `overrides.json`, "Rigenera stanza / piano /
mondo" with an impact preview and consent, snapshots and undo, and linked expansion rooms past a room's capacity
(generator 1.5). Its milestone is M3: "a heterogeneous collection generated automatically, navigable and customizable
without manual per-game work" (roadmap, I4b). What each step built is in [docs/i4b/](../i4b/). This page says what the
gate checks, how to run it, the figures of its runs on the reference machine, where ADR-0015 and M3 part 2's
requirements are proven, the follow-ups, and how to try the editor, a regeneration and an undo.

**Status: not passed yet** (October 30, 2026, reference machine; the runs below), **the user's trial pending**. Every
correctness check passed: the build matrix, both test suites, every golden, every I4b area, the real library's snapshots
(0 differences), the CLI edit session, generator 1.5's update, the dry runs, the commits and undos, the focus watch. Two
things failed: three Debug GPU cases ran out of time under another session's load (they pass when run again, one only
without ctest's timeout), and the regeneration preview of a room in the app takes 2.6 to 4.1 s on the real library
against ADR-0015 §9's 2 s, with the machine loaded. The preview's limit is the user's decision (fix the preview, or
accept it); the gate must then run once on a quiet machine.

## What the gate checks

`scripts/gate.ps1 -Gate M3` runs, under `scripts/focus_watch.py --watch build` (a test process taking the foreground
or showing a window where the user sees it fails the gate):

- **Gate A's build matrix**: `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release`, a compiler, linker or
  CMake warning failing its row.
- **The unit tests and the GPU smoke tests** of Debug and Release (ctest's `unit` and `gpu` labels, the goldens apart).
- **Every golden**, G01 to G20, in Debug and Release: a row per golden with the verdict its case writes, a failed row
  for any the run does not report and, new at this gate, for any that skips (another GPU or driver than the golden's,
  no OpenGL 4.5 context): no golden may pass unproven. I4b added none (the look did not change); G19 and G20 are I5's.
- **The I4b checks**, per Debug and Release, one row per area counted from the unit and GPU runs (`scripts/gate.ps1`'s
  `$i4bAreas`; a case counts in one area only):

  | Area | Unit cases | GPU cases |
  |---|---|---|
  | Overrides and snapshots | `world/overrides`, `world/snapshots` (but undo's), `world/validation: pins`, `world/update: pins are kept ...`, `cli/worldplan check: the overrides ...` | |
  | Edits | `world/edit`, `cli/worldplan edit` | |
  | The editor's games | `app/world_edit` (but the chain's), `app/edit_bar_document`, the directory's "Modifica stanza", the card's "Sposta" | `gpu.world_edit` (but the furniture's) |
  | The editor's furniture | `app/world_edit_furniture`, the bar's furniture, the secondary TV following its sideboard, the cabinets in the editor | `gpu.world_edit: a poster and a side-wall module ...`, `gpu.cabinets`' two editor cases, `gpu.gpu.secondary_tv: the room editor ...` |
  | Regenerations and their preview | `world/regeneration`, `app/world_regenerate` (but the undo's), `ui/regenerate_preview`, `cli/worldplan regenerate`, `cli/worldplan generate --replace` | `gpu.world_regenerate` (but the undo's) |
  | Undo | `world/snapshots`' undo cases, `cli/worldplan undo`, `app/world_regenerate: a room's preview writes nothing; ... undo restores the plan's SHA-256` | `gpu.world_regenerate: a room regenerated with its pin kept, then undone ...` |
  | Expansion rooms in the world | `world/expansion`, `world/expansion chains`, `world/migration: a room past its successor's capacity ...`, `cli/worldplan update: generator 1.4's ...`, `cli/worldplan: the overflow fixture's ...`, `esde/overflow_fixture`, `world/plan_json: shared, arcade and expansion rooms ...` | |
  | Expansion rooms in the app | `app/directory_chains`, `world/expansion_signs`, the directory's chains, `app/world_build: the expansions an update added ...`, `app/world_edit: a game carried into another room of its chain` | `gpu.world_expansion` |

- **The benchmark path** in Release (Gate B's: 2,000 titles at 1920 x 1080, `--no-vsync`) against the acceptable
  limits, its GPU passes informative.
- **ADR-0015 §9's rows** in Release, read from the run reports the editor's and the regenerations' GPU cases leave in
  `build/msvc-x64-release/smoke/<case>/<run>.json` during the gate's GPU run: every edit shown (0.25 s), every impact
  preview (a room 2 s, a floor and the world 15 s; an undo's preview judged as a room's), the room shown again after
  every save, commit and undo (3 s).
- **With `-RealLibrary`** (ES-DE closed; the folders from `-EsdeHome`, `-EsdeInstall`, `-RomRoot`, `-MediaRoot` or the
  `RV_REAL_*` variables): one `scripts/real-library.ps1` run, read-only snapshots before and after with 0 differences,
  `RV_LAUNCH_ALLOWLIST` naming the build's fake emulator alone, of

  | Filter | Test | What its rows read |
  |---|---|---|
  | `[real-library][building]~[migration]` | `tests/unit/app/test_building_real.cpp` | Gate C's rows: the building planned twice with one plan, the unchanged restart, NFR-01 to the lobby and the quick catalog's first page (5 s), the busiest floor's GPU memory (512 MiB), the rides cold and warm with the destination's arrays and the room after them |
  | `[real-library][expansion]` | `tests/unit/cli/test_cli_worldplan_expansion_real.cpp` | the building updated by generator 1.5 from the plan 1.4 stored: no capacity entry before or after, every room's elements byte for byte |
  | `[real-library][regenerate]` | `tests/unit/cli/test_cli_worldplan_regenerate_real.cpp` | the dry runs of "Rigenera stanza" on room:snes, of the busiest floor and of the world: nothing written, every impact count 0, the preview's time |
  | `[real-library][regenerate-app]` | `tests/unit/app/test_world_regenerate_real.cpp` | the app's previews on the pool and in the app (room:snes and the computer floor's busiest room, room:amstradcpc), "Rigenera stanza", "Annulla ultima modifica" and "Rigenera piano" committed in the app: the room shown again |
  | `[real-library][edit-cli]` | `tests/unit/cli/test_cli_worldplan_edit_real.cpp` (new) | a CLI edit session on the real plan (a move, a swap, a pin and a fixture moved, one `--script` session behind one snapshot), `worldplan check` (every pin honoured), an update moving nothing, an undo restoring the plan's SHA-256 |
  | `[real-library][secondary_tv]` | `tests/unit/app/test_secondary_tv_real.cpp` | the busiest floor's largest room with a secondary TV at 1920 x 1080, both screens playing, cold and warm: GPU memory at its peak (512 MiB) |

- **`-TimingOnly`** (as Gate D's): the Release build, the benchmark path and, with `-RealLibrary`, the real library's
  run with its correctness rows: a quiet-machine rerun.

The gate sets `RV_LAUNCH_BACKGROUND=1` and `RV_AUDIO=null` for everything it runs. Every GPU case runs with
`RV_LAUNCH_ALLOWLIST` naming its build's fake emulator alone (ctest `ENVIRONMENT`), and so does the real library's run
(`scripts/real-library.ps1`); the gate does not set it for the unit run, whose runner tests start copies of the fake
under temporary names that such a list refuses (a first run with it set failed two of them,
`platform_win/process_runner: an image Windows cannot load ...` and `... a program missing a DLL ...`, refused by the
guard as it should). Every step logs into `build/gate-m3` (the build and test logs, the JUnit
and Catch2 reports, `results.json`, `focus_watch.json`, `real-library.log` and `real-library/`).

## How to run it

```powershell
./scripts/gate.ps1 -Gate M3                              # fixtures only
./scripts/gate.ps1 -Gate M3 -RealLibrary                 # also the real library, ES-DE closed
./scripts/gate.ps1 -Gate M3 -TimingOnly -RealLibrary     # the timing parts again, on a quiet machine
```

Close ES-DE first (`Get-Process ES-DE` must find nothing). The gate exits 0 when no step failed, 1 otherwise, 2 when it
cannot start.

## The figures of the runs

October 30, 2026, on the reference machine ([reference-hardware.md](reference-hardware.md): Intel Core i7-9700K, 8
cores, 64 GB, RTX 2080 SUPER with OpenGL 4.5.0 NVIDIA 621.01, Windows 11 Home 10.0.29671), branch `i4b/step-9` at
commit 09a96918 (run 1 with the same `gate.ps1` before it was committed). **The machine was not quiet**: other agent
sessions ran beside every run (another worktree's Debug and Release test suites during run 1; during runs 2 and 3 two
recursive `grep` searches of another project, each holding a core at 100% for hours, besides the editors), so the
timing figures below are this load's, not a quiet machine's. Nothing else of this project ran meanwhile.

**Run 1**, `./scripts/gate.ps1 -Gate M3` (68 steps, 12,206 s, logs in `build/gate-m3-run1`): **FAILED**, 1 failed
row, 0 skipped; every correctness row passed.

| Step | Result | Limit | Details |
|---|---|---|---|
| Build `msvc-x64-debug`, `-relwithdebinfo`, `-release` | passed | 0 warnings | 0 warnings each |
| Unit tests Debug / Release | passed | | 2,230 passed each (1,901 s / 586 s) |
| GPU smoke tests Debug | **FAILED** | | 153 of 156 passed: three timed out under the load (below) |
| GPU smoke tests Release | passed | | 153 passed, 1 skipped (the overlay's cursor case, as in every Release run) |
| Goldens G01, G02, G02s, G06, G07, G08a, G08b, G10 to G20, Debug and Release | passed | 0.5%, none skipped | 36 comparisons: G01 73 of 2,073,600 pixels beyond 6/255 (0.004%, the memory margin's known difference), every other golden 0 pixels (largest difference 0/255) |
| I4b overrides and snapshots | passed | | 15 passed each (15 unit) |
| I4b edits | passed | | 18 passed each (18 unit) |
| I4b editor games | passed | | 16 passed each (13 unit, 3 GPU) |
| I4b editor furniture | passed | | 15 passed each (11 unit, 4 GPU) |
| I4b regenerations | passed | | 36 passed each (32 unit, 4 GPU) |
| I4b undo | passed | | 6 passed each (5 unit, 1 GPU) |
| I4b expansion world | passed | | 22 passed each (22 unit) |
| I4b expansion app | passed | | 18 passed each (13 unit, 5 GPU) |
| ADR-0015 §9: an edit shown (Release, fixtures) | passed | <= 250 ms | 15 edits over 15 sessions: at most 58.5 ms, median 19.2 ms |
| ADR-0015 §9: the impact preview (Release, fixtures) | passed | room 2 s, floor and world 15 s | room 112 ms at most (5 previews), floor 150 ms (3), world 144 ms (3), an undo's 1.4 ms (2) |
| ADR-0015 §9: a save, a commit or an undo (Release, fixtures) | passed | <= 3 s | the room shown again: 6 saves at most 107 ms, 4 commits at most 426 ms, 1 undo 172 ms |
| Benchmark path, snes-stress (2,000 titles), frame times | passed | p99 <= 33.3 ms, p50 <= 16.7 ms | p50 3.41 ms, p95 4.17 ms, **p99 4.62 ms** (217 FPS at p99), max 282 ms over 1,230 frames |
| Benchmark path, GPU per pass | info | | p50/p99 ms: shadow 0.047/0.054, depth 0.050/0.088, SSAO 0.422/0.457, scene 0.471/0.858, post 0.385/0.815, total 1.379/1.915 |
| Benchmark path, GPU memory | passed | all <= 512 MiB | collection arrays 76.7 MiB (BC1, 2,000 items), all textures at most 240.0 MiB |
| Focus watch (whole gate) | passed | no finding | 109 foreground changes, **0 to test processes**; 70 windows shown, **0 visible of test processes**, 2 hidden or off-screen (the fake emulator's) |

The three Debug GPU failures are time limits that the load broke, none a wrong result: `gpu.world floor app: two
rooms of one media family ...` and `gpu.world floor app: without --start-room ...` failed `world.firstFrame == 0`
because the lockstep's wait for the building ran past its 10 s ("Frame 0: the building did not come within 10 s",
seven runs of the case), and `gpu.world floor: light reaches through the doorways ...` hit ctest's 600 s timeout. Run
again afterwards (`./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter ...`, the same load), the two floor-app cases
passed and the light case timed out again; run directly (`rv_smoke_tests.exe "world floor: light reaches ..."`, no
ctest timeout) it passed in **579 s** (6,294 assertions). Gate D's Debug run took 474 s for it: in Debug it sits
within 25% of its timeout (BACKLOG). An earlier attempt of run 1 (logs in `build/gate-m3-attempt1`) set
`RV_LAUNCH_ALLOWLIST` for the whole gate and failed two runner unit tests (above), and timed out one more case under
the same load (`app/world_flow: a start room kept until a regeneration ...`, its 60 s budget; it passes alone in 23 s
in Debug); the gate no longer sets the variable.

**Runs 2 and 3**, `./scripts/gate.ps1 -Gate M3 -TimingOnly -RealLibrary` (38 steps each, 1,057 s and 993 s;
logs in `build/gate-m3-run2` and `-run3`): **FAILED**, 4 rows each, all of them the room previews' time (ADR-0015 §9);
every correctness row passed. The Release build and the benchmark path as in run 1 (p99 4.93 and 4.60 ms, 240.0 MiB).
The real library, ES-DE closed (ES-DE 3.4.1, 45 gamelists; the catalog of 90,246 games; the building of 44 rooms on 9
floors, 19,985 placements; nothing derived from the titles is written here):

| Step | Result | Limit | Run 2 | Run 3 |
|---|---|---|---|---|
| Real library: snapshots (NFR-10) | passed | 0 differences | **0 differences among 542,120 entries** | 0 among 542,120 |
| Real library: the six tests, Release | FAILED | | 797 assertions in 6 test cases, 3 failed (`[regenerate-app]`'s three room previews, below) | the same |
| Real building: planned twice | passed | one SHA-256 | one plan digest over the nine runs, one file SHA-256; the jobs 5.96 and 4.84 s | 7.12 and 5.20 s |
| Real building: unchanged restart | passed | skipped, <= 3 s | the job skipped by its stamp in 0.66 and 0.71 s, nothing written (the whole job, its stamp removed: 3.80 s) | 0.62 and 0.59 s (3.71 s) |
| NFR-01 to the lobby | passed | < 5,000 ms | the stored lobby **3,985 and 4,247 ms**, the quick catalog's first page **2,537 and 2,711 ms** after the process started | 3,896 and 3,670 ms; 2,440 and 2,348 ms |
| Real building: the busiest floor's GPU memory | passed | <= 512 MiB | the computer floor (10 rooms, 5,314 items): 451.7 MiB cold and warm (arrays 210.9 MiB, BC1; the default 1280 x 720 window) | the same |
| Real building: rides cold | passed | view back <= 10 s | the lobby to room:amstradcpc (1,646 items): **2.73 s** (`timed-out`: the floor built at 1.05 s, 1,509 covers in view still coming; the destination's arrays checked by the test), 451.7 MiB; back 1.45 s | 2.51 s (1.05 s, 1,494 coming), 447.1 MiB; back 1.21 s |
| Real building: the room after the cold ride | passed | <= 30 s | 13.15 s | 11.61 s |
| Real building: rides warm | passed | view back <= 3 s | **2.24 s** (`arrived`: built at 1.16 s, its covers in view at 1.87 s), 443.9 MiB; back 1.27 s | 2.48 s (1.05 s, 2.23 s); back 1.41 s |
| Real building: the room after the warm ride | passed | <= 3 s | 1.87 s | 2.23 s |
| Real library: the building updated by generator 1.5 | passed | no capacity entry, every room byte for byte | generator 1.4's stored plan updated by 1.5 in 5.4 s: **44 of 44 rooms' elements byte for byte, 0 capacity entries before and after**, nothing added, unplaced or moved; fresh: 0 for capacity, 0 expansion rooms (the busiest room room:nes, 2,217 placed, within its template) | 5.9 s, the same |
| Real regeneration: dry run of room:snes (CLI) | passed | <= 2,000 ms | the preview **1,733 ms** (the selections 1,426 ms apart), nothing written, every impact count 0 | 1,692 ms |
| Real regeneration: dry run of the computer floor (CLI) | passed | <= 15,000 ms | 2,003 ms, 10 rooms | 2,009 ms |
| Real regeneration: dry run of the world (CLI) | passed | <= 15,000 ms | 2,195 ms, 44 rooms | 2,193 ms |
| Real app regeneration: the catalog's inputs | info | | 1,512 ms (the snapshot 1,149 ms, 192 selections 353 ms) | 1,819 ms |
| Real app preview: room:snes (on the pool, the inputs ready) | **FAILED** | <= 2,000 ms | **2,332 ms** (read 507, regeneration 1,807) | **2,832 ms** (651, 2,164) |
| Real app preview: room:snes again (warm) | passed | <= 2,000 ms | 1,680 ms | 1,751 ms |
| Real app preview: the computer floor / the world | passed | <= 15,000 ms | 2,359 / 2,418 ms | 2,035 / 2,450 ms |
| Real app preview in the app: room:snes | **FAILED** | <= 2,000 ms | **3,271 ms** after the request (the inputs read ahead by the tab) | **2,646 ms** |
| Real app preview in the app: room:amstradcpc | **FAILED** | <= 2,000 ms | **3,608 ms** | **4,102 ms** |
| Real app: Rigenera stanza / Annulla ultima modifica, room:snes | passed | <= 3,000 ms | the room shown again 1,766 / 1,854 ms after the choice | 1,229 / 1,363 ms |
| Real app: Rigenera stanza / Annulla ultima modifica, room:amstradcpc | passed | <= 3,000 ms | 2,486 / 1,935 ms | 1,498 / 1,638 ms |
| Real app: Rigenera piano of floor:gen4 | passed | <= 3,000 ms | 1,542 ms | 1,282 ms |
| Real edit: a CLI session | passed | written, one snapshot | `worldplan edit --script` on room:snes: a move, a swap, a pin and room:snes/fx:dec-se moved 100 mm (its place found by one dry run), saved in **2.1 s** behind one snapshot | 1.9 s |
| Real edit: `worldplan check` | passed | 5 honoured, 0 hard | 0.7 s: 0 hard, 0 soft findings; **5 pins honoured**, 0 orphaned, 0 conflicting, 0 unhonoured | 0.6 s, the same |
| Real edit: an update moving nothing | passed | nothing written | `worldplan update` in 4.7 s: up to date, the plan and `overrides.json` byte for byte | 4.8 s |
| Real edit: undo restoring the plan's SHA-256 | passed | one SHA-256 | `worldplan undo` in 0.4 s: **the plan's SHA-256 the generated one**, `overrides.json` removed | 0.2 s |
| Real busiest floor, both screens playing | passed | <= 512 MiB | room:amstradcpc at 1920 x 1080, two decoders: **477.5 MiB** at its peak cold and warm (and with a screenshot); procedural surfaces 84.9, collection arrays 210.9, scene textures 106.0, targets 60.2, buffers 6.5, hero textures 3.1 MiB; frame p99 108.3 ms cold, 15.2 ms warm | 477.5 MiB; p99 30.3 and 6.6 ms |
| Focus watch (whole gate) | passed | no finding | 0 foreground changes; 1 window shown, 0 visible of test processes | 1 change, 0 to test processes; 0 visible |

### The open measurements against their limits

| Measurement (where it was open) | Limit | Measured now | Verdict |
|---|---|---|---|
| The lobby after the start (step 8: 5.5 and 9.0 s under load) | 5 s | 3.7 to 4.2 s (four starts) | within, with less room than Gate D's 2.8 to 2.9 s |
| The quick catalog's first page (step 8: 7.6 s under load) | 5 s | 2.3 to 2.7 s | within |
| The cold ride (and its collection arrays) | 10 s | 2.51 and 2.73 s, the destination's arrays checked | within |
| The warm ride | 3 s | 2.24 and 2.48 s | within |
| room:amstradcpc's regeneration preview in the app (step 6: 1.6 s quiet, 3.2 s under load) | 2 s | **3.61 and 4.10 s** | **over** |
| room:snes's preview in the app | 2 s | **3.27 and 2.65 s** | **over** |
| The busiest floor at 1920 x 1080, both screens playing (the memory margin's estimate: about 477.5 MiB) | 512 MiB | **477.5 MiB**, as estimated | within (34.5 MiB to spare) |
| A commit or an undo, the room shown again | 3 s | 1.23 to 2.49 s (ten commits and undos) | within |
| A CLI edit session saved | (3 s) | 1.9 and 2.1 s | within |

**The room preview is over its limit.** The breakdown the runs give: the catalog's inputs (1.5 to 1.8 s) are read
ahead by the directory's tab and not counted; the preview's own job reads the world (0.5 to 0.65 s: the plan parsed
and its overrides) and regenerates (1.2 to 2.2 s for one room; a large room alone, `esde/snes-stress`'s 2,000 titles,
takes under a tenth of a second, `docs/i5/memory-margin.md`, so the world around the room is the cost: the plan copied
and the building's inputs and plans digested whole); the same job in the app takes another 0.9 to 2 s, sharing the
pool with the cover loader and the frame. A warm second preview in the test's process is within (1.68 and 1.75 s), so
are the CLI's dry runs (1.69 to 1.73 s; room:amstradcpc's by hand 2.11 s), and the commits are within 3 s. The load
stretched Gate D's quiet figures by 1.3 to 1.4 times in these runs; on a quiet machine the in-app previews would still
be about 2 to 3 s, over. Nothing was changed: the candidate fix (BACKLOG, "Measurement": the digests' canonical text
written straight from the plan instead of through a JSON document of the whole world) is not a small change, and the
plan's parse in the preview's read is the next cost. Left for the user to decide.

## Where ADR-0015's decisions are proven

| ADR-0015 | Decision | Where it is proven |
|---|---|---|
| §1 | The plan is the truth, overrides the user's claims, every manual change pinned | `world/edit: a game moved into a free slot is pinned there ...`, `world/edit: games swapped, pinned and unpinned ...`, `world/edit: fixtures moved on the grid and turned, pinned where put ...`, `world/edit: random edit sessions keep the plan valid, every manual change pinned ...` |
| §2 | `overrides.json` schema 2, provenance, the schema-1 lift, the digest | `world/overrides: schema 2 written and read back, schema 1 lifted`, `world/overrides: pins read from overrides.json`, `world/edit: the overrides join the inputDigest; a world without overrides keeps its digest`, `world/snapshots: overrides saved under the world lock` |
| §3 | Updates keep every placement and moved fixture, regenerations honour the overrides, conflicts with reasons never dropped silently | `world/edit: an update with additions keeps every pin and moved fixture ...`, `world/edit: a fixture moved by the user is never moved by the room's growth`, `world/regeneration: the overrides honoured, never dropped without the consent`, `world/regeneration: conflicts with their reasons, kept in the overrides`, `world/regeneration: a conflict recorded on its pin, reported as orphaned at every update ...`, `world/regeneration: the pin wins the representative`; `worldplan check` listing each pin honoured, orphaned or conflicting with its reason (`world/overrides: each pin measured as ...`, `cli/worldplan check: the overrides beside the plan ...`); the real session's check (gate) |
| §4 | The editor: games and furniture, the bar, the draft replayed on save, the room's static part rebuilt alone | `app/world_edit: ...`, `app/world_edit_furniture: ...`, `app/edit_bar_document: ...`, `gpu.world_edit: ...` (four cases), `gpu.world_expansion: the editor carries a game of the head through the hall into its expansion` |
| §5 | Explicit regeneration of a room, a floor or the world with an impact preview and consent; the drop's second confirmation | `world/regeneration: ...`, `ui/regenerate_preview: ...`, `app/world_regenerate: ...`, `gpu.world_regenerate: ...` (five cases); the real dry runs (gate) |
| §6 | Snapshots, the journal, undo of explicit actions, no redo | `world/snapshots: ...` (ten cases), `cli/worldplan undo: a regeneration undone ...`, `gpu.world_regenerate: a room regenerated with its pin kept, then undone to the plan's SHA-256`; the real undo restoring the plan's SHA-256 (gate) |
| §7 | Expansion rooms, generator 1.5, the migration of capacity entries | `world/expansion: ...`, `world/expansion chains: ...`, `cli/worldplan update: generator 1.4's titles unplaced for capacity enter their chains`, `gpu.world_expansion: ...` (five cases); the real building updated by 1.5 (gate) |
| §8 | The command line: `edit`, `regenerate`, `snapshots`, `undo`, `check` | `cli/worldplan edit: ...`, `cli/worldplan regenerate: ...`, `cli/worldplan undo: ...`, `cli/worldplan check: the overrides beside the plan, a pin honoured, one orphaned and a recorded conflict`; the real CLI session (gate) |
| §9 | The acceptable rows: an edit shown, the preview, a save, commit or undo | ADR-0015 §9's rows on the fixtures and the real library's rows (gate) |

## Where M3 part 2's requirements are proven

| Requirement | Where it is proven |
|---|---|
| SC-09 (a shelf moved, a favourite on a display case, saved; games added and the library updated; nothing overwritten without consent) | `gpu.world_edit: a poster and a side-wall module moved and saved ...`, `gpu.world_edit: "Sposta" on the card takes a shelf's cartridge ... onto a display case`, `gpu.world_edit: a scripted session ... a restart finds them there`; `world/edit: an update with additions keeps every pin and moved fixture ...`; `world/regeneration: the overrides honoured, never dropped without the consent`, `gpu.world_regenerate: "Rigenera e scarta le personalizzazioni" drops the room's pins only after the second confirmation` |
| FR-009 (incremental additions stable: pins and placements kept) | `world/edit: an update with additions keeps every pin ...`, `world/update: pins are kept, orphans reported, a change that would move one refused`, `world/expansion: one fill order per chain, the head's freed slots used first`, `world/expansion chains: a stale pin neither refuses growth nor pulls titles`; the real update after the CLI session and generator 1.5's update moving nothing (gate) |
| FR-025 (the furniture editor: moves saved and kept) | `world/edit: fixtures moved on the grid and turned ...`, `world/edit: a poster moved along or onto the south wall follows it when the room grows ...`, `app/world_edit_furniture: ...` (eight cases), `gpu.world_edit: a poster and a side-wall module moved and saved, a refused drop, the room walked after`; the real session's fixture moved, checked and kept by the update (gate) |
| §7.6 (stable incremental changes; pins kept; confirmation for structural changes; "Rigenera stanza / mondo" with an impact summary) | points 2, 4 and 7: the FR-009 row; point 6: expansion rooms only when needed (`world/expansion: no title unplaced for capacity while the floor has room ...`, `world/expansion: a full floor keeps the titles unplaced for capacity ...`); point 8: `app/world_regenerate: the notice offers the room of the generator's own needs-confirmation`; the explicit actions: `ui/regenerate_preview: ...`, `gpu.world_regenerate: ...`; "Riorganizza scaffale" is "Rigenera stanza" (plan, decision 13) |
| §16.3 (atomic writes, rotating backups, a backup before destructive regenerations, overrides kept) | `world/snapshots: the plan and the overrides committed together behind a snapshot`, `... an action interrupted between its writes is rolled back at the next lock`, `... the last 20 snapshots are kept`, `... undo restores the plan and the overrides byte for byte, never an undo`; every regeneration and save behind a snapshot (`cli/worldplan regenerate: ...`, `app/world_edit: a save writes the draft behind a snapshot that undo restores`) |
| §21.3 (same input, same plan; no pin changed by one addition; a regeneration never drops overrides without an explicit action) | `world/regeneration: a world without overrides regenerated equals its fresh plan, the goldens unchanged`, `world/expansion: ...` (twice one SHA-256), `world/edit: random edit sessions keep the plan valid ... the update writing nothing`, `world/regeneration: random edit sessions then a changed catalog: no override dropped, every pin honoured or a conflict ...`; the real building planned twice with one SHA-256 (gate) |

## Follow-ups

Gathered in `BACKLOG.md`, "Follow-ups from I4b" and "Measurement":

- **The room's regeneration preview on the real library** (ADR-0015 §9's 2 s): 2.6 to 4.1 s in the app under load,
  about 1.7 s for the CLI's dry run (above). The candidate fixes: the digests' canonical text written straight from
  the plan (the BACKLOG's), the plan's parse shared with the app's own copy, the preview's job ahead of the cover
  loader on the pool.
- **The Debug GPU cases that are close to their time limits under load**: `gpu.world floor: light reaches through the
  doorways ...` (474 to 579 s in Debug against ctest's 600 s), the floor-app cases' 10 s lockstep wait for the
  building, `app/world_flow: a start room kept ...`'s 60 s; a gate on a loaded machine fails them on time alone.
- **`RV_LAUNCH_ALLOWLIST` for the unit run**: the runner's tests start copies of the fake under temporary names, so a
  list naming the builds' fakes refuses them; the gate leaves the variable to the GPU cases and the real library's run
  (`docs/reference/safety.md`'s BACKLOG item stays open).
- **The lobby's margin**: 3.7 to 4.2 s under this load against 5 s (Gate D's quiet 2.8 to 2.9 s); the hub's build is
  the cost (Gate D).
- **`worldplan check` and unhonoured pins**: a pin the plan does not honour although what it names is there (a hand
  edit) is listed, never a failure; the app's editor does not show the check's list (the preview shows the conflicts).
- From the steps' notes, unchanged: the in-place apply's long frame in a large room (step 3), Salva's per-room digests
  (step 4), a floor's or the world's regeneration that places rooms again built whole (step 6), a fixture never moving
  between the rooms of a chain and the ranges of a chain without its items files (step 8); ADR-0015's "Deferred"
  (redo, undo of updates, moving games between systems, the hero corner).

## Try the editor, a regeneration and an undo

On the reference machine, with ES-DE closed. Retroverse only reads the library: the catalog, the plan, its overrides
and snapshots, the derived covers and the logs go to the data directory, here `build/rvdata` (any folder outside
ES-DE's works; the programs refuse one inside them). From a PowerShell in the repository, after
`./scripts/build.ps1 -Preset msvc-x64-release`:

```powershell
$bin = './build/msvc-x64-release/bin'; $data = 'build/rvdata'
& "$bin/retroverse-cli.exe" --data-dir $data library create   # finds ES-DE as ES-DE finds itself (once)
& "$bin/retroverse-cli.exe" --data-dir $data import           # the whole library (about 20 s; again to refresh)
& "$bin/retroverse.exe" --data-dir $data --fullscreen --start-room room:snes   # the building, at room:snes's doorway
```

Games launch with ES-DE's own commands from a PowerShell where `RV_LAUNCH_ALLOWLIST` is not set; to try the editor
without any emulator starting, set it to the build's fake first (`$env:RV_LAUNCH_ALLOWLIST = (Resolve-Path
"$bin/rv_fake_emulator.exe").Path`).

**The editor** (games and furniture):

1. In room:snes, M opens the directory; its third tab "Modifiche" (the shoulders, or Right on the tab bar) lists
   "Modifica stanza". Or point at a cartridge, E twice to inspect it, and "Sposta" on its card (it starts the editor
   with that game in the hand).
2. In the editor the bar at the bottom names the room and what the reticle targets. E picks up the game under the
   reticle; E on an empty slot moves it there (a ghost of it shows where it would stand, outlined warm white, red when
   a rule refuses it), E on another game swaps them, E on its own slot or Esc puts it back. P (the pad's X) pins or
   unpins the game targeted. The D-pad (the arrows) steps over every slot, empty ones included.
3. Where no game or slot is under the reticle it targets a side-wall module, a display case or a decoration: E picks
   it up, it follows the floor (a poster its wall) on the 100 mm grid, Q and R (the shoulders) turn it, the arrows
   nudge it by 10 cm; E drops it (refused with the rule named on the bar), Esc puts it back.
4. Enter (Start) opens the menu: **Salva**, Annulla ultima azione, Esci senza salvare, Continua. Salva writes the plan
   and `overrides.json` behind a snapshot; the room shows again at once.
5. Quit and start again with the same command: the games and the furniture stand where you put them.

**A regeneration**: M, "Modifiche", "Rigenera stanza" (the room you stand in; "Rigenera piano" the floor, "Rigenera
mondo" the building). The preview lists what would change (rooms, games that change place, the display cases,
fixtures), the customizations kept and those that cannot be, with why. **Rigenera** keeps every customization;
**Rigenera e scarta le personalizzazioni** asks a second confirmation ("Indietro" focused) before dropping them;
**Annulla** writes nothing. After Rigenera the room is rebuilt and you stand at its doorway.

**An undo**: M, "Modifiche", "Annulla ultima modifica" names what it would undo (the saved edits or the regeneration,
its room and time); "Annulla la modifica" restores the plan and the overrides as they were, byte for byte. There is no
redo; undo walks back through the explicit actions (the last 20 kept).

The same from the command line, on the same data directory (each prints what it did; `--dry-run` writes nothing):

```powershell
& "$bin/retroverse-cli.exe" --data-dir $data worldplan show                     # the floors and rooms
& "$bin/retroverse-cli.exe" --data-dir $data worldplan edit pin --game <GameId>  # where-is <GameId> finds a game
& "$bin/retroverse-cli.exe" --data-dir $data worldplan check (Join-Path $data 'worlds/esde/world-plan.json')
                                              # the pins: honoured, orphaned, conflicting (with why), unhonoured
& "$bin/retroverse-cli.exe" --data-dir $data worldplan regenerate --room room:snes --dry-run   # the impact
& "$bin/retroverse-cli.exe" --data-dir $data worldplan snapshots                # the actions undo can restore
& "$bin/retroverse-cli.exe" --data-dir $data worldplan undo                     # the last one, byte for byte
```

(`worlds/esde` is the world of the library `library create` names `esde`; `worldplan show` prints the plan's path.)
