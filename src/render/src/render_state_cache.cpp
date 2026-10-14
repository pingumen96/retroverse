#include <rv/render/render_state_cache.hpp>

namespace rv::gl {
namespace {

/// Updates `cached` and reports whether GL must be told.
template <class T>
bool changes(std::optional<T>& cached, const T& value) {
    if (cached && *cached == value) {
        return false;
    }
    cached = value;
    return true;
}

void setCapability(GLenum capability, bool enabled) {
    if (enabled) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

} // namespace

void RenderStateCache::setDepthTest(bool enabled) {
    if (changes(m_depthTest, enabled)) {
        setCapability(GL_DEPTH_TEST, enabled);
    }
}

void RenderStateCache::setDepthWrite(bool enabled) {
    if (changes(m_depthWrite, enabled)) {
        glDepthMask(enabled ? GL_TRUE : GL_FALSE);
    }
}

void RenderStateCache::setDepthFunc(GLenum func) {
    if (changes(m_depthFunc, func)) {
        glDepthFunc(func);
    }
}

void RenderStateCache::setCullFace(bool enabled) {
    if (changes(m_cullFace, enabled)) {
        setCapability(GL_CULL_FACE, enabled);
    }
}

void RenderStateCache::setCullMode(GLenum face) {
    if (changes(m_cullMode, face)) {
        glCullFace(face);
    }
}

void RenderStateCache::setBlend(bool enabled) {
    if (changes(m_blend, enabled)) {
        setCapability(GL_BLEND, enabled);
    }
}

void RenderStateCache::setBlendFunc(GLenum sourceFactor, GLenum destinationFactor) {
    if (changes(m_blendFunc, std::pair{sourceFactor, destinationFactor})) {
        glBlendFunc(sourceFactor, destinationFactor);
    }
}

void RenderStateCache::setFramebufferSrgb(bool enabled) {
    if (changes(m_framebufferSrgb, enabled)) {
        setCapability(GL_FRAMEBUFFER_SRGB, enabled);
    }
}

void RenderStateCache::setPolygonMode(GLenum mode) {
    if (changes(m_polygonMode, mode)) {
        glPolygonMode(GL_FRONT_AND_BACK, mode);
    }
}

void RenderStateCache::setViewport(const glm::ivec4& rectangle) {
    if (changes(m_viewport, rectangle)) {
        glViewport(rectangle.x, rectangle.y, rectangle.z, rectangle.w);
    }
}

void RenderStateCache::invalidate() noexcept {
    m_depthTest.reset();
    m_depthWrite.reset();
    m_depthFunc.reset();
    m_cullFace.reset();
    m_cullMode.reset();
    m_blend.reset();
    m_blendFunc.reset();
    m_framebufferSrgb.reset();
    m_polygonMode.reset();
    m_viewport.reset();
}

} // namespace rv::gl
