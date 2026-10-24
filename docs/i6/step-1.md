# I6 step 1: light through doorways

Roadmap row: I6 step 1. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §4 (amending ADR-0013 §9).
The direction: [look.md](look.md) ("Light").

## What was built

- **Neighbour lights** (`rv/scene/scene_areas.hpp`, ADR-0014 §4 as amended after the review). Two areas are
  neighbours where their boxes touch over a face (a room's doorway passage and its hall's interior; `areaNeighbours()`,
  the common face is their portal; a passage stands to its doorway's head, so the portal is the opening). A view packs
  the eye's area's directional lights and key light, then a budget of point and spot lights that does not depend on
  the view: the neighbours' lights that may be seen through a portal (within 10 m, a room's depth, plus their range of
  it) get a share of the 16 (6, at most half, or every slot the area's own lights leave: a hall of 3 lights leaves 13),
  by importance (intensity / (1 + d^2), d the path from the eye through the portal to the light, less for a portal
  beside or behind the eye); the area's own take the rest, nearest the eye (one behind the eye's plane counted farther
  by twice its distance behind it), and the share's slots the neighbours cannot use while their lights lie wholly
  behind the eye. A full floor (every place of a floor's rows holding a room of 15 lights): the hall
  facing a doorway packs 10 of its own and the neighbours' 6, at least 4 of them the faced room's (its key light
  among them); the room facing out its own and at least 3 of the hall's, its doorway's fill among them.
- **Stable.** A light chosen stays while no light left out is nearer by 1 m (own) or twice as important (neighbours'),
  and half a second at least; a light left out is not taken back for half a second; a light wholly behind the eye is
  left out 1 m past its range and taken back at it. A still eye, a 0.4 degree turn to and fro, a 2 cm strafe, a slow
  turn all around and a walk down a full hall toggle no light (unit-tested on a full floor and on the review's spots).
- **Faded.** Every point and spot light carries a weight eased in and out over 0.3 s as it is chosen or left out (a
  light chosen waits for a slot while the 16 hold lights fading out; the key light takes the faintest one's), the key
  light's shadow likewise (the shadow of the key light the eye left fades out, then the next one's fades in). By the
  frame's time, so `--fixed-dt` runs alike; at once at the first frame, at `--start-room`, after a teleport (the
  directory's same-floor teleport, the overlay's "go to" and "reset to the scene camera") and under `reduceMotion`.
  `gl::packLights(lights, LightSelection, environment)` scales each light's intensity by its weight.
- **Clipped to the portal** (`common/lights.glsl`'s `portalVisibility`): a neighbour's light, and the light of an area
  with a single portal (a room, or a hall of one room), carries its portal's rectangle (`PunctualLightUniforms`, 80
  bytes; `LightsUniforms` 1360). A fragment beyond the portal's plane is lit only along a line through the opening,
  with a soft edge (a light taken as 0.25 m wide), so no light shines through the wall beside a doorway: the review's
  ceiling hotspot beside the snes room's doorway (+17/255) is gone, pixel for pixel as with the neighbours cut.
- **Never the key light.** The selection keeps the own lights before the others (`AreaLighting::ownCount()`), and
  only the light `packLights` is given (the eye's area's key light, or the one it left while its shadow fades) casts:
  a neighbour's key spot is packed unshadowed, and a hall, which has no key light, never borrows a room's.
- **Lights wholly behind the eye are left out** (deviation, see below): given the view's direction, a point or spot
  light whose whole range lies behind the eye lights nothing in view, so leaving it out changes no pixel and frees its
  place. The key light always stays.
- **A fill spot per doorway** (`toFloorSceneFile()`): in the hall, 0.9 m before the doorway's middle and 0.3 m below
  its head, aimed 4 m into the room at 1.2 m (the cases and the hero wall rather than a strip of floor), its outer cone
  the opening's half angle seen from the aim point plus a margin (at least 28 degrees), its inner cone 0.7 of it
  (`hall:<floor>/doorway-fill:<room id>`, the hall's area, never shadowed).
- **The hall role** of the lighting profiles (`"hall"`: `light`, `doorwayFill`, `environment`): the hall's ceiling
  lights take it instead of the rooms' pendant style (evening: `#FFD0A0`, 12, range 9 m, against the pendant's
  `#FFD7B0`, 16, 10 m), hung 0.6 m below the ceiling (was 0.3 m: a hot rippled halo on the ceiling); the doorway fill
  `#FFD6AE`, 3.5. The built-in profile holds the same hall and environment as `evening.json` (unit-tested).
- **An environment per area** (scene file `areas[].environment`: exposure, ambient sky, ground and intensity, fill
  colour and intensity, each optional over the scene's). The hall's comes from the hall role; the rooms keep the
  profile's environment. `scene::EnvironmentEase` eases the views from one area's to the next over 0.5 s (smoothstep,
  by the frame's time, so `--fixed-dt` runs alike), at once under `reduceMotion`, at a scene's first frame and after a
  teleport. `SceneInstance::viewEnvironment()` gives the eased terms; the exposure reaches the post pass through it.
  The developer overlay shows and edits the eye's area's terms (an edit in a hall is the hall's).
- **The fill term**: `EnvironmentSetup::fillColor` and `fillIntensity` (scene keys, 0 by default: the reference scenes
  and G01 do not change), an even radiance added to the hemispheric ambient's diffuse part whatever the normal,
  occluded like it (`LightsUniforms::ambientFill`, `common/lights.glsl`'s `diffuseAmbient`); glossy surfaces and the
  CRT's glass reflect the hemisphere alone, so the fill lifts dark walls without a sheen on floors and screens.
- **Upper walls and ceilings no longer black.** In I4 the hemisphere's ground (`#4A4642` x 0.25) lit what faces down:
  the ceiling read about 4/255. Now the ground term is the warm floor's bounce (`#B49C88` x 0.30 in rooms, `#BCA08A` x
  0.45 in the halls, under a dimmer sky `#8A8078`) and an even fill (`#FFE0C8` x 0.22 in rooms, `#FFDCC0` x 0.15 in
  halls): the ceilings and upper walls read warm and dark, the covers not washed out, the halls' floors about twice
  their walls (2.6 times before the review). The halls' terms are less saturated than they were (`#C89A72`,
  `#FFD2A8` x 0.25), the warmth left to the lamps. `afternoon.json` gets the same structure with its daylight values.
- **The run report** (`sceneAreas`): `packed`, the lights block as packed (each light's name, weight and whether a
  portal clips it); `last`, the light of the last view (exposure, ambient and fill intensities as eased, lights packed
  and fading, the sum of the weights); `samples`, the same at the first view and at every frame while something eases.

## Figures

The light case (`tests/smoke/test_world_floor.cpp`, `world_floor_light`): the mean luma (0-255) of the doorway's
opening between eye height and its head, the far side's upper walls and ceiling, with this step's light, with the
same scene with only the neighbours cut (the ambient, the fill spot and the hall role kept) and as I4 lit it (Debug,
1920 x 1080; the case asks for more than 12, twice I4's and a quarter plus 2 more than with the neighbours cut):

| Floor | Room | Room from the hall | cut | I4 | Hall from the room | cut | I4 |
|---|---|---|---|---|---|---|---|
| ground | `area:arcade` | 51.0 | 15.6 | 10.3 | 27.5 | 13.9 | 2.9 |
| gen4 | `room:snes` | 31.1 | 10.9 | 2.1 | 41.7 | 14.8 | 3.7 |
| gen5 | `room:psx` | 32.3 | 11.8 | 3.0 | 41.7 | 14.8 | 3.7 |
| handheld | `area:shared:handheld` | 28.9 | 11.4 | 2.6 | 30.5 | 15.3 | 4.8 |
| handheld | `room:gb` | 28.6 | 10.9 | 2.1 | 31.2 | 16.1 | 5.6 |
| computer | `area:shared:computer` | 32.0 | 11.4 | 2.6 | 41.7 | 14.8 | 3.7 |

The leak probes (the ceilings 2 m and floors 2.5 m beside each doorway inside the room, 1.5 m beside it in the hall,
and the doorway's wall on both sides): equal to the neighbours-cut views to the hundredth on every floor. The review's
ceiling view beside the snes room's doorway (`--spawn 9.5,0,-5,180,35`) matches the neighbours-cut view pixel for
pixel (it read 17/255 brighter).

The review's cases (gen4, Debug):
- **The doorway's threshold.** Walking through it (`floor_walk_into_room.json`), the frame's mean luma reads 43.5 at
  frame 200 in the hall, 43.7 at frame 202 in the room, then rises smoothly to 50.7 at frame 236 (the room's ambient
  eased over 30 frames, its own lights faded in as the hall's fade out); the review measured 41.7 to 51.9 in one
  frame. On gen4 the hall's view and the room's pack the same 16 lights at the threshold (the hall's 3 leave 13 slots
  to the room). Two fresh starts either side of it (`--spawn 7.65,0,-1.45` and `-1.55`) still differ (43.6 and 49.3),
  each at once its area's environment.
- **The slight turn.** From the room's hero corner (`--spawn 8.2718,0,-8.5292,195.5,-20`) turning 0.4 degrees to and
  fro every frame for 2 s: the same 16 lights at weight 1 every frame (the run report's `samples` holds the first
  frame alone), the frame's mean luma 50.8 and 51.0 (the review: a light toggled, 58.8 and 56.9).
- **A full floor** (unit test, the snes room cloned into every place gen4's rows hold: more than 20 rooms of 15
  lights, a hall of a ceiling light every 6 m and a fill per doorway): every doorway seen from the hall packs at least 4 of its room's lights and its key light; a
  slow turn all around, a 2 cm strafe and a walk down the hall toggle no light within half a second.

The gen4 hall's ceiling around its lamp from the elevator: (4, 2, 1) in the baseline, (80, 59, 40) now; the hall's
floor about 2.2 times its walls (2.6 before the review); the snes room's hero wall through the doorway from the hall
55 (29 before the review), the fill spot's floor strip 68 (85).

The benchmark path (Release, `--no-vsync`, 1,230 frames): p50 3.28 ms, p99 4.13 ms (3.21 and 3.98 before the
review), within the acceptable 16.7 and 33.3 ms. The selection is a pass over the eye's area's lights, its
neighbours' and the scene's light states per frame (unit test: well under 200 microseconds a frame in Debug).

## Screenshots

In `build/i6/step1` (from the light case; `*_cut.png` the same view with the neighbours cut, `*_i4.png` lit as in I4,
`*_hall_up`, `*_hall_down`, `*_room_up` the leak probes' views; `walk_f<frame>.png` the walk through the snes room's
doorway, `flicker_f60/61.png` the slight turn, `ceil_new/cut.png` the review's ceiling view), beside
`build/i6/baseline` and the views before the review in `build/i6/step1-before-review`:

- `<floor>_elevator.png`: the hall warm, its ceiling lit around each lamp in a softer halo (the light hung 0.6 m
  below it), its walls a dark warm brown, the floor less glossy; the rooms' interiors show through their doorways.
- `<floor>_<room>_from_hall.png`: through the doorway the room reads as it does from inside: its hero wall lit by its
  key light and washers, the sign's spot, the CRT, the cases; the fill spot lands on the cases rather than a floor
  strip. The `_cut` view shows the same doorway with the room's own light gone: its hero wall dark.
- `<floor>_<room>_looking_out.png`: the hall beyond the doorway lit warm; the hall's light lands on the room's floor
  inside the doorway as a soft-edged pool, nothing on the walls beside it.
- G02 to G13 (regenerated on purpose): the rooms as before the review's fixes, every lamp lit (from the doorway the
  hall's lights, behind the camera, leave their slots to the room), their ceilings warm and dark; G11 the lobby with
  the arcade room's lamp and floor lit through its doorway. Against `example.png` the light has its fill layer and
  reads through the doorways (style §4.1); still darker and plainer than the concept: the halls' and rooms' materials
  and dressing are steps 2 and 3.

## Deviations and follow-ups

- **Lights behind the eye** are left out of the selection, beyond ADR-0014 §4's wording (exact: no light whose range
  lies behind the eye can reach a visible surface); so are a neighbour's lights while its doorway lies behind the eye
  (its light reaches only the floor around the viewer): their slots go to the area's own until they come into view.
  Both with hysteresis.
- **Own lights by distance, not importance.** As the review asked; a light behind the eye's plane counts farther by
  twice its distance behind it, so that a lamp in view is not left dark for a glow behind the viewer.
- **A hall of several doorways lights its own lights unclipped**: from the hall, a room's ceiling just inside its
  doorway may catch a little of a hall light through the wall above the doorway, gone once the eye is in the room
  (where the hall's lights are clipped). A hall of one room, and every room, is clipped. BACKLOG.
- The walls are the palette's dark `#383735`: much more ambient would wash out the covers before the walls read
  brighter. Step 2's hall materials and step 3's packs are where the halls and rooms gain lighter surfaces.
- `retroverse-cli worldplan show --floor` counts the hall's doorway fills among its lights.
- A floor's area boxes: a room's doorway passage now stands to its doorway's head (2.3 m), so that the portal it
  shares with the hall is the opening (the scene files' digests change, no placement).
- `world elevator`'s fade check allows a worst value 24 (was 16) above the dimmed one: a handful of anti-aliased
  values at the elevator doors' foot, higher in contrast with the lifted ambient (6 of 6.2 million, up to 19.5); the
  mean and the brightest dim exactly as before.
- `world look`'s chroma check leaves the posters' prints out of the other tiles, as it leaves the covers (invented
  covers, pictures as saturated as they come), and holds them apart: no poster more than 8 above the CRT (evening:
  posters 112, CRT 110, sign 122, the other tiles at most 109; afternoon: 88, 115, 122, 88).
