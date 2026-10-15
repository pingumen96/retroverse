#pragma once

#include <memory>

#include <rv/app/app_logic.hpp>
#include <rv/preview/preview_options.hpp>

namespace rv::preview {

/// The application logic of retroverse-asset-preview (style §19.3), on the host, renderer, scene instantiation and
/// developer overlay of rv_app: app::SceneGraphics draws an app::SceneInstance of the subject (makePreviewScene: the
/// model viewer's studio around a glTF model, a manifest's model or prefab, a procedural prefab), with
///  - an orbit camera (scene::OrbitController: left drag or right stick orbits, middle or right drag or left
///    stick pans, wheel or triggers zoom; F frames the subject in the part of the view the panels leave visible, R
///    turns the auto-rotation on and off);
///  - the overlays of buildPreviewOverlay as debug lines (floor grid, scale references, bounds, node bounds, pivot,
///    sockets, collision proxies, normals) and their labels, drawn by the developer overlay over the scene;
///  - the view modes of ViewMode (keys 1 to 5): the scene's shading, the inspection views of gl::SurfaceView, or the
///    triangles' edges;
///  - the screens of glTF models showing a test pattern (kScreenMaterialName with kScreenPattern instead of their own
///    material), as the procedural CRT's screen role does;
///  - the panels of preview_panels.hpp and the developer overlay's Materials, Stats and Log panels, always shown (F1
///    hides them), also in screenshots.
/// A file dropped on the window (SDL_EVENT_DROP_FILE) becomes the subject; the subject's files are polled
/// (core::FileWatchSet) and reloaded when they change. F5 reloads the shaders, F12 writes a screenshot; Esc closes a
/// popup, else hides the panels, else quits. Frame-limited runs write --screenshot like retroverse.exe and fail the
/// same way (exit code 3).
class PreviewApp final : public app::IAppLogic {
public:
    explicit PreviewApp(PreviewOptions options);
    ~PreviewApp() override;
    PreviewApp(const PreviewApp&) = delete;
    PreviewApp& operator=(const PreviewApp&) = delete;

    [[nodiscard]] Status onInit(app::AppContext& context) override;
    void onEvent(app::AppContext& context, const SDL_Event& event) override;
    void onUpdate(app::AppContext& context, double deltaSeconds) override;
    void onRender(app::AppContext& context) override;
    void onShutdown(app::AppContext& context) override;

private:
    struct State;

    PreviewOptions m_options;
    std::unique_ptr<State> m_state; ///< GPU resources and the subject; empty before onInit and after onShutdown
};

} // namespace rv::preview
