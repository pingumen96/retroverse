#pragma once

#include <memory>

#include <rv/app/app_logic.hpp>

namespace rv::app {

/// The Retroverse application logic. M0 step 10 content: loads the scene selected by --scene (default m0_testroom), or
/// with --model one glTF model in the model viewer (makeModelViewerScene: the model at the origin, a three-point light
/// rig, a camera framing it), turns it into drawable content (SceneInstance: deduplicated meshes and materials, glTF
/// models, render items, lights, collision world) and draws it with the batched, instanced gl::SceneRenderer (PBR and
/// unlit emissive materials, scene textures such as cartridge covers and CRT color bars, the scene's lights with the
/// key light's shadow map, hemispheric ambient darkened by screen-space ambient occlusion after a depth pre-pass) as
/// linear radiance into an HDR target, post-processed (bloom, exposure, vignette, ACES
/// tonemapping, dithering: the post-processing of the scene's environment) into an sRGB target that is copied to the
/// window, from a first-person camera walking the scene (FreeRoam: starts at the scene camera, collides with the walls
/// and furniture). The window and the screenshots each have their own gl::ViewTargets. --debug-draw adds debug lines
/// over the scene: object bounds, socket axes and a world grid.
///
/// Frame statistics (gl::FrameStats: draw calls, instances, triangles, batches, shader switches, culled items, GPU
/// times of the shadow map, the depth pre-pass, the ambient occlusion, the scene pass and the post-processing) and
/// frame times are logged once per second in developer builds (RV_DEV_UI), and for the whole run when a frame-limited
/// run ends.
///
/// Input goes through the action map (assets/config/default_bindings.json, overridden per action by
/// <user data>/config/bindings.json): Move/Look/Sprint drive the walker; Back (Esc, gamepad B) leaves one level
/// (backAction: releases the captured mouse, else closes the developer overlay, else quits); ReloadShaders (F5)
/// reloads the shaders; Screenshot (F12) writes <user data>/screenshots/retroverse-<timestamp>.png at
/// --screenshot-size. Interactive runs capture the mouse when the window has focus, and again on a click into it;
/// frame-limited runs only on a click. --input-script replaces the actions of chosen frames (each finished step logs
/// the pose); frame-limited runs log the final camera pose at exit. Developer builds (RV_DEV_ASSET_ROOT) also reload
/// shaders automatically when their files change. A frame-limited run with --screenshot writes its last frame to that
/// file (RGB: the window shows no alpha); --self-check srgb checks the sRGB output pipeline after initialization
/// (runSrgbSelfCheck), --self-check bloom the bloom chain (runBloomSelfCheck), both with the programs the views use. A
/// frame-limited run fails (exit code 3, AppContext::failRun) when that file is not written
/// (write error, or the run ended before its last frame), when a scene or post-processing shader never built, so
/// frames were never drawn completely, or when the self-check fails. A --input-script that cannot be loaded fails the
/// initialization (exit code 2).
///
/// Developer builds (RV_DEV_UI) add the Dear ImGui developer overlay (devui::DevUi), toggled by ToggleDevUI (F1) and
/// open from the start with --devui-open: dockable panels for frame statistics, the camera and walker, the lights (the
/// key light that casts shadows) and environment, the materials, rendering switches (debug lines, wireframe, vsync,
/// shadow biases, ambient occlusion, exposure, bloom and vignette, shader reload), the log and the scene objects ("go
/// to"). It sees every event first and keeps what it uses; opening it frees
/// a captured mouse for its panels and closing it captures the mouse again. It is drawn last into the window, and into
/// screenshots only with
/// --screenshot-include-ui. --debug-draw starts with every kind of debug line on; the overlay toggles each kind.
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
