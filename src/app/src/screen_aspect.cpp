#include <rv/app/screen_aspect.hpp>

#include <cmath>

namespace rv::app {

glm::vec4 pictureRect(glm::vec2 picture, glm::vec2 screen) noexcept {
    const auto usable = [](glm::vec2 size) {
        return std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0f && size.y > 0.0f;
    };
    if (!usable(picture) || !usable(screen)) {
        return kWholeScreen;
    }
    // The picture's aspect over the screen's: above 1 the picture is wider than its screen.
    const double ratio = (static_cast<double>(picture.x) / static_cast<double>(picture.y)) /
                         (static_cast<double>(screen.x) / static_cast<double>(screen.y));
    if (std::abs(ratio - 1.0) <= kPictureAspectTolerance) {
        return kWholeScreen;
    }
    if (ratio > 1.0) {
        const double height = 1.0 / ratio; // letterbox: the whole width, bars above and below
        return {0.0f, static_cast<float>(0.5 * (1.0 - height)), 1.0f, static_cast<float>(height)};
    }
    const double width = ratio; // pillarbox: the whole height, bars at the sides
    return {static_cast<float>(0.5 * (1.0 - width)), 0.0f, static_cast<float>(width), 1.0f};
}

bool isWholeScreen(const glm::vec4& rect) noexcept {
    return rect.x == kWholeScreen.x && rect.y == kWholeScreen.y && rect.z == kWholeScreen.z &&
           rect.w == kWholeScreen.w;
}

} // namespace rv::app
