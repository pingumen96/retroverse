// The media families' rooms in retroverse.exe (I4 step 2, ADR-0013 §4): the building fixture
// (tests/fixtures/esde/building) recorded as the library of the case's data directory, imported and planned with
// retroverse-cli (building_support.hpp);
// retroverse.exe --start-room room:psx then opens its jewel-case room on its floor (disc_case_cd: the family's item and
// fixtures, every cover fitted to the square label quad, its cache entry named with the shape), --start-room room:amiga
// its big-box room without any media (every front a generated label laid out at the box's shape), --start-room the
// mame system's room its arcade room of portrait flyers (generic_case). (I4 step 2 had --room-system; step 5a's
// --start-room replaced it.) The checks: the run report's world and residency records, the plan's room and
// fixtures, the cache's entries, the band's fronts showing their pictures up close, and the card of an inspected item
// (tests/input_scripts/golden_g07_inspect.json from the item's Focus pose): its cover at the family's label shape, the
// hero picture made at that shape, unstretched (ADR-0011 §5), the hero textures within ADR-0011 §8's budget.
// I6 step 4 (ADR-0014 §2): the esde/case-shelves fixture's jewel-case room of 160 titles, most of them spine-out on
// their side (disc_case_cd_v2), seen up close: every spine label upright on its case, a picture resident on it.

#include "building_support.hpp"
#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

#include <rv/app/cover_requests.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The window's size: the UI is laid out for it and drawn into the screenshot scaled uniformly.
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
/// The walker's eye above its feet (scene::FirstPersonController's default).
constexpr float kEyeHeight = 1.65f;
/// ADR-0011 §8: 8 hero textures of at most 1,024 px, about 32 MiB.
constexpr std::uint64_t kHeroBudgetBytes = 32ull * 1024ull * 1024ull;

/// The building fixture of a case under world_families/: each family's own room, amiga's 20 titles too (world.json's
/// sharedRoomMinTitles 0, PlannedBuilding's default; I4 step 4: else a section of a shared room).
struct ImportedBuilding : PlannedBuilding {
    explicit ImportedBuilding(std::string_view name) : PlannedBuilding(std::string("world_families/") + std::string(name)) {}
};

/// The esde/case-shelves fixture of a case under world_families/ (I6 step 4: rooms of cases and boxes, most of them
/// spine-out), each system in a room of its own.
struct ShelvesBuilding : PlannedBuilding {
    explicit ShelvesBuilding(std::string_view name)
        : PlannedBuilding(std::string("world_families/") + std::string(name), true, "esde/case-shelves") {}
};

