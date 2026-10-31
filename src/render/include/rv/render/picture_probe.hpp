#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glad/gl.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>

namespace rv::gl {

class RenderStateCache;

/// A small copy of a texture read back without stalling the frame (I10 step 11: the captured game's black bars,
/// app::findPictureBounds): request() blits the texture down to kSize (linear) and starts reading it into a pixel buffer
/// behind a fence; poll() hands the pixels over once the GPU passed the fence (RGBA8, rows from the image's top as the
/// texture's first row is). One read at a time. Main thread, GL context current.
class PictureProbe {
public:
    static constexpr glm::ivec2 kSize{256, 144};

    [[nodiscard]] static Expected<std::unique_ptr<PictureProbe>> create(std::string_view debugName);
    ~PictureProbe();
    PictureProbe(const PictureProbe&) = delete;
    PictureProbe& operator=(const PictureProbe&) = delete;

    /// Starts reading `area` (corner and size, texels, rows from the image's top) of `texture` (a 2D texture, level 0).
    /// False while a read is pending (nothing started).
    bool request(GLuint texture, const glm::ivec4& area, RenderStateCache& state);
    /// The pixels of the read once it is done (kSize.x * kSize.y * 4 bytes), else nothing; valid until the next call.
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> poll();
    [[nodiscard]] bool pending() const noexcept { return m_fence != nullptr; }

private:
    PictureProbe() = default;

    GLuint m_small = 0;
    GLuint m_readFramebuffer = 0;
    GLuint m_drawFramebuffer = 0;
    GLuint m_buffer = 0;
    GLsync m_fence = nullptr;
    std::vector<std::uint8_t> m_pixels;
};

} // namespace rv::gl
