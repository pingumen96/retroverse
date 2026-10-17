# Backlog

Ideas and deferred work that is out of scope for the current iteration. Pull items into the roadmap deliberately.

## Removed from scope (decided)

- Integrated scraper (spec §10, FR-021 to FR-023): ES-DE stays the scraping tool, Retroverse reads its metadata and
  media in place and falls back on generated labels and screenshots for what is missing (ADR-0003).

## Deferred beyond v1 (decided)

- Writing back to `gamelist.xml` (spec §4.6): read-only until a round-trip test suite exists.
- Legacy EmulationStation, RetroBat and Batocera library adapters.
- VR, Linux build, hardware video decoding (D3D11VA), OpenGL 3.3 fallback.
- Alternative world style presets beyond `RetroMuseumCozy` (style §18).

## Follow-ups from M0

Consolidated at the close of M0 (Gate A). Each item names where the current behavior is documented.

### Image quality

- **Anti-aliasing beyond FXAA:** the views are anti-aliased by FXAA after the tonemap (M0 step 12), which smooths the
  stair-steps of edges within one frame but cannot rebuild geometry thinner than a pixel: shelf boards, frame moldings
  and far cartridges still crawl a little in motion, the bright bevel along a board's top edge breaks into dashes from
  a few meters away (the boards and cabinet tops bevel their top edges only, as a dark bottom bevel aliased worse,
  below FXAA's threshold), and the pass softens small print: its sub-pixel blend runs at a quarter strength, which
  leaves the cartridge titles at about 3:1 against their band from 1.3 m (4.8:1 without the pass). MSAA on the HDR
  scene target (a sample count in `gl::FramebufferDesc`, resolved with a tonemapped average before the bloom) or a
  temporal pass (jittered projection, reprojection with the depth; it would also feed the temporal accumulation of the
  ambient occlusion, below) would steady them and keep small print sharp.
- **Exposure per room or eye adaptation:** the exposure is one value per scene (`environment.exposure`, the overlay's
  Rendering panel). Rooms of very different brightness (a dim arcade next to the lit hub, I4) need an exposure per room
  or an automatic one (a luminance histogram of the HDR target, adapted over a second or two).
- **Bloom of thin emitters and highlights per theme:** emitted light always blooms, reflected light above a fixed soft
  threshold (`gl::kBloomThreshold`, `gl::kBloomKnee`: exposed brightness 4, knee 1), and the per-tap Karis average of
  the first bloom level, which keeps fireflies steady, also keeps only about 60 to 75% of the glow of emitters one or
  two pixels wide (neon tubes seen from afar; large screens and shades keep about 95%). A neon-heavy theme pack
  (`ArcadeAfterDark`, style §14.4) may want a stronger `bloomStrength`, a Karis weight on a wider luminance scale
  (`1 / (1 + luminance / k)`) or a lower highlight threshold; make them scene keys when such a room exists.

### Lighting, shadows and ambient occlusion

- **Bounce light for dark rooms:** the hemispheric ambient cannot lift 4%-albedo walls out of the ACES toe, so the
  test room relies on direct light, and its ambient occlusion (M0 step 11), which darkens the ambient only, stays a
  subtle cue there; the upper walls and the ceiling far from the lights fall to black (golden G01,
  `docs/testing/gate-a.md`). Candidates: light probes / IBL, a hemisphere tinted by the lit floor (indoor bounce comes
  mostly from below), or the fake screen light of style §4.3 (the test room's `crt_glow` is a fixed one; a point light
  near each TV with the slowly updated average screen color).
- **More lights per view:** the lights block holds 16 point/spot lights and one directional light; the test room uses
  14 (the ceiling key light, a pendant, a picture light on the right poster, the floor lamp's bulb and two shade cones,
  the table lamp's bulb and shade cone, the magenta accent, the sign's glow, the CRT's light on its cabinet, a spot per
  shelf, the entrance washer). Real rooms and the hub need per-room light lists or clustered shading.
- **More shadowed lights:** one key light per view casts shadows (M0 step 11). Rooms with several focal lights (the
  hub, a showcase spot, the floor lamp of the test room, whose corner its own unshadowed lights dominate, so it casts
  no visible shadow; no single key light can reach the lamp, the shelf and the TV at once) need a few shadowed lights
  per view: a shadow atlas with one tile per light, budgeted by screen coverage (style §20.2: 1-3 relevant
  shadow-casting lights). Meanwhile an unshadowed lamp standing on furniture must not reach the floor: the test room's
  table lamp ends its down light 0.8 m below the bulb, or it would light the floor under the sideboard and its
  compartment through the top.
- **Point-light shadows:** point lights cast none (key lights should be spots, logged otherwise). A cube map or a
  dual-paraboloid map would shadow a bare bulb or a pendant.
- **Cascaded shadow maps for large directional areas:** the directional map is fitted to all the shadow casters, which
  wastes resolution once a sun covers a large world (the garden area, an atrium); cascades fitted to the view frustum
  slices, snapped to whole texels like the single map.
- **Shadow map caching:** the map depends on the light and the casters only, yet every view draws it again (the window
  and a screenshot in the same frame: twice). Static rooms could draw it once and again only when a caster or the
  light changes.
- **Softer penumbrae:** the 3 x 3 tent of hardware PCF fetches gives a fixed penumbra of about 4 texels (1-3 cm in the
  test room). Large practical lights (ceiling panels, a window) would want contact-hardening shadows (PCSS) or a wider,
  rotated Poisson filter. A filter wider than the stair-steps of the map would also let thin geometry cast: the
  posters of the test room have `castsShadows` false, since their frames' shadows, a few millimeters wide, break into
  dashes under the 3 x 3 tent.
- **Ambient occlusion quality:** normals are reconstructed from the depth (slivers one pixel thin get no estimate);
  normals from a G-buffer attachment, temporal accumulation (with the jitter of a temporal anti-aliasing pass) and bent
  normals for the ambient would refine it. An art option to let it darken the unshadowed fill lights a little (as some
  engines do) would make contact cues visible in rooms lit mostly by lights; style §4 asks for the ambient only for
  now (the GPU smoke test measures about 11% at a block's base under a white ambient).

### Performance and memory

- **Texture array (or atlas) for cartridge labels:** the 96 labels of the test room share one mesh but each shows its
  own cover, so each has a material instance of its own and is a batch of one (96 of the room's 129 draw calls in the
  lit pass and in the depth pre-pass, and those in the key light's frustum in the shadow map; the shells are one batch
  of 96). Covers in a texture array indexed per instance (an extra instance attribute or an SSBO next to the model
  matrix) would draw every label in one batch; plan I3 already foresees covers in an LRU texture array. Meanwhile the
  depth passes could merge consecutive opaque batches of one mesh whatever their materials (they bind none). The GPU
  smoke test bounds the room at 160 draw calls: lower it when this lands.
- **Instance upload without the staging copy:** `scene::BatchPlanner` gathers the plan's matrices into
  `BatchPlan::instanceTransforms` (kept by `gl::SceneRenderer`, so no allocation), which `gl::InstanceBuffer::write`
  then copies into the mapped region: 64 B per instance twice. When instance counts reach the tens of thousands, let the
  planner emit item indices and gather the matrices straight into the region the buffer hands out.
- **Per-instance normal matrix:** `pbr.vert` inverts the instance's 3x3 matrix per vertex; precompute it per instance
  (or restrict instances to rotation + uniform scale) once vertex work matters.
- **Procedural surfaces at startup:** the maps of the eight surfaces of `assets::proceduralSurfaces()` are generated on
  the main thread before the first frame (0.6 s in Release, 3 s in Debug; the rug alone a third of it) and take 80 MiB
  of the test room's 105 MiB of textures (RGBA8 with mips). Once worker threads exist (I2), generate them off the main
  thread while the room shows its flat material colors, or cache the encoded maps under `<user data>/cache` keyed by a
  generator version.
- **The Low profile (style §20.1, I8):** halve or compress the procedural surface maps (BC7 for color, BC5 for
  normals); make the HDR targets cheaper (the scene and bloom targets are `GL_RGBA16F`, 8 bytes per pixel, and the scene
  target has two attachments, the radiance and its emitted part; `GL_R11F_G11F_B10F` would halve their bandwidth on
  integrated GPUs: no alpha, fewer mantissa bits, so check the dark gradients for banding first; the emission
  attachment needs no alpha of its own, the blend reads the shader's); drop FXAA, the ambient occlusion and the shadow
  map first (`docs/testing/gate-a.md`, the Low question of the checklist).

### Content and assets

- **Upholstery surface:** `MAT_Fabric_Sofa` (the test room's armchair, golden G01's largest foreground object) is a
  flat #683D42 at roughness 0.9, a smooth block next to the grained wood and the rug (style §2.1, §3.2: the normal map
  is optional). A procedural `upholstery` surface (a fine weave in the normal map, roughness micro-variation, the
  bordeaux of style §3 with a faint nap) and meter uvs on the armchair's boxes (`BoxUv::WorldMeters`) would make it read
  as fabric at 2 m.
- **glTF sampler state:** the loader does not expose the samplers of a model yet, so `gl::GpuModel` samples every model
  image with repeat and trilinear filtering (the glTF default); honor clamp/mirror wrap modes and nearest filters.
- **Screens of glTF models in scenes:** the asset preview shows its test pattern on a glTF model's ScreenSurface by
  drawing the primitives of the socket's node with `MAT_Screen_CRT` and the pattern (it finds them by
  `gl::GpuModel::meshKey`), but scene files cannot do it: the `materials` and `textures` of model objects are ignored
  (only prefab roles take them). Covers and video on glTF CRTs (I3, I5) need a scene binding for model screens (a
  material and texture per ScreenSurface socket, applied by `gl::GpuModel` when it appends the render items).
- **Normal-map tangent conventions:** validate them with the Khronos `NormalTangentMirrorTest` sample (ADR-0002), now
  that glTF models are drawn (`--model`): generated tangents were checked with synthetic normal maps in M0 step 6 (glTF
  tangent space, +Y towards decreasing v); file tangents and mirrored uv islands still need the sample.

### Tools and the developer overlay

- **Asset preview, further (style §19.3):** the LODs of a manifest (`runtime.lods`) and their switch distances are not
  loaded yet (no LOD exists in M0); files open by drag and drop only (an open-file dialog, `SDL_ShowOpenFileDialog`,
  and a folder browser would help); the scene labels are drawn by the overlay, over the scene whatever is in front, and
  placed for the window's aspect, so a screenshot of another aspect than the window's misplaces them (draw them into
  the scene targets with a text renderer, depth-tested, once one exists); glTF Validator results (style §26.3) are not
  shown. The floor grid's 10 cm lines follow the subject's size (shown up to 6 m across), not the camera: zoomed far
  out they crowd, zoomed into a room they are missing; a grid shader fading each level by its projected spacing would
  suit any distance. Crowded labels stack in a column at their point and can leave the visible area when many share a
  spot (a placement that also moves sideways would keep them in view). The panels settle over the first three frames,
  so a screenshot needs `--frames 3`: building the default layout and its tab focus before the first frame would make
  every frame complete.
- **Saving the developer overlay's edits:** light, environment and material tweaks are live only (M0 step 9). Writing
  them back to a scene file or to `materials.json` needs a JSON writer that keeps the files' comments and layout.
- **Hints that name keys:** log lines and overlay labels name the default keys ("F1 hides it", "Reload shaders (F5)",
  "Esc releases it"), which are wrong once `<user data>/config/bindings.json` rebinds those actions. Format the bound
  keys from the active `input::ActionMap` (a display name per key and pad button) when the controls settings screen
  arrives (I7).

### Testing and automation

- **Continuous integration:** none yet: the GitHub repository has no workflow and every build and test runs locally
  (setting up CI acts outside the machine and waits for the owner's go-ahead). A hosted Windows runner can build every
  preset and run the unit tests; its GPU smoke tests would skip (no OpenGL 4.5 context: exit code 77), so the GPU tests
  and the golden comparison need a self-hosted runner on the reference machine (`scripts/gate.ps1 -Gate A` is the
  command), and the vcpkg binary cache a shared location.
- **More golden images:** G01 is the only golden and is compared on the reference machine only (GL_RENDERER and
  GL_VERSION must match its metadata). G02-G10 (style §26.1) come with the rooms they show (I3-I8); golden images per
  quality profile come with the profiles (I8). Other machines could compare with a perceptual metric and a looser
  tolerance instead of skipping.
- **Run report of an invalid command line:** `--run-report` takes effect once the command line parses, so a script
  that passes a bad option gets exit code 2 and the usage on stderr but no report. A minimal report (exit code, the
  parse error) would need the report path read before the other options are validated.
- **Benchmark runs:** the run report (`--run-report`) records the last view's statistics and GPU times; the frame time
  benchmarks of specification NFR-03 and NFR-04 (60 FPS at 1080p, p99 at most 33.3 ms; a `--bench --frames 600` run)
  need frame time percentiles over the run in it and a fixed camera path (`--input-script`), first in I2/I3, in full in
  I8.

### Platform and infrastructure

- **Wide-character log paths:** decide whether spdlog should be built with wide-character file names for non-ASCII
  log paths.
- **Atomic writes of critical state:** consider `FlushFileBuffers` before the rename of `core::writeFileAtomic` for the
  world plans of I2/I3.
- **Log copy with worker threads:** the overlay's `devui::LogView` reads `core::loggedMessageCount()` and then the
  newest lines, so a line logged by another thread in between can show twice. Once workers log (I2), let `rv::core`
  return the lines and the count under one lock.

## Follow-ups from I1

### Video (for I5)

- **Decoder scheduler** (spec §9.5, NFR-07): 2 decoders in the Balanced profile, 1 in Low; pause or stop out-of-view
  screens (`media::VideoPlayback::pause`); share one `gl::VideoTexture` between screens showing the same clip; drop to
  one decoder when frames come late; stagger 720p uploads over frames (two uploads due in one frame cost p99 2.1 to
  2.7 ms).
- **Process priority while rendering:** frame pacing held at normal priority up to about 46% machine load and failed
  from 51% (with no other agent running, 25 to 31%, every run held it); ADR-0004 makes above-normal priority of the
  process while it renders an I5 requirement (a boost of the render thread alone, or MMCSS, is the alternative to
  measure).
- **Cheaper uploads:** convert straight into the mapped PBOs (about 0.8 ms per 720p frame saved on the render thread);
  YUV 4:2:0 planes converted in the screen shader (1.5 instead of 4 bytes per pixel) if 1080p previews or more decoders
  are wanted (a full 1080p RGBA upload has p99 2.37 ms); a cheaper downscale for 1080p sources (5.4 ms per frame).
- **Asynchronous copies:** NVIDIA reports id 131154 ("Pixel transfer is synchronized with 3D rendering") for every
  upload, about 0.5 ms GPU per 720p frame: try a second GL context for the copies, and let the debug filter drop that id
  for video uploads.
- **Clip audio** (spec §9.7): swresample and miniaudio, and which clock drives presentation.
- **Library-wide checks:** the corruption and truncation thresholds against every video of the collection; an hour-long
  soak with the scene renderer.

### Product UI

- **Per-change cost under 1 ms:** the selected row is marked by class and the list's rows are recycled (I2 step 6),
  yet a held direction that scrolls the list costs about a tap (2.8 to 3.0 ms at p50): RmlUi 6.3 lays the whole screen
  out again for the entering row's text. Remaining: the list and the detail card each in a document of their own, a
  pooled vertex buffer in the vendored GL3 renderer; profile RmlUi with Tracy (`RMLUI_TRACY_PROFILING` is off in
  vcpkg's build).
- **Card settle delay:** tune it (0.15 s) on a TV.
- **Text entry with the pad:** an on-screen keyboard for the product UI (I7); the hint bar built from the active
  bindings. Until then the quick catalog's search takes typed text: M1's manual session is pad-only except for its one
  search step, typed with the keyboard (decided at Gate M1, I2 step 14).
- **Keyboard defaults for Vedi media, Informazioni and the filters:** they have pad buttons only (I2 step 6), since
  letters, digits, the space bar, Backspace, Delete and Tab stay with RmlUi's text fields; bind keys if any can be
  spared from text entry.
- **Localization:** tables in `assets/ui/locale` bound through RmlUi's data model, and number formatting with them (the
  quick catalog's count reads "100000 giochi"); the game-session panel's sentences, which hold the game's title, are
  written in `assets/ui/game_session.rml` itself until then (I2 step A1; I7).
- **Pictures in the UI:** an image source scheme (`rv-texture:<key>`) mapping covers and video frames to
  `gl::TextureCache` textures in the render interface (I3, I5).
- **Upstream:** propose the vendored renderer's three local changes to RmlUi (output target, state-backup opt-out,
  sampler reset).
- **Registry:** register `ui/quick_catalog.rml`, `ui/quick_catalog.rcss`, `ui/game_session.rml` and
  `ui/game_session.rcss` (kinds ui-document and ui-style-sheet, provenance retroverse-original) once the asset validator
  covers `assets/ui`.
- **Shared overlay helper:** move `devui::fitOverlayCapture` below both overlays; `rv_ui` links `rv_devui_model` only
  for it.
- **GPU timings by result:** a result counter in `gl::GpuTimer`, so samples are recorded per query result (2 to 8% of
  quiet frames get no GPU sample today).
- **Automated pad input:** an `SdlPlatform` option to accept background joystick events in automated runs (`spike_ui`
  sets the hint itself).

### Emulator launch (for I2 and I7)

- **Frame stalls after returning from an emulator:** the first return of a run stalls about 100 ms then 147 to 325 ms
  (60 FPS back in 307 to 442 ms, up to 710 ms after an exclusive display mode); later returns 31 to 118 ms. Likely a
  presentation-mode change (independent flip against composed): check with PresentMon (`docs/spikes/i1-launch.md`,
  finding 7).
- **Batch launchers:** `.bat` files need a verified profile kind with cmd-specific escaping (the BatBadBut class of
  injection); until then only PE programs start.
- **More emulators:** DuckStation and real exclusive-fullscreen emulators from an isolated copy; an emulator on another
  monitor than Retroverse's; re-grant `AllowSetForegroundWindow` once a console emulator's window exists (it fails with
  error 87 while suspended).
- **Crash and loader-error returns under the default error mode:** not measured (the spike's crash return ran with the
  fake emulator's own error dialogs off), because on this machine a crash opens the Visual Studio JIT debugger prompt
  and a missing DLL the loader's modal box; decide Retroverse's error-mode policy (in the product, hard-error dialogs
  are disabled only around `CreateProcessW` today; background runs, `RV_LAUNCH_BACKGROUND`, disable them for the whole
  process since the I2 step focus).
- **Release subsystem:** consider a GUI-subsystem `retroverse.exe` for Release (ADR-0006 open risk).

## Follow-ups from I2

### Catalog

- **Sort keys that fold diacritics and are composed (NFC)** (`kSortKeyVersion` 2, `rv/catalog/metadata.hpp`): version 1
  only lower-cases with the invariant locale, so accented titles sort after "z" ("Écran" after "Zone"); fold accents to
  their base letters (Windows' normalization functions can decompose them) and consider a natural order of numbers ("2"
  before "10"). Version 1 does not normalize either: the one-character filter reads both forms of a character meanwhile
  (I2 step 9), and the NFC helper private to `catalog_query.cpp` (`NormalizeString`) moves to `rv/core/utf8.hpp` when
  the sort keys need it.
- **The gamelist entry kind in the snapshot** (I2 steps 7 and 8): over the library snapshot, `media --coverage`'s
  `gamelist-entries` population is "every `<game>` entry" only as far as the snapshot tells entries apart. It leaves out
  the `<game>` entries `EsdeLibrarySource` skips with an issue (`gamelist-kind-mismatch`, `gamelist-entry-inside-game`)
  and counts the `<folder>` entries ES-DE excludes (`NotAGame` with a gamelist path); neither happens on the reference
  library, the nested-roms and noload fixtures show both. Record which kind of gamelist entry named an entry, keep the
  skipped `<game>` entries (as entries or a count per system), and let `inPopulation()` take the kind instead of the
  state.
- **`discover` and `MAMENameStripExtraInfo`:** the settings whitelist gained the key at I2 step 7 (arcade names follow
  it), but `discover` still prints eight of the nine settings it reads.
- **`--system` for `db rebuild-index`:** a partial rebuild is refused (I2 step 11a); rebuilding one system would repair
  it without reading the whole library.
- **Folder times as listed lag behind the folder:** a folder's last write time comes from its parent folder's index,
  which NTFS brings up to date later, on its own, so two imports a moment apart can see two times for a Folder entry or
  a game directory and write its row again once (I2 step 11a, "Folder times as Windows lists them"); harmless today.
- **A compact history for re-keying:** a casing table change re-keys every identity with one `rekeyed` history row each
  (about 105,000 rows on the reference library, I2 step 11b); one event for the library would say the same.
- **Reconciliation across imports:** only identities that vanished at this import are candidates (ADR-0008 §4), so a
  file deleted at one import and back elsewhere at a later one gets a new identity; the review of I2 step 11b proposed
  every missing identity with a stored quick hash as a candidate.
- **The quick-hash pass with several readers:** it reads files one by one: 32 files/s cold from the reference library's
  external drive (gb: 8,611 files in 268.8 s, I2 step 11b), where the hashing costs little (812 to 1,014 files/s warm);
  `EsdeLibrarySource` already reads cue sheets with 8 threads (I2 step 7).
- **The quick-hash pass scheduled by the app:** in idle time on the catalog worker, between imports, a limit per slice
  (I7; the catalog worker of I2 step 13 does not run it yet: `retroverse-cli identities hash` does).
- **Review rows in the product UI:** shown and resolved through `ICatalogStore::reviews()` and
  `catalog::resolveReview()`, offering the review's candidates only (I7); `retroverse-cli identities reviews` and
  `resolve` have them now.
- **XML warnings with the user's paths:** `detail::loadXmlFile` (`src/esde/src/xml.cpp`, used by the settings, systems
  and find rules readers) logs what it read leniently at warning level with the file's path, under the user's ES-DE
  home; log it at debug level, as `readGamelist` does (I2 step 3), and report it as issues.

### Plan

- **Step 13r, the refresh on a worker, moved to I3** (decided at Gate M1): an import as a job of the app's catalog
  worker, with cancellation and the log race fix, the frame p99 within 33.3 ms during a refresh and quitting during one
  within a bound (roadmap row 13r; `docs/i2/step-13.md`, "Notes for step 13r", says how the worker takes it: the
  import's job with a cancel flag, the systems' readiness resolved again after its commit, the readiness cache cleared,
  the screen told to refresh; a long import in slices so that lists and session writes do not wait behind it; the
  quick-hash pass taking turns on the same worker). M1 refreshes with `retroverse-cli import`: an app open meanwhile
  reads the new rows with its next list (a search, a filter) and resolves its systems' readiness again at its next start.
- **The quick hash's whole-library cold pass:** about 45 minutes on the reference machine, estimated from I2 step 11b's
  gb sample (980.9 MiB in 268.8 s, cold); Gate M1 runs it on demand only (`scripts/gate.ps1 -Gate M1 -RealLibrary
  -FullHashPass`, `retroverse-cli identities hash`), not by default.

### Launcher

The I1 spike's launch items stay under "Emulator launch (for I2 and I7)" above; these come from the I2 steps.

- **The gate and test scripts under the automation guard** (ADR-0008 §8): `scripts/gate.ps1` and `scripts/test.ps1` set
  no `RV_LAUNCH_ALLOWLIST`, so the guard does nothing in the runner, launcher and spike unit tests and in the
  `desktop.launch.*` cases of `spike_launch`, which start the fake emulator unguarded (the `retroverse-cli launch` tests
  set it for their own duration since I2 step 12, `ScopedLaunchAllowList`); the GPU smoke tests of `rv_smoke_tests`
  already set it per case through ctest `ENVIRONMENT` since I2 step A1 (the build's fake emulator alone). Set it to
  `build/<preset>/bin/rv_fake_emulator.exe`, and check that the runner tests that start unlisted files on purpose (a
  text file or a broken program named `.exe`) still test what they mean to, since the guard refuses them first: the unit
  test "platform_win/process_runner: an image Windows cannot load fails to start, without a dialog" (I1 spike f, in
  `rv_tests` since I2 step 0b) expects `CreateProcessW` to fail, and the guard would refuse the broken image before it.
  The image is written into a scratch folder per run, which no list can name in advance: that test must clear the
  variable for itself, or point it at its own image, before the scripts set it.
- **Derived profiles:** an `extends` field, so that a user profile changes a few fields of a shipped one instead of
  replacing it whole by id (I2 step 5), if users ask for it.
- **Startup grace per profile:** a `startupGraceSeconds` beside `windowTimeoutSeconds` in `emulators.json`; today every
  emulator gets `LaunchTimeouts::startupGrace` (2 s, within which an unclean exit is a failure to start and a clean one
  a quick exit).
- **Hard links in the automation guard:** a hard link or copy of DuckStation or RetroArch under another name is kept
  out by the allow-list only; compare file ids with known emulator installations.
- **Best-fit argument mangling ("WorstFit") for every emulator:** an emulator whose `main()` reads the ANSI code page
  receives a ROM path the code page does not hold as other text, and a fullwidth quotation mark splits the argument;
  the guard refuses that for RetroArch only. Plans could refuse or warn for such paths, or prefer emulators known to
  read UTF-16 (their manifest, `wmain`).
- **The guard's RetroArch option table** (`launch_guard.cpp`) holds RetroArch 1.21.0's options: newer options are
  refused until the table learns them, so follow RetroArch releases.
- **`%GAMEENTRYDIR%` in `%STARTDIR%`:** it requires launcher configuration today (I2 step 10); no command of ES-DE
  3.4.1 uses it.
- **A suggestion for a system that cannot launch** (SC-10): `profiles` lists the readiness of each command and profile
  of a system; it, and the app once it shows readiness (I2 step 13), could name the first ready one when the chosen one
  is blocked (ps2's first command, for example).
- **Profiles refused by the file opened:** step 5's `planLaunch()` refuses a profile's executable by the name found only
  (`isRefusedProgram()`), where the ES-DE translation also checks the file it opens (an 8.3 alias, a symbolic link
  named like an emulator, I2 step 10); give profiles the same check.
- **Ctrl+C during `retroverse-cli launch --wait` is not tested:** the console control handler stops waiting (the session
  left running, `quit`), but a test would need a console shared with the test process (I2 step 12).
- **"Verified" per command and profile in `profiles`:** `rv_launch_verifications` holds the latest normal return per
  library, system and choice (I2 step 12); `profiles` could show it beside each command's and profile's readiness.
- **A per-game choice in `launch --dry-run --path`:** the path dry run shows the system's choice only, since it does not
  look the identity up by its path; `launch --dry-run --game` shows both (I2 step 12).
- **A pending session write lost at exit:** a session write that meets a busy database is kept and tried again, the end
  for 2 minutes (`kPatientRetry`), and a CLI that still cannot write it prints it and exits 3; a process that ends
  before the database is free loses it (I2 step 12). The app's catalog worker can keep it longer.

### Test automation

These come from the I2 step focus (`docs/i2/step-focus.md`).

- **`focus_watch.py` in Gate A and the test script:** `scripts/gate.ps1 -Gate M1` runs under
  `scripts/focus_watch.py --watch build` since I2 step 14 and fails on a finding; Gate A and `scripts/test.ps1` could
  do the same, so that a test that takes the foreground is caught when it is added rather than by the user at the
  keyboard. A failing program of the user's during the run (WerFault, csrss boxes) fails the gate too: the watcher's
  attribution rules decide how often, and the gate's row names the rule that matched, so that such a finding can be
  told from a test's (the gate cannot tell it by itself).
- **Dialogs left in `rv_platform_tests` and `spike_launch_tests`:** they start no program and show no window, so they
  do not call `disableFailureDialogs()` and keep the C runtime's default dialogs (a failed assertion in Debug would
  show its box and wait until the ctest timeout). SDL's own assertion handler (`SDL_assert`, a message box) is not
  configured in any test program either (`SDL_SetAssertionHandler`, or `SDL_ASSERT=abort` in the test environment).

### App shell

Candidates from I2 step 13 (the real catalog in the app, `docs/i2/step-13.md`):

- **The app's log before the data-directory guard:** `retroverse.exe` sets up its log in `<user data>/logs` before
  the catalog worker checks the user data directory against the library's folders; `retroverse-cli` logs to stderr
  until the guard passed. The guard keeps the database out of the library's folders, but a `--data-dir` inside one
  would get the log file.
- **One launch library for the CLI and the app:** `rv_app_catalog`'s `CatalogLaunchLibrary` mirrors the CLI's
  `LaunchLibrary` (`launch_text.hpp`) and `cmd_launch.cpp`'s `resolveCatalogGame`, private to `rv_cli_core`; move
  them into one shared library (below the app and the CLI) so that the two cannot drift.
- **A refresh when another process imports:** the open app keeps the list it materialized until something asks again;
  an import made by `retroverse-cli` meanwhile shows at the next query or start (step 13r in I3, a file watcher in I7).
- **Readiness for every game in bulk:** a game's own readiness is resolved lazily, when its row is first read (step 13);
  a filter "ready only" would need it for every game at the opening (with disk reads for `%INJECT%` files and folder
  games).

The I1 spike's product UI items stay under "Product UI" above; these come from I2 step A1.

- **The quick catalog's focus after `show()`:** `ui::QuickCatalogScreen::show()` after `hide()` relies on RmlUi's focus
  event to restore its focus record, which RmlUi does not send when its focus never left the element; `ProductUi` moves
  RmlUi's focus to the context's root before `show()`. Fix it in `rv_ui`: record the focus explicitly in `show()`.
- **The game-session panel in `rv_ui`:** `GameSessionPanel` is private to `rv_app` (its GPU-free contract in
  `game_session_ui.hpp`); it can move beside `QuickCatalogScreen`. Its document, `assets/ui/game_session.rml`, does not
  reload when saved in developer builds yet (the catalog's documents do).
- **"Vedi media" and "Informazioni"** (`ui_media`, `ui_info`) do nothing in the app yet: the media viewer and the game's
  details come with the media and screens (I3, I5).
