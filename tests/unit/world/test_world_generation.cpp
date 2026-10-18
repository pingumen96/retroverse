// A fresh room's generation (rv/world/generation.hpp, ADR-0010 §3 to §8; I3 step 3): golden plans for the reference
// sizes (tests/fixtures/world/golden, rewritten only with RV_UPDATE_GOLDEN=1), two runs byte-identical, the room at the
// reference sizes (G02's 50 titles all face-out, the reference library's 890 in three side-wall modules, growth and
// capacity at 8,000), every plan of the golden sizes passing I3 step 4's validation on the manifests' slots, and the
// display rule's properties over 1,000 random sizes (60 in Debug): the face-out titles exactly the first min(T, D + B)
// by priority (the featured the first D), the fewest side-wall modules, every module but the last filled to its
// quotas, every unneeded side-wall socket decorated, no level bare, the levels the prefabs', no overlapping fixtures, at
// most 16 lights, every title placed once or unplaced for capacity, no hard finding of the validation. Timings are
// hidden [.bench] cases.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/validation.hpp>

#include "world_generation_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::Fixture;
using rv::world::FixtureId;
using rv::world::FixtureRole;
using rv::world::GenerationResult;
using rv::world::Section;
using rv::world::WorldPlan;
using rv::test::world::generateRoom;
using rv::test::world::makeSelection;
using rv::test::world::snesData;

namespace {

const std::filesystem::path kGoldenDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "golden";

/// The selections of the golden plans: one per size, the same for every world seed.
constexpr std::uint64_t kSelectionSeed = 20261018;

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN");
    return value != nullptr && std::string_view(value) == "1";
}

std::string planText(const WorldPlan& plan) {
    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    return std::move(*text);
}

/// The first line where two texts differ (1-based), for a readable failure.
std::size_t firstDifferentLine(std::string_view a, std::string_view b) {
    std::size_t line = 1;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] != b[i]) {
            return line;
        }
        line += a[i] == '\n' ? 1 : 0;
    }
    return line;
}

/// The section of each slot level, by fixture and level.
using SectionMap = std::map<std::pair<FixtureId, std::uint32_t>, Section>;

SectionMap sectionsOf(const WorldPlan& plan) {
    SectionMap sections;
    for (const Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            sections[{fixture.id, level.level}] = level.section;
        }
    }
    return sections;
}

std::set<GameId> gamesIn(const WorldPlan& plan, Section section) {
    const SectionMap sections = sectionsOf(plan);
    std::set<GameId> games;
    for (const rv::world::Placement& placement : plan.placements) {
        if (sections.at({placement.slot.fixture(), placement.slot.level()}) == section) {
            games.insert(placement.game);
        }
    }
    return games;
}

std::size_t countRole(const WorldPlan& plan, FixtureRole role) {
    return static_cast<std::size_t>(
        std::count_if(plan.fixtures.begin(), plan.fixtures.end(), [&](const Fixture& f) { return f.role == role; }));
}

const Fixture* fixtureAt(const WorldPlan& plan, std::string_view socket) {
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.socket() == socket && fixture.id.part().empty()) {
            return &fixture;
        }
    }
    return nullptr;
}

/// A fixture's footprint from its pivot, yaw and prefab bounds (axis-aligned yaws; others by the box's corners).
rv::world::Footprint footprintOf(const Fixture& fixture) {
    const rv::world::PrefabInfo* info = snesData().prefabs.find(fixture.prefab);
    REQUIRE(info != nullptr);
    std::int64_t minX = 0;
    std::int64_t maxX = 0;
    std::int64_t minZ = 0;
    std::int64_t maxZ = 0;
    switch (fixture.yawCdeg) {
    case 0:
        minX = info->minXMm, maxX = info->maxXMm, minZ = info->minZMm, maxZ = info->maxZMm;
        break;
    case 9000: // local +X to -Z, +Z to +X
        minX = info->minZMm, maxX = info->maxZMm, minZ = -info->maxXMm, maxZ = -info->minXMm;
        break;
    case 18000:
        minX = -info->maxXMm, maxX = -info->minXMm, minZ = -info->maxZMm, maxZ = -info->minZMm;
        break;
    case 27000:
        minX = -info->maxZMm, maxX = -info->minZMm, minZ = info->minXMm, maxZ = info->maxXMm;
        break;
    default: {
        // A turned hero corner part: the circle around its bounds.
        const std::int64_t radius = std::max({std::abs(info->minXMm), std::abs(info->maxXMm), std::abs(info->minZMm),
                                              std::abs(info->maxZMm)});
        minX = minZ = -radius;
        maxX = maxZ = radius;
    }
    }
    return {fixture.xMm + minX, fixture.zMm + minZ, fixture.xMm + maxX, fixture.zMm + maxZ};
}

