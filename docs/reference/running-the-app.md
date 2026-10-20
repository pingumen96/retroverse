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
  first frame and uploads, the fallbacks with their reasons, the launches' stops and reopenings; `{}` without a CRT),
  `world` (`{}` without a room: the state, notice, outcome and rebuild, the plan's digest, placements per section and
  `unplaced` per reason (`roomKept` since I4 step 1), items, clusters, lights, findings, `times` from the process's creation; `preview`,
  `reconcile`, `itemsWritten`, `screenGames`, `objectFallbacks` of the persisted start; `interaction` (events, counts,
  `inspect`), `card` (`shownFrames`, `coverFrames`, `squeezedFrames`, what it showed, `focused`, `coverBox`, `events`),
  `launch` (`launches`, `returned`, `notReturned`, `showInRoom`, `walkerBefore`, `walkerAfter`, `poseRestored`, the
  placement asked, `events`), `liveUpdates` (one per update after a refresh: kind `unchanged`, `textures`, `items`,
  `rebuild` or `kept`, kept, moved, added, removed, the arrays grown)), `residency` (the collection's layers: format,
  array bytes, fronts and spines resident, covers, labels and failures, the cache's hits, misses and stores, `times`
  {`shownAtMs`, `allResidentAtMs`, `residentAfterShownMs`}, `arrival`, `uploads`, `lockstep`, `pausedRounds`,
  `retries`, `hero`) and `screens` (the media viewer and Informazioni: `viewerFrames`, `infoFrames`, `pictures`,
  `failures`, `pictureBox`, `stageBox`, `events` with the game and `from` card or catalog); also `catalog.refreshes`
  (one per refresh: trigger, outcome, times, the longest slice, counts, the lists asked meanwhile, `before` and
  `after`), `catalog.lists` (the waits of the lists), `gpuPasses` in `frameTimes` and in every phase (`shadow`,
  `depth`, `ssao`, `scene`, `post`, `outline`, `total`: p50, p95, p99, max over the frames past the warm-up that
  reported them; I3 step 17b), and in `lastView` the static instance groups (`staticGroups`), `outlinedInstances` and
  `gpuMilliseconds.outline`, only when the view has them; since I4 step 3, for a scene with areas, `sceneAreas`
  (`areas`, the `area` that held the eye at the last view, the `lights` packed for it by name and its `keyLight`, and
  `changes`, the area at the first view and each change with its frame). Phases opened by the code: `refresh`,
  `coverLoad`, `animation`, and by input scripts, `benchmark` for one. Frame-limited runs log the frame times (with the GPU passes),
  phases, CPU sections and GPU memory at the end. Read it instead of the log in scripts and tests.
- `--scene <name|file>`: a name loads `<asset root>/style_tests/reference_scenes/<name>.json` (default
  `m0_testroom`; also `material_test`, `instancing_stress`, `instancing_growth`), anything with a path separator or
  `.json` is a file. Scene objects can name glTF models (`"model": "<asset-relative path>"`, loaded once per path).
  A scene file may hold `"areas"` (I4 step 3, ADR-0013 §9; `rv/scene/scene_description.hpp`): parts of the scene,
  each an `id` and `boxes` ({`min`, `max`} in meters), whose lights are their own; every light then names its
  `"area"`. Each frame the app finds the area that holds the walker's eye (the first holding it, else the last one
  held) and the renderer packs only that area's lights: its key light (its first light with `castsShadows`) shadowed,
  its other point and spot lights nearest the eye up to the 16 the block holds; the other areas get the ambient only
  (`rv/scene/scene_areas.hpp`). A scene without areas packs all its lights in list order as before. A floor's scene
  (`retroverse-cli worldplan show --floor <id> --scene <file>`, `docs/reference/cli.md`) has an area per room (its
  interior and its doorway's passage) and one for the hall (`hall:<floor>`); its camera stands in front of the
  elevator facing east, and the rooms' doorways are open onto the hall (their stops left out), so the walker can walk
  from the elevator into any room.
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
- Surfaces are procedural, generated at startup (`rv/assets/surface_textures.hpp`, about 0.6 s in Release, 3 s in Debug,
  logged per surface): `oak_planks`, `walnut`, `varnished_wood`, `oak`, `painted_plaster`, `ornate_rug`,
  `brushed_metal`, `aged_plastic`, each with base color, metallic-roughness and (except the rug) normal maps. Materials
  name a map as `"procedural:<surface>/<map>"` in a texture slot, scene files as `{"procedural": "<surface>/<map>"}` in
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
  room's or the generated room's): one 480-row decoder, never two decoder threads. It plays the selected game's clip
  (the quick catalog's selection, or the room's item focused or inspected, after a 300 ms dwell; before any, the most
  recently played placed game with a video, else the first featured one), else its screenshot, title screen or cover
  (fitted to 640 x 480 on a worker), else its colour bars; it pauses out of view, stops (joined within 1 s) before a
  launch and reopens at its position after the return. Its games come from the ES-DE catalog (the room's items, since
  I3 step 10c) or from `--mock-catalog`'s `"media"` (`video`, `screenshot`, `titlescreen`, `cover`; absolute or
  relative to the file) and `"lastPlayed"`. `--video-freeze-pts <seconds>` (frame-limited
  runs, 0 to 86400) opens every clip paused at that time and holds the frame: two runs give the same screenshot byte for
  byte. NVIDIA's message 131154 (every video upload) is dropped like the informational ids.
