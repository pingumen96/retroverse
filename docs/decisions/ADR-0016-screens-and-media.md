# ADR-0016: Screens and media: the decoder scheduler, playlists, audio, CRT presets, fallbacks

- Status: accepted (the user's "procedi" on I4b and I5; the choices below made on the user's behalf in planning,
  listed in [i5/plan.md](../i5/plan.md), "Risks and open questions")
- Date: 2026-10-27
- Amends: ADR-0004 (rows per screen, audio on the decoder thread, the presentation clock, staggering by bytes),
  ADR-0012 §5 and §7 (the viewer's videos and manuals; the hero CRT's playlist, its fallback's end, its sound),
  ADR-0013 §9 (one decoder per floor becomes the scheduler's budget), ADR-0014 §1 and §6 (a secondary TV as a pack's
  furniture; cabinets' screens playing clips; a pack's screen preset); specification §9.3, §9.7 and style §15.1 as
  recorded under "Amendments"
- Relates to: ADR-0003 (no scraper: no "Scarica anteprima"), ADR-0005 (`rv-texture:` pictures), ADR-0006 (the
  launch, the process priority around it), ADR-0011 §8 (hero textures); roadmap I5 (Gate M4)

## Context

I3 put one 480-row decoder on the hero CRT (ADR-0012 §7); I4 moved it to the CRT of the room that holds the eye
(ADR-0013 §9); I6 gave the arcade a hero cabinet carrying the same video and rows of cabinets whose screens show
covers (ADR-0014 §6). Everything else M4 asks for was deferred here:

- **Specification §9:** screen types (§9.1), playlists `SelectedGame`, `RoomShuffle`, `RoomSequential`, `Favorites`,
  `RecentlyPlayed`, `ExplicitPlaylist`, `IdleAttract` (§9.2), priorities (§9.3), at most 2 decoders in the normal
  profile and 1 in the low one, stills for screens out of view, a shared texture for one clip on several screens
  (§9.5), CRT presets with a clean option (§9.6), the clips' sound through miniaudio, spatial and ducked, one dominant
  source (§9.7); NFR-06 (2 s to the first frame), NFR-07 (2 decoders, asserted), FR-017, FR-030, SC-03, SC-08.
- **Style:** one hero TV and at most 1-2 secondary ones per room (§15.3), "the preview stays linked to its physical
  object" in inspection, one dominant TV sound, ducking under a card, mute always at hand (§15.4), stills when the scene
  is out of view (§20.2), the fallback chain screenshot, title screen, cover, idle screen (§21), the insertion into the
  console in 300-650 ms, always skippable (§16.2).
- **ADR-0004's open points:** the process priority boost, upload staggering (two 720p uploads due on one frame reach
  2.1 to 2.7 ms at p99), dropping to one decoder when frames come late, the audio clock, cheaper uploads if needed.
- **BACKLOG:** no colour bars between clips, library-wide checks of the clips (thresholds, an hour-long soak), video
  frames in the UI, videos and manuals in the media viewer.

Facts that bound the design:

- **The clips:** the reference library's are H.264 640 x 480 at 59.94 fps (82% of the games have one, ADR-0003); a
  third of the SNES clips carry 96 kHz audio. 57% of the games have a manual (PDF).
- **GPU memory:** the busiest real floor holds 471.8 MiB of the acceptable 512 MiB (Gate D), the hero CRT's 480-row
  texture and ring (about 4.8 MiB) included.
- **Costs per decoder (ADR-0004):** a 480p clip 7-9% of a core, uploads p99 about 0.4 ms; a 720p one 3.5 MiB per
  frame, p99 1.2-1.4 ms.
- **Libraries at the pinned baseline:** miniaudio 0.11.25 (header-only, Unlicense or MIT-0); no permissive PDF
  renderer (pdfium and mupdf are absent, poppler is GPL).
- **Tests:** they never take the foreground, and must not play sound on the user's machine.

## Decision

### 1. Screens and their kinds

A **screen** is a surface that can show a picture: a `ScreenSurface` or a prefab's picture role, with a material
instance whose base colour the service overrides (ADR-0012 §7's binding, generalized).

| Kind | What it is | Plays by default |
|---|---|---|
| `hero` | the CRT of a collection room's hero corner, or the arcade's hero cabinet | the room's playlist; the focused or inspected game after its dwell (not in the arcade) |
| `secondary` | a small TV a theme pack stands in a decoration socket (§5) | its own playlist, at low priority |
| `cabinet` | an arcade cabinet's screen, a collection item (ADR-0014 §6) | its cover; its own clip when focused, inspected or nearest in attract (§6) |
| `viewer` | the media viewer's video page (§10) | the viewed game's clip |

The hub's information screens and the totem stay boards; neon and signs are no screens. A screen belongs to an area;
only screens of the area that holds the eye (ADR-0013 §9) ask for video.

### 2. The decoder scheduler

`app::ScreenScheduler`, GPU-free in `rv_app_cli`, decides each frame from the screens' requests. `app::ScreenService`
in `rv_app` carries the decisions out: decoders, textures, stills, overrides and audio.

- **Budget.** `maxDecoders` is 2 (the Balanced profile) or 1 (the Low profile, I8's, or `video.json`, or
  `--max-decoders 1`). NFR-07 becomes a runtime assertion on the budget: never more than 2 decoders active, and
  never more decoder threads alive than the budget.
- **Slots.** A slot owns one `media::VideoDecoder`, one `gl::VideoTexture` and one audio ring. A slot opens its next
  clip only once its previous decoder's thread has ended (I3's rule, per slot). The render loop never waits for it.
