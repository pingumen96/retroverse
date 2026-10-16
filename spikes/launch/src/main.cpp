// spike_launch: I1 spike (f), the return from an emulator (docs/spikes/i1-launch.md, ADR-0006).
//
// A Retroverse-like borderless fullscreen window renders an animated scene at the display's refresh rate. Each launch
// hands the window over (rv::platform::WindowHandover: minimized, or kept without rendering), starts the target through
// rv::launcher::LaunchSession and rv::platform::Win32ProcessRunner, waits without rendering and without input, then
// takes the window back and measures, from the system's exit time of the emulator: (a) the window restored, (b) the
// foreground regained, (c) borderless fullscreen re-applied, (d) the first frame submitted (SwapBuffers returned: with
// vsync the frame reaches the screen later, which (e) includes), (e) frame pacing back at 60 FPS, (f) a press of an SDL
// virtual gamepad, injected after the return, reaching the input collector. The JSON report holds every launch, the
// remedies the foreground needed, the machine's load and the build that measured.
//
// Exit codes: 0 every launch met the criteria, 1 some did not, 2 bad options or initialization failure, 3 runtime
// failure. The harness ends a target only when it outlives --timeout-s (recorded), and only processes still known to
// run in its tree; the product never ends one.

#include <SDL3/SDL_main.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/input/codes.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/launcher/exit_status.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_collector.hpp>
#include <rv/platform/sdl_platform.hpp>
#include <rv/platform/win32_process_runner.hpp>
#include <rv/platform/window_handover.hpp>
#include <rv/render/gl_device.hpp>
#include <spike_launch/measurements.hpp>
#include <spike_launch/targets.hpp>

#include "harness.hpp"
#include "spike_scene.hpp"

// Hybrid-GPU laptops: run on the discrete GPU, like retroverse.exe.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

using namespace std::chrono_literals;
namespace launcher = rv::launcher;
namespace platform = rv::platform;
namespace spike = rv::spikes::launch;
using spike::Clock;

constexpr int kExitOk = 0;
constexpr int kExitCriteriaMissed = 1;
constexpr int kExitInitialization = 2;
constexpr int kExitRuntime = 3;

/// The exit code the harness gives a target it has to end (recorded in the report).
constexpr std::uint32_t kHarnessExitCode = 0xDEAD;

const glm::vec4 kStatusWarmUp{0.45f, 0.45f, 0.5f, 1.0f};
const glm::vec4 kStatusReturning{0.9f, 0.55f, 0.1f, 1.0f};
const glm::vec4 kStatusBack{0.2f, 0.75f, 0.3f, 1.0f};

struct SpikeOptions {
    spike::TargetKind target = spike::TargetKind::Fake;
    launcher::WaitStrategy strategy = launcher::WaitStrategy::DirectProcess;
    platform::HandoverPolicy policy = platform::HandoverPolicy::Minimize;
    int repeat = 1;
    std::filesystem::path dataDir;
    std::filesystem::path report;
    spike::FakeScenario fake;
    std::filesystem::path retroarch;
    std::filesystem::path core;
    std::filesystem::path rom;
    int maxFrames = 240;
    std::chrono::milliseconds targetRun{2000};
    launcher::SessionState expected = launcher::SessionState::Returned;
    std::chrono::milliseconds settle{1500};
    std::chrono::milliseconds warmUp{1500};
    std::chrono::seconds timeout{60};
    std::chrono::milliseconds windowTimeout{30000};
    bool windowed = false;
    /// What the build was made from (measure.ps1 passes the git commit and the state of the working tree).
    std::string sourceRevision;
    std::vector<std::string> extraArguments;
    std::vector<std::string> commandLine;
};

nlohmann::json optionalMs(Clock::time_point origin, const std::optional<Clock::time_point>& point) {
    if (!point) {
        return nullptr;
    }
    return std::round(spike::millisecondsBetween(origin, *point) * 100.0) / 100.0;
}

std::string utf8(const std::filesystem::path& path) {
    try {
        return rv::core::pathToUtf8(path);
    } catch (...) {
        return "?";
    }
}

nlohmann::json describeWindowState(const platform::WindowState& state) {
    const auto rect = [](const platform::PixelRect& r) { return nlohmann::json{r.left, r.top, r.right, r.bottom}; };
    return nlohmann::json{{"fullscreen", state.fullscreen},
                          {"desktopFullscreen", state.desktopFullscreen},
                          {"minimized", state.minimized},
                          {"foreground", state.foreground},
                          {"display", state.display},
                          {"displayMode",
                           fmt::format("{}x{} {:.2f} Hz", state.displayWidth, state.displayHeight,
                                       state.displayRefreshRate)},
                          {"windowRect", rect(state.windowRect)},
                          {"monitorRect", rect(state.monitorRect)},
                          {"coversMonitor", platform::coversMonitor(state)}};
}

/// When the window was the foreground window again after losing it to the emulator: the earlier of the hook's first
/// confirmed record of it (delivered while the window was the foreground window) and the moment the hand-over saw it
/// there. Both are upper bounds: an event is delivered after it happened (its own time stamp is too coarse, 10-30 ms),
/// and polling sees it once per frame. Unconfirmed events do not count: a refused raise reports the window too.
std::optional<Clock::time_point> foregroundBack(const platform::HandoverReport& report) {
    std::optional<Clock::time_point> back = report.foreground;
    bool lost = !report.before.foreground;
    for (const platform::ForegroundChange& change : report.foregroundChanges) {
        if (!change.ours) {
            lost = true;
        } else if (lost && change.confirmed) {
            if (!back || change.at < *back) {
                back = change.at;
            }
            break;
        }
    }
    return back;
}

