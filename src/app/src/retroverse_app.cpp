#include <rv/app/retroverse_app.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <glm/vec2.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/command_line.hpp>
#include <rv/app/screenshot_name.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

#include "scene_view.hpp"

namespace rv::app {
namespace {

#if defined(RV_DEV_ASSET_ROOT)
constexpr bool kShaderHotReload = true;
#else
constexpr bool kShaderHotReload = false;
#endif

constexpr std::string_view kMaterialLibraryPath = "materials/base/materials.json";

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

} // namespace

struct RetroverseApp::State {
    std::unique_ptr<gl::ShaderLibrary> shaders;
    gl::ShaderHandle unlitShader;
    gl::UniformBuffer<gl::FrameUniforms> frameUniforms;
    SceneView scene;
    gl::Framebuffer windowTarget;
    gl::Framebuffer screenshotTarget;
    std::optional<glm::ivec2> failedWindowSize;
    std::optional<glm::ivec2> failedScreenshotSize;
    bool screenshotRequested = false; ///< F12 pressed; written after the next frame is rendered
    bool lastFrameCaptured = false;   ///< --screenshot written

    void renderScene(AppContext& context, const gl::Framebuffer& target) {
        scene.render(context.device(), shaders->get(unlitShader), frameUniforms, target,
                     static_cast<float>(context.clock().totalSeconds()));
    }

    /// Renders the current view at --screenshot-size into the screenshot target and writes it as a PNG.
    Status captureScreenshot(AppContext& context, const std::filesystem::path& path) {
        const CommandLineOptions& options = context.options();
        const glm::ivec2 size(options.screenshotWidth, options.screenshotHeight);
        if (!prepareTarget(screenshotTarget, size, "ScreenshotTarget", failedScreenshotSize)) {
            return makeError(ErrorCode::GraphicsError, "no render target for the screenshot", core::pathToUtf8(path));
        }
        renderScene(context, screenshotTarget);
        gl::Framebuffer::unbind(); // whatever draws next targets the window again
        auto image = screenshotTarget.readPixels();
        if (!image) {
            return propagate(image.error());
        }
        if (auto written = assets::writePng(path, *image); !written) {
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
    const std::filesystem::path scenePath = resolveScenePath(context.options().scene, paths.assetRoot());
    auto description = scene::loadSceneDescription(scenePath);
    if (!description) {
        Error error = description.error();
        error.message = "cannot load the scene: " + error.message;
        return propagate(std::move(error));
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
    auto unlit = state->shaders->load(gl::ShaderDesc{"unlit", "unlit/unlit.vert", "unlit/unlit.frag", {}});
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
    state->scene = SceneView::create(*description, geometry);
    spdlog::info("Scene view: {} mesh(es) on the GPU, {} draw call(s) per view", state->scene.meshCount(),
                 state->scene.drawCount());

    const CommandLineOptions& options = context.options();
    if (options.screenshot) {
        spdlog::info("Screenshot of frame {} at {}x{} will be written to {}", options.frameLimit.value_or(0),
                     options.screenshotWidth, options.screenshotHeight, core::pathToUtf8(*options.screenshot));
    }
    m_state = std::move(state);
    return ok();
}

void RetroverseApp::onEvent(AppContext& /*context*/, const SDL_Event& event) {
    if (!m_state || event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }
    if (event.key.key == SDLK_F5) {
        spdlog::info("F5 pressed, reloading shaders");
        m_state->shaders->reloadAll();
    } else if (event.key.key == SDLK_F12) {
        m_state->screenshotRequested = true;
    }
}

void RetroverseApp::onUpdate(AppContext& /*context*/, double /*deltaSeconds*/) {
    if constexpr (kShaderHotReload) {
        if (m_state) {
            m_state->shaders->pollHotReload();
        }
    }
}

void RetroverseApp::onRender(AppContext& context) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    const glm::ivec2 windowSize = context.framebufferSize();
    if (windowSize.x > 0 && windowSize.y > 0 &&
        prepareTarget(state.windowTarget, windowSize, "WindowTarget", state.failedWindowSize)) {
        state.renderScene(context, state.windowTarget);
        state.windowTarget.blitToDefault(context.device().state(), windowSize);
    }

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
}

void RetroverseApp::onShutdown(AppContext& context) {
    const CommandLineOptions& options = context.options();
    // An unattended run must not pass when it could never draw its scene (its frames and screenshot show nothing).
    if (m_state && options.frameLimit && !m_state->shaders->get(m_state->unlitShader).valid()) {
        context.failRun(Error{ErrorCode::GraphicsError,
                              "the scene was never drawn: shader 'unlit' did not build (see the errors above)",
                              core::pathToUtf8(m_state->shaders->root())});
    }
    if (m_state && options.screenshot && !m_state->lastFrameCaptured && !context.failure()) {
        spdlog::warn("--screenshot: the run ended before its last frame, {} was not written",
                     core::pathToUtf8(*options.screenshot));
    }
    m_state.reset();
}

} // namespace rv::app