- **Priority classes** (specification §9.3):
  1. `direct`: the viewer's video, or an inspected cabinet.
  2. `focus`: the screen showing the focused or inspected game.
  3. `room`: the eye's room's hero screen playing its playlist.
  4. `ambient`: secondary TVs and attract cabinets.

  Ties go to the nearer screen, then the screen id, so decisions are deterministic.
- **Sharing.** Requests for the same clip share one slot and one texture (§9.5, style §15.3). Each screen keeps its own
  picture rectangle (§7).
- **Visibility.** A screen is visible when its bounds are in the view frustum and its area holds the eye. No occlusion
  queries.
  - A screen that leaves the view pauses at once and keeps its frame (`VideoPlayback::pause`).
  - It is stopped after 10 s out of view, or when its slot is needed.
  - Its screen then shows its still (§8).
  - A slot taken from a clip still wanted (`needed`, `budget`) leaves its screen its kept frame instead (§6 amended).
- **Stability.** A slot changes hands at most once a second, unless a `direct` or `focus` request asks. Ambient
  requests enter and leave by `proximityMeters` with a 0.5 m hysteresis.
- **Late frames.** A slot whose late plus dropped frames exceed 5% over 4 s drops the budget to 1 for 30 s, and the
  lowest class loses its slot (ADR-0004's open risk). Each drop is logged and reported.
- **Launches and the window.**
  - Before the hand-over every slot is stopped and its thread waited for, at most 1 s in all (ADR-0012 §6), and the
    audio device stops.
  - After the return the slots reopen at their positions.
  - While the window is minimized the slots pause.
  - At shutdown `waitForStoppedDecoders()` runs.
- **Rows.** World screens decode at most **480 rows** in both profiles: the clips are 480p and the screens are small.
  The viewer decodes at most 720 rows in Balanced and 480 in Low. This replaces ADR-0004 point 4's 720 default.
- **Upload staggering.** The video uploads of one render frame carry at most **4 MiB**: one 720p frame or three 480p
  ones. A frame over the budget stays pending one render frame, oldest first, or is superseded (ADR-0004's budget, by
  bytes rather than by count).
- **Process priority** (ADR-0004 point 9). `rv_platform_win` sets `ABOVE_NORMAL_PRIORITY_CLASS` while the 3D view
  renders and the window is not minimized. It returns to `NORMAL_PRIORITY_CLASS` before an emulator starts and while
  minimized. The run report records each change. A thread-only boost and MMCSS were not measured, so not adopted.
- **Cheaper uploads are not built.** Converting straight into the mapped PBO, YUV planes and a second context for
  asynchronous copies stay contingencies, in that order, used only if Gate M4's frame rows fail.

*Amended on 2026-10-28 (I5 step 2's review):* while the window is minimized the slots pause at once and, like screens
out of view, **stop after 10 s** (reason `minimized`), not only pause: the decoders and their threads are freed. An
interactive run's host renders nothing while minimized but still ticks the screens with the time it waits
(`IAppLogic::onMinimized`), and sets the normal priority. "Its area holds the eye" means the room bound: in a hall,
between rooms, the **last room's screens stay bound and may keep asking for video** (paused once out of view, stopped
10 s later) until the eye enters another room. `video.json`'s **`rooms.<room id>.screens.<object id>`** (§4) takes a
kind's keys for one screen; it is for tests and hand-made scenes (`tests/smoke/scenes/screens_four.json`): a
generated room's object ids come from the generator and are not a user-facing name, so users set a room's kinds. During
a late-frame drop, open plus reserved slots never exceed the budget, and a lockstep run (`--fixed-dt`) does not watch
late frames unless `--watch-late-frames` asks (its frames are presented on simulated time, decoded on the wall clock).

### 3. Playlists

`app::Playlist` is GPU-free. It draws its games from the room's items (ADR-0013 §9's items files: the clip, the stills,
the favourite flag, the last play, the sort key). Its order is deterministic: seeds come from `core::deriveSeed(room
id, screen id, mode, cycle)`.

- **`shuffle` (`RoomShuffle`, the default).** A permutation of the room's games with a playable clip. No game repeats
  until all have played. A new cycle reshuffles, never starting with the previous cycle's last game.
- **`sequential`.** The room's sort order.
- **`favorites`.** The room's favourites, in shuffle order; `shuffle` when there are none.
- **`recent`.** The 10 games played most recently, the newest first.
- **`explicit`.** The GameIds listed in the user's `video.json` for that room and screen. Unknown ids are skipped and
  logged once.
- **A clip** plays for `previewDuration` (30 s), or to its end if shorter, then the next one plays. A clip that fails
  is skipped for the rest of the run. With no clip in the playlist the screen shows its game's still.
- **The hero screen (`IdleAttract` + `SelectedGame`).**
  - At the start it plays ADR-0012 §7's start choice, then its playlist.
  - A selection (focus or inspect, after the 300 ms dwell) interrupts the playlist (SC-03).
  - Once the walker is back in free roam, the selected clip plays to its end, at most `previewDuration`. The playlist
    then resumes where it stopped.
  - This replaces ADR-0012 §7's "keeps looping until another item is selected".
  - In the arcade the hero cabinet never follows the selection: the selected cabinet plays the clip itself (§6; the
    quick catalog's selection plays on it since I5 step 4's review, §6 amended).
- **Frozen runs.** Under `--video-freeze-pts` every screen opens the first clip of its playlist (the start choice for
  the hero screen), paused at that time. Goldens stay reproducible and the existing room goldens unchanged.