/// Records a failed expectation of checkProperties() with its line and the subject being checked.
#define EXPECT(...)                                                                                       \
    do {                                                                                                  \
        if (!(__VA_ARGS__)) {                                                                             \
            failures.push_back(fmt::format("line {}: {} ({})", __LINE__, #__VA_ARGS__, where));          \
        }                                                                                                 \
    } while (false)

/// The properties every fresh plan keeps (the file's header), the plan generated from `selection`.
void checkProperties(const GenerationResult& result, const rv::world::RoomSelection& selection) {
    // Failures are gathered and reported together: a run checks some thousands of facts, and Catch2's
    // assertions would cost more than the generation they check.
    std::vector<std::string> failures;
    std::string where;
    const WorldPlan& plan = result.plan;
    const rv::world::WorldData& data = snesData();
    const rv::world::RoomTemplate& roomTemplate = data.roomTemplate;
    auto capacity = rv::world::collectionCapacity(roomTemplate, data.prefabs, data.mediaFamily);
    REQUIRE(capacity.has_value());
    const std::uint64_t titleCount = selection.titles.size();
    const rv::world::CollectionCounts counts = rv::world::countCollection(*capacity, titleCount, 150);
    REQUIRE(plan.rooms.size() == 1);
    const rv::world::Room& room = plan.rooms.front();
    EXPECT(room.growthSteps == counts.growthSteps);

    // The face-out titles are exactly the first min(T, D + B) by priority, the featured the first D.
    const std::uint64_t faceOut = std::min<std::uint64_t>(titleCount, capacity->displaySlots + capacity->bandSlots);
    const std::uint64_t featuredCount = std::min<std::uint64_t>(titleCount, capacity->displaySlots);
    std::set<GameId> firstFeatured;
    std::set<GameId> nextBand;
    for (std::uint64_t i = 0; i < faceOut; ++i) {
        (i < featuredCount ? firstFeatured : nextBand).insert(selection.titles[i].game);
    }
    EXPECT(gamesIn(plan, Section::Featured) == firstFeatured);
    EXPECT(gamesIn(plan, Section::Band) == nextBand);

    // Every title placed once, or unplaced for capacity (the last in priority order); the selection's unplaced kept.
    std::map<GameId, std::size_t> placed;
    for (const rv::world::Placement& placement : plan.placements) {
        ++placed[placement.game];
    }
    EXPECT(placed.size() == plan.placements.size());
    EXPECT(placed.size() == counts.placed);
    for (std::uint64_t i = 0; i < titleCount; ++i) {
        EXPECT(placed.contains(selection.titles[i].game) == (i < counts.placed));
    }
    std::size_t capacityEntries = 0;
    for (const rv::world::UnplacedGame& game : plan.unplaced) {
        capacityEntries += game.reason == rv::world::UnplacedReason::Capacity ? 1 : 0;
        EXPECT(!(placed.contains(game.game)));
    }
    EXPECT(capacityEntries == counts.overCapacity);
    EXPECT(plan.unplaced.size() == selection.unplaced.size() + counts.overCapacity);

    // The fewest side-wall modules that hold the spine-out titles with the reserve: one fewer would not.
    const std::size_t sideModules = countRole(plan, FixtureRole::SideModule);
    EXPECT(sideModules == counts.sideModules);
    const std::uint64_t needed = counts.spine + counts.reserve;
    if (counts.overCapacity == 0) {
        EXPECT(needed <= capacity->heroSpineSlots + sideModules * capacity->sideModuleSlots);
        if (sideModules > 0) {
            EXPECT(needed > capacity->heroSpineSlots + (sideModules - 1) * capacity->sideModuleSlots);
        }
    } else {
        EXPECT(sideModules == capacity->maxSideSockets());
    }
    EXPECT(countRole(plan, FixtureRole::HeroModule) == 2);
    EXPECT(countRole(plan, FixtureRole::Display) == 3);

    // The modules in the collection's order: every one but the last holds its quota on every spine-out level (one
    // quota for the section), the last no more; levels no title needs are dressed, never bare.
    std::map<std::pair<FixtureId, std::uint32_t>, std::uint32_t> perLevel;
    std::map<std::pair<FixtureId, std::uint32_t>, std::uint32_t> highestSlot;
    for (const rv::world::Placement& placement : plan.placements) {
        const auto key = std::pair{placement.slot.fixture(), placement.slot.level()};
        ++perLevel[key];
        highestSlot[key] = std::max(highestSlot[key], placement.slot.index());
    }
    std::map<std::pair<FixtureId, std::uint32_t>, const rv::world::DressedLevel*> dressed;
    for (const rv::world::DressedLevel& level : plan.dressing) {
        const auto key = std::pair{level.level.fixture(), level.level.level()};
        EXPECT(!(dressed.contains(key)));
        dressed[key] = &level;
    }
    std::vector<const Fixture*> modules;
    for (const std::string& socket : roomTemplate.collectionOrder) {
        if (const Fixture* module = fixtureAt(plan, socket);
            module != nullptr &&
            (module->role == FixtureRole::HeroModule || module->role == FixtureRole::SideModule)) {
            modules.push_back(module);
        }
    }
    EXPECT(modules.size() == 2 + sideModules);
    const auto spineTitles = [&](const Fixture& module) {
        std::uint32_t total = 0;
        for (const rv::world::FixtureLevel& level : module.levels) {
            if (level.section == Section::Spine) {
                total += perLevel[{module.id, level.level}];
            }
        }
        return total;
    };
    // The last module holding spine-out titles; those after it hold none (only the hero-wall modules can follow it).
    std::size_t last = 0;
    for (std::size_t m = 0; m < modules.size(); ++m) {
        if (spineTitles(*modules[m]) > 0) {
            last = m;
        }
    }
    if (sideModules > 0) {
        EXPECT(last == modules.size() - 1);
    }
    std::optional<std::uint32_t> quota;
    for (std::size_t m = 0; m < modules.size(); ++m) {
        for (const rv::world::FixtureLevel& level : modules[m]->levels) {
            if (level.section != Section::Spine) {
                continue;
            }
            const std::uint32_t count = perLevel[{modules[m]->id, level.level}];
            if (m < last) {
                EXPECT(count > 0);
                if (!quota) {
                    quota = count;
                }
                EXPECT(count == *quota);
            } else if (m > last || counts.spine == 0) {
                EXPECT(count == 0);
            } else if (quota) {
                EXPECT(count <= *quota);
            }
        }
    }
    if (quota) {
        EXPECT(*quota >= 28); // floor(33 / 1.15), or more when the quotas had to rise
    }
    for (const Fixture& fixture : plan.fixtures) {
        const rv::world::PrefabInfo* info = data.prefabs.find(fixture.prefab);
        REQUIRE(info != nullptr);
        const bool collection = fixture.role == FixtureRole::HeroModule || fixture.role == FixtureRole::SideModule ||
                                fixture.role == FixtureRole::Display;
        // The levels and their slots are the prefab's sockets.
        EXPECT(fixture.levels.size() == (collection ? info->levels.size() : 0));
        for (std::size_t i = 0; i < fixture.levels.size() && collection; ++i) {
            EXPECT(fixture.levels[i].level == info->levels[i].level);
            EXPECT(fixture.levels[i].slots == info->levels[i].slots);
        }
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            where = fixture.id.toString() + " level " + std::to_string(level.level);
            const auto key = std::pair{fixture.id, level.level};
            const std::uint32_t count = perLevel.contains(key) ? perLevel.at(key) : 0;
            // Titles stand from slot 0 without gaps; whatever follows them is dressed (face-out) or the reserve.
            if (count > 0) {
                EXPECT(highestSlot.at(key) + 1 == count);
            }
            const bool isDressed = dressed.contains(key);
            EXPECT((count > 0 || isDressed)); // no bare board
            if (isDressed) {
                EXPECT(dressed.at(key)->firstSlot == count);
                EXPECT(dressed.at(key)->firstSlot + dressed.at(key)->count == level.slots);
            }
            if (isDressed && count > 0) {
                EXPECT(rv::world::isFaceOut(level.section)); // a spine-out level keeps its free slots as the reserve
            }
        }
    }

    // Every side-wall socket of the room holds a module or decoration; no door-reserved socket holds anything.
    for (const rv::world::TemplateSocket& socket : roomTemplate.sockets) {
        where = socket.id;
        const Fixture* fixture = fixtureAt(plan, socket.id);
        if (socket.kind == rv::world::SocketKind::SideWall && socket.growthStep <= room.growthSteps) {
            REQUIRE(fixture != nullptr);
            EXPECT((fixture->role == FixtureRole::SideModule || fixture->role == FixtureRole::Decoration));
        }
        if (socket.kind == rv::world::SocketKind::DoorReserved ||
            (socket.kind == rv::world::SocketKind::SideWall && socket.growthStep > room.growthSteps)) {
            EXPECT(fixture == nullptr);
        }
    }

    // No two fixtures of different sockets overlap; each stands inside its socket (a hero corner part inside the
    // hero corner), clear of the doorway.
    const rv::world::Footprint doorway = rv::world::doorwayClearance(roomTemplate, room.growthSteps);
    for (std::size_t i = 0; i < plan.fixtures.size(); ++i) {
        const Fixture& a = plan.fixtures[i];
        const rv::world::TemplateSocket* socket = roomTemplate.socket(a.id.socket());
        REQUIRE(socket != nullptr);
        const rv::world::Footprint home = rv::world::socketFootprint(roomTemplate, *socket, room.growthSteps);
        const rv::world::Footprint footprint = footprintOf(a);
        where = a.id.toString();
        if (a.role == FixtureRole::HeroCorner) {
            EXPECT(home.contains({a.xMm, a.zMm, a.xMm, a.zMm}));
        } else {
            EXPECT(home.contains(footprint));
        }
        EXPECT(!(footprint.overlaps(doorway)));
        for (std::size_t j = i + 1; j < plan.fixtures.size(); ++j) {
            const Fixture& b = plan.fixtures[j];
            if (a.id.socket() != b.id.socket()) {
                EXPECT(!(footprint.overlaps(footprintOf(b))));
            }
        }
    }

    // The validation (I3 step 4) finds no hard rule broken.
    for (const rv::world::ValidationFinding& finding : result.validation.findings) {
        where = finding.rule + " " + finding.subject + ": " + finding.message;
        EXPECT(finding.severity == rv::world::Severity::Soft);
    }

    // Lights within the template's limit and the renderer's 16, one key.
    EXPECT(plan.lights.size() <= roomTemplate.lightLimit);
    EXPECT(plan.lights.size() <= rv::world::kMaxSceneLights);
    const auto keys = std::count_if(plan.lights.begin(), plan.lights.end(), [](const rv::world::PlanLight& light) {
        return light.role == rv::world::LightRole::Key;
    });
    EXPECT(keys == 1);
    for (std::size_t i = 0; i < std::min<std::size_t>(failures.size(), 20); ++i) {
        UNSCOPED_INFO(failures[i]);
    }
    CHECK(failures.empty());
}

#undef EXPECT

} // namespace

