# I5 step 6: CRT presets

Roadmap row: I5 step 6. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §7 ("Presets", "Which preset
a screen takes"; amended on 2026-10-30 for this step: "Decisions" below), specification §9.6 and §9.1's "Monitor
moderno", style §15.2. After step 4 (the picture's rectangle in the same material block).

## What was built

- **The presets' data** (`rv_assets`, `crt_presets.hpp`): `assets/materials/crt_presets.json` (JSON with comments)
  names four sets of `CrtEffect`'s seven values (lines and their depth, triads and their strength, curvature,
  vignette, the glass's sheen), every key required: `neutral`, `consumer`, `arcade`, `monitor`.
  `<user data>/config/crt_presets.json` (read, never written) overrides them key by key; a user file that does not
  load is left out whole, a shipped file that does not load leaves `builtInCrtPresets()` (its copy, unit-tested
  equal); both are logged. `parseCrtEffect()` is the one reader of a `CrtEffect` object (the material library's
  `"crt"` uses it too). `CrtEffect`'s defaults are now the consumer preset's.
- **Today's values moved into data.** The three screen materials (`MAT_Screen_CRT`, `MAT_Screen_CRT_Room`,
  `MAT_Screen_Arcade`) keep `"crt": {}`, the mark of a tube; the values are their screen's preset.
- **The packs** (`world::ThemeMaterials::screenPreset`, optional, checked against the four names): the optical-era
  lounge's `monitor`, the arcade's `arcade`; every other pack names none.
- **The choice** (`rv_app_cli`, `crt_looks.hpp`, GPU-free): `chooseCrtPreset()` is ADR-0016 §7's order;
  `ScreenLooks` holds the presets, the run's `video.json` and the scene's packs by room, and records each screen's
  look. `applyScreenLooks()` gives a scene's screen material instances (`<object id>/screen...` with a `"crt"` block)
  their preset's values before they are registered: the hero (`...fx:hero.crt`, the crt prefab or the arcade's hero
  cabinet; a hand-made scene's first crt), the secondaries (`secondary_tv`, a hand-made scene's other crts). The
  arcade's cabinets take their room's `cabinet` look in `CollectionInstance` (the static fronts, the hero front, the
  pooled video materials).
- **The wiring** (`rv_app`): `SceneGraphics` loads the presets and holds the `ScreenLooks` (`SceneResources::
  screenLooks`, used by `SceneInstance::create()` and `replaceObjects()`); `RetroverseApp` gives it `video.json`;
  `WorldSession::showFloor()` passes the floor's rooms' packs (`SceneShow::screenPresets`).
- **The report**: `crtLooks` (`crtEffect`, `presetsSource`, each screen's `screen`, `room`, `kind`, `preset`, `chosen`,
  `rule`).
- **The shader is unchanged**: a preset only sets the material block's CRT fields, as before; the same program, the
  same uniform block (112 bytes), the same cost. A neutral tube is the tube with every effect at 0 (no scanline, no
  mask, a flat picture with square corners, no vignette).

## The presets

| preset | lines | scanline depth | triads | mask | curvature | vignette | glass |
|---|---|---|---|---|---|---|---|
| `neutral` | 240 | 0 | 320 | 0 | 0 | 0 | 0.25 |
| `consumer` | 240 | 0.35 | 320 | 0.25 | 0.06 | 0.3 | 0.25 |
| `arcade` | 240 | 0.5 | 240 | 0.35 | 0.09 | 0.3 | 0.2 |
| `monitor` | 480 | 0.12 | 640 | 0.1 | 0.02 | 0.12 | 0.25 |

`consumer` is exactly `MAT_Screen_CRT`'s and `MAT_Screen_CRT_Room`'s former values. The arcade's cabinets drew a
lighter tube until now (`MAT_Screen_Arcade`: 0.3, 240 triads at 0.2, curvature 0.04, vignette 0.25, glass 0.2); its
coarser grille and darker glass are kept in `arcade`.

## Decisions

