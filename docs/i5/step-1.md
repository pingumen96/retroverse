# I5 step 1: playlists and fallbacks on the hero screen

Roadmap row: I5 step 1. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §3, §4 and §8 (amending
ADR-0012 §7). One decoder still: the scheduler is step 2.

## What was built

- **`app::Playlist`** (`rv/app/playlist.hpp`, GPU-free in `rv_app_cli`). A room's games with a playable clip, cycle
  after cycle: `shuffle` (the default; `core::Random` seeded by `deriveSeed(room id, screen id, mode, cycle)`, a new
  cycle never starting with the previous one's last game), `sequential` (the sort keys), `favorites` (shuffled with
  their own seeds), `recent` (the 10 newest), `explicit` (`video.json`'s GameIds in order, each once; ids the room
  lacks or without a clip skipped, and listed). The last three fall back to `shuffle` without their games. A clip that
  fails is excluded for the rest of the run, across rebinds too.
- **The hero's turns** (`ScreenPolicy::tick`). The start choice (ADR-0012 §7's) plays for `previewDuration`, or to
  its clip's end when shorter, then the playlist's games; the game just shown is skipped when the playlist has
  another. A selection (focus, inspect, the quick catalog, after the 300 ms dwell) interrupts it and is held while
  anything is hovered; let go, its clip plays to the end of its current pass (at most `previewDuration`, the whole of
  it without a clip), then the game it interrupted plays again from its start and the playlist goes on after it. The
  items' clock runs only while the screen is in view and no game runs; `--video-freeze-pts` advances nothing, so the
  frozen goldens keep their start choice. A room without any clip shows its first game's stills, its card last
  (`chooseStillGame`), never its pattern: ADR-0016 §3 ("with no clip in the playlist the screen shows its game's
  still") and Gate M4's "never standby while the room has media".
- **`video.json`** (`rv/app/video_settings.hpp`, the shipped `assets/config/video.json` with every key, the user's
  `<user data>/config/video.json` merged key by key, read and never written; a user file that does not load is left
  out whole). Step 1 uses `enabled`, the hero's keys and `rooms.<room id>.hero`; the rest is read and checked for the
  steps after. The test room's room id is its scene's name, `m0_testroom`.
- **The title card** (`assets::drawTitleCard`): the generated front label's design drawn directly at 640 x 480 (the
  label's sizes times 2.5, never resampled), on the worker, as the chain's end after the cover. A building's game
  carries its item's text and its label's colour (the placement's seed on the pack's palette), so the card matches the
  item on the shelf; placed games without any media join the screen's games with their card only (never in a
  playlist; the start choice only of a room without media). `drawFrontLabel` now shares its drawing with the card;
  its pixels are unchanged (the hashed table).
- **`standby`** (`assets::makeStandby`, a scene pattern): a dim cold raster with an amber lamp, the building's CRTs' and
  the arcade's hero cabinet's own picture where the colour bars were. The test room keeps its bars (G01 unchanged).
