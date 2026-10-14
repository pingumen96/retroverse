#pragma once

#include <memory>

#include <rv/app/app_logic.hpp>

namespace rv::app {

/// The Retroverse application logic. M0 step 4 content: loads the scene selected by --scene (default m0_testroom),
/// builds its procedural geometry and draws it unlit, in flat per-part material colors, from the scene camera into an
/// offscreen sRGB framebuffer that is copied to the window.
///
/// Keys: F5 reloads the shaders; F12 writes <user data>/screenshots/retroverse-<timestamp>.png at --screenshot-size.
/// Developer builds (RV_DEV_ASSET_ROOT) also reload shaders automatically when their files change. A frame-limited
/// run with --screenshot writes its last frame to that file (RGB: the window shows no alpha). A frame-limited run
/// fails (exit code 3, AppContext::failRun) when that file is not written (write error, or the run ended before its
/// last frame) or when the scene shader never built, so nothing was drawn.
class RetroverseApp final : public IAppLogic {
public:
    RetroverseApp();
    ~RetroverseApp() override;
    RetroverseApp(const RetroverseApp&) = delete;
    RetroverseApp& operator=(const RetroverseApp&) = delete;

    [[nodiscard]] Status onInit(AppContext& context) override;
    void onEvent(AppContext& context, const SDL_Event& event) override;
    void onUpdate(AppContext& context, double deltaSeconds) override;
    void onRender(AppContext& context) override;
    void onShutdown(AppContext& context) override;

private:
    struct State;

    std::unique_ptr<State> m_state; ///< GPU resources and scene; empty before onInit and after onShutdown
};

} // namespace rv::app
