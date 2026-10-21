// The live update of the room in retroverse.exe (I3 step 15b), on the snes-room fixture (tests/fixtures/esde/snes-room:
// 50 invented titles with generated covers; G10's additions laid over it one ROM and cover at a time) recorded as the
// library of the case's data directory and imported with retroverse-cli. A ROM added since the last run and
// --refresh-library: the refresh imports it before the room's snapshot, the stored room's reconciliation places exactly
// one new cartridge (its level's dressing taken: the room committed again with its pictures kept, the walker where it
// stands) and moves nothing. A ROM added while the room shows and "Aggiorna libreria" pressed: the room's job runs
// again (world::update, the plan saved), the new title is placed into its free slot in place (the static part, the
// walker and every other item untouched, their pictures not uploaded again), its cover loaded by the cover loader; the
// room then renders as a fresh start of the same catalog does (outside the hero CRT's picture). Every run is hidden and
// lockstep
// (--fixed-dt).

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// A pixel differs when a channel moves by more than this (retroverse-screenshot-diff's default).
constexpr int kPixelThreshold = 6;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The snes-room fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("world_live/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    /// Lays G10's `title` (its ROM and cover) over the fixture's root: the catalog knows it after an import.
    void addGame(std::string_view title) const {
        const std::string rom = "roms/snes/" + std::string(title) + " (USA).sfc";
        const std::string cover = "media/snes/covers/" + std::string(title) + " (USA).png";
        for (const std::string& file : {rom, cover}) {
            std::error_code error;
            fs::copy_file(fixture.path("additions/" + file), fixture.path(file), error);
            INFO(file << ": " << error.message());
            REQUIRE_FALSE(error);
        }
    }
};

std::vector<std::string> roomArguments(std::string_view frames, const std::optional<fs::path>& shot = std::nullopt,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames", std::string(frames), "--fixed-dt",
                                       "0.016667"};
    if (shot) {
        arguments.insert(arguments.end(), {"--screenshot", arg(*shot)});
    }
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

fs::path writeScript(const fs::path& dir, std::string_view name, const Json& steps) {
    const fs::path file = dir / rv::core::pathFromUtf8(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

/// The plan stored after the run and its room's collection and scene as retroverse.exe makes them (its build's assets).
struct StoredRoom {
    rv::world::StoredPlan stored;
    rv::world::CollectionLayout layout;
    rv::scene::SceneDescription scene;
};

StoredRoom storedRoom(const ImportedRoom& room, const rv::app::RunReport& report) {
    const auto library = rv::catalog::LibraryId::parse(report.world.library);
    REQUIRE(library.has_value());
    auto stored = rv::world::readStoredPlan(rv::world::worldFiles(room.data, *library).plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    return StoredRoom{std::move(*stored), std::move(*layout), std::move(*scene)};
}

/// The placements of `before` found byte for byte in `after`, and the items of `before` at the same pick id and
/// transform in `after` (the instance transforms compared).
struct Standing {
    std::size_t placements = 0;
    std::size_t unmoved = 0;
};

Standing standingOf(const StoredRoom& before, const StoredRoom& after) {
    Standing standing;
    for (const rv::world::Placement& placement : before.stored.plan.placements) {
        standing.placements += std::find(after.stored.plan.placements.begin(), after.stored.plan.placements.end(),
                                         placement) != after.stored.plan.placements.end()
                                   ? 1
                                   : 0;
    }
    for (const rv::world::CollectionItem& item : before.layout.items) {
        const rv::world::CollectionItem* again = after.layout.findGame(item.game);
        standing.unmoved +=
            again != nullptr && again->pickId == item.pickId && again->transform == item.transform ? 1 : 0;
    }
    return standing;
}

/// Where the room's hero CRT appears in a kShotSize screenshot from the scene's camera, with a margin for its bloom.
std::pair<glm::ivec2, glm::ivec2> crtRegion(const rv::scene::SceneDescription& scene) {
    constexpr int kBloomMargin = 96;
    const auto crt = std::find_if(scene.objects.begin(), scene.objects.end(), [](const rv::scene::SceneObject& object) {
        return object.source == rv::scene::SceneObject::Source::Prefab && object.kind == "crt";
    });
    REQUIRE(crt != scene.objects.end());
    auto prefab = rv::geometry::buildPrefab(crt->kind, crt->params);
    REQUIRE(prefab.has_value());
    const rv::core::Aabb bounds = prefab->bounds.transformed(crt->transform);
    glm::ivec2 min(kShotSize);
    glm::ivec2 max(0);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                              (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                              (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
        const std::optional<glm::ivec2> pixel = projectToPixel(scene.camera, kShotSize, point);
        REQUIRE(pixel.has_value());
        min = glm::min(min, *pixel);
        max = glm::max(max, *pixel);
    }
    return {glm::max(min - kBloomMargin, glm::ivec2(0)), glm::min(max + kBloomMargin, kShotSize - 1)};
}

/// The pixels of `a` and `b` outside `region` that differ by more than kPixelThreshold.
std::size_t changesOutside(const rv::assets::ImageData& a, const rv::assets::ImageData& b,
                           const std::pair<glm::ivec2, glm::ivec2>& region) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    std::size_t changed = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (x >= region.first.x && y >= region.first.y && x <= region.second.x && y <= region.second.y) {
                continue;
            }
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(a.channels);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(static_cast<int>(a.pixels[at + channel]) - static_cast<int>(b.pixels[at + channel])) >
                    kPixelThreshold) {
                    ++changed;
                    break;
                }
            }
        }
    }
    return changed;
}

} // namespace