/// A lockstep run starting in `system`'s room: `frames` frames at a fixed step, a screenshot at the end.
std::vector<std::string> roomArguments(const PlannedBuilding& building, std::string_view system,
                                       std::string_view frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", building.roomOf(system), "--exit-after-frames", "--frames",
                                       std::string(frames), "--fixed-dt", "0.016667", "--screenshot", arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The room every run must show (the floor's only collection room): from the first frame (lockstep), every item's
/// layers resident.
void requireRoomShown(const PlannedBuilding& building, const AppRun& run, std::string_view room, std::uint64_t items) {
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    REQUIRE(world.bound);
    CHECK(world.state == "room");
    CHECK(world.room == room);
    CHECK(world.notice.empty());
    CHECK(world.hardFindings == 0);
    CHECK(world.items == items);
    CHECK(placementsIn(building.plan().plan, room) == items);
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
    // The room's static group, and the hub showcase's when the floor is the ground floor and it holds games.
    CHECK(run.report->lastView->staticGroups == (world.showcase.games.empty() ? 1u : 2u));
}

struct ShownRoom {
    rv::world::StoredPlan stored;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const PlannedBuilding& building, const rv::app::RunReport& report) {
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
                              roomArguments(building, system, "12", shelf, {"--spawn", spawnArgument(close.spawn)}));
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

rv::app::SpawnPoint focusSpawn(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return spawn;
}

/// The fixture's cover of `title` in `system` (its file stem is the title and its region, or the title itself: a
/// playlist without a gamelist entry is titled by its file name).
fs::path coverOf(const ImportedBuilding& building, std::string_view system, const std::string& title) {
    std::vector<fs::path> matches;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(building.fixture.path("media/" + std::string(system) + "/covers"))) {
        const std::string stem = rv::core::pathToUtf8(entry.path().stem());
        if (stem == title || stem.starts_with(title + " (")) {
            matches.push_back(entry.path());
        }
    }
    INFO("the cover of " << title);
    REQUIRE(matches.size() == 1);
    return matches.front();
}

/// The mean absolute difference per channel (0-255) between the screenshot's pixels inside `box` (window pixels:
/// x, y, width, height; its border `inset` of its size left out) and `picture` box-filtered to their footprint; and
/// the screenshot's luma spread there (a flat box is no picture). As test_world_card.cpp's, but for `columns`: the
/// share of the picture's width the box shows, centred (all of it by default; a stretched picture's middle part).
struct PictureMatch {
    double meanDifference = 0.0;
    double lumaSpread = 0.0;
};

PictureMatch comparePicture(const rv::assets::ImageData& shot, const std::vector<double>& box,
                            const rv::assets::ImageData& picture, double inset, double columns = 1.0) {
    const double scale = static_cast<double>(kShotSize.x) / static_cast<double>(kWindowSize.x);
    const double firstColumn = (1.0 - columns) / 2.0 * static_cast<double>(picture.width);
    const double x0 = (box[0] + inset * box[2]) * scale;
    const double y0 = (box[1] + inset * box[3]) * scale;
    const double x1 = (box[0] + (1.0 - inset) * box[2]) * scale;
    const double y1 = (box[1] + (1.0 - inset) * box[3]) * scale;
    const double toPictureX = columns * static_cast<double>(picture.width) / (box[2] * scale);
    const double toPictureY = static_cast<double>(picture.height) / (box[3] * scale);
    double sum = 0.0;
    std::size_t samples = 0;
    double lumaMin = 255.0;
    double lumaMax = 0.0;
    for (int y = static_cast<int>(std::ceil(y0)); y < static_cast<int>(y1); ++y) {
        for (int x = static_cast<int>(std::ceil(x0)); x < static_cast<int>(x1); ++x) {
            const double px0 = firstColumn + (x - box[0] * scale) * toPictureX;
            const double py0 = (y - box[1] * scale) * toPictureY;
            const int ix0 = std::clamp(static_cast<int>(std::floor(px0)), 0, picture.width - 1);
            const int iy0 = std::clamp(static_cast<int>(std::floor(py0)), 0, picture.height - 1);
            const int ix1 = std::clamp(static_cast<int>(std::ceil(px0 + toPictureX)), ix0 + 1, picture.width);
            const int iy1 = std::clamp(static_cast<int>(std::ceil(py0 + toPictureY)), iy0 + 1, picture.height);
            double expected[3] = {0.0, 0.0, 0.0};
            int count = 0;
            for (int v = iy0; v < iy1; ++v) {
                for (int u = ix0; u < ix1; ++u) {
                    const std::size_t at = (static_cast<std::size_t>(v) * static_cast<std::size_t>(picture.width) +
                                            static_cast<std::size_t>(u)) *
                                           static_cast<std::size_t>(picture.channels);
                    for (int c = 0; c < 3; ++c) {
                        expected[c] += picture.pixels[at + static_cast<std::size_t>(c)];
                    }
                    ++count;
                }
            }
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(shot.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(shot.channels);
            double luma = 0.0;
            for (int c = 0; c < 3; ++c) {
                const double actual = shot.pixels[at + static_cast<std::size_t>(c)];
                sum += std::abs(actual - expected[c] / count);
                luma += actual / 3.0;
            }
            lumaMin = std::min(lumaMin, luma);
            lumaMax = std::max(lumaMax, luma);
            samples += 3;
        }
    }
    REQUIRE(samples > 0);
    return PictureMatch{sum / static_cast<double>(samples), lumaMax - lumaMin};
}

/// `item` of `system`'s room (face-out) inspected from its Focus pose (golden_g07_inspect.json: interact at frame 40,
/// the card beside it from then on): the card's cover box has the family's label shape `label` at its frame's height,
/// and shows the hero picture made at that shape, unstretched; the hero textures stay within their budget.
void checkInspectedCard(const ImportedBuilding& building, const ShownRoom& shown, std::string_view system,
                        const rv::world::CollectionItem& item, const rv::assets::LabelAspect& label) {
    const fs::path shot = building.dir / (std::string(system) + "-card.png");
    const AppRun run = runApp(retroverseExe(), building.dir, std::string(system) + "_card",
                              roomArguments(building, system, "60", shot,
                                            {"--spawn", spawnArgument(focusSpawn(item, shown.scene.camera)),
                                             "--input-script", arg(inputScript("golden_g07_inspect.json"))}));
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->world.interaction.state == "inspect");
    const rv::app::CardRecord& card = run.report->world.card;
    REQUIRE(card.bound);
    CHECK(card.game == item.game.toString());
    CHECK(card.coverFrames > 0);
    REQUIRE(card.coverBox.size() == 4);
    // The frame's height (188 dp), at the label's aspect: a square cover square, a portrait one portrait.
    const double aspect = static_cast<double>(label.width) / static_cast<double>(label.height);
    WARN(system << "'s card cover box: " << card.coverBox[2] << " x " << card.coverBox[3] << " px, label aspect "
                << aspect);
    CHECK(card.coverBox[3] > 100.0);
    CHECK(std::abs(card.coverBox[2] / card.coverBox[3] - aspect) <= aspect * 0.02);
    // Its picture: the fixture's cover made into the hero picture at the label's shape, unstretched in the box.
    const rv::assets::ImageData image = requireScreenshot(run, shot, kShotSize);
    const fs::path coverFile = coverOf(building, system, card.title);
    auto bytes = rv::core::readBinaryFile(coverFile);
    REQUIRE(bytes.has_value());
    auto hero =
        rv::app::makeHeroPicture(*bytes, rv::core::pathToUtf8(coverFile.filename()), rv::app::kHeroMaxSide, label);
    REQUIRE(hero.has_value());
    CHECK(std::abs(static_cast<double>(hero->width) / static_cast<double>(hero->height) - aspect) <= aspect * 0.01);
    const PictureMatch match = comparePicture(image, card.coverBox, *hero, 0.08);
    // What I4 step 2's first card showed: the picture stretched across its whole 256:188 frame, the box's share of it
    // in the middle.
    const double frameWidth = card.coverBox[3] * 256.0 / 188.0;
    const PictureMatch stretched = comparePicture(image, card.coverBox, *hero, 0.08, card.coverBox[2] / frameWidth);
    WARN(system << "'s card cover vs hero picture: mean difference " << match.meanDifference << " (stretched "
                << stretched.meanDifference << "), luma spread " << match.lumaSpread);
    CHECK(match.meanDifference < 16.0);
    CHECK(match.meanDifference * 2.0 < stretched.meanDifference);
    CHECK(match.lumaSpread > 40.0);
    // ADR-0011 §8's budget whatever the shape.
    const rv::app::HeroRecord& heroes = run.report->residency.hero;
    CHECK(heroes.loaded >= 1);
    CHECK(heroes.peakBytes <= kHeroBudgetBytes);
    CHECK(heroes.bytes * rv::app::HeroLru::kDefaultCapacity <=
          std::max<std::uint64_t>(1, heroes.resident) * kHeroBudgetBytes);
}

} // namespace

