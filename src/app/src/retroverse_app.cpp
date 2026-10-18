#include <rv/app/retroverse_app.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/vec2.hpp>
#include <spdlog/spdlog.h>

#include <SDL3/SDL_events.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/back_action.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/debug_line_selection.hpp>
#include <rv/app/input_context.hpp>
#include <rv/app/mock_catalog.hpp>
#include <rv/app/model_viewer.hpp>
#include <rv/app/overlay_policy.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/app/self_check.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/devui/overlay_input.hpp>
#include <rv/input/action.hpp>
#include <rv/input/input_script.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/debug_lines.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

#if defined(RV_DEV_UI)
#include <rv/devui/dev_ui.hpp>
#include <rv/devui/frame_time_history.hpp>
#include <rv/devui/panels.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/viewpoint.hpp>
#endif

#include "app_input.hpp"
#include "bloom_self_check.hpp"
#include "free_roam.hpp"
#include "fxaa_self_check.hpp"
#include "product_ui.hpp"
#include "srgb_self_check.hpp"

namespace rv::app {
namespace {

#if defined(RV_DEV_UI)
/// Developer builds (with the developer overlay) log the frame statistics once per second.
constexpr bool kDeveloperBuild = true;
#else
constexpr bool kDeveloperBuild = false;
#endif

/// Where the developer overlay keeps its window layout, under <user data>/config.
constexpr std::string_view kOverlayLayoutFile = "imgui.ini";
/// Reference grid of --debug-draw in scenes: 1 m cells over 20 x 20 m around the origin.
constexpr DebugGrid kSceneDebugGrid{10.0f, 1.0f};

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

/// A self-check that could not run: recorded in the run report and failing the run (exit code 3).
void failSelfCheck(AppContext& context, SelfCheck check, Error error) {
    error.message = fmt::format("{} self-check could not run: {}", toString(check), error.message);
    context.report().selfCheck = SelfCheckRecord{check, false, error.message};
    context.failRun(std::move(error));
}

/// --self-check srgb: logs the outcome and records it in the run report; a check that fails, or cannot run, fails the
/// run (exit code 3).
void runSrgbCheck(AppContext& context, const SceneGraphics& graphics) {
    const auto results =
        runSrgbSelfCheck(context.device(), graphics.textureDefaults(), graphics.unlitProgram(), graphics.postShaders());
    if (!results) {
        failSelfCheck(context, SelfCheck::Srgb, results.error());
        return;
    }
    const auto describe = [](const SrgbCheckResult& result) {
        return fmt::format("{}..{} in {} pixel(s)", result.minimum, result.maximum, result.pixelCount);
    };
    const std::string summary =
        fmt::format("linear {} stored as {} without and {} with bloom, dithering and anti-aliasing, expected {} +- {}",
                    kSrgbCheckLinearValue, describe(results->plain), describe(results->postProcessed),
                    results->plain.expected, kSrgbCheckTolerance);
    context.report().selfCheck = SelfCheckRecord{SelfCheck::Srgb, results->passed(), summary};
    if (results->passed()) {
        spdlog::info("sRGB self-check passed: {}", summary);
    } else {
        context.failRun(Error{ErrorCode::ValidationFailed, "sRGB self-check failed: " + summary, "--self-check=srgb"});
    }
}

/// --self-check bloom: logs the measurements and the outcome and records them in the run report; a check that fails,
/// or cannot run, fails the run (exit code 3).
void runBloomCheck(AppContext& context, const SceneGraphics& graphics) {
    const auto measurements = runBloomSelfCheck(context.device(), graphics.postShaders());
    if (!measurements) {
        failSelfCheck(context, SelfCheck::Bloom, measurements.error());
        return;
    }
    const BloomCheckResult result = evaluateBloomCheck(*measurements);
    const auto [lowest, highest] =
        std::minmax_element(measurements->fireflyEnergies.begin(), measurements->fireflyEnergies.end());
    const std::string summary = fmt::format(
        "lit square below the threshold blooms {:g} ({}), emitter keeps {:.1f}% of its energy ({}, +- {:g}%), "
        "single bright pixels bloom {:.3g} to {:.3g} ({:.1f}% from their mean: {}, at most {:g}%)",
        measurements->litBloomPeak, result.litStaysOut ? "ok" : "FAILED", 100.0f * measurements->emitterEnergyRatio,
        result.energyKept ? "ok" : "FAILED", 100.0f * kBloomCheckEnergyTolerance,
        lowest == measurements->fireflyEnergies.end() ? 0.0f : *lowest,
        highest == measurements->fireflyEnergies.end() ? 0.0f : *highest, 100.0f * result.fireflySpread,
        result.firefliesSteady ? "ok" : "FAILED", 100.0f * kBloomCheckFireflyTolerance);
    context.report().selfCheck = SelfCheckRecord{SelfCheck::Bloom, result.passed, summary};
    if (result.passed) {
        spdlog::info("Bloom self-check passed: {}", summary);
    } else {
        context.failRun(
            Error{ErrorCode::ValidationFailed, "bloom self-check failed: " + summary, "--self-check=bloom"});
    }
}

/// --self-check fxaa: logs the outcome and records it in the run report; a check that fails, or cannot run, fails the
/// run (exit code 3).
void runFxaaCheck(AppContext& context, const SceneGraphics& graphics) {
    const auto result =
        runFxaaSelfCheck(context.device(), graphics.textureDefaults(), graphics.unlitProgram(), graphics.postShaders());
    if (!result) {
        failSelfCheck(context, SelfCheck::Fxaa, result.error());
        return;
    }
    const std::string summary = fmt::format(
        "without anti-aliasing the edge is hard ({}); with it {} of {} rows crossed by the edge hold blended pixels "
        "({}, at least {:g}%), {} of {} pixels of flat areas changed ({})",
        result->aliasedIsHard ? "ok" : "FAILED", result->blendedEdgeRows, result->edgeRows,
        result->edgeBlended ? "ok" : "FAILED", 100.0f * kFxaaCheckMinBlendedShare, result->flatChanged,
        result->flatPixels, result->flatCopied ? "ok" : "FAILED");
    context.report().selfCheck = SelfCheckRecord{SelfCheck::Fxaa, result->passed, summary};
    if (result->passed) {
        spdlog::info("FXAA self-check passed: {}", summary);
    } else {
        context.failRun(Error{ErrorCode::ValidationFailed, "FXAA self-check failed: " + summary, "--self-check=fxaa"});
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

/// Loads the --mock-catalog file, if any, and reports what it holds.
Expected<std::optional<MockCatalog>> loadCatalog(const CommandLineOptions& options) {
    if (!options.mockCatalog) {
        return std::optional<MockCatalog>{};
    }
    auto catalog = loadMockCatalog(*options.mockCatalog);
    if (!catalog) {
        Error error = catalog.error();
        error.message = "cannot load the mock catalog: " + error.message;
        return propagate(std::move(error));
    }
    std::size_t launchable = 0;
    for (const MockGame& game : catalog->games) {
        launchable += game.launch ? 1 : 0;
    }
    spdlog::info("Mock catalog {}: {} game(s), {} with a launch target, {} profile(s) of its own",
                 core::pathToUtf8(*options.mockCatalog), catalog->games.size(), launchable,
                 catalog->profiles.profiles.size());
    return std::optional<MockCatalog>(std::move(*catalog));
}

/// Input events dropped while the host idles (an emulator has the screen): presses, motion and text, which would act
/// once frames resume. Releases and every other event (focus, window, devices) are delivered, so that nothing stays
/// held; whatever the wait dropped is released when it ends (AppInput::releaseAll).
bool droppedWhileIdle(const SDL_Event& event) noexcept {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        return true;
    default:
        return false;
    }
}

/// A press the user made in window `windowId`: a key (not a repeat), a mouse button in it, a gamepad button (SDL
/// delivers those only while a window of the application has the focus).
bool isUserPress(const SDL_Event& event, std::uint32_t windowId) noexcept {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        return !event.key.repeat && event.key.windowID == windowId;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return event.button.windowID == windowId;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return true;
    default:
        return false;
    }
}

} // namespace

struct RetroverseApp::State {
    /// Shaders, textures, registries, the renderer and the outputs. Declared first: destroyed last, after the scene
    /// content and the overlay, which refer to it.
    std::unique_ptr<SceneGraphics> graphics;
    std::optional<SceneInstance> scene;
    /// The scene as described: its camera (start, reset) and objects (the developer overlay's list).
    scene::SceneDescription description;
    /// Reference grid of the debug lines.
    DebugGrid debugGrid;
    /// Debug lines wanted (--debug-draw, the developer overlay) and those shown.
    DebugLineSelection debugLines;
    DebugLineSelection shownDebugLines;
    /// Developer overlay toggles: the scene pass as wireframe, anti-aliasing of the views, collision of the walker
    /// (off: noclip).
    bool wireframe = false;
    bool antialiasing = true;
    bool collision = true;
    /// How the key light's shadow map is drawn and read (renderer tuning; the developer overlay edits it live).
    gl::ShadowSettings shadowSettings;
    /// ToggleDevUI pressed in a run without the overlay: logged once.
    bool overlayNoticeLogged = false;
    /// The view camera: the scene camera's lens and pose, then driven by free roam.
    scene::Camera camera;
    std::optional<FreeRoam> freeRoam;
    std::unique_ptr<AppInput> input;
    /// Sum of the update steps: the shader time, so that --fixed-dt runs render the same frames on every run.
    double simulatedSeconds = 0.0;
    /// The phases the --input-script holds open (its steps' "phase"), sorted.
    std::vector<std::string> scriptPhases;
    bool screenshotRequested = false; ///< Screenshot action pressed; captured during the next render
    /// The mouse between first-person look and the layers over the scene (the developer overlay, the product UI):
    /// the first layer to open frees a captured mouse, the last to close captures it again when it freed it.
    devui::CaptureHandover captureHandover;
    bool layersOpen = false;
    /// The product UI had the input at the last syncCaptureLayers(): opening it frees a mouse captured under the
    /// developer overlay too.
    bool productLayerOpen = false;
    /// QuickCatalog pressed in a run without a catalog (the model viewer): logged once.
    bool catalogNoticeLogged = false;
    /// The product UI (the quick catalog, the game-session panel) and its launches: over the ES-DE catalog of the user
    /// data directory, or --mock-catalog's; none in the model viewer.
    std::unique_ptr<ProductUi> product;
#if defined(RV_DEV_UI)
    devui::FrameTimeHistory frameTimes;
    /// The developer overlay. Declared last: destroyed first, before the state its panels refer to.
    std::unique_ptr<devui::DevUi> devUi;
#endif

