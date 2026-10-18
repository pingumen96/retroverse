// The world's files (rv/world/world_store.hpp, ADR-0010 §9; I3 step 9): the folder's layout, a plan saved, replaced
// after a backup and not written again when unchanged, the meta sidecar, the last 10 backups kept, interrupted writes
// leaving the previous plan readable and their temporary files removed by the next holder of the lock, the lock held
// by one holder at a time, plans edited by hand or written by a newer Retroverse refused, results that must never be
// saved refused. The lock across processes is retroverse-cli's test (test_cli_worldplan_update.cpp).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/backups.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

#include "support/temp_dir.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::world::GenerationResult;
using rv::world::WorldFiles;
using rv::world::WorldLock;

namespace {

std::span<const std::byte> bytesOf(const std::string& text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string readText(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

/// A clock that ticks one second per call from 2026-10-18T10:00:00Z.
std::function<std::chrono::system_clock::time_point()> ticking() {
    auto next = std::make_shared<std::chrono::system_clock::time_point>(
        rv::core::parseUtcIso8601("2026-10-18T10:00:00.000Z").value());
    return [next] {
        const auto now = *next;
        *next += std::chrono::seconds(1);
        return now;
    };
}

GenerationResult freshResult(std::size_t titles, std::uint64_t seed) {
    return rv::test::world::generateRoom(titles, 4242, seed);
}

} // namespace

TEST_CASE("world/store: the folder of a library's world", "[world][store]") {
    const WorldFiles files =
        rv::world::worldFiles(rv::core::pathFromUtf8("C:/data"), *rv::catalog::LibraryId::parse("esde"));
    CHECK(files.folder == rv::core::pathFromUtf8("C:/data") / "worlds" / "esde");
    CHECK(files.plan == files.folder / "world-plan.json");
    CHECK(files.meta == files.folder / "world-plan.meta.json");
    CHECK(files.overrides == files.folder / "overrides.json");
    CHECK(files.backups == files.folder / "backups");
    CHECK(files.lock == files.folder / "world.lock");
}

TEST_CASE("world/store: a plan saved, replaced after a backup, and not written again unchanged", "[world][store]") {
    const rv::test::TempDir dir("rv-world-store");
    const WorldFiles files = WorldFiles::in(dir.file("worlds/esde"));
    auto lock = WorldLock::acquire(files);
    REQUIRE(lock.has_value());
    CHECK(std::filesystem::exists(files.lock));

    rv::world::SaveOptions options;
    options.writer = "a test";
    options.duration = std::chrono::microseconds(1234);
    options.clock = ticking();
    const GenerationResult first = freshResult(50, 1);
    auto saved = rv::world::savePlan(*lock, first, options);
    REQUIRE(saved.has_value());
    CHECK(saved->written);
    CHECK_FALSE(saved->backup.has_value());
    CHECK(readText(files.plan) == *rv::world::writePlan(first.plan));
    CHECK(saved->digest == rv::core::sha256Hex(readText(files.plan)));
    CHECK(saved->digest == first.changes.digestAfter);

    // The meta sidecar: what the write was, read back.
    auto meta = rv::world::readPlanMeta(readText(files.meta), "meta");
    REQUIRE(meta.has_value());
    CHECK(meta->writer == "a test");
    CHECK(meta->writtenAt == "2026-10-18T10:00:00.000Z");
    CHECK(meta->durationUs == 1234);
    CHECK(meta->planDigest == saved->digest);
    CHECK(meta->outcome == rv::world::GenerationOutcome::Generated);
    CHECK(meta->rebuild == rv::world::RebuildLevel::Full);
    CHECK(meta->placements == 50);
    CHECK(meta->added == 50);
    CHECK(meta->digestBefore.empty());
    CHECK(rv::world::writePlanMeta(*meta) == readText(files.meta));

    // The same plan again: nothing written, no backup.
    const auto written = rv::core::lastWriteTime(files.plan);
    auto again = rv::world::savePlan(*lock, first, options);
    REQUIRE(again.has_value());
    CHECK_FALSE(again->written);
    CHECK(rv::core::listBackupsOf(files.plan, files.backups).empty());
    CHECK(rv::core::lastWriteTime(files.plan) == written);

    // Another plan: the previous one kept in backups/ first.
    const GenerationResult second = freshResult(51, 1);
    auto replaced = rv::world::savePlan(*lock, second, options);
    REQUIRE(replaced.has_value());
    CHECK(replaced->written);
    REQUIRE(replaced->backup.has_value());
    CHECK(readText(*replaced->backup) == *rv::world::writePlan(first.plan));
    CHECK(readText(files.plan) == *rv::world::writePlan(second.plan));
    auto stored = rv::world::readStoredPlan(files.plan);
    REQUIRE(stored.has_value());
    CHECK(stored->plan == second.plan);
    CHECK(stored->digest == replaced->digest);
}

TEST_CASE("world/store: the last 10 plans are kept in backups", "[world][store]") {
    const rv::test::TempDir dir("rv-world-store-backups");
    const WorldFiles files = WorldFiles::in(dir.file("world"));
    auto lock = WorldLock::acquire(files);
    REQUIRE(lock.has_value());
    rv::world::SaveOptions options;
    options.clock = ticking();
    GenerationResult result = freshResult(15, 1);
    std::vector<std::string> texts;
    for (std::uint64_t i = 0; i < 13; ++i) {
        result.plan.seed = i; // another text each time
        texts.push_back(*rv::world::writePlan(result.plan));
        REQUIRE(rv::world::savePlan(*lock, result, options).has_value());
    }
    const std::vector<std::filesystem::path> backups = rv::core::listBackupsOf(files.plan, files.backups);
    REQUIRE(backups.size() == rv::world::kPlanBackupsKept);
    // Newest first: the plans before the last, the oldest two dropped.
    for (std::size_t i = 0; i < backups.size(); ++i) {
        CHECK(readText(backups[i]) == texts[texts.size() - 2 - i]);
    }
    CHECK(readText(files.plan) == texts.back());
}

TEST_CASE("world/store: an interrupted write leaves the previous plan readable", "[world][store]") {
    const rv::test::TempDir dir("rv-world-store-interrupted");
    const WorldFiles files = WorldFiles::in(dir.file("world"));
    const GenerationResult first = freshResult(50, 1);
    const std::string next = *rv::world::writePlan(freshResult(60, 1).plan);
    {
        auto lock = WorldLock::acquire(files);
        REQUIRE(lock.has_value());
        REQUIRE(rv::world::savePlan(*lock, first).has_value());
    }
    for (const auto stop : {rv::core::detail::AtomicWriteStop::AfterPartialWrite,
                            rv::core::detail::AtomicWriteStop::BeforeRename}) {
        // A writer that ended mid-write: its temporary file left beside the plan, the plan whole.
        CHECK(rv::core::detail::writeFileAtomicStoppingAt(files.plan, bytesOf(next), stop).error().code ==
              rv::ErrorCode::Cancelled);
        auto stored = rv::world::readStoredPlan(files.plan);
        REQUIRE(stored.has_value());
        CHECK(stored->plan == first.plan);
        // The next holder of the lock removes the temporary file, and its own write succeeds.
        auto lock = WorldLock::acquire(files);
        REQUIRE(lock.has_value());
        REQUIRE(lock->staleFilesRemoved().size() == 1);
        CHECK_THAT(lock->staleFilesRemoved().front().filename().string(), ContainsSubstring("world-plan.json.tmp-"));
        for (const auto& entry : std::filesystem::directory_iterator(files.folder)) {
            CHECK(entry.path().filename().string().find(".tmp-") == std::string::npos);
        }
    }
    // A temporary file of a backup interrupted the same way is removed too; anything else is left alone.
    std::filesystem::create_directories(files.backups);
    REQUIRE(rv::core::writeTextFileAtomic(files.backups / "world-plan-20261018T100000.000Z.json.tmp-12-3", "x")
                .has_value());
    REQUIRE(rv::core::writeTextFileAtomic(files.folder / "notes.tmp-x", "x").has_value());
    auto lock = WorldLock::acquire(files);
    REQUIRE(lock.has_value());
    CHECK(lock->staleFilesRemoved().size() == 1);
    CHECK(std::filesystem::exists(files.folder / "notes.tmp-x"));

    // A sidecar that cannot be written leaves the plan saved and says so.
    std::filesystem::remove(files.meta);
    std::filesystem::create_directories(files.meta); // a folder where the sidecar goes
    const GenerationResult other = freshResult(70, 1);
    auto saved = rv::world::savePlan(*lock, other);
    REQUIRE(saved.has_value());
    CHECK(saved->written);
    CHECK(saved->metaError.has_value());
    CHECK(rv::world::readStoredPlan(files.plan)->plan == other.plan);
}

TEST_CASE("world/store: the world lock has one holder at a time", "[world][store]") {
    const rv::test::TempDir dir("rv-world-store-lock");
    const WorldFiles files = WorldFiles::in(dir.file("world"));
    auto held = WorldLock::acquire(files);
    REQUIRE(held.has_value());
    const auto start = std::chrono::steady_clock::now();
    auto second = WorldLock::acquire(files, std::chrono::milliseconds(100));
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == rv::ErrorCode::Conflict);
    CHECK_THAT(second.error().message, ContainsSubstring("another program writes this world"));
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(100));
    // Moved, the lock is still held; released, it is taken at once.
    WorldLock moved = std::move(*held);
    CHECK(moved.held());
    CHECK_FALSE(held->held());
    CHECK(rv::world::savePlan(*held, freshResult(15, 1)).error().code == rv::ErrorCode::InvalidArgument);
    CHECK_FALSE(WorldLock::acquire(files, std::chrono::milliseconds(0)).has_value());
    { const WorldLock released = std::move(moved); }
    auto taken = WorldLock::acquire(files, std::chrono::milliseconds(0));
    CHECK(taken.has_value());
}

