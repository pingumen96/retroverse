#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::smoke {

/// A process to run to completion.
struct ProcessRequest {
    std::filesystem::path executable;
    /// UTF-8 arguments, quoted for the Windows command line as needed.
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    /// Receives the standard output and error of the process (created or truncated; parent directories must exist).
    std::filesystem::path outputFile;
    /// Environment variables of this process left out of the child's environment (names compared ignoring case).
    std::vector<std::string> removedVariables;
    /// Longest wait; a process still running then is terminated.
    std::chrono::milliseconds timeout{std::chrono::minutes(4)};
};

/// How the process ended.
struct ProcessOutcome {
    /// The exit code; meaningless when timedOut.
    int exitCode = -1;
    /// The process was still running at the timeout and was terminated.
    bool timedOut = false;
    std::chrono::milliseconds elapsed{0};
};

/// Runs the request's process with its standard input on NUL, waits for it to exit or the timeout to pass, and
/// terminates it then, so no process is ever left behind. Errors: the output file or the process cannot be created
/// (IoError, PlatformError).
[[nodiscard]] Expected<ProcessOutcome> runProcess(const ProcessRequest& request);

/// `argument` quoted for a Windows command line so that CommandLineToArgvW (and the C runtime) read it back unchanged:
/// as it is when it holds no space, tab, newline or quote, else in quotes with its quotes and the backslashes before
/// them escaped.
[[nodiscard]] std::wstring quoteArgument(std::wstring_view argument);

} // namespace rv::smoke
