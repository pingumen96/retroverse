#include <rv/devui/overlay_capture.hpp>

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>

namespace rv::devui {

CaptureFit fitOverlayCapture(glm::vec2 windowSize, glm::ivec2 targetSize) noexcept {
    const glm::vec2 target(targetSize);
    // Written so that NaN sizes fail the test too.
    if (!(windowSize.x > 0.0f && windowSize.y > 0.0f && target.x > 0.0f && target.y > 0.0f) ||
        !std::isfinite(windowSize.x) || !std::isfinite(windowSize.y)) {
        return {};
    }
    CaptureFit fit;
    fit.scale = std::min(target.x / windowSize.x, target.y / windowSize.y);
    // The slack of the fitted axis is 0 up to rounding: never let that push the overlay off the target.
    const glm::vec2 slack = glm::max(target - windowSize * fit.scale, glm::vec2(0.0f));
    fit.offset = glm::round(slack * 0.5f);
    return fit;
}

} // namespace rv::devui