ADR-0016 §7 is amended (2026-10-30) with what it left open; the rules' order is unchanged:

- **Rule 2 ("the room's video.json")** is video.json's `crtPreset` for the screen: the screen's key, else the room's,
  else the kind's (`hero.crtPreset`...). The kind's key ships `""` and is the user's, so it ranks with the room's above
  the pack; **rule 4** is the built-in kind default (`consumer`; `arcade` for cabinets, which stand only in the arcade).
- **`vintage`** steps `monitor` to `consumer` and `consumer` to `arcade`; `arcade` stays, and so does `neutral` (the
  clean video, never chosen by accident). `light` keeps the choice.
- **The pack's preset is every screen's of its rooms** (the hero, the secondaries, the cabinets), as §7 reads.
- **Cabinets take one look per room**: their screens are one collection material (and its pool), so a per-screen key
  does not reach a single cabinet.
- **Every screen of the floor shown** gets its look at registration, the rooms seen through doorways too, not only the
  bound room's: a preset never changes as the eye walks in.
- **`consumer` is "today's CrtEffect defaults"** read as the values today's screens draw (the struct's former defaults
  were never drawn); the struct's defaults now equal them.
- **Goldens**: the arcade's screens change on purpose with the lounge's (§7's "the arcade's `arcade`"): see Results.

## Proofs

- Unit: `tests/unit/assets/test_crt_presets.cpp` (7 cases: the names; the shipped file equal to the built-in copy,
  consumer today's values, neutral without effects, arcade deeper, monitor finer, every preset within legible bounds;
  the screen materials' tube; `parseCrtEffect`'s errors; the user's file key by key and never written; a broken user
  file left out whole (8 kinds); a missing or incomplete shipped file leaving the built-in values),
  `tests/unit/app/test_crt_looks.cpp` (6 cases: the kinds' defaults and vintage's step; the rules' order as a table of
  18 rows; video.json's screen, room and kind keys and the scene's packs; the screens' rooms and kinds; a floor's
  material instances given their looks and recorded; a hand-made scene's first crt as its hero),
  `test_theme_packs.cpp` (the pack's `screenPreset`: read, refused, the shipped packs'), `test_run_report.cpp`
  (`crtLooks`' round trip).
- GPU (`tests/smoke/test_crt_presets.cpp`, `[crt_presets]`, on `tests/smoke/scenes/crt_presets.json`: four CRTs at
  eye height showing the colour bars, one preset each from the case's `video.json`):
  - each preset close up (2560 x 1440, the picture about 1,150 rows, a scanline over 4.8 pixels): the luma of a
    5-pixel column down the green bar, row by row; the ripple (the mean step between neighbouring rows over the mean
    luma): neutral 0.001 (no row modulation), consumer 0.038, arcade 0.057 (1.5 times consumer's), monitor 0.005
    (its 480 lines at about 2.4 pixels each mostly faded, its depth 0.12); the bar's mean luma 172 to 176 for all four
    (the scanlines keep the picture's mean). Checked: neutral below 0.005, consumer above 0.02, arcade above 1.25
    times consumer, monitor below 0.6 times consumer, the four means within 12%;
  - `crtEffect` `off` makes all four neutral, `vintage` steps them (neutral stays, consumer and arcade arcade, monitor
    consumer); without video.json every screen is consumer by its kind; the user's `crt_presets.json` read
    (`presetsSource` `user`), a broken one left out (`shipped`).
- Goldens (`test_golden_room.cpp`): G02's CRT `consumer` by its kind, G15's hero cabinet and cabinets `arcade` by the
  pack, G17's hero CRT `monitor` by the pack (the reports' `crtLooks`).

## Results

- Goldens. The machine's NVIDIA driver is now 621.01 and the goldens were recorded under 617.42, so
  `test_golden_room.cpp` skips their comparison here (the reference machine's rule). Each was compared instead with
  `retroverse-screenshot-diff` against its committed image, rendered from this branch in Debug: G01 (`m0_testroom`),
  G02, G02s, G06, G07, G08a, G08b, G10, G11, G12, G13, G14, G16, G18 and G19 **0 pixels apart (largest difference
  0/255)**: every consumer tube, the secondary TVs, the hub and the halls unchanged. Changed on purpose: G17 (the
  lounge's monitor tubes, 1,089 pixels, 0.05%, largest 83/255), G15 (the arcade's screens, 6,953 pixels, 0.34%, largest
  138/255) and G20 (the cabinet in Focus, 58,873 pixels, 2.8%, largest 185/255). The three were regenerated with
  `RV_UPDATE_GOLDEN=1` in Debug (their metadata now records driver 621.01) and pass their comparison after; their
  descriptions name the preset. The other goldens' re-baselining to the new driver is left to main.
- The reports: G02's CRT `consumer` by its kind, G15's hero cabinet and its cabinets `arcade` by the pack, G17's hero
  CRT `monitor` by the pack.
- Four golden runs of the loaded machine (G02s, G06, G08a, G18: another agent's GPU run, this branch's unit run and its
  Release build beside them) showed the room on frame 1 instead of 0 (`world.firstFrame`, the first frame's wait ran
  out); their pixels were 0/255 apart all the same. Not this step's (no world or wait code changed).
- Debug unit tests (`./scripts/test.ps1`): 2193 of 2193 passed (the build clean, no warning).
- Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2355 tests (2193 unit, 162 GPU), all
  passed; 16 skipped: the 15 goldens recorded under driver 617.42 and, as always, the overlay's pick. G15, G17 and G20
  compared and passed; every golden's Release render 0/255 apart from its committed image (the screenshot diff), the
  three regenerated ones included. The two `[crt_presets]` cases passed.
- GPU cost: unchanged by construction (the same program and block; only the block's CRT values differ per screen).

## Screenshots

Hidden, frame-limited Debug runs (`build/i5/step6/`), looked at beside style §15.2 and `example.png`:
- `crt_presets_overview.png`: the four tubes on their pedestals, left to right neutral (a flat picture with square
  corners, the bars clean), consumer (today's tube: rounded corners, a slight bulge), arcade (rounder, its corners
  further in, the bars a little darker at the edges), monitor (nearly flat, small rounded corners). The bars read the
  same from the camera on all four: the effects fade at a distance.
- `crt_close_neutral.png`, `crt_close_consumer.png`, `crt_close_arcade.png`, `crt_close_monitor.png` (2560 x 1440, the
  probes' close-ups): neutral clean and flat, the lamp's sheen on the glass at the right; consumer the familiar fine
  scanlines and grille; arcade visibly deeper scanline gaps and a coarser RGB grille, the picture bowed with round
  corners, still legible; monitor nearly flat, its finer grille hardly visible: the "modern monitor".
- `consumer_g08a_snes_crt.png` (G08a's view: the SNES room's CRT playing its clip, unchanged), `monitor_g17_lounge.png`
  (G17: the lounge's hero CRT and secondary TV flatter, their corners tighter), `arcade_g15_room.png` (G15: the hero
  cabinet and the rows' covers through the arcade tube) and `arcade_g20_cabinet_focus.png` (G20: the clip in Focus
  bowed into the portrait screen with round corners, the neighbouring cabinet's cover with deeper scanlines, its title
  still legible).

## Open issues

- The goldens' driver: G15, G17 and G20 now record driver 621.01, the others 617.42 (skipped on this machine until
  main re-baselines them).
- The step touched more than the plan's ownership note gave it (`rv_render`'s CRT block, `assets/materials`): the
  packs (`room_look`, two theme files), `collection_instance.cpp` and `world_session.cpp` (one hook each) carry the
  pack's preset to the screens. The shader and `rv_render` are unchanged: the block already held the values.
- A floor's scene shown from `--scene` (`retroverse-cli worldplan show --floor`) has no packs: its screens take their
  kinds' presets.
- Phosphor persistence stays deferred (ADR-0016 §7, BACKLOG).
