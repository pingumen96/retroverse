// The real library's busiest floor with both decoder slots playing (I5 step 3, ADR-0016 §5 and §12: the hero CRT and
// a secondary TV, GPU memory within the acceptable 512 MiB). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, the folders
// in the RV_REAL_* variables (or the script's parameters) and RV_AUDIO=null:
//   ./scripts/real-library.ps1 -Filter '[real-library][secondary_tv]'
// The whole library is imported into a new catalog under the build tree (build/<preset>/real-library/secondary_tv/
// data, library.json naming the real ES-DE home, install, ROM and media roots), then retroverse.exe of this build
// (hidden, lockstep):
//   - a first start in the lobby: the building planned and written;
//   - the busiest floor (the most placements) and on it the largest room whose pack stands a secondary TV
//     (world::secondaryTvFixtures()), started there (--start-room) with the walker where the hero CRT and the
//     secondary both stand in view, 300 frames, cold (the floor's covers derived in that run) and warm: both screens
//     playing their clips on the two slots, the floor's GPU memory at its peak within 512 MiB (runs without
//     --screenshot, whose render targets the window never has); then the same with a screenshot, and without clips
//     (informative: the frame times with and without the two decoders). Every run in a 1920 x 1080 window (I5 step
//     3's review: the render targets follow the window's size, and the default window is 1280 x 720).
// The figures are printed (the lines starting "Real secondary TV:"); the runs' reports and screenshots stay in the
// build tree (they show the user's covers and clips: never committed).

#include "support/child_process.hpp"
#include "support/fixture.hpp"

#include <rv/app/directory_data.hpp>
#include <rv/app/run_report.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <glm/glm.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr std::uint64_t kAcceptableGpuBytes = 512ull * 1024ull * 1024ull;
constexpr float kEyeHeight = 1.65f;

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

double mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

/// retroverse.exe with `arguments` on `data`, hidden, its output in <folder>/<name>.log and its report in
/// <folder>/<name>.json; the report read back. A 1920 x 1080 window (the measurement's: a full-HD screen's targets).
rv::app::RunReport runApp(const fs::path& folder, const fs::path& data, const std::string& name,
                          std::vector<std::string> arguments) {
    const fs::path report = folder / (name + ".json");
    arguments.insert(arguments.end(), {"--width", "1920", "--height", "1080", "--hidden", "--data-dir",
                                       rv::core::pathToUtf8(data), "--run-report", rv::core::pathToUtf8(report)});
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / "retroverse.exe";
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
    CHECK(run->exitCode == 0);
    auto read = rv::app::readRunReport(report);
    REQUIRE(read.has_value());
    return std::move(*read);
}

/// The middle of the screen of the scene's object `id` (its prefab's ScreenSurface at the object's transform).
glm::vec3 screenPointOf(const rv::scene::SceneDescription& scene, std::string_view id) {
    const rv::scene::SceneObject* object = scene.findObject(id);
    INFO("the scene object " << id);
    REQUIRE(object != nullptr);
    auto prefab = rv::geometry::buildPrefab(object->kind, object->params);
    REQUIRE(prefab.has_value());
    const rv::geometry::Socket* surface = rv::geometry::findSocket(prefab->sockets, "ScreenSurface");
    REQUIRE(surface != nullptr);
    return glm::vec3(object->instanceTransforms().front() * surface->transform *
                     glm::vec4(surface->bounds.center(), 1.0f));
}

/// "x,y,z,yaw,pitch" for --spawn: the walker in front of the middle between two screens, inside the room, far enough
/// that they subtend at most 55 degrees, looking at that middle (the room's frame).
std::string bothInView(const rv::world::Room& room, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec2 middle(0.5f * (a.x + b.x), 0.5f * (a.z + b.z));
    const glm::vec2 along(b.x - a.x, b.z - a.z);
    glm::vec2 normal = glm::normalize(glm::vec2(-along.y, along.x));
    const glm::vec2 centre(0.5f * static_cast<float>(room.minXMm + room.maxXMm) / 1000.0f,
                           0.5f * static_cast<float>(room.minZMm + room.maxZMm) / 1000.0f);
    if (glm::dot(centre - middle, normal) < 0.0f) {
        normal = -normal;
    }
    const float distance = 0.5f * glm::length(along) / std::tan(glm::radians(27.5f));
    glm::vec2 eye = middle + distance * normal;
    eye.x = std::clamp(eye.x, static_cast<float>(room.minXMm) / 1000.0f + 0.8f,
                       static_cast<float>(room.maxXMm) / 1000.0f - 0.8f);
    eye.y = std::clamp(eye.y, static_cast<float>(room.minZMm) / 1000.0f + 0.8f,
                       static_cast<float>(room.maxZMm) / 1000.0f - 0.8f);
    const glm::vec3 look(middle.x - eye.x, 0.5f * (a.y + b.y) - kEyeHeight, middle.y - eye.y);
    return fmt::format("{:.9g},0,{:.9g},{:.9g},{:.9g}", eye.x, eye.y,
                       rv::core::radiansToDegrees(std::atan2(look.x, -look.z)),
                       rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z))));
}