    /// Opens the phases of the --input-script's steps that cover `frame` and closes those that no longer do, so that a
    /// step's phase holds its frames exactly (FrameTiming).
    void syncScriptPhases(FrameTiming& timing, std::uint64_t frame) {
        const input::InputScript* script = input->script();
        if (script == nullptr) {
            return;
        }
        std::vector<std::string> wanted = script->phasesAt(frame);
        for (const std::string& name : scriptPhases) {
            if (!std::binary_search(wanted.begin(), wanted.end(), name)) {
                timing.closePhase(name);
                spdlog::info("Frame {}: phase '{}' closed by the input script", frame, name);
            }
        }
        for (const std::string& name : wanted) {
            if (!std::binary_search(scriptPhases.begin(), scriptPhases.end(), name)) {
                timing.openPhase(name);
                spdlog::info("Frame {}: phase '{}' opened by the input script", frame, name);
            }
        }
        scriptPhases = std::move(wanted);
    }

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

    /// The view of the frame: the scene from the walker's camera, with the overlay's rendering toggles.
    [[nodiscard]] gl::RenderView view() const {
        gl::RenderView view = makeRenderView(*scene, camera);
        view.shadowSettings = shadowSettings;
        view.post.antialiasing = antialiasing;
        view.timeSeconds = static_cast<float>(simulatedSeconds);
        view.wireframe = wireframe;
        return view;
    }

