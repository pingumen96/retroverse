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
- Anti-aliasing before the Gate A golden screenshot (style §2.1): the M0 frames stair-step every slanted edge. Either
  an MSAA sample count in `gl::FramebufferDesc` with a resolve before present and readback, or a post-process pass.
- Localized bloom on emissive surfaces (style §4.3, §15.2): the CRT screen and the lamp shade glow only by
  brightness in M0.
- Bounce light for dark rooms: the hemispheric ambient cannot lift 4%-albedo walls out of the ACES toe, so the
  test room relies on direct light. It uses all 8 point/spot slots of the lights block (two wall washers, a pendant
  fill, the floor lamp's bulb and two shade cones, the magenta accent, the entrance washer), and the upper walls and
  the ceiling far from them still fall to black. Candidates: light probes / IBL, or the fake screen light of style
  §4.3 (a point light near each TV with the slowly updated average screen color); real rooms will also need more
  than 8 lights per view (per-room light lists or clustered shading).
- Texture array (or atlas) for cartridge labels: the 24 labels of the test room share one mesh but each shows its own
  cover, so each has a material instance of its own and is a batch of one (24 of the room's 47 draw calls; the shells
  are one batch of 24). Covers in a texture array indexed per instance (an extra instance attribute or an SSBO next
  to the model matrix) would draw every label in one batch; plan I3 already foresees covers in an LRU texture array.
- glTF sampler state: the loader does not expose the samplers of a model yet, so `gl::GpuModel` samples every model
  image with repeat and trilinear filtering (the glTF default); honor clamp/mirror wrap modes and nearest filters.
- Per-instance normal matrix: `pbr.vert` inverts the instance's 3x3 matrix per vertex; precompute it per instance (or
  restrict instances to rotation + uniform scale) once vertex work matters.
- Instance upload without the staging copy: `scene::BatchPlanner` gathers the plan's matrices into
  `BatchPlan::instanceTransforms` (kept by `gl::SceneRenderer`, so no allocation), which `gl::InstanceBuffer::write`
  then copies into the mapped region: 64 B per instance twice. When instance counts reach the tens of thousands, let the
  planner emit item indices and gather the matrices straight into the region the buffer hands out.
