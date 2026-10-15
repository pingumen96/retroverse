#pragma once

// Pixel measurements of the screenshots the GPU smoke tests take. Images are RGB (or RGBA, alpha ignored), rows top
// first, sRGB-encoded as the window shows them. Luma here is the weighted sum 0.2126 R + 0.7152 G + 0.0722 B of the
// encoded bytes, 0..255: a measure of how bright a pixel looks, not of the light.

#include <cstddef>
#include <cstdint>
#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::smoke {

/// Luma statistics of an image.
struct LumaSummary {
    double mean = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
};

[[nodiscard]] LumaSummary lumaSummary(const assets::ImageData& image);

/// Luma of pixel (x, y).
[[nodiscard]] double lumaAt(const assets::ImageData& image, int x, int y);

/// Mean luma of the (2 radius + 1)^2 window around `center`, clipped to the image.
[[nodiscard]] double windowLuma(const assets::ImageData& image, glm::ivec2 center, int radius);

/// Largest difference of a color channel between two images of the same size over the window around `center`.
[[nodiscard]] int windowMaxDifference(const assets::ImageData& a, const assets::ImageData& b, glm::ivec2 center,
                                      int radius);

/// Pixels exactly of color `rgb`.
[[nodiscard]] std::size_t countColor(const assets::ImageData& image, const glm::ivec3& rgb);

/// Over every pixel of two images of the same size: how many are brighter in `b` than in `a` by more than `tolerance`
/// luma.
[[nodiscard]] std::size_t brighterPixels(const assets::ImageData& a, const assets::ImageData& b, double tolerance);

/// The pixel where `world` appears in an image of `imageSize` seen from `camera` (a scene's camera, which the walker of
/// retroverse.exe starts at): x right, y down from the top-left corner. Empty when the point is behind the camera or
/// outside the image.
[[nodiscard]] std::optional<glm::ivec2> projectToPixel(const scene::CameraSetup& camera, glm::ivec2 imageSize,
                                                       const glm::vec3& world);

} // namespace rv::smoke
