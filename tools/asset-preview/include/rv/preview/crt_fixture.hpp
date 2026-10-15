#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <glm/vec3.hpp>

#include <rv/assets/glb_writer.hpp>

namespace rv::preview {

/// Bounds of the CRT-like fixture: a consumer 4:3 television of style §11's example, 0.71 x 0.61 x 0.56 m.
inline constexpr glm::vec3 kCrtFixtureSize{0.71f, 0.61f, 0.56f};

/// The small CRT-like test model that retroverse-asset-preview --write-fixture writes, so that the tool can make its
/// own sample (original content, built in code). Root > Body, ScreenSurface, Anchor_TV_Stand, Anchor_ScreenLook,
/// ColliderProxy, following the conventions of style §9 and ADR-0002 (meters, +Y up, front +Z, pivot on the floor):
///  - Body: a box of 0.71 x 0.61 x 0.55 m standing on the origin, its back at z = -0.28, in a dark warm plastic with a
///    32 x 32 base color texture (a fine vertical grain, so the material inspector has an image to show);
///  - ScreenSurface: a 4:3 quad of 0.48 x 0.36 m facing +Z 1 cm in front of the body's front (z = 0.28, the bounds'
///    front), its center 0.33 m above the floor, uv (0, 0) at its top-left, so a picture maps upright; dark glass;
///  - Anchor_TV_Stand: the origin, where the set stands; Anchor_ScreenLook: 0.9 m in front of the screen's
///    center, where a viewer looks at it from;
///  - ColliderProxy: the body's box, a collision proxy the loader keeps out of the drawn items.
/// The drawn bounds are kCrtFixtureSize, from (-0.355, 0, -0.28) to (0.355, 0.61, 0.28).
[[nodiscard]] assets::GlbDescription makeCrtFixture();

} // namespace rv::preview
