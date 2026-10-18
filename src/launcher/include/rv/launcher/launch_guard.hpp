#pragma once

// The automation guard (ADR-0008 §8): while RV_LAUNCH_ALLOWLIST is set (gate scripts, test harnesses, agents), only the
// executables it lists may start, RetroArch only isolated by an appended configuration, DuckStation never, so that an
// automated run cannot reach a real emulator's settings and saves by mistake. Unset, the guard does nothing.
//
// Where it applies: every IProcessRunner implementation checks a request with checkLaunchGuard() before it creates a
// process (Win32ProcessRunner::start does), so LaunchSession refuses it as FailedToStart (StartFailed) with the
// guard's message and nothing starts. Code that starts a LaunchRequest without a runner must call checkLaunchGuard()
// first; dry runs may call it to say what an automated run would refuse. It is a safety net for automation, not a
// security boundary: code that creates processes by other means is not covered.
//
// RV_LAUNCH_ALLOWLIST holds absolute executable paths separated by ';' (blanks and one pair of double quotes around an
// entry are ignored; relative entries and device paths are ignored and allow nothing). A request is allowed when all of
// these hold, in this order:
//  - its executable is an absolute path without an alternate data stream, a device or \\?\GLOBALROOT form;
//  - its file name, as Windows normalizes the path (trailing dots and spaces dropped) and as the file system resolves
//    it (8.3 short names expanded, symbolic links and junctions followed), does not match duckstation*.exe;
//  - the file exists and can be resolved (opened for its attributes, its file id read);
//  - when either name matches retroarch*.exe, it is isolated: its command line read as RetroArch 1.21.0 reads it,
//    conservatively (libretro-common's compat getopt, which its Windows builds use), appends configuration files it
//    can open:
//     - the command line reaches it as written: RetroArch's main() reads the ANSI code page's form of it, made with
//       best-fit characters, so the executable's path and every argument must be held exactly by that code page;
//     - every argument that may select --appendconfig in any reading (its name after "--", or after "--" and any '='
//       as RetroArch reads it, is appendconfig, an abbreviation of it or a name holding it), wherever it stands, is
//       "--appendconfig=<files>" or "--appendconfig <files>" spelled out (options are case-sensitive);
//     - read as RetroArch reads them (every option wherever it stands, long options by exact name, values after '=' or
//       in the next argument, grouped short options from its option string), the arguments hold no option it does not
//       know ("--" and abbreviations included), no lone "-", no grouped short options running into a long one
//       ("-v-appendconfig"), no option without its value, and at least one --appendconfig, which RetroArch reads last
//       wins;
//     - the '|'-separated files of every --appendconfig are files RetroArch opens: absolute drive or UNC paths in ASCII
//       (it opens the files of its ANSI arguments as UTF-8 text; any text when the ANSI code page is UTF-8), shorter
//       than 260 characters (it is not long-path aware), existing; the value at most 4095 bytes (it keeps no more). A
//       file it cannot open is only logged by RetroArch, which then runs on its own configuration;
//  - the file it resolves to is the one an entry of the list resolves to: the same final path (compared with the
//    file-system casing) and the same file (volume and file id), so that two files that a folder with case-sensitive
//    names tells apart stay apart; an entry that cannot be opened allows nothing.
// Hard links and copies under another name are not recognized by name: only the list keeps them from starting.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/launch_request.hpp>

namespace rv::launcher {

/// The environment variable that activates the guard.
inline constexpr std::string_view kLaunchAllowListVariable = "RV_LAUNCH_ALLOWLIST";

/// The rule that decided a request.
enum class GuardRule : std::uint8_t {
    Inactive,             ///< allowed: RV_LAUNCH_ALLOWLIST is not set
    Allowed,              ///< allowed: listed (and, for RetroArch, isolated)
    UnsupportedPath,      ///< refused: a relative executable path, an alternate data stream, a device path
    DuckStation,          ///< refused: duckstation*.exe, listed or not
    Unresolved,           ///< refused: the executable cannot be opened to see which file it is
    RetroArchNotIsolated, ///< refused: retroarch*.exe whose command line is not surely isolated by --appendconfig
                          ///< (the verdict's message says why)
    NotListed,            ///< refused: not one of the listed executables
};

/// "inactive", "allowed", "unsupported path", "DuckStation", "unresolved", "RetroArch not isolated", "not listed".
[[nodiscard]] std::string_view toString(GuardRule rule) noexcept;

/// The guard's decision on one request.
struct GuardVerdict {
    GuardRule rule = GuardRule::Inactive;
    /// Why it was refused (naming the executable); empty when allowed.
    std::string message;

    [[nodiscard]] bool allowed() const noexcept { return rule == GuardRule::Inactive || rule == GuardRule::Allowed; }
};

/// The guard of one allow-list. Default-constructed: inactive, every request allowed.
class LaunchGuard {
public:
    LaunchGuard() = default;

    /// An active guard listing the entries of `allowList` (RV_LAUNCH_ALLOWLIST's syntax). An empty or blank list is
    /// active and allows nothing.
    [[nodiscard]] static LaunchGuard fromAllowList(std::string_view allowList);
    /// The guard of this process now: inactive when RV_LAUNCH_ALLOWLIST is not set, fromAllowList() of its value when
    /// it is (also when empty).
    [[nodiscard]] static LaunchGuard fromEnvironment();

    [[nodiscard]] bool active() const noexcept { return m_active; }
    /// The entries that can allow something: absolute paths, as written.
    [[nodiscard]] std::span<const std::filesystem::path> entries() const noexcept { return m_entries; }
    /// The entries left out (relative, device paths, streams, not UTF-8), as written.
    [[nodiscard]] std::span<const std::string> ignoredEntries() const noexcept { return m_ignored; }

    /// Decides `request` (its executable and arguments) by the rules above. The file system is read (the executable,
    /// the listed files, the appended configurations), never written.
    [[nodiscard]] GuardVerdict check(const LaunchRequest& request) const;

private:
    bool m_active = false;
    std::vector<std::filesystem::path> m_entries;
    std::vector<std::string> m_ignored;
};

/// LaunchGuard::fromEnvironment().check(request) as a Status: a refusal is ValidationFailed with the verdict's message,
/// context RV_LAUNCH_ALLOWLIST.
[[nodiscard]] Status checkLaunchGuard(const LaunchRequest& request);

/// True when `error` is the guard's refusal as checkLaunchGuard() reports it (ValidationFailed, context
/// RV_LAUNCH_ALLOWLIST): what a runner returns unchanged when the guard refused the request.
[[nodiscard]] bool isGuardRefusal(const Error& error) noexcept;

} // namespace rv::launcher
