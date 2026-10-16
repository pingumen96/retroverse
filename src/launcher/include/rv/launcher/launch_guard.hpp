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
//  - the file exists and can be resolved (opened for its attributes);
//  - when either name matches retroarch*.exe, it is isolated: every argument that spells --appendconfig or abbreviates
//    it is "--appendconfig=<files>" or "--appendconfig <files>" spelled out (options are case-sensitive), whose
//    '|'-separated files are absolute paths to existing files, and one of them is read as that option when the
//    arguments are read as RetroArch's getopt_long reads them (options before the first "--", the values of its options
//    that take one skipped; an option it does not know, which stops RetroArch, refuses as well);
//  - the file it resolves to is the one an entry of the list resolves to (compared with the file-system casing).
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
    RetroArchNotIsolated, ///< refused: retroarch*.exe without a usable --appendconfig
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

} // namespace rv::launcher
