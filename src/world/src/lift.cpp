#include <rv/world/lift.hpp>

#include <utility>

namespace rv::world {

Expected<WorldPlan> lift(const WorldPlan& v1, const BuildingData& building, const SystemsData& systems) {
    if (v1.schemaVersion != 1) {
        return makeError(ErrorCode::InvalidArgument,
                         "the lift takes a schema-1 plan, not one of schema " + std::to_string(v1.schemaVersion),
                         "lift");
    }
    if (v1.rooms.size() != 1 || v1.rooms.front().id.kind() != RoomIdKind::System ||
        v1.rooms.front().id.isExpansion()) {
        return makeError(ErrorCode::InvalidArgument, "a schema-1 plan holds exactly one room, room:<system>", "lift");
    }
    WorldPlan plan = v1;
    plan.schemaVersion = kPlanSchemaVersion;
    Room& room = plan.rooms.front();
    const catalog::SystemName system = room.id.system();
    const Classification classification = classify(system, {}, systems, building);
    if (!classification.hasRoom()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the system " + system.str() + " gets no room in the building (" +
                             std::string(toString(classification.kind())) + ")",
                         "lift");
    }
    room.kind = RoomKind::Console;
    room.floor = classification.floor;
    room.sections = {system};
    room.generatorVersion = v1.generatorVersion;
    room.expands = {};
    // The north row's first position: the floor holds no other room.
    const auto placement = placeEnteringRoom(room, floorRows({}, room.floor, building.layout), building.layout);
    if (!placement) {
        return makeError(ErrorCode::InvalidArgument, "the building's rows hold no room", "lift");
    }
    room.placement = *placement;
    const auto ground = building.planFloor(kGroundFloor);
    const auto own = building.planFloor(room.floor);
    if (!ground || !own) {
        return makeError(ErrorCode::InvalidArgument, "the building has no ground floor", "lift");
    }
    plan.floors.clear();
    plan.floors.push_back(*ground);
    if (room.floor != kGroundFloor) {
        plan.floors.push_back(*own);
    }
    sortPlan(plan);
    return plan;
}

Expected<WorldPlan> liftIfNeeded(const WorldPlan& plan, const BuildingData& building, const SystemsData& systems) {
    if (plan.schemaVersion == 1) {
        return lift(plan, building, systems);
    }
    return plan;
}

} // namespace rv::world
