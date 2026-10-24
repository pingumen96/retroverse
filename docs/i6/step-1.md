# I6 step 1: light through doorways

Roadmap row: I6 step 1. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §4 (amending ADR-0013 §9).
The direction: [look.md](look.md) ("Light").

## What was built

- **Neighbour lights** (`rv/scene/scene_areas.hpp`). Two areas are neighbours where their boxes touch over a face (a
  room's doorway passage and its hall's interior; `areaNeighbours()`, the common face is their portal). A view packs
  the eye's area's own lights first, as before (directional, key light, the nearest the eye), then, while the 16 leave
  room, up to 4 point and spot lights of each neighbour: those nearest the portal, within 6 m of it and within their
  own range (a light that cannot reach the doorway cannot shine through it), the nearest the eye first. Rooms a wall
  apart are never neighbours, so no light leaks through a wall without a doorway.
- **Never the key light.** The selection keeps the own lights before the neighbours' (`AreaLighting::ownCount()`), and
  `gl::packLights(..., keyCandidates)` lets only the own ones be the key light: a neighbour's key spot is packed
  unshadowed, and a hall, which has no key light, never borrows a room's.
- **Lights wholly behind the eye are left out** (deviation, see below): given the view's direction, a point or spot
  light whose whole range lies behind the eye lights nothing in view, so leaving it out changes no pixel and frees its
  place. A room holds up to 16 lights of its own (ADR-0010): without this, from inside a room the hall's lights never
  fit. The key light always stays.
- **A fill spot per doorway** (`toFloorSceneFile()`): in the hall, 0.9 m before the doorway's middle and 0.3 m below its
  head, aimed 3 m into the room at 0.8 m, its outer cone the opening's half angle seen from the aim point plus a margin (at
  least 28 degrees: it lights the room's middle, not the walls beside the doorway)
  (`hall:<floor>/doorway-fill:<room id>`, the hall's area, never shadowed).
- **The hall role** of the lighting profiles (`"hall"`: `light`, `doorwayFill`, `environment`): the hall's ceiling
  lights take it instead of the rooms' pendant style (evening: `#FFD0A0`, 12, range 9 m, against the pendant's
  `#FFD7B0`, 16, 10 m), warmer and more even. The built-in profile holds the same hall and environment as
  `evening.json` (unit-tested).
