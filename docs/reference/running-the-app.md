# Running the app

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

`retroverse.exe --help` lists every option. Automated runs must always be frame-limited so no process is left
behind: `--smoke` (render `--frames N`, default 3, then exit; any high-severity GL debug message fails the run) or
`--exit-after-frames --frames N`. `--hidden`, `--frames` and `--screenshot` require one of them; frame-limited runs
never show message boxes. Exit codes: 0 ok, 1 smoke check failed, 2 initialization failure (window, context,
GL < 4.5, paths, scene file, input script, mock catalog), 3 runtime failure (unexpected error, `--screenshot` not
written, scene shader never built, `--self-check` failed, developer overlay asked for by a frame-limited run unable to
start, run report not written, the GPU memory accounting missing a texture of the cache). Logs:
`<user data>/logs/retroverse.log` (`--data-dir <dir>` keeps test runs out of `%LOCALAPPDATA%`; use `--data-dir
build/rvdata`).

- Host options of both executables, besides those described below: `--width`/`--height` (the window, default
  1280x720), `--fullscreen` (borderless), `--no-vsync`, `--gl-selftest` (provokes one GL_INVALID_ENUM to prove that
  OpenGL errors reach the log; a `--smoke` run still passes when it is the only one), `--asset-root <dir>` (else
  `RV_ASSET_ROOT`, the source tree in developer builds, `<exe>/assets`), `--data-dir <dir>` or `--portable` (user data
  in `<exe>/RetroverseData`, else `%LOCALAPPDATA%\Retroverse`), `--log-level trace|debug|info|warn|error`,
  `--no-dialogs` (implied by every frame-limited run), `--version`, `--exit` (alias of `--exit-after-frames`).
