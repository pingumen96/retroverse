// The incremental update (rv/world/incremental.hpp, ADR-0010 §10, NFR-08; I3 step 9): an unchanged catalog changes
// nothing; NFR-08's property suite over random sequences of additions, removals, games gone missing and priority
// changes (20 seeds with 15, 50, 890 and 2,000 titles in Release, fewer in Debug): a game added gives one placement
// more or one unplaced entry, every earlier placement byte-identical, the new title in the spine-out section next to its
// alphabetical predecessor, the face-out set unchanged, every updated plan valid; across growth; a full section taking
// a dressed level, then a side-wall module in place of decoration, then growth, then capacity; the representatives
// chosen once, aliases, pins, RequiresExplicitRegeneration and newer plans; every plan of 1,000 seeds at 4 sizes valid;
// golden update sequences (tests/fixtures/world/golden/update-*.json, rewritten only with RV_UPDATE_GOLDEN=1). Timings
// are hidden [.bench] cases.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_store.hpp>

#include "support/temp_dir.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::Fixture;
using rv::world::FixtureRole;
using rv::world::GenerationOutcome;
using rv::world::GenerationResult;
using rv::world::Placement;
using rv::world::RebuildLevel;
using rv::world::RoomSelection;
using rv::world::RoomTitle;
using rv::world::Section;
using rv::world::UnplacedGame;
using rv::world::UnplacedReason;
using rv::world::WorldPlan;
using rv::test::world::addTitle;
using rv::test::world::addUnplaced;
using rv::test::world::countFixtures;
using rv::test::world::countSpine;
using rv::test::world::faceOutPlacements;
using rv::test::world::gameValuesOf;
using rv::test::world::generateSelection;
using rv::test::world::levelSections;
using rv::test::world::makeSelection;
using rv::test::world::planText;
using rv::test::world::SpineCounts;
using rv::test::world::snesData;
using rv::test::world::subsetSelection;
using rv::test::world::updateRoom;

namespace {

constexpr std::uint64_t kSelectionSeed = 20261018;
constexpr std::uint64_t kWorldSeed = 102938;

const std::filesystem::path kGoldenDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "golden";

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN");
    return value != nullptr && std::string_view(value) == "1";
}

/// The titles of a selection that no other version names as its representative.
std::vector<GameId> titlesWithoutVersions(const RoomSelection& selection) {
    std::set<GameId> represented;
    for (const UnplacedGame& game : selection.unplaced) {
        if (game.reason == UnplacedReason::DuplicateTitle) {
            represented.insert(game.representative);
        }
    }
    std::vector<GameId> titles;
    for (const RoomTitle& title : selection.titles) {
        if (!represented.contains(title.game)) {
            titles.push_back(title.game);
        }
    }
    return titles;
}

/// `selection` without the title `game` and its other versions.
void removeTitle(RoomSelection& selection, const GameId& game) {
    std::erase_if(selection.titles, [&](const RoomTitle& title) { return title.game == game; });
    std::erase_if(selection.unplaced, [&](const UnplacedGame& unplaced) {
        return unplaced.reason == UnplacedReason::DuplicateTitle && unplaced.representative == game;
    });
}

std::optional<Section> sectionOfSlot(const WorldPlan& plan, const rv::world::SlotId& slot) {
    const auto sections = levelSections(plan);
    const auto found = sections.find({slot.fixture(), slot.level()});
    return found != sections.end() ? std::optional(found->second) : std::nullopt;
}

/// Records a failed expectation with its line and what was being checked.
#define EXPECT(...)                                                                                       \
    do {                                                                                                  \
        if (!(__VA_ARGS__)) {                                                                             \
            failures.push_back(fmt::format("line {}: {} ({})", __LINE__, #__VA_ARGS__, where));          \
        }                                                                                                 \
    } while (false)

/// What every update keeps (the file's header), `result` the update of `previous` to a selection.
void checkUpdate(const WorldPlan& previous, const GenerationResult& result, std::vector<std::string>& failures,
                 const std::string& where) {
    const WorldPlan& plan = result.plan;
    EXPECT(result.changes.outcome == GenerationOutcome::Updated);
    EXPECT(result.validation.passed());
    for (const rv::world::ValidationFinding& finding : result.validation.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            failures.push_back(fmt::format("{}: hard finding {} {}: {}", where, finding.rule, finding.subject,
                                           finding.message));
        }
    }
    // Every earlier placement byte-identical.
    EXPECT(std::includes(plan.placements.begin(), plan.placements.end(), previous.placements.begin(),
                         previous.placements.end()));
    EXPECT(result.changes.kept == previous.placements.size());
    // The face-out set unchanged.
    EXPECT(faceOutPlacements(plan) == faceOutPlacements(previous));
    // The new placements stand spine-out and are the games the report adds.
    std::vector<Placement> added;
    std::set_difference(plan.placements.begin(), plan.placements.end(), previous.placements.begin(),
                        previous.placements.end(), std::back_inserter(added));
    std::vector<GameId> addedGames;
    for (const Placement& placement : added) {
        EXPECT(sectionOfSlot(plan, placement.slot) == Section::Spine);
        addedGames.push_back(placement.game);
    }
    std::sort(addedGames.begin(), addedGames.end());
    EXPECT(addedGames == result.changes.added);
    // Nothing that stood moved: a fixture of the previous plan is in the new one as it was, but for what a growth
    // moves (the south wall's) and the decoration a new module replaced.
    std::map<rv::world::FixtureId, const Fixture*> fixtures;
    for (const Fixture& fixture : plan.fixtures) {
        fixtures.emplace(fixture.id, &fixture);
    }
    for (const Fixture& fixture : previous.fixtures) {
        const auto found = fixtures.find(fixture.id);
        if (fixture.role == FixtureRole::Decoration) {
            continue;
        }
        EXPECT(found != fixtures.end());
        if (found != fixtures.end() && !std::binary_search(result.changes.fixturesMoved.begin(),
                                                           result.changes.fixturesMoved.end(), fixture.id)) {
            EXPECT(*found->second == fixture);
        }
    }
    // The plan reads back as itself, and its digest is the report's.
    auto text = rv::world::writePlan(plan);
    EXPECT(text.has_value());
    if (text) {
        EXPECT(rv::core::sha256Hex(*text) == result.changes.digestAfter);
        auto read = rv::world::readPlan(*text, "updated");
        EXPECT(read.has_value() && *read == plan);
    }
}

void report(const std::vector<std::string>& failures) {
    for (std::size_t i = 0; i < std::min<std::size_t>(failures.size(), 20); ++i) {
        UNSCOPED_INFO(failures[i]);
    }
    CHECK(failures.empty());
}

/// Sort and title keys by GameId.
using KeyMap = std::map<GameId, std::pair<std::string, std::string>>;

/// The spine-out placement just before `title` in the alphabet among those the plan places spine-out (keys from
/// `keys`).
std::optional<Placement> predecessorOf(const WorldPlan& plan, const KeyMap& keys, const RoomTitle& title) {
    const auto sections = levelSections(plan);
    std::optional<std::tuple<std::string, std::string, GameId>> best;
    std::optional<Placement> found;
    const auto wanted = std::tuple(title.sortKey, title.titleKey, title.game);
    for (const Placement& placement : plan.placements) {
        const auto section = sections.find({placement.slot.fixture(), placement.slot.level()});
        const auto key = keys.find(placement.game);
        if (section == sections.end() || section->second != Section::Spine || key == keys.end()) {
            continue;
        }
        const auto candidate = std::tuple(key->second.first, key->second.second, placement.game);
        if (candidate < wanted && (!best || *best < candidate)) {
            best = candidate;
            found = placement;
        }
    }
    return found;
}

/// Whether the level of `slot` has a slot after it that no title takes and no dressing covers.
bool freeAfter(const WorldPlan& plan, const rv::world::SlotId& slot) {
    std::uint32_t slots = 0;
    for (const Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            if (fixture.id == slot.fixture() && level.level == slot.level()) {
                slots = level.slots;
            }
        }
    }
    std::vector<bool> used(slots, false);
    for (const Placement& placement : plan.placements) {
        if (placement.slot.fixture() == slot.fixture() && placement.slot.level() == slot.level()) {
            used[placement.slot.index()] = true;
        }
    }
    for (const rv::world::DressedLevel& dressed : plan.dressing) {
        if (dressed.level == slot.levelId()) {
            for (std::uint32_t i = dressed.firstSlot; i < dressed.firstSlot + dressed.count && i < slots; ++i) {
                used[i] = true;
            }
        }
    }
    for (std::uint32_t i = slot.index() + 1; i < slots; ++i) {
        if (!used[i]) {
            return true;
        }
    }
    return false;
}

