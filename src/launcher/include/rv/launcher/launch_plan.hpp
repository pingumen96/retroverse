#pragma once

// Launch plans (ADR-0008 §8): a neutral launch target (a ROM, its system, the launch's own variables) and an emulator
// profile become a LaunchRequest ready for a LaunchSession, or a blocker that says why the game cannot start. No
// catalog type is involved: the catalog's launch resolution (I2 step 10) fills the target, chooses the profile and maps
// the blockers to its readiness states, whose order the checks follow (ROM missing, requires launcher configuration,
// emulator not found, core not found): the ROM, the configuration, the executable, the request's own validation, then
// the required files. What depends on the executable (a refused program, a request Windows cannot start) can only be
// told once it is found, so it follows "emulator not found", still before "core not found".

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <rv/core/system_probe.hpp>
#include <rv/launcher/arg_template.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_request.hpp>

namespace rv::launcher {

/// What to launch, in neutral terms.
struct LaunchTarget {
    /// The game's file, or its folder for the games that are folders: an absolute path (any length), as the catalog
    /// builds it. {rom} gives it with '\' separators.
    std::filesystem::path rom;
    /// The ES-DE system name ("gb"): {system}, and which of the profile's systems settings apply.
    std::string system;
    /// The launch's own variables (a per-game setting), the first scope of {var:NAME}, before the system's and the
    /// profile's.
    TemplateVariables variables;
    /// Names the launch in LaunchRequest::label and the logs ("Mole Mania"); empty: the ROM's file name.
    std::string title;
};

/// Why a launch cannot start, in the order the plan checks (the readiness the catalog shows in parentheses).
enum class BlockerKind : std::uint8_t {
    RomMissing,          ///< the ROM does not exist, or its path is not absolute ("ROM missing")
    TemplateError,       ///< a template cannot be expanded: a syntax error or unknown token, an environment variable
                         ///< outside the allow-list, variables that refer to each other, a working directory that is
                         ///< not absolute ("requires launcher configuration")
    VariableMissing,     ///< a {var:NAME} defined nowhere for the launch, or an {env:NAME} that is not set ("requires
                         ///< launcher configuration")
    ExecutableNotFound,  ///< the profile's executable is nowhere on the machine ("emulator not found")
    ProgramRefused,      ///< the executable is a command interpreter or a launcher, isRefusedProgram() ("requires
                         ///< launcher configuration")
    InvalidRequest,      ///< validateLaunchRequest() refuses the request: a batch file, a file that is not a Windows
                         ///< program, a working directory that does not exist, a command line too long ("requires
                         ///< launcher configuration")
    RequiredFileMissing, ///< a file the profile requires does not exist: a RetroArch core ("core not found")
};

/// "ROM missing", "template error", "variable missing", "executable not found", "program refused",
/// "invalid request", "required file missing".
[[nodiscard]] std::string_view toString(BlockerKind kind) noexcept;

/// Why a launch cannot start.
struct LaunchBlocker {
    BlockerKind kind = BlockerKind::InvalidRequest;
    /// One sentence naming what is missing or wrong: the token or the variable with the profile, the field and the
    /// system, the path that was looked for.
    std::string message;
};

/// A planned launch: a request, or a blocker.
struct LaunchPlan {
    /// The request when nothing blocks the launch: validateLaunchRequest() accepted it.
    std::optional<LaunchRequest> request;
    /// Why the launch cannot start, otherwise.
    std::optional<LaunchBlocker> blocker;
    /// The executable when it was found, also when a later check blocks the launch (retroverse-cli reports it).
    std::optional<LocatedExecutable> executable;

    [[nodiscard]] bool ready() const noexcept { return request.has_value(); }
};

/// How a plan is made.
struct PlanOptions {
    /// Checks that the ROM exists (a file or a folder, through its \\?\ form). Off: the catalog's readiness of a
    /// system, where the game's own availability is known from the import.
    bool checkRom = true;
    /// The values of {env:NAME}; nullptr: processEnvironmentLookup().
    EnvironmentLookup environment;
};

/// Plans the launch of `target` with `profile` on the machine `probe` describes. The first check that fails gives the
/// blocker:
///  1. the ROM: an absolute path; it exists (PlanOptions::checkRom);
///  2. the configuration: every template the launch uses (arguments, working directory, environment values, required
///     files) expands for the target's system, the variables looked up in the target, the system's settings, then the
///     profile, before the executable is looked for (checkTemplateElement());
///  3. the executable: locateExecutable(); not a refused program;
///  4. the values: arguments (one per element), the working directory (absolute; empty: the executable's folder), the
///     environment overrides, the wait strategy, the window timeout, hideConsoleWindow and the label
///     ("<title> with <profile's display name>");
///  5. validateLaunchRequest() accepts the request;
///  6. the required files exist (ISystemProbe::fileExists).
/// Never starts anything; the automation guard applies when the request starts (launch_guard.hpp).
[[nodiscard]] LaunchPlan planLaunch(const LaunchTarget& target, const EmulatorProfile& profile,
                                    const core::ISystemProbe& probe, const PlanOptions& options = {});

/// True when `executable`'s file name, without its extension and compared with the file-system casing, is a command
/// interpreter or a launcher that runs text of its own: cmd, powershell, pwsh, wscript, cscript, mshta, rundll32,
/// explorer, conhost, wt, wsl, bash, msiexec or regsvr32 (ADR-0008 §8). Their command lines are not parsed as
/// CommandLineToArgvW parses them, so a ROM name holding '&' or '%' could become a command: never started.
[[nodiscard]] bool isRefusedProgram(const std::filesystem::path& executable);

} // namespace rv::launcher
