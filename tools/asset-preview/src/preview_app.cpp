#include "preview_app.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <fmt/format.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <rv/app/back_action.hpp>
#include <rv/app/bindings.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/model_viewer.hpp>
#include <rv/app/overlay_policy.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/file_watch_set.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/devui/dev_ui.hpp>
#include <rv/devui/frame_time_history.hpp>
#include <rv/devui/panels.hpp>
#include <rv/input/action.hpp>
#include <rv/input/input_system.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_collector.hpp>
#include <rv/preview/asset_report.hpp>
#include <rv/preview/preview_controls.hpp>
#include <rv/preview/preview_overlay.hpp>
#include <rv/preview/preview_scene.hpp>
#include <rv/preview/preview_subject.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/gpu_model.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/orbit_controller.hpp>

#include "preview_panels.hpp"

namespace rv::preview {
namespace {

/// Where the overlay keeps the preview's window layout, under <user data>/config (its panels are not the app's).
constexpr std::string_view kLayoutFile = "asset-preview-imgui.ini";
/// Labels of the scene: a dark plate behind the text, a dot on the labelled point.
constexpr LabelLayout kLabelLayout{};
constexpr ImU32 kLabelPlate = IM_COL32(0, 0, 0, 170);

/// Why a subject is loaded, which decides what the view keeps.
enum class Load : std::uint8_t {
    Start,  ///< the target of the command line: framed; its manifest comes forward unless --panel chose a tab
    Open,   ///< another target (a dropped file): framed; its manifest comes forward
    Reload, ///< the same target again (its files changed, the Reload button): the camera and the tabs stay
};

/// Width / height of a viewport, 16:9 for an empty one.
float aspectOf(glm::ivec2 size) noexcept {
    return size.x > 0 && size.y > 0 ? static_cast<float>(size.x) / static_cast<float>(size.y) : 16.0f / 9.0f;
}

void logMessages(const PreviewSubject& subject) {
    for (const PreviewMessage& message : subject.messages) {
        switch (message.severity) {
        case PreviewMessage::Severity::Info:
            spdlog::info("{}: {}", subject.title, message.text);
            break;
        case PreviewMessage::Severity::Warning:
            spdlog::warn("{}: {}", subject.title, message.text);
            break;
        case PreviewMessage::Severity::Error:
            spdlog::error("{}: {}", subject.title, message.text);
            break;
        }
    }
}

/// The mouse buttons that drag the camera, as bits of a mask.
std::uint8_t dragBit(std::uint8_t button) noexcept {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return 1u;
    case SDL_BUTTON_MIDDLE:
        return 2u;
    case SDL_BUTTON_RIGHT:
        return 4u;
    default:
        return 0u;
    }
}

} // namespace

struct PreviewApp::State {
    /// Shaders, textures, registries, the renderer and the outputs. Declared first: destroyed last.
    std::unique_ptr<app::SceneGraphics> graphics;
    std::filesystem::path assetRoot;

    PreviewSubject subject;
    std::uint64_t subjectRevision = 0;
    AssetReport report;
    std::vector<ScreenSurface> screens;
    std::optional<app::SceneInstance> scene;
    /// The scene's render items, the screens of a glTF model drawn with the screen pattern.
    std::vector<scene::RenderItem> items;

    ViewMode viewMode = ViewMode::Lit;
    OverlaySelection overlays;
    std::optional<OverlaySelection> shownOverlays; ///< the selection the debug lines show; empty: to be rebuilt
    std::vector<SceneLabel> labels;
    scene::OrbitController orbit;
    scene::Camera camera;
    /// The camera of the frame being built, which the labels are placed with.
    glm::mat4 labelViewProjection{1.0f};
    /// The labels in view in the frame being built: their index in `labels`, screen point and text size, and their
    /// plates (placeLabelPlates). Kept from frame to frame for their storage.
    std::vector<std::size_t> shownLabels;
    std::vector<glm::vec2> labelAnchors;
    std::vector<glm::vec2> labelSizes;
    std::vector<glm::vec4> labelPlates;
    /// The subject is to be framed (a new subject, F, the Frame button).
    bool framePending = false;
    /// The framing takes in the sockets and scale references around the subject (F again, the empty studio), not the
    /// subject's bounds alone (framingBounds).
    bool frameSurroundings = false;
    /// The part of the view the camera was framed in while the user has not moved it since: the framing follows the
    /// panels' layout (which settles over the first frames and changes with the window) until the user takes the
    /// camera.
    std::optional<scene::ViewArea> autoFramedArea;
    double simulatedSeconds = 0.0;