KeyMap keysOf(const RoomSelection& selection) {
    KeyMap keys;
    for (const RoomTitle& title : selection.titles) {
        keys[title.game] = {title.sortKey, title.titleKey};
    }
    return keys;
}

void checkGolden(std::string_view name, const WorldPlan& plan) {
    const std::string text = planText(plan);
    const std::filesystem::path golden = kGoldenDir / rv::core::pathFromUtf8(std::string(name));
    if (updatingGoldens()) {
        REQUIRE(rv::core::writeTextFileAtomic(golden, text).has_value());
        WARN("rewrote " << rv::core::pathToUtf8(golden));
        return;
    }
    auto expected = rv::core::readTextFile(golden);
    REQUIRE(expected.has_value());
    CHECK(text == *expected);
}

} // namespace

TEST_CASE("world/update: an unchanged catalog changes nothing", "[world][update]") {
    for (const std::size_t titles : {0, 1, 15, 50, 56, 57, 890, 2000}) {
        INFO(titles << " titles");
        const RoomSelection selection = makeSelection(titles, kSelectionSeed);
        const GenerationResult fresh = generateSelection(selection, kWorldSeed);
        const GenerationResult updated = updateRoom(fresh.plan, selection, kWorldSeed);
        CHECK(updated.changes.outcome == GenerationOutcome::Updated);
        CHECK(updated.changes.rebuild == RebuildLevel::NoRebuild);
        CHECK(updated.changes.added.empty());
        CHECK(updated.changes.unplaced.empty());
        CHECK(updated.changes.fixturesAdded.empty());
        CHECK(updated.changes.kept == fresh.plan.placements.size());
        CHECK(updated.plan == fresh.plan);
        CHECK(planText(updated.plan) == planText(fresh.plan));
        CHECK(updated.changes.digestBefore == fresh.changes.digestAfter);
        CHECK(updated.changes.digestAfter == fresh.changes.digestAfter);
        CHECK(updated.validation.passed());
    }
}

