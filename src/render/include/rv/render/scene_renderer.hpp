#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/vec4.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/failure_streak.hpp>
#include <rv/render/frame_stats.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/instance_buffer.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/shadow_map.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/render/ssao_pass.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::gl {

class DebugDraw;
class GLDevice;
class MaterialRegistry;
class MeshRegistry;
class ShaderProgram;
class ViewTargets;

/// Programs the SceneRenderer draws with. A null or empty program (a shader that never built) skips the batches of its
/// kind, or the debug lines; without both depth programs a view has neither shadow, depth pre-pass nor ambient
/// occlusion; the ambient occlusion and post-processing programs fall back as SsaoShaders and PostShaders document.
struct SceneShaders {
    const ShaderProgram* pbr = nullptr;   ///< PBR_Base: pbr/pbr.vert + pbr/pbr.frag
    const ShaderProgram* unlit = nullptr; ///< Unlit_Emissive: pbr/pbr.vert + unlit/unlit_emissive.frag
    /// Depth only, for opaque batches: pbr/pbr.vert + depth/depth.frag (the shadow map and the depth pre-pass).
    const ShaderProgram* depth = nullptr;
    /// Depth only with the alpha test, for alpha-masked batches: depth/depth.frag with RV_ALPHA_TEST.
    const ShaderProgram* depthMasked = nullptr;
    const ShaderProgram* debugLines = nullptr; ///< debug/debug_lines.vert + debug/debug_lines.frag
    SsaoShaders ssao;                          ///< the ambient occlusion passes
    PostShaders post;                          ///< bloom, the post pass and the anti-aliasing
    /// An inspection view (SurfaceView: pbr/pbr.vert + debug/inspect.frag): when set, the lit pass draws every batch
    /// with it instead of its material's program, still binding the material (its textures, factors and alpha mode).
    /// Such a view is meant to be shown without tonemapping (PostSettings::tonemap off). Null for the materials' own
    /// shading.
    const ShaderProgram* inspection = nullptr;
};

/// One view of the scene for SceneRenderer::render().
struct RenderView {
    /// Eye and lens; the aspect ratio is taken from the target.
    scene::Camera camera;
    /// What to draw; the mesh and material handles belong to the renderer's registries.
    std::span<const scene::RenderItem> items;
    /// Scene lights and ambient (packLights).
    LightsUniforms lights;
    /// The key light that casts the view's shadow (PackedLights::shadowLight); none when no light casts shadows.
    ShadowLight shadowLight;
    /// How the shadow map is drawn and read: on or off, depth bias, normal offset.
    ShadowSettings shadowSettings;
    /// World bounds of the items that cast shadows: a directional key light's map is fitted to them.
    core::Aabb shadowCasterBounds;
    /// Ambient occlusion of the view (the scene environment's ssao).
    scene::SsaoSetup ssao;
    /// Background: linear radiance where nothing is drawn, post-processed like the surfaces (exposed and tonemapped).
    glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    /// Bloom, exposure, vignette, dithering and anti-aliasing of the view (postSettingsFor() of the scene's
    /// environment).
    PostSettings post;
    /// FrameUniforms::time: simulated seconds.
    float timeSeconds = 0.0f;
    /// Lines drawn over the post-processed scene with SceneShaders::debugLines; null for none.
    const DebugDraw* debugDraw = nullptr;
    /// Draws the scene's triangles as their edges (glPolygonMode GL_LINE) to inspect geometry, without depth pre-pass
    /// or ambient occlusion (hidden edges stay visible); the clear, the post-processing and the debug lines are
    /// unaffected.
    bool wireframe = false;
};

/// Batched, instanced scene renderer (spec §14.2, §14.4) with the key light's shadow and ambient occlusion (style
/// §4.1 contact and shadow cues). For each view, through the ViewTargets of its output, it:
///  - writes FrameUniforms (binding 0: camera, time) and, when they changed since the last view, LightsUniforms
///    (binding 1) and ShadowUniforms (binding 4);
///  - plans the items with scene::BatchPlanner: frustum culling against the registered mesh bounds, opaque batches,
///    then alpha-masked ones, each sorted by shader, material and mesh, then alpha-blended ones back to front; when the
///    view has a key light (RenderView::shadowLight, shadowSettings.enabled), also the shadow casters
///    (PlanOptions::shadowCastersOnly) against the frustum of its shadow map (shadowProjectionFor: a perspective map
///    for a spot light, an orthographic one fitted to RenderView::shadowCasterBounds for the directional light). The
///    planner and both plans are kept from view to view, so planning allocates nothing once they have grown;
///  - writes the instance transforms of both plans into its InstanceBuffer in one write (persistently mapped, fenced
///    regions). A view whose instances cannot be written (allocation failure, fence timeout) is not drawn: the failure
///    is logged once, and again only when its cause changes, then the first view drawn again logs how many were
///    skipped;
///  - draws the shadow map (ShadowMap, kShadowMapResolution texels square) with the depth programs from the light,
///    the caster triangles pushed back by the slope-scaled depth bias of the settings (glPolygonOffset), culled like
///    their materials (double-sided ones on both sides);
///  - clears the HDR target to the view's background radiance (GLDevice::beginPass) and draws the depth of the opaque
///    and alpha-masked batches into it (depth pre-pass: the lit pass then shades every pixel once, and the ambient
///    occlusion reads this depth);
///  - computes the ambient occlusion from that depth (SsaoPass: half-resolution estimate, depth-aware blur) into the
///    targets' ambientOcclusion();
///  - draws each batch with one glDrawElementsInstancedBaseInstance into the HDR target (GL_LEQUAL against the
///    pre-pass, without depth writes), binding the program and the material (uniform block range, textures,
///    samplers, blend and culling) only when they change; the lit shader reads the shadow map and the ambient
///    occlusion (SceneTextureUnit) and writes linear radiance, neither exposed nor tonemapped;
///  - post-processes the HDR scene into the sRGB output (PostProcessor: bloom, exposure, vignette, the ACES fitted
///    tonemap, dithering);
///  - draws the debug lines of the view, if any, over the output, depth-tested against the scene (whose depth is
///    copied into the output's depth buffer first);
///  - times the shadow map, the depth pre-pass, the ambient occlusion, the lit pass and the post-processing on the GPU
///    (GpuTimer) and records FrameStats.
/// The registries must outlive the renderer. Main thread only; destroy it while the GL context is current.
class SceneRenderer {
public:
    /// Instances per region of the instance buffer at creation; it grows when a view needs more. A view writes the
    /// instances of its plan and of its shadow map into one region (instancing_stress: 1001 + 1000, so it grows on the
    /// first frame).
    static constexpr std::size_t kInitialInstanceCapacity = 1024;

