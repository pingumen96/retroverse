#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/core/failure_streak.hpp>
#include <rv/render/frame_stats.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/instance_buffer.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

class DebugDraw;
class GLDevice;
class MaterialRegistry;
class MeshRegistry;
class ShaderProgram;
class ViewTargets;

/// Programs the SceneRenderer draws with. A null or empty program (a shader that never built) skips the batches of its
/// kind, or the debug lines; the post-processing programs fall back as PostShaders documents.
struct SceneShaders {
    const ShaderProgram* pbr = nullptr;        ///< PBR_Base: pbr/pbr.vert + pbr/pbr.frag
    const ShaderProgram* unlit = nullptr;      ///< Unlit_Emissive: pbr/pbr.vert + unlit/unlit_emissive.frag
    const ShaderProgram* debugLines = nullptr; ///< debug/debug_lines.vert + debug/debug_lines.frag
    PostShaders post;                          ///< bloom and the post pass
};

/// One view of the scene for SceneRenderer::render().
struct RenderView {
    /// Eye and lens; the aspect ratio is taken from the target.
    scene::Camera camera;
    /// What to draw; the mesh and material handles belong to the renderer's registries.
    std::span<const scene::RenderItem> items;
    /// Scene lights and ambient (packLights).
    LightsUniforms lights;
    /// Background: linear radiance where nothing is drawn, post-processed like the surfaces (exposed and tonemapped).
    glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    /// Bloom, exposure, vignette and dithering of the view (postSettingsFor() of the scene's environment).
    PostSettings post;
    /// FrameUniforms::time: simulated seconds.
    float timeSeconds = 0.0f;
    /// Lines drawn over the post-processed scene with SceneShaders::debugLines; null for none.
    const DebugDraw* debugDraw = nullptr;
    /// Draws the scene's triangles as their edges (glPolygonMode GL_LINE) to inspect geometry; the clear, the
    /// post-processing and the debug lines are unaffected.
    bool wireframe = false;
};

/// Batched, instanced scene renderer (spec §14.2, §14.4). For each view, through the ViewTargets of its output, it:
///  - clears the HDR target to the view's background radiance (GLDevice::beginPass);
///  - writes FrameUniforms (binding 0: camera, time) and, when they changed since the last view, LightsUniforms
///    (binding 1);
///  - plans the items with scene::BatchPlanner: frustum culling against the registered mesh bounds, opaque batches,
///    then alpha-masked ones, each sorted by shader, material and mesh, then alpha-blended ones back to front (the
///    planner and the plan are kept from view to view, so planning allocates nothing once they have grown);
///  - writes every instance transform of the plan into its InstanceBuffer (persistently mapped, fenced regions) and
///    draws each batch with one glDrawElementsInstancedBaseInstance into the HDR target, binding the program and the
///    material (uniform block range, textures, samplers, blend, depth writes and culling) only when they change; the
///    material shaders write linear radiance, neither exposed nor tonemapped. A view whose instances cannot be written
///    (allocation failure, fence timeout) is not drawn: the failure is logged once, and again only when its cause
///    changes, then the first view drawn again logs how many were skipped;
///  - post-processes the HDR scene into the sRGB output (PostProcessor: bloom, exposure, vignette, the ACES fitted
///    tonemap, dithering);
///  - draws the debug lines of the view, if any, over the output, depth-tested against the scene (whose depth is
///    copied into the output's depth buffer first);
///  - times the scene pass and the post-processing on the GPU (GpuTimer) and records FrameStats.
/// The registries must outlive the renderer. Main thread only; destroy it while the GL context is current.
class SceneRenderer {
public:
    /// Instances per region of the instance buffer at creation; it grows when a view needs more.
    static constexpr std::size_t kInitialInstanceCapacity = 1024;

    /// Creates the uniform buffers, the instance buffer, the post-processor and the GPU timer. Errors: GPU objects that
    /// cannot be created.
    [[nodiscard]] static Expected<SceneRenderer> create(MeshRegistry& meshes, const MaterialRegistry& materials);

    /// Renders `view` through `targets` with the passes above; the image ends in targets.output(), which stays bound
    /// for drawing (overlays) with GL_FRAMEBUFFER_SRGB enabled. Leaves the state defaults documented on
    /// GLDevice::state() (blending off, depth writes on, back faces culled, filled polygons). Batches whose mesh,
    /// material or program is unavailable are skipped (never a crash). Does nothing for empty targets.
    void render(GLDevice& device, const SceneShaders& shaders, const RenderView& view, const ViewTargets& targets);

    /// Statistics of the last render() call.
    [[nodiscard]] const FrameStats& stats() const noexcept { return m_stats; }

private:
    SceneRenderer(MeshRegistry& meshes, const MaterialRegistry& materials) noexcept
        : m_meshes(&meshes), m_materials(&materials) {}

    /// Writes the frame and lights blocks and binds them; false when they could not be written.
    bool updateUniforms(const RenderView& view, const scene::Camera& camera);
    /// Writes the instance transforms of m_plan into the instance buffer and attaches the meshes to it: the base
    /// instance of the plan, or why the view cannot be drawn.
    [[nodiscard]] Expected<std::uint32_t> uploadInstances();
    /// Draws the batches of m_plan, whose instances start at `baseInstance` in the instance buffer.
    void drawBatches(GLDevice& device, const SceneShaders& shaders, std::uint32_t baseInstance,
                     FrameStats& stats) const;

    MeshRegistry* m_meshes = nullptr;
    const MaterialRegistry* m_materials = nullptr;
    UniformBuffer<FrameUniforms> m_frameUniforms;
    UniformBuffer<LightsUniforms> m_lightsUniforms;
    /// Contents of m_lightsUniforms, once written.
    std::optional<LightsUniforms> m_uploadedLights;
    InstanceBuffer m_instances;
    /// Times the scene pass.
    GpuTimer m_timer;
    PostProcessor m_post;
    scene::BatchPlanner m_planner;
    /// Plan of the current view; kept so that its vectors keep their capacity from view to view.
    scene::BatchPlan m_plan;
    /// Views not drawn because their instances could not be uploaded, for the log.
    core::FailureStreak m_uploadFailures;
    FrameStats m_stats;
};

} // namespace rv::gl
