# I12 step 5: shelf detail

ADR-0021 §1 (as amended by this step); roadmap I12 step 5. The shelves said nothing of what they hold: a wall of spines
with no letter on it, display cases of covers with no name under them, and a shared room's sections running into each
other under one sign. Now every spine-out level has a letter tab, every face-out title of a display case a caption
card, and every section of a shared or arcade room a sign of its own.

## What was built

- **The labels, GPU-free** (`app/shelf_labels.hpp`, `rv_app_cli`): from a room's collection layout, its plan, its scene
  data and its games' text (`RoomShelfTexts`, gathered by the floor's job from the rooms' items files: a game's title,
  `captionDetail()`'s "year · developer", publisher without a developer, its sort key and initial, its section):
  - a **letter tab** on every spine-out level of a module (prefab kind `shelf`) that holds an item: a brass holder of
    66 x 26 mm round a paper insert of 60 x 20 mm, at the board's left end (the prefab's `Level_<n>` anchor), its top
    3 mm over the board, hanging over its front edge, saying the level's least and greatest initials
    (`letterRun()`: "A–C", "D", "0-9", "#"; by `initialRank()`, so that an editor's move never reads backwards);
  - a **caption card** under every face-out item of a display case: 75 x 28 mm (narrower at the same shape where the
    slots stand closer), hanging from its ledge's lip and leaning with it (the lip's place is the display case's,
    `geometry::kDisplayCaseLipFront`, `kDisplayCaseLipHeight`);
  - a **section sign** over the first fixture of each section of a shared or arcade room: the module, wall shelf or
    display case holding the section's first item by sort key, a lit plate of the hall's (its trim, its sign
    material) up to 50 x 14 cm standing on its top, at a module's front and at a display case's back, sharing the
    fixture's width with the other sections that begin there.
  The floor shown keeps each room's labels (`ShownCollection::shelfLabels`, made by `shownFloorOf()` under the plan
  shown, so a live update, an edit's draft and its commit all bring them along).
- **A prefab kind** `label_plates` (`geometry/prefab_label_plates.cpp`, 39 kinds to 40): thin plates, each with its own
  rigid frame, size and atlas cell, a box of role `plate` with a face of role `picture` in front of it (inset for the
  tab's holder, flush for a card); no collider. Every pack names its two roles (the tabs' metal: brass, graphite in the
  90s room and the lounge, brushed steel in the arcade; the paper `MAT_Paper_Print`).
- **The atlases** (`assets/label_atlas.hpp`): one per room for its tabs (cells of 192 x 64 px, four across) and one for
  its cards (256 x 96 px, two across), each cell inside a 4 px gutter of its own paper so that its mips do not bleed;
  the tabs' text in the bold face from 46 down to 20 px, the cards' title in bold from 32 down to 22 px and the detail
  at 24 px, cut with an ellipsis, a hairline between them.
- **The runtime** (`WorldShelfLabels`, as the plaques): per room of the floor shown an object `tabs:<room>/plates`
  and one `captions:<room>/plates` (one mesh and one material each), a poster frame `section:<room>/<n>` per sign, put
  in the scene through `replaceObjects()` one room a frame and none while a ride waits for its view (every one in a
  lockstep frame: the real library's busiest floor's plates built at once held the frame a ride arrives in for about
  80 ms); the atlases drawn on the pool behind every cover (BC1 with their mips where the GPU has S3TC), two uploaded
  a frame under the new `gl::GpuMemoryKind::LabelTextures`, laid on the objects' pictures; an atlas whose texts are
  the same is kept. The section signs' pictures are `WorldSigns`': their generic one names the section (its system's
  full name, the hall's sign lettering), and the identity's sign goes over it (`IdentitySignPlace::Section`: the
  section's logo, else its identity's name). The run report's catalog record counts them (`shelfTabs`,
  `shelfCaptions`, `sectionSigns`, `shelfAtlases`, `shelfAtlasesShown`, `shelfLabelsFailed`, `shelfLabelBytes`).

## Tests

