// retroverse-cli worldplan update: the incremental update of a previous plan (ADR-0010 §10): every placement kept, the
// new games added, the change report (--diff). A stub until I3 step 9 implements it (docs/roadmap.md).

#include <span>
#include <string>

#include "commands.hpp"

namespace rv::cli {

ExitCode runWorldPlanUpdate(CommandContext& context, std::span<const std::string> /*arguments*/) {
    return notImplemented(context, "update", "I3 step 9");
}

} // namespace rv::cli
