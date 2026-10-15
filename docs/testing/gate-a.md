# Gate A: the M0 test room

Gate A closes the visual Phase A of `style.md` §27: an empty but credible room, with its style and lighting already
recognizable. M0 step 12 tuned `m0_testroom` for it and recorded golden G01, regenerated after the step's review
(rounded CRT picture corners and a fainter glass reflection, cartridge titles in whole words and larger type, the table
lamp's light kept off the floor under the sideboard, shelf boards and cabinet tops with square bottom edges, a lighter
FXAA sub-pixel blend). This page answers the checklist of style §26.2 for that image, lists what the room still lacks
and records the automated checks of M0 steps 14 and 15 (`scripts/gate.ps1 -Gate A`).

**Status: passed** (October 15, 2026, on the reference machine): the checklist below, and every step of the gate
script (see "Automated checks").

## Golden G01

| | |
|---|---|
| Image | `assets/style_tests/screenshots/m0_testroom_golden.png`, 1920 x 1080 RGB |
| Metadata | `assets/style_tests/screenshots/m0_testroom_golden.json`: GPU, driver, preset, the commit of the sources, SHA-256 of the scene file and of the image, the command |
| Scene | `assets/style_tests/reference_scenes/m0_testroom.json`, scene camera (no `--spawn`, no input script) |
| Frame | the last of a 30-frame run (frame 29) |
| Build | `msvc-x64-debug` on the reference machine (`docs/testing/reference-hardware.md`); `msvc-x64-release` writes the same bytes |

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 30 --hidden `
  --screenshot-size 1920x1080 --screenshot build/screenshots/m0_testroom_golden.png --data-dir build/rvdata
```

The scene is static and the run uses no time-dependent effect, so the same build, driver and scene give the same
bytes. Regenerate G01 only when a change alters the look on purpose, and record the new metadata with it. A changed
driver or GPU can move pixels by an 8-bit step or two (dither, filtering, FXAA): compare with a tolerance, not
byte for byte. The GPU smoke test `gpu.golden/G01` does (style §26.1): it renders the frame above (`--smoke`, which
also fails on OpenGL errors) and compares it with `rv_screenshot_diff_core`, the core of `retroverse-screenshot-diff`
(a pixel differs when a channel moves by more than 6/255; at most 0.5% of the pixels may differ), writes a heatmap
next to the screenshot on a mismatch, and is skipped unless GL_RENDERER and GL_VERSION (which names the driver) match
the metadata. On the reference machine 0 pixels differ, in Debug and in Release. By hand:

```powershell
build/msvc-x64-debug/bin/retroverse-screenshot-diff.exe assets/style_tests/screenshots/m0_testroom_golden.png `
  build/screenshots/m0_testroom_golden.png --heatmap build/screenshots/m0_testroom_heatmap.png
