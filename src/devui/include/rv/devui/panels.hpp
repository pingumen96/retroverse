#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <rv/devui/dev_ui.hpp>
#include <rv/devui/frame_time_history.hpp>

namespace rv::gl {
struct DroppedLight;
struct FrameStats;
class MaterialRegistry;
class MeshRegistry;
class ShaderLibrary;
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

/// "Stats": frame rate and times with a graph of the last FrameTimeHistory::kCapacity frames, GPU time, what the
/// renderer's last view drew (gl::FrameStats) and the GPU resources of the registries and the texture cache.
struct StatsPanelModel {
    const FrameTimeHistory& frameTimes;
    /// gl::SceneRenderer::stats(): the last view rendered.
    const gl::FrameStats& renderStats;
    const gl::MeshRegistry& meshes;
    const gl::MaterialRegistry& materials;
    const gl::TextureCache& textures;
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

/// "Lights": every scene light (position, direction, color shown in sRGB and stored linear, intensity, range, cone
/// angles, shadow flag) and the environment (ambient sky, ground and intensity, exposure, background radiance), edited
/// live; the lights the renderer leaves out are listed with the reason.
struct LightsPanelModel {
    /// The scene's lights as described (linear colors), edited in place.
    std::span<scene::Light> lights;
    /// Edited in place.
    scene::EnvironmentSetup& environment;
    /// Lights left out of the lights block by the last apply().
    const std::vector<gl::DroppedLight>& dropped;
    /// Repacks the lights block and the background after an edit.
    std::function<void()> apply;
};
[[nodiscard]] std::unique_ptr<Panel> makeLightsPanel(LightsPanelModel model);

/// "Materials": the materials of the registry with the number of render items that use each, and for the selected one
/// its base color (shown in sRGB, stored linear), metallic, roughness and emissive strength, applied to the GPU
/// material at once (gl::MaterialRegistry::setFactors) and never saved; a revert button restores the registered values.
struct MaterialsPanelModel {
    gl::MaterialRegistry& materials;
    /// The items drawn, to count the users of each material.
    std::span<const scene::RenderItem> items;
};
[[nodiscard]] std::unique_ptr<Panel> makeMaterialsPanel(const MaterialsPanelModel& model);

/// "Rendering": the debug line kinds (world grid, object bounds, socket axes), wireframe for the scene pass, vsync,
/// and the shaders: a reload button (F5) and the build state of every program with its last compile error.
struct RenderingPanelModel {
    bool& showGrid;
    bool& showBounds;
    bool& showSockets;
    bool& wireframe;
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
