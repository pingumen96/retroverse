#pragma once

// Private to rv_cli_core: the run function of each row of the command table (command.cpp), one per cmd_<name>.cpp,
// and what the dispatcher offers them.

#include <filesystem>
#include <span>
#include <string>

#include <rv/cli/command.hpp>
#include <rv/core/expected.hpp>

namespace rv::cli {

/// The data-directory guard (ADR-0008 §6, §7): refuses (InvalidArgument, naming the folder) a user data directory that
/// lies inside one of `folders`, the folders of a library Retroverse only reads (esde::readOnlyFolders()), with
/// core::checkUserDataDirectory(). Nothing is written.
[[nodiscard]] Status checkDataDirectory(const CommandContext& context, std::span<const std::filesystem::path> folders);

/// Starts the run's log file, <user data>/logs/retroverse-cli.log at --log-level, with the run's banner (the version,
/// the command, the directories); what was logged before went to stderr only. Once: later calls do nothing. The
/// dispatcher calls it before a command runs, but for those with Command::checksDataDirectory, which call it once
/// checkDataDirectory() passed.
void startFileLog(const CommandContext& context);

ExitCode runDiscover(CommandContext& context);
ExitCode runLibrary(CommandContext& context);
ExitCode runDb(CommandContext& context);
ExitCode runImport(CommandContext& context);
ExitCode runSystems(CommandContext& context);
ExitCode runGames(CommandContext& context);
ExitCode runSearch(CommandContext& context);
ExitCode runShow(CommandContext& context);
ExitCode runMedia(CommandContext& context);
ExitCode runProfiles(CommandContext& context);
ExitCode runLaunch(CommandContext& context);
ExitCode runIdentities(CommandContext& context);
ExitCode runAssign(CommandContext& context);
ExitCode runSessions(CommandContext& context);
ExitCode runWorldPlan(CommandContext& context);
ExitCode runWhereIs(CommandContext& context);

/// The subcommands of worldplan (cmd_worldplan.cpp dispatches them), one source each (cmd_worldplan_<name>.cpp), so
/// that the I3 steps implementing them never edit one file: `arguments` are those after the subcommand's name.
ExitCode runWorldPlanGenerate(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanUpdate(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanShow(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanCheck(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanSnapshots(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanUndo(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanEdit(CommandContext& context, std::span<const std::string> arguments);
ExitCode runWorldPlanRegenerate(CommandContext& context, std::span<const std::string> arguments);

} // namespace rv::cli