- **No prefetch** of the next clip: a 480p clip's first frame comes in about 20 ms, and its still covers the switch
  (specification §9.5: prefetch only within budget).

### 4. Configuration: `video.json`

`assets/config/video.json` holds the shipped defaults (JSON with comments). The user's `<user data>/config/video.json`
overrides them per key. Retroverse reads it and never writes it, so the mute state is the run's own.

- **Global keys:**
  - `enabled`;
  - `maxDecoders` (1 or 2);
  - `maxRows` (world screens) and `viewerMaxRows`;
  - `proximityMeters` (12);
  - `crtEffect` (`off`, `light`, `vintage`);
  - `audio`: `mode` (`nearest-focused` or `off`), `master`, `video`, `effects`, `ambience`, `muted`.
- **Per screen kind** (`hero`, `secondary`, `cabinet`): `enabled`, `playlist`, `previewDuration`, `showOnFocus`,
  `volume`, `crtPreset`.
- **Per room** (`rooms.<room id>.<kind>`): the same keys, and `games` for `explicit`.

Specification §9.2's other parameters map as follows:
- `autoplay` is `enabled`;
- `priority` is the kind's class;
- `loop` is the playlist's cycle;
- `maxResolution` is the rows;
- `fallbackImageType` is the chain of §8, which is not configurable.

### 5. Secondary TVs: a theme pack's furniture

A secondary TV is a **look, not a plan fact** (ADR-0014 §1). A pack's furniture may list a `secondary_tv` piece (a
sideboard carrying a small consumer CRT whose screen is a `ScreenSurface`) under a decoration kind. "Consumer" names
the model (style §15.1's CRT), not the look: its preset follows its room's pack like every screen's (§7, amended on
2026-10-30).

- **Where.** The scene builder draws at most one per room (two in a room grown past 12 m), at the sockets of that kind
  that the room's seed picks, within their footprints.
- **What changes.** No plan, slot, migration or navigation changes. Scene digests change, and goldens are regenerated
  on purpose where the piece shows.
- **Where none.** The arcade has none (its cabinets are its screens), and neither do the hub nor the halls.
- **The reader rule.** ADR-0014 §1's rule that refuses `crt` as furniture outside the arcade now admits a
  `secondary_tv` piece in decoration kinds only.
- **What it plays.** Its own playlist (`shuffle` with its own seed by default) in the `ambient` class, with its game's
  still when it has no slot.

*Amended on 2026-10-29 (I5 step 3):*
- **The piece.** One prefab, `secondary_tv` (`geometry::SecondaryTvParams`): the sideboard's body (tv_cabinet's)
  carrying the consumer CRT's model at 0.46 x 0.4 x 0.32 m, its screen the CRT's `ScreenSurface`, its default footprint
  the narrow sideboard's (0.8 x 0.36 m). A pack lists it once, without a weight, in a decoration kind's list (every pack
  but the arcade's and the hub's: under `tv_cabinet`); it is no weighted variant. Its materials are the pack's
  `tv_cabinet` and `crt` kinds'. The reader refuses a second one, a weight, one among a hero part's pieces, and a
  decoration piece of kind `crt`.
- **Which sockets.** The room's decoration fixtures of that kind whose own prefab's footprint the TV fits, ordered by
  `core::deriveSeed(room seed, "secondary_tv", fixture id)`: the first, and in a room deeper than 12 m (14 and 16 m) the
  next one on another wall (else the next one). A room with no such decoration has none (a room whose side walls are
  all modules and whose decoration drew no sideboard). "Past 12 m" is the room's depth, so a 12 m room holds one.
- **Where it stands.** Where its fixture stands, judged within that fixture's own prefab's footprint there, not its
  socket's: a sideboard the room editor moves (I4b) carries its TV, and the editor's collisions and the walkable
  passages judge the plan's fixture, which the TV never leaves.
- **Expansions** (I4b step 7) are rooms of their own: each holds its own secondary TV by the same rule and seed.
- **The screen.** `app::findScreens()` takes the hero by its role (the hero corner's `crt` part, or the arcade's hero
  cabinet with its id), then binds every `secondary_tv` object of the hero's room as a secondary screen (a hand-made
  scene's further `crt`s too). Its distance for `proximityMeters` is the eye's to its object's bounds' middle, as every
  screen's. When the room editor rebuilds a room's static part in place, the bound screens move onto the new objects
  (`ScreenService::rebind()`): a moved TV keeps its object id and, its screen's picture unchanged, its material (the
  registry keys a material by its object, role and picture), its clip playing on (the review below: it plays nothing
  while the editor runs).

*Amended on 2026-10-29 (I5 step 3's review):*
- **Which sockets (replaces "the first, and ... the next one on another wall").** The candidates are ranked by where
  their sockets stand at home (never where the editor moved them), in integers from the plan and the room's seed alone,
  so that the lesser screen never competes with the hero (style §15.3): never the one nearest the doorway's camera (ties
  by fixture id), which the doorway's view shows as large and as bright as the hero; first those in the room's back
  half from the doorway or within 3 m of the hero corner, the farthest from the doorway's camera first among those,
  then the seed's order (sockets mirrored across the room). A room's only candidate is kept only
  where it is preferred. In a room deeper than 12 m the second stands in another third of the room's depth from the
  doorway, else there is none: never a pair mirrored across the room. Plans stay byte-identical; scene digests and the
  goldens where a TV shows change.