- The generated room (I3, `docs/i3/step-10b.md` to `step-17b.md`; ADR-0010 to ADR-0012; the building since I4 step 1,
  ADR-0013, `docs/i4/step-1.md`): without `--scene`, `--model` or `--mock-catalog`, `retroverse.exe` opens the room of
  its ES-DE catalog's snes games: the plan under `<user data>/worlds/<library id>/` generated or updated and saved
  (generator 1.3, schema 2: the building, of which the app updates and shows the snes room alone, in its own frame, the
  other rooms as they stand, until I4 step 5a; a stored schema-1 plan of I3 is read lifted into the building and written
  in schema 2 by its first update, the schema-1 file kept in `backups/`; a room the update keeps as it is, for an older
  template version, prefab levels that changed or another generator major, shows with the regeneration's notice and its
  new games unplaced as `room-kept`), the covers and spines through the derived-texture cache
  `<user data>/cache/textures/<library id>`; the empty room while it is made; the test room with a notice when there is
  no catalog (no library.json or database) or no snes games; the empty room with a notice when the media root is out of
  reach before a first plan or the plan cannot be used. `--scene m0_testroom` shows the test room as before (G01's
  command has it). `--room-system <system>` (I4 step 2, until step 5a's floors and `--start-room`) shows another
  system's room instead of the snes one, its media family's items and fixtures and its covers fitted to their labels
  (ADR-0013 §4; a data directory with that system's catalog: the `esde/building` fixture's, or any); a system
  systems.json does not name is classified by the catalog's platform tags of it, as `worldplan` does. With a plan
  stored, its room shows at the first frame (covers and spines from the cache by the room's items and the system's
  platform tags, `<user data>/cache/world/<library id>/room-<system>.json`) before the snapshot is asked for, then is
  reconciled with it (unchanged: nothing moved, rebuilt or written; changed: committed again with the walker kept). A
  manifest missing from the asset root falls back to its family's prefab, warned once per run. The walker starts at the
  room's doorway camera (or `--spawn`). Covers come nearest first (the hovered item, the view by projected size, the
  rest by distance); the hovered item's front shows its hero texture (its cover at its label's shape, up to 1,024 px on
  the long side and 1,024 x 752 pixels, 8 kept); nothing is loaded while a game runs; after "Aggiorna libreria" the
  covers the media root could not give are tried again. Lockstep runs (`--fixed-dt`) wait before the first frame and
  between frames for the room, the layers of the frame's view and the hovered item's hero texture, and for the whole
  room before their last frame (10 s at a time): a stored room at frame 0, the reconciled one from frame 1.
  The item the reticle rests on within 2.5 m is outlined and labelled with its title; after 150 ms it leaves its slot (a
  face-out cartridge tips forward, a spine-out one slides out and turns its cover to the viewer). E, a captured left
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
  position after it. The quick catalog's card offers "Mostra nella stanza" for a game the room places (the walker eased
  to its item's Focus pose) and says why for one it does not. After "Aggiorna libreria" or `--refresh-library` the room
  follows the refreshed catalog live (the room's job again, the plan saved; new titles placed into their slots with
  their covers, nothing else moved; a dressed level, module or growth commits the room again with the walker and the
  pictures kept; never during a game). `<user data>/config/world.json` takes `"reduceMotion": true` (every transition
  instant) and `"lightingProfile": "evening" | "afternoon"` (`assets/world/lighting`, applied when the room is drawn,
  never stored in the plan; the profiles' `"on": "display"` accents light the display cases, `case_light`, and the
  featured shelf, `shelf_light`, while the 16 lights leave room); the room's materials come from
  `assets/world/themes/classic_16bit_warm.json`. With the mouse free (the developer overlay open) the cursor picks.
- Controls go through the action map (`assets/config/default_bindings.json`, overridden per action by
  `<user data>/config/bindings.json`): WASD/arrows walk (Left Shift sprints), the mouse looks around once captured,
  gamepad sticks walk and look; Tab (gamepad Y) opens the quick catalog (below); Esc (gamepad B)
  leaves one level: it closes a popup of the developer overlay (a combo's list, a color picker), else the game-session
  panel, else steps back through the quick catalog and closes it, else releases the mouse, else closes the overlay, else
  quits; F1 toggles the overlay; F5 reloads the shaders, F12 saves `<user data>/screenshots/retroverse-<timestamp>.png`.
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
  action like any other. `spike_ui` reads them, and so does `retroverse.exe` while the quick catalog or the game-session
  panel is open, the world's actions while both are closed (`rv/app/input_context.hpp`: the walker stands still under
  the catalog).
- Developer overlay (Dear ImGui, `rv_devui`; developer builds only: Release compiles without it and only warns about the
  two options below): F1 shows and hides it, `--devui-open` opens it from the first frame (automation, screenshots).
  Docked panels around the scene: Stats (FPS, a graph of the last 240 frame times, GPU times of the shadow map, the
  depth pre-pass, the ambient occlusion, the scene pass, the post-processing and the outline, what the last view drew
  with its static groups' draw calls, instances and clusters, GPU resources, the GPU memory per kind now and at its
  peak, the hero CRT's "Video" section, the room's "Collection" section: layers resident, the cache, the hero
  textures), Camera (feet, yaw, pitch, FOV, speeds, mouse sensitivity, invert Y, collision
  off = noclip, reset to the scene camera), Lights (the key light, every scene light with its shadow flag and strength,
  the background and the ambient, live), Materials (base color, metallic, roughness, emissive strength, live and never
  saved), Rendering (debug line kinds, wireframe, vsync, the key light's shadow on/off with its slope and constant bias
  and normal offset, ambient occlusion on/off with radius and intensity, post-processing: exposure, bloom strength and
  radius, vignette, all live and never saved; shader reload with the last compile errors), Log (level filter,
  autoscroll) and Scene (the objects, with "go to", which frames the object in the part of the view the panels leave
  visible). The default layout widens its side columns with the display scale (at least 18 and 22 font sizes).
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
  the scene camera; the final camera that frame-limited runs log reads the same way. The walk scripts start at `--spawn
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

