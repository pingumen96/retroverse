#include <rv/app/screen_aspect.hpp>

#include <cmath>

namespace rv::app {

namespace {

bool usable(glm::vec2 size) noexcept {
    return std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0f && size.y > 0.0f;
}

constexpr double kFourByThree = 4.0 / 3.0;

} // namespace

double displayAspect(glm::vec2 picture) noexcept {
    if (!usable(picture)) {
        return 0.0;
    }
    const double aspect = static_cast<double>(picture.x) / static_cast<double>(picture.y);
    if (picture.y <= kLowResMaxRows && std::abs(aspect / kFourByThree - 1.0) <= kLowResAspectTolerance) {
        return kFourByThree; // a console's frame its television showed at 4:3
    }
    return aspect;
}

glm::vec4 pictureRect(glm::vec2 picture, glm::vec2 screen) noexcept {
    if (!usable(picture) || !usable(screen)) {
        return kWholeScreen;
    }
    // The picture's aspect as shown over the screen's: above 1 the picture is wider than its screen.
    const double ratio = displayAspect(picture) / (static_cast<double>(screen.x) / static_cast<double>(screen.y));
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

glm::vec4 croppedPictureRect(glm::vec2 picture, const glm::vec4& crop, glm::vec2 screen) noexcept {
    if (!(crop.z > 0.0f) || !(crop.w > 0.0f) || !std::isfinite(crop.x) || !std::isfinite(crop.y) ||
        isWholeScreen(crop)) {
        return pictureRect(picture, screen);
    }
    const glm::vec4 shown = pictureRect(picture * glm::vec2(crop.z, crop.w), screen);
    const glm::vec2 scale(shown.z / crop.z, shown.w / crop.w);
    return {shown.x - crop.x * scale.x, shown.y - crop.y * scale.y, scale.x, scale.y};
}

bool isWholeScreen(const glm::vec4& rect) noexcept {
    return rect.x == kWholeScreen.x && rect.y == kWholeScreen.y && rect.z == kWholeScreen.z &&
           rect.w == kWholeScreen.w;
}

} // namespace rv::app
