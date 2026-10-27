# Gate D: the building reads like the concept

Gate D closes iteration I6, the styling pass of the building ([ADR-0014](../decisions/ADR-0014-the-styling-pass.md)):
light through the doorways and per area, dressed halls, a theme pack per floor with its era furniture, cases and boxes
standing on their side with an upright spine (migrated once), the hub, the arcade's cabinets and the handhelds'
showcases. Its gate asks that "the building reads like the concept, era by era" (roadmap, I6). What each step built and
measured is in [docs/i6/](../i6/), the direction floor by floor in [i6/look.md](../i6/look.md). This page says what the
gate checks, how to run it, the figures of its runs on the reference machine, the style §26.2 checklist for every
golden against its `example.png` panel, the limits still open, and what to look at when walking the building.

**Status: passed** (October 27, 2026, reference machine; the runs below). Every correctness check passed and every
timing row is within the acceptable limits. The user declared I6 (and I4) done on October 27, 2026; the walk of the
building ("Walk the building", below) is postponed.

## What the gate checks

`scripts/gate.ps1 -Gate D` runs everything Gate C runs ([gate-c.md](gate-c.md): the build matrix of Debug,
RelWithDebInfo and Release without a warning, the unit tests and the GPU checks of Debug and Release, the I4 rows per
area, the benchmark path in Release against the acceptable limits) and adds:

