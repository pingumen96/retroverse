# Gate A: the M0 test room

Gate A closes the visual Phase A of `style.md` §27: an empty but credible room, with its style and lighting already
recognizable. M0 step 12 tuned `m0_testroom` for it and recorded golden G01. This page answers the checklist of
style §26.2 for that image and lists what the room still lacks.

## Golden G01

| | |
|---|---|
| Image | `assets/style_tests/screenshots/m0_testroom_golden.png`, 1920 x 1080 RGB |
| Metadata | `assets/style_tests/screenshots/m0_testroom_golden.json`: GPU, driver, preset, the commit of the sources, SHA-256 of the scene file and of the image, the command |
| Scene | `assets/style_tests/reference_scenes/m0_testroom.json`, scene camera (no `--spawn`, no input script) |
| Frame | the last of a 30-frame run (frame 29) |
| Build | `msvc-x64-debug` on the reference machine (`docs/testing/reference-hardware.md`) |

```powershell
build/msvc-x64-debug/bin/retroverse.exe --scene m0_testroom --exit-after-frames --frames 30 --hidden `
  --screenshot-size 1920x1080 --screenshot build/screenshots/m0_testroom_golden.png --data-dir build/rvdata
```

The scene is static and the run uses no time-dependent effect, so the same build, driver and scene give the same
bytes. Regenerate G01 only when a change alters the look on purpose, and record the new metadata with it. A changed
driver or GPU can move pixels by an 8-bit step or two (dither, filtering, FXAA): compare with a tolerance, not
byte for byte. Automated comparisons come later (style §26.1; roadmap step 14 and I8).

The step also checked these viewpoints (same command with other options, not golden images):

| View | Options | What to look for |
|---|---|---|
| Shelf at 1 m | `--spawn -1.5,0,-3.5,0,-14` | the titles of the main shelf legible: at a glance around eye height, enlarged on the lowest levels (1.5 m from the eye) |
| CRT close-up | `--spawn 1.62,0,-4.0,0,-48` | curvature, rounded picture corners, aperture grille, the glass reflection, the screen's light on the cabinet |
| Walk | `--spawn 0,0,3.2,0,-6 --frames 120 --fixed-dt 0.016667 --input-script tests/input_scripts/walk_forward_2s.json` | sign, main shelf, left poster and TV from 3.2 m |
| Entrance | `--spawn 0,0,3.2,0,-6 --frames 10 --fixed-dt 0.016667 --input-script tests/input_scripts/turn_around.json` | the doorway and the south wall |

## The room

A 9 x 10 m console room (style §5.3) with a 3 m ceiling. The north wall is both the collection wall (two shelf units
holding 96 cartridges on their slots, the bottom levels left free for growth) and the hero wall (the CRT on its
cabinet with the console, the ornamental rug in front of it, an armchair turned towards it, a sideboard with a table
lamp), under an illuminated "16-BIT CORNER" sign (style §17.1-17.2, an original theme name). Posters hang on three
walls; a varnished wainscot with a rail runs around the room. A 1.2 m way leads from the doorway to the hero wall and
every passage is at least 1.2 m wide (`test_reference_scenes.cpp` checks both); the south half stays open.

Every surface is procedural and original: oak floor planks, walnut, varnished wood and oak boards with grain along each
part's length, painted plaster, the ornamental rug, brushed metal and aged plastic (`rv/assets/surface_textures.hpp`);
cartridge labels and posters are generated cover placeholders, the sign's lettering a bitmap font. Light comes in the
layers of style §4.1: a warm-white ceiling key spot that casts the room's shadows, a pendant fill, a spot per shelf, a
picture light, amber practicals (floor lamp, table lamp), the CRT's light on its cabinet, the sign's glow, a magenta
accent on the TV corner (style §4.2, 16-bit room: tungsten with a light magenta accent), an entrance washer, and a dim
hemisphere ambient with screen-space ambient occlusion. The post-processing adds a localized bloom, a slight vignette,
ACES tonemapping, dithering and FXAA.

## Checklist (style §26.2)

| Question | Answer | Notes |
|---|---|---|
| Is the image recognizable as a retrogaming space without the Retroverse title? | Partial | The CRT with a cartridge console beside it, two shelves of cartridges and the "16-BIT CORNER" sign read as a game collection. Nothing on show is a game, though: the labels are abstract placeholders and the screen shows color bars. Real covers come with the catalog (I2-I3), video with the decoder (I5). |
| Is there at least one strong scenographic element? | Yes | The hero corner: the CRT, with the sign the most saturated element of the frame, on its cabinet, framed by the rug, the armchair turned towards it and the magenta accent, under the sign. |
| Are the readable covers real and undistorted? | Partial | Undistorted and readable: labels are generated at the aspect ratio of their surface (about 2.56 px per mm); 1 m from a shelf the titles around eye height read at a glance, those of the lowest levels only enlarged (inspecting a game, I3, brings its cover close). Not real: M0 has no catalog, so they are placeholders. |
| Is it clear where to enter and where to find the games? | Partial | The scene camera faces the collection and the hero wall, each shelf has its own spot, and a 1.2 m way leads from the doorway to the hero wall. The doorway, behind the camera, opens on darkness: no corridor or hub yet (I4). |
| Do wood and warm light give the area coherence? | Yes | Oak floor, varnished wainscot, walnut furniture and oak frames, all with procedural grain, under warm-white spots and amber practicals; the only other hue in the light is the magenta accent. |
| Does the neon leave the models' details intact? | Yes | The sign is an unlit emitter whose bloom stays local; its letters and its frame stay crisp, and the magenta accent spot is weak (intensity 7), so the CRT shell and the console keep their detail. |
| Does the CRT look like a physical object, not a floating video plane? | Yes | A deep shell with bezel and bulged screen. The picture curves, darkens towards its edges and rounds off into the tube's border; the aperture grille and the scanlines show from up close and fade out before they could cause moiré. The glass reflects the room's lights, and the set casts the key light's shadow and lights its cabinet and the console. |
| Does the room stay credible without commercial assets? | Yes | Everything is procedural and original: prefabs, the eight surfaces, labels, posters, sign. No logos, trademarks or downloaded textures (style §17.2). |
| Do the props leave the ROM search unhindered? | Yes | Decoration density about 0.4 (style §6.3): nothing stands in front of the shelves, the furniture keeps 1.2 m passages, decor hangs above or beside the collection. |
| Does the Low view keep the same aesthetic priorities? | No | Not assessable: there is one quality level. The Low, Balanced and High profiles (style §4.3, §20.1) come in I8; a Low profile would first drop FXAA, the ambient occlusion and the shadow map. |

## Measurements

Linear luminance of the golden's pixels (0 black, 1 white), median and 95th percentile per area:

| Area | Median | 95th percentile |
|---|---:|---:|
| CRT picture (75% color bars) | 0.22 | 0.72 |
| Left poster, under the key light | 0.32 | 0.85 |
| Right poster, under its picture light | 0.17 | 0.70 |
| Table lamp shade (emissive) | 0.75 | 0.78 |
| Main shelf (labels, boards) | 0.03 | 0.54 |
| Rug | 0.06 | 0.35 |
| Floor, left half | 0.05 | 0.23 |

The upper walls and the ceiling stay near 0.004 (black). The CRT and the sign are the most saturated elements of the
frame, but the CRT is not the brightest surface: the light print of the left poster sits in the key light's pool (the
step gave it a dark design, which lowered its median by a third). A rough classification of the pixels by hue,
saturation and brightness gives about 83% warm neutrals, 13% colored detail and 4% bright accents: a little more
neutral than the 70/20/10 rule of style §3.1.

At 1920 x 1080 in `msvc-x64-release` on the reference machine a frame takes 2.9 ms (GPU: shadow map 0.03 ms, depth
pre-pass 0.04 ms, ambient occlusion 0.40 ms, scene 0.40 ms, post-processing 0.44 ms with FXAA, whose pass adds 0.1
to 0.15 ms). The test room draws 231 items in 129 batches. Its 125 textures take 105 MiB, 80 of them the maps of the
procedural surfaces with their mip chains, which take 0.6 s to generate at startup in Release (3 s in Debug).

## Remaining gaps

- **Content:** placeholder labels instead of covers, color bars instead of a game (I2-I3, I5); no plants, boxes,
  controllers or display case yet (style §8 catalog), and only one console family.
- **Light:** the upper walls and the ceiling fall to black: no bounce light, light probes or IBL (BACKLOG "Bounce
  light for dark rooms"). One shadowed light per view: the lamps cast no shadows (BACKLOG "More shadowed lights").
  14 of the 16 light slots per view are in use.
- **Wayfinding:** the doorway opens on darkness; the hub and the corridors arrive in I4.
- **Image quality:** FXAA smooths the stair-steps of edges in each frame but cannot rebuild geometry thinner than a
  pixel, so thin edges (shelf boards, frames, far cartridges) still crawl a little in motion (BACKLOG "Anti-aliasing
  beyond FXAA").
- **CRT:** at 1920 x 1080 the 240 scanlines appear only within about 1 m of the screen and reach full strength within
  0.55 m (they fade out between 4 and 2 pixels per line, by design, against moiré); a game picture will need the
  per-content brightness of style §15.2.
- **Profiles:** no Low or High profile to compare (I8).
- **Golden comparisons:** G01 is a reference image, not yet an automated test (style §26.1; roadmap step 14, I8).
