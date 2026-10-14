# Backlog

Ideas and deferred work that is out of scope for the current iteration. Pull items into the roadmap deliberately.

## Deferred beyond v1 (decided)

- ScreenScraper live scraping: requires developer credentials from the ScreenScraper project (spec §10.2).
- Writing back to `gamelist.xml` (spec §4.6): read-only until a round-trip test suite exists.
- Legacy EmulationStation, RetroBat and Batocera library adapters.
- VR, Linux build, hardware video decoding (D3D11VA), OpenGL 3.3 fallback.
- Alternative world style presets beyond `RetroMuseumCozy` (style §18).

## Open items

- Validate normal-map tangent sign conventions with the Khronos `NormalTangentMirrorTest` sample (ADR-0002).
- Decide whether spdlog should be built with wide-character file names for non-ASCII log paths.
- Consider `FlushFileBuffers` for atomic writes of critical state (world plans) in I2/I3.
- Anti-aliasing before the Gate A golden screenshot (style §2.1): the M0 frames stair-step every slanted edge. Either
  an MSAA sample count in `gl::FramebufferDesc` with a resolve before present and readback, or a post-process pass.
- A close-up reference view of the `m0_testroom` hero corner (camera 2-4 m from the north wall, framing the shelf and
  the TV) for the step 6 texture and lighting screenshots: from the doorway camera a cartridge is about 20x12 px.
