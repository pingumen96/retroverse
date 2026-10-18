// retroverse-cli worldplan check: a plan's validation (ADR-0010 §11): the hard rules, exit 1 when one fails, and the
// soft ones reported. A stub until I3 step 4 implements it (docs/roadmap.md).

#include <span>
#include <string>

#include "commands.hpp"

namespace rv::cli {

ExitCode runWorldPlanCheck(CommandContext& context, std::span<const std::string> /*arguments*/) {
    return notImplemented(context, "check", "I3 step 4");
}

} // namespace rv::cli
