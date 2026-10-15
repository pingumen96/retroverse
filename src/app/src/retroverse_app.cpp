#include <rv/app/retroverse_app.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <glm/vec2.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/back_action.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/debug_line_selection.hpp>
#include <rv/app/frame_stats_log.hpp>
#include <rv/app/model_viewer.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/app/screenshot_name.hpp>
#include <rv/app/self_check.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action.hpp>
#include <rv/input/input_script.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/render/debug_draw.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/debug_lines.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

#if defined(RV_DEV_UI)
#include <rv/devui/dev_ui.hpp>
#include <rv/devui/frame_time_history.hpp>
#include <rv/devui/overlay_input.hpp>
#include <rv/devui/panels.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/viewpoint.hpp>
#endif

#include "app_input.hpp"
#include "free_roam.hpp"
#include "srgb_self_check.hpp"

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
/// Where the developer overlay keeps its window layout, under <user data>/config.
constexpr std::string_view kOverlayLayoutFile = "imgui.ini";
/// Anisotropic filtering of textures and material samplers, and of the checker that replaces missing textures (plan
/// 5.4: 8x), clamped to the device limit.
constexpr float kTextureAnisotropy = 8.0f;
/// Reference grid of --debug-draw in scenes: 1 m cells over 20 x 20 m around the origin.
constexpr DebugGrid kSceneDebugGrid{10.0f, 1.0f};

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

/// Width / height of a viewport, 16:9 for an empty one.
float aspectOf(glm::ivec2 size) noexcept {
    return size.x > 0 && size.y > 0 ? static_cast<float>(size.x) / static_cast<float>(size.y) : 16.0f / 9.0f;
}

