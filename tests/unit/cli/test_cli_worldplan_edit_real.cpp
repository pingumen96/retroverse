// A CLI edit session on the real library's plan (Gate M3, I4b step 9; ADR-0015 §4, §6, §8): a move, a swap, a pin and
// a fixture moved as one `worldplan edit --script` session behind a snapshot, `worldplan check` measuring the pins
// against the plan (each honoured), an update of the unchanged catalog moving nothing and writing nothing, then
// `worldplan undo` restoring the plan's SHA-256 and the world without overrides. Hidden ([.real-library]): ctest never
// runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after (0
// differences), and the folders in the RV_REAL_* variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][edit-cli]'
// The whole library is imported into a new catalog in a data directory under the build tree
// (build/<preset>/real-library/edit-cli/data, emptied first; library.json naming the real ES-DE home, install, ROM and
// media roots) and its building generated. The session edits room:snes (else the room with the most placements that
// has a free spine slot): its first game moved into a free spine slot, two others swapped, a fourth pinned in place,
// and a fixture of the room (a decoration, else a side-wall module or display fixture) moved 100 or 200 mm along x or z
// to the first place a dry run accepts. The figures go to stdout ("Real edit:" lines, for docs/testing/gate-m3.md);
// nothing derived from the user's titles is written outside that folder (docs/reference/safety.md: never under %TEMP%).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

/// The file's bytes, or nothing when it is not there.
std::optional<std::string> contentOf(const fs::path& path) {
    auto text = rv::core::readTextFile(path);
    if (!text) {
        REQUIRE(text.error().code == rv::ErrorCode::NotFound);
        return std::nullopt;
    }
    return *text;
}

/// The spine-out slots of `room` that hold no game, the undressed ones first.
std::vector<rv::world::SlotId> freeSpineSlots(const rv::world::WorldPlan& plan, const rv::world::RoomId& room) {
    std::set<rv::world::SlotId> taken;
    for (const rv::world::Placement& placement : plan.placements) {
        taken.insert(placement.slot);
    }
    std::vector<rv::world::SlotId> slots;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != room) {
            continue;
        }
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            if (level.section != rv::world::Section::Spine) {
                continue;
            }
            for (std::uint32_t index = 0; index < level.slots; ++index) {
                const rv::world::SlotId slot = *rv::world::SlotId::make(fixture.id, level.level, index);
                const bool dressed = std::any_of(plan.dressing.begin(), plan.dressing.end(), [&](const auto& span) {
                    return span.level == slot.levelId() && index >= span.firstSlot &&
                           index < span.firstSlot + span.count;
                });
                if (!taken.contains(slot)) {
                    slots.insert(dressed ? slots.end() : slots.begin(), slot);
                }
            }
        }
    }
    return slots;
}

} // namespace

