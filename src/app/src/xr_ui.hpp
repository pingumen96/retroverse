#pragma once

#include <optional>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/panel_pass.hpp>
#include <rv/render/texture.hpp>
#include <rv/scene/camera.hpp>
#include <rv/xr/panels.hpp>
#include <rv/xr/pose.hpp>

namespace rv::gl {
class GLDevice;
} // namespace rv::gl

namespace rv::app {

class ProductUi;

/// The product UI in VR (I10 step 5, ADR-0017 §6): its whole canvas drawn each frame into a texture of the window's
/// layout size, shown on one panel in the world (xr::PanelMode): in front of the user, world-locked, while the UI has
/// the input, the controllers' ray its mouse there (the trigger clicks, the right stick scrolls); otherwise as a
/// heads-up display locked to the head, where the hover's label lands over its item (the label is projected through
/// hudCamera()). The panel draws into each eye over its post-processing (gl::PanelPass); the window and the
/// screenshots then show it through the eyes, not as an overlay. Main thread, GL context current.
class XrUi {
public:
    /// The panel follows the head for the frame: the world panel placed in front of `head` when the UI takes the
    /// input (`uiHasInput` turning true), the heads-up display on the head otherwise. `canvas`: the UI's size.
    void place(const xr::Pose& head, bool uiHasInput, glm::ivec2 canvas);
    /// The camera the UI's world anchors are projected through in VR (the heads-up display's field from the head).
    [[nodiscard]] scene::Camera hudCamera(const scene::Camera& head) const;

    /// The controllers' ray on the world panel this frame (none: off it, or the panel is the heads-up display), as the
    /// UI's mouse: moved there, the trigger's press and release a click, `wheel` notches of scrolling.
    struct Pointing {
        glm::vec2 pixel{0.0f};
        float distance = 0.0f;
    };
    [[nodiscard]] std::optional<Pointing> point(const std::optional<core::Ray>& ray) const;

    /// Draws `product`'s canvas into the panel's texture (cleared first, mipmapped after). Errors: the texture cannot
    /// be allocated.
    [[nodiscard]] Status render(gl::GLDevice& device, ProductUi& product, glm::ivec2 canvas);
    /// The panel to draw into the eyes, faded by `fade` (the view's): nothing before the first render().
    [[nodiscard]] std::optional<gl::Panel> panel(float fade) const;
    /// Back on the desktop: the canvas and the panel's picture are let go of (kept through a session, from the first
    /// render() on, the UI opening and closing in it).
    void release();

    [[nodiscard]] xr::PanelMode mode() const noexcept { return m_mode; }
    [[nodiscard]] const xr::PanelPose& pose() const noexcept { return m_pose; }

private:
    xr::PanelMode m_mode = xr::PanelMode::Head;
    xr::PanelPose m_pose;
    glm::ivec2 m_canvas{0};
    /// The canvas as the UI draws it (GL_SRGB8_ALPHA8 written raw), and the panel's picture copied from it.
    std::optional<gl::Framebuffer> m_target;
    std::optional<gl::Texture2D> m_texture;
    std::optional<gl::TextureTarget> m_textureTarget;
    bool m_rendered = false;
};

} // namespace rv::app
