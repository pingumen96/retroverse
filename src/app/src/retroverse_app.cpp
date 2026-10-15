#include <rv/app/retroverse_app.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <glm/vec2.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/command_line.hpp>
#include <rv/app/screenshot_name.hpp>
#include <rv/app/self_check.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action.hpp>
#include <rv/input/input_script.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

#include "app_input.hpp"
#include "free_roam.hpp"
#include "scene_view.hpp"
#include "srgb_self_check.hpp"

namespace rv::app {
namespace {

#if defined(RV_DEV_ASSET_ROOT)
constexpr bool kShaderHotReload = true;
#else
constexpr bool kShaderHotReload = false;
#endif

constexpr std::string_view kMaterialLibraryPath = "materials/base/materials.json";
/// Anisotropic filtering of textures and material samplers, and of the checker that replaces missing textures (plan
/// 5.4: 8x), clamped to the device limit.
constexpr float kTextureAnisotropy = 8.0f;

/// Creates `target` at `size`, or resizes it. Failures are logged once per size so a persistent one does not flood the
/// log at the frame rate.
bool prepareTarget(gl::Framebuffer& target, glm::ivec2 size, std::string_view debugName,
                   std::optional<glm::ivec2>& failedSize) {
    Status status = ok();
    if (!target.valid()) {
        auto created =
            gl::Framebuffer::create(gl::FramebufferDesc{size, gl::ColorFormat::Srgb8Alpha8, true}, debugName);
        if (created) {
            target = std::move(*created);
        } else {
            status = propagate(created.error());
        }
    } else {
        status = target.resize(size);
    }
    if (status) {
        failedSize.reset();
        return true;
    }
    if (failedSize != size) {
        spdlog::error("Render target '{}' unavailable at {}x{}: {}", debugName, size.x, size.y,
                      status.error().toString());
        failedSize = size;
    }
    return false;
}

assets::MaterialLibrary loadMaterials(const core::Paths& paths) {
    auto library = assets::MaterialLibrary::loadFromFile(paths.assetRoot() / core::pathFromUtf8(kMaterialLibraryPath));
    if (library) {
        return std::move(*library);
    }
    assets::MaterialLibrary builtin;
    spdlog::error("Material library unavailable, every material falls back to {}: {}", builtin.fallback().name,
                  library.error().toString());
    return builtin;
}

/// --self-check srgb: logs the outcome; a check that fails, or cannot run, fails the run (exit code 3).
void runSrgbCheck(AppContext& context, const gl::TextureDefaults& defaults, const std::filesystem::path& shaderRoot) {
    const auto result = runSrgbSelfCheck(context.device(), defaults, shaderRoot);
    if (!result) {
        Error error = result.error();
        error.message = "sRGB self-check could not run: " + error.message;
        context.failRun(std::move(error));
        return;
    }
    const std::string summary =
        fmt::format("linear {} stored as {}..{} in {} pixel(s), expected {} +- {}", kSrgbCheckLinearValue,
                    result->minimum, result->maximum, result->pixelCount, result->expected, kSrgbCheckTolerance);
    if (result->passed) {
        spdlog::info("sRGB self-check passed: {}", summary);
    } else {
        context.failRun(Error{ErrorCode::ValidationFailed, "sRGB self-check failed: " + summary, "--self-check=srgb"});
    }
}

/// Loads the --input-script file, if any, and reports what it will do.
Expected<std::optional<input::InputScript>> loadInputScript(const CommandLineOptions& options) {
    if (!options.inputScript) {
        return std::optional<input::InputScript>{};
    }
    auto script = input::InputScript::loadFromFile(*options.inputScript);
    if (!script) {
        Error error = script.error();
        error.message = "cannot load the input script: " + error.message;
        return propagate(std::move(error));
    }
    spdlog::info("Input script {}: {} step(s) up to frame {}", core::pathToUtf8(*options.inputScript),
                 script->steps().size(), script->lastFrame());
    if (options.frameLimit && script->lastFrame() >= *options.frameLimit) {
        spdlog::warn("Input script: the run stops after frame {} (--frames {}), so frames up to {} never run",
                     *options.frameLimit - 1, *options.frameLimit, script->lastFrame());
    }
    if (!options.fixedDeltaSeconds) {
        spdlog::warn("Input script without --fixed-dt: scripted movement depends on the measured frame times");
    }
    return std::optional<input::InputScript>(std::move(*script));
}

} // namespace

struct RetroverseApp::State {
    std::unique_ptr<gl::ShaderLibrary> shaders;
    gl::ShaderHandle pbrShader;
    gl::ShaderHandle unlitShader;
    gl::UniformBuffer<gl::FrameUniforms> frameUniforms;
    // Declared before the scene view, whose materials bind them, so that they are destroyed after it.
    std::unique_ptr<gl::TextureDefaults> textureDefaults;
    std::unique_ptr<gl::TextureCache> textures;
    std::optional<SceneView> scene;
    std::optional<FreeRoam> freeRoam;
    std::unique_ptr<AppInput> input;
    gl::Framebuffer windowTarget;
    gl::Framebuffer screenshotTarget;
    std::optional<glm::ivec2> failedWindowSize;
    std::optional<glm::ivec2> failedScreenshotSize;
    /// Sum of the update steps: the shader time, so that --fixed-dt runs render the same frames on every run.
    double simulatedSeconds = 0.0;
    bool screenshotRequested = false; ///< Screenshot action pressed; captured during the next render
    bool lastFrameCaptured = false;   ///< --screenshot written