TEST_CASE("world/generation: golden plans of the reference sizes and two runs byte-identical", "[world][generation]") {
    struct Case {
        std::size_t titles;
        std::uint64_t seed;
    };
    std::vector<Case> cases;
    for (const std::size_t titles : {0, 1, 15, 50, 56, 57, 890, 2000}) {
        for (const std::uint64_t seed : {std::uint64_t{102938}, std::uint64_t{7}, std::uint64_t{0xFFFFFFFFFFFFFFFF}}) {
            cases.push_back({titles, seed});
        }
    }
    cases.push_back({8000, 102938});
    for (const Case& test : cases) {
        const std::string name = fmt::format("plan-{}-seed-{}.json", test.titles, test.seed);
        INFO(name);
        const std::string text = planText(generateRoom(test.titles, kSelectionSeed, test.seed).plan);
        CHECK(planText(generateRoom(test.titles, kSelectionSeed, test.seed).plan) == text);
        const std::filesystem::path golden = kGoldenDir / name;
        if (updatingGoldens()) {
            REQUIRE(rv::core::writeTextFileAtomic(golden, text).has_value());
            WARN("rewrote " << rv::core::pathToUtf8(golden));
            continue;
        }
        auto expected = rv::core::readTextFile(golden);
        REQUIRE(expected.has_value());
        INFO("first different line " << firstDifferentLine(*expected, text) << " (RV_UPDATE_GOLDEN=1 rewrites)");
        CHECK(text == *expected);
        // A golden plan reads back as the plan it is.
        auto read = rv::world::readPlan(*expected, name);
        REQUIRE(read.has_value());
        CHECK(planText(*read) == *expected);
    }
}

