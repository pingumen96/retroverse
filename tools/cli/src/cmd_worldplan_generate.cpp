// retroverse-cli worldplan generate: a fresh plan for a system's room from the catalog's snapshot (ADR-0010 §2 to §8),
// validated and written atomically under <user data>/worlds/<library id>/; with --replace, over a previous plan after a
// backup (I3 step 9). A stub until I3 step 3 implements it (docs/roadmap.md).

#include <span>
#include <string>

#include "commands.hpp"

namespace rv::cli {

ExitCode runWorldPlanGenerate(CommandContext& context, std::span<const std::string> /*arguments*/) {
    return notImplemented(context, "generate", "I3 step 3");
}

} // namespace rv::cli
