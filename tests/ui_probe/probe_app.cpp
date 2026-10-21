#include "probe_app.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/bindings.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/ui/directory_model.hpp>
#include <rv/ui/directory_screen.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/ui_command.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

#include "directory_fixture.hpp"

namespace rv::probe {
namespace {

namespace fixture = directory_fixture;

/// PNG files directly in `directory` (none when it does not exist).
std::vector<std::filesystem::path> pngFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (core::equalsIgnoreCaseAscii(core::pathToUtf8(it->path().extension()), ".png")) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// Moves `from` to `to` (replacing it), copying when a rename across volumes fails.
bool moveFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (to.has_parent_path()) {
        std::filesystem::create_directories(to.parent_path(), error);
    }
    std::filesystem::remove(to, error);
    error.clear();
    std::filesystem::rename(from, to, error);
    if (!error) {
        return true;
    }
    error.clear();
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return false;
    }
    std::filesystem::remove(from, error);
    return true;
}

nlohmann::json boxJson(const std::optional<glm::vec4>& box) {
    if (!box) {
        return nullptr;
    }
    return nlohmann::json::array({box->x, box->y, box->z, box->w});
}

const fixture::Checkpoint* checkpointAt(std::uint64_t frame) {
    for (const fixture::Checkpoint& checkpoint : fixture::kCheckpoints) {
        if (checkpoint.frame == frame) {
            return &checkpoint;
        }
    }
    return nullptr;
}

} // namespace

struct UiProbeApp::State {
    /// Declared first: destroyed last.
    std::unique_ptr<app::SceneGraphics> graphics;
    std::optional<app::SceneInstance> scene;
    scene::Camera camera;
    input::InputSystem input;
    std::optional<input::InputScriptPlayer> script;
    ui::NavInput nav;
    std::unique_ptr<ui::UiSystem> ui;
    std::unique_ptr<ui::UiView> view;
    std::unique_ptr<ui::DirectoryScreen> screen;
    ui::DirectoryModel model;
    /// The last card request answered (directory_fixture::fillCard).
    std::uint64_t cardServed = 0;
    double simulatedSeconds = 0.0;
    nlohmann::json checkpoints = nlohmann::json::array();
    nlohmann::json commands = nlohmann::json::array();
    nlohmann::json shots = nlohmann::json::array();
    bool shotMissing = false;

    void record(std::uint64_t frame, const std::optional<ui::DirectoryCommand>& command) {
        if (!command) {
            return;
        }
        spdlog::info("Frame {}: the directory asks {} '{}'", frame, ui::toString(command->kind), command->id);
        commands.push_back({{"frame", frame}, {"kind", ui::toString(command->kind)}, {"id", command->id}});
    }
};

UiProbeApp::UiProbeApp(UiProbeOptions options) : m_options(std::move(options)) {}

UiProbeApp::~UiProbeApp() = default;

Status UiProbeApp::onInit(app::AppContext& context) {
    auto state = std::make_unique<State>();
    State& s = *state;
    const core::Paths& paths = context.paths();

    auto graphics = app::SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    s.graphics = std::move(*graphics);
    const std::filesystem::path scenePath = app::resolveScenePath(context.options().scene, paths.assetRoot());
    auto description = scene::loadSceneDescription(scenePath);
    if (!description) {
        return propagate(description.error());
    }
    auto instance = app::SceneInstance::create(*description, s.graphics->materialLibrary(),
                                               scene::assetModelLoader(paths.assetRoot()), s.graphics->resources());
    if (!instance) {
        return propagate(instance.error());
    }
    s.scene.emplace(std::move(*instance));
    s.camera.setLens(description->camera.lens);
    s.camera.setPose(description->camera.position, core::degreesToRadians(description->camera.yawDeg),
                     core::degreesToRadians(description->camera.pitchDeg));

    // The UI actions as retroverse.exe reads them: the bindings (the user's over the defaults), the script over them.
    const input::ActionMap bindings = app::loadBindings(paths);
    s.input.setMap(bindings);
    if (const auto& scriptPath = context.options().inputScript) {
        auto script = input::InputScript::loadFromFile(*scriptPath);
        if (!script) {
            return propagate(script.error());
        }
        spdlog::info("Input script {}: {} step(s) up to frame {}", core::pathToGenericUtf8(*scriptPath),
                     script->steps().size(), script->lastFrame());
        s.script.emplace(std::move(*script));
    }

    auto ui = ui::UiSystem::create(context.window(), ui::UiSystemConfig{paths.assetRoot()});
    if (!ui) {
        return propagate(ui.error());
    }
    s.ui = std::move(*ui);
    auto view = ui::UiView::create(*s.ui, "window", context.framebufferSize(), context.window().displayScale());
    if (!view) {
        return propagate(view.error());
    }
    s.view = std::move(*view);
    s.view->setCommandKeys(ui::commandKeys(bindings));
    auto screen = ui::DirectoryScreen::create(*s.view);
    if (!screen) {
        return propagate(screen.error());
    }
    s.screen = std::move(*screen);
    State* raw = state.get();
    s.graphics->setOverlay([raw](gl::GLDevice& device) { raw->view->render(device); },
                           [raw](gl::GLDevice& device, const gl::Framebuffer& target) {
                               raw->view->renderTo(device, target);
                           });

    s.model.setBuilding(fixture::building());
    s.model.setGames(fixture::games());
    s.model.open(ui::DirectoryTab::Floors);
    spdlog::info("UI probe: the directory of {} floor(s) and {} game(s), opened on the floors",
                 s.model.building().floors.size(), s.model.games().size());
    m_state = std::move(state);
    return ok();
}