TEST_CASE("world/generation: every plan of the golden sizes passes the validation", "[world][generation]") {
    // Judged as worldplan check judges a plan: the prefabs' slots read from the manifests (manifestPrefabSlots), not the
    // generator's own measures; the soft findings are the room's as the template and the prefabs give it.
    const rv::world::WorldData& data = snesData();
    std::map<std::string, std::set<std::size_t>> softBySize;
    for (const std::size_t titles : {0, 1, 15, 50, 56, 57, 890, 2000, 8000}) {
        for (const std::uint64_t seed : {std::uint64_t{102938}, std::uint64_t{7}, std::uint64_t{0xFFFFFFFFFFFFFFFF}}) {
            if (titles == 8000 && seed != 102938) {
                continue;
            }
            INFO(titles << " titles, seed " << seed);
            const GenerationResult result = generateRoom(titles, kSelectionSeed, seed);
            rv::world::ValidationContext context;
            context.templates = {data.roomTemplate};
            context.config.seed = seed;
            context.selections = {makeSelection(titles, kSelectionSeed)};
            context.prefabSlots = rv::world::manifestPrefabSlots(rv::test::world::assetRoot());
            context.lights = {{result.plan.rooms.front().id, static_cast<std::uint32_t>(result.plan.lights.size())}};
            const rv::world::ValidationReport report = rv::world::validatePlan(result.plan, context);
            for (const rv::world::ValidationFinding& finding : report.findings) {
                if (finding.severity == rv::world::Severity::Hard) {
                    UNSCOPED_INFO(finding.rule << " " << finding.subject << ": " << finding.message);
                } else {
                    softBySize[finding.rule + " (" + finding.subject + "): " + finding.message].insert(titles);
                }
            }
            CHECK(report.passed());
            // The generator's own validation, on the slots it measured, finds the same.
            CHECK(result.validation.findings == report.findings);
            CHECK(report.layoutScorePermille.has_value());
        }
    }
    std::string soft;
    for (const auto& [finding, sizes] : softBySize) {
        soft += fmt::format("\n  {} [{} titles]", finding, fmt::join(sizes, ", "));
    }
    WARN("soft findings:" << (soft.empty() ? std::string(" none") : soft));
}

