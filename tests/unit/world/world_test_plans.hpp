#pragma once

// Helpers of the world tests: hand-built WorldPlans of the shipped template console_room_9x10 (I3 step 4: the
// validation is tested on plans built by hand, since step 3's generator is not there yet), with the selection, the
// prefabs' slots and the lights that validate them.
//
// validRoomPlan() is the base room furnished as ADR-0010 §3 and §4 describe it: the two hero-wall modules (spine
// levels and a band level), the hero corner's parts at the template's positions, the featured wall shelf, three
// side-wall modules on each side wall (w01-w03, e01-e03) and decoration on the other side-wall sockets (posters and two
// floor lamps), the two display cases, a sideboard either side of the doorway and the hero wall's corners left empty;
// 30 games placed (spine, band, featured), two dressed spans, a duplicate title and a game without a cover unplaced.
// It passes every rule, the soft ones included. Its room is a schema-2 console room of snes on the floor gen4, at the
// north row's first position, the plan holding the ground floor and gen4 (I4 step 1).

#include <cstdint>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::test {

/// The shipped template (assets/world/templates/console_room_9x10.json).
[[nodiscard]] world::RoomTemplate shippedRoomTemplate();

[[nodiscard]] catalog::GameId testGameId(std::uint64_t value);
[[nodiscard]] world::RoomId snesRoomId();
[[nodiscard]] world::FixtureId snesFixtureId(std::string_view socket, std::string_view part = {});
[[nodiscard]] world::SlotId snesSlotId(std::string_view socket, std::uint32_t level, std::uint32_t index);

/// The valid plan above.
[[nodiscard]] world::WorldPlan validRoomPlan();

/// The room grown by `growthSteps` steps (16 m deep at 3): a module in every side-wall socket (26 at 16 m), the hero
/// corner, the displays and the south wall's sideboards moved with it, every spine slot of the side modules holding a
/// game (2,000 and more titles).
[[nodiscard]] world::WorldPlan grownRoomPlan(std::uint32_t growthSteps);

/// The fixture of `socket` (and `part`) in the plan; fails the test when there is none.
[[nodiscard]] world::Fixture& fixtureOf(world::WorldPlan& plan, std::string_view socket, std::string_view part = {});

/// The selection that names exactly the plan's games: its placed games as titles, its unplaced entries as unplaced.
[[nodiscard]] world::RoomSelection selectionOf(const world::WorldPlan& plan);

/// The prefab slots of the plan's collection fixtures, as their levels list them.
[[nodiscard]] world::PrefabSlotSource prefabSlotsOf(const world::WorldPlan& plan);

/// The context that validates the plan: the shipped template, the shipped preset's configuration, selectionOf(),
/// prefabSlotsOf() and 12 lights in the room.
[[nodiscard]] world::ValidationContext validContextFor(const world::WorldPlan& plan);

} // namespace rv::test
