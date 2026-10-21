#include <rv/app/app_host.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL_events.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/log.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/platform/dialogs.hpp>
#include <rv/platform/event_pump.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_platform.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::app {
namespace {

#if defined(RV_GL_DEBUG)
constexpr bool kGlDebug = true;
#else
constexpr bool kGlDebug = false;
#endif

#if defined(RV_DEV_UI)
constexpr bool kDevUi = true;
#else
constexpr bool kDevUi = false;
#endif

constexpr double kTitleRefreshSeconds = 1.0;
constexpr double kMebibyte = 1024.0 * 1024.0;
constexpr std::chrono::milliseconds kMinimizedWait{100};

/// Shown when OpenGL 4.5 is not available.
std::string openGlRequired(std::string_view name) {
    return fmt::format("{0} requires OpenGL 4.5 (core profile). Update the graphics driver, or make sure Windows runs "
                       "{0} on a GPU that supports OpenGL 4.5.",
                       name);
}

std::string describeRunMode(const CommandLineOptions& options) {
    std::string mode;
    if (options.smoke) {
        mode = fmt::format("smoke test, {} frame(s)", options.frameLimit.value_or(0));
    } else if (options.exitAfterFrames) {
        mode = fmt::format("exit after {} frame(s)", options.frameLimit.value_or(0));
    } else {
        mode = "interactive";
    }
    if (options.hidden) {
        mode += ", hidden window";
    }
    if (options.fullscreen) {
        mode += ", borderless fullscreen";
    }
    if (!options.vsync) {
        mode += ", vsync off";
    }
    if (options.glSelfTest) {
        mode += ", GL self-test";
    }
    if (options.fixedDeltaSeconds) {
        mode += fmt::format(", fixed time step {} s", *options.fixedDeltaSeconds);
    }
    if (options.inputScript) {
        mode += ", input script " + core::pathToUtf8(*options.inputScript);
    }
    if (options.model) {
        mode += ", model viewer " + core::pathToUtf8(*options.model);
    }
    if (options.debugDraw) {
        mode += ", debug lines";
    }
    // Builds without the overlay ignore both options (and warn about them).
    if constexpr (kDevUi) {
        if (options.devUiOpen) {
            mode += ", developer overlay open";
        }
        if (options.screenshotIncludeUi) {
            mode += ", overlay in screenshots";
        }
    }
    return mode;
}

void logStartupBanner(std::string_view name, const core::Paths& paths, const CommandLineOptions& options) {
    if (name == core::kProductName) {
        spdlog::info("{}", core::versionBanner());
    } else {
        spdlog::info("{}: {}", core::versionBanner(), name);
    }
    spdlog::info("Executable: {}", core::pathToUtf8(core::executablePath()));
    spdlog::info("Asset root: {} ({})", core::pathToUtf8(paths.assetRoot()), core::toString(paths.assetRootSource()));
    spdlog::info("User data: {}{}", core::pathToUtf8(paths.userDataRoot()), paths.isPortable() ? " (portable)" : "");
    const std::filesystem::path logFile = core::activeLogFile();
    spdlog::info("Log file: {}", logFile.empty() ? std::string("disabled") : core::pathToUtf8(logFile));
    spdlog::info("Build: GL debug output {}, developer UI {}", kGlDebug ? "on" : "off", kDevUi ? "on" : "off");
    spdlog::info("Run mode: {}", describeRunMode(options));
}

platform::WindowDesc makeWindowDesc(const CommandLineOptions& options, std::string title) {
    platform::WindowDesc desc;
    desc.title = std::move(title);
    desc.width = options.windowWidth;
    desc.height = options.windowHeight;
    desc.hidden = options.hidden;
    desc.mode = options.fullscreen ? platform::WindowMode::Borderless : platform::WindowMode::Windowed;
    return desc;
}

platform::GlContextDesc makeContextDesc(const CommandLineOptions& options) {
    platform::GlContextDesc desc;
    desc.debug = kGlDebug;
    desc.vsync = options.vsync;
    return desc;
}

void logWindow(const platform::GraphicsWindow& window) {
    const glm::ivec2 size = window.windowSize();
    const glm::ivec2 pixels = window.drawableSize();
    spdlog::info("Window: {}x{} ({}x{} pixels, display scale {:.2f}), {}, vsync {}", size.x, size.y, pixels.x, pixels.y,
                 window.displayScale(), window.isFullscreen() ? "borderless fullscreen" : "windowed",
                 platform::toString(window.vsync()));
}

SelfTestOutcome runGlSelfTest(gl::GLDevice& device) {
    spdlog::info("GL self-test: calling glEnable(0xFFFF) to provoke GL_INVALID_ENUM; the next GL error is expected");
    const gl::DebugSelfTestResult result = device.runDebugSelfTest();
    SelfTestOutcome outcome;
    outcome.errorFlagRaised = result.errorFlagRaised;
    outcome.debugOutputActive = result.debugOutputActive;
    outcome.messagesCaptured = result.messagesCaptured;
    outcome.highSeverityCaptured = result.highSeverityCaptured;
    if (!outcome.errorFlagRaised) {
        spdlog::error("GL self-test failed: glGetError() did not report GL_INVALID_ENUM");
    } else if (!outcome.debugOutputActive) {
        spdlog::warn("GL self-test: GL_INVALID_ENUM raised; debug output is off in this build, so the callback was "
                     "not exercised");
    } else if (outcome.messagesCaptured == 0) {
        spdlog::error("GL self-test failed: GL_INVALID_ENUM was raised but the debug callback reported nothing");
    } else {
        spdlog::info("GL self-test passed: GL_INVALID_ENUM raised and reported by the debug callback "
                     "({} message(s), {} high severity)",
                     outcome.messagesCaptured, outcome.highSeverityCaptured);
    }
    return outcome;
}

/// "<count> frame(s), p50 <a> ms, p95 <b> ms, p99 <c> ms, max <d> ms", or "no frame".
std::string describeTimes(const std::optional<TimeSummary>& times) {
    if (!times || times->count == 0) {
        return "no frame";
    }
    return fmt::format("{} frame(s), p50 {:.2f} ms, p95 {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms", times->count,
                       times->p50Ms, times->p95Ms, times->p99Ms, times->maxMs);
}

/// "; GPU <pass> p50 <a> ms p99 <b> ms, ..." for the passes that reported a time, or nothing.
std::string describeGpuPasses(const std::vector<GpuPassRecord>& passes) {
    std::string text;
    for (const GpuPassRecord& pass : passes) {
        text += fmt::format("{}{} p50 {:.3f} ms p99 {:.3f} ms", text.empty() ? "; GPU " : ", ", pass.name,
                            pass.times.p50Ms, pass.times.p99Ms);
    }
    return text;
}

/// The measurements of a run, once at its end: frame times (and GPU passes), phases, CPU sections, GPU memory.
void logMeasurements(const RunReport& report) {
    spdlog::info("Frame times of the run (the first {} frame(s) left out): {}{}", report.frameTimes.warmUpFrames,
                 describeTimes(report.frameTimes.times), describeGpuPasses(report.frameTimes.gpuPasses));
    for (const PhaseRecord& phase : report.phases) {
        spdlog::info("Phase '{}': opened {} time(s){}, {} frame(s); frame times {}{}", phase.name, phase.opened,
                     phase.open ? ", open at the end" : "", phase.frames, describeTimes(phase.times),
                     describeGpuPasses(phase.gpuPasses));
    }
    for (const CpuSectionRecord& section : report.cpuSections) {
        spdlog::info("CPU section '{}': {}", section.name, describeTimes(section.times));
    }
    std::string kinds;
    for (const gl::GpuMemoryKind kind : gl::kGpuMemoryKinds) {
        const gl::GpuMemoryUsage& usage = report.gpuMemory.usage(kind);
        kinds += fmt::format("{}{} {:.1f} MiB ({})", kinds.empty() ? "" : ", ", gl::toString(kind),
                             static_cast<double>(usage.bytes) / kMebibyte, usage.allocations);
    }
    spdlog::info("GPU memory (the renderer's accounting): {:.1f} MiB, peak {:.1f} MiB; {}",
                 static_cast<double>(report.gpuMemory.totalBytes) / kMebibyte,
                 static_cast<double>(report.gpuMemory.peakTotalBytes) / kMebibyte, kinds);
}

/// The report fields of the executable itself.
RunReport initialReport(const AppIdentity& identity, const CommandLineOptions& options) {
    RunReport report;
    report.application = std::string(identity.name);
    report.version = std::string(core::kVersion);
    report.buildType = std::string(core::kBuildType);
    report.developerOverlay = kDevUi;
    report.glDebugOutput = kGlDebug;
    report.frameLimit = options.frameLimit;
    return report;
}

/// Records the GL debug message counts of a device into the report when it goes out of scope. Declared after the
/// device and before the logic's shutdown guard, it records after the logic released its GPU resources and before the
/// device is destroyed, on every way out of the run.
class DebugMessageRecorder {
public:
    DebugMessageRecorder(const gl::GLDevice& device, RunReport& report) noexcept : m_device(device), m_report(report) {}
    DebugMessageRecorder(const DebugMessageRecorder&) = delete;
    DebugMessageRecorder& operator=(const DebugMessageRecorder&) = delete;
    ~DebugMessageRecorder() {
        if (!m_device.debugOutputEnabled()) {
            return;
        }
        const std::uint64_t high = m_device.debugMessageCount(gl::DebugSeverity::High);
        const std::uint64_t mediumOrMore = m_device.debugMessageCount(gl::DebugSeverity::Medium);
        const std::uint64_t lowOrMore = m_device.debugMessageCount(gl::DebugSeverity::Low);
        m_report.debugMessages = DebugMessageCounts{high, mediumOrMore - high, lowOrMore - mediumOrMore};
    }

private:
    const gl::GLDevice& m_device;
    RunReport& m_report;
};

/// Calls IAppLogic::onShutdown exactly once: explicitly at the end of a normal run, or on scope exit when
/// initialization fails or an exception unwinds the run (the window and device are still alive at that point).
class LogicShutdownGuard {
public:
    LogicShutdownGuard(IAppLogic& logic, AppContext& context) noexcept : m_logic(logic), m_context(context) {}
    LogicShutdownGuard(const LogicShutdownGuard&) = delete;
    LogicShutdownGuard& operator=(const LogicShutdownGuard&) = delete;
    ~LogicShutdownGuard() { shutdown(); }