TEST_CASE("world/generation: the room at the reference sizes", "[world][generation]") {
    SECTION("no title: the shell furnished, every level dressed") {
        const GenerationResult result = generateRoom(0, kSelectionSeed, 1);
        const WorldPlan& plan = result.plan;
        CHECK(plan.placements.empty());
        CHECK(countRole(plan, FixtureRole::SideModule) == 0);
        CHECK(countRole(plan, FixtureRole::Decoration) >= 14); // every side-wall socket
        std::size_t levels = 0;
        for (const Fixture& fixture : plan.fixtures) {
            levels += fixture.levels.size();
        }
        CHECK(plan.dressing.size() == levels);
        checkProperties(result, makeSelection(0, kSelectionSeed));
    }
    SECTION("G02's 50 titles: all face-out, the hero band partly dressed, no side-wall module") {
        const GenerationResult result = generateRoom(50, kSelectionSeed, 102938);
        const WorldPlan& plan = result.plan;
        CHECK(gamesIn(plan, Section::Featured).size() == 44);
        CHECK(gamesIn(plan, Section::Band).size() == 6);
        CHECK(gamesIn(plan, Section::Spine).empty());
        CHECK(countRole(plan, FixtureRole::SideModule) == 0);
        // The band's west module first: hw-w's band level holds the six, hw-e's is dressed.
        const Fixture* west = fixtureAt(plan, "hw-w");
        REQUIRE(west != nullptr);
        for (const rv::world::Placement& placement : plan.placements) {
            if (placement.slot.fixture().socket() == "hw-e") {
                FAIL("a title on hw-e");
            }
        }
        checkProperties(result, makeSelection(50, kSelectionSeed));
    }
    SECTION("the reference library's 890: 56 face-out, three side-wall modules, the third half full") {
        const GenerationResult result = generateRoom(890, kSelectionSeed, 102938);
        const WorldPlan& plan = result.plan;
        CHECK(gamesIn(plan, Section::Featured).size() == 44);
        CHECK(gamesIn(plan, Section::Band).size() == 12);
        CHECK(gamesIn(plan, Section::Spine).size() == 834);
        CHECK(countRole(plan, FixtureRole::SideModule) == 3);
        // In socket order: w01, e01, w02; the other eleven side-wall sockets hold decoration.
        for (const std::string_view socket : {"w01", "e01", "w02"}) {
            const Fixture* module = fixtureAt(plan, socket);
            REQUIRE(module != nullptr);
            CHECK(module->role == FixtureRole::SideModule);
        }
        // The hero-wall modules hold 6 x 28 each, the first two side-wall modules 7 x 28, the third 106: levels 4, 5
        // and 3 at 28 (band order around 1.3 m), level 6 22; levels 0 to 2 dressed.
        std::map<std::string, std::size_t> perModule;
        std::map<std::uint32_t, std::size_t> w02Levels;
        for (const rv::world::Placement& placement : plan.placements) {
            ++perModule[placement.slot.fixture().socket()];
            if (placement.slot.fixture().socket() == "w02") {
                ++w02Levels[placement.slot.level()];
            }
        }
        CHECK(perModule["hw-w"] == 6 * 28 + 6);
        CHECK(perModule["hw-e"] == 6 * 28 + 6);
        CHECK(perModule["w01"] == 7 * 28);
        CHECK(perModule["e01"] == 7 * 28);
        CHECK(perModule["w02"] == 106);
        CHECK(w02Levels == std::map<std::uint32_t, std::size_t>{{3, 28}, {4, 28}, {5, 28}, {6, 22}});
        checkProperties(result, makeSelection(890, kSelectionSeed));
    }
    SECTION("2,000 titles: eight side-wall modules, every one full to its quota") {
        const GenerationResult result = generateRoom(2000, kSelectionSeed, 102938);
        CHECK(countRole(result.plan, FixtureRole::SideModule) == 8);
        CHECK(result.plan.rooms.front().growthSteps == 0);
        checkProperties(result, makeSelection(2000, kSelectionSeed));
    }
    SECTION("8,000 titles: the room grows to 16 m, the rest unplaced for capacity") {
        const GenerationResult result = generateRoom(8000, kSelectionSeed, 102938);
        const WorldPlan& plan = result.plan;
        const rv::world::Room& room = plan.rooms.front();
        CHECK(room.growthSteps == 3);
        CHECK(room.maxZMm - room.minZMm == 16000);
        CHECK(countRole(plan, FixtureRole::SideModule) == 26);
        CHECK(plan.placements.size() == 5615);
        checkProperties(result, makeSelection(8000, kSelectionSeed));
        // The south wall's decoration moved with the growth; the side-wall modules and the hero wall did not.
        const Fixture* w01 = fixtureAt(plan, "w01");
        REQUIRE(w01 != nullptr);
        const GenerationResult small = generateRoom(890, kSelectionSeed, 102938);
        const Fixture* w01Small = fixtureAt(small.plan, "w01");
        REQUIRE(w01Small != nullptr);
        CHECK(std::tie(w01->xMm, w01->zMm) == std::tie(w01Small->xMm, w01Small->zMm));
    }
}

