# I4 step 2: systems, families, the building fixture

Roadmap row: I4 step 2. Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §2, §3, §4 (and §8's census).

## What was built

- **Every system classified** (`assets/world/systems.json`): the 195 systems of ES-DE 3.4.1's `es_systems.xml`, 69
  home, 24 handheld, 24 arcade, 44 computer, 31 other and 3 excluded, each with its generation (Wikipedia's
  generations), media family, entry order, generic sign and ES-DE platform tags. The names alone are test data
  (`tests/fixtures/world/esde-3.4.1-systems.txt`); a unit test keeps the table total over them and nothing else.
- **The media families** (`assets/world/media_families.json`, `rv/world/media_family.hpp`): eight families join
  `cartridge_home_16bit`, each with its item manifest and its four fixtures (40 manifests). A family also states its
  label quad, which `checkItemManifest()` proves by building the item, and its cover fit. `withFamilyFixtures()`
  stands a family's fixtures in the template's sockets wherever a room's data is read (`loadWorldData`,
  `loadSceneData`), so capacity, layout, the update and the plan's fixture refs follow the family.

  | Family | Item (cm) | Label (mm) | Side module | Hero module (band) | D / B |
  |---|---|---|---|---|---|
  | cartridge_home_16bit | 12.5 x 8.5 x 2.0 | 101 x 61, I3's fit | 7 levels x 33 | 7 (level 5) x 33, 6 | 44 / 12 |
  | cartridge_home_8bit | 12.0 x 13.3 x 2.0 | 100 x 113 | 8 x 33 | 7 (4) x 33, 6 | 44 / 12 |
  | cartridge_home_64bit | 11.6 x 7.6 x 1.8 | 96 x 60 | 8 x 36 | 8 (5) x 36, 6 | 44 / 12 |
  | cartridge_handheld | 5.7 x 6.5 x 0.8 | 47 x 53 | 12 x 66 | 12 (8) x 66, 12 | 88 / 24 |
  | case_handheld | 12.5 x 14.0 x 1.4 | 115 x 130 | 8 x 44 | 7 (4) x 44, 6 | 44 / 12 |
  | disc_case_cd | 14.2 x 12.5 x 1.0 | 120 x 120 | 7 x 56 | 7 (4) x 56, 5 | 38 / 10 |
  | disc_case_dvd | 13.5 x 19.0 x 1.4 | 123 x 178 | 7 x 44 | 5 (3) x 44, 5 | 38 / 10 |
  | big_box | 19.0 x 24.0 x 4.5 | 180 x 230 | 5 x 16 | 4 (2) x 16, 4 | 28 / 8 |
  | generic_case | 13.0 x 18.0 x 1.5 | 118 x 168 | 8 x 41 | 6 (4) x 41, 5 | 38 / 10 |

- **Covers fitted to the label's shape** (`rv/assets/cover_fit.hpp`'s `LabelAspect`): I3's rules (never stretched,
  at most 8% cropped, else bands) decided against the label quad's shape, then one resample into the 256 x 188 layer;
  the generated front label laid out on a canvas of that shape (`labelCanvas()`) and resampled likewise; the hero
  picture made at the label's aspect. The front cache keys gain the shape; `kCoverPipelineVersion` is unchanged. The
  app's cover requests carry the room's family's shape (`CoverItem::label`).
- **`--room-system <system>`** shows another system's generated room (its family's items, fixtures and covers), until
  step 5a's floors and `--start-room`. The GPU cases `gpu.world families: ...` open the building fixture's jewel-case
  room (40 covers fitted to the square label, their cache entries named with the shape, the band's fronts showing
  their pictures) and its big-box room without media (20 generated labels, no console).
- **The building fixture** `tests/fixtures/esde/building` ([README](../../tests/fixtures/esde/building/README.md)):
  nine systems (mame, snes, psx with two multi-disc games, gb, pokemini, a custom `pocketlab` with gb's platform tag,
  amiga without media, kodi excluded, megadrive in `additions/`), 180 entries. The collection generator gains
  `coverSizes`, `covers` and `from`, so that an overlay adds titles with the whole gamelist and the new files alone.
  The CLI plans it whole: seven console rooms on five floors with their families; after `additions/` an update places
  34 titles and moves nothing.

## Decisions

- **Classification.** Home consoles by first release; the principal systems of a generation first in `order`, then
  their variants and add-ons by maker. `neogeo` is a 4th-generation home console (ADR-0013 §3 lists it among the
  shared rooms, not the arcade room). `switch` is a home console of generation 8 in handheld cases. The pinball and
  laserdisc systems are arcade. `sgb` is a handheld. LCD games, the 8-bit computers and every `other` take the generic
  case. The rare formats take the nearest family: the fds disk cards the handheld cartridge, the HuCard consoles
  (`pcengine`, `tg16`, `supergrafx`) the jewel case their cards came in, `n64dd` and `vsmile` the 64-bit cartridge.
- **The platform-tag fallback** takes the first row in the file's order, and the rows stand in the floors' order. A
  custom `pcwindows` system therefore takes `windows9x`'s row (a computer, big boxes), not `windows`'s (PC shortcuts):
  both stand on the computer floor.
- **The fixtures' rule.** Each family's fixtures keep the 16-bit ones' widths, heights, plinth, boards, gaps and
  margins. The side module holds the most levels whose compartments fit the item spine-out with its 2 cm headroom; the
  hero module the most that also fit it face-out, the band on the level whose items' centre is nearest the 1.3 m eye
  height. The display case is the 16-bit one at the item's face-out size; the wall shelf grows to a leaning item.