    void shutdown() noexcept {
        if (m_done) {
            return;
        }
        m_done = true;
        try {
            m_logic.onShutdown(m_context);
        } catch (const std::exception& e) {
            spdlog::error("Application shutdown failed: {}", e.what());
        } catch (...) {
            spdlog::error("Application shutdown failed with an unknown exception");
        }
    }

private:
    IAppLogic& m_logic;
    AppContext& m_context;
    bool m_done = false;
};

} // namespace

AppHost::AppHost(CommandLineOptions options, AppIdentity identity)
    : m_options(std::move(options)), m_identity(identity), m_report(initialReport(m_identity, m_options)) {}

std::string AppHost::baseWindowTitle() const {
    return fmt::format("{} {}", m_identity.name, core::kVersion);
}

int AppHost::run(IAppLogic& logic) noexcept {
    ExitCode exitCode = ExitCode::RuntimeFailure;
    try {
        exitCode = runUnchecked(logic);
    } catch (const std::exception& e) {
        reportUnexpectedFailure(e.what());
    } catch (...) {
        reportUnexpectedFailure("unknown exception");
    }
    recordMeasurements();
    exitCode = finishReport(exitCode);
    try {
        spdlog::info("Exiting with code {} ({})", toInt(exitCode), toString(exitCode));
        core::shutdownLogging();
    } catch (...) {
        // The exit code matters more than the last log line.
    }
    return toInt(exitCode);
}