- **The reader** admits the TV only under a floor-standing decoration kind (`tv_cabinet`, `floor_lamp`, `plant`): under
  a poster's kind it would float. Its materials, a role's later source winning: the pack's `tv_cabinet`, `crt` and
  `secondary_tv` kinds', then the piece's own (the 90s collector pack's charcoal set: its hero's grey reads ivory under
  the room's warm light).
- **In view as well as within proximity.** An `ambient` screen opens a slot only within `proximityMeters` and in view:
  out of view it takes no free slot (a TV behind the walker on entering opens nothing; after a launch's return, one out
  of view opens again, where it stopped, once in view). One already playing keeps its slot out of view, paused, for
  §2's 10 s.
- **While the room editor runs** (ADR-0015 §4's session) the secondaries ask for no clip: their slots stop
  (`unwanted`) and they show their stills, as the cabinets (§6). A held TV's bounds therefore never steer a decoder.
  After the session they ask again, on the objects `rebind()` (or a new `bind()`) gave them.
- **Rebinding.** `ScreenService::rebind()` matches the bound screens to the objects found again by object id, whatever
  their order (`app::rebindOrder()`), and `findScreens()` takes each screen's material from its object's own render
  items, never the first registry entry under its key's prefix: the registry is append-only, so an object rebuilt with
  another picture (another `@<w>x<h>` or picture hash) leaves its old material there under the same prefix. Screens that
  are no longer the bound ones (another id or kind) are bound again with the room's games (`bind()`), never left on the
  old materials; the report counts the rebinds (`video.rebinds`).
- **The selection.** A secondary follows the world's selection as every screen does (only the arcade's hero cabinet
  does not, §6): its kind's `showOnFocus` (false by default) decides whether it shows the focused or inspected game.

### 6. Arcade cabinets with clips

A cabinet's screen is a collection item showing its cover (ADR-0014 §6). With a slot:
- the item is drawn on the dynamic path, as a pulled-out item is;
- it uses one of a pool of `MAT_Screen_Arcade` video instances (as many as `maxDecoders`), whose texture is the slot's;
- its static instance is hidden.

Which cabinet plays:
- **The focused or inspected cabinet** (`focus`; `direct` when inspected). The clip plays in the hand during the
  inspection: the preview stays linked to its object (style §15.3).
- **In attract** (`ambient`), the nearest cabinet in view within 3 m whose screen faces the eye, after a 1 s dwell.
- **The hero cabinet** keeps the room's playlist (`room`).

With one decoder, a focused cabinet takes the slot and the hero cabinet keeps its paused frame. A cabinet without a
slot shows its cover, as today; its still is the cover.

*Amended on 2026-10-28 (I5 step 4):*
- **Attract's terms.** "Facing the eye" is the direction from the screen's centre to the eye within 60 degrees of the
  screen's normal. The cabinet playing keeps attract to 3.5 m and 70 degrees (a hysteresis, so walking along a row does
  not flip it at the limit), and while another's 1 s dwell runs; it stops at once when it no longer qualifies (out of
  view, turned away, walked away). Attract plays only in free roam: Focus, Inspect and the editor reset its dwell.
- **The classes.** Focus asks `focus`; Inspect, and a launch from it until the return, ask `direct`, so that the
  inspected cabinet's clip position is kept for the return; through all three the cabinet is in the hand (in view, at
  arm's length), whatever its slot's box (I5 step 4's review). A cabinet loops its own game's clip: `video.json`'s
  `cabinet.enabled` is attract and `showOnFocus` the focused cabinet; its `playlist` and `previewDuration` are unused. A
  clip that fails on a cabinet leaves that cabinet on its cover for the rest of the run.
- **The pool.** The cabinets' collection makes its video materials (the label's `MAT_Screen_Arcade` with a base colour
  texture of its own) the first time one plays, as many as the decoder slots: slot i's picture on material i.
  Appended: no handle moves. A pooled material no cabinet shows on draws its own white again (its override and
  rectangle cleared), and so does every one when the cabinets go, unless the registry is being cleared. The cabinets'
  screens are named by their family (`world::kArcadeRoomFamily`), sized by its label (298 x 398 mm). A change of the
  room's games or of the clips' verdicts lets go only the cabinets whose game or clip changed.
- **"Never follows the selection"** is the world's selection (the focused or inspected cabinet plays its clip
  itself): the arcade's hero cabinet never takes it (`ScreenBinding::followsWorldSelection`). *Narrowed by I5 step 4's
  review:* the quick catalog's selection of a game of the eye's room plays on the hero cabinet, as on every hero
  screen.
- **While the room editor runs** (ADR-0015 §4's session) no cabinet plays, attract and focus alike: its fixtures are
  held, moved and have their room's static part rebuilt. Every cabinet shows its cover from the frame the session
  begins; after it, attract starts again from its dwell. A cabinet's clip follows its item by pick id, so a collection
  laid out again in place keeps it; a collection dropped (a floor committed again, a ride) lets every one go.
- **The paused frame** is a GPU copy of the slot's picture that every bound screen (the hero, a secondary) keeps when
  its slot is taken from a clip it still wants (`needed`, `budget`), shown until that clip has a slot again, in place of
  its still (§2's "Its screen then shows its still (§8)." stays for the other cases: a stop out of view, a clip never
  opened).

### 7. The picture's aspect and CRT presets

- **Aspect** (style §15.2). Each screen material instance gains a picture rectangle (`MaterialUniforms`, a `vec4`), set
  from the clip's size and the screen's. Pictures are letterboxed or pillarboxed in the tube's black and never
  stretched: a portrait arcade clip fills a cabinet's 3:4 screen, a 16:9 clip shows bars on a 4:3 CRT. A 4:3 clip on a
  4:3 screen keeps the full rectangle, so its pixels are unchanged.
- **Presets** (`assets/materials/crt_presets.json`, specification §9.6). Each preset is a set of `assets::CrtEffect`
  values; an effect at 0 is off:
  - `neutral`: the clean video (no scanlines, mask, curvature or vignette);
  - `consumer`: today's `CrtEffect` defaults exactly, so every existing screen's pixels stay;
  - `arcade`: deeper scanlines, a stronger mask, more curvature;
  - `monitor`: a fine mask, little curvature.
- **Which preset a screen takes** (the first that applies):
  1. the user's `crtEffect`: `off` makes every screen `neutral`, `vintage` steps each one stronger (`consumer` to
     `arcade`);
  2. the room's `video.json`;
  3. the theme pack's `screenPreset` (the optical-era lounge's `monitor`, the arcade's `arcade`);
  4. the screen kind's default.

  A preset is a look: it moves nothing.
