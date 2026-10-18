// retroverse-cli worldplan show: a plan as text or JSON (ADR-0010 §8); with --scene <file>, its static part written as
// a scene file (I3 step 10a). A stub until I3 step 3 implements it (docs/roadmap.md).

#include <span>
#include <string>

#include "commands.hpp"

namespace rv::cli {

ExitCode runWorldPlanShow(CommandContext& context, std::span<const std::string> /*arguments*/) {
    return notImplemented(context, "show", "I3 step 3");
}

} // namespace rv::cli
