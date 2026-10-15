#include <rv/app/overlay_policy.hpp>

namespace rv::app {

bool overlayRequired(const CommandLineOptions& options) noexcept {
    return options.frameLimit.has_value() && (options.devUiOpen || options.screenshotIncludeUi);
}

std::optional<glm::ivec2> overlayCaptureSize(const CommandLineOptions& options, std::uint64_t frameIndex,
                                             bool screenshotRequested) noexcept {
    const bool lastFrameShot =
        options.screenshot.has_value() && options.frameLimit.has_value() && frameIndex + 1 == *options.frameLimit;
    if (!options.screenshotIncludeUi || (!screenshotRequested && !lastFrameShot)) {
        return std::nullopt;
    }
    return glm::ivec2(options.screenshotWidth, options.screenshotHeight);
}

} // namespace rv::app