void AppHost::recordMeasurements() noexcept {
    try {
        m_report.frameTimes = m_frameTiming.runTimes();
        m_report.phases = m_frameTiming.phases();
        m_report.cpuSections = cpuSectionRecords(core::mainThreadCpuTimers());
        if (!m_gpuMemoryRecorded) {
            m_report.gpuMemory = gpuMemoryRecord(gl::gpuMemoryLedger());
        }
        if (m_options.frameLimit) {
            logMeasurements(m_report);
        }
    } catch (const std::exception& e) {
        spdlog::error("The run's measurements are incomplete: {}", e.what());
    } catch (...) {
        // The report goes out with what it has.
    }
}

ExitCode AppHost::finishReport(ExitCode exitCode) noexcept {
    if (!m_options.runReport) {
        return exitCode;
    }
    try {
        m_report.exitCode = toInt(exitCode);
        m_report.outcome = std::string(toString(exitCode));
        if (auto written = writeRunReport(*m_options.runReport, m_report); !written) {
            spdlog::error("Run report not written: {}", written.error().toString());
            return exitCode == ExitCode::Ok ? ExitCode::RuntimeFailure : exitCode;
        }
        spdlog::info("Run report written: {}", core::pathToUtf8(*m_options.runReport));
    } catch (const std::exception& e) {
        spdlog::error("Run report not written: {}", e.what());
        return exitCode == ExitCode::Ok ? ExitCode::RuntimeFailure : exitCode;
    } catch (...) {
        return exitCode == ExitCode::Ok ? ExitCode::RuntimeFailure : exitCode;
    }
    return exitCode;
}

