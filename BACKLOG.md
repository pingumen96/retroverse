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
  matrix) would draw every label in one batch. The arrays exist since I3 step 6 (`gl::CollectionArrays`, the layers in
  the 80-byte instance record of step 0c), and the generated room is to draw its cartridges with them (steps 10b, 12);
  the test room keeps its per-label textures (ADR-0011 §4) until it moves to them. Meanwhile the depth passes could
  merge consecutive opaque batches of one mesh whatever their materials (they bind none). The GPU smoke test bounds
  the room at 160 draw calls: lower it when this lands.
- **Instance upload without the staging copy:** `scene::BatchPlanner` gathers the plan's instance records into
  `BatchPlan::instances` (kept by `gl::SceneRenderer`, so no allocation), which `gl::InstanceBuffer::write` then copies
  into the mapped region: 80 B per instance twice (I3 step 0c). The collection's static instance groups (I3 step 8)
  write nothing for a still room; when the dynamic path's instance counts reach the tens of thousands, let the planner
  emit item indices and gather the records straight into the region the buffer hands out.
- **Per-instance normal matrix:** `pbr.vert` inverts the instance's 3x3 matrix per vertex; precompute it per instance
  (or restrict instances to rotation + uniform scale) once vertex work matters.
- **Procedural surfaces at startup:** the maps of the eight surfaces of `assets::proceduralSurfaces()` are generated on
  the main thread before the first frame (0.6 s in Release, 3 s in Debug; the rug alone a third of it) and take 80 MiB
  of the test room's 105 MiB of textures (RGBA8 with mips). Generate them off the main thread on the worker pool
  (`core::WorkerPool`, I3 step 0b) while the room shows its flat material colors, or cache the encoded maps under
  `<user data>/cache` keyed by a generator version, as the derived-texture cache does for covers (I3 step 5).
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
- **More golden images:** G01 and the room goldens G02, G02s, G06, G07, G08a/b and G10 (I3 step 17a) are compared on
  the reference machine only (GL_RENDERER and GL_VERSION must match their metadata). G09 and golden images per quality
  profile come with the profiles (I8). Other machines could compare with a perceptual metric and a looser tolerance
  instead of skipping.
- **Run report of an invalid command line:** `--run-report` takes effect once the command line parses, so a script
  that passes a bad option gets exit code 2 and the usage on stderr but no report. A minimal report (exit code, the
  parse error) would need the report path read before the other options are validated.
- **Benchmark runs:** the frame time benchmarks of specification NFR-03 and NFR-04 (60 FPS at 1080p, a p99 of at
  most 33.3 ms) run in Gate B on one room (I3 step 17b's `bench_snes_room.json`, the `benchmark` phase with its GPU
  passes, `[benchmark]` and `[real-library][room-real]`); the full suite across rooms and profiles is I8's.

### Platform and infrastructure

- **Wide-character log paths:** decide whether spdlog should be built with wide-character file names for non-ASCII
  log paths.

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
  upload, about 0.5 ms GPU per 720p frame: try a second GL context for the copies (the debug filter drops the id on
  NVIDIA since I3 step 11).
- **No colour bars between two clips:** done in I5 step 1 (`docs/i5/step-1.md`): the previous picture stays up until
  the next game's still or first frame, and the building's screens show `standby`, not the bars.
- **Clip audio** (spec §9.7): swresample and miniaudio, and which clock drives presentation.
- **Library-wide checks:** done in I5 step 7 (`docs/i5/step-7.md`): 32,806 of 32,809 clips playable, the thresholds
  stand. The hour-long soak found the private bytes growing: see the next item.
