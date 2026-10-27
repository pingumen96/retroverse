// The world's explicit actions (rv/world/world_snapshots.hpp, ADR-0015 §6; I4b step 1): saveOverrides() under the
// world lock, the plan and the overrides committed together behind a paired snapshot, snapshot.json read back, an
// action interrupted at each point rolled back at the next lock (the plan and the overrides never disagree), the last
// 20 snapshots kept, undo restoring both files byte for byte and walking back, undo of undo refused, a dry run writing
// nothing, a damaged snapshot refused. The CLI's undo on a fixture world is test_cli_worldplan_undo.cpp's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/time.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "support/temp_dir.hpp"
#include "world_generation_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::world::GenerationResult;
using rv::world::SnapshotKind;
using rv::world::WorldAction;
using rv::world::WorldFiles;
using rv::world::WorldLock;
using rv::world::WorldOverrides;
using rv::world::WorldSnapshot;
using rv::world::detail::TransactionStop;

namespace {

/// A file's bytes as text, or nothing when it is not there.
std::optional<std::string> contentOf(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    if (!text) {
        REQUIRE(text.error().code == rv::ErrorCode::NotFound);
        return std::nullopt;
    }
    return *text;
}

/// The world's three files that change together (the plan, its sidecar, the overrides), as they are.
struct WorldState {
    std::optional<std::string> plan;
    std::optional<std::string> meta;
    std::optional<std::string> overrides;

    bool operator==(const WorldState&) const = default;
};

WorldState stateOf(const WorldFiles& files) {
    return WorldState{contentOf(files.plan), contentOf(files.meta), contentOf(files.overrides)};
}

/// A clock that ticks one second per call from 2026-10-27T10:00:00Z.
std::function<std::chrono::system_clock::time_point()> ticking() {
    auto next = std::make_shared<std::chrono::system_clock::time_point>(
        rv::core::parseUtcIso8601("2026-10-27T10:00:00.000Z").value());
    return [next] {
        const auto now = *next;
        *next += std::chrono::seconds(1);
        return now;
    };
}

GenerationResult resultOf(std::size_t titles) {
    return rv::test::world::generateRoom(titles, 4242, 1);
}

/// Overrides of `count` game pins of `result`'s plan, made by the editor, sorted as the reader sorts them.
WorldOverrides pinsOf(const GenerationResult& result, std::size_t count) {
    WorldOverrides overrides;
    for (std::size_t i = 0; i < count && i < result.plan.placements.size(); ++i) {
        const rv::world::Placement& placement = result.plan.placements[i];
        overrides.gamePins.push_back({placement.game,
                                      placement.slot,
                                      {rv::world::PinOrigin::Editor, "2026-10-27T09:00:00.000Z"}});
    }
    std::sort(overrides.gamePins.begin(), overrides.gamePins.end(),
              [](const rv::world::GamePin& a, const rv::world::GamePin& b) { return a.game < b.game; });
    return overrides;
}

/// A world with a plan of `titles` titles saved, and the options of the actions that follow.
struct World {
    rv::test::TempDir dir{"rv-world-snapshots"};
    WorldFiles files = WorldFiles::in(dir.file("world"));
    rv::world::SaveOptions options;

    explicit World(std::size_t titles) {
        options.writer = "a test";
        options.clock = ticking();
        auto lock = WorldLock::acquire(files);
        REQUIRE(lock.has_value());
        REQUIRE(rv::world::savePlan(*lock, resultOf(titles), options).has_value());
    }
};

} // namespace

