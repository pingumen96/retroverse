# I12 step 2: the gallery

ADR-0021 §2 (as amended by this step); roadmap I12 step 2. The upper walls of a room held only the step 1 damask. Now a
framed picture hangs over each module, under the picture rail, and it shows the user's own media of the room's games:
fanart as posters, 3D boxes (else covers) as portraits, marquees as lit lightboxes, screenshots on a mat. Every other
frame has a picture light. A frame without a medium keeps the pack's print, and "Galleria della stanza" turns the
gallery off.

## What was built

- **The frames** (`world/gallery.hpp`, `scene_builder.cpp`'s `addGallery()`), at scene build from the pack's
  `roomDressing.gallery` (room_look.hpp): one `poster_frame` over each hero and side module against a wall, its kind
  drawn by the pack's weights from the room's seed and the fixture's id. The pictures are 704 x 396 (poster), 480 x 600
  (portrait), 780 x 260 (lightbox) and 600 x 450 mm (frame) inside 25 mm bars. A frame hangs between 80 mm over its
  module and 50 mm under the rail (and under a cove), in the band's middle, at most 350 mm over its lowest place. Every
  other frame but the lightboxes gets a picture light: a hood on an arm and its glow on the wall around the frame, a new
  `picturewash` surface (a cone from the hood). Ids: `dressing:gallery:<fixture>`, `/hood`, `/arm`, `/wash`.
- **The packs** (`assets/world/themes`):

  | Pack | Weights (poster, portrait, lightbox, frame) | Frames | Print |
  |---|---|---|---|
  | `classic_16bit_warm` | 4, 2, 1, 3 | walnut, brass lights | `print` |
  | `console_90s_collector` | 4, 3, 1, 2 | oak, brushed lights | `poster_memphis` |
  | `optical_era_lounge` | 6, 2, 0, 2 | dark varnished, graphite lights | `poster_lounge` |
  | `handheld_archive` | 3, 3, 1, 3 | oak, brass lights | `poster_console` |
  | `computer_studio` | 2, 2, 0, 6 | walnut, brass lights | `print` |
  | `arcade_after_dark` | lightboxes only | graphite, no light | `marquee` |

  The handhelds' and the arcade's wall pictures stay over their showcases; over the modules the gallery replaces them.
- **The items files, schema 3** (`room_items.hpp`): a game's screen data also names its fanart, 3D box, marquee and
  miximage, and its text the catalog's developer, publisher and genre for step 5's caption cards
  (`catalog::RoomEntry` reads them from `idx_games`). Schemas 1 and 2 are still read; the building job rewrites the
  files once.
- **The choice** (`app/gallery_pictures.hpp`, GPU-free): when a floor is built, each room's candidates
  (`RoomCollection::gallery`): its placed games in the room's priority order with each kind's medium. A poster takes
  fanart; a portrait a 3D box, else the cover; a lightbox a marquee; a frame a screenshot, else a title screen, else a
  miximage. The frames of a room are walked in the scene's order. Each takes, from the unused candidates with its kind's
  medium among the first twice-as-many-as-frames (else among all), one drawn by the room's seed and its fixture. A game
  appears at most once a room.
- **The picture** (`makeGalleryPicture()`): decoded upright, its transparency composited, fitted inside the frame's
  picture without stretching on a mat of the dominant colour of its opaque pixels, darkened; a screenshot on a mat all
  round; a lightbox's mat nearly black. Made at 1024 x 576, 640 x 800, 1024 x 344 or 768 x 576 pixels.
- **At runtime** (`world_gallery.cpp`, the signs' pattern): the frames of the scene shown found again at each scene
  change; a job per picture on the pool (read, fit, mip chain, BC1 where the GPU takes S3TC); two uploads a frame under
  the new GPU memory kind `galleryTextures`; the picture put on the frame's material as a texture override. The floor
  shown's pictures stay resident; a guard stops asking at 256 MiB of estimated pictures a scene. A medium that cannot be
  shown is logged once without its name (at debug level with it) and its frame keeps its print.
- **"Galleria della stanza"** (`world.gallery`, world.json and settings.json, Impostazioni's world rows): off, every
  frame keeps its print. It only draws: the plan's input digest leaves it out.
- **The run report's catalog section:** `gallery`, `galleryFrames`, `galleryWithMedia`, `galleryShown`,
  `galleryFailed`, `galleryBytes`, `galleryPeakBytes`.

## Tests

- **Unit:**
  - `test_room_dressing.cpp`: the gallery's reader and six refusals; on every golden plan's room a frame over each
    module, of exactly its kind's size, under the rail, turned as its module, with the pack's materials, every other
    one lit; every kind somewhere.
  - `test_gallery_pictures.cpp` (new): the candidates' order and media; the scene's frames and their kinds; the choice
    (among the first candidates, a game once, the seed's, the print where nothing fits); the picture's fit, its mat and
    a marquee's ground; the upload sizes.
  - `test_world_start.cpp`, `test_world_build.cpp`, `test_room_query.cpp`: schema 3's shape and round trip, schema 2
    still read, the texts from the catalog; `test_surface_textures.cpp`: the picture wash's cone;
    `test_era_furniture.cpp`: the arcade's lightboxes under its coves; `test_world_config.cpp`,
    `test_settings_store.cpp`: the new key.
- **GPU** (`test_world_look.cpp`): the snes room's frames all show a medium of its games, none failing, their bytes the
  gallery's GPU memory; with the gallery off the same frames show their prints and nothing is on the GPU for them, and
  each frame's centre differs between the two views.

## Departures

- **Over the modules only.** The plan also hung frames over the low decoration and on the hero wall's sides. A
  decoration's drawn piece can stand taller than its plan prefab (a secondary TV, step 4's tall variants), so its frame
  waits for step 4, which knows the drawn heights; the hero wall's sides keep the sign and the plaque clear.
- **A guard on asking, not an eviction.** Past 256 MiB of estimated pictures a scene the frames keep their prints; no
  room is evicted for another (a real floor stays far under it, below).
- **No overlay fixture.** `esde/snes-room` already has every kind of medium and a truncated screenshot.
- **No disk cache.** A picture is made again at each run; the figures below say whether a cache is worth it.

## Measured

**Gate D's measures** barely move on the fixture rooms' goldens (the median and the top quarter within 2 luma, the
colour and accent shares within 0.6 points of step 1's): from the doorway the frames are small, and the fixtures'
media are invented, low-chroma pictures. The colour the concept panels owe to art comes from the user's own media:
in the real library's rooms the frames show fanart, 3D boxes and covers, and the arcade's lightboxes its marquees
lit (checked by eye on Release screenshots under `build/i12/real/`, never committed).

**Performance** (Release, the reference machine, with Chrome and Task Manager holding about 70% of the CPU):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 3.10 ms | 5.16 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 3.18 ms | 5.73 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 7.51 ms | 9.58 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.77 ms | 6.34 ms | informative |
| The room's catalog read, the largest system (3,835 entries; `[room_query][bench]`) | 40 ms | | 50 ms |

**Memory:** a picture takes about 0.3 to 0.4 MiB on the GPU (BC1 with its mips); a real room's frames 2 to 6 MiB. The
real library's busiest floor, the computers' (ten rooms, 5,314 items), peaks at 606.9 MiB, against the 4 GiB row:
591.5 MiB with the gallery off and 541.0 MiB at step 1 (the difference, about 50 MiB, mostly the frames' prints: a
pattern texture each).

**The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed): the library's
snapshots differ in none of their entries, and the memory rows pass. Its timing rows did not, with the gallery on or
off alike: the lobby shown in 5.1 to 6.1 s against 5 s, the warm ride's view back timed out (2.5 s) with its covers
resident just after, and the lobby's arrays measured on the ride back before the arcade room's covers were resident.
Step 1's run passed them. The gallery's own load was made lighter in the step (at most two
pictures on the pool at once, below every cover in priority, the previous floor's jobs dropped on a ride); the rows
are to be run again on a quiet machine before the gate.
