#pragma once

// Translation of SamplingDesc into OpenGL parameters, shared by Texture2D (texture parameters) and Sampler (sampler
// object parameters).

#include <glad/gl.h>

#include <rv/render/texture.hpp>

namespace rv::gl::detail {

[[nodiscard]] inline GLint wrapMode(TextureWrap wrap) noexcept {
    switch (wrap) {
    case TextureWrap::Repeat:
        return GL_REPEAT;
    case TextureWrap::ClampToEdge:
        return GL_CLAMP_TO_EDGE;
    case TextureWrap::MirroredRepeat:
        return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

/// Minification filter; `mipmaps` is false for a texture with a single level, which then never uses a mipmap filter.
[[nodiscard]] inline GLint minFilter(TextureFilter filter, bool mipmaps) noexcept {
    switch (filter) {
    case TextureFilter::Nearest:
        return GL_NEAREST;
    case TextureFilter::Bilinear:
        return GL_LINEAR;
    case TextureFilter::Trilinear:
        return mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    }
    return GL_LINEAR;
}

[[nodiscard]] inline GLint magFilter(TextureFilter filter) noexcept {
    return filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
}

/// GL_TEXTURE_MAX_ANISOTROPY is set only for filtered sampling with an anisotropy above 1: the parameter does not
/// exist without anisotropic filtering support, where SamplingDesc::anisotropy stays 1.
[[nodiscard]] inline bool usesAnisotropy(const SamplingDesc& desc) noexcept {
    return desc.filter != TextureFilter::Nearest && desc.anisotropy > 1.0f;
}

} // namespace rv::gl::detail
