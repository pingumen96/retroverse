// The covers of the generated room (I3 step 12; ADR-0011 §6 to §8): the cover loader's order (the hero item, the items
// in view by projected size, the rest by distance), its budget per frame, the hero texture of the hovered item (its
// cover at up to 1,024 px drawn by the dynamic path in place of its front), the lockstep of fixed-time runs narrowed to
// the frame's view (the whole room before the last frame), a media root unplugged after a first run (the cache serves
// what it holds, generated labels the rest) and tried again after a refresh, and the RGBA8 fallback; on the snes-room
// fixture (50 titles) and on snes-stress (2,000 titles). Every run hidden; the runs without --fixed-dt never wait.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
constexpr float kEyeHeight = 1.65f;
/// ADR-0011 §8: 8 hero textures of at most 1,024 px, about 32 MiB.
constexpr std::uint64_t kHeroBudgetBytes = 32ull * 1024ull * 1024ull;

/// A fixture recorded as the library of <caseDir>/data and imported with retroverse-cli (as test_world_room.cpp's).
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    ImportedRoom(std::string_view name, std::string_view fixtureName)
        : fixture(rv::test::Fixture::materialize(fixtureName).value()),
          dir(freshCaseDir(std::string("covers/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseExe().parent_path() / "retroverse-cli.exe", dir,
                                      std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    /// Moves the fixture's media folder away (the media root unplugged) or back.
    void moveMedia(bool away) const {
        std::error_code error;
        if (away) {
            fs::rename(fixture.path("media"), fixture.path("media-away"), error);
        } else {
            fs::rename(fixture.path("media-away"), fixture.path("media"), error);
        }
        INFO(error.message());
        REQUIRE_FALSE(error);
    }
};

std::vector<std::string> lockstep(std::string_view frames, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames", std::string(frames), "--fixed-dt",
                                       "0.016667"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

std::vector<std::string> unpaced(std::string_view frames, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames", std::string(frames)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The room's collection as the run made it from its plan.
struct ShownRoom {
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const ImportedRoom& room, const rv::app::RunReport& report) {
    const fs::path plan =
        rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan;
    auto stored = rv::world::readStoredPlan(plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    return ShownRoom{std::move(*scene), std::move(*layout)};
}

/// The walker `distance` in front of `item`'s visible face, looking at its middle, and the camera it gives.
struct Viewpoint {
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
    glm::vec3 face{0.0f};
};

Viewpoint viewpointOf(const rv::world::CollectionItem& item, float distance, const rv::scene::CameraSetup& lens) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 center = glm::vec3(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out = glm::vec3(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 eye(center.x + distance * out.x, kEyeHeight, center.z + distance * out.z);
    const glm::vec3 look = center - eye;
    Viewpoint viewpoint;
    viewpoint.face = center;
    viewpoint.spawn.feet = glm::vec3(eye.x, 0.0f, eye.z);
    viewpoint.spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    viewpoint.spawn.pitchDeg =
        rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    viewpoint.camera = lens;
    viewpoint.camera.position = eye;
    viewpoint.camera.yawDeg = *viewpoint.spawn.yawDeg;
    viewpoint.camera.pitchDeg = *viewpoint.spawn.pitchDeg;
    return viewpoint;
}

const rv::world::CollectionItem& firstOf(const ShownRoom& room, rv::world::Section section) {
    const auto found = std::find_if(room.layout.items.begin(), room.layout.items.end(),
                                    [&](const rv::world::CollectionItem& item) { return item.section == section; });
    REQUIRE(found != room.layout.items.end());
    return *found;
}

rv::app::TimeSummary section(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::CpuSectionRecord& record : report.cpuSections) {
        if (record.name == name) {
            return record.times;
        }
    }
    return rv::app::TimeSummary{};
}

const rv::app::PhaseRecord* phase(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::PhaseRecord& record : report.phases) {
        if (record.name == name) {
            return &record;
        }
    }
    return nullptr;
}

/// The luma's standard deviation in the square of `radius` around `center` (a cover varies, the pending label not).
double lumaSpread(const rv::assets::ImageData& image, glm::ivec2 center, int radius) {
    double sum = 0.0;
    double squares = 0.0;
    int count = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(image.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(image.width - 1, center.x + radius); ++x) {
            const double luma = lumaAt(image, x, y);
            sum += luma;
            squares += luma * luma;
            ++count;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    const double mean = sum / count;
    return std::sqrt(std::max(0.0, squares / count - mean * mean));
}

/// Deletes the worlds' files of the case's data directory (the stored plan and its room's items): the next run makes the
/// plan again and shows no preview of a stored one (I3 step 10c).
void dropStoredPlan(const ImportedRoom& room) {
    std::error_code error;
    fs::remove_all(room.data / "worlds", error);
    INFO(error.message());
    REQUIRE_FALSE(error);
}

/// Deletes every other derived-texture cache entry of the fronts (sorted by name); returns how many it deleted.
std::size_t deleteEveryOtherFront(const fs::path& data) {
    std::vector<fs::path> entries;
    for (const auto& entry : fs::recursive_directory_iterator(data / "cache" / "textures")) {
        const fs::path& path = entry.path();
        if (entry.is_regular_file() && path.extension() == ".rvtex" &&
            path.parent_path().parent_path().filename() == "front") {
            entries.push_back(path);
        }
    }
    std::sort(entries.begin(), entries.end());
    std::size_t deleted = 0;
    for (std::size_t index = 0; index < entries.size(); index += 2) {
        std::error_code error;
        if (fs::remove(entries[index], error)) {
            ++deleted;
        }
    }
    return deleted;
}

/// Whether `file` holds `text` (a log another process writes, shared for reading).
bool fileHolds(const fs::path& file, std::string_view text) {
    std::ifstream stream(file, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return content.find(text) != std::string::npos;
}

/// The checks of every run with a room: shown, clean records, the arrays and hero textures within their budgets.
void requireRoom(const AppRun& run) {
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    CHECK(run.report->world.state == "room");
    REQUIRE(run.report->residency.bound);
    const rv::app::ResidencyRecord& residency = run.report->residency;
    CHECK(residency.maxLayersPerRound <= 64);
    CHECK(residency.hero.peakBytes <= kHeroBudgetBytes);
    CHECK(run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::HeroTextures).peakBytes <= kHeroBudgetBytes + 4);
    CHECK(residency.pausedRounds == 0);
}

} // namespace

TEST_CASE("covers: the snes-room's covers arrive nearest first, the hovered item's hero texture in place of its front",
          "[gpu][world][covers]") {
    ImportedRoom room("hero", "esde/snes-room");
    // A first run from the doorway makes the plan (and fills the cache); the room's layout says where to stand.
    const AppRun first = runApp(retroverseExe(), room.dir, "doorway", lockstep("3"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    requireRoom(first);
    // The doorway sees every item: all of them are the first view's, all resident before the first frame.
    CHECK(first.report->residency.viewItems == 50);
    CHECK(first.report->residency.inViewAmongFirst == 50);
    CHECK(first.report->residency.frontsResident == 50);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& band = firstOf(shown, rv::world::Section::Band);

    // Cold again (the cache and the stored plan deleted: no preview, I3 step 10c), 0.9 m in front of the hero band's
    // first item, the reticle on it.
    std::error_code removed;
    fs::remove_all(room.data / "cache" / "textures", removed);
    REQUIRE_FALSE(removed);
    dropStoredPlan(room);
    const Viewpoint close = viewpointOf(band, 0.9f, shown.scene.camera);
    const fs::path shot = room.dir / "hero.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "hero",
                              lockstep("12", {"--spawn", spawnArgument(close.spawn), "--screenshot", arg(shot)}));
    requireCleanRun(run);
    requireRoom(run);
    const rv::app::ResidencyRecord& residency = run.report->residency;
    INFO(run.describe());
    // Every front and spine resident before the screenshot (the last frame waits for the whole room).
    CHECK(residency.frontsResident == 50);
    CHECK(residency.spinesResident == 50);
    CHECK(residency.pending == 0);
    // Nearest first: the items in view (a few of the band) arrive before the rest, then the rest by distance.
    CHECK(residency.viewItems >= 3);
    CHECK(residency.viewItems < 50);
    // The pool starts every in-view job before any other; while the view's last jobs run, a quick out-of-view one (a
    // generated label) may finish first on another of the pool's threads.
    CHECK(residency.inViewAmongFirst + 4 >= residency.viewItems);
    REQUIRE(residency.viewResidentAfterShownMs.has_value());
    REQUIRE(residency.residentAfterShownMs.has_value());
    CHECK(*residency.viewResidentAfterShownMs <= *residency.residentAfterShownMs);
    const auto& quarters = residency.arrivalDistanceQuarters;
    INFO("arrival distance quarters " << quarters[0] << ", " << quarters[1] << ", " << quarters[2] << ", "
                                      << quarters[3]);
    CHECK(quarters[0] < quarters[1]);
    CHECK(quarters[1] < quarters[3]);
    CHECK(quarters[2] < quarters[3]);
    // The hovered item's hero texture: made, uploaded and shown in place of its front.
    CHECK(run.report->world.hoveredGame == band.game.toString());
    CHECK(residency.hero.loaded >= 1);
    CHECK(residency.hero.failed == 0);
    CHECK(residency.hero.resident >= 1);
    CHECK(residency.hero.shownGame == band.game.toString());
    // At most 1,024 x 752 with its mips (a 680 x 497 cover gives 680 x 499).
    CHECK(residency.hero.bytes >= 16ull * 12ull * 4ull);
    CHECK(residency.hero.bytes <= 1024ull * 752ull * 4ull * 4ull / 3ull + 4096ull);
    const rv::assets::ImageData image = requireScreenshot(run, shot, kShotSize);
    const std::optional<glm::ivec2> centre = projectToPixel(close.camera, kShotSize, close.face);
    REQUIRE(centre.has_value());
    CHECK(lumaSpread(image, *centre, 20) >= 6.0);
}

TEST_CASE("covers: a media root unplugged after a first run shows the cache's covers and generated labels, and the "
          "covers come back after a refresh",
          "[gpu][world][covers]") {
    ImportedRoom room("unplugged", "esde/snes-room");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", lockstep("3"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    requireRoom(first);
    CHECK(first.report->residency.covers == 48);

    // Half the fronts' entries deleted, the media folder renamed: the cache serves the covers it still holds, the
    // others show their generated labels; nothing is read under the media root, no hero texture is asked.
    CHECK(deleteEveryOtherFront(room.data) >= 20);
    room.moveMedia(true);
    const fs::path shot = room.dir / "unplugged.png";
    const AppRun away = runApp(retroverseExe(), room.dir, "away", lockstep("5", {"--screenshot", arg(shot)}));
    requireCleanRun(away);
    requireRoom(away);
    INFO(away.describe());
    CHECK(away.report->world.notice == "media-unknown-kept");
    CHECK(away.report->world.outcome == "kept");
    const rv::app::ResidencyRecord& residency = away.report->residency;
    CHECK(residency.frontsResident == 50);
    CHECK(residency.spinesResident == 50);
    CHECK(residency.covers == 0);
    const std::uint64_t cached = residency.cachedCovers;
    CHECK(cached > 0);
    CHECK(cached < 48);
    CHECK(residency.labels == 50 - cached);
    CHECK(residency.cacheHitsWithoutSource >= cached);
    CHECK(residency.hero.requests == 0);
    requireScreenshot(away, shot, kShotSize);

    // Started with the media root still away; once the room showed, the folder comes back, and "Aggiorna libreria"
    // (frame 300) refreshes the catalog: the loader finds the media root readable and serves again every item whose
    // front did not come from its cover. Only the generated labels change into covers (the cached covers are the same
    // pictures).
    StartedApp retry(room.dir, "retry",
                     lockstep("400", {"--input-script", arg(inputScript("world_cover_retry.json"))}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    bool shownLogged = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fileHolds(room.dir / "retry.log", "the room is shown")) {
            shownLogged = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    room.moveMedia(false);
    REQUIRE(shownLogged);
    const AppRun retried = retry.finish();
    requireCleanRun(retried);
    requireRoom(retried);
    INFO(retried.describe());
    REQUIRE(retried.report->catalog.has_value());
    REQUIRE(retried.report->catalog->refreshes.size() == 1);
    CHECK(retried.report->catalog->refreshes[0].outcome == "done");
    const rv::app::ResidencyRecord& after = retried.report->residency;
    CHECK(after.retryRounds == 1);
    CHECK(after.retriedItems == 50);
    CHECK(after.retriedChanged == 48 - cached);
    CHECK(after.covers == 48);
    CHECK(after.cachedCovers == 0);
    CHECK(after.labels == 2);
    CHECK(after.frontsResident == 50);
    CHECK(after.pending == 0);
}

TEST_CASE("covers: the snes-stress room: nearest first, the budget per frame, the RGBA8 fallback, the media root "
          "unplugged",
          "[gpu][world][covers]") {
    ImportedRoom room("stress", "esde/snes-stress");
    // The plan first (a lockstep run of one frame from the doorway waits for the whole room: covers made cold); the
    // layout says where a side wall's spine-out module stands.
    const AppRun plan = runApp(retroverseExe(), room.dir, "plan", lockstep("1", {"--no-texture-compression"}));
    skipWithoutOpenGl(plan);
    requireCleanRun(plan);
    requireRoom(plan);
    CHECK(plan.report->residency.format == "rgba8-srgb");
    CHECK(plan.report->world.items == 2000);
    const ShownRoom shown = shownRoom(room, *plan.report);
    const rv::world::CollectionItem& spine = firstOf(shown, rv::world::Section::Spine);
    const Viewpoint close = viewpointOf(spine, 1.2f, shown.scene.camera);
    const std::string spawn = spawnArgument(close.spawn);
    // Without a stored plan the next run generates the same one (deterministic) and shows no preview of it (I3 step
    // 10c): the loader's arrivals are the cold run's own.
    dropStoredPlan(room);

    // Cold in BC1, without a lockstep: the frames never wait; the first arrivals are the items in view, the rest come
    // by distance; never more than the budget's 64 layers in a frame.
    const AppRun cold = runApp(retroverseExe(), room.dir, "cold", unpaced("900", {"--spawn", spawn}));
    requireCleanRun(cold);
    requireRoom(cold);
    INFO(cold.describe());
    const rv::app::ResidencyRecord& residency = cold.report->residency;
    CHECK(residency.format == "bc1-srgb");
    // ADR-0011 §9: one array of 2,048 layers per class, within 80 MiB.
    CHECK(residency.arrayBytes <= 80ull * 1024ull * 1024ull);
    CHECK(residency.viewItems >= 20);
    CHECK(residency.lockstepWaits == 0);
    const std::uint64_t arrived = residency.frontsResident;
    INFO("cold: " << arrived << " fronts resident, " << residency.viewItems << " in view, "
                  << residency.inViewAmongFirst << " of the first arrivals in view; quarters "
                  << residency.arrivalDistanceQuarters[0] << ", " << residency.arrivalDistanceQuarters[1] << ", "
                  << residency.arrivalDistanceQuarters[2] << ", " << residency.arrivalDistanceQuarters[3]);
    REQUIRE(arrived >= 40);
    // The pool runs the in-view jobs first; a few out-of-view ones may still finish before the view's last.
    CHECK(residency.inViewAmongFirst + 12 >= std::min(arrived, residency.viewItems));
    // Out of view, by distance (when enough of them arrived to fill the quarters).
    if (arrived >= residency.viewItems + 16) {
        const auto& quarters = residency.arrivalDistanceQuarters;
        CHECK(quarters[0] <= quarters[1] + 0.05);
        CHECK(quarters[1] <= quarters[2] + 0.05);
        CHECK(quarters[2] <= quarters[3] + 0.05);
        CHECK(quarters[0] < quarters[3]);
    }
    const rv::app::TimeSummary uploads = section(*cold.report, "layerUploads");
    INFO("layer uploads: p50 " << uploads.p50Ms << " ms, p99 " << uploads.p99Ms << " ms, max " << uploads.maxMs
                               << " ms; rounds over the budget " << residency.overBudgetRounds << ", longest "
                               << residency.maxRoundMs << " ms");
    CHECK(uploads.p50Ms <= 1.0);
    if (residency.overBudgetRounds > 0) {
        WARN("upload rounds over the 1 ms budget: " << residency.overBudgetRounds << " (longest "
                                                    << residency.maxRoundMs
                                                    << " ms; informative while other work shares the machine)");
    }
    const rv::app::PhaseRecord* coverLoad = phase(*cold.report, "coverLoad");
    REQUIRE(coverLoad != nullptr);
    CHECK(coverLoad->opened == 1);

    // The media root unplugged: the cache serves what the first runs made in BC1 (the in-view items first, the same
    // ones), the others get their generated labels; no cover is read.
    room.moveMedia(true);
    const AppRun away = runApp(retroverseExe(), room.dir, "away", unpaced("1500", {"--spawn", spawn}));
    requireCleanRun(away);
    requireRoom(away);
    INFO(away.describe());
    CHECK(away.report->world.notice == "media-unknown-kept");
    CHECK(away.report->residency.covers == 0);
    CHECK(away.report->residency.cachedCovers > 0);
    CHECK(away.report->residency.cacheMisses >= away.report->residency.labels);
    room.moveMedia(false);
}

// Hidden ([.bench], run by hand in Release: rv_smoke_tests "covers: the figures*"): the covers of the snes-stress room
// (2,000 titles) cold and warm without a lockstep at 1920 x 1080, --no-vsync (the frames never wait): ADR-0011 §9's
// budgets (every layer resident 15 s cold and 1.5 s warm after the room showed; NFR-04: frame p99 at most 33.3 ms over
// the coverLoad phase; uploads within 1 ms a frame) printed; the residency times checked in Release.
TEST_CASE("covers: the figures of the snes-stress room's covers, cold and warm", "[.bench][gpu][world][covers]") {
    ImportedRoom room("stress_bench", "esde/snes-stress");
    const AppRun plan = runApp(retroverseExe(), room.dir, "plan", lockstep("1", {"--no-texture-compression"}));
    skipWithoutOpenGl(plan);
    requireCleanRun(plan);
    const std::vector<std::string> extra{"--no-vsync", "--width", "1920", "--height", "1080"};
    // The cold run makes the plan again (no preview of a stored one); the warm run starts as a restart does (I3 step
    // 10c: the stored room shown, its covers from the cache, then reconciled).
    dropStoredPlan(room);
    for (const std::string_view name : {"cold", "warm"}) {
        const AppRun run = runApp(retroverseExe(), room.dir, name, unpaced(name == "cold" ? "8000" : "3000", extra));
        requireCleanRun(run);
        requireRoom(run);
        INFO(run.describe());
        const rv::app::ResidencyRecord& residency = run.report->residency;
        CHECK(residency.frontsResident == residency.items);
        CHECK(residency.spinesResident == residency.items);
        const rv::app::TimeSummary uploads = section(*run.report, "layerUploads");
        const rv::app::PhaseRecord* load = phase(*run.report, "coverLoad");
        REQUIRE(load != nullptr);
        const rv::app::TimeSummary loadTimes = load->times.value_or(rv::app::TimeSummary{});
        WARN(name << ": every layer resident " << residency.residentAfterShownMs.value_or(-1.0)
                  << " ms after the room showed (the " << residency.viewItems << " in view after "
                  << residency.viewResidentAfterShownMs.value_or(-1.0) << " ms); coverLoad " << load->frames
                  << " frames, p50 " << loadTimes.p50Ms << " ms, p99 " << loadTimes.p99Ms << " ms, max "
                  << loadTimes.maxMs << " ms; uploads p50 " << uploads.p50Ms << " ms, p99 " << uploads.p99Ms
                  << " ms, max " << uploads.maxMs << " ms, " << residency.overBudgetRounds
                  << " rounds over 1 ms, at most " << residency.maxLayersPerRound << " layers; cache "
                  << residency.cacheHits << " hits, " << residency.cacheMisses << " misses; arrays "
                  << residency.arrayBytes << " bytes; GPU memory peak " << run.report->gpuMemory.peakTotalBytes);
#if defined(NDEBUG)
        REQUIRE(residency.residentAfterShownMs.has_value());
        CHECK(*residency.residentAfterShownMs <= (name == "cold" ? 15000.0 : 1500.0));
#endif
        // NFR-04's frame p99 over the phase: informative here (the gate measures it on a quiet machine; frames share
        // the cores with whatever else runs, which the workers' lower priority cannot help).
        if (loadTimes.p99Ms > 33.3) {
            WARN(name << ": frame p99 over the coverLoad phase " << loadTimes.p99Ms << " ms, above 33.3 ms");
        }
    }
}
