#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/xr/fake_runtime.hpp>

namespace rv::xr {

/// `count` GL_SRGB8_ALPHA8 textures of `size` (one level), the fake runtime's swapchain images in the app: what an
/// OpenXR runtime's GL swapchain hands out. Empty when one cannot be created. Main thread, GL context current.
[[nodiscard]] std::vector<std::uint32_t> createGlImages(glm::ivec2 size, std::size_t count);
/// Deletes textures createGlImages() made.
void destroyGlImages(std::span<const std::uint32_t> images) noexcept;

/// `config` with its images made by createGlImages() and destroyed with the runtime.
[[nodiscard]] FakeRuntimeConfig withGlImages(FakeRuntimeConfig config);

} // namespace rv::xr
