# I11 step 8: the Blender pipeline

ADR-0019 §7; roadmap I11 step 8. Step 7 shows a console model that passes its checks. This step makes such models from
what the user downloads, or builds them from measurements. The work lives in the private sibling repository
`retroverse-content` (`E:\Progetti\retroverse-content`, no remote), never in this one. Retroverse's side is the
command step 7 added: `retroverse-cli content add-console` installs a result, `content check` judges it.

## What was built (in retroverse-content)

- **`dimensions.json`:** each system's real width, height and depth in centimetres, and its cartridge slot (its middle
  on the top and its opening, in Retroverse's axes), each with its source. The first entry is snes: 20.0 x 7.2 x
  24.2 cm, the PAL and Super Famicom shape.
- **`normalize_console.py`** runs Blender 4.3 in the background (`blender -b --factory-startup --python ...`). In order:
  1. It imports a `.glb`, `.gltf`, `.fbx` or `.obj`, or every object of a `.blend`.
  2. It keeps the meshes, their parents' transforms applied, and joins them into one.
  3. It turns the front to +Z. `--front` names the axis the front faces as imported, in glTF's axes.
  4. It scales the model uniformly to the measurements, using the median of the three ratios. A side more than 5% off
     is reported: the model's proportions differ.
  5. It puts the origin at the middle of the bottom.
  6. It decimates to 60,000 triangles (Collapse) when the model has more.
  7. It bakes with Cycles, from the model as imported onto the decimated one, on a new UV map: a 1024 base colour, a
     512 tangent-space normal map, and a 512 occlusion-roughness-metal map (roughness baked, metal the materials'
     mean). The final material is wired so that the glTF exporter writes them. `--no-bake` keeps the model's own
     materials.
  8. It adds `Anchor_CartridgeSlot`, a thin dark plane at the measured slot whose bounds are the opening, and
     `Anchor_Front`.
  9. It exports a binary glTF and prints a JSON summary. It exits 1 when a side is outside 5 to 45 cm or the model has
     more than 60,000 triangles.
- **`author/_kit.py`:** the helpers for consoles built from measurements: rounded boxes, materials in sRGB, the slot
  plane, the anchors and the export. The console is built front towards Blender's -Y, which the exporter turns into
  +Z.
- **`author/snes.py`:** the Super Nintendo, PAL and Super Famicom shape. SOURCES.md found no free model of it.
  - The parts: the rounded body, the raised cartridge housing with its slot, the power and reset sliders on their dark
    track, the eject button, the power light and the two controller ports.
  - No logo and no lettering.
  - 20.0 x 7.2 x 24.5 cm (the ports stand 3 mm proud), 1054 triangles, no textures.
- **`tests/run_tests.ps1 -Cli <retroverse-cli.exe>`:** the chain end to end. `tests/make_messy_input.py` writes an
  invented console as downloads often come:
  - in centimetres;
  - its front facing +X, away from the origin, under a parent;
  - 76,812 triangles, two materials, one with a picture embedded in the FBX.

  The test then:
  1. normalizes that input (24 x 8 x 20 cm, 58,200 triangles, baked: 1.00 MiB of textures once Retroverse compresses
     them);
  2. builds every authored console;
  3. installs each into a data directory under `tests/out`;
  4. requires `content check` to pass them all.

  It runs in about 15 seconds.
- **`SOURCES.md`** marks snes `authored`. The README has the commands.

## Checks

- **`run_tests.ps1`** passes: the normalized messy model and the authored SNES both pass `content check`.
- **`retroverse-asset-preview <folder>`:**
  - The SNES shows its eleven nodes, its two sockets and the slot anchor on the housing's top.
  - The normalized model shows the input's coloured grid baked onto its one material, with the front anchor.
- **The real library, by hand:**
  - The SNES was installed into this worktree's private data directory (`build/rvdata-real`) with `content
    add-console`.
  - In `room:snes` it stands on the cabinet beside the CRT: 1 model shown, none refused.
  - The screenshots stay under `build/`.

## Departures

- **The downloaded half of the proof is an invented download.** The plan wanted "one downloaded and one authored
  console". Every candidate in SOURCES.md is on Sketchfab, which needs the user's login. The normalizer is proven on
  `make_messy_input.py`'s model, which has a download's faults; the first real download comes with step 9.
- **No occlusion bake.** The ORM's occlusion is 1: a console a metre or two away gains little from it, and the
  room's ambient occlusion shades its contact with the cabinet.
- **Authored consoles carry no textures.** Plain materials read well at the cabinet's distance and cost no memory.
  A printed detail can come with a model that needs it.
