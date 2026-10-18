// retroverse-cli where-is: where a game stands in its room's plan, from the plan's index (ADR-0010 §9): `where-is
// <gameId>` names its room, fixture and slot, or the reason it is not placed. A stub until I3 step 9 implements it
// (docs/roadmap.md).

#include "commands.hpp"

namespace rv::cli {

ExitCode runWhereIs(CommandContext& context) {
    return notImplemented(context);
}

} // namespace rv::cli