    /// Shows the debug lines of `debugLines` (--debug-draw, the developer overlay's toggles) when they differ from
    /// those shown: the selected kinds (the reference grid, then the bounds and sockets of the scene's objects). A
    /// failure is reported once per change; no lines are drawn then.
    Status syncDebugLines() {
        if (debugLines == shownDebugLines) {
            return ok();
        }
        shownDebugLines = debugLines;
        if (!debugLines.any()) {
            spdlog::info("Debug lines off");
            return graphics->setDebugLines(scene::DebugLines{});
        }
        const scene::DebugLines lines =
            composeDebugLines(debugLines, debugGrid, scene->boundsLines(), scene->socketLines());
        if (auto shown = graphics->setDebugLines(lines); !shown) {
            return shown;
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

    /// What Back can leave (backAction).
    [[nodiscard]] BackState backState() const noexcept {
        BackState levels;
        levels.mouseCaptured = input->mouseCaptured();
#if defined(RV_DEV_UI)
        if (devUi) {
            levels.overlayVisible = devUi->visible();
            levels.overlayHasKeyboard = devUi->hasKeyboard();
            levels.overlayPopupOpen = devUi->popupOpen();
        }
#endif
        if (product) {
            levels.panelOpen = product->panelOpen();
            levels.catalogOpen = product->catalogOpen();
        }
        return levels;
    }

    /// Who reads this frame's actions: the product UI while its catalog or panel is open, the world otherwise.
    [[nodiscard]] InputContext inputContext() const noexcept {
        return product ? inputContextFor(product->catalogOpen(), product->panelOpen()) : InputContext::World;
    }

    /// Hands the mouse over when the layers over the scene change: the first to open (the developer overlay, the
    /// product UI) frees a captured mouse for its cursor, the last to close captures it again when it was freed. The
    /// product UI always needs the cursor: opening it while the overlay shows frees a mouse that a click into the scene
    /// captured again meanwhile (given back, too, when the last layer closes).
    void syncCaptureLayers() {
        const bool productOpen = product && product->hasInput();
        const bool open = overlayVisible() || productOpen;
        const bool productOpened = productOpen && !productLayerOpen;
        productLayerOpen = productOpen;
        if (open != layersOpen) {
            layersOpen = open;
            if (open) {
                if (captureHandover.open(input->mouseCaptured())) {
                    input->releaseMouse(productOpen ? "quick catalog opened; Esc or B leaves it"
                                                    : "developer overlay opened; F1 or Esc closes it");
                }
            } else if (captureHandover.close(input->mouseCaptured())) {
                input->captureMouseWhenFocused("back to the scene");
            }
        } else if (productOpened && input->mouseCaptured() && captureHandover.open(true)) {
            input->releaseMouse("quick catalog opened over the developer overlay; Esc or B leaves it");
        }
#if defined(RV_DEV_UI)
        if (devUi) {
            devUi->setMouseCaptured(input->mouseCaptured());
        }
#endif
    }

    /// QuickCatalog in the world: opens the catalog of the run (nothing without one, logged once).
    void openCatalog() {
        if (!product) {
            if (!catalogNoticeLogged) {
                catalogNoticeLogged = true;
                spdlog::info("QuickCatalog: no catalog in the model viewer");
            }
            return;
        }
        product->openCatalog();
        syncCaptureLayers();
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

    /// Shows or hides the developer overlay, handing the mouse over (syncCaptureLayers): opening it frees a captured
    /// mouse for the panels, closing it gives first-person look the capture back.
    void setOverlayVisible([[maybe_unused]] bool visible) {
#if defined(RV_DEV_UI)
        if (!devUi || devUi->visible() == visible) {
            return;
        }
        devUi->setVisible(visible);
        syncCaptureLayers();
        devUi->setMouseCaptured(input->mouseCaptured());
#endif
    }

    /// Back (Esc, gamepad B; ui_back while the product UI has the input): leaves one level (backAction).
    void back(AppContext& context) {
        switch (backAction(backState())) {
        case BackAction::Ignore:
            break;
        case BackAction::ClosePopup:
#if defined(RV_DEV_UI)
            devUi->closePopup();
#endif
            break;
        case BackAction::PanelBack:
            product->panelBack();
            break;
        case BackAction::CatalogBack:
            product->catalogBack();
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
    /// Creates the developer overlay and its panels; the error tells why it cannot start (the run goes on without it).
    /// Interactive runs keep the layout in <user data>/config/imgui.ini; frame-limited runs always start from the
    /// default layout and save nothing, so that their screenshots never depend on earlier runs.
    Status createDevUi(AppContext& context) {
        devui::DevUiConfig config;
        if (!context.options().frameLimit) {
            config.iniFile = context.paths().configDir() / core::pathFromUtf8(kOverlayLayoutFile);
        }
        auto created = devui::DevUi::create(context.window(), config);
        if (!created) {
            return propagate(created.error());
        }
        devUi = std::move(*created);
        platform::GraphicsWindow& window = context.window();
        devUi->addPanel(devui::makeStatsPanel({frameTimes, graphics->renderStats(), graphics->meshes(),
                                               graphics->materials(), graphics->textures(),
                                               gl::gpuMemoryLedger()}),
                        devui::DockSlot::Left);
        devUi->addPanel(devui::makeCameraPanel({camera, freeRoam->controller(), input->system(), collision,
                                                [this] { resetToSceneCamera(); }}),
                        devui::DockSlot::LeftBottom);
        // Tabs of one node; the first is shown.
        devUi->addPanel(devui::makeLightsPanel({scene->sceneLights(), scene->environment(), scene->droppedLights(),
                                                scene->shadowLight(), scene->shadowNote(),
                                                [this] { scene->applyLighting(); }}),
                        devui::DockSlot::Right);
        devUi->addPanel(devui::makeMaterialsPanel(
                            {graphics->materials(), [this] { return scene->renderItems(); }, graphics->textures()}),
                        devui::DockSlot::Right);
        devUi->addPanel(devui::makeScenePanel({description,
                                               [this, &window](std::size_t index) {
                                                   goTo(index, aspectOf(window.drawableSize()),
                                                        devUi->sceneViewArea().value_or(scene::ViewArea{}));
                                               }}),
                        devui::DockSlot::Right);
        devUi->addPanel(devui::makeRenderingPanel({debugLines.grid, debugLines.bounds, debugLines.sockets, wireframe,
                                                   antialiasing, scene->environment(), shadowSettings,
                                                   graphics->shaders(), window}),
                        devui::DockSlot::RightBottom);
        devUi->addPanel(devui::makeLogPanel(), devui::DockSlot::Bottom);
        return ok();
    }

    /// Records the frame time and builds the overlay frame; its panels edit the state the frame then simulates and
    /// renders.
    void updateOverlay(const AppContext& context) {
        frameTimes.record(context.clock().deltaSeconds());
        if (!devUi) {
            return;
        }
        devUi->setMouseCaptured(input->mouseCaptured());
        devUi->buildFrame(overlayCaptureSize(context.options(), context.frameIndex(), screenshotRequested));
    }

#endif

    /// The single overlay slot of SceneGraphics, composed: the product UI over the scene, the developer overlay over
    /// both. In screenshots the product UI always (it is what the window shows), the developer overlay only with
    /// --screenshot-include-ui. Nothing is set when neither exists, so that such runs draw the scene alone.
    void installOverlays(const AppContext& context) {
        bool developerOverlay = false;
#if defined(RV_DEV_UI)
        developerOverlay = devUi != nullptr;
#endif
        if (!product && !developerOverlay) {
            return;
        }
        const bool overlayInScreenshots = developerOverlay && context.options().screenshotIncludeUi;
        std::function<void(gl::GLDevice&, const gl::Framebuffer&)> intoScreenshot;
        if (product || overlayInScreenshots) {
            intoScreenshot = [this, overlayInScreenshots](gl::GLDevice& device, const gl::Framebuffer& target) {
                if (product) {
                    product->renderTo(device, target);
                }
#if defined(RV_DEV_UI)
                if (overlayInScreenshots && devUi) {
                    devUi->renderTo(device, target);
                }
#else
                static_cast<void>(overlayInScreenshots);
#endif
            };
        }
        graphics->setOverlay(
            [this](gl::GLDevice& device) {
                if (product) {
                    product->renderToWindow(device);
                }
#if defined(RV_DEV_UI)
                if (devUi) {
                    devUi->renderToWindow(device);
                }
#endif
            },
            std::move(intoScreenshot));
    }

#if defined(RV_DEV_UI)
    /// The overlay's "reset to scene camera": the walker and the lens as the scene starts them.
    void resetToSceneCamera() {
        freeRoam->teleportToCamera(description.camera);
        camera.setLens(description.camera.lens);
        spdlog::info("Camera reset to the scene camera: {}", freeRoam->describePose());
    }

    /// The overlay's "go to": the walker stands in front of objects[index], looking at it (scene::frontViewpoint over
    /// the bounds of all its instances, framed in `area` of a view of `aspect`: the part the overlay leaves visible).
    void goTo(std::size_t index, float aspect, const scene::ViewArea& area) {
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
                                  walker.settings().eyeHeight, camera.lens(), aspect, area);
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
    auto mock = loadCatalog(options);
    if (!mock) {
        return propagate(std::move(mock.error()));
    }
    // The catalog first: the ES-DE catalog's worker opens the database while the scene loads (NFR-01: the first page
    // within 5 s of the start). --mock-catalog takes precedence; the model viewer has none.
    std::unique_ptr<AppCatalog> appCatalog;
    if (*mock) {
        appCatalog = std::make_unique<MockAppCatalog>(std::move(**mock), launcher::loadEmulatorProfiles(paths));
    } else if (!options.model) {
        EsdeAppCatalog::Options esde;
        esde.paths = paths;
        esde.wake = context.wakeCallback();
        const platform_win::ProcessIdentity process = platform_win::currentProcessIdentity();
        esde.owner = catalog::SessionOwner{process.processId, process.creationTime};
        esde.processCreated = process.creationTime;
        appCatalog = std::make_unique<EsdeAppCatalog>(std::move(esde));
    }
    auto state = std::make_unique<State>();
    state->description = std::move(source->description);
    state->debugGrid = source->grid;
    auto graphics = SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    state->graphics = std::move(*graphics);
    auto sceneInstance = SceneInstance::create(state->description, state->graphics->materialLibrary(),
                                               source->loadModel, state->graphics->resources());
    if (!sceneInstance) {
        return propagate(sceneInstance.error());
    }
    state->scene.emplace(std::move(*sceneInstance));
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
    state->freeRoam.emplace(state->description, state->scene->collisionWorld(), options.spawn);

    input::ActionMap bindings = loadBindings(paths);
    state->input = std::make_unique<AppInput>(context.window(), bindings, std::move(*script));
    if (appCatalog) {
        State* raw = state.get();
        auto product = ProductUi::create(
            context, std::move(appCatalog), bindings, [raw, &context] { raw->back(context); },
            [raw]() -> std::optional<SpawnPoint> { return raw->freeRoam->pose(); });
        if (!product) {
            Error error = product.error();
            error.message = "the quick catalog could not start: " + error.message;
            return propagate(std::move(error));
        }
        state->product = std::move(*product);
    }
#if defined(RV_DEV_UI)
    if (auto created = state->createDevUi(context); !created) {
        spdlog::error("Developer overlay unavailable: {}", created.error().toString());
        // An unattended run that shows the overlay must not pass with screenshots of the scene alone.
        if (overlayRequired(options)) {
            Error error = created.error();
            error.message = "the developer overlay asked for by --devui-open or --screenshot-include-ui could not "
                            "start: " +
                            error.message;
            context.failRun(std::move(error));
        }
    } else if (options.devUiOpen) {
        state->devUi->setVisible(true);
        state->layersOpen = true;
    }
#else
    if (options.devUiOpen || options.screenshotIncludeUi) {
        spdlog::warn("--devui-open and --screenshot-include-ui do nothing: this build has no developer overlay");
    }
#endif
    state->installOverlays(context);
    // Frame-limited runs may be unattended: they never take the user's mouse (and stray motion never reaches them).
    // With the overlay open from the start, the mouse stays free for its panels.
    if (!options.frameLimit && !state->overlayVisible()) {
        state->input->captureMouseWhenFocused("startup");
    }

    if (options.selfCheck) {
        // With the programs the views use.
        switch (*options.selfCheck) {
        case SelfCheck::Srgb:
            runSrgbCheck(context, *state->graphics);
            break;
        case SelfCheck::Bloom:
            runBloomCheck(context, *state->graphics);
            break;
        case SelfCheck::Fxaa:
            runFxaaCheck(context, *state->graphics);
            break;
        }
    }
    m_state = std::move(state);
    return ok();
}

void RetroverseApp::onEvent(AppContext& context, const SDL_Event& event) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    if (state.product && (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) &&
        event.window.windowID == context.window().id()) {
        state.product->focusChanged(event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
    }
    // An emulator has the screen: nothing it did not mean for Retroverse may act once frames resume. A press in the
    // focused window may still bring the session's question back (the press itself acts on nothing).
    if (context.idle() && droppedWhileIdle(event)) {
        if (state.product && isUserPress(event, context.window().id())) {
            state.product->userPressed();
        }
        return;
    }
#if defined(RV_DEV_UI)
    // The overlay sees events first and keeps what it uses (a click on a panel, keys while a field is edited).
    if (state.devUi && state.devUi->handleEvent(event, state.input->mouseCaptured())) {
        return;
    }
#endif
    // Then the product UI while it has the input (the mouse, text, keys that are not UI commands), then the game. A
    // click the UI did not use never captures the mouse while the catalog is open.
    if (state.product && state.product->hasInput()) {
        if (state.product->handleEvent(event) || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            return;
        }
    }
    state.input->handleEvent(event);
}

void RetroverseApp::onUpdate(AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    const std::uint64_t frame = context.frameIndex();
    state.simulatedSeconds += deltaSeconds;
    const input::ActionState actions = state.input->update(frame);
    state.syncScriptPhases(context.frameTiming(), frame);
    // The context of the frame is the one it began in: the press that opens the catalog is no command inside it.
    const InputContext inputContext = state.inputContext();

    if (actions.pressed(input::Action::ToggleDevUI)) {
        state.toggleOverlay();
    }
    if (actions.pressed(input::Action::ReloadShaders)) {
        state.graphics->reloadShaders();
    }
    if (actions.pressed(input::Action::Screenshot)) {
        state.screenshotRequested = true;
    }
    if (inputContext == InputContext::World) {
        if (actions.pressed(input::Action::Back)) {
            state.back(context);
        }
        if (catalogToggleRequested(inputContext, actions)) {
            state.openCatalog();
        }
    } else if (catalogToggleRequested(inputContext, actions) && !state.product->panelOpen()) {
        state.product->closeCatalog();
    }

#if defined(RV_DEV_UI)
    state.updateOverlay(context);
#endif
    if (auto shown = state.syncDebugLines(); !shown) {
        spdlog::error("Debug lines unavailable: {}", shown.error().toString());
    }
    state.freeRoam->setCollisionEnabled(state.collision);
    state.freeRoam->update(actionsFor(inputContext, actions), static_cast<float>(deltaSeconds), state.camera);
    if (state.product) {
        // Text an --input-script types on this frame, into the product UI's focused field (the catalog's search).
        if (const input::InputScript* script = state.input->script()) {
            if (const std::string text = script->textAt(frame); !text.empty()) {
                state.product->typeText(text);
            }
        }
        // The UI commands only when the frame began in the UI's context (UI Back goes through back()).
        const input::ActionState uiActions = actionsFor(InputContext::Ui, actions);
        state.product->update(inputContext == InputContext::Ui ? &uiActions : nullptr, deltaSeconds,
                              state.simulatedSeconds);
        // After this frame is presented the host waits: a launch starts, or an emulator has the screen.
        if (state.product->wantsIdle()) {
            context.setIdle(true);
        }
    }
    state.syncCaptureLayers();
    state.logScriptProgress(frame);
    state.graphics->pollShaderReload();
}

void RetroverseApp::onRender(AppContext& context) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    state.graphics->renderFrame(context, state.view(), state.screenshotRequested);
    state.screenshotRequested = false;
    if (state.product) {
        state.product->frameRendered();
        // A question nobody can see is presented once: the host waits after this frame (LaunchFlow::hostIdle).
        if (state.product->wantsIdle()) {
            context.setIdle(true);
        }
    }
}

void RetroverseApp::onIdle(AppContext& context) {
    if (!m_state || !m_state->product) {
        context.setIdle(false);
        return;
    }
    State& state = *m_state;
    state.product->onIdle();
    context.setIdle(state.product->wantsIdle());
    if (!context.idle()) {
        // Frames resume: whatever the wait dropped (the release of the button that launched among them) is released.
        state.input->releaseAll();
    }
}

void RetroverseApp::onShutdown(AppContext& context) {
    if (m_state) {
        // The quit hook: a game still running is left running and recorded, before anything else ends.
        if (m_state->product) {
            m_state->product->finish();
        }
        if (context.options().frameLimit) {
            m_state->freeRoam->logFinalPose(context.frameIndex(), m_state->simulatedSeconds);
        }
        context.report().finalCamera = m_state->freeRoam->pose();
        m_state->graphics->finishRun(context);
    }
    m_state.reset();
}

} // namespace rv::app
