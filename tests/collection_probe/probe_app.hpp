#pragma once

#include <memory>

#include <rv/app/app_logic.hpp>
#include <rv/render/surface_view.hpp>

#include "probe_layout.hpp"

namespace rv::probe {

/// What the probe's own options choose (the rest of its command line is retroverse.exe's).
struct ProbeOptions {
    Scenario scenario = Scenario::Chunks;
    /// The surfaces' shading (lit) or the base color inspection view (exact colors: no light, tonemap or FXAA).
    gl::SurfaceView view = gl::SurfaceView::Lit;
    /// The groups scenario: a static instance group or render items.
    DrawPath path = DrawPath::Static;
    /// Where the camera stands.
    CameraMode camera = CameraMode::Wall;
    /// The groups scenario: highlightItem() flagged kHighlighted from kHighlightFrame on (the outline).
    bool highlight = false;
    /// The groups scenario: hiddenItem() flagged kHidden in the static group, left out of the render items.
    bool hide = false;
};

/// rv_collection_probe's logic: a wall of test items (probe_layout.hpp) drawn through gl::CollectionArrays with the
/// renderer of rv_app (SceneGraphics), the run's array format (--no-texture-compression), the layers uploaded under the
/// per-frame budget, and the scenario's growth; in the groups scenario through a gl::StaticInstanceGroup (or render
/// items), with a key light, ambient occlusion, the outline of a highlighted item and a hidden one. A frame-limited run
/// fails (exit 3) when a layer is still pending at its last frame, so that its screenshot always shows every item's own
/// pictures.
class ProbeApp final : public app::IAppLogic {
public:
    explicit ProbeApp(ProbeOptions options);
    ~ProbeApp() override;

    [[nodiscard]] Status onInit(app::AppContext& context) override;
    void onUpdate(app::AppContext& context, double deltaSeconds) override;
    void onRender(app::AppContext& context) override;
    void onShutdown(app::AppContext& context) override;

private:
    struct State;

    ProbeOptions m_options;
    std::unique_ptr<State> m_state;
};

} // namespace rv::probe
