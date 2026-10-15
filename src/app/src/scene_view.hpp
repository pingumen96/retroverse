#pragma once

#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/gpu_mesh.hpp>
#include <rv/render/material_set.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::gl {
class Framebuffer;
class GLDevice;
class ShaderProgram;
class TextureCache;
class TextureDefaults;
struct GLCapabilities;
} // namespace rv::gl

namespace rv::app {

/// Programs a SceneView draws with, one per scene::ShaderKind. A null or empty program (a shader that never built)
/// skips the draws of its kind.
struct SceneShaders {
    const gl::ShaderProgram* pbr = nullptr;   ///< PBR_Base: pbr/pbr.vert + pbr/pbr.frag
    const gl::ShaderProgram* unlit = nullptr; ///< Unlit_Emissive: pbr/pbr.vert + unlit/unlit_emissive.frag
};

/// Lit scene path (M0 step 6): the meshes of a loaded scene on the GPU with their materials, textures and lights,
/// drawn from the view camera with one draw per part and instance.
///  - Materials: one GPU material per material of the scene (library name, inline material, fallback), and one more
///    per object and role whose part has a scene texture (ScenePart::texture), which becomes the base color texture
///    of that instance: cover patterns on cartridge labels and posters, color bars on the CRT screen. Pattern images
///    take the aspect ratio of their surface (geometry::uvMetersPerUnit). Textures named by the materials themselves
///    are loaded from the asset root; missing ones are drawn as a checker (TextureCache).
///  - Lights: the scene lights and environment packed into the lights block (gl::packLights; lights beyond its
///    capacity are logged and left out).
///  - Order: opaque materials, then alpha-masked ones, each sorted by shader, material and mesh; alpha-blended
///    materials last, in scene order, without depth sorting (blending on, depth writes off): no M0 scene uses one,
///    and sorting arrives with the batching renderer.
/// Main thread only; destroy it while the GL context is current.
class SceneView {
public:
    /// Uploads every part of `geometry` (one GpuMesh each; a part that cannot be uploaded is logged and skipped),
    /// creates the materials with textures from `textures` (whose anisotropy the material samplers share; uniform
    /// buffer alignment from `caps`), packs the lights and takes the camera and environment of `scene`. `textures` and
    /// `defaults` must outlive the view. Errors: GPU objects that cannot be created (samplers, uniform buffers).
    [[nodiscard]] static Expected<SceneView> create(const scene::SceneDescription& scene,
                                                    const scene::SceneGeometry& geometry, gl::TextureCache& textures,
                                                    const gl::TextureDefaults& defaults,
                                                    const gl::GLCapabilities& caps);

    /// Renders into `target`: clears to the scene's clear color, sets the camera aspect ratio from the target, writes
    /// FrameUniforms to `frameUniforms`, binds it and the lights block, and draws every part instance with the program
    /// of its material's shader kind, which receives uModel (local -> world).
    void render(gl::GLDevice& device, const SceneShaders& shaders,
                gl::UniformBuffer<gl::FrameUniforms>& frameUniforms, const gl::Framebuffer& target, float timeSeconds);

    /// The view camera: created with the scene camera's lens and pose; the pose is the caller's to drive (free roam),
    /// render() only sets the aspect ratio from the target.
    [[nodiscard]] scene::Camera& camera() noexcept { return m_camera; }

    /// Draw calls issued by render() when both programs are available.
    [[nodiscard]] std::size_t drawCount() const noexcept { return m_draws.size(); }
    [[nodiscard]] std::size_t meshCount() const noexcept { return m_meshes.size(); }
    [[nodiscard]] std::size_t materialCount() const noexcept { return m_materials.size(); }

private:
    struct DrawItem {
        std::size_t mesh = 0; ///< index into m_meshes
        scene::MaterialHandle material;
        glm::mat4 model{1.0f};
    };

    std::vector<gl::GpuMesh> m_meshes;
    gl::MaterialSet m_materials;
    /// Sorted by drawing order (see the class comment).
    std::vector<DrawItem> m_draws;
    gl::UniformBuffer<gl::LightsUniforms> m_lights;
    scene::Camera m_camera;
    glm::vec4 m_clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace rv::app
