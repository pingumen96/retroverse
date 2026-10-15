# Backlog

Ideas and deferred work that is out of scope for the current iteration. Pull items into the roadmap deliberately.

## Deferred beyond v1 (decided)

- ScreenScraper live scraping: requires developer credentials from the ScreenScraper project (spec §10.2).
- Writing back to `gamelist.xml` (spec §4.6): read-only until a round-trip test suite exists.
- Legacy EmulationStation, RetroBat and Batocera library adapters.
- VR, Linux build, hardware video decoding (D3D11VA), OpenGL 3.3 fallback.
- Alternative world style presets beyond `RetroMuseumCozy` (style §18).

## Open items

- Validate normal-map tangent sign conventions with the Khronos `NormalTangentMirrorTest` sample (ADR-0002) once
  glTF models are drawn: generated tangents were checked with synthetic normal maps in M0 step 6 (glTF tangent
  space, +Y towards decreasing v); file tangents and mirrored uv islands still need the sample.
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
- Sort alpha-blended draws back to front (the M0 scene view draws them last in scene order).
