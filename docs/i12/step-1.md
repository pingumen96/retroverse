# I12 step 1: the shell

ADR-0021 §1 (as amended by this step); roadmap I12 step 1. A console room was a plastered box above its wainscot: bare
upper walls, a flat ceiling, one pendant light without a lamp. Now every room pack dresses the room's architecture
(the pack's damask from the wainscot up to a picture rail, a cornice, beams across the ceiling) and hangs its lamps,
a drum near the doorway and spot cans over the display cases; the 90s, handheld and arcade rooms walk on herringbone
parquet, and the packs' seats are woven or grained.
The plan is unchanged: everything is chosen when the room's scene is built, so stored worlds show it at once.

## What was built

- **Four procedural surfaces** (`rv_assets`, `surface_wallpaper.cpp`, `surface_fabric.cpp`, `surface_wood.cpp`;
  1024 x 1024 with base colour, roughness and normal maps, seamless, generated on the first request of a material that
  samples them):
  - `wallpaper_damask`, a 1 m tile: a rosette of eight rounded petals in a roundel, framed by an onion-shaped ogee on
    a 0.5 m lattice with quatrefoils where the frames meet, a grey detail map each pack tints through its material's
    base colour;
  - `herringbone_parquet`, a 2 m tile: blocks about 7 by 35 cm, each with its tint and wear, the joints bevelled;
  - `fabric_weave`, a 0.5 m tile: a plain weave of 320 threads;
  - `leather`, a 0.5 m tile: pebbles, creases and soft wrinkles.
- **Sixteen materials** (`assets/materials/base/materials.json`): `MAT_Floor_Herringbone` and `_Dark`; the packs'
  damasks `MAT_Wallpaper_Warm`, `_Mauve`, `_Slate`, `_Cream`, `_Study`, each its pack's plaster colour divided by the
  damask's mean so that it is as bright and no more saturated; the woven seats `MAT_Fabric_Sofa_Weave`,
  `MAT_Fabric_Teal_Weave`, `MAT_Fabric_Slate_Weave`, `MAT_Fabric_Wool_Green_Weave` and `MAT_Fabric_Leather_Grain`; the
  lounge's `MAT_Cove_Blue` and `MAT_Cove_Wash_Blue`; the pendants' `MAT_Pendant_Shade` (lit from inside) and
  `MAT_Pendant_Diffuser`.
- **The packs' data** (`room_look.hpp`): `roomDressing.shell` names the materials of the wallpaper, the picture rail
  (`railYMm`, 2700 by default), an optional frieze above it, the cornice and the beams (`beamPitchMm`, 3000), each part
  optional; `roomDressing.pendants` names the shade, the diffuser and the metal. The reader refuses an unknown key, a
  height or pitch out of range and a frieze without a rail. The built-in pack is the classic one's copy, its dressing
  included.
- **The packs** (`assets/world/themes`):

  | Pack | Wood | Damask | Pendants | Floor, seats |
  |---|---|---|---|---|
  | `classic_16bit_warm` | walnut | warm brown | parchment, brass | planks; woven red seats |
  | `console_90s_collector` | oak | mauve | brushed metal | herringbone; woven teal seats |
  | `optical_era_lounge` | dark varnished | slate | graphite | dark planks; grained leather; the blue cove at 2.75 m |
  | `handheld_archive` | oak | cream | brass | herringbone; woven slate seats; its cove at 2.75 m |
  | `computer_studio` | walnut | green | green shade, brass | planks; leather, a green wool office chair |
  | `arcade_after_dark` | a graphite cornice only | none | none | dark herringbone; leather |

  The hub's pack dresses halls, not rooms, and is unchanged.
- **The architecture** (`scene_builder.cpp`, `addShellDressing()`), as dressing primitives that neither collide nor
  cast shadows: the damask from the wainscot's top (1.0 m) to the picture rail's bottom, 5 mm proud; the rail 45 mm
  tall and 28 mm deep under 2.70 m; the cornice 90 mm tall and 60 mm deep under the ceiling; the beams 200 mm deep and
  180 mm wide every 3 m from the north wall while 1 m short of the south one. Each band runs corner to corner on the
  north and south walls and between them on the others; on the doorway's wall it stops at the casing and continues
  over it. The paper's texture coordinates are in metres anchored at each piece's place, so a wall's pieces tile on
  without a seam.
- **The lamps** (`addPendants()`), hung from the ceiling or from a beam's underside:
  - near the doorway, 1.5 m into the room, a drum 42 cm wide and 22 cm tall, open and lit from inside, under a metal
    top, closed by a diffuser, on a stem under a canopy, its bottom 1.12 m under the ceiling;
  - over each display case that stands free of the walls, a spot can as the arcade's (11 cm wide, 12 cm tall) of
    the pack's metal, its lens the diffuser 2.35 m up, on a stem: small warm points at the top of the doorway's view,
    as the concept's ceiling lights.
- **The look pendant's light** (`lookPendantLight()`): a point light in the doorway's drum with the profile's pendant
  colour at half its intensity, named `look:pendant`. It comes after the plan's lights and the accents and takes only
  a slot they leave free in the room's 16. The display cases' cans need none: the profile's case lights are theirs.
- **The seats** (`prefab_armchair.cpp`, `prefab_seating.cpp`): the armchair's, the sofa's and the office chair's fabric
  boxes take texture coordinates in world metres, so the weave and the grain keep their scale on every piece.
- **The empty room** (`shellSceneDescription()`) takes the built-in pack's dressing too.

## Tests

- **Unit:**
  - `test_surface_textures.cpp`: the four surfaces' sizes, square texels, seamless tiling, unit normals and their
    tilt; the herringbone's tone, its joints and the damask's ink; the roughness of each; a hidden case
    (`[.surface-dump]`, `RV_SURFACE_DUMP=<folder>`) writes every map as a PNG to look at.
  - `test_room_dressing.cpp` (new): the reader's keys, defaults and six refusals; every shipped pack's materials in
    the library, the arcade's cornice alone; on every golden plan's room the bands against their walls, clear of the
    doorway and its casing and meeting at the corners without overlapping, the beams, the lamps' places and parts,
    under a beam or the ceiling; the look pendant's light last, never in an accent's place, absent from a plan of 16
    lights and from a pack without pendants.
  - `test_material_library.cpp`, `test_gpu_memory.cpp`, `test_plan_scene.cpp`, `test_shell_scene.cpp`: the sixteen
    materials; the surfaces' memory; the dressing beside the fixtures; the light order; the empty room's dressing.
  - The scene goldens (`tests/fixtures/world/scene/`) rewritten on purpose: the dressing in every room's scene, the
    look pendant's light in the smaller rooms.
- **GPU** (`test_world_look.cpp`): the snes room from its doorway holds six pieces of paper, four of rail and of
  cornice, its beams, the doorway's drum and a can over each display case; the look pendant's light is packed for
  the view; the papered walls and the plaster above the rail are never black; the cans' lenses glow far above the
  walls.

## Departures

- **The rail at 2.70 m, not 2.25 m, and the paper below it.** At 2.25 m the rail would leave half a metre of paper
  above the modules and break at the doorway's casing (2.37 m). At 2.70 m the damask covers the wall between and above
  the modules, and the rail runs unbroken over the doorway. Step 2's frames hang under the rail (ADR-0021 §2 as
  amended).
