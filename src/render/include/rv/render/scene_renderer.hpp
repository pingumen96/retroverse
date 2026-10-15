#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/frame_stats.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/instance_buffer.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

class DebugDraw;
class Framebuffer;
class GLDevice;
class MaterialRegistry;
class MeshRegistry;
class ShaderProgram;

/// Programs the SceneRenderer draws with. A null or empty program (a shader that never built) skips the batches of its
/// kind, or the debug lines.
struct SceneShaders {
    const ShaderProgram* pbr = nullptr;        ///< PBR_Base: pbr/pbr.vert + pbr/pbr.frag
    const ShaderProgram* unlit = nullptr;      ///< Unlit_Emissive: pbr/pbr.vert + unlit/unlit_emissive.frag
    const ShaderProgram* debugLines = nullptr; ///< debug/debug_lines.vert + debug/debug_lines.frag
};

/// One view of the scene for SceneRenderer::render().
struct RenderView {
    /// Eye and lens; the aspect ratio is taken from the target.
    scene::Camera camera;
    /// What to draw; the mesh and material handles belong to the renderer's registries.
    std::span<const scene::RenderItem> items;
    /// Scene lights and environment (packLights).
    LightsUniforms lights;
    /// Background: a linear display value (exposed and tonemapped, see gl::tonemap), encoded by the sRGB target.
    glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    /// FrameUniforms::time: simulated seconds.
    float timeSeconds = 0.0f;
    /// Lines drawn over the scene with SceneShaders::debugLines; null for none.
    const DebugDraw* debugDraw = nullptr;
};

/// Batched, instanced scene renderer (spec §14.4). For each view it:
///  - writes FrameUniforms (binding 0: camera, time) and, when they changed since the last view, LightsUniforms
///    (binding 1);
///  - plans the items with scene::BatchPlanner: frustum culling against the registered mesh bounds, opaque batches,
///    then alpha-masked ones, each sorted by shader, material and mesh, then alpha-blended ones back to front;
///  - writes every instance transform of the plan into its InstanceBuffer (persistently mapped, fenced regions) and
///    draws each batch with one glDrawElementsInstancedBaseInstance, binding the program and the material (uniform
///    block range, textures, samplers, blend, depth writes and culling) only when they change;
///  - draws the debug lines of the view, if any, over the scene;
///  - times the view on the GPU (GpuTimer) and records FrameStats.
/// The registries must outlive the renderer. Main thread only; destroy it while the GL context is current.
class SceneRenderer {
public:
    /// Instances per region of the instance buffer at creation; it grows when a view needs more.
    static constexpr std::size_t kInitialInstanceCapacity = 1024;

    /// Creates the uniform buffers, the instance buffer and the GPU timer. Errors: GPU objects that cannot be created.
    [[nodiscard]] static Expected<SceneRenderer> create(MeshRegistry& meshes, const MaterialRegistry& materials);

    /// Renders `view` into `target`: GLDevice::beginPass (clear to the view's clear color), then the passes above.
    /// Leaves the state defaults documented on GLDevice::state() (blending off, depth writes on, back faces culled).
    /// Batches whose mesh, material or program is unavailable are skipped (never a crash).
    void render(GLDevice& device, const SceneShaders& shaders, const RenderView& view, const Framebuffer& target);

    /// Statistics of the last render() call.
    [[nodiscard]] const FrameStats& stats() const noexcept { return m_stats; }

private:
    SceneRenderer(MeshRegistry& meshes, const MaterialRegistry& materials) noexcept
        : m_meshes(&meshes), m_materials(&materials) {}

    /// Writes the frame and lights blocks and binds them; false when they could not be written.
    bool updateUniforms(const RenderView& view, const scene::Camera& camera);
    /// Draws the batches of `plan`, whose instances start at `baseInstance` in the instance buffer.
    void drawBatches(GLDevice& device, const SceneShaders& shaders, const scene::BatchPlan& plan,
                     std::uint32_t baseInstance, FrameStats& stats) const;

    MeshRegistry* m_meshes = nullptr;
    const MaterialRegistry* m_materials = nullptr;
    UniformBuffer<FrameUniforms> m_frameUniforms;
    UniformBuffer<LightsUniforms> m_lightsUniforms;
    /// Contents of m_lightsUniforms, once written.
    std::optional<LightsUniforms> m_uploadedLights;
    InstanceBuffer m_instances;
    GpuTimer m_timer;
    scene::BatchPlanner m_planner;
    FrameStats m_stats;
};

} // namespace rv::gl
