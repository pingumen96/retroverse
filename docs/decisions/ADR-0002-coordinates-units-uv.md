# ADR-0002: Coordinates, units, winding, UVs and color spaces

- Status: accepted
- Date: 2026-10-14

## Context

Procedural geometry, glTF models exported from Blender and the world generator must agree on scale and
orientation, or prefabs end up rotated, mirrored or a hundred times too large (style §9.1, §6.2).

## Decision

- **Units:** 1 world unit = 1 meter.
- **Axes:** right-handed, +Y up, +X right, +Z towards the viewer. The *front* of a prefab (screen of a CRT, label of
  a cartridge, open side of a shelf) faces +Z in its local space. This is the glTF 2.0 convention, so glTF data is
  used as-is (no axis conversion in the loader). Blender exports with "+Y Up".
- **Pivots:** furniture, consoles and room pieces have their origin on the floor (y = 0) at the center of their
  footprint; small inspectable media (cartridges, cases, discs) are centered on their bounding box.
- **Winding:** counter-clockwise front faces, back-face culling enabled for single-sided materials.
- **Matrices:** glm, column-major, column vectors (`clip = P * V * M * p`), OpenGL clip space with depth in [-1, 1].
- **UVs:** glTF convention: (0, 0) is the top-left corner of the image, v grows downwards. Images are decoded with
  the first row at the top and uploaded without flipping, so texture coordinate t = 0 samples the image's top row.
  Procedural quads assign uv (0, 0) to their top-left corner so pictures appear upright. Screenshots read back with
  `glReadPixels` (bottom row first) are flipped once when written to PNG.
- **Tangents:** `tangent.w` is the bitangent sign; `bitangent = cross(normal, tangent.xyz) * tangent.w` (glTF).
  Normal-map sign conventions are validated against the Khronos `NormalTangentMirrorTest` sample when normal maps
  are first used (open item).
- **Color spaces:** art-direction colors are sRGB hex codes (style §3) converted to linear on load; base color and
  emissive textures are sRGB (`GL_SRGB8_ALPHA8`), data textures (metallic-roughness, normal, occlusion) are linear;
  lighting is computed in linear space and written to an sRGB framebuffer (`GL_FRAMEBUFFER_SRGB`). Tonemapping:
  exposure + ACES fitted curve.
- **Camera defaults:** eye height 1.65 m, horizontal FOV 75 degrees (70-85 configurable), near 0.05 m, far 100 m.

## Consequences

Assets that do not follow these rules are normalized at import time (style §10), never patched at runtime. The
glTF loader warns about suspicious scales (bounds larger than 20 m or smaller than 5 mm).
