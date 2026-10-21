// The building's NFR-08 property suite (rv/world/incremental.hpp's updateBuilding(); ADR-0013 §3, §5, §7; I4 step 6a):
// random sequences (fixed seeds) of a building's updates, on a test-only small room template that fills, grows and runs
// out of capacity within a few hundred titles and test copies of building.json (two rooms per row) and systems.json
// (tests/fixtures/world/building-suite/, laid over a copy of the shipped world data): titles arriving and leaving,
// bursts that add modules, grow rooms and reach their capacity, new systems (large, small, arcade, a custom one with
// gb's platform tag, a custom one no tag places), small systems crossing the shared threshold, full floors, selections
// left out (SC-06: unknown is not empty, a shared room held, a system waiting for it). After every step the update
// keeps the building's invariants (world_building_support.hpp's checkBuildingUpdate(): every placement, every room's
// floor, kind, x, yaw, sections and the rest of its record, z only with growth, every fixture but decoration and what a
// growth moved, the face-out set, new placements spine-out, the halls' lights only appended, every selected game placed
// or unplaced exactly once, no hard finding); a room whose selection is left out stands as it is; a new room takes the
// east end of its row; an update with nothing new writes nothing; and at the end the update of the building of no room
// is the fresh building of the same selections. A Debug build runs fewer seeds and steps of the same sequences.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>

#include "support/temp_dir.hpp"
#include "world_building_support.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using rv::catalog::GameId;
using rv::world::GenerationResult;
using rv::world::Room;
using rv::world::RoomSelection;
using rv::world::WorldPlan;
using rv::test::world::BuildingWorld;
using rv::test::world::StoredRoomData;
using rv::test::world::systemName;

