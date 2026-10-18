#pragma once

// Parts of the plan's canonical JSON that other documents share (private to rv_world; defined in plan_json.cpp): the
// unplaced games, which a RoomSelection lists exactly as a plan does.

#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/world/world_plan.hpp>

#include "data_reader.hpp"

namespace rv::world::detail {

/// {"game", "room", "reason"} and "representative" when it is set.
[[nodiscard]] nlohmann::json unplacedJson(const UnplacedGame& unplaced);

/// The array member `key` of `reader` as unplaced games, sorted and without one twice.
[[nodiscard]] std::vector<UnplacedGame> readUnplacedList(const ObjectReader& reader, std::string_view key);

} // namespace rv::world::detail
