// The card beside the inspected item in the generated room (I3 step 14a; ADR-0012 §5): the snes-room fixture
// (tests/fixtures/esde/snes-room: 50 invented titles with generated covers, every one face-out) imported into the
// case's data directory, retroverse.exe without --scene, lockstep (--fixed-dt), hidden, driven by
// tests/input_scripts/world_card.json from the Focus pose of the hero wall's band item fx:hw-w/s:5.4 computed from the
// plan the run wrote (as test_world_focus.cpp does). The checks read the run report's world.card record (what it
// showed, its focus and presses by frame, what the interaction answered) and the screenshots: the card shows the
// catalog's title, platform, year and readiness of the inspected game; its buttons are reached with the arrows and the
// D-pad and pressed with ui_activate (Enter, A), Vedi media and Informazioni also with their pad buttons, Avvia with
// the pad's Start; Indietro and Back close it; its cover holds the game's hero texture (the fixture's cover fitted as
// the hero picture, compared pixel by pixel); its content is never cut. Avvia launches the game (I3 step 15a): the
// fixture's snes games run the fake emulator of this build through the "fake-return" profile (as test_world_launch.cpp
// assigns it), and Inspect of the same item comes back after the return. Helpers repeat test_world_focus.cpp's (see
// docs/i3/step-14a.md, "For the merge").

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/cover_requests.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/ui_assets.hpp>
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
          dir(freshCaseDir(std::string("world_card/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        // Avvia launches the fake emulator of this build (I3 step 15a), never the fixture's own copy.
        const nlohmann::json profile{
            {"id", "fake-return"},
            {"executable", arg(fakeEmulatorExe())},
            {"arguments", {"--sleep-ms", "2500", "--exit-timeout-ms", "30000", "--", "{rom}"}},
            {"windowTimeoutSeconds", 0},
            {"hideConsoleWindow", true},
            {"systems", {{"snes", nlohmann::json::object()}}}};
        const nlohmann::json document{{"schemaVersion", 1}, {"profiles", nlohmann::json::array({profile})}};
        REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "emulators.json", document.dump(2) + "\n").has_value());
        cli("assign", {"assign", "snes", "--profile", "fake-return"});
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
        "--exit-after-frames", "--frames", std::to_string(frames), "--fixed-dt", "0.016667", "--screenshot", arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

struct ShownRoom {
    rv::scene::CameraSetup camera;
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
    return ShownRoom{scene->camera, std::move(*layout)};
}

const rv::world::CollectionItem& itemIn(const ShownRoom& room, std::string_view slot) {
    const auto found = std::find_if(room.layout.items.begin(), room.layout.items.end(), [&](const auto& item) {
        return item.slot.toString().ends_with("/" + std::string(slot));
    });
    INFO("the item in " << slot);
    REQUIRE(found != room.layout.items.end());
    return *found;
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

const rv::app::CardRecord& requireCard(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.state == "room");
    REQUIRE(run.report->world.interaction.bound);
    const rv::app::CardRecord& card = run.report->world.card;
    REQUIRE(card.bound);
    CHECK(card.eventsDropped == 0);
    // The card's content is never cut by its height.
    CHECK(card.squeezedFrames == 0);
    return card;
}

/// The card's events of `kind`, as "<frame>:<button>[:<via>]" (or "<frame>" for those without a button).
std::vector<std::string> eventsOf(const rv::app::CardRecord& card, std::string_view kind) {
    std::vector<std::string> found;
    for (const rv::app::CardEventRecord& event : card.events) {
        if (event.event == kind) {
            std::string text = std::to_string(event.frame);
            if (!event.button.empty()) {
                text += ":" + event.button;
            }
            if (!event.via.empty()) {
                text += ":" + event.via;
            }
            found.push_back(std::move(text));
        }
    }
    return found;
}

std::vector<std::uint64_t> framesOf(const rv::app::CardRecord& card, std::string_view kind) {
    std::vector<std::uint64_t> frames;
    for (const rv::app::CardEventRecord& event : card.events) {
        if (event.event == kind) {
            frames.push_back(event.frame);
        }
    }
    return frames;
}

/// The fixture's cover of `title` (its USA release when there are several).
fs::path coverOf(const ImportedRoom& room, const std::string& title) {
    std::vector<fs::path> matches;
    for (const fs::directory_entry& entry : fs::directory_iterator(room.fixture.path("media/snes/covers"))) {
        const std::string stem = rv::core::pathToUtf8(entry.path().stem());
        if (stem.starts_with(title + " (")) {
            matches.push_back(entry.path());
        }
    }
    std::sort(matches.begin(), matches.end());
    REQUIRE_FALSE(matches.empty());
    const auto usa = std::find_if(matches.begin(), matches.end(), [](const fs::path& path) {
        return rv::core::pathToUtf8(path.filename()).find("(USA)") != std::string::npos;
    });
    return usa != matches.end() ? *usa : matches.front();
}

/// The mean absolute difference per channel (0-255) between the screenshot's pixels inside `box` (window pixels:
/// x, y, width, height; its border `inset` of its size left out) and `picture` box-filtered to their footprint; and
/// the screenshot's luma spread there (a flat box is no picture).
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
    // Shot pixel -> picture texels.
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

TEST_CASE("world card: the inspected item's card shows its game, its buttons reached and pressed by pad and keyboard, "
          "Indietro and Back close it",
          "[gpu][world][world_card]") {
    ensureLaunchAllowList();
    ImportedRoom room("buttons");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const std::string game = item.game.toString();
    const std::vector<std::string> script{"--spawn", spawnArgument(focusSpawn(item, shown.camera)), "--input-script",
                                          arg(inputScript("world_card.json"))};

    // Mid-way (frame 145): Inspect of the item again after Avvia, the card on Avvia.
    const fs::path cardShot = room.dir / "card.png";
    const AppRun mid = runApp(retroverseExe(), room.dir, "card", roomArguments(146, cardShot, script));
    const rv::app::CardRecord& shownCard = requireCard(mid);
    INFO(mid.describe());
    CHECK(mid.report->world.interaction.state == "inspect");
    CHECK(shownCard.game == game);
    // The catalog's data of the game: its title, the platform's full name, the year and a readiness the
    // quick catalog's card labels too.
    CHECK(shownCard.details);
    // (The fixture's cover of that title is the one compared below.)
    CHECK_FALSE(shownCard.title.empty());
    CHECK(shownCard.platform == "Lumen 16 Home System");
    CHECK(shownCard.year >= 1990);
    CHECK(shownCard.year <= 1998);
    const auto& readiness = rv::ui::quick_catalog::kReadinessKeys;
    CHECK(std::find(readiness.begin(), readiness.end(), shownCard.readiness) != readiness.end());
    CHECK(shownCard.focused == "play");
    // The cover: the game's hero texture, the fixture's cover as the hero picture.
    CHECK(shownCard.coverFrames > 0);
    REQUIRE(shownCard.coverBox.size() == 4);
    CHECK(shownCard.coverBox[2] > 100.0);
    const rv::assets::ImageData image = requireScreenshot(mid, cardShot, kShotSize);
    const fs::path coverFile = coverOf(room, shownCard.title);
    auto bytes = rv::core::readBinaryFile(coverFile);
    REQUIRE(bytes.has_value());
    auto hero = rv::app::makeHeroPicture(*bytes, rv::core::pathToUtf8(coverFile.filename()));
    REQUIRE(hero.has_value());
    const PictureMatch match = comparePicture(image, shownCard.coverBox, *hero, 0.08);
    WARN("card cover vs hero picture: mean difference " << match.meanDifference << ", luma spread "
                                                        << match.lumaSpread);
    CHECK(match.meanDifference < 12.0);
    CHECK(match.lumaSpread > 40.0);

    // The whole script.
    const fs::path endShot = room.dir / "end.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "all", roomArguments(260, endShot, script));
    const rv::app::CardRecord& card = requireCard(run);
    INFO(run.describe());
    // Shown on entering Inspect (frame 40), hidden while Avvia's launch runs and shown again after its return (I3 step
    // 15a: the decision the next frame, the game while the host waits, Inspect again the frame after), hidden at
    // Indietro (170), shown at the second Inspect (200), hidden at Back (230).
    CHECK(framesOf(card, "shown") == std::vector<std::uint64_t>{40, 120, 132, 200});
    CHECK(framesOf(card, "hidden") == std::vector<std::uint64_t>{118, 130, 170, 230});
    // The focus moved by the arrows and the D-pad within the grid (Avvia, Vedi media / Informazioni, Indietro); a
    // card shown again starts on Avvia.
    const std::vector<std::string> focus = eventsOf(card, "focus");
    const std::vector<std::string> expectedFocus{"70:media", "82:back", "88:info", "100:play", "150:media", "156:back"};
    CHECK(focus == expectedFocus);
    // The presses: ui_activate on the focused button (Vedi media, Informazioni, Avvia, Indietro).
    const std::vector<std::string> presses = eventsOf(card, "press");
    const std::vector<std::string> expectedPresses{"76:media:activate", "94:info:activate", "130:play:activate",
                                                   "170:back:activate"};
    CHECK(presses == expectedPresses);
    // What the interaction answered: the card's presses and the shortcuts (the pad's Y and X, the pad's Start).
    CHECK(framesOf(card, "media") == std::vector<std::uint64_t>{76, 106});
    CHECK(framesOf(card, "info") == std::vector<std::uint64_t>{94, 112});
    CHECK(framesOf(card, "launch") == std::vector<std::uint64_t>{118, 130});
    // The interaction's states: Inspect at 40, Focus at Indietro (170), Inspect at 200, Focus at Back (230), Avvia's
    // round trips between (two launches, both returned).
    CHECK(run.report->launches.size() == 2);
    CHECK(run.report->world.launch.returned == 2);
    std::vector<std::uint64_t> focusStates;
    for (const rv::app::InteractionEventRecord& event : run.report->world.interaction.events) {
        if (event.event == "state" && event.state == "focus") {
            focusStates.push_back(event.frame);
        }
    }
    CHECK(focusStates == std::vector<std::uint64_t>{170, 230});
    CHECK(run.report->world.interaction.state == "focus");
    CHECK(card.focused.empty());
    CHECK(card.shownFrames > 100);
}