- **Phosphor persistence** needs the previous frame, and is not built (BACKLOG). The "light bloom" is the scene's bloom
  on the emissive screen.

*Amended on 2026-10-28 (I5 step 4):* the aspect is the clip's frame as decoded (square pixels: the stream's sample
aspect applied) against its screen's `ScreenSurface` (a cabinet's: its family's label since the step's review). A clip
within 2% of its screen's aspect fills it (an encoder's crop such as 640 x 478 or 584 x 446 shows no hairline bars);
beyond, it keeps its own (but see the next amendment for a console's low-resolution frame). The rectangle
(`MaterialUniforms::pictureRect`, a `vec4` after the CRT fields: the block grows from 96 to 112 bytes) is read only
under a new flag (`kPictureRect`), set only while a clip of another aspect shows, so that a 4:3 clip on a 4:3 tube draws
exactly as before; a still (fitted to 4:3) and a screen's own pattern fill their screen. The bars are the tube's black:
the picture's coverage is anti-aliased over a pixel, and the tube's scanlines, mask, vignette and rounded edge stay the
screen's.

*Amended on 2026-10-29 (I5 step 4's review):* a console's low-resolution frame keeps the aspect its television gave it
(style §15.2, the original aspect), with no per-system data: a frame of at most 240 rows whose aspect is within 15% of
4:3 (256 x 224, 8:7; 320 x 224) is shown at 4:3 (`app::displayAspect()`), so it fills a 4:3 screen and is letterboxed
as a 4:3 picture on a cabinet's portrait screen. A 256 x 240 frame (20% off) and every clip of more than 240 rows keep
their own aspect. The bars are the lit tube's off-black (`RV_CRT_BLACK_LEVEL`, linear 0.01, through the tube's
scanlines, mask and vignette), as a powered CRT's black, not 0: they never read as a hole in the screen. A 4:3 clip on
a 4:3 tube is unchanged (the flag stays clear).

*Amended on 2026-10-30 (I5 step 6), the presets as built (`docs/i5/step-6.md`); the order of the rules is unchanged:*
- **`consumer`** is the tube every CRT screen drew before the presets, the values `MAT_Screen_CRT` and
  `MAT_Screen_CRT_Room` carried (240 lines at 0.35, 320 triads at 0.25, curvature 0.06, vignette 0.3, glass 0.25),
  which `CrtEffect`'s defaults now are (its former defaults were never drawn: every screen material named all seven
  values). The screen materials keep `"crt": {}`, the mark of a tube; its look is its screen's preset.
- **Rule 2** is video.json's `crtPreset` for the screen: its screen's (`rooms.<room>.screens.<id>`), else its room's
  (`rooms.<room>.<kind>`), else its kind's (`hero`, `secondary`, `cabinet`), the shipped `""` being none at every
  level (a room's or a screen's `""` leaves its kind's key). A kind's key is the user's choice, as a room's is;
  **rule 4** is the kind's built-in default: `consumer`, the cabinets `arcade`.
- **`vintage`** steps `monitor` to `consumer` and `consumer` to `arcade`; `arcade` and `neutral` (the clean video,
  chosen on purpose) stay. `light` keeps the preset chosen.
- **The pack's `screenPreset`** is every screen's of its rooms: the hero, the secondaries and the cabinets. The
  arcade's cabinets take one look per room (their collection's material): a per-screen key does not reach them.
- **Every screen of the scene shown** takes its look when its material is registered (`app::ScreenLooks`), the bound
  room's and the others' seen through their doorways alike; nothing changes per frame.
- **Goldens:** the arcade's screens change with the lounge's on purpose (`arcade` is deeper than the cabinets' former
  tube): G15 (the arcade room) and G20 (a cabinet in Focus) are regenerated beside G17 (Costs, below).

### 8. Fallbacks and missing media

- **The chain** (style §21, SC-08): clip, screenshot, title screen, cover, then a **generated title card**. The card
  shows the game's title and year and the system's full name in the bundled font, on the room pack's label palette:
  - it is 640 x 480, made on the worker with `rv_assets`' label rasterizer;
  - it is never a trademark;
  - it replaces the colour bars as the chain's end.
- **Standby.** A screen with no game at all shows a dim original raster (a new scene pattern, `standby`). The colour
  bars stay the test room's own pattern, so G01 is unchanged.
- **Between clips.** The previous clip's last frame stays up until the next picture is ready: the next game's still,
  else its first frame. Never standby, never bars (the BACKLOG item).