    platform::SdlInputCollector collector;
    input::InputSystem input;
    /// Mouse buttons held since a press over the scene (dragBit): their motion moves the camera.
    std::uint8_t dragButtons = 0;
    /// A file dropped on the window, loaded at the next update.
    std::optional<std::string> droppedFile;
    /// The Reload button was pressed: the subject is loaded again at the next update.
    bool reloadRequested = false;
    core::FileWatchSet watch;
    bool screenshotRequested = false;
    /// --panel was given: the start shows that tab, not the Manifest one.
    bool startPanelChosen = false;

    devui::FrameTimeHistory frameTimes;
    /// The panels. Declared last: destroyed first, before the state they refer to.
    std::unique_ptr<devui::DevUi> devUi;

    /// Loads `target` and shows it.
    void load(const PreviewTarget& target, Load kind) {
        spdlog::info("Previewing {}", target.label());
        show(loadPreviewSubject(target, assetRoot), kind);
    }

    /// Replaces the content with `loaded`. A new subject is framed; its Manifest panel comes forward when it brings a
    /// manifest that was not shown before (bringsManifestForward), except at a start that chose its tab (--panel).
    void show(PreviewSubject loaded, Load kind) {
        const bool manifestForward =
            bringsManifestForward(subject, loaded, kind == Load::Reload) && !(kind == Load::Start && startPanelChosen);
        subject = std::move(loaded);
        ++subjectRevision;
        logMessages(subject);
        graphics->clearContent();
        scene.reset();
        items.clear();
        const scene::SceneDescription description = makePreviewScene(subject);
        auto instance = app::SceneInstance::create(description, graphics->materialLibrary(),
                                                   previewModelLoader(subject), graphics->resources());
        if (instance) {
            scene.emplace(std::move(*instance));
            items.assign(scene->renderItems().begin(), scene->renderItems().end());
        } else {
            spdlog::error("{} cannot be shown: {}", subject.title, instance.error().toString());
        }
        report = buildAssetReport(subject);
        screens = screenSurfaces(subject);
        showScreenPatterns();
        shownOverlays.reset();
        watch.assign(subject.watchedFiles);
        if (kind != Load::Reload) {
            framePending = true;
            frameSurroundings = false;
        }
        if (manifestForward && devUi) {
            devUi->focusPanel(kManifestPanelTitle);
        }
    }

    /// Draws the screens of a glTF model with the screen material showing the test pattern, sized to each screen.
    void showScreenPatterns() {
        if (!subject.model || screens.empty() || !scene) {
            return;
        }
        gl::MaterialRegistry& materials = graphics->materials();
        gl::TextureCache& textures = graphics->textures();
        const assets::MaterialDesc& screenMaterial = graphics->materialLibrary().getOrFallback(kScreenMaterialName);
        const std::string modelKey = core::pathToUtf8(subject.modelFile); // the model object's path in the scene
        std::vector<std::pair<scene::MeshHandle, scene::MaterialHandle>> replaced;
        for (const ScreenSurface& screen : screens) {
            gl::MaterialTextures slots = gl::resolveMaterialTextures(screenMaterial, textures);
            slots[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] =
                &textures.pattern(kScreenPattern, "", 0, screen.patternSize, gl::ColorSpace::Srgb);
            const std::string key = "preview#" + screen.socketId;
            const scene::MaterialHandle material =
                materials.add(key, fmt::format("{} ({})", screenMaterial.name, key), screenMaterial, slots);
            for (const std::uint32_t mesh : screen.modelMeshes) {
                if (const auto handle = graphics->meshes().find(gl::GpuModel::meshKey(modelKey, mesh))) {
                    replaced.emplace_back(*handle, material);
                }
            }
        }
        if (auto uploaded = materials.upload(graphics->resources().uniformBufferOffsetAlignment, "MaterialUniforms");
            !uploaded) {
            spdlog::error("The screen pattern cannot be shown: {}", uploaded.error().toString());
            return;
        }
        for (scene::RenderItem& item : items) {
            for (const auto& [mesh, material] : replaced) {
                if (item.mesh == mesh) {
                    item.material = material;
                    item.shader = materials.material(material).shader;
                }
            }
        }
        spdlog::info("{}: {} screen surface(s) show the {} pattern", subject.title, screens.size(), kScreenPattern);
    }

    /// F, the Frame button: frames the subject; pressed again while the camera still shows that framing, frames it with
    /// its surroundings (its sockets and scale references), and back.
    void requestFraming() {
        frameSurroundings = autoFramedArea.has_value() && !framePending ? !frameSurroundings : false;
        framePending = true;
    }