nlohmann::json describeChanges(const std::vector<platform::ForegroundChange>& changes, Clock::time_point origin) {
    nlohmann::json list = nlohmann::json::array();
    for (const platform::ForegroundChange& change : changes) {
        list.push_back({{"atMsFromLaunch", optionalMs(origin, change.at)},
                        {"eventMsFromLaunch", optionalMs(origin, change.eventAt)},
                        {"processId", change.processId},
                        {"image", change.image},
                        {"ours", change.ours},
                        {"confirmed", change.confirmed}});
    }
    return list;
}

/// The last-write time of a file, UTC ("2026-10-16T02:47:18Z"), or empty.
std::string modifiedUtc(const std::filesystem::path& path) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {};
    }
    const std::time_t seconds = std::chrono::system_clock::to_time_t(
        std::chrono::floor<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(written)));
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0) {
        return {};
    }
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec);
}

/// What the harness does with a Notepad launch: finds the window Notepad opened on the launch's file, leaves it open
/// for a while, then closes it (WM_CLOSE) and waits for it to go.
struct NotepadHarness {
    std::string titlePart;
    std::vector<void*> windows;
    std::optional<Clock::time_point> shownAt;
    std::optional<Clock::time_point> closeSentAt;
    std::optional<Clock::time_point> closedAt;

    void update(Clock::time_point now, std::chrono::milliseconds runFor) {
        if (closedAt || titlePart.empty()) {
            return;
        }
        if (windows.empty()) {
            windows = spike::harness::findWindows("notepad.exe", titlePart);
            if (!windows.empty()) {
                shownAt = now;
            }
            return;
        }
        if (!closeSentAt) {
            if (now - *shownAt >= runFor) {
                for (void* window : windows) {
                    spike::harness::closeWindow(window);
                }
                closeSentAt = now;
            }
            return;
        }
        if (std::none_of(windows.begin(), windows.end(), [](void* w) { return spike::harness::windowExists(w); })) {
            closedAt = now;
        }
    }
};

class LaunchSpike {
public:
    explicit LaunchSpike(SpikeOptions options) : m_options(std::move(options)) {}
    LaunchSpike(const LaunchSpike&) = delete;
    LaunchSpike& operator=(const LaunchSpike&) = delete;
    ~LaunchSpike() {
        if (m_pad != nullptr) {
            SDL_CloseJoystick(m_pad);
        }
        if (m_padId != 0) {
            SDL_DetachVirtualJoystick(m_padId);
        }
    }

    int run() {
        if (auto initialized = initialize(); !initialized) {
            spdlog::critical("spike_launch could not start: {}", initialized.error().toString());
            return kExitInitialization;
        }
        m_report["startup"] = warmUp();
        int succeeded = 0;
        for (int i = 0; i < m_options.repeat && !m_quit; ++i) {
            bool success = false;
            m_report["launches"].push_back(performLaunch(i, success));
            succeeded += success ? 1 : 0;
        }
        m_report["summary"] = summarize(succeeded);
        if (auto written = rv::core::writeTextFileAtomic(m_options.report, m_report.dump(2)); !written) {
            spdlog::error("The report could not be written: {}", written.error().toString());
            return kExitRuntime;
        }
        spdlog::info("Report: {}", utf8(m_options.report));
        if (m_quit && succeeded < m_options.repeat) {
            spdlog::warn("Stopped before every launch ran");
        }
        return succeeded == m_options.repeat ? kExitOk : kExitCriteriaMissed;
    }

private:
    rv::Status initialize() {
        rv::core::markMainThread();
        std::error_code ec;
        std::filesystem::create_directories(m_options.dataDir / "logs", ec);
        rv::core::LogConfig logConfig;
        logConfig.directory = m_options.dataDir / "logs";
        logConfig.fileName = "spike-launch.log";
        rv::core::initLogging(logConfig);
        spdlog::info("{}: launch spike, {} x {} ({}, {})", rv::core::versionBanner(), m_options.repeat,
                     spike::toString(m_options.target), launcher::toString(m_options.strategy),
                     platform::toString(m_options.policy));

        auto sdl = platform::SdlPlatform::init({"Retroverse launch spike", std::string(rv::core::kVersion),
                                                "io.github.pingumen96.retroverse.spikelaunch"});
        if (!sdl) {
            return rv::propagate(sdl.error());
        }
        m_sdl.emplace(std::move(*sdl));

        platform::WindowDesc windowDesc;
        windowDesc.title = "Retroverse launch spike";
        windowDesc.mode = m_options.windowed ? platform::WindowMode::Windowed : platform::WindowMode::Borderless;
        platform::GlContextDesc contextDesc;
        contextDesc.vsync = true;
        auto window = platform::createGraphicsWindow(windowDesc, contextDesc);
        if (!window) {
            return rv::propagate(window.error());
        }
        m_window.emplace(std::move(*window));
        auto device = rv::gl::GLDevice::create(m_window->glLoader(), rv::gl::GLDeviceConfig{});
        if (!device) {
            return rv::propagate(device.error());
        }
        m_device = std::move(*device);
        auto scene = spike::SpikeScene::create();
        if (!scene) {
            return rv::propagate(scene.error());
        }
        m_scene.emplace(std::move(*scene));

        m_wakeEvent = SDL_RegisterEvents(1);
        // The pad whose press proves that input resumes: SDL delivers it only while a Retroverse window has focus.
        SDL_VirtualJoystickDesc padDesc;
        SDL_INIT_INTERFACE(&padDesc);
        padDesc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        padDesc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        padDesc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        padDesc.name = "Retroverse launch spike pad";
        m_padId = SDL_AttachVirtualJoystick(&padDesc);
        m_pad = m_padId != 0 ? SDL_OpenJoystick(m_padId) : nullptr;
        if (m_pad == nullptr) {
            return rv::makeError(rv::ErrorCode::PlatformError,
                                 std::string("the virtual gamepad could not be attached: ") + SDL_GetError());
        }
        m_collector.gamepads().openConnected();
        m_presents.reserve(1u << 16);
        m_intervals.reserve(1u << 16);
        m_start = Clock::now();

        const rv::gl::GLCapabilities& caps = m_device->caps();
        m_report["spike"] = "I1 (f): emulator launch and return to the borderless fullscreen window";
        m_report["version"] = std::string(rv::core::kVersion);
        m_report["buildType"] = std::string(rv::core::kBuildType);
        const std::filesystem::path executable = rv::core::executablePath();
        m_report["build"] = {{"sourceRevision", m_options.sourceRevision},
                             {"executable", utf8(executable)},
                             {"executableModifiedUtc", modifiedUtc(executable)},
                             {"fakeEmulatorModifiedUtc", modifiedUtc(executable.parent_path() / "rv_fake_emulator.exe")}};
        m_report["commandLine"] = m_options.commandLine;
        m_report["machine"] = {{"foregroundLockTimeoutMs", spike::harness::foregroundLockTimeoutMs()},
                               {"glRenderer", caps.renderer},
                               {"glVersion", caps.version},
                               {"sdl", platform::SdlPlatform::versionString()},
                               {"displays", spike::harness::describeDisplays()}};
        m_report["options"] = {{"target", spike::toString(m_options.target)},
                               {"strategy", launcher::toString(m_options.strategy)},
                               {"policy", platform::toString(m_options.policy)},
                               {"repeat", m_options.repeat},
                               {"expect", launcher::toString(m_options.expected)},
                               {"settleMs", m_options.settle.count()},
                               {"timeoutS", m_options.timeout.count()},
                               {"windowTimeoutMs", m_options.windowTimeout.count()},
                               {"windowed", m_options.windowed}};
        m_report["launches"] = nlohmann::json::array();
        spdlog::info("Window {}x{} pixels, {}, vsync {}", m_window->drawableSize().x, m_window->drawableSize().y,
                     m_window->isFullscreen() ? "borderless fullscreen" : "windowed",
                     platform::toString(m_window->vsync()));
        return rv::ok();
    }