- **Video verdicts.** `<user data>/cache/media/videos.json` is a canonical JSON keyed by the clip's path under the media
  root, its size and its time. Each entry holds `playable`, or the `DecodeFailure` with its duration, size and audio
  rate.
  - It is written by the app when a clip fails or plays through once clean, and by `retroverse-cli media
    --check-videos`.
  - A clip whose verdict is a failure and whose file is unchanged is never opened: its screen goes to its still at once.
  - A changed file is checked again.
  - Nothing is ever written under the media root.
- **`media --check-videos [--system <name>] [--jobs N]`.** It decodes the first 2 s of each clip and 1 s from its
  middle, and checks its streams' durations, at below-normal priority with 2 jobs by default. It is resumable and
  reports counts per failure kind and system. This settles ADR-0004's thresholds on the whole library.
- **No "Scarica anteprima"** (specification §9.3): there is no scraper (ADR-0003). I7's media coverage report reads
  the verdicts.

*Amended on 2026-10-28 (I5 step 7's review):*
- **What a verdict holds:** the video stream's duration in milliseconds, its coded size, the first audio stream's rate,
  the codec, the decode errors and damaged frames counted, and its source (`check` or `play`). Its file's time is the
  last write time in FILETIME ticks as the folder lists it; paths are compared folded, as the file system compares
  names. The format is version 1, documented in `video_verdicts.hpp`.
- **What the check decodes:** the first 2 s, 1 s from the middle and the last second, each clip within 60 s. A stream
  that ends after either seek is held to ADR-0004's truncation rule (90% of the video stream's own duration, less
  0.5 s), so a clip cut before or after its middle fails as `truncated`. A clip that declares at most 7 s is decoded
  whole, under the decoder's own first-pass rule.
- **What the CLI checks:** the files of ES-DE's video extensions in each system's videos folder, not the catalog's
  games. It removes the verdicts of clips gone from a folder listed whole. `--recheck-failures` checks the clips with
  a failure verdict again. A merge that fails keeps its verdicts for the next one.
- **What gives no verdict:** a missing file, a file the system could not open, read or seek (the decoder's new
  `ReadError`: access denied, a sharing violation, a device error), and the app's own texture failure; the CLI lists
  such a clip as not checked. A clip is playable only after a clean first pass from its start. The app keeps only the
  failures, and of the playable clips their keys and files' facts; it looks at a failure's file (size and time) on its
  verdicts thread, once per clip and run, the clip left out meanwhile.
- **When "never opened" holds:** from the first frame in lockstep runs. An interactive run that reads its verdicts
  after its first room is bound may open a known-bad clip once.
- **Two writers** read, merge and write under a named mutex of the file's path, and readers take it too (a rename over
  a file another program holds open fails). The app waits at most 2 s and counts a missed batch in `writeFailures`;
  the CLI waits 10 s and retries a missed batch at its next merge. A file that cannot be read is never replaced, only a
  text that is not the verdicts' (or of another version).