TEST_CASE("world/update: NFR-08 over random sequences of additions, removals and changes", "[world][update]") {
    // A Debug build updates some ten times slower: fewer seeds, sizes and steps of the same sequences.
#if defined(NDEBUG)
    constexpr int kSeeds = 20;
    constexpr int kSteps = 12;
    const std::vector<std::size_t> sizes{15, 50, 890, 2000};
#else
    constexpr int kSeeds = 2;
    constexpr int kSteps = 5;
    const std::vector<std::size_t> sizes{15, 50, 890};
#endif
    std::vector<std::string> failures;
    for (int seed = 0; seed < kSeeds; ++seed) {
        for (const std::size_t size : sizes) {
            const std::uint64_t selectionSeed = rv::core::deriveSeed("update-suite", seed, size);
            const std::uint64_t worldSeed = rv::core::deriveSeed("update-world", seed);
            rv::core::Random random(rv::core::deriveSeed("update-steps", seed, size));
            const RoomSelection full = makeSelection(size, selectionSeed);
            std::set<std::uint64_t> taken = gameValuesOf(full);
            // Hold back up to 8 titles without other versions: they come back one at a time.
            std::vector<GameId> held = titlesWithoutVersions(full);
            random.shuffle(std::span<GameId>(held));
            held.resize(std::min<std::size_t>(held.size(), 8));
            const std::set<GameId> heldSet(held.begin(), held.end());
            RoomSelection current = subsetSelection(full, [&](const GameId& game) { return !heldSet.contains(game); });
            WorldPlan plan = generateSelection(current, worldSeed).plan;
            for (int step = 0; step < kSteps; ++step) {
                const std::string where = fmt::format("seed {}, {} titles, step {}", seed, size, step);
                const std::uint64_t action = random.below(100);
                const WorldPlan before = plan;
                std::optional<RoomTitle> addedTitle;
                bool addsOneGame = false;
                bool movesNothing = false;
                if (action < 40) {
                    // A title arrives: a held one, or a new one.
                    if (!held.empty()) {
                        const GameId game = held.back();
                        held.pop_back();
                        for (const RoomTitle& title : full.titles) {
                            if (title.game == game) {
                                current.titles.insert(std::upper_bound(current.titles.begin(), current.titles.end(),
                                                                       title, rv::world::priorityLess),
                                                      title);
                                addedTitle = title;
                            }
                        }
                    } else {
                        addedTitle = addTitle(current, random, taken, random.below(4) == 0, random.below(50));
                    }
                    addsOneGame = true;
                } else if (action < 55) {
                    // Another version of a title the room holds: a duplicate.
                    if (!current.titles.empty()) {
                        const RoomTitle& title = current.titles[random.below(current.titles.size())];
                        addUnplaced(current, random, taken, UnplacedReason::DuplicateTitle, title.game);
                        addsOneGame = true;
                    }
                } else if (action < 65) {
                    addUnplaced(current, random, taken, UnplacedReason::NoCover);
                    addsOneGame = true;
                } else if (action < 80) {
                    // A title leaves the catalog, or its file goes missing: its placement is kept.
                    if (!current.titles.empty()) {
                        const GameId game = current.titles[random.below(current.titles.size())].game;
                        removeTitle(current, game);
                        if (random.below(2) == 0) {
                            UnplacedGame missing{game, current.room, UnplacedReason::Unavailable, {}};
                            current.unplaced.insert(
                                std::upper_bound(current.unplaced.begin(), current.unplaced.end(), missing), missing);
                        }
                        movesNothing = true;
                    }
                } else {
                    // A title becomes a favourite and the most played: it moves nowhere.
                    if (!current.titles.empty()) {
                        RoomTitle title = current.titles[random.below(current.titles.size())];
                        std::erase(current.titles, title);
                        title.favorite = true;
                        title.playCount = 100000 + random.below(1000);
                        current.titles.insert(std::upper_bound(current.titles.begin(), current.titles.end(), title,
                                                               rv::world::priorityLess),
                                              title);
                        movesNothing = true;
                    }
                }
                const GenerationResult result = updateRoom(before, current, worldSeed);
                checkUpdate(before, result, failures, where);
                if (addsOneGame) {
                    // One placement more, or one unplaced entry more (with its reason), nothing else.
                    const std::size_t entriesBefore = before.placements.size() + before.unplaced.size();
                    const std::size_t entriesAfter = result.plan.placements.size() + result.plan.unplaced.size();
                    EXPECT(entriesAfter == entriesBefore + 1);
                    EXPECT(result.plan.placements.size() <= before.placements.size() + 1);
                }
                if (movesNothing) {
                    EXPECT(result.plan.placements == before.placements);
                    EXPECT(result.changes.added.empty());
                }
                if (addedTitle && result.plan.placements.size() == before.placements.size() + 1) {
                    // Next to its alphabetical predecessor when a slot is free there.
                    const auto keys = keysOf(current);
                    const auto predecessor = predecessorOf(before, keys, *addedTitle);
                    const auto placed = std::find_if(result.plan.placements.begin(), result.plan.placements.end(),
                                                     [&](const Placement& p) { return p.game == addedTitle->game; });
                    EXPECT(placed != result.plan.placements.end());
                    if (predecessor && placed != result.plan.placements.end() && freeAfter(before, predecessor->slot)) {
                        EXPECT(placed->slot.levelId() == predecessor->slot.levelId());
                        EXPECT(placed->slot.index() > predecessor->slot.index());
                    }
                }
                plan = result.plan;
            }
        }
    }
    report(failures);
}

TEST_CASE("world/update: across growth, the room grows south and moves nothing else", "[world][update]") {
#if defined(NDEBUG)
    constexpr int kSeeds = 3;
#else
    constexpr int kSeeds = 1;
#endif
    std::vector<std::string> failures;
    for (int seed = 0; seed < kSeeds; ++seed) {
        // The base room holds 3,205 titles with their reserve: 3,200 of them (14 side-wall modules and a reserve of
        // 480), then 5 batches of 120, past the reserve.
        const RoomSelection full = makeSelection(3800, rv::core::deriveSeed("growth-suite", seed));
        std::vector<GameId> held = titlesWithoutVersions(full);
        rv::core::Random random(rv::core::deriveSeed("growth-steps", seed));
        random.shuffle(std::span<GameId>(held));
        REQUIRE(held.size() >= 600);
        held.resize(600);
        std::set<GameId> heldSet(held.begin(), held.end());
        RoomSelection current = subsetSelection(full, [&](const GameId& game) { return !heldSet.contains(game); });
        WorldPlan plan = generateSelection(current, kWorldSeed).plan;
        REQUIRE(plan.rooms.front().growthSteps == 0);
        REQUIRE(countFixtures(plan, FixtureRole::SideModule) == 14);
        bool grew = false;
        for (int batch = 0; batch < 5; ++batch) {
            const std::string where = fmt::format("seed {}, batch {}", seed, batch);
            for (int i = 0; i < 120; ++i) {
                heldSet.erase(held[static_cast<std::size_t>(batch * 120 + i)]);
            }
            current = subsetSelection(full, [&](const GameId& game) { return !heldSet.contains(game); });
            const GenerationResult result = updateRoom(plan, current, kWorldSeed);
            checkUpdate(plan, result, failures, where);
            EXPECT(result.changes.added.size() == 120);
            if (result.plan.rooms.front().growthSteps > plan.rooms.front().growthSteps) {
                grew = true;
                EXPECT(result.changes.rebuild == RebuildLevel::RoomExpansion);
                EXPECT(result.plan.rooms.front().maxZMm == plan.rooms.front().maxZMm + 2000);
                // What stands on the south wall moved by the step, and only that.
                for (const rv::world::FixtureId& id : result.changes.fixturesMoved) {
                    const rv::world::TemplateSocket* socket = snesData().roomTemplate.socket(id.socket());
                    EXPECT(socket != nullptr && socket->wall == rv::world::Wall::South);
                }
                EXPECT(!result.changes.fixturesMoved.empty());
                EXPECT(countFixtures(result.plan, FixtureRole::SideModule) ==
                       countFixtures(plan, FixtureRole::SideModule) + 1);
            }
            plan = result.plan;
        }
        const std::string where = fmt::format("seed {}", seed);
        EXPECT(grew);
        EXPECT(plan.rooms.front().growthSteps == 1);
    }
    report(failures);
}