TEST_CASE("world families: the building's jewel-case room with its covers fitted to their label",
          "[gpu][world][families]") {
    ImportedBuilding building("psx");
    const fs::path doorway = building.dir / "psx-doorway.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "psx_doorway", roomArguments(building, "psx", "40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    // 38 single-disc games and the two playlists, every one with a cover: all face-out (38 display slots, then the
    // band's 2 of 10). The building planned by retroverse-cli is the app's: nothing written.
    requireRoomShown(building, run, "room:psx", 40);
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.outcome == "unchanged");
    CHECK_FALSE(world.planWritten);
    CHECK(world.floor == "gen5");
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
    const rv::world::Room& room = roomIn(shown.stored.plan, "room:psx");
    CHECK(room.mediaFamily == "disc_case_cd_v2");
    CHECK(room.floor == "gen5");
    std::size_t familyFixtures = 0;
    for (const rv::world::Fixture& fixture : shown.stored.plan.fixtures) {
        familyFixtures += (fixture.prefab.manifest.find(".cd_jewel.") != std::string::npos ||
                           fixture.prefab.manifest.find(".cd_jewel_side.") != std::string::npos)
                              ? 1
                              : 0;
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

    // Inspected, a band case's square cover is square on the card too.
    const auto band =
        std::find_if(shown.layout.items.begin(), shown.layout.items.end(),
                     [](const rv::world::CollectionItem& item) { return item.section == rv::world::Section::Band; });
    REQUIRE(band != shown.layout.items.end());
    checkInspectedCard(building, shown, "psx", *band, {120, 120});
}