- **The picture held between games.** On a switch the screen keeps what it shows (the video texture holds the last
  clip's last frame; a still is kept as `heldStill`) until the next game's still or first frame is ready. Only a screen
  with no game shows its own pattern.
- **The report.** `video` gains the playlist (`playlistMode`, `playlistRoom`, `previewSeconds`, `playlistOrder`,
  `origin`), `played` and `shown` (each game and each picture, with the run's simulated seconds), `heldFrames` and
  `idleFramesWithGame`.

## Decisions beyond ADR-0016

- **The hero's screen id in the seeds is `hero`** (one hero screen per room), not its object id: an order survives a
  change of the scene's ids. Secondary TVs (step 3) get ids of their own.
- **"Resumes where it stopped"** is the interrupted game played again from its start (one decoder: no position is kept
  for it), the playlist's cursor after it.
- **"To its end"** for a selection let go is the end of the looping clip's current pass, at most `previewDuration`;
  without a clip (stills, the card), `previewDuration`.
- **`video.json`'s `enabled: false`** keeps the turns with the stills and cards; a kind's `enabled: false` leaves the
  screen to selections (no game, its pattern, between them); `showOnFocus: false` ignores selections.
- **The card waits for the clip.** A game whose clip may still play never shows its card over the wait for its first
  frame (the previous picture is held instead); the card comes when the clip fails, is not playable, or clips are off.
  The first version drew it at once in lockstep runs, a flash of card at every switch, which the GPU case caught.
- **The card's room palette** comes from the building's cover items (world_build), so the snapshot's form of the
  screen games carries no card; `test_world_build` compares them without it.
- **The scene goldens are rewritten** for `standby` (`tests/fixtures/world/scene/`: the CRT's pattern name and the
  digests, nothing else): a file I4b's expansion rooms may rewrite too (i5/plan.md, "Files both iterations touch").

## Proofs

- Unit (`tests/unit/app/test_playlist.cpp`, `test_screen_policy.cpp`, `test_video_settings.cpp`,
  `tests/unit/assets/test_collection_labels.cpp`, `test_test_patterns.cpp`): each mode's order as a table over cycles
  (the shuffles from `scripts/world_seed_oracle.py`'s `derive_seed` and `SplitMix64`, independent of rv_core), no
  repeat within a cycle nor across its boundary for seven rooms, the fallbacks to shuffle, the 10 newest, excluded
  clips; the hero's turns frame by frame (the start choice then e002, e003, e001, e002, e001, e003 at frames 120 to 720
  for the test room's three clips at 2 s), a clip shorter than `previewDuration`, the selection's hold and its return,
  a hold renewed by a new hover, frozen runs, autoplay off, the clock stopped out of view and during a game, a failed
  clip leaving the playlist and its card at the chain's end, unknown explicit ids skipped; `video.json`'s defaults,
  overrides and refusals; the card's size, colour, contrast and layout; the standby raster.
- GPU (`tests/smoke/test_video.cpp`, `[playlist]`): the test room's three clips at a 2 s `previewDuration`
  (`video.json` in the case's data directory), `--fixed-dt`: the start choice then the unit table's e002, e003, e001,
  2 s apart; after the first frame every picture shown is a clip, frames held between them, and 10 screenshots around
  the first two switches differ from the colour bars on the tube (probes); the game without media shows its title card,
  and 2 s after the quick catalog closes the start choice it interrupted plays again. The fallbacks case's
  unsupported codec now ends at the card. G01 to G18 in Debug: 15 of 18 0 pixels apart; G06, G13 and G17 are
  regenerated on purpose (below).

## Deviation: three goldens regenerated

The proof row asks for G01 to G18 0 pixels apart. Three of them showed a CRT whose room has no clip, with the colour
bars: G06 (the single ROM without media), G13 (the shared handheld room) and G17 (the optical-era lounge). ADR-0016
gives such a CRT a picture of its game (§3) and ends the chain at the title card, never the bars (§8), and Gate M4
asks for "never standby while the room has media"; so they now show the lone game's title card (G06) and the first
featured game's cover (G13, G17), and were regenerated on purpose (their descriptions say so; looked at). Every other
golden, G01 included, is 0 pixels apart.

## Results

- Debug unit tests: 1964 of 1964 passed. Release unit and GPU (`msvc-x64-release-gpu`): 2085 tests, all passed but
  `launch: a closed catalog draws nothing over the scene`, whose test room now showed the mock game's card; the case
  turns the hero's autoplay off in its `video.json` (fixed, passing in Debug and Release).
- Goldens: G01 to G18 0 pixels apart in Debug and Release, G06, G13 and G17 after their regeneration.
- The playlist run: 4 games played 2 s apart in the unit table's order, 6 to 14 frames held across the 3 switches in
  the runs made (a few frames each while the previous decoder's thread ends and the next clip's first frame comes),
  one frame of the pattern before the run's first frame.
- Screenshots (hidden runs, `build/i5/step1/`): `switch_01.png` to `switch_10.png` around the first two switches (the
  last frame of a clip held, then the next clip), `title_card.png` and `title_card_closeup.png` (the card of a game
  without media), `standby_closeup.png` (a building CRT without a game). The real library was not run: nothing of the
  catalog or the import changed.

## Follow-ups

- A room's first picture after a bind still waits a few frames for a clip's first frame (its pattern meanwhile):
  `idleFramesWithGame` counts them. Holding the previous room's picture across a bind is no switch between clips, and
  step 2's slots change it anyway.
- Placed games without media join the screen's games only in the building's own form (`world_build`), not in the
  snapshot's form, which is the tests' reference.