namespace {

constexpr std::string_view kSuiteTemplate = "suite_room_small";

/// The suite's asset root: the shipped world data and prefab manifests copied under a temporary folder (removed at
/// exit), with tests/fixtures/world/building-suite/ laid over it.
const std::filesystem::path& suiteRoot() {
    static const rv::test::TempDir dir("rv-world-building-suite");
    static const std::filesystem::path root = [] {
        const std::filesystem::path assets = dir.path() / "assets";
        std::filesystem::create_directories(assets);
        for (const std::string_view folder : {"world", "manifests"}) {
            std::filesystem::copy(rv::test::world::assetRoot() / folder, assets / folder,
                                  std::filesystem::copy_options::recursive);
        }
        const std::filesystem::path fixture =
            rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "building-suite";
        std::filesystem::copy(fixture, assets / "world",
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);
        return assets;
    }();
    return root;
}

/// The suite's world: its asset root, template, building.json and systems.json, the custom systems' platform tags.
BuildingWorld suiteWorld(std::uint64_t worldSeed) {
    BuildingWorld world;
    world.root = suiteRoot();
    world.templateId = std::string(kSuiteTemplate);
    const rv::world::WorldData& snes = rv::test::world::enteringData(world.root, "snes", {}, kSuiteTemplate);
    world.building = snes.building;
    world.systems = snes.systems;
    world.config.seed = worldSeed;
    world.tags[systemName("pocketlab")] = {"gb"};
    world.tags[systemName("zzcustom")] = {"zzplatform"};
    return world;
}

const rv::world::RoomTemplate& suiteTemplate() {
    return rv::test::world::enteringData(suiteRoot(), "snes", {}, kSuiteTemplate).roomTemplate;
}

/// The systems a sequence draws from, in a fixed order.
const std::vector<std::string_view>& suiteSystems() {
    static const std::vector<std::string_view> systems{"mame",   "fbneo",     "nes",      "snes",  "sfc",
                                                       "megadrive", "genesis", "neogeo",  "gb",    "atarilynx",
                                                       "pokemini",  "amiga",   "pocketlab", "zzcustom"};
    return systems;
}

/// `selection` without the title `game` and its other versions.
void removeTitle(RoomSelection& selection, const GameId& game) {
    std::erase_if(selection.titles, [&](const rv::world::RoomTitle& title) { return title.game == game; });
    std::erase_if(selection.unplaced, [&](const rv::world::UnplacedGame& unplaced) {
        return unplaced.reason == rv::world::UnplacedReason::DuplicateTitle && unplaced.representative == game;
    });
}

/// The room holding `system`'s section, or null.
const Room* roomOfSystem(const WorldPlan& plan, std::string_view system) {
    const rv::catalog::SystemName name = systemName(system);
    for (const Room& room : plan.rooms) {
        if (std::find(room.sections.begin(), room.sections.end(), name) != room.sections.end()) {
            return &room;
        }
    }
    return nullptr;
}

/// Each room added stands at the east end of its row as the rows stood before it (ADR-0013 §7): its footprint's west
/// edge on the rows' start or on the east edge of a room of its row, east of every room of its row that stood before.
void checkNewRoomsAtTheEnd(const WorldPlan& before, const GenerationResult& result, const BuildingWorld& world,
                           std::vector<std::string>& failures, const std::string& where) {
    const rv::world::BuildingLayout& layout = world.building.layout;
    for (const rv::world::RoomId& id : result.changes.roomsAdded) {
        const Room* room = rv::test::world::findRoom(result.plan, id.toString());
        if (room == nullptr) {
            continue;
        }
        const rv::world::Footprint own = rv::world::floorFootprint(*room, layout);
        bool onAnEdge = own.minXMm == layout.rowStartXMm;
        std::uint32_t inRow = 0;
        for (const Room& other : result.plan.rooms) {
            if (other.id == room->id || other.floor != room->floor ||
                rv::world::rowOf(other.placement) != rv::world::rowOf(room->placement)) {
                continue;
            }
            ++inRow;
            const rv::world::Footprint footprint = rv::world::floorFootprint(other, layout);
            onAnEdge = onAnEdge || footprint.maxXMm == own.minXMm;
            const bool stoodBefore = rv::test::world::findRoom(before, other.id.toString()) != nullptr;
            if (stoodBefore && footprint.maxXMm > own.minXMm) {
                failures.push_back(fmt::format("{}: {} stands west of {}, which stood in its row", where,
                                               id.toString(), other.id.toString()));
            }
        }
        if (!onAnEdge) {
            failures.push_back(fmt::format("{}: {} is not at the east end of its row", where, id.toString()));
        }
        if (inRow + 1 > layout.maxRoomsPerRow) {
            failures.push_back(fmt::format("{}: {}'s row holds more than {} rooms", where, id.toString(),
                                           layout.maxRoomsPerRow));
        }
    }
}

} // namespace