    /// Points the camera at the subject from the front right and above (the model viewer's angles), framed in `area`,
    /// the part of the view the panels leave visible. The empty studio frames its scale references.
    void frameSubject(glm::ivec2 viewport, const scene::ViewArea& area) {
        const bool surroundings = frameSurroundings || !subject.target;
        orbit.frame(framingBounds(subject, overlays, surroundings), -app::kModelViewerAzimuthDeg,
                    -app::kModelViewerElevationDeg, camera.lens(), aspectOf(viewport), area);
        framePending = false;
        autoFramedArea = area;
        spdlog::debug("Framed {} from {:.3f} m in the view area ({:.2f}, {:.2f}) to ({:.2f}, {:.2f})",
                      surroundings ? "the subject and its surroundings" : "the subject", orbit.distance(), area.min.x,
                      area.min.y, area.max.x, area.max.y);
    }

    [[nodiscard]] scene::ViewArea visibleArea() const {
        return devUi ? devUi->sceneViewArea().value_or(scene::ViewArea{}) : scene::ViewArea{};
    }

    /// The debug lines and labels of the overlays, rebuilt when the selection or the subject changed.
    void syncOverlays() {
        if (shownOverlays == overlays) {
            return;
        }
        shownOverlays = overlays;
        PreviewOverlay overlay = buildPreviewOverlay(subject, overlays);
        labels = std::move(overlay.labels);
        if (auto shown = graphics->setDebugLines(overlay.lines); !shown) {
            spdlog::error("The overlays cannot be drawn: {}", shown.error().toString());
        }
    }

    /// Draws the labels over the scene (the overlay's background draw list, under the panels): each on a plate above
    /// and right of its point, moved up or down clear of the plates before it and of every labelled point, with a
    /// leader line then (placeLabelPlates); the dots last.
    void drawLabels() {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        shownLabels.clear();
        labelAnchors.clear();
        labelSizes.clear();
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const std::optional<glm::vec2> at =
                projectToViewport(labelViewProjection, labels[index].position, glm::vec2(display.x, display.y));
            if (at) {
                const ImVec2 size = ImGui::CalcTextSize(labels[index].text.c_str());
                shownLabels.push_back(index);
                labelAnchors.push_back(*at);
                labelSizes.emplace_back(size.x, size.y);
            }
        }
        placeLabelPlates(labelAnchors, labelSizes, kLabelLayout, labelPlates);
        for (std::size_t shown = 0; shown < shownLabels.size(); ++shown) {
            const SceneLabel& label = labels[shownLabels[shown]];
            const glm::vec2 at = labelAnchors[shown];
            const glm::vec4& plate = labelPlates[shown];
            const ImU32 color =
                ImGui::ColorConvertFloat4ToU32(ImVec4(label.color.r, label.color.g, label.color.b, label.color.a));
            if (plateMoved(plate, at, kLabelLayout)) {
                draw->AddLine(ImVec2(at.x, at.y), ImVec2(plate.x, 0.5f * (plate.y + plate.w)), color);
            }
            draw->AddRectFilled(ImVec2(plate.x, plate.y), ImVec2(plate.z, plate.w), kLabelPlate, kLabelLayout.padding);
            draw->AddText(ImVec2(plate.x + kLabelLayout.padding, plate.y + kLabelLayout.padding), color,
                          label.text.c_str());
        }
        for (std::size_t shown = 0; shown < shownLabels.size(); ++shown) {
            const glm::vec4& color = labels[shownLabels[shown]].color;
            draw->AddCircleFilled(ImVec2(labelAnchors[shown].x, labelAnchors[shown].y), kLabelLayout.dotRadius,
                                  ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a)));
        }
    }

    /// Back (Esc, gamepad B): leaves one level: a popup of the panels, the panels, the tool.
    void back(app::AppContext& context) {
        app::BackState levels;
        levels.overlayVisible = devUi->visible();
        levels.overlayHasKeyboard = devUi->hasKeyboard();
        levels.overlayPopupOpen = devUi->popupOpen();
        switch (app::backAction(levels)) {
        case app::BackAction::Ignore:
        case app::BackAction::ReleaseMouse: // the preview never captures the mouse
            break;
        case app::BackAction::ClosePopup:
            devUi->closePopup();
            break;
        case app::BackAction::CloseOverlay:
            devUi->setVisible(false);
            break;
        case app::BackAction::MenuBack:
        case app::BackAction::OpenMenu: // no product UI here
            break;
        case app::BackAction::Quit:
            spdlog::info("Back pressed, quitting");
            context.requestQuit();
            break;
        }
    }

    /// Creates the overlay with the preview's panels, visible from the start, drawn over the window and the
    /// screenshots; the labels of the scene stay when the panels are hidden.
    Status createDevUi(app::AppContext& context) {
        devui::DevUiConfig config;
        if (!context.options().frameLimit) {
            config.iniFile = context.paths().configDir() / core::pathFromUtf8(kLayoutFile);
        }
        auto created = devui::DevUi::create(context.window(), config);
        if (!created) {
            return propagate(created.error());
        }
        devUi = std::move(*created);
        const PreviewPanelModel model{subject,
                                      report,
                                      screens,
                                      subjectRevision,
                                      viewMode,
                                      overlays,
                                      orbit,
                                      graphics->textures(),
                                      graphics->materials(),
                                      [this] { requestFraming(); },
                                      [this] { reloadRequested = true; }};
        devUi->addPanel(makeAssetPanel(model), devui::DockSlot::Left);
        devUi->addPanel(makeModelPanel(model), devui::DockSlot::LeftBottom);
        // Tabs of one node; the first is shown (the Manifest tab is brought forward when a manifest is loaded).
        devUi->addPanel(makeStatisticsPanel(model), devui::DockSlot::Right);
        devUi->addPanel(devui::makeMaterialsPanel({graphics->materials(),
                                                   [this] { return std::span<const scene::RenderItem>(items); },
                                                   graphics->textures()}),
                        devui::DockSlot::Right);
        devUi->addPanel(makeManifestPanel(model), devui::DockSlot::Right);
        devUi->addPanel(makeScreenPanel(model), devui::DockSlot::RightBottom);
        devUi->addPanel(devui::makeStatsPanel({frameTimes, graphics->renderStats(), graphics->meshes(),
                                               graphics->materials(), graphics->textures(),
                                               gl::gpuMemoryLedger()}),
                        devui::DockSlot::RightBottom);
        devUi->addPanel(makeWarningsPanel(model), devui::DockSlot::Bottom);
        devUi->addPanel(devui::makeLogPanel(), devui::DockSlot::Bottom);
        devUi->setSceneDecoration([this] { drawLabels(); });
        graphics->setOverlay(
            [this](gl::GLDevice& device) { devUi->renderToWindow(device); },
            [this](gl::GLDevice& device, const gl::Framebuffer& target) { devUi->renderTo(device, target); });
        devUi->setVisible(true);
        return ok();
    }
};