TEST_CASE("world/update: a full section takes a dressed level, then a module, then growth, then capacity",
          "[world][update]") {
    rv::core::Random random(4711);
    RoomSelection selection = makeSelection(890, kSelectionSeed);
    std::set<std::uint64_t> taken = gameValuesOf(selection);
    WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    const auto addTitles = [&](std::uint64_t count) {
        for (std::uint64_t i = 0; i < count; ++i) {
            addTitle(selection, random, taken);
        }
        const GenerationResult result = updateRoom(plan, selection, kWorldSeed);
        std::vector<std::string> failures;
        checkUpdate(plan, result, failures, fmt::format("{} titles added", count));
        report(failures);
        REQUIRE(result.changes.added.size() == count);
        return result;
    };

    // The reserve first: the free slots of the used levels, no dressing touched, no fixture added.
    SpineCounts counts = countSpine(plan);
    REQUIRE(counts.freeSlots > 0);
    REQUIRE(counts.dressedSlots > 0);
    GenerationResult result = addTitles(counts.freeSlots);
    CHECK(result.changes.dressingRemoved.empty());
    CHECK(result.changes.fixturesAdded.empty());
    CHECK(result.changes.rebuild == RebuildLevel::LocalReflow);
    plan = result.plan;
    CHECK(countSpine(plan).freeSlots == 0);

    // Then a dressed level: its dressing removed, no fixture added.
    result = addTitles(1);
    CHECK(result.changes.dressingRemoved.size() == 1);
    CHECK(result.changes.fixturesAdded.empty());
    plan = result.plan;

    // Every dressed slot taken, still in the three side-wall modules; then a fourth module in the next side-wall
    // socket (e02), in place of its decoration, every level dressed but the one the title took.
    counts = countSpine(plan);
    result = addTitles(counts.freeSlots + counts.dressedSlots);
    CHECK(result.changes.fixturesAdded.empty());
    CHECK(countFixtures(result.plan, FixtureRole::SideModule) == 3);
    plan = result.plan;
    REQUIRE(countSpine(plan).freeSlots + countSpine(plan).dressedSlots == 0);
    result = addTitles(1);
    CHECK(countFixtures(result.plan, FixtureRole::SideModule) == 4);
    CHECK(result.changes.fixturesAdded == std::vector{*rv::world::FixtureId::parse("room:snes/fx:e02")});
    CHECK(result.changes.fixturesRemoved == std::vector{*rv::world::FixtureId::parse("room:snes/fx:e02")});
    CHECK(result.changes.dressingRemoved.size() == 1);
    CHECK(result.changes.rebuild == RebuildLevel::LocalReflow);
    // The wall-washers were placed again over the modules as they stand.
    CHECK(result.plan.lights != plan.lights);
    CHECK(result.plan.lights.size() <= rv::world::kMaxSceneLights);
    plan = result.plan;

    // The base room full (3,205 titles: every side-wall socket with a module) and every free slot taken: then the room
    // grows by a step, the south wall's fixtures moved, the step's first socket given a module.
    selection = makeSelection(3205, kSelectionSeed);
    taken = gameValuesOf(selection);
    plan = generateSelection(selection, kWorldSeed).plan;
    REQUIRE(countFixtures(plan, FixtureRole::SideModule) == 14);
    counts = countSpine(plan);
    result = addTitles(counts.freeSlots + counts.dressedSlots);
    CHECK(result.plan.rooms.front().growthSteps == 0);
    CHECK(result.changes.fixturesAdded.empty());
    plan = result.plan;
    result = addTitles(1);
    CHECK(result.changes.rebuild == RebuildLevel::RoomExpansion);
    CHECK(result.plan.rooms.front().growthSteps == 1);
    CHECK(countFixtures(result.plan, FixtureRole::SideModule) == 15);
    CHECK_FALSE(result.changes.fixturesMoved.empty());
    // The step's other three side-wall sockets hold decoration, as a fresh room's unneeded sockets do.
    CHECK(result.changes.fixturesAdded.size() == 4);

    // At the template's limit (5,615 titles: 16 m, 26 side-wall modules) every free slot taken: then capacity.
    selection = makeSelection(5615, kSelectionSeed);
    taken = gameValuesOf(selection);
    plan = generateSelection(selection, kWorldSeed).plan;
    REQUIRE(countFixtures(plan, FixtureRole::SideModule) == 26);
    counts = countSpine(plan);
    result = addTitles(counts.freeSlots + counts.dressedSlots);
    plan = result.plan;
    CHECK(plan.rooms.front().growthSteps == 3);
    const RoomTitle last = addTitle(selection, random, taken);
    result = updateRoom(plan, selection, kWorldSeed);
    CHECK(result.changes.added.empty());
    CHECK(result.changes.unplaced == std::vector{last.game});
    const auto entry = std::find_if(result.plan.unplaced.begin(), result.plan.unplaced.end(),
                                    [&](const UnplacedGame& game) { return game.game == last.game; });
    REQUIRE(entry != result.plan.unplaced.end());
    CHECK(entry->reason == UnplacedReason::Capacity);
    CHECK(result.validation.passed());
    // A title unplaced for capacity stays so: the next update does not place it, even with a slot freed.
    CHECK(updateRoom(result.plan, selection, kWorldSeed).plan == result.plan);
}

