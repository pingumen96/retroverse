#include <rv/app/retroverse_app.hpp>

#include <string_view>
#include <utility>

#include <rv/core/math.hpp>
#include <rv/render/gl_device.hpp>

#include "triangle_demo.hpp"

namespace rv::app {
namespace {

/// Wall color of the style guide palette (§3), the background until the room scene exists.
constexpr std::string_view kClearColor = "#383735";

} // namespace

RetroverseApp::RetroverseApp() = default;

RetroverseApp::~RetroverseApp() = default;

Status RetroverseApp::onInit(AppContext& context) {
    const auto clearColor = core::hexToLinearRgb(kClearColor);
    if (!clearColor) {
        return propagate(clearColor.error());
    }
    m_clearColorLinear = glm::vec4(*clearColor, 1.0f);

    const bool encodeSrgbInShader = !context.device().caps().srgbDefaultFramebuffer;
    auto triangle = TriangleDemo::create(encodeSrgbInShader);
    if (!triangle) {
        return propagate(triangle.error());
    }
    m_triangle = std::move(*triangle);
    return ok();
}

void RetroverseApp::onUpdate(AppContext& /*context*/, double deltaSeconds) {
    if (m_triangle) {
        m_triangle->update(deltaSeconds);
    }
}

void RetroverseApp::onRender(AppContext& context) {
    gl::GLDevice& device = context.device();
    device.beginFrame(context.framebufferSize(), m_clearColorLinear);
    if (m_triangle) {
        m_triangle->draw(device, context.framebufferSize());
    }
}

void RetroverseApp::onShutdown(AppContext& /*context*/) {
    m_triangle.reset();
}

} // namespace rv::app
