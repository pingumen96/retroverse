#pragma once

#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include <rv/render/gpu_mesh.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::gl {
class Framebuffer;
class GLDevice;
class ShaderProgram;
} // namespace rv::gl

namespace rv::app {

/// Minimal scene path (M0 step 4): the meshes of a loaded scene on the GPU, drawn from the view camera with one unlit
/// program in flat per-part colors (the base color factor of each part's material), one draw per part and instance.
/// Textures, lighting and batching arrive with the scene renderer. Main thread only; destroy it while the GL context
/// is current.
class SceneView {
public:
    /// Uploads every part of `geometry` (one GpuMesh each) and takes the camera and environment of `scene`. A part
    /// that cannot be uploaded is logged and skipped.
    [[nodiscard]] static SceneView create(const scene::SceneDescription& scene, const scene::SceneGeometry& geometry);

    /// Renders into `target`: clears to the scene's clear color, sets the camera aspect ratio from the target, writes
    /// FrameUniforms to `frameUniforms` (bound to UniformBinding::Frame) and draws every part instance with `program`,
    /// which receives the uniforms uModel (local -> world) and uBaseColor (linear RGBA). With an empty program (a
    /// shader that never compiled) only the clear happens.
    void render(gl::GLDevice& device, const gl::ShaderProgram& program,
                gl::UniformBuffer<gl::FrameUniforms>& frameUniforms, const gl::Framebuffer& target, float timeSeconds);

    /// The view camera: created with the scene camera's lens and pose; the pose is the caller's to drive (free roam),
    /// render() only sets the aspect ratio from the target.
    [[nodiscard]] scene::Camera& camera() noexcept { return m_camera; }

    /// Draw calls issued by render().
    [[nodiscard]] std::size_t drawCount() const noexcept { return m_draws.size(); }
    [[nodiscard]] std::size_t meshCount() const noexcept { return m_meshes.size(); }

private:
    struct DrawItem {
        std::size_t mesh = 0; ///< index into m_meshes
        glm::mat4 model{1.0f};
        glm::vec4 color{1.0f}; ///< linear RGBA
        bool doubleSided = false;
    };

    std::vector<gl::GpuMesh> m_meshes;
    std::vector<DrawItem> m_draws;
    scene::Camera m_camera;
    glm::vec4 m_clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    float m_exposure = 1.0f;
};

} // namespace rv::app
