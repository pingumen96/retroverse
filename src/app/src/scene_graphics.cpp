#include <rv/app/scene_graphics.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <glm/vec2.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/frame_stats_log.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/screenshot_name.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/render/debug_draw.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::app {
namespace {

#if defined(RV_DEV_ASSET_ROOT)
constexpr bool kShaderHotReload = true;
#else
constexpr bool kShaderHotReload = false;
#endif

#if defined(RV_DEV_UI)
/// Developer builds (with the developer overlay) log the frame statistics once per second.
constexpr bool kDeveloperBuild = true;
#else
constexpr bool kDeveloperBuild = false;
#endif

constexpr std::string_view kMaterialLibraryPath = "materials/base/materials.json";
/// Anisotropic filtering of textures and material samplers, and of the checker that replaces missing textures (plan
/// 5.4: 8x), clamped to the device limit.
constexpr float kTextureAnisotropy = 8.0f;
/// The inspection views, in the order of gl::kSurfaceViews without Lit.
constexpr std::array<gl::SurfaceView, 3> kInspectionViews{gl::SurfaceView::BaseColor, gl::SurfaceView::Normals,
                                                          gl::SurfaceView::UvChecker};

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

/// The programs of the scene, its shadow and ambient occlusion and its post-processing (ShaderLibrary handles), loaded
/// at startup.
struct ScenePrograms {
    gl::ShaderHandle pbr;
    gl::ShaderHandle unlit;
    gl::ShaderHandle depth;
    gl::ShaderHandle depthMasked;
    gl::ShaderHandle ssao;
    gl::ShaderHandle ssaoBlur;
    gl::ShaderHandle bloomDownsampleFirst;
    gl::ShaderHandle bloomDownsample;
    gl::ShaderHandle bloomUpsample;
    gl::ShaderHandle post;
    gl::ShaderHandle antialiasing;
};

/// Loads the programs of ScenePrograms. A program that does not build is logged and stays empty until a reload
/// succeeds (ShaderLibrary::load); errors only for an invalid description.
Expected<ScenePrograms> loadScenePrograms(gl::ShaderLibrary& library) {
    ScenePrograms programs;
    const std::pair<gl::ShaderHandle ScenePrograms::*, gl::ShaderDesc> descriptions[] = {
        {&ScenePrograms::pbr, {"pbr", "pbr/pbr.vert", "pbr/pbr.frag", {}}},
        {&ScenePrograms::unlit, {"unlit_emissive", "pbr/pbr.vert", "unlit/unlit_emissive.frag", {}}},
        {&ScenePrograms::depth, {"depth", "pbr/pbr.vert", "depth/depth.frag", {}}},
        {&ScenePrograms::depthMasked, {"depth_masked", "pbr/pbr.vert", "depth/depth.frag", {"RV_ALPHA_TEST"}}},
        {&ScenePrograms::ssao, {"ssao", "post/fullscreen.vert", "ssao/ssao.frag", {}}},
        {&ScenePrograms::ssaoBlur, {"ssao_blur", "post/fullscreen.vert", "ssao/ssao_blur.frag", {}}},
        {&ScenePrograms::bloomDownsampleFirst,
         {"bloom_downsample_karis", "post/fullscreen.vert", "post/bloom_downsample.frag", {"RV_BLOOM_KARIS"}}},
        {&ScenePrograms::bloomDownsample,
         {"bloom_downsample", "post/fullscreen.vert", "post/bloom_downsample.frag", {}}},
        {&ScenePrograms::bloomUpsample, {"bloom_upsample", "post/fullscreen.vert", "post/bloom_upsample.frag", {}}},
        {&ScenePrograms::post, {"post", "post/fullscreen.vert", "post/post.frag", {}}},
        {&ScenePrograms::antialiasing, {"fxaa", "post/fullscreen.vert", "post/fxaa.frag", {}}},
    };
    for (const auto& [handle, description] : descriptions) {
        auto loaded = library.load(description);
        if (!loaded) {
            return propagate(loaded.error());
        }
        programs.*handle = *loaded;
    }
    return programs;
}

/// Creates the render targets of an output (`targets`) at `size`, or resizes them. Failures are logged once per size so
/// a persistent one does not flood the log at the frame rate.
bool prepareTargets(gl::ViewTargets& targets, glm::ivec2 size, std::string_view debugName,
                    std::optional<glm::ivec2>& failedSize) {
    Status status = ok();
    if (!targets.valid()) {
        auto created = gl::ViewTargets::create(size, debugName);
        if (created) {
            targets = std::move(*created);
        } else {
            status = propagate(created.error());
        }
    } else {
        status = targets.resize(size);
    }
    if (status) {
        failedSize.reset();
        return true;
    }
    if (failedSize != size) {
        spdlog::error("Render targets '{}' unavailable at {}x{}: {}", debugName, size.x, size.y,
                      status.error().toString());
        failedSize = size;
    }
    return false;
}

/// Index of an inspection view in kInspectionViews.
std::size_t inspectionIndex(gl::SurfaceView view) noexcept {
    for (std::size_t index = 0; index < kInspectionViews.size(); ++index) {
        if (kInspectionViews[index] == view) {
            return index;
        }
    }
    return 0;
}

} // namespace