PreviewApp::PreviewApp(PreviewOptions options) : m_options(std::move(options)) {}

PreviewApp::~PreviewApp() = default;

Status PreviewApp::onInit(app::AppContext& context) {
    auto state = std::make_unique<State>();
    state->assetRoot = context.paths().assetRoot();
    auto graphics = app::SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    state->graphics = std::move(*graphics);
    state->input = input::InputSystem(app::loadBindings(context.paths()));
    state->collector.gamepads().openConnected();
    // The panels are the tool: without them there is nothing to inspect.
    if (auto created = state->createDevUi(context); !created) {
        Error error = created.error();
        error.message = "the panels of the asset preview cannot start: " + error.message;
        return propagate(std::move(error));
    }
    const app::CommandLineOptions& run = context.options();
    if (run.screenshot && run.frameLimit && *run.frameLimit < kPanelsSettledFrames) {
        spdlog::warn("--screenshot of frame {}: the panels are laid out from frame {} on (--frames {} or more)",
                     *run.frameLimit - 1, kPanelsSettledFrames - 1, kPanelsSettledFrames);
    }
    state->viewMode = m_options.view;
    state->startPanelChosen = m_options.panel.has_value();
    if (m_options.target) {
        state->load(*m_options.target, Load::Start);
    } else {
        spdlog::info("No file to preview yet: drop a .glb, .gltf or manifest .json file on the window");
        state->show(PreviewSubject{}, Load::Start);
    }
    if (m_options.panel) {
        state->devUi->focusPanel(*m_options.panel);
    }
    m_state = std::move(state);
    return ok();
}

void PreviewApp::onEvent(app::AppContext& /*context*/, const SDL_Event& event) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    // The panels see events first and keep what they use (a click on a panel, keys while a field is edited).
    if (state.devUi->handleEvent(event, false)) {
        return;
    }
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
        state.droppedFile = std::string(event.drop.data);
        return;
    }
    // A press over the scene starts a drag; its motion moves the camera until the button is released.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        state.dragButtons = static_cast<std::uint8_t>(state.dragButtons | dragBit(event.button.button));
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        state.dragButtons = static_cast<std::uint8_t>(state.dragButtons & ~dragBit(event.button.button));
    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        state.dragButtons = 0;
    }
    state.collector.setMouseCaptured(state.dragButtons != 0);
    state.collector.handleEvent(event);
}

