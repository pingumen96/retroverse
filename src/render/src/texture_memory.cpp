#include <rv/render/texture_memory.hpp>

#include <algorithm>

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

} // namespace rv::gl
