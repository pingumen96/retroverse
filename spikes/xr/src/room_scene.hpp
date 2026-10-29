#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {
class GLDevice;
} // namespace rv::gl

namespace rv::spikes::xr {

/// What the headset shows: a 6 x 6 m room, 2.8 m high, around the reference space's origin, ray-cast analytically in
/// one fullscreen triangle per eye from the eye's inverse view-projection. It checks what the spike is about at almost
/// no GPU cost: tracking (the room stays put while the head moves), the stereo projection (asymmetric fields, the
/// distance between the eyes: the cube in front floats at a believable depth) and the floor (a ring where the reference
/// space's origin is, an arrow towards -Z, a yellow line round the walls at 1.60 m).
///
/// Like spike_launch's scene, it draws with rv_render's program, vertex array and state cache; its few direct GL calls
/// (binding the eye's framebuffer, the draw call, the mirror's blit) are this measurement tool's exception to "OpenGL
/// calls only in rv_render, rv_devui and rv_app": the product's VR mode adds the helpers to rv_render instead (I10
/// step 2). Main thread only, while the GL context is current.
class RoomScene {
public:
    [[nodiscard]] static Expected<RoomScene> create();

    /// Draws the room into `framebuffer` (`size` pixels) as seen through `viewProjection`. `floorOffset` lifts the
    /// eye above the floor in a reference space whose origin is not on it (LOCAL: 1.6 m). `srgb` encodes the linear
    /// output for an sRGB swapchain.
    void render(gl::GLDevice& device, std::uint32_t framebuffer, glm::ivec2 size, const glm::mat4& viewProjection,
                float floorOffset, bool srgb, double timeSeconds) const;

private:
    RoomScene(gl::ShaderProgram program, gl::VertexArray vertexArray) noexcept
        : m_program(std::move(program)), m_vertexArray(std::move(vertexArray)) {}

    gl::ShaderProgram m_program;
    gl::VertexArray m_vertexArray;
};

/// One framebuffer per image of a swapchain, each with the image's texture as its color attachment.
class ImageFramebuffers {
public:
    ImageFramebuffers() = default;

    /// Fails with GraphicsError when a framebuffer is incomplete (a texture the runtime gave is unusable).
    [[nodiscard]] static Expected<ImageFramebuffers> create(std::span<const std::uint32_t> textures,
                                                            std::string_view debugName);

    [[nodiscard]] std::uint32_t framebuffer(std::size_t image) const noexcept { return m_framebuffers[image].id(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_framebuffers.size(); }

private:
    std::vector<gl::FramebufferObject> m_framebuffers;
};

/// Shows `framebuffer` (`source` pixels, an eye) in the window's default framebuffer, fitted and centred: the mirror.
void blitToWindow(gl::GLDevice& device, std::uint32_t framebuffer, glm::ivec2 source, glm::ivec2 window);

} // namespace rv::spikes::xr
