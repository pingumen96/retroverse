#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <optional>
#include <string_view>

namespace rv::app {

/// Process exit codes of retroverse.exe.
enum class ExitCode : int {
    Ok = 0,
    /// A --smoke run stopped before rendering all its frames, recorded a High-severity GL debug message that
    /// --gl-selftest did not provoke, or the self-test failed (see exitCodeForCompletedRun).
    SmokeCheckFailed = 1,
    /// Paths, SDL, window, OpenGL 4.5 context, GL device or application initialization failed: logged, and shown in a
    /// message box unless dialogs are off (CommandLineOptions::noDialogs). An invalid command line also exits with
    /// this code; it is printed to stderr with the usage text, before logging starts and without a dialog.
    InitializationFailed = 2,
    /// Failure after initialization: an unexpected one (exception, failed presentation) or one the application logic
    /// reported because it must not pass silently (AppContext::failRun: a --screenshot that could not be written, a
    /// frame-limited run whose scene shader never built). Logged, shown in a message box unless dialogs are off, never
    /// propagated.
    RuntimeFailure = 3,
};

[[nodiscard]] constexpr int toInt(ExitCode code) noexcept {
    return static_cast<int>(code);
}

[[nodiscard]] std::string_view toString(ExitCode code) noexcept;

/// Outcome of --gl-selftest, which issues one deliberately invalid GL call (glEnable(0xFFFF)).
struct SelfTestOutcome {
    bool errorFlagRaised = false;           ///< glGetError() reported the provoked GL_INVALID_ENUM
    bool debugOutputActive = false;         ///< the GL debug callback was installed (RV_GL_DEBUG builds)
    std::uint64_t messagesCaptured = 0;     ///< debug messages of any severity delivered for the invalid call
    std::uint64_t highSeverityCaptured = 0; ///< the High-severity part of messagesCaptured

    /// The error path works: the error was flagged and, when debug output is active, the callback reported it.
    [[nodiscard]] bool passed() const noexcept;
};

/// What a finished run observed, as input to exitCodeForCompletedRun().
struct RunSummary {
    bool smoke = false;
    /// Frames the run was asked to render before exiting (CommandLineOptions::frameLimit); empty when unlimited.
    std::optional<std::uint32_t> frameLimit;
    /// Frames actually presented: fewer than frameLimit when a quit request (Escape, window close, Ctrl+C in the
    /// console) ended the run early.
    std::uint64_t framesRendered = 0;
    /// High-severity GL debug messages over the whole run, the self-test's included.
    std::uint64_t highSeverityGlMessages = 0;
    std::optional<SelfTestOutcome> selfTest;
    /// The application logic reported a failure that ended the run (AppContext::failRun).
    bool logicFailed = false;
};

/// Exit code of a run that initialized and left its main loop normally.
///
/// A failure reported by the application logic gives ExitCode::RuntimeFailure, whatever the kind of run. Otherwise
/// only --smoke runs can fail (ExitCode::SmokeCheckFailed), when:
///  - fewer than frameLimit frames were rendered (the run was interrupted, so it proves nothing), or
///  - High-severity messages exceed the ones the self-test provoked itself (so `--smoke --gl-selftest` exits 0 when
///    the provoked GL_INVALID_ENUM was the only one), or
///  - the self-test ran and failed (error not flagged, or debug output active but the callback stayed silent).
/// Interactive and --exit-after-frames runs only log GL messages and return ExitCode::Ok, also when the user quits
/// before the frame limit.
[[nodiscard]] ExitCode exitCodeForCompletedRun(const RunSummary& summary) noexcept;

} // namespace rv::app
