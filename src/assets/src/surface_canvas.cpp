#include "surface_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <execution>
#include <numeric>
#include <vector>

#include <glm/geometric.hpp>

#include <rv/core/math.hpp>

namespace rv::assets::detail {
namespace {

constexpr int kTableSteps = 65535;

/// Linear [0, 1] -> 8-bit sRGB, one entry per 1/65535 of the linear range (well below half an 8-bit step anywhere).
const std::vector<std::uint8_t>& srgbEncodeTable() {
    static const std::vector<std::uint8_t> table = [] {
        std::vector<std::uint8_t> encoded(static_cast<std::size_t>(kTableSteps) + 1u);
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            const float linear = static_cast<float>(i) / static_cast<float>(kTableSteps);
            encoded[i] = static_cast<std::uint8_t>(std::lround(core::linearToSrgb(linear) * 255.0f));
        }
        return encoded;
    }();
    return table;
}

std::uint8_t encodeSrgb(float linear, const std::vector<std::uint8_t>& table) noexcept {
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    return table[static_cast<std::size_t>(clamped * static_cast<float>(kTableSteps) + 0.5f)];
}

std::uint8_t encodeUnit(float value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

ImageData blankRgba(glm::ivec2 size) {
    ImageData image;
    image.width = size.x;
    image.height = size.y;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4u);
    return image;
}

/// Runs `row(y)` for every y in [0, height), in parallel.
template <class Row>
void forEachRow(int height, const Row& row) {
    std::vector<int> rows(static_cast<std::size_t>(height));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), row);
}

} // namespace

glm::vec3 linearFromHex(std::uint32_t hex) noexcept {
    const auto channel = [hex](int shift) {
        return core::srgbToLinear(static_cast<float>((hex >> shift) & 0xFFu) / 255.0f);
    };
    return {channel(16), channel(8), channel(0)};
}

ProceduralSurface renderSurface(const CanvasDesc& desc, const SurfaceFunction& surface) {
    const int width = desc.size.x;
    const int height = desc.size.y;
    const int samples = std::max(desc.samplesPerAxis, 1);
    const float sampleWeight = 1.0f / static_cast<float>(samples * samples);
    const std::vector<std::uint8_t>& table = srgbEncodeTable();

    ProceduralSurface out;
    out.baseColor = blankRgba(desc.size);
    out.metallicRoughness = blankRgba(desc.size);
    std::vector<float> heights(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    forEachRow(height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            SurfaceSample sum{glm::vec3(0.0f), 0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < samples; ++sy) {
                const float v =
                    (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / static_cast<float>(samples)) /
                    static_cast<float>(height);
                for (int sx = 0; sx < samples; ++sx) {
                    const float u =
                        (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / static_cast<float>(samples)) /
                        static_cast<float>(width);
                    const SurfaceSample s = surface(u, v);
                    sum.albedo += s.albedo;
                    sum.roughness += s.roughness;
                    sum.metallic += s.metallic;
                    sum.height += s.height;
                }
            }
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                      static_cast<std::size_t>(x);
            std::uint8_t* color = out.baseColor.pixels.data() + index * 4u;
            color[0] = encodeSrgb(sum.albedo.r * sampleWeight, table);
            color[1] = encodeSrgb(sum.albedo.g * sampleWeight, table);
            color[2] = encodeSrgb(sum.albedo.b * sampleWeight, table);
            color[3] = 255;
            std::uint8_t* material = out.metallicRoughness.pixels.data() + index * 4u;
            material[0] = 255;
            material[1] = encodeUnit(sum.roughness * sampleWeight);
            material[2] = encodeUnit(sum.metallic * sampleWeight);
            material[3] = 255;
            heights[index] = sum.height * sampleWeight;
        }
    });

    if (!desc.normalMap) {
        return out;
    }
    out.normal = blankRgba(desc.size);
    // Slopes of the height in meters per meter: central differences over two pixels.
    const float scaleX = desc.normalStrength * static_cast<float>(width) / (2.0f * desc.meters.x);
    const float scaleY = desc.normalStrength * static_cast<float>(height) / (2.0f * desc.meters.y);
    const auto neighbor = [&desc](int i, int count) {
        if (desc.tiles) {
            return (i % count + count) % count;
        }
        return std::clamp(i, 0, count - 1);
    };
    const auto heightAt = [&](int x, int y) {
        return heights[static_cast<std::size_t>(neighbor(y, height)) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(neighbor(x, width))];
    };
    forEachRow(height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const float slopeRight = (heightAt(x + 1, y) - heightAt(x - 1, y)) * scaleX;
            const float slopeDown = (heightAt(x, y + 1) - heightAt(x, y - 1)) * scaleY;
            // z = h(X, Y) with X along +u and Y towards the top of the image (-v): n = (-dh/dX, -dh/dY, 1).
            const glm::vec3 normal = glm::normalize(glm::vec3(-slopeRight, slopeDown, 1.0f));
            std::uint8_t* encoded = out.normal.pixels.data() +
                                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)) *
                                        4u;
            encoded[0] = encodeUnit(normal.x * 0.5f + 0.5f);
            encoded[1] = encodeUnit(normal.y * 0.5f + 0.5f);
            encoded[2] = encodeUnit(normal.z * 0.5f + 0.5f);
            encoded[3] = 255;
        }
    });
    return out;
}

} // namespace rv::assets::detail