TEST_CASE("world/generation: the display rule's properties over 1000 random sizes", "[world][generation]") {
    // A Debug build generates some ten times slower: it checks the first 60 of the same sizes.
#if defined(NDEBUG)
    constexpr int kRuns = 1000;
#else
    constexpr int kRuns = 60;
#endif
    rv::core::Random random(19937);
    for (int run = 0; run < kRuns; ++run) {
        // Mostly sizes the base room holds, some that grow it, a few past the template's limit.
        const std::uint64_t pick = random.below(100);
        const std::size_t titles = static_cast<std::size_t>(pick < 70   ? random.below(700)
                                                            : pick < 95 ? random.below(3300)
                                                                        : 3300 + random.below(3000));
        const std::uint64_t selectionSeed = random.next();
        const std::uint64_t worldSeed = random.next();
        INFO("run " << run << ": " << titles << " titles, selection " << selectionSeed << ", seed " << worldSeed);
        const GenerationResult result = generateRoom(titles, selectionSeed, worldSeed);
        checkProperties(result, makeSelection(titles, selectionSeed));
    }
}

TEST_CASE("world/generation: the result's reports and the plan's versions", "[world][generation]") {
    const GenerationResult result = generateRoom(57, kSelectionSeed, 102938);
    const WorldPlan& plan = result.plan;
    CHECK(plan.schemaVersion == rv::world::kPlanSchemaVersion);
    CHECK(plan.generatorVersion == rv::world::kGeneratorVersion);
    CHECK(plan.randomAlgorithm == "splitmix64/1");
    CHECK(plan.titleKeyRule == "title-key/1");
    CHECK(plan.seed == 102938);
    CHECK(plan.preset == "retro_museum_cozy");
    CHECK(rv::world::isSha256Hex(plan.inputDigest));
    CHECK(result.changes.outcome == rv::world::GenerationOutcome::Generated);
    CHECK(result.changes.rebuild == rv::world::RebuildLevel::Full);
    CHECK(result.changes.added.size() == 57);
    CHECK(std::is_sorted(result.changes.added.begin(), result.changes.added.end()));
    CHECK(result.changes.fixturesAdded.size() == plan.fixtures.size());
    CHECK(result.changes.digestBefore.empty());
    CHECK(result.changes.digestAfter == *rv::world::planDigest(plan));
    CHECK(result.validation.passed());

    // The input digest follows the inputs: another seed, another selection, another reserve.
    const std::string digest = plan.inputDigest;
    CHECK(generateRoom(57, kSelectionSeed, 102939).plan.inputDigest != digest);
    CHECK(generateRoom(57, kSelectionSeed + 1, 102938).plan.inputDigest != digest);
    // The default seed is the library's (ADR-0010 §7).
    rv::world::GenerationInput input;
    input.library = rv::test::world::library();
    input.selection = makeSelection(57, kSelectionSeed);
    auto byDefault = rv::world::generate(input, snesData());
    REQUIRE(byDefault.has_value());
    CHECK(byDefault->plan.seed == 0xa1fac9cfdf9c5474ull);
    // reduceMotion only shows the room: the same plan.
    input.config.reduceMotion = true;
    CHECK(*rv::world::generate(input, snesData()) == *byDefault);
}