void UiProbeApp::onEvent(app::AppContext& /*context*/, const SDL_Event& event) {
    if (m_state && m_state->screen->visible()) {
        static_cast<void>(m_state->screen->handlePointer(event));
    }
}

void UiProbeApp::onUpdate(app::AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    const std::uint64_t frame = context.frameIndex();
    s.simulatedSeconds += deltaSeconds;
    s.ui->setTime(s.simulatedSeconds);

    // No device is read: the script stands for the user.
    input::ActionState actions = s.input.resolve(input::RawInputFrame{});
    if (s.script) {
        s.script->apply(frame, actions, s.input.settings().pressThreshold);
    }
    ui::UiCommandList commands;
    s.nav.update(actions, deltaSeconds, commands);
    for (const ui::UiCommand command : commands.commands()) {
        s.record(frame, s.model.apply(command));
    }
    if (const std::optional<ui::DirectoryClick> click = s.screen->takeClicked()) {
        s.record(frame, s.model.click(*click));
    }
    fixture::fillCard(s.model, s.cardServed);
    if (s.model.isOpen()) {
        s.screen->show(s.model);
    } else {
        s.screen->hide();
    }
    s.view->resize(context.framebufferSize(), context.window().displayScale());
    s.view->update();
    s.screen->afterUpdate();

    if (const fixture::Checkpoint* checkpoint = checkpointAt(frame)) {
        const std::string modelFocus = s.model.isOpen() ? s.model.focusedElement() : std::string();
        s.checkpoints.push_back({
            {"frame", frame},
            {"open", s.model.isOpen()},
            {"tab", ui::toString(s.model.tab())},
            {"view", ui::toString(s.model.view())},
            {"modelFocus", modelFocus},
            {"screenFocus", s.screen->focusedElement()},
            {"visible", s.screen->visible()},
            {"cardSqueezed", s.screen->cardSqueezed()},
            {"panelBox", boxJson(s.view->elementBox(ui::directory::kPanelId))},
            {"listBox", boxJson(s.view->elementBox(ui::directory::kListId))},
            {"cardBox", boxJson(s.view->elementBox(ui::directory::kCardId))},
            {"focusBox", boxJson(modelFocus.empty() ? std::nullopt : s.view->elementBox(modelFocus))},
        });
        spdlog::info("Frame {}: checkpoint, directory {} on {} / {}, focus {}", frame,
                     s.model.isOpen() ? "open" : "closed", ui::toString(s.model.tab()), ui::toString(s.model.view()),
                     s.screen->focusedElement());
    }
}

void UiProbeApp::onRender(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    const std::uint64_t frame = context.frameIndex();
    gl::RenderView view = app::makeRenderView(*s.scene, s.camera);
    view.timeSeconds = static_cast<float>(s.simulatedSeconds);

    // A checkpoint's screenshot: SceneGraphics writes it under <user data>/screenshots (the scene, then the UI through
    // the screenshot overlay), from where it moves to the case's folder.
    const fixture::Checkpoint* checkpoint = checkpointAt(frame);
    const bool capture = checkpoint != nullptr && !checkpoint->shot.empty() && m_options.shots.has_value();
    const std::filesystem::path screenshots = context.paths().screenshotsDir();
    const std::vector<std::filesystem::path> before = capture ? pngFiles(screenshots) : std::vector<std::filesystem::path>{};
    s.graphics->renderFrame(context, view, capture);
    if (!capture) {
        return;
    }
    std::vector<std::filesystem::path> written;
    for (const std::filesystem::path& file : pngFiles(screenshots)) {
        if (!std::binary_search(before.begin(), before.end(), file)) {
            written.push_back(file);
        }
    }
    const std::filesystem::path target = *m_options.shots / core::pathFromUtf8(checkpoint->shot);
    const bool moved = written.size() == 1 && moveFile(written.front(), target);
    if (moved) {
        spdlog::info("Frame {}: screenshot {}", frame, core::pathToGenericUtf8(target));
    } else {
        spdlog::error("Frame {}: screenshot {} not written", frame, core::pathToGenericUtf8(target));
        s.shotMissing = true;
    }
    s.shots.push_back({{"frame", frame}, {"path", core::pathToGenericUtf8(target)}, {"written", moved}});
}

void UiProbeApp::onShutdown(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    const ui::UiLogCounts log = s.ui->logCounts();
    if (m_options.report) {
        const nlohmann::json report{
            {"screen", "directory"},
            {"frames", context.frameIndex()},
            {"checkpoints", s.checkpoints},
            {"commands", s.commands},
            {"shots", s.shots},
            {"rmlErrors", log.errors},
            {"rmlWarnings", log.warnings},
            {"binds", s.screen->binds()},
        };
        if (auto written = core::writeTextFileAtomic(*m_options.report, report.dump(2) + "\n"); !written) {
            context.failRun(written.error());
        }
    }
    if (context.options().frameLimit && s.shotMissing) {
        context.failRun(Error{ErrorCode::IoError, "a checkpoint's screenshot was not written", "ui probe"});
    }
    s.screen.reset();
    s.view.reset();
    s.ui.reset();
    s.graphics->finishRun(context);
    m_state.reset();
}

} // namespace rv::probe
