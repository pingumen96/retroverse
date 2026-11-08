# I11 step 7: the console model in the room

ADR-0019 §7; roadmap I11 step 7. A console room's hero console was always the procedural cartridge console. Now, when
`<user data>/content/consoles/<system>/` holds a model of that system's console that passes its checks, the model
stands on the cabinet in its place, and the insertion puts a cartridge into its slot. Without content, or with a model
that fails, the room keeps its procedural console exactly as before.

## What was built

- **The content root:** `Paths::contentDir()` is `<user data>/content`. Only `retroverse-cli content` writes there.
- **A console's folder** (`assets/console_content.hpp`, GPU-free) holds `console.glb` and `console.json` (schema 1,
  unknown keys refused):
  - `model`, a file name in the folder;
  - `kind`: home, handheld or computer;
  - `front`: the axis the model's front faces, +z by default;
  - `sockets`: the nodes of CartridgeSlot, DiscTray and Front, else the model's `Anchor_` nodes;
  - `scaleCm`: the real console's size, checked;
  - `source`: URL, author, licence, date, modifications.
- **The checks** (`checkConsoleModel()`), which the app and the command share:
  - each side 5 to 45 cm;
  - a footprint of at most 42.5 x 40 cm, the cabinet's free width beside the CRT;
  - `scaleCm` within 10% on each side;
  - at most 60,000 triangles drawn;
  - at most 2 MiB of textures once compressed, their mips included;
  - every socket the manifest names present in the model.

  Each problem is reported in a sentence. A model the loader cannot read is refused too: only its glTF features are
  taken.
- **The textures, compressed** (`compressConsoleTextures()`, through `stb_dxt`):
  - Every image the materials sample, with its full mip chain, encoded as S3TC.
  - **Formats:** BC1 sRGB for an opaque base colour or an emissive map; BC3 sRGB for a base colour with transparency
    (`encodeBc3()`, new); BC1 linear for the normal and occlusion-roughness-metal maps.
  - **Mips:** sRGB levels are filtered in linear light; linear maps by their raw values.
- **On the GPU:** `Texture2D::createCompressed()` uploads S3TC blocks with their levels. `GpuFormat::Bc3` is new, and
  so is the memory kind `contentTextures`: the scene textures' kind stays exactly the texture cache's.
- **In the room** (`WorldConsoles`, at runtime, the pattern of steps 4 and 5):
  - **Finding the site:** the procedural hero consoles of the scene shown (`consoleSites()`: `<room>/fx:hero.console`
    in a console room) whose system has a folder.
  - **On the pool, once per system and run:** the folder is read, checked and compressed. The images the GPU model
    would upload become 1 x 1 stand-ins.
  - **A model that passes** takes the console's place through `SceneInstance::replaceObjects()`, under the console's
    own id:
    - The scene's model loader resolves its `content:consoles/<system>/console.glb` path from `WorldConsoles`.
    - It stands at the procedural console's site: its front turned to +Z, its bottom's middle on the cabinet
      (`consolePlacement()`). A model wider than the cabinet leaves to its right moves left (`consoleModelLocal()`).
    - Its collider is a box from its bounds, as for every model.
    - Its materials take the compressed textures as overrides.
  - **The insertion:** the room's media socket becomes the model's (`consoleModelSocket()`, then
    `WorldSession::setConsoleSocket()`). That is CartridgeSlot, else DiscTray, else the top of its bounds' middle; the
    hero CRT's screen is kept.
  - **A model that fails** is logged once and never shown: the procedural console stays, never the magenta cube.
  - **When it applies:** again after every scene change, since a rebuilt room brings its procedural console back.
    Nothing is replaced while the room editor holds render items. A lockstep run waits for its models.
- **`retroverse-cli content`:**
  - `add-console`: checks the model, then copies the `.glb` and writes `console.json` with its source, author and
    licence. It refuses a model that fails, and a second model for a system unless `--replace`.
  - `check` and `list` (`--json`).
- **`retroverse-asset-preview <content folder>`** opens the folder's `console.glb` and reports the app's checks of its
  `console.json`, watching both.
- **The run report's catalog section:** `consoleModels`, `consoleModelsRefused`, `consoleTextureBytes`.

## Tests

- **Unit:**
  - `test_console_content.cpp`:
    - the manifest written and read back, its defaults, eleven refusals;
    - the checks on the fixture console (24 x 7 x 20 cm): passes; twice as large; `scaleCm` off; turned by its front;
      a 2048-pixel texture over 2 MiB; a missing socket node;
    - BC1, BC3 and the mip chain's bytes;
    - the media socket from the anchor, and from the top without one;
    - the placement; a folder read.
  - `test_content_consoles.cpp`: the sites of a scene; the content paths; the model object at the site, its bottom on
    the cabinet; the shift left; the socket with the CRT's screen kept.
  - `test_cli_content.cpp`: add-console, `--replace`, check `--json`, a refused model writing nothing, the arguments.
  - `test_preview_subject.cpp`: a content folder in the asset preview.
- **The fixture console** (`tests/support/fixture_console.cpp`) is made at test time: the procedural console's geometry
  as a binary glTF, a teal striped body and `Anchor_CartridgeSlot`. It is no real console's shape, name or picture.
- **GPU** (`test_world_launch.cpp`, `[content]`): the snes-room fixture with the fixture console, a quarter larger, as
  snes' content.
  - From the doorway it stands on the cabinet beside the CRT: 1 model shown, none refused, and its 256-pixel texture's
    BC1 bytes the only content texture.
  - The insertion, skipped by a button, puts the item into the model's slot, 1.75 cm higher than the procedural
    console's.
- **Every golden** is unchanged: no fixture of theirs has content.

## Departures

- **At runtime, not at scene build.** The ADR draws the model "at scene build". As for the signs and the plaque, the
  scene files and plans keep the procedural console, and the model is swapped in once it is read and checked. A
  failing model then simply never replaces anything.
- **No cache under `cache/content/`.** A model is read, checked and compressed once per run on the pool, in about a
  second in Debug for the fixture. The cache waits for real models (steps 9 and 10) and their measurements.
- **Normal maps in BC1, not BC5.** The shaders read a normal map's three channels; BC5 holds two, and reconstructing
  the third would change every model's shading, the goldens included.
- **The console only on the cabinet.** A handheld's stand, the showcases with `"item": "console"`, the computer rooms
  and the shared rooms' sections are step 10's. A handheld model stands where the procedural console stands meanwhile.
- **The memory row waits for real models.** The busiest real floor with a model in every room is measured when steps
  9 and 10 bring them. A model takes at most 2 MiB of `contentTextures`.
- **The fixture console is made in C++, not by `make_test_models.py`.** No smoke test runs Python. The glb writer and
  the procedural console make it within the test.
