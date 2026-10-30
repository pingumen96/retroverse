// The media viewer and Informazioni (I3 step 14b; ADR-0012 §5) from the world's card and from the quick catalog: the
// snes-room fixture (tests/fixtures/esde/snes-room: invented titles, generated pictures) imported into the case's data
// directory, retroverse.exe without --scene, lockstep (--fixed-dt: the frames wait for the viewer's pictures too),
// hidden, driven by tests/input_scripts/media_viewer.json from the Focus pose of Amber Lantern (USA)'s item (every image
// kind) and by media_viewer_corrupt.json (Bramble Burrow Rally's truncated screenshot). The checks read the run
// report's screens record (what opened from where, the pages by frame, the pictures shown or failed) and the
// screenshots: the viewer pages through the game's images in its order and skips the kinds it lacks, a picture shows
// the fixture's file (compared pixel by pixel), a corrupt one its fallback; Informazioni lists the other version of a
// title with its region; the quick catalog's Vedi media and Informazioni open the same screens; RmlUi never refused a
// file (it reads under the asset root only, the pictures are textures). Helpers repeat test_world_card.cpp's (see
// docs/i3/step-14b.md, "For the merge").

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The window's size: the UI is laid out for it and drawn into the screenshot scaled uniformly.
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};

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
          dir(freshCaseDir(std::string("media_viewer/") + std::string(name))), data(dir / "data") {
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

std::vector<std::string> roomArguments(std::uint32_t frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{
        "--start-room", "room:snes", "--exit-after-frames", "--frames", std::to_string(frames), "--fixed-dt", "0.016667", "--screenshot", arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The GameId of a snes file of the fixture in the run's library.
rv::catalog::GameId gameOf(const rv::app::RunReport& report, std::string_view file) {
    const auto library = rv::catalog::LibraryId::parse(report.world.library);
    REQUIRE(library.has_value());
    const auto key = rv::catalog::makeLookupKey(file);
    REQUIRE(key.has_value());
    const auto id = rv::catalog::stableGameId(*library, rv::catalog::SystemName::parse("snes").value(), key->key());
    REQUIRE(id.has_value());
    return *id;
}

/// The Focus pose of `game`'s item in the room the run wrote.
rv::app::SpawnPoint focusSpawnOf(const ImportedRoom& room, const rv::app::RunReport& report,
                                 const rv::catalog::GameId& game) {
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
    const rv::world::CollectionItem* item = layout->findGame(game);
    INFO("the item of " << game.toString());
    REQUIRE(item != nullptr);
    rv::app::FocusViewer viewer;
    viewer.lens = scene->camera.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(*item, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return spawn;
}

const rv::app::GameScreensRecord& requireScreens(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->screens.bound);
    CHECK(run.report->screens.eventsDropped == 0);
    // RmlUi read nothing outside the asset root (the pictures are the application's textures).
    auto log = rv::core::readTextFile(run.outputFile);
    REQUIRE(log.has_value());
    CHECK(log->find("UI file '") == std::string::npos);
    return run.report->screens;
}

/// The events of `kind`, as "<frame>:<from>:<kind>:<detail>" (empty parts left empty).
std::vector<std::string> eventsOf(const rv::app::GameScreensRecord& screens, std::string_view kind) {
    std::vector<std::string> found;
    for (const rv::app::GameScreenEventRecord& event : screens.events) {
        if (event.event == kind) {
            found.push_back(std::to_string(event.frame) + ":" + event.from + ":" + event.kind + ":" + event.detail);
        }
    }
    return found;
}

const rv::app::GameScreenEventRecord* firstEvent(const rv::app::GameScreensRecord& screens, std::string_view kind,
                                                 std::uint64_t frame) {
    for (const rv::app::GameScreenEventRecord& event : screens.events) {
        if (event.event == kind && event.frame == frame) {
            return &event;
        }
    }
    return nullptr;
}

/// The mean absolute difference per channel (0-255) between the screenshot's pixels inside `box` (window pixels: x, y,
/// width, height; its border `inset` of its size left out) and `picture` box-filtered to their footprint; and the
/// screenshot's luma spread there (a flat box is no picture).
struct PictureMatch {
    double meanDifference = 0.0;
    double lumaSpread = 0.0;
};

PictureMatch comparePicture(const rv::assets::ImageData& shot, const std::vector<double>& box,
                            const rv::assets::ImageData& picture, double inset) {
    const double scale = static_cast<double>(kShotSize.x) / static_cast<double>(kWindowSize.x);
    const double x0 = (box[0] + inset * box[2]) * scale;
    const double y0 = (box[1] + inset * box[3]) * scale;
    const double x1 = (box[0] + (1.0 - inset) * box[2]) * scale;
    const double y1 = (box[1] + (1.0 - inset) * box[3]) * scale;
    const double toPictureX = static_cast<double>(picture.width) / (box[2] * scale);
    const double toPictureY = static_cast<double>(picture.height) / (box[3] * scale);
    double sum = 0.0;
    std::size_t samples = 0;
    double lumaMin = 255.0;
    double lumaMax = 0.0;
    for (int y = static_cast<int>(std::ceil(y0)); y < static_cast<int>(y1); ++y) {
        for (int x = static_cast<int>(std::ceil(x0)); x < static_cast<int>(x1); ++x) {
            const double px0 = (x - box[0] * scale) * toPictureX;
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

} // namespace

TEST_CASE("media viewer: from the card and the quick catalog, the pages in order with the missing kinds skipped, "
          "Informazioni with the other versions, Back closes",
          "[gpu][world][media_viewer]") {
    ImportedRoom room("screens");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const rv::catalog::GameId amber = gameOf(*first.report, "Amber Lantern (USA).sfc");
    const std::string cobaltUsa = gameOf(*first.report, "Cobalt Compass (USA).sfc").toString();
    const std::string cobaltEurope = gameOf(*first.report, "Cobalt Compass (Europe).sfc").toString();
    const std::vector<std::string> script{"--spawn", spawnArgument(focusSpawnOf(room, *first.report, amber)),
                                          "--input-script", arg(inputScript("media_viewer.json"))};

    // Frame 87: the viewer from the card on the 3D box (4 of 8), the fixture's picture in its stage.
    const fs::path boxShot = room.dir / "box.png";
    const AppRun box = runApp(retroverseExe(), room.dir, "box", roomArguments(88, boxShot, script));
    const rv::app::GameScreensRecord& boxScreens = requireScreens(box);
    INFO(box.describe());
    CHECK(box.report->world.interaction.state == "inspect");
    REQUIRE(boxScreens.pictureBox.size() == 4);
    REQUIRE(boxScreens.stageBox.size() == 4);
    // Fitted without stretching (560 x 700), as high as the stage.
    CHECK(std::abs(boxScreens.pictureBox[2] / boxScreens.pictureBox[3] - 560.0 / 700.0) < 0.02);
    CHECK(std::abs(boxScreens.pictureBox[3] - boxScreens.stageBox[3]) < 2.0);
    const rv::assets::ImageData boxImage = requireScreenshot(box, boxShot, kShotSize);
    auto boxPicture = rv::assets::loadImage(room.fixture.path("media/snes/3dboxes/Amber Lantern (USA).png"), 4);
    REQUIRE(boxPicture.has_value());
    const PictureMatch match = comparePicture(boxImage, boxScreens.pictureBox, *boxPicture, 0.04);
    WARN("viewer picture vs the fixture's 3D box: mean difference " << match.meanDifference << ", luma spread "
                                                                    << match.lumaSpread);
    CHECK(match.meanDifference < 12.0);
    CHECK(match.lumaSpread > 40.0);

    // Frame 129: Informazioni of the same game from the card (screenshot reviewed by hand).
    const AppRun info = runApp(retroverseExe(), room.dir, "info", roomArguments(130, room.dir / "info.png", script));
    const rv::app::GameScreensRecord& infoScreens = requireScreens(info);
    CHECK(infoScreens.infoFrames > 0);
    static_cast<void>(requireScreenshot(info, room.dir / "info.png", kShotSize));

    // Frame 183: Informazioni from the quick catalog, on a Cobalt Compass (screenshot reviewed by hand).
    const AppRun catalogInfo =
        runApp(retroverseExe(), room.dir, "catalog_info", roomArguments(184, room.dir / "catalog_info.png", script));
    static_cast<void>(requireScreens(catalogInfo));
    static_cast<void>(requireScreenshot(catalogInfo, room.dir / "catalog_info.png", kShotSize));

    // The whole script.
    const AppRun run = runApp(retroverseExe(), room.dir, "all", roomArguments(200, room.dir / "end.png", script));
    const rv::app::GameScreensRecord& screens = requireScreens(run);
    INFO(run.describe());
    // I5 step 8: Amber Lantern's clip has its page after the title screen (it has no manual).
    const std::string allKinds =
        "covers,backcovers,physicalmedia,3dboxes,screenshots,titlescreens,videos,marquees,fanart";
    // The viewer from the card at 70 (every kind), from the quick catalog at 172 on a Cobalt Compass (the USA release:
    // a cover and its cartridge; the European one: its cover alone; the kinds they lack skipped).
    const std::vector<std::string> media = eventsOf(screens, "media");
    REQUIRE(media.size() == 2);
    CHECK(media[0] == "70:card::" + allKinds);
    const rv::app::GameScreenEventRecord* fromCatalog = firstEvent(screens, "media", 172);
    REQUIRE(fromCatalog != nullptr);
    CHECK(fromCatalog->from == "catalog");
    CHECK((fromCatalog->game == cobaltUsa || fromCatalog->game == cobaltEurope));
    CHECK(fromCatalog->detail == (fromCatalog->game == cobaltUsa ? "covers,physicalmedia,videos" : "covers"));
    REQUIRE(firstEvent(screens, "media", 70) != nullptr);
    CHECK(firstEvent(screens, "media", 70)->game == amber.toString());
    // The pages: right through every kind (the clip's page too, I5 step 8), nothing past the last (106), left, Home,
    // End; the catalog's viewer on its first.
    const std::vector<std::string> pages = eventsOf(screens, "page");
    const std::vector<std::string> expectedPages{
        "70:card:covers:1",      "76:card:backcovers:2",   "80:card:physicalmedia:3", "84:card:3dboxes:4",
        "88:card:screenshots:5", "92:card:titlescreens:6", "96:card:videos:7",        "100:card:marquees:8",
        "104:card:fanart:9",     "108:card:marquees:8",    "112:card:covers:1",       "116:card:fanart:9",
        "172:catalog:covers:1"};
    CHECK(pages == expectedPages);
    // Every picture of Amber Lantern shown at its size; none failed.
    std::vector<std::string> pictures;
    for (const rv::app::GameScreenEventRecord& event : screens.events) {
        if (event.event == "picture" && event.game == amber.toString()) {
            pictures.push_back(event.kind + " " + event.detail);
        }
    }
    std::sort(pictures.begin(), pictures.end());
    CHECK(pictures.size() == 8);
    for (const std::string_view expected : {"3dboxes shown 560x700", "fanart shown 1280x720",
                                            "marquees shown 800x300", "screenshots shown 512x448",
                                            "titlescreens shown 512x448"}) {
        INFO(expected);
        CHECK(std::find(pictures.begin(), pictures.end(), expected) != pictures.end());
    }
    CHECK(screens.failures == 0);
    // Informazioni: from the card at 120 (in place of the viewer), from the quick catalog at 166 and 178 with the other
    // release of the title and its region; the viewer of the same game from it at 172.
    const std::vector<std::string> infos = eventsOf(screens, "info");
    REQUIRE(infos.size() == 3);
    CHECK(infos[0] == "120:card::");
    const rv::app::GameScreenEventRecord* catalogInfoEvent = firstEvent(screens, "info", 166);
    REQUIRE(catalogInfoEvent != nullptr);
    CHECK(catalogInfoEvent->from == "catalog");
    CHECK(catalogInfoEvent->game == fromCatalog->game);
    CHECK(catalogInfoEvent->detail ==
          (catalogInfoEvent->game == cobaltUsa ? "Cobalt Compass (Europe)" : "Cobalt Compass (USA)"));
    // Closed: the viewer by Informazioni (120, 178), Informazioni by Back (132, 186) and by the viewer (172).
    const std::vector<std::string> closed = eventsOf(screens, "closed");
    const std::vector<std::string> expectedClosed{"120:card::media", "132:card::info", "172:catalog::info",
                                                  "178:catalog::media", "186:catalog::info"};
    CHECK(closed == expectedClosed);
    // Back closed only the screens: the world went on from Inspect to Focus, the quick catalog stayed open.
    std::vector<std::uint64_t> focusStates;
    for (const rv::app::InteractionEventRecord& event : run.report->world.interaction.events) {
        if (event.event == "state" && (event.state == "focus" || event.state == "free roam")) {
            focusStates.push_back(event.frame);
        }
    }
    CHECK(focusStates == std::vector<std::uint64_t>{138});
    REQUIRE(run.report->catalog.has_value());
    CHECK(run.report->catalog->open);
    CHECK(screens.viewerFrames > 50);
    CHECK(screens.infoFrames > 20);
}

TEST_CASE("media viewer: a picture that cannot be decoded shows its fallback", "[gpu][world][media_viewer]") {
    ImportedRoom room("corrupt");
    const fs::path shot = room.dir / "corrupt.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "corrupt",
                              roomArguments(67, shot, {"--input-script", arg(inputScript("media_viewer_corrupt.json"))}));
    skipWithoutOpenGl(run);
    const rv::app::GameScreensRecord& screens = requireScreens(run);
    INFO(run.describe());
    const std::string bramble = gameOf(*run.report, "Bramble Burrow Rally (USA).sfc").toString();
    const rv::app::GameScreenEventRecord* opened = firstEvent(screens, "media", 56);
    REQUIRE(opened != nullptr);
    CHECK(opened->game == bramble);
    CHECK(opened->from == "catalog");
    CHECK(opened->detail == "covers,physicalmedia,screenshots,videos");
    CHECK(eventsOf(screens, "page") ==
          std::vector<std::string>{"56:catalog:covers:1", "60:catalog:videos:4", "63:catalog:screenshots:3"});
    std::vector<std::string> pictures;
    for (const rv::app::GameScreenEventRecord& event : screens.events) {
        if (event.event == "picture") {
            pictures.push_back(event.kind + " " + event.detail.substr(0, event.detail.find(' ')));
        }
    }
    std::sort(pictures.begin(), pictures.end());
    CHECK(pictures == std::vector<std::string>{"covers shown", "physicalmedia shown", "screenshots failed"});
    CHECK(screens.failures == 1);
    // The fallback's page shows no picture (screenshot reviewed by hand).
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
}
