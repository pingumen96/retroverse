#pragma once

#include <string_view>

#include <glad/gl.h>

#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/texture.hpp>

namespace rv::gl {

/// Move-only RAII sampler object (glCreateSamplers): sampling state shared by every texture bound with it, overriding
/// the textures' own parameters. The renderer binds samplers with its filtering quality (anisotropy from the
/// device capabilities) next to the material textures. A default-constructed or moved-from sampler is empty. Main
/// thread only.
class Sampler {
public:
    Sampler() noexcept = default;

    /// Creates the sampler object with `desc` (Trilinear uses GL_LINEAR_MIPMAP_LINEAR, which a texture without
    /// mipmaps samples bilinearly). Errors: GraphicsError when no object can be created.
    [[nodiscard]] static Expected<Sampler> create(const SamplingDesc& desc, std::string_view debugName);

    [[nodiscard]] GLuint id() const noexcept { return m_sampler.id(); }
    [[nodiscard]] bool valid() const noexcept { return m_sampler.valid(); }
    [[nodiscard]] const SamplingDesc& desc() const noexcept { return m_desc; }

private:
    SamplerObject m_sampler;
    SamplingDesc m_desc;
};

} // namespace rv::gl