TEST_CASE("world/generation: inputs it refuses", "[world][generation]") {
    rv::world::GenerationInput input;
    input.library = rv::test::world::library();
    input.selection = makeSelection(20, kSelectionSeed);

    rv::world::GenerationInput otherRoom = input;
    otherRoom.selection.room = *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse("nes"));
    for (rv::world::UnplacedGame& game : otherRoom.selection.unplaced) {
        game.room = otherRoom.selection.room;
    }
    auto refused = rv::world::generate(otherRoom, snesData());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(refused.error().message, ContainsSubstring("room:nes"));

    rv::world::GenerationInput unordered = input;
    std::reverse(unordered.selection.titles.begin(), unordered.selection.titles.end());
    CHECK(rv::world::generate(unordered, snesData()).error().code == rv::ErrorCode::InvalidArgument);

    rv::world::GenerationInput twice = input;
    twice.selection.unplaced.push_back(
        {twice.selection.titles.front().game, twice.selection.room, rv::world::UnplacedReason::NoCover, {}});
    std::sort(twice.selection.unplaced.begin(), twice.selection.unplaced.end());
    refused = rv::world::generate(twice, snesData());
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error().message, ContainsSubstring("is a title and unplaced"));

    rv::world::GenerationInput badConfig = input;
    badConfig.config.growthReservePermille = 1001;
    CHECK(rv::world::generate(badConfig, snesData()).error().code == rv::ErrorCode::InvalidArgument);

    // A module that does not fit its socket.
    rv::world::WorldData narrow = snesData();
    for (rv::world::TemplateSocket& socket : narrow.roomTemplate.sockets) {
        if (socket.id == "w01") {
            socket.widthMm = 800;
        }
    }
    input.selection = makeSelection(900, kSelectionSeed);
    refused = rv::world::generate(input, narrow);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::ValidationFailed);
    CHECK_THAT(refused.error().message, ContainsSubstring("does not fit socket w01"));
}

