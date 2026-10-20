// The media families' rooms in retroverse.exe (I4 step 2, ADR-0013 §4): the building fixture
// (tests/fixtures/esde/building) recorded as the library of the case's data directory and imported with retroverse-cli;
// retroverse.exe --room-system psx then opens its jewel-case room (disc_case_cd: the family's item and fixtures, every
// cover fitted to the square label quad, its cache entry named with the shape) and --room-system amiga its big-box
// room without any media (every front a generated label laid out at the box's shape). The checks: the run report's
// world and residency records, the plan's room and fixtures, the cache's entries, and the band's fronts showing their
// pictures up close.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/assets/derived_texture_cache.hpp>
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
/// The walker's eye above its feet (scene::FirstPersonController's default).
constexpr float kEyeHeight = 1.65f;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The building fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
struct ImportedBuilding {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedBuilding(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/building").value()),
          dir(freshCaseDir(std::string("world_families/") + std::string(name))), data(dir / "data") {
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
};

/// A lockstep run of `system`'s room: `frames` frames at a fixed step, a screenshot at the end.
std::vector<std::string> roomArguments(std::string_view system, std::string_view frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--room-system", std::string(system), "--exit-after-frames", "--frames",
                                       std::string(frames), "--fixed-dt", "0.016667", "--screenshot", arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The room every run must show: from the first frame (lockstep), every item's layers resident.
void requireRoomShown(const AppRun& run, std::string_view room, std::uint64_t items) {
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    REQUIRE(world.bound);
    CHECK(world.state == "room");
    CHECK(world.room == room);
    CHECK(world.notice.empty());
    CHECK(world.hardFindings == 0);
    CHECK(world.items == items);
    CHECK(world.placements == items);
    CHECK(world.skipped == 0);
    CHECK(world.firstFrame == std::optional<std::uint64_t>(0));
    const rv::app::ResidencyRecord& residency = run.report->residency;
    REQUIRE(residency.bound);
    CHECK(residency.items == items);
    CHECK(residency.frontsResident == items);
    CHECK(residency.spinesResident == items);
    CHECK(residency.pending == 0);
    CHECK(residency.failed == 0);
    CHECK(residency.format == "bc1-srgb");
    REQUIRE(run.report->lastView.has_value());
    CHECK(run.report->lastView->staticGroups == 1);
}

struct ShownRoom {
    rv::world::StoredPlan stored;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const ImportedBuilding& building, const rv::app::RunReport& report) {
    auto stored = rv::world::readStoredPlan(
        rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan);
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

/// The middle of an item's visible face, its outward normal, and the walker 0.9 m in front of it looking at it.
struct Viewpoint {
    glm::vec3 face{0.0f};
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Viewpoint viewpointOf(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 face(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 eye(face.x + 0.9f * out.x, kEyeHeight, face.z + 0.9f * out.z);
    const glm::vec3 look = face - eye;
    Viewpoint viewpoint;
    viewpoint.face = face;
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

/// The spread of luma around `center`: a picture varies, the pending flat label does not.
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

/// The band's fronts seen from 0.9 m in front of its first item: how many were probed, how many show a picture.
std::pair<std::size_t, std::size_t> bandFronts(const ImportedBuilding& building, const ShownRoom& shown,
                                               std::string_view system) {
    const auto band =
        std::find_if(shown.layout.items.begin(), shown.layout.items.end(),
                     [](const rv::world::CollectionItem& item) { return item.section == rv::world::Section::Band; });
    REQUIRE(band != shown.layout.items.end());
    const Viewpoint close = viewpointOf(*band, shown.scene.camera);
    const fs::path shelf = building.dir / (std::string(system) + "-band.png");
    const AppRun run = runApp(retroverseExe(), building.dir, std::string(system) + "_band",
                              roomArguments(system, "12", shelf, {"--spawn", spawnArgument(close.spawn)}));
    requireCleanRun(run);
    const rv::assets::ImageData image = requireScreenshot(run, shelf, kShotSize);
    std::size_t probed = 0;
    std::size_t textured = 0;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        if (item.section != rv::world::Section::Band) {
            continue;
        }
        const std::optional<glm::ivec2> pixel = projectToPixel(close.camera, kShotSize, viewpointOf(item, {}).face);
        if (!pixel) {
            continue;
        }
        ++probed;
        const double spread = lumaSpread(image, *pixel, 12);
        INFO("item " << item.game.toString() << " at (" << pixel->x << ", " << pixel->y << "): luma spread " << spread);
        textured += spread >= 6.0 ? 1 : 0;
    }
    return {probed, textured};
}

} // namespace

TEST_CASE("world families: the building's jewel-case room with its covers fitted to their label",
          "[gpu][world][families]") {
    ImportedBuilding building("psx");
    const fs::path doorway = building.dir / "psx-doorway.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "psx_doorway", roomArguments("psx", "40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    // 38 single-disc games and the two playlists, every one with a cover: all face-out (38 display slots, then the
    // band's 2 of 10).
    requireRoomShown(run, "room:psx", 40);
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.outcome == "generated");
    CHECK(world.planWritten);
    CHECK(world.featured == 38);
    CHECK(world.band == 2);
    CHECK(world.spine == 0);
    const rv::app::ResidencyRecord& residency = run.report->residency;
    CHECK(residency.covers == 40);
    CHECK(residency.labels == 0);
    CHECK(residency.coverFailures == 0);
    (void)requireScreenshot(run, doorway, kShotSize);

    // The plan's room: the jewel-case family and its own fixtures.
    const ShownRoom shown = shownRoom(building, *run.report);
    const rv::world::Room& room = shown.stored.plan.rooms.front();
    CHECK(room.mediaFamily == "disc_case_cd");
    CHECK(room.floor == "gen5");
    std::size_t familyFixtures = 0;
    for (const rv::world::Fixture& fixture : shown.stored.plan.fixtures) {
        familyFixtures += fixture.prefab.manifest.find(".cd_jewel.") != std::string::npos ? 1 : 0;
    }
    CHECK(familyFixtures == 5); // the two hero-wall modules, the featured wall shelf and the two display cases

    // Every cover's entry is named with the label's shape (ADR-0013 §4), none with the layer's.
    const fs::path cache = building.data / "cache" / "textures" / "esde";
    const rv::assets::DerivedTextureCache entries(cache);
    std::size_t shaped = 0;
    std::size_t unshaped = 0;
    for (const auto& file : fs::directory_iterator(building.fixture.path("media/psx/covers"))) {
        const std::string relative = "psx/covers/" + rv::core::pathToUtf8(file.path().filename());
        shaped += fs::exists(entries.entryPath(rv::assets::coverCacheKey(rv::assets::TextureClass::Front,
                                                                         rv::assets::TextureFormat::Bc1Srgb, "covers",
                                                                         relative, {120, 120})))
                      ? 1
                      : 0;
        unshaped += fs::exists(entries.entryPath(rv::assets::coverCacheKey(
                        rv::assets::TextureClass::Front, rv::assets::TextureFormat::Bc1Srgb, "covers", relative)))
                        ? 1
                        : 0;
    }
    CHECK(shaped == 40);
    CHECK(unshaped == 0);

    // Up close, the band's jewel cases show their covers.
    const auto [probed, textured] = bandFronts(building, shown, "psx");
    CHECK(probed >= 2);
    CHECK(textured == probed);
}

TEST_CASE("world families: the building's big-box room without media shows generated labels",
          "[gpu][world][families]") {
    ImportedBuilding building("amiga");
    const fs::path doorway = building.dir / "amiga-doorway.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "amiga_doorway", roomArguments("amiga", "40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    // No game has a cover: every game placed (ADR-0010 §3), all 20 on the 28 display slots.
    requireRoomShown(run, "room:amiga", 20);
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.featured == 20);
    CHECK(world.band == 0);
    const rv::app::ResidencyRecord& residency = run.report->residency;
    CHECK(residency.covers == 0);
    CHECK(residency.labels == 20);
    (void)requireScreenshot(run, doorway, kShotSize);
    const ShownRoom shown = shownRoom(building, *run.report);
    CHECK(shown.stored.plan.rooms.front().mediaFamily == "big_box");
    CHECK(shown.stored.plan.rooms.front().floor == "computer");
    // No console in a computer room's hero corner (ADR-0013 §2's defaults).
    for (const rv::world::Fixture& fixture : shown.stored.plan.fixtures) {
        CHECK(fixture.prefab.kind != "console_cartridge");
    }
}
