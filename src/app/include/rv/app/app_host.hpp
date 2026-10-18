#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/version.hpp>

union SDL_Event;

namespace rv::app {

class AppContext;
class IAppLogic;

/// How an application built on AppHost presents itself. The defaults are retroverse.exe's.
struct AppIdentity {
    /// Shown in the window title (followed by the version and, once per second, the frame rate) and as the title of
    /// message boxes.
    std::string_view name = core::kProductName;
    /// SDL application identifier, reverse domain notation.
    std::string_view identifier = "io.github.pingumen96.retroverse";
    /// The log file under <user data>/logs.
    std::string_view logFileName = "retroverse.log";
};

/// Owns a run of the application (specification §15.3, §15.5 Boot -> active -> Shutdown): resolves the directories,
/// configures logging (<user data>/logs/<AppIdentity::logFileName>), initializes SDL, opens the window with an OpenGL
/// 4.5 core context, creates the GL device (debug output in RV_GL_DEBUG builds) and drives an IAppLogic through the
/// main loop.
///
/// The host handles quit requests, window close, resizes (framebuffer size) and Alt+Enter (borderless fullscreen; the
/// key press is consumed, so it never reaches the logic's bindings), refreshes the window title with FPS and frame time
/// once per second, passes the frame delta (or the CommandLineOptions::fixedDeltaSeconds step) to the logic, and stops
/// after CommandLineOptions::frameLimit frames in --smoke / --exit-after-frames runs. Every other input, Escape
/// included, is the logic's. Exit codes: see ExitCode.
///
/// With --run-report the host writes the RunReport of the run when it ends, whatever the outcome (an initialization
/// failure included): its own fields, and what the logic added through AppContext::report(). A report that cannot be
/// written is logged and turns a successful run into ExitCode::RuntimeFailure.
///
/// While the logic keeps it idle (AppContext::setIdle: an emulator has the screen), the host renders nothing and waits
/// for events instead, at most kIdleWait at a time, calling IAppLogic::onIdle after each wait; AppContext::wakeCallback
/// ends a wait from any thread. Frames do not advance meanwhile, so a frame-limited run renders its whole --frames
/// whatever the wait, and the wait is no frame time. Quit requests and window close still end the run (the logic's
/// onShutdown follows, as always).
class AppHost {
public:
    /// The longest idle wait between two IAppLogic::onIdle calls when nothing wakes the host: how often the logic
    /// follows a launch's timeouts.
    static constexpr std::chrono::milliseconds kIdleWait{100};

    explicit AppHost(CommandLineOptions options, AppIdentity identity = {});

    /// Runs `logic` to completion on the calling (main) thread and returns the process exit code. Never throws:
    /// unexpected exceptions are logged and reported as ExitCode::RuntimeFailure.
    [[nodiscard]] int run(IAppLogic& logic) noexcept;

private:
    [[nodiscard]] ExitCode runUnchecked(IAppLogic& logic);
    /// Logs the failure, records it in the report and shows it in a message box unless dialogs are disabled.
    [[nodiscard]] ExitCode failInitialization(std::string_view summary, const Error& error);
    /// Logs an unexpected failure, records it in the report and, unless dialogs are disabled, tells the user where the
    /// log is.
    void reportUnexpectedFailure(std::string_view what) noexcept;
    /// Same for a failure the logic reported through AppContext::failRun.
    void reportRunFailure(std::string_view what) noexcept;
    void reportFailure(std::string_view logPrefix, std::string_view headline, std::string_view what) noexcept;
    /// Completes the report with `exitCode` (and the frame times, the phases, the CPU sections and, unless the main loop
    /// recorded it, the GPU memory) and writes it to the --run-report file; returns the exit code of the run,
    /// ExitCode::RuntimeFailure instead of ExitCode::Ok when the report could not be written.
    [[nodiscard]] ExitCode finishReport(ExitCode exitCode) noexcept;
    /// Adds the measurements of the run (FrameTiming, core::mainThreadCpuTimers()) to the report, and logs them once
    /// for a frame-limited run.
    void recordMeasurements() noexcept;
    /// Runs until quit or the frame limit, keeping RunReport::framesRendered current after every frame; an error means
    /// presentation failed. Records each frame's time (FrameTiming) and closes the CPU sections' frame.
    [[nodiscard]] Status runMainLoop(IAppLogic& logic, AppContext& context);
    /// Handles quit, window close, resize and the fullscreen shortcut. Returns true when the event is consumed and must
    /// not reach the logic: the Alt+Enter press (platform::isFullscreenToggle), the host's wake event.
    [[nodiscard]] bool handleHostEvent(AppContext& context, const SDL_Event& event) const;
    /// "<name> <version>".
    [[nodiscard]] std::string baseWindowTitle() const;

    CommandLineOptions m_options;
    AppIdentity m_identity;
    /// The report of the run, filled as it goes.
    RunReport m_report;
    /// The frame times and phases of the run (AppContext::frameTiming).
    FrameTiming m_frameTiming;
    /// RunReport::gpuMemory was recorded when the main loop ended.
    bool m_gpuMemoryRecorded = false;
};

} // namespace rv::app