TEST_CASE("world/update: new titles stand spine-out even while display slots are free", "[world][update]") {
    // 15 titles: 29 display slots free, every spine-out level dressed. A new title, even the favourite, takes the
    // first dressed level of the hero-wall modules in fill order (hw-w's level at eye height).
    RoomSelection selection = makeSelection(15, kSelectionSeed);
    std::set<std::uint64_t> taken = gameValuesOf(selection);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    rv::core::Random random(1);
    const RoomTitle favourite = addTitle(selection, random, taken, true, 999);
    REQUIRE(selection.titles.front().game == favourite.game); // first by priority
    const GenerationResult result = updateRoom(plan, selection, kWorldSeed);
    REQUIRE(result.changes.added == std::vector{favourite.game});
    const auto placed = std::find_if(result.plan.placements.begin(), result.plan.placements.end(),
                                     [&](const Placement& p) { return p.game == favourite.game; });
    REQUIRE(placed != result.plan.placements.end());
    CHECK(sectionOfSlot(result.plan, placed->slot) == Section::Spine);
    CHECK(placed->slot.toString() == "room:snes/fx:hw-w/s:4.0");
    CHECK(result.changes.dressingRemoved.size() == 1);
    CHECK(faceOutPlacements(result.plan) == faceOutPlacements(plan));
    // A second title next to the first in the alphabet lands on that level's free slots.
    const RoomTitle second = addTitle(selection, random, taken);
    const GenerationResult again = updateRoom(result.plan, selection, kWorldSeed);
    REQUIRE(again.changes.added == std::vector{second.game});
    CHECK(again.changes.dressingRemoved.empty());
    for (const Placement& p : again.plan.placements) {
        if (p.game == second.game) {
            CHECK(p.slot.levelId().toString() == "room:snes/fx:hw-w/l:4");
        }
    }
}

TEST_CASE("world/update: priorities change nothing; G10's 10 games added to G02's 50", "[world][update]") {
    SECTION("a new favourite and more plays move nothing") {
        RoomSelection selection = makeSelection(890, kSelectionSeed);
        const GenerationResult fresh = generateSelection(selection, kWorldSeed);
        // The last title by priority, a spine-out one, becomes the first.
        RoomTitle title = selection.titles.back();
        selection.titles.pop_back();
        title.favorite = true;
        title.playCount = 1000000;
        selection.titles.insert(selection.titles.begin(), title);
        const GenerationResult updated = updateRoom(fresh.plan, selection, kWorldSeed);
        CHECK(updated.plan.placements == fresh.plan.placements);
        CHECK(updated.plan.fixtures == fresh.plan.fixtures);
        CHECK(updated.plan.dressing == fresh.plan.dressing);
        CHECK(updated.changes.rebuild == RebuildLevel::NoRebuild);
        CHECK(updated.plan.inputDigest != fresh.plan.inputDigest); // the inputs it read are others
        // A regeneration chooses the face-out set again: the favourite stands on a display fixture.
        const GenerationResult regenerated = generateSelection(selection, kWorldSeed);
        bool featured = false;
        for (const Placement& placement : faceOutPlacements(regenerated.plan)) {
            featured = featured || placement.game == title.game;
        }
        CHECK(featured);
    }
    SECTION("G10: 10 games added to G02's 50, the face-out set unchanged, the 10 spine-out") {
        const RoomSelection full = makeSelection(60, kSelectionSeed);
        std::vector<GameId> held = titlesWithoutVersions(full);
        rv::core::Random random(10);
        random.shuffle(std::span<GameId>(held));
        held.resize(10);
        const std::set<GameId> heldSet(held.begin(), held.end());
        const RoomSelection base = subsetSelection(full, [&](const GameId& game) { return !heldSet.contains(game); });
        REQUIRE(base.titles.size() == 50);
        const WorldPlan g02 = generateSelection(base, kWorldSeed).plan;
        const GenerationResult g10 = updateRoom(g02, full, kWorldSeed);
        std::vector<std::string> failures;
        checkUpdate(g02, g10, failures, "G10");
        report(failures);
        CHECK(g10.changes.added.size() == 10);
        CHECK(countSpine(g10.plan).titles == 10);
        CHECK(faceOutPlacements(g10.plan).size() == 50);
        checkGolden("update-50-plus-10-seed-102938.json", g10.plan);
    }
}

TEST_CASE("world/update: games gone missing, removed or without a cover keep their slots", "[world][update]") {
    const RoomSelection selection = makeSelection(890, kSelectionSeed);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    // Three placed titles without other versions: one now missing (MissingFile), one gone from the catalog, one
    // without its cover.
    const std::vector<GameId> titles = titlesWithoutVersions(selection);
    const GameId missing = titles[3];
    const GameId gone = titles[100];
    const GameId uncovered = titles[500];
    RoomSelection now = selection;
    for (const GameId& game : {missing, gone, uncovered}) {
        removeTitle(now, game);
    }
    for (const auto& [game, reason] : {std::pair{missing, UnplacedReason::Unavailable},
                                       std::pair{uncovered, UnplacedReason::NoCover}}) {
        const UnplacedGame entry{game, now.room, reason, {}};
        now.unplaced.insert(std::upper_bound(now.unplaced.begin(), now.unplaced.end(), entry), entry);
    }
    const GenerationResult result = updateRoom(plan, now, kWorldSeed);
    CHECK(result.plan.placements == plan.placements);
    CHECK(result.changes.rebuild == RebuildLevel::NoRebuild);
    CHECK(result.validation.passed());
    const rv::world::PlanIndex index(result.plan);
    for (const GameId& game : {missing, gone, uncovered}) {
        const auto location = index.find(game);
        REQUIRE(location.has_value());
        CHECK(location->placed());
    }
    // ... and the title of the gone game coming back later is that game again: nothing changes.
    CHECK(updateRoom(result.plan, selection, kWorldSeed).plan.placements == plan.placements);
}