gl::RenderView makeRenderView(const SceneInstance& scene, const scene::Camera& camera) {
    const scene::EnvironmentSetup& environment = scene.environment();
    gl::RenderView view;
    view.camera = camera;
    view.items = scene.renderItems();
    view.lights = scene.lights();
    view.shadowLight = scene.shadowLight();
    view.shadowCasterBounds = scene.shadowCasterBounds();
    view.ssao = environment.ssao;
    view.clearColor = glm::vec4(environment.clearColor, 1.0f);
    view.post = gl::postSettingsFor(environment);
    return view;
}

struct SceneGraphics::State {
    explicit State(assets::MaterialLibrary materialLibrary) : library(std::move(materialLibrary)) {}

    assets::MaterialLibrary library;
    std::unique_ptr<gl::ShaderLibrary> shaders;
    ScenePrograms programs;
    std::optional<gl::ShaderHandle> debugShader; ///< loaded when debug lines are first set
    std::array<std::optional<gl::ShaderHandle>, kInspectionViews.size()> inspectionShaders; ///< loaded when first shown
    gl::SurfaceView surfaceView = gl::SurfaceView::Lit;
    // Declared before the registries and the renderer, which refer to them, so that they are destroyed after.
    std::unique_ptr<gl::TextureDefaults> textureDefaults;
    std::unique_ptr<gl::TextureCache> textures;
    gl::MaterialRegistry materials;
    gl::MeshRegistry meshes;
    std::optional<gl::SceneRenderer> renderer;
    int uniformBufferOffsetAlignment = 256;
    std::optional<gl::DebugDraw> debugDraw; ///< created when debug lines are first set
    bool debugLinesShown = false;
    std::function<void(gl::GLDevice&)> windowOverlay;
    std::function<void(gl::GLDevice&, const gl::Framebuffer&)> screenshotOverlay;
    /// Render targets of the window's view and of screenshots, each kept at its size (allocated on first use).
    gl::ViewTargets windowTargets;
    gl::ViewTargets screenshotTargets;
    std::optional<glm::ivec2> failedWindowSize;
    std::optional<glm::ivec2> failedScreenshotSize;
    FrameStatsLog statsLog;
    bool lastFrameCaptured = false; ///< --screenshot written
    bool viewRendered = false;      ///< a view was rendered during the current frame (window or screenshot)
    bool anyViewRendered = false;   ///< a view was rendered during the run

