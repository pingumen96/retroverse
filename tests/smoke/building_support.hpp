#pragma once

// The building fixture in the GPU smoke tests (I4 step 5a): tests/fixtures/esde/building recorded as the library of a
// case's data directory, imported and planned whole with retroverse-cli (worldplan generate, as the app plans it), so
// that a case knows its rooms, floors and placements before retroverse.exe starts in one (--start-room). The helpers
// use Catch2 assertions: call them from a test case.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/world/world_plan.hpp>
#include <rv/world/world_store.hpp>

#include "smoke_support.hpp"
#include "support/fixture.hpp"

namespace rv::smoke {

/// The building fixture recorded as the library of <case>/data, imported and planned.
struct PlannedBuilding {
    test::Fixture fixture;
    std::filesystem::path dir;
    std::filesystem::path data;

    /// `caseName`: the case's folder under the work root. `consoleRooms`: every system of a home or handheld kind in a
    /// console room of its own (world.json's sharedRoomMinTitles 0, when the building's data has that key: I4 step 4's
    /// shared rooms take the systems under its threshold otherwise), so that the fixture's small systems keep their
    /// families' rooms.
    /// `fixtureName`: the ES-DE fixture planned (tests/fixtures/<name>). `prepare` (I5 step 7) changes the materialized
    /// fixture before its import (a variant without some of its clips).
    explicit PlannedBuilding(std::string_view caseName, bool consoleRooms = true,
                             std::string_view fixtureName = "esde/building",
                             const std::function<void(const test::Fixture&)>& prepare = {});

    /// retroverse-cli --data-dir <data> `arguments`, which must exit 0.
    AppRun cli(std::string_view name, std::vector<std::string> arguments) const;

    /// The building's stored plan.
    [[nodiscard]] world::StoredPlan plan() const;
    /// The id of the room that holds `system` (its sections), which the plan must have.
    [[nodiscard]] std::string roomOf(std::string_view system) const;
};

/// The room of `plan` whose id is `room` (REQUIREd).
[[nodiscard]] const world::Room& roomIn(const world::WorldPlan& plan, std::string_view room);
/// The placements of `plan` in its room `room`.
[[nodiscard]] std::size_t placementsIn(const world::WorldPlan& plan, std::string_view room);

} // namespace rv::smoke
