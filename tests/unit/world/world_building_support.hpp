#pragma once

// What the building update's tests share (I4 step 6a, ADR-0013 §3, §5, §7): the world's data of a system entering the
// building and of a stored room, read once per asset root (loadWorldData(), loadRoomData()); selections of invented
// titles for any system; a building's generation and update with the data every room and entering system needs; and
// the invariants every update of a building keeps, checked after each step (nothing that stands moves).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/validation.hpp>

#include "world_generation_support.hpp"

namespace rv::test::world {

inline rv::catalog::SystemName systemName(std::string_view text) {
    return *rv::catalog::SystemName::parse(text);
}

/// The data of `system` entering a building of the asset root `root` (loadWorldData() with its platform tags and the
/// template `templateId`), read once per run.
inline const rv::world::WorldData& enteringData(const std::filesystem::path& root, std::string_view system,
                                                const std::vector<std::string>& tags = {},
                                                std::string_view templateId = rv::world::kDefaultRoomTemplate) {
    static std::map<std::string, std::unique_ptr<rv::world::WorldData>> cache;
    std::string key = rv::core::pathToUtf8(root) + "|" + std::string(system) + "|" + std::string(templateId);
    for (const std::string& tag : tags) {
        key += "|" + tag;
    }
    auto found = cache.find(key);
    if (found == cache.end()) {
        rv::world::RoomDataRequest request;
        request.system = systemName(system);
        request.platformTags = tags;
        request.templateId = std::string(templateId);
        auto data = rv::world::loadWorldData(root, request);
        INFO(system << ": " << (data ? std::string("loaded") : data.error().toString()));
        REQUIRE(data.has_value());
        found = cache.emplace(key, std::make_unique<rv::world::WorldData>(std::move(*data))).first;
    }
    return *found->second;
}

/// The data of a stored room of the asset root `root` (loadRoomData() with its first section's platform tags), read
/// once per run.
inline const rv::world::WorldData& storedData(const std::filesystem::path& root, const rv::world::Room& room,
                                              const std::vector<std::string>& tags = {}) {
    static std::map<std::string, std::unique_ptr<rv::world::WorldData>> cache;
    std::string key = fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", rv::core::pathToUtf8(root), room.id.toString(),
                                  rv::world::toString(room.kind), room.floor,
                                  room.sections.empty() ? std::string() : room.sections.front().str(),
                                  room.mediaFamily, room.themePack, room.templateId, room.templateVersion);
    for (const std::string& tag : tags) {
        key += "|" + tag;
    }
    auto found = cache.find(key);
    if (found == cache.end()) {
        auto data = rv::world::loadRoomData(root, room, tags);
        INFO(room.id.toString() << ": " << (data ? std::string("loaded") : data.error().toString()));
        REQUIRE(data.has_value());
        found = cache.emplace(key, std::make_unique<rv::world::WorldData>(std::move(*data))).first;
    }
    return *found->second;
}

/// makeSelection()'s selection of `titles` titles for `system`'s room, `distinct` distinct titles over all its games
/// (at least the titles).
inline rv::world::RoomSelection systemSelection(std::string_view system, std::size_t titles, std::uint64_t seed,
                                                std::size_t distinct = 0) {
    rv::world::RoomSelection selection = makeSelection(titles, seed);
    selection.room = *rv::world::RoomId::forSystem(systemName(system));
    for (rv::world::UnplacedGame& game : selection.unplaced) {
        game.room = selection.room;
    }
    selection.distinctTitles = std::max(distinct, titles);
    return selection;
}

/// A building's world: its asset root, its template, building.json's and systems.json's data, the configuration and
/// the systems' platform tags.
struct BuildingWorld {
    std::filesystem::path root = assetRoot();
    std::string templateId = std::string(rv::world::kDefaultRoomTemplate);
    rv::world::BuildingData building;
    rv::world::SystemsData systems;
    rv::world::WorldConfig config;
    std::map<rv::catalog::SystemName, std::vector<std::string>> tags;

    [[nodiscard]] std::vector<std::string> tagsOf(const rv::catalog::SystemName& system) const {
        const auto found = tags.find(system);
        return found != tags.end() ? found->second : std::vector<std::string>{};
    }
};

/// The world of the shipped data (the asset root's building.json and systems.json), with the world seed `seed`.
inline BuildingWorld shippedWorld(std::uint64_t seed) {
    BuildingWorld world;
    const rv::world::WorldData& snes = enteringData(world.root, "snes");
    world.building = snes.building;
    world.systems = snes.systems;
    world.config.seed = seed;
    return world;
}