- **Every golden**, G01 to G18, in Debug and Release: Gate C's eleven and I6's G14 (the hub from its entrance), G15
  (the arcade), G16 (a handheld room), G17 (an optical-era lounge room) and G18 (a floor's hall), with a failed row for
  any the run does not report. G11, G12 and G13 were regenerated on purpose by I6's steps; G01 never changed.
- **The I6 checks**, per Debug and Release, one row per area, counted from the unit and GPU runs (`scripts/gate.ps1`'s
  `$i6Areas`; a case counts in one I6 area only and may count in an I4 row too):

  | Area | Unit cases | GPU cases |
  |---|---|---|
  | Light | `scene/areas`, `render/light_packing`, `world/room_look`, `world/floor`'s doorway cases | `gpu.world floor: light reaches through the doorways ...` |
  | Halls | `world/floor`'s hall dressing and materials, `assets/surface_textures`, `assets/test_patterns: a hall's print ...` | |
  | Theme packs | `world/theme packs`, the stale pack of `cli/worldplan`, the lit shelf, neon lettering, the procedural surfaces' memory | `gpu.world look: ...` |
  | Spines | `world/families: cases ...`, `world/migration`, the side spine, the printed spine, `cli/worldplan update: generator 1.3 ...` | `gpu.world families`: the jewel cases on their side, generator 1.3's building migrated once |
  | Era furniture | `world/era furniture`, the era prefabs and patterns | |
  | Hub | `world/hub`, `app/hub_showcase`, the totem's prompt, `ui/directory: openGame() ...`, the hub's prefabs and board | `gpu.world hub` (5 cases) |
  | Arcade and handhelds | `world/families: the arcade ...`, the cabinets', counters' and step 6 review's prefabs, the marquee and side art | `gpu.world families`: the cabinets' screens, a launch from a cabinet |

- **The focus watch** over the whole gate, as for Gates M1, B and C.
- **With `-RealLibrary`** (ES-DE closed; the folders from `-EsdeHome`, `-EsdeInstall`, `-RomRoot`, `-MediaRoot` or the
  `RV_REAL_*` variables; generator 1.3's `retroverse-cli.exe` from `-LegacyCli` or `RV_LEGACY_CLI`, built as
  [i6/step-4.md](../i6/step-4.md) says): Gate C's one `scripts/real-library.ps1 -Filter
  '[real-library][building],[real-library][room-real]'` run (read-only snapshots before and after, 0 differences;
  `RV_LAUNCH_ALLOWLIST` naming the fake emulator alone), whose `[building]` filter now holds the migrations' case too
  (`tests/unit/app/test_building_real.cpp`, I6 steps 4 to 6). The gate reads Gate C's rows (the building planned twice
  with one plan, the unchanged restart, NFR-01 to the hub, the busiest floor's GPU memory, the rides cold and warm and
  the room after them, the real SNES room) and two more from the case's reports in
  `build/msvc-x64-release/real-library/migration`: **generator 1.3's building migrated once** (the rooms of succeeded
  families migrated in one update with the notice, every other room byte for byte in its elements, no title over
  capacity, a second start and the whole job writing nothing) and **the ground floor's rooms re-placed onto the hub** in
  that same update. Without generator 1.3's CLI the case skips and those rows fail.
- **`-TimingOnly`** builds Release alone and runs the timing parts again (the benchmark path and, with `-RealLibrary`,
  the real library's run with its correctness rows): the quiet-machine rerun below.

Every step logs into `build/gate-d` (the build and test logs, the JUnit and Catch2 reports, `results.json`,
`focus_watch.json`, `real-library.log` and `real-library/`).

## How to run it

```powershell
./scripts/gate.ps1 -Gate D                                     # fixtures only
./scripts/gate.ps1 -Gate D -RealLibrary -LegacyCli <dir>\retroverse-cli.exe   # also the real library, ES-DE closed
./scripts/gate.ps1 -Gate D -TimingOnly -RealLibrary            # the timing parts again, on a quiet machine
```

Close ES-DE first (`Get-Process ES-DE` must find nothing). The gate exits 0 when no step failed, 1 otherwise, 2 when it
cannot start.

## The figures of the runs

October 27, 2026, on the reference machine ([reference-hardware.md](reference-hardware.md): Intel Core i7-9700K, 8
cores, 64 GB, RTX 2080 SUPER with OpenGL 4.5.0 NVIDIA 617.42, Windows 11 Home 10.0.29671), at commit 2852076 (I6 step
7 on branch `i6/step-7`). Runs 1 and 2: no other build or test of the project ran, but the user's desktop programs
and other agents' tools did; run 3, the quiet-machine rerun of the timing parts, with nothing else building or
testing (CPU load 10% before it started). The timing figures are measured against the acceptable limits only
(roadmap, "Acceptable performance"); the correctness checks are not affected.

**Run 1**, `./scripts/gate.ps1 -Gate D` (75 steps, 6,937 s): **PASSED**, 0 failed, 0 skipped (logs in
`build/gate-d-run1`).

| Step | Result | Limit | Details |
|---|---|---|---|
| Build `msvc-x64-debug`, `-relwithdebinfo`, `-release` | passed | 0 warnings | 0 warnings each |
| Unit tests Debug / Release | passed | | 1,942 passed each (1,306 s / 383 s) |
| GPU smoke tests Debug / Release | passed | | 105 / 102 passed (Release: 1 skipped, the overlay's cursor case, as in every Release run) |
| Goldens G01, G02, G02s, G06, G07, G08a, G08b, G10 to G18, Debug and Release | passed | 0.5% | **0 of 2,073,600 pixels differ** by more than 6/255 in each of the 32 comparisons (largest difference 0/255) |
| I4 rows, Debug / Release | passed | | world building 68, floors 38, shared rooms 9, update suite 8, directory 36, elevator 21, floor app 25, start and live 26 passed each |
| I6 light | passed | | 31 passed each (30 unit, 1 GPU) |
| I6 halls | passed | | 15 passed each (15 unit) |
| I6 theme packs | passed | | 16 passed each (15 unit, 1 GPU) |
| I6 spines | passed | | 14 passed each (12 unit, 2 GPU) |
| I6 era furniture | passed | | 10 passed each (10 unit) |
| I6 hub | passed | | 23 passed each (18 unit, 5 GPU) |
| I6 arcade and handhelds | passed | | 8 passed each (6 unit, 2 GPU) |
| Benchmark path, snes-stress (2,000 titles), frame times | passed | p99 <= 33.3 ms, p50 <= 16.7 ms | p50 **3.32 ms**, p95 3.59 ms, **p99 3.81 ms** (262 FPS at p99), max 5.29 ms over 1,230 frames |
| Benchmark path, GPU per pass | info | | p50/p99 ms: shadow 0.048/0.053, depth 0.053/0.092, SSAO 0.426/0.457, scene 0.477/0.559, post 0.398/0.442, total 1.391/1.572 |
| Benchmark path, GPU memory | passed | all <= 512 MiB | collection arrays 76.7 MiB (BC1, 2,000 items), all textures at most 265.5 MiB |
| Focus watch (whole gate) | passed | no finding | 0 foreground changes; 2 windows shown, both hidden or off-screen (the fake emulator's) |

Beside the gate, the focus watcher around the Release suites (`python -I scripts/focus_watch.py --watch
build/msvc-x64-release --out build/focus/gate-d.json -- pwsh -NoProfile -File scripts/test.ps1 -Preset
msvc-x64-release-gpu`): 2,061 tests (1,942 unit, 119 GPU), all passed but the overlay's cursor case (skipped as
always); **0 foreground changes, 0 visible windows of test processes** (1 window shown, hidden or off-screen), 1,528 s.

**Run 2**, `./scripts/gate.ps1 -Gate D -RealLibrary -LegacyCli <generator 1.3's CLI>` (98 steps, 7,097 s): **PASSED**,
0 failed, 0 skipped (logs in `build/gate-d-run2`). Its fixture rows are run 1's (the same counts, the 32 golden
comparisons 0 pixels apart; the benchmark path's p50 3.31 ms, p99 3.81 ms, 265.5 MiB at most; the focus watch 0
foreground changes, 2 windows hidden or off-screen). The real library's rows, ES-DE closed (ES-DE 3.4.1, 45
gamelists; the catalog of 105,130 entries, 90,246 games; the building of 44 rooms on 9 floors, 19,985 placements;
nothing derived from the titles is written here), with run 3's beside them:

