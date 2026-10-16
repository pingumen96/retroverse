#include <rv/launcher/launch_plan.hpp>

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

namespace rv::launcher {
namespace {

/// Command interpreters and launchers, by file name without extension (ADR-0008 §8).
constexpr std::array<std::string_view, 14> kRefusedPrograms{
    "cmd",      "powershell", "pwsh",    "wscript", "cscript", "mshta", "rundll32",
    "explorer", "conhost",    "wt",      "wsl",     "bash",    "msiexec", "regsvr32",
};

/// A template the launch uses and where the profile writes it ("arguments[1]", "workingDirectory").
struct UsedTemplate {
    std::string where;
    const std::string* text = nullptr;
};

/// Every template of `profile` that a launch expands, in the order the plan expands them.
std::vector<UsedTemplate> templatesOf(const EmulatorProfile& profile) {
    std::vector<UsedTemplate> used;
    for (std::size_t i = 0; i < profile.arguments.size(); ++i) {
        used.push_back({fmt::format("arguments[{}]", i), &profile.arguments[i]});
    }
    if (!profile.workingDirectory.empty()) {
        used.push_back({"workingDirectory", &profile.workingDirectory});
    }
    for (const EnvironmentOverride& variable : profile.environment) {
        if (variable.value) {
            used.push_back({fmt::format("environment.{}", variable.name), &*variable.value});
        }
    }
    for (std::size_t i = 0; i < profile.requiredFiles.size(); ++i) {
        used.push_back({fmt::format("requiredFiles[{}]", i), &profile.requiredFiles[i]});
    }
    return used;
}

/// A template error as a blocker: an undefined variable or an unset environment variable is missing configuration,
/// anything else a template that cannot work.
LaunchBlocker templateBlocker(const Error& error, const EmulatorProfile& profile, const LaunchTarget& target,
                              const UsedTemplate& used) {
    const BlockerKind kind = error.code == ErrorCode::NotFound ? BlockerKind::VariableMissing
                                                               : BlockerKind::TemplateError;
    return LaunchBlocker{kind, fmt::format("{} (profile '{}', {} \"{}\", system \"{}\")", error.message, profile.id,
                                           used.where, *used.text, target.system)};
}

/// A ROM may be a file or a folder (the games that are folders), at any length.
bool romExists(const std::filesystem::path& rom) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(core::extendedLengthPath(rom)), ec) && !ec;
}

} // namespace

std::string_view toString(BlockerKind kind) noexcept {
    switch (kind) {
    case BlockerKind::RomMissing:
        return "ROM missing";
    case BlockerKind::TemplateError:
        return "template error";
    case BlockerKind::VariableMissing:
        return "variable missing";
    case BlockerKind::ExecutableNotFound:
        return "executable not found";
    case BlockerKind::ProgramRefused:
        return "program refused";
    case BlockerKind::InvalidRequest:
        return "invalid request";
    case BlockerKind::RequiredFileMissing:
        return "required file missing";
    }
    return "unknown";
}

bool isRefusedProgram(const std::filesystem::path& executable) {
    const auto stem = core::tryPathToUtf8(executable.stem());
    if (!stem || stem->empty()) {
        return false;
    }
    // No wildcard in the names: matchesWildcard() compares them with the file-system casing, as NTFS compares names.
    return std::any_of(kRefusedPrograms.begin(), kRefusedPrograms.end(),
                       [&](std::string_view name) { return core::matchesWildcard(*stem, name); });
}

