// The generated room in retroverse.exe (I3 step 10b): the snes-room fixture (tests/fixtures/esde/snes-room: 50 invented
// titles with generated covers) recorded as the library of the case's data directory and imported with retroverse-cli;
// retroverse.exe without --scene then opens the room: the plan generated on the worker pool and written under
// <data>/worlds/<library>/, the static part instantiated, the 50 cartridges on a static instance group with their
// covers and spines made through the derived-texture cache and uploaded within the budget. Every run but the budget's
// has --fixed-dt, so its frames wait for the room and its layers (lockstep: the first frame shows the room whole). The
// checks: the run report's world and residency records, the plan file, screenshots at the doorway and close to the hero
// wall's band (probes: the fronts show their covers, the outline frames the item the reticle points at), a restart
// that reuses the plan unchanged, a ROM added between two runs that places exactly one new cartridge and moves nothing,
// the RGBA8 arrays of --no-texture-compression, a missing media root that writes no plan and shows the empty room with
// its notice, and the uploads within their budget per frame.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/instance_clusters.hpp>
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
/// The walker's eye above its feet (scene::FirstPersonController's default).
constexpr float kEyeHeight = 1.65f;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// A fixture (the snes-room one unless named) recorded as the library of <caseDir>/data and imported with
/// retroverse-cli (`prepare`, when given, changes the fixture before).
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name, std::string_view fixtureName = "esde/snes-room",
                          const std::function<void(rv::test::Fixture&)>& prepare = {})
        : fixture(rv::test::Fixture::materialize(fixtureName).value()),
          dir(freshCaseDir(std::string("world/") + std::string(name))), data(dir / "data") {
        if (prepare) {
            prepare(fixture);
        }
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    /// retroverse-cli --data-dir <data> `arguments`, which must exit 0.
    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }
};