ExitCode AppHost::runUnchecked(IAppLogic& logic) {
    core::markMainThread();
    // An unattended run (frame-limited or --no-dialogs, or started by a test: RV_LAUNCH_BACKGROUND) shows no failure
    // dialog at all: neither its own message boxes (noDialogs) nor the loader's, the crash or the C runtime's.
    if (m_options.noDialogs || platform_win::backgroundLaunchesRequested()) {
        platform_win::disableFailureDialogs();
    }

    // --- Directories and logging (§16.1) -----------------------------------------------------------------------------
    core::PathOptions pathOptions;
    pathOptions.assetRootOverride = m_options.assetRoot;
    pathOptions.userDataOverride = m_options.dataDir;
    pathOptions.portable = m_options.portable;
    const auto paths = core::Paths::resolve(pathOptions, core::PathEnvironment::fromProcess());

    core::LogConfig logConfig;
    logConfig.fileName = std::string(m_identity.logFileName);
    logConfig.level = m_options.logLevel;
    if (paths) {
        logConfig.directory = paths->logsDir();
    }
    core::initLogging(logConfig);
    if (!paths) {
        return failInitialization(fmt::format("{} could not locate its directories.", m_identity.name), paths.error());
    }
    logStartupBanner(m_identity.name, *paths, m_options);
    m_report.assetRoot = paths->assetRoot();
    m_report.assetRootSource = std::string(core::toString(paths->assetRootSource()));

    // --- SDL, window, OpenGL context, GL device ----------------------------------------------------------------------
    const platform::AppMetadata metadata{std::string(m_identity.name), std::string(core::kVersion),
                                         std::string(m_identity.identifier)};
    const auto sdl = platform::SdlPlatform::init(metadata);
    if (!sdl) {
        return failInitialization(fmt::format("{} could not initialize SDL.", m_identity.name), sdl.error());
    }
    spdlog::info("{}, video driver: {}", platform::SdlPlatform::versionString(), platform::SdlPlatform::videoDriver());

    auto window =
        platform::createGraphicsWindow(makeWindowDesc(m_options, baseWindowTitle()), makeContextDesc(m_options));
    if (!window) {
        // GraphicsError: the window exists but no OpenGL 4.5 core context could be created for it.
        m_report.openGlUnavailable = window.error().code == ErrorCode::GraphicsError;
        return failInitialization(m_report.openGlUnavailable
                                      ? openGlRequired(m_identity.name)
                                      : fmt::format("{} could not create its window.", m_identity.name),
                                  window.error());
    }
    logWindow(*window);

    const auto device = gl::GLDevice::create(window->glLoader(), gl::GLDeviceConfig{.debugOutput = kGlDebug});
    if (!device) {
        m_report.openGlUnavailable = true;
        return failInitialization(openGlRequired(m_identity.name), device.error());
    }
    const gl::GLCapabilities& caps = (*device)->caps();
    m_report.context = RunReportContext{caps.vendor, caps.renderer, caps.version, caps.glslVersion};
    const DebugMessageRecorder debugMessages(**device, m_report);

    // --- Application logic -------------------------------------------------------------------------------------------
    core::FrameClock clock;
    AppContext context(m_options, *paths, *window, **device, clock, m_report, m_frameTiming);
    context.m_framebufferSize = window->drawableSize();
    // The wake event of the idle mode: pushed from any thread (SDL_PushEvent is thread-safe), kept by the host.
    context.m_wakeEvent = SDL_RegisterEvents(1);
    if (context.m_wakeEvent != 0) {
        context.m_wake = [type = context.m_wakeEvent] {
            SDL_Event event{};
            event.type = type;
            SDL_PushEvent(&event);
        };
    } else {
        spdlog::warn("No SDL event type left for the wake event: an idle wait ends only after {} ms",
                     kIdleWait.count());
    }

    LogicShutdownGuard logicScope(logic, context);
    if (auto initialized = logic.onInit(context); !initialized) {
        return failInitialization(fmt::format("{} could not load its content.", m_identity.name), initialized.error());
    }
    std::optional<SelfTestOutcome> selfTest;
    if (m_options.glSelfTest) {
        selfTest = runGlSelfTest(**device);
        m_report.glSelfTest = selfTest;
    }

    const Status loop = runMainLoop(logic, context);
    // What the last frame had allocated, before the logic releases it.
    m_report.gpuMemory = gpuMemoryRecord(gl::gpuMemoryLedger());
    m_gpuMemoryRecorded = true;
    if (!loop) {
        reportUnexpectedFailure(loop.error().toString());
        return ExitCode::RuntimeFailure;
    }
    // Release the logic's GPU resources now, so that messages they cause still count towards the result.
    logicScope.shutdown();
    if (context.m_failure) {
        reportRunFailure(context.m_failure->toString());
    }

    RunSummary summary;
    summary.smoke = m_options.smoke;
    summary.frameLimit = m_options.frameLimit;
    summary.framesRendered = context.m_frameIndex;
    summary.highSeverityGlMessages = (*device)->debugMessageCount(gl::DebugSeverity::High);
    summary.selfTest = selfTest;
    summary.logicFailed = context.m_failure.has_value();
    summary.logicFailureCode = context.m_failureCode;
    const ExitCode exitCode = exitCodeForCompletedRun(summary);
    if (m_options.smoke) {
        const std::uint64_t provoked = selfTest ? selfTest->highSeverityCaptured : 0;
        const std::string glCheck =
            (*device)->debugOutputEnabled()
                ? fmt::format("{} high-severity GL debug message(s) ({} provoked by --gl-selftest)",
                              summary.highSeverityGlMessages, provoked)
                : std::string("OpenGL errors not checked (GL debug output is off in this build)");
        const std::string verdict = fmt::format("{} of {} frame(s) rendered, {}", summary.framesRendered,
                                                summary.frameLimit.value_or(0), glCheck);
        spdlog::log(exitCode == ExitCode::Ok ? spdlog::level::info : spdlog::level::err, "Smoke test {}: {}",
                    exitCode == ExitCode::Ok ? "passed" : "FAILED", verdict);
        if (exitCode == ExitCode::SmokeCheckFailed && !m_report.failure) {
            m_report.failure = "smoke test failed: " + verdict;
        }
    }
    return exitCode;
}