- **Memory per clip opened (I5 step 7's soak):** done in I5 step 7's review (`docs/i5/step-7.md`, "Review"): every
  thread that ended while the process presented OpenGL frames cost 36 to 73 KiB in the graphics driver, and a clip
  started a thread; the decoders now run on worker threads that live on. The run report's per-frame samples (about
  80 bytes a frame) are kept exactly for 65,536 samples, then in a histogram within 1%.

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
- **Video frames in the UI:** the `rv-texture:<key>` images (I3 step 14a: covers in the world's card and the media
  viewer) for the video frames of the media viewer and the quick catalog's card (I5).
- **Upstream:** propose the vendored renderer's local changes to RmlUi (output target, state-backup opt-out, sampler
  reset, the `rv-texture:` images; `third_party/rmlui_backends`'s README lists them).
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

- **The video spike's timing bound under load:** `gpu.spike/video: two generated 720p clips play on time for 3 s`
  (Debug) failed once in Gate M1's verification with its p99 upload CPU time at 4.02 ms against 3 ms, while the user
  worked at the machine, and passed on four reruns; the gate measures on a machine in use, so a bound like this
  should allow for load (a retry, a looser Debug bound, or the figure reported rather than failed).

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
  an import made by `retroverse-cli` meanwhile shows at the next query or start. The app refreshes itself on request
  since I3 step 13r ("Aggiorna libreria", `--refresh-library`); noticing another process's import, or ES-DE's changes,
  by itself is the file watcher's (I7).
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
- **Videos and manuals in the media viewer:** Vedi media pages the images (I3 step 14b); the clip with audio and the
  manual's pages come with the decoder scheduler and audio (I5).

## Follow-ups from I3

Candidates from the I3 steps' notes (`docs/i3/step-<n>.md`).

### The collection

