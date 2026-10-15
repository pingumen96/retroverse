#pragma once

#include <memory>

#include <rv/app/app_logic.hpp>

namespace rv::app {

/// The Retroverse application logic. M0 step 6 content: loads the scene selected by --scene (default m0_testroom),
/// builds its procedural geometry and draws it lit (SceneView: PBR and unlit emissive materials, scene textures such as
/// cartridge covers and CRT color bars, the scene's lights and hemispheric ambient, exposure and ACES tonemapping)
/// into an offscreen sRGB framebuffer that is copied to the window, from a first-person camera walking the scene
/// (FreeRoam: starts at the scene camera, collides with the walls and furniture).
///
/// Input goes through the action map (assets/config/default_bindings.json, overridden per action by
/// <user data>/config/bindings.json): Move/Look/Sprint drive the walker; Back (Esc, gamepad B) releases the captured
/// mouse, or quits when the mouse is free; ReloadShaders (F5) reloads the shaders; Screenshot (F12) writes
/// <user data>/screenshots/retroverse-<timestamp>.png at --screenshot-size. Interactive runs capture the mouse when the
/// window has focus, and again on a click into it; frame-limited runs only on a click. --input-script replaces the
/// actions of chosen frames (each finished step logs the pose); frame-limited runs log the final camera pose at exit.
/// Developer builds (RV_DEV_ASSET_ROOT) also reload shaders automatically when their files change. A frame-limited
/// run with --screenshot writes its last frame to that file (RGB: the window shows no alpha); --self-check srgb checks
/// the sRGB output pipeline after initialization (runSrgbSelfCheck). A frame-limited run fails (exit code 3,
/// AppContext::failRun) when that file is not written (write error, or the run ended before its last frame), when a
/// scene shader never built, so surfaces were never drawn, or when the self-check fails. A --input-script that cannot
/// be loaded fails the initialization (exit code 2).
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
