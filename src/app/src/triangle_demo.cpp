#include "triangle_demo.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <rv/core/math.hpp>
#include <rv/render/gl_device.hpp>

namespace rv::app {
namespace {

struct TriangleVertex {
    glm::vec2 position; ///< before rotation and aspect correction, in normalized device coordinates
    glm::vec3 color;    ///< linear RGB
};
static_assert(sizeof(TriangleVertex) == 5 * sizeof(float), "TriangleVertex must be tightly packed");

constexpr GLuint kPositionLocation = 0;
constexpr GLuint kColorLocation = 1;
constexpr GLuint kVertexBinding = 0;
constexpr float kCircumradius = 0.6f;
constexpr float kAngularSpeed = 0.5f; // radians per second

constexpr std::string_view kVertexShader = R"glsl(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 0) out vec3 vColor;
uniform mat4 uTransform;

void main() {
    vColor = aColor;
    gl_Position = uTransform * vec4(aPosition, 0.0, 1.0);
}
)glsl";

constexpr std::string_view kFragmentShader = R"glsl(#version 450 core
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 oColor;
// True when the default framebuffer cannot encode sRGB itself (GL_FRAMEBUFFER_SRGB has no effect on it).
uniform bool uEncodeSrgb;

vec3 linearToSrgb(vec3 linear) {
    vec3 c = clamp(linear, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    oColor = vec4(uEncodeSrgb ? linearToSrgb(vColor) : vColor, 1.0);
}
)glsl";

glm::vec2 pointOnCircle(float degrees) {
    const float radians = core::degreesToRadians(degrees);
    return glm::vec2(std::cos(radians), std::sin(radians)) * kCircumradius;
}

} // namespace

TriangleDemo::TriangleDemo(gl::Buffer vertices, gl::VertexArray vertexArray, gl::ShaderProgram program) noexcept
    : m_vertices(std::move(vertices)), m_vertexArray(std::move(vertexArray)), m_program(std::move(program)) {}

Expected<std::unique_ptr<TriangleDemo>> TriangleDemo::create(bool encodeSrgbInShader) {
    const auto tungsten = core::hexToLinearRgb("#FFC88A");
    const auto coldBlue = core::hexToLinearRgb("#4C8BC9");
    const auto magenta = core::hexToLinearRgb("#B34DCE");
    if (!tungsten || !coldBlue || !magenta) {
        return makeError(ErrorCode::InvalidArgument, "invalid triangle palette color");
    }
    // Counter-clockwise, i.e. front-facing (ADR-0002): top, bottom-left, bottom-right.
    const std::array<TriangleVertex, 3> vertices{{
        {pointOnCircle(90.0f), *tungsten},
        {pointOnCircle(210.0f), *coldBlue},
        {pointOnCircle(330.0f), *magenta},
    }};

    auto buffer = gl::Buffer::create(std::as_bytes(std::span(vertices)), 0, "TriangleDemo.vertices");
    if (!buffer) {
        return propagate(buffer.error());
    }
    auto vertexArray = gl::VertexArray::create("TriangleDemo.vertexArray");
    if (!vertexArray) {
        return propagate(vertexArray.error());
    }
    Status layout = vertexArray->setVertexBuffer(kVertexBinding, *buffer, 0, sizeof(TriangleVertex));
    if (layout) {
        layout = vertexArray->setFloatAttribute(kPositionLocation, kVertexBinding, 2, offsetof(TriangleVertex, position));
    }
    if (layout) {
        layout = vertexArray->setFloatAttribute(kColorLocation, kVertexBinding, 3, offsetof(TriangleVertex, color));
    }
    if (!layout) {
        return propagate(layout.error());
    }

    auto program = gl::ShaderProgram::build(kVertexShader, kFragmentShader, "TriangleDemo");
    if (!program) {
        return propagate(program.error());
    }
    program->setInt("uEncodeSrgb", encodeSrgbInShader ? 1 : 0);

    return std::unique_ptr<TriangleDemo>(
        new TriangleDemo(std::move(*buffer), std::move(*vertexArray), std::move(*program)));
}

void TriangleDemo::update(double deltaSeconds) {
    m_angleRadians = core::wrapAngleRadians(m_angleRadians + static_cast<float>(deltaSeconds) * kAngularSpeed);
}

void TriangleDemo::draw(gl::GLDevice& device, glm::ivec2 framebufferSize) {
    const gl::ScopedDebugGroup group(device, "TriangleDemo");
    gl::RenderStateCache& state = device.state();
    state.setDepthTest(false);
    state.setBlend(false);
    state.setCullFace(true);
    state.setCullMode(GL_BACK);
    state.setPolygonMode(GL_FILL);

    // Keep the triangle equilateral on screen: squeeze the longer axis by the framebuffer aspect ratio.
    glm::vec3 scale(1.0f);
    if (framebufferSize.x > 0 && framebufferSize.y > 0) {
        const float aspect = static_cast<float>(framebufferSize.x) / static_cast<float>(framebufferSize.y);
        scale = aspect >= 1.0f ? glm::vec3(1.0f / aspect, 1.0f, 1.0f) : glm::vec3(1.0f, aspect, 1.0f);
    }
    const glm::mat4 transform =
        glm::scale(glm::mat4(1.0f), scale) * glm::rotate(glm::mat4(1.0f), m_angleRadians, glm::vec3(0.0f, 0.0f, 1.0f));
    m_program.setMat4("uTransform", transform);
    m_program.use();
    m_vertexArray.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace rv::app