TEST_CASE("world families: jewel cases stand on their side on the shelves, their spines upright",
          "[gpu][world][families]") {
    ShelvesBuilding building("side_spines");
    const fs::path doorway = building.dir / "psx-doorway.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "psx_doorway", roomArguments(building, "psx", "40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    // 160 titles: 38 featured, the band's 10, 112 spine-out in the hero-wall modules.
    requireRoomShown(building, run, "room:psx", 160);
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.featured == 38);
    CHECK(world.band == 10);
    CHECK(world.spine == 112);
    (void)requireScreenshot(run, doorway, kShotSize);

    const ShownRoom shown = shownRoom(building, *run.report);
    const rv::world::Room& room = roomIn(shown.stored.plan, "room:psx");
    CHECK(room.mediaFamily == "disc_case_cd_v2");
    std::size_t sideModules = 0;
    for (const rv::world::Fixture& fixture : shown.stored.plan.fixtures) {
        sideModules += fixture.prefab.manifest.find(".cd_jewel_side.") != std::string::npos ? 1 : 0;
    }
    CHECK(sideModules == 2); // the two hero-wall modules

    // Up close: the spine-out case nearest the eye's height on the first hero-wall module, from 0.6 m.
    const rv::world::CollectionItem* target = nullptr;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        if (item.stance != rv::world::ItemStance::SpineOut) {
            continue;
        }
        const float y = (item.slotFrame * glm::vec4(item.slotBox.center(), 1.0f)).y;
        const float best = target == nullptr
                               ? 1e9f
                               : std::abs((target->slotFrame * glm::vec4(target->slotBox.center(), 1.0f)).y - 1.3f);
        if (std::abs(y - 1.3f) < best - 1e-4f) {
            target = &item;
        }
    }
    REQUIRE(target != nullptr);
    Viewpoint close = viewpointOf(*target, shown.scene.camera);
    {
        // 0.6 m out rather than 0.9 m: the spines' text fills more pixels.
        const glm::vec3 eye = close.camera.position;
        const glm::vec3 toward = glm::normalize(glm::vec3(close.face.x - eye.x, 0.0f, close.face.z - eye.z));
        const glm::vec3 nearer = eye + toward * 0.3f;
        close.spawn.feet = glm::vec3(nearer.x, 0.0f, nearer.z);
        close.camera.position = nearer;
        const glm::vec3 look = close.face - nearer;
        close.spawn.pitchDeg =
            rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
        close.camera.pitchDeg = *close.spawn.pitchDeg;
    }
    const fs::path shelf = building.dir / "psx-spines.png";
    const AppRun near = runApp(retroverseExe(), building.dir, "psx_spines",
                               roomArguments(building, "psx", "12", shelf, {"--spawn", spawnArgument(close.spawn)}));
    requireCleanRun(near);
    CHECK(near.report->residency.spinesResident == 160);
    const rv::assets::ImageData image = requireScreenshot(near, shelf, kShotSize);

    // Each spine-out case in view: its spine label (the Spine socket of the item's prefab, on its -X face) upright on
    // the screen, taller than wide (40 x 256: 6.4 times; lying on its side it would be 6 times wider), and a picture on
    // it (the title's text).
    // The label's edges' middles in the item's space: on its -X face, 7 x 44.8 mm, behind the front pocket. Upright on
    // the screen: its long axis (bottom to top) within 45 degrees of vertical and at least twice its short axis (a camera
    // looking down leans the verticals off the view's centre, so its box would not tell).
    const float spineX = -0.071f;
    const float spineZ = -0.5f * (0.0008f + 0.0005f);
    const std::array<glm::vec3, 4> edges{glm::vec3(spineX, -0.0224f, spineZ), glm::vec3(spineX, 0.0224f, spineZ),
                                         glm::vec3(spineX, 0.0f, spineZ - 0.0035f),
                                         glm::vec3(spineX, 0.0f, spineZ + 0.0035f)};
    std::size_t probed = 0;
    std::size_t upright = 0;
    std::size_t textured = 0;
    float leastRatio = 1e9f;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        if (item.stance != rv::world::ItemStance::SpineOut) {
            continue;
        }
        std::array<glm::vec2, 4> pixels{};
        bool visible = true;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const std::optional<glm::ivec2> pixel =
                projectToPixel(close.camera, kShotSize, glm::vec3(item.transform * glm::vec4(edges[i], 1.0f)));
            if (!pixel) {
                visible = false;
                break;
            }
            pixels[i] = glm::vec2(*pixel);
        }
        if (!visible) {
            continue;
        }
        ++probed;
        const glm::vec2 along = pixels[0] - pixels[1]; // bottom minus top: down the screen
        const glm::vec2 across = pixels[3] - pixels[2];
        const float ratio = glm::length(along) / std::max(1.0f, glm::length(across));
        leastRatio = std::min(leastRatio, ratio);
        upright += along.y > std::abs(along.x) && ratio > 2.0f ? 1 : 0;
        const glm::ivec2 centre((pixels[0] + pixels[1]) * 0.5f);
        const double spread = lumaSpread(image, centre, std::max(1, static_cast<int>(glm::length(across) * 0.3f)));
        textured += spread >= 4.0 ? 1 : 0;
        if (probed <= 3) {
            WARN("spine of " << item.game.toString() << ": " << glm::length(across) << " x " << glm::length(along)
                             << " px at (" << centre.x << ", " << centre.y << "), luma spread " << spread);
        }
    }
    WARN("side spines in view: " << probed << ", upright " << upright << " (the least height / width " << leastRatio
                                  << "), with a picture " << textured);
    CHECK(probed >= 20);
    CHECK(upright == probed);
    CHECK(textured * 10 >= probed * 9);

    // The big boxes of the same building on their side (big_box_v2): 120 titles, 36 face-out, 84 spine-out.
    const fs::path boxes = building.dir / "amiga-doorway.png";
    const AppRun amiga =
        runApp(retroverseExe(), building.dir, "amiga_doorway", roomArguments(building, "amiga", "40", boxes));
    requireCleanRun(amiga);
    requireRoomShown(building, amiga, "room:amiga", 120);
    CHECK(amiga.report->world.featured == 28);
    CHECK(amiga.report->world.band == 8);
    CHECK(amiga.report->world.spine == 84);
    CHECK(roomIn(building.plan().plan, "room:amiga").mediaFamily == "big_box_v2");
    (void)requireScreenshot(amiga, boxes, kShotSize);
}

