#include "room_scene.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <fmt/format.h>
#include <glad/gl.h>
#include <glm/gtc/matrix_inverse.hpp>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>

namespace rv::spikes::xr {
namespace {

constexpr std::string_view kVertexShader = R"(#version 450 core
// One triangle covering the eye's image, without vertex buffers.
void main() {
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}
)";

constexpr std::string_view kFragmentShader = R"(#version 450 core
layout(location = 0) out vec4 outColor;
uniform mat4 uInverseViewProjection;
uniform vec2 uResolution;
uniform float uFloorOffset;
uniform float uTime;

const vec3 kRoomMin = vec3(-3.0, 0.0, -3.0);
const vec3 kRoomMax = vec3(3.0, 2.8, 3.0);
const vec3 kCubeCentre = vec3(0.0, 1.3, -1.2);
const float kCubeHalf = 0.15;
const float kEyeLine = 1.6;
const vec3 kLight = vec3(0.0, 2.6, 0.5);

// The ray leaving the room's box from inside: its distance, and the normal of the surface it hits (into the room).
float roomExit(vec3 origin, vec3 direction, out vec3 normal) {
    vec3 inverse = 1.0 / direction;
    vec3 far = max((kRoomMin - origin) * inverse, (kRoomMax - origin) * inverse);
    float t = min(min(far.x, far.y), far.z);
    if (t == far.x) {
        normal = vec3(-sign(direction.x), 0.0, 0.0);
    } else if (t == far.y) {
        normal = vec3(0.0, -sign(direction.y), 0.0);
    } else {
        normal = vec3(0.0, 0.0, -sign(direction.z));
    }
    return t;
}

// The cube turning slowly about +Y: the distance of the ray's entry (negative when missed), its normal in the cube's
// frame (which colors the faces) and in the room's.
float cubeEntry(vec3 origin, vec3 direction, out vec3 localNormal, out vec3 normal) {
    float angle = uTime * 0.5;
    mat3 rotation = mat3(cos(angle), 0.0, -sin(angle), 0.0, 1.0, 0.0, sin(angle), 0.0, cos(angle));
    vec3 o = transpose(rotation) * (origin - kCubeCentre);
    vec3 d = transpose(rotation) * direction;
    vec3 inverse = 1.0 / d;
    vec3 t1 = (vec3(-kCubeHalf) - o) * inverse;
    vec3 t2 = (vec3(kCubeHalf) - o) * inverse;
    vec3 near = min(t1, t2);
    vec3 far = max(t1, t2);
    float tNear = max(max(near.x, near.y), near.z);
    float tFar = min(min(far.x, far.y), far.z);
    if (tFar < max(tNear, 0.0)) {
        return -1.0;
    }
    if (tNear == near.x) {
        localNormal = vec3(-sign(d.x), 0.0, 0.0);
    } else if (tNear == near.y) {
        localNormal = vec3(0.0, -sign(d.y), 0.0);
    } else {
        localNormal = vec3(0.0, 0.0, -sign(d.z));
    }
    normal = rotation * localNormal;
    return tNear;
}

vec3 shade(vec3 albedo, vec3 position, vec3 normal) {
    vec3 toLight = kLight - position;
    float distance = length(toLight);
    float lambert = max(dot(normal, toLight / distance), 0.0);
    return albedo * (0.12 + 2.4 * lambert / (1.0 + 0.25 * distance * distance));
}

vec3 floorAlbedo(vec3 p) {
    float checker = mod(floor(p.x * 2.0) + floor(p.z * 2.0), 2.0);
    vec3 albedo = mix(vec3(0.30, 0.17, 0.08), vec3(0.20, 0.11, 0.05), checker);
    float radius = length(p.xz);
    // The reference space's origin, and the way to -Z.
    albedo = mix(albedo, vec3(0.85, 0.80, 0.70), step(abs(radius - 0.30), 0.012));
    albedo = mix(albedo, vec3(0.85, 0.80, 0.70), step(abs(p.x), 0.02) * step(-0.85, p.z) * step(p.z, -0.35));
    return albedo;
}

vec3 wallAlbedo(vec3 p, vec3 normal) {
    // The wall in front (at -Z, facing +Z) is greener, so that "forward" is plain.
    vec3 albedo = normal.z > 0.5 ? vec3(0.16, 0.24, 0.18) : vec3(0.26, 0.22, 0.20);
    float along = abs(normal.x) > 0.5 ? p.z : p.x;
    albedo *= 1.0 - 0.35 * step(abs(fract(along * 2.0) - 0.5), 0.008);
    albedo = mix(albedo, vec3(0.95, 0.80, 0.20), step(abs(p.y - kEyeLine), 0.008));
    return albedo;
}

vec3 cubeAlbedo(vec3 localNormal) {
    if (localNormal.x > 0.5) return vec3(0.80, 0.15, 0.10);
    if (localNormal.x < -0.5) return vec3(0.35, 0.06, 0.04);
    if (localNormal.y > 0.5) return vec3(0.15, 0.70, 0.20);
    if (localNormal.y < -0.5) return vec3(0.06, 0.30, 0.08);
    if (localNormal.z > 0.5) return vec3(0.15, 0.30, 0.85);
    return vec3(0.06, 0.12, 0.35);
}

void main() {
    vec2 ndc = gl_FragCoord.xy / uResolution * 2.0 - 1.0;
    vec4 nearPoint = uInverseViewProjection * vec4(ndc, -1.0, 1.0);
    vec4 farPoint = uInverseViewProjection * vec4(ndc, 1.0, 1.0);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    vec3 origin = nearPoint.xyz + vec3(0.0, uFloorOffset, 0.0);
    vec3 direction = normalize(farPoint.xyz - nearPoint.xyz);

    vec3 normal;
    float t = roomExit(origin, direction, normal);
    vec3 localNormal;
    vec3 cubeNormal;
    float tCube = cubeEntry(origin, direction, localNormal, cubeNormal);
    vec3 color;
    if (tCube > 0.0 && tCube < t) {
        vec3 p = origin + direction * tCube;
        color = shade(cubeAlbedo(localNormal), p, cubeNormal);
    } else {
        vec3 p = origin + direction * t;
        vec3 albedo = normal.y > 0.5 ? floorAlbedo(p) : (normal.y < -0.5 ? vec3(0.10, 0.09, 0.09) : wallAlbedo(p, normal));
        color = shade(albedo, p, normal);
    }
    outColor = vec4(color, 1.0);
}
)";

} // namespace

