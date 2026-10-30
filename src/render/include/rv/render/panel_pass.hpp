#pragma once

#include <span>

#include <glad/gl.h>
#include <glm/mat4x4.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {

class Framebuffer;
class RenderStateCache;
class ShaderProgram;

/// A flat picture in the world (I10 step 5: the VR mode's UI): `texture` (premultiplied alpha, display-encoded sRGB
/// values, as RmlUi draws them) on the unit square [0, 1]² that `model` places in the world, x to the right of the
/// picture and y up it (its top row at y = 1).
struct Panel {
    GLuint texture = 0;
    glm::mat4 model{1.0f};
    /// [0, 1]: the picture faded out (the view's fade).
    float opacity = 1.0f;
};

/// Draws panels over a view's display-ready output (ADR-0017 §6's in-scene panels): after the post-processing, blended
/// in the display's encoding like the UI over the window (GL_FRAMEBUFFER_SRGB off, premultiplied alpha), on top of
/// the scene (no depth test), both faces. Shaders: xr/panel.vert and xr/panel.frag (uniforms uPanelViewProjection,
/// uPanelModel, uPanelOpacity; the picture on texture unit 14). Main thread.
class PanelPass {
public:
    [[nodiscard]] static Expected<PanelPass> create();

    /// Draws `panels` into `target` seen through `viewProjection`; panels without a texture are skipped. Leaves
    /// GL_FRAMEBUFFER_SRGB off, the blend off.
    void draw(const ShaderProgram& program, RenderStateCache& state, const Framebuffer& target,
              const glm::mat4& viewProjection, std::span<const Panel> panels) const;

private:
    explicit PanelPass(VertexArray quad) noexcept : m_quad(std::move(quad)) {}

    VertexArray m_quad;
};

} // namespace rv::gl
