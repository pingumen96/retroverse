#pragma once

namespace rv::app {

/// Application entry point called by main(): parses the command line (--help and --version print and return), then
/// runs RetroverseApp through AppHost. Returns the process exit code (rv::app::ExitCode). `argv` is UTF-8 encoded
/// (SDL_main converts the Windows command line). Never throws.
[[nodiscard]] int run(int argc, char** argv);

} // namespace rv::app
