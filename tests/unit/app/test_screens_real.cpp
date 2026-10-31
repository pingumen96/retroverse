// Every collection room of the real building with its hero screen playing (Gate M4, I5 step 10; ADR-0016 §2, §3 and
// §8, specification §9.1: "every room that holds games holds at least one active display"). Hidden
// ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the
// library before and after, the folders in the RV_REAL_* variables (or the script's parameters) and RV_AUDIO=null:
//   ./scripts/real-library.ps1 -Filter '[real-library][screens]'
// The whole library is imported into a new catalog under the build tree (build/<preset>/real-library/screens/data,
// library.json naming the real ES-DE home, install, ROM and media roots), then retroverse.exe of this build (hidden,
// lockstep):
//   - a first start in the lobby: the building planned and written;
//   - every room of the plan that holds games (console rooms, shared rooms, the arcade, expansion rooms), started at
//     its doorway (--start-room) for 240 frames (4 s at --fixed-dt 0.016667): its hero screen (the hero CRT, the
//     arcade's hero cabinet) shows a clip or a still of its game's fallback chain (screenshot, title screen, cover,
//     the generated title card), never the building's standby pattern once it showed a picture and never at the
//     end; never more than 2 decoders open nor 2 decoder threads, every thread joined at the exit.
// The figures are printed (the lines starting "Real screens:", each flushed whole: a line cut by the next test's log
// once missed scripts/gate.ps1's pattern): ids, kinds, counts and pictures, nothing derived from the user's titles. The runs' reports stay in the build tree (never committed).

#include "support/child_process.hpp"
#include "support/fixture.hpp"

#include <rv/app/run_report.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

/// 4 s of a lockstep run: the hero's first clip waits for its first frame (at most NFR-06's 2 s), a still for its
/// picture, so the run's end shows what the room's screen settled on.
constexpr int kFrames = 240;
/// ADR-0016 §2 and NFR-07: two decoders in the Balanced profile (video.json's maxDecoders, the shipped default).
constexpr std::uint64_t kDecoderBudget = 2;

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// retroverse.exe with `arguments` on `data`, hidden, its output in <folder>/<name>.log and its report in
/// <folder>/<name>.json; the report read back (nothing when the run left none: its checks failed).
std::optional<rv::app::RunReport> runApp(const fs::path& folder, const fs::path& data, const std::string& name,
                                         std::vector<std::string> arguments) {
    const fs::path report = folder / (name + ".json");
    arguments.insert(arguments.end(), {"--hidden", "--data-dir", rv::core::pathToUtf8(data), "--run-report",
                                       rv::core::pathToUtf8(report)});
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / "retroverse.exe";
    request.arguments = std::move(arguments);
    request.stdoutFile = folder / (name + ".log");
    request.stderrFile = request.stdoutFile;
    request.environment.push_back({"RV_ASSET_ROOT", std::nullopt});
    request.timeout = std::chrono::minutes(10);
    const auto run = rv::test::runChild(request);
    REQUIRE(run.has_value());
    INFO(name << ": exit code " << run->exitCode << (run->timedOut ? " (timed out)" : "") << "; "
              << rv::core::pathToUtf8(request.stdoutFile));
    CHECK_FALSE(run->timedOut);
    CHECK(run->exitCode == 0);
    auto read = rv::app::readRunReport(report);
    CHECK(read.has_value());
    if (!read) {
        return std::nullopt;
    }
    return std::move(*read);
}

bool isStill(std::string_view picture) {
    return picture == "screenshot" || picture == "title-screen" || picture == "cover" || picture == "title-card";
}

} // namespace