- **How long a verdict lasts:** until its file changes. Deleting the file forgets every verdict, and
  `--recheck-failures` the failures; an FFmpeg upgrade (or a change of ADR-0004's thresholds) raises the format's
  version, which replaces the file.

### 9. Audio

- **The module.** A new module, `rv_audio`, GPU-free, depends on `rv_core` only.
  - It uses **miniaudio** 0.11.25 from vcpkg: header-only, its implementation in one source of `rv_audio`, its headers
    external so that `/W4 /WX` still applies to our code.
  - It is a new dependency, not a baseline change. `docs/third-party.md` records it.
  - Layer rules forbid `rv_audio` from reaching SDL3, glad, FFmpeg, RmlUi or SQLite.
  - `rv_media` stays FFmpeg's only home. `rv_app` connects the two.
- **Decoding.** The clip's first audio stream is decoded on its decoder's thread, keeping one thread per clip
  (ADR-0004 point 2).
  - It is resampled by swresample to the mixer's rate, stereo float: 44.1, 48 and 96 kHz sources, mono up-mixed,
    more than two channels down-mixed.
  - It goes into a bounded ring of 0.5 s per slot, timestamped.
  - A slot that is not audible discards its audio packets, so it costs no audio decoding.
- **The clock.** The render clock stays the master: `media::PresentationClock` is deterministic under `--fixed-dt` and
  its pacing is proven. Audio follows the video's media time:
  - within 20 ms, nothing is done;
  - from 20 to 120 ms, the source's resampling ratio corrects by up to 1%;
  - beyond 120 ms (a seek, a loop, a resume, a stall), a hard resync drops samples or pads silence.

  A pause fades the sound out in 30 ms.
- **One dominant source** (§9.3, style §15.4). The audible screen is the visible one of the best class, then the
  nearest, within `proximityMeters`. A change cross-fades in 300 ms; the others are silent.
  - **Gain** is 1 within 1.5 m, then 1.5/d, fading to 0 over the last 2 m before `proximityMeters`. A screen outside
    the eye's area is silent.
  - **Pan** is equal-power, from the screen's azimuth, limited to +-0.6.
  - **Volumes** multiply: master, then the bus, then the screen, then the spatial gain, then the duck.
  - **Ducking** is -9 dB while the card, the directory or the quick catalog is open. The viewer's own clip plays at
    full volume while the world is paused.
- **Devices.** `--audio device|null|off`.
  - `device`, the default of interactive runs, is miniaudio's shared-mode default endpoint.
  - `null` never touches the operating system's audio stack: the main thread pulls the mix, `dt` x rate frames per
    frame under `--fixed-dt` (deterministic), otherwise the elapsed time.
  - `off` builds no mixer and decodes no audio.
  - Every frame-limited run defaults to `null`, and `RV_AUDIO=null` forces it. The unit and gpu presets and
    `scripts/real-library.ps1` set `RV_AUDIO=null`. Every smoke case asserts the report's `audio.device` is `null` or
    `off`, so no test can play sound on the user's machine.
- **Mute.** The `toggle_mute` action (F8; no pad button by default, I7 remaps it) and `--mute` mute at once.
- **Launches.** Before a launch the mix fades out in 50 ms and the device stops, leaving the endpoint to the emulator.
  It restarts after the return.
- **Effects and ambience.** These buses exist with their volumes, but carry no sounds in I5: interaction sounds and
  ambience need original sounds with provenance (I8).

*Amended on 2026-10-28 (I5 step 5's review):* the sync is the mixer's: swresample only converts the rate on the
decoder's thread, and each source reads its ring at 1 +- up to 1% by linear interpolation (an exact copy at ratio 1),
the correction growing with the error, full from 30 ms, and once started continuing until the error is within 5 ms
(hysteresis), so "within 20 ms nothing is done" applies only before a correction starts. A clip's sound is trimmed at
its picture's end (the last finished pass's end, else the video stream's declared duration) and drained at each pass's
end, so every loop starts in sync. The ring drops and never blocks: what does not fit is counted
(`audio.sync.overflowFrames`) and healed by the next resync; a sample that is not a finite number is written as silence.
The pan is equal-power with its centre at unity, so a centred source passes bit for bit, and neither channel's gain
exceeds 1 (the louder side at unity, the quieter at the equal-power ratio); it is ramped per sample like the gain, at
most 1 in 50 ms. The audible screen changes only when a candidate of its class is at least 0.5 m nearer, one of a higher
class appears, or it no longer qualifies. Audible means in the frustum with the eye in the bound room: in a hall the
last room's screens keep their video (§2's amendment) but are silent. A source's gain rises only once it sounds; a newly
audible slot decodes from where its demuxer stands, so about 100 ms of silence precedes its 300 ms fade-in; a slot given
its next clip while the previous one still sounds lets that one fade out (30 ms) first. Mute fades in 10 ms, the duck in
100 ms. `RV_AUDIO`, trimmed and in any case, forces `null` or `off`, never `device`; other values are ignored with a
warning. A device that does not open falls back to `null` (reason `device-failed`); a device lost (miniaudio's stopped
notification) is tried again at once, one not restarted after a launch every 2 s, never while suspended
(`audio.deviceFailures`, `deviceLosses`, `deviceRecoveries`). Before a launch the fade begins before the screens'
decoders stop and the device stops once it has played out. `audio.mode: off` means off, and the model viewer is off.
Lockstep runs (`--fixed-dt`) wait at most 100 ms per frame for the heard clip's sound to be decoded 40 ms past its
picture. §12's row "a clip's sound against its picture" is measured at the mixer's output; device latency (one WASAPI
shared period) is not compensated.

### 10. Videos and manuals in the media viewer

- **The `videos` page** comes after the title screen. It plays the clip with its sound in the `direct` class at
  `viewerMaxRows`.
  - `ui_activate` pauses it and plays it again.
  - The world's screens pause while the viewer is open. In Low the world's slot is taken, and its screen shows its
    kept frame while its clip waits for a slot (§6 amended).
  - The frame reaches RmlUi as `rv-texture:media-viewer-video`, through a `GL_RGBA8` view of the video's
    `GL_SRGB8_ALPHA8` texture (the UI composites encoded sRGB values, as `devui::TexturePreviews` does).
  - A clip whose verdict is a failure shows the page's fallback with its reason, from the RML document.
- **The `manuals` page** names the manual and offers "Apri il manuale". The action opens the PDF in Windows' default
  program through `rv_launcher`:
  - the plan is `explorer.exe` with the file as its one argument, run through `Win32ProcessRunner`, never a shell
    string;
  - it is not a game session;
  - the automation guard refuses it in automated runs unless allowlisted;
  - Retroverse keeps running.

  In-app pages need a permissive PDF renderer outside the pinned baseline, which needs a new ADR (BACKLOG).
- **Not built in I5:** a video preview in the quick catalog's card (I7, BACKLOG).

### 11. The insertion animation (style §16.2, specification §8.3)

Included, as `AnimatingToConsole` between Inspect and Launching:
- **When.** Avvia from the world's card in a room whose hero corner has a console. The quick catalog, the arcade and a
  room without a console have no animation.
- **The motion.** The held item eases over 450 ms to the console's media socket (added to the console prefabs where
  missing: a socket is no plan change) and shrinks into it. Meanwhile the hero screen shows the game's title card and
  the sound fades.
- **Skipping.** Any button skips it, and `reduceMotion` makes it instant.
- **The hand-over** starts after both the animation and the decoders' stop (at most 1 s), so the animation hides the
  stop's wait.
- **Failures and the return.** A refused or failed launch returns to Inspect with the item in the hand. The return is
  ADR-0012 §6's: Inspect, the item back on its shelf.

### 12. Budgets and acceptable performance

- **GPU memory.** The world's video is at most two 480-row slots (texture and ring, about 4.8 MiB each). The viewer's
  720-row slot is about 14 MiB, allocated only while it plays. Stills and title cards stay within the hero textures'
  8 (ADR-0011 §8).
  - The busiest real floor rises from 471.8 to about 495 MiB with everything playing.
  - If a step measures it over 512 MiB, the first remedy is per-floor residency of the procedural surfaces (about
    34 MiB, BACKLOG), not the video.