- **No new skirting.** The `room_shell`'s baseboard stays: a second board 18 mm proud would show only between the
  modules and cut into their gap to the wall.
- **Lamps near the doorway and over the cases, not along the axis.** A drum between the doorway and the hero wall
  hides the sign from the doorway once the room grows.
- **Spot cans over the display cases, not drums.** Drums there filled the doorway's view: two large white shapes in
  its upper third, brighter than the neon and the CRT. The concept lights its rooms from above with small warm
  spots, and so do the cans.
- **Papers in their plaster's colours.** The first papers were lighter and up to twice as saturated as the plaster;
  under a floor lamp the classic room's red damask came out more saturated than its neon sign, which the room-look
  check refuses (style §3.1).
- **A rosette, not a palmette.** The first damask's palmette, a fan of seven pointed leaves on a stem, read as a
  cannabis leaf from across the room.
- **One look light, last.** Drawn before the accents, it took a display case's light in the 2000-title room and the
  featured shelf's in the 890-title one. Drawn last, it never does; the larger rooms, whose lights fill the 16, show
  their doorway drum by its glow alone (BACKLOG, "More lights per view"). No light per growth step.
- **New materials for the seats.** The woven and grained looks are new materials the packs' seats take, so
  `MAT_Fabric_Sofa` and the leathers stay plain: G01, G21, G11 and G14 are unchanged. The weave is 1024 pixels over
  0.5 m (320 threads), not the planned 512 over 0.1 m.
