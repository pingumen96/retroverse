#pragma once

#include <glm/vec2.hpp>

#include <rv/scene/camera.hpp>

namespace rv::scene {

/// The left eye's field the stereo preview uses: a Quest 3S's over Link, measured in the I10 spike (52 degrees
/// outwards, 45 inwards, 48 up, 50 down). The right eye's mirrors it.
[[nodiscard]] EyeFov stereoPreviewLeftField() noexcept;
/// The distance between the preview's eyes: 64 mm, an average adult's.
inline constexpr float kStereoPreviewIpdMeters = 0.064f;

/// The right eye's field of a left eye's: left and right swapped and mirrored.
[[nodiscard]] EyeFov mirroredField(const EyeFov& left) noexcept;

/// Two eyes of a head.
struct StereoEyes {
    Camera left;
    Camera right;
};

/// The eyes of a head at `head`'s position and orientation (its lens's planes kept): each `ipdMeters` / 2 along the
/// head's right, looking where it looks, the left with `leftField` and the right with its mirror (ADR-0017 §4).
[[nodiscard]] StereoEyes stereoEyesFor(const Camera& head, float ipdMeters, const EyeFov& leftField) noexcept;

/// Where two eyes go in a side-by-side image of `outputSize` pixels: each eye's image takes half the width (the left
/// eye on the left), its height following the field's proportions (the tangents' extents), centered vertically and
/// never taller than the output; a field taller than wide fits the height and centers horizontally in its half.
struct StereoLayout {
    glm::ivec2 eyeSize{0};
    /// Lower left corners, OpenGL's pixel origin.
    glm::ivec2 leftOffset{0};
    glm::ivec2 rightOffset{0};
};

/// The layout of `field` (both eyes' extents are the same) in `outputSize`; an empty layout for an output narrower
/// than 2 pixels or less than 1 high.
[[nodiscard]] StereoLayout stereoLayout(glm::ivec2 outputSize, const EyeFov& field) noexcept;

} // namespace rv::scene