TEST_CASE("world/update: a title's representative is chosen once", "[world][update]") {
    RoomSelection selection = makeSelection(890, kSelectionSeed);
    std::set<std::uint64_t> taken = gameValuesOf(selection);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    rv::core::Random random(5);

    SECTION("a better version arriving later is a duplicate of the placed one") {
        // Title T (placed) gets a better version B: the selection now names B the title and T its duplicate, with
        // T's other version D pointing at B.
        const RoomTitle title = selection.titles[300];
        RoomSelection now = selection;
        const UnplacedGame other = addUnplaced(now, random, taken, UnplacedReason::DuplicateTitle, title.game);
        const GameId better = rv::test::world::freshGameId(random, taken);
        for (RoomTitle& t : now.titles) {
            if (t.game == title.game) {
                t.game = better;
            }
        }
        for (UnplacedGame& game : now.unplaced) {
            if (game.representative == title.game) {
                game.representative = better;
            }
        }
        const UnplacedGame demoted{title.game, now.room, UnplacedReason::DuplicateTitle, better};
        now.unplaced.insert(std::upper_bound(now.unplaced.begin(), now.unplaced.end(), demoted), demoted);
        std::sort(now.unplaced.begin(), now.unplaced.end());
        std::sort(now.titles.begin(), now.titles.end(), rv::world::priorityLess);
        const GenerationResult result = updateRoom(plan, now, kWorldSeed);
        CHECK(result.plan.placements == plan.placements);
        CHECK(result.validation.passed());
        const rv::world::PlanIndex index(result.plan);
        CHECK(index.find(title.game)->placed());
        CHECK(index.find(better)->reason == UnplacedReason::DuplicateTitle);
        CHECK(index.find(better)->representative == title.game);
        CHECK(index.find(other.game)->representative == title.game);
    }
    SECTION("a duplicate whose placed representative left stays its duplicate") {
        // Q is a version of the placed P; P's file goes missing and the selection makes Q the title.
        std::optional<UnplacedGame> version;
        for (const UnplacedGame& game : selection.unplaced) {
            if (game.reason == UnplacedReason::DuplicateTitle && !version) {
                version = game;
            }
        }
        REQUIRE(version.has_value());
        RoomSelection now = selection;
        for (RoomTitle& title : now.titles) {
            if (title.game == version->representative) {
                title.game = version->game;
            }
        }
        std::erase_if(now.unplaced, [&](const UnplacedGame& game) { return game.game == version->game; });
        for (UnplacedGame& game : now.unplaced) {
            if (game.representative == version->representative) {
                game.representative = version->game;
            }
        }
        const UnplacedGame missing{version->representative, now.room, UnplacedReason::Unavailable, {}};
        now.unplaced.insert(std::upper_bound(now.unplaced.begin(), now.unplaced.end(), missing), missing);
        std::sort(now.unplaced.begin(), now.unplaced.end());
        const GenerationResult result = updateRoom(plan, now, kWorldSeed);
        CHECK(result.plan.placements == plan.placements);
        CHECK(result.validation.passed());
        const rv::world::PlanIndex index(result.plan);
        CHECK(index.find(version->representative)->placed());
        CHECK(index.find(version->game)->reason == UnplacedReason::DuplicateTitle);
        CHECK(index.find(version->game)->representative == version->representative);
    }
}

TEST_CASE("world/update: aliases hand their slots over or free them", "[world][update]") {
    const RoomSelection selection = makeSelection(890, kSelectionSeed);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    std::set<std::uint64_t> taken = gameValuesOf(selection);
    rv::core::Random random(9);
    const std::vector<Placement> faceOut = faceOutPlacements(plan);
    const Placement spine = *std::find_if(plan.placements.begin(), plan.placements.end(), [&](const Placement& p) {
        return sectionOfSlot(plan, p.slot) == Section::Spine;
    });

    // A spine-out game became an alias of an identity the plan does not hold: that identity takes its slot.
    RoomSelection now = selection;
    const GameId identity = rv::test::world::freshGameId(random, taken);
    for (RoomTitle& title : now.titles) {
        if (title.game == spine.game) {
            title.game = identity;
        }
    }
    for (UnplacedGame& game : now.unplaced) {
        if (game.representative == spine.game) {
            game.representative = identity;
        }
    }
    // A face-out game became an alias of a game the plan places already: its slot is freed and dressed.
    const Placement freed = faceOut.front();
    const Placement keeper = faceOut.back();
    removeTitle(now, freed.game);
    std::sort(now.titles.begin(), now.titles.end(), rv::world::priorityLess);
    const GenerationResult result =
        updateRoom(plan, now, kWorldSeed, {}, {{spine.game, identity}, {freed.game, keeper.game}});
    CHECK(result.validation.passed());
    const rv::world::PlanIndex index(result.plan);
    REQUIRE(index.find(identity).has_value());
    CHECK(index.find(identity)->place->slot == spine.slot);
    CHECK_FALSE(index.find(spine.game).has_value());
    CHECK_FALSE(index.gameAt(freed.slot).has_value());
    CHECK(result.changes.slotsFreed == std::vector{freed.slot});
    CHECK(result.changes.added == std::vector{identity});
    const bool dressed = std::any_of(result.plan.dressing.begin(), result.plan.dressing.end(), [&](const auto& d) {
        return d.level == freed.slot.levelId() && d.firstSlot == freed.slot.index() && d.count == 1;
    });
    CHECK(dressed);
    // The placement handed over keeps its offset; the rest stands as it stood.
    CHECK(result.changes.kept == plan.placements.size() - 2);
}