TEST_CASE("world: a ROM added and --refresh-library: exactly one new cartridge, no other item moved, the plan saved",
          "[gpu][world][world_live]") {
    ImportedRoom room("refresh_library");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments("5"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    REQUIRE(first.report->world.items == 50);
    const StoredRoom before = storedRoom(room, *first.report);

    // The ROM and its cover only: the run's refresh imports them (retroverse-cli is not run again).
    room.addGame("Zinnia Zephyr");
    const AppRun refreshed =
        runApp(retroverseExe(), room.dir, "refreshed", roomArguments("60", std::nullopt, {"--refresh-library"}));
    requireCleanRun(refreshed);
    INFO(refreshed.describe());
    const rv::app::RunReport& report = *refreshed.report;
    REQUIRE(report.catalog.has_value());
    REQUIRE(report.catalog->refreshes.size() == 1);
    CHECK(report.catalog->refreshes.front().outcome == "done");
    CHECK(report.catalog->refreshes.front().added == 1);
    const rv::app::WorldRecord& world = report.world;
    CHECK(world.state == "room");
    CHECK(world.items == 51);
    CHECK(world.spine == 1);
    CHECK(world.featured == 44);
    CHECK(world.band == 6);
    // The stored room first, then reconciled with the refreshed catalog's snapshot (the room's job waits behind the
    // refresh on the catalog worker): the first title past the face-out set takes a dressed level, so the room is
    // committed again, the pictures of the 50 that stood kept.
    REQUIRE(world.reconcile.has_value());
    CHECK(world.reconcile->kind == "rebuild");
    CHECK(world.reconcile->kept == 50);
    CHECK(world.reconcile->moved == 0);
    CHECK(world.reconcile->added == 1);
    CHECK(world.reconcile->removed == 0);
    // The refresh's own live update then finds the room as the catalog is.
    REQUIRE(world.liveUpdates.size() == 1);
    CHECK(world.liveUpdates.front().kind == "unchanged");
    CHECK(world.liveUpdates.front().outcome == "unchanged");
    CHECK_FALSE(world.liveUpdates.front().planWritten);
    const rv::app::ResidencyRecord& residency = report.residency;
    CHECK(residency.frontsResident == 51);
    CHECK(residency.spinesResident == 51);
    CHECK(residency.adoptedItems == 50);
    CHECK(residency.adoptedChanged == 0);

    const StoredRoom after = storedRoom(room, report);
    CHECK(after.stored.plan.placements.size() == 51);
    const Standing standing = standingOf(before, after);
    CHECK(standing.placements == 50);
    CHECK(standing.unmoved == 50);
}

TEST_CASE(
    "world: a ROM added while the room shows and Aggiorna libreria pressed: placed live in its slot, nothing else "
    "moved or reloaded",
    "[gpu][world][world_live]") {
    ImportedRoom room("aggiorna");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments("5"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    // A first addition, imported between the runs: its level is undressed from now on.
    room.addGame("Zinnia Zephyr");
    room.cli("import_zinnia", {"import"});
    const AppRun second = runApp(retroverseExe(), room.dir, "second", roomArguments("5"));
    requireCleanRun(second);
    REQUIRE(second.report->world.items == 51);
    const StoredRoom before = storedRoom(room, *second.report);

    // The next addition lies in the ROM folder only; the quick catalog opens on frame 2, Home and Up four times reach
    // "Aggiorna libreria" (tests/input_scripts/catalog_refresh.json's path), A presses it on frame 20, the quick
    // catalog closes on frame 24. The room's frames go on during the refresh; its live update is waited for (lockstep).
    room.addGame("Kestrel Kite Festival");
    const fs::path script =
        writeScript(room.dir, "aggiorna.json",
                    Json::array({{{"fromFrame", 2}, {"toFrame", 2}, {"actions", {{"quick_catalog", 1}}}},
                                 {{"fromFrame", 4}, {"toFrame", 4}, {"actions", {{"ui_home", 1}}}},
                                 {{"fromFrame", 6}, {"toFrame", 6}, {"actions", {{"ui_up", 1}}}},
                                 {{"fromFrame", 8}, {"toFrame", 8}, {"actions", {{"ui_up", 1}}}},
                                 {{"fromFrame", 10}, {"toFrame", 10}, {"actions", {{"ui_up", 1}}}},
                                 {{"fromFrame", 12}, {"toFrame", 12}, {"actions", {{"ui_up", 1}}}},
                                 {{"fromFrame", 20}, {"toFrame", 20}, {"actions", {{"ui_activate", 1}}}},
                                 {{"fromFrame", 24}, {"toFrame", 24}, {"actions", {{"quick_catalog", 1}}}}}));
    const fs::path liveShot = room.dir / "live.png";
    const AppRun live =
        runApp(retroverseExe(), room.dir, "live", roomArguments("240", liveShot, {"--input-script", arg(script)}));
    requireCleanRun(live);
    INFO(live.describe());
    const rv::app::RunReport& report = *live.report;
    REQUIRE(report.catalog.has_value());
    REQUIRE(report.catalog->refreshes.size() == 1);
    CHECK(report.catalog->refreshes.front().trigger == "user");
    CHECK(report.catalog->refreshes.front().outcome == "done");
    CHECK(report.catalog->refreshes.front().added == 1);
    const rv::app::WorldRecord& world = report.world;
    // The start: the stored room, reconciled at frame 1 with the catalog as it was (nothing new imported yet).
    REQUIRE(world.reconcile.has_value());
    CHECK(world.reconcile->kind == "textures");
    // The refresh's live update: the new title into a free slot of the undressed level, applied in place.
    REQUIRE(world.liveUpdates.size() == 1);
    const rv::app::WorldLiveUpdateRecord& update = world.liveUpdates.front();
    CHECK(update.trigger == "refresh");
    CHECK(update.requestFrame > 20);
    CHECK(update.frame >= update.requestFrame);
    CHECK(update.kind == "items");
    CHECK(update.outcome == "updated");
    CHECK(update.planWritten);
    CHECK(update.added == 1);
    CHECK(update.kept == 51);
    CHECK(update.moved == 0);
    CHECK(update.removed == 0);
    CHECK(update.flagsChanged == 0);
    CHECK(update.items == 52);
    CHECK(update.staticRecords == 156);
    CHECK(world.items == 52);
    CHECK(world.spine == 2);
    CHECK(world.planWritten);
    // The covers: the 51 that stood adopted and none uploaded again; the new one's front and spine made and cached.
    const rv::app::ResidencyRecord& residency = report.residency;
    CHECK(residency.frontsResident == 52);
    CHECK(residency.spinesResident == 52);
    CHECK(residency.adoptedItems == 51);
    CHECK(residency.adoptedChanged == 0);
    CHECK(residency.cacheStored == 2);

    const StoredRoom after = storedRoom(room, report);
    CHECK(after.stored.plan.placements.size() == 52);
    const Standing standing = standingOf(before, after);
    CHECK(standing.placements == 51);
    CHECK(standing.unmoved == 51);

    // A fresh start of the same catalog: the walker where the live run left it, the room alike but for the CRT's
    // picture (its clip at another time).
    const fs::path freshShot = room.dir / "fresh.png";
    const AppRun fresh = runApp(retroverseExe(), room.dir, "fresh", roomArguments("240", freshShot));
    requireCleanRun(fresh);
    INFO(fresh.describe());
    CHECK(fresh.report->world.outcome == "unchanged");
    REQUIRE(fresh.report->world.reconcile.has_value());
    CHECK(fresh.report->world.reconcile->kind == "textures");
    REQUIRE(report.finalCamera.has_value());
    REQUIRE(fresh.report->finalCamera.has_value());
    CHECK(glm::length(report.finalCamera->feet - fresh.report->finalCamera->feet) < 1e-4f);
    const rv::assets::ImageData liveImage = requireScreenshot(live, liveShot, kShotSize);
    const rv::assets::ImageData freshImage = requireScreenshot(fresh, freshShot, kShotSize);
    const std::size_t changed = changesOutside(liveImage, freshImage, crtRegion(after.scene));
    INFO(changed << " pixel(s) differ outside the CRT");
    CHECK(changed <= static_cast<std::size_t>(kShotSize.x) * static_cast<std::size_t>(kShotSize.y) / 200);
}