void PreviewApp::onUpdate(app::AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    state.simulatedSeconds += deltaSeconds;
    state.frameTimes.record(context.clock().deltaSeconds());
    const input::RawInputFrame raw = state.collector.endFrame();
    const input::ActionState actions = state.input.resolve(raw);
    if (actions.pressed(input::Action::ToggleDevUI)) {
        state.devUi->setVisible(!state.devUi->visible());
    }
    if (actions.pressed(input::Action::Back)) {
        state.back(context);
    }
    if (actions.pressed(input::Action::ReloadShaders)) {
        state.graphics->reloadShaders();
    }
    if (actions.pressed(input::Action::Screenshot)) {
        state.screenshotRequested = true;
    }

    // A dropped file, or the subject's files changed on disk.
    if (state.droppedFile) {
        const std::string file = std::exchange(state.droppedFile, std::nullopt).value();
        if (auto target = parsePreviewTarget(file)) {
            state.load(*target, Load::Open);
        } else {
            spdlog::warn("Dropped file not previewed: {}", target.error().toString());
        }
    } else if (std::exchange(state.reloadRequested, false) && state.subject.target) {
        state.load(*state.subject.target, Load::Reload);
    } else if (const std::vector<std::filesystem::path> changed = state.watch.changed();
               !changed.empty() && state.subject.target) {
        spdlog::info("Reloading {}: {} changed on disk", state.subject.title, core::pathToGenericUtf8(changed.front()));
        state.load(*state.subject.target, Load::Reload);
    }

    const PreviewShortcuts shortcuts = previewShortcutsFrom(raw);
    if (shortcuts.frameSubject) {
        state.requestFraming();
    }
    if (shortcuts.toggleAutoRotate) {
        state.orbit.setAutoRotate(!state.orbit.autoRotate());
    }
    if (shortcuts.viewMode) {
        state.viewMode = *shortcuts.viewMode;
    }

    // The camera: framed in the part of the view the panels leave visible, following it until the user takes the
    // camera over, then orbited.
    const glm::ivec2 viewport = context.framebufferSize();
    const scene::ViewArea area = state.visibleArea();
    state.camera.setAspect(aspectOf(viewport));
    if (state.framePending || (state.autoFramedArea && *state.autoFramedArea != area)) {
        state.frameSubject(viewport, area);
    }
    const scene::OrbitInput orbitInput = orbitInputFrom(raw);
    if (movesCamera(orbitInput) || state.orbit.autoRotate()) {
        state.autoFramedArea.reset();
    }
    state.orbit.update(orbitInput, static_cast<float>(deltaSeconds), state.camera,
                       static_cast<float>(context.window().windowSize().y));
    state.orbit.apply(state.camera, area);
    state.labelViewProjection = state.camera.viewProjection();

    state.syncOverlays();
    state.devUi->buildFrame(
        app::overlayCaptureSize(context.options(), context.frameIndex(), state.screenshotRequested));
    state.graphics->pollShaderReload();
}

void PreviewApp::onRender(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    // The view mode as it stands after the panels were built (their radio buttons change it): the surfaces' program
    // and the wireframe flag below come from the same mode.
    if (auto surfaces = state.graphics->setSurfaceView(surfaceViewOf(state.viewMode)); !surfaces) {
        spdlog::error("View mode {} unavailable: {}", toString(state.viewMode), surfaces.error().toString());
        state.viewMode = ViewMode::Lit;
        (void)state.graphics->setSurfaceView(gl::SurfaceView::Lit); // Lit loads no program: it cannot fail
    }
    gl::RenderView view;
    if (state.scene) {
        view = app::makeRenderView(*state.scene, state.camera);
    } else {
        view.camera = state.camera;
    }
    view.items = state.items;
    view.post.antialiasing = true;
    view.wireframe = wireframeOf(state.viewMode);
    view.timeSeconds = static_cast<float>(state.simulatedSeconds);
    state.graphics->renderFrame(context, view, state.screenshotRequested);
    state.screenshotRequested = false;
}

void PreviewApp::onShutdown(app::AppContext& context) {
    if (m_state) {
        if (context.options().frameLimit) {
            spdlog::info("Final view of {}: {}, target ({:.3f}, {:.3f}, {:.3f}), distance {:.3f} m",
                         m_state->subject.title.empty() ? std::string("the empty preview") : m_state->subject.title,
                         toString(m_state->viewMode), m_state->orbit.target().x, m_state->orbit.target().y,
                         m_state->orbit.target().z, m_state->orbit.distance());
        }
        m_state->graphics->finishRun(context);
    }
    m_state.reset();
}

} // namespace rv::preview
