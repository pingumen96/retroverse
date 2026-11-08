# I11 step 6: the systems' colours

ADR-0019 §5; roadmap I11 step 6. Every room's neon was its theme pack's colour: the magenta of the 16-bit rooms, the
cold blue of the optical era. Now a console room whose system's identity has a palette glows in that system's colour,
kept within style §3's accents. A setting turns it off.

## What was built

- **The colour** (`app::systemAccent()`, `clampToAccentBand()`, GPU-free):
  - **The source:** the identity's colours, `systemColor` then `systemColorPalette1` to `4`. The catalog worker now
    carries them in `CatalogSystem::colors`, and `AppCatalog::signSystem()` hands the accent over.
  - **The choice:** the entry with the largest chroma (max minus min of its sRGB channels). A palette whose best entry
    has less than 0.10 has no accent, since a grey is no system's colour.
  - **The clamp:** in HSV of the sRGB-encoded colour, the hue is kept and the rest is held to the band of the packs'
    neons (#4C8BC9, #B34DCE, #6EAF93, #D8584A, #5AB4D6, #F0509E):
    - saturation 0.35 to 0.67;
    - value 0.68 to 0.94.
  - Those neons stand in the band unchanged. A louder colour comes in at its hue: #FF0000 becomes #F04F4F. The accent
    stays a light of the 10% (style §3.1).
- **Where it goes** (`WorldSigns::tint()`, at runtime, as step 4's signs):
  - The room's neon (`<room>/fx:hero.sign`) and the hub's fascia over its doorway, when the fascia takes the neon: their
    picture materials get the accent as their base colour (`gl::MaterialRegistry::setFactors`). The emissive strength
    stays the pack's.
  - The neon's glow (`<room>/accent:sign_glow`) takes the accent.
  - The sign's picture light (`<room>/fx:hero.sign-light`) has the pack's neon tint (`world::signLightTint()`)
    exchanged for the accent's.
  - Console rooms only. A shared or arcade room's neon names its kind and keeps the pack's colour.
  - Taken off (a rescan, the setting off, the scene gone), every material and light is back to what it was.
- **The setting "Colori dei sistemi"** (`world.systemColors`, on by default):
  - It lives in world.json and settings.json beside `reduceMotion`, and as a row of Impostazioni' Mondo group.
  - It applies at once, with no floor rebuilt.
  - It never shapes a plan: it is left out of the building's digest, as `reduceMotion` and `lightingProfile` are
    (ADR-0018 §2).
  - The canonical config document reads one written before it as on.
- **The run report's catalog section:** `systemColors`, `tintedNeons`, `tintedLights`.

## Tests

- **Unit:**
  - the clamp's table: the packs' neons unchanged, louder, darker and paler colours brought in at their hue;
  - the palette's choice, and a grey palette without an accent;
  - world.json's and settings.json's new key; the canonical document; the settings rows.
- **GPU, G22 rewritten** (`[identity]`):
  - With the colours on (the default), the snes room's neon, its glow and its picture light are in the fixture
    palette's yellow (#FED01B clamped), beside step 4's logo: `tintedNeons` at least 1, `tintedLights` 2.
  - With `{"systemColors": false}` in world.json, nothing is tinted, the plan is the same, and against G02's golden only
    the sign's box differs, as step 4's check had it. With the colours on, the image differs from that one beyond the
    sign, in the glow on the wall.
- **The real library, by hand:** the snes floor's neons take their systems' accents: 3 materials and 6 lights tinted,
  with the 15 signs. The screenshots stay under `build/`.
- **G02 to G21** are unchanged: their fixtures have no theme, so no system has a palette.

## Departures

- **The band is the packs' neons', in HSV.** ADR-0019 §5 says "style §3's accent band" without numbers. The packs'
  seven neons give the limits. The hub's amber (#FFB45A, value 1.0) stands just above them and is brought to #F0A955
  when a palette asks for it.
- **The choice is by chroma, not saturation.** HSV saturation calls a near-black red fully saturated. Chroma picks the
  colour that reads as the system's.
- **No row of its own in the wizard.** The wizard's appearance page keeps the lighting profile and reduceMotion.
