#pragma once

// GPU-free (part of rv_cli_core): the command table and the dispatcher of retroverse-cli (ADR-0008 §2).

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/common.h>

#include <rv/core/expected.hpp>

namespace rv::core {
class Paths;
struct PathEnvironment;
} // namespace rv::core

namespace rv::cli {

class Output;

/// Process exit codes of retroverse-cli, as those of retroverse.exe.
enum class ExitCode : int {
    Ok = 0,
    /// The command ran and found problems (a check, a verification).
    ProblemsFound = 1,
    /// Invalid arguments, an unknown command, a command not implemented yet, or a failed initialization (directories).
    UsageError = 2,
    /// The command failed while running (files, the database, an unexpected exception).
    RuntimeFailure = 3,
};

[[nodiscard]] constexpr int toInt(ExitCode code) noexcept {
    return static_cast<int>(code);
}

/// The options given before the command name:
/// `retroverse-cli [--data-dir <dir> | --portable] [--asset-root <dir>] [--log-level <level>] <command> [options]`.
struct GlobalOptions {
    std::optional<std::filesystem::path> dataDir;   ///< --data-dir: the user data directory
    bool portable = false;                          ///< --portable: user data next to the executable
    std::optional<std::filesystem::path> assetRoot; ///< --asset-root
    /// --log-level: the level of the log file; stderr shows warnings and errors, and everything at debug and trace.
    spdlog::level::level_enum logLevel = spdlog::level::info;
    bool showHelp = false;    ///< --help / -h
    bool showVersion = false; ///< --version
};

/// A parsed command line: the global options, then a command and its own arguments.
struct Invocation {
    GlobalOptions global;
    std::string command;                ///< empty when none was given
    std::vector<std::string> arguments; ///< everything after the command's name, for the command to parse
};

/// Parses arguments (UTF-8, program name excluded): global options up to the first argument that is not an option,
/// which names the command; the arguments after it belong to the command. Errors (InvalidArgument, naming the option):
/// unknown options and missing values before the command, --data-dir with --portable, empty paths, a --log-level other
/// than trace, debug, info, warn or error. With --help or --version the values are not validated.
[[nodiscard]] Expected<Invocation> parseInvocation(std::span<const std::string> arguments);

struct Command;

/// What a command runs with.
struct CommandContext {
    const Command& command;                 ///< its row of the command table
    const GlobalOptions& global;            ///< the options given before it
    std::span<const std::string> arguments; ///< its own arguments
    const core::Paths& paths;               ///< the resolved directories (asset root, user data)
    Output& out;                            ///< results: stdout (a command's --output names a file instead)
    Output& err;                            ///< diagnostics: stderr
};

using CommandFunction = ExitCode (*)(CommandContext& context);

/// One row of the command table.
struct Command {
    std::string_view name;
    std::string_view summary; ///< one line, listed by --help
    int step = 0;             ///< the I2 step that implements it (docs/roadmap.md)
    CommandFunction run = nullptr;
    /// The command checks the user data directory itself against the folders of the library it reads, which its
    /// options can name (discover, library), and starts the log file only once that passed. For the other commands
    /// the dispatcher checks it against the library library.json records before they run (run()).
    bool checksDataDirectory = false;
};

/// The command table, in the order --help lists it.
[[nodiscard]] std::span<const Command> commands() noexcept;
/// The command named `name` (exact match), or null.
[[nodiscard]] const Command* findCommand(std::string_view name) noexcept;

/// The text --help prints: the syntax, the commands with their summaries, the global options and the exit codes.
[[nodiscard]] std::string helpText();

/// What a command does until its I2 step implements it: writes "retroverse-cli <name>: not implemented yet (I2 step
/// <step>)" to the context's stderr and returns ExitCode::UsageError.
[[nodiscard]] ExitCode notImplemented(CommandContext& context);

/// Runs retroverse-cli on `arguments` (UTF-8, program name excluded) and returns its exit code:
///  - --version writes the version banner to `out`, --help the help text; both exit 0;
///  - invalid global options, no command (the help text goes to `err`) or an unknown command (named) exit 2;
///  - otherwise the directories are resolved from the global options and `environment` (failure: exit 2), logging
///    starts (console sink on stderr: warnings and errors, everything at debug and trace) and the command runs. An
///    exception escaping it is logged and exits 3. Logging is shut down before returning.
/// Retroverse writes nothing in the folders of the library it reads (ADR-0008 §6, §7), not even its log file: the log
/// file <user data>/logs/retroverse-cli.log (at --log-level) starts once the user data directory is known to lie
/// outside them. A command with Command::checksDataDirectory checks that itself; for the others, when
/// <user data>/config/library.json records a library, the dispatcher refuses a user data directory inside its folders
/// (esde::readOnlyFolders(): the message on `err`, exit 2, nothing written) before the command runs.
/// Never writes logs to `out`. `out` and `err` are finished by the caller.
[[nodiscard]] ExitCode run(std::span<const std::string> arguments, Output& out, Output& err,
                           const core::PathEnvironment& environment);

} // namespace rv::cli