    /// Logs the pose at the end of every input script step that ends on `frame`.
    void logScriptProgress(std::uint64_t frame) const {
        const input::InputScript* script = input->script();
        if (script == nullptr) {
            return;
        }
        const std::span<const input::InputScriptStep> steps = script->steps();
        for (std::size_t index = 0; index < steps.size(); ++index) {
            if (steps[index].toFrame == frame) {
                spdlog::info("Input script step {} of {} (frames {}-{}) done: {}", index + 1, steps.size(),
                             steps[index].fromFrame, steps[index].toFrame, freeRoam->describePose());
            }
        }
    }

    void renderScene(AppContext& context, const gl::Framebuffer& target) {
        const SceneShaders programs{&shaders->get(pbrShader), &shaders->get(unlitShader)};
        scene->render(context.device(), programs, frameUniforms, target, static_cast<float>(simulatedSeconds));
    }

    /// Name of a scene program that never built, or empty when both did.
    [[nodiscard]] std::string_view missingSceneShader() const {
        if (!shaders->get(pbrShader).valid()) {
            return "pbr";
        }
        if (!shaders->get(unlitShader).valid()) {
            return "unlit_emissive";
        }
        return {};
    }

    /// Renders the current view at --screenshot-size into the screenshot target and writes it as an RGB PNG.
    Status captureScreenshot(AppContext& context, const std::filesystem::path& path) {
        const CommandLineOptions& options = context.options();
        const glm::ivec2 size(options.screenshotWidth, options.screenshotHeight);
        if (!prepareTarget(screenshotTarget, size, "ScreenshotTarget", failedScreenshotSize)) {
            return makeError(ErrorCode::GraphicsError, "no render target for the screenshot", core::pathToUtf8(path));
        }
        renderScene(context, screenshotTarget);
        gl::Framebuffer::unbind(); // onRender leaves the default framebuffer bound, also when the window is not drawn
        auto image = screenshotTarget.readPixels();
        if (!image) {
            return propagate(image.error());
        }
        // The window is opaque: it shows the stored color whatever alpha the passes wrote (a material's alpha, say).
        // Dropping the alpha channel makes the PNG show the same pixels.
        if (auto written = assets::writePng(path, assets::convertChannels(*image, 3)); !written) {
            return propagate(written.error());
        }
        spdlog::info("Screenshot written: {} ({}x{})", core::pathToUtf8(path), size.x, size.y);
        return ok();
    }
};

RetroverseApp::RetroverseApp() = default;

RetroverseApp::~RetroverseApp() = default;

Status RetroverseApp::onInit(AppContext& context) {
    const core::Paths& paths = context.paths();
    const CommandLineOptions& options = context.options();
    const std::filesystem::path scenePath = resolveScenePath(options.scene, paths.assetRoot());
    auto description = scene::loadSceneDescription(scenePath);
    if (!description) {
        Error error = description.error();
        error.message = "cannot load the scene: " + error.message;
        return propagate(std::move(error));
    }
    auto script = loadInputScript(options);
    if (!script) {
        return propagate(std::move(script.error()));
    }

    const assets::MaterialLibrary materials = loadMaterials(paths);
    const scene::SceneGeometry geometry = scene::buildSceneGeometry(*description, materials);
    spdlog::info("Scene '{}' loaded from {}: {} object(s), {} instance(s), {} mesh(es), {} triangle(s) per view, "
                 "{} placeholder(s), {} part(s) with the fallback material",
                 description->name, core::pathToUtf8(scenePath), geometry.objects.size(), geometry.instanceCount(),
                 geometry.meshCount(), geometry.drawnTriangleCount(), geometry.placeholderCount(),
                 geometry.fallbackMaterialCount());

    auto state = std::make_unique<State>();
    state->shaders = std::make_unique<gl::ShaderLibrary>(paths.assetRoot() / "shaders");
    auto pbr = state->shaders->load(gl::ShaderDesc{"pbr", "pbr/pbr.vert", "pbr/pbr.frag", {}});
    if (!pbr) {
        return propagate(pbr.error());
    }
    state->pbrShader = *pbr;
    auto unlit =
        state->shaders->load(gl::ShaderDesc{"unlit_emissive", "pbr/pbr.vert", "unlit/unlit_emissive.frag", {}});
    if (!unlit) {
        return propagate(unlit.error());
    }
    state->unlitShader = *unlit;
    spdlog::info("Shader hot reload {}; F5 reloads every shader",
                 kShaderHotReload ? "on (sources checked every 500 ms)" : "off in this build");

    auto frameUniforms = gl::UniformBuffer<gl::FrameUniforms>::create("FrameUniforms");
    if (!frameUniforms) {
        return propagate(frameUniforms.error());
    }
    state->frameUniforms = std::move(*frameUniforms);

    const gl::GLCapabilities& caps = context.device().caps();
    const float anisotropy = gl::clampAnisotropy(kTextureAnisotropy, caps.maxAnisotropy);
    auto textureDefaults = gl::TextureDefaults::create(anisotropy);
    if (!textureDefaults) {
        return propagate(textureDefaults.error());
    }
    state->textureDefaults = std::make_unique<gl::TextureDefaults>(std::move(*textureDefaults));
    state->textures = std::make_unique<gl::TextureCache>(paths.assetRoot(), *state->textureDefaults, anisotropy);
    auto sceneView = SceneView::create(*description, geometry, *state->textures, *state->textureDefaults, caps);
    if (!sceneView) {
        return propagate(sceneView.error());
    }
    state->scene.emplace(std::move(*sceneView));
    spdlog::info("Scene view: {} mesh(es) on the GPU, {} material(s), {} draw call(s) per view",
                 state->scene->meshCount(), state->scene->materialCount(), state->scene->drawCount());
    state->freeRoam.emplace(*description, geometry);

    state->input = std::make_unique<AppInput>(context.window(), loadBindings(paths), std::move(*script));
    // Frame-limited runs may be unattended: they never take the user's mouse (and stray motion never reaches them).
    if (!options.frameLimit) {
        state->input->captureMouseWhenFocused();
    }

    if (options.screenshot && options.frameLimit) {
        spdlog::info("Screenshot of the last frame (frame {}, counted from 0) at {}x{} will be written to {}",
                     *options.frameLimit - 1, options.screenshotWidth, options.screenshotHeight,
                     core::pathToUtf8(*options.screenshot));
    }
    if (options.selfCheck == SelfCheck::Srgb) {
        runSrgbCheck(context, *state->textureDefaults, state->shaders->root());
    }
    m_state = std::move(state);
    return ok();
}

void RetroverseApp::onEvent(AppContext& /*context*/, const SDL_Event& event) {
    if (m_state) {
        m_state->input->handleEvent(event);
    }
}

void RetroverseApp::onUpdate(AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    const std::uint64_t frame = context.frameIndex();
    state.simulatedSeconds += deltaSeconds;
    const input::ActionState actions = state.input->update(frame);

    if (actions.pressed(input::Action::Back)) {
        if (state.input->mouseCaptured()) {
            state.input->releaseMouse("Back pressed; press it again to quit");
        } else {
            spdlog::info("Back pressed, quitting");
            context.requestQuit();
        }
    }
    if (actions.pressed(input::Action::ReloadShaders)) {
        spdlog::info("Reloading shaders");
        state.shaders->reloadAll();
    }
    if (actions.pressed(input::Action::Screenshot)) {
        state.screenshotRequested = true;
    }

    state.freeRoam->update(actions, static_cast<float>(deltaSeconds), state.scene->camera());
    state.logScriptProgress(frame);

    if constexpr (kShaderHotReload) {
        state.shaders->pollHotReload();
    }
}

void RetroverseApp::onRender(AppContext& context) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    // Screenshots are rendered before the window view, so the frame ends with the window blit and the state it
    // documents (default framebuffer with the window's viewport, GL_FRAMEBUFFER_SRGB off) for whatever draws next.
    if (state.screenshotRequested) {
        state.screenshotRequested = false;
        const std::filesystem::path path =
            context.paths().screenshotsDir() / screenshotFileName(currentLocalTimestamp());
        if (auto written = state.captureScreenshot(context, path); !written) {
            spdlog::error("Screenshot failed: {}", written.error().toString());
        }
    }

