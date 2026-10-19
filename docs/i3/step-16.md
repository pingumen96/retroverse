# I3 step 16: look development of the generated room

Roadmap row: I3 step 16 (wave 5, after 10c, 12 and 13a; steps 14b and 15a ran beside it and touch the app's interaction
and UI wiring: this step keeps to assets, `rv_world`'s scene data, the prefabs and the materials, and one hook in
`rv_app`, below). Style §4.1, §4.2, §5.3, §6, §8.1 (two lighting profiles), §13.4, §14.1, §15, §26.2; the open notes of
steps 1b (the display glass's haze, the dressing's materials), 3 (only posters fitted the side-wall sockets), 10a (the
lighting profile and the theme's materials in code, the sign's size, the display cases' empty levels), 12 (near-black
spines from the covers' black frame) and 13b (the held cover's darks washed out by the label's sheen). Golden G01 (the
test room) is unchanged: every material the test room uses is untouched, the generated room takes new ones.

## What was built

| Where | What |
|---|---|
| `assets/world/lighting/evening.json`, `afternoon.json` (new) | The two lighting profiles (style §8.1): colour, intensity, range and cones per plan light role, the key's shadow strength, the environment (ambient, exposure, bloom, vignette, SSAO) and **accent lights** of the look alone, anchored on the hero corner's parts (the CRT's cool spill, the sign's magenta glow), drawn while the scene's 16 lights leave room. `evening` is the default (tungsten spots, amber lamps, a magenta-tinted picture light on the sign, the upper walls falling to black); `afternoon` a cooler, brighter daylight fill (ambient 0.6, less bloom and vignette, lamps subdued, the upper walls dark grey). |
| `assets/world/themes/classic_16bit_warm.json` (new) | The theme pack's materials per prefab kind and material role (style §14.1), the sign's under `heroParts`, the collection's item under `cartridge`. |
| `rv/world/room_look.hpp`, `src/room_look.cpp` (`rv_world`, new) | `LightStyle`, `LightingProfile` (moved from `scene_builder.hpp`, plus SSAO and `accents`), `AccentLight`, `parseLightingProfile`, `loadLightingProfile`, `lightingProfileIds`, `roomLightingProfile` (world.json's choice, else the default, else the built-in profile, each fallback logged); `ThemeMaterials`, `parseThemeMaterials`, `loadThemeMaterials`, `builtInThemeMaterials` (the fallback, a copy of the file), `themeMaterials(kind)` (the built-in one, which the app's collection instance reads). |
| `rv/world/world_config.hpp`, `src/world_config.cpp` | `WorldConfig::lightingProfile` (world.json's `"lightingProfile"`, default `"evening"`); left out of the input digest like `reduceMotion` (`generation.cpp`), so switching profiles moves nothing. |
| `rv/world/scene_builder.hpp`, `src/scene_builder.cpp`, `src/room_parts.hpp` | `SceneData::lighting` and `::theme` read from the data (`loadSceneData(..., lighting)`); the materials from the theme; the environment's SSAO from the profile; the accents after the plan's lights; **riser cards** on the display cases' empty levels (`detail::riserCard`: a paper card as wide as the span's slots and as tall as their items, leaning on the level's panel, an invented cover), so no display level stands bare (`SceneBuild::undressed` keeps the spans nothing can show). |
| `src/fixture_layout.cpp`, `.hpp`, `src/incremental.cpp`, `generation.hpp` | Decoration that does not repeat the one beside it on its wall (1.2 m) and lamps at least 2.2 m apart while another choice fits (style §13.4: no uniform sequences); generator **1.1** (a minor version: no game moves). |
| `src/data_reader.*` | `ObjectReader::number` (JSON numbers within a range). |
| `assets/world/templates/console_room_9x10.json` | **Version 2**: the sign is `decor.sign.room.generic` (1.6 x 0.4 m, the test room's size, centred at 2.5 m); the decoration choices fit the side-wall sockets (0.9 x 0.4 m): a poster, a slim floor lamp, a narrow cabinet, a plant (version 1's floor lamp, 0.44 m deep, and sideboard, 0.5 m deep, fitted no socket). |
| `src/geometry/src/prefab_plant.cpp` (new), `prefabs.hpp`, `prefab_factory.cpp`, `prefab_manifest.hpp` | The `plant` generator: a rosette of upright sword leaves (two-faced strips, `leaves` and a lighter `leaves_alt`) in a tapered glazed pot with a rim and soil, on a saucer as wide as the leaves' reach (exact bounds); seeded; its bounds collide. |
| `assets/manifests/prefabs/` (new), `families.json` | `decor.plant.snake.generic` (family `decor.plant`), `decor.lamp.floor_slim.generic`, `furniture.tv_cabinet.narrow.generic`, `decor.sign.room.generic` (family `decor.sign`). |
| `assets/materials/base/materials.json` | New: `MAT_Floor_Parquet` and `MAT_Wood_Varnished_Dark` (the oak planks and the varnish less orange: under tungsten they read deep brown, as in the concept, and below the sign's and the CRT's saturation), `MAT_Screen_CRT_Room` (the tube at 1.3 instead of 2.0, so that the tonemap keeps its colours), `MAT_Fabric_Display` (the cases' lining), `MAT_Paper_Magazine`, `MAT_Book_Cloth`, `MAT_Book_Cloth_Alt`, `MAT_Ceramic_Glazed`, `MAT_Soil`, `MAT_Plant_Leaf`, `MAT_Plant_Leaf_Light`, `MAT_Label_Cover` (the collection's covers and spines, roughness 0.9: step 13b's sheen). Changed (unused by the test room): `MAT_Glass_Display` (dark, alpha 0.07: no haze), `MAT_Cardboard_Box` (darker kraft). |
| `rv/assets/cover_fit.hpp`, `src/cover_fit.cpp`, `rvtex.hpp` | `dominantColour` prefers colourful bins (channel spread at least 48, brightest channel at least 64) when they hold 15% of the cover: the spines take the cover's colour, not its black frame; `kCoverPipelineVersion` 2 (the cache names new entries). |
| `src/app/src/world_build.cpp` (hook) | `buildScene` passes `world::roomLightingProfile(paths.assetRoot, paths.configDir)` to `loadSceneData`. |
| `tools/asset-preview/src/preview_scene.cpp` | The plant's materials in the preview. |

## Probe checks (`tests/smoke/test_world_look.cpp`, `gpu.world look: ...`, `[world_look]`)

The snes-room fixture imported once; for each profile (world.json written between the runs; the second run reports the
first plan `unchanged`, the same digest), a doorway screenshot and one 0.9 m from the first display case's first item:

| Probe | evening | afternoon |
|---|---|---|
| hero wall luminance (its modules over their height, the wall behind the hero corner; linear) | 0.102 | 0.089 |
| every module's eye band (1.1 to 1.5 m) at least a third of it (mean of the bands) | 0.078 | 0.073 |
| the upper walls (2.6 to 3.0 m) darker than the bands | 0.0028 | 0.0051 |
| chroma (90th percentile): sign / CRT screen / the most saturated other tile, the collection's items aside | 122 / 111 / 102 | 122 / 116 / 87 |
| behind the glass: every cover's 5th percentile luma at most 70 and a spread of at least 60 | yes | yes |

The collection's items are left out of the saturation probe: the covers are the user's pictures, as saturated as they
come; the sign and the screen are what the room itself makes stand out. Before the tuning, the varnished back panels
and wainscot under the warm spots (sRGB 179, 91, 44) out-saturated the CRT (p90 chroma 124 against 93).

## Screenshots (Debug, 1920 x 1080, the snes-room fixture, under `build/step16/`)

- `before_doorway.png`, `before_hero.png`, `before_side.png`: main's assets with this build (`--asset-root`; the theme
  and the profile fall back to the built-in ones, `MAT_Label_Cover` shimmed into the copy): every side-wall socket a
  poster, the portrait sign, the hazy display glass, orange varnish and floor.
- `after_evening_*.png`, `after_afternoon_*.png`: the doorway, the hero corner (`--spawn -0.3,0,-1.6,0,-6`) and the
  south-east corner (`--spawn 2.6,0,1.6,-130,-8`).
- The probe runs' own shots: `build/msvc-x64-debug/smoke/world_look/profiles/{evening,afternoon}_{doorway,display}.png`.

Against `example.png`'s SNES room: deep brown wood and warm light, the neon sign and the CRT the most saturated
elements, a magenta accent around the sign, the upper walls in shadow, the walls varied (cabinet, lamp, poster, plant)
instead of a row of posters, the cases' covers clear behind the glass. What still differs: the concept's walls are full
of shelves (a 50-title room fills only the hero modules and the displays; the snes-stress room shows the spine-out
modules), and its ceiling carries visible fixtures.

## Style §26.2, pre-answered for the generated room (evening)

- Recognisable as a retrogaming space without the title: yes (CRT on its cabinet, console, cartridges face-out, sign).
- At least one strong scenic element: the hero corner under the key light, the sign.
- Readable covers real, not deformed: fitted without stretching (step 5); clear behind the glass (probe).
- Where to enter and where to find the games: the doorway's view frames the hero wall and both display cases.
- Wood and warm light give coherence: yes, deep brown wood under tungsten in both profiles.
- The neon does not burn the models' details: the sign's bloom stays on its wall (bloom 0.18, glow light 1.5).
- The CRT a physical object, not a floating plane: body, bezel, curvature, its spill on the cabinet and the rug.
- Credible without commercial assets: every prefab procedural, every material original, invented titles only.
- The props do not hinder finding a ROM: decoration only on sockets the collection does not need; no prop in a module.
- The Low view keeps the priorities: the profiles change light, not composition (Low itself is I8's).

## Decisions taken within the step

1. **Profiles and theme are data, with a built-in fallback each.** A broken or missing file never stops the room: the
   built-in evening profile (step 10a's) and theme are used, logged. The built-in theme is a copy of the file
   (unit-tested equal): the app's collection instance and the empty room's shell read it.
2. **Accent lights belong to the profile, not the plan.** The plan's lights stay what the generator placed (their 16
   counted there); accents are added after them only while the scene's limit leaves room, never cast shadows, and are
   anchored on hero corner parts, so they move with the template's data.
3. **The template's version is 2**: its hero corner names another sign prefab and its decoration list changed; a plan
   of version 1 needs an explicit regeneration (ADR-0010 §10), as the version rule says.
4. **The decoration rule is generator 1.1**: neighbours compared by their sockets' anchors on the same wall; the update
   path passes the plan's fixtures, so growth decorates the new sockets by the same rule.
5. **New materials rather than changed ones** for everything the test room uses (floor, varnish, CRT screen, labels): G01
   stays to the byte.
6. **The spine colour rule lives in `dominantColour`**: the front layer's stored dominant colour is what spines start
   from, so the rule needed the pipeline version raised (old cache entries are never read).

## Tests

- `tests/unit/world/test_room_look.cpp` (new): both profiles read (two shipped, the default evening, warm keys), a
  broken one refused at its line, world.json's choice and its fallbacks, the canonical configuration with the profile,
  the theme's materials all in the library, every decoration fitting a side-wall socket.
- `tests/unit/world/test_plan_scene.cpp`: the lights with the shipped profile and its accents; the empty room's display
  levels carry riser cards (none bare).
- `tests/unit/geometry/test_prefabs.cpp`: the plant in every kind table, its bounds and seed variation.
- `tests/unit/assets/test_cover_fit.cpp`: a black frame gives way to the cover's colour, and stays under 15% of colour.
- Golden plans (`tests/fixtures/world/golden`) and scenes (`tests/fixtures/world/scene`) rewritten with
  `RV_UPDATE_GOLDEN=1`: template version 2, generator 1.1, the sign's size, the new decoration and the scenes' materials,
  lights and riser cards; no placement moved.
- `tests/smoke/test_world_look.cpp` (new): the probes above.

## Open issues

- The light positions are still constants in `fixture_layout.cpp` (step 3's note): moving them into the template is a
  template version of its own, left for when a second template exists.
- The CRT's saturation margin over the room (111 against 102) depends on the clip shown; real clips vary.
- The south half of a 50-title room is dim in the evening profile (the pendant and the decoration lamps only).
- `world_session.cpp`'s empty room (while the plan is made) still uses the built-in evening profile.
- Seen on main after the wave-5 merges, not this step's: `gpu.world inspect: held, turned, zoomed, lowered` expects the
  Back of frame 260 to return to Focus, but since 14b Vedi media (frame 230) opens the media viewer and that Back closes
  it (1 state event of 3). `covers: the snes-stress room ...` missed its whole-room residency once under the machine's
  load (1,570 and 1,986 of 2,000) and passed alone.

## For the merge

- **Hooks outside the step's area**: `src/app/src/world_build.cpp` (one call: the lighting profile from world.json).
  Not done, for the app's owners: `world_session.cpp`'s `shellScene` could pass `roomLightingProfile`; the collection
  instance could read `SceneData::theme.materials("cartridge")` instead of the built-in `themeMaterials`.
- **CLAUDE.md**: "Running the app": `<user data>/config/world.json` takes `"lightingProfile": "evening" | "afternoon"`
  (`assets/world/lighting`); the room's materials come from `assets/world/themes/classic_16bit_warm.json`. GPU smoke
  tests: `gpu.world look: ...` (`tests/smoke/test_world_look.cpp`). Layout: `rv/world/room_look.hpp`. Prefabs: `plant`.
- **docs/architecture.md**: `rv_world`'s `room_look` (profiles and theme as data).
- **docs/roadmap.md**: row 16 done.
- **BACKLOG.md**: "Light positions in the template's data"; "The empty room in the chosen profile".
- Stored plans of template version 1 (the real library's under `build/`) need `worldplan generate --replace`.