| Step | Result | Limit | Run 2 | Run 3 (quiet) |
|---|---|---|---|---|
| Real library: snapshots (NFR-10) | passed | 0 differences | **0 differences among 542,120 entries** | 0 among 542,120 |
| Real library: `[building]` and `[room-real]` tests, Release | passed | | 648 assertions in 3 test cases (the building, the migrations, the SNES room) | the same |
| Real building: planned twice | passed | one SHA-256 | one plan digest over the nine runs and one file SHA-256 (the test compares them; never written here); the jobs 3.31 s and 3.12 s | 3.22 s and 2.90 s |
| Real building: unchanged restart | passed | skipped, <= 3 s | the job skipped by its stamp in **0.48 s** and 0.49 s, nothing written (the whole job, its stamp removed: 2.53 s) | 0.47 s and 0.46 s (2.58 s) |
| NFR-01 to the hub | passed | < 5,000 ms | the stored hub shown **2,866 ms** and 2,936 ms after the process started, the quick catalog's first page 1,801 and 1,819 ms | 2,814 and 2,773 ms; 1,699 and 1,727 ms |
| Real building: the busiest floor's GPU memory | passed | <= 512 MiB | the computer floor (10 rooms, 5,314 items, the most placements of the 9 floors): **471.8 MiB** cold and warm (collection arrays 210.9 MiB, BC1) | 471.8 MiB |
| Real building: rides cold | passed | view back <= 10 s | the hub to the busiest floor's largest room (room:amstradcpc, 1,646 items): the view back **2.50 s** (`timed-out`: the floor built at 0.75 s, 1,463 of its covers in view still coming), 471.8 MiB; back 1.00 s | 2.50 s (0.78 s; 1,457 coming); back 0.97 s |
| Real building: the room after the cold ride | passed | <= 30 s | the room whole **13.89 s** after the request | 14.57 s |
| Real building: rides warm | passed | view back <= 3 s | the view back **1.84 s** (`arrived`: the floor built at 0.76 s, its covers in view resident at 1.59 s); back 0.95 s | **1.53 s** (0.72 s, 1.28 s); back 0.94 s |
| Real building: the room after the warm ride | passed | <= 3 s | 1.59 s | 1.28 s |
| Real building: generator 1.3 migrated once | passed | once, 0 over capacity | **25 rooms migrated** in one update of 3.61 s with the notice (12 generic_case to generic_case_v2, 5 disc_case_cd, 3 big_box, 2 disc_case_dvd, 2 case_handheld to their `_v2`, the arcade room's generic_case to arcade_cabinet), the other **19 byte for byte** in their elements, **0 titles over capacity**; a second start and the whole job (its stamp removed) unchanged, nothing migrated or written; the CLI's update writes nothing | the same; the update 3.49 s |
| Real building: re-placed onto the hub | passed | in the same update | the arcade room re-placed from I4's lobby (x 7,650, z -8,150 mm) onto the hub's north wall (x 9,000, z -14,150 mm: its z following the depth it grew to in the same update) | the same |
| Real room: the plan generated twice | passed | one SHA-256 | 890 placements (44 featured, 12 band, 834 spine), 6,907 unplaced; 19 lights, 35 clusters | the same |
| Real room: restart after a second import | passed | NoRebuild, 0 moves | unchanged, `no-rebuild`, the plan not written; 890 kept, 0 moved | the same |
| Real room: time to the room | passed | restart <= 5 s | 1,747 ms (restart), 1,981 ms (cold), 1,921 ms (again) | 1,639, 2,236, 1,927 ms |
| Real room: covers resident, cold / warm | passed | 30 s / 3 s | **10.98 s** cold (1,780 layers derived and stored), 0.29 s and 0.41 s warm | **7.21 s**; 0.33 s and 0.46 s |
| Real room: frames while covers load | passed | p99 <= 50 ms | coverLoad p99 5.73 ms cold, 5.93 and 16.28 ms warm | 6.62, 8.74 and 16.17 ms |
| Real room: benchmark path | passed | p99 <= 33.3 ms, p50 <= 16.7 ms; 512 MiB | p50 3.41 ms, p99 **4.04 ms** (248 FPS), max 5.79 ms; 228.6 MiB at most | p50 3.41 ms, p99 4.12 ms, max 9.65 ms; 228.6 MiB |
| Real room: refresh of the library | passed | p99 <= 50 ms, 0 moves | the whole library imported while the room showed in 14.3 s; 4,005 frames, p50 3.48 ms, **p99 5.08 ms**; the room's live update unchanged, 0 moved | 13.6 s; 3,820 frames, p99 4.98 ms |
| NFR-01 with the room | passed | < 5,000 ms | the quick catalog's first page **1,658 ms** after the process started | 1,622 ms |

**Run 3**, `./scripts/gate.ps1 -Gate D -TimingOnly -RealLibrary ...` on a quiet machine (30 steps, 649 s): **PASSED**
(logs in `build/gate-d-run3`): the Release build (nothing to do), the benchmark path (p50 3.31 ms, p95 3.56 ms, p99
3.81 ms, max 5.13 ms, 263 FPS at p99; 265.5 MiB at most) and the real library's rows above; the focus watch saw
nothing. Runs 2 and 3 agree within the machine's noise: the timing rows that missed under other worktrees' load at I6
step 4 (the warm ride, the lobby's NFR-01) passed in both with room to spare (the warm ride's view back 1.84 and 1.53
s for 3 s, the hub at 2.8 to 2.9 s for 5 s). Against Gate C: the busiest floor's memory rose from 407.3 to 471.8 MiB
(the theme packs' procedural surfaces and dressing, I6 steps 3 to 6; within 512), the hub shows about 0.7 s later
than I4's lobby did (2.1 s: a larger hall to build), and the cold room is whole sooner (13.9 against 17.2 s).

## The goldens and the style §26.2 checklist

Every golden was opened and judged against its concept panel of `example.png` (1 the hub, 2 the SNES room, 3 the
interaction, 4 the PlayStation room, 5 the arcade, 6 the handhelds; a hall has no panel of its own: the hub's portals
are the nearest). The measures are I6's (step 3 to step 6: at 768 x 432, the panels without their captions): the
median luma, the median of the top quarter (the upper walls and ceiling, style §4), the calm, colour and accent shares
(the 70/20/10 rule, style §3.1) and the pixels below luma 12. Every golden is rendered from an original fixture
(invented titles, procedural covers and clips), so §26.2's eighth question ("credible without commercial assets") is
answered by every one; its tenth (the Low view keeps the same priorities) is not covered by the goldens, which use the
default profile (`gpu.world look: ...` checks the light levels of both profiles).

| Image | Panel | Median luma | Top quarter | Calm / colour / accent % | Luma < 12 % |
|---|---|--:|--:|---|--:|
| concept panel 1 (hub) | | 42.0 | 33.3 | 88.8 / 5.5 / 5.6 | 6.6 |
| concept panel 2 (SNES) | | 38.8 | 24.2 | 79.5 / 13.0 / 7.5 | 17.2 |
| concept panel 3 (interaction) | | 35.0 | 39.9 | 77.5 / 11.5 / 11.0 | 16.0 |
| concept panel 4 (PlayStation) | | 34.8 | 26.5 | 82.4 / 13.0 / 4.6 | 13.8 |
| concept panel 5 (arcade) | | 27.4 | 23.8 | 57.1 / 30.7 / 12.1 | 27.1 |
| concept panel 6 (handhelds) | | 54.8 | 53.2 | 85.6 / 8.8 / 5.6 | 6.9 |
| G01 `m0_testroom` | 2 | 47.3 | 13.6 | 93.6 / 5.1 / 1.4 | 11.8 |
| G02 `g02_snes_room` (G10 alike) | 2 | 37.1 | 22.2 | 94.0 / 5.3 / 0.7 | 1.3 |
| G02s `g02s_snes_stress` | 2 | 41.4 | 19.0 | 94.0 / 5.6 / 0.4 | 2.9 |
| G06 `g06_snes_single` | 2 | 37.1 | 22.2 | 93.6 / 5.4 / 1.0 | 1.3 |
| G07 `g07_inspect_card` | 3 | 42.1 | 47.3 | 95.2 / 1.6 / 3.1 | 2.3 |
| G08a `g08a_crt_frozen` | 2 | 79.5 | 102.5 | 91.7 / 1.3 / 7.0 | 0.5 |
| G08b `g08b_crt_fallback` | 2 | 81.3 | 102.5 | 91.4 / 1.1 / 7.5 | 0.3 |
| G11 `g11_lobby` (the hub) | 1 | 41.3 | 37.5 | 94.1 / 4.7 / 1.2 | 4.1 |
| G12 `g12_jewel_case_room` | 4 | 47.2 | 26.0 | 96.2 / 3.0 / 0.8 | 0.9 |
| G13 `g13_shared_room` | 6 | 49.4 | 33.5 | 96.7 / 0.7 / 2.6 | 0.7 |
| G14 `g14_hub_entrance` | 1 | 35.2 | 33.6 | 90.3 / 8.6 / 1.1 | 5.6 |
| G15 `g15_arcade_room` | 5 | 30.3 | 19.1 | 74.6 / 18.6 / 6.8 | 4.4 |
| G16 `g16_handheld_room` | 6 | 52.0 | 35.0 | 96.5 / 0.5 / 3.0 | 0.9 |
| G17 `g17_optical_era_room` | 4 | 28.2 | 16.9 | 97.1 / 1.8 / 1.1 | 3.4 |
| G18 `g18_handheld_hall` | (1) | 47.4 | 72.0 | 95.2 / 3.5 / 1.2 | 3.3 |

The checklist (style §26.2): **1** recognisable as a retrogaming space without the title; **2** at least one strong
scenic element; **3** the readable covers real and not deformed; **4** where to enter and where the games are is
clear; **5** wood and warm light make the areas coherent; **6** the neon does not burn the models' details; **7** the
CRT is a physical object, not a floating video plane; **9** the props do not hinder finding a game (8 and 10 above).
"yes", "partly" (said why), "n/a" (not in the view).

| Golden | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 9 | Against its panel |
|---|---|---|---|---|---|---|---|---|---|
| G01 the test room | yes | yes: the CRT corner, the 16-BIT CORNER neon | yes | yes | yes | yes | yes | yes | Gate A's look development room, unchanged: shelves of generated cartridges, a CRT on its desk, rug and armchair; the upper walls dark (top quarter 14). |
| G02, G06, G10 the SNES room | yes | yes: the neon, the CRT and its console | yes: every cover fitted to its label | yes: the doorway's view, the cases ahead | yes | yes | yes | yes | Panel 2's brightness (37 against 39) and warmth, far calmer (accent 0.7 against 7.5%): 50 titles all face-out in two cases leave the walls bare where the panel has walls of boxes and posters. G06's one ROM shows its generated label in the case. |
| G02s 2,000 titles | yes | yes | yes | yes: the side walls of spines | yes | yes | yes | yes | The closest to panel 2's walls of games; spines too small to read from the doorway, as the panel's. |
| G07 the inspected item | yes | yes: the item held with its card | yes | yes: Avvia, Vedi media, Informazioni, Indietro | yes | n/a | n/a | yes | Panel 3's card and actions; the item in Focus on its shelf rather than in a hand before the CRT. |
| G08a, G08b the hero CRT | yes | yes | yes (G08b: the cover on the screen) | n/a | yes | n/a | **yes**: a CRT with its bezel and depth on its cabinet, the console beside it, a lamp | n/a | The panel's TV corner; ours brighter up close (the lamp beside it). |
| G11, G14 the hub | yes | **yes**: the glowing globe in its railed showcase, the balcony, the chandelier | n/a (G14: the showcase's proxies, small) | yes: the ARCADE portal, the elevator's and the entrance's signs, the totem | yes: walnut columns, oak floor, rugs | yes | n/a | yes | Panel 1's composition (landmark, balcony, portals, sofas, rugs), as bright (41 and 35 against 42); calmer (accent about 1% against 5.6%): the panel's four brand neons are four rooms' portals, the fixture's ground floor has one room. |
| G12 the psx room (gen5) | yes | yes: the CD HOME neon, the bean bags | yes: square and portrait covers fitted to the square label | yes | yes | yes | yes | yes | The 90s collector's pack (look.md: panels 2 and 4): warm walls and colourful geometric posters, brighter than panel 4 (47 against 35) and without its blue. |
| G13, G16 the handheld rooms | yes | yes: the counters of lit handhelds, the neon and pictogram | yes: a card per empty slot, the covers fitted | yes | yes | yes | yes | yes | Panel 6's brightness (49 and 52 against 55) and its low glass counters; calmer (colour under 1% against 9%): its colour is its walls of coloured boxes and a brand neon, our handheld cartridges are small and dark. |
| G15 the arcade | yes | **yes**: rows of cabinets with lit marquees, the ARCADE neon, magenta and cyan coves | partly: the flyers on the screens are small from the doorway | yes: the aisle to the hero cabinet | partly: wood floor and wainscot under a magenta and cyan room, as the panel | yes: the coves wash the walls, the cabinets keep their side art | yes: the hero cabinet carries the clip on its screen | yes | Panel 5's brightness (30 against 27); less colour (19 against 31%) and accent (7 against 12%): the panel's screens show full-colour action and its walls are covered in art. |
| G17 the optical-era lounge (gen6) | yes | partly: the blue DVD HOME neon and the lit glass cases, in a dim room | yes: the keep cases' portrait covers fitted | yes | partly: the wainscot, floor and lamps are warm, the dark blue upper walls fall to near black under the ceiling | yes | yes | yes | Panel 4's dark walls and glass, but darker (28 against 35, top quarter 17 against 27) and far calmer (colour 1.8 against 13%, blue 5 against 19%): the panel's large posters, blue-lit shelves and lit sofa; ours shows the sofa's plain dark back from the doorway and its posters on the front wall behind the walker (BACKLOG). |
| G18 the handheld hall | yes | yes: the signed portals and the elevator under the HANDHELDS sign | n/a | **yes**: both doorways lit and signed, the rooms' interiors seen through them, never black | yes: warm plaster, wainscot, beams, sconces, runner | yes | n/a | n/a | No panel of its own; it has panel 1's language (signed portals, warm wood, a runner), brighter (47; the beams and plaster lit by the sconces). |

Overall: every golden reads as the same warm museum of games, and each era is told apart by its pack (materials,
neon, furniture, light): the classic rooms' warm magenta, the 90s collector's posters and bean bags, the optical era's
dark blue and glass, the handhelds' ivory counters, the arcade's after-dark coves. Against the concept the rooms are
calmer: the panels' colour comes from commercial art (box walls, posters, brand neons, game screens) that Retroverse
does not ship; ours from the user's covers, which the fixtures' procedural covers stand in for.

## Limits still open

From the step notes and `BACKLOG.md` ("The look (the styling pass, I6)"), none of them a failed check:

- **Colour against the concept.** The rooms' colour and accent shares stay below the panels' (above); the
  optical-era lounge is the furthest (G17: dark, its upper walls near black, its sofa a plain back from the doorway).
- **The arcade** holds 53 titles at most (more are unplaced for capacity, ADR-0014 §6 amended); slim cabinets (0.4 m
  deep, the template's sockets); generic marquees; the screens show the cover (one decoder until I5); the light
  probe through its doorway passes with a thin margin (21.2 against 21.0).
- **The handhelds** stay calmer than panel 6; a station in a dark corner stays dim.
- **The hub**: no stairs to the balcony; the showcase's proxies are keep cases; its accent share is one portal's.
- **The halls**: the sconces light nothing but their wash; the runner's rugs show their borders; one print palette.
- **The spines**: a printed spine is squeezed along the jewel case's length; a room kept by an older template is not
  migrated until a regeneration; face-out items still upload a spine layer they never draw.
- **Light through doorways**: a hall with several doorways lights its rooms' walls near a doorway unclipped.
- **The ride's covers in view** (I4): cold, the view comes back by the 3 s timeout with the largest room's covers
  still arriving.

## Walk the building

As [gate-c.md](gate-c.md), "Try the building" (the commands and the controls are the same). What to look at:

- **The hub** from the elevator and from the entrance: the globe readable at once, the portals signed, warm but not
  dim; the totem ("Indice") opens the directory; a proxy of the favourites' showcase opens its game's card.
- **A hall** of each floor looking down it: the rooms glowing through their doorways, the portals' signs, nothing in
  the passage; no light popping when crossing a doorway or turning slowly near one.
- **A room of each pack** beside its panel: the 16-bit rooms (gen2 to gen4), the 90s collector (gen5), the optical
  era (gen6 and gen7: the darkest; is it too dark?), the computers' study, the handhelds; the shelves lit inside, the
  cases and boxes standing with their spines upright.
- **The arcade** from the hub's portal and inside: the rows of cabinets, the hero cabinet's video, darker than the hub.
- **The migrated rooms**: the notice at the first start of this build on a building planned before I6 step 4, the
  cases standing on their side afterwards, nothing else moved.
- **The rides** to the busiest floor cold and warm: the view back within a few seconds, the last covers arriving.