/// A lockstep run of the room: `frames` frames at a fixed step, a screenshot at the end.
std::vector<std::string> roomArguments(std::string_view frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames", std::string(frames), "--fixed-dt", "0.016667",
                                       "--screenshot",        arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The world's plan file of the run's library.
fs::path planFileOf(const ImportedRoom& room, const rv::app::RunReport& report) {
    return rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan;
}

/// The plan the run wrote and the room's collection, as retroverse.exe made them from it (the assets of its build).
struct ShownRoom {
    rv::world::StoredPlan stored;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const ImportedRoom& room, const rv::app::RunReport& report) {
    auto stored = rv::world::readStoredPlan(planFileOf(room, report));
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    return ShownRoom{std::move(*stored), std::move(*scene), std::move(*layout)};
}

/// The middle of an item's visible face and its outward normal, in the room.
struct Face {
    glm::vec3 center{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

Face faceOf(const rv::world::CollectionItem& item) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    Face face;
    face.center = glm::vec3(item.slotFrame * glm::vec4(local, 1.0f));
    face.normal = glm::normalize(glm::vec3(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
    return face;
}

/// The walker `distance` in front of `face` on the floor, looking at its middle (--spawn), and the camera it gives.
struct Viewpoint {
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Viewpoint viewpointOf(const Face& face, float distance, const rv::scene::CameraSetup& lens) {
    glm::vec3 out(face.normal.x, 0.0f, face.normal.z);
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 eye(face.center.x + distance * out.x, kEyeHeight, face.center.z + distance * out.z);
    const glm::vec3 look = face.center - eye;
    const float yaw = std::atan2(look.x, -look.z);
    const float pitch = std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z));
    Viewpoint viewpoint;
    viewpoint.spawn.feet = glm::vec3(eye.x, 0.0f, eye.z);
    viewpoint.spawn.yawDeg = rv::core::radiansToDegrees(yaw);
    viewpoint.spawn.pitchDeg = rv::core::radiansToDegrees(pitch);
    viewpoint.camera = lens;
    viewpoint.camera.position = eye;
    viewpoint.camera.yawDeg = *viewpoint.spawn.yawDeg;
    viewpoint.camera.pitchDeg = *viewpoint.spawn.pitchDeg;
    return viewpoint;
}

/// The spread of luma (its standard deviation) in the square of `radius` around `center`: a cover's picture and its
/// title vary, the pending picture (a flat label) does not.
double lumaSpread(const rv::assets::ImageData& image, glm::ivec2 center, int radius) {
    double sum = 0.0;
    double squares = 0.0;
    int count = 0;
    for (int y = center.y - radius; y <= center.y + radius; ++y) {
        for (int x = center.x - radius; x <= center.x + radius; ++x) {
            if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
                continue;
            }
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

/// The outline's pixels (scene::kOutlineColorSrgb exactly) in the square of `radius` around `center`.
std::size_t outlinePixels(const rv::assets::ImageData& image, glm::ivec2 center, int radius) {
    const glm::ivec3 color(0xF4, 0xEF, 0xE6);
    std::size_t count = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(image.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(image.width - 1, center.x + radius); ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(image.channels);
            if (image.pixels[at] == color.r && image.pixels[at + 1] == color.g && image.pixels[at + 2] == color.b) {
                ++count;
            }
        }
    }
    return count;
}

/// What every run of the room must report: the room shown from the first frame (lockstep), every item's layers
/// resident within the cold cache's budget, the static group and the pick index whole.
void requireRoomShown(const AppRun& run, std::uint64_t items) {
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    REQUIRE(world.bound);
    CHECK(world.state == "room");
    CHECK(world.room == "room:snes");
    CHECK(world.notice.empty());
    CHECK(world.hardFindings == 0);
    CHECK(world.items == items);
    CHECK(world.placements == items);
    CHECK(world.skipped == 0);
    CHECK(world.staticRecords == 3 * items);
    CHECK(world.pickTargets == items);
    CHECK(world.firstFrame == std::optional<std::uint64_t>(0));
    REQUIRE(world.committedAtMs.has_value());
    REQUIRE(world.snapshotToCommitMs.has_value());
    const rv::app::ResidencyRecord& residency = run.report->residency;
    REQUIRE(residency.bound);
    CHECK(residency.items == items);
    CHECK(residency.frontsResident == items);
    CHECK(residency.spinesResident == items);
    CHECK(residency.pending == 0);
    CHECK(residency.failed == 0);
    REQUIRE(residency.residentAfterShownMs.has_value());
    // ADR-0011 §9: the cold cache's budget for the reference room (890 items) is 10 s; 50 come well within it.
    CHECK(*residency.residentAfterShownMs <= 10000.0);
    CHECK(residency.arrayBytes > 0);
    CHECK(residency.arrayBytes <= run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).peakBytes);
    // One static group, its batches the family's parts in one chunk of arrays: the shells, the fronts, the spines.
    REQUIRE(run.report->lastView.has_value());
    CHECK(run.report->lastView->staticGroups == 1);
    CHECK(run.report->lastView->staticBatches == 3);
}

} // namespace

TEST_CASE("world: the snes-room fixture opens its generated room, its covers resident, a restart reusing the plan",
          "[gpu][world]") {
    ImportedRoom room("room");
    const fs::path doorway = room.dir / "doorway.png";
    const AppRun first = runApp(retroverseExe(), room.dir, "doorway", roomArguments("40", doorway));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    requireRoomShown(first, 50);
    const rv::app::WorldRecord& world = first.report->world;
    // A first plan, written: G02's room, every title face-out (ADR-0010 §3: 44 display slots, then the band).
    CHECK(world.outcome == "generated");
    CHECK(world.changeOutcome == "generated");
    CHECK(world.planWritten);
    CHECK(world.saveError.empty());
    CHECK(world.featured == 44);
    CHECK(world.band == 6);
    CHECK(world.spine == 0);
    CHECK(world.unplaced == 5);
    CHECK(world.duplicateTitle == 2);
    CHECK(world.noCover == 3);
    CHECK(world.added == 50);
    CHECK(world.lights >= 1);
    // Its covers: 48 decode, the truncated one and the one that is no image show their generated labels.
    const rv::app::ResidencyRecord& residency = first.report->residency;
    CHECK(residency.covers == 48);
    CHECK(residency.labels == 2);
    CHECK(residency.coverFailures == 2);
    CHECK(residency.format == "bc1-srgb");
    CHECK(residency.cacheStored >= 100);

    const ShownRoom shown = shownRoom(room, *first.report);
    CHECK(shown.stored.digest == world.planDigest);
    CHECK(shown.layout.items.size() == 50);
    const rv::assets::ImageData doorwayImage = requireScreenshot(first, doorway, kShotSize);

    // Close to the hero wall's band: the walker 0.9 m in front of its first item, the reticle on it.
    const auto band =
        std::find_if(shown.layout.items.begin(), shown.layout.items.end(),
                     [](const rv::world::CollectionItem& item) { return item.section == rv::world::Section::Band; });
    REQUIRE(band != shown.layout.items.end());
    const Viewpoint close = viewpointOf(faceOf(*band), 0.9f, shown.scene.camera);
    const fs::path shelf = room.dir / "shelf.png";
    const AppRun closeRun =
        runApp(retroverseExe(), room.dir, "shelf", roomArguments("12", shelf, {"--spawn", spawnArgument(close.spawn)}));
    requireCleanRun(closeRun);
    requireRoomShown(closeRun, 50);
    // The restart reused the plan as it was: nothing written.
    CHECK(closeRun.report->world.outcome == "unchanged");
    CHECK_FALSE(closeRun.report->world.planWritten);
    CHECK(closeRun.report->world.planDigest == world.planDigest);
    // The warm cache served every layer (the two covers that are no usable image are looked for again, and fail).
    CHECK(closeRun.report->residency.cacheMisses == closeRun.report->residency.coverFailures);
    CHECK(closeRun.report->residency.cacheStored == 0);
    // The hover: the reticle's item, outlined.
    CHECK(closeRun.report->world.hoveredGame == band->game.toString());
    CHECK(closeRun.report->world.hoverChanges >= 1);
    const rv::assets::ImageData shelfImage = requireScreenshot(closeRun, shelf, kShotSize);
    std::size_t textured = 0;
    std::size_t probed = 0;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        if (item.section != rv::world::Section::Band) {
            continue;
        }
        const std::optional<glm::ivec2> pixel = projectToPixel(close.camera, kShotSize, faceOf(item).center);
        if (!pixel) {
            continue;
        }
        ++probed;
        const double spread = lumaSpread(shelfImage, *pixel, 12);
        INFO("item " << item.game.toString() << " at (" << pixel->x << ", " << pixel->y << "): luma spread " << spread);
        textured += spread >= 6.0 ? 1 : 0;
    }
    CHECK(probed >= 3);
    CHECK(textured == probed);
    const std::optional<glm::ivec2> centre = projectToPixel(close.camera, kShotSize, faceOf(*band).center);
    REQUIRE(centre.has_value());
    CHECK(outlinePixels(shelfImage, *centre, 260) >= 200);
    // From the doorway nothing is within reach of the reticle: no outline.
    CHECK(world.hoveredGame.empty());
    CHECK(outlinePixels(doorwayImage, kShotSize / 2, 960) == 0);
}

TEST_CASE("world: a ROM added between two runs places exactly one new cartridge and moves nothing", "[gpu][world]") {
    ImportedRoom room("added");
    const AppRun before = runApp(retroverseExe(), room.dir, "before", roomArguments("5", room.dir / "before.png"));
    skipWithoutOpenGl(before);
    requireCleanRun(before);
    requireRoomShown(before, 50);
    const ShownRoom first = shownRoom(room, *before.report);

    // One of G10's additions, its ROM and its cover, without a gamelist entry (its title is its file name's).
    for (const std::string_view file :
         {"roms/snes/Zinnia Zephyr (USA).sfc", "media/snes/covers/Zinnia Zephyr (USA).png"}) {
        std::error_code error;
        fs::copy_file(room.fixture.path(std::string("additions/") + std::string(file)), room.fixture.path(file), error);
        INFO(file << ": " << error.message());
        REQUIRE_FALSE(error);
    }
    room.cli("import_again", {"import"});

    const AppRun after = runApp(retroverseExe(), room.dir, "after", roomArguments("5", room.dir / "after.png"));
    requireCleanRun(after);
    requireRoomShown(after, 51);
    const rv::app::WorldRecord& world = after.report->world;
    CHECK(world.outcome == "updated");
    CHECK(world.changeOutcome == "updated");
    CHECK(world.planWritten);
    CHECK(world.added == 1);
    CHECK(world.kept == 50);
    // The new title stands spine-out (ADR-0010 §3: the face-out set changes only with a regeneration).
    CHECK(world.spine == 1);
    CHECK(world.featured == 44);
    CHECK(world.band == 6);
    const ShownRoom second = shownRoom(room, *after.report);
    CHECK(second.stored.plan.placements.size() == 51);
    std::size_t kept = 0;
    for (const rv::world::Placement& placement : first.stored.plan.placements) {
        const bool same = std::find(second.stored.plan.placements.begin(), second.stored.plan.placements.end(),
                                    placement) != second.stored.plan.placements.end();
        kept += same ? 1 : 0;
    }
    CHECK(kept == 50);
    // The items that stood stand where they stood: the same transforms, the same pick ids.
    std::size_t unmoved = 0;
    for (const rv::world::CollectionItem& item : first.layout.items) {
        const rv::world::CollectionItem* again = second.layout.findGame(item.game);
        unmoved += again != nullptr && again->pickId == item.pickId && again->transform == item.transform ? 1 : 0;
    }
    CHECK(unmoved == 50);
    // The new item's cover and spine are made; the rest comes from the cache.
    CHECK(after.report->residency.cacheMisses == 2 + after.report->residency.coverFailures);
    CHECK(after.report->residency.cacheStored == 2);
}

TEST_CASE("world: --no-texture-compression keeps the room's arrays in RGBA8", "[gpu][world]") {
    ImportedRoom room("rgba8");
    const fs::path shot = room.dir / "rgba8.png";
    const AppRun run =
        runApp(retroverseExe(), room.dir, "rgba8", roomArguments("5", shot, {"--no-texture-compression"}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    requireRoomShown(run, 50);
    CHECK(run.report->residency.format == "rgba8-srgb");
    requireScreenshot(run, shot, kShotSize);
}

TEST_CASE("world: a media root out of reach writes no plan and shows the empty room with its notice", "[gpu][world]") {
    ImportedRoom room("media_unknown");
    std::error_code error;
    fs::rename(room.fixture.path("media"), room.fixture.path("media-away"), error);
    REQUIRE_FALSE(error);
    const fs::path shot = room.dir / "shell.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "shell", roomArguments("5", shot));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    const rv::app::WorldRecord& world = run.report->world;
    REQUIRE(world.bound);
    CHECK(world.state == "shell");
    CHECK(world.notice == "media-unknown");
    CHECK(world.outcome == "no-plan");
    CHECK_FALSE(world.planWritten);
    CHECK(world.items == 0);
    CHECK_FALSE(run.report->residency.bound);
    // No plan, no world folder's plan file.
    CHECK_FALSE(
        fs::exists(rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(world.library).value()).plan));
    requireScreenshot(run, shot, kShotSize);
}

TEST_CASE("world: without a lockstep the layers go up within their budget per frame", "[gpu][world]") {
    ImportedRoom room("budget");
    // A first run makes the plan and fills the cache; the second, without --fixed-dt, never waits for the room: its
    // frames go on while the plan is read and the layers come from the cache and go up, a budget's worth per frame.
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments("2", room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const AppRun run = runApp(retroverseExe(), room.dir, "budget",
                              {"--start-room", "room:snes", "--exit-after-frames", "--frames", "900"});
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    REQUIRE(world.bound);
    CHECK(world.state == "room");
    const rv::app::ResidencyRecord& residency = run.report->residency;
    REQUIRE(residency.bound);
    CHECK(residency.frontsResident == 50);
    CHECK(residency.spinesResident == 50);
    CHECK(residency.maxLayersPerRound <= 64);
    const auto uploads =
        std::find_if(run.report->cpuSections.begin(), run.report->cpuSections.end(),
                     [](const rv::app::CpuSectionRecord& section) { return section.name == "layerUploads"; });
    REQUIRE(uploads != run.report->cpuSections.end());
    INFO("layer uploads: p50 " << uploads->times.p50Ms << " ms, p99 " << uploads->times.p99Ms << " ms, max "
                               << uploads->times.maxMs << " ms");
    // The budget's millisecond, with the meter's one layer of slack (the first layer of a frame always goes): in Debug a
    // warm cache fills every frame's millisecond since I3 step 12 (its median at the budget, as step 6's probe), so
    // step 6's 5% of slack.
    CHECK(uploads->times.p50Ms <= 1.05);
}

TEST_CASE("world: the quick catalog's lists are answered within 100 ms while the room is made", "[gpu][world]") {
    ImportedRoom room("catalog_lists");
    // Without --fixed-dt: the frames never wait, the catalog opens and searches while the room is made.
    const AppRun run = runApp(
        retroverseExe(), room.dir, "lists",
        {"--start-room", "room:snes", "--input-script", arg(inputScript("world_catalog_search.json")),
         "--exit-after-frames", "--frames", "600"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    REQUIRE(run.report->catalog.has_value());
    const rv::app::CatalogRecord& catalog = *run.report->catalog;
    CHECK(catalog.search == "lanterno");
    REQUIRE(catalog.lists.has_value());
    INFO("lists: " << catalog.lists->count << ", p50 " << catalog.lists->p50Ms << " ms, max " << catalog.lists->maxMs
                   << " ms");
    CHECK(catalog.lists->count >= 2);
    // Row 10b (ADR-0010 §12): the catalog worker runs only the room's snapshot, never its generation.
    CHECK(catalog.lists->maxMs <= 100.0);
}

// Hidden ([.bench], run by hand in Release: rv_smoke_tests "[world][.bench]"): the room of 2,000 titles
// (tests/fixtures/esde/snes-stress), cold and warm; the figures of the roadmap's row 10b and of ADR-0011 §9 printed.
TEST_CASE("world: the figures of the snes-stress room (2,000 titles)", "[.bench][gpu][world]") {
    ImportedRoom room("stress", "esde/snes-stress");
    for (const std::string_view name : {"cold", "warm"}) {
        const AppRun run = runApp(retroverseExe(), room.dir, name,
                                  {"--start-room", "room:snes", "--exit-after-frames", "--frames", "300",
                                   "--fixed-dt", "0.016667", "--no-vsync", "--width", "1920", "--height", "1080"});
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        INFO(run.describe());
        const rv::app::WorldRecord& world = run.report->world;
        const rv::app::ResidencyRecord& residency = run.report->residency;
        REQUIRE(world.state == "room");
        CHECK(residency.frontsResident == residency.items);
        // ADR-0011 §9: one array of 2,048 layers per class, within 80 MiB; the fronts and spines one batch each.
        CHECK(residency.arrayBytes <= 80ull * 1024ull * 1024ull);
        REQUIRE(run.report->lastView.has_value());
        CHECK(run.report->lastView->staticBatches == 3);
        const auto section = [&](std::string_view sectionName) {
            for (const rv::app::CpuSectionRecord& record : run.report->cpuSections) {
                if (record.name == sectionName) {
                    return record.times;
                }
            }
            return rv::app::TimeSummary{};
        };
        const rv::app::TimeSummary planning = section("collectionPlanning");
        const rv::app::TimeSummary uploads = section("layerUploads");
        const rv::app::TimeSummary frames = run.report->frameTimes.times.value_or(rv::app::TimeSummary{});
        WARN(name << ": " << world.placements << " placements (" << world.featured << " featured, " << world.band
                  << " band, " << world.spine << " spine), " << world.items << " items in " << world.clusters
                  << " clusters; snapshot " << world.snapshotMs.value_or(-1.0) << " ms, room job "
                  << world.buildMs.value_or(-1.0) << " ms, commit " << world.commitMs.value_or(-1.0)
                  << " ms, snapshot to commit " << world.snapshotToCommitMs.value_or(-1.0) << " ms; layers resident "
                  << residency.residentAfterShownMs.value_or(-1.0) << " ms after the room showed (cache "
                  << residency.cacheHits << " hits, " << residency.cacheMisses << " misses); arrays "
                  << residency.arrayBytes << " bytes; planning p50 " << planning.p50Ms << " ms p99 " << planning.p99Ms
                  << " ms; uploads p50 " << uploads.p50Ms << " ms max " << uploads.maxMs << " ms; frames p50 "
                  << frames.p50Ms << " ms p99 " << frames.p99Ms << " ms");
#if defined(NDEBUG)
        // Row 10b: the room committed within 1 s of the snapshot; planning within 0.3 ms per frame (Release).
        CHECK(world.snapshotToCommitMs.value_or(1e9) <= 1000.0);
        CHECK(planning.p50Ms <= 0.3);
#endif
    }
}

TEST_CASE("world: without a catalog, or without a system that gives a room, the test room shows with its notice",
          "[gpu][world]") {
    // No library.json: the catalog cannot open, the test room shows from the start.
    const fs::path dir = freshCaseDir("world/no_catalog");
    const fs::path shot = dir / "no_catalog.png";
    const AppRun run = runApp(retroverseExe(), dir, "no_catalog", roomArguments("5", shot));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    REQUIRE(world.bound);
    CHECK(world.state == "test-room");
    CHECK(world.notice == "no-catalog");
    CHECK(world.catalogState == "no-library");
    CHECK_FALSE(run.report->residency.bound);
    CHECK_FALSE(fs::exists(dir / "data" / "worlds"));
    requireScreenshot(run, shot, kShotSize);

    // A catalog whose games are of no system with a room (the building fixture's kodi alone: excluded, ADR-0013 §2;
    // the other systems' ROM folders and gamelists removed, since a gamelist's entries stay as source unavailable):
    // the empty shell while the catalog opens, then the test room.
    ImportedRoom other("no_games", "esde/building", [](rv::test::Fixture& fixture) {
        for (const std::string_view system : {"amiga", "gb", "mame", "pocketlab", "pokemini", "psx", "snes"}) {
            fs::remove_all(fixture.path("roms/" + std::string(system)));
            fs::remove_all(fixture.path("ES-DE/gamelists/" + std::string(system)));
        }
    });
    const fs::path otherShot = other.dir / "no_games.png";
    const AppRun otherRun = runApp(retroverseExe(), other.dir, "no_games", roomArguments("5", otherShot));
    requireCleanRun(otherRun);
    INFO(otherRun.describe());
    const rv::app::WorldRecord& otherWorld = otherRun.report->world;
    REQUIRE(otherWorld.bound);
    CHECK(otherWorld.state == "test-room");
    CHECK(otherWorld.notice == "no-games");
    CHECK_FALSE(otherRun.report->residency.bound);
    CHECK_FALSE(fs::exists(other.data / "worlds"));
    requireScreenshot(otherRun, otherShot, kShotSize);

    // The same catalog in the default start (the lobby, no --start-room): the test room with the building's notice.
    const fs::path lobbyShot = other.dir / "no_games_lobby.png";
    const AppRun lobbyRun = runApp(retroverseExe(), other.dir, "no_games_lobby",
                                   {"--exit-after-frames", "--frames", "5", "--fixed-dt", "0.016667", "--screenshot",
                                    arg(lobbyShot)});
    requireCleanRun(lobbyRun);
    INFO(lobbyRun.describe());
    CHECK(lobbyRun.report->world.state == "test-room");
    CHECK(lobbyRun.report->world.notice == "no-games");
    CHECK_FALSE(fs::exists(other.data / "worlds"));
    requireScreenshot(lobbyRun, lobbyShot, kShotSize);
}