- **A label palette per theme:** the generated spines' and front labels' palette lives in code
  (`assets::kDefaultLabelPalette`, style §3's roles; I3 step 5) until a theme's data holds one (style §14's theme
  packs); `paletteColour` already takes any palette, so the label requests would carry the room's.

- **The display cases' base lit on the side facing the doorway** (I3 step 17a): the vitrines read as lit glass since
  the step's review, but the wooden base under them stays dark on its unlit south face (luma about 10 from the doorway).
- **Light positions in the template's data** (I3 step 16): the lights' positions are still constants in
  `fixture_layout.cpp`; moving them into the template is a template version of its own, for when a second template
  exists.
- **The empty room in the chosen profile** (I3 step 16): while the plan is made, `world_session.cpp`'s empty room uses
  the built-in evening profile (`shellScene` could pass `roomLightingProfile`), and the collection instance its built-in
  `themeMaterials` instead of `SceneData::theme.materials("cartridge")`.
- **ES-DE's lastplayed in the room's snapshot** (I3 step 10c): `<lastplayed>` is not in `RoomEntry`, so the hero CRT's
  start choice knows only Retroverse's sessions (a user who never launched from Retroverse sees the first featured
  title's clip); adding it is a catalog change.
- **The import's unknown media recorded** (I3 steps 2 and 10b): after an import whose `ImportStatistics::mediaUnknown`
  was not 0 the stored media presence may be stale, and the database does not record it, so a later start cannot tell.

### The app's room

- **The room's static part built on the pool** (I3 step 10b): the commit builds the static part's meshes on the main
  thread (about 50 ms in Release for the reference rooms, one hitch); `scene::buildSceneGeometry` could run on the
  worker pool (the material library's missing-name log would need a lock).
- **A pixel buffer ring for the collection's uploads** (I3 step 12, ADR-0011 §6): single layer uploads of 12 to 92 ms
  appeared in cold runs on a loaded machine (the first layer of a frame always goes, and a driver stall lands on it);
  `VideoTexture`'s ring is the remedy if a quiet machine sees them too.
- **Shared helpers of the room's smoke tests:** `test_world_focus.cpp`, `test_world_inspect.cpp`,
  `test_world_card.cpp`, `test_media_viewer.cpp` and `test_world_launch.cpp` each repeat a few helpers of
  `test_world_room.cpp` (the imported fixture, the room shown, `comparePicture`), kept apart while the steps of a wave
  ran in parallel; fold them into one header.

### Measurement

- **An option ending a frame-limited run once every layer is resident** (I3 step 17b): the real room's cold and warm
  runs wait out their frame count (20,000 and 8,000 frames, about 60 s and 25 s on a quiet machine) because nothing ends
  a run when the room is whole.

## Follow-ups from I4

Consolidated at the close of I4 (Gate C, `docs/testing/gate-c.md`) from the steps' notes (`docs/i4/step-<n>.md`) and
ADR-0013's "Deferred". Each item names where the current behavior is documented.

### The plan and its update

- **Expansion rooms and per-room regeneration (I4b; ADR-0013 §8, I4 step 6a):** no reference system overflows a room
  grown to its limit (step 2's census), so `unplaced: capacity` stays and the ids `room:<system>:<n>` and the
  `expands` field stay reserved; "Rigenera stanza / piano / mondo" with an impact preview, backup and undo is the only
  way to move a system between rooms or floors (`docs/i4/step-6a.md`).
- **`worldplan check` and kept rooms (I4 step 1):** the check judges a room made from an older template version or by
  another generator major as kept (`room-kept`, soft), but a room whose prefabs' levels changed still fails
  `prefab-slots` there, while `world::updateBuilding` keeps such a room and saves the plan (`docs/i4/step-1.md`). The
  check could load each room's data (`loadRoomData`) and ask `roomKeepReason()` as the update does.
- **Old template versions for kept rooms (I4 step 1):** a room `world::updateBuilding` keeps for an older template
  version, or for prefab levels that no longer hold its placements or dressing, cannot be drawn: the scene builder
  reads the template's current version and the prefabs as they are, so the app shows the shell with the `failed`
  notice (as I3 did for the same changes). ADR-0013 §4 keeps an old template version's file while a plan names it;
  `loadRoomTemplate` could read the version a room names (`docs/i4/step-1.md`).
- **A system waiting for a held room (I4 step 6a):** it has no entry in the plan meanwhile, so the quick catalog cannot
  say where it will stand; and on a full floor without a shared room a capacity entry of a small system's game that
  left the catalog stays until a regeneration (the plan does not say which section an entry was of;
  `docs/i4/step-6a.md`).

### The app

- **A floor's build behind its cold covers (I4 step 6b):** the floor-build job shares the worker pool with the cover
  loader's jobs; on the real computer floor started cold (5,314 covers to make) it took 13 s against 1.2 s warm, while
  the stored floor showed. A floor build ahead of the covers' jobs (its own thread, or the loader holding back while
  one is queued) would bring a cold rebuild's commit forward (`docs/i4/step-6b.md`).
- **The building stamp's misses (I4 step 6b):** a system that loses every entry keeps its last items file, so the stamp
  never matches until its room is updated and the whole job runs at each start meanwhile; a rebuild of the executable
  (even one that changes nothing) runs the whole job once; a start skipped by the stamp reports the plan's soft
  findings but no change report (`docs/i4/step-6b.md`).
- **The ride's covers in view (I4 step 8):** a ride's view comes back once the covers in view of the room that holds
  the eye are resident, or 3 s after the request; "in view" is the cover loader's frustum test, without occlusion, so
  from a doorway it holds the whole room (the real library's largest room: 1,646 of 1,646 items), and a cold ride comes
  back by the timeout (2.5 s, the room whole at about 17 s; `docs/testing/gate-c.md`). The view's items could be those
  whose projected size passes a bound, or those not behind a shelf's back panel, so that the wait means "what the eye
  can read".
- **A background warm-up of the building's covers (ADR-0013 "Deferred"):** a floor visited for the first time derives
  its covers then (the view comes back within about 3 s since I4 step 8, the room's last covers later); a low-priority
  pass over the other floors' covers while the walker stands still would make every first ride warm.
- **The favourites and recent games query scans (I4 step 7a):** `idx_games` has no index on the favourite flag or
  `<lastplayed>`, so the directory's tab reads every row (140 to 190 ms median on 100,000 generated games, Release,
  on the catalog worker; the tab says it is reading); an index would be a migration (`docs/i4/step-7a.md`).

### The look (the styling pass, I6)

- **The halls (I4 step 3):** dressed in I6 step 1 (the hall light role) and step 2 (`docs/i6/step-2.md`: trim,
  portals with flat and blade signs, beams, sconces with a baked wash on the wall, muted prints, benches with plants, a
  runner). Left: the sconces light nothing but their baked wash (no punctual light: step 1's budget; a picture light
  over a print would be one); the runner's rugs show their borders where they meet: a runner surface of its own (a
  long border and a repeating field instead of the medallion rug end to end); the prints share one muted palette, not
  their floor pack's (step 3's packs could carry a print palette); the east end wall's print and trim move with the
  wall as a hall grows; the lobby's 3D displays (the hub, done in step 5) and stairs (still deferred).
- **Theme packs (I6 step 3, `docs/i6/step-3.md`):** done per floor. Left: the neon sign is the pixel font's lettering
  as rounded tubes with a halo, not a neon typeface or a shaped neon (the concept's console outline); the portal signs
  of the halls stay tungsten (a dim version of their room's neon was left out); the era's colour beyond materials and
  light (since step 3b the furniture, rugs and posters are the era's, but from the doorway the colour shares barely
  move: the concept's colour is its posters' art, screens and signs); a print
  palette per pack for the halls. Gate D's G17 (the optical-era lounge from its doorway, `docs/testing/gate-d.md`):
  the room reads darker and calmer than concept panel 4 (median luma 28 against 35, colour 1.8% against 13%, blue
  5% against 19%), its upper walls and ceiling falling close to black (the top quarter's median 17 against 27: style
  §4 asks that they never do; a blue wash or cove on the upper walls, as the arcade's and the handhelds' got at step
  6), the sofa seen from the doorway a plain dark back, the large posters on the front wall behind the walker.
- **Era furniture (I6 step 3b, `docs/i6/step-3b.md`):** done in the hero corner and the decoration sockets. Left:
  furniture that needs a place of its own (a coffee table, the study's bookcases of boxes), a lounge chair of its own for the handheld archive (since the review a one-seat sofa on tall legs: from
  behind a plain upholstered back; an armchair whose back is thinner than its arms and leans back), the bean bags'
  shape (two ellipsoids, matte since the review), shelf dressing per era (the storage boxes and magazines are every
  pack's). (The rugs' metallic-roughness maps went to a quarter of their size with every procedural surface's in
  I6 step 5's review: their roughness alone, R8.)
- **The hub (I6 step 5, `docs/i6/step-5.md`):** done (the balcony, the coffered ceiling, the landmark, the portals,
  the totem, a screen, the favourites' showcase). Left: stairs to the balcony and a walkable gallery; the showcase's
  proxies are all keep cases (a proxy in its own system's family would need each family's prefab and arrays in the
  hub); the landmark's consoles are generic boxes on its plinth. After the step's review (signs in their rooms' neon,
  a calm blue entrance, a green elevator sign, the globe's light, darker floor and rugs): the accent share stays at 1
  to 4% against the concept's 5.6% (its brand neons; a hub of the fixture has one room's portal), and the wall band
  under the balcony at 27 to 47 against 47 (a brighter wainscot made it warmer, not brighter); the rugs repeat one
  design (the runner as rugs end to end: `runner_rug` along it, one large rug before the core, the packs' variants
  would vary it); procedural surfaces stay resident for the whole run (every one generated at the start: per-floor
  residency would save about 34 MiB on a floor but needs their generation off the commit); the hub's lights reach the
  area's 15 with the globe's (a light per room sign pushed the doorway's fill out of the arcade's view: the
  signs glow by their bloom alone; a hub of several portals adds a fill each, step 1's selection applies).
- **Arcade and handhelds (I6 step 6, `docs/i6/step-6.md`):** done (the arcade's rows of cabinets and its hero
  cabinet, the handhelds' counters and stations). Left: an arcade of more than 53 titles places the rest as unplaced
  for capacity (a second arcade room, a back room of cases or cabinets of two games, ADR-0014 §6 amended); the
  cabinets are slim (0.4 m deep, to fit the template's sockets: an arcade template with deeper sockets, a cabinet with
  a stool before each); a cabinet's marquee shows a generic word, not its game's title or marquee art (a marquee
  layer per item in the collection's arrays); the screens show the cover, never the game's screenshot or a clip (one
  decoder: ADR-0004, I5); empty cabinets of the reserve ask for a coin; the handheld rooms stay calmer than concept
  panel 6 (its colour comes from coloured boxes and a brand neon; our handheld cartridges are small and dark), the
  counters' handhelds seen from the aisle only. After the step's review (`docs/i6/step-6.md`, "Review"): the arcade's
  colour share stays at about 18% from its doorway against the panel's 31% (the panel's screens show full-colour
  action and its walls are covered in art; our floor and lower walls stay dark wood); taller cabinets (1.85 m, a 0.24 m
  marquee) are parked: they change the arcade room's plan (its fixtures' heights) and so every stored arcade room, for
  a look; the cabinets' hood still casts no separate shadow (the hero cabinet casts none at all: a per-part shadow
  flag would let its body shadow the wall while its hood does not); the other packs' display cases still stretch one
  riser card across an empty level (a card per slot changes G01 and the console rooms' goldens: done for the
  handhelds' counters only); the handhelds' glass has no brushed-metal edges (oak rails); a station in a dark corner
  stays dim (its strip lights its vitrine, not the corner); the doorway light probe of I6 step 1 passes for the arcade
  with a thin margin (21.2 against 21.0): emissive dressing (coves, flyers, side art) counts in its cut view as in its
  lit one, so a brighter arcade dressing needs the probe to subtract an emissive-only view first.
- **Light seen through doorways (I4 step 3):** done in I6 step 1 (`docs/i6/step-1.md`, ADR-0014 §4). Left: a hall
  with several doorways lights its rooms' walls near a doorway unclipped (a hall's own lights carry no portal; a
  room's, and a hall's of a single room, do), a faint light on a room's ceiling beside its doorway seen from the hall
  that goes when the eye enters the room; per-light portals for a hall's lights (the nearest doorway's) would close it.
- **The media families' faces (I4 step 2):** cases and boxes stand spine-out with their height in the shelf's depth
  until a side-face facing exists; a face-out item shows its spine label on its top face, upside down seen from the
  front (visible on the big boxes' 45 mm tops); the jewel cases' spine labels are 7 mm wide and the handheld cartridges'
  4.8 mm, too small to read beyond arm's length. Side-face spines, face-out items without a top label and the faithful
  families are I6's (`docs/i4/step-2.md`). Done in I6 step 4 (`docs/i6/step-4.md`): the `*_v2` families stand on their
  side with an upright spine, face-out items draw no spine label, stored rooms migrated once; since its review the spine
  label covers the side face top to bottom (a printed spine drawn at that shape into the unchanged 40 x 256 layer, a
  darker band at its top) and the cases' shells are dark. Left: a printed spine's text is squeezed 2.7 times along the
  jewel case's length in the layer (a taller spine layer would keep its glyphs sharper up close); a room kept by another
  rule (an older template) is not migrated until a regeneration; face-out items still get a spine layer made and
  uploaded that they never draw (the residency counts two layers per item: leaving them out needs the counts and the
  ride's wait per class); a missing `*_side` item manifest falls back at run time to its prefab family's generic
  (`media.case.plain.generic`, its spine on the top face; a manifest's `fallbackPrefab` must name its family's generic,
  so the side generic would need a prefab family of its own); a big box's shell tinted from its spine's colour (the
  spine array's background row sampled by the shell's material); the faithful per-console families.
- **Shared and arcade rooms (I4 step 4):** a sign per section of a shared room, L-shaped templates and layout candidates (`docs/i4/step-4.md`, ADR-0013
  "Deferred"). In a shared room of few titles the display cases' riser cards (an invented cover across the empty
  slots) outnumber the games there and read like games (G13: five cards beside four cases); plainer cards or other
  dressing on a mostly empty case.

### Tests

- **A GPU case of a room that grows (I4 step 6b):** no room of `esde/building` grows with its `additions/`, so the
  walker moved with a grown room (or with a start room grown under the scene's frame) is proven by `walkerMove()`'s
  unit test only; a fixture variant that fills a room to its growth step would show it end to end.
- **The ride's refusals end to end (I4 step 7b):** a ride refused while a floor applies (a start that does not wait) or
  while a launch is being decided, so the world's `room-unreachable` notice of the quick catalog's, and a building's
  result built again for the floor shown after a ride (`retargetResult()`) with its wait for a launch's end, have no
  GPU case: a lockstep run reconciles at its second frame and waits for both jobs. The routing's unit tests cover
  them (`docs/i4/step-7b.md`).

## Follow-ups from I4b

### The editor

- **The in-place apply in very large rooms (I4b step 3):** a game's move or swap shown in place lays the room's
  collection out again on the main thread, the cover loader adopting the floor's pictures and the plan state copied:
  about 80 ms per edit in a room of 1,561 big boxes (Release), one long frame per user action. Applying only the
  edited room's loader items and sharing the plan state instead of copying it would narrow it
  (`docs/i4b/step-3.md`).
- **A clickable bar (I4b step 3):** the editor's bar (`edit_bar.rml`) and its menu take no pointer input (pad and
  keyboard only), and the hover's title label does not show in the editor (the bar names the target). Clicks on the
  menu's buttons and on the bar's actions would follow the directory's and the card's (`docs/i4b/step-3.md`).
- **Furniture from the D-pad (I4b step 4):** a fixture is targeted by the reticle only, where no game or slot is (a
  shelf full of games by its frame, side or plinth); the D-pad's steps reach games and slots. A step that also reaches
  the room's movable fixtures (by wall, then the free-standing ones) would let a pad pick furniture without aiming
  (`docs/i4b/step-4.md`).
- **The wall-washers while a fixture is held (I4b step 4):** a held lamp's light follows it (since the step's review),
  but the room's wall-washers are placed again only at the drop (the room's static part rebuilt). Placing them again
  for each probed place (rv_world's lights of the probe's room, the scene's lights replaced in place) would preview the
  wall's light too.
- **The scene digest of a floor edited room by room (I4b step 4):** after a session whose static part changed, the
  floor shown keeps a synthetic scene digest, so the next live update after a refresh commits the floor once (2 s on
  the real computer floor, the walker kept); building the saved floor for its exact digest took 1.6 s, over Salva's 3 s.
  Digests per room (the floor's digest a digest of its rooms' and its hall's) would keep it exact at a room's cost.
- **Salva on a large building (I4b step 4):** 0.95 s on the real computer floor since the step's review (was 2.9 s),
  the commit's 0.86 s most of it: the plan of 19,985 placements serialised once (0.45 to 0.5 s), snapshotted (0.14 s)
  and written with its backup (0.2 s). A faster plan writer, or a snapshot that moves the files it replaces instead of
  copying them, would halve it.
- **Registries after many moves (I4b step 4):** a room's static part rebuilt alone registers the meshes and materials of
  the objects that changed; the old ones stay registered (unused), across sessions, until the floor is committed again
  (a ride, a building's result, a refresh's commit). Sessions that move furniture for long grow them a little;
  releasing unused entries needs reference counts in the registries.
