#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rv::assets {

/// Decoded 8-bit image, rows stored top to bottom (first row = top of the image, as loaded by stb_image
/// without flipping; see docs/decisions/ADR-0002).
struct ImageData {
    int width = 0;
    int height = 0;
    int channels = 0; ///< 1 (R), 2 (RG), 3 (RGB) or 4 (RGBA)
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool isValid() const noexcept {
        return width > 0 && height > 0 && channels >= 1 && channels <= 4 &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                    static_cast<std::size_t>(channels);
    }
    [[nodiscard]] std::size_t rowBytes() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    }
};

} // namespace rv::assets
