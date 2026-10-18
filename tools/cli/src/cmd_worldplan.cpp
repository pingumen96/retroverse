// retroverse-cli worldplan: the plan of a system's room (ADR-0010), one source per subcommand so that the I3 steps
// implementing them never edit one file:
//   worldplan generate  a fresh plan from the catalog's snapshot of a system (cmd_worldplan_generate.cpp, I3 step 3;
//                       --replace, the explicit regeneration after a backup, I3 step 9)
//   worldplan update    the incremental update of a previous plan, with its change report (cmd_worldplan_update.cpp,
//                       I3 step 9)
//   worldplan show      a plan as text or JSON (cmd_worldplan_show.cpp, I3 step 3; --scene, its static part as a scene
//                       file, I3 step 10a)
//   worldplan check     a plan's validation, exit 1 on a hard failure (cmd_worldplan_check.cpp, I3 step 4)
// This file only dispatches: the arguments after a subcommand's name are its own.

#include <array>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rv/cli/output.hpp>

#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan <subcommand> [options]\n"
    "\n"
    "The plan of a system's room: which games it shows and where, generated from the catalog's snapshot and\n"
    "kept under <user data>/worlds/<library id>/.\n"
    "\n"
    "Subcommands:\n"
    "  generate  Generate a fresh plan for a system's room.\n"
    "  update    Update a previous plan incrementally and report what changed.\n"
    "  show      Show a plan.\n"
    "  check     Validate a plan; exit 1 when a hard rule fails.\n"
    "\n"
    "Run 'retroverse-cli worldplan <subcommand> --help' for its options.\n"
    "\n"
    "Exit codes: 0 ok; 1 the check found problems; 2 usage error; 3 runtime failure.\n";

using SubcommandFunction = ExitCode (*)(CommandContext& context, std::span<const std::string> arguments);

struct Subcommand {
    std::string_view name;
    SubcommandFunction run = nullptr;
};

constexpr std::array<Subcommand, 4> kSubcommands{{
    {"generate", runWorldPlanGenerate},
    {"update", runWorldPlanUpdate},
    {"show", runWorldPlanShow},
    {"check", runWorldPlanCheck},
}};

} // namespace

ExitCode runWorldPlan(CommandContext& context) {
    if (context.arguments.empty()) {
        context.err.write(kUsage);
        return ExitCode::UsageError;
    }
    const std::string& name = context.arguments.front();
    if (name == "--help" || name == "-h") {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    for (const Subcommand& subcommand : kSubcommands) {
        if (subcommand.name == name) {
            return subcommand.run(context, context.arguments.subspan(1));
        }
    }
    context.err.writeLine(fmt::format("retroverse-cli worldplan: unknown {} '{}'",
                                      name.starts_with('-') ? "option" : "subcommand", name));
    context.err.writeLine("Run 'retroverse-cli worldplan --help' for the subcommands.");
    return ExitCode::UsageError;
}

} // namespace rv::cli