- **The spine label keeps the spine texture's 40 x 256 aspect** on every item: a thin item's label is short (the
  handheld cartridge's 4.8 x 30.7 mm) rather than its text stretched.
- **The front keys name the shape, not the family** (ADR-0013 §4 says "the family id"; the task allowed either): the
  pixels depend on the shape alone, so two families of one shape share their entries. The shape is the label in lowest
  terms ("1x1" for the jewel case), one more key element, only for a shape other than the layer's: the 16-bit
  cartridges' entries keep their names and warm caches stay warm.
- **The 16-bit exception is data**: `coverFit: "layer"` runs I3's code. Its labels, keys and hero pictures are I3's to
  the pixel (the hashed layers of `test_collection_labels.cpp` did not move).
- **The inputDigest** adds a family's fixtures only when it names any. The 16-bit family's rooms hash as before, and
  the label and cover fit (which only draw a room) are not hashed.

## The capacity census (ADR-0013 §8)

Each family's room capacity with the reserve (`test_world_families.cpp`; ADR-0010 §5's way:
`countCollection` over the template's sockets), against the reference library's placeable titles per system
(`build/i4-survey/media-survey.json`'s `estimateB`, the room selection's rule; for a system without any cover, every
game's title) by the family of this step's `systems.json`.

| Family | 9 x 10 m | 16 m | Reference systems (placeable titles) |
|---|--:|--:|---|
| cartridge_home_16bit | 3,205 | 5,615 | snes 890, megadrive 779, neogeo 0 (2 titles, no cover) |
| cartridge_home_8bit | 3,606 | 6,361 | nes 2,217, atari2600 563, mastersystem 413, colecovision 273, intellivision 155, atari5200 95, odyssey2 82, atari7800 60, channelf 27 |
| cartridge_home_64bit | 3,993 | 6,998 | atarijaguar 73, n64 2 |
| cartridge_handheld | 11,001 | 19,266 | gba 1,690, gb 1,392, gbc 1,107, fds 188, atarilynx 97, virtualboy 60, ngpc 24, pokemini 11 |
| case_handheld | 4,793 | 8,466 | nds 107, psp 12, switch 0 (229 titles, no cover) |
| disc_case_cd | 5,398 | 9,488 | psx 1,401, dreamcast 82, amigacd32, pcengine, saturn, segacd 1 each, pcfx 0 (11 titles, no cover) |
| disc_case_dvd | 4,097 | 7,311 | ps2 2,134, gc 450, ps3 6, wii 2 |
| big_box | 1,088 | 1,923 | amiga 1,055, atarist 295, dos 7, pc 0 (21 titles, no cover) |
| generic_case | 4,391 | 7,813 | amstradcpc 1,646, atari800 1,207, apple2 494, c64 408, mame 36, vic20 2, windows 1, trs-80 0 (178 titles, no cover) |

- **No reference system overflows a room grown to its limit**, nor even the 9 x 10 m room: the closest is amiga, 1,055
  titles in a big-box room of 1,088. Per ADR-0013 §8, **expansion rooms move to I4b**; `unplaced: capacity` stays. (For
  information, under `placement.include: all` c64's 10,734 titles and amiga's 3,294 would overflow; snes 3,934, nes
  3,767 and gb 3,051 would fit grown rooms.)
- **The shared rooms** (fewer than 30 distinct titles over all games, ADR-0013 §3): channelf 28 (gen2), pc 21
  (computer), ps3 13 and wii 7 (gen7), psp 12 and pokemini 11 (handheld), pcfx 11 (gen5), neogeo 2 and segacd 1
  (gen4), windows 1 (computer): the ten systems ADR-0013 names. mame is the arcade room's section. The other 37
  reference systems get their own rooms.

## Proof by eye

An invented library under `build/step2/eyelib` (700 psx titles with square and portrait covers, 300 amiga big boxes),
imported into `build/step2/data-eye`, planned with `retroverse-cli worldplan generate`:

- `worldplan show --scene` of each room, rendered with `retroverse.exe --scene` (`build/step2/static-psx-doorway.png`,
  `static-amiga-doorway.png`): the families' fixtures, with the big boxes' 5-level side modules, the 4-level hero
  modules, the deeper wall shelf; no console in the computer room's hero corner.
- The same rooms in the app with their covers, `--room-system psx` and `amiga` (`build/step2/app-<room>-*.png`): from
  the doorway, shelves full of jewel cases and of big boxes, face-out on the hero band, the wall shelf and the display
  cases, spine-out in the modules. Up close (`--spawn` before a hero module, a side module and a display case): every
  spine a text strip (the big boxes' titles read at 1 m, the jewel cases' small), square covers square on the jewel
  cases, portrait covers banded, the big boxes' covers whole; no item through a board, every item within its
  compartment, the hovered item pulled out as in I3.

## Deviations and limits

- `--room-system` is an interim option of this step, for the proof by eye; step 5a's `--start-room` replaces it.
- A face-out item shows its spine label on its top face, upside down seen from the front: visible on the big boxes'
  45 mm tops (I3's cartridges are 2 cm deep). A face-out item without a top label is the styling pass's.
- The jewel cases' spine labels are 7 mm wide, the handheld cartridges' 4.8 mm: strips with titles too small to read
  beyond arm's length.
- Shared and arcade rooms are step 4's: in this step every system of a building, mame and pokemini included, gets its
  own console room.

## Follow-ups

- **Step 3:** floors of these rooms; **step 4:** the shared and arcade rooms with `generic_case`.
- ADR-0013 §4's "front cache keys gain the family id" reads "the label's shape" (above): to align at Gate C.
- The styling pass: face-out items without a top label, side-face spines for cases and boxes, faithful families.