    [[nodiscard]] std::function<void()> wakeCallback() const {
        return [type = m_wakeEvent] {
            SDL_Event event{};
            event.type = type;
            SDL_PushEvent(&event); // thread-safe: wakes SDL_WaitEventTimeout on the main thread
        };
    }

    void handleEvent(const SDL_Event& event, bool feedInput) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_quit = true;
            return;
        case SDL_EVENT_KEY_DOWN:
            if (feedInput && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                m_quit = true;
            }
            break;
        default:
            break;
        }
        if (feedInput || event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) {
            m_collector.handleEvent(event);
        }
    }

    void pumpEvents(bool feedInput) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handleEvent(event, feedInput);
        }
    }

    void setPad(bool down) {
        if (m_pad != nullptr) {
            SDL_SetJoystickVirtualButton(m_pad, SDL_GAMEPAD_BUTTON_SOUTH, down);
        }
    }

    void renderFrame(const glm::vec4& status) {
        m_scene->render(*m_device, m_window->drawableSize(), spike::millisecondsBetween(m_start, Clock::now()) / 1000.0,
                        status);
        if (auto presented = m_window->swap(); !presented) {
            spdlog::error("Presenting failed: {}", presented.error().toString());
        }
        m_presents.push_back(Clock::now());
    }

    /// Retroverse is the foreground application when a player starts a game: before a launch the spike takes the
    /// foreground back if something else took it (with the remedies, rendering meanwhile, at most 2 s), and records
    /// what that needed.
    nlohmann::json ensureForeground() {
        platform::WindowHandover takeBack(m_window->handle(), platform::HandoverPolicy::Keep);
        if (takeBack.isForeground()) {
            return nlohmann::json{{"foreground", true}, {"remedies", nlohmann::json::array()}};
        }
        takeBack.restore();
        const Clock::time_point begin = Clock::now();
        while (!m_quit && !takeBack.update() && Clock::now() - begin < 2s) {
            pumpEvents(true);
            (void)m_collector.endFrame();
            renderFrame(kStatusWarmUp);
        }
        nlohmann::json remedies = nlohmann::json::array();
        for (const platform::ForegroundAttempt& attempt : takeBack.report().attempts) {
            remedies.push_back(platform::toString(attempt.remedy));
        }
        spdlog::info("Before the launch: {} ({})",
                     takeBack.isForeground() ? "foreground taken back" : "NOT in the foreground", remedies.dump());
        return nlohmann::json{{"foreground", takeBack.isForeground()}, {"remedies", remedies}};
    }

    nlohmann::json warmUp() {
        platform::WindowHandover startup(m_window->handle(), platform::HandoverPolicy::Keep);
        startup.restore(); // takes the foreground, with the remedies if Windows refuses it
        m_presents.clear();
        const Clock::time_point begin = Clock::now();
        while (!m_quit && Clock::now() - begin < m_options.warmUp) {
            pumpEvents(true);
            startup.update();
            (void)m_collector.endFrame();
            renderFrame(kStatusWarmUp);
        }
        const platform::HandoverReport& report = startup.report();
        nlohmann::json attempts = nlohmann::json::array();
        for (const platform::ForegroundAttempt& attempt : report.attempts) {
            attempts.push_back({{"remedy", platform::toString(attempt.remedy)},
                                {"atMs", optionalMs(begin, attempt.at)},
                                {"foregroundAfter", attempt.foregroundAfter}});
        }
        const spike::IntervalStats baseline = spike::intervalStats(m_presents, m_intervals);
        spdlog::info("Warm-up: {} frames, intervals p50 {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms; {}",
                     baseline.count + 1, baseline.p50Ms, baseline.p99Ms, baseline.maxMs,
                     startup.isForeground() ? "in the foreground" : "NOT in the foreground");
        return nlohmann::json{{"foreground", startup.isForeground()},
                              {"foregroundAtMs", optionalMs(begin, report.foreground)},
                              {"attempts", attempts},
                              {"window", describeWindowState(platform::captureWindowState(m_window->handle()))},
                              {"frameIntervals", spike::toJson(baseline)}};
    }

    /// The request of launch `index` (0-based) for the target, and its description in `target`.
    rv::Expected<launcher::LaunchRequest> makeRequest(int index, nlohmann::json& target, NotepadHarness& notepad) {
        launcher::LaunchRequest request;
        request.waitStrategy = m_options.strategy;
        request.timeouts.windowTimeout = m_options.windowTimeout;
        const int number = index + 1;
        switch (m_options.target) {
        case spike::TargetKind::Fake: {
            spike::FakeScenario scenario = m_options.fake;
            scenario.wrapper = m_options.strategy == launcher::WaitStrategy::ProcessTree;
            const std::filesystem::path log = m_options.dataDir / fmt::format("fake-{}.json", number);
            std::error_code ec;
            std::filesystem::remove(log, ec);
            scenario.log = utf8(log);
            request.executable = rv::core::executablePath().parent_path() / "rv_fake_emulator.exe";
            request.arguments = spike::fakeArguments(scenario);
            request.label = scenario.wrapper ? "fake emulator (through a wrapper)" : "fake emulator";
            request.outputLog = m_options.dataDir / "logs" / fmt::format("fake-{}.out.log", number);
            // A console program standing for a windowed emulator: its own console, without a window.
            request.hideConsoleWindow = true;
            target = {{"kind", "fake"}, {"log", scenario.log}};
            break;
        }
        case spike::TargetKind::RetroArch: {
            for (const auto& [name, path] : {std::pair{"--retroarch", m_options.retroarch},
                                             std::pair{"--core", m_options.core}, std::pair{"--rom", m_options.rom}}) {
                std::error_code ec;
                if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
                    return rv::makeError(rv::ErrorCode::NotFound, fmt::format("{} must name an existing file", name),
                                         utf8(path));
                }
            }
            spike::RetroArchSetup setup;
            setup.executable = m_options.retroarch;
            setup.core = m_options.core;
            setup.rom = m_options.rom;
            setup.scratch = m_options.dataDir / "retroarch";
            setup.logFile = setup.scratch / "logs" / fmt::format("retroarch-{}.log", number);
            setup.maxFrames = m_options.maxFrames;
            std::error_code ec;
            for (const std::filesystem::path& directory : spike::retroArchScratchDirectories(setup.scratch)) {
                std::filesystem::create_directories(directory, ec);
            }
            const std::filesystem::path config = spike::retroArchAppendConfigPath(setup.scratch);
            if (auto written = rv::core::writeTextFileAtomic(config, spike::retroArchAppendConfig(setup.scratch));
                !written) {
                return rv::propagate(written.error());
            }
            request.executable = setup.executable;
            request.arguments = spike::retroArchArguments(setup);
            request.label = "RetroArch (" + utf8(setup.rom.filename()) + ")";
            request.outputLog = m_options.dataDir / "logs" / fmt::format("retroarch-{}.out.log", number);
            target = {{"kind", "retroarch"},     {"core", utf8(setup.core)},
                      {"rom", utf8(setup.rom)},  {"appendConfig", utf8(config)},
                      {"maxFrames", setup.maxFrames}, {"log", utf8(setup.logFile)}};
            break;
        }
        case spike::TargetKind::Notepad: {
            if (spike::harness::isProcessRunning("notepad.exe")) {
                return rv::makeError(rv::ErrorCode::ValidationFailed,
                                     "Notepad is already running: close it first, so that the harness only ever "
                                     "closes the window this launch opens");
            }
            const std::string stem = fmt::format("spike-notepad-{}", number);
            const std::filesystem::path file = m_options.dataDir / (stem + ".txt");
            if (auto written = rv::core::writeTextFileAtomic(file, "Retroverse launch spike: Notepad target.\n");
                !written) {
                return rv::propagate(written.error());
            }
            request.executable = spike::harness::systemNotepad();
            request.arguments = {utf8(file)};
            request.label = "Notepad";
            notepad.titlePart = stem;
            target = {{"kind", "notepad"}, {"file", utf8(file)}};
            break;
        }
        }
        request.arguments.insert(request.arguments.end(), m_options.extraArguments.begin(),
                                 m_options.extraArguments.end());
        target["executable"] = utf8(request.executable);
        target["arguments"] = request.arguments;
        return request;
    }

    nlohmann::json performLaunch(int index, bool& success) {
        success = false;
        nlohmann::json record;
        record["index"] = index + 1;
        NotepadHarness notepad;
        auto request = makeRequest(index, record["target"], notepad);
        if (!request) {
            record["error"] = request.error().toString();
            spdlog::error("Launch {}: {}", index + 1, record["error"].get<std::string>());
            return record;
        }

        record["beforeLaunch"] = ensureForeground();
        platform::WindowHandover handover(m_window->handle(), m_options.policy);
        m_collector.releaseAll();
        setPad(false);
        (void)m_collector.endFrame();
        spike::harness::CpuLoadSampler cpu;
        cpu.start();

        // Leave: the window is handed over once the process exists (Retroverse still in the foreground when it allows
        // the emulator to take it), nothing renders, input is ignored.
        const Clock::time_point launchAt = Clock::now();
        handover.prepare();
        launcher::LaunchSession session(*request, m_runner);
        session.start(Clock::now(), launcher::StartCallbacks{wakeCallback(), [&handover](std::uint32_t /*processId*/) {
                                                                 handover.handOver();
                                                             }});
        if (session.process() != nullptr) {
            const platform::Win32StartInfo& start = m_runner.lastStart();
            record["start"] = {{"processId", start.processId},
                               {"jobAssigned", start.jobAssigned},
                               {"jobError", start.jobError},
                               {"foregroundAllowed", start.foregroundAllowed},
                               {"foregroundError", start.foregroundError},
                               {"startMs", static_cast<double>(start.startDuration.count()) / 1000.0},
                               {"commandLine", start.commandLine}};
        }
        bool harnessTerminated = false;
        const Clock::time_point deadline = launchAt + m_options.timeout;
        while (!m_quit && !session.finished()) {
            if (session.state() == launcher::SessionState::AwaitingConfirmation &&
                (m_options.target != spike::TargetKind::Notepad || notepad.closedAt)) {
                break; // the user would now come back and confirm: take the window back
            }
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, 100)) {
                handleEvent(event, false);
                pumpEvents(false);
            }
            const Clock::time_point now = Clock::now();
            session.poll(now);
            notepad.update(now, m_options.targetRun);
            if (now > deadline && !harnessTerminated) {
                harnessTerminated = true;
                spdlog::warn("Harness: the target outlived {} s; ending it (never done by the product)",
                             m_options.timeout.count());
                // Only processes still known to run in the tree, each checked against what was recorded of it: an id
                // whose process has exited may already name an unrelated program of the user's.
                session.poll(Clock::now());
                if (session.times().started) {
                    spike::harness::terminateProcessIf(session.processId(), utf8(request->executable.filename()),
                                                       *session.times().started, kHarnessExitCode);
                }
                for (const launcher::TreeProcess& child : session.liveChildren()) {
                    if (!child.image.empty()) { // unverified ones are left alone
                        spike::harness::terminateProcessIf(child.processId, child.image, child.seenAt,
                                                           kHarnessExitCode);
                    }
                }
                if (session.strategy() == launcher::WaitStrategy::UserConfirmReturn) {
                    session.confirmReturn(Clock::now());
                }
            }
        }
        const Clock::time_point noticedAt = Clock::now();
        // Windows lets the process that received the last keyboard or mouse input take the foreground: the spike's own
        // process may only have received it if it came after the spike started.
        const std::uint32_t idleAtExit = spike::harness::millisecondsSinceLastInput();
        const double uptimeAtExit = spike::millisecondsBetween(m_start, noticedAt);

        // Return.
        handover.restore();
        m_presents.clear();
        std::optional<Clock::time_point> inputAt;
        bool padDown = false;
        const Clock::time_point resumeAt = Clock::now();
        while (!m_quit) {
            const Clock::time_point now = Clock::now();
            const bool notepadPending = m_options.target == spike::TargetKind::Notepad && !notepad.closedAt &&
                                        now - launchAt < m_options.timeout;
            if (now - resumeAt >= m_options.settle && (inputAt || now - resumeAt >= m_options.timeout) &&
                !notepadPending) {
                break;
            }
            pumpEvents(true);
            handover.update();
            const rv::input::RawInputFrame frame = m_collector.endFrame();
            if (!inputAt) {
                const bool pressed = frame.gamepadConnected && m_collector.gamepads().activeId() == m_padId &&
                                     frame.gamepadButtonsPressed[rv::input::toIndex(rv::input::GamepadButton::South)];
                if (pressed) {
                    inputAt = Clock::now();
                    padDown = false;
                    setPad(false);
                    session.confirmReturn(*inputAt); // the confirmation of UserConfirmReturn; ignored otherwise
                } else {
                    padDown = !padDown;
                    setPad(padDown);
                }
            }
            session.poll(Clock::now());
            notepad.update(Clock::now(), m_options.targetRun);
            const bool back = inputAt.has_value() && handover.isForeground();
            renderFrame(back ? kStatusBack : kStatusReturning);
        }
        setPad(false);
        const double cpuLoad = cpu.load();

        // Measurements, from the emulator's exit.
        const launcher::SessionTimes& times = session.times();
        Clock::time_point exitAt = noticedAt;
        std::string exitReference = "the end of the session, noticed";
        if (m_options.target == spike::TargetKind::Notepad &&
            m_options.strategy == launcher::WaitStrategy::UserConfirmReturn && notepad.closedAt) {
            exitAt = *notepad.closedAt;
            exitReference = "Notepad's window closed (the stub's tree does not hold the real Notepad)";
        } else if (times.exited) {
            exitAt = *times.exited;
            exitReference = fmt::format("the system's exit time of process {}", session.outcome().exitProcessId);
        }
        const auto fromExit = [&](const std::optional<Clock::time_point>& point) -> std::optional<double> {
            if (!point) {
                return std::nullopt;
            }
            return spike::millisecondsBetween(exitAt, *point);
        };
        const platform::HandoverReport& handoverReport = handover.report();
        spike::ReturnTimes returnTimes;
        returnTimes.restored = fromExit(handoverReport.restored);
        returnTimes.foreground = fromExit(foregroundBack(handoverReport));
        returnTimes.fullscreen = fromExit(handoverReport.fullscreen);
        if (!m_presents.empty()) {
            returnTimes.firstFrame = fromExit(m_presents.front());
        }
        const std::optional<std::size_t> steady = spike::steadyFrom(m_presents);
        if (steady) {
            returnTimes.steady60 = fromExit(m_presents[*steady]);
        }
        returnTimes.input = fromExit(inputAt);
        const spike::IntervalStats afterReturn = spike::intervalStats(m_presents, m_intervals);

        const launcher::LaunchOutcome& outcome = session.outcome();
        const bool outcomeAsExpected = session.state() == m_options.expected;
        const bool criteria = spike::meetsCriteria(returnTimes);
        success = outcomeAsExpected && criteria && !harnessTerminated && !m_quit;

        record["strategy"] = {{"requested", launcher::toString(m_options.strategy)},
                              {"effective", launcher::toString(session.strategy())}};
        record["policy"] = platform::toString(m_options.policy);
        record["session"] = {{"state", launcher::toString(session.state())},
                             {"failure", launcher::toString(outcome.failure)},
                             {"exitCode",
                              outcome.exitCode ? nlohmann::json(*outcome.exitCode) : nlohmann::json(nullptr)},
                             {"exitDescription",
                              outcome.exitCode ? launcher::describeExitCode(*outcome.exitCode) : std::string()},
                             {"exitKind", launcher::toString(outcome.exitKind)},
                             {"exitProcessId", outcome.exitProcessId},
                             {"message", outcome.message},
                             {"quickExit", outcome.quickExit},
                             {"confirmedByUser", outcome.confirmedByUser},
                             {"processesSeen", outcome.processesSeen},
                             {"processesRemained", outcome.processesRemained},
                             {"wasStillRunning", outcome.wasStillRunning}};
        record["timesFromLaunchMs"] = {{"begun", optionalMs(launchAt, times.begun)},
                                       {"started", optionalMs(launchAt, times.started)},
                                       {"firstWindow", optionalMs(launchAt, times.firstWindow)},
                                       {"mainExited", optionalMs(launchAt, times.mainExited)},
                                       {"exited", optionalMs(launchAt, times.exited)},
                                       {"exitObserved", optionalMs(launchAt, times.exitObserved)},
                                       {"sessionEnded", optionalMs(launchAt, times.ended)},
                                       {"noticed", optionalMs(launchAt, noticedAt)},
                                       {"handedOver", optionalMs(launchAt, handoverReport.handedOver)},
                                       {"restoreBegun", optionalMs(launchAt, handoverReport.restoreBegun)}};
        record["exitReference"] = exitReference;
        record["idleAtExitMs"] = idleAtExit;
        record["spikeUptimeAtExitMs"] = std::round(uptimeAtExit);
        // The last input happened before the spike's process existed: the spike cannot have received it.
        record["lastInputBeforeSpike"] = static_cast<double>(idleAtExit) > uptimeAtExit;
        record["detectionLatencyMs"] = times.exited && times.exitObserved
                                           ? optionalMs(*times.exited, times.exitObserved)
                                           : nlohmann::json(nullptr);
        record["returnMs"] = spike::toJson(returnTimes);
        record["meetsCriteria"] = criteria;
        record["outcomeAsExpected"] = outcomeAsExpected;
        record["harnessTerminated"] = harnessTerminated;
        record["success"] = success;
        record["cpuLoad"] = std::round(cpuLoad * 1000.0) / 1000.0;

        nlohmann::json attempts = nlohmann::json::array();
        for (const platform::ForegroundAttempt& attempt : handoverReport.attempts) {
            attempts.push_back({{"remedy", platform::toString(attempt.remedy)},
                                {"atMsFromExit", optionalMs(exitAt, attempt.at)},
                                {"foregroundAfter", attempt.foregroundAfter}});
        }
        record["handover"] = {{"before", describeWindowState(handoverReport.before)},
                              {"afterReturn", describeWindowState(handoverReport.afterReturn)},
                              {"restoredByWin32", handoverReport.restoredByWin32},
                              {"fullscreenReapplied", handoverReport.fullscreenReapplied},
                              {"movedBackToDisplay", handoverReport.movedBackToDisplay},
                              {"displayModeChanged", handoverReport.displayModeChanged},
                              {"userActionNeeded", handoverReport.userActionNeeded},
                              {"attempts", attempts},
                              {"foregroundChanges", describeChanges(handoverReport.foregroundChanges, launchAt)},
                              {"polledChanges", describeChanges(handoverReport.polledChanges, launchAt)}};
        nlohmann::json firstIntervals = nlohmann::json::array();
        for (std::size_t i = 1; i < m_presents.size() && i <= 12; ++i) {
            firstIntervals.push_back(std::round(spike::millisecondsBetween(m_presents[i - 1], m_presents[i]) * 100.0) /
                                     100.0);
        }
        record["frames"] = {{"afterReturn", spike::toJson(afterReturn)},
                            {"firstIntervalsMs", firstIntervals},
                            {"steadyFromFrame", steady ? nlohmann::json(*steady) : nlohmann::json(nullptr)}};
        record["input"] = {{"seen", inputAt.has_value()}};
        nlohmann::json history = nlohmann::json::array();
        for (const launcher::ProcessEvent& observed : session.history()) {
            history.push_back(
                {{"event", launcher::toString(observed.kind)},
                 {"processId", observed.processId},
                 {"image", observed.image},
                 {"exitCode", observed.exitCode ? nlohmann::json(*observed.exitCode) : nlohmann::json(nullptr)},
                 {"occurredMsFromLaunch", optionalMs(launchAt, observed.occurredAt)},
                 {"observedMsFromLaunch", optionalMs(launchAt, observed.observedAt)}});
        }
        record["processEvents"] = history;
        if (m_options.target == spike::TargetKind::Fake) {
            const std::filesystem::path log = rv::core::pathFromUtf8(record["target"]["log"].get<std::string>());
            if (auto json = rv::core::readJsonFile(log)) {
                record["fake"] = *json;
            }
        }
        if (m_options.target == spike::TargetKind::Notepad) {
            record["notepad"] = {{"shownMsFromLaunch", optionalMs(launchAt, notepad.shownAt)},
                                 {"closeSentMsFromLaunch", optionalMs(launchAt, notepad.closeSentAt)},
                                 {"closedMsFromLaunch", optionalMs(launchAt, notepad.closedAt)}};
        }

        const auto show = [](const std::optional<double>& value) {
            return value ? fmt::format("{:7.1f}", *value) : std::string("      -");
        };
        std::string remedies;
        for (const platform::ForegroundAttempt& attempt : handoverReport.attempts) {
            remedies += (remedies.empty() ? "" : ",") + std::string(platform::toString(attempt.remedy));
        }
        spdlog::info("Launch {}: {} ({}); ms after exit: restored {} foreground {} fullscreen {} first frame {} "
                     "60 FPS {} input {}; remedies [{}]; load {:.0f}%; {}",
                     index + 1, launcher::toString(session.state()), outcome.message, show(returnTimes.restored),
                     show(returnTimes.foreground), show(returnTimes.fullscreen), show(returnTimes.firstFrame),
                     show(returnTimes.steady60), show(returnTimes.input), remedies, cpuLoad * 100.0,
                     success ? "OK" : "FAILED");
        return record;
    }

    nlohmann::json summarize(int succeeded) const {
        std::map<std::string, std::vector<double>> values;
        std::map<std::string, int> remedies;
        std::vector<double> loads;
        int reapplied = 0;
        int modeChanged = 0;
        int userAction = 0;
        for (const nlohmann::json& launch : m_report["launches"]) {
            if (!launch.contains("returnMs")) {
                continue;
            }
            for (const char* key :
                 {"restored", "foreground", "fullscreen", "firstFrame", "steady60", "input", "latest"}) {
                const nlohmann::json& value = launch["returnMs"][key];
                if (value.is_number()) {
                    values[key].push_back(value.get<double>());
                }
            }
            for (const nlohmann::json& attempt : launch["handover"]["attempts"]) {
                ++remedies[attempt["remedy"].get<std::string>()];
            }
            reapplied += launch["handover"]["fullscreenReapplied"].get<bool>() ? 1 : 0;
            modeChanged += launch["handover"]["displayModeChanged"].get<bool>() ? 1 : 0;
            userAction += launch["handover"]["userActionNeeded"].get<bool>() ? 1 : 0;
            loads.push_back(launch["cpuLoad"].get<double>());
        }
        nlohmann::json returnMs = nlohmann::json::object();
        for (const auto& [key, list] : values) {
            returnMs[key] = spike::toJson(spike::summarize(list));
        }
        nlohmann::json summary = {{"launches", m_report["launches"].size()},
                                  {"succeeded", succeeded},
                                  {"allMetCriteria", succeeded == m_options.repeat},
                                  {"returnMs", returnMs},
                                  {"remedyAttempts", remedies},
                                  {"fullscreenReapplied", reapplied},
                                  {"displayModeChanged", modeChanged},
                                  {"userActionNeeded", userAction},
                                  {"cpuLoad", spike::toJson(spike::summarize(loads))}};
        spdlog::info("Summary: {} of {} launch(es) met the criteria; latest return median {} ms, best {} ms",
                     succeeded, m_report["launches"].size(),
                     values.contains("latest") ? fmt::format("{:.1f}", spike::summarize(values.at("latest")).median)
                                               : std::string("-"),
                     values.contains("latest") ? fmt::format("{:.1f}", spike::summarize(values.at("latest")).best)
                                               : std::string("-"));
        return summary;
    }

    SpikeOptions m_options;
    std::optional<platform::SdlPlatform> m_sdl;
    std::optional<platform::GraphicsWindow> m_window;
    std::unique_ptr<rv::gl::GLDevice> m_device;
    std::optional<spike::SpikeScene> m_scene;
    platform::SdlInputCollector m_collector;
    platform::Win32ProcessRunner m_runner;
    SDL_JoystickID m_padId = 0;
    SDL_Joystick* m_pad = nullptr;
    Uint32 m_wakeEvent = 0;
    Clock::time_point m_start{};
    std::vector<Clock::time_point> m_presents;
    std::vector<double> m_intervals;
    nlohmann::json m_report;
    bool m_quit = false;
};