TEST_CASE("world families: the arcade room's cabinets show their games' flyers on their screens and on the card",
          "[gpu][world][families]") {
    ImportedBuilding building("mame");
    const fs::path doorway = building.dir / "mame-doorway.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "mame_doorway", roomArguments(building, "mame", "40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    // The 15 sets with a cover, each a cabinet's screen (I6 step 6, ADR-0014 §2 and §6), the first six by priority in
    // the display pairs (featured), the rest in the modules' cabinets; the 10 without one unplaced (no-cover). The
    // ground floor holds the arcade room (area:arcade since I4 step 4, its one section mame's) and the hub.
    CHECK(building.roomOf("mame") == "area:arcade");
    requireRoomShown(building, run, "area:arcade", 15);
    CHECK(run.report->world.featured == 6);
    CHECK(run.report->world.band == 0);
    CHECK(run.report->residency.covers == 15);
    CHECK(run.report->residency.coverFailures == 0);
    // The hero cabinet carries the room's video.
    REQUIRE(run.report->video.bound);
    CHECK(run.report->video.screen == "area:arcade/fx:hero.crt");
    (void)requireScreenshot(run, doorway, kShotSize);
    const ShownRoom shown = shownRoom(building, *run.report);
    const rv::world::Room& room = roomIn(shown.stored.plan, building.roomOf("mame"));
    CHECK(room.mediaFamily == "arcade_cabinet");
    CHECK(room.floor == "ground");
    // Every item a screen facing out, in a cabinet; the room dresses none.
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        CHECK(item.stance == rv::world::ItemStance::FaceOut);
    }
    CHECK(std::none_of(shown.stored.plan.dressing.begin(), shown.stored.plan.dressing.end(), [](const auto& dressed) {
        return dressed.level.fixture().room().toString() == "area:arcade";
    }));
    // A screen up close: its game's flyer, lit (an unlit picture tube over the front layer), not the dark glass; the
    // first featured cabinet's (the display pairs', as the first items stood before the cabinets).
    const auto found = std::find_if(shown.layout.items.begin(), shown.layout.items.end(), [](const auto& item) {
        return item.section == rv::world::Section::Featured;
    });
    REQUIRE(found != shown.layout.items.end());
    const rv::world::CollectionItem& screen = *found;
    const fs::path close = building.dir / "mame-screen.png";
    const Viewpoint view = viewpointOf(screen, shown.scene.camera);
    const AppRun closeRun = runApp(retroverseExe(), building.dir, "mame_screen",
                                   roomArguments(building, "mame", "12", close, {"--spawn", spawnArgument(view.spawn)}));
    requireCleanRun(closeRun);
    const rv::assets::ImageData closeImage = requireScreenshot(closeRun, close, kShotSize);
    const std::optional<glm::ivec2> centre = projectToPixel(view.camera, kShotSize, view.face);
    REQUIRE(centre.has_value());
    double sum = 0.0;
    std::size_t samples = 0;
    for (int y = centre->y - 30; y < centre->y + 30; ++y) {
        for (int x = centre->x - 30; x < centre->x + 30; ++x) {
            if (x < 0 || y < 0 || x >= kShotSize.x || y >= kShotSize.y) {
                continue;
            }
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(closeImage.width) +
                                    static_cast<std::size_t>(x)) *
                                   static_cast<std::size_t>(closeImage.channels);
            sum += 0.2126 * closeImage.pixels[at] + 0.7152 * closeImage.pixels[at + 1] +
                   0.0722 * closeImage.pixels[at + 2];
            ++samples;
        }
    }
    REQUIRE(samples > 0);
    WARN("mame's screen up close: mean luma " << sum / static_cast<double>(samples));
    CHECK(sum / static_cast<double>(samples) > 40.0);
    // A flyer inspected: portrait on the card, at the screen's 298 x 398 mm label.
    checkInspectedCard(building, shown, "mame", screen, {298, 398});
}

