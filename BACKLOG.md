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

- **UI actions in the action map:** `ui_*` actions in `assets/config/default_bindings.json`, with `ui::NavBindings`
  built from the action map, so user rebindings reach the product UI (ADR-0005).
- **Per-change cost under 1 ms:** a pooled vertex buffer in the vendored GL3 renderer, the detail card in its own
  document, the selected row marked by class; profile RmlUi with Tracy (`RMLUI_TRACY_PROFILING` is off in vcpkg's
  build).
- **Large collections:** a virtualized game list; tune the card's settle delay (0.15 s) on a TV.
- **Text entry with the pad:** an on-screen keyboard (I7); the hint bar built from the active bindings.
- **Localization:** tables in `assets/ui/locale` bound through RmlUi's data model.
- **Pictures in the UI:** an image source scheme (`rv-texture:<key>`) mapping covers and video frames to
  `gl::TextureCache` textures in the render interface (I3, I5).
- **Upstream:** propose the vendored renderer's three local changes to RmlUi (output target, state-backup opt-out,
  sampler reset).
- **Registry:** register `ui/quick_catalog.rml` and `ui/quick_catalog.rcss` (kinds ui-document and ui-style-sheet,
  provenance retroverse-original) once the asset validator covers `assets/ui`.
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
- **While the emulator closes:** with the keep policy the last frame shows for the 170 to 350 ms RetroArch takes to shut
  down; resume when its windows are gone and the foreground is back, or draw a "returning" state.
- **"Game running" screen** (I2/I7) with return and go-back options, a StillRunning notice, and the quick-exit hint of
  `LaunchOutcome::quickExit`.
- **Batch launchers:** `.bat` files need a verified profile kind with cmd-specific escaping (the BatBadBut class of
  injection); until then only PE programs start.
- **More emulators:** DuckStation and real exclusive-fullscreen emulators from an isolated copy; an emulator on another
  monitor than Retroverse's; a window timeout per profile for windowless console emulators; re-grant
  `AllowSetForegroundWindow` once a console emulator's window exists (it fails with error 87 while suspended).
- **Crash and loader-error returns under the default error mode:** not measured (the spike's crash return ran with the
  fake emulator's own error dialogs off), because on this machine a crash opens the Visual Studio JIT debugger prompt
  and a missing DLL the loader's modal box; decide Retroverse's error-mode policy (hard-error dialogs are disabled only
  around `CreateProcessW` today).
- **Release subsystem:** consider a GUI-subsystem `retroverse.exe` for Release (ADR-0006 open risk).
- **Shared quoting:** `tests/smoke/child_process.cpp` duplicates the command-line quoting: use
  `rv::launcher::buildCommandLine`.
- **Fake emulator home:** move `rv_fake_emulator` from `spikes/launch` to `tests/support` when I2 adopts the launcher.

### Build

- **Which vcpkg the builds use (decision pending, ADR needed):** the presets take the toolchain from `VCPKG_ROOT`, and
  README, ADR-0001 and `scripts/dev.ps1` mean the local clone (`E:/Progetti/vcpkg` here, at the baseline). But
  `Enter-VsDevShell` runs the developer environment's vcpkg extension (`VC/vcpkg/vcpkg-init.cmd`), which points
  `VCPKG_ROOT` at Visual Studio's bundled, read-only vcpkg whatever it was, and `dev.ps1` checks the variable only
  afterwards: every tree has been configured with the bundled copy (`CMAKE_TOOLCHAIN_FILE` in `CMakeCache.txt`; tool
  2026-07-27 against the clone's 2026-09-26, 16 of the helper scripts they share differ, and the scripts enter the
  packages' ABI hashes). Options: the clone as documented (`dev.ps1` restores `VCPKG_ROOT` after entering the
  environment; one full rebuild of the packages, FFmpeg included; the clone is shared with other projects, whose updates
  would then rebuild Retroverse's packages), a clone for Retroverse alone (isolated; one rebuild and about 2 GB), or the
  bundled copy (no rebuild; the README and `dev.ps1`'s check change; a Visual Studio update can rebuild every package at
  the next configure, and a later baseline may need a newer tool than Visual Studio ships).