TEST_CASE("world/update: pins are kept, orphans reported, a change that would move one refused", "[world][update]") {
    RoomSelection selection = makeSelection(890, kSelectionSeed);
    std::set<std::uint64_t> taken = gameValuesOf(selection);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    rv::core::Random random(77);
    rv::world::WorldOverrides overrides;
    const Placement pinned = plan.placements[200];
    overrides.gamePins.push_back({pinned.game, pinned.slot});
    overrides.gamePins.push_back(
        {*GameId::fromValue(0x123456789), *rv::world::SlotId::parse("room:snes/fx:w01/s:0.0")});
    // e02, the next side-wall socket for a module, holds a poster: pinned.
    const Fixture* e02 = nullptr;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.socket() == "e02") {
            e02 = &fixture;
        }
    }
    REQUIRE(e02 != nullptr);
    REQUIRE(e02->role == FixtureRole::Decoration);
    overrides.fixturePins.push_back({e02->id, e02->prefab});
    overrides.fixturePins.push_back({*rv::world::FixtureId::parse("room:snes/fx:nowhere"), std::nullopt});

    // Fill the three modules: the next module goes to w03, not e02, and the change is reported.
    SpineCounts counts = countSpine(plan);
    for (std::uint64_t i = 0; i < counts.freeSlots + counts.dressedSlots + 1; ++i) {
        addTitle(selection, random, taken);
    }
    const GenerationResult result = updateRoom(plan, selection, kWorldSeed, overrides);
    CHECK(result.validation.passed());
    CHECK(result.changes.fixturesAdded == std::vector{*rv::world::FixtureId::parse("room:snes/fx:w03")});
    REQUIRE(result.changes.needsConfirmation.size() == 1);
    CHECK_THAT(result.changes.needsConfirmation.front(), ContainsSubstring("socket e02"));
    CHECK(result.changes.orphanedPins.size() == 2);
    const rv::world::PlanIndex index(result.plan);
    CHECK(index.find(pinned.game)->place->slot == pinned.slot);
    const bool posterStands = std::any_of(result.plan.fixtures.begin(), result.plan.fixtures.end(),
                                          [&](const Fixture& fixture) { return fixture == *e02; });
    CHECK(posterStands);

    // A pinned fixture on the south wall: the room does not grow; the titles that need it are unplaced for capacity.
    const RoomSelection big = makeSelection(3205, kSelectionSeed);
    const WorldPlan full = generateSelection(big, kWorldSeed).plan;
    rv::world::WorldOverrides south;
    for (const Fixture& fixture : full.fixtures) {
        const rv::world::TemplateSocket* socket = snesData().roomTemplate.socket(fixture.id.socket());
        if (socket != nullptr && socket->wall == rv::world::Wall::South && south.fixturePins.empty()) {
            south.fixturePins.push_back({fixture.id, std::nullopt});
        }
    }
    REQUIRE_FALSE(south.fixturePins.empty());
    RoomSelection more = big;
    std::set<std::uint64_t> bigTaken = gameValuesOf(big);
    counts = countSpine(full);
    for (std::uint64_t i = 0; i < counts.freeSlots + counts.dressedSlots + 3; ++i) {
        addTitle(more, random, bigTaken);
    }
    const GenerationResult held = updateRoom(full, more, kWorldSeed, south);
    CHECK(held.validation.passed());
    CHECK(held.plan.rooms.front().growthSteps == 0);
    CHECK(held.changes.unplaced.size() == 3);
    REQUIRE(held.changes.needsConfirmation.size() == 1);
    CHECK_THAT(held.changes.needsConfirmation.front(), ContainsSubstring("growth"));
    // Without the pin the same update grows the room.
    CHECK(updateRoom(full, more, kWorldSeed).plan.rooms.front().growthSteps == 1);
}

TEST_CASE("world/update: another seed, preset, template or generator major needs an explicit regeneration",
          "[world][update]") {
    const RoomSelection selection = makeSelection(57, kSelectionSeed);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    const std::string digest = *rv::world::planDigest(plan);
    const auto refused = [&](const GenerationResult& result, std::string_view why,
                             const std::optional<WorldPlan>& kept = std::nullopt) {
        CHECK(result.changes.outcome == GenerationOutcome::RequiresExplicitRegeneration);
        CHECK(result.plan == kept.value_or(plan));
        const std::string keptDigest = kept ? *rv::world::planDigest(*kept) : digest;
        CHECK(result.changes.digestBefore == keptDigest);
        CHECK(result.changes.digestAfter == keptDigest);
        CHECK(result.changes.added.empty());
        CHECK_THAT(result.changes.regenerationReason, ContainsSubstring(std::string(why)));
    };
    refused(updateRoom(plan, selection, kWorldSeed + 1), "world seed");

    rv::world::UpdateInput input;
    input.library = rv::test::world::library();
    input.selection = selection;
    input.config.seed = kWorldSeed;
    input.previous = plan;
    input.config.preset = "another_preset";
    refused(*rv::world::update(input, snesData()), "preset");
    input.config.preset = std::string(rv::world::kDefaultPreset);

    rv::world::WorldData otherTemplate = snesData();
    otherTemplate.roomTemplate.version += 1;
    refused(*rv::world::update(input, otherTemplate), "template");

    input.previous.generatorVersion = {0, 9};
    refused(*rv::world::update(input, snesData()), "generator", input.previous);

    // A plan of a newer generator or schema is refused outright, as are a plan of another library or room.
    input.previous = plan;
    input.previous.generatorVersion = {rv::world::kGeneratorVersion.major, rv::world::kGeneratorVersion.minor + 1};
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::Unsupported);
    input.previous = plan;
    input.previous.schemaVersion = rv::world::kPlanSchemaVersion + 1;
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::Unsupported);
    input.previous = plan;
    input.library = *rv::catalog::LibraryId::parse("other");
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::InvalidArgument);
    input.library = rv::test::world::library();
    input.previous.rooms.clear();
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::InvalidArgument);
    // A selection readRoomSelection() would refuse: titles out of priority order, a game twice, a stray representative.
    input.previous = plan;
    std::reverse(input.selection.titles.begin(), input.selection.titles.end());
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::InvalidArgument);
    input.selection = selection;
    input.selection.titles.push_back(input.selection.titles.back());
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::InvalidArgument);
    input.selection = selection;
    REQUIRE_FALSE(input.selection.unplaced.empty());
    for (UnplacedGame& game : input.selection.unplaced) {
        if (game.reason != UnplacedReason::DuplicateTitle) {
            game.representative = input.selection.titles.front().game;
        }
    }
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::InvalidArgument);
    input.selection = selection;
    // The prefabs' slots changed (a manifest edited): the slots the placements name may not exist.
    input.previous = plan;
    rv::world::WorldData otherSlots = snesData();
    for (rv::world::PrefabInfo& info : otherSlots.prefabs.prefabs) {
        if (info.ref == otherSlots.roomTemplate.heroModule) {
            info.levels.front().slots += 1;
        }
    }
    refused(*rv::world::update(input, otherSlots), "levels");
}

TEST_CASE("world/update: every plan of 1000 seeds at four catalog sizes is valid", "[world][update]") {
    // The generator's output checked by the validation as worldplan check judges it (the manifests' slots).
#if defined(NDEBUG)
    constexpr int kSeedsPerSize = 250;
#else
    constexpr int kSeedsPerSize = 4;
#endif
    rv::world::ValidationContext context;
    context.templates = {snesData().roomTemplate};
    context.prefabSlots = rv::world::manifestPrefabSlots(rv::test::world::assetRoot());
    std::vector<std::string> failures;
    for (const std::size_t size : {15, 50, 890, 2000}) {
        const RoomSelection selection = makeSelection(size, kSelectionSeed + size);
        context.selections = {selection};
        for (int i = 0; i < kSeedsPerSize; ++i) {
            const std::uint64_t seed = rv::core::deriveSeed("validity", size, i);
            const GenerationResult result = generateSelection(selection, seed);
            context.config.seed = seed;
            context.lights = {{result.plan.rooms.front().id, static_cast<std::uint32_t>(result.plan.lights.size())}};
            const rv::world::ValidationReport validation = rv::world::validatePlan(result.plan, context);
            for (const rv::world::ValidationFinding& finding : validation.findings) {
                if (finding.severity == rv::world::Severity::Hard) {
                    failures.push_back(fmt::format("{} titles, seed {}: {} {}: {}", size, seed, finding.rule,
                                                   finding.subject, finding.message));
                }
            }
        }
    }
    report(failures);
}

