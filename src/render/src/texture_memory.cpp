#include <rv/render/texture_memory.hpp>

#include <algorithm>
#include <cstdint>

namespace rv::gl {

int fullMipLevelCount(glm::ivec2 size) noexcept {
    int largest = std::max(size.x, size.y);
    int levels = 1;
    while (largest > 1) {
        largest /= 2;
        ++levels;
    }
    return levels;
}

std::size_t mipChainBytes(glm::ivec2 size, int levels) noexcept {
    if (size.x <= 0 || size.y <= 0) {
        return 0;
    }
    std::size_t bytes = 0;
    glm::ivec2 level = size;
    for (int i = 0; i < levels; ++i) {
        bytes += static_cast<std::size_t>(level.x) * static_cast<std::size_t>(level.y) * 4u;
        level = glm::ivec2(std::max(level.x / 2, 1), std::max(level.y / 2, 1));
    }
    return bytes;
}

GpuFormat metallicRoughnessFormat(const assets::ImageData& image) noexcept {
    if (!image.isValid() || image.channels < 3) {
        return GpuFormat::Rgba8;
    }
    const std::size_t channels = static_cast<std::size_t>(image.channels);
    const std::uint8_t first = image.pixels[2];
    if (first != 0 && first != 255) {
        return GpuFormat::Rg8;
    }
    for (std::size_t at = 2; at < image.pixels.size(); at += channels) {
        if (image.pixels[at] != first) {
            return GpuFormat::Rg8;
        }
    }
    return GpuFormat::R8;
}

} // namespace rv::gl