TEST_CASE("app/world: every collection room of the real building shows a clip or a still on its hero screen, within "
          "the decoder budget",
          "[.real-library][real-library][screens]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("real").value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;

    // A new data directory under the build tree, never in the library: library.json and the whole library imported.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "screens";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / std::string(rv::esde::kLibraryFileName), profile,
                                       rv::esde::ExistingFile::Replace)
                .has_value());
    rv::core::PathOptions pathOptions;
    pathOptions.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const rv::core::Paths paths = rv::core::Paths::resolve(pathOptions, environment).value();
    {
        const Clock::time_point start = Clock::now();
        auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
        REQUIRE(database.has_value());
        auto store = rv::catalog_db::SqliteCatalogStore::open(*database);
        REQUIRE(store.has_value());
        const auto settings = rv::esde::readSettings(profile.esdeHome);
        REQUIRE(settings.has_value());
        rv::esde::EsdeLibrarySource source;
        rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
        rv::catalog::IdentityPairing pairing;
        rv::catalog::ImportService service(source, media, **store, &pairing);
        const auto imported = service.run(profile, rv::catalog::ImportOptions{});
        INFO((imported ? std::string() : imported.error().toString()));
        REQUIRE(imported.has_value());
        std::cout << fmt::format("Real screens: the whole library imported in {:.1f} s\n",
                                 std::chrono::duration<double>(Clock::now() - start).count())
                  << std::flush;
    }

    // The first start, in the lobby: the building planned and written.
    const std::optional<rv::app::RunReport> first =
        runApp(folder, data, "first", {"--exit-after-frames", "--frames", "30", "--fixed-dt", "0.016667"});
    REQUIRE(first.has_value());
    REQUIRE(first->world.bound);
    CHECK(first->world.hardFindings == 0);
    const auto stored = rv::world::readStoredPlan(rv::world::worldFiles(data, profile.id).plan);
    REQUIRE(stored.has_value());
    const rv::world::WorldPlan& plan = stored->plan;
    std::map<std::string, std::size_t> placements;
    for (const rv::world::Placement& placement : plan.placements) {
        ++placements[placement.slot.fixture().room().toString()];
    }

    // Every room that holds games, in the plan's order.
    std::size_t started = 0;
    std::size_t onClip = 0;
    std::size_t onStandby = 0;
    std::map<std::string, std::size_t> stills;
    std::uint64_t mostOpen = 0;
    std::uint64_t mostThreads = 0;
    std::uint64_t decoders = 0;
    std::vector<std::string> notShowing;
    for (const rv::world::Room& room : plan.rooms) {
        const std::string id = room.id.toString();
        const std::size_t placed = placements[id];
        if (placed == 0) {
            std::cout << fmt::format("Real screens: {} ({}, {}) holds no games: not started\n", id,
                                     rv::world::toString(room.kind), room.floor)
                      << std::flush;
            continue;
        }
        ++started;
        const std::string name = fmt::format("room-{}", started);
        const Clock::time_point start = Clock::now();
        const std::optional<rv::app::RunReport> run =
            runApp(folder, data, name,
                   {"--start-room", id, "--exit-after-frames", "--frames", std::to_string(kFrames), "--fixed-dt",
                    "0.016667"});
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        INFO(id << " (" << name << ")");
        if (!run) {
            notShowing.push_back(id + " (no report)");
            continue;
        }
        CHECK(run->world.state == "room");
        CHECK(run->world.room == id);
        const rv::app::VideoRecord& video = run->video;
        CHECK(video.bound);
        // The hero screen is the room's own (its hero CRT, or the arcade's hero cabinet).
        CHECK(video.screen.starts_with(id + "/"));
        CHECK(video.maxDecoders == kDecoderBudget);
        CHECK(video.maxOpenDecoders <= kDecoderBudget);
        CHECK(video.maxDecoderThreads <= kDecoderBudget);
        CHECK(video.joinedAtExit);
        REQUIRE(run->audio.has_value());
        CHECK(run->audio->device == "null");
        // A clip or a still at the end, and never the standby pattern again once a picture showed.
        const bool showing = video.picture == "video" || isStill(video.picture);
        CHECK(showing);
        bool standbyAfterPicture = false;
        bool pictured = false;
        std::string sequence;
        for (const rv::app::VideoShownRecord& shown : video.shown) {
            if (shown.picture == "idle-pattern") {
                standbyAfterPicture = standbyAfterPicture || pictured;
            }
            else {
                pictured = true;
            }
            if (!sequence.empty()) {
                sequence += ", ";
            }
            sequence += shown.picture;
        }
        CHECK_FALSE(standbyAfterPicture);
        if (!showing || standbyAfterPicture) {
            notShowing.push_back(id);
        }
        if (video.picture == "video") {
            ++onClip;
        }
        else if (isStill(video.picture)) {
            ++stills[video.picture];
        }
        else {
            ++onStandby;
        }
        mostOpen = std::max(mostOpen, video.maxOpenDecoders);
        mostThreads = std::max(mostThreads, video.maxDecoderThreads);
        decoders += video.decodersStarted;
        std::cout << fmt::format(
            "Real screens: {} ({}, {}, {} placed): the hero {} shows {} at the end (shown: {}); {} decoder(s) started, "
            "at most {} open, {} thread(s); first frame {}; {} fallback(s), {} idle frame(s) before its first "
            "picture; the run {:.1f} s\n",
            id, rv::world::toString(room.kind), room.floor, placed, video.screen, video.picture,
            sequence.empty() ? std::string("nothing") : sequence, video.decodersStarted, video.maxOpenDecoders,
            video.maxDecoderThreads,
            video.firstFrameMs ? fmt::format("{:.0f} ms after its decoder started", *video.firstFrameMs)
                               : std::string("none"),
            video.fallbacks.size(), video.idleFramesWithGame, seconds)
                  << std::flush;
    }
    std::string stillText;
    for (const auto& [picture, count] : stills) {
        stillText += fmt::format("{}{} {}", stillText.empty() ? "" : ", ", picture, count);
    }
    std::size_t stillCount = 0;
    for (const auto& [picture, count] : stills) {
        stillCount += count;
    }
    std::cout << fmt::format("Real screens: {} collection room(s) of {} started: {} on a clip, {} on a still ({}), {} "
                             "on standby; at most {} decoder(s) open and {} thread(s) in any run, {} decoder(s) "
                             "started in all; not showing: {}\n",
                             started, plan.rooms.size(), onClip, stillCount,
                             stillText.empty() ? std::string("none") : stillText, onStandby, mostOpen, mostThreads,
                             decoders, notShowing.empty() ? std::string("none") : fmt::format("{}", notShowing.size()))
              << std::flush;
    CHECK(started > 0);
    CHECK(onStandby == 0);
    CHECK(notShowing.empty());
}