- **An environment per area** (scene file `areas[].environment`: exposure, ambient sky, ground and intensity, fill
  colour and intensity, each optional over the scene's). The hall's comes from the hall role; the rooms keep the
  profile's environment. `scene::EnvironmentEase` eases the views from one area's to the next over 0.5 s (smoothstep,
  by the frame's time, so `--fixed-dt` runs alike), at once under `reduceMotion` and at a scene's first frame.
  `SceneInstance::viewEnvironment()` gives the eased terms; the exposure reaches the post pass through it.
- **The fill term**: `EnvironmentSetup::fillColor` and `fillIntensity` (scene keys, 0 by default: the reference scenes
  and G01 do not change), an even radiance added to the hemispheric ambient whatever the normal, occluded like it
  (`LightsUniforms::ambientFill`, 1104 bytes, `common/lights.glsl`).
- **Upper walls and ceilings no longer black.** In I4 the hemisphere's ground (`#4A4642` x 0.25) lit what faces down:
  the ceiling read about 4/255. Now the ground term is the warm floor's bounce (`#B49C88` x 0.30 in rooms, `#C89A72` x
  0.35 in the warmer halls) and an even fill (`#FFE0C8` x 0.22 in rooms, `#FFD2A8` x 0.25 in halls): the ceilings and
  upper walls read warm and dark, the covers not washed out. The rooms' ground and fill are kept less saturated than
  the halls' so that warm wood never outshines the CRT (`world look`: the most saturated other tile 104 against the
  CRT's 109; I4 106 against 112). `afternoon.json` gets the same structure with its daylight values.

## Figures

The light case (`tests/smoke/test_world_floor.cpp`, `world_floor_light`): the mean luma (0-255) of the doorway's
opening between eye height and its head, the far side's upper walls and ceiling, with this step's light and with the
same scene lit as I4 lit it (Debug, 1920 x 1080; the case asks for more than 12 and twice I4's):

| Floor | Room | Room from the hall | I4 | Hall from the room | I4 |
|---|---|---|---|---|---|
| ground | `area:arcade` | 27.2 | 10.3 | 30.4 | 2.9 |
| gen4 | `room:snes` | 28.9 | 2.1 | 42.0 | 3.7 |
| gen5 | `room:psx` | 29.7 | 3.0 | 42.0 | 3.7 |
| handheld | `area:shared:handheld` | 29.3 | 2.6 | 32.6 | 4.8 |
| handheld | `room:gb` | 28.9 | 2.1 | 33.3 | 5.6 |
| computer | `area:shared:computer` | 29.3 | 2.6 | 42.0 | 3.7 |

The gen4 hall's ceiling from the elevator: (4, 2, 1) in the baseline, (80, 58, 38) now; a room's upper wall beside
its doorway about (31, 24, 17), its ceiling (45, 32, 21). The concept's ceilings and upper walls read 25 to 90.

The benchmark path (Release, `--no-vsync`, 1,230 frames): p50 3.21 ms, p99 3.98 ms, within the acceptable 16.7 and
33.3 ms. The selection is a pass over the eye's area's lights and its neighbours' per frame.

## Screenshots

In `build/i6/step1` (from the light case; `*_i4.png` the same view lit as in I4), beside `build/i6/baseline`:

- `<floor>_elevator.png`, every floor from its elevator: the hall warm, its ceiling lit around each lamp and no
  longer black, its walls a dark warm brown; the rooms' interiors show through their doorways (a poster, a lamp, the
  floor lit by the fill spot) where the baseline showed a dark gap.
- `<floor>_<room>_from_hall.png`: through the doorway the room reads whole, its floor lit by the fill spot, its
  middle by its pendant, its hero wall, sign, CRT and cases visible; the I4 view shows the same doorway nearly black
  above the floor.
- `<floor>_<room>_looking_out.png`: from inside, the hall beyond the doorway lit warm (its far wall and floor), on
  `handheld` the room across the hall glowing through both doorways (concept panel 1's "partial view of the
  neighbouring areas"); the I4 view shows a black hall.
- G02 to G13 (regenerated): the same rooms, their ceilings warm and dark rather than black, the covers as legible as
  before, no washed-out surface. Against `example.png` the light now has its fill layer (style §4.1), still darker
  and plainer than the concept: the halls' and rooms' materials and dressing are steps 2 and 3.

## Deviations and follow-ups

- **Lights behind the eye** are left out of the selection, beyond ADR-0014 §4's wording. It is exact (no light whose
  range lies behind the eye can reach a visible surface) and is what lets a full room show the hall beyond its doorway.
- **Room lights far from the doorway** stay out of the hall's view (by design: within 6 m of the portal and in range).
  Seen from the hall, a room's hero wall is lit by its pendant, the fill spot and the ambient, dimmer than from inside;
  entering the room switches its far lights on at once (the environment eases, the lights do not).
- The walls are the palette's dark `#383735`: much more ambient would wash out the covers before the walls read
  brighter. Step 2's hall materials and step 3's packs are where the halls and rooms gain lighter surfaces.
- `retroverse-cli worldplan show --floor` counts the hall's doorway fills among its lights.
- `world elevator`'s fade check allows a worst value 24 (was 16) above the dimmed one: a handful of anti-aliased
  values at the elevator doors' foot, higher in contrast with the lifted ambient (6 of 6.2 million, up to 19.5); the
  mean and the brightest dim exactly as before.
- `world look`'s chroma check now leaves the posters' prints out, as it leaves the covers: invented covers, pictures
  as saturated as they come, which the brighter walls show at their own colour (a poster 98 in I4, 115 now, against the
  CRT's 109). The check still guards what it is for: the sign and the screen stand out from every lit surface that is
  not a picture.