TEST_CASE("world/snapshots: overrides saved under the world lock", "[world][snapshots]") {
    const World world(30);
    auto lock = WorldLock::acquire(world.files);
    REQUIRE(lock.has_value());
    // A world without overrides keeps no file.
    auto none = rv::world::saveOverrides(*lock, WorldOverrides{});
    REQUIRE(none.has_value());
    CHECK_FALSE(none->written);
    CHECK(none->digest.empty());
    CHECK_FALSE(std::filesystem::exists(world.files.overrides));
    // Pins: written atomically, read back; the same again writes nothing.
    const WorldOverrides pins = pinsOf(resultOf(30), 3);
    auto saved = rv::world::saveOverrides(*lock, pins);
    REQUIRE(saved.has_value());
    CHECK(saved->written);
    CHECK(contentOf(world.files.overrides) == *rv::world::writeOverrides(pins));
    CHECK(saved->digest == rv::core::sha256Hex(*contentOf(world.files.overrides)));
    CHECK(rv::world::loadOverrides(world.files.overrides) == pins);
    CHECK_FALSE(rv::world::saveOverrides(*lock, pins)->written);
    // Every pin removed: the file says so (it stays).
    CHECK(rv::world::saveOverrides(*lock, WorldOverrides{})->written);
    CHECK(rv::world::loadOverrides(world.files.overrides)->empty());
    // Overrides the reader would refuse, and a lock moved from.
    WorldOverrides twice = pins;
    twice.gamePins.push_back(twice.gamePins.front());
    CHECK(rv::world::saveOverrides(*lock, twice).error().code == rv::ErrorCode::InvalidArgument);
    WorldLock moved = std::move(*lock);
    CHECK(rv::world::saveOverrides(*lock, pins).error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("world/snapshots: the plan and the overrides committed together behind a snapshot", "[world][snapshots]") {
    World world(30);
    const WorldState before = stateOf(world.files);
    auto lock = WorldLock::acquire(world.files);
    REQUIRE(lock.has_value());
    const GenerationResult next = resultOf(31);
    const WorldOverrides pins = pinsOf(next, 2);
    auto committed = rv::world::commitWorld(*lock, WorldAction{"edit", "room:snes"}, next, pins, world.options);
    REQUIRE(committed.has_value());
    CHECK(committed->written);
    REQUIRE(committed->snapshot.has_value());
    const WorldSnapshot& snapshot = *committed->snapshot;
    CHECK(snapshot.name == "20261027T100001.000Z-edit");
    CHECK(snapshot.sequence == 1);
    CHECK(snapshot.kind == SnapshotKind::Action);
    CHECK(snapshot.action == "edit");
    CHECK(snapshot.scope == "room:snes");
    CHECK(snapshot.writer == "a test");
    CHECK(snapshot.takenAt == "2026-10-27T10:00:01.000Z");
    CHECK(snapshot.planDigest == rv::core::sha256Hex(*before.plan));
    CHECK(snapshot.metaDigest == rv::core::sha256Hex(*before.meta));
    CHECK(snapshot.overridesDigest.empty()); // there was no overrides.json
    CHECK_FALSE(snapshot.undone);
    // The world holds both new files; the snapshot the old ones, byte for byte, and its record.
    CHECK(contentOf(world.files.plan) == *rv::world::writePlan(next.plan));
    CHECK(contentOf(world.files.overrides) == *rv::world::writeOverrides(pins));
    CHECK(committed->plan.written);
    CHECK(committed->overrides.written);
    const std::filesystem::path folder = world.files.snapshots / snapshot.name;
    CHECK(contentOf(folder / "world-plan.json") == before.plan);
    CHECK(contentOf(folder / "world-plan.meta.json") == before.meta);
    CHECK_FALSE(std::filesystem::exists(folder / "overrides.json"));
    const auto record = rv::world::readSnapshotRecord(*contentOf(folder / "snapshot.json"), "snapshot.json");
    REQUIRE(record.has_value());
    WorldSnapshot named = *record;
    named.name = snapshot.name;
    CHECK(named == snapshot);
    CHECK(rv::world::writeSnapshotRecord(*record) == *contentOf(folder / "snapshot.json"));
    CHECK_FALSE(std::filesystem::exists(world.files.transaction));
    CHECK(rv::world::listSnapshots(world.files) == std::vector<WorldSnapshot>{snapshot});
    CHECK(rv::world::nextUndo(rv::world::listSnapshots(world.files)) == snapshot);

    // The same plan and overrides again: nothing written, no snapshot.
    auto again = rv::world::commitWorld(*lock, WorldAction{"edit", "room:snes"}, next, pins, world.options);
    REQUIRE(again.has_value());
    CHECK_FALSE(again->written);
    CHECK_FALSE(again->snapshot.has_value());
    CHECK(rv::world::listSnapshots(world.files).size() == 1);

    // Refused before anything is written: an action's name, a plan that breaks a hard rule, overrides the reader
    // would refuse.
    const WorldState committedState = stateOf(world.files);
    CHECK(rv::world::commitWorld(*lock, WorldAction{"Edit!", ""}, next, pins).error().code ==
          rv::ErrorCode::InvalidArgument);
    GenerationResult broken = resultOf(32);
    broken.validation.findings.push_back(
        rv::world::ValidationFinding{"a-rule", rv::world::Severity::Hard, "room:snes", "broken on purpose"});
    CHECK(rv::world::commitWorld(*lock, WorldAction{"edit", ""}, broken, pins).error().code ==
          rv::ErrorCode::ValidationFailed);
    WorldOverrides twice = pins;
    twice.gamePins.push_back(twice.gamePins.front());
    CHECK(rv::world::commitWorld(*lock, WorldAction{"edit", ""}, next, twice).error().code ==
          rv::ErrorCode::InvalidArgument);
    CHECK(stateOf(world.files) == committedState);
    CHECK(rv::world::listSnapshots(world.files).size() == 1);
}

TEST_CASE("world/snapshots: an action interrupted between its writes is rolled back at the next lock",
          "[world][snapshots]") {
    for (const TransactionStop stop :
         {TransactionStop::AfterJournal, TransactionStop::BetweenWrites, TransactionStop::BeforeJournalEnds}) {
        INFO("stop " << static_cast<int>(stop));
        World world(30);
        {
            auto lock = WorldLock::acquire(world.files);
            REQUIRE(lock.has_value());
            REQUIRE(rv::world::saveOverrides(*lock, pinsOf(resultOf(30), 1)).has_value());
        }
        const WorldState before = stateOf(world.files);
        std::string snapshotName;
        {
            auto lock = WorldLock::acquire(world.files);
            REQUIRE(lock.has_value());
            const GenerationResult next = resultOf(31);
            const WorldOverrides pins = pinsOf(next, 4);
            auto interrupted = rv::world::detail::commitWorldStoppingAt(*lock, WorldAction{"edit", "room:snes"}, next,
                                                                        pins, world.options, stop);
            REQUIRE_FALSE(interrupted.has_value());
            CHECK(interrupted.error().code == rv::ErrorCode::Cancelled);
            REQUIRE(std::filesystem::exists(world.files.transaction));
            const WorldState torn = stateOf(world.files);
            if (stop == TransactionStop::BetweenWrites) {
                // The crash this guards against: the new overrides beside the old plan.
                CHECK(torn.overrides == *rv::world::writeOverrides(pins));
                CHECK(torn.plan == before.plan);
            } else if (stop == TransactionStop::BeforeJournalEnds) {
                CHECK(torn.plan == *rv::world::writePlan(next.plan));
            }
            snapshotName = rv::world::listSnapshots(world.files).front().name;
        } // the process "ends": its lock is released
        auto lock = WorldLock::acquire(world.files);
        REQUIRE(lock.has_value());
        CHECK(lock->rolledBack() == snapshotName);
        CHECK(stateOf(world.files) == before);
        CHECK_FALSE(std::filesystem::exists(world.files.transaction));
        // The action never happened: its snapshot is gone, nothing to undo.
        CHECK(rv::world::listSnapshots(world.files).empty());
        CHECK(rv::world::undoLastAction(*lock).error().code == rv::ErrorCode::NotFound);
    }

    // Interrupted before its journal: nothing changed and nothing is rolled back (the snapshot, of the state as it
    // stands, stays); a snapshot half made is removed by the next lock.
    World world(30);
    const WorldState before = stateOf(world.files);
    {
        auto lock = WorldLock::acquire(world.files);
        REQUIRE(lock.has_value());
        CHECK(rv::world::detail::commitWorldStoppingAt(*lock, WorldAction{"edit", ""}, resultOf(31), {},
                                                       world.options, TransactionStop::AfterSnapshot)
                  .error()
                  .code == rv::ErrorCode::Cancelled);
    }
    std::filesystem::create_directories(world.files.snapshots / "20261027T120000.000Z-edit.partial");
    REQUIRE(rv::core::writeTextFileAtomic(world.files.snapshots / "20261027T120000.000Z-edit.partial" /
                                              "world-plan.json",
                                          "half")
                .has_value());
    auto lock = WorldLock::acquire(world.files);
    REQUIRE(lock.has_value());
    CHECK_FALSE(lock->rolledBack().has_value());
    CHECK(stateOf(world.files) == before);
    CHECK_FALSE(std::filesystem::exists(world.files.snapshots / "20261027T120000.000Z-edit.partial"));
    CHECK(rv::world::listSnapshots(world.files).size() == 1);

    // A journal that names no snapshot: nothing is written into the world until it is repaired.
    REQUIRE(rv::core::writeTextFileAtomic(world.files.transaction,
                                          R"({"schemaVersion": 1, "snapshot": "nowhere", "action": "edit"})")
                .has_value());
    { const WorldLock released = std::move(*lock); }
    auto refused = WorldLock::acquire(world.files);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::NotFound);
}

TEST_CASE("world/snapshots: the last 20 snapshots are kept", "[world][snapshots]") {
    World world(20);
    auto lock = WorldLock::acquire(world.files);
    REQUIRE(lock.has_value());
    std::vector<std::string> names;
    for (std::size_t i = 0; i < 25; ++i) {
        auto committed =
            rv::world::commitWorld(*lock, WorldAction{"edit", ""}, resultOf(21 + i), {}, world.options);
        REQUIRE(committed.has_value());
        REQUIRE(committed->snapshot.has_value());
        names.push_back(committed->snapshot->name);
        CHECK(committed->snapshotsRemoved ==
              (i < rv::world::kSnapshotsKept ? std::vector<std::string>{}
                                             : std::vector<std::string>{names[i - rv::world::kSnapshotsKept]}));
    }
    const std::vector<WorldSnapshot> kept = rv::world::listSnapshots(world.files);
    REQUIRE(kept.size() == rv::world::kSnapshotsKept);
    CHECK(kept.front().name == names.back()); // newest first
    CHECK(kept.front().sequence == 25);
    CHECK(kept.back().name == names[5]);
    CHECK(kept.back().sequence == 6);
    CHECK_FALSE(std::filesystem::exists(world.files.snapshots / names[4]));
}

TEST_CASE("world/snapshots: undo restores the plan and the overrides byte for byte, never an undo",
          "[world][snapshots]") {
    World world(30);
    const WorldState first = stateOf(world.files); // a plan, no overrides
    auto lock = WorldLock::acquire(world.files);
    REQUIRE(lock.has_value());
    // Nothing to undo yet.
    CHECK(rv::world::undoLastAction(*lock).error().code == rv::ErrorCode::NotFound);

    const GenerationResult second = resultOf(31);
    REQUIRE(rv::world::commitWorld(*lock, WorldAction{"edit", "room:snes"}, second, pinsOf(second, 2), world.options)
                .has_value());
    const WorldState afterFirstAction = stateOf(world.files);
    const GenerationResult third = resultOf(32);
    auto last = rv::world::commitWorld(*lock, WorldAction{"regenerate", "world"}, third, pinsOf(third, 5),
                                       world.options);
    REQUIRE(last.has_value());
    const WorldState afterSecondAction = stateOf(world.files);

    // A dry run names the snapshot and writes nothing.
    rv::world::UndoOptions options;
    options.writer = "a test's undo";
    options.clock = world.options.clock;
    options.dryRun = true;
    auto dry = rv::world::undoLastAction(*lock, options);
    REQUIRE(dry.has_value());
    CHECK(dry->restored == *last->snapshot);
    CHECK_FALSE(dry->kept.has_value());
    CHECK(stateOf(world.files) == afterSecondAction);
    CHECK(rv::world::listSnapshots(world.files).size() == 2);

    // The last action undone: the state before it, byte for byte; the state it replaced kept as an undo snapshot.
    options.dryRun = false;
    auto undone = rv::world::undoLastAction(*lock, options);
    REQUIRE(undone.has_value());
    CHECK(stateOf(world.files) == afterFirstAction);
    CHECK(undone->restored.name == last->snapshot->name);
    CHECK(undone->restored.undone);
    REQUIRE(undone->kept.has_value());
    CHECK(undone->kept->kind == SnapshotKind::Undo);
    CHECK(undone->kept->action == "undo");
    CHECK(undone->kept->undoes == last->snapshot->name);
    CHECK(undone->kept->writer == "a test's undo");
    CHECK(undone->kept->planDigest == rv::core::sha256Hex(*afterSecondAction.plan));
    CHECK(undone->kept->overridesDigest == rv::core::sha256Hex(*afterSecondAction.overrides));
    CHECK_FALSE(std::filesystem::exists(world.files.transaction));

    // Undo walks back: the first action undone, the world as it was before it (no overrides.json: removed).
    auto back = rv::world::undoLastAction(*lock, options);
    REQUIRE(back.has_value());
    CHECK(stateOf(world.files) == first);
    CHECK_FALSE(std::filesystem::exists(world.files.overrides));

    // Undo of undo refused: the undo snapshots are never offered, and no action is left.
    auto refused = rv::world::undoLastAction(*lock, options);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::NotFound);
    CHECK_THAT(refused.error().message, ContainsSubstring("never undone"));
    CHECK(stateOf(world.files) == first);
    const std::vector<WorldSnapshot> snapshots = rv::world::listSnapshots(world.files);
    REQUIRE(snapshots.size() == 4);
    std::map<SnapshotKind, std::size_t> kinds;
    for (const WorldSnapshot& snapshot : snapshots) {
        ++kinds[snapshot.kind];
        CHECK((snapshot.kind == SnapshotKind::Undo ? !snapshot.undone : snapshot.undone));
    }
    CHECK(kinds == std::map<SnapshotKind, std::size_t>{{SnapshotKind::Action, 2}, {SnapshotKind::Undo, 2}});

    // A new action after an undo is undone as any other.
    REQUIRE(rv::world::commitWorld(*lock, WorldAction{"edit", ""}, second, {}, world.options).has_value());
    REQUIRE(rv::world::undoLastAction(*lock, options).has_value());
    CHECK(stateOf(world.files) == first);
}

