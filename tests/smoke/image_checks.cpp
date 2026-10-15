#include "image_checks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <glm/glm.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>

namespace rv::smoke {
namespace {

const std::uint8_t* pixelAt(const assets::ImageData& image, int x, int y) {
    return &image.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                          static_cast<std::size_t>(x)) *
                         static_cast<std::size_t>(image.channels)];
}

/// The color of a pixel as RGB (grey replicated).
glm::ivec3 rgbAt(const assets::ImageData& image, int x, int y) {
    const std::uint8_t* pixel = pixelAt(image, x, y);
    if (image.channels < 3) {
        return glm::ivec3(pixel[0]);
    }
    return glm::ivec3(pixel[0], pixel[1], pixel[2]);
}

double luma(const glm::ivec3& rgb) {
    return 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
}

} // namespace

LumaSummary lumaSummary(const assets::ImageData& image) {
    LumaSummary summary;
    if (!image.isValid()) {
        return summary;
    }
    summary.minimum = 255.0;
    double sum = 0.0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const double value = luma(rgbAt(image, x, y));
            sum += value;
            summary.minimum = std::min(summary.minimum, value);
            summary.maximum = std::max(summary.maximum, value);
        }
    }
    summary.mean = sum / (static_cast<double>(image.width) * static_cast<double>(image.height));
    return summary;
}

double lumaAt(const assets::ImageData& image, int x, int y) {
    return luma(rgbAt(image, x, y));
}

double windowLuma(const assets::ImageData& image, glm::ivec2 center, int radius) {
    double sum = 0.0;
    int count = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(image.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(image.width - 1, center.x + radius); ++x) {
            sum += lumaAt(image, x, y);
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / count;
}

int windowMaxDifference(const assets::ImageData& a, const assets::ImageData& b, glm::ivec2 center, int radius) {
    int largest = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(a.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(a.width - 1, center.x + radius); ++x) {
            const glm::ivec3 difference = glm::abs(rgbAt(a, x, y) - rgbAt(b, x, y));
            largest = std::max({largest, difference.r, difference.g, difference.b});
        }
    }
    return largest;
}

std::size_t countColor(const assets::ImageData& image, const glm::ivec3& rgb) {
    std::size_t count = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            count += rgbAt(image, x, y) == rgb ? 1 : 0;
        }
    }
    return count;
}

std::size_t brighterPixels(const assets::ImageData& a, const assets::ImageData& b, double tolerance) {
    std::size_t count = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            count += lumaAt(b, x, y) > lumaAt(a, x, y) + tolerance ? 1 : 0;
        }
    }
    return count;
}

std::optional<glm::ivec2> projectToPixel(const scene::CameraSetup& camera, glm::ivec2 imageSize,
                                         const glm::vec3& world) {
    scene::Camera view;
    view.setLens(camera.lens);
    view.setAspect(static_cast<float>(imageSize.x) / static_cast<float>(imageSize.y));
    view.setPose(camera.position, core::degreesToRadians(camera.yawDeg), core::degreesToRadians(camera.pitchDeg));
    const glm::vec4 clip = view.viewProjection() * glm::vec4(world, 1.0f);
    if (clip.w <= 0.0f) {
        return std::nullopt;
    }
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    const glm::ivec2 pixel(static_cast<int>(std::floor((ndc.x * 0.5f + 0.5f) * static_cast<float>(imageSize.x))),
                           static_cast<int>(std::floor((0.5f - ndc.y * 0.5f) * static_cast<float>(imageSize.y))));
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= imageSize.x || pixel.y >= imageSize.y) {
        return std::nullopt;
    }
    return pixel;
}

} // namespace rv::smoke
