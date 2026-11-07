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
#include <rv/launcher/window_choice.hpp>

namespace rv::launcher {

/// When a launch session considers the emulator gone (specification §11.4: waitDirectProcess, waitProcessTree,
/// userConfirmReturn).
enum class WaitStrategy : std::uint8_t {
    DirectProcess,     ///< the started process exits (emulators that run in the process Retroverse starts)
    ProcessTree,       ///< every process of its tree has exited (wrappers and launchers that start the emulator and
                       ///< exit; requires the tree to be followed, a job object)
    UserConfirmReturn, ///< the user confirms the return; the started process's exit is recorded, not authoritative
                       ///< (launcher stubs whose emulator runs outside the tree, or a tree that cannot be followed)
    /// The game's window followed wherever it opens (I11 step 13, ADR-0020 §4: a protocol activation, ES-DE's
    /// %RUNINBACKGROUND%): the tree's windows and those of processes started since the launch by the tree, by
    /// Retroverse or by the request's launcherProcesses; once one has shown, the session ends when all of them have
    /// exited. Without a window it waits for one, or for the user (StillRunning after the window timeout).
    FollowWindow,
};

/// "DirectProcess", "ProcessTree", "UserConfirmReturn", "FollowWindow".
[[nodiscard]] std::string_view toString(WaitStrategy strategy) noexcept;
/// Accepts the names of toString() and the short forms "direct", "tree", "confirm", "follow".
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
    /// A console program always gets a console of its own, never Retroverse's (whose closing would end it): with a
    /// window, as when ES-DE starts it, or without one when true (CREATE_NO_WINDOW, what ES-DE's %HIDEWINDOW% does).
    /// No effect on windowed (GUI) programs.
    bool hideConsoleWindow = false;
    /// Every argument in double quotes, whatever it holds (ArgumentQuoting::Always): for a program that splits an
    /// unquoted argument its own way (a manual's explorer.exe, I5 step 8's review).
    bool quoteEveryArgument = false;
    /// Names the launch in logs and reports ("Mole Mania with RetroArch (gambatte)").
    std::string label;
    /// How the game's window is chosen among the tree's (I11 step 11, ADR-0020 §2); an emulator profile's VR rules
    /// add to the defaults (I11 step 12).
    WindowRules windowRules;
    /// I11 step 13 (ADR-0020 §3): a protocol activation instead of a program ("steam://rungameid/400"), opened by the
    /// shell as one value (ShellExecuteExW, the verb "open"), never a command line. The executable, the arguments and
    /// the environment stay empty; the session follows the game's window (WaitStrategy::FollowWindow). Accepted as
    /// checkActivationUrl() says; never in an automated run (launch_guard.hpp).
    std::string activationUrl;
    /// FollowWindow: the file names of a launcher's own processes ("steam.exe"), compared ignoring case: their windows
    /// are never the game's, and the processes they start since the launch may be.
    std::vector<std::string> launcherProcesses;

    [[nodiscard]] bool isActivation() const noexcept { return !activationUrl.empty(); }
};

/// The longest activation URL accepted.
inline constexpr std::size_t kMaxActivationUrlLength = 2048;

/// The scheme of `url` in lower case ("steam" for "steam://run/1"): letters, digits, '+', '-' and '.' after a first
/// letter, up to the first ':'; nothing when it has none.
[[nodiscard]] std::optional<std::string> activationScheme(std::string_view url);

/// Whether `url` may be activated: valid UTF-8 of at most kMaxActivationUrlLength bytes, no control character, blank
/// or double quote, a scheme (activationScheme()) followed by something, and not a scheme that opens documents,
/// folders, web pages, searches or Windows' own pages, whatever list allows it: about, data, file, ftp, hcp, http,
/// https, javascript, ldap, mailto, mk, ms-* and microsoft-*, res, search, search-ms, shell, telnet, vbscript.
/// Errors: ValidationFailed, the context "activation URL".
[[nodiscard]] Status checkActivationUrl(std::string_view url);

/// Checks a request before anything starts (specification §11.4, §19.1: untrusted inputs, no shell). An activation
/// (LaunchRequest::activationUrl) passes checkActivationUrl() and names no executable, argument, working directory,
/// environment or output log. Errors name the offending field in their context:
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

/// The command line of a request: buildCommandLine(executable, arguments), every argument quoted when
/// quoteEveryArgument, in UTF-8; an activation's URL.
[[nodiscard]] Expected<std::string> commandLineOf(const LaunchRequest& request);

/// The directory a request runs in: the given one, else the executable's directory (empty for an activation).
[[nodiscard]] std::filesystem::path workingDirectoryOf(const LaunchRequest& request);

/// The strategy a session can honor once the process runs: ProcessTree needs its tree followed (a job object); without
/// it the session waits for the started process and then for the user's confirmation (UserConfirmReturn), the fallback
/// of the roadmap. The other strategies need nothing (FollowWindow follows new processes by their windows).
[[nodiscard]] WaitStrategy effectiveStrategy(WaitStrategy requested, bool treeFollowed) noexcept;

} // namespace rv::launcher
