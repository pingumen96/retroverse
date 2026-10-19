// Atomic writes interrupted at each point (rv/core/file_io.hpp, writeFileAtomic since I3 step 0b) and rotating backups
// (rv/core/backups.hpp, ADR-0010 §9).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "support/log_capture.hpp"
#include "support/temp_dir.hpp"
#include <rv/core/backups.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/time.hpp>

using rv::core::BackupOptions;
using rv::core::detail::AtomicWriteStop;

namespace {

std::vector<std::string> namesIn(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::span<const std::byte> bytesOf(const std::string& text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::chrono::system_clock::time_point at(std::string_view text) {
    return rv::core::parseUtcIso8601(text).value();
}

/// A clock that ticks one second per backup from `start`.
std::function<std::chrono::system_clock::time_point()> ticking(std::string_view start) {
    auto next = std::make_shared<std::chrono::system_clock::time_point>(at(start));
    return [next] {
        const auto now = *next;
        *next += std::chrono::seconds(1);
        return now;
    };
}

} // namespace

TEST_CASE("core/file_io: an interrupted atomic write leaves the previous file", "[core][file_io]") {
    rv::test::TempDir dir("rv-atomic");
    const auto plan = dir.file("worlds/esde/world-plan.json");
    const std::string previous = "{\"plan\":1}\n";
    REQUIRE(rv::core::writeTextFileAtomic(plan, previous).has_value());
    const std::string next(100'000, 'x');

    for (const AtomicWriteStop stop : {AtomicWriteStop::AfterPartialWrite, AtomicWriteStop::BeforeRename}) {
        INFO("stop " << static_cast<int>(stop));
        const auto stopped = rv::core::detail::writeFileAtomicStoppingAt(plan, bytesOf(next), stop);
        REQUIRE_FALSE(stopped.has_value());
        CHECK(stopped.error().code == rv::ErrorCode::Cancelled);
        CHECK(rv::core::readTextFile(plan).value() == previous);
    }
    // What an ended process leaves: its temporary files beside the plan, which a later write does not mind.
    const auto names = namesIn(plan.parent_path());
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "world-plan.json");
    CHECK(names[1].starts_with("world-plan.json.tmp-" + std::to_string(GetCurrentProcessId()) + "-"));
    CHECK(names[2].starts_with("world-plan.json.tmp-"));
    CHECK(std::filesystem::file_size(plan.parent_path() / names[1]) +
              std::filesystem::file_size(plan.parent_path() / names[2]) ==
          next.size() / 2 + next.size());

    REQUIRE(rv::core::writeTextFileAtomic(plan, next).has_value());
    CHECK(rv::core::readTextFile(plan).value() == next);
}

TEST_CASE("core/file_io: an unflushed atomic write replaces the file whole, its stops leave the previous one",
          "[core][file_io]") {
    // I3 step 12: the derived-texture cache's entries are written without the flush.
    rv::test::TempDir dir("rv-atomic-unflushed");
    const auto entry = dir.file("cache/front/ab/entry.rvtex");
    const std::string previous(4096, 'p');
    REQUIRE(rv::core::writeFileAtomic(entry, bytesOf(previous), rv::core::Durability::Unflushed).has_value());
    CHECK(rv::core::readTextFile(entry).value() == previous);
    const std::string next(100'000, 'n');
    const auto stopped = rv::core::detail::writeFileAtomicStoppingAt(entry, bytesOf(next),
                                                                     AtomicWriteStop::BeforeRename,
                                                                     rv::core::Durability::Unflushed);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(rv::core::readTextFile(entry).value() == previous);
    REQUIRE(rv::core::writeFileAtomic(entry, bytesOf(next), rv::core::Durability::Unflushed).has_value());
    CHECK(rv::core::readTextFile(entry).value() == next);
    // The stopped write's temporary beside the entry, nothing else.
    CHECK(namesIn(entry.parent_path()).size() == 2);
}

TEST_CASE("core/file_io: a failed replace leaves the previous file and no temporary", "[core][file_io]") {
    rv::test::TempDir dir("rv-atomic-locked");
    const auto file = dir.file("world-plan.json");
    REQUIRE(rv::core::writeTextFileAtomic(file, "before").has_value());
    {
        // Another program holds the file open without sharing its deletion: the rename cannot replace it.
        const HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(held != INVALID_HANDLE_VALUE);
        const auto refused = rv::core::writeTextFileAtomic(file, "after");
        CloseHandle(held);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == rv::ErrorCode::IoError); // access denied
        CHECK(refused.error().message.find("cannot replace the destination") != std::string::npos);
    }
    CHECK(rv::core::readTextFile(file).value() == "before");
    CHECK(namesIn(dir.path()) == std::vector<std::string>{"world-plan.json"});
    // The destination a folder: refused alike.
    std::filesystem::create_directories(dir.file("folder"));
    CHECK_FALSE(rv::core::writeTextFileAtomic(dir.file("folder"), "x").has_value());
    CHECK(namesIn(dir.path()) == std::vector<std::string>{"folder", "world-plan.json"});
}

TEST_CASE("core/backups: 10 backups kept, the oldest dropped", "[core][backups]") {
    rv::test::TempDir dir("rv-backups");
    const auto plan = dir.file("worlds/esde/world-plan.json");
    const auto folder = dir.file("worlds/esde/backups");
    BackupOptions options;
    options.clock = ticking("2026-10-18T12:00:00.000Z");

    std::vector<std::filesystem::path> written;
    for (int i = 0; i < 12; ++i) {
        REQUIRE(rv::core::writeTextFileAtomic(plan, "plan " + std::to_string(i)).has_value());
        const auto report = rv::core::backUpFile(plan, folder, options);
        REQUIRE(report.has_value());
        CHECK(report->backup.parent_path() == folder);
        CHECK(rv::core::readTextFile(report->backup).value() == "plan " + std::to_string(i));
        if (i < 10) {
            CHECK(report->removed.empty());
        } else {
            // The oldest goes, one per backup beyond ten.
            REQUIRE(report->removed.size() == 1);
            CHECK(report->removed[0] == written[static_cast<std::size_t>(i - 10)]);
        }
        written.push_back(report->backup);
    }
    CHECK(written[0].filename() == "world-plan-20261018T120000.000Z.json");
    CHECK(written[11].filename() == "world-plan-20261018T120011.000Z.json");

    const auto kept = rv::core::listBackupsOf(plan, folder);
    REQUIRE(kept.size() == 10);
    // Newest first: plans 11 down to 2.
    for (std::size_t i = 0; i < kept.size(); ++i) {
        CHECK(kept[i] == written[11 - i]);
    }
    CHECK(rv::core::readTextFile(plan).value() == "plan 11"); // the file itself untouched
}

TEST_CASE("core/backups: names within one millisecond, a clock set back, other files", "[core][backups]") {
    rv::test::TempDir dir("rv-backups-names");
    const auto plan = dir.file("world-plan.json");
    const auto folder = dir.file("backups");
    REQUIRE(rv::core::writeTextFileAtomic(plan, "plan").has_value());

    BackupOptions sameTime;
    sameTime.keep = 3;
    sameTime.clock = [] {
        return at("2026-10-18T12:00:00.250Z");
    };
    std::vector<std::string> names;
    for (int i = 0; i < 3; ++i) {
        names.push_back(rv::core::backUpFile(plan, folder, sameTime).value().backup.filename().string());
    }
    CHECK(names == std::vector<std::string>{"world-plan-20261018T120000.250Z.json",
                                            "world-plan-20261018T120000.250Z-2.json",
                                            "world-plan-20261018T120000.250Z-3.json"});
    // Files that are not backups of this name stay: another file's, a near miss, a temporary file.
    for (const char* other : {"world-plan.meta-20261018T110000.000Z.json", "world-plan-20261018T110000.000Z.txt",
                              "world-plan-2026-10-18.json", "world-plan-20261018T110000.000Z-1.json",
                              "world-plan-20261018T110000.000Z.json.tmp-1-0", "notes.json"}) {
        REQUIRE(rv::core::writeTextFileAtomic(folder / other, "x").has_value());
    }
    // A clock set back an hour: the new backup sorts oldest by its name, and is still the one kept.
    BackupOptions setBack;
    setBack.keep = 2;
    setBack.clock = [] {
        return at("2026-10-18T11:00:00.000Z");
    };
    const auto report = rv::core::backUpFile(plan, folder, setBack);
    REQUIRE(report.has_value());
    CHECK(report->backup.filename() == "world-plan-20261018T110000.000Z.json");
    REQUIRE(report->removed.size() == 2);
    CHECK(report->removed[0].filename() == "world-plan-20261018T120000.250Z-2.json");
    CHECK(report->removed[1].filename() == "world-plan-20261018T120000.250Z.json");
    CHECK(namesIn(folder) == std::vector<std::string>{
                                 "notes.json", "world-plan-2026-10-18.json", "world-plan-20261018T110000.000Z-1.json",
                                 "world-plan-20261018T110000.000Z.json", "world-plan-20261018T110000.000Z.json.tmp-1-0",
                                 "world-plan-20261018T110000.000Z.txt", "world-plan-20261018T120000.250Z-3.json",
                                 "world-plan.meta-20261018T110000.000Z.json"});
}

TEST_CASE("core/backups: errors", "[core][backups]") {
    rv::test::TempDir dir("rv-backups-errors");
    const auto missing = rv::core::backUpFile(dir.file("world-plan.json"), dir.file("backups"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(dir.file("backups")));
    CHECK(rv::core::listBackupsOf(dir.file("world-plan.json"), dir.file("backups")).empty());

    // Keep 0 is taken as 1: the backup just written stays.
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("world-plan.json"), "plan").has_value());
    BackupOptions none;
    none.keep = 0;
    none.clock = ticking("2026-10-18T12:00:00.000Z");
    REQUIRE(rv::core::backUpFile(dir.file("world-plan.json"), dir.file("backups"), none).has_value());
    const auto second = rv::core::backUpFile(dir.file("world-plan.json"), dir.file("backups"), none);
    REQUIRE(second.has_value());
    CHECK(second->removed.size() == 1);
    CHECK(rv::core::listBackupsOf(dir.file("world-plan.json"), dir.file("backups")) ==
          std::vector<std::filesystem::path>{second->backup});
}