/// The fresh building of `selections`.
inline rv::world::GenerationResult generateBuildingOf(const BuildingWorld& world,
                                                      const std::vector<rv::world::RoomSelection>& selections) {
    std::vector<rv::world::WorldData> rooms;
    for (const rv::world::RoomSelection& selection : selections) {
        const rv::catalog::SystemName& system = selection.room.system();
        rooms.push_back(enteringData(world.root, system.str(), world.tagsOf(system), world.templateId));
    }
    rv::world::BuildingInput input;
    input.library = library();
    input.selections = selections;
    input.platformTags = world.tags;
    input.config = world.config;
    auto result = rv::world::generateBuilding(input, world.building, world.systems, rooms);
    INFO((result ? std::string("generated") : result.error().toString()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// The update of `previous` to `selections`: the stored rooms' data (loadRoomData()) and the entering systems'.
inline rv::world::GenerationResult updateBuildingOf(const BuildingWorld& world, const rv::world::WorldPlan& previous,
                                                    const std::vector<rv::world::RoomSelection>& selections) {
    std::vector<rv::world::WorldData> rooms;
    for (const rv::world::Room& room : previous.rooms) {
        rooms.push_back(storedData(world.root, room, world.tagsOf(room.sections.front())));
    }
    for (const rv::world::RoomSelection& selection : selections) {
        const rv::catalog::SystemName& system = selection.room.system();
        const bool stored = std::any_of(previous.rooms.begin(), previous.rooms.end(), [&](const rv::world::Room& room) {
            return std::find(room.sections.begin(), room.sections.end(), system) != room.sections.end();
        });
        if (!stored) {
            rooms.push_back(enteringData(world.root, system.str(), world.tagsOf(system), world.templateId));
        }
    }
    rv::world::BuildingUpdateInput input;
    input.library = library();
    input.selections = selections;
    input.platformTags = world.tags;
    input.config = world.config;
    input.previous = previous;
    auto result = rv::world::updateBuilding(input, world.building, world.systems, rooms);
    INFO((result ? std::string("updated") : result.error().toString()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// The room `id` of `plan`, or null.
inline const rv::world::Room* findRoom(const rv::world::WorldPlan& plan, std::string_view id) {
    const auto found = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                    [&](const rv::world::Room& room) { return room.id.toString() == id; });
    return found != plan.rooms.end() ? &*found : nullptr;
}

inline std::vector<std::string> sectionNames(const rv::world::Room& room) {
    std::vector<std::string> sections;
    for (const rv::catalog::SystemName& system : room.sections) {
        sections.push_back(system.str());
    }
    return sections;
}

/// The hall of `floor` as the plan's rooms derive it (`roomTemplate` for every room).
inline rv::world::Hall hallOf(const rv::world::WorldPlan& plan, std::string_view floor,
                              const rv::world::RoomTemplate& roomTemplate, const rv::world::BuildingLayout& layout) {
    std::vector<rv::world::FloorRoom> rooms;
    for (const rv::world::Room& room : plan.rooms) {
        rooms.push_back({&room, &roomTemplate});
    }
    return rv::world::deriveHall(floor, rooms, layout);
}

/// What every update of a building keeps (rv/world/incremental.hpp, ADR-0013 §5, §7), `result` the update of `before`
/// to `given`: the outcome Updated and no hard finding; every placement of `before` as it stood; every stored room on its
/// floor, of its kind, at its x and yaw, its expansion's head and its sections a prefix of its sections now, its z
/// changed exactly when its growth did (following its depth); the halls' lights only appended; every game of the given
/// selections placed or unplaced exactly once (a waiting system's nowhere); the rooms added the plan's new rooms; the
/// plan read back as written, its digest the report's. Failures are appended to `failures`, tagged with `where`.
inline void checkBuildingUpdate(const rv::world::WorldPlan& before, const rv::world::GenerationResult& result,
                                std::span<const rv::world::RoomSelection> given, const BuildingWorld& world,
                                const rv::world::RoomTemplate& roomTemplate, std::vector<std::string>& failures,
                                const std::string& where) {
    const auto expect = [&](bool condition, std::string_view what) {
        if (!condition) {
            failures.push_back(fmt::format("{}: {}", where, what));
        }
    };
    const rv::world::WorldPlan& plan = result.plan;
    expect(result.changes.outcome == rv::world::GenerationOutcome::Updated, "the outcome is Updated");
    for (const rv::world::ValidationFinding& finding : result.validation.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            failures.push_back(
                fmt::format("{}: hard finding {} {}: {}", where, finding.rule, finding.subject, finding.message));
        }
    }
    // Every placement as it stood (both lists sorted).
    expect(std::includes(plan.placements.begin(), plan.placements.end(), before.placements.begin(),
                         before.placements.end()),
           "every previous placement stands as it stood");
    expect(result.changes.kept == before.placements.size(), "the report keeps every previous placement");
    // Every stored room where it stood.
    std::set<std::string> roomsBefore;
    for (const rv::world::Room& room : before.rooms) {
        roomsBefore.insert(room.id.toString());
        const rv::world::Room* now = findRoom(plan, room.id.toString());
        if (now == nullptr) {
            failures.push_back(fmt::format("{}: {} is gone", where, room.id.toString()));
            continue;
        }
        const std::string id = room.id.toString();
        expect(now->floor == room.floor && now->kind == room.kind && now->expands == room.expands,
               id + " keeps its floor, kind and expansion's head");
        expect(now->placement.xMm == room.placement.xMm && now->placement.yawHalfTurns == room.placement.yawHalfTurns,
               id + " keeps its x and yaw");
        expect(now->sections.size() >= room.sections.size() &&
                   std::equal(room.sections.begin(), room.sections.end(), now->sections.begin()),
               id + "'s sections stand first, in their order");
        expect((now->placement.zMm != room.placement.zMm) == (now->growthSteps != room.growthSteps),
               id + "'s z changes exactly with its growth");
        expect(now->placement == rv::world::followDepth(*now, world.building.layout), id + "'s z follows its depth");
        expect(now->growthSteps >= room.growthSteps, id + " never shrinks");
    }
    // The floors stay, the halls' lights are only appended.
    for (const rv::world::Floor& floor : before.floors) {
        expect(std::find(plan.floors.begin(), plan.floors.end(), floor) != plan.floors.end(),
               "the floor " + floor.id + " stays");
        const std::vector<rv::world::FloorPoint> lightsBefore =
            hallOf(before, floor.id, roomTemplate, world.building.layout).lights;
        const std::vector<rv::world::FloorPoint> lightsNow =
            hallOf(plan, floor.id, roomTemplate, world.building.layout).lights;
        expect(lightsNow.size() >= lightsBefore.size() &&
                   std::equal(lightsBefore.begin(), lightsBefore.end(), lightsNow.begin()),
               "the hall of " + floor.id + " only appends lights");
    }
    // The rooms added are the plan's new rooms.
    std::vector<rv::world::RoomId> added;
    for (const rv::world::Room& room : plan.rooms) {
        if (!roomsBefore.contains(room.id.toString())) {
            added.push_back(room.id);
        }
    }
    expect(added == result.changes.roomsAdded, "the rooms added are the plan's new rooms");
    // Every game of the given selections once, a waiting system's nowhere.
    std::map<rv::catalog::GameId, std::size_t> entries;
    for (const rv::world::Placement& placement : plan.placements) {
        ++entries[placement.game];
    }
    for (const rv::world::UnplacedGame& game : plan.unplaced) {
        ++entries[game.game];
    }
    std::set<rv::catalog::SystemName> waiting;
    for (const rv::world::SystemRoom& system : result.changes.systemsWaiting) {
        waiting.insert(system.system);
    }
    for (const rv::world::RoomSelection& selection : given) {
        const bool waits = waiting.contains(selection.room.system());
        std::size_t wrong = 0;
        const auto count = [&](const rv::catalog::GameId& game) {
            const auto found = entries.find(game);
            const std::size_t n = found != entries.end() ? found->second : 0;
            wrong += waits ? (n != 0 ? 1 : 0) : (n != 1 ? 1 : 0);
        };
        for (const rv::world::RoomTitle& title : selection.titles) {
            count(title.game);
        }
        for (const rv::world::UnplacedGame& game : selection.unplaced) {
            count(game.game);
        }
        expect(wrong == 0, fmt::format("{} game(s) of {} {}", wrong, selection.room.system().str(),
                                       waits ? "stand in a plan their system waits to enter"
                                             : "are not placed or unplaced exactly once"));
    }
    // The plan reads back as itself, and its digest is the report's.
    auto text = rv::world::writePlan(plan);
    expect(text.has_value(), "the plan is written");
    if (text) {
        expect(rv::core::sha256Hex(*text) == result.changes.digestAfter, "the digest after is the plan's");
        auto read = rv::world::readPlan(*text, "updated");
        expect(read.has_value() && *read == plan, "the plan reads back as written");
    }
}

/// Reports the first failures of a suite.
inline void reportFailures(const std::vector<std::string>& failures) {
    for (std::size_t i = 0; i < std::min<std::size_t>(failures.size(), 20); ++i) {
        UNSCOPED_INFO(failures[i]);
    }
    CHECK(failures.empty());
}

} // namespace rv::test::world
