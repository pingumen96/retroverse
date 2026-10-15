#pragma once

// GPU-free core of retroverse-screenshot-diff (rv_screenshot_diff_core): no SDL, OpenGL or ImGui header is included.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#include <glm/vec2.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::screenshot_diff {

/// How far a screenshot may drift from its golden image (style §26.1, docs/testing/gate-a.md): a pixel differs when one
/// of its color channels differs by more than channelThreshold (of 255), and the images match when at most
/// maxDifferingPercent of their pixels differ. The defaults absorb what another run of the same build, driver and GPU
/// may change (a step or two of dithering, texture filtering or FXAA), not a change of the look.
struct Tolerance {
    /// 0..255.
    int channelThreshold = 6;
    /// 0..100.
    double maxDifferingPercent = 0.5;
};

/// Outcome of compareImages().
struct Comparison {
    glm::ivec2 expectedSize{0};
    glm::ivec2 actualSize{0};
    /// Pixels compared: width * height of both images, 0 when their sizes differ.
    std::size_t pixels = 0;
    /// Pixels with a channel that differs by more than the threshold.
    std::size_t differingPixels = 0;
    /// Pixels with any difference, those within the threshold included.
    std::size_t changedPixels = 0;
    /// Largest difference of a channel, 0..255.
    int maxChannelDifference = 0;
    /// Bounds of the differing pixels, from min (inclusive) to max (exclusive); empty (min == max) when none differ.
    glm::ivec2 differingMin{0};
    glm::ivec2 differingMax{0};
    /// The sizes are equal and at most Tolerance::maxDifferingPercent of the pixels differ.
    bool matches = false;

    [[nodiscard]] bool sameSize() const noexcept { return expectedSize == actualSize; }
    /// differingPixels as a percentage of the pixels (0 without pixels).
    [[nodiscard]] double differingPercent() const noexcept;
    /// One line for people: "12 of 2073600 pixels (0.001%) differ by more than 6/255 (largest difference 9/255, in
    /// (10, 20) - (30, 25)): match, at most 0.5% may differ", or why the sizes do not compare.
    [[nodiscard]] std::string describe(const Tolerance& tolerance) const;
};

/// Compares the colors of two images channel by channel: RGB images as they are, RGBA ones without their alpha
/// (screenshots are opaque), grey ones expanded to RGB (assets::convertChannels). Images of different sizes do not
/// match (Comparison::sameSize). Errors: InvalidArgument for an invalid image (assets::ImageData::isValid) or a
/// tolerance outside its range.
[[nodiscard]] Expected<Comparison> compareImages(const assets::ImageData& expected, const assets::ImageData& actual,
                                                 const Tolerance& tolerance);

/// A picture of the differences, RGB at the size of the images: pixels that do not differ show the expected image as a
/// dark grey (a quarter of its luma, for orientation), pixels within the threshold dim blue, pixels above it from red
/// (just above) to yellow (64/255 or more apart). Errors: invalid images, different sizes, a threshold outside 0..255.
[[nodiscard]] Expected<assets::ImageData> makeHeatmap(const assets::ImageData& expected,
                                                      const assets::ImageData& actual, int channelThreshold);

/// Loads two PNG (or JPEG) files and compares them; when they do not match and `heatmapOnMismatch` is given, writes
/// the heatmap there as a PNG (when their sizes allow one). Errors: unreadable images, invalid tolerance, a heatmap
/// that cannot be written.
[[nodiscard]] Expected<Comparison> compareFiles(const std::filesystem::path& expected,
                                                const std::filesystem::path& actual, const Tolerance& tolerance,
                                                const std::optional<std::filesystem::path>& heatmapOnMismatch);

} // namespace rv::screenshot_diff