- **The coves at 2.75 m**, just above the rail; the lounge's cove was planned at 2.98 m, over a gallery band that now
  ends under the rail.
- **No `MAT_Pendant_Bulb` or `MAT_Picture_Wash`.** The diffuser is the bulb seen from below; the picture wash
  is step 2's.
- **The lounge's room in the GPU case.** Only room:snes has a smoke case; the lounge is judged by G17 and its
  measures below.

## Measured

**Gate D's measures** on the regenerated goldens (I6's table: 768 x 432, the panels without their captions), before
the step and after it, beside the concept panel:

| Golden | Median luma | Top quarter | Colour % | Accent % | Luma < 12 % | Panel: median, top, colour, accent |
|---|--:|--:|--:|--:|--:|---|
| G02, the snes room | 37.1, 43.7 | 22.2, 23.8 | 5.3, 6.1 | 0.7, 0.7 | 1.3, 1.1 | 2: 38.8, 24.2, 13.0, 7.5 |
| G02s, 2,000 titles | 41.4, 42.6 | 19.0, 20.0 | 5.6, 5.7 | 0.4, 0.4 | 2.9, 2.4 | 2 |
| G12, the 90s collector | 47.4, 51.0 | 26.0, 29.7 | 3.1, 2.9 | 0.8, 0.8 | 0.9, 0.2 | 4: 34.8, 26.5, 13.0, 4.6 |
| G13, the shared handheld room | 49.4, 51.3 | 33.5, 39.9 | 0.8, 0.8 | 2.6, 2.9 | 0.7, 0.7 | 6: 54.8, 53.2, 8.8, 5.6 |
| G15, the arcade | 30.3, 30.0 | 19.1, 18.4 | 18.5, 17.3 | 6.8, 6.8 | 4.7, 5.4 | 5: 27.4, 23.8, 30.7, 12.1 |
| G16, the handheld room | 51.8, 53.5 | 35.0, 40.8 | 0.6, 0.6 | 3.1, 3.4 | 0.9, 0.9 | 6 |
| G17, the lounge | 28.2, 30.5 | 16.9, 18.6 | 1.8, 3.1 | 1.0, 1.7 | 3.4, 2.3 | 4 |

- **The top quarter** rises in every room but the arcade, whose graphite cornice and dark ceiling stay as they were:
  G02 now matches panel 2, the handheld rooms and the lounge come nearer theirs. The median rises more than any part
  of the view because it falls between the dark upper half and the lit floor; each quarter rose by 2 to 7 luma.
- **Colour and accent** barely move: the shell is the calm 70% of style §3.1. The colour the panels have is their art,
  which step 2's gallery brings.
- **G01, G21, G11, G14 and G20 are unchanged** (compared, within the tolerance); the other fifteen goldens were
  regenerated on purpose and compared again, all twenty passing.

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 2.78 ms | 4.03 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 2.80 ms | 3.43 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz, both eyes at 2048 x 2048 | 7.33 ms | 9.20 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.77 ms | 6.48 ms | informative |

The last gates measured the same paths at a desktop p99 of 4.6 ms and a VR p50 of 7.2 ms at 72 Hz.

**Memory:** the four new surfaces take 48 MiB on the GPU when all are in use (`test_gpu_memory.cpp`: the catalog's
surfaces 133 MiB, from 85); a room samples two or three of them, and only when a material it shows asks for one.

**The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, Release, ES-DE closed): the
tests passed, and the two snapshots of the library differ in none of their 542,121 entries. The busiest floor, the
computers' (ten rooms, 5,314 items), peaks at 541.0 MiB of GPU memory cold and warm, against the 4 GiB row; its
collection arrays take 210.9 MiB of that.
