#include "xr_ui.hpp"

#include <cstddef>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/render/gl_device.hpp>

#include "product_ui.hpp"

namespace rv::app {
namespace {

float aspectOf(glm::ivec2 canvas) noexcept {
    return canvas.x > 0 && canvas.y > 0 ? static_cast<float>(canvas.x) / static_cast<float>(canvas.y) : 16.0f / 9.0f;
}

} // namespace

void XrUi::place(const xr::Pose& head, bool uiHasInput, glm::ivec2 canvas) {
    const float aspect = aspectOf(canvas);
    if (uiHasInput) {
        // Placed once, where the user looked when the UI took the input; it stays there.
        if (m_mode != xr::PanelMode::World) {
            m_pose = xr::worldPanel(head, aspect);
            m_mode = xr::PanelMode::World;
        }
        return;
    }
    m_mode = xr::PanelMode::Head;
    m_pose = xr::hudPanel(head, aspect);
}

scene::Camera XrUi::hudCamera(const scene::Camera& head) const {
    scene::Camera camera = head;
    camera.setEye(head.position(), head.orientation(), xr::hudField(aspectOf(m_canvas)));
    return camera;
}

std::optional<XrUi::Pointing> XrUi::point(const std::optional<core::Ray>& ray) const {
    if (!ray || m_mode != xr::PanelMode::World || m_canvas.x <= 0 || m_canvas.y <= 0) {
        return std::nullopt;
    }
    if (const std::optional<xr::PanelHit> hit = xr::panelHit(m_pose, *ray, glm::vec2(m_canvas))) {
        return Pointing{hit->pixel, hit->distance};
    }
    return std::nullopt;
}

Status XrUi::render(gl::GLDevice& device, ProductUi& product, glm::ivec2 canvas) {
    if (canvas.x <= 0 || canvas.y <= 0) {
        return ok();
    }
    if (!m_target || m_target->size() != canvas) {
        m_rendered = false;
        m_target.reset();
        m_texture.reset();
        m_textureTarget.reset();
        gl::FramebufferDesc desc;
        desc.size = canvas;
        desc.depthStencil = false;
        auto target = gl::Framebuffer::create(desc, "XrUiCanvas");
        if (!target) {
            return propagate(target.error());
        }
        // The panel's picture: the canvas's display-encoded bytes as they are (GL_RGBA8: sampled without decoding),
        // mipmapped for a panel seen from afar.
        assets::ImageData blank;
        blank.width = canvas.x;
        blank.height = canvas.y;
        blank.channels = 4;
        blank.pixels.assign(static_cast<std::size_t>(canvas.x) * static_cast<std::size_t>(canvas.y) * 4, 0);
        gl::TextureDesc textureDesc;
        textureDesc.colorSpace = gl::ColorSpace::Linear;
        textureDesc.mipmaps = true;
        textureDesc.sampling.wrap = gl::TextureWrap::ClampToEdge;
        textureDesc.memoryKind = gl::GpuMemoryKind::Targets;
        auto texture = gl::Texture2D::create(blank, textureDesc, "XrUiPanel");
        if (!texture) {
            return propagate(texture.error());
        }
        auto textureTarget = gl::TextureTarget::create(texture->id(), canvas, "XrUiPanelTarget");
        if (!textureTarget) {
            return propagate(textureTarget.error());
        }
        m_target = std::move(*target);
        m_texture = std::move(*texture);
        m_textureTarget = std::move(*textureTarget);
    }
    m_canvas = canvas;
    m_target->clear(glm::vec4(0.0f));
    product.renderTo(device, *m_target);
    device.state().setFramebufferSrgb(false);
    m_target->copyColorTo(*m_textureTarget);
    glGenerateTextureMipmap(m_texture->id());
    m_rendered = true;
    return ok();
}

std::optional<gl::Panel> XrUi::panel(float fade) const {
    if (!m_rendered || !m_texture) {
        return std::nullopt;
    }
    return gl::Panel{m_texture->id(), m_pose.model(), 1.0f - fade};
}

} // namespace rv::app