/// "world grid (1 m cells out to 10 m), object bounds" for the kinds of `selection`.
std::string describeDebugLines(const DebugLineSelection& selection, const DebugGrid& grid) {
    std::string kinds;
    const auto add = [&kinds](std::string_view kind) {
        kinds += kinds.empty() ? "" : ", ";
        kinds += kind;
    };
    if (selection.grid) {
        add(fmt::format("world grid ({:g} m cells out to {:g} m)", grid.spacing, grid.halfExtent));
    }
    if (selection.bounds) {
        add("object bounds");
    }
    if (selection.sockets) {
        add("socket axes");
    }
    return kinds;
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

/// What the application shows: a scene file (--scene), or one glTF model in the model viewer (--model), with the loader
/// of its models.
struct SceneSource {
    scene::SceneDescription description;
    scene::ModelLoader loadModel;
    /// Reference grid of --debug-draw.
    DebugGrid grid = kSceneDebugGrid;
};

void logModel(const std::filesystem::path& file, const assets::ModelData& model) {
    spdlog::info("{}", describeModel(core::pathToUtf8(file), model));
    for (const geometry::Socket& socket : model.sockets) {
        const glm::vec3 position(socket.transform[3]);
        spdlog::info("Model socket '{}' ({}, node '{}') at ({:.3f}, {:.3f}, {:.3f})", socket.id,
                     geometry::toString(socket.kind), socket.nodeName, position.x, position.y, position.z);
    }
}

/// --model: the viewer scene around the model file. A file that cannot be loaded is logged and shown as the magenta
/// placeholder (style §21), framed as such.
SceneSource modelViewerSource(const std::filesystem::path& file) {
    std::shared_ptr<const assets::ModelData> model;
    std::optional<Error> failure;
    core::Aabb bounds = scene::placeholderBounds();
    if (auto loaded = assets::loadGltf(file)) {
        model = std::make_shared<const assets::ModelData>(std::move(*loaded));
        logModel(file, *model);
        if (!model->drawItems.empty()) {
            bounds = model->bounds;
        }
    } else {
        spdlog::error("Model {} cannot be loaded and is shown as a placeholder: {}", core::pathToUtf8(file),
                      loaded.error().toString());
        failure = loaded.error();
    }
    SceneSource source;
    source.description = makeModelViewerScene(core::pathToUtf8(file), bounds);
    source.loadModel = [model, failure](std::string_view) -> Expected<std::shared_ptr<const assets::ModelData>> {
        if (model) {
            return model;
        }
        return propagate(*failure);
    };
    source.grid = modelViewerGrid(bounds);
    return source;
}

Expected<SceneSource> loadSceneSource(const CommandLineOptions& options, const core::Paths& paths) {
    if (options.model) {
        return modelViewerSource(*options.model);
    }
    const std::filesystem::path scenePath = resolveScenePath(options.scene, paths.assetRoot());
    auto description = scene::loadSceneDescription(scenePath);
    if (!description) {
        Error error = description.error();
        error.message = "cannot load the scene: " + error.message;
        return propagate(std::move(error));
    }
    spdlog::info("Scene '{}' loaded from {}", description->name, core::pathToUtf8(scenePath));
    SceneSource source;
    source.description = std::move(*description);
    source.loadModel = scene::assetModelLoader(paths.assetRoot());
    return source;
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
    std::optional<gl::ShaderHandle> debugShader; ///< loaded when debug lines are first shown
    // Declared before the scene content and the renderer, which refer to them, so that they are destroyed after.
    std::unique_ptr<gl::TextureDefaults> textureDefaults;
    std::unique_ptr<gl::TextureCache> textures;
    gl::MaterialRegistry materials;
    gl::MeshRegistry meshes;
    std::optional<SceneInstance> scene;
    std::optional<gl::SceneRenderer> renderer;
    std::optional<gl::DebugDraw> debugDraw; ///< created when debug lines are first shown
    /// The scene as described: its camera (start, reset) and objects (the developer overlay's list).
    scene::SceneDescription description;
    /// Reference grid of the debug lines.
    DebugGrid debugGrid;
    /// Debug lines wanted (--debug-draw, the developer overlay) and those debugDraw holds.
    DebugLineSelection debugLines;
    DebugLineSelection shownDebugLines;
    /// Developer overlay toggles: the scene pass as wireframe, collision of the walker (off: noclip).
    bool wireframe = false;
    bool collision = true;
    /// ToggleDevUI pressed in a run without the overlay: logged once.
    bool overlayNoticeLogged = false;
    /// The view camera: the scene camera's lens and pose, then driven by free roam.
    scene::Camera camera;
    std::optional<FreeRoam> freeRoam;
    std::unique_ptr<AppInput> input;
    gl::Framebuffer windowTarget;
    gl::Framebuffer screenshotTarget;
    std::optional<glm::ivec2> failedWindowSize;
    std::optional<glm::ivec2> failedScreenshotSize;
    FrameStatsLog statsLog;
    /// Sum of the update steps: the shader time, so that --fixed-dt runs render the same frames on every run.
    double simulatedSeconds = 0.0;
    bool screenshotRequested = false; ///< Screenshot action pressed; captured during the next render
    bool lastFrameCaptured = false;   ///< --screenshot written
    bool viewRendered = false;        ///< renderScene() ran during the current onRender (window or screenshot)
#if defined(RV_DEV_UI)
    devui::FrameTimeHistory frameTimes;
    devui::CaptureHandover captureHandover;
    /// The developer overlay. Declared last: destroyed first, before the state its panels refer to.
    std::unique_ptr<devui::DevUi> devUi;
#endif

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
        gl::SceneShaders programs;
        programs.pbr = &shaders->get(pbrShader);
        programs.unlit = &shaders->get(unlitShader);
        programs.debugLines = debugShader ? &shaders->get(*debugShader) : nullptr;
        gl::RenderView view;
        view.camera = camera;
        view.items = scene->renderItems();
        view.lights = scene->lights();
        view.clearColor = scene->clearColor();
        view.timeSeconds = static_cast<float>(simulatedSeconds);
        view.debugDraw = debugDraw && shownDebugLines.any() ? &*debugDraw : nullptr;
        view.wireframe = wireframe;
        renderer->render(context.device(), programs, view, target);
        viewRendered = true;
    }

    /// Name of a program the scene draws with that never built, or empty when all did.
    [[nodiscard]] std::string_view missingSceneShader() const {
        if (!shaders->get(pbrShader).valid()) {
            return "pbr";
        }
        if (!shaders->get(unlitShader).valid()) {
            return "unlit_emissive";
        }
        if (debugShader && !shaders->get(*debugShader).valid()) {
            return "debug_lines";
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
#if defined(RV_DEV_UI)
        if (devUi && options.screenshotIncludeUi) {
            devUi->renderTo(context.device(), screenshotTarget);
        }
#endif
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

    /// Shows the debug lines of `debugLines` (--debug-draw, the developer overlay's toggles) when they differ from
    /// those shown: loads the debug program and creates the line buffer on first use, then uploads the selected kinds
    /// (the reference grid, then the bounds and sockets of the scene's objects). A failure is reported once per change;
    /// no lines are drawn then.
    Status syncDebugLines() {
        if (debugLines == shownDebugLines) {
            return ok();
        }
        shownDebugLines = debugLines;
        if (!debugLines.any()) {
            spdlog::info("Debug lines off");
            return ok();
        }
        if (!debugShader) {
            auto debug =
                shaders->load(gl::ShaderDesc{"debug_lines", "debug/debug_lines.vert", "debug/debug_lines.frag", {}});
            if (!debug) {
                return propagate(debug.error());
            }
            debugShader = *debug;
        }
        if (!debugDraw) {
            auto draw = gl::DebugDraw::create("DebugLines");
            if (!draw) {
                return propagate(draw.error());
            }
            debugDraw = std::move(*draw);
        }
        const scene::DebugLines lines =
            composeDebugLines(debugLines, debugGrid, scene->boundsLines(), scene->socketLines());
        if (auto uploaded = debugDraw->setLines(lines); !uploaded) {
            return propagate(uploaded.error());
        }
        spdlog::info("Debug lines: {} line(s): {}", lines.lineCount(), describeDebugLines(debugLines, debugGrid));
        return ok();
    }

    [[nodiscard]] bool overlayVisible() const noexcept {
#if defined(RV_DEV_UI)
        return devUi && devUi->visible();
#else
        return false;
#endif
    }

    /// A text field or a dragged value of the overlay has the keyboard.
    [[nodiscard]] bool overlayHasKeyboard() const noexcept {
#if defined(RV_DEV_UI)
        return devUi && devUi->hasKeyboard();
#else
        return false;
#endif
    }

    /// ToggleDevUI (F1).
    void toggleOverlay() {
#if defined(RV_DEV_UI)
        if (devUi) {
            setOverlayVisible(!devUi->visible());
            return;
        }
#endif
        if (!overlayNoticeLogged) {
            overlayNoticeLogged = true;
            spdlog::info("ToggleDevUI: no developer overlay in this run ({})",
                         kDeveloperBuild ? "it could not start, see the error above" : "not part of this build");
        }
    }

    /// Shows or hides the developer overlay, handing the mouse over: opening it frees a captured mouse for the panels,
    /// closing it gives first-person look the capture back.
    void setOverlayVisible([[maybe_unused]] bool visible) {
#if defined(RV_DEV_UI)
        if (!devUi || devUi->visible() == visible) {
            return;
        }
        if (visible) {
            if (captureHandover.open(input->mouseCaptured())) {
                input->releaseMouse("developer overlay opened; F1 or Esc closes it");
            }
        } else if (captureHandover.close(input->mouseCaptured())) {
            input->captureMouseWhenFocused();
        }
        devUi->setVisible(visible);
        devUi->setMouseCaptured(input->mouseCaptured());
#endif
    }

    /// Back (Esc, gamepad B): leaves one level (backAction).
    void back(AppContext& context) {
        switch (backAction(input->mouseCaptured(), overlayVisible(), overlayHasKeyboard())) {
        case BackAction::Ignore:
            break;
        case BackAction::ReleaseMouse:
            input->releaseMouse(overlayVisible() ? "Back pressed; press it again to close the developer overlay"
                                                 : "Back pressed; press it again to quit");
            break;
        case BackAction::CloseOverlay:
            setOverlayVisible(false);
            break;
        case BackAction::Quit:
            spdlog::info("Back pressed, quitting");
            context.requestQuit();
            break;
        }
    }

#if defined(RV_DEV_UI)
    /// Creates the developer overlay and its panels. When it cannot start, the error is logged and the run goes on
    /// without it.
    void createDevUi(AppContext& context) {
        devui::DevUiConfig config;
        config.iniFile = context.paths().configDir() / core::pathFromUtf8(kOverlayLayoutFile);
        auto created = devui::DevUi::create(context.window(), config);
        if (!created) {
            spdlog::error("Developer overlay unavailable: {}", created.error().toString());
            return;
        }
        devUi = std::move(*created);
        platform::GraphicsWindow& window = context.window();
        devUi->addPanel(devui::makeStatsPanel({frameTimes, renderer->stats(), meshes, materials, *textures}),
                        devui::DockSlot::Left);
        devUi->addPanel(devui::makeCameraPanel({camera, freeRoam->controller(), input->system(), collision,
                                                [this] { resetToSceneCamera(); }}),
                        devui::DockSlot::LeftBottom);
        // Tabs of one node; the first is shown.
        devUi->addPanel(devui::makeLightsPanel({scene->sceneLights(), scene->environment(), scene->droppedLights(),
                                                [this] { scene->applyLighting(); }}),
                        devui::DockSlot::Right);
        devUi->addPanel(devui::makeMaterialsPanel({materials, scene->renderItems()}), devui::DockSlot::Right);
        devUi->addPanel(
            devui::makeScenePanel(
                {description, [this, &window](std::size_t index) { goTo(index, aspectOf(window.drawableSize())); }}),
            devui::DockSlot::Right);
        devUi->addPanel(devui::makeRenderingPanel(
                            {debugLines.grid, debugLines.bounds, debugLines.sockets, wireframe, *shaders, window}),
                        devui::DockSlot::RightBottom);
        devUi->addPanel(devui::makeLogPanel(), devui::DockSlot::Bottom);
    }

    /// Records the frame time and builds the overlay frame; its panels edit the state the frame then simulates and
    /// renders.
    void updateOverlay(const AppContext& context) {
        frameTimes.record(context.clock().deltaSeconds());
        if (!devUi) {
            return;
        }
        devUi->setMouseCaptured(input->mouseCaptured());
        devUi->buildFrame(overlayCaptureSize(context));
    }

    /// Size of the screenshot the overlay is drawn into this frame (--screenshot-include-ui), if any.
    [[nodiscard]] std::optional<glm::ivec2> overlayCaptureSize(const AppContext& context) const {
        const CommandLineOptions& options = context.options();
        const bool lastFrameShot =
            options.screenshot && options.frameLimit && context.frameIndex() + 1 == *options.frameLimit;
        if (!options.screenshotIncludeUi || (!screenshotRequested && !lastFrameShot)) {
            return std::nullopt;
        }
        return glm::ivec2(options.screenshotWidth, options.screenshotHeight);
    }

    /// The overlay's "reset to scene camera": the walker and the lens as the scene starts them.
    void resetToSceneCamera() {
        freeRoam->teleportToCamera(description.camera);
        camera.setLens(description.camera.lens);
        spdlog::info("Camera reset to the scene camera: {}", freeRoam->describePose());
    }

    /// The overlay's "go to": the walker stands in front of objects[index], looking at it (scene::frontViewpoint over
    /// the bounds of all its instances, for a view of `aspect`).
    void goTo(std::size_t index, float aspect) {
        if (index >= description.objects.size() || index >= scene->objectBounds().size()) {
            return;
        }
        const scene::SceneObject& object = description.objects[index];
        const core::Aabb& local = scene->objectBounds()[index];
        core::Aabb bounds;
        for (const glm::mat4& instance : object.instanceTransforms()) {
            bounds.expand(local.transformed(instance));
        }
        scene::FirstPersonController& walker = freeRoam->controller();
        const scene::WalkerPose pose =
            scene::frontViewpoint(bounds, scene::frontDirection(object.transform), walker.feetPosition().y,
                                  walker.settings().eyeHeight, camera.lens(), aspect);
        walker.teleport(pose.feet, pose.yaw, pose.pitch);
        spdlog::info("Go to '{}': {}", object.id, freeRoam->describePose());
    }
#endif
};

RetroverseApp::RetroverseApp() = default;

RetroverseApp::~RetroverseApp() = default;

Status RetroverseApp::onInit(AppContext& context) {
    const core::Paths& paths = context.paths();
    const CommandLineOptions& options = context.options();
    auto source = loadSceneSource(options, paths);
    if (!source) {
        return propagate(source.error());
    }
    auto script = loadInputScript(options);
    if (!script) {
        return propagate(std::move(script.error()));
    }
    const assets::MaterialLibrary library = loadMaterials(paths);

    auto state = std::make_unique<State>();
    state->description = std::move(source->description);
    state->debugGrid = source->grid;
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

    const SceneResources resources{state->meshes, state->materials, *state->textures,
                                   caps.uniformBufferOffsetAlignment};
    auto sceneInstance = SceneInstance::create(state->description, library, source->loadModel, resources);
    if (!sceneInstance) {
        return propagate(sceneInstance.error());
    }
    state->scene.emplace(std::move(*sceneInstance));
    auto renderer = gl::SceneRenderer::create(state->meshes, state->materials);
    if (!renderer) {
        return propagate(renderer.error());
    }
    state->renderer.emplace(std::move(*renderer));
    if (options.debugDraw) {
        state->debugLines = kAllDebugLines;
        if (auto shown = state->syncDebugLines(); !shown) {
            return propagate(shown.error());
        }
    }

    const scene::CameraSetup& camera = state->description.camera;
    state->camera.setLens(camera.lens);
    state->camera.setPose(camera.position, core::degreesToRadians(camera.yawDeg),
                          core::degreesToRadians(camera.pitchDeg));
    state->freeRoam.emplace(state->description, state->scene->collisionWorld());

    state->input = std::make_unique<AppInput>(context.window(), loadBindings(paths), std::move(*script));
#if defined(RV_DEV_UI)
    state->createDevUi(context);
    if (state->devUi && options.devUiOpen) {
        state->devUi->setVisible(true);
    }
#else
    if (options.devUiOpen || options.screenshotIncludeUi) {
        spdlog::warn("--devui-open and --screenshot-include-ui do nothing: this build has no developer overlay");
    }
#endif
    // Frame-limited runs may be unattended: they never take the user's mouse (and stray motion never reaches them).
    // With the overlay open from the start, the mouse stays free for its panels.
    if (!options.frameLimit && !state->overlayVisible()) {
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
    if (!m_state) {
        return;
    }
#if defined(RV_DEV_UI)
    // The overlay sees events first and keeps what it uses (a click on a panel, keys while a field is edited).
    if (m_state->devUi && m_state->devUi->handleEvent(event, m_state->input->mouseCaptured())) {
        return;
    }
#endif
    m_state->input->handleEvent(event);
}

void RetroverseApp::onUpdate(AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    const std::uint64_t frame = context.frameIndex();
    state.simulatedSeconds += deltaSeconds;
    const input::ActionState actions = state.input->update(frame);

    if (actions.pressed(input::Action::ToggleDevUI)) {
        state.toggleOverlay();
    }
    if (actions.pressed(input::Action::Back)) {
        state.back(context);
    }
    if (actions.pressed(input::Action::ReloadShaders)) {
        spdlog::info("Reloading shaders");
        state.shaders->reloadAll();
    }
    if (actions.pressed(input::Action::Screenshot)) {
        state.screenshotRequested = true;
    }

#if defined(RV_DEV_UI)
    state.updateOverlay(context);
#endif
    if (auto shown = state.syncDebugLines(); !shown) {
        spdlog::error("Debug lines unavailable: {}", shown.error().toString());
    }
    state.freeRoam->setCollisionEnabled(state.collision);
    state.freeRoam->update(actions, static_cast<float>(deltaSeconds), state.camera);
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
    state.viewRendered = false;
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
#if defined(RV_DEV_UI)
        // Last, over the scene in the window: the screenshots above show the scene alone unless asked otherwise.
        if (state.devUi) {
            state.devUi->renderToWindow(context.device());
        }
#endif
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

void RetroverseApp::onShutdown(AppContext& context) {
    const CommandLineOptions& options = context.options();
    if (m_state && options.frameLimit) {
        m_state->freeRoam->logFinalPose(context.frameIndex(), m_state->simulatedSeconds);
        if (const std::optional<std::string> summary = m_state->statsLog.runSummary("Frame stats of the run")) {
            spdlog::info("{}", *summary);
        }
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
