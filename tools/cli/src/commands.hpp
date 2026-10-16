#pragma once

// Private to rv_cli_core: the run function of each row of the command table (command.cpp), one per cmd_<name>.cpp.

#include <rv/cli/command.hpp>

namespace rv::cli {

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

} // namespace rv::cli
