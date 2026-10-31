#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// The black borders of a captured game's window (I10 step 11): an emulator in a borderless window as wide as the monitor
// pillarboxes a 4:3 game (RetroArch's 2560 x 1440 window holds the game in its middle 1920 x 1440), and the hero CRT,
// fitting the whole window into its tube, letterboxed that again. The window's picture is read back small a few times a
// second (gl::PictureProbe), findPictureBounds() finds its bars, and PictureBounds keeps the crop steady: content coming
// into a bar widens it at once, a bar that grows narrows it only once it held for a while. The CRT then shows the crop at
// its own aspect (croppedPictureRect(), screen_aspect.hpp).
//
// Emulators centre the game's picture, so the bars are taken in pairs (left and right alike, top and bottom alike: the
// thinner of the two), and a line of the window is a bar while hardly any of its pixels is lit (an emulator's on-screen
// notice in a corner of a bar leaves it a bar). A crop narrower or lower than half the window is none (a dark scene, a
// logo on black).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace rv::app {

/// A pixel whose red, green and blue are all at most this (of 255) is black: an emulator's bars are 0, a video's
/// black a few levels above.
inline constexpr std::uint8_t kBorderBlackLevel = 24;
/// A row or a column is a bar while at most this share of its pixels is lit.
inline constexpr float kBarLitShare = 0.06f;
/// A crop narrower or lower than this share of the window is no crop.
inline constexpr float kMinCropShare = 0.5f;
/// Bars thinner than this share of the window's side are left (a hairline is no bar).
inline constexpr float kMinBarShare = 0.01f;

/// The picture's part between its bars: the corner (x, y) and the size (z, w) in the picture's texture coordinates
/// (uv (0, 0) the top-left), from `rgba` (`size.x` x `size.y` pixels of four bytes, rows from the top), centred. The
/// whole picture (0, 0, 1, 1) when it has no bar; nothing when it is all black, or the part is narrower or lower than
/// kMinCropShare (nothing to decide on). Each bar loses one sampled line (the content's edge kept).
[[nodiscard]] std::optional<glm::vec4> findPictureBounds(std::span<const std::uint8_t> rgba, glm::ivec2 size) noexcept;

/// The crop of a captured window, kept steady over its samples.
class PictureBounds {
public:
    /// A crop that narrows must be found this many samples in a row before it is taken (about 2 s at 4 a second).
    static constexpr std::size_t kNarrowSamples = 8;
    /// Crops whose edges are this close (a share of the picture) are the same crop.
    static constexpr float kSameCrop = 0.01f;

    /// One sample's findPictureBounds(). Nothing (a dark frame) keeps the crop. True when the crop changed.
    bool update(const std::optional<glm::vec4>& found) noexcept;
    /// The crop shown: the whole picture until samples say otherwise.
    [[nodiscard]] const glm::vec4& crop() const noexcept { return m_crop; }
    /// Back to the whole picture (another window).
    void reset() noexcept;

private:
    glm::vec4 m_crop{0.0f, 0.0f, 1.0f, 1.0f};
    std::optional<glm::vec4> m_candidate;
    std::size_t m_candidateSamples = 0;
};

} // namespace rv::app
