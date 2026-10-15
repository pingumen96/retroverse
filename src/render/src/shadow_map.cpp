#include <rv/render/shadow_map.hpp>

#include <array>
#include <utility>

#include <fmt/format.h>
#include <glm/vec4.hpp>

#include <rv/render/render_state_cache.hpp>

#include "gl_object_label.hpp"

namespace rv::gl {

Expected<ShadowMap> ShadowMap::create(int resolution, std::string_view debugName) {
    std::string name(debugName);
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (resolution < 1 || resolution > maxTextureSize) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("shadow map resolution {} is outside 1..{}", resolution, maxTextureSize), name);
    }

    ShadowMap map;
    map.m_debugName = std::move(name);
    map.m_resolution = resolution;

    GLuint depth = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &depth);
    map.m_depth = TextureObject(depth);
    if (depth == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateTextures returned no texture", map.m_debugName);
    }
    glTextureStorage2D(depth, 1, GL_DEPTH_COMPONENT32F, resolution, resolution);
    GLint immutable = GL_FALSE;
    glGetTextureParameteriv(depth, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate a {0}x{0} shadow map", resolution), map.m_debugName);
    }
    detail::setObjectLabel(GL_TEXTURE, depth, map.m_debugName + ".depth");

    GLuint sampler = 0;
    glCreateSamplers(1, &sampler);
    map.m_comparison = SamplerObject(sampler);
    if (sampler == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateSamplers returned no sampler", map.m_debugName);
    }
    glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    constexpr std::array<GLfloat, 4> kFarBorder{1.0f, 1.0f, 1.0f, 1.0f};
    glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, kFarBorder.data());
    detail::setObjectLabel(GL_SAMPLER, sampler, map.m_debugName + ".comparison");

    GLuint framebuffer = 0;
    glCreateFramebuffers(1, &framebuffer);
    map.m_framebuffer = FramebufferObject(framebuffer);
    if (framebuffer == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateFramebuffers returned no framebuffer", map.m_debugName);
    }
    glNamedFramebufferTexture(framebuffer, GL_DEPTH_ATTACHMENT, depth, 0);
    glNamedFramebufferDrawBuffer(framebuffer, GL_NONE);
    glNamedFramebufferReadBuffer(framebuffer, GL_NONE);
    const GLenum status = glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("shadow map framebuffer is incomplete (0x{:04X})", status), map.m_debugName);
    }
    detail::setObjectLabel(GL_FRAMEBUFFER, framebuffer, map.m_debugName);
    return map;
}

void ShadowMap::beginPass(RenderStateCache& state) const {
    if (!valid()) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer.id());
    state.setViewport(glm::ivec4(0, 0, m_resolution, m_resolution));
    state.setDepthWrite(true);
    constexpr GLfloat kFar = 1.0f;
    glClearNamedFramebufferfv(m_framebuffer.id(), GL_DEPTH, 0, &kFar);
}

void ShadowMap::bind(GLuint unit) const {
    glBindTextureUnit(unit, m_depth.id());
    glBindSampler(unit, m_comparison.id());
}

} // namespace rv::gl
