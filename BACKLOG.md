# Backlog

Ideas and deferred work that is out of scope for the current iteration. Pull items into the roadmap deliberately.

## Deferred beyond v1 (decided)

- ScreenScraper live scraping: requires developer credentials from the ScreenScraper project (spec §10.2).
- Writing back to `gamelist.xml` (spec §4.6): read-only until a round-trip test suite exists.
- Legacy EmulationStation, RetroBat and Batocera library adapters.
- VR, Linux build, hardware video decoding (D3D11VA), OpenGL 3.3 fallback.
- Alternative world style presets beyond `RetroMuseumCozy` (style §18).

## Open items

- Validate normal-map tangent sign conventions with the Khronos `NormalTangentMirrorTest` sample (ADR-0002), now that
  glTF models are drawn (`--model`, M0 step 7): generated tangents were checked with synthetic normal maps in M0 step 6
  (glTF tangent space, +Y towards decreasing v); file tangents and mirrored uv islands still need the sample.
- Decide whether spdlog should be built with wide-character file names for non-ASCII log paths.
- Consider `FlushFileBuffers` for atomic writes of critical state (world plans) in I2/I3.
- Anti-aliasing beyond FXAA: since M0 step 12 the views are anti-aliased by FXAA after the tonemap, which smooths the
  stair-steps of edges within one frame but cannot rebuild geometry thinner than a pixel: shelf boards, frame moldings
  and far cartridges still crawl a little in motion, the bright bevel along a board's top edge breaks into dashes from
  a few meters away (the boards and cabinet tops bevel their top edges only, as a dark bottom bevel aliased worse,
  below FXAA's threshold), and the pass softens small print: its sub-pixel blend runs at a quarter strength, which
  leaves the cartridge titles at about 3:1 against their band from 1.3 m (4.8:1 without the pass). MSAA on the HDR
  scene target (a sample count in `gl::FramebufferDesc`, resolved with a tonemapped average before the bloom) or a
  temporal pass (jittered projection, reprojection with the depth; it would also feed the temporal accumulation of the
  ambient occlusion) would steady them and keep small print sharp.
- Exposure per room or eye adaptation: the exposure is one value per scene (`environment.exposure`, the overlay's
  Rendering panel). Rooms of very different brightness (a dim arcade next to the lit hub, I4) need an exposure per room
  or an automatic one (a luminance histogram of the HDR target, adapted over a second or two).
- Cheaper HDR targets for the Low profile (style §20.1): the scene and bloom targets are `GL_RGBA16F` (8 bytes per
  pixel, and the scene target has two attachments, the radiance and its emitted part); `GL_R11F_G11F_B10F` would halve
  their bandwidth on integrated GPUs (no alpha, fewer mantissa bits: check the dark gradients for banding first; the
  emission attachment needs no alpha of its own, the blend reads the shader's).
- Bloom of thin emitters and highlights per theme: emitted light always blooms, reflected light above a fixed soft
  threshold (`gl::kBloomThreshold`, `gl::kBloomKnee`: exposed brightness 4, knee 1), and the per-tap Karis average of
  the first bloom level, which keeps fireflies steady, also keeps only about 60 to 75% of the glow of emitters one or
  two pixels wide (neon tubes seen from afar; large screens and shades keep about 95%). A neon-heavy theme pack
  (`ArcadeAfterDark`, style §14.4) may want a stronger `bloomStrength`, a Karis weight on a wider luminance scale
  (`1 / (1 + luminance / k)`) or a lower highlight threshold; make them scene keys when such a room exists.
- Bounce light for dark rooms: the hemispheric ambient cannot lift 4%-albedo walls out of the ACES toe, so the test room
  relies on direct light, and its ambient occlusion (M0 step 11), which darkens the ambient only, stays a subtle cue
  there. It uses 14 of the 16 point/spot slots of the lights block (M0 step 12: the ceiling key light, a pendant, a
  picture light on the right poster, the floor lamp's bulb and two shade cones, the table lamp's bulb and shade cone,
  the magenta accent, the sign's glow, the CRT's light on its cabinet, a spot per shelf, the entrance washer), and the
  upper walls and the ceiling far from them still fall to black (golden G01, `docs/testing/gate-a.md`). Candidates:
  light probes / IBL, a hemisphere tinted by the lit floor (indoor bounce comes mostly from below), or the fake screen
  light of style §4.3 (the test room's `crt_glow` is a fixed one; a point light near each TV with the slowly updated
  average screen color); real rooms will need more than 16 lights per view (per-room light lists or clustered
  shading).
- Texture array (or atlas) for cartridge labels: the 96 labels of the test room share one mesh but each shows its own
  cover, so each has a material instance of its own and is a batch of one (96 of the room's 129 draw calls in the lit
  pass and in the depth pre-pass, and those in the key light's frustum in the shadow map; the shells are one batch of
  96). Covers in a texture array indexed per instance (an extra instance attribute or an SSBO next to the model matrix)
  would draw every label in one batch; plan I3 already foresees covers in an LRU texture array. Meanwhile the depth
  passes could merge consecutive opaque batches of one mesh whatever their materials (they bind none).
- Procedural surfaces at startup: the maps of the eight surfaces of `assets::proceduralSurfaces()` are generated on
  the main thread before the first frame (0.6 s in Release, 3 s in Debug; the rug alone a third of it) and take 80 MiB
  of the test room's 105 MiB of textures (RGBA8 with mips). Once worker threads exist (I2), generate them off the main
  thread while the room shows its flat material colors, or cache the encoded maps under `<user data>/cache` keyed by a
  generator version; for the Low profile (style §20.1) halve them or compress them (BC7 for color, BC5 for normals).
- Upholstery surface: `MAT_Fabric_Sofa` (the test room's armchair, golden G01's largest foreground object) is a flat
  #683D42 at roughness 0.9, a smooth block next to the grained wood and the rug (style §2.1, §3.2: the normal map is
  optional). A procedural `upholstery` surface (a fine weave in the normal map, roughness micro-variation, the bordeaux
  of style §3 with a faint nap) and meter uvs on the armchair's boxes (`BoxUv::WorldMeters`) would make it read as
  fabric at 2 m.
- glTF sampler state: the loader does not expose the samplers of a model yet, so `gl::GpuModel` samples every model
  image with repeat and trilinear filtering (the glTF default); honor clamp/mirror wrap modes and nearest filters.
- Per-instance normal matrix: `pbr.vert` inverts the instance's 3x3 matrix per vertex; precompute it per instance (or
  restrict instances to rotation + uniform scale) once vertex work matters.
