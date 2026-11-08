# I11 step 4: logos in the world

ADR-0019 §3; roadmap I11 step 4. A room's signs read "16-BIT CORNER" in the generic bitmap lettering. Now, when the
room's system has an identity, they show its logo, or its name when it has none. Every scene without a theme draws
as before.

## What was built

- **The sign's picture** (`assets::makeIdentitySign()`, `layoutIdentitySign()`, `rv/assets/identity_sign.hpp`): GPU-free,
  deterministic.
  - **Layout:**
    - The generic signs' margin of a twelfth of the shorter side.
    - The logos in one row at one height, at most four.
    - The name in Atkinson Hyperlegible Bold through `assets::FontFace` (UTF-8, mixed case), on one line, or on two
      broken at the space nearest its middle when that lets it be larger.
    - The name sits beside the logos when the inner box is at least 4.5 times as wide as tall (the room's neon, the
      portals' plates, the hub's fascia), and under them otherwise (the blades).
  - **Lit**, for the halls' plates (the portal's plate, its blades, a fascia without a neon):
    - The logo in its own colours, composited in linear light on black. A dark logo (a mean luminance under 0.06)
      becomes a white silhouette, or it would vanish.
    - The name in white.
    - The plate's unlit tungsten material multiplies the picture, as it does the generic lettering.
  - **Neon**, for the room's neon and a fascia that takes it:
    - The logo's outline and the edges between its colours (a dot, a bar, a word inside a badge) as tubes half a
      letter's stroke wide, and the name's letters as tubes.
    - Both are drawn by the generic neon's tube renderer, now public as `assets::makeNeonTubes()`.
    - Grey levels that the neon's material colours.
- **Which objects are signs, and what they show** (`app::identitySignTargets()`, `identitySignContent()`, GPU-free):
  - **The signs:**
    - `<room>/fx:hero.sign`, and a pack's further pieces;
    - `hall:<floor>/[hub/]portal:<room>/sign`, `blade-east` and `blade-west`;
    - `hall:<floor>/hub/fascia-sign:<room>`.
    - Each counts only with a "neon" or "sign" pattern on its picture, which also sets its style.
  - **A console room:** its system's logo alone, since the themes' logos are wordmarks and already say the name. A
    system without a logo shows its name. An expansion's number follows either, as `chainSignText()` adds it to the
    generic sign.
  - **When the system counts as having an identity:** it has a logo file, or its shown name differs from ES-DE's
    `<fullname>`.
  - **A shared or arcade room:** the hall's signs show its sections' logos, in the plan's order, with an expansion's
    number. Its own neon keeps the generic sign, which names the room's kind.
- **On the GPU** (`WorldSigns`, `src/app/src/world_signs.cpp`):
  - **When the signs are found:** for the scene shown, when it changes (a floor shown, objects replaced) and when the
    catalog's systems change (`systemsRevision()`).
  - **Drawing:**
    - Each sign is drawn on the worker pool at the size of the generic picture it replaces.
    - Each logo file is rasterized once into 1024 x 256.
    - Equal signs share one texture: a portal's two blades.
  - **Upload:** two a frame, sRGB with mips, under the new GPU memory kind `signTextures`.
  - **Display:** the texture goes on the picture's material as a texture override, as the hero CRT's stills do. The
    tint and the emissive strength stay the pack's.
  - **A lockstep run** waits for its signs, at most 5 s, so that its frames never depend on the pool's timing.
  - **Failures:** a logo or a sign that cannot be drawn is logged, and that sign stays generic.
- **The catalog** gives each system as the signs show it: `AppCatalog::signSystem()`, the shown name, ES-DE's name and
  the logo file.
- **The run report's catalog section:** `identitySigns`, `identitySignsShown`, `identitySignsFailed`.

## Tests

- **Unit:**
  - `test_identity_sign.cpp`:
    - the layout per case: a blade stacked, the room's neon and the fascia side by side, a name alone, a long name on
      two lines, one word never broken, a shared room's four logos at most, nothing to draw;
    - the lit plate's pixels: the logo's colour, white letters, a dark logo's silhouette;
    - the neon: grey levels, a filled shape's inside dark and its outline lit;
    - the same pixels twice.
  - `test_identity_signs.cpp`:
    - each sign id and those that are not signs;
    - the scene's targets by pattern and role;
    - each content rule: the logo alone, the name without a logo, an expansion's number, ES-DE's name only, a shared
      room's logos, a room's own neon.
- **GPU, G22** (`test_golden_room.cpp`, `[identity]`):
  - G02's run with `identity.json` pointing at the invented theme fixture: the snes room's neon shows the fixture
    logo's tubes.
  - Against G02's golden, only the sign's box differs (more than 2,000 pixels, all within it). Every sign is drawn
    before the frame.
  - A second run from the hall, checked by eye: the portal's plate and its blade show the logo in its colours.
- **The real library, by hand** (a data directory of its own under `build/`, ES-DE read only):
  - `--start-room room:snes` and a run from its hall: the 15 signs of the snes floor all drawn, none failed, a peak of
    282 MiB.
  - The room's neon and the hall's plates show the theme's logo; the screenshots stay under `build/`.
- **Every other golden** (G02 to G21) is 0 pixels apart. Their fixtures hold no theme, so no sign has an identity.

## Departures

- **No logo cache and no BC3 for the signs.** ADR-0019 §3 caches the rasterized logos under `cache/identity/`,
  compressed to BC3.
  - The signs keep the generic pictures' size (512 pixels wide at most), about 0.3 MiB each with mips.
  - The snes-room fixture's three signs take 0.9 MiB.
  - A logo is rasterized once per run, in a few milliseconds.
  - The cache waits for a measurement that needs it: the busiest real floor within its 512 MiB, the gate's memory row.
- **The signs are swapped at runtime, not at scene build.** The plan said "resolved when the scene is built". The scene
  files keep their generic signs, so the scene and plan goldens never hold a trade name, and the catalog's identity
  (on its worker) can change without rebuilding a floor.
- **A shared room's neon stays generic.** The plan gave its portal its sections' logos. Its own neon names the room's
  kind.
- **No name beside a logo.** The plan put the name under the logo. On the real library every logo is a wordmark, and
  the name beside it said the same thing twice: the user chose the logo alone (2026-11-08). The name shows for a
  system without a logo.
- **Colours:** the neon keeps its pack's colour. The systems' colours are step 6.
