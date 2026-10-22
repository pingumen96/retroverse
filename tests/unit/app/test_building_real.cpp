// The real library's building in retroverse.exe (I4 step 6b). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the
// folders in the RV_REAL_* variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][building]'
// The whole library is imported into a new catalog under the build tree (build/<preset>/real-library/building/data,
// library.json naming the real ES-DE home, install, ROM and media roots), then retroverse.exe of this build (hidden,
// frame-limited) starts on that data directory:
//   - a first start (no plan): the lobby's shell at once, the whole building planned, generated and written (lockstep);
//     Gate C: the plans' folder removed and the building planned again from nothing (`again`): one plan digest and one
//     file SHA-256;
//   - an unchanged second start, as the user starts (--no-vsync, no lockstep, the quick catalog opened on frame 1):
//     NFR-01, the stored lobby shown from the plan and the items files and the quick catalog's first page within 5 s
//     of the process's creation; the building job skipped by its stamp;
//   - the building stamp removed: the whole job on an unchanged library (the time I4 step 6b set out to cut; lockstep,
//     so that the run waits for it), then the user's start again with the stamp it wrote;
//   - a start in the busiest floor's largest room (--start-room, lockstep; the floor with the most placements in the
//     stored plan, the first in the elevator's order on a tie: the computer floor on the reference machine): the
//     floor's GPU memory at its peak within the acceptable 512 MiB, cold (its covers derived and stored in that run)
//     then warm;
//   - I4 step 7b: the derived covers emptied, a ride from the lobby to the busiest floor (the busiest of the others when
//     that is the ground floor, where the directory teleports), to its largest room through the elevator and the
//     directory, and back to the lobby's elevator with Map, as the user rides (no lockstep), cold then warm: the view
//     back within the acceptable row (10 s cold, 3 s warm; since I4 step 8 the view comes back once the room's covers in
//     view are resident, by 3 s after the request at the latest), the room whole within its row (30 s cold, 3 s warm),
//     the GPU memory after each ride the destination's (the collection arrays of a start on that floor, of the lobby);
//   - retroverse-cli worldplan update on the unchanged library (the same update code, informative).
// The figures are printed for docs/i4/step-6b.md, step-7b.md and Gate C (the lines starting "Real building:"; the gate
// reads the runs' reports in build/<preset>/real-library/building). The checks hold whatever the
// collection holds (this machine's figures are not versioned); nothing derived from the user's titles is written
// outside the build tree.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/app/directory_data.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_build.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/world_store.hpp>

#include "support/child_process.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

/// The acceptable limits (docs/roadmap.md, "Acceptable performance").
constexpr double kStartLimitMs = 5000.0;
constexpr std::uint64_t kAcceptableGpuBytes = 512ull * 1024ull * 1024ull;

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// `program` (one of this build's) with `arguments`, its output in <folder>/<name>.log; the exit code.
int runProgram(const fs::path& folder, const std::string& program, const std::string& name,
               std::vector<std::string> arguments) {
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / program;
    request.arguments = std::move(arguments);
    request.stdoutFile = folder / (name + ".log");
    request.stderrFile = request.stdoutFile;
    request.environment.push_back({"RV_ASSET_ROOT", std::nullopt});
    request.timeout = std::chrono::minutes(15);
    const auto run = rv::test::runChild(request);
    REQUIRE(run.has_value());
    INFO(name << ": exit code " << run->exitCode << (run->timedOut ? " (timed out)" : "") << "; "
              << rv::core::pathToUtf8(request.stdoutFile));
    REQUIRE_FALSE(run->timedOut);
    return run->exitCode;
}

/// retroverse.exe with `arguments` on `data`, hidden, its report in <folder>/<name>.json; the report read back.
rv::app::RunReport runApp(const fs::path& folder, const fs::path& data, const std::string& name,
                          std::vector<std::string> arguments) {
    const fs::path report = folder / (name + ".json");
    arguments.insert(arguments.end(), {"--hidden", "--data-dir", rv::core::pathToUtf8(data), "--run-report",
                                       rv::core::pathToUtf8(report)});
    CHECK(runProgram(folder, "retroverse.exe", name, std::move(arguments)) == 0);
    auto read = rv::app::readRunReport(report);
    REQUIRE(read.has_value());
    return std::move(*read);
}

std::string ms(const std::optional<double>& value) {
    return value ? fmt::format("{:.0f} ms", *value) : std::string("-");
}

double mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

