#include "spike_app.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <RmlUi/Core/Core.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/command_line.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_collector.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/quick_catalog_screen.hpp>
#include <rv/ui/ui_command.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

#include "mock_catalog.hpp"
#include "scenario.hpp"
#include "spike_report.hpp"
#include "virtual_pad.hpp"

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace rv::spike_ui {
namespace {

using Clock = std::chrono::steady_clock;

/// Developer builds (assets read from the source tree) reload the document when it is saved.
#if defined(RV_SPIKE_WATCH_DOCUMENTS)
constexpr bool kWatchDocuments = true;
#else
constexpr bool kWatchDocuments = false;
#endif

/// Samples kept when the run has no frame limit (--interactive without --frames).
constexpr std::size_t kInteractiveSampleCapacity = 100000;

[[nodiscard]] double millisecondsSince(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// --- Heap allocations of the UI pass (Debug builds: the debug CRT's allocation hook sees every module that uses the
// debug CRT, rmlui.dll included). Counted on the main thread only, between begin() and end(). ---------------------
#if defined(_DEBUG)
thread_local bool t_countingAllocations = false;
std::int64_t g_allocations = 0;

int countAllocation(int allocationType, void*, size_t, int, long, const unsigned char*, int) {
    if (t_countingAllocations && (allocationType == _HOOK_ALLOC || allocationType == _HOOK_REALLOC)) {
        ++g_allocations;
    }
    return 1; // let the allocation proceed
}

class AllocationCounter {
public:
    static constexpr bool kAvailable = true;
    AllocationCounter() noexcept : m_previous(_CrtSetAllocHook(&countAllocation)) {}
    ~AllocationCounter() { _CrtSetAllocHook(m_previous); }
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    static void begin() noexcept { t_countingAllocations = true; }
    static void end() noexcept { t_countingAllocations = false; }
    static std::int64_t take() noexcept { return std::exchange(g_allocations, 0); }

private:
    _CRT_ALLOC_HOOK m_previous;
};
#else
class AllocationCounter {
public:
    static constexpr bool kAvailable = false;
    static void begin() noexcept {}
    static void end() noexcept {}
    static std::int64_t take() noexcept { return -1; }
};
#endif

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

/// Moves `from` to `to` (creating its directory), copying when a rename across volumes fails.
bool moveFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (to.has_parent_path()) {
        std::filesystem::create_directories(to.parent_path(), error);
    }
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

[[nodiscard]] bool isCheck(StepKind kind) noexcept {
    switch (kind) {
    case StepKind::Press:
    case StepKind::Release:
    case StepKind::Stick:
    case StepKind::Resize:
    case StepKind::Text:
    case StepKind::Key:
        return false;
    default:
        return true;
    }
}

[[nodiscard]] bool near(const glm::vec4& a, const glm::vec4& b, float tolerance) noexcept {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
           std::fabs(a.z - b.z) <= tolerance && std::fabs(a.w - b.w) <= tolerance;
}

[[nodiscard]] std::string joined(const std::vector<std::string>& values) {
    std::string text;
    for (const std::string& value : values) {
        text += text.empty() ? value : "," + value;
    }
    return text;
}

/// The elements whose boxes a layout records.
constexpr std::array<std::string_view, 7> kLayoutElements{"catalog", "card",   "hint-bar", "game-list",
                                                          "search",  "chips", "actions"};
/// The panels that must stay inside the view at every size.
constexpr std::array<std::string_view, 3> kPanels{"catalog", "card", "hint-bar"};

} // namespace

struct SpikeApp::State {
    // Declared in the order they are created; destroyed in reverse: the screen before its model and view, the view
    // before the UI system, the UI before the scene's GPU resources.
    std::unique_ptr<app::SceneGraphics> graphics;
    std::optional<app::SceneInstance> scene;
    scene::Camera camera;
    std::unique_ptr<ui::UiSystem> ui;
    std::unique_ptr<ui::UiView> view;
    std::unique_ptr<ui::QuickCatalogModel> model;
    std::unique_ptr<ui::QuickCatalogScreen> screen;

    platform::SdlInputCollector collector;
    ui::NavInput nav;
    ui::UiCommandList commands;
    std::optional<VirtualPad> pad;
    bool padFailureLogged = false;
    Scenario scenario;
    std::size_t nextInputStep = 0;
    std::size_t nextCheckStep = 0;
    std::size_t nextScreenshot = 0;
#if defined(_DEBUG)
    std::optional<AllocationCounter> allocationCounter;
#endif

    std::uint32_t frame = 0;
    /// The window's drawable and its display's content scale this frame.
    glm::ivec2 drawable{0};
    float displayScale = 1.0f;
    double simulatedSeconds = 0.0;
    double inputMs = 0.0;
    std::int64_t updateAllocations = -1;
    /// --fps-cap: when the next frame may start (SDL_GetTicksNS), 0 before the first.
    Uint64 nextFrameNs = 0;
    std::optional<double> lastGpuMs;
    std::size_t focusLogged = 0;
    std::size_t actionsLogged = 0;
    std::vector<std::string> launched;
    std::vector<std::string> mediaAsked;
    bool infoShown = false;
    ReportData report;

    // --- The screen's events: recorded now, logged after the measured part of the frame --------------------------

    ui::QuickCatalogEvents events() {
        ui::QuickCatalogEvents handlers;
        handlers.focusChanged = [this](const ui::FocusTarget& target) {
            report.focusChanges.push_back(FocusRecord{frame, target.region, target.element});
        };
        handlers.launch = [this](const ui::CatalogEntry& entry) {
            launched.push_back(entry.id);
            report.actions.push_back(ActionRecord{frame, "launch", entry.id});
            screen->setStatus(fmt::format("Avvio di {}: nel prototipo nessun emulatore parte.", entry.title));
        };
        handlers.media = [this](const ui::CatalogEntry& entry) {
            mediaAsked.push_back(entry.id);
            report.actions.push_back(ActionRecord{frame, "media", entry.id});
            screen->setStatus(fmt::format("Media di {}: non disponibili nel prototipo.", entry.title));
        };
        handlers.info = [this](const ui::CatalogEntry& entry, bool shown) {
            infoShown = shown;
            report.actions.push_back(ActionRecord{frame, shown ? "info-shown" : "info-hidden", entry.id});
        };
        handlers.close = [this] {
            report.actions.push_back(ActionRecord{frame, "close", {}});
            screen->hide();
        };
        return handlers;
    }

    /// Logs what the screen recorded during the frame (outside the measured part: logging writes to a file).
    void logRecords() {
        for (; focusLogged < report.focusChanges.size(); ++focusLogged) {
            const FocusRecord& record = report.focusChanges[focusLogged];
            spdlog::info("Frame {}: focus on {} ({})", record.frame, record.element, record.region);
        }
        for (; actionsLogged < report.actions.size(); ++actionsLogged) {
            const ActionRecord& record = report.actions[actionsLogged];
            spdlog::info("Frame {}: {} {}", record.frame, record.action, record.game);
        }
    }

    // --- The scenario ------------------------------------------------------------------------------------------------

    void runInputSteps(app::AppContext& context) {
        const std::vector<Step>& steps = scenario.steps;
        for (; nextInputStep < steps.size() && steps[nextInputStep].frame <= frame; ++nextInputStep) {
            const Step& step = steps[nextInputStep];
            if (isCheck(step.kind) || !pad) {
                continue;
            }
            const SDL_WindowID window = SDL_GetWindowID(context.window().handle());
            bool done = true;
            switch (step.kind) {
            case StepKind::Press:
                done = pad->button(step.button, true);
                break;
            case StepKind::Release:
                done = pad->button(step.button, false);
                break;
            case StepKind::Stick:
                done = pad->axis(SDL_GAMEPAD_AXIS_LEFTX, step.value.x) &&
                       pad->axis(SDL_GAMEPAD_AXIS_LEFTY, step.value.y);
                break;
            case StepKind::Resize: {
                const int width = static_cast<int>(step.value.x);
                const int height = static_cast<int>(step.value.y);
                spdlog::info("Frame {}: resizing the window to {}x{}", frame, width, height);
                done = SDL_SetWindowSize(context.window().handle(), width, height);
                break;
            }
            case StepKind::Text: {
                // Delivered with the next frame's events; the step's text outlives it (the scenario lives as long as
                // the run).
                SDL_Event event{};
                event.type = SDL_EVENT_TEXT_INPUT;
                event.text.windowID = window;
                event.text.text = step.text.c_str();
                done = SDL_PushEvent(&event);
                break;
            }
            case StepKind::Key:
                for (std::uint32_t time = 0; time < step.count && done; ++time) {
                    for (const bool down : {true, false}) {
                        SDL_Event event{};
                        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                        event.key.windowID = window;
                        event.key.scancode = step.scancode;
                        event.key.key = step.keycode;
                        event.key.down = down;
                        done = done && SDL_PushEvent(&event);
                    }
                }
                break;
            default:
                break;
            }
            if (!done && !padFailureLogged) {
                spdlog::error("Frame {}: scenario step '{}' failed: {}", frame, toString(step.kind), SDL_GetError());
                padFailureLogged = true;
            }
        }
    }

    void check(std::string name, bool passed, std::string detail) {
        if (passed) {
            spdlog::info("Frame {}: check passed: {} ({})", frame, name, detail);
        } else {
            spdlog::error("Frame {}: check FAILED: {} ({})", frame, name, detail);
        }
        report.checks.push_back(CheckRecord{frame, std::move(name), passed, std::move(detail), false});
    }

    void skip(std::string name, std::string reason) {
        spdlog::warn("Frame {}: check skipped: {} ({})", frame, name, reason);
        report.checks.push_back(CheckRecord{frame, std::move(name), false, std::move(reason), true});
    }

    [[nodiscard]] const LayoutRecord* layoutLabelled(std::string_view label) const {
        for (const LayoutRecord& layout : report.layouts) {
            if (layout.label == label) {
                return &layout;
            }
        }
        return nullptr;
    }

    void recordLayout(const std::string& label) {
        LayoutRecord layout;
        layout.frame = frame;
        layout.label = label;
        layout.size = view->size();
        layout.dpRatio = view->dpRatio();
        layout.focused = screen->focus() ? screen->focus()->element : std::string();
        for (const std::string_view id : kLayoutElements) {
            if (const std::optional<glm::vec4> box = view->elementBox(id)) {
                layout.boxes.emplace_back(std::string(id), *box);
            }
        }
        const auto boxOf = [&layout](std::string_view id) -> std::optional<glm::vec4> {
            for (const auto& [name, box] : layout.boxes) {
                if (name == id) {
                    return box;
                }
            }
            return std::nullopt;
        };
        if (!layout.focused.empty()) {
            if (const std::optional<glm::vec4> box = view->elementBox(layout.focused)) {
                layout.boxes.emplace_back("focused:" + layout.focused, *box);
            }
        }
        // Every panel inside the view.
        constexpr float kTolerance = 1.0f;
        bool inside = true;
        std::string outside;
        for (const std::string_view id : kPanels) {
            const std::optional<glm::vec4> box = boxOf(id);
            if (!box || box->x < -kTolerance || box->y < -kTolerance ||
                box->x + box->z > static_cast<float>(layout.size.x) + kTolerance ||
                box->y + box->w > static_cast<float>(layout.size.y) + kTolerance) {
                inside = false;
                outside += outside.empty() ? std::string(id) : ", " + std::string(id);
            }
        }
        check("layout " + label + ": panels inside the view", inside,
              inside ? fmt::format("{}x{} pixels, dp ratio {:.2f}", layout.size.x, layout.size.y, layout.dpRatio)
                     : "outside: " + outside);
        // The focused game visible in the list (scrolled into view).
        if (layout.focused.starts_with(ui::quick_catalog::kGamePrefix)) {
            const std::optional<glm::vec4> list = boxOf("game-list");
            const std::optional<glm::vec4> focused = view->elementBox(layout.focused);
            const bool visible = list && focused && focused->y >= list->y - kTolerance &&
                                 focused->y + focused->w <= list->y + list->w + kTolerance;
            check("layout " + label + ": focused game visible in the list", visible, layout.focused);
        }
        // The context follows the window's drawable, at the display's density (no scale drift across resizes).
        check("layout " + label + ": the view follows the drawable",
              layout.size == drawable && layout.dpRatio == displayScale * view->textScale(),
              fmt::format("view {}x{} at dp ratio {:.2f}, drawable {}x{} at display scale {:.2f}", layout.size.x,
                          layout.size.y, layout.dpRatio, drawable.x, drawable.y, displayScale));
        report.layouts.push_back(std::move(layout));
    }

    void compareLayouts(const std::string& label, const std::string& reference) {
        const LayoutRecord* now = layoutLabelled(label);
        const LayoutRecord* before = layoutLabelled(reference);
        if (now == nullptr || before == nullptr) {
            check("same-layout " + label + " = " + reference, false, "a layout was not recorded");
            return;
        }
        if (now->size != before->size || now->dpRatio != before->dpRatio) {
            // The window could not get its first drawable back, or the display's scale changed (another program changed
            // the display mode during the run): the layouts cannot be compared.
            skip("same-layout " + label + " = " + reference,
                 fmt::format("the view is {}x{} at dp ratio {:.2f}, it was {}x{} at {:.2f}", now->size.x, now->size.y,
                             now->dpRatio, before->size.x, before->size.y, before->dpRatio));
            return;
        }
        std::string differences;
        for (const auto& [id, box] : before->boxes) {
            const auto match = std::find_if(now->boxes.begin(), now->boxes.end(),
                                            [&id](const auto& entry) { return entry.first == id; });
            if (match == now->boxes.end() || !near(match->second, box, 0.5f)) {
                differences += differences.empty() ? id : ", " + id;
            }
        }
        check("same-layout " + label + " = " + reference, differences.empty(),
              differences.empty() ? fmt::format("{} boxes equal at {}x{}", before->boxes.size(), now->size.x,
                                                now->size.y)
                                  : "differ: " + differences);
    }

    void runChecks() {
        const std::vector<Step>& steps = scenario.steps;
        for (; nextCheckStep < steps.size() && steps[nextCheckStep].frame <= frame; ++nextCheckStep) {
            const Step& step = steps[nextCheckStep];
            if (!isCheck(step.kind)) {
                continue;
            }
            switch (step.kind) {
            case StepKind::ExpectFocus: {
                const std::string actual = screen->focus() ? screen->focus()->element : std::string("(none)");
                check("expect-focus " + step.text, actual == step.text, "focus on " + actual);
                break;
            }
            case StepKind::ExpectSteady: {
                const auto changes = static_cast<std::uint32_t>(
                    std::count_if(report.focusChanges.begin(), report.focusChanges.end(),
                                  [&step](const FocusRecord& record) { return record.frame >= step.since; }));
                check(fmt::format("expect-steady since frame {}", step.since), changes == step.count,
                      fmt::format("{} focus change(s), {} expected", changes, step.count));
                break;
            }
            case StepKind::ExpectLaunched:
                check("expect-launched " + step.text, joined(launched) == step.text, "launched: " + joined(launched));
                break;
            case StepKind::ExpectMedia:
                check("expect-media " + step.text, joined(mediaAsked) == step.text, "media: " + joined(mediaAsked));
                break;
            case StepKind::ExpectInfo:
                check("expect-info " + step.text, (infoShown ? "shown" : "hidden") == step.text,
                      infoShown ? "details shown" : "details hidden");
                break;
            case StepKind::ExpectHistory: {
                const std::size_t depth = screen->history().depth();
                check(fmt::format("expect-history {}", step.count), depth == step.count,
                      fmt::format("{} entr{}", depth, depth == 1 ? "y" : "ies"));
                break;
            }
            case StepKind::ExpectVisible: {
                const std::size_t visible = model->visible().size();
                check(fmt::format("expect-visible {}", step.count), visible == step.count,
                      fmt::format("{} game(s) listed", visible));
                break;
            }
            case StepKind::Layout:
                recordLayout(step.label);
                break;
            case StepKind::SameLayout:
                compareLayouts(step.label, step.text);
                break;
            default:
                break;
            }
        }
    }

    /// With the catalog closed (Back at its root): Y or A opens it again, Back quits.
    void whileClosed(app::AppContext& context) {
        for (const ui::UiCommand command : commands.commands()) {
            if (command == ui::UiCommand::Media || command == ui::UiCommand::Activate) {
                screen->show();
                nav.reset();
                report.actions.push_back(ActionRecord{frame, "open", {}});
                return;
            }
            if (command == ui::UiCommand::Back) {
                spdlog::info("Back with the catalog closed: quitting");
                context.requestQuit();
                return;
            }
        }
    }

    /// The screenshot asked for this frame, if any.
    [[nodiscard]] const ScreenshotRequest* captureFor(const std::vector<ScreenshotRequest>& requests) {
        while (nextScreenshot < requests.size() && requests[nextScreenshot].frame < frame) {
            ++nextScreenshot; // a frame the run skipped (never: frames are consecutive)
        }
        return nextScreenshot < requests.size() && requests[nextScreenshot].frame == frame ? &requests[nextScreenshot]
                                                                                            : nullptr;
    }

    /// --fps-cap: waits until the frame's slot starts (precisely: SDL_DelayPrecise), so the GPU never queues frames
    /// ahead; a frame later than one period restarts the pacing from now.
    void pace(int framesPerSecond) {
        if (framesPerSecond <= 0) {
            return;
        }
        const Uint64 period = 1000000000ull / static_cast<Uint64>(framesPerSecond);
        const Uint64 now = SDL_GetTicksNS();
        if (nextFrameNs == 0 || now > nextFrameNs + period) {
            nextFrameNs = now + period;
            return;
        }
        if (nextFrameNs > now) {
            SDL_DelayPrecise(nextFrameNs - now);
        }
        nextFrameNs += period;
    }

    void recordSample(app::AppContext& context) {
        FrameSample sample;
        sample.frame = frame;
        sample.inputMs = inputMs;
        sample.updateMs = view->times().updateMs;
        sample.renderMs = view->times().renderMs;
        sample.beginFrameMs = view->times().beginFrameMs;
        sample.contextRenderMs = view->times().contextRenderMs;
        sample.endFrameMs = view->times().endFrameMs;
        sample.drawCalls = view->times().drawCalls;
        if (const std::optional<double> gpu = view->times().gpuMs; gpu && gpu != lastGpuMs) {
            sample.gpuMs = *gpu; // a new result (results of two spans are never bit-identical in practice)
            lastGpuMs = gpu;
        }
        sample.frameMs = context.clock().deltaSeconds() * 1000.0;
        sample.updateAllocations = updateAllocations;
        sample.renderAllocations = AllocationCounter::take();
        if (report.samples.size() < report.samples.capacity()) {
            report.samples.push_back(sample); // reserved at start: no allocation per frame
        }
    }
};

SpikeApp::SpikeApp(SpikeOptions options) : m_options(std::move(options)) {}

SpikeApp::~SpikeApp() = default;

Status SpikeApp::onInit(app::AppContext& context) {
    auto state = std::make_unique<State>();
    State& s = *state;
    const core::Paths& paths = context.paths();

    // The 3D background: the test room through the unchanged rv_app pieces.
    auto graphics = app::SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    s.graphics = std::move(*graphics);
    const std::filesystem::path scenePath = app::resolveScenePath(m_options.run.scene, paths.assetRoot());
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

    // The product UI.
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
    s.view->setTextScale(m_options.textScale);
    s.model = std::make_unique<ui::QuickCatalogModel>(mockCatalog());
    auto screen = ui::QuickCatalogScreen::create(*s.view, *s.model, s.events(), kWatchDocuments);
    if (!screen) {
        return propagate(screen.error());
    }
    s.screen = std::move(*screen);
    s.screen->setHighContrast(m_options.highContrast);
    s.graphics->setOverlay(
        [this](gl::GLDevice& device) {
            AllocationCounter::begin();
            m_state->view->render(device);
            AllocationCounter::end();
        },
        [this](gl::GLDevice& device, const gl::Framebuffer& target) { m_state->view->renderTo(device, target); });

    // Input: real pads, and the scripted virtual pad.
    s.collector.gamepads().openConnected();
    const std::optional<std::uint32_t> frameLimit = m_options.run.frameLimit;
    if (m_options.scenario) {
        // The scripted pad must reach a window that never has the focus (hidden runs): background joystick events are
        // allowed for this run only (SdlPlatform disables them so that an emulator's pad input never reaches us).
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        auto pad = VirtualPad::attach("Retroverse Spike Pad");
        if (!pad) {
            return propagate(pad.error());
        }
        s.pad.emplace(std::move(*pad));
        s.scenario = defaultScenario();
        if (!frameLimit || *frameLimit < s.scenario.frames) {
            spdlog::warn("The scenario needs {} frames; with {} the checks after the last frame are not run",
                         s.scenario.frames, frameLimit ? std::to_string(*frameLimit) : std::string("no limit"));
        }
    }
    s.report.samples.reserve(frameLimit ? *frameLimit : kInteractiveSampleCapacity);
    s.report.focusChanges.reserve(frameLimit ? *frameLimit / 2 + 64 : 4096);
    s.report.actions.reserve(256);
#if defined(_DEBUG)
    s.allocationCounter.emplace();
#endif

    ReportData& report = s.report;
    report.build = std::string(core::kBuildType);
    report.rmluiVersion = Rml::GetVersion();
    report.glRenderer = context.device().caps().renderer;
    report.glVersion = context.device().caps().version;
    report.initialSize = context.framebufferSize();
    report.displayScale = context.window().displayScale();
    report.textScale = m_options.textScale;
    report.highContrast = m_options.highContrast;
    report.scenario = m_options.scenario;
    report.fixedDeltaSeconds = m_options.run.fixedDeltaSeconds;
    report.vsync = m_options.run.vsync;
    report.fpsCap = m_options.fpsCap;
    report.fonts = s.ui->loadedFontFaces();
    report.phases = s.scenario.phases;
    spdlog::info("spike_ui: {}x{} pixels, display scale {:.2f}, {} scripted step(s), {} screenshot(s) asked",
                 report.initialSize.x, report.initialSize.y, report.displayScale, s.scenario.steps.size(),
                 m_options.screenshots.size());
    s.logRecords(); // the first focus
    m_state = std::move(state);
    return ok();
}

void SpikeApp::onEvent(app::AppContext& /*context*/, const SDL_Event& event) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    // The UI first (mouse, text, keys that are not commands); what it does not use goes to the input collector, which
    // turns pads and command keys into the raw frame NavInput reads.
    if (s.view->handleEvent(event)) {
        return;
    }
    s.collector.handleEvent(event);
}

void SpikeApp::onUpdate(app::AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    // The frame starts when its slot does (--fps-cap): the previous frame's swap has submitted all its commands, so
    // no GPU span measured this frame waits on a later flush.
    s.pace(m_options.fpsCap);
    s.frame = static_cast<std::uint32_t>(context.frameIndex());
    s.simulatedSeconds += deltaSeconds;
    s.ui->setTime(s.simulatedSeconds);
    s.runInputSteps(context);

    const input::RawInputFrame raw = s.collector.endFrame();
    s.commands.clear();
    s.nav.update(raw, deltaSeconds, s.commands);
    s.report.commandsDropped += s.commands.dropped();

    // The measured UI pass, CPU side: the commands, then the context's update.
    AllocationCounter::begin();
    const Clock::time_point inputStart = Clock::now();
    if (s.screen->visible()) {
        s.screen->apply(s.commands);
    } else {
        s.whileClosed(context);
    }
    s.inputMs = millisecondsSince(inputStart);
    s.drawable = context.framebufferSize();
    s.displayScale = context.window().displayScale();
    s.view->resize(s.drawable, s.displayScale);
    s.view->update();
    s.screen->afterUpdate();
    AllocationCounter::end();
    s.updateAllocations = AllocationCounter::take();

    s.logRecords();
    s.runChecks();
    if constexpr (kWatchDocuments) {
        s.screen->reloadIfChanged();
    }
    s.graphics->pollShaderReload();
}

void SpikeApp::onRender(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    gl::RenderView view = app::makeRenderView(*s.scene, s.camera);
    view.post.antialiasing = true;
    view.timeSeconds = static_cast<float>(s.simulatedSeconds);

    // A screenshot of this frame: SceneGraphics renders it (scene, then the UI through the screenshot overlay) and
    // writes it under <user data>/screenshots, from where it moves to the path asked for.
    const ScreenshotRequest* capture = s.captureFor(m_options.screenshots);
    const std::filesystem::path screenshots = context.paths().screenshotsDir();
    const std::vector<std::filesystem::path> before = capture != nullptr ? pngFiles(screenshots)
                                                                         : std::vector<std::filesystem::path>{};
    s.graphics->renderFrame(context, view, capture != nullptr);
    if (capture != nullptr) {
        std::vector<std::filesystem::path> written;
        for (const std::filesystem::path& file : pngFiles(screenshots)) {
            if (!std::binary_search(before.begin(), before.end(), file)) {
                written.push_back(file);
            }
        }
        const bool moved = written.size() == 1 && moveFile(written.front(), capture->path);
        if (moved) {
            spdlog::info("Frame {}: screenshot {}", s.frame, core::pathToGenericUtf8(capture->path));
        } else {
            spdlog::error("Frame {}: screenshot {} not written", s.frame, core::pathToGenericUtf8(capture->path));
        }
        s.report.screenshots.push_back(ScreenshotRecord{s.frame, core::pathToGenericUtf8(capture->path), moved});
        ++s.nextScreenshot;
    }
    s.recordSample(context);
}

void SpikeApp::onShutdown(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    s.graphics->finishRun(context);
    ReportData& report = s.report;
    report.framesRendered = context.frameIndex();
    const ui::UiLogCounts log = s.ui->logCounts();
    report.rmlErrors = log.errors;
    report.rmlWarnings = log.warnings;
    for (const std::string& line : timingSummaryLines(report)) {
        spdlog::info("{}", line);
    }
    const auto failed = static_cast<std::size_t>(std::count_if(
        report.checks.begin(), report.checks.end(), [](const CheckRecord& c) { return !c.passed && !c.skipped; }));
    const auto skipped = static_cast<std::size_t>(
        std::count_if(report.checks.begin(), report.checks.end(), [](const CheckRecord& c) { return c.skipped; }));
    const bool screenshotsWritten =
        std::all_of(report.screenshots.begin(), report.screenshots.end(), [](const ScreenshotRecord& r) {
            return r.written;
        });
    spdlog::info("spike_ui: {} check(s), {} failed, {} skipped; {} focus change(s); RmlUi log: {} error(s), {} "
                 "warning(s)",
                 report.checks.size(), failed, skipped, report.focusChanges.size(), log.errors, log.warnings);
    if (m_options.report) {
        if (auto written = writeReport(*m_options.report, report); written) {
            spdlog::info("Report written: {}", core::pathToGenericUtf8(*m_options.report));
        } else {
            context.failRun(Error{ErrorCode::IoError, "the report could not be written: " + written.error().message,
                                  core::pathToUtf8(*m_options.report)});
        }
    }
    if (failed > 0) {
        context.failRun(Error{ErrorCode::ValidationFailed, fmt::format("{} scenario check(s) failed", failed), {}});
    } else if (log.errors > 0 || log.warnings > 0) {
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("RmlUi logged {} error(s) and {} warning(s)", log.errors, log.warnings), {}});
    } else if (!screenshotsWritten) {
        context.failRun(Error{ErrorCode::IoError, "a --screenshot-at was not written", {}});
    }
    // UI first (screen, model, view, system), then the scene's GPU resources, while the context is current.
    s.screen.reset();
    s.model.reset();
    s.view.reset();
    s.ui.reset();
    s.pad.reset();
    s.scene.reset();
    s.graphics.reset();
    m_state.reset();
}

} // namespace rv::spike_ui
