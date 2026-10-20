#pragma once

// The schema-1 lift (ADR-0013 §5; I4 step 1): I3's plan of one room made a building, a pure function of the plan and
// the building's data. The v1 room becomes a console room of its system (its id, template and version, bounds, height,
// growth steps, seed, media family and theme pack, and every fixture, placement, dressing, light and unplaced element
// as they were) on the floor its system's classification gives (systems.json, without platform tags: a plan holds
// none), at the north row's first position; the plan holds that floor and the ground floor, and nothing else: the
// building update adds the other systems. The room records the generator that made it (the v1 plan's version), the
// plan its inputDigest as provenance. The frozen oracles of tests/fixtures/world/v1/ check the lift with a 1.3 update.

#include <rv/core/expected.hpp>
#include <rv/world/building.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// The schema-2 building of a schema-1 plan (readPlan()'s content of a schema-1 file). Errors: InvalidArgument for a
/// plan that is not of schema 1, does not hold exactly one room, or whose room's system gets no room (excluded, or on
/// no floor of `building`).
[[nodiscard]] Expected<WorldPlan> lift(const WorldPlan& v1, const BuildingData& building, const SystemsData& systems);

/// `plan` as a building: lift()'s for schema 1, the plan itself for schema 2. Errors: those of lift().
[[nodiscard]] Expected<WorldPlan> liftIfNeeded(const WorldPlan& plan, const BuildingData& building,
                                               const SystemsData& systems);

} // namespace rv::world
