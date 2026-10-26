#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstddef>

#include <glm/vec2.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

/// Mip levels of a full chain for an image of `size`: floor(log2(max(width, height))) + 1, 1 for an empty size.
[[nodiscard]] int fullMipLevelCount(glm::ivec2 size) noexcept;

/// Bytes of the first `levels` levels of an RGBA8 texture of `size` (4 bytes per texel; each level half the previous
/// one, at least 1 x 1): the memory Texture2D allocates, about 4/3 of the base level with a full chain. 0 for an empty
/// size or no level.
[[nodiscard]] std::size_t mipChainBytes(glm::ivec2 size, int levels) noexcept;

/// The storage a metallic-roughness map takes (TextureDesc::metallicRoughness, I6 step 5's review): R8 when its every B
/// (metallic) is 0, or every B is 255; RG8 otherwise; RGBA8 for an invalid image or one of fewer than 3 channels (kept
/// as it is).
[[nodiscard]] GpuFormat metallicRoughnessFormat(const assets::ImageData& image) noexcept;

} // namespace rv::gl