```

The golden images stay in the repository: release builds do not copy `style_tests/screenshots` next to the executable.

M0 step 12 also checked these viewpoints (same command with other options, not golden images):

| View | Options | What to look for |
|---|---|---|
| Shelf at 1 m | `--spawn -1.5,0,-3.5,0,-14` | the titles of the main shelf legible at 1:1 (the labels are 1.3 m from the eye): two lines of whole words, glyphs about 8 px tall around eye height, softened by the texture filtering and FXAA to a contrast of about 3:1 against their band; the lowest levels, 1.5 m from the eye, read enlarged |
| CRT close-up | `--spawn 1.62,0,-4.0,0,-48` | curvature, rounded picture corners, aperture grille, the screen's light on the top of the cabinet |
| CRT seen seated | `--spawn 1.62,-0.65,-3.55,0,0` (the eye at 1 m, 0.84 m from the screen) | the glass: the pendant's reflection is a faint sheen over the tube's dark top-left border, the picture intact (no bright patch washing out the bars) |
| Walk | `--spawn 0,0,3.2,0,-6 --frames 120 --fixed-dt 0.016667 --input-script tests/input_scripts/walk_forward_2s.json` | sign, main shelf, left poster and TV from 3.2 m |
| Entrance | `--spawn 0,0,3.2,0,-6 --frames 10 --fixed-dt 0.016667 --input-script tests/input_scripts/turn_around.json` | the doorway and the south wall |

## The room

A 9 x 10 m console room (style §5.3) with a 3 m ceiling. The north wall is both the collection wall (two shelf units
holding 96 cartridges on their slots, the bottom levels left free for growth) and the hero wall (the CRT on its
cabinet with the console, the ornamental rug in front of it, an armchair turned towards it, a sideboard with a table
lamp), with an illuminated "16-BIT CORNER" sign (style §17.1-17.2, an original theme name) above the middle of the
wall, between the collection and the hero corner. Posters hang on three walls; a varnished wainscot with a rail runs
around the room. A 1.2 m way leads from the doorway to the hero wall and every passage is at least 1.2 m wide
(`test_reference_scenes.cpp` checks both); the south half stays open.

Every surface is procedural and original: oak floor planks, walnut, varnished wood and oak boards with grain along each
part's length, painted plaster, the ornamental rug, brushed metal and aged plastic (`rv/assets/surface_textures.hpp`);
cartridge labels and posters are generated cover placeholders, the sign's lettering a bitmap font. Light comes in the
layers of style §4.1: a warm-white ceiling key spot that casts the room's shadows, a pendant fill, a spot per shelf, a
picture light, amber practicals (floor lamp, table lamp; they cast no shadow, so the table lamp's down light ends above
the floor rather than shine through the sideboard), the CRT's light on the top of its cabinet, the sign's glow, a
magenta accent on the TV corner (style §4.2, 16-bit room: tungsten with a light magenta accent), an entrance washer,
and a dim hemisphere ambient with screen-space ambient occlusion. The post-processing adds a localized bloom, a slight
vignette, ACES tonemapping, dithering and FXAA.

## Checklist (style §26.2)

Answered for G01 in M0 step 12 and confirmed at the close of M0 (step 15): the frame the gate renders today matches the
golden to the byte, so six answers stand as Yes, three as Partial (placeholder covers and screen, the doorway onto
darkness) and one as not assessable (the Low profile). None blocks Gate A: the partial ones wait for the catalog, the
video decoder and the hub of later iterations.

| Question | Answer | Notes |
|---|---|---|
| Is the image recognizable as a retrogaming space without the Retroverse title? | Partial | The CRT with a cartridge console beside it, two shelves of cartridges and the "16-BIT CORNER" sign read as a game collection. Nothing on show is a game, though: the labels are abstract placeholders and the screen shows color bars. Real covers come with the catalog (I2-I3), video with the decoder (I5). |
| Is there at least one strong scenographic element? | Yes | The hero corner right of the sign: the CRT, with the sign the most saturated element of the frame, on its cabinet, framed by the rug, the armchair turned towards it and the magenta accent. |
| Are the readable covers real and undistorted? | Partial | Undistorted and readable: labels are generated at the aspect ratio of their surface (about 2.56 px per mm), their titles wrapped between words at the largest scale of the bitmap font that keeps every word whole. 1 m from a shelf the titles around eye height read at 1:1: two lines of glyphs about 8 px tall, which the texture filtering and FXAA soften to a contrast of about 3:1 against their band (1.6 to 4.2:1 over eight titles; 4.8:1 without FXAA). Those of the lowest levels read only enlarged (inspecting a game, I3, brings its cover close). Not real: M0 has no catalog, so they are placeholders. |
| Is it clear where to enter and where to find the games? | Partial | The scene camera faces the collection and the hero wall, each shelf has its own spot, and a 1.2 m way leads from the doorway to the hero wall. The doorway, behind the camera, opens on darkness: no corridor or hub yet (I4). |
| Do wood and warm light give the area coherence? | Yes | Oak floor, varnished wainscot, walnut furniture and oak frames, all with procedural grain, under warm-white spots and amber practicals; the only other hue in the light is the magenta accent. The armchair's fabric is a flat color, though (see the gaps). |
| Does the neon leave the models' details intact? | Yes | The sign is an unlit emitter whose bloom stays local; its letters and its frame stay crisp, and the magenta accent spot is weak (intensity 7), so the CRT shell and the console keep their detail. |
| Does the CRT look like a physical object, not a floating video plane? | Yes | A deep shell with bezel and bulged screen. The picture curves, darkens towards its edges and ends in the tube's dark border with rounded corners; the aperture grille and the scanlines show from up close and fade out before they could cause moiré. The hazy glass adds a faint sheen of the room's lights, which shows on the dark border (seen seated, the pendant's reflection lies over the top-left corner without washing out the bars), and the set casts the key light's shadow and lights the top of its cabinet. |
| Does the room stay credible without commercial assets? | Yes | Everything is procedural and original: prefabs, the eight surfaces, labels, posters, sign. No logos, trademarks or downloaded textures (style §17.2). |
| Do the props leave the ROM search unhindered? | Yes | Decoration density about 0.4 (style §6.3): nothing stands in front of the shelves, the furniture keeps 1.2 m passages, decor hangs above or beside the collection. |
| Does the Low view keep the same aesthetic priorities? | Not assessable | There is one quality level. The Low, Balanced and High profiles (style §4.3, §20.1) come in I8; a Low profile would first drop FXAA, the ambient occlusion and the shadow map. |

## Measurements

Linear luminance of the golden's pixels (0 black, 1 white: 0.2126 R + 0.7152 G + 0.0722 B of the sRGB-decoded
channels), median and 95th percentile over a rectangle per area, in pixels from the image's top-left corner (x0 and y0
included, x1 and y1 excluded):

| Area | Rectangle (x0, y0) - (x1, y1) | Median | 95th percentile |
|---|---|---:|---:|
| CRT picture (75% color bars) | (1205, 480) - (1325, 570) | 0.53 | 0.74 |
| Left poster, under the key light | (878, 342) - (966, 474) | 0.40 | 0.85 |
| Right poster, under its picture light | (1514, 314) - (1594, 438) | 0.25 | 0.71 |
| Table lamp shade (emissive) | (1648, 460) - (1694, 490) | 0.76 | 0.78 |
| Main shelf (labels, boards) | (405, 300) - (690, 770) | 0.03 | 0.59 |
| Rug | (1150, 790) - (1600, 900) | 0.04 | 0.40 |
| Floor, left half | (0, 800) - (960, 1080) | 0.05 | 0.22 |
| Upper walls and ceiling | (0, 0) - (1920, 60) | 0.002 | 0.011 |

The upper walls and the ceiling fall to black. The CRT and the sign are the most saturated elements of the frame, and
the emitters are the brightest areas (the lamp shade, then the CRT's picture), but the CRT does not hold the brightest
pixels: the light print of the left poster sits in the key light's pool and reaches 0.85 where the CRT's bars reach
0.74 (the step gave the poster a dark design, which lowered its median by a third). Classified by hue, saturation and
value (sRGB HSV;
bright accents: value at least 0.85, or at least 0.6 with a saturation of at least 0.45 outside the warm hues from
345 to 50 degrees; colored detail: the other pixels with a saturation of at least 0.3 outside the warm hues; the rest
warm neutrals), the frame holds about 92% warm neutrals, 6% colored detail and 2% bright accents: more neutral than
the 70/20/10 rule of style §3.1, the wood and the dark walls taking most of the frame.

At 1920 x 1080 in `msvc-x64-release` on the reference machine a frame takes 2.6 to 2.9 ms (GPU: shadow map 0.03 ms,
depth pre-pass 0.04 ms, ambient occlusion 0.40 ms, scene 0.40 ms, post-processing 0.41 to 0.44 ms with FXAA, whose
pass adds 0.1 to 0.15 ms). The test room draws 231 items in 129 batches. Its 125 textures take 105 MiB, 80 MiB of it
in the 23 maps of the procedural surfaces with their mip chains, which take 0.6 s to generate at startup in Release
(3 s in Debug).

## Remaining gaps

- **Content:** placeholder labels instead of covers, color bars instead of a game (I2-I3, I5); no plants, boxes,
  controllers or display case yet (style §8 catalog), and only one console family.
- **Materials:** the armchair, the largest object in the foreground, is a flat bordeaux block without a weave: a
  smooth surface next to the grained wood and the rug (BACKLOG "Upholstery surface").
- **Light:** the upper walls and the ceiling fall to black: no bounce light, light probes or IBL (BACKLOG "Bounce
  light for dark rooms"). One shadowed light per view: the lamps cast no shadows, and the table lamp's down light ends
  above the floor instead (BACKLOG "More shadowed lights"). 14 of the 16 light slots per view are in use (BACKLOG
  "More lights per view").
- **Wayfinding:** the doorway opens on darkness; the hub and the corridors arrive in I4.
- **Image quality:** FXAA smooths the stair-steps of edges in each frame but cannot rebuild geometry thinner than a
  pixel, so thin edges (shelf boards, frames, far cartridges) still crawl a little in motion, and the bright bevel
  along the top edge of each shelf board breaks into dashes from a few meters away (the boards and cabinet tops bevel
  their top edges only: their bottom bevels, facing away from the lights, broke into near-black dashes that FXAA left
  alone, below its threshold). FXAA also softens small print: the cartridge titles keep about 3:1 against their band
  1 m from the shelf (BACKLOG "Anti-aliasing beyond FXAA").
- **CRT:** at 1920 x 1080 the 240 scanlines appear only within about 1 m of the screen and reach full strength within
  0.55 m (they fade out between 4 and 2 pixels per line, by design, against moiré); a game picture will need the
  per-content brightness of style §15.2.
- **Profiles:** no Low or High profile to compare (I8).
- **Golden coverage:** G01 is the only golden image and is compared on the reference machine only; G02 to G10 (style
  §26.1) come with the rooms they show (I3 to I8).

## Automated checks

`scripts/gate.ps1 -Gate A` runs every check of the gate and prints a summary (logs in `build/gate-a`):

- **Build matrix:** `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` build without a compiler,
  linker or CMake warning (`/WX`, and the build output is scanned; `-Clean` rebuilds every target).
- **Unit tests** of Debug and Release (label `unit`: `rv_tests`, `rv_platform_tests`).
- **GPU smoke tests** of Debug and Release (label `gpu`, `tests/smoke`; each case starts the executables with
  `--hidden`, a data directory under `build/<preset>/smoke/<case>` and `--run-report`, and checks the report and the
  screenshots):

  | Case | What it checks |
  |---|---|
  | `scenes/m0_testroom` | a clean run (exit code 0, every frame, no high or medium OpenGL debug message, the assets of the configuration: the source tree in Debug, the copy next to the executable in Release, which every build mirrors from the source tree), a screenshot of the requested size that is neither uniform nor black, at most 160 draw calls for its 231 items (129 at M0), a shadow map and a depth pre-pass |
  | `scenes/material_test` | the same, its 26 items in at most 26 draw calls |
  | `scenes/instancing_stress` | the same, 1000 boxes in at most 4 batches, the sun's shadow map drawing them too |
  | `scenes/instancing_growth` | one frame does not grow the instance buffer; turning around (`turn_around.json`) grows it mid-run, and the last frame, drawn after the growth, matches a run spawned at the final pose within the golden tolerance (not to the byte: the final yaw reads back through radians, which moves a few edges by a fraction of a pixel) |
  | `self-check/srgb`, `bloom`, `fxaa` | `--self-check` with the programs of the views: linear 0.5 stored as 188, the bloom chain's energy and thresholds, FXAA blending a slanted edge while copying flat areas exactly |
  | `gl-selftest` | `--gl-selftest --smoke`: the provoked GL_INVALID_ENUM is flagged and, with debug output, reported by the callback; the report records it (`glSelfTest`) beside the run's message counts, which hold nothing else |
  | `effects/shadow` | `tests/smoke/scenes/shadow_caster.json` with and without the key light's shadow: the floor behind the box keeps about a quarter of its brightness (less than half required), lit surfaces keep their exact pixels (no acne), nothing changes outside the shadow's region, nothing gets brighter |
  | `effects/ambient occlusion` | `tests/smoke/scenes/ambient_corner.json` with and without ambient occlusion: the floor and the face at the block's base darken (about 11%), the open floor keeps its exact pixels, and without it the base reads like the open floor |
  | `developer-overlay` | `--devui-open --screenshot-include-ui`: the focused title bar's sRGB 41,74,122 reaches the screenshot exactly (about 28,000 pixels); without `--screenshot-include-ui`, none (Debug only) |
  | `asset-preview` | `retroverse-asset-preview` writes its CRT fixture and renders it cleanly with its panels (Debug only) |
  | `golden/G01` | the comparison above |
  | `support/child_process` | the smoke tests' own process runner: arguments quoted for the Windows command line read back unchanged, a process past its timeout is terminated |

  A case that cannot create an OpenGL 4.5 context, and the golden comparison on another GPU or driver, are skipped
  (exit code 77, ctest's skip code), not failed.
