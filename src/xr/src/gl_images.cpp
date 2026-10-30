#include <rv/xr/gl_images.hpp>

#include <glad/gl.h>

#include <fmt/format.h>

namespace rv::xr {

std::vector<std::uint32_t> createGlImages(glm::ivec2 size, std::size_t count) {
    std::vector<std::uint32_t> images(count, 0);
    glCreateTextures(GL_TEXTURE_2D, static_cast<GLsizei>(count), images.data());
    for (std::size_t i = 0; i < images.size(); ++i) {
        if (images[i] == 0) {
            destroyGlImages(images);
            return {};
        }
        glTextureStorage2D(images[i], 1, GL_SRGB8_ALPHA8, size.x, size.y);
        const std::string label = fmt::format("FakeXrSwapchain.image{}", i);
        glObjectLabel(GL_TEXTURE, images[i], static_cast<GLsizei>(label.size()), label.c_str());
    }
    return images;
}

void destroyGlImages(std::span<const std::uint32_t> images) noexcept {
    for (const std::uint32_t image : images) {
        if (image != 0) {
            glDeleteTextures(1, &image);
        }
    }
}

FakeRuntimeConfig withGlImages(FakeRuntimeConfig config) {
    config.createImages = [](glm::ivec2 size, std::size_t count) { return createGlImages(size, count); };
    config.destroyImages = [](std::span<const std::uint32_t> images) { destroyGlImages(images); };
    return config;
}

} // namespace rv::xr