TEST_CASE("world/update: golden update sequences", "[world][update]") {
    // 890 titles, then five arrive and three leave (their slots kept), then a better version of a title and an alias:
    // the plan after each step byte for byte.
    const RoomSelection full = makeSelection(893, kSelectionSeed);
    std::vector<GameId> held = titlesWithoutVersions(full);
    rv::core::Random random(893);
    random.shuffle(std::span<GameId>(held));
    held.resize(5);
    std::set<GameId> heldSet(held.begin(), held.end());
    RoomSelection current = subsetSelection(full, [&](const GameId& game) { return !heldSet.contains(game); });
    WorldPlan plan = generateSelection(current, kWorldSeed).plan;
    current = full;
    const std::vector<GameId> leaving{current.titles[10].game, current.titles[400].game, current.titles[800].game};
    for (const GameId& game : leaving) {
        removeTitle(current, game);
    }
    GenerationResult result = updateRoom(plan, current, kWorldSeed);
    CHECK(result.changes.added.size() == 5);
    checkGolden("update-890-plus-5-minus-3-seed-102938.json", result.plan);

    // 3,205 titles fill the base room's side-wall sockets: their reserve and dressed levels taken, 20 more grow it by
    // a step.
    const RoomSelection big = makeSelection(3205, kSelectionSeed);
    std::set<std::uint64_t> taken = gameValuesOf(big);
    plan = generateSelection(big, kWorldSeed).plan;
    const rv::test::world::SpineCounts counts = countSpine(plan);
    RoomSelection more = big;
    for (std::uint64_t i = 0; i < counts.freeSlots + counts.dressedSlots + 20; ++i) {
        addTitle(more, random, taken);
    }
    result = updateRoom(plan, more, kWorldSeed);
    CHECK(result.changes.rebuild == RebuildLevel::RoomExpansion);
    CHECK(result.plan.rooms.front().growthSteps == 1);
    checkGolden("update-3205-full-plus-20-seed-102938.json", result.plan);
}

TEST_CASE("world/update: the time of 2000 titles plus one", "[.bench][world][update]") {
    using Clock = std::chrono::steady_clock;
    RoomSelection selection = makeSelection(2000, kSelectionSeed);
    const WorldPlan plan = generateSelection(selection, kWorldSeed).plan;
    std::set<std::uint64_t> taken = gameValuesOf(selection);
    rv::core::Random random(2001);
    addTitle(selection, random, taken);
    rv::world::UpdateInput input;
    input.library = rv::test::world::library();
    input.selection = selection;
    input.config.seed = kWorldSeed;
    input.previous = plan;
    input.previousDigest = *rv::world::planDigest(plan);
    std::vector<double> times;
    std::optional<GenerationResult> last;
    for (int run = 0; run < 21; ++run) {
        const auto start = Clock::now();
        auto result = rv::world::update(input, snesData());
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        REQUIRE(result.has_value());
        REQUIRE(result->changes.added.size() == 1);
        last = std::move(*result);
    }
    std::sort(times.begin(), times.end());
    // What its parts cost, as generate pays them: the input digest and the plan's digest with the writers' checks by
    // the reader (the update takes both without them: plan_parts.hpp), the validation.
    const auto median = [](auto&& work) {
        std::vector<double> runs;
        for (int run = 0; run < 11; ++run) {
            const auto start = Clock::now();
            work();
            runs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        std::sort(runs.begin(), runs.end());
        return runs[runs.size() / 2];
    };
    const rv::world::GenerationInput generation{input.library, input.selection, input.config, {}};
    const double digestMs = median([&] { REQUIRE(rv::world::generationInputDigest(generation, snesData())); });
    const double textMs = median([&] { REQUIRE(rv::world::planDigest(last->plan)); });
    rv::world::ValidationContext context;
    context.templates = {snesData().roomTemplate};
    context.selections = {selection};
    const double validationMs = median([&] { (void)rv::world::validatePlan(last->plan, context); });
    WARN(fmt::format("checked input digest {:.2f} ms, checked plan digest {:.2f} ms, validation {:.2f} ms", digestMs,
                     textMs, validationMs));
    // The plan written atomically into a world folder (each write after a backup of the one before).
    const rv::test::TempDir dir("rv-world-update-bench");
    const rv::world::WorldFiles files = rv::world::WorldFiles::in(dir.file("world"));
    auto lock = rv::world::WorldLock::acquire(files);
    REQUIRE(lock.has_value());
    std::vector<double> writes;
    for (int run = 0; run < 21; ++run) {
        GenerationResult result = *last;
        result.plan.seed = static_cast<std::uint64_t>(run); // another text each time
        const auto start = Clock::now();
        REQUIRE(rv::world::savePlan(*lock, result).has_value());
        writes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(writes.begin(), writes.end());
    // Where-is: the index of the plan.
    std::vector<double> indexes;
    for (int run = 0; run < 21; ++run) {
        const auto start = Clock::now();
        const rv::world::PlanIndex index(last->plan);
        indexes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        REQUIRE(index.placedCount() == 2001);
    }
    std::sort(indexes.begin(), indexes.end());
    WARN(fmt::format("update(2000 + 1): median {:.2f} ms, max {:.2f} ms; savePlan median {:.2f} ms, max {:.2f} ms; "
                     "PlanIndex median {:.3f} ms",
                     times[times.size() / 2], times.back(), writes[writes.size() / 2], writes.back(),
                     indexes[indexes.size() / 2]));
#if defined(NDEBUG)
    CHECK(times[times.size() / 2] <= 50.0);  // the budgets are Release's (docs/roadmap.md, I3's budgets)
    CHECK(writes[writes.size() / 2] <= 50.0);
#endif
}