const rv::app::VideoScreenRecord* screenOf(const rv::app::VideoRecord& video, std::string_view screen) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [&](const auto& record) { return record.screen == screen; });
    return found != video.screens.end() ? &*found : nullptr;
}

std::string describe(const rv::app::VideoScreenRecord* screen) {
    if (screen == nullptr) {
        return "not bound";
    }
    return fmt::format("{} ({}, {}), slot {}", screen->picture, screen->screenClass, screen->gameId,
                       screen->slot ? std::to_string(*screen->slot) : std::string("none"));
}

} // namespace

TEST_CASE("app/world: the real library's busiest floor with the hero CRT and a secondary TV playing",
          "[.real-library][real-library][secondary_tv]") {
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
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "secondary_tv";
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
        std::cout << fmt::format("Real secondary TV: the whole library imported in {:.1f} s\n",
                                 std::chrono::duration<double>(Clock::now() - start).count());
    }

    // The first start, in the lobby: the building planned and written.
    const rv::app::RunReport first = runApp(
        folder, data, "first", {"--exit-after-frames", "--frames", "30", "--fixed-dt", "0.016667"});
    REQUIRE(first.world.bound);
    CHECK(first.world.hardFindings == 0);
    const auto stored = rv::world::readStoredPlan(rv::world::worldFiles(data, profile.id).plan);
    REQUIRE(stored.has_value());
    const rv::world::WorldPlan& plan = stored->plan;
    std::map<std::string, std::size_t> placements;
    for (const rv::world::Placement& placement : plan.placements) {
        ++placements[placement.slot.fixture().room().toString()];
    }

    // The busiest floor (the most placements, the first in the elevator's order on a tie).
    const std::vector<rv::app::WorldFloorRecord> floors = rv::app::floorRecordsOf(plan);
    REQUIRE_FALSE(floors.empty());
    const rv::app::WorldFloorRecord* busiest = &floors.front();
    for (const rv::app::WorldFloorRecord& record : floors) {
        if (record.placed > busiest->placed) {
            busiest = &record;
        }
    }
    // Its rooms and their secondary TVs; the largest room with one.
    const fs::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const rv::world::Room* chosen = nullptr;
    std::string secondary;
    std::size_t floorItems = 0;
    std::size_t withTv = 0;
    const std::vector<const rv::world::Room*> rooms = rv::world::roomsOnFloor(plan, busiest->id);
    for (const rv::world::Room* room : rooms) {
        floorItems += placements[room->id.toString()];
        auto sceneData = rv::world::loadSceneData(assets, plan, room->id);
        REQUIRE(sceneData.has_value());
        auto tvs = rv::world::secondaryTvFixtures(plan, room->id, *sceneData);
        REQUIRE(tvs.has_value());
        CHECK(tvs->size() <= (room->maxZMm - room->minZMm > rv::world::kSecondaryTvTwoPastDepthMm ? 2u : 1u));
        std::cout << fmt::format("Real secondary TV: {} on {}: {} items, {} m deep, {} secondary TV(s){}\n",
                                 room->id.toString(), busiest->id, placements[room->id.toString()],
                                 static_cast<double>(room->maxZMm - room->minZMm) / 1000.0, tvs->size(),
                                 tvs->empty() ? std::string() : " at " + tvs->front().localText());
        if (tvs->empty()) {
            continue;
        }
        ++withTv;
        if (chosen == nullptr || placements[room->id.toString()] > placements[chosen->id.toString()]) {
            chosen = room;
            secondary = tvs->front().localText();
        }
    }
    std::cout << fmt::format("Real secondary TV: the busiest floor {} ({} rooms, {} items), {} room(s) with a "
                             "secondary TV\n",
                             busiest->id, rooms.size(), floorItems, withTv);
    INFO("a room of the busiest floor with a secondary TV");
    REQUIRE(chosen != nullptr);
    auto sceneData = rv::world::loadSceneData(assets, plan, chosen->id);
    REQUIRE(sceneData.has_value());
    auto scene = rv::world::toSceneDescription(plan, chosen->id, *sceneData);
    REQUIRE(scene.has_value());
    const std::string roomId = chosen->id.toString();
    const std::string spawn =
        bothInView(*chosen, screenPointOf(*scene, "fx:hero.crt"), screenPointOf(*scene, secondary));

    // The room started with both screens in view, cold then warm: both slots playing, the floor's GPU memory. No
    // screenshot in these runs: --screenshot adds its own render targets at the screenshot's size (about 72 MiB), which
    // the user's window never has; the screenshot is a run of its own after them.
    for (const std::string name : {"both-cold", "both-warm", "both-screenshot"}) {
        std::vector<std::string> arguments{"--start-room", roomId, "--spawn", spawn, "--exit-after-frames", "--frames",
                                           "300", "--fixed-dt", "0.016667"};
        const bool shot = name == "both-screenshot";
        if (shot) {
            arguments.insert(arguments.end(), {"--screenshot", rv::core::pathToUtf8(folder / (name + ".png"))});
        }
        const rv::app::RunReport run = runApp(folder, data, name, arguments);
        INFO(name);
        CHECK(run.world.state == "room");
        CHECK(run.world.room == roomId);
        CHECK(run.world.floor == busiest->id);
        const rv::app::VideoRecord& video = run.video;
        REQUIRE(video.bound);
        CHECK(video.maxOpenDecoders <= 2u);
        CHECK(video.maxDecoderThreads <= 2u);
        CHECK(video.joinedAtExit);
        REQUIRE(run.audio.has_value());
        CHECK(run.audio->device == "null");
        const rv::app::VideoScreenRecord* hero = screenOf(video, roomId + "/fx:hero.crt");
        const rv::app::VideoScreenRecord* tv = screenOf(video, roomId + "/" + secondary);
        REQUIRE(hero != nullptr);
        REQUIRE(tv != nullptr);
        CHECK(tv->kind == "secondary");
        CHECK(tv->screenClass == "ambient");
        CHECK(hero->visible);
        CHECK(tv->visible);
        // Both slots playing (the hero's clip and the secondary's own playlist's).
        CHECK(video.maxOpenDecoders == 2u);
        CHECK(hero->picture == "video");
        CHECK(tv->picture == "video");
        if (!shot) {
            CHECK(run.gpuMemory.peakTotalBytes <= kAcceptableGpuBytes);
        }
        std::cout << fmt::format(
            "Real secondary TV: {}: {} ({} items on the floor {}), the hero {}, the secondary {} {}; at most {} "
            "decoder(s) open; GPU memory at its peak {:.1f} MiB (video {:.1f} MiB at the end), frame p50 {:.2f} ms, "
            "p99 {:.2f} ms\n",
            name, roomId, run.world.items, busiest->id, describe(hero), secondary, describe(tv), video.maxOpenDecoders,
            mib(run.gpuMemory.peakTotalBytes), mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::Video).bytes),
            run.frameTimes.times ? run.frameTimes.times->p50Ms : 0.0,
            run.frameTimes.times ? run.frameTimes.times->p99Ms : 0.0);
        std::cout << fmt::format("Real secondary TV: {}: procedural surfaces {:.1f} MiB, collection arrays {:.1f} MiB, "
                                 "hero textures {:.1f} MiB, targets {:.1f} MiB, scene textures {:.1f} MiB, buffers "
                                 "{:.1f} MiB\n",
                                 name, mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::ProceduralSurfaces).bytes),
                                 mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes),
                                 mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::HeroTextures).bytes),
                                 mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::Targets).bytes),
                                 mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::SceneTextures).bytes),
                                 mib(run.gpuMemory.usage(rv::gl::GpuMemoryKind::Buffers).bytes));
    }

    // The same start without clips (video.json's enabled false, written for this run and removed after): what the
    // floor holds without the two slots, for the figures (informative).
    const fs::path videoJson = data / "config" / "video.json";
    REQUIRE(rv::core::writeTextFileAtomic(videoJson, "{\"enabled\": false}\n").has_value());
    const rv::app::RunReport stills =
        runApp(folder, data, "clips-off",
               {"--start-room", roomId, "--spawn", spawn, "--exit-after-frames", "--frames", "300", "--fixed-dt",
                "0.016667"});
    fs::remove(videoJson, removed);
    CHECK(stills.video.decodersStarted == 0u);
    std::cout << fmt::format("Real secondary TV: clips-off: GPU memory at its peak {:.1f} MiB (video {:.1f} MiB), "
                             "targets {:.1f} MiB, frame p50 {:.2f} ms, p99 {:.2f} ms\n",
                             mib(stills.gpuMemory.peakTotalBytes),
                             mib(stills.gpuMemory.usage(rv::gl::GpuMemoryKind::Video).bytes),
                             mib(stills.gpuMemory.usage(rv::gl::GpuMemoryKind::Targets).bytes),
                             stills.frameTimes.times ? stills.frameTimes.times->p50Ms : 0.0,
                             stills.frameTimes.times ? stills.frameTimes.times->p99Ms : 0.0);
}