    /// Renders `view` into `targets` with the programs of the views, the debug lines and the surface view.
    void render(gl::GLDevice& device, const gl::RenderView& view, const gl::ViewTargets& targets) {
        gl::SceneShaders shaderSet;
        shaderSet.pbr = &shaders->get(programs.pbr);
        shaderSet.unlit = &shaders->get(programs.unlit);
        shaderSet.depth = &shaders->get(programs.depth);
        shaderSet.depthMasked = &shaders->get(programs.depthMasked);
        shaderSet.debugLines = debugShader ? &shaders->get(*debugShader) : nullptr;
        shaderSet.ssao.occlusion = &shaders->get(programs.ssao);
        shaderSet.ssao.blur = &shaders->get(programs.ssaoBlur);
        shaderSet.post = postShaders();
        gl::RenderView shown = view;
        shown.debugDraw = debugDraw && debugLinesShown ? &*debugDraw : nullptr;
        if (surfaceView != gl::SurfaceView::Lit) {
            // The inspection programs write the colors to display: nothing may darken, shift or tonemap them. The
            // background is the one the lit view displays, so that switching views keeps the backdrop.
            shaderSet.inspection = &shaders->get(*inspectionShaders[inspectionIndex(surfaceView)]);
            shown.clearColor = glm::vec4(gl::displayColor(glm::vec3(view.clearColor), view.post), view.clearColor.a);
            shown.post.tonemap = false;
            shown.ssao.enabled = false;
            shown.shadowSettings.enabled = false;
        }
        renderer->render(device, shaderSet, shown, targets);
        viewRendered = true;
        anyViewRendered = true;
    }

    [[nodiscard]] gl::PostShaders postShaders() const {
        gl::PostShaders post;
        post.bloomDownsampleFirst = &shaders->get(programs.bloomDownsampleFirst);
        post.bloomDownsample = &shaders->get(programs.bloomDownsample);
        post.bloomUpsample = &shaders->get(programs.bloomUpsample);
        post.post = &shaders->get(programs.post);
        post.antialiasing = &shaders->get(programs.antialiasing);
        return post;
    }

