#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <rv/devui/dev_ui.hpp>
#include <rv/preview/asset_report.hpp>
#include <rv/preview/preview_overlay.hpp>
#include <rv/preview/preview_scene.hpp>
#include <rv/preview/preview_subject.hpp>
#include <rv/scene/orbit_controller.hpp>

namespace rv::gl {
class TextureCache;
} // namespace rv::gl

// The panels of the asset preview (developer overlay panels, Dear ImGui): thin views over the PreviewApp's state, which
// they read every frame and edit in place.
namespace rv::preview {

/// What the panels show and edit; everything must outlive them.
struct PreviewPanelModel {
    const PreviewSubject& subject;
    const AssetReport& report;
    const std::vector<ScreenSurface>& screens;
    /// Counts the subjects loaded, so that panels caching something derived from the subject rebuild it.
    const std::uint64_t& subjectRevision;
    ViewMode& viewMode;
    OverlaySelection& overlays;
    scene::OrbitController& orbit;
    /// The textures of the scene: the screen pattern's preview.
    gl::TextureCache& textures;
    /// Frames the subject in the view again (F).
    std::function<void()> frameSubject;
    /// Loads the subject's target again (at the next update).
    std::function<void()> reload;
};

/// "Asset": what is shown and its state, the view mode, the overlays, the camera (frame, auto-rotation), the controls.
[[nodiscard]] std::unique_ptr<devui::Panel> makeAssetPanel(PreviewPanelModel model);
/// "Model": the node hierarchy of a glTF model with each node's primitives (triangles, vertices, material) and socket,
/// or the parts and sockets of a prefab.
[[nodiscard]] std::unique_ptr<devui::Panel> makeModelPanel(PreviewPanelModel model);
/// "Statistics": the bounds in centimeters, nodes, meshes, primitives (potential draw calls), triangles, vertices,
/// materials, sockets, the textures with their sizes and estimated GPU memory, the glTF extensions used.
[[nodiscard]] std::unique_ptr<devui::Panel> makeStatisticsPanel(PreviewPanelModel model);
/// "Manifest": the manifest of the subject (id, asset type, category, role, source and license, runtime, tags, hash)
/// and the results of its checks with their severities.
[[nodiscard]] std::unique_ptr<devui::Panel> makeManifestPanel(PreviewPanelModel model);
/// "Screen": every ScreenSurface with its size and aspect, the video surface of the manifest, and the test pattern it
/// shows.
[[nodiscard]] std::unique_ptr<devui::Panel> makeScreenPanel(PreviewPanelModel model);
/// "Warnings": the loader's warnings and what could not be loaded or built.
[[nodiscard]] std::unique_ptr<devui::Panel> makeWarningsPanel(PreviewPanelModel model);

/// Panel titles (DevUi::focusPanel).
inline constexpr const char* kManifestPanelTitle = "Manifest";

} // namespace rv::preview