ExitCode AppHost::failInitialization(std::string_view summary, const Error& error) {
    spdlog::critical("{} {}", summary, error.toString());
    if (!m_report.failure) {
        m_report.failure = fmt::format("{} {}", summary, error.toString());
    }
    if (!m_options.noDialogs) {
        platform::showErrorDialog(m_identity.name, fmt::format("{}\n\n{}", summary, error.toString()));
    }
    return ExitCode::InitializationFailed;
}

void AppHost::reportUnexpectedFailure(std::string_view what) noexcept {
    reportFailure("Unexpected failure", "stopped because of an unexpected error", what);
}

void AppHost::reportRunFailure(std::string_view what) noexcept {
    reportFailure("Run failed", "stopped because of an error", what);
}

void AppHost::reportFailure(std::string_view logPrefix, std::string_view headline, std::string_view what) noexcept {
    try {
        if (!m_report.failure) {
            m_report.failure = fmt::format("{}: {}", logPrefix, what);
        }
        spdlog::critical("{}: {}", logPrefix, what);
        if (!m_options.noDialogs) {
            const std::filesystem::path logFile = core::activeLogFile();
            const std::string logLocation =
                logFile.empty() ? std::string("(file logging disabled)") : core::pathToUtf8(logFile);
            platform::showErrorDialog(m_identity.name, fmt::format("{} {}:\n\n{}\n\nDetails are in the log: {}",
                                                                   m_identity.name, headline, what, logLocation));
        }
    } catch (...) {
        // Reporting must not turn a failure into a crash.
    }
}