    /// Renders `view` at --screenshot-size into the screenshot targets, with the overlay when it shows in screenshots,
    /// and writes it as an RGB PNG.
    Status captureScreenshot(AppContext& context, const gl::RenderView& view, const std::filesystem::path& path) {
        const CommandLineOptions& options = context.options();
        const glm::ivec2 size(options.screenshotWidth, options.screenshotHeight);
        if (!prepareTargets(screenshotTargets, size, "ScreenshotTargets", failedScreenshotSize)) {
            return makeError(ErrorCode::GraphicsError, "no render targets for the screenshot", core::pathToUtf8(path));
        }
        render(context.device(), view, screenshotTargets);
        if (screenshotOverlay) {
            screenshotOverlay(context.device(), screenshotTargets.output());
        }
        gl::Framebuffer::unbind(); // the frame leaves the default framebuffer bound, also when the window is not drawn
        auto image = screenshotTargets.output().readPixels();
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

    /// Name of a program of the views that never built, or empty when all did.
    [[nodiscard]] std::string_view missingProgram() const {
        for (std::size_t index = 0; index < shaders->size(); ++index) {
            if (const gl::ShaderStatus status = shaders->status(index); !status.valid) {
                return status.name;
            }
        }
        return {};
    }
};

SceneGraphics::SceneGraphics(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}

SceneGraphics::~SceneGraphics() = default;

Expected<std::unique_ptr<SceneGraphics>> SceneGraphics::create(AppContext& context) {
    const core::Paths& paths = context.paths();
    auto state = std::make_unique<State>(loadMaterials(paths));
    state->shaders = std::make_unique<gl::ShaderLibrary>(paths.assetRoot() / "shaders");
    auto programs = loadScenePrograms(*state->shaders);
    if (!programs) {
        return propagate(programs.error());
    }
    state->programs = *programs;
    spdlog::info("Shader hot reload {}; F5 reloads every shader",
                 kShaderHotReload ? "on (sources checked every 500 ms)" : "off in this build");

    const gl::GLCapabilities& caps = context.device().caps();
    const float anisotropy = gl::clampAnisotropy(kTextureAnisotropy, caps.maxAnisotropy);
    auto textureDefaults = gl::TextureDefaults::create(anisotropy);
    if (!textureDefaults) {
        return propagate(textureDefaults.error());
    }
    state->textureDefaults = std::make_unique<gl::TextureDefaults>(std::move(*textureDefaults));
    state->textures = std::make_unique<gl::TextureCache>(paths.assetRoot(), *state->textureDefaults, anisotropy);
    auto materials = gl::MaterialRegistry::create(*state->textureDefaults, anisotropy);
    if (!materials) {
        return propagate(materials.error());
    }
    state->materials = std::move(*materials);
    auto renderer = gl::SceneRenderer::create(state->meshes, state->materials);
    if (!renderer) {
        return propagate(renderer.error());
    }
    state->renderer.emplace(std::move(*renderer));
    state->uniformBufferOffsetAlignment = caps.uniformBufferOffsetAlignment;

    const CommandLineOptions& options = context.options();
    if (options.screenshot && options.frameLimit) {
        spdlog::info("Screenshot of the last frame (frame {}, counted from 0) at {}x{} will be written to {}",
                     *options.frameLimit - 1, options.screenshotWidth, options.screenshotHeight,
                     core::pathToUtf8(*options.screenshot));
    }
    return std::unique_ptr<SceneGraphics>(new SceneGraphics(std::move(state)));
}

const assets::MaterialLibrary& SceneGraphics::materialLibrary() const noexcept {
    return m_state->library;
}

SceneResources SceneGraphics::resources() noexcept {
    return SceneResources{m_state->meshes, m_state->materials, *m_state->textures,
                          m_state->uniformBufferOffsetAlignment};
}

void SceneGraphics::clearContent() noexcept {
    // Materials first: they name the textures.
    m_state->materials.clear();
    m_state->meshes.clear();
    m_state->textures->clearContent();
}

gl::ShaderLibrary& SceneGraphics::shaders() noexcept {
    return *m_state->shaders;
}

gl::MeshRegistry& SceneGraphics::meshes() noexcept {
    return m_state->meshes;
}

gl::MaterialRegistry& SceneGraphics::materials() noexcept {
    return m_state->materials;
}

gl::TextureCache& SceneGraphics::textures() noexcept {
    return *m_state->textures;
}

const gl::TextureDefaults& SceneGraphics::textureDefaults() const noexcept {
    return *m_state->textureDefaults;
}

const gl::FrameStats& SceneGraphics::renderStats() const noexcept {
    return m_state->renderer->stats();
}

gl::PostShaders SceneGraphics::postShaders() const noexcept {
    return m_state->postShaders();
}

const gl::ShaderProgram& SceneGraphics::unlitProgram() const noexcept {
    return m_state->shaders->get(m_state->programs.unlit);
}

Status SceneGraphics::setDebugLines(const scene::DebugLines& lines) {
    State& state = *m_state;
    state.debugLinesShown = false;
    if (lines.empty()) {
        return ok();
    }
    if (!state.debugShader) {
        auto debug =
            state.shaders->load(gl::ShaderDesc{"debug_lines", "debug/debug_lines.vert", "debug/debug_lines.frag", {}});
        if (!debug) {
            return propagate(debug.error());
        }
        state.debugShader = *debug;
    }
    if (!state.debugDraw) {
        auto draw = gl::DebugDraw::create("DebugLines");
        if (!draw) {
            return propagate(draw.error());
        }
        state.debugDraw = std::move(*draw);
    }
    if (auto uploaded = state.debugDraw->setLines(lines); !uploaded) {
        return propagate(uploaded.error());
    }
    state.debugLinesShown = true;
    return ok();
}

Status SceneGraphics::setSurfaceView(gl::SurfaceView view) {
    State& state = *m_state;
    if (view != gl::SurfaceView::Lit) {
        std::optional<gl::ShaderHandle>& handle = state.inspectionShaders[inspectionIndex(view)];
        if (!handle) {
            auto loaded = state.shaders->load(gl::ShaderDesc{std::string(gl::inspectionProgramName(view)),
                                                             "pbr/pbr.vert",
                                                             "debug/inspect.frag",
                                                             {std::string(gl::inspectionDefine(view))}});
            if (!loaded) {
                return propagate(loaded.error());
            }
            handle = *loaded;
        }
    }
    if (view != state.surfaceView) {
        spdlog::info("Surfaces show: {}", gl::toString(view));
    }
    state.surfaceView = view;
    return ok();
}

gl::SurfaceView SceneGraphics::surfaceView() const noexcept {
    return m_state->surfaceView;
}

void SceneGraphics::setOverlay(std::function<void(gl::GLDevice&)> window,
                               std::function<void(gl::GLDevice&, const gl::Framebuffer&)> screenshot) {
    m_state->windowOverlay = std::move(window);
    m_state->screenshotOverlay = std::move(screenshot);
}

void SceneGraphics::renderFrame(AppContext& context, const gl::RenderView& view, bool screenshotRequested) {
    State& state = *m_state;
    state.viewRendered = false;
    // Screenshots are rendered before the window view, so the frame ends with the window blit and the state it
    // documents (default framebuffer with the window's viewport, GL_FRAMEBUFFER_SRGB off) for whatever draws next.
    if (screenshotRequested) {
        const std::filesystem::path path =
            context.paths().screenshotsDir() / screenshotFileName(currentLocalTimestamp());
        if (auto written = state.captureScreenshot(context, view, path); !written) {
            spdlog::error("Screenshot failed: {}", written.error().toString());
        }
    }
    const CommandLineOptions& options = context.options();
    const bool lastFrame = options.frameLimit && context.frameIndex() + 1 == *options.frameLimit;
    if (options.screenshot && lastFrame) {
        if (auto written = state.captureScreenshot(context, view, *options.screenshot); written) {
            state.lastFrameCaptured = true;
        } else {
            Error error = written.error();
            error.message = "--screenshot failed: " + error.message;
            context.failRun(std::move(error));
        }
    }

    const glm::ivec2 windowSize = context.framebufferSize();
    if (windowSize.x > 0 && windowSize.y > 0) {
        if (prepareTargets(state.windowTargets, windowSize, "WindowTargets", state.failedWindowSize)) {
            state.render(context.device(), view, state.windowTargets);
            state.windowTargets.output().blitToDefault(context.device().state(), windowSize);
        } else {
            // No scene this frame (logged): a black window, with the overlay and its log still over it.
            gl::Framebuffer::clearDefault(context.device().state(), windowSize);
        }
        // Last, over the scene in the window.
        if (state.windowOverlay) {
            state.windowOverlay(context.device());
        }
    }

    // The statistics of the last view rendered this frame: the window's, or the screenshot's when the window is empty.
    // A frame that rendered no view (a minimized window) counts its time only: the renderer's statistics still
    // describe an earlier view.
    if (state.viewRendered) {
        state.statsLog.record(context.clock().deltaSeconds(), state.renderer->stats());
    } else {
        state.statsLog.recordWithoutView(context.clock().deltaSeconds());
    }
    if constexpr (kDeveloperBuild) {
        if (const std::optional<std::string> summary = state.statsLog.takeIntervalSummary()) {
            spdlog::info("{}", *summary);
        }
    }
}

void SceneGraphics::reloadShaders() {
    spdlog::info("Reloading shaders");
    m_state->shaders->reloadAll();
}

void SceneGraphics::pollShaderReload() {
    if constexpr (kShaderHotReload) {
        m_state->shaders->pollHotReload();
    }
}

void SceneGraphics::finishRun(AppContext& context) {
    const CommandLineOptions& options = context.options();
    const State& state = *m_state;
    RunReport& report = context.report();
    if (state.anyViewRendered) {
        report.lastView = state.renderer->stats();
    }
    report.instanceCapacity = state.renderer->instanceCapacity();
    if (options.screenshot && state.lastFrameCaptured) {
        report.screenshot = ScreenshotRecord{*options.screenshot, options.screenshotWidth, options.screenshotHeight};
    }
    if (!options.frameLimit) {
        return;
    }
    if (const std::optional<std::string> summary = state.statsLog.runSummary("Frame stats of the run")) {
        spdlog::info("{}", *summary);
    }
    // An unattended run must not pass when it could never draw its scene completely (its frames and screenshot miss
    // surfaces, the bloom or the tonemap).
    if (const std::string_view missing = state.missingProgram(); !missing.empty()) {
        context.failRun(Error{ErrorCode::GraphicsError,
                              fmt::format("the scene was never drawn completely: shader '{}' did not build (see the "
                                          "errors above)",
                                          missing),
                              core::pathToUtf8(state.shaders->root())});
    }
    // Nor when it ended (Esc, window closed, quit request) before the last frame, which --screenshot captures: a file
    // left at that path by an earlier run must not pass for this run's result.
    if (options.screenshot && !state.lastFrameCaptured && !context.failure()) {
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("--screenshot was not written: the run ended after {} of {} frame(s)",
                                          context.frameIndex(), options.frameLimit.value_or(0)),
                              core::pathToUtf8(*options.screenshot)});
    }
}

} // namespace rv::app
