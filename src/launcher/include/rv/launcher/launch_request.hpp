#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/environment.hpp>

namespace rv::launcher {

/// When a launch session considers the emulator gone (specification §11.4: waitDirectProcess, waitProcessTree,
/// userConfirmReturn).
enum class WaitStrategy : std::uint8_t {
    DirectProcess,     ///< the started process exits (emulators that run in the process Retroverse starts)
    ProcessTree,       ///< every process of its tree has exited (wrappers and launchers that start the emulator and
                       ///< exit; requires the tree to be followed, a job object)
    UserConfirmReturn, ///< the user confirms the return; the started process's exit is recorded, not authoritative
                       ///< (launcher stubs whose emulator runs outside the tree, or a tree that cannot be followed)
};

/// "DirectProcess", "ProcessTree", "UserConfirmReturn".
[[nodiscard]] std::string_view toString(WaitStrategy strategy) noexcept;
/// Accepts the names of toString() and the short forms "direct", "tree", "confirm".
[[nodiscard]] std::optional<WaitStrategy> parseWaitStrategy(std::string_view text) noexcept;

/// Timeouts of a launch session (specification §11.4: reasonable timeouts to detect errors). None of them ever ends an
/// emulator: Retroverse never terminates one without the user's confirmation.
struct LaunchTimeouts {
    /// A process that ends sooner than this after it started did not really start: an unclean exit is a failure to
    /// start, a clean one is reported as a quick exit (a launcher stub, or a wrapper waited for as a direct process).
    /// Zero disables the rule.
    std::chrono::milliseconds startupGrace{2000};
    /// No window of the process tree visible for this long while its processes still run, at the start or after the
    /// last window closed: the session becomes StillRunning, so that the user can be offered the return (the emulator
    /// may still be loading, or a helper process outlived it). A window shown again resumes the session. Zero disables
    /// the rule.
    std::chrono::milliseconds windowTimeout{30000};
};

/// One launch of an external program, as validated tokens (specification §11.2-11.4): never a shell string.
struct LaunchRequest {
    /// Absolute path of the program, a .exe or .com file.
    std::filesystem::path executable;
    /// argv[1..] in UTF-8, each element exactly one argument of the started program.
    std::vector<std::string> arguments;
    /// Current directory of the started program; empty: the executable's directory.
    std::filesystem::path workingDirectory;
    /// Changes to the environment Retroverse passes on (empty: the program inherits it unchanged).
    std::vector<EnvironmentOverride> environment;
    WaitStrategy waitStrategy = WaitStrategy::DirectProcess;
    LaunchTimeouts timeouts;
    /// Empty: output not captured. Otherwise the program's standard output and error are appended to this file (its
    /// directory is created when missing).
    std::filesystem::path outputLog;
    /// Console programs run without a console window (CREATE_NO_WINDOW, what ES-DE's %HIDEWINDOW% does); no effect on
    /// windowed programs.
    bool hideConsoleWindow = false;
    /// Names the launch in logs and reports ("Mole Mania with RetroArch (gambatte)").
    std::string label;
};

/// Checks a request before anything starts (specification §11.4, §19.1: untrusted inputs, no shell). Errors name the
/// offending field in their context:
///  - executable: an absolute path to an existing regular file whose extension is .exe or .com (NotFound when missing;
///    Unsupported for other types: batch files would run through cmd.exe, which parses arguments differently from
///    CommandLineToArgvW and expands % and ^, and shortcuts or documents need the shell), holding a Windows program for
///    x64, x86 or ARM64 (a PE image: Unsupported for DOS and 16-bit programs, which 64-bit Windows refuses with a modal
///    message box, and for files that are not programs);
///  - working directory: an existing directory when given (NotFound);
///  - arguments: the command line built by commandLineOf() (no NUL, valid UTF-8, within the Windows length limit);
///  - environment: every override passes validateOverride();
///  - output log: an absolute path when given.
/// Paths must be representable in UTF-8.
[[nodiscard]] Status validateLaunchRequest(const LaunchRequest& request);

/// The command line of a request: buildCommandLine(executable, arguments), in UTF-8.
[[nodiscard]] Expected<std::string> commandLineOf(const LaunchRequest& request);

/// The directory a request runs in: the given one, else the executable's directory.
[[nodiscard]] std::filesystem::path workingDirectoryOf(const LaunchRequest& request);

/// The strategy a session can honor once the process runs: ProcessTree needs its tree followed (a job object); without
/// it the session waits for the started process and then for the user's confirmation (UserConfirmReturn), the fallback
/// of the roadmap. The other strategies need nothing.
[[nodiscard]] WaitStrategy effectiveStrategy(WaitStrategy requested, bool treeFollowed) noexcept;

} // namespace rv::launcher
