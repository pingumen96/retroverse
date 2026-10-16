#pragma once

// Runs a program to completion for a test: its arguments quoted by rv::launcher::buildCommandLine (what the product
// does, so CommandLineToArgvW and the C runtime give them back unchanged), its standard output and standard error
// written to files of the test's choosing and read back, apart or together. The smoke tests run the executables of
// their build with it; unit tests run retroverse-cli and the fake emulator.

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/environment.hpp>

namespace rv::test {

/// A program to run to completion.
struct ChildRequest {
    std::filesystem::path executable;
    /// UTF-8 arguments (argv[1] on), passed unchanged.
    std::vector<std::string> arguments;
    /// Empty: this process's current directory.
    std::filesystem::path workingDirectory;
    /// Changes to this process's environment for the child (launcher::mergeEnvironment): a value sets a variable, none
    /// removes it ({"RV_ASSET_ROOT", std::nullopt}).
    std::vector<launcher::EnvironmentOverride> environment;
    /// The files that receive the standard output and the standard error, created or truncated (their folders must
    /// exist). The same path for both: one file holding both streams in the order they were written.
    std::filesystem::path stdoutFile;
    std::filesystem::path stderrFile;
    /// Longest wait: a process still running then is terminated (the process started, not the ones it started).
    std::chrono::milliseconds timeout{std::chrono::minutes(4)};
};

/// How the program ended and what it wrote.
struct ChildResult {
    /// The exit code; meaningless when timedOut.
    int exitCode = -1;
    /// Still running at the timeout: terminated.
    bool timedOut = false;
    std::chrono::milliseconds elapsed{0};
    /// The standard output file's content after the exit.
    std::string standardOutput;
    /// The standard error file's (the same text as standardOutput when both went to one file).
    std::string standardError;
};

/// Runs `request` with its standard input on NUL and without a console window (CREATE_NO_WINDOW), only the three
/// standard handles inherited, waits for the exit or the timeout and reads the outputs back. Errors: an argument
/// buildCommandLine refuses (ValidationFailed), no output file named (InvalidArgument), an output file or the process
/// that cannot be created (IoError, PlatformError), an output that cannot be read back (IoError, OutOfRange beyond
/// 64 MiB).
[[nodiscard]] Expected<ChildResult> runChild(const ChildRequest& request);

} // namespace rv::test
