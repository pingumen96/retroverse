#pragma once

namespace rv::app {

/// Application entry point called by main(). Returns the process exit code.
/// `argv` is UTF-8 encoded.
[[nodiscard]] int run(int argc, char** argv);

} // namespace rv::app
