#include <rv/xr/swapchain_format.hpp>

#include <algorithm>

#include <fmt/format.h>

namespace rv::xr {

std::optional<ColorFormatChoice> chooseColorFormat(std::span<const std::int64_t> offered) noexcept {
    const auto has = [&](std::int64_t format) {
        return std::find(offered.begin(), offered.end(), format) != offered.end();
    };
    if (has(kGlSrgb8Alpha8)) {
        return ColorFormatChoice{kGlSrgb8Alpha8, true};
    }
    if (has(kGlRgba8)) {
        return ColorFormatChoice{kGlRgba8, false};
    }
    return std::nullopt;
}

std::string glFormatName(std::int64_t format) {
    switch (format) {
    case kGlSrgb8Alpha8:
        return "GL_SRGB8_ALPHA8";
    case kGlRgba8:
        return "GL_RGBA8";
    case kGlRgb10A2:
        return "GL_RGB10_A2";
    case kGlRgba16f:
        return "GL_RGBA16F";
    default:
        return fmt::format("0x{:X}", format);
    }
}

} // namespace rv::xr