TEST_CASE("cli/worldplan edit: a session on the real library's plan, checked, updated and undone",
          "[cli][worldplan][edit-cli][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    const LoggingReset loggingReset;
    // Under the build tree, never in the library nor in %TEMP%.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "edit-cli";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    fs::create_directories(folder, removed);
    REQUIRE_FALSE(removed);
    rv::core::PathEnvironment environment;
    environment.executableDir = folder;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(folder / "data")});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return std::make_tuple(code, out.text(), err.text());
    };
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    };

    const auto [created, createdOut, createdErr] =
        run({"library", "create", "--home", rv::core::pathToUtf8(home->parent_path()), "--install",
             rv::core::pathToUtf8(*install), "--rom-root", rv::core::pathToUtf8(*romRoot), "--media-root",
             rv::core::pathToUtf8(*mediaRoot)});
    INFO(createdErr);
    REQUIRE(created == ExitCode::Ok);
    Clock::time_point start = Clock::now();
    const auto [imported, importedOut, importedErr] = run({"import"});
    INFO(importedErr);
    REQUIRE(imported == ExitCode::Ok);
    std::cout << fmt::format("Real edit: the whole library imported in {:.1f} s\n", seconds(start));
    start = Clock::now();
    const auto [generated, generatedOut, generatedErr] = run({"worldplan", "generate", "--json"});
    INFO(generatedErr);
    REQUIRE(generated == ExitCode::Ok);
    std::cout << fmt::format("Real edit: the building generated in {:.1f} s\n", seconds(start));
    const fs::path planFile = rv::core::pathFromUtf8(nlohmann::json::parse(generatedOut)["plan"].get<std::string>());
    const rv::world::WorldFiles files = rv::world::WorldFiles::in(planFile.parent_path());
    const auto original = contentOf(planFile);
    REQUIRE(original.has_value());
    REQUIRE_FALSE(contentOf(files.overrides).has_value());
    const std::string originalSha = rv::core::sha256Hex(std::string_view(*original));
    auto plan = rv::world::readPlan(*original, "plan");
    REQUIRE(plan.has_value());

    // The room: room:snes, else the room with the most placements that has a free spine slot.
    std::map<rv::world::RoomId, std::vector<rv::world::Placement>> placed;
    for (const rv::world::Placement& placement : plan->placements) {
        placed[placement.slot.fixture().room()].push_back(placement);
    }
    std::vector<std::pair<std::size_t, rv::world::RoomId>> rooms;
    for (const auto& [id, games] : placed) {
        rooms.emplace_back(id.toString() == "room:snes" ? SIZE_MAX : games.size(), id);
    }
    std::sort(rooms.rbegin(), rooms.rend());
    std::optional<rv::world::RoomId> room;
    std::vector<rv::world::SlotId> free;
    for (const auto& [count, id] : rooms) {
        if (placed[id].size() >= 4 && !(free = freeSpineSlots(*plan, id)).empty()) {
            room = id;
            break;
        }
    }
    REQUIRE(room.has_value());
    const std::vector<rv::world::Placement>& games = placed[*room];
    const std::string slot = free.front().toString();
    // The game moved: the first of the room's games that a dry run moves into the slot; the next two swapped, the next
    // pinned.
    const auto accepted = [&](std::vector<std::string> operation) {
        operation.insert(operation.begin(), {"worldplan", "edit"});
        operation.push_back("--dry-run");
        return std::get<0>(run(operation)) == ExitCode::Ok;
    };
    std::size_t first = 0;
    while (first + 4 <= games.size() && first < 8 &&
           !accepted({"move", "--game", games[first].game.toString(), "--slot", slot})) {
        ++first;
    }
    REQUIRE(first + 4 <= games.size());
    REQUIRE(first < 8);
    const std::string moved = games[first].game.toString();
    const std::string swapA = games[first + 1].game.toString();
    const std::string swapB = games[first + 2].game.toString();
    const std::string pinned = games[first + 3].game.toString();
    REQUIRE(accepted({"swap", "--game", swapA, "--with", swapB}));

    // The fixture: the first of the room's movable fixtures that a dry run accepts 100 or 200 mm away along x or z.
    std::vector<const rv::world::Fixture*> movable;
    for (const rv::world::FixtureRole role :
         {rv::world::FixtureRole::Decoration, rv::world::FixtureRole::SideModule, rv::world::FixtureRole::Display}) {
        for (const rv::world::Fixture& fixture : plan->fixtures) {
            if (fixture.id.room() == *room && fixture.role == role) {
                movable.push_back(&fixture);
            }
        }
    }
    struct Place {
        std::string fixture;
        std::int64_t xMm = 0;
        std::int64_t zMm = 0;
        std::int32_t yawCdeg = 0;
    };
    std::optional<Place> place;
    std::size_t tried = 0;
    start = Clock::now();
    constexpr std::size_t kMaxTries = 24;
    for (const rv::world::Fixture* fixture : movable) {
        for (const auto& [dx, dz] : std::vector<std::pair<std::int64_t, std::int64_t>>{
                 {100, 0}, {-100, 0}, {0, 100}, {0, -100}, {200, 0}, {-200, 0}, {0, 200}, {0, -200}}) {
            if (place || tried >= kMaxTries) {
                break;
            }
            ++tried;
            const Place candidate{fixture->id.toString(), fixture->xMm + dx, fixture->zMm + dz, fixture->yawCdeg};
            const auto [code, out, err] =
                run({"worldplan", "edit", "move-fixture", "--fixture", candidate.fixture, "--x-mm",
                     std::to_string(candidate.xMm), "--z-mm", std::to_string(candidate.zMm), "--yaw-cdeg",
                     std::to_string(candidate.yawCdeg), "--dry-run"});
            if (code == ExitCode::Ok) {
                place = candidate;
            }
        }
    }
    INFO(tried << " places tried for " << movable.size() << " movable fixtures of " << room->toString());
    REQUIRE(place.has_value());
    std::cout << fmt::format("Real edit: {} ({} placed): {} of its {} movable fixtures' places tried by dry runs in "
                             "{:.1f} s, {} moved to x {} z {} mm\n",
                             room->toString(), games.size(), tried, movable.size(), seconds(start), place->fixture,
                             place->xMm, place->zMm);
    CHECK(contentOf(planFile) == original);
    CHECK_FALSE(contentOf(files.overrides).has_value());

    // The session: a move, a swap, a pin and the fixture moved, all or nothing, one snapshot.
    const fs::path script = folder / "session.json";
    const std::string scriptText = fmt::format(
        "{{\"schemaVersion\": 1, \"operations\": [\n"
        "  {{\"op\": \"move\", \"game\": \"{}\", \"slot\": \"{}\"}},\n"
        "  {{\"op\": \"swap\", \"game\": \"{}\", \"with\": \"{}\"}},\n"
        "  {{\"op\": \"pin\", \"game\": \"{}\"}},\n"
        "  {{\"op\": \"move-fixture\", \"fixture\": \"{}\", \"xMm\": {}, \"zMm\": {}, \"yawCdeg\": {}}}]}}\n",
        moved, slot, swapA, swapB, pinned, place->fixture, place->xMm, place->zMm, place->yawCdeg);
    REQUIRE(rv::core::writeTextFileAtomic(script, scriptText).has_value());
    start = Clock::now();
    const auto [edited, editedOut, editedErr] =
        run({"worldplan", "edit", "--script", rv::core::pathToUtf8(script), "--json"});
    const double editSeconds = seconds(start);
    INFO(editedOut << editedErr);
    REQUIRE(edited == ExitCode::Ok);
    const nlohmann::json session = nlohmann::json::parse(editedOut);
    CHECK(session["written"] == true);
    CHECK(session["changed"] == true);
    CHECK(session["operations"].size() == 4);
    CHECK(session["gamePins"] == 4);
    CHECK(session["fixturePins"] == 1);
    CHECK(session["snapshot"].is_string());
    const auto editedPlan = contentOf(planFile);
    REQUIRE(editedPlan.has_value());
    CHECK(*editedPlan != *original);
    const auto overrides = contentOf(files.overrides);
    REQUIRE(overrides.has_value());
    std::cout << fmt::format("Real edit: the session (a move, a swap, a pin, a fixture moved) saved in {:.1f} s behind "
                             "the snapshot {}\n",
                             editSeconds, session["snapshot"].get<std::string>());

    // worldplan check: every pin honoured, the plan passing its hard rules.
    start = Clock::now();
    const auto [checked, checkedOut, checkedErr] =
        run({"worldplan", "check", rv::core::pathToUtf8(planFile), "--json"});
    const double checkSeconds = seconds(start);
    INFO(checkedErr);
    CHECK(checked == ExitCode::Ok);
    const nlohmann::json check = nlohmann::json::parse(checkedOut);
    CHECK(check["passed"] == true);
    const nlohmann::json& pins = check["overrides"];
    CHECK(pins["honoured"] == 5);
    CHECK(pins["orphaned"] == 0);
    CHECK(pins["conflicting"] == 0);
    CHECK(pins["unhonoured"] == 0);
    std::cout << fmt::format("Real edit: worldplan check in {:.1f} s: {} hard and {} soft findings; pins {} honoured, "
                             "{} orphaned, {} conflicting, {} unhonoured\n",
                             checkSeconds, check["hardCount"].get<std::size_t>(), check["softCount"].get<std::size_t>(),
                             pins["honoured"].get<std::size_t>(), pins["orphaned"].get<std::size_t>(),
                             pins["conflicting"].get<std::size_t>(), pins["unhonoured"].get<std::size_t>());

    // The catalog unchanged: the update moves nothing and writes nothing.
    start = Clock::now();
    const auto [updated, updatedOut, updatedErr] = run({"worldplan", "update"});
    const double updateSeconds = seconds(start);
    INFO(updatedOut << updatedErr);
    CHECK(updated == ExitCode::Ok);
    CHECK(updatedOut.find("is up to date") != std::string::npos);
    CHECK(contentOf(planFile) == editedPlan);
    CHECK(contentOf(files.overrides) == overrides);
    std::cout << fmt::format("Real edit: the update after the session in {:.1f} s: up to date, nothing written\n",
                             updateSeconds);

    // Undone: the plan's SHA-256 as generated, no overrides.
    start = Clock::now();
    const auto [undone, undoneOut, undoneErr] = run({"worldplan", "undo"});
    const double undoSeconds = seconds(start);
    INFO(undoneOut << undoneErr);
    REQUIRE(undone == ExitCode::Ok);
    CHECK(undoneOut.find("Undid edit of " + room->toString()) != std::string::npos);
    const auto restored = contentOf(planFile);
    REQUIRE(restored.has_value());
    const std::string restoredSha = rv::core::sha256Hex(std::string_view(*restored));
    CHECK(restoredSha == originalSha);
    CHECK_FALSE(contentOf(files.overrides).has_value());
    std::cout << fmt::format("Real edit: undone in {:.1f} s: the plan's SHA-256 {} the generated one, overrides.json "
                             "{}\n",
                             undoSeconds, restoredSha == originalSha ? "equal to" : "NOT equal to",
                             contentOf(files.overrides).has_value() ? "still there" : "removed");
}