TEST_CASE("world families: a game launched from its cabinet's screen returns to the arcade room",
          "[gpu][world][families]") {
    // I6 step 6: the cabinet's screen is the game's item: from its Focus pose, interact inspects it and shows its card,
    // Avvia launches it into the fake emulator (RV_LAUNCH_ALLOWLIST), and the room comes back as it was.
    ensureLaunchAllowList();
    ImportedBuilding building("mame_launch");
    const nlohmann::json profile{{"id", "fake-return"},
                                 {"executable", arg(fakeEmulatorExe())},
                                 {"arguments", {"--sleep-ms", "2500", "--exit-timeout-ms", "30000", "--", "{rom}"}},
                                 {"windowTimeoutSeconds", 0},
                                 {"hideConsoleWindow", true},
                                 {"systems", {{"mame", nlohmann::json::object()}}}};
    const nlohmann::json document{{"schemaVersion", 1}, {"profiles", nlohmann::json::array({profile})}};
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "emulators.json", document.dump(2) + "\n")
                .has_value());
    building.cli("assign_mame", {"assign", "mame", "--profile", "fake-return"});
    const fs::path first = building.dir / "first.png";
    const AppRun firstRun = runApp(retroverseExe(), building.dir, "first", roomArguments(building, "mame", "2", first));
    skipWithoutOpenGl(firstRun);
    requireCleanRun(firstRun);
    const ShownRoom shown = shownRoom(building, *firstRun.report);
    REQUIRE_FALSE(shown.layout.items.empty());
    const rv::world::CollectionItem& item = shown.layout.items.front();
    const fs::path shot = building.dir / "returned.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "launch",
                              roomArguments(building, "mame", "120", shot,
                                            {"--spawn", spawnArgument(focusSpawn(item, shown.scene.camera)),
                                             "--input-script", arg(inputScript("world_launch.json"))}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.launches.size() == 1);
    CHECK(report.launches.front().gameId == item.game.toString());
    CHECK(report.launches.front().profile == "fake-return");
    CHECK(report.launches.front().outcome == "returned");
    CHECK(report.world.launch.returned == 1);
    CHECK(report.world.launch.poseRestored);
    CHECK(report.world.interaction.state == "inspect");
    CHECK(report.world.interaction.focusedGame == item.game.toString());
    CHECK(report.world.room == "area:arcade");
}

