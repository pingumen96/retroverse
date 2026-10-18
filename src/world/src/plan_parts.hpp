#pragma once

// Parts of the plan's canonical JSON that other documents share (private to rv_world; defined in plan_json.cpp): the
// unplaced games, which a RoomSelection lists exactly as a plan does.

#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_plan.hpp>

#include "data_reader.hpp"

namespace rv::world::detail {

/// {"game", "room", "reason"} and "representative" when it is set.
[[nodiscard]] nlohmann::json unplacedJson(const UnplacedGame& unplaced);

/// The array member `key` of `reader` as unplaced games, sorted and without one twice.
[[nodiscard]] std::vector<UnplacedGame> readUnplacedList(const ObjectReader& reader, std::string_view key);

// The canonical documents without the writers' check by the reader (I3 step 9): the incremental update, whose 50 ms
// budget the check would take a third of, checks its inputs itself and has its plan validated and written (with the
// check) by savePlan. They give the same documents as roomSelectionToJson() and writePlan() when those succeed.

/// The selection's canonical document, its unplaced games sorted (defined in room_selection.cpp).
[[nodiscard]] nlohmann::json roomSelectionDocument(const RoomSelection& selection);
/// The SHA-256 of writePlan()'s text of a plan whose lists sortPlan() has sorted already. Errors: those of
/// core::writeCanonicalJson().
[[nodiscard]] Expected<std::string> sortedPlanDigest(const WorldPlan& sortedPlan);

} // namespace rv::world::detail