- `--run-report <file.json>` (both executables) writes a JSON report when the run ends, also when it fails, once the
  command line is valid (an invalid one, `--help`, `--version` and `--write-fixture` write none;
  `rv/app/run_report.hpp`): version and build type, overlay and debug output built in, the asset root and its source,
  exit code, outcome and first failure, `openGlUnavailable` (no OpenGL 4.5 core context), the GL vendor, renderer and
  version (the driver), debug messages per severity (those `--gl-selftest` provoked included; its `glSelfTest` record
  counts them apart, so in a clean run `high` equals its `highSeverityCaptured`), frames, the last view's statistics
  (draw calls, batches, instances, triangles, culled items, shadow and depth draw calls, GPU times), the instance
  buffer's capacity, the screenshot written, the walker's final pose (feet, yaw, pitch as `--spawn` takes them), the
  self-check, the quick catalog (`catalog`: `source` `esde` or `mock`, file, games, times `opened`, `open` at the end,
  the selection; for the ES-DE catalog since I2 step 13 also `notice`, `library`, `systems`, `search`, `filter`,
  `times` {`openedMs`, `firstListMs`, `firstPageMs`} from the process's creation (NFR-01), `pages` and `frames` (p50,
  p95, p99, max of the page fetches and of the frames while it showed games), `interruptedSessions`, `cardSqueezed`
  (the card's blocks the layout cut: empty in every smoke case), `listed` and `shownCount` (the games of the list and
  the count the screen showed, equal once a list from the catalog's worker is shown)) and every launch (`launches`:
  game, profile, final state, `outcome` with `reason` and `detail`, exit code, the panels shown in order (`panels`),
  `frames` of the request,
  start and end, `idleWakes`, `times`; for the ES-DE catalog `before` and `after`, the selection, search, filter and
  walker's pose when Avvia asked and on the first frame after the end, and `session` {`id`, `error`, `verified`}), and
  since I3 these sections, written by every report (empty or not): `frameTimes` (p50, p95, p99, max swap to swap, the
  first 30 frames left out), `phases` (named spans opened by the code, `refresh` for one, or by an input script, each
  with its frames and frame times), `cpuSections` (main-thread sections: `productUi`, `sceneSubmit`,
  `collectionPlanning`, `layerUploads`, `videoUpload`; p50, p95, p99, max over the frames each ran in), `gpuMemory` (the
  renderer's own accounting per kind: `proceduralSurfaces`, `collectionArrays`, `heroTextures`, `video`, `targets`,
  `sceneTextures`, `buffers`; now and peak), `video` (the hero CRT: the game and picture shown, the one decoder, the
  first frame and uploads, the fallbacks with their reasons, the launches' stops and reopenings, since I4 step 6b
  `binds`, the screens bound after the run's first (at a room change, or a scene shown in place of another; the first,
  at the start room or the first room the eye stays in, is not one): frame, screen, games, `bindMs`, decoder threads
  still ending, `clipStopped` (a clip open on the previous screen, stopped, never waited for); since I5 step 3's review
  `rebinds`, the bound screens moved onto their objects replaced in place (the room editor rebuilt the room's static
  part: no bind, their clips playing on); `waitsBeforeExit`, the waits for the decoders' threads before the run's end,
  0; `{}` without a CRT),
  `world` (`{}` without a room: the state, notice, outcome and rebuild, the plan's digest, placements per section and
  `unplaced` per reason (`roomKept` since I4 step 1), items, clusters, lights, findings, `times` from the process's
  creation; since I4 step 5a `floor` (the floor shown), `rooms` (its rooms: `id`, `floor`, `x` and `z` in millimetres
  and `yaw` in half turns, their placements on the floor) and `room` (the room that held the eye at the end, "" in a
  hall); since I4 step 6b `screenRoom` (the room whose CRT was the hero CRT at the end) and `times.job` (the building
  job's phases: `lockMs`, `selectMs`, `dataMs`, `planMs`, `saveMs`, `itemsMs`, `floorMs`, `totalMs`, and `skipped` when
  its stamp let it plan nothing); the placements per section are the floor's, the placements and unplaced games the
  building's; `preview`, `reconcile` (since I4 step 6b with `rooms`, each room's part: `room`, `kind` `unchanged`,
  `textures`, `items`, `added`, `removed` or `rebuilt`, its items compared in its own frame, `coversChanged`; and
  `walker`, where a floor committed again kept the walker: `area`, `shift`, `halfTurn`), `itemsWritten`, `screenGames`
  (the bound CRT's games), `objectFallbacks` of the persisted start; `interaction` (events, counts,
  `inspect`), `card` (`shownFrames`, `coverFrames`, `squeezedFrames`, what it showed, `focused`, `coverBox`, `events`),
  `edits` (I4b step 3: one per editor session, `room`, `via` `directory` or `card`, `enterFrame`, `readyFrame`,
  `prepareMs`, `failed`; `operations` with `frame`, `op`, `operation`, `refused` (its rule), `rebuild`, `path`
  `in-place`, `room` (I4b step 4: the room's static part rebuilt alone), `floor` or `none`, `editMs`, `shownFrame`,
  `shownMs`; `undone`, `menus`, `probes` (I4b step 4: a held fixture's places judged), `end` `saved`, `discarded`,
  `exited`, `failed` or "" still open, `endFrame`, `written`, `replayed`, `saveRefused` (a rule, or `failed`),
  `snapshot`, `planDigest` (the plan shown at the end),
  `saveMs`, `backMs`, `liveUpdateDeferred`); `regenerationInputsReleased` (I4b step 6's review: the times the
  regenerations' inputs were let go of, the directory closed with nothing pending); `regenerations` (I4b step 6: one
  per preview asked, `kind` `room`,
  `floor`, `world` or `undo`, `scope`, `via` `directory` or `notice`, `requestFrame`, `previewFrame`, `previewMs` (from
  the request to the preview shown), `previewJobMs`, `inputsTaken` and `inputsMs` (the catalog's snapshot and
  selections, when the preview took them), `regenerateMs`, `failure`, the counts `roomsRegenerated`, `roomsAdded`,
  `roomsRemoved`, `roomsMoved`, `gamesMoved`, `faceOutEntering`, `faceOutLeaving`, `newlyPlaced`, `newlyUnplaced`,
  `fixturesAdded`, `fixturesRemoved`, `fixturesMoved`, `pinsKept`, `conflicts`, `overrides`, `changed`,
  `undoSnapshot`, `confirmations` (the drop's second confirmations shown), `choice` `regenerate`, `regenerate-drop`,
  `undo` or `cancel`, `choiceFrame`, `written`, `reused` (the preview's own result committed), `snapshot`,
  `planDigest`, `commitMs`, `backFrame` and `backMs` (from the choice to the frame that shows the room again), `path`
  `in-place`, `floor` or `none`, `walker` `doorway:<room>`, `elevator` or `kept`, `end` `committed`, `cancelled`,
  `failed` or "" still open);
  `launch` (`launches`, `returned`, `notReturned`, `showInRoom`, `walkerBefore`, `walkerAfter`, `poseRestored`, the
  placement asked, `events`), `liveUpdates` (one per update after a refresh: kind `unchanged`, `textures`, `items`,
  `rebuild` or `kept`, kept, moved, added, removed, the arrays grown, `rooms` and `walker` as `reconcile`'s); since I4
  step 7b `floors` (the plan's floors in the elevator's order: `id`, `order`, `rooms`, `placed`), `rides` (each
  elevator ride: `from`, `to`, `reason` `floor`, `room` or `showInRoom`, `room`, `game`, the frames `requestedFrame`,
  `fadedOutFrame`, `swappedFrame`, `fadedInFrame`, the milliseconds from the request `builtMs`, `residentMs`, `totalMs`
  (the view back: the acceptable row's measure) and the floor job's `jobMs`, since I4 step 8 `viewItems` and
  `viewPending` (the covers in view of the room that holds the eye when the view came back, those not resident yet) and
  `roomResidentMs` (the destination room whole, followed after the view is back while the walker stays on its floor),
  `outcome` `arrived`, `timed-out`, `failed` or `running`, the `walker` at the end, `arrayBytes`, `gpuBytes` and
  `gpuKinds` (each kind's bytes) then), `teleports`
  (`frame`, `to` a room or
  `elevator`, `reason`, `walker`) and `elevatorPromptFrames`),
  `residency` (the collection's layers, the
  floor's totals since I4 step 5a and each collection room's in `rooms`: `room`, `items`, `viewItems` (its items in
  the cover loader's first view: none for a room that neither holds the eye nor has its doorway in view),
  `frontsResident`, `spinesResident`, `pending`, `arrayBytes`, `arrays` and `ownArrays`, the arrays its materials
  sample, all its own, since I4 step 6b `uploads` and `reuploads` (layers uploaded into its collection, and of them
  over a picture its item showed already: 0 for a room a reconcile left unchanged); `adopted.resident`, the items a new
  cover loader took over as they showed, nothing served again:
  format, array bytes, fronts and spines resident, covers, labels and failures, the cache's hits, misses and stores, `times`
  {`shownAtMs`, `allResidentAtMs`, `residentAfterShownMs`}, `arrival`, `uploads`, `lockstep`, `pausedRounds`,
  `retries`, `hero`), `screens` (the media viewer and Informazioni: `viewerFrames`, `infoFrames`, `pictures`,
  `failures`, `pictureBox`, `stageBox`, `events` with the game and `from` card or catalog) and, since I4 step 7b,
  `directory` (`{}` without the building: `opened`, at the end `open`, `tab`, `view`, `notice`, `gamesState`;
  `gamesRequests`, the `games` last listed with `gameId`, `title`, `system`, `favorite`, `played` "YYYY-MM-DD"; the card
  last shown `cardGame`, `cardRoom`, `cardOutcome`; `events` `open` (tab, `elevator` or `map`), `command`, `refused`,
  `games`, `card`, `launch-ended`, `closed`); also `catalog.refreshes`
  (one per refresh: trigger, outcome, times, the longest slice, counts, the lists asked meanwhile, `before` and
  `after`), `catalog.lists` (the waits of the lists), `gpuPasses` in `frameTimes` and in every phase (`shadow`,
  `depth`, `ssao`, `scene`, `post`, `outline`, `total`: p50, p95, p99, max over the frames past the warm-up that
  reported them; I3 step 17b), and in `lastView` the static instance groups (`staticGroups`), `outlinedInstances` and
  `gpuMilliseconds.outline`, only when the view has them; since I4 step 3, for a scene with areas, `sceneAreas`
  (`areas`, the `area` that held the eye at the last view, the `lights` selected for it by name and its `keyLight`,
  and `changes`, the area at the first view and each change with its frame; since I6 step 1 `packed`, `last` and
  `samples`, see `--scene`); since I4b step 4's review `sceneContents` (the scene's `objects`, the `colliders` of its
  collision world and its `lights` at the end). Phases opened by the code: `refresh`,
  `coverLoad`, `animation`, `screenBind`, `elevator` (each ride, I4 step 7b: its frames behind the fade are exempt from
  the limits), and by input scripts, `benchmark` for one. Frame-limited runs log the frame times (with the GPU passes),
  phases, CPU sections and GPU memory at the end. Read it instead of the log in scripts and tests.
- `--scene <name|file>`: a name loads `<asset root>/style_tests/reference_scenes/<name>.json` (default
  `m0_testroom`; also `material_test`, `instancing_stress`, `instancing_growth`), anything with a path separator or
  `.json` is a file. Scene objects can name glTF models (`"model": "<asset-relative path>"`, loaded once per path).
  A scene file may hold `"areas"` (I4 step 3, ADR-0013 §9; `rv/scene/scene_description.hpp`): parts of the scene,
  each an `id` and `boxes` ({`min`, `max`} in meters), whose lights are their own; every light then names its
  `"area"`. Each frame the app finds the area that holds the walker's eye (the first holding it, else the last one
  held) and the renderer packs that area's lights first: its key light (its first light with `castsShadows`)
  shadowed, its other point and spot lights nearest the eye; then its neighbours' (areas whose boxes touch the eye's
  area's over a face: a room's doorway passage and its hall) that may be seen through the doorway they share (within
  10 m plus their range of it), by importance (intensity over the squared path from the eye through the doorway,
  less for a doorway beside or behind the eye), never the key light and never shadowed, in a share of the 16 that
  does not depend on the view: 6, or all the slots the area's own lights leave (I6 step 1, ADR-0014 §4 as amended).
  The choice has hysteresis (a light chosen stays half a second at least, and while no other is clearly nearer or more
  important), and each light fades in and out over 0.3 s (at once at the first frame, after a teleport and under
  `reduceMotion`), the key light's shadow too; the neighbours' lights (and a room's own) are clipped to the doorway in
  the shaders, so no light shines through the wall beside it. A point or spot light whose whole range lies behind the
  eye is left out (it lights nothing in view), with a 1 m margin. The other lights are left out, so light never leaks
  through a wall without a doorway (`rv/scene/scene_areas.hpp`). An area may carry its own `"environment"` (`exposure`,
  `ambientSky`, `ambientGround`, `ambientIntensity`, `fillColor`, `fillIntensity`, and since I6 step 3 `bloomStrength`
  (a room whose theme pack blooms otherwise, the arcade's), each optional, else the scene's):
  the views ease to the eye's area's over 0.5 s when it changes areas, at once under `reduceMotion` and at the first
  frame; `fillColor` and `fillIntensity` (also keys of the scene's `"environment"`, 0 by default) add an even fill to
  the hemispheric ambient's diffuse part. The developer overlay shows and edits the eye's area's environment. The log
  names every area's key light at load, or why it casts no shadow (`Scene '<name>', area '<id>': key light ...`), and
  each area the eye enters; the run report's `sceneAreas.lights` lists the lights selected (the eye's area's own, then
  the others), `sceneAreas.packed` the lights block as packed (each light's `name`, `weight` and whether a portal
  `clipped` it), `sceneAreas.last` the light of the last view (`exposure`, `ambientIntensity`, `fillIntensity` as
  eased, how many `lights` are packed and `fading`, the sum of their `weight`s) and `sceneAreas.samples` the same at
  the first view and at every frame while the environment eases or a light fades (at most 2,000); `sceneAreas.keyLight`
  and the developer overlay's Lights panel name the key light of the area that holds the eye. A scene without areas
  packs all its lights in list order as before. A
  floor's scene (`retroverse-cli worldplan show --floor <id> --scene <file>`, `docs/reference/cli.md`) has an area per
  room (its interior and its doorway's passage) and one for the hall (`hall:<floor>`, with the lighting profile's hall
  environment: I6 step 1); the hall's lights are the profile's `hall` role, with a low fill spot before each doorway
  aimed into its room (`hall:<floor>/doorway-fill:<room id>`); its camera stands in front of the elevator facing east,
  and the rooms' doorways are open onto the hall (their stops left out), so the walker can walk from the elevator into
  any room. Since I6 step 2 the hall is dressed (`docs/i6/step-2.md`): a panelled wainscot under dark warm plaster,
  beams, a portal at each doorway with the room's sign over it and on a blade sign standing out from the wall, sconces
  (lit fixtures washing their wall, no lights of their own) and prints in turn, benches with a plant beside them that
  the walker walks around (they collide), a runner down the middle.
- `--model <file.gltf|.glb>` (not with `--scene`): model viewer, the model at the origin with a three-point light rig
  and a camera framing its bounds from the front right and above, on a dark mid-grey background (about sRGB 61, so
  dark and light models both stand out), without bloom or vignette; a file that cannot be loaded is shown as the
  magenta placeholder (logged).
  `--debug-draw` draws debug lines over any scene or model: object bounds (amber), socket axes (X red, Y green, Z
  blue; the parts hidden by surfaces dimmed, so a socket under a surface still shows its origin) and a world grid on
  y = 0 (1 m cells in scenes; for a model, cells of the power of ten that makes it span 2 to 20 of them: 10 cm for a
  0.5 m model, 1 mm for a 1 cm part).
- Frame statistics (draw calls, batches, instances, triangles, culled items, shader and material switches, the draw
  calls of the shadow map and of the depth pre-pass, GPU times from timer queries, `GPU shadow ... ms, depth ... ms,
  SSAO ... ms, scene ... ms, post ... ms`, frame times) are logged once per second in developer builds and as `Frame
  stats of the run` at the end of every frame-limited run; measure frame times with `--no-vsync` in Release (at
  1920x1080 on the reference GPU the test room's frame takes about 2.9 ms: shadow map 0.03 ms, depth pre-pass 0.04 ms,
  ambient occlusion 0.4 ms, scene 0.4 ms, post-processing with FXAA 0.45 ms: `--width 1920 --height 1080 --no-vsync
  --hidden --exit-after-frames --frames 600`).
- `--screenshot <file.png>` writes the last frame of a frame-limited run; a run that ends before that frame (Esc,
  window closed) exits 3 like a failed write. `--screenshot-size WxH` (default 1920x1080, independent of the
  window) also applies to F12.
- `--self-check <name>` (frame-limited runs only) checks the rendering after initialization with the programs the
  views use, else the run exits 3. `srgb`: an unlit grey quad of the radiance the tonemap shows as linear 0.5, drawn
  into an HDR target and taken through the post-processing into an sRGB target, must read back as 188 +- 2, without
  and then with bloom, dithering and anti-aliasing. `bloom`: known patterns through the bloom chain: a lit square just
  below the highlight threshold must not bloom, an emitter must keep its energy (+-5%), a single very bright pixel must
  glow alike (+-20%) whatever its value and place. `fxaa`: a hard edge slanted 12 degrees between black and white,
  post-processed without dithering, without and then with anti-aliasing: at least half the rows it crosses must get
  intermediate values, every pixel of a flat 3x3 neighborhood must stay exactly as it was. Use `retroverse.exe
  --self-check srgb --smoke --hidden --data-dir build/rvdata` (and `--self-check bloom`, `--self-check fxaa`).
- Scenes are lit (PBR and unlit emissive materials, scene lights, hemispheric ambient) in linear HDR (`GL_RGBA16F`) and
  post-processed once per view: bloom of the light surfaces emit (screens, lamp shades, neon) and of the highlights
  above a soft threshold (lit paper, wood and walls never bloom), vignette, exposure + ACES, dithering, then FXAA (the
  overlay's Rendering panel turns it off for comparisons). The look is tuned in the scene files (`environment` exposure,
  ambient and `clearColor`, which is background radiance post-processed like the surfaces; `bloomStrength`, the share of
  that light moved into the glow, 0.06, `bloomRadius` 0.7 and `vignette` 0.05 by default; `lights`, at most 16
  point/spot plus one directional), material presets in `assets/materials/base/materials.json`, shaders in
  `assets/shaders/{common,pbr,unlit,depth,ssao,post}`.
- Surfaces are procedural (`rv/assets/surface_textures.hpp`), generated when first used and kept for the run (the
  texture cache, logged per surface): a world run makes every one at its start, before its first room (about 0.9 s in
  Release since I6 step 3b, the five packs' rugs 0.5 s of it), so no room's commit or ride waits for one; a scene
  (`--scene`, `--model`) makes those its materials name. All of them take 85 MiB of GPU memory (123 MiB until
  I6 step 5's review stored each metallic-roughness map as its roughness alone, R8; `gpuMemory`'s
  `proceduralSurfaces`), held for the whole run: `oak_planks`, `walnut`, `varnished_wood`,
  `oak`, `painted_plaster`, `ornate_rug` (and since I6 step 3b the packs' rugs `ornate_rug_blue`, `ornate_rug_sage`,
  `ornate_rug_green`, `geometric_rug`, `runner_rug`), `brushed_metal`, `aged_plastic`, the halls' `wallwash` and the
  shelves' `shelfglow`, each with base color, metallic-roughness and (except the rugs and the pictures) normal maps.
  Materials name a map as `"procedural:<surface>/<map>"` in a texture slot, scene files as `{"procedural": "<surface>/<map>"}` in
  `textures`; they repeat, at the scale each surface's description gives (the furniture prefabs and the room shell map
  their faces in meters along the grain). Never add downloaded textures: new looks are new generators or parameters. A
  screen material with a `crt` block (`MAT_Screen_CRT`: line count, scanline and mask strength, curvature, which also
  rounds the picture's corners, vignette, glass reflection, 0.25: a faint sheen that must never wash out the picture)
  shows its picture as a CRT tube; the scanlines and the mask fade out below 4 pixels per period, so at 1920x1080 they
  appear only within about 1 m of the TV.
- Shadows and ambient occlusion: the first light with `"castsShadows": true` is the key light. A spot key light gets a
  perspective 2048x2048 shadow map over twice its cone, a directional one an orthographic map fitted to the shadow
  casters (objects with `castsShadows`, default true; blended ones cast nothing) and snapped to whole texels; point
  lights cast no shadow, nor does a directional light while no object casts (the log and the Lights panel say so: make
  key lights spots). The lit shader filters the map with a 3x3 tent of hardware 2x2 PCF fetches; slope-scaled depth
  bias and a normal offset of 1.5 texels keep surfaces from shadowing themselves (no acne) with shadows attached to
  their casters; `"shadowStrength"` (0..1, default 1) on the light sets how much of its light its shadows take away.
  Thin geometry on walls (posters, signs) gets `"castsShadows": false` in scene files: its shadows would be narrower
  than the filter and break into dashes. Every view first draws a depth pre-pass, from which screen-space ambient
  occlusion (`environment.ssao`: `{"enabled": true, "radius": 0.4, "intensity": 1.0}` by default; half-resolution
  estimate, depth-aware blur; occluders farther than the radius in depth count for nothing, so no halos) darkens the
  ambient light only, never a light's (a subtle effect in rooms lit mostly by lights). The model viewer has neither.
  Shadows are world-locked (the maps depend on the light, not on the camera): walking never makes them shimmer.
- The collection's rendering (I3 steps 0c, 6, 8; the room's cartridges, and `rv_collection_probe`'s test items): every
  instance carries an 80-byte record (`scene::InstanceData`: the matrix, then the
  front and spine layers, flags and pick id at attribute location 8). Covers and spines live in texture arrays per
  class (fronts 256 x 188, spines 40 x 256, BC1 sRGB; the log's extensions line names `EXT_texture_compression_s3tc` and
  `EXT_texture_sRGB`), layer 0 the pending picture, uploads within 1 ms a frame (`layerUploads`);
  `--no-texture-compression`, or either extension missing, keeps them in RGBA8 sRGB at half the resolution, and the
  log names the choice (`Collection texture arrays: ...`). Static instance groups upload their records once and cull
  clusters per view (`collectionPlanning`). An instance flagged highlighted gets the hover outline after the
  post-processing (a 2 px ring at 1080p in #F4EFE6, scaled with the output's height, its own GPU timer); one flagged
  hidden is not drawn.
- The hero CRT (I3 step 11, `docs/i3/step-11.md`; the scene's first `crt` prefab with a picture on its screen, the test
  room's or, on a floor, the CRT of the room that holds the eye since I4 step 6b: bound once the eye has stayed in a
  room for 300 ms, the last room's kept in a hall, rebound without joining the previous clip's decoder on the render
  loop, the previous screen's colour bars back): one 480-row decoder, never two decoder threads (since I5 step 2 the
  scheduler's slots, below). It plays the selected
  game's clip
  (the quick catalog's selection when the game stands in the eye's room, or the room's item focused or inspected,
  after a 300 ms dwell; before any, the most recently played placed game with a video, else the first featured one;
  a room's CRT plays its placed games only), else its screenshot, title screen or cover
  (fitted to 640 x 480 on a worker), else its colour bars (since I5 step 1 its title card, below); it pauses out of
  view, stops (joined within 1 s) before a launch and reopens at its position after the return. Its games come from
  the ES-DE catalog (the room's items, since
  I3 step 10c) or from `--mock-catalog`'s `"media"` (`video`, `screenshot`, `titlescreen`, `cover`; absolute or
  relative to the file) and `"lastPlayed"`. `--video-freeze-pts <seconds>` (frame-limited
  runs, 0 to 86400) opens every clip paused at that time and holds the frame: two runs give the same screenshot byte for
  byte. NVIDIA's message 131154 (every video upload) is dropped like the informational ids.
  Since I5 step 1 (ADR-0016 §3, §4, §8; `docs/i5/step-1.md`) the start choice plays for `previewDuration` (its clip's
  end when shorter), then the room's playlist (`app::Playlist`: `shuffle` by default, `sequential`, `favorites`,
  `recent`, `explicit`; its games those with a playable clip, a clip that fails skipped for the run, the order seeded
  by the room's id, `hero`, the mode and the cycle); a selection interrupts it and is held while focused, inspected or
  selected in the quick catalog, then its clip plays to its end (at most `previewDuration`) and the game it
  interrupted plays again, the playlist going on after it; the items' clock stops out of view and during a game, and
  nothing advances under `--video-freeze-pts`. The chain's end is the game's generated title card (its title, year and
  system on its room's label palette in its label's colour, 640 x 480, drawn on the worker; a placed game without any
  media has its card only; drawn only once a clip cannot play), not the colour bars; a room without any clip shows its
  first game's stills and card; between two games the previous picture stays up (the last frame of its clip, or its
  still) until the next one's still or first frame is ready. A screen with no game shows its own
  pattern: the test room's colour bars, the building's `standby` (a dim cold raster with a faint trace; the arcade's hero
  cabinet too). `<user data>/config/video.json` (read, never written; the shipped defaults `assets/config/video.json`,
  merged key by key; a file that does not load is left out whole, said in the log) sets `enabled` (clips at all),
  `maxDecoders`, `maxRows`, `viewerMaxRows`, `proximityMeters`, `crtEffect`, `audio` (I5 step 5, below), and
  per screen kind (`hero`, `secondary`, `cabinet`) `enabled` (autoplay), `playlist`, `previewDuration` (1 to 3600 s),
  `showOnFocus`, `volume`, `crtPreset`, and per room `rooms.<room id>.<kind>` the same keys and `games` (the explicit
  mode's GameIds; ids the room lacks, or without a clip, skipped and logged once). The test room's room id is
  `m0_testroom`. The report's `video` section adds `playlistMode`, `playlistRoom`, `previewSeconds`, `playlistOrder`
  (the current cycle), `origin` (`start`, `playlist`, `selection`), `played` and `shown` (each game played and each
  picture shown, with the run's simulated seconds, the first 512), `heldFrames` and `idleFramesWithGame`.
- The decoder scheduler (I5 step 2, ADR-0016 §2, `docs/i5/step-2.md`): the room's screens, the hero first (by its role
  since I5 step 3: the hero corner's CRT `...fx:hero.crt`, or the arcade's hero cabinet; a hand-made scene's first
  `crt`), then the secondaries: the room's secondary TV (I5 step 3, `docs/i5/step-3.md`: a pack's `secondary_tv`, a
  small CRT on a sideboard standing in one of the room's sideboard sockets away from the doorway, at most two in a room
  deeper than 12 m, none in the arcade or the hub) and a hand-made scene's further `crt`s (in a scene with areas, those
  in the hero's area); a secondary plays its own playlist (`secondary`'s keys, seeded by its object id, no start choice)
  in the `ambient` class, opening a slot only within `proximityMeters` of the eye (to its object's bounds' middle) and
  in view, else its still; it plays nothing while the room editor runs. `app::ScreenScheduler` gives the clips the
  budget's decoder slots: `--max-decoders 1|2`, else `video.json`'s `maxDecoders` (2). Screens asking for one clip share
  its slot and texture; clips rank by being in view, then their class (`direct`, `focus`: the selected game's screen,
  `room`: the hero, `ambient`: secondaries within `proximityMeters`, 0.5 m hysteresis), their nearest screen and its id;
  a screen without a slot shows its still (its title card last). A slot whose screens all leave the view pauses on that
  frame and stops after 10 s (its clip reopens where it stopped), or sooner when a request in view needs it (out of
  view, a clip takes only a free slot, opening paused, and an `ambient` one none); a minimized window counts as every
  screen out of view (reason `minimized`). A slot changes hands at most once a second unless `focus` or `direct` asks; a
  clip no screen wants stops at once, and a screen switching clips waits for its own slot's thread (one thread per
  screen, as in I5 step 1; the next room's hero waits for the last hero's slot). Late plus dropped frames over 5% for 4
  s drop the budget to 1 for 30 s (open plus reserved slots never above it); `--fixed-dt` runs do not watch late frames
  unless `--watch-late-frames` (tests). World screens decode at `video.json`'s `maxRows` (480); one render frame uploads
  at most 4 MiB of video, the longest pending frame first. `rooms.<room id>.screens.<screen's object id>` takes a kind's
  keys for one screen, for tests and hand-made scenes (a generated room's object ids are the generator's; the smoke
  scene `tests/smoke/scenes/screens_four.json`, room `screens_four`, gives each of its four CRTs one game). The report's
  `video` adds `maxDecoders` (the budget), `maxOpenDecoders`, `handOvers`, `maxUploadBytes` (the most in one frame),
  `deferredUploads`, `oversizeUploads` (a single frame over 4 MiB), `screens` (each screen at the end: `screen`, `kind`,
  `screenClass`, `gameId`, `picture`, `slot`, `playing`, `textureId`, the GL texture its material shows, equal on
  screens sharing a slot, `visible`), `slotEvents` (`frame`, `atSeconds`, `slot`, `action` open, stop, pause or resume,
  `gameId`, `reason`: wanted, unwanted, needed, out-of-view, in-view, budget, minimized, launch, unbind, shutdown,
  `screens`; the first 512), `budgetDrops` and `slotEventsDropped` (the events after the first 512); `maxDecoderThreads`
  and `decodersStarted` count the whole service. The process runs at `ABOVE_NORMAL_PRIORITY_CLASS` while it renders and
  is not minimized, `NORMAL_PRIORITY_CLASS` while minimized, from just before an emulator starts until the first frame
  after the return, and at the exit: the report's `processPriority.changes` (`frame`, `priority` read back:
  `above-normal`, `normal`, `other`; `reason`: rendering, minimized, launch, exit; `applied`). An interactive run
  renders nothing while minimized, but after each wait (0.1 s) it sets the priority and ticks the screens with the time
  waited; `--simulate-minimized FRAME:SECONDS` (tests, frame-limited runs) takes that path after FRAME frames for
  SECONDS, the window untouched, each wait counting 0.1 s without waiting under `--fixed-dt`.
- The arcade cabinets playing their clips and the picture's aspect (I5 step 4, ADR-0016 §6 and §7,
  `docs/i5/step-4.md`): in the arcade room the focused cabinet (`focus`) or the inspected one (`direct`, its clip in the
  hand) plays its own game's clip, looping, and in free roam attract plays the nearest cabinet in view within 3 m whose
  screen faces the eye (within 60 degrees of its normal), after a 1 s dwell; it keeps it to 3.5 m and 70 degrees and
  stops once it leaves the view. `video.json`'s `cabinet.enabled` is attract, `cabinet.showOnFocus` the focused
  cabinet. A cabinet's screen is drawn with its slot's pooled video material on the dynamic path while its clip shows
  (its static front hidden), its cover otherwise; no cabinet plays while the room editor runs. The hero cabinet keeps
  the room's playlist and never follows the world's selection (the focused cabinet plays itself); it follows the quick
  catalog's, as every hero screen does. With one decoder the inspected cabinet takes the slot, and a screen whose slot
  is taken from a clip it still wants keeps that clip's frame until it plays again. A clip of another aspect than its
  screen's is letterboxed or pillarboxed in the lit tube's off-black (within 2% it fills the screen; a console's frame
  of at most 240 rows within 15% of 4:3, such as 256 x 224, is shown at 4:3). The report's `video.screens` lists the
  cabinets asking for their clips at the end too (`kind` `cabinet`), each screen's `pictureRect` (x, y, width, height;
  empty for the whole screen), `heldFrame` and (I5 step 3's review) `bounds` (the box its distance and view test
  read: min x, y, z, max x, y, z); `video.cabinetEvents` (`frame`, `atSeconds`, `screen`
  "<room>/cabinet:<pick id>", `gameId`, `screenClass`, `picture` `video` or `cover`; the first 512, the rest counted in
  `cabinetEventsDropped`) and `heldFrameShows` (frames a kept frame showed); `world.cabinetClipFrames` and
  `world.cabinetCoverBeneathFrames` (frames a cabinet's clip drew, and those with its cover drawn beneath it: 0).
  Since the step's review a lockstep run waits, at most 2 s (NFR-06), for the first frame of a clip a slot opened for a
  screen in view, so that it shows on the frame the slot opened (`video.firstFrameWaits`, `firstFrameWaitMaxMs`,
  `firstFrameWaitTimeouts`); a slot's `open` event gives the position it opened at (`fromSeconds`; absent: the
  clip's start).
- The CRT presets (I5 step 6, ADR-0016 §7, `docs/i5/step-6.md`): every screen's tube (scanlines, phosphor mask,
  curvature, vignette, the glass's sheen) is one of `assets/materials/crt_presets.json`'s presets, `neutral` (the clean
  video), `consumer` (the look every CRT had before), `arcade` (deeper scanlines, a stronger and coarser mask, more
  curvature) and `monitor` (a fine mask, little curvature), chosen when the scene's screen materials are registered:
  `video.json`'s `crtEffect` `off` makes every screen `neutral` and `vintage` steps each one stronger (`monitor` to
  `consumer`, `consumer` to `arcade`); else `video.json`'s `crtPreset` for the screen (`rooms.<room>.screens.<object
  id>`, then `rooms.<room>.<kind>`, then the kind's own key; `""` is none); else the room's theme pack's
  `screenPreset` (the optical-era lounge's `monitor`, the arcade's `arcade`); else the kind's (`consumer`, the
  cabinets `arcade`). The arcade's cabinets take one look per room. `<user data>/config/crt_presets.json` (read, never
  written) overrides the presets' values key by key (`{"schemaVersion": 1, "presets": {"arcade": {"scanlineStrength":
  0.4}}}`); a user file that does not load is left out whole, a shipped one that does not load leaves the built-in
  copy (both said in the log). The report's `crtLooks` gives `crtEffect`, `presetsSource` (`shipped`, `user`,
  `built-in`) and `screens` (each screen of the scene shown at the end: `screen`, its object's id or a room's cabinets'
  `collection/<room>/<family>`; `room`, `kind`, `preset`, `chosen` before `crtEffect`, `rule` `video.json`, `pack` or
  `kind`). The smoke scene `tests/smoke/scenes/crt_presets.json` (room `crt_presets`) holds four CRTs at eye height
  showing the colour bars, one per preset with its case's `video.json`.
- The generated building (I3's room, `docs/i3/step-10b.md` to `step-17b.md`; ADR-0010 to ADR-0012; the building since
  I4, ADR-0013, `docs/i4/step-1.md` to `step-5a.md`): without `--scene`, `--model` or `--mock-catalog`, `retroverse.exe`
  plans the building of its ES-DE catalog and shows one floor of it. The building job (on the worker pool) reads every
  system's snapshot within one read transaction on a read-only connection of its own (the catalog worker keeps
  answering the quick catalog; a refresh running is waited for first), plans the building as `retroverse-cli worldplan
  generate` does (every system with a room and a placeable title; excluded systems out; a system under
  `sharedRoomMinTitles` a section of its floor's `area:shared:<floor>`, every arcade system a section of `area:arcade`)
  or updates its rooms as `worldplan update` does (each system's selection to the room that holds its section; a shared
  or arcade room lacking a section's selection held as it is, logged; a new system with a placeable title enters, its
  own room or a section, logged, since I4 step 6b), saves it
  under `<user data>/worlds/<library id>/` (generator 1.5, schema 2: since I4b step 7 a room past its template's
  capacity is a chain of a head and expansion rooms, `room:<system>:<n>`, which the app shows as rooms of their floor
  until step 8 signs them; a stored schema-1 plan of I3 is read lifted and
  written in schema 2 by its first update, the schema-1 file kept in `backups/`; a room the update keeps as it is shows
  with the regeneration's notice and its new games unplaced as `room-kept`; since I6 step 4 a room of cases or boxes
  planned by generator 1.3 is migrated once to its family's successor, standing on its side, logged, in the run
  report's `world.migratedRooms` {`room`, `from`, `to`, `unplacedForCapacity`} and with the notice `rooms-migrated`;
  since I6 step 5 a ground floor room standing at I4's lobby is re-placed once onto the hub's wall, logged, in
  `world.replacedRooms` {`room`, `fromXMm`, `fromZMm`, `toXMm`, `toZMm`} and with the same notice, and a plan kept or
  previewed as it is draws it there already;
  what the update left undone for a pin, a pinned room not migrated among it, is logged and in
  `world.needsConfirmation`) and writes, only when they changed, each
  system's room items (`<user data>/cache/world/<library id>/room-<system>.json`: titles, cover paths, years, the
  system's full name and platform tags, and since I4 step 6b each placed game's CRT data: its clip, screenshot and title
  screen, its last play, its title's priority; a system of a shared room has its own, its section's games with their
  text; schema 2, schema 1 still read without the CRTs' data), and its building stamp (`building-stamp.json` beside
  them: the digests of the plan, of the catalog's stamp and of the world's inputs, the program's executable among them,
  and the plan's soft findings; at the next start an unchanged catalog, data, configuration, program and media root
  plan nothing, the floor built from the plan and the items files; a job that leaves a room kept until a regeneration,
  a room held, a system waiting to enter one or a notice writes none and removes the one there; `docs/i4/step-6b.md`),
  from which the floor-build job makes the floor
  without the catalog: its static part (`worldplan show --floor`'s
  scene: its rooms and its derived hall), each collection room's collection on a static group of its own
  (every `collection/<room id>/...` registry key carrying the room id, so that two rooms of one family sample their own
  arrays) and their covers and spines through the derived-texture cache `<user data>/cache/textures/<library id>` (one
  cover loader for the floor: the view is the room that holds the eye and the rooms whose doorway the frustum meets).
  The run starts in the ground floor's hub (I6 step 5, below), at the elevator's arrival point facing east (no hero CRT
  bound until the eye stays in a room); `--start-room <room id>` (`room:snes`, `area:arcade`, ...) starts at that
  room's doorway camera on its floor, its CRT the hero CRT from the start (a shared or arcade room's playing all its
  sections' games; since I6 step 6 the arcade room's hero cabinet, which stands for its CRT; a room the plan lacks once it exists ends the run: exit 2). The scene's frame is the start
  room's when one is given, else the floor's (building.hpp: x east from the elevator, the hall's centre line on z 0):
  `--spawn`, the input scripts' poses and the run report's walker poses are in that frame, and the start room's own
  objects keep the very coordinates of its standalone scene. Hover, pull-out, Focus, Inspect, the card and launches
  work in the room that holds the eye (a pick index per room; none in a hall). The empty shell while the building is
  made (the lobby, or the start room's empty room); the test room with a notice when there is no catalog (no
  library.json or database) or no system that gives a room (and no plan stored: the notice names the start room's
  system only when `--start-room` gives one); the empty shell with a notice when the media root is out
  of reach before a first plan or the plan cannot be used. `--scene m0_testroom` shows the test room as before (G01's
  command has it). A system systems.json does not name is classified by the catalog's platform tags of it, as
  `worldplan` does. The systems left out of the building for want of a room (excluded) are named in one log line per
  building job. With a plan stored, its start floor shows at the first frame (covers and spines from the cache by
  the rooms' items, its CRTs' games from them under the media root when it can be read, each game's last play the
  catalog's play sessions now, so that a CRT starts on the game the building's result chooses) before the building job
  runs, then is reconciled with its result room by room (I4 step 6b: each room by its content digest; at the start
  every room's covers are served again from the cache to check their sources, the media root known now, nothing
  uploaded over a picture that showed; a live update leaves an unchanged room's collection and pictures as they are,
  its items taken over as they show; a room whose items changed has them applied in place; a change of the floor's
  static part commits the floor again with every room's pictures kept and the walker kept in the frame of the area that
  holds it, moved with it; either ends a Focus or Inspect, the walker keeping the pose it had; the items compared in
  their rooms' own frames, so a room that grew moves none). A
  manifest missing from the asset root falls back to its family's prefab, warned once per run. Covers come nearest
  first (the hovered item, the view by projected size, the rest by distance); the hovered item's front shows its hero
  texture (its cover at its label's shape, up to 1,024 px on the long side and 1,024 x 752 pixels, 8 kept); nothing is
  loaded while a game runs; after "Aggiorna libreria" the covers the media root could not give are tried again.
  Lockstep runs (`--fixed-dt`) wait before the first frame and between frames for the floor, the layers of the frame's
  view and the hovered item's hero texture, and for the whole floor before their last frame (10 s at a time): a stored
  floor at frame 0, the reconciled one from frame 1.
  The item the reticle rests on within 2.5 m is outlined and labelled with its title; after 150 ms it leaves its slot (a
  face-out cartridge tips forward, a spine-out one slides out and turns its cover to the viewer; a case or box standing
  on its side, I6 step 4, slides out like a book and turns its cover to the viewer, upright). E, a captured left
  click or the pad's south button focuses a spine-out cartridge (the walker eased to 0.9 m in front of it) or inspects a
  face-out one; in Focus the arrows and the D-pad step between cartridges (repeating while held). In Inspect the item is
  held upright 0.45 m in front of the eye, left of and below the view's centre (beside the card), the scene dimmed by
  one stop; the right stick or the captured mouse turns it, the triggers, Page Up / Down and the wheel zoom it (0.3 to
  1.0 m); a card beside it shows its game (cover, at its family's label shape within the 256 x 188 dp frame, title,
  platform, year, readiness and first blocker, developer, genre, players, other versions): the arrows and the D-pad move
  between Avvia, Vedi media, Informazioni and Indietro, Enter or A presses, a click (mouse free) the one clicked; Y, X
  and Start are Vedi media, Informazioni and Avvia; Esc / B leaves
  one level (Inspect to Focus to free roam). The hero CRT plays the clip of the item focused or inspected for 300 ms.
  Vedi media (the card's button, Y; the quick catalog's too) opens the media viewer: the game's images one page per kind
  (cover, back cover, physical media, 3D box, screenshot, title screen, marquee, fan art; the missing kinds skipped),
  left and right page, Home and End jump, Back closes; a file that cannot be read shows its fallback. Informazioni (X)
  shows the description, the metadata and the other versions the plan knows with their regions; Up and Down scroll; X
  and Y switch between the two. Avvia launches as the quick catalog's does; after the return the walker stands where it
  stood and the item is in the hand again; the hero CRT's decoder stops before the launch and reopens at the clip's
  position after it. The quick catalog's card offers "Mostra nella stanza" for a game the building places (the walker
  eased to its item's Focus pose; since I4 step 7b in any room: teleported to the room's doorway first on the same
  floor, a ride first to another floor, below) and says why for one it does not: one of a system without a room is not
  on display (`not-in-room`), an unplaced one gives its reason. After "Aggiorna
  libreria" or `--refresh-library` the floor follows the refreshed catalog live (the building job again, the plan
  saved, the other floors only saved; room by room as at the start: new titles placed into their slots with their
  covers, nothing else moved; a dressed level, module, growth or a room entering commits the floor again with the
  pictures kept and the walker kept in its area's frame; never during a game: a result arriving meanwhile applies
  after it). `<user data>/config/world.json` takes `"reduceMotion": true` (every transition
  instant) and `"lightingProfile": "evening" | "afternoon"` (`assets/world/lighting`, applied when the room is drawn,
  never stored in the plan; the profiles' `"on": "display"` accents light the display cases, `case_light`, and the
  featured shelf, `shelf_light`, while the 16 lights leave room); the room's look comes from its theme pack
  (`assets/world/themes`, I6 step 3, `docs/i6/step-3.md`): its floor's (`building.json`'s `themePack`: the 16-bit
  floors `classic_16bit_warm`, gen5 `console_90s_collector`, gen6 and gen7 `optical_era_lounge`, the handhelds
  `handheld_archive`, the computers `computer_studio`, the ground floor's hall `retro_library_hub`), the arcade room's
  `arcade_after_dark` wherever it stands, a platform manifest's only when it names one (`snes.json` names none);
  resolved when the room is drawn (materials, the neon sign and the light on it, the lit shelves, the generated
  spines' and labels' palette, light tints, the bloom; since I6 step 6's review its own accents, the arcade's pairs lit
  cyan and red in turn, and its `roomDressing`: the arcade's neon coves, backlit flyers, spot cans and runner, the
  handhelds' warm cove, posters and pictogram beside the sign), never a reason to move anything. With the mouse free
  (the developer overlay open) the cursor picks.
- The elevator and the directory (I4 step 7b, `docs/i4/step-7b.md`; ADR-0013 §10): in front of a floor's elevator (its
  proximity zone, 1.5 m deep and 2 m wide; the arrival point is in it), facing its doors (their middle less than 80
  degrees off the view: the arrival point faces the hall, so the walker turns to them first), a prompt names the
  interact key ("Ascensore", E or the pad's A: "Scegli il piano"), which opens the directory on its floors; M (the pad's
  Back/Select) opens it anywhere in free roam (not in Focus, Inspect, a launch or a ride, nor in the frame an interact
  takes an item) and closes it again. It has the UI's input (the world stands still) and Esc / B leaves one level (the
  card to the games, a floor's rooms to the floors, then it closes). "Piani" lists the plan's floors in the elevator's order (the walker's marked), a floor its hall
  ("Corridoio e ascensore") and its rooms along the hall with their games; "Preferiti e recenti" (the shoulders, or Left
  and Right on the tabs) the catalog's favourites, then its most recently played games (Retroverse's sessions and
  ES-DE's `<lastplayed>`, at most 30), a game its card with Avvia (the quick catalog's launch, its outcome on the card)
  and "Mostra nella stanza". Choosing another floor's hall rides there, the walker's own floor's hall puts the walker
  back at its elevator; a room of the walker's floor teleports to its doorway, one of another floor rides there first.
  A ride: the view fades out in 250 ms (instant with `reduceMotion`; the post pass dims the tonemapped view's sRGB
  values alike, its lights too, the UI above it), the destination floor is built on the pool from the plan, the rooms'
  items files and the catalog's play sessions (a game played since the items were written starts its room's CRT), the
  resident floor is dropped only once it is built (a failure leaves the walker where it was, the directory saying it
  cannot change floors now) and swapped behind the fade in the frame after it ended (the hero CRT unbound, bound again
  by the room the eye stays in), the walker at its elevator facing the hall or at the room's doorway, and the view
  fades back in once the covers in view of the room that holds the eye are resident (I4 step 8; a hall at once), or so
  that it is back 3 s after the request (the wait ends at 2.25 s, a fade-in begun late by a long frame shortened to end
  by 3 s: `timed-out`); the rest of the room's covers stream in afterwards (the ride's record says when the room was
  whole, `roomResidentMs`, and how many covers were in view, `viewItems`, `viewPending`); the fades follow the real
  clock; under lockstep the frames' times and the frames wait instead (15 frames out, the swap in the next, the eye's
  area in the one after, 15 frames in at `--fixed-dt 0.016667`, on every run). The input is
  suspended meanwhile; a building's result or a live update that ends during a ride applies after it (and never while a
  launched game runs). A ride or teleport is refused while a floor applies (the persisted start's reconciliation, a
  result built again for the floor shown), in Focus or Inspect, or while a launch is active: the directory comes back
  where it was with its notice (`ride-unavailable`; `building-pending` before the building is planned); the quick
  catalog's "Mostra nella stanza" refused or failed so says the world's notice (`room-unreachable`, 12 s), and in the
  room the walker is in it is taken while a floor applies, as before I4. After a ride the scene's frame is the
  destination floor's (the start room's on its own floor): `--spawn` applies to the start only.
- The editor (I4b step 3, `docs/i4b/step-3.md`; ADR-0015 §4): while the walker is in a room with games (no floor on
  its way, no ride, no launch), the directory shows "Modifica stanza" above its floors (Up from the first floor, then
  Enter or A), and the card of a game of that room in Inspect shows "Sposta" below its buttons (the game then in the
  hand): the world's state `Edit` (the bar at the bottom names the room, the game or empty slot the reticle rests
  on, the pending changes and the keys of the device used last). The walker walks and looks; the arrows and the D-pad
  step over every slot, empty ones included, the walker eased in front of it (the arrow keys step instead of walking).
  E (A) picks up the game targeted, held smaller in the hand, its slot empty; on an empty slot it puts it there (where
  it would stand outlined in warm white; a slot's decoration gives way, the room's static part rebuilt alone), on
  another game it swaps the two (the room's collection laid out again in place), on its own slot it puts it back; Esc
  (B) puts it back; P (the pad's X) pins or unpins the game targeted in place. Every move and swap pins its games
  (`overrides.json`, origin `editor`). Each edit is applied on the pool to a draft of the plan and validated; a refusal
  ("Quello spazio è già occupato", ...) leaves the game in the hand. The reticle and the D-pad target only while the eye
  is in the edited room. Enter or Start opens the menu (Esc with pending changes too; Start again closes it, the walker
  stands still while it is open): Salva (the session's operations saved under the world lock behind a snapshot, action `edit`, which
  `retroverse-cli worldplan undo` undoes; replayed onto the stored plan if it changed meanwhile, refused with nothing
  written when one no longer applies), Annulla ultima azione (the draft's last), Esci senza salvare (the room as stored
  again), Continua. Esc with nothing pending leaves the editor. The quick catalog, the directory and rides wait, and a
  refreshed catalog's live update waits for the session's end. `overrides.json` that cannot be read (written by a newer
  Retroverse) edits nothing: the world's notice `edit-unavailable`. Lockstep runs wait between frames for each edit.
  Furniture (I4b step 4, `docs/i4b/step-4.md`): where the reticle rests on no game or slot, it targets a side-wall
  module, a display fixture or a decoration (within 4 m, not through the hero corner or a hero-wall module, which are
  fixed; the bar names it by what it is drawn as: "Scaffale", "Cabinato", "Lampada da tavolo", ...; a kept room's
  furniture names its refusal at once); E (A) picks it up ("Stai spostando: ..."), held where it was taken: it follows
  the floor point under the reticle (a poster the wall point, along the walls only) on the room's 100 mm grid, drawn
  there with its games and dressing, a lamp with its light, outlined 3.5 px wide over a view dimmed a little, warm white
  where it may drop, a lighter red where a rule refuses it (the bar names the rule: another fixture, the doorway, a
  door-reserved wall, a module or the TV no longer reached through 1.2 m, the walker standing there, ...); each new
  place is judged on the pool. Q and R (the pad's shoulders) turn it a quarter turn (decoration 15 degrees; a poster
  turns with its wall), the arrows (the D-pad) nudge it by 10 cm along the room's axis nearest the view's (a poster
  along its wall) until the walker looks or walks; E (A) drops it (a refusal keeps it in the hand; over the walker it
  is refused at once), where it stood puts it back, as Esc (B) does; P (X) pins or unpins the fixture targeted. A drop
  rebuilds the room's static part alone in place (its objects, dressing, wall-washers, a lamp's light and colliders),
  then lays its collection out again; the fixture is pinned where it was put. Salva logs its parts ("Editor: saved in
  ... ms: read, result, commit, digest").
- Regeneration and undo (I4b step 6, `docs/i4b/step-6.md`; ADR-0015 §5, §6): the directory's third tab, "Modifiche"
  (the shoulders, or Right on the tab bar), lists "Modifica stanza" (while a room can be edited), "Rigenera stanza"
  (the room that holds the walker; in a hall the row says so and does nothing), "Rigenera piano" (the walker's floor),
  "Rigenera mondo" and "Annulla ultima modifica"; showing the tab reads the catalog's selections ahead on the pool.
  A row opens the preview (`regenerate_preview.rml`): "Calcolo di cosa cambierebbe..." while the dry run runs on the
  pool (nothing written, not even `world.lock`), then the rooms regenerated, added, removed and moved, the games that
  change place, those entering and leaving the display cases, the fixtures, the customizations kept and those that
  cannot be (each with why, kept in `overrides.json`), and the choices: Rigenera (only when something would change),
  Rigenera e scarta le personalizzazioni (only when the scope has some; a second confirmation names how many,
  "Indietro" focused above "Scarta e rigenera") and Annulla; Rigenera focused when it shows, else Annulla, and the
  hints show the pad's or the keys' glyphs. The commit runs under the world lock behind a snapshot (action
  `regenerate`; the drop is refused, nothing written, when the plan or the overrides changed since the preview); the
  rooms it changed are rebuilt alone in place when the floor's other rooms and its hall stand as they did (the run
  report's path `room`), else the floor follows the plan stored (room by room, `in-place`, else committed again with
  the walker kept, `floor`); a walker in a regenerated room stands at its doorway (at the elevator when its room is
  gone, or when it stood in the hall of a regenerated floor). "Annulla ultima modifica" names the action it undoes (a
  saved edit or a regeneration, its room, floor or the building, when) before "Annulla la modifica" ("Indietro"
  focused); there is no redo. A preview or a commit refused for now (a floor on its way, a ride, a launch) puts the
  directory back with `change-unavailable`; while one runs, rides, the editor and live updates wait, and lockstep runs
  wait between frames for its jobs. An update that left a room as it stands because it would have moved something
  pinned shows the notice `needs-confirmation` (12 s, after the update's own notice), naming the room: Enter (the pad's
  Start) in free roam opens the preview of that room's "Rigenera stanza".
- The hub (I6 step 5, `docs/i6/step-5.md`; ADR-0014 §3): the ground floor's hall, 18 x 14 x 5 m (building.json's
  `layout.hub`), its rooms on its long walls (the arcade room in the middle of the north wall), a balcony on columns
  along three sides (not walkable), the landmark in its middle (a glowing wireframe globe in a railed circular showcase
  of consoles; it collides as a 4.2 m square), lit bookcases, sofas, rugs, plants. Its directory totem stands west of the
  landmark facing the elevator: in its zone (1.5 m deep before its screen) facing it the prompt reads "Indice" (E or
  A: "Piani e stanze") and interact opens the directory at its floors (`via` "totem"; the frames it shows are
  `world.totemPromptFrames`). Its favourites' showcase against the south wall holds up to 12 runtime proxies of the
  catalog's favourite and recent games (the directory's list's first, as keep cases with their covers from the rooms'
  items files, a generated label otherwise; never in the plan), made on the pool when the hub shows, again when a
  launched game returns and after a refresh; the reticle on one within 2.5 m outlines it and labels its title, interact
  opens the directory at its card (Avvia, "Mostra nella stanza"; `via` "showcase"); lockstep runs wait for them like the
  floor's covers. The run report's `world.showcase` {`games`, `builds`, `cardsOpened`, `lastCard`, `hovered`,
  `frontsResident`}.
- The clips' sound (I5 step 5, ADR-0016 §9, `docs/i5/step-5.md`): `--audio device|null|off` picks the output: `device`
  (the default of interactive runs) is the default audio endpoint in shared mode (miniaudio, 48 kHz stereo float,
  converted to the endpoint's format), `null` (the default of frame-limited runs) mixes and throws the mix away without
  touching the system's audio, pulled by the main thread (dt x 48000 frames a frame under `--fixed-dt`), `off` builds no
  mixer and decodes no sound. The environment variable `RV_AUDIO=null` (or `off`, blanks trimmed, any case) forces that
  output over `--audio` (the test presets, `scripts/real-library.ps1` and the smoke tests' runs set it; `device` or
  another value is ignored with a warning); a device that does not open falls back to `null` (reason `device-failed`),
  and `video.json`'s `audio.mode` `off` means `off`. `--mute` (or `audio.muted`) starts muted; F8 (`toggle_mute`, no pad
  button) mutes and sounds again at once. One screen is heard: the visible one playing of the best class (`direct`,
  `focus`, `room`, `ambient`), then the nearest, within `proximityMeters` (the screen heard keeps it until one of its
  class is 0.5 m nearer), while the eye is in the room whose screens are bound (in a hall the last room's screens are
  silent); a change cross-fades in 300 ms, a pause fades out in 30 ms, and the other slots' decoders discard their audio
  packets. Its gain is the master times the video bus (`audio.master`, `audio.video`), the screen's `volume`, and 1
  within 1.5 m, then 1.5/d, fading to 0 over the last 2 m before `proximityMeters`; its pan is equal-power from its
  azimuth, at most 0.6 either side (the centre at unity, no channel above it), ramped per sample. The sound ducks by 9
  dB while the world's card (Inspect), the directory or the quick catalog is open. The clip's first audio stream is
  decoded on its decoder's thread and resampled by swresample (44.1, 48, 96 kHz; mono to both channels, more channels
  down-mixed) into the slot's 0.5 s ring, timestamped on the video's timeline, trimmed at the picture's end so that
  every loop starts in sync; the sound follows the picture's media time (nothing within 20 ms, the reading ratio
  corrected by up to 1% to 120 ms, a resync beyond). Before a launch the mix fades out in 50 ms (begun before the
  screens' decoders stop) and the device stops; it starts again after the return. A device lost, or not started again,
  is tried again (at once after a loss, then every 2 s), never while a game runs. `--audio-capture <wav>` (tests,
  frame-limited, the null output) writes the run's mix as a 32-bit float 48 kHz stereo WAV. The report's `audio` section
  (written last): `device`, `reason` (`RV_AUDIO`, `--audio`, `frame-limited`, `interactive`, `video.json`,
  `device-failed`, `model-viewer`), `deviceName`, `sampleRate`, `mutedAtStart`, `muted`, `muteToggles`, `duckedFrames`,
  `duckChanges`, `blocks`, `framesMixed`, `capture`, `captureStartSeconds` (the run's time of the capture's first
  frame), `sync` (`blocks`, `resyncs`, `corrections`, `underrunFrames`, `overflowFrames`, `p50Milliseconds`,
  `p95Milliseconds`, `maxMilliseconds`), `suspends`, `deviceStops`, `deviceStarts`, `deviceFailures`, `deviceLosses`,
  `deviceRecoveries`, `audible` (each change of the heard screen: `frame`, `atSeconds`, `screen`, `gameId`), `levels`
  (every 0.25 s: `distance`, `spatialGain`, `pan`, `gain`), `presented` (with a capture: each frame of the heard clip
  shown, `atSeconds` and `ptsSeconds`), `clips` (each clip heard: `gameId`, `codec`, `sampleRate`, `channels`); the
  lists keep their first 2048 entries.
- The clips' verdicts (I5 step 7, ADR-0016 §8, `docs/i5/step-7.md`): `<user data>/cache/media/videos.json` (canonical
  JSON, one verdict per clip: its path under the media root, its file's size and time, `playable` or the decoder's
  failure, its duration, size, audio rate and codec) is read by the screens' service on a thread of its own when it
  starts (a `--fixed-dt` run waits for it), under the lock its writers share. A clip with a failure verdict whose file
  is unchanged is never opened: its game loses its clip before any screen sees it (no decoder, no playlist turn), and
  its screens show its stills from their first picture. Whether its file is unchanged (its size and time) is looked at
  on that thread, once per clip and run, the clip left out meanwhile (a `--fixed-dt` run waits for the look); a file of
  another size or time is opened again. A clip that fails on a screen, or plays through once from its start with no
  decode error or damaged frame, gives a verdict merged into the file on that thread (the CLI's kept: `retroverse-cli
  media --check-videos` writes the same file); a missing file, a file the system could not read (`read-error`: access
  denied, a sharing violation, a device error) and a texture that could not be made give none. A playable verdict the
  file holds already for the clip's file as it is is not written again. The app keeps the failure verdicts and, of the
  playable ones, only their keys and files' facts. Deleting `cache/media/videos.json` forgets every verdict, and
  `retroverse-cli media --check-videos --recheck-failures` checks the failures again. Only clips under the media root
  have verdicts (a `--mock-catalog`'s have none). Nothing is ever written under the media root. The report's
  `video.verdicts`: `loaded` (the file was read: a missing file is no verdict), `entries`, `knownBad` (the games whose
  clips were left out: `gameId`, `verdict`), `given` (the run's verdicts, one per clip), `written` (those merged into
  the file), `writeFailures`.
- Controls go through the action map (`assets/config/default_bindings.json`, overridden per action by
  `<user data>/config/bindings.json`): WASD/arrows walk (Left Shift sprints), the mouse looks around once captured,
  gamepad sticks walk and look; Tab (gamepad Y) opens the quick catalog (below); M (gamepad Back/Select) opens the
  directory, E (gamepad A) at an elevator, the hub's totem or a proxy of its showcase too (above); P (gamepad X) pins
  in the editor (`edit_pin`, I4b step 3, above), Q and R (the gamepad's shoulders) turn a fixture it holds
  (`edit_rotate_left`, `edit_rotate_right`, I4b step 4); Esc (gamepad B)
  leaves one level: it closes a popup of the developer overlay (a combo's list, a color picker), else the game-session
  panel, else steps back through the quick catalog or the directory and closes it, else releases the mouse, else closes
  the overlay, else quits; F1 toggles the overlay; F5 reloads the shaders, F8 mutes the clips' sound or sounds it
  again (I5 step 5), F12 saves `<user data>/screenshots/retroverse-<timestamp>.png`.
  Interactive runs capture the mouse when the window has focus and on a click into it; frame-limited runs only on a
  click. Alt+Enter toggles fullscreen: the host handles it before the action map (fixed, not rebindable, never seen as
  Enter). The walker collides with walls and furniture and can step into a doorway but not leave the room. Developer
  builds also reload a shader within ~0.5 s of saving any of its files (`assets/shaders`); a broken edit logs
  `<file>:<line>: error ...` and the previous program stays in use.
- The product UI reads sixteen `ui_*` actions from the same map (`rv/input/action.hpp`, `input::isUiAction`; defaults
  in `default_bindings.json` and `ActionMap::defaults()`, which a test keeps equal): `ui_up` ... `ui_right` (arrows,
  D-pad), `ui_nav_x`/`ui_nav_y` (the left stick, bound without a dead zone: `ui::NavInput`'s hysteresis of 0.55 / 0.35
  of the travel is its dead zone), `ui_page_up`/`ui_page_down` (Page Up/Down, the triggers), `ui_home`/`ui_end` (Home,
  End, the stick clicks), `ui_activate` (Enter, A), `ui_back` (Esc, B), `ui_media` (Y), `ui_info` (X),
  `ui_filter_previous`/`ui_filter_next` (the shoulders). They share keys and buttons with the world's actions; letters,
  digits, space, Backspace, Delete and Tab stay with RmlUi's text fields. A user's `bindings.json` overrides them per
  action like any other. `spike_ui` reads them, and so does `retroverse.exe` while the quick catalog, the directory or
  the game-session panel is open (and `map`, which closes the directory), the world's actions while they are closed
  (`rv/app/input_context.hpp`: the walker stands still under the catalog and the directory).
- Developer overlay (Dear ImGui, `rv_devui`; developer builds only: Release compiles without it and only warns about the
  two options below): F1 shows and hides it, `--devui-open` opens it from the first frame (automation, screenshots).
  Docked panels around the scene: Stats (FPS, a graph of the last 240 frame times, GPU times of the shadow map, the
  depth pre-pass, the ambient occlusion, the scene pass, the post-processing and the outline, what the last view drew
  with its static groups' draw calls, instances and clusters, GPU resources, the GPU memory per kind now and at its
  peak, the hero CRT's "Video" section, the room's "Collection" section: layers resident, the cache, the hero textures),
  Camera (feet, yaw, pitch, FOV, speeds, mouse sensitivity, invert Y, collision off = noclip, reset to the scene
  camera), Lights (the key light, in a scene with areas the eye's area's, every scene light with its shadow flag and
  strength, the background and the ambient, live), Materials (base color, metallic, roughness, emissive strength, live
  and never saved), Rendering (debug line kinds, wireframe, vsync, the key light's shadow on/off with its slope and
  constant bias and normal offset, ambient occlusion on/off with radius and intensity, post-processing: exposure, bloom
  strength and radius, vignette, all live and never saved; shader reload with the last compile errors), Log (level
  filter, autoscroll) and Scene (the objects, with "go to", which frames the object in the part of the view the panels
  leave visible). The default layout widens its side columns with the display scale (at least 18 and 22 font sizes).
  Interactive runs keep the layout in `<user data>/config/imgui.ini` (delete it for the default layout); frame-limited
  runs always start from the default layout and save nothing, so overlay screenshots never depend on earlier runs. The
  overlay sees every event first and keeps what it uses (a click on a panel, keys while a value is edited, so Esc in a
  text field never quits). Opening it frees a captured mouse for the panels; a click into the scene captures it again,
  Esc releases it, and closing the overlay gives back the capture it took and closes its popups. Screenshots leave the
  overlay out (the product UI, when open, is always in them) unless `--screenshot-include-ui` is given: the overlay is
  then drawn into `--screenshot` and F12 images too, scaled uniformly to fit `--screenshot-size` and centered (another
  aspect than the window's leaves bands of scene beside it). A frame-limited run given `--devui-open` or
  `--screenshot-include-ui` fails (exit 3) when the overlay cannot start.
- `--spawn x,y,z[,yaw[,pitch]]` starts the walker with its feet at (x, y, z) meters (the eye 1.65 m above), looking
  along yaw and pitch degrees (0 looks at -Z, positive yaw turns right; the scene camera's when left out), instead of at
  the scene camera; the final camera that frame-limited runs log reads the same way. In the generated building the
  frame is `--start-room`'s room's, else the floor's. The walk scripts start at `--spawn
  0,0,3.2,0,-6`, on the way in of the test room, so they do not depend on its scene camera.
- `--input-script <json>` replaces the actions of chosen frames (`rv/input/input_script.hpp`: an array of
  `{"fromFrame": a, "toFrame": b, "actions": {"move_forward": 1.0, "look_delta_x": 20.0}}`, frames counted from 0,
  both included; UI actions too, `{"ui_down": 1}`, `{"ui_nav_y": -0.9}`, in [-1, 1], a button action held at the press
  threshold) and logs the pose at the end of each step; a step may carry `"text"` (UTF-8, 1 to 256 bytes, no control
  character), typed once on its `fromFrame` into the product UI's focused text field (the quick catalog's search field
  is two `ui_up` above the list's first game), or `"phase": "<name>"`, a phase of the run report open on its frames,
  with or without actions (`measurement_phases.json`). Only the listed actions are scripted: in a visible window with
  focus, keys and pads still drive the others, so automated runs add `--hidden`. `--fixed-dt <seconds>`
  replaces the measured frame time, for the walker and the shader time alike, so scripted runs are reproducible.
  Frame-limited runs log the final camera (`Final camera after N frame(s) (T s simulated): feet ..., yaw ..., pitch
  ...`) and the nearest collider with its clearance.
- The quick catalog (`rv_ui`'s, over the scene, below the developer overlay) shows the ES-DE catalog of the user data
  directory (I2 step 13, `docs/i2/step-13.md`; not in the model viewer): `library.json` and the database that
  `retroverse-cli library create` and `import` made, opened on a catalog worker thread (`rv_app_catalog`'s
  `CatalogWorker`: the database's writer, the lists, each game's launch readiness, Avvia's resolution right before the
  launch and the play sessions; the main thread fetches pages on its own read-only connection and never waits). Without
  them the list says how to make them (notices `no-library`, `no-database`, `data-directory`, `library-mismatch`,
  `unavailable`; nothing is written into the data directory but the log). The card shows each game's readiness (one
  Italian line) and its first blocker's sentence; Avvia on a game that is not ready shows why and records a blocked
  session. Runs with `--fixed-dt` wait between frames for the worker (lockstep, at most 10 s at a time) so that scripted
  frames see the same lists and launches every run; other runs never wait. "Aggiorna libreria" (a row under the header,
  Up from the search field; I3 step 13r, `docs/i3/step-13r.md`) and `--refresh-library` (once the catalog opened with
  its first list) refresh the catalog from ES-DE: the import of `retroverse-cli import` as a job of the catalog worker,
  in slices of about 4 ms, lists and readiness answered meanwhile (Avvia and sessions outside its write transaction);
  after its commit the systems' readiness is resolved again and the list runs again with the selection kept by GameId; a
  line beside the button tells the progress and the result; quitting cancels it (the import rolled back or committed
  whole). The mock catalog has no refresh (`--refresh-library` records `unavailable`); a refresh is no lockstep wait.
- `--mock-catalog <file.json>` replaces the ES-DE catalog with a file's games, for the tests (`docs/i2/step-A1.md`;
  `rv/app/mock_catalog.hpp`: JSON with comments, `schemaVersion` 1, `games` each with an optional `launch` {`profile`,
  `rom` absolute or relative to the file, `system`, `variables`}, optional `profiles` over the shipped and the user's;
  unknown fields are errors, a file that cannot be loaded exits 2). Either way, Tab opens and
  closes the catalog (the pad's Y opens it and is "Vedi media" inside, X "Informazioni": the screens above),
  arrows or the D-pad move, Enter (A) on a game is "Avvia": the launch is planned with the game's profile and started
  through `LaunchSession` on `Win32ProcessRunner` (the guard applies) with ADR-0006's keep policy. While the game runs
  the host renders nothing and waits for events, at most 100 ms at a time, woken by the runner: frames, the `--frames`
  budget and input-script frames stand still. The game-session panel ("Gioco in corso", `assets/ui/game_session.rml`,
  not reloaded when saved yet) asks when the session needs it and tells the outcome. Quitting or closing the window
  mid-game leaves the game running; `--launch-wait-limit <seconds>` (frame-limited runs only; default 120, at most
  3600) leaves a game still running then and lets the run finish its frames. Hidden windows hand nothing over and count
  as focused. To try it, copy `tests/smoke/catalogs/fake_emulator_games.json` under `build/` with `@FAKE_EMULATOR@`
  replaced by the build's `rv_fake_emulator.exe` and its `roms/*.gb` created (as `tests/smoke/test_launch.cpp` does),
  set `RV_LAUNCH_ALLOWLIST` to the fake, and run `--mock-catalog <copy> --input-script
  tests/input_scripts/quick_catalog_launch.json --exit-after-frames --frames 50 --hidden --fixed-dt 0.016667
  --data-dir build/rvdata --run-report build/a1/run.json`.

