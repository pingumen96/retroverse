#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/devui/dev_ui.hpp>
#include <rv/devui/frame_time_history.hpp>
#include <rv/devui/texture_previews.hpp>

namespace rv::gl {
struct DroppedLight;
struct FrameStats;
class GpuMemoryLedger;
class MaterialRegistry;
class MeshRegistry;
class ShaderLibrary;
struct ShadowLight;
struct ShadowSettings;
class TextureCache;
} // namespace rv::gl

namespace rv::input {
class InputSystem;
} // namespace rv::input

namespace rv::platform {
class GraphicsWindow;
} // namespace rv::platform

namespace rv::scene {
class Camera;
struct EnvironmentSetup;
class FirstPersonController;
struct Light;
struct RenderItem;
struct SceneDescription;
} // namespace rv::scene

// The panels of the developer overlay: thin views over state that lives elsewhere, which they read every frame and
// edit in place. Each model holds references to that state; everything it refers to must outlive the panel (the
// DevUi it is added to).
namespace rv::devui {

/// "Stats": frame rate and times with a graph of the last FrameTimeHistory::kCapacity frames, the GPU times of the
/// shadow map, the depth pre-pass, the ambient occlusion, the lit pass and the post-processing, what the renderer's
/// last view drew (gl::FrameStats), the GPU resources of the registries and the texture cache, and the GPU memory of the
/// renderer's accounting per kind (gl::GpuMemoryLedger: now and the run's peak).
struct StatsPanelModel {
    const FrameTimeHistory& frameTimes;
    /// gl::SceneRenderer::stats(): the last view rendered.
    const gl::FrameStats& renderStats;
    const gl::MeshRegistry& meshes;
    const gl::MaterialRegistry& materials;
    const gl::TextureCache& textures;
    /// gl::gpuMemoryLedger().
    const gl::GpuMemoryLedger& gpuMemory;
    /// The hero CRT's video, one line each (its "Video" section; none without it): the picture shown, the decoder's
    /// state and threads, the frames uploaded (I3 step 11).
    std::function<std::vector<std::string>()> videoStatus = {};
    /// The generated room's collection, one line each (its "Collection" section; none without a room): the layers
    /// resident, where the fronts came from, the cache's hits and misses, the loader's queue, the hero textures (I3
    /// step 12).
    std::function<std::vector<std::string>()> collectionStatus = {};
};
[[nodiscard]] std::unique_ptr<Panel> makeStatsPanel(const StatsPanelModel& model);

/// "Camera": the walker's feet, yaw and pitch (edits teleport it), the horizontal field of view (live), walk and sprint
/// speeds, mouse sensitivity and Y inversion, collision on or off (noclip), and a reset to the scene's camera.
struct CameraPanelModel {
    /// The view camera: its lens is edited, its eye position shown.
    scene::Camera& camera;
    /// The walker that poses the camera every frame.
    scene::FirstPersonController& walker;
    /// Mouse sensitivity and Y inversion (input::InputSettings).
    input::InputSystem& input;
    /// Walls and furniture stop the walker; false walks through them.
    bool& collision;
    /// Puts the walker and the lens back to the scene's camera.
    std::function<void()> resetToSceneCamera;
};
[[nodiscard]] std::unique_ptr<Panel> makeCameraPanel(CameraPanelModel model);

/// "Lights": the key light that casts the shadows (or why there is none), every scene light (position, direction, color
/// shown in sRGB and stored linear, intensity, range, cone angles, whether it casts shadows and how strongly: the first
/// that does is the key light, in a scene with areas the first of the area that holds the eye) and the environment
/// (background radiance, ambient sky, ground and intensity), edited live; the lights the renderer leaves out are listed
/// with the reason. The exposure and the ambient occlusion are the Rendering panel's.
struct LightsPanelModel {
    /// The scene's lights as described (linear colors), edited in place: asked each time the panel is drawn, since the
    /// application may replace its scene (the generated room after its shell, I3 step 10b).
    std::function<std::span<scene::Light>()> lights;
    /// Edited in place.
    scene::EnvironmentSetup& environment;
    /// Lights left out of the lights block by the last apply().
    const std::vector<gl::DroppedLight>& dropped;
    /// The key light of the last apply(), its index in lights() (gl::PackedLights::keyLight: the first light packed
    /// that asks for shadows, of the area of the eye in a scene with areas), and why that light has none.
    const gl::ShadowLight& shadowLight;
    const std::optional<std::size_t>& keyLight;
    const std::string& shadowNote;
    /// Repacks the lights block and the background after an edit.
    std::function<void()> apply;
};
[[nodiscard]] std::unique_ptr<Panel> makeLightsPanel(LightsPanelModel model);

/// "Materials": the materials of the registry with the number of render items that use each (the full name and the
/// registry key in a tooltip), and for the selected one its shading, alpha mode and textures (thumbnails, enlarged with
/// their size, color space and memory in a tooltip: TexturePreviews), its base color (shown in sRGB, stored linear),
/// metallic, roughness and emissive strength, applied to the GPU material at once (gl::MaterialRegistry::setFactors)
/// and never saved; a revert button restores the registered values.
struct MaterialsPanelModel {
    gl::MaterialRegistry& materials;
    /// The items drawn, to count the users of each material; called every frame the panel is drawn.
    std::function<std::span<const scene::RenderItem>()> items;
    /// The cache of the materials' textures (their thumbnails are dropped when it releases its content, also while the
    /// panel is not shown).
    const gl::TextureCache& textures;
};
[[nodiscard]] std::unique_ptr<Panel> makeMaterialsPanel(const MaterialsPanelModel& model);

/// "Rendering": the debug line kinds (world grid, object bounds, socket axes), wireframe for the scene pass, the
/// anti-aliasing of the views, vsync, the key light's shadow (on or off, depth bias, normal offset), the ambient
/// occlusion of the scene's environment (on or off, radius, intensity), its post-processing (exposure, bloom strength
/// and radius, vignette; all live, never saved), and the shaders: a reload button (F5) and the build state of every
/// program with its last compile error.
struct RenderingPanelModel {
    bool& showGrid;
    bool& showBounds;
    bool& showSockets;
    bool& wireframe;
    /// The FXAA pass of the views (gl::PostSettings::antialiasing).
    bool& antialiasing;
    /// Its ambient occlusion, exposure, bloom and vignette are edited in place; the renderer reads them every view.
    scene::EnvironmentSetup& environment;
    /// Edited in place; the renderer reads it every view.
    gl::ShadowSettings& shadows;
    gl::ShaderLibrary& shaders;
    /// Its swap interval (vsync).
    platform::GraphicsWindow& window;
};
[[nodiscard]] std::unique_ptr<Panel> makeRenderingPanel(const RenderingPanelModel& model);

/// "Log": the recent log lines (core::recentLogLines, through a LogView) colored by level, with a minimum level filter
/// and autoscroll.
[[nodiscard]] std::unique_ptr<Panel> makeLogPanel();

/// "Scene": the objects of the scene (id, kind, source, instances) with a "go to" button for each.
struct ScenePanelModel {
    const scene::SceneDescription& scene;
    /// Moves the walker in front of objects[index].
    std::function<void(std::size_t index)> goTo;
};
[[nodiscard]] std::unique_ptr<Panel> makeScenePanel(ScenePanelModel model);

} // namespace rv::devui