Status AppHost::runMainLoop(IAppLogic& logic, AppContext& context) {
    using Clock = std::chrono::steady_clock;
    platform::EventPump events;
    platform::GraphicsWindow& window = context.m_window;
    core::FrameClock& clock = context.m_clock;
    core::CpuTimers& cpuTimers = core::mainThreadCpuTimers();
    const std::string baseTitle = baseWindowTitle();
    double nextTitleRefresh = kTitleRefreshSeconds;
    // The end of the previous frame (its swap), from which the next frame's time is measured; none before the first.
    std::optional<Clock::time_point> previousFrameEnd;

    while (!context.m_quitRequested) {
        events.poll([&](const SDL_Event& event) {
            if (!handleHostEvent(context, event)) {
                logic.onEvent(context, event);
            }
        });
        if (context.m_quitRequested) {
            break;
        }
        // Idle (an emulator has the screen): no update, no render, no swap, no frame. The logic follows its launch after
        // every wait; a wait ends with any event, the wake event among them, or after kIdleWait.
        if (context.m_idle) {
            logic.onIdle(context);
            if (context.m_quitRequested) {
                break;
            }
            if (context.m_idle) {
                events.waitForEvents(kIdleWait);
                continue;
            }
            // Back to rendering: the wait is no frame time (the next delta starts now, for the walker, the frame
            // statistics and the frame times).
            clock.tick();
            if (previousFrameEnd) {
                previousFrameEnd = Clock::now();
            }
        }
        // Nothing is visible while minimized: an interactive run sleeps until something happens instead of spinning on
        // an unthrottled swap. A frame-limited run keeps rendering (presenting to a minimized window is harmless), so
        // that it always reaches its limit and exits.
        if (!m_options.frameLimit && window.isMinimized()) {
            events.waitForEvents(kMinimizedWait);
            if (previousFrameEnd) {
                previousFrameEnd = Clock::now(); // nor is this wait
            }
            continue;
        }

        clock.tick();
        logic.onUpdate(context, m_options.fixedDeltaSeconds.value_or(clock.clampedDeltaSeconds()));
        logic.onRender(context);
        if (auto presented = window.swap(); !presented) {
            return propagate(presented.error());
        }
        const Clock::time_point frameEnd = Clock::now();
        std::optional<double> frameMilliseconds;
        if (previousFrameEnd) {
            frameMilliseconds = std::chrono::duration<double, std::milli>(frameEnd - *previousFrameEnd).count();
        }
        previousFrameEnd = frameEnd;
        m_frameTiming.recordFrame(context.m_frameIndex, frameMilliseconds);
        cpuTimers.endFrame();
        ++context.m_frameIndex;
        // Kept current every frame, so that a report written after an exception escaped the loop counts the frames
        // presented before it.
        context.m_report.framesRendered = context.m_frameIndex;

        if (m_options.frameLimit && context.m_frameIndex >= *m_options.frameLimit) {
            spdlog::info("Rendered {} frame(s), stopping as requested", context.m_frameIndex);
            break;
        }
        if (clock.totalSeconds() >= nextTitleRefresh) {
            const std::string stats =
                fmt::format("{:.0f} FPS | {:.2f} ms", clock.framesPerSecond(), clock.smoothedDeltaSeconds() * 1000.0);
            window.setTitle(fmt::format("{} | {}", baseTitle, stats));
            spdlog::debug("Frame {}: {}", context.m_frameIndex, stats);
            nextTitleRefresh = clock.totalSeconds() + kTitleRefreshSeconds;
        }
    }
    return ok();
}

bool AppHost::handleHostEvent(AppContext& context, const SDL_Event& event) const {
    platform::GraphicsWindow& window = context.m_window;
    if (context.m_wakeEvent != 0 && event.type == context.m_wakeEvent) {
        return true; // it only ends an idle wait
    }
    if (platform::isFullscreenToggle(event)) {
        if (auto toggled = window.toggleBorderlessFullscreen(); !toggled) {
            spdlog::warn("Fullscreen toggle failed: {}", toggled.error().toString());
        } else {
            spdlog::info("Switched to {}", window.isFullscreen() ? "borderless fullscreen" : "windowed mode");
        }
        return true; // Enter is also Confirm in the action map: the logic must not see this press
    }
    switch (event.type) {
    case SDL_EVENT_QUIT:
        spdlog::info("Quit requested");
        context.requestQuit();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == window.id()) {
            spdlog::info("Window closed");
            context.requestQuit();
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event.window.windowID == window.id()) {
            context.m_framebufferSize = window.drawableSize();
            spdlog::debug("Framebuffer resized to {}x{}", context.m_framebufferSize.x, context.m_framebufferSize.y);
        }
        break;
    default:
        break;
    }
    return false;
}

} // namespace rv::app