- **CPU.** Two 480p decoders use about 15 to 20% of one core. Uploads stay within 4 MiB per frame (§2).
- **New rows in the roadmap's "Acceptable performance".** Video's "first frame within 2 s" stands.

  | Measure | Acceptable |
  |---|---|
  | Frame time with two clips playing (the benchmark path past a secondary TV) | p50 at most 16.7 ms, p99 at most 33.3 ms |
  | GPU memory with the screens playing (the busiest floor, two slots; the viewer's clip) | all textures at most 512 MiB |
  | A clip's sound against its picture | within 80 ms at p95 |

## Amendments

- **ADR-0004.**
  - **Point 4:** world screens decode at 480 rows in both profiles, the viewer at 720 in Balanced.
  - **Point 2:** the clip's audio is decoded on its decoder's thread.
  - *Amended on 2026-10-28 (I5 step 7's review):* **point 2's thread** is a worker the process keeps: a decoder runs
    on an idle worker, or on a new one when none is idle, and the worker takes the next decoder once its own has ended.
    There are as many workers as decoders ever ran at once; `stop()` still never waits, and `threadRunning()` and
    `waitForStoppedDecoders()` follow the decoder's run on its worker.
  - *Amended on 2026-10-28 (I5 step 7's review):* **point 5's buffers.** A frame's buffer extends 256 bytes
    (64 pixels) past its last row. swscale's SIMD converters write whole blocks past a row that is no whole number of
    them: a 140 x 96 clip corrupted the heap. A row's overrun lands in the next row, written after it. This holds for
    one-threaded swscale filling rows top to bottom; any buffer swscale writes into, the zero-copy PBO contingency
    included, needs the same padding.
  - *Amended on 2026-10-28 (I5 step 7's review):* **point 5's "the hour-long soak settles the rest".** The soak (a
    Release app, four screens switching clips every 4 s at about 165 frames a second) grew about 110 MiB an hour, from
    two causes. A thread per clip: every thread that ended while the process presented OpenGL frames cost 36 to 73 KiB
    in the graphics driver (`tests/platform/test_thread_soak.cpp`; nothing with an idle context, nothing for a thread
    that lives on), so the decoders run on workers (point 2's amendment). And the run report's per-frame samples, about
    80 bytes a frame, now exact for 65,536 samples and a histogram within 1% after them. After both, 15 minutes of the
    same soak (294 decoders, 150,000 frames) held the private bytes at 258.0 MiB (minutes 1 to 3) and 258.7 MiB (the
    last 2), against +9.0 MiB in under 7 minutes before; handles and threads stay flat. Gate M4 requires the soak's
    growth explained and bounded.
  - *Amended on 2026-10-28 (I5 step 7's review):* **the open risk "Edge content".** The thresholds stand on the whole
    library: 32,809 clips, 32,806 playable, no near miss; 2 `invalid-file`, 1 `corrupt-data`.
  - **The open risk "Audio":** the render clock stays the master and audio follows it (§9).
  - **Budgets:** staggering by bytes (4 MiB per frame); dropping to one decoder on late frames.
- **ADR-0012.**
  - **§5:** the viewer gains its video and manual pages.
  - **§7:** the hero CRT plays a playlist and returns to it after a selection; its chain ends at a generated title card,
    not the colour bars; its clip has sound; the one-decoder assertion becomes the budget's (§2).
- **ADR-0013 §9.** "The hero CRT ... with one decoder" reads: every screen of the area that holds the eye, within the
  scheduler's budget.
- **ADR-0014.**
  - **§1:** a `secondary_tv` piece is admitted in decoration kinds, and a pack may name a `screenPreset`.
  - **§6:** cabinets' screens play clips on the dynamic path (§6 above).
- **Specification.**
  - **§9.3:** no "Scarica anteprima" (ADR-0003).
  - **§9.7:** interaction sounds and ambience are I8's.
  - **§9.1:** the "Monitor moderno" is the optical-era pack's `monitor` preset on every screen of the lounge's rooms
    (the hero CRT and the secondary TVs: §7 as amended on 2026-10-30); the information monitor is the hub's board.
- **Style §15.1.** One consumer CRT model in two sizes (the hero and the secondary). The LCD and projection screens are
  deferred.

## Consequences

**Gains**
- **The rooms.** Every room's hero screen plays a rotating playlist with sound, follows the selection and comes back.
  Rooms gain a secondary TV, the arcade's cabinets play their own games, and the viewer plays clips.
- **Bounds.** Decoders and GPU memory stay inside the acceptable limits and NFR-07's assertion.
- **No colour bars** ever show between clips or for a game without media.
- **Broken clips** are found once, library-wide, and never opened again until they change.

**Costs**
- **New pieces:** a new module and a new dependency (miniaudio), a GPU-free scheduler with many states (unit-tested by
  tables), and a material uniform (the picture rectangle).
- **Goldens:** those of rooms that gain a secondary TV, and the optical-era lounge's (its preset), are regenerated on
  purpose; since I5 step 6 (§7 amended) the arcade's too (G15, G20: its preset). Two new goldens: a console room with
  both screens playing frozen, and a cabinet playing in Focus.
- **The process** runs above normal priority while it renders.

**Deferred**
- In-app manual pages (a permissive PDF renderer).
- A video in the quick catalog's card (I7).
- Interaction sounds and ambience (I8).
- Phosphor persistence; LCD and projection screens.
- Hardware decoding (beyond v1), and more than 2 decoders in a High profile (I8, after measurement).
- The hub's screens playing favourites' clips.
