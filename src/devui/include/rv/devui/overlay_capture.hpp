#pragma once

// GPU-free (target rv_devui_model): no SDL, OpenGL or Dear ImGui header is included.

#include <glm/vec2.hpp>

namespace rv::devui {

/// Where the developer overlay, laid out for the window, lands in a screenshot (--screenshot-include-ui).
struct CaptureFit {
    /// Screenshot pixels per window unit, the same on both axes.
    float scale = 1.0f;
    /// Screenshot pixels from the top-left corner of the screenshot to the top-left corner of the overlay, whole
    /// pixels.
    glm::vec2 offset{0.0f};
};

/// Fits the overlay of a window of `windowSize` units into a screenshot of `targetSize` pixels: scaled uniformly by
/// the smaller of the two ratios, so that panels and glyphs keep their proportions whatever the screenshot's aspect,
/// and centered; when the aspects differ, the bands on either side show the scene alone. A window or a screenshot
/// without area gives scale 1 and no offset.
[[nodiscard]] CaptureFit fitOverlayCapture(glm::vec2 windowSize, glm::ivec2 targetSize) noexcept;

} // namespace rv::devui