- Saving the developer overlay's edits: light, environment and material tweaks are live only (M0 step 9). Writing them
  back to a scene file or to `materials.json` needs a JSON writer that keeps the files' comments and layout.
- Hints that name keys: log lines and overlay labels name the default keys ("F1 hides it", "Reload shaders (F5)",
  "Esc releases it"), which are wrong once `<user data>/config/bindings.json` rebinds those actions. Format the bound
  keys from the active `input::ActionMap` (a display name per key and pad button) when the controls settings screen
  arrives (I7).
- Log copy with worker threads: the overlay's `devui::LogView` reads `core::loggedMessageCount()` and then the newest
  lines, so a line logged by another thread in between can show twice. Once workers log (I2), let `rv::core` return
  the lines and the count under one lock.
- Instance upload without the staging copy: `scene::BatchPlanner` gathers the plan's matrices into
  `BatchPlan::instanceTransforms` (kept by `gl::SceneRenderer`, so no allocation), which `gl::InstanceBuffer::write`
  then copies into the mapped region: 64 B per instance twice. When instance counts reach the tens of thousands, let the
  planner emit item indices and gather the matrices straight into the region the buffer hands out.
- More shadowed lights: one key light per view casts shadows (M0 step 11). Rooms with several focal lights (the hub,
  a showcase spot, the floor lamp of the test room, whose corner its own unshadowed lights dominate, so it casts no
  visible shadow: the step 11 plan lists the lamp among the grounded furniture, and no single key light can reach it,
  the shelf and the TV at once) need a few shadowed lights per view: a shadow atlas with one tile per light, budgeted
  by screen coverage (style §20.2: 1-3 relevant shadow-casting lights). Meanwhile an unshadowed lamp standing on
  furniture must not reach the floor: the test room's table lamp ends its down light 0.8 m below the bulb, or it would
  light the floor under the sideboard and its compartment through the top.
- Point-light shadows: point lights cast none (key lights should be spots, logged otherwise). A cube map or a
  dual-paraboloid map would shadow a bare bulb or a pendant.
- Cascaded shadow maps for large directional areas: the directional map is fitted to all the shadow casters, which
  wastes resolution once a sun covers a large world (the garden area, an atrium); cascades fitted to the view frustum
  slices, snapped to whole texels like the single map.
- Shadow map caching: the map depends on the light and the casters only, yet every view draws it again (the window and
  a screenshot in the same frame: twice). Static rooms could draw it once and again only when a caster or the light
  changes.
- Softer penumbrae: the 3 x 3 tent of hardware PCF fetches gives a fixed penumbra of about 4 texels (1-3 cm in the test
  room). Large practical lights (ceiling panels, a window) would want contact-hardening shadows (PCSS) or a wider,
  rotated Poisson filter. A filter wider than the stair-steps of the map would also let thin geometry cast: the posters
  of the test room have `castsShadows` false, since their frames' shadows, a few millimeters wide, break into dashes
  under the 3 x 3 tent.
- Ambient occlusion quality: normals are reconstructed from the depth (slivers one pixel thin get no estimate);
  normals from a G-buffer attachment, temporal accumulation (with the jitter of a temporal anti-aliasing pass) and bent
  normals for the ambient would refine it. An art option to let it darken the unshadowed fill lights a little (as some
  engines do) would make contact cues visible in rooms lit mostly by lights; style §4 asks for the ambient only for
  now.