TEST_CASE("world/snapshots: an undo interrupted is rolled back, a damaged snapshot refused", "[world][snapshots]") {
    World world(30);
    const GenerationResult next = resultOf(31);
    std::string target;
    {
        auto lock = WorldLock::acquire(world.files);
        REQUIRE(lock.has_value());
        auto committed = rv::world::commitWorld(*lock, WorldAction{"edit", ""}, next, pinsOf(next, 3), world.options);
        REQUIRE(committed.has_value());
        target = committed->snapshot->name;
    }
    const WorldState edited = stateOf(world.files);
    for (const TransactionStop stop : {TransactionStop::BetweenWrites, TransactionStop::BeforeJournalEnds}) {
        INFO("stop " << static_cast<int>(stop));
        {
            auto lock = WorldLock::acquire(world.files);
            REQUIRE(lock.has_value());
            rv::world::UndoOptions options;
            options.clock = world.options.clock;
            CHECK(rv::world::detail::undoLastActionStoppingAt(*lock, options, stop).error().code ==
                  rv::ErrorCode::Cancelled);
        }
        auto lock = WorldLock::acquire(world.files);
        REQUIRE(lock.has_value());
        CHECK(lock->rolledBack().has_value());
        CHECK(stateOf(world.files) == edited);
        // The snapshot it was restoring is not undone: the next undo offers it again.
        const std::vector<WorldSnapshot> snapshots = rv::world::listSnapshots(world.files);
        REQUIRE(snapshots.size() == 1);
        CHECK(snapshots.front().name == target);
        CHECK_FALSE(snapshots.front().undone);
    }

    // A snapshot whose file is not the one it kept: refused, nothing written.
    REQUIRE(rv::core::writeTextFileAtomic(world.files.snapshots / target / "world-plan.json", "{}").has_value());
    auto lock = WorldLock::acquire(world.files);
    REQUIRE(lock.has_value());
    auto damaged = rv::world::undoLastAction(*lock);
    REQUIRE_FALSE(damaged.has_value());
    CHECK(damaged.error().code == rv::ErrorCode::ValidationFailed);
    CHECK_THAT(damaged.error().message, ContainsSubstring("damaged"));
    CHECK(stateOf(world.files) == edited);
    CHECK(rv::world::listSnapshots(world.files).size() == 1);
}

