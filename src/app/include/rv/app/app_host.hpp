#pragma once

#include <string_view>

#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/core/expected.hpp>

union SDL_Event;

namespace rv::app {

class AppContext;
class IAppLogic;

/// Owns a run of the application (specification §15.3, §15.5 Boot -> active -> Shutdown): resolves the directories,
/// configures logging (<user data>/logs/retroverse.log), initializes SDL, opens the window with an OpenGL 4.5 core
/// context, creates the GL device (debug output in RV_GL_DEBUG builds) and drives an IAppLogic through the main loop.
///
/// The host handles quit requests, window close, resizes (framebuffer size), Escape (quit) and Alt+Enter (borderless
/// fullscreen), refreshes the window title with FPS and frame time once per second, and stops after
/// CommandLineOptions::frameLimit frames in --smoke / --exit-after-frames runs. Exit codes: see ExitCode.
class AppHost {
public:
    explicit AppHost(CommandLineOptions options);

    /// Runs `logic` to completion on the calling (main) thread and returns the process exit code. Never throws:
    /// unexpected exceptions are logged and reported as ExitCode::RuntimeFailure.
    [[nodiscard]] int run(IAppLogic& logic) noexcept;

private:
    [[nodiscard]] ExitCode runUnchecked(IAppLogic& logic);
    /// Logs the failure and shows it in a message box unless dialogs are disabled.
    [[nodiscard]] ExitCode failInitialization(std::string_view summary, const Error& error) const;
    /// Logs an unexpected failure and, unless dialogs are disabled, tells the user where the log is.
    void reportUnexpectedFailure(std::string_view what) const noexcept;
    /// Runs until quit or the frame limit; an error means presentation failed.
    [[nodiscard]] Status runMainLoop(IAppLogic& logic, AppContext& context) const;
    void handleHostEvent(AppContext& context, const SDL_Event& event) const;

    CommandLineOptions m_options;
};

} // namespace rv::app