TEST_CASE("world/building suite: NFR-08 over random sequences of a building's updates", "[world][building_suite]") {
#if defined(NDEBUG)
    constexpr int kSeeds = 16;
    constexpr int kSteps = 24;
#else
    constexpr int kSeeds = 2;
    constexpr int kSteps = 8;
#endif
    const rv::world::RoomTemplate& roomTemplate = suiteTemplate();
    std::vector<std::string> failures;
    std::size_t entered = 0;
    std::size_t waited = 0;
    std::size_t grew = 0;
    std::size_t capacity = 0;
    std::size_t held = 0;
    std::size_t crossed = 0;
    for (int seed = 0; seed < kSeeds; ++seed) {
        const BuildingWorld world = suiteWorld(rv::core::deriveSeed("building-suite-world", seed));
        rv::core::Random random(rv::core::deriveSeed("building-suite-steps", seed));
        std::map<std::string, RoomSelection> library; // by system
        std::vector<std::string_view> pool = suiteSystems();
        std::set<std::uint64_t> taken;
        const auto arrive = [&]() -> std::string_view {
            const std::size_t at = random.below(pool.size());
            const std::string_view system = pool[at];
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(at));
            // Large (a room of its own, unless an arcade system), or small (a section of its floor's shared room).
            const bool large = random.below(2) == 0;
            const std::size_t titles = large ? 30 + random.below(120) : 1 + random.below(25);
            RoomSelection selection = rv::test::world::systemSelection(
                system, titles, rv::core::deriveSeed("building-suite-system", seed, system));
            // Its games' ids are unique in the library (another system's can never stand for them).
            for (rv::world::RoomTitle& title : selection.titles) {
                title.game = rv::test::world::freshGameId(random, taken);
            }
            std::sort(selection.titles.begin(), selection.titles.end(), rv::world::priorityLess);
            for (rv::world::UnplacedGame& game : selection.unplaced) {
                game.game = rv::test::world::freshGameId(random, taken);
                if (game.reason == rv::world::UnplacedReason::DuplicateTitle) {
                    game.representative = selection.titles[random.below(selection.titles.size())].game;
                }
            }
            std::sort(selection.unplaced.begin(), selection.unplaced.end());
            library.emplace(std::string(system), std::move(selection));
            return system;
        };
        const auto present = [&](std::size_t index) -> RoomSelection& {
            auto it = library.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(index % library.size()));
            return it->second;
        };
        const auto selectionsOf = [&](std::optional<std::string> leftOut = std::nullopt) {
            std::vector<RoomSelection> selections;
            for (const auto& [system, selection] : library) {
                if (system != leftOut) {
                    selections.push_back(selection);
                }
            }
            return selections;
        };

        // The first building: two systems.
        arrive();
        arrive();
        WorldPlan plan = rv::test::world::generateBuildingOf(world, selectionsOf()).plan;
        // The plan is the update of every selection as the library stands (no selection left out, no system waiting):
        // an update with nothing new then writes nothing. (After a step that left a selection out, the next whole
        // update records every input again, and enters the systems that waited.)
        bool settled = true;
        for (int step = 0; step < kSteps; ++step) {
            const std::string where = fmt::format("seed {}, step {}", seed, step);
            const std::uint64_t action = random.below(100);
            std::optional<std::string> leftOut;
            bool nothingNew = false;
            if (action < 25 && !pool.empty()) {
                arrive(); // a new system
            } else if (action < 45) {
                // A few titles arrive, and now and then another version or a game without a cover.
                RoomSelection& selection = present(random.below(library.size()));
                for (std::uint64_t i = 0, n = 1 + random.below(6); i < n; ++i) {
                    rv::test::world::addTitle(selection, random, taken, random.below(4) == 0, random.below(50));
                }
                if (random.below(3) == 0 && !selection.titles.empty()) {
                    rv::test::world::addUnplaced(selection, random, taken, rv::world::UnplacedReason::DuplicateTitle,
                                                 selection.titles[random.below(selection.titles.size())].game);
                }
            } else if (action < 57) {
                // A burst, mostly to the largest system: side-wall modules, growth, capacity.
                RoomSelection* selection = &present(random.below(library.size()));
                if (random.below(3) != 0) {
                    for (auto& [system, candidate] : library) {
                        selection = candidate.titles.size() > selection->titles.size() ? &candidate : selection;
                    }
                }
                for (std::uint64_t i = 0, n = 200 + random.below(500); i < n; ++i) {
                    rv::test::world::addTitle(*selection, random, taken);
                }
            } else if (action < 67) {
                // A small system crosses the shared threshold (it stays in its section).
                for (auto& [system, selection] : library) {
                    if (selection.distinctTitles < 30) {
                        while (selection.distinctTitles < 30 + random.below(10)) {
                            rv::test::world::addTitle(selection, random, taken);
                        }
                        ++crossed;
                        break;
                    }
                }
            } else if (action < 79) {
                // Titles leave the catalog, or their files go missing: their placements stay.
                RoomSelection& selection = present(random.below(library.size()));
                for (std::uint64_t i = 0, n = 1 + random.below(3); i < n && !selection.titles.empty(); ++i) {
                    const GameId game = selection.titles[random.below(selection.titles.size())].game;
                    removeTitle(selection, game);
                    if (random.below(2) == 0) {
                        rv::world::UnplacedGame missing{game, selection.room, rv::world::UnplacedReason::Unavailable,
                                                        {}};
                        selection.unplaced.insert(
                            std::upper_bound(selection.unplaced.begin(), selection.unplaced.end(), missing), missing);
                    }
                }
            } else if (action < 92) {
                // A system's selection is unknown this time (SC-06), and maybe a new system arrives meanwhile.
                leftOut = std::string(present(random.below(library.size())).room.system().str());
                if (!pool.empty() && random.below(2) == 0) {
                    arrive();
                }
            } else {
                nothingNew = true;
            }

            const WorldPlan before = plan;
            const std::vector<RoomSelection> given = selectionsOf(leftOut);
            const GenerationResult result =
                rv::test::world::updateBuildingOf(world, before, given, StoredRoomData::OfItsFirstSection);
            rv::test::world::checkBuildingUpdate(before, result, given, world, roomTemplate, failures, where);
            checkNewRoomsAtTheEnd(before, result, world, failures, where);
            if (nothingNew && settled && result.plan != before) {
                failures.push_back(where + ": an update with nothing new changed the plan");
            }
            if (nothingNew && settled && result.changes.digestAfter != result.changes.digestBefore) {
                failures.push_back(where + ": an update with nothing new has another digest");
            }
            settled = !leftOut && result.changes.systemsWaiting.empty();
            if (leftOut) {
                // The room of the system left out stands as it is (held, when other sections share it).
                if (const Room* room = roomOfSystem(before, *leftOut)) {
                    const auto then = rv::world::roomContentDigest(before, room->id);
                    const auto now = rv::world::roomContentDigest(result.plan, room->id);
                    if (!then || !now || *then != *now) {
                        failures.push_back(fmt::format("{}: {} changed while {}'s selection was unknown", where,
                                                       room->id.toString(), *leftOut));
                    }
                    held += room->sections.size() > 1 ? 1 : 0;
                }
            }
            entered += result.changes.systemsEntered.size();
            waited += result.changes.systemsWaiting.size();
            grew += result.changes.rebuild == rv::world::RebuildLevel::RoomExpansion ? 1 : 0;
            for (const rv::world::UnplacedGame& game : result.plan.unplaced) {
                if (game.reason == rv::world::UnplacedReason::Capacity) {
                    ++capacity;
                    break;
                }
            }
            plan = result.plan;
        }
        // At the end: once settled, the same selections change nothing, and the building of no room updated with them
        // is the fresh building.
        const std::vector<RoomSelection> all = selectionsOf();
        if (!settled) {
            plan = rv::test::world::updateBuildingOf(world, plan, all, StoredRoomData::OfItsFirstSection).plan;
        }
        const GenerationResult same =
            rv::test::world::updateBuildingOf(world, plan, all, StoredRoomData::OfItsFirstSection);
        if (same.plan != plan || same.changes.digestAfter != same.changes.digestBefore) {
            failures.push_back(fmt::format("seed {}: the same selections changed the plan", seed));
        }
        const GenerationResult fresh = rv::test::world::generateBuildingOf(world, all);
        const WorldPlan empty = rv::test::world::generateBuildingOf(world, {}).plan;
        const GenerationResult fromNothing =
            rv::test::world::updateBuildingOf(world, empty, all, StoredRoomData::OfItsFirstSection);
        if (*rv::world::writePlan(fromNothing.plan) != *rv::world::writePlan(fresh.plan)) {
            failures.push_back(fmt::format("seed {}: the building of no room updated is not the fresh building", seed));
        }
    }
    rv::test::world::reportFailures(failures);
    // The sequences reach what they are for.
    INFO("entered " << entered << ", waited " << waited << ", grew " << grew << ", capacity " << capacity
                    << ", held " << held << ", crossed " << crossed);
    CHECK(entered > 0);
    CHECK(grew > 0);
    CHECK(crossed > 0);
#if defined(NDEBUG)
    CHECK(waited > 0);
    CHECK(capacity > 0);
    CHECK(held > 0);
#endif
}