TEST_CASE("world/store: plans it refuses to read or to save", "[world][store]") {
    const rv::test::TempDir dir("rv-world-store-refused");
    const WorldFiles files = WorldFiles::in(dir.file("world"));
    CHECK(rv::world::readStoredPlan(files.plan).error().code == rv::ErrorCode::NotFound);
    const GenerationResult result = freshResult(15, 1);
    const std::string text = *rv::world::writePlan(result.plan);
    // Edited by hand (a space, a line feed): not the canonical text of a plan.
    REQUIRE(rv::core::writeTextFileAtomic(files.plan, text + "\n").has_value());
    CHECK(rv::world::readStoredPlan(files.plan).error().code == rv::ErrorCode::ValidationFailed);
    std::string edited = text;
    edited.replace(edited.find("\"seed\":"), 7, "\"seed\": ");
    REQUIRE(rv::core::writeTextFileAtomic(files.plan, edited).has_value());
    CHECK(rv::world::readStoredPlan(files.plan).error().code == rv::ErrorCode::ValidationFailed);
    // Written by a newer Retroverse.
    std::string newer = text;
    newer.replace(newer.find("\"schemaVersion\":1"), 17, "\"schemaVersion\":2");
    REQUIRE(rv::core::writeTextFileAtomic(files.plan, newer).has_value());
    CHECK(rv::world::readStoredPlan(files.plan).error().code == rv::ErrorCode::Unsupported);

    auto lock = WorldLock::acquire(files);
    REQUIRE(lock.has_value());
    // A plan that needs an explicit regeneration was kept, not updated: nothing to save.
    GenerationResult kept = result;
    kept.changes.outcome = rv::world::GenerationOutcome::RequiresExplicitRegeneration;
    CHECK(rv::world::savePlan(*lock, kept).error().code == rv::ErrorCode::InvalidArgument);
    // A plan that breaks a hard rule is never saved.
    GenerationResult broken = result;
    broken.validation.findings.push_back({"slot-shared", rv::world::Severity::Hard, "slot", "two games"});
    CHECK(rv::world::savePlan(*lock, broken).error().code == rv::ErrorCode::ValidationFailed);
    CHECK(readText(files.plan) == newer);
    // The sidecar's reader refuses what is not one.
    CHECK_FALSE(rv::world::readPlanMeta("{}", "meta").has_value());
    CHECK(rv::world::readPlanMeta("{\"schemaVersion\": 2}", "meta").error().code == rv::ErrorCode::Unsupported);
}