- **Unit:** `test_shelf_labels.cpp` (the initials, runs and captions' lines; a golden plan's room: a tab per spine-out
  level with its run, at its board in front of its items, a card per face-out item of the display cases under it and
  leaning with it, the same inputs the same labels, no label for unknown text; the room moved on its floor and a
  module moved along its wall, its tabs with it and their texts kept; the room read as a shared room of two sections,
  a sign each over the fixture of its first item, its object and id; the scene objects built, their cells the
  atlas's); `test_label_atlas.cpp` (the layout, the tabs' and the cards' ink, margins and gutters, deterministic);
  `test_prefabs.cpp` (the kind, its roles, bounds, cells, frames and refusals); `test_identity_signs.cpp` (the section
  sign's target and content).
- **GPU** (`test_world_look.cpp`, `[shelf_labels]`): `snes-stress`'s room counts as many tabs and cards as its layout
  has, both atlases on their objects, their bytes the `LabelTextures` kind's; up close (0.7 m) the first tab and the
  first card show ink on their paper; the building fixture's shared handheld room has its two section signs.

- **Two of step 4's GPU checks, found failing** (`test_world_look.cpp`; step 4's last round of it ran before its
  final changes): the gallery's case counted every frame with a medium, but since step 4 frames hang over the low
  decoration too, more than the fixture's one game with fanart, marquee and screenshot can fill (five frames kept
  their prints and one took the fixture's screenshot damaged on purpose); the case now lays the fixture's new
  `gallery/` overlay over it (fanart for fourteen games, marquees for four, that screenshot whole), and its frames
  hidden behind a tall piece from the doorway are not compared. The doorway's chroma check found a bookcase's ochre
  spines as saturated as the CRT (112 against 111): the drawn bookcases, racks and pegboards are held apart with the
  posters' prints (never more than kPosterChromaAllowance over the CRT).

## Departures

- **One object per room** for its tabs and one for its cards (the plan had one per fixture): one mesh, one material
  and one atlas a room rather than thirty; a module moved makes its room's object again with the same atlas.
- **The labels follow the room editor at the end of its session**, not during it: while the editor holds render items
  by index no object is added or taken away (as the plaques, the consoles and the gallery).
- **Section signs on display cases and wall shelves too**: a small shared room holds every title face-out (G13's
  pocketlab and pokemini sections), so its sections begin in its display cases.
- **No section sign in an arcade room without modules**: the cabinets hold no sign.

## Measured

**The goldens:** G02, G02s, G06, G10, G12, G13, G15, G16, G17, G18, G19 and G22 rewritten on purpose, after comparing
every golden first (G10 and G22 compare with G02 and moved with it): the cards under the display cases' titles, the
tabs on the modules, G13's two section signs over its wall shelf; G01, G07, G08a, G08b, G11, G14, G20 and G21 compare
as they were. No scene golden or golden plan moves (the labels are the runtime's).

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 3.15 ms | 4.83 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 3.13 ms | 4.83 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 7.47 ms | 8.71 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.63 ms | 5.57 ms | informative |

**Memory:** the real library's busiest floor (computer, ten rooms, 5,314 items: 201 tabs, 192 cards, 2 section signs)
peaks at 669.1 MiB against the 4 GiB row (654.6 MiB at step 4): its rooms' atlases and the plates.

**The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed): the library's
snapshots differ in none of their 542,121 entries; every row passes but the warm ride's to the computers' amstradcpc
room, whose 1,646 covers in view were resident at 2.26 to 2.38 s on four runs, past the view's deadline (about
2.25 s), so it came back at its timeout; a run with the labels turned off gave the same (2.28 s), and the floor's job
took 1.04 to 1.11 s where step 4's quiet run took 0.92 s (BACKLOG: the warm ride's margin). On the way the labels'
cost on the ride was found and removed: their atlases' jobs now queue behind every cover, and their rooms are placed
one a frame once the ride is over (the floor's plates built at once took about 80 ms of the frame the ride arrived
in). The lobby shown 4.1 s after the start.

**The suites:** the unit suite 2,656 of 2,656; the GPU suite passes but for the two `process_runner` cases
the launch allowlist refuses by design and the two `xr` cases whose `§` the console's code page turns into a filter
that matches nothing (both pass when run by name). It also found that the test room's surface check of
`test_measurements.cpp` had not left out the five surfaces steps 1 and 2 added (made on first request, none in the
test room): fixed here.
