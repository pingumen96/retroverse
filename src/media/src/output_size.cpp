#include <rv/media/output_size.hpp>

#include <algorithm>
#include <cmath>

namespace rv::media {

FrameSize scaledFrameSize(FrameSize coded, double sampleAspect, int maxHeight) noexcept {
    if (!coded.valid()) {
        return {};
    }
    double width = static_cast<double>(coded.width);
    double height = static_cast<double>(coded.height);
    // Non-square pixels: shrink the side they stretch, so that no side grows.
    if (std::isfinite(sampleAspect) && sampleAspect > 0.0 && std::abs(sampleAspect - 1.0) > 1e-6) {
        if (sampleAspect < 1.0) {
            width *= sampleAspect;
        } else {
            height /= sampleAspect;
        }
    }
    if (maxHeight > 0 && height > static_cast<double>(maxHeight)) {
        width *= static_cast<double>(maxHeight) / height;
        height = static_cast<double>(maxHeight);
    }
    const auto side = [](double value) {
        return static_cast<int>(std::max(1.0, std::round(value)));
    };
    return FrameSize{side(width), side(height)};
}

} // namespace rv::media