TEST_CASE("world/generation: the time of a fresh plan of 2000 titles", "[.bench][world][generation]") {
    using Clock = std::chrono::steady_clock;
    const auto loadStart = Clock::now();
    auto data = rv::world::loadWorldData(rv::test::world::assetRoot(), rv::test::world::snes());
    const double loadMs = std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count();
    REQUIRE(data.has_value());
    rv::world::GenerationInput input;
    input.library = rv::test::world::library();
    input.selection = makeSelection(2000, kSelectionSeed);
    input.config.seed = 102938;
    std::vector<double> times;
    for (int run = 0; run < 21; ++run) {
        const auto start = Clock::now();
        auto result = rv::world::generate(input, *data);
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        REQUIRE(result.has_value());
    }
    std::sort(times.begin(), times.end());
    // What the plan's canonical text costs of it (writePlan, as planDigest writes it).
    const WorldPlan plan = rv::world::generate(input, *data)->plan;
    std::vector<double> writes;
    for (int run = 0; run < 21; ++run) {
        const auto start = Clock::now();
        REQUIRE(rv::world::writePlan(plan).has_value());
        writes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(writes.begin(), writes.end());
    WARN(fmt::format("world data read in {:.1f} ms; generate(2000 titles): median {:.2f} ms, max {:.2f} ms; "
                     "writePlan median {:.2f} ms",
                     loadMs, times[times.size() / 2], times.back(), writes[writes.size() / 2]));
#if defined(NDEBUG)
    CHECK(times[times.size() / 2] <= 200.0); // the budget is Release's (docs/roadmap.md, I3's budgets)
#endif
}