/// Parses the command line; prints the usage or the error itself. Nothing when the program must exit with `exitCode`.
std::optional<SpikeOptions> parseOptions(int argc, char** argv, int& exitCode) {
    std::vector<std::string> arguments;
    std::vector<std::string> extra;
    bool inExtra = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i] != nullptr ? argv[i] : "";
        if (!inExtra && argument == "--") {
            inExtra = true;
            continue;
        }
        (inExtra ? extra : arguments).push_back(argument);
    }
    rv::core::ArgParser parser("spike_launch", "I1 spike (f): launches an emulator from a borderless fullscreen "
                                               "window and measures the return. Arguments after -- go to the target.");
    parser.option("target", "fake|retroarch|notepad", "What to launch", "fake")
        .option("strategy", "direct|tree|confirm", "When the emulator counts as gone", "direct")
        .option("policy", "minimize|keep", "What the window does while the emulator runs", "minimize")
        .option("repeat", "count", "Launches in a row", "1")
        .option("report", "file", "The JSON report (default <data-dir>/launch-report.json)")
        .option("data-dir", "dir", "Logs, report and scratch files (default <exe dir>/spike-launch-data)")
        .option("frames", "count", "fake: frames of its fullscreen window", "150")
        .option("fake-exit-code", "code", "fake: its exit code", "0")
        .flag("fake-crash", "fake: an access violation after its frames")
        .flag("fake-exclusive", "fake: exclusive fullscreen in another display mode")
        .flag("fake-steal-focus", "fake: forces itself into the foreground")
        .flag("fake-input", "fake: a key tap into its window before it closes (the foreground lock holds)")
        .option("fake-foreground-window", "ms",
                "fake: leaves another application's window in the foreground for this long when it exits", "0")
        .flag("fake-lock-foreground", "fake: that window also calls LockSetForegroundWindow")
        .flag("fake-hung-foreground", "fake: that window stops responding (the fake exits once Windows sees it hung)")
        .option("retroarch", "file", "retroarch: retroarch.exe")
        .option("core", "file", "retroarch: the libretro core")
        .option("rom", "file", "retroarch: the game, only read")
        .option("max-frames", "count", "retroarch: frames before it exits by itself", "240")
        .option("target-run-ms", "ms", "notepad: how long the harness leaves its window open", "2000")
        .option("expect", "returned|crashed|failed", "The outcome every session must have", "returned")
        .option("settle-ms", "ms", "Rendering after each return", "1500")
        .option("warmup-ms", "ms", "Rendering before the first launch", "1500")
        .option("timeout-s", "seconds", "The harness ends a target still running after this", "60")
        .option("window-timeout-ms", "ms", "The session's window timeout (StillRunning)", "30000")
        .option("source-revision", "text", "What the build was made from, recorded in the report (measure.ps1)")
        .flag("windowed", "A 1280x720 window instead of borderless fullscreen");
    const auto parsed = parser.parse(arguments);
    if (!parsed) {
        std::fprintf(stderr, "%s\n\n%s", parsed.error().toString().c_str(), parser.helpText().c_str());
        exitCode = kExitInitialization;
        return std::nullopt;
    }
    if (parsed->has("help")) {
        std::fputs(parser.helpText().c_str(), stdout);
        exitCode = kExitOk;
        return std::nullopt;
    }
    const auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "spike_launch: %s\n", message.c_str());
        exitCode = kExitInitialization;
        return std::nullopt;
    };
    SpikeOptions options;
    options.extraArguments = std::move(extra);
    for (int i = 0; i < argc; ++i) {
        options.commandLine.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    const auto target = spike::parseTargetKind(parsed->value("target").value_or("fake"));
    const auto strategy = launcher::parseWaitStrategy(parsed->value("strategy").value_or("direct"));
    const auto policy = platform::parseHandoverPolicy(parsed->value("policy").value_or("minimize"));
    if (!target || !strategy || !policy) {
        return fail("unknown --target, --strategy or --policy");
    }
    options.target = *target;
    options.strategy = *strategy;
    options.policy = *policy;
    const std::string expect = parsed->value("expect").value_or("returned");
    if (expect == "returned") {
        options.expected = launcher::SessionState::Returned;
    } else if (expect == "crashed") {
        options.expected = launcher::SessionState::Crashed;
    } else if (expect == "failed") {
        options.expected = launcher::SessionState::FailedToStart;
    } else {
        return fail("--expect takes returned, crashed or failed");
    }
    const auto integer = [&](std::string_view name, int fallback, int low, int high) -> std::optional<int> {
        const auto value = parsed->intValue(name, fallback);
        if (!value || *value < low || *value > high) {
            return std::nullopt;
        }
        return *value;
    };
    const auto repeat = integer("repeat", 1, 1, 1000);
    const auto frames = integer("frames", 150, 1, 1000000);
    const auto maxFrames = integer("max-frames", 240, 1, 1000000);
    const auto targetRun = integer("target-run-ms", 2000, 0, 600000);
    const auto settle = integer("settle-ms", 1500, 100, 600000);
    const auto warm = integer("warmup-ms", 1500, 0, 600000);
    const auto timeout = integer("timeout-s", 60, 1, 3600);
    const auto windowTimeout = integer("window-timeout-ms", 30000, 0, 3600000);
    const auto foregroundWindow = integer("fake-foreground-window", 0, 0, 600000);
    const auto exitCodeValue = parsed->intValue("fake-exit-code", 0);
    if (!repeat || !frames || !maxFrames || !targetRun || !settle || !warm || !timeout || !windowTimeout ||
        !foregroundWindow || !exitCodeValue) {
        return fail("a numeric option is out of range");
    }
    options.repeat = *repeat;
    options.fake.frames = *frames;
    options.fake.exitCode = static_cast<std::uint32_t>(*exitCodeValue);
    options.fake.crash = parsed->has("fake-crash");
    options.fake.exclusive = parsed->has("fake-exclusive");
    options.fake.stealFocus = parsed->has("fake-steal-focus");
    options.fake.inputAtEnd = parsed->has("fake-input");
    options.fake.foregroundWindowMs = *foregroundWindow;
    options.fake.lockForeground = parsed->has("fake-lock-foreground");
    options.fake.hungForeground = parsed->has("fake-hung-foreground");
    options.sourceRevision = parsed->value("source-revision").value_or("");
    options.maxFrames = *maxFrames;
    options.targetRun = std::chrono::milliseconds(*targetRun);
    options.settle = std::chrono::milliseconds(*settle);
    options.warmUp = std::chrono::milliseconds(*warm);
    options.timeout = std::chrono::seconds(*timeout);
    options.windowTimeout = std::chrono::milliseconds(*windowTimeout);
    options.windowed = parsed->has("windowed");
    options.retroarch = rv::core::pathFromUtf8(parsed->value("retroarch").value_or(""));
    options.core = rv::core::pathFromUtf8(parsed->value("core").value_or(""));
    options.rom = rv::core::pathFromUtf8(parsed->value("rom").value_or(""));
    const auto absolute = [](const std::filesystem::path& path) {
        std::error_code ec;
        const std::filesystem::path result = std::filesystem::absolute(path, ec);
        return ec ? path : result;
    };
    options.dataDir = parsed->value("data-dir")
                          ? absolute(rv::core::pathFromUtf8(*parsed->value("data-dir")))
                          : rv::core::executablePath().parent_path() / "spike-launch-data";
    options.report = parsed->value("report") ? absolute(rv::core::pathFromUtf8(*parsed->value("report")))
                                             : options.dataDir / "launch-report.json";
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    int exitCode = kExitOk;
    std::optional<SpikeOptions> options = parseOptions(argc, argv, exitCode);
    if (!options) {
        return exitCode;
    }
    try {
        LaunchSpike spike(std::move(*options));
        exitCode = spike.run();
    } catch (const std::exception& e) {
        spdlog::critical("spike_launch failed: {}", e.what());
        exitCode = kExitRuntime;
    } catch (...) {
        spdlog::critical("spike_launch failed with an unknown exception");
        exitCode = kExitRuntime;
    }
    rv::core::shutdownLogging();
    return exitCode;
}