TEST_CASE("world/snapshots: snapshot records it refuses", "[world][snapshots]") {
    WorldSnapshot snapshot;
    snapshot.sequence = 3;
    snapshot.action = "regenerate";
    snapshot.scope = "world";
    snapshot.writer = "a test";
    snapshot.takenAt = "2026-10-27T10:00:00.000Z";
    snapshot.planDigest = rv::core::sha256Hex(std::string_view("plan"));
    const std::string text = rv::world::writeSnapshotRecord(snapshot);
    CHECK(rv::world::readSnapshotRecord(text, "s") == snapshot);
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string edited = text;
        edited.replace(edited.find(from), from.size(), to);
        return rv::world::readSnapshotRecord(edited, "snapshot.json");
    };
    CHECK(replaced("\"schemaVersion\": 1", "\"schemaVersion\": 2").error().code == rv::ErrorCode::Unsupported);
    CHECK(replaced("\"kind\": \"action\"", "\"kind\": \"redo\"").error().code == rv::ErrorCode::ValidationFailed);
    CHECK(replaced("\"action\": \"regenerate\"", "\"action\": \"Re generate\"").error().code ==
          rv::ErrorCode::ValidationFailed);
    CHECK(replaced("\"metaDigest\": \"\"", "\"metaDigest\": \"abc\"").error().code == rv::ErrorCode::ValidationFailed);
    CHECK(replaced("\"undoes\": \"\"", "\"undoes\": \"../elsewhere\"").error().code ==
          rv::ErrorCode::ValidationFailed);
    CHECK(replaced("\"undone\": false", "\"undone\": false, \"extra\": 1").error().code ==
          rv::ErrorCode::ValidationFailed);
    CHECK(rv::world::isValidActionName("edit"));
    CHECK(rv::world::isValidActionName("move-fixture"));
    CHECK_FALSE(rv::world::isValidActionName(""));
    CHECK_FALSE(rv::world::isValidActionName("2edit"));
    CHECK_FALSE(rv::world::isValidActionName("edit/../x"));
    CHECK_FALSE(rv::world::isValidActionName(std::string(33, 'a')));
}
