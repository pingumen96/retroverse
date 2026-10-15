# Architecture

This document describes the code that exists today. The target architecture is in `specification.md` §15.

## Modules

| Module | CMake target | Responsibility | May depend on |
|---|---|---|---|
| core | `rv_core` | Errors (`rv::Expected`), logging (`FailureStreak`: which failures of an operation retried every frame to log; the in-memory recent lines with a message count and `parseLogLine` for the overlay), paths, frame clock, AABB/ray math, sRGB color helpers, file I/O, file change polling (`FileWatchSet`), JSON, command line parsing | glm, spdlog, nlohmann-json, tl-expected |
| geometry | `rv_geometry` | CPU meshes (`MeshData`, 48-byte `Vertex` with glTF tangent space), sockets, procedural mesh builder (box uvs in meters along each face's longer side, `BoxUv::GrainMeters`, for surfaces with a grain) and procedural prefabs with their collision boxes, uv scale of a surface, also as drawn with a transform (`uvMetersPerUnit`) | core |
| input | `rv_input` | Device-neutral codes, logical actions, action maps (with per-action user overrides), per-frame `ActionState`, input scripts for automated runs (`InputScript`, `InputScriptPlayer`) | core |
| assets | `rv_assets` | Image decoding/encoding, glTF loading, prefab manifests, material presets (texture files, procedural textures, the `crt` block of screens), shader source loading, procedural test patterns (cover placeholders, color bars, sign lettering; sized to their surface: `patternLongSideFor`, `patternSizeForAspect`), procedural PBR surfaces (`surface_textures.hpp`: wood, plaster, rug, metal, plastic) | core, geometry, fastgltf, stb |
| scene | `rv_scene` | Camera, first-person controller and viewpoints (`frontViewpoint`: where a walker stands to look at an object, framed in a part of the view, `ViewArea`), lights, render items, draw passes and the batching plan (`BatchPlanner`: frustum culling, opaque/mask/blend passes, blend back to front; the world bounds of what a shadow map draws, `shadowCasterBounds`), scene description files, scene content assembly (`buildSceneGeometry`: meshes shared by objects of the same shape, resolved materials, scene textures, glTF models through a `ModelLoader`, sockets, bounds and colliders per object), debug lines (`DebugLines`, `addSceneBoundsLines`, `addSceneSocketLines`), collision (`CollisionWorld`, `buildCollisionWorld`) | core, geometry, input, assets |
| platform | `rv_platform` | SDL3 window, OpenGL context, event pump, dialogs; input: SDL events -> `input::RawInputFrame` (`SdlInputCollector`, code tables in `sdl_input_codes.hpp`), gamepad hot-plug (`GamepadManager`), mouse capture (`MouseCapture`) | core, input, SDL3 |
| render | `rv_render` | OpenGL 4.5 device and state cache, buffers, vertex arrays, shader programs and the hot-reloading `ShaderLibrary` (with the build state and last error of every program), `GpuMesh`, uniform buffers, framebuffers (`Framebuffer`, sampleable depth, window blit, `clearDefault`), textures (`Texture2D`, `Sampler`, `TextureDefaults`, `TextureCache`), registries (`MeshRegistry`, `MaterialRegistry` with live factor edits; GPU bytes of both), glTF models on the GPU (`GpuModel`), the batched instanced `SceneRenderer` (`InstanceBuffer`, `GpuTimer`, wireframe) with the key light's shadow map (`ShadowMap`), the depth pre-pass and screen-space ambient occlusion (`SsaoPass`), the render targets of an output (`ViewTargets`: HDR scene target with its depth, ambient occlusion targets, bloom chain, tonemapped image, sRGB output), bloom, the post pass and FXAA (`PostProcessor`), debug lines (`DebugDraw`) | core, geometry, assets, scene, glad |
| devui | `rv_devui` | Dear ImGui developer overlay (developer builds only, `RV_DEV_UI`): `DevUi` (ImGui context, SDL3 and OpenGL3 backends, event routing, popups closed by Back, dock space and its see-through scene area, rendering into the window or a screenshot) and its panels (`panels.hpp`: Stats, Camera, Lights, Materials, Rendering, Log, Scene) | core, input, scene, platform, render, imgui |
| app | `rv_app` | Composition root: application host, main loop, input pipeline, FreeRoam walking, scene loading and instantiation (`SceneInstance`), the model viewer (`--model`), screenshots, the sRGB and bloom self-checks, the developer overlay's panels wired to the app state (developer builds) | all of the above |

Four GPU-free sub-targets keep testable policy out of the GPU-side libraries:

| Sub-target | Part of | Content |
|---|---|---|
| `rv_app_cli` | app | Command line options and validation (the walker's start of `--spawn`: `SpawnPoint`, `parseSpawnPoint`), scene selection (`resolveScenePath`), screenshot file names, exit-code policy, the Back action (`backAction` over a `BackState`: close the overlay's topmost popup, else release the mouse, else close the overlay, else quit), when a run needs the overlay and which frames draw it into screenshots (`overlayRequired`, `overlayCaptureSize`), the debug line kinds shown (`DebugLineSelection`, `composeDebugLines`), self-check verdicts (`evaluateSrgbCheck`, `evaluateBloomCheck`), the scene content plan (`planSceneContent`: distinct meshes, materials and picture instances, render items, model instances), the model viewer scene (`makeModelViewerScene`: framing, three-point light rig, background; `modelViewerGrid`; the model summary `describeModel`), the frame statistics log (`FrameStatsLog`) |
| `rv_devui_model` | devui | Built in every configuration: the frame time history of the Stats panel (`FrameTimeHistory`), the overlay's input routing (`routeOverlayEvent`) and mouse capture hand-over (`CaptureHandover`), the Log panel's copy of the log (`LogView`), where the overlay lands in a screenshot (`fitOverlayCapture`) |
| `rv_render_debug_filter` | render | Which OpenGL debug messages are logged, dropped or deduplicated |
| `rv_render_uniforms` | render | std140 uniform block mirrors and binding tables (`uniform_blocks.hpp`), scene lights and ambient packed into `LightsUniforms` (`packLights`, which also picks the key light that casts the shadows, `ShadowLight`; `requireShadowCasters` takes a directional key light's shadow away when nothing casts), the key light's shadow map (`shadow_settings.hpp`: `spotShadowProjection`, `directionalShadowProjection` fitted to the casters and snapped to whole texels, `shadowProjectionFor`, `ShadowSettings` and their `ShadowUniforms`, `makeShadowUniforms`, the depth bias of the shadow pass, `shadowDepthBias`), the ambient occlusion (`ssao_settings.hpp`: the deterministic sample kernel `ssaoKernel`, `SsaoUniforms` from the scene's `SsaoSetup`, `makeSsaoUniforms`, the half-resolution target size `ssaoTargetSize`), materials into `MaterialUniforms` (`makeMaterialUniforms`, with the picture-tube parameters of CRT screens), the post-processing of a view (`post_settings.hpp`: `PostSettings` from the scene environment, `postSettingsFor`, anti-aliasing on or off; its `PostUniforms` block, `makePostUniforms`, the bloom strength as its mix; the radius as the upsampling scatter, `bloomScatter`; the highlight threshold and knee; the bloom level sizes, `bloomLevelSizes`), the fragment outputs of the scene shaders (`SceneOutput`), the CPU mirror of the display transform (`tonemap.hpp`: exposure + ACES fitted, which tests use to predict displayed colors, and its inverse for greys, `greyRadianceForDisplay`), the renderer's frame statistics (`FrameStats`, header only), live material factor edits (`MaterialFactors`, `withFactors`) |

`core`, `geometry`, `input`, `assets`, `scene` and the four sub-targets are **GPU-free**: they never reach SDL3,
glad or ImGui (enforced at configure time by `cmake/LayerRules.cmake`), so they are unit-tested without a window or
a context. `render` receives the OpenGL function loader as a function pointer and never includes SDL. The SDL side of
`platform` (code tables, collector, gamepads through SDL virtual joysticks, mouse capture on a hidden window, the
fullscreen shortcut) is tested by its own executable, `rv_platform_tests` (label `unit`, SDL but no OpenGL), so
`rv_tests` stays SDL-free. Because `rv_render_uniforms` is GPU-free, tests check its std140 mirrors, material flags,
texture units, fragment outputs, tonemapping constants, bloom filter weights, shadow and ambient occlusion blocks and
constants against the GLSL sources, and the shadow map projections (cones on the map's edges, texel snapping)
without a GPU.

Cross-module contracts written by independent work streams (prefab parameters and sockets, material names, scene
files, manifests, shader sources and uniform blocks) are pinned by `tests/unit/integration`, which builds the real
geometry, resolves the real materials and preprocesses the real shaders.

## Rendering path (M0)

- **Scene:** `--scene <name|file>` (default `m0_testroom`) is loaded with `scene::loadSceneDescription`; `--model
  <file>` instead builds the model viewer scene (`app::makeModelViewerScene`: the model at the origin, a camera framing
  its bounds from the front right and above, a three-point light rig sized to it, a neutral environment whose dark
  mid-grey background, shown as about sRGB 61, sets off black plastic as well as ivory, without bloom, vignette,
  ambient occlusion or shadows).
  `app::SceneInstance` (`rv/app/scene_instance.hpp`) turns the description into drawable content, the reusable step
  between a scene and the renderer: `scene::buildSceneGeometry` builds every prefab or primitive once per kind and
  parameters (the 96 cartridges of the test room share one shell mesh and one label mesh, `ScenePart::meshKey`),
  loads every glTF model once per path (`scene::ModelLoader`: `assets::loadGltf` under the asset root, or the viewer's
  file), resolves each part's material (library name, inline material, magenta fallback; magenta placeholder cubes
  for unavailable geometry: builder errors, models that cannot be loaded or have nothing to draw) and attaches the
  texture the scene binds to the part's role. The GPU-free plan `app::planSceneContent` (unit-tested) lists the
  distinct meshes, the materials and one render item per part and object instance; `SceneInstance` registers them in
  the `gl::MeshRegistry` and `gl::MaterialRegistry` (deduplicated by key), uploads every distinct model as a
  `gl::GpuModel` (meshes `<path>#mesh<i>`, materials `<path>#material<i>`, images through the `TextureCache` as
  `<path>#image<i>`, the draw items with their node transforms, the sockets) and adds one render item per model
  primitive and model instance. It also packs the lights (with the key light that casts the shadows, and why the
  first light that asks for shadows has none, when it cannot), sums the world bounds of what the shadow pass draws
  (`scene::shadowCasterBounds`: the items that cast shadows, blended ones left out; a directional key light's map is
  fitted to them, and with none it casts no shadow: `gl::requireShadowCasters`), builds the collision world and the
  debug lines (bounds of every
  object instance, axes of every socket), and keeps the environment (background radiance, ambient, ambient occlusion,
  post-processing) that the overlay edits and every view reads.
- **Renderer:** `gl::SceneRenderer` draws a `gl::RenderView` (camera, render items, lights, the key light with the
  shadow settings and the casters' bounds, ambient occlusion, background radiance, post-processing settings, time,
  optional debug lines) through the `gl::ViewTargets` of an output, in passes: the key light's shadow map, the depth
  pre-pass into the HDR target, the ambient occlusion, the lit scene pass into the HDR target, then the
  post-processing into its sRGB output (below). Per view it writes `FrameUniforms` and, when they changed,
  `LightsUniforms` and `ShadowUniforms` (and the frame block of the shadow map, the light's view); plans the items
  with `scene::BatchPlanner` (frustum culling against the registered mesh bounds; opaque batches, then alpha-masked
  ones, each sorted by shader, material and mesh; then alpha-blended items back to front by the distance to their
  world bounds, consecutive ones with the same key merged), and when the view has a key light also its shadow casters
  against the light's frustum (`PlanOptions::shadowCastersOnly`: items with `castsShadow`, blended ones left out); the
  renderer keeps the planner and both plans, so planning allocates nothing once they have grown to the largest view.
  It copies every instance matrix of both plans into the `gl::InstanceBuffer` in one write (persistently mapped,
  coherent, three regions used in turn, each guarded by a fence placed after the draws that read it, grown and
  re-attached to the meshes when a view needs more) and issues one `glDrawElementsInstancedBaseInstance` per batch,
  binding the program and the material (uniform block range, textures, samplers, blend, depth writes, culling) only
  when they change; the depth passes draw opaque batches without binding their material (only its culling) and bind
  it for alpha-masked ones (the alpha test). A view whose instances cannot be written is skipped;
  `core::FailureStreak` logs the failure once per cause and the recovery with the number of skipped views. Mesh
  shaders read the instance matrix at vertex attribute locations 4..7 (divisor 1). `gl::GpuTimer`s (rings of
  `GL_TIME_ELAPSED` queries read frames later, never stalling) time the shadow map, the depth pre-pass (with the clear
  of the HDR target), the ambient occlusion, the lit pass and the post-processing of each view. `gl::FrameStats`
  (items, culled items, batches, draw calls, instances, triangles, shader and material switches, the draw calls and
  instances of the shadow map, the draw calls of the depth pre-pass, debug lines, the five GPU times; a view without a
  shadow map reports no shadow time) are kept per view; `app::FrameStatsLog` logs them with the frame times once per
  second in developer builds (`RV_DEV_UI`) and for the whole run at the end of frame-limited runs (frames that render
  no view, a minimized window, count their time only). The test room draws its 231 parts in 129 batches from the
  scene camera, which culls 6 (35 meshes uploaded), its shadow map 179 instances in 101 draw calls and its depth
  pre-pass 129; `instancing_stress` draws 1001 items in 4,
  its shadow map the 1000 boxes in 3, about 2000 instances per view in all, more than the 1024 per region the
  instance buffer starts with, so it grows on the first frame; `instancing_growth`, without shadows, starts below that
  and grows mid-run, turned around (about 2000 instances).
- **Shadows and ambient occlusion (M0 step 11, style §4.1 contact and shadow cues):** the key light is the first scene
  light with `castsShadows` (`gl::packLights` records it as a `gl::ShadowLight`: its index in the lights block, its
  geometry, its `shadowStrength`; a point light, one left out of the block, or a directional light while nothing
  casts shadows (`gl::requireShadowCasters`) casts none, and the log and the Lights panel say why). Its depth goes into
  a `gl::ShadowMap`, a 2048 x 2048 `GL_DEPTH_COMPONENT32F` texture in a
  framebuffer without color: `gl::spotShadowProjection` looks from a spot light along its axis with a square field of
  view of twice its outer cone (at most 150 degrees), from 5 cm to its range; `gl::directionalShadowProjection` fits
  an orthographic map to the casters' bounds (`scene::shadowCasterBounds`: what the shadow pass draws) in a light space
  that only rotates the world and snaps its edges to whole texels, so moving bounds slide the grid by whole texels.
  Neither depends on the camera: shadows are locked to the world and never shimmer as the walker moves (the frame block
  of the map carries no time either, so a static light writes it once). The shadow pass draws the casters with the
  depth programs (`pbr/pbr.vert` + `depth/depth.frag`, `RV_ALPHA_TEST` for alpha-masked materials), culled like their
  materials (double-sided ones on both sides), their depth pushed back by a slope-scaled bias (`glPolygonOffset`,
  factor and units 2, `gl::ShadowSettings`). Thin geometry is a culling choice of the scene: flat wall decorations
  (posters, signs) are marked `castsShadows: false`, since their shadows would be a few millimeters wide, narrower than
  the filter below, and the stair-steps of their silhouettes in the map would break them into dashes; decals that lie
  on or in their object (cartridge labels in their pocket) cast with it, and the biases keep the surface under them
  from shadowing itself. `pbr.frag` (`common/shadow.glsl`) darkens only the key light's radiance by its
  visibility: the receiver is moved along its geometric normal by 1.5 texels of the map at its distance (normal
  offset, a few millimeters at room scale), then compared in hardware (`sampler2DShadow`, `GL_LEQUAL`, bilinear: each
  fetch a 2 x 2 percentage-closer filter) in a 3 x 3 tent of fetches one texel apart, mixed towards lit by 1 - the
  shadow strength; outside the map, behind a spot light or beyond the far plane nothing is shadowed. The overlay edits
  `ShadowSettings` live (on/off, both biases, the normal offset). Every view then draws a depth pre-pass of its opaque
  and alpha-masked batches into the HDR target (color writes off), and the lit pass shades each pixel once against it
  (`GL_LEQUAL`, no depth writes; `gl_Position` is `invariant` in `pbr.vert`, which every mesh program shares, so the
  depths match). `gl::SsaoPass` estimates the ambient occlusion from that depth (`common/ssao.glsl`): at half the
  resolution (`ssao/ssao.frag`, `ViewTargets::ambientOcclusionRaw()`, `GL_RG16F`) it reconstructs the view-space
  position and the normal of each surface from the depth (the normal from the neighbors that continue the surface's
  plane, judged by extrapolating the reciprocal distance, which is linear in screen space on a plane; at the image's
  borders only the side inside it; slivers one pixel thin get no estimate), and tests 16 samples of a cosine-weighted
  hemisphere kernel (`gl::ssaoKernel`, scaled by the radius) rotated and scaled per pixel by a 4 x 4 Bayer pattern
  against the scene depth (bilinear: exact on planes), occluders counting fully up to three quarters of the radius away
  in depth and not at all beyond the radius (a lamp pole or a TV's face farther in front of a wall than the radius
  leaves no halo on it); then a depth-aware blur at full resolution (`ssao/ssao_blur.frag`,
  `ViewTargets::ambientOcclusion()`, `GL_R8`) averages the 4 x 4 estimates around each pixel, every rotation and scale
  once (at the borders the window moves inside the image), each weighted by how close its depth is to the depth the
  pixel's plane predicts there (2% tolerance: flat surfaces blur fully at any angle, nothing bleeds across depth edges,
  no halos). `pbr.frag`
  (`common/occlusion.glsl`) multiplies the ambient terms only by it (the diffuse ambient directly, the specular
  ambient through the specular occlusion of Lagarde and de Rousiers), never a light's; blended surfaces ignore it.
  Without ambient occlusion (off in the environment or the overlay, its programs missing, wireframe views, which also
  skip the pre-pass) the target is cleared to 1. At 1920 x 1080 on the reference GPU the test room's shadow map takes
  0.03 ms, its pre-pass 0.04 ms and its ambient occlusion 0.4 ms.
- **Debug lines:** `--debug-draw` adds `gl::DebugDraw` over the post-processed output, depth-tested against the scene
  (its depth is copied from the HDR target first): `scene::DebugLines` (a world grid with its +Y marker drawn first, so
  the floor axes show where they overlap it; every object instance's bounds as an oriented box; the axes of every
  socket: shelf slots, the CRT screen surface, model anchors; the overlay's Rendering panel toggles each kind,
  `app::composeDebugLines`, and the program and buffer are created when lines are first shown) uploaded when the
  selection changes and drawn as `GL_LINES` with `debug/debug_lines.{vert,frag}` (linear colors encoded by the sRGB
  output, neither lit, tonemapped nor bloomed; depth-tested without depth writes, each vertex pulled towards the eye by
  0.2% of its distance so that lines lying on surfaces do not flicker). Socket axes are markers: a second draw shows
  their parts hidden by surfaces, dimmed (the complementary depth test), so a socket under a surface, such as the CRT's
  screen plane under its bulged glass, still shows its origin.
- **Materials:** one GPU material per material of the scene, and one more per object, role and pattern size with a scene
  texture, which becomes that instance's base color texture (cover patterns on cartridge labels and posters, color bars
  on the CRT screen, the lettering of a sign; patterns are generated at 2560 pixels per meter of their surface's longer
  side, 64 to 512 pixels, `assets::patternLongSideFor`, and at the aspect ratio of their surface as drawn,
  `geometry::uvMetersPerUnit` with the instance transform, so a poster scaled unevenly is not stretched). Pattern
  textures are single pictures, sampled clamped to their edges; procedural surface textures a scene names
  (`{"procedural": "<surface>/<map>"}`, without a size in their key), textures named by materials and model images
  repeat. `MaterialRegistry` keeps every `MaterialUniforms` block in one uniform buffer (offsets aligned to
  `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`, dynamic storage: the overlay's live edits, `setFactors`, rewrite one block with
  `gl::withFactors`) and binds per material the block range, five textures (base color and emissive sRGB;
  metallic-roughness, normal and occlusion linear; neutral `TextureDefaults` in empty slots) and sampler objects
  (trilinear, 8x anisotropy clamped to the device, the wrap mode of the texture). Material flags tell the shaders which
  slots hold a texture and the alpha mode; fragment stages sample every texture before an alpha-mask discard
  (derivatives are undefined after it). Shader families (`scene::ShaderKind`): `Pbr` (`pbr/pbr.vert` + `pbr/pbr.frag`)
  and `Unlit` (`pbr/pbr.vert` + `unlit/unlit_emissive.frag`: base color * texture * emissive strength; neither shadowed
  nor occluded; a screen with a `crt` block is shown as a picture tube, below). Both write linear scene radiance,
  neither exposed nor tonemapped (the post pass does that once for the whole view), and the part of it they emit
  (`common/scene_output.glsl`, `gl::SceneOutput`: the emissive term of a lit material, all of an unlit one's light) into
  the two attachments of the HDR target.
- **Textures:** `TextureCache` uploads each source once (asset files, `pattern:<name>:<title>:<seed>@WxH` patterns,
  `procedural:<surface>/<map>` surface maps, decoded model images; keys include the color space and the wrap mode) as
  immutable `GL_RGBA8` / `GL_SRGB8_ALPHA8` textures with full mip chains. The first request of a procedural surface
  generates all of its maps (`assets::makeProceduralSurface`, logged with its time) and uploads each in its color space.
  A missing or broken source is logged once and drawn as the checker (style §21). The test room's 125 textures take 105
  MiB, 80 of them the maps of its eight procedural surfaces.
- **Procedural surfaces (M0 step 12, style §3.2-3.3):** `rv/assets/surface_textures.hpp` generates the PBR maps of the
  shared materials at startup, so Retroverse needs no texture download and every design is original: `oak_planks` (14.3
  cm planks with staggered joints, a tint and a slight cup per plank, dark gaps covered in proportion to the share of
  the pixel they take, worn traffic areas; 2 x 2 m per repeat), `walnut`, `varnished_wood` and `oak` (boards glued edge
  to edge, each a flat slice through a log: growth rings around a pith at its own depth and offset, straight far from it
  and closing into arches near it, drifting along the board, with pores, streaks and a tone per board; 1 x 1 m per
  repeat, the grain along u), `painted_plaster`, `ornate_rug` (a medallion rug in the style §3 palette, a picture for
  the rug prefab's 2.4 x 1.6 m, without normal map), `brushed_metal` and `aged_plastic`. Each is a pure function of (u,
  v) built from integer-hash value noise on lattices that repeat over the image (`src/assets/src/procedural_noise.*`),
  rendered row by row in parallel (`std::execution::par`) into an sRGB base color, a linear metallic-roughness map (glTF
  channels) and a tangent-space normal map from central differences of its heights (+Y towards decreasing v, ADR-0002).
  "Full" surfaces carry albedo and roughness (their materials keep white and 1 as factors); the "detail" surface
  `aged_plastic` modulates its material's own factors around 1. Material presets name a map as
  `procedural:<surface>/<map>` in a texture slot (`MAT_Floor_OakPlanks`, `MAT_Wood_*`, `MAT_Wall_Painted`,
  `MAT_Carpet_Ornate`, `MAT_Metal_Brushed`, `MAT_Plastic_*`). For the wood to run along each part and keep its scale,
  the furniture prefabs and the room shell map their boxes in meters along each face's longer side
  (`geometry::BoxUv::GrainMeters`, `kWoodMetersPerTile` 1 m; the room's floor and walls `metersPerTile` 2 m). The
  surfaces take about 0.6 s at startup in Release (3 s in Debug); tests check that each one is deterministic, tiles
  without a seam, averages its palette color and has unit normals facing out.
- **CRT screens (M0 step 12, style §15.2, `CRT_Subtle` of §25):** an unlit screen material with a `crt` block
  (`assets::CrtEffect`: `lineCount` 240, `scanlineStrength`, `maskTriadCount`, `maskStrength`, `curvature`, `vignette`,
  `glassReflection`; `MAT_Screen_CRT`) sets `material_flags::kCrtScreen` and fills the crt fields of its
  `MaterialUniforms`. `unlit_emissive.frag` (`common/crt.glsl`) then bends the picture's uvs with a barrel curvature, so
  its corners round off into the tube's dark border (antialiased over a pixel), multiplies it by scanlines and an RGB
  aperture grille whose means are 1 (the brightness is kept) and which fade out as their period shrinks from 4 to 2
  pixels (no moiré at a distance; at 1920 x 1080 the test room's scanlines appear within about 1 m of the screen, seen
  head-on, and reach full strength within 0.55 m), and by a vignette towards the picture's edges; all of it is emitted
  (it blooms). The glass in front reflects the scene's lights and the ambient like a smooth dielectric (4% at normal
  incidence, roughness 0.2), added to the radiance but not to the emission, through the lit shader's own light
  evaluation (`punctualLightRadiance` of `common/lights.glsl`). The test room's `crt_glow` spot, in front of the screen,
  stands in for the tube's light on the cabinet and the console.
- **Lighting:** `gl::packLights` turns the scene lights (contract of `scene::Light`) and environment into
  `LightsUniforms`: up to 16 point/spot lights (inverse square with a smooth window reaching 0 at the range, smoothstep
  spot cones; the test room uses 14) and one directional light, and the hemispherical sky/ground ambient. `pbr.frag`:
  GGX NDF, height-correlated Smith visibility, Schlick Fresnel, Lambert diffuse, metallic workflow, normal maps in the
  glTF tangent space (ADR-0002), occlusion, emissive, the key light's shadow; the ambient lights the diffuse lobe from
  the hemisphere around the normal and the specular lobe from the hemisphere around the reflection (analytic environment
  BRDF), so that metals reflect the room, both darkened by the view's ambient occlusion. The environment's clear color
  is background radiance, cleared into the HDR target and post-processed like the surfaces, so the void beyond a doorway
  sits on the scale of the lit room.
- **Post-processing (M0 step 10, style §4.3, spec §14.2):** `gl::PostProcessor` takes the HDR scene of a
  `gl::ViewTargets` to its sRGB output with the `gl::PostSettings` of the view (`gl::postSettingsFor` the scene's
  environment: exposure, `bloomStrength`, `bloomRadius`, `vignette`, and one 8-bit step of dither noise), in passes
  that draw one fullscreen triangle each without a vertex buffer (`post/fullscreen.vert`, an empty vertex array, the
  sources on texture units 5 to 7, `gl::PostTextureUnit`, with a bilinear sampler clamped to the edges; parameters in
  `PostUniforms` at binding 3, `common/post.glsl`). Bloom (Jimenez 2014), localized (style §4.3), when its strength is
  above 0: the light of the scene that blooms (`bloomSource`: all the light surfaces emit, from the HDR target's
  emission attachment, so screens, lamp shades and neon glow however bright they are, and of the reflected light only
  what is above a soft threshold at exposed brightness 4, knee 1, `gl::kBloomThreshold`: the mirror-like highlights
  of the lights; lit paper, wood, plastic and walls, shown at most near sRGB 240 below exposed 3, never bloom) is
  downsampled into 6 levels of half, a quarter, ... of the view with the 13-tap filter, the first level with the
  Karis average against fireflies (`RV_BLOOM_KARIS`: every tap weighted by 1 / (1 + its exposed luminance) times its
  share of the filter, so a lone very bright pixel adds about what a pixel of exposed luminance 1 adds, whatever its
  value and wherever it falls under the pixel grid, and its glow does not pulse as the camera moves; the edges of
  bright areas lose a little of their glow); then, from the smallest level up, the 3 x 3 tent upsampling of each level
  is mixed into the next larger one by the fixed-function blend (constant alpha `bloomScatter(bloomRadius)`). Filters
  and blend keep the energy of the image. The post pass (`post/post.frag`) replaces the share `bloomStrength` of the
  light that blooms by its bloom (energy is moved, never added: 0.04 to 0.08 is subtle), applies the vignette, the
  exposure and the ACES fitted curve (`common/tonemap.glsl`, mirrored on the CPU by `gl::tonemap`), and dithers the
  result with interleaved gradient noise in its sRGB encoding against banding. Views are then anti-aliased
  (`PostSettings::antialiasing`, on for every scene view; the overlay's FXAA toggle): the post pass writes into
  `ViewTargets::tonemapped()` instead of the output, with the perceptual luma of each pixel (the sRGB encoding of its
  luminance) in the alpha, and the FXAA pass (`post/fxaa.frag`, after Lottes 2011) takes it to the output: a pixel
  whose luma differs little from its four neighbors is copied exactly; on an edge it finds the edge's direction, walks
  along it both ways to the ends of the stair-step (up to 23.5 pixels) and blends the pixel towards its neighbor across
  the edge by its distance from the nearer end, or by its contrast with the 3 x 3 average for features thinner than a
  pixel (strength 0.5, which keeps small print legible). A program that never built falls back: without the bloom
  programs the view is composited without bloom, without the FXAA program the post pass writes the output directly,
  without the post program the HDR scene is copied to the output as it is (clamped, neither exposed nor tonemapped).
  At 1920x1080 the post-processing takes about 0.45 ms on the reference GPU, FXAA 0.1 to 0.15 ms of it.
- **Frames:** every view is rendered through a `gl::ViewTargets`, the targets of one output, all of its size: the HDR
  scene target (two `GL_RGBA16F` color attachments, the radiance and its emitted part, `gl::SceneOutput`, + a
  depth24/stencil8 texture, which the ambient occlusion reads, `Framebuffer::bindDepthTexture`), the half-resolution
  ambient occlusion estimate (`GL_RG16F`, `gl::ssaoTargetSize`) and the ambient occlusion the lit pass reads (`GL_R8`),
  the 6 bloom levels (`GL_RGBA16F`, `gl::bloomLevelSizes`), the tonemapped image the FXAA pass reads (`GL_SRGB8_ALPHA8`)
  and the output (`GL_SRGB8_ALPHA8` color + depth24/stencil8), which the post or the FXAA pass writes with
  `GL_FRAMEBUFFER_SRGB` enabled, so shaders output linear values and the targets store sRGB. The window and the
  screenshots each keep their own `ViewTargets`, reallocated only when their size changes (a complete new set first, so
  a failure keeps the old one). The window frame's output is copied to the default framebuffer with a raw blit
  (`GL_FRAMEBUFFER_SRGB` disabled), so the window shows exactly the stored bytes whatever its own encoding. The
  developer overlay is drawn last, into the default framebuffer, with `GL_FRAMEBUFFER_SRGB` disabled (Dear ImGui's
  colors are sRGB already); the backend changes GL state behind the `RenderStateCache`, which is invalidated afterwards.
  A frame whose window targets cannot be allocated clears the window to black (`Framebuffer::clearDefault`) and still
  draws the overlay over it, so its log shows why. `RenderView::wireframe` draws the scene pass with
  `glPolygonMode(GL_LINE)`, without depth pre-pass or ambient occlusion, so hidden edges stay visible. The key light's
  `gl::ShadowMap` belongs to the renderer, not to an output: every view draws it again (it depends on the light only).
  Two self-checks run the programs the views use on the GPU. `--self-check srgb` proves the output path: an unlit grey
  quad of the radiance that the CPU mirror of the tonemap shows as linear 0.5 (`gl::greyRadianceForDisplay`), drawn with
  the scene's unlit program into the HDR target of 8x8 `ViewTargets` and taken by the `PostProcessor` to the output,
  must read back as 188 +- 2, once with exposure 1 and bloom, vignette, dithering and anti-aliasing off (the GPU's ACES
  curve against its CPU mirror), once with bloom, dithering and anti-aliasing on (a uniform field, which none may
  change: FXAA copies it exactly). `--self-check bloom` writes known patterns straight into the HDR target of 512x512
  `ViewTargets` (scissored clears) and reads the largest bloom level back as floats (`Framebuffer::readLinearPixels`): a
  lit square just below the highlight knee must not bloom at all, an emitter square must keep its energy within 5%
  (98.6% on the reference GPU), and a single pixel 100 and 1000 times brighter than white, at the four places of a 2x2
  pixel block, must bloom alike within 20% (2.6%).
- **Screenshots:** `--screenshot` (last frame of a frame-limited run) and F12 render the same view through a second
  `ViewTargets` at `--screenshot-size` (default 1920x1080, independent of the window; the bloom levels scale with it,
  so the glow covers the same share of the image), read the output back through an `RGBA8` texture view
  (raw sRGB bytes on every driver), flip the rows to top-first (ADR-0002) and write an RGB PNG (the window is opaque, so
  the alpha channel is dropped). They show the scene alone; with `--screenshot-include-ui` the open overlay is drawn
  over them too, from the frame's draw data scaled uniformly from the window to fit the screenshot and centered
  (`devui::fitOverlayCapture`: another aspect than the window's leaves bands of scene beside it instead of stretching
  the panels; that frame's glyphs are rasterized for the screenshot's density, so the text stays sharp).
- **Uniforms:** binding points are listed once in `rv/render/uniform_blocks.hpp` (0 frame, 1 lights, 2 material, 3 post,
  4 shadow, 5 ambient occlusion, 6+ reserved) with the std140 mirrors `FrameUniforms` (view, projection, viewProjection,
  cameraPosition, time: `common/common.glsl`), `LightsUniforms` (16 point/spot lights: `common/lights.glsl`),
  `MaterialUniforms` (with the CRT parameters: `common/material.glsl`), `PostUniforms` (exposure, bloom mix, threshold
  and knee, vignette, dither amplitude: `common/post.glsl`), `ShadowUniforms` (the shadow map's transform, the light's
  index and position, the normal offset, the strength, the texel size: `common/shadow.glsl`) and `SsaoUniforms` (the 16
  kernel samples, radius, intensity, bias, the blur's depth tolerance: `common/ssao.glsl`). Texture units: 0..4 the
  material, 5..7 the post-processing (`gl::PostTextureUnit`), 8 the shadow map and 9 the ambient occlusion
  (`gl::SceneTextureUnit`, read by the lit shader), 10 the scene depth and 11 the raw occlusion (`gl::SsaoTextureUnit`).
  Model matrices are per-instance vertex attributes (locations 4..7), not uniforms.
- **Shaders:** `gl::ShaderLibrary` assembles GLSL from `<asset root>/shaders` (`#include`, defines, `#line`) and maps
  compiler messages to `<file>:<line>`. The app's programs: `pbr`, `unlit_emissive`, `depth` and `depth_masked`
  (`RV_ALPHA_TEST`) on `pbr/pbr.vert`; `ssao`, `ssao_blur` and the post-processing (`bloom_downsample_karis`,
  `bloom_downsample`, `bloom_upsample`, `post`, `fxaa`) on `post/fullscreen.vert`; `debug_lines` when debug lines are
  first shown. F5 rebuilds every program; developer builds (`RV_DEV_ASSET_ROOT`) also poll the files of every program,
  includes included, every 500 ms (after a failed assembly: the files it read and the one it could not read, so creating
  a missing include rebuilds the program). A failed rebuild keeps the previous program.

## Input path (M0)

One pass per frame on the main thread, in `RetroverseApp` (`src/app/src/app_input.*`, `free_roam.*`):

- **Events:** the host consumes the Alt+Enter press (`platform::isFullscreenToggle`, borderless fullscreen), so it never
  reaches the bindings (Enter is Confirm). In developer builds the overlay sees every other event first
  (`devui::routeOverlayEvent`) and keeps what it uses: mouse events over a panel or while a widget is dragged, key
  presses while a text field or a dragged value has the keyboard (so Esc there never quits), text input; releases always
  reach the game, so nothing stays held, and while first-person look holds the mouse every mouse event is the game's.
  Then `platform::MouseCapture` sees the event: a click into the focused window captures the mouse (SDL relative mode)
  and is consumed, focus loss releases it. Interactive runs also capture at startup once the window has focus;
  frame-limited runs never do, so unattended runs neither take the user's mouse nor pick up its motion. Every other
  event goes to `platform::SdlInputCollector`: keys by scancode (layout-independent WASD), mouse buttons, relative
  motion while captured, wheel, gamepad buttons and axes (SDL ranges to [-1, 1], +Y up, triggers [0, 1]) of the pads
  `platform::GamepadManager` opened on hot-plug; the most recently used pad is reported. Focus loss releases everything
  held. Joystick events never arrive in the background (`SdlPlatform` hint).
- **Actions:** the collector's `RawInputFrame` is resolved by `input::InputSystem` with the bindings of
  `<asset root>/config/default_bindings.json` (`ActionMap::defaults()` with a warning when unusable), overridden per
  action by `<user data>/config/bindings.json` (`ActionMap::applyOverrides`). `--input-script` then replaces the
  actions of the frames it covers (`input::InputScriptPlayer`, edges kept consistent).
- **Use:** Back leaves one level (`app::backAction`: ignored while the overlay has the keyboard, else closes the
  overlay's topmost popup, else releases the captured mouse, else closes the overlay, else quits), ToggleDevUI shows or
  hides the overlay (`devui::CaptureHandover`: opening it frees a captured mouse, closing it captures the mouse again),
  ReloadShaders and Screenshot are handled by the app. `FreeRoam` drives `scene::FirstPersonController` with the actions
  and the frame delta (`--fixed-dt` replaces it for reproducible runs; the sum of the deltas is also the shader time):
  the walker starts at the scene camera (feet = camera position - eye height) or at `--spawn` (its feet, and its yaw and
  pitch, else the scene camera's), and collides with the collision world of the `SceneInstance`
  (`scene::buildCollisionWorld()` of the scene content), built from the colliders each object declares (for `room_shell`
  one box per wall part around the doorway and a stop in the doorway's outer face, so the walker can step into the
  opening but not leave the room; the bounds for furniture; the `ColliderProxy` nodes of glTF models, else their bounds;
  none for `rug`; objects with `"collides": false` have none). Objects turned about +Y collide with their exact
  footprints, tilted ones with their world boxes. Frame-limited runs log the final feet position, yaw and pitch, the
  simulated time, the distance walked and the nearest collider with its clearance.

## Developer overlay (M0 step 9)

`devui::DevUi` (`rv/devui/dev_ui.hpp`, developer builds) owns the Dear ImGui context (1.92, docking branch) with the
SDL3 platform backend (gamepads left to the game; the SDL hints the backend sets for itself are restored, so the game's
input behaves the same) and the OpenGL3 renderer backend (GLSL `#version 450`, its own GL loader). The app creates it
after the scene and adds the panels (`rv/devui/panels.hpp`), thin views that hold references to the state they show and
edit in place:

- Stats: `devui::FrameTimeHistory` (the last 240 wall-clock frame times, recorded every frame), the renderer's
  `gl::FrameStats` (the GPU times of the shadow map, the depth pre-pass, the ambient occlusion, the scene pass and the
  post-processing first, then the counts, those of the shadow map and the depth pre-pass included), mesh, material and
  texture counts and bytes from the registries and the `TextureCache`.
- Camera: the walker (`scene::FirstPersonController` of `FreeRoam`: feet, yaw and pitch teleport it; speeds), the view
  camera's lens (FOV), the input settings (mouse sensitivity, invert Y), the app's collision flag (noclip), and a reset
  to the scene camera.
- Lights: `SceneInstance::sceneLights()` and `environment()` (background radiance and ambient), edited in place and
  repacked (`applyLighting`: `gl::packLights`); colors are edited as sRGB and stored linear. It names the key light
  (`shadowLight()`, or the warning of `shadowNote()`); each light's "Casts shadows" flag and shadow strength are edited
  like the rest, so ticking another light first makes it the key light.
- Materials: `gl::MaterialRegistry` (`setFactors`: base color, metallic, roughness, emissive strength; never saved; a
  revert goes back to `GpuMaterial::source`).
- Rendering: the app's debug line selection, wireframe and FXAA flags, the window's vsync, the app's
  `gl::ShadowSettings` (the key light's shadow on or off, slope and constant depth bias, normal offset), the ambient
  occlusion and the post-processing of `SceneInstance::environment()` (on or off, radius, intensity; exposure, bloom
  strength and radius, vignette; read by every view, so edits show at once), `ShaderLibrary` (reload, build state and
  last error of every program).
- Log: `devui::LogView`, a ring of the recent log lines (time, level, message) refreshed only when
  `core::loggedMessageCount()` changes.
- Scene: the objects of the scene description (id, then kind and source on a line of their own, so a narrow column
  shows them whole); "go to" places the walker with `scene::frontViewpoint` in front of the object's world bounds
  (all its instances), facing its front (+Z) and framing it in the area the panels leave see-through
  (`DevUi::sceneViewArea`, the dock space's central node, as a `scene::ViewArea` in normalized device coordinates),
  or at the center of an object larger than 4 m along both x and z (a room); long furniture is looked at.

Each frame the app records the frame time and, while the overlay is visible, builds its frame in `onUpdate` before
the walker moves, so edits apply to the frame being rendered; `onRender` draws it after the window blit. While hidden
no ImGui frame is built. The default layout (Stats and Camera on the left, Lights/Materials/Scene tabs and Rendering on
the right, Log under the scene, a see-through central node) is built with the dock builder when no saved layout
exists; its side columns are at least 18 and 22 font sizes wide (at most 30% and 40% of the width they split), so
the panels stay readable as fonts and sizes follow `SDL_GetWindowDisplayScale`. Interactive runs load and save the
layout in `<user data>/config/imgui.ini`; frame-limited runs always use the default layout and save nothing, so their
screenshots never depend on earlier runs. Panels reserve exactly their widest label beside the widgets
(`widgets::pushLabelWidth`) and put vectors of three values on a line of their own (`widgets::labelAbove`).
Keyboard navigation stays off (it would take the game's keys), so Dear ImGui never closes a popup on Escape: Back
does (`DevUi::closePopup`, one level at a time), and hiding the overlay closes every popup. A frame-limited run that
asks for the overlay (`app::overlayRequired`) fails with exit code 3 when it cannot start. Release builds compile
neither `rv_devui` nor ImGui.

## Threading (M0)

Single main thread: event pump, input, simulation, OpenGL calls, presentation. Asset loading is synchronous at
startup. Worker threads (catalog, media decoding, video) arrive in later iterations and must hand GPU work to the
main thread through a command queue (spec §15.3). `rv::core::assertMainThread` guards main-thread-only code.

## Data conventions

See `docs/decisions/ADR-0002-coordinates-units-uv.md`: meters, +Y up, right-handed, +Z front, CCW front faces,
glTF UV convention, linear-space shading with sRGB textures and framebuffer. Tangent space as in glTF:
`bitangent = cross(normal, tangent.xyz) * tangent.w` points towards decreasing v (the top of the image), the "+Y up"
of normal maps; `geometry::computeTangents` and the procedural builders generate tangents that way (checked with
synthetic normal maps in M0 step 6: `tests/unit/geometry/test_mesh_data.cpp`).

## Runtime directories

- Asset root: `--asset-root`, `RV_ASSET_ROOT`, `<source>/assets` (developer builds), `<exe>/assets`.
- User data: `--data-dir`, `<exe>/RetroverseData` with `--portable`, `%LOCALAPPDATA%\Retroverse` otherwise.
  Logs go to `<user data>/logs/retroverse.log`, F12 screenshots to `<user data>/screenshots`, the developer
  overlay's layout to `<user data>/config/imgui.ini`.