    const CommandLineOptions& options = context.options();
    const bool lastFrame = options.frameLimit && context.frameIndex() + 1 == *options.frameLimit;
    if (options.screenshot && lastFrame) {
        if (auto written = state.captureScreenshot(context, *options.screenshot); written) {
            state.lastFrameCaptured = true;
        } else {
            Error error = written.error();
            error.message = "--screenshot failed: " + error.message;
            context.failRun(std::move(error));
        }
    }

    const glm::ivec2 windowSize = context.framebufferSize();
    if (windowSize.x > 0 && windowSize.y > 0 &&
        prepareTarget(state.windowTarget, windowSize, "WindowTarget", state.failedWindowSize)) {
        state.renderScene(context, state.windowTarget);
        state.windowTarget.blitToDefault(context.device().state(), windowSize);
    }
}

void RetroverseApp::onShutdown(AppContext& context) {
    const CommandLineOptions& options = context.options();
    if (m_state && options.frameLimit) {
        m_state->freeRoam->logFinalPose(context.frameIndex(), m_state->simulatedSeconds);
    }
    // An unattended run must not pass when it could never draw its scene (its frames and screenshot miss surfaces).
    if (m_state && options.frameLimit && !m_state->missingSceneShader().empty()) {
        context.failRun(Error{ErrorCode::GraphicsError,
                              fmt::format("the scene was never drawn completely: shader '{}' did not build (see the "
                                          "errors above)",
                                          m_state->missingSceneShader()),
                              core::pathToUtf8(m_state->shaders->root())});
    }
    // Nor when it ended (Esc, window closed, quit request) before the last frame, which --screenshot captures: a file
    // left at that path by an earlier run must not pass for this run's result.
    if (m_state && options.screenshot && !m_state->lastFrameCaptured && !context.failure()) {
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("--screenshot was not written: the run ended after {} of {} frame(s)",
                                          context.frameIndex(), options.frameLimit.value_or(0)),
                              core::pathToUtf8(*options.screenshot)});
    }
    m_state.reset();
}

} // namespace rv::app
