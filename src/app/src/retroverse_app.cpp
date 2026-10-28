#include <rv/app/retroverse_app.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <SDL3/SDL_mouse.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/audio_policy.hpp>
#include <rv/app/back_action.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/debug_line_selection.hpp>
#include <rv/app/elevator_prompt_ui.hpp>
#include <rv/app/input_context.hpp>
#include <rv/app/mock_catalog.hpp>
#include <rv/app/model_viewer.hpp>
#include <rv/app/overlay_policy.hpp>
#include <rv/app/process_priority.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/app/self_check.hpp>
#include <rv/app/video_settings.hpp>
#include <rv/app/world_build.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/audio/audio_engine.hpp>
#include <rv/assets/surface_textures.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/devui/overlay_input.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/input/action.hpp>
#include <rv/input/input_script.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/media/video_verdicts.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform_win/process_priority.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/render/texture_cache.hpp>
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
#include "screen_service.hpp"
#include "srgb_self_check.hpp"
#include "world_session.hpp"

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

/// The scene file `scene` names (resolveScenePath: the test room when empty).
Expected<SceneSource> sceneFileSource(const std::optional<std::string>& scene, const core::Paths& paths) {
    const std::filesystem::path scenePath = resolveScenePath(scene, paths.assetRoot());
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

/// The generated room is the run's scene (I3 step 10b): no --scene, --model or --mock-catalog.
bool roomMode(const CommandLineOptions& options) noexcept {
    return !options.scene && !options.model && !options.mockCatalog;
}

/// The room the run starts in: --start-room's (I4 step 5a), else none (the ground floor's lobby).
std::optional<world::RoomId> startRoomOf(const CommandLineOptions& options) {
    if (options.startRoom.empty()) {
        return std::nullopt;
    }
    auto room = world::RoomId::parse(options.startRoom);
    return room ? std::optional<world::RoomId>(std::move(*room)) : std::nullopt;
}

/// Without library.json or the catalog's database the ES-DE catalog cannot open: the room is decided at once (the test
/// room, its notice naming the catalog's state), so that such runs never show the room's shell first.
std::optional<WorldResult> roomWithoutCatalog(const core::Paths& paths) {
    std::error_code ignored;
    std::string state;
    if (!std::filesystem::exists(paths.configDir() / esde::kLibraryFileName, ignored)) {
        state = std::string(toString(CatalogState::NoLibrary));
    } else if (!std::filesystem::exists(catalog_db::databaseFile(paths), ignored)) {
        state = std::string(toString(CatalogState::NoDatabase));
    } else {
        return std::nullopt;
    }
    WorldResult result;
    result.testRoom = true;
    result.notice = std::string(world_notice::kNoCatalog);
    result.catalogState = state;
    result.reason = fmt::format("the ES-DE catalog is not open ({})", state);
    return result;
}

Expected<SceneSource> loadSceneSource(const CommandLineOptions& options, const core::Paths& paths,
                                      const std::optional<WorldResult>& decided) {
    if (options.model) {
        return modelViewerSource(*options.model);
    }
    if (!roomMode(options) || decided) {
        return sceneFileSource(options.scene, paths);
    }
    // While the building is made: the empty shell (the WorldGenerating state): the lobby, or --start-room's empty room.
    auto shell = WorldSession::shellScene(paths.assetRoot(), startRoomOf(options));
    if (!shell) {
        spdlog::error("The building's shell cannot be made, the test room shows while the building is made: {}",
                      shell.error().toString());
        return sceneFileSource(std::nullopt, paths);
    }
    spdlog::info("Scene '{}': the empty shell while the building is made", shell->name);
    SceneSource source;
    source.description = std::move(*shell);
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

/// video.json (I5 step 1): the shipped defaults with the user's file over them (a user file that does not load is left
/// out, said in the log); the built-in defaults when the shipped file itself does not load.
VideoSettings videoSettingsOf(const core::Paths& paths) {
    auto loaded = loadVideoSettings(paths.assetRoot(), paths.configDir());
    if (!loaded) {
        spdlog::error("Video: the shipped video.json does not load ({}); the built-in defaults are used",
                      loaded.error().toString());
        return VideoSettings{};
    }
    for (const Error& issue : loaded->issues) {
        spdlog::warn("Video: the user's video.json is left out, the defaults used: {}", issue.toString());
    }
    return loaded->settings;
}

} // namespace

struct RetroverseApp::State {
    /// Shaders, textures, registries, the renderer and the outputs. Declared first: destroyed last, after the scene
    /// content and the overlay, which refer to it.
    std::unique_ptr<SceneGraphics> graphics;
    std::optional<SceneInstance> scene;
    /// The area that held the eye at the first view of a scene with areas, then each change (I4 step 3).
    std::vector<SceneAreaChange> areaChanges;
    /// The light of the first view and of every frame while it eased, and of the last view (I6 step 1).
    std::vector<SceneLightingSample> lightingSamples;
    SceneLightingSample lastLighting;
    /// video.json, read once (I5 step 1): the screens' settings and the sound's.
    std::optional<VideoSettings> video;
    /// The clips' sound (I5 step 5, ADR-0016 §9): the engine (none with --audio off or video.json's audio mode off),
    /// declared before the screens' service, which feeds its mixer, so that it outlives it; the run's mute and duck,
    /// the end of the mix pulled so far (the null output), the report's section.
    std::unique_ptr<audio::AudioEngine> audio;
    bool muted = false;
    bool ducked = false;
    std::optional<double> audioPulledTo;
    AudioRecord audioRecord;
    /// The screens' service (I3 step 11; since I5 step 2 every CRT of the room, the hero first, sharing the decoder
    /// scheduler's slots): their decoders and the pictures of their fallback chains; none without a CRT (the model
    /// viewer). Destroyed before the graphics, whose material registry shows its pictures.
    std::unique_ptr<ScreenService> screen;
    /// The process priority (I5 step 2, ADR-0016 §2): above normal while the view renders, normal while minimized and
    /// during a game; each change for the run report.
    std::optional<bool> aboveNormal;
    ProcessPriorityRecord priority;
    /// The app's worker pool (ADR-0011 §6: the room's generation and its covers) and the generated room (I3 step 10b),
    /// in room mode only. The pool outlives the session, whose destructor cancels its jobs; the session (its collection
    /// on the GPU) goes before the graphics.
    std::unique_ptr<core::WorkerPool> pool;
    std::unique_ptr<WorldSession> world;
    /// The scene as described: its camera (start, reset) and objects (the developer overlay's list).
    scene::SceneDescription description;
    /// Loads the scene's models (asset-relative paths, the model viewer's file).
    scene::ModelLoader loadModel;
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
    /// The last update step: what the area's environment eases by at the next render (I6 step 1).
    double stepSeconds = 0.0;
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

    /// In a scene with areas (a floor), the lights of the area that holds this frame's eye (I4 step 3) and its
    /// environment, eased by the last update step (I6 step 1); each change of area is recorded for the run report.
    void followSceneAreas(std::uint64_t frame) {
        if (!scene || scene->areas().empty()) {
            return;
        }
        scene->setReduceMotion(world && world->reduceMotion());
        (void)scene->followEye(camera.position(), camera.forward(), static_cast<float>(stepSeconds));
        const bool eased = scene->advanceEnvironment(static_cast<float>(stepSeconds));
        if (areaChanges.empty() || areaChanges.back().area != scene->areaId()) {
            areaChanges.push_back(SceneAreaChange{frame, std::string(scene->areaId())});
            spdlog::info("Frame {}: the eye is in the area '{}' ({} light(s) packed)", frame, scene->areaId(),
                         scene->lights().punctualLightCount);
        }
        // The light of the first view and of every frame while it eases, its last step included (I6 step 1).
        const SceneLightingSample sample = lightingSample(frame);
        if ((lightingSamples.empty() || eased || scene->environmentEasing() || sample.fading > 0 ||
             scene->lightsFading()) &&
            lightingSamples.size() < kMaxSceneLightingSamples) {
            lightingSamples.push_back(sample);
        }
        lastLighting = sample;
    }

    /// The light of this frame's view in a scene with areas.
    [[nodiscard]] SceneLightingSample lightingSample(std::uint64_t frame) const {
        SceneLightingSample sample;
        sample.frame = frame;
        sample.area = std::string(scene->areaId());
        const scene::EnvironmentSetup environment = scene->viewEnvironment();
        sample.exposure = environment.exposure;
        sample.ambientIntensity = environment.ambientIntensity;
        sample.fillIntensity = environment.fillIntensity;
        for (const gl::PackedLightSource& light : scene->packedLights()) {
            ++sample.lights;
            sample.fading += light.weight < 1.0f ? 1 : 0;
            sample.weight += light.weight;
        }
        return sample;
    }

    /// The scene's areas for the run report: the area that held the eye at the last view and its lights.
    [[nodiscard]] std::optional<SceneAreasRecord> sceneAreasRecord() const {
        if (!scene || scene->areas().empty()) {
            return std::nullopt;
        }
        SceneAreasRecord record;
        record.areas = scene->areas().size();
        record.area = std::string(scene->areaId());
        const std::span<const scene::Light> lights = scene->sceneLights();
        for (const std::uint32_t index : scene->areaSelection()) {
            record.lights.push_back(gl::lightName(lights[index], index));
        }
        // The light the views' shadow map came from: the packed key light, when it casts.
        if (const std::optional<std::size_t> key = scene->keyLight();
            key && *key < lights.size() && scene->shadowLight().casts()) {
            record.keyLight = gl::lightName(lights[*key], *key);
        }
        record.changes = areaChanges;
        for (const gl::PackedLightSource& light : scene->packedLights()) {
            record.packed.push_back(PackedLightRecord{light.index < lights.size()
                                                          ? gl::lightName(lights[light.index], light.index)
                                                          : std::string("?"),
                                                      light.weight, light.clipped});
        }
        record.last = lastLighting;
        record.samples = lightingSamples;
        return record;
    }

    /// The view of the frame: the scene from the walker's camera, with the overlay's rendering toggles.
    [[nodiscard]] gl::RenderView view() const {
        gl::RenderView view = makeRenderView(*scene, camera);
        view.shadowSettings = shadowSettings;
        view.post.antialiasing = antialiasing;
        view.timeSeconds = static_cast<float>(simulatedSeconds);
        view.wireframe = wireframe;
        if (world) {
            view.staticGroups = world->staticGroups();
            // The room's dynamic path after the scene's items: the items pulled out of their slots (I3 step 13a), the
            // inspected one in the hand and the scene behind it dimmed (I3 step 13b), the hero texture's front (I3
            // step 12).
            view.items = world->viewItems(view.items, camera);
            world->applyInspection(view);
            // The elevator ride's fade (I4 step 7b): after the tonemap, every pixel dimmed alike (the UI above it).
            view.post.fade = world->fade();
        }
        return view;
    }

    /// The world's interaction this frame (I3 step 13a), before the walker moves: `actions` in the World context (null
    /// while the product UI has the input, which also casts no pick ray), the ray from the reticle or, while the mouse
    /// is free, from the cursor (frame-limited runs never read the user's mouse: their cursor rests at the window's
    /// centre).
    [[nodiscard]] InteractionResult interactWorld(AppContext& context, const input::ActionState* actions,
                                                  float deltaSeconds) {
        const scene::FirstPersonController& walker = freeRoam->controller();
        WorldHover::Frame frame;
        frame.actions = actions;
        // The card's buttons (I3 step 14a): the arrows, ui_activate and the clicks on it press them.
        if (product) {
            frame.card = world->cardPress(actions, *product);
        }
        frame.deltaSeconds = deltaSeconds;
        frame.walker = scene::WalkerPose{walker.feetPosition(), walker.yaw(), walker.pitch()};
        frame.back = backState();
        // The wheel zooms an inspected item (I3 step 13b); no action is bound to it.
        frame.wheelNotches = actions != nullptr ? input->mouseWheel() : 0.0f;
        const glm::ivec2 drawable = context.window().drawableSize();
        frame.viewer.floorY = walker.feetPosition().y;
        frame.viewer.eyeHeight = walker.settings().eyeHeight;
        frame.viewer.lens = camera.lens();
        frame.viewer.aspect = aspectOf(drawable);
        if (actions != nullptr) {
            frame.raySource = pickRaySource(input->mouseCaptured(), actions->activeDevice());
            const glm::vec2 windowSize(context.window().windowSize());
            glm::vec2 cursor = 0.5f * windowSize;
            if (frame.raySource == PickRaySource::Cursor && !context.options().frameLimit) {
                SDL_GetMouseState(&cursor.x, &cursor.y);
            }
            frame.ray = pickRay(camera, frame.raySource, cursor, windowSize);
        }
        return world->interact(frame);
    }

    /// Places the walker where the world's interaction drives it this frame (an ease, a skip, a restore), the camera
    /// with it.
    void placeWalker(const scene::WalkerPose& pose) {
        freeRoam->controller().teleport(pose.feet, pose.yaw, pose.pitch);
        freeRoam->update(input::ActionState{}, 0.0f, camera);
    }

    /// The screens' frame (`minimized`: every screen out of view). The hero CRT follows the game selected in the quick
    /// catalog while it is open, else the item in the world's Focus or Inspect (I3 step 13b), after its dwell. I5 step 4:
    /// the arcade cabinets asking for their clips, and those showing them drawn by the world; the two selections go
    /// apart, the arcade's hero cabinet following the quick catalog's alone (its focused cabinet plays itself).
    void updateScreens(std::uint64_t frame, bool minimized) {
        if (!screen) {
            return;
        }
        const std::optional<std::string> catalogSelection =
            product ? product->selectedGameId() : std::optional<std::string>();
        WorldSession::ScreenSelections selections{std::nullopt, catalogSelection};
        if (world) {
            selections = world->screenSelections(catalogSelection);
            auto [cabinets, cabinetPool] =
                world->cabinetScreens(camera, simulatedSeconds, screen->slotCount(), video ? *video : VideoSettings{});
            screen->setCabinets(std::move(cabinets), std::move(cabinetPool));
        }
        const auto view = [](const std::optional<std::string>& id) {
            return id ? std::optional<std::string_view>(*id) : std::nullopt;
        };
        screen->update(frame, simulatedSeconds, camera, ScreenSelection{view(selections.world), view(selections.catalog)},
                       minimized, world ? world->eyeInScreenRoom() : true);
        if (world) {
            world->showCabinets(screen->cabinetsShown());
        }
    }

    /// video.json (the shipped defaults with the user's over them), read at its first use.
    const VideoSettings& videoSettings(const AppContext& context) {
        if (!video) {
            video = videoSettingsOf(context.paths());
        }
        return *video;
    }

    /// The clips' sound (I5 step 5): the output the run uses (app::resolveAudioOutput; video.json's audio mode off
    /// builds none), the device falling back to the null output when it does not open; the master, the video bus and
    /// the mute set.
    void createAudio(const AppContext& context) {
        const CommandLineOptions& options = context.options();
        const VideoSettings& settings = videoSettings(context);
        const char* environment = std::getenv("RV_AUDIO");
        AudioOutputChoice choice =
            resolveAudioOutput(options.audio, environment != nullptr ? std::optional<std::string_view>(environment)
                                                                     : std::nullopt,
                               options.frameLimit.has_value());
        if (!choice.warning.empty()) {
            spdlog::warn("Audio: {}", choice.warning);
        }
        if (settings.audio.mode == AudioMode::Off && choice.output != audio::AudioOutput::Off) {
            choice = AudioOutputChoice{audio::AudioOutput::Off, "video.json"};
        }
        if (options.audioCapture && choice.output != audio::AudioOutput::Null) {
            spdlog::warn("Audio: --audio-capture records the null output's mix; the {} output has none",
                         audio::toString(choice.output));
        }
        muted = options.mute || settings.audio.muted;
        audioRecord.mutedAtStart = muted;
        if (choice.output != audio::AudioOutput::Off) {
            audio::AudioEngineOptions engineOptions;
            engineOptions.output = choice.output;
            if (choice.output == audio::AudioOutput::Null) {
                engineOptions.capture = options.audioCapture;
            }
            auto engine = audio::AudioEngine::create(engineOptions);
            if (!engine && choice.output == audio::AudioOutput::Device) {
                spdlog::warn("Audio: {}; the clips' sound goes to the null output", engine.error().toString());
                choice = AudioOutputChoice{audio::AudioOutput::Null, "device-failed"};
                engineOptions.output = audio::AudioOutput::Null;
                engineOptions.capture.reset();
                engine = audio::AudioEngine::create(engineOptions);
            }
            if (engine) {
                audio = std::move(*engine);
                audio->mixer().setMaster(settings.audio.master * settings.audio.video);
                audio->mixer().setMuted(muted);
            } else {
                spdlog::error("Audio: {}; no sound", engine.error().toString());
                choice = AudioOutputChoice{audio::AudioOutput::Off, "failed"};
            }
        }
        audioRecord.device = std::string(audio::toString(choice.output));
        audioRecord.reason = choice.reason;
        audioRecord.sampleRate = audio ? static_cast<std::uint64_t>(audio->mixer().settings().sampleRate) : 0;
        audioRecord.deviceName = audio ? audio->deviceName() : std::string();
        spdlog::info("Audio: output {} ({}){}", audioRecord.device, audioRecord.reason, muted ? ", muted" : "");
    }

    /// The sound's frame (I5 step 5): F8's mute, the duck under the card, the directory and the quick catalog, and the
    /// null output's mix pulled up to the run's time.
    void updateAudio(const input::ActionState* actions) {
        if (actions != nullptr && actions->pressed(input::Action::ToggleMute)) {
            muted = !muted;
            ++audioRecord.muteToggles;
            spdlog::info("Audio: {}", muted ? "muted" : "sound on");
            if (audio) {
                audio->mixer().setMuted(muted);
            }
        }
        const bool card = world && world->context() == WorldContext::Inspect;
        const bool duck =
            audioDucked(card, product && product->directoryOpen(), product && product->catalogOpen());
        if (duck != ducked) {
            ducked = duck;
            ++audioRecord.duckChanges;
            if (audio) {
                audio->mixer().setDucked(duck);
            }
        }
        if (ducked) {
            ++audioRecord.duckedFrames;
        }
        pullAudio();
    }

    /// The null output's mix from where it stopped up to the run's time; a device lost, or not started again after a
    /// launch, tried again (every 2 s).
    void pullAudio() {
        if (!audio) {
            return;
        }
        audio->maintain(audio->clockSeconds(simulatedSeconds));
        const double from = audioPulledTo.value_or(simulatedSeconds - stepSeconds);
        if (simulatedSeconds > from) {
            audio->pull(from, simulatedSeconds - from);
        }
        audioPulledTo = simulatedSeconds;
    }

    /// The run report's audio section, the capture written.
    void recordAudio(RunReport& report) {
        if (audio) {
            if (Status written = audio->finish(); !written) {
                spdlog::error("Audio: the capture was not written: {}", written.error().toString());
            }
            const audio::SyncSummary sync = audio->mixer().syncSummary();
            const audio::AudioEngineStats stats = audio->stats();
            audioRecord.blocks = stats.blocks;
            audioRecord.framesMixed = stats.framesRendered;
            audioRecord.deviceStops = stats.deviceStops;
            audioRecord.deviceStarts = stats.deviceStarts;
            audioRecord.deviceFailures = stats.deviceFailures;
            audioRecord.deviceLosses = stats.deviceLosses;
            audioRecord.deviceRecoveries = stats.deviceRecoveries;
            audioRecord.capture = audio->capturePath();
            audioRecord.captureStartSeconds = audio->captureStartSeconds();
            audioRecord.syncBlocks = sync.blocks;
            audioRecord.resyncs = sync.resyncs;
            audioRecord.corrections = sync.corrections;
            audioRecord.underrunFrames = sync.underrunFrames;
            audioRecord.syncP50Milliseconds = sync.p50Milliseconds;
            audioRecord.syncP95Milliseconds = sync.p95Milliseconds;
            audioRecord.syncMaxMilliseconds = sync.maxMilliseconds;
        }
        if (screen) {
            screen->recordAudio(audioRecord);
        }
        audioRecord.muted = muted;
        report.audio = audioRecord;
    }

    /// Sets the process priority when it changes (I5 step 2): `choice` at `frame`, read back and recorded.
    void setPriority(std::uint64_t frame, const PriorityChoice& choice) {
        if (aboveNormal == choice.aboveNormal) {
            return;
        }
        aboveNormal = choice.aboveNormal;
        const Status set = platform_win::setProcessPriority(choice.aboveNormal ? platform_win::ProcessPriority::AboveNormal
                                                                               : platform_win::ProcessPriority::Normal);
        const platform_win::ProcessPriority now = platform_win::currentProcessPriority();
        if (!set) {
            spdlog::warn("Frame {}: the process priority stays {} ({})", frame, platform_win::toString(now),
                         set.error().toString());
        } else {
            spdlog::info("Frame {}: process priority {} ({})", frame, platform_win::toString(now), choice.reason);
        }
        priority.changes.push_back(PriorityChangeRecord{frame, std::string(platform_win::toString(now)),
                                                        std::string(choice.reason), set.has_value()});
    }

    /// The screens' service on the scene's CRTs (`heroScreenPrefix`: those whose object ids start with it, none for an
    /// empty one: a floor whose eye's room has no CRT bound yet, I4 step 6b), if it has any (not in the model viewer);
    /// the run's service, kept unbound across a scene shown in place of another, is bound to them instead
    /// (VideoRecord::binds).
    void createScreen(const AppContext& context, std::vector<ScreenGame> games,
                      const std::optional<std::string>& heroScreenPrefix = std::nullopt) {
        const CommandLineOptions& options = context.options();
        if (options.model) {
            return;
        }
        if (std::vector<ScreenBinding> screens =
                findScreens(description, *scene, graphics->materials(), heroScreenPrefix);
            !screens.empty()) {
            if (screen) {
                screen->bind(std::move(screens), std::move(games), context.frameIndex());
                return;
            }
            ScreenServiceOptions screenOptions;
            screenOptions.freezePtsSeconds = options.videoFreezePtsSeconds;
            screenOptions.lockstep = options.fixedDeltaSeconds.has_value();
            // I5 step 1: the rooms' playlists and the title cards' fonts; step 2: the decoder budget; step 5: the sound.
            screenOptions.video = videoSettings(context);
            screenOptions.audio = audio.get();
            screenOptions.recordPresentedFrames = options.audioCapture.has_value();
            screenOptions.maxDecoders = options.maxDecoders;
            screenOptions.watchLateFrames = options.watchLateFrames;
            screenOptions.assetRoot = context.paths().assetRoot();
            // I5 step 7: the clips' verdicts, in the user data's cache.
            screenOptions.verdictsFile = media::videoVerdictsFile(context.paths().cacheDir());
            screen = std::make_unique<ScreenService>(std::move(screens), graphics->materials(), std::move(games),
                                                     screenOptions);
        } else if (heroScreenPrefix && heroScreenPrefix->empty()) {
            spdlog::info("Scene '{}': no hero CRT bound yet (the CRT of the room that holds the eye, once it stays in "
                         "one)",
                         description.name);
        } else {
            spdlog::info("Scene '{}' has no hero CRT (a crt object with a picture on its screen){}: no video",
                         description.name, heroScreenPrefix ? " in " + *heroScreenPrefix : std::string());
        }
    }

    /// The generated building of the run (I3 step 10b's room, I4 step 5a's floor): the worker pool and the session that
    /// makes and shows it; `decided`: the test room the run shows already for want of a catalog.
    void startWorld(AppContext& context, std::optional<WorldResult> decided) {
        const core::Paths& paths = context.paths();
        if (!decided) {
            // Every procedural surface now, as the test room's start makes them: the room's commit then only builds
            // and uploads its meshes (a surface generated at the commit would hold that frame for its time).
            for (const assets::ProceduralSurfaceInfo& surface : assets::proceduralSurfaces()) {
                static_cast<void>(graphics->textures().procedural(
                    fmt::format("{}/{}", surface.name, assets::toString(assets::SurfaceMap::BaseColor)),
                    gl::ColorSpace::Srgb));
            }
        }
        pool = std::make_unique<core::WorkerPool>();
        WorldSession::Options options;
        options.paths = WorldPaths{paths.assetRoot(), paths.userDataRoot(), paths.configDir(), paths.cacheDir()};
        options.startRoom = startRoomOf(context.options());
        options.writer = fmt::format("retroverse ({})", core::versionBanner());
        const bool lockstep = context.options().fixedDeltaSeconds.has_value();
        options.lockstep = lockstep;
        options.processCreated = platform_win::currentProcessIdentity().creationTime;
        WorldSession::SceneHost host;
        // I3 step 10c: the stored plan's floor replaced by its reconciliation keeps the walker where it stands; the
        // start room's hero CRT plays its games (I4 step 5a).
        host.showScene = [this, &context](scene::SceneDescription next, const WorldSession::SceneShow& show) {
            return replaceScene(context, std::move(next), scene::assetModelLoader(context.paths().assetRoot()),
                                show.keepWalker, show.heroScreenPrefix, show.walkerMove, show.screenGames,
                                show.atCamera);
        };
        // I4 step 7b: the teleports on the floor shown (the walker at a room's doorway, at the elevator).
        host.teleport = [this](const scene::CameraSetup& at) {
            freeRoam->teleportToCamera(at);
            freeRoam->update(input::ActionState{}, 0.0f, camera);
            // A cut: the light of the place reached at once, never eased from the place left (I6 step 1).
            if (scene) {
                scene->snapLighting();
            }
        };
        host.walker = [this]() -> scene::WalkerPose {
            const scene::FirstPersonController& walker = freeRoam->controller();
            return scene::WalkerPose{walker.feetPosition(), walker.yaw(), walker.pitch()};
        };
        // I4b step 4: a room's static part rebuilt alone in place (the walker where it stands), and a held fixture's
        // render items.
        host.replaceObjects = [this](const SceneObjectsChange& change) -> bool {
            if (!scene) {
                return false;
            }
            auto replaced = scene->replaceObjects(description, change, graphics->materialLibrary(), loadModel,
                                                  graphics->resources());
            if (!replaced) {
                spdlog::error("World: the objects of {} cannot be replaced: {}", change.prefix,
                              replaced.error().toString());
                if (replaced.error().code == ErrorCode::InvalidArgument) {
                    return false; // nothing changed
                }
            }
            freeRoam->setCollision(scene->collisionWorld());
            shownDebugLines = DebugLineSelection{};
            if (Status shown = syncDebugLines(); !shown) {
                spdlog::error("Debug lines unavailable: {}", shown.error().toString());
            }
            return true;
        };
        host.sceneItemsOf = [this](const std::function<bool(std::string_view)>& match) {
            return scene ? scene->itemsOf(match) : std::vector<std::size_t>{};
        };
        // I4b step 4's review: a held lamp's light follows it (the scene's lights moved in place, packed again).
        host.sceneLightsOf = [this](const std::function<bool(std::string_view)>& match) {
            std::vector<std::pair<std::size_t, scene::Light>> lights;
            if (scene) {
                const std::span<const scene::Light> all = std::as_const(*scene).sceneLights();
                for (std::size_t index = 0; index < all.size(); ++index) {
                    if (match(all[index].name)) {
                        lights.emplace_back(index, all[index]);
                    }
                }
            }
            return lights;
        };
        host.setSceneLights = [this](std::span<const std::pair<std::size_t, scene::Light>> lights) {
            if (!scene) {
                return;
            }
            const std::span<scene::Light> all = scene->sceneLights();
            for (const auto& [index, light] : lights) {
                if (index < all.size() && all[index].name == light.name) {
                    all[index] = light;
                }
            }
            scene->applyLighting();
        };
        // I4 step 6b: the CRT of the room that holds the eye, bound once the eye stayed there (never joining the
        // previous clip's decoder on the render loop).
        host.bindScreen = [this, &context](const std::string& prefix, std::vector<ScreenGame> games) -> bool {
            if (!scene || context.options().model) {
                return false;
            }
            std::vector<ScreenBinding> screens = findScreens(description, *scene, graphics->materials(), prefix);
            if (screens.empty()) {
                return false;
            }
            if (screen) {
                screen->bind(std::move(screens), std::move(games), context.frameIndex());
            } else {
                createScreen(context, std::move(games), prefix);
            }
            return true;
        };
        host.setScreenGames = [this](std::vector<ScreenGame> games) {
            if (screen) {
                screen->setGames(std::move(games));
            } else if (!games.empty()) {
                spdlog::info("World: the room has no hero CRT for its {} game(s) with media", games.size());
            }
        };
        host.showTestRoom = [this, &context]() -> Status {
            auto testRoom = sceneFileSource(std::nullopt, context.paths());
            if (!testRoom) {
                return propagate(testRoom.error());
            }
            return replaceScene(context, std::move(testRoom->description), std::move(testRoom->loadModel));
        };
        host.camera = [this]() -> const scene::Camera& {
            return camera;
        };
        // The area that holds the eye now (the camera's: the scene's tracker follows it before each view).
        host.eyeArea = [this]() -> std::string {
            if (!scene || scene->areas().empty()) {
                return {};
            }
            if (const std::optional<std::size_t> index = scene::areaAt(scene->areas(), camera.position())) {
                return scene->areas()[*index].id;
            }
            return std::string(scene->areaId());
        };
        const std::string start =
            options.startRoom ? "starting in " + options.startRoom->toString() : std::string("starting in the lobby");
        world = std::make_unique<WorldSession>(context, std::move(options), *pool, *graphics, std::move(host),
                                               std::move(decided));
        if (product) {
            // Informazioni lists the other versions of a title the room's plan knows (I3 step 14b).
            product->setVersionLookup([this](const catalog::GameId& game) {
                return world ? world->otherVersionsOf(game) : std::vector<catalog::GameId>{};
            });
        }
        spdlog::info("World: the building, {}, on a worker pool of {} thread(s){}", start, pool->threadCount(),
                     lockstep ? " (lockstep: frames wait for the floor and its layers)" : "");
    }

    /// Shows `next` in place of the scene shown, at a frame boundary (the generated room after its shell, the test room
    /// instead of it; I3 step 10b): the content of the registries and the scene go, the new scene is instantiated, the
    /// walker stands at its camera (or at --spawn), the camera's lens is its, the hero CRT's service binds the new
    /// scene's CRT of `heroScreenPrefix` with `screenGames` (none: it stays unbound), the debug lines are made again.
    /// The world's collection is gone by then. The service lives on for the run (I4 step 6b): its screen let go first,
    /// a clip playing stops as a clip replacement does, its decoder's thread never joined here (ADR-0004: only at the
    /// run's end). `keepWalker`: the walker stays where it stands (I3 step 10c: a room committed again in place of
    /// itself), moved by `walkerMove` with the area that holds it (I4 step 6b, ADR-0013 §7).
    Status replaceScene(AppContext& context, scene::SceneDescription next, scene::ModelLoader loader,
                        bool keepWalker = false, const std::optional<std::string>& heroScreenPrefix = std::nullopt,
                        const std::optional<FrameMove>& walkerMove = std::nullopt,
                        std::vector<ScreenGame> screenGames = {}, bool atCamera = false) {
        // A ride's destination puts the walker at its camera (I4 step 7b), whatever --spawn said for the start.
        std::optional<SpawnPoint> spawn = keepWalker ? std::optional<SpawnPoint>(freeRoam->pose())
                                          : atCamera ? std::nullopt
                                                     : context.options().spawn;
        if (spawn && keepWalker && walkerMove) {
            spawn->feet = glm::vec3(walkerMove->matrix * glm::vec4(spawn->feet, 1.0f));
            if (walkerMove->halfTurn && spawn->yawDeg) {
                float yaw = *spawn->yawDeg + 180.0f;
                while (yaw > 180.0f) {
                    yaw -= 360.0f;
                }
                spawn->yawDeg = yaw;
            }
        }
        if (screen) {
            screen->unbind(false); // the registry is cleared below: its material goes with it
        }
        graphics->clearContent();
        // The overlay's panels refer to members of the scene instance (its environment, its lights' packing): the new
        // instance takes the old one's place in the same storage, which they keep referring to.
        scene.reset();
        description = std::move(next);
        loadModel = std::move(loader);
        auto instance =
            SceneInstance::create(description, graphics->materialLibrary(), loadModel, graphics->resources());
        if (!instance) {
            // Never without a scene (the views and the overlay's panels read it): an empty one in its place, with the
            // camera the failed one had, until the caller shows another.
            Error error = instance.error();
            scene::SceneDescription empty;
            empty.name = "empty";
            empty.camera = description.camera;
            empty.environment = description.environment;
            description = std::move(empty);
            if (auto blank =
                    SceneInstance::create(description, graphics->materialLibrary(), loadModel, graphics->resources())) {
                scene.emplace(std::move(*blank));
                freeRoam->resetScene(description, scene->collisionWorld(), spawn);
            }
            return propagate(std::move(error));
        }
        scene.emplace(std::move(*instance));
        freeRoam->resetScene(description, scene->collisionWorld(), spawn);
        camera.setLens(description.camera.lens);
        // The camera takes the walker's new pose now: this frame renders the new scene from it.
        freeRoam->update(input::ActionState{}, 0.0f, camera);
        createScreen(context, std::move(screenGames), heroScreenPrefix);
        shownDebugLines = DebugLineSelection{};
        if (Status shown = syncDebugLines(); !shown) {
            spdlog::error("Debug lines unavailable: {}", shown.error().toString());
        }
        return ok();
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
            levels.directoryOpen = product->directoryOpen();
        }
        return levels;
    }

    /// Who reads this frame's actions: the product UI while its catalog, the directory or its panel is open, the world
    /// otherwise.
    [[nodiscard]] InputContext inputContext() const noexcept {
        return product ? inputContextFor(product->catalogOpen() || product->directoryOpen(), product->panelOpen())
                       : InputContext::World;
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
                    input->releaseMouse(productOpen ? "the quick catalog or the directory opened; Esc or B leaves it"
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
    void back(AppContext& context) { performBack(context, backAction(backState())); }

    /// Performs the Back level `action` (backAction's, or the shell's level the world's interaction leaves to it).
    void performBack(AppContext& context, BackAction action) {
        switch (action) {
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
        case BackAction::DirectoryBack:
            product->directoryBack();
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
        devUi->addPanel(
            devui::makeStatsPanel({frameTimes, graphics->renderStats(), graphics->meshes(), graphics->materials(),
                                   graphics->textures(), gl::gpuMemoryLedger(),
                                   [this] { return screen ? screen->statusLines() : std::vector<std::string>{}; },
                                   [this] { return world ? world->statusLines() : std::vector<std::string>{}; }}),
            devui::DockSlot::Left);
        devUi->addPanel(devui::makeCameraPanel({camera, freeRoam->controller(), input->system(), collision,
                                                [this] { resetToSceneCamera(); }}),
                        devui::DockSlot::LeftBottom);
        // Tabs of one node; the first is shown.
        devUi->addPanel(devui::makeLightsPanel({[this] { return scene->sceneLights(); }, scene->environment(),
                                                scene->droppedLights(),
                                                scene->shadowLight(), scene->keyLight(), scene->shadowNote(),
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
        scene->snapLighting();
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
        scene->snapLighting();
        spdlog::info("Go to '{}': {}", object.id, freeRoam->describePose());
    }
#endif
};

RetroverseApp::RetroverseApp() = default;

RetroverseApp::~RetroverseApp() = default;

Status RetroverseApp::onInit(AppContext& context) {
    const core::Paths& paths = context.paths();
    const CommandLineOptions& options = context.options();
    // Without --scene, --model or --mock-catalog the run shows the generated room (I3 step 10b): its shell while it is
    // made, or the test room at once when the catalog cannot open at all.
    const bool room = roomMode(options);
    std::optional<WorldResult> decided = room ? roomWithoutCatalog(paths) : std::nullopt;
    auto source = loadSceneSource(options, paths, decided);
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
    // The games the hero CRT may play: the mock catalog's media (the ES-DE catalog's come with the room once its
    // snapshot answered: WorldSession's setScreenGames, I3 step 10c).
    std::vector<ScreenGame> screenGames = *mock ? (*mock)->screenGames() : std::vector<ScreenGame>{};
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
    state->loadModel = std::move(source->loadModel);
    state->debugGrid = source->grid;
    auto graphics = SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    state->graphics = std::move(*graphics);
    auto sceneInstance = SceneInstance::create(state->description, state->graphics->materialLibrary(),
                                               state->loadModel, state->graphics->resources());
    if (!sceneInstance) {
        return propagate(sceneInstance.error());
    }
    state->scene.emplace(std::move(*sceneInstance));
    if (!options.model) {
        state->createAudio(context);
    } else {
        state->audioRecord.device = std::string(audio::toString(audio::AudioOutput::Off));
        state->audioRecord.reason = "model-viewer";
    }
    state->createScreen(context, std::move(screenGames));
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
        // The screens' decoders stop and are waited for before a launch, and open again after it (ADR-0012 §6); the
        // process priority is normal before the emulator starts (I5 step 2).
        LaunchHooks hooks;
        hooks.starting = [raw, &context] {
            // ADR-0016 §9: the mix fades out (50 ms, begun before the screens' decoders stop, so that it runs while
            // they do) and the device stops, leaving the endpoint to the emulator.
            if (raw->audio) {
                raw->audio->beginSuspend();
            }
            if (raw->screen) {
                raw->screen->launchStarting();
            }
            if (raw->audio) {
                raw->audio->suspend();
                ++raw->audioRecord.suspends;
            }
            raw->setPriority(context.frameIndex(), processPriorityFor(false, true));
        };
        hooks.ended = [raw] {
            if (raw->screen) {
                raw->screen->launchEnded();
            }
            if (raw->audio) {
                raw->audio->resume();
            }
        };
        state->product->setLaunchHooks(std::move(hooks));
        if (room) {
            state->startWorld(context, std::move(decided));
        }
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
    // A lockstep run's first frame waits for the room (the host idles before it: onIdle).
    if (state->world && state->world->waits()) {
        context.setIdle(true);
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
    // The world's card takes the clicks on it while the mouse is free (I3 step 14a; keys and pads reach it as actions).
    if (state.world && state.product && !state.input->mouseCaptured() && state.product->handleWorldCardEvent(event)) {
        return;
    }
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
    state.stepSeconds = deltaSeconds;
    // Above normal while the view renders (I5 step 2, ADR-0016 §2); normal while minimized or during a game.
    state.setPriority(frame, processPriorityFor(context.window().isMinimized(),
                                                state.product != nullptr && state.product->launchActive()));
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
    // With a room (I3 step 13a) the world's interaction reads the World context: Back's levels (its own, then the
    // shell's it leaves to the application), the quick catalog, the hover, the pull-out and Focus; its context decides
    // what the walker gets.
    const WorldContext worldContext = state.world ? state.world->context() : WorldContext::FreeRoam;
    InteractionResult worldResult;
    // I4 step 7b: an elevator ride suspends the input (ADR-0013 §10): the world, the walker and the product UI read
    // nothing until it ends (a ride the frame's commands start included).
    bool riding = false;
    if (state.world) {
        // I3 step 15a: "Mostra nella stanza" and a launch's end before the interaction, Avvia after it; I4 step 7b: the
        // directory's commands (a ride, a teleport) there too.
        const scene::FirstPersonController& walker = state.freeRoam->controller();
        const scene::WalkerPose walkerNow{walker.feetPosition(), walker.yaw(), walker.pitch()};
        if (state.product) {
            state.world->beforeInteraction(*state.product, walkerNow);
        }
        riding = state.world->riding();
        worldResult = state.interactWorld(context,
                                          inputContext == InputContext::World && !riding ? &actions : nullptr,
                                          static_cast<float>(deltaSeconds));
        if (state.product) {
            state.world->afterInteraction(worldResult, *state.product, worldResult.walker.value_or(walkerNow));
        }
    }
    if (inputContext == InputContext::World) {
        if (state.world) {
            if (worldResult.shellBack) {
                state.performBack(context, *worldResult.shellBack);
            }
            if (worldResult.openCatalog) {
                state.openCatalog();
            }
            // I4 step 7b: the directory, at the elevator by interact (its prompt shows there), anywhere in free roam
            // by Map (M, the pad's Back/Select); never in Focus, Inspect, a launch or a ride: the frame began in free
            // roam and its interaction left it there, no ease driving the walker (an interact that took an item in this
            // frame, with Map pressed too, opens nothing).
            const bool freeRoamNow =
                worldContext == WorldContext::FreeRoam && state.world->context() == WorldContext::FreeRoam &&
                !worldResult.walker;
            if (state.product && !riding && freeRoamNow && !state.product->launchActive() &&
                !state.product->hasInput()) {
                if (actions.pressed(input::Action::Map)) {
                    state.world->openDirectory(*state.product, ui::DirectoryTab::Floors, "map");
                } else if (actions.pressed(input::Action::Interact) && state.world->idleAtElevator()) {
                    state.world->openDirectory(*state.product, ui::DirectoryTab::Floors, "elevator");
                } else if (actions.pressed(input::Action::Interact) && state.world->idleAtTotem()) {
                    // I6 step 5: the hub's directory totem opens it at its floors too.
                    state.world->openDirectory(*state.product, ui::DirectoryTab::Floors, "totem");
                } else if (actions.pressed(input::Action::Interact) && state.world->showcaseHovered()) {
                    // I6 step 5: a proxy of the hub's favourites showcase opens the directory at its card.
                    state.world->openShowcaseCard(*state.product);
                }
                state.syncCaptureLayers();
            }
        } else {
            if (actions.pressed(input::Action::Back)) {
                state.back(context);
            }
            if (catalogToggleRequested(inputContext, actions)) {
                state.openCatalog();
            }
        }
    } else if (catalogToggleRequested(inputContext, actions) && !state.product->panelOpen()) {
        state.product->closeCatalog();
    } else if (actions.pressed(input::Action::Map) && state.product->directoryOpen() && !riding) {
        // Map closes the directory it opens (I4 step 7b).
        state.product->closeDirectory();
    }

#if defined(RV_DEV_UI)
    state.updateOverlay(context);
#endif
    if (auto shown = state.syncDebugLines(); !shown) {
        spdlog::error("Debug lines unavailable: {}", shown.error().toString());
    }
    state.freeRoam->setCollisionEnabled(state.collision);
    if (worldResult.walker) {
        state.placeWalker(*worldResult.walker); // an ease of the world drives the walker: no actions this frame
    } else if (riding) {
        state.freeRoam->update(input::ActionState{}, static_cast<float>(deltaSeconds), state.camera);
    } else if (state.world) {
        // I4b step 3: the world's context sees the arrows too (the editor's walker does not walk while one steps); the
        // editor's menu holds the walker (its arrows choose a button).
        const WorldContext walkerContext = state.world->editHoldsWalker() ? WorldContext::Suspended : worldContext;
        state.freeRoam->update(walkerActionsFor(walkerContext, inputContext == InputContext::World
                                                                   ? actions
                                                                   : actionsFor(inputContext, actions)),
                               static_cast<float>(deltaSeconds), state.camera);
    } else {
        state.freeRoam->update(actionsFor(inputContext, actions), static_cast<float>(deltaSeconds), state.camera);
    }
    if (state.world && state.product) {
        // The hover's title label over the item as this frame's camera shows it (none under the product UI).
        const bool worldShown = inputContext == InputContext::World && !riding;
        state.product->setWorldLabel(worldShown ? state.world->label(state.camera, context.window().drawableSize())
                                                : std::nullopt);
        // The card beside the inspected item (I3 step 14a).
        state.world->showCard(worldResult, *state.product);
        // The elevator's prompt in front of a floor's elevator (I4 step 7b), naming the interact key of the device
        // used last.
        std::optional<std::string> prompt;
        bool totem = false;
        if (worldShown && !state.product->hasInput() && state.world->idleAtElevator()) {
            prompt = promptKeyLabel(state.input->system().map(), input::Action::Interact, actions.activeDevice());
        } else if (worldShown && !state.product->hasInput() && state.world->idleAtTotem()) {
            // I6 step 5: the hub's directory totem's prompt, "Indice".
            prompt = promptKeyLabel(state.input->system().map(), input::Action::Interact, actions.activeDevice());
            totem = true;
        }
        state.world->elevatorPrompt(*state.product, std::move(prompt), totem);
    }
    if (state.product) {
        // Text an --input-script types on this frame, into the product UI's focused field (the catalog's search).
        if (const input::InputScript* script = state.input->script()) {
            if (const std::string text = script->textAt(frame); !text.empty()) {
                state.product->typeText(text);
            }
        }
        // The UI commands only when the frame began in the UI's context (UI Back goes through back()); none during a
        // ride (I4 step 7b).
        const input::ActionState uiActions = actionsFor(InputContext::Ui, actions);
        state.product->update(inputContext == InputContext::Ui && !riding ? &uiActions : nullptr, deltaSeconds,
                              state.simulatedSeconds);
        // After this frame is presented the host waits: a launch starts, or an emulator has the screen.
        if (state.product->wantsIdle()) {
            context.setIdle(true);
        }
    }
    if (state.world) {
        // The room (I3 step 10b): its result committed at this frame boundary, its covers' round.
        state.world->update(*state.product, false, state.simulatedSeconds);
        // A lockstep run's frame waits for the layers of its view (I3 step 12).
        state.world->settle(*state.product);
    }
    state.updateScreens(frame, context.window().isMinimized());
    state.updateAudio(&actions);
    state.syncCaptureLayers();
    state.logScriptProgress(frame);
    state.graphics->pollShaderReload();
}

void RetroverseApp::onRender(AppContext& context) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    state.followSceneAreas(context.frameIndex());
    state.graphics->renderFrame(context, state.view(), state.screenshotRequested);
    state.screenshotRequested = false;
    if (state.world) {
        state.world->frameRendered();
        // A lockstep run waits for the room or its layers before its next frame.
        if (state.world->waits()) {
            context.setIdle(true);
        }
    }
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
    // A lockstep run's wait for the room (never while a game has the screen: nothing is committed or uploaded then).
    if (state.world && !state.product->launchActive()) {
        state.world->update(*state.product, true, state.simulatedSeconds);
    }
    context.setIdle(state.product->wantsIdle() || (state.world && state.world->waits()));
    if (!context.idle()) {
        // Frames resume: whatever the wait dropped (the release of the button that launched among them) is released.
        state.input->releaseAll();
    }
}

void RetroverseApp::onMinimized(AppContext& context, double elapsedSeconds) {
    if (!m_state) {
        return;
    }
    // No frame renders while minimized (I5 step 2's review): the time waited passes, the priority is normal, and the
    // screens' decoders pause at once and stop after kScreenStopAfterSeconds ("minimized").
    State& state = *m_state;
    state.simulatedSeconds += elapsedSeconds;
    state.setPriority(context.frameIndex(),
                      processPriorityFor(true, state.product != nullptr && state.product->launchActive()));
    state.updateScreens(context.frameIndex(), true);
    state.updateAudio(nullptr);
}

void RetroverseApp::onShutdown(AppContext& context) {
    if (m_state) {
        // The quit hook: a game still running is left running and recorded, before anything else ends.
        if (m_state->product) {
            m_state->product->finish();
        }
        // The room's jobs end (within the pool's bound) before what they feed goes; its records go into the report.
        if (m_state->world) {
            m_state->world->record(context.report());
        }
        if (m_state->pool) {
            m_state->pool->shutdown();
        }
        // A decoder stopped for a game still running stays stopped; every decoder thread is waited for.
        if (m_state->screen) {
            m_state->screen->shutdown();
            m_state->screen->record(context.report().video);
        }
        // The clips' sound (I5 step 5): the capture written, the section recorded (with the screens' part).
        m_state->recordAudio(context.report());
        // Normal for the rest of the exit (the joins and the report need no boost), the report's last change. The class
        // ends with the process: the console that started it never had it.
        m_state->setPriority(context.frameIndex(), PriorityChoice{false, "exit"});
        context.report().processPriority = m_state->priority;
        if (context.options().frameLimit) {
            m_state->freeRoam->logFinalPose(context.frameIndex(), m_state->simulatedSeconds);
        }
        context.report().finalCamera = m_state->freeRoam->pose();
        context.report().sceneAreas = m_state->sceneAreasRecord();
        if (m_state->scene) {
            context.report().sceneContents =
                SceneContentsRecord{m_state->scene->objectBounds().size(),
                                    m_state->scene->collisionWorld().boxes().size(),
                                    std::as_const(*m_state->scene).sceneLights().size()};
        }
        m_state->graphics->finishRun(context);
    }
    m_state.reset();
}

} // namespace rv::app
