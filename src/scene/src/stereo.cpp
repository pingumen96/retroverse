#include <rv/scene/stereo.hpp>

#include <algorithm>
#include <cmath>

#include <rv/core/math.hpp>

namespace rv::scene {

EyeFov stereoPreviewLeftField() noexcept {
    return EyeFov{core::degreesToRadians(-52.0f), core::degreesToRadians(45.0f), core::degreesToRadians(48.0f),
                  core::degreesToRadians(-50.0f)};
}

EyeFov mirroredField(const EyeFov& left) noexcept {
    return EyeFov{-left.right, -left.left, left.up, left.down};
}

StereoEyes stereoEyesFor(const Camera& head, float ipdMeters, const EyeFov& leftField) noexcept {
    const glm::quat orientation = head.orientation();
    const glm::vec3 halfOffset = head.right() * (0.5f * ipdMeters);
    StereoEyes eyes{head, head};
    eyes.left.setEye(head.position() - halfOffset, orientation, leftField);
    eyes.right.setEye(head.position() + halfOffset, orientation, mirroredField(leftField));
    return eyes;
}

StereoLayout stereoLayout(glm::ivec2 outputSize, const EyeFov& field) noexcept {
    const int half = outputSize.x / 2;
    if (half < 1 || outputSize.y < 1) {
        return {};
    }
    // Width over height of the field's image plane at distance 1.
    const float aspect = (std::tan(field.right) - std::tan(field.left)) / (std::tan(field.up) - std::tan(field.down));
    glm::ivec2 size(half, static_cast<int>(std::lround(static_cast<float>(half) / aspect)));
    if (size.y > outputSize.y) {
        size = glm::ivec2(std::max(1, static_cast<int>(std::lround(static_cast<float>(outputSize.y) * aspect))),
                          outputSize.y);
    }
    size.y = std::max(size.y, 1);
    const glm::ivec2 margin((half - size.x) / 2, (outputSize.y - size.y) / 2);
    return StereoLayout{size, margin, glm::ivec2(half + margin.x, margin.y)};
}

} // namespace rv::scene
