#pragma once

#include <optional>
#include <utility>

#include <glad/gl.h>
#include <glm/vec4.hpp>

namespace rv::gl {

/// Shadow copy of the fixed-function state the renderer changes, so redundant GL calls are skipped. Every setter issues
/// its GL call only when the value differs from the cached one or is unknown.
///
/// Code that changes this state behind the cache's back (third-party renderers such as Dear ImGui, raw GL calls) must
/// call invalidate() afterwards; the next setter calls then reach GL unconditionally. Main thread only.
class RenderStateCache {
public:
    void setDepthTest(bool enabled);
    void setDepthWrite(bool enabled);
    /// GL_LESS, GL_LEQUAL, GL_GREATER, ...
    void setDepthFunc(GLenum func);
    void setCullFace(bool enabled);
    /// Faces to cull: GL_BACK, GL_FRONT or GL_FRONT_AND_BACK (front faces are counter-clockwise, ADR-0002).
    void setCullMode(GLenum face);
    void setBlend(bool enabled);
    /// glBlendFunc factors, applied to color and alpha.
    void setBlendFunc(GLenum sourceFactor, GLenum destinationFactor);
    /// glBlendColor: the constant of the GL_CONSTANT_COLOR and GL_CONSTANT_ALPHA blend factors (RGBA).
    void setBlendColor(const glm::vec4& color);
    /// GL_FRAMEBUFFER_SRGB: linear shader output is encoded to sRGB when the bound color buffer is sRGB.
    void setFramebufferSrgb(bool enabled);
    /// GL_FILL, GL_LINE or GL_POINT, for both faces (core profile).
    void setPolygonMode(GLenum mode);
    /// glColorMask for every channel of every draw buffer: false leaves the color attachments untouched (depth-only
    /// passes).
    void setColorWrite(bool enabled);
    /// GL_POLYGON_OFFSET_FILL with glPolygonOffset(factor, units): the depth of filled polygons is pushed back by
    /// factor times their depth slope plus units of the smallest depth step (shadow maps). Disabled, the factors are
    /// left as they are.
    void setPolygonOffset(bool enabled, float factor = 0.0f, float units = 0.0f);
    /// x, y, width, height in pixels, origin at the bottom-left corner.
    void setViewport(const glm::ivec4& rectangle);

    /// Forgets every cached value.
    void invalidate() noexcept;

private:
    std::optional<bool> m_depthTest;
    std::optional<bool> m_depthWrite;
    std::optional<GLenum> m_depthFunc;
    std::optional<bool> m_cullFace;
    std::optional<GLenum> m_cullMode;
    std::optional<bool> m_blend;
    std::optional<std::pair<GLenum, GLenum>> m_blendFunc;
    std::optional<glm::vec4> m_blendColor;
    std::optional<bool> m_framebufferSrgb;
    std::optional<GLenum> m_polygonMode;
    std::optional<bool> m_colorWrite;
    std::optional<bool> m_polygonOffset;
    std::optional<std::pair<float, float>> m_polygonOffsetFactors;
    std::optional<glm::ivec4> m_viewport;
};

} // namespace rv::gl
