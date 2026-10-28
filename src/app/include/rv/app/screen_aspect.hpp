#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// The picture's aspect on a screen (I5 step 4, ADR-0016 §7, style §15.2): a clip fills the rectangle of its screen that
// keeps its own aspect, letterboxed (a wider clip: the screen's whole width, black above and below) or pillarboxed (a
// narrower one: the whole height, black at the sides) in the tube's black, never stretched. The screen service sets the
// rectangle in the screen material's MaterialUniforms (gl::MaterialRegistry::setPictureRect) from the clip's frame size
// (the decoder's output, whose pixels are square: the stream's sample aspect applied) and the screen's size. A 4:3 clip
// on a 4:3 screen keeps the whole screen, so that its pixels are exactly what they were before.

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace rv::app {

/// A picture whose aspect is within this share of its screen's fills the whole screen: an encoder's crop (640 x 478,
/// 584 x 446 for 4:3) shows no hairline bars. Beyond it the picture keeps its own aspect.
inline constexpr double kPictureAspectTolerance = 0.02;

/// The whole screen as a picture rectangle: corner (0, 0), size (1, 1).
inline constexpr glm::vec4 kWholeScreen{0.0f, 0.0f, 1.0f, 1.0f};

/// The rectangle of a screen of `screen` (its width and height, in any unit) that a picture of `picture` (its width and
/// height as shown, pixels) fills, in the screen's texture coordinates (uv (0, 0) top-left): its corner (x, y) and size
/// (z, w), centred. A wider picture spans the whole width (letterbox), a narrower one the whole height (pillarbox);
/// kWholeScreen when the aspects are within kPictureAspectTolerance of each other, or when a size is empty or not
/// finite (nothing known: the screen as it is).
[[nodiscard]] glm::vec4 pictureRect(glm::vec2 picture, glm::vec2 screen) noexcept;

/// `rect` is the whole screen.
[[nodiscard]] bool isWholeScreen(const glm::vec4& rect) noexcept;

} // namespace rv::app