std::string fileSha256(const fs::path& file) {
    auto bytes = rv::core::readBinaryFile(file);
    REQUIRE(bytes.has_value());
    return rv::core::sha256Hex(std::span<const std::byte>(*bytes));
}

/// The building job's phases of a run, as a line.
std::string jobLine(const rv::app::WorldRecord& world) {
    if (!world.job) {
        return "no job";
    }
    const rv::app::WorldJobRecord& job = *world.job;
    return fmt::format("{} {} in {:.0f} ms (snapshot {}, lock and plan {:.0f}, selections {:.0f}, rooms' data {:.0f}, "
                       "plan {:.0f}, save {:.0f}, items {:.0f}, floor {:.0f})",
                       world.outcome, job.skipped ? "(skipped by its stamp)" : "(planned)", job.totalMs,
                       ms(world.snapshotMs), job.lockMs, job.selectMs, job.dataMs, job.planMs, job.saveMs, job.itemsMs,
                       job.floorMs);
}

} // namespace

TEST_CASE("app/world: the real library's building in retroverse.exe: NFR-01 to the lobby, an unchanged start, the "
          "busiest floor's memory",
          "[.real-library][real-library][building]") {
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
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "building";
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
        REQUIRE(imported->statistics.mediaUnknown == 0);
        std::cout << fmt::format("Real building: the whole library imported in {:.1f} s\n",
                                 std::chrono::duration<double>(Clock::now() - start).count());
    }
    const auto openCatalog = [&](const std::string& name) {
        const fs::path script = folder / (name + "-catalog.json");
        REQUIRE(rv::core::writeTextFileAtomic(
                    script, "[{\"fromFrame\": 1, \"toFrame\": 1, \"actions\": {\"quick_catalog\": 1}}]\n")
                    .has_value());
        return rv::core::pathToUtf8(script);
    };
    const std::vector<std::string> lockstep{"--exit-after-frames", "--frames", "30", "--fixed-dt", "0.016667"};

    // The first start: no plan; the lobby's shell at once, the whole building planned and written.
    const rv::app::RunReport first = runApp(folder, data, "first", lockstep);
    REQUIRE(first.world.bound);
    CHECK(first.world.outcome == "generated");
    CHECK(first.world.planWritten);
    CHECK(first.world.hardFindings == 0);
    CHECK(first.world.floor == "ground");

    // Gate C: the building planned twice. The plans' folder removed, the same catalog planned again from nothing: one
    // plan digest and one file SHA-256 (ADR-0010 §8, ADR-0013 §5: plan equality is the file's SHA-256).
    const fs::path planFile = rv::world::worldFiles(data, profile.id).plan;
    const std::string firstFileHash = fileSha256(planFile);
    fs::remove_all(data / "worlds", removed);
    REQUIRE_FALSE(removed);
    const rv::app::RunReport again = runApp(folder, data, "again", lockstep);
    CHECK(again.world.outcome == "generated");
    CHECK(again.world.planWritten);
    CHECK(again.world.hardFindings == 0);
    CHECK(again.world.planDigest == first.world.planDigest);
    const std::string secondFileHash = fileSha256(planFile);
    CHECK(secondFileHash == firstFileHash);
    std::cout << fmt::format("Real building: planned twice: the plan digest {} and the file's SHA-256 {} ({} placements; "
                             "the jobs {} and {})\n",
                             again.world.planDigest == first.world.planDigest ? "equal" : "DIFFERENT",
                             secondFileHash == firstFileHash ? "equal" : "DIFFERENT", again.world.placements,
                             jobLine(first.world), jobLine(again.world));

    // The unchanged start as the user starts (no lockstep): NFR-01, the stored lobby and the first page within 5 s.
    // Enough frames (a few milliseconds each) for the building job's result to arrive: the whole job's takes seconds.
    const auto userStart = [&](const std::string& name, std::uint32_t frames) {
        return runApp(folder, data, name,
                      {"--no-vsync", "--exit-after-frames", "--frames", std::to_string(frames), "--input-script",
                       openCatalog(name)});
    };
    const rv::app::RunReport second = userStart("second", 3000);
    // The whole job on the unchanged library (the stamp removed): lockstep, so that the run waits for its result.
    const fs::path stamp = rv::app::buildingStampFile(paths.cacheDir(), profile.id);
    REQUIRE(fs::exists(stamp));
    REQUIRE(fs::remove(stamp));
    const rv::app::RunReport whole = runApp(folder, data, "whole", lockstep);
    CHECK(whole.world.outcome == "unchanged");
    REQUIRE(whole.world.job.has_value());
    CHECK_FALSE(whole.world.job->skipped);
    std::cout << fmt::format("Real building: the whole job on the unchanged library: {}\n", jobLine(whole.world));
    const rv::app::RunReport third = userStart("third", 3000);
    for (const auto& [name, report] :
         {std::pair<const char*, const rv::app::RunReport*>{"second", &second}, {"third", &third}}) {
        INFO(name);
        const rv::app::WorldRecord& world = report->world;
        CHECK(world.state == "room");
        CHECK(world.outcome == "unchanged");
        CHECK_FALSE(world.planWritten);
        CHECK(world.planDigest == first.world.planDigest);
        REQUIRE(world.preview.has_value());
        REQUIRE(world.preview->committedAtMs.has_value());
        CHECK(*world.preview->committedAtMs <= kStartLimitMs);
        REQUIRE(report->catalog.has_value());
        REQUIRE(report->catalog->firstPageMs.has_value());
        CHECK(*report->catalog->firstPageMs <= kStartLimitMs);
        REQUIRE(world.job.has_value());
        std::cout << fmt::format("Real building: {} start: the lobby shown {} after the process started (its "
                                 "preview built in {}), the quick catalog's first page {}; the building's result "
                                 "committed {}; the job: {}\n",
                                 name, ms(world.preview->committedAtMs), ms(world.preview->buildMs),
                                 ms(report->catalog->firstPageMs), ms(world.committedAtMs), jobLine(world));
    }
    CHECK(second.world.job->skipped);
    CHECK(third.world.job->skipped);

    // The busiest floor (the most placements, the first in the elevator's order on a tie: the floor whose GPU memory
    // the acceptable row bounds) and its largest room: the floor's GPU memory at its peak, cold (nothing but the lobby
    // showed in this data directory: no cover of the floor derived yet) then warm.
    const auto stored = rv::world::readStoredPlan(rv::world::worldFiles(data, profile.id).plan);
    REQUIRE(stored.has_value());
    std::map<std::string, std::size_t> placements;
    for (const rv::world::Placement& placement : stored->plan.placements) {
        ++placements[placement.slot.fixture().room().toString()];
    }
    const std::vector<rv::app::WorldFloorRecord> floorRecords = rv::app::floorRecordsOf(stored->plan);
    REQUIRE(floorRecords.size() > 1);
    // The busiest floor among the records for which `admits` holds.
    const auto busiestOf = [&](const auto& admits) {
        const rv::app::WorldFloorRecord* busiest = nullptr;
        for (const rv::app::WorldFloorRecord& record : floorRecords) {
            if (admits(record) && (busiest == nullptr || record.placed > busiest->placed)) {
                busiest = &record;
            }
        }
        REQUIRE(busiest != nullptr);
        return busiest->id;
    };
    const std::string busiest = busiestOf([](const rv::app::WorldFloorRecord&) { return true; });
    // A floor's largest room and the items the floor holds.
    const auto largestRoomOf = [&](const std::string& floor) {
        std::string largest;
        std::size_t floorItems = 0;
        for (const rv::world::Room* room : rv::world::roomsOnFloor(stored->plan, floor)) {
            const std::size_t count = placements[room->id.toString()];
            floorItems += count;
            if (largest.empty() || count > placements[largest]) {
                largest = room->id.toString();
            }
        }
        REQUIRE_FALSE(largest.empty());
        return std::pair<std::string, std::size_t>(largest, floorItems);
    };
    const auto [busiestLargest, busiestItems] = largestRoomOf(busiest);
    std::uint64_t busiestArrays = 0;
    for (const std::string name : {"busiest-cold", "busiest-warm"}) {
        std::vector<std::string> arguments{"--start-room", busiestLargest};
        arguments.insert(arguments.end(), {"--exit-after-frames", "--frames", "120", "--fixed-dt", "0.016667"});
        const rv::app::RunReport run = runApp(folder, data, name, arguments);
        INFO(name);
        CHECK(run.world.state == "room");
        CHECK(run.world.floor == busiest);
        CHECK(run.world.items == busiestItems);
        CHECK(run.gpuMemory.peakTotalBytes <= kAcceptableGpuBytes);
        if (name == "busiest-cold") {
            // Cold: the floor's covers derived from the media and stored in this run.
            CHECK(run.residency.cacheMisses > 0);
            CHECK(run.residency.cacheStored > 0);
        }
        busiestArrays = run.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes;
        std::cout << fmt::format("Real building: {}: the busiest floor, {} ({} rooms, {} items; its largest room {} "
                                 "items): GPU memory at its peak {:.1f} MiB, collection arrays {:.1f} MiB ({}), "
                                 "{} of {} fronts resident at the end (the cache: {} hits, {} misses, {} stored); the "
                                 "job: {}\n",
                                 name, busiest, run.world.rooms.size(), busiestItems, placements[busiestLargest],
                                 mib(run.gpuMemory.peakTotalBytes), mib(run.residency.arrayBytes),
                                 run.residency.format, run.residency.frontsResident, run.residency.items,
                                 run.residency.cacheHits, run.residency.cacheMisses, run.residency.cacheStored,
                                 jobLine(run.world));
    }

    // I4 step 7b: a ride from the lobby to the busiest floor (the busiest of the others when that is the ground floor,
    // where the directory teleports), to its largest room through the elevator (the walker turned to its doors,
    // interact, the floor, the room), then back to the lobby's elevator with Map, as the user rides (no lockstep: the
    // view comes back once the covers in view of the room that holds the eye are resident, a hall at once, or by 3 s
    // after the request: I4 step 8), cold (the derived covers emptied: the room's covers made from the media again) then
    // warm; the GPU memory after each ride, and when the room was whole. The script waits long enough between the two
    // choices for a cold ride and the room's covers at any frame rate (both rides must have ended).
    const std::string rideFloor = busiest != rv::world::kGroundFloor
                                      ? busiest
                                      : busiestOf([](const rv::app::WorldFloorRecord& record) {
                                            return record.id != rv::world::kGroundFloor;
                                        });
    const std::string largest = largestRoomOf(rideFloor).first;
    const fs::path textures = rv::assets::textureCacheRoot(paths.cacheDir(), profile.id.str()).value();
    fs::remove_all(textures, removed);
    REQUIRE_FALSE(removed);
    const rv::ui::DirectoryBuilding directory = rv::app::directoryBuildingOf(stored->plan, {});
    const auto rowOf = [](const auto& rows, std::string_view id) {
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        REQUIRE(found != rows.end());
        return static_cast<std::size_t>(found - rows.begin());
    };
    const std::size_t groundRow = rowOf(directory.floors, rv::world::kGroundFloor);
    const std::size_t rideRow = rowOf(directory.floors, rideFloor);
    // A floor's list: its hall's row, then its rooms along the hall (the first room selected on another floor).
    const std::size_t largestRow = 1 + rowOf(directory.floors[rideRow].rooms, largest);
    // Frames before the first choice (the stored lobby reconciled with the building's result: no ride while a floor
    // applies) and between the two (a cold ride at any frame rate).
    constexpr std::uint64_t kStartFrames = 6000;
    constexpr std::uint64_t kRideWaitFrames = 20000;
    const auto rideScript = [&](const std::string& name) {
        std::string script = "[";
        const auto press = [&](std::uint64_t frame, std::string_view action) {
            script += fmt::format("{}{{\"fromFrame\": {}, \"toFrame\": {}, \"actions\": {{\"{}\": 1}}}}",
                                  script.size() > 1 ? ", " : "", frame, frame, action);
        };
        const auto move = [&](std::uint64_t& frame, std::size_t from, std::size_t to) {
            for (std::size_t row = from; row != to; row = row < to ? row + 1 : row - 1) {
                press(frame += 3, row < to ? "ui_down" : "ui_up");
            }
        };
        // The lobby's arrival point faces the hall: half a turn to the elevator's doors (10 * 142.8 mouse pixels), its
        // prompt, interact: the floors (the ground floor selected), down to the ride's floor, its rooms, the largest.
        std::uint64_t frame = kStartFrames;
        script += fmt::format("{{\"fromFrame\": {}, \"toFrame\": {}, \"actions\": {{\"look_delta_x\": 142.8}}}}", frame,
                              frame + 9);
        press(frame += 13, "interact");
        move(frame, groundRow, rideRow);
        press(frame += 3, "ui_activate");
        move(frame, 1, largestRow);
        press(frame += 3, "ui_activate");
        // In the room, Map: the floors (the ride's floor selected), up to the ground floor, its hall's row, chosen.
        frame = kStartFrames + kRideWaitFrames;
        press(frame, "map");
        move(frame, rideRow, groundRow);
        press(frame += 3, "ui_activate");
        press(frame += 3, "ui_up");
        press(frame += 3, "ui_activate");
        script += "]\n";
        const fs::path file = folder / (name + "-script.json");
        REQUIRE(rv::core::writeTextFileAtomic(file, script).has_value());
        return std::pair<std::string, std::uint64_t>(rv::core::pathToUtf8(file), frame + kRideWaitFrames);
    };
    std::vector<rv::app::RunReport> rides;
    for (const std::string name : {"ride-cold", "ride-warm"}) {
        const auto [script, frames] = rideScript(name);
        rides.push_back(runApp(folder, data, name,
                               {"--no-vsync", "--exit-after-frames", "--frames", std::to_string(frames),
                                "--input-script", script}));
        const rv::app::RunReport& run = rides.back();
        INFO(name);
        REQUIRE(run.world.rides.size() == 2);
        for (const rv::app::RideRecord& ride : run.world.rides) {
            CHECK(ride.gpuBytes <= kAcceptableGpuBytes);
            std::cout << fmt::format("Real building: {}: {} to {} ({}{}{}): {}, the view back {} after it was asked "
                                     "(the floor built at {}, its job {}, the eye's area resident at {}; {} cover(s) "
                                     "in view, {} pending); the room whole at {}; GPU memory {:.1f} MiB, collection "
                                     "arrays {:.1f} MiB\n",
                                     name, ride.from, ride.to, ride.reason, ride.room.empty() ? "" : " ", ride.room,
                                     ride.outcome, ms(ride.totalMs), ms(ride.builtMs), ms(ride.jobMs),
                                     ms(ride.residentMs), ride.viewItems.value_or(0), ride.viewPending.value_or(0),
                                     ms(ride.roomResidentMs), mib(ride.gpuBytes), mib(ride.arrayBytes));
        }
        const rv::app::RideRecord& there = run.world.rides[0];
        const rv::app::RideRecord& back = run.world.rides[1];
        CHECK(there.to == rideFloor);
        CHECK(there.reason == "room");
        CHECK(there.room == largest);
        CHECK(back.to == rv::world::kGroundFloor);
        CHECK(back.reason == "floor");
        CHECK(back.outcome == "arrived");
        // Warm, the room's covers in the cache: its every item resident before the view comes back.
        if (name == "ride-warm") {
            CHECK(there.outcome == "arrived");
        } else {
            CHECK((there.outcome == "arrived" || there.outcome == "timed-out"));
        }
        CHECK(run.world.floor == rv::world::kGroundFloor);
        CHECK(run.gpuMemory.peakTotalBytes <= kAcceptableGpuBytes);
        // The arrays after each ride: the busiest floor's as a start on it has them (when the ride went there), then
        // the lobby's again.
        if (rideFloor == busiest) {
            CHECK(there.arrayBytes == busiestArrays);
        }
        CHECK(back.arrayBytes == second.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes);
        CHECK(run.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes ==
              second.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes);
    }
    // The acceptable rows (ADR-0013 §13): the view back within 10 s cold, within 3 s warm; the room that holds the walker
    // (the largest room, where the first ride leaves it) whole within 30 s cold, within 3 s warm.
    for (const rv::app::RideRecord& ride : rides[0].world.rides) {
        CHECK(ride.totalMs.value_or(1e9) <= 10000.0);
    }
    for (const rv::app::RideRecord& ride : rides[1].world.rides) {
        CHECK(ride.totalMs.value_or(1e9) <= 3000.0);
    }
    CHECK(rides[0].world.rides[0].roomResidentMs.value_or(1e9) <= 30000.0);
    CHECK(rides[1].world.rides[0].roomResidentMs.value_or(1e9) <= 3000.0);

    // The CLI's update on the unchanged library: the same update code, the whole of it (informative).
    const Clock::time_point cliStart = Clock::now();
    CHECK(runProgram(folder, "retroverse-cli.exe", "cli-update",
                     {"--data-dir", rv::core::pathToUtf8(data), "worldplan", "update"}) == 0);
    std::cout << fmt::format("Real building: retroverse-cli worldplan update, unchanged: {:.1f} s\n",
                             std::chrono::duration<double>(Clock::now() - cliStart).count());
    std::cout << fmt::format("Real building: {} rooms on {} floors, {} placements, {} unplaced\n",
                             stored->plan.rooms.size(), stored->plan.floors.size(), stored->plan.placements.size(),
                             stored->plan.unplaced.size());
}
