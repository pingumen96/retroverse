#include <rv/render/sampler.hpp>

#include <string>
#include <utility>

#include "gl_object_label.hpp"
#include "sampling_params.hpp"

namespace rv::gl {

Expected<Sampler> Sampler::create(const SamplingDesc& desc, std::string_view debugName) {
    GLuint id = 0;
    glCreateSamplers(1, &id);
    SamplerObject sampler(id);
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateSamplers returned no sampler", std::string(debugName));
    }
    const GLint wrap = detail::wrapMode(desc.wrap);
    glSamplerParameteri(id, GL_TEXTURE_WRAP_S, wrap);
    glSamplerParameteri(id, GL_TEXTURE_WRAP_T, wrap);
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, detail::minFilter(desc.filter, true));
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, detail::magFilter(desc.filter));
    if (detail::usesAnisotropy(desc)) {
        glSamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, desc.anisotropy);
    }
    detail::setObjectLabel(GL_SAMPLER, id, debugName);

    Sampler result;
    result.m_sampler = std::move(sampler);
    result.m_desc = desc;
    return result;
}

} // namespace rv::gl
