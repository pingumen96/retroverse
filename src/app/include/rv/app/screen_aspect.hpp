#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// The picture's aspect on a screen (I5 step 4, ADR-0016 §7, style §15.2): a clip fills the rectangle of its screen that
// keeps its own aspect, letterboxed (a wider clip: the screen's whole width, black above and below) or pillarboxed (a
// narrower one: the whole height, black at the sides) in the tube's black, never stretched. The screen service sets the
// rectangle in the screen material's MaterialUniforms (gl::MaterialRegistry::setPictureRect) from the clip's frame size
// (the decoder's output, whose pixels are square: the stream's sample aspect applied) and the screen's size. A 4:3 clip
// on a 4:3 screen keeps the whole screen, so that its pixels are exactly what they were before.
//
// A console's low-resolution frame stored with square pixels (I5 step 4's review, ADR-0016 §7 amended): a frame of at
// most kLowResMaxRows rows whose aspect is within kLowResAspectTolerance of 4:3 (256 x 224, 8:7; 320 x 224) was shown
// at 4:3 by the television or monitor it was made for (style §15.2, "the original aspect"), so it is shown at 4:3: it
// fills a 4:3 screen and is letterboxed as a 4:3 picture on a portrait one. No per-system data is needed.

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace rv::app {

/// A picture whose aspect is within this share of its screen's fills the whole screen: an encoder's crop (640 x 478,
/// 584 x 446 for 4:3) shows no hairline bars. Beyond it the picture keeps its own aspect.
inline constexpr double kPictureAspectTolerance = 0.02;

/// A low-resolution frame (see the file comment): at most this many rows (a console's or an arcade board's own
/// resolution, not a 480-row capture), and an aspect within this share of 4:3 (8:7 is 14.3% narrower, 8:7.5 20%).
inline constexpr float kLowResMaxRows = 240.0f;
inline constexpr double kLowResAspectTolerance = 0.15;

/// The aspect `picture` (its width and height in pixels, square) was shown at: 4:3 for a low-resolution frame near it
/// (see the file comment), else its own. 0 for an empty or not finite size.
[[nodiscard]] double displayAspect(glm::vec2 picture) noexcept;

/// The whole screen as a picture rectangle: corner (0, 0), size (1, 1).
inline constexpr glm::vec4 kWholeScreen{0.0f, 0.0f, 1.0f, 1.0f};

/// The rectangle of a screen of `screen` (its width and height, in any unit) that a picture of `picture` (its width and
/// height as decoded, pixels; shown at displayAspect()) fills, in the screen's texture coordinates (uv (0, 0)
/// top-left): its corner (x, y) and size (z, w), centred. A wider picture spans the whole width (letterbox), a narrower
/// one the whole height (pillarbox);
/// kWholeScreen when the aspects are within kPictureAspectTolerance of each other, or when a size is empty or not
/// finite (nothing known: the screen as it is).
[[nodiscard]] glm::vec4 pictureRect(glm::vec2 picture, glm::vec2 screen) noexcept;

/// `rect` is the whole screen.
[[nodiscard]] bool isWholeScreen(const glm::vec4& rect) noexcept;

} // namespace rv::app