    /// Creates the uniform buffers, the instance buffer, the shadow map, the ambient occlusion pass, the
    /// post-processor and the GPU timers. Errors: GPU objects that cannot be created.
    [[nodiscard]] static Expected<SceneRenderer> create(MeshRegistry& meshes, const MaterialRegistry& materials);

    /// Renders `view` through `targets` with the passes above; the image ends in targets.output(), which stays bound
    /// for drawing (overlays) with GL_FRAMEBUFFER_SRGB enabled. Leaves the state defaults documented on
    /// GLDevice::state() (depth test with GL_LESS, depth writes, color writes, blending off, back faces culled, no
    /// polygon offset, filled polygons). Batches whose mesh, material or program is unavailable are skipped (never a
    /// crash). Does nothing for empty targets.
    void render(GLDevice& device, const SceneShaders& shaders, const RenderView& view, const ViewTargets& targets);

    /// Statistics of the last render() call.
    [[nodiscard]] const FrameStats& stats() const noexcept { return m_stats; }
    /// Instance matrices per region of the instance buffer: kInitialInstanceCapacity until a view needs more.
    [[nodiscard]] std::size_t instanceCapacity() const noexcept { return m_instances.regionCapacity(); }

private:
    /// Draw calls, instances and triangles of a pass.
    struct DrawCounts {
        std::size_t drawCalls = 0;
        std::size_t instances = 0;
        std::size_t triangles = 0;
        std::size_t shaderSwitches = 0;
        std::size_t materialSwitches = 0;
    };

    SceneRenderer(MeshRegistry& meshes, const MaterialRegistry& materials) noexcept
        : m_meshes(&meshes), m_materials(&materials) {}

    /// Writes the frame, lights and shadow blocks (and the frame block of the shadow map when `shadow` is set) and
    /// binds them; false when they could not be written.
    bool updateUniforms(const RenderView& view, const scene::Camera& camera,
                        const std::optional<ShadowProjection>& shadow);
    /// Writes the instance transforms of m_plan, then those of m_shadowPlan when `withShadow`, into the instance
    /// buffer and attaches the meshes to it: the base instance of m_plan (m_shadowPlan's follow it), or why the view
    /// cannot be drawn.
    [[nodiscard]] Expected<std::uint32_t> uploadInstances(bool withShadow);
    /// Draws the shadow casters of m_shadowPlan, whose instances start at `baseInstance`, into the shadow map.
    void drawShadowMap(GLDevice& device, const SceneShaders& shaders, std::uint32_t baseInstance,
                       const ShadowSettings& settings, FrameStats& stats);
    /// Draws the depth of the opaque and alpha-masked batches of `plan` with the depth programs; blended batches cast
    /// and occlude nothing. The caller sets the target and the depth state.
    void drawDepthBatches(GLDevice& device, const SceneShaders& shaders, const scene::BatchPlan& plan,
                          std::uint32_t baseInstance, DrawCounts& counts) const;
    /// Draws the batches of m_plan with their materials' programs; `afterPrePass`: the depth is in place already, so
    /// nothing writes depth.
    void drawShadedBatches(GLDevice& device, const SceneShaders& shaders, std::uint32_t baseInstance, bool afterPrePass,
                           DrawCounts& counts) const;

    MeshRegistry* m_meshes = nullptr;
    const MaterialRegistry* m_materials = nullptr;
    UniformBuffer<FrameUniforms> m_frameUniforms;
    /// The frame block of the shadow map: the key light's view (time 0: the depth programs read none, so the block
    /// changes only with the light), read by the depth program while the map is drawn.
    UniformBuffer<FrameUniforms> m_shadowFrameUniforms;
    UniformBuffer<LightsUniforms> m_lightsUniforms;
    UniformBuffer<ShadowUniforms> m_shadowUniforms;
    /// Contents of m_lightsUniforms, m_shadowUniforms and m_shadowFrameUniforms, once written.
    std::optional<LightsUniforms> m_uploadedLights;
    std::optional<ShadowUniforms> m_uploadedShadow;
    std::optional<FrameUniforms> m_uploadedShadowFrame;
    InstanceBuffer m_instances;
    ShadowMap m_shadowMap;
    /// Time the shadow map, the depth pre-pass and the lit pass.
    GpuTimer m_shadowTimer;
    GpuTimer m_depthTimer;
    GpuTimer m_timer;
    SsaoPass m_ssao;
    PostProcessor m_post;
    scene::BatchPlanner m_planner;
    /// Plans of the current view and of its shadow map; kept so that their vectors keep their capacity.
    scene::BatchPlan m_plan;
    scene::BatchPlan m_shadowPlan;
    /// Views not drawn because their instances could not be uploaded, for the log.
    core::FailureStreak m_uploadFailures;
    FrameStats m_stats;
};

} // namespace rv::gl