Expected<RoomScene> RoomScene::create() {
    auto program = gl::ShaderProgram::build(kVertexShader, kFragmentShader, "spike_xr.room");
    if (!program) {
        return propagate(program.error());
    }
    auto vertexArray = gl::VertexArray::create("spike_xr.fullscreen");
    if (!vertexArray) {
        return propagate(vertexArray.error());
    }
    return RoomScene(std::move(*program), std::move(*vertexArray));
}

void RoomScene::render(gl::GLDevice& device, std::uint32_t framebuffer, glm::ivec2 size,
                       const glm::mat4& viewProjection, float floorOffset, bool srgb, double timeSeconds) const {
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    gl::RenderStateCache& state = device.state();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer); // see RoomScene: the spike's direct GL calls
    state.setViewport(glm::ivec4(0, 0, size.x, size.y));
    state.setFramebufferSrgb(srgb);
    state.setDepthTest(false);
    state.setDepthWrite(false);
    state.setCullFace(false);
    state.setBlend(false);
    m_program.setMat4("uInverseViewProjection", glm::inverse(viewProjection));
    m_program.setVec2("uResolution", glm::vec2(size));
    m_program.setFloat("uFloorOffset", floorOffset);
    // Wrapped, so that a long run keeps the float precise.
    m_program.setFloat("uTime", static_cast<float>(std::fmod(timeSeconds, 3600.0)));
    m_program.use();
    m_vertexArray.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gl::Framebuffer::unbind();
}

Expected<ImageFramebuffers> ImageFramebuffers::create(std::span<const std::uint32_t> textures,
                                                      std::string_view debugName) {
    ImageFramebuffers result;
    result.m_framebuffers.reserve(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i) {
        GLuint id = 0;
        glCreateFramebuffers(1, &id);
        gl::FramebufferObject framebuffer(id);
        glNamedFramebufferTexture(id, GL_COLOR_ATTACHMENT0, textures[i], 0);
        const std::string label = fmt::format("{}[{}]", debugName, i);
        glObjectLabel(GL_FRAMEBUFFER, id, static_cast<GLsizei>(label.size()), label.c_str());
        const GLenum status = glCheckNamedFramebufferStatus(id, GL_DRAW_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            return makeError(ErrorCode::GraphicsError,
                             fmt::format("the framebuffer of swapchain image {} (texture {}) is incomplete: 0x{:X}", i,
                                         textures[i], status),
                             std::string(debugName));
        }
        result.m_framebuffers.push_back(std::move(framebuffer));
    }
    return result;
}

void blitToWindow(gl::GLDevice& device, std::uint32_t framebuffer, glm::ivec2 source, glm::ivec2 window) {
    if (source.x <= 0 || source.y <= 0 || window.x <= 0 || window.y <= 0) {
        return;
    }
    // Black bars around the eye, then a raw copy of its (already encoded) pixels, like Framebuffer::blitToDefault.
    gl::Framebuffer::clearDefault(device.state(), window);
    const float scale = std::min(static_cast<float>(window.x) / static_cast<float>(source.x),
                                 static_cast<float>(window.y) / static_cast<float>(source.y));
    const glm::ivec2 fitted(static_cast<int>(static_cast<float>(source.x) * scale),
                            static_cast<int>(static_cast<float>(source.y) * scale));
    const glm::ivec2 corner = (window - fitted) / 2;
    glBlitNamedFramebuffer(framebuffer, 0, 0, 0, source.x, source.y, corner.x, corner.y, corner.x + fitted.x,
                           corner.y + fitted.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

} // namespace rv::spikes::xr
