#include <rv/app/app_host.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
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

void warnAboutPendingFeatures(const CommandLineOptions& options) {
    if (options.scene) {
        spdlog::warn("--scene is not implemented yet; ignoring '{}'", *options.scene);
    }
    if (options.screenshot) {
        spdlog::warn("--screenshot is not implemented yet; ignoring '{}'", core::pathToUtf8(*options.screenshot));
    }
    if (options.model) {
        spdlog::warn("--model is not implemented yet; ignoring '{}'", core::pathToUtf8(*options.model));
    }
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
    warnAboutPendingFeatures(m_options);

    // --- SDL, window, OpenGL context, GL device ------------------------------------------------------------------------
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

    // --- Application logic --------------------------------------------------------------------------------------------
    core::FrameClock clock;
    AppContext context(m_options, *paths, *window, **device, clock);
    context.m_framebufferSize = window->drawableSize();

    LogicShutdownGuard logicScope(logic, context);
    if (auto initialized = logic.onInit(context); !initialized) {
        return failInitialization("Retroverse could not initialize its renderer.", initialized.error());
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

    RunSummary summary;
    summary.smoke = m_options.smoke;
    summary.highSeverityGlMessages = (*device)->debugMessageCount(gl::DebugSeverity::High);
    summary.selfTest = selfTest;
    const ExitCode exitCode = exitCodeForCompletedRun(summary);
    if (m_options.smoke) {
        const std::uint64_t provoked = selfTest ? selfTest->highSeverityCaptured : 0;
        spdlog::log(exitCode == ExitCode::Ok ? spdlog::level::info : spdlog::level::err,
                    "Smoke test {}: {} frame(s), {} high-severity GL debug message(s) ({} provoked by --gl-selftest)",
                    exitCode == ExitCode::Ok ? "passed" : "FAILED", context.m_frameIndex,
                    summary.highSeverityGlMessages, provoked);
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
    try {
        spdlog::critical("Unexpected failure: {}", what);
        if (!m_options.noDialogs) {
            const std::filesystem::path logFile = core::activeLogFile();
            platform::showErrorDialog(
                kDialogTitle,
                fmt::format("Retroverse stopped because of an unexpected error:\n\n{}\n\nDetails are in the log: {}",
                            what, logFile.empty() ? std::string("(file logging disabled)") : core::pathToUtf8(logFile)));
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
            handleHostEvent(context, event);
            logic.onEvent(context, event);
        });
        if (context.m_quitRequested) {
            break;
        }
        if (window.isMinimized()) {
            // Nothing is visible: sleep until something happens instead of spinning on an unthrottled swap.
            events.waitForEvents(kMinimizedWait);
            continue;
        }

        clock.tick();
        logic.onUpdate(context, clock.clampedDeltaSeconds());
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

void AppHost::handleHostEvent(AppContext& context, const SDL_Event& event) const {
    platform::GraphicsWindow& window = context.m_window;
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
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            break;
        }
        if (event.key.key == SDLK_ESCAPE) {
            spdlog::info("Escape pressed, quitting");
            context.requestQuit();
        } else if ((event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) &&
                   (event.key.mod & SDL_KMOD_ALT) != 0) {
            if (auto toggled = window.toggleBorderlessFullscreen(); !toggled) {
                spdlog::warn("Fullscreen toggle failed: {}", toggled.error().toString());
            } else {
                spdlog::info("Switched to {}", window.isFullscreen() ? "borderless fullscreen" : "windowed mode");
            }
        }
        break;
    default:
        break;
    }
}

} // namespace rv::app