LaunchPlan planLaunch(const LaunchTarget& target, const EmulatorProfile& profile, const core::ISystemProbe& probe,
                      const PlanOptions& options) {
    LaunchPlan plan;
    const auto block = [&](BlockerKind kind, std::string message) {
        plan.blocker = LaunchBlocker{kind, std::move(message)};
        return plan;
    };
    const EnvironmentLookup environment = options.environment ? options.environment : processEnvironmentLookup();
    const std::string romShown = core::pathToUtf8Lossy(target.rom);

    // 1. The ROM.
    if (target.rom.empty() || !target.rom.is_absolute()) {
        return block(BlockerKind::RomMissing, fmt::format("the ROM's path is not absolute: \"{}\"", romShown));
    }
    if (options.checkRom && !romExists(target.rom)) {
        return block(BlockerKind::RomMissing, fmt::format("the ROM does not exist: {}", romShown));
    }

    // 2. The configuration, before the executable is looked for: a profile that cannot work for this system is a matter
    // of configuration whether its emulator is installed or not.
    TemplateContext context;
    context.rom = target.rom;
    context.system = target.system;
    context.variables = {&target.variables, profile.systemVariables(target.system), &profile.variables};
    context.environment = environment;
    const std::vector<UsedTemplate> used = templatesOf(profile);
    for (const UsedTemplate& entry : used) {
        if (auto valid = checkTemplateElement(*entry.text, context); !valid) {
            LaunchBlocker blocker = templateBlocker(valid.error(), profile, target, entry);
            return block(blocker.kind, std::move(blocker.message));
        }
    }

    // 3. The executable.
    auto located = locateExecutable(profile.executable, probe, environment);
    if (!located) {
        return block(BlockerKind::ExecutableNotFound,
                     fmt::format("{} (profile '{}'): {}", profile.displayName, profile.id, located.error().message));
    }
    plan.executable = *located;
    if (isRefusedProgram(located->path)) {
        return block(BlockerKind::ProgramRefused,
                     fmt::format("{} is a command interpreter or a launcher, which Retroverse never starts (profile "
                                 "'{}')",
                                 core::pathToUtf8Lossy(located->path), profile.id));
    }
    context.emulatorDirectory = located->path.parent_path();

    // 4. The values.
    const auto expand = [&](const UsedTemplate& entry) { return expandTemplateElement(*entry.text, context); };
    LaunchRequest request;
    request.executable = located->path;
    std::size_t next = 0; // `used` holds the arguments first, then the working directory, environment, required files
    for (; next < profile.arguments.size(); ++next) {
        auto argument = expand(used[next]);
        if (!argument) {
            LaunchBlocker blocker = templateBlocker(argument.error(), profile, target, used[next]);
            return block(blocker.kind, std::move(blocker.message));
        }
        request.arguments.push_back(std::move(*argument));
    }
    if (!profile.workingDirectory.empty()) {
        const UsedTemplate& entry = used[next++];
        auto directory = expand(entry);
        if (!directory) {
            LaunchBlocker blocker = templateBlocker(directory.error(), profile, target, entry);
            return block(blocker.kind, std::move(blocker.message));
        }
        auto path = core::tryPathFromUtf8(*directory);
        if (!path || !path->is_absolute()) {
            return block(BlockerKind::TemplateError,
                         fmt::format("the working directory is not an absolute path once expanded: \"{}\" (profile "
                                     "'{}', workingDirectory \"{}\")",
                                     *directory, profile.id, profile.workingDirectory));
        }
        request.workingDirectory = path->lexically_normal();
    }
    for (const EnvironmentOverride& variable : profile.environment) {
        EnvironmentOverride resolved{variable.name, std::nullopt};
        if (variable.value) {
            const UsedTemplate& entry = used[next++];
            auto value = expand(entry);
            if (!value) {
                LaunchBlocker blocker = templateBlocker(value.error(), profile, target, entry);
                return block(blocker.kind, std::move(blocker.message));
            }
            resolved.value = std::move(*value);
        }
        request.environment.push_back(std::move(resolved));
    }
    request.waitStrategy = profile.waitStrategy;
    request.timeouts.windowTimeout = profile.windowTimeout;
    request.hideConsoleWindow = profile.hideConsoleWindow;
    std::string title = target.title;
    if (title.empty()) {
        std::filesystem::path rom = target.rom;
        if (!rom.has_filename() && rom.has_relative_path()) {
            rom = rom.parent_path(); // a folder named with a trailing separator
        }
        title = core::pathToUtf8Lossy(rom.filename());
    }
    request.label = fmt::format("{} with {}", title, profile.displayName);

    // 5. The request's own checks: a program Windows can start without a shell, its working directory, its command
    // line. Before the required files: a request that can never start requires configuration (ADR-0008 §8), which
    // comes before "core not found" whatever the files.
    if (auto valid = validateLaunchRequest(request); !valid) {
        return block(BlockerKind::InvalidRequest,
                     fmt::format("{} cannot be started: {} (profile '{}')", request.label, valid.error().toString(),
                                 profile.id));
    }

    // 6. The required files.
    for (; next < used.size(); ++next) {
        auto file = expand(used[next]);
        if (!file) {
            LaunchBlocker blocker = templateBlocker(file.error(), profile, target, used[next]);
            return block(blocker.kind, std::move(blocker.message));
        }
        auto path = core::tryPathFromUtf8(*file);
        if (!path || !path->is_absolute() || !probe.fileExists(*path)) {
            return block(BlockerKind::RequiredFileMissing,
                         fmt::format("a file {} needs does not exist: {} (profile '{}', {} \"{}\", system \"{}\")",
                                     profile.displayName, *file, profile.id, used[next].where, *used[next].text,
                                     target.system));
        }
    }
    plan.request = std::move(request);
    return plan;
}

} // namespace rv::launcher