TEST_CASE("world families: the building's big-box room without media shows generated labels",
          "[gpu][world][families]") {
    ImportedBuilding building("amiga");
    const fs::path doorway = building.dir / "amiga-doorway.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "amiga_doorway", roomArguments(building, "amiga", "40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    // No game has a cover: every game placed (ADR-0010 §3), all 20 on the 28 display slots.
    requireRoomShown(building, run, "room:amiga", 20);
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.featured == 20);
    CHECK(world.band == 0);
    const rv::app::ResidencyRecord& residency = run.report->residency;
    CHECK(residency.covers == 0);
    CHECK(residency.labels == 20);
    (void)requireScreenshot(run, doorway, kShotSize);
    const ShownRoom shown = shownRoom(building, *run.report);
    CHECK(roomIn(shown.stored.plan, "room:amiga").mediaFamily == "big_box_v2");
    CHECK(roomIn(shown.stored.plan, "room:amiga").floor == "computer");
    // No console in a computer room's hero corner (ADR-0013 §2's defaults).
    for (const rv::world::Fixture& fixture : shown.stored.plan.fixtures) {
        if (fixture.id.room().toString() == "room:amiga") {
            CHECK(fixture.prefab.kind != "console_cartridge");
        }
    }
}

TEST_CASE("world families: a stored building of generator 1.3 is migrated once", "[gpu][world][families]") {
    // I6 step 4 (ADR-0014 §2): main's golden building as generator 1.3 wrote it (tests/fixtures/world/legacy/: the
    // building fixture's systems, the cases standing on their end) as the stored plan of a data directory of the
    // building fixture, the world seed its own. The first start updates it: the jewel-case room and the two shared
    // rooms migrated once, in the run report and with the notice, and since I6 step 6 the arcade room too. The second
    // start migrates nothing and shows no notice.
    PlannedBuilding building("world_families/legacy_migration", false);
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "world.json", "{\"seed\": 102938}\n").has_value());
    const rv::world::WorldFiles files =
        rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse("esde").value());
    const auto legacy = rv::core::readTextFile(sourceDir() / "tests" / "fixtures" / "world" / "legacy" /
                                               "building-esde-seed-102938-generator-1.3.json");
    REQUIRE(legacy.has_value());
    REQUIRE(rv::core::writeTextFileAtomic(files.plan, *legacy).has_value());
    std::error_code ignored;
    fs::remove(files.meta, ignored);
    REQUIRE(building.plan().plan.generatorVersion == rv::world::GeneratorVersion{1, 3});

    const auto start = [&](std::string_view name) {
        const fs::path shot = building.dir / (std::string(name) + ".png");
        const std::vector<std::string> arguments{"--start-room",    "room:psx", "--exit-after-frames", "--frames", "30",
                                                 "--fixed-dt",      "0.016667", "--screenshot",        arg(shot)};
        AppRun run = runApp(retroverseExe(), building.dir, std::string(name), arguments);
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        INFO(run.describe());
        REQUIRE(run.report.has_value());
        REQUIRE(run.report->world.bound);
        CHECK(run.report->world.hardFindings == 0);
        CHECK(run.report->world.room == "room:psx");
        return run;
    };
    const AppRun first = start("migrate");
    const rv::app::WorldRecord& migrated = first.report->world;
    CHECK(migrated.outcome == "updated");
    CHECK(migrated.planWritten);
    CHECK(migrated.notice == "rooms-migrated");
    const std::vector<rv::app::MigratedRoomRecord> expected{
        {"area:arcade", "generic_case", "arcade_cabinet", 0}, // I6 step 6: its cases become its cabinets
        {"area:shared:computer", "generic_case", "generic_case_v2", 0},
        {"area:shared:handheld", "generic_case", "generic_case_v2", 0},
        {"room:psx", "disc_case_cd", "disc_case_cd_v2", 0},
    };
    CHECK(migrated.migratedRooms == expected);
    CHECK(migrated.needsConfirmation.empty());
    // I6 step 5 (ADR-0014 §3): the arcade room, standing at I4's lobby, re-placed onto the hub once, in the same
    // update; the report names where it stands in the stored plan (its z following the depth of its cabinets' room, I6
    // step 6's review).
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::RoomPlacement arcadeNow = roomIn(stored.plan, "area:arcade").placement;
    CHECK(arcadeNow.xMm == 9000);
    const std::vector<rv::app::ReplacedRoomRecord> replaced{
        {"area:arcade", 7650, -8150, arcadeNow.xMm, arcadeNow.zMm}};
    CHECK(migrated.replacedRooms == replaced);
    CHECK(stored.plan.generatorVersion == rv::world::kGeneratorVersion);
    CHECK(roomIn(stored.plan, "room:psx").mediaFamily == "disc_case_cd_v2");
    CHECK(roomIn(stored.plan, "area:arcade").mediaFamily == "arcade_cabinet");

    const AppRun second = start("again");
    const rv::app::WorldRecord& again = second.report->world;
    CHECK(again.outcome == "unchanged");
    CHECK_FALSE(again.planWritten);
    CHECK(again.notice.empty());
    CHECK(again.migratedRooms.empty());
    CHECK(again.replacedRooms.empty());
    CHECK(again.needsConfirmation.empty());
}
