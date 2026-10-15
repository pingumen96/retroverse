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
#include <rv/render/gl_device.hpp>

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

constexpr std::string_view kAppIdentifier = "io.github.pingumen96.retroverse";
constexpr std::string_view kDialogTitle = "Retroverse";
constexpr std::string_view kOpenGlRequired =
    "Retroverse requires OpenGL 4.5 (core profile). Update the graphics driver, or make sure Windows runs "
    "Retroverse on a GPU that supports OpenGL 4.5.";
constexpr double kTitleRefreshSeconds = 1.0;
constexpr std::chrono::milliseconds kMinimizedWait{100};

std::string baseWindowTitle() {
    return fmt::format("{} {}", core::kProductName, core::kVersion);
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

void logStartupBanner(const core::Paths& paths, const CommandLineOptions& options) {
    spdlog::info("{}", core::versionBanner());
    spdlog::info("Executable: {}", core::pathToUtf8(core::executablePath()));
    spdlog::info("Asset root: {} ({})", core::pathToUtf8(paths.assetRoot()), core::toString(paths.assetRootSource()));
    spdlog::info("User data: {}{}", core::pathToUtf8(paths.userDataRoot()), paths.isPortable() ? " (portable)" : "");
    const std::filesystem::path logFile = core::activeLogFile();
    spdlog::info("Log file: {}", logFile.empty() ? std::string("disabled") : core::pathToUtf8(logFile));
    spdlog::info("Build: GL debug output {}, developer UI {}", kGlDebug ? "on" : "off", kDevUi ? "on" : "off");
    spdlog::info("Run mode: {}", describeRunMode(options));
}

platform::WindowDesc makeWindowDesc(const CommandLineOptions& options) {
    platform::WindowDesc desc;
    desc.title = baseWindowTitle();
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

AppHost::AppHost(CommandLineOptions options) : m_options(std::move(options)) {}

int AppHost::run(IAppLogic& logic) noexcept {
    ExitCode exitCode = ExitCode::RuntimeFailure;
    try {
        exitCode = runUnchecked(logic);
    } catch (const std::exception& e) {
        reportUnexpectedFailure(e.what());
    } catch (...) {
        reportUnexpectedFailure("unknown exception");
    }
    try {
        spdlog::info("Exiting with code {} ({})", toInt(exitCode), toString(exitCode));
        core::shutdownLogging();
    } catch (...) {
        // The exit code matters more than the last log line.
    }
    return toInt(exitCode);
}

ExitCode AppHost::runUnchecked(IAppLogic& logic) {
    core::markMainThread();

    // --- Directories and logging (§16.1) -----------------------------------------------------------------------------
    core::PathOptions pathOptions;
    pathOptions.assetRootOverride = m_options.assetRoot;
    pathOptions.userDataOverride = m_options.dataDir;
    pathOptions.portable = m_options.portable;
    const auto paths = core::Paths::resolve(pathOptions, core::PathEnvironment::fromProcess());

    core::LogConfig logConfig;
    logConfig.level = m_options.logLevel;
    if (paths) {
        logConfig.directory = paths->logsDir();
    }
    core::initLogging(logConfig);
    if (!paths) {
        return failInitialization("Retroverse could not locate its directories.", paths.error());
    }
    logStartupBanner(*paths, m_options);

    // --- SDL, window, OpenGL context, GL device ----------------------------------------------------------------------
    const platform::AppMetadata metadata{std::string(core::kProductName), std::string(core::kVersion),
                                         std::string(kAppIdentifier)};
    const auto sdl = platform::SdlPlatform::init(metadata);
    if (!sdl) {
        return failInitialization("Retroverse could not initialize SDL.", sdl.error());
    }
    spdlog::info("{}, video driver: {}", platform::SdlPlatform::versionString(), platform::SdlPlatform::videoDriver());

    auto window = platform::createGraphicsWindow(makeWindowDesc(m_options), makeContextDesc(m_options));
    if (!window) {
        return failInitialization(window.error().code == ErrorCode::GraphicsError
                                      ? kOpenGlRequired
                                      : std::string_view("Retroverse could not create its window."),
                                  window.error());
    }
    logWindow(*window);

    const auto device = gl::GLDevice::create(window->glLoader(), gl::GLDeviceConfig{.debugOutput = kGlDebug});
    if (!device) {
        return failInitialization(kOpenGlRequired, device.error());
    }

    // --- Application logic -------------------------------------------------------------------------------------------
    core::FrameClock clock;
    AppContext context(m_options, *paths, *window, **device, clock);
    context.m_framebufferSize = window->drawableSize();

    LogicShutdownGuard logicScope(logic, context);
    if (auto initialized = logic.onInit(context); !initialized) {
        return failInitialization("Retroverse could not load its content.", initialized.error());
    }
    std::optional<SelfTestOutcome> selfTest;
    if (m_options.glSelfTest) {
        selfTest = runGlSelfTest(**device);
    }

    if (auto loop = runMainLoop(logic, context); !loop) {
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
    const ExitCode exitCode = exitCodeForCompletedRun(summary);
    if (m_options.smoke) {
        const std::uint64_t provoked = selfTest ? selfTest->highSeverityCaptured : 0;
        const std::string glCheck =
            (*device)->debugOutputEnabled()
                ? fmt::format("{} high-severity GL debug message(s) ({} provoked by --gl-selftest)",
                              summary.highSeverityGlMessages, provoked)
                : std::string("OpenGL errors not checked (GL debug output is off in this build)");
        spdlog::log(exitCode == ExitCode::Ok ? spdlog::level::info : spdlog::level::err,
                    "Smoke test {}: {} of {} frame(s) rendered, {}", exitCode == ExitCode::Ok ? "passed" : "FAILED",
                    summary.framesRendered, summary.frameLimit.value_or(0), glCheck);
    }
    return exitCode;
}

ExitCode AppHost::failInitialization(std::string_view summary, const Error& error) const {
    spdlog::critical("{} {}", summary, error.toString());
    if (!m_options.noDialogs) {
        platform::showErrorDialog(kDialogTitle, fmt::format("{}\n\n{}", summary, error.toString()));
    }
    return ExitCode::InitializationFailed;
}

void AppHost::reportUnexpectedFailure(std::string_view what) const noexcept {
    reportFailure("Unexpected failure", "Retroverse stopped because of an unexpected error", what);
}

void AppHost::reportRunFailure(std::string_view what) const noexcept {
    reportFailure("Run failed", "Retroverse stopped because of an error", what);
}

void AppHost::reportFailure(std::string_view logPrefix, std::string_view headline,
                            std::string_view what) const noexcept {
    try {
        spdlog::critical("{}: {}", logPrefix, what);
        if (!m_options.noDialogs) {
            const std::filesystem::path logFile = core::activeLogFile();
            const std::string logLocation =
                logFile.empty() ? std::string("(file logging disabled)") : core::pathToUtf8(logFile);
            platform::showErrorDialog(
                kDialogTitle, fmt::format("{}:\n\n{}\n\nDetails are in the log: {}", headline, what, logLocation));
        }
    } catch (...) {
        // Reporting must not turn a failure into a crash.
    }
}

Status AppHost::runMainLoop(IAppLogic& logic, AppContext& context) const {
    platform::EventPump events;
    platform::GraphicsWindow& window = context.m_window;
    core::FrameClock& clock = context.m_clock;
    const std::string baseTitle = baseWindowTitle();
    double nextTitleRefresh = kTitleRefreshSeconds;

    while (!context.m_quitRequested) {
        events.poll([&](const SDL_Event& event) {
            if (!handleHostEvent(context, event)) {
                logic.onEvent(context, event);
            }
        });
        if (context.m_quitRequested) {
            break;
        }
        // Nothing is visible while minimized: an interactive run sleeps until something happens instead of spinning on
        // an unthrottled swap. A frame-limited run keeps rendering (presenting to a minimized window is harmless), so
        // that it always reaches its limit and exits.
        if (!m_options.frameLimit && window.isMinimized()) {
            events.waitForEvents(kMinimizedWait);
            continue;
        }

        clock.tick();
        logic.onUpdate(context, m_options.fixedDeltaSeconds.value_or(clock.clampedDeltaSeconds()));
        logic.onRender(context);
        if (auto presented = window.swap(); !presented) {
            return propagate(presented.error());
        }
        ++context.m_frameIndex;

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
