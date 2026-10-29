// The arcade cabinets playing their clips (I5 step 4, ADR-0016 §6): the building fixture (tests/fixtures/esde/building)
// recorded as the library of the case's data directory, every placed mame title given a generated clip
// (media::writeTestVideo: a moving pattern, its frame number burned in; never committed, never a real clip) before its
// import, retroverse.exe --start-room area:arcade, lockstep (--fixed-dt), hidden. The run reports tell what the screens
// did (the video section's cabinetEvents, screens and slots' events); F12 screenshots (<data>/screenshots) show it:
//  - the inspected cabinet plays its clip in the hand (`direct`), in Focus after Indietro (`focus`), and shows its cover
//    again on Back; the hero cabinet never follows the selection;
//  - attract plays on the nearest cabinet in view within 3 m facing the eye after its 1 s dwell, and stops when it
//    leaves the view;
//  - with one decoder the inspected cabinet takes the hero cabinet's slot, and the hero keeps its paused frame (two
//    screenshots: the same frame, lit, not its still);
//  - no cabinet plays while the room editor runs (I4b's session: its fixtures move, are held and rebuilt), and attract
//    comes back after it; a cabinet moved by the editor and saved plays attract at its new place, never over its cover;
//  - a launch from the inspected cabinet (the fake emulator) reopens its clip in the hand where it stopped;
//  - the quick catalog's selection of an arcade game plays on the hero cabinet (I5 step 4's review: it ignores only the
//    world's selection).

#include "building_support.hpp"
#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

#include <rv/app/cabinet_screens.hpp>
#include <rv/app/room_items.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/media/test_video.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The window's size: Focus poses are framed for its aspect.
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
/// The walker's eye above its feet (scene::FirstPersonController's default), the mouse's look per pixel.
constexpr float kEyeHeight = 1.65f;
constexpr float kRadiansPerPixel = 0.0022f;
constexpr std::string_view kRoom = "area:arcade";
/// The hero cabinet's screen (I6 step 6): it keeps the room's playlist.
constexpr std::string_view kHeroScreen = "area:arcade/fx:hero.crt";
/// A frame of the run's 60 a second: the attract dwell (1 s) is 60 of them.
constexpr int kDwellFrames = 60;

/// The clips the cabinets play: every mame title with a cover (the placed ones) gets one at media/mame/videos (ES-DE's
/// folder: the import finds it), 4 s at 30 fps; the second in name order a portrait 3:4 clip (a vertical monitor's,
/// which fills a cabinet's screen), the third a 16:9 one, the others 4:3 (letterboxed on the portrait screens).
void writeArcadeClips(const rv::test::Fixture& fixture) {
    std::vector<fs::path> covers;
    for (const fs::directory_entry& entry : fs::directory_iterator(fixture.path("media/mame/covers"))) {
        covers.push_back(entry.path());
    }
    std::sort(covers.begin(), covers.end());
    fs::create_directories(fixture.path("media/mame/videos"));
    for (std::size_t index = 0; index < covers.size(); ++index) {
        rv::media::TestVideoSpec spec;
        spec.width = index == 1 ? 360 : index == 2 ? 854 : 640;
        spec.height = 480;
        spec.frameRate = 30;
        spec.durationSeconds = 4.0;
        spec.seed = 400u + static_cast<std::uint32_t>(index);
        spec.quantizer = 5;
        spec.keyFrameInterval = 30;
        const fs::path clip = fixture.path("media/mame/videos") /
                              rv::core::pathFromUtf8(rv::core::pathToUtf8(covers[index].stem()) + ".mp4");
        INFO("clip " << arg(clip));
        REQUIRE(rv::media::writeTestVideo(clip, spec).has_value());
    }
}

/// The building of a case under cabinets/, its mame titles with clips.
struct ArcadeBuilding : PlannedBuilding {
    explicit ArcadeBuilding(std::string_view name)
        : PlannedBuilding(std::string("cabinets/") + std::string(name), true, "esde/building", writeArcadeClips) {}
};

/// video.json in the building's data directory.
void writeVideoSettings(const PlannedBuilding& building, const Json& settings) {
    const fs::path config = building.data / "config";
    fs::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
}

Json step(int from, int to, Json actions) {
    return Json{{"fromFrame", from}, {"toFrame", to}, {"actions", std::move(actions)}};
}

fs::path writeScript(const PlannedBuilding& building, std::string_view name, Json steps) {
    const fs::path file = building.dir / rv::core::pathFromUtf8(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

/// A lockstep run in the arcade room: `frames` frames, a screenshot at the end, `extra` arguments.
AppRun runArcade(const PlannedBuilding& building, std::string_view name, int frames, std::vector<std::string> extra) {
    std::vector<std::string> arguments{"--start-room", std::string(kRoom), "--exit-after-frames", "--frames",
                                       std::to_string(frames), "--fixed-dt", "0.016667", "--screenshot",
                                       arg(building.dir / (std::string(name) + ".png"))};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), building.dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    return run;
}

/// The checks of every run: clean, the arcade room shown, its hero cabinet bound, never more decoders than `budget`.
const rv::app::VideoRecord& requireArcade(const AppRun& run, std::uint64_t budget) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    CHECK(run.report->world.state == "room");
    CHECK(run.report->world.room == kRoom);
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.screen == kHeroScreen);
    CHECK(video.maxDecoders == budget);
    CHECK(video.maxOpenDecoders <= budget);
    CHECK(video.maxDecoderThreads <= budget);
    CHECK(video.joinedAtExit);
    return video;
}

/// The plan the run showed and the arcade room's scene and collection, as retroverse.exe made them from it.
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

/// The walker `distance` (0.9 m) in front of an item's visible face and `lateral` along its wall, looking at its middle;
/// the face and the camera then.
struct Viewpoint {
    glm::vec3 face{0.0f};
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Viewpoint viewpointOf(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens, float distance = 0.9f,
                      float lateral = 0.0f) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 face(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
    // `lateral` along the item's wall (its +X).
    const glm::vec3 along(out.z, 0.0f, -out.x);
    const glm::vec3 eye(face.x + distance * out.x + lateral * along.x, kEyeHeight,
                        face.z + distance * out.z + lateral * along.z);
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

/// The walker at `item`'s Focus pose as --spawn takes it (the default window's aspect).
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

/// The hero cabinet's fixture transform (its hero corner's "crt" part).
glm::mat4 heroCabinetTransform(const ShownRoom& shown) {
    const auto& fixtures = shown.stored.plan.fixtures;
    const auto hero = std::find_if(fixtures.begin(), fixtures.end(), [](const rv::world::Fixture& fixture) {
        return fixture.id.room().toString() == kRoom && fixture.role == rv::world::FixtureRole::HeroCorner &&
               fixture.id.part() == "crt";
    });
    REQUIRE(hero != fixtures.end());
    return rv::world::room_frames::fixtureTransform(*hero);
}

/// The middle of the hero cabinet's screen in the room: its prefab's ScreenSurface (built from the scene object's
/// parameters) at the object's transform.
glm::vec3 heroScreenPoint(const ShownRoom& shown) {
    const auto object = std::find_if(shown.scene.objects.begin(), shown.scene.objects.end(), [](const auto& candidate) {
        return candidate.id.ends_with("fx:hero.crt");
    });
    REQUIRE(object != shown.scene.objects.end());
    auto prefab = rv::geometry::buildPrefab(object->kind, object->params);
    REQUIRE(prefab.has_value());
    const rv::geometry::Socket* surface = rv::geometry::findSocket(prefab->sockets, "ScreenSurface");
    REQUIRE(surface != nullptr);
    const glm::vec4 local = surface->transform * glm::vec4(surface->bounds.center(), 1.0f);
    return glm::vec3(object->instanceTransforms().front() * local);
}

/// A cabinet on the hero cabinet's wall (its screen facing the same way, of another game than `except`) and where to
/// stand looking at it (`distance` in front of its screen, `lateral` along its wall) so that, inspected, the hero
/// cabinet's screen shows at the left edge of the view beside the item in the hand (whose card covers the right): the
/// hero's screen's pixel then.
struct HeroInView {
    const rv::world::CollectionItem* item = nullptr;
    float distance = 0.0f;
    float lateral = 0.0f;
    glm::ivec2 heroPixel{0};
};

HeroInView cabinetBesideHero(const ShownRoom& shown, std::string_view except) {
    const glm::vec3 hero = heroScreenPoint(shown);
    const glm::vec3 heroFacing = glm::normalize(glm::vec3(heroCabinetTransform(shown) * glm::vec4(0, 0, 1, 0)));
    HeroInView best;
    float bestReach = 0.0f;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        const glm::vec3 facing = glm::normalize(glm::vec3(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
        if (item.game.toString() == except || glm::dot(facing, heroFacing) < 0.98f) {
            continue;
        }
        for (int across = -24; across <= 24; ++across) {
            for (int out = 16; out <= 47; ++out) {
                const float lateral = 0.05f * static_cast<float>(across);
                const float distance = 0.05f * static_cast<float>(out);
                // Within the reticle's 2.5 m (the eye at most 2.35 m from the screen along the floor, 0.55 m above
                // it), and at most 25 degrees off its screen's axis: the cabinets' side panels hide a screen seen more
                // aslant from the pick.
                const float reach = std::sqrt(lateral * lateral + distance * distance);
                if (reach > 2.35f || std::abs(lateral) > 0.466f * distance) {
                    continue;
                }
                const Viewpoint view = viewpointOf(item, shown.scene.camera, distance, lateral);
                const std::optional<glm::ivec2> pixel = projectToPixel(view.camera, kShotSize, hero);
                if (!pixel || pixel->x < 90 || pixel->x > 160 || pixel->y < 250 || pixel->y > 850) {
                    continue;
                }
                if (best.item == nullptr || reach < bestReach) {
                    best = HeroInView{&item, distance, lateral, *pixel};
                    bestReach = reach;
                }
            }
        }
    }
    REQUIRE(best.item != nullptr);
    return best;
}

/// A featured cabinet (the display pairs facing the aisle) whose game is not `except` (the hero cabinet's: its clip
/// would share the hero's slot).
const rv::world::CollectionItem& featuredCabinet(const ShownRoom& shown, std::string_view except) {
    const auto found = std::find_if(shown.layout.items.begin(), shown.layout.items.end(), [&](const auto& item) {
        return item.section == rv::world::Section::Featured && item.game.toString() != except;
    });
    REQUIRE(found != shown.layout.items.end());
    return *found;
}

/// The first run: the room planned, the covers into the cache, the hero cabinet's start choice.
struct FirstRun {
    ShownRoom shown;
    std::string heroGame;
};

FirstRun firstRun(const PlannedBuilding& building) {
    const AppRun first = runArcade(building, "first", 2, {});
    requireArcade(first, 2);
    return FirstRun{shownRoom(building, *first.report), first.report->video.gameId};
}

/// The F12 screenshots of a run (<data>/screenshots), in the order they were taken (their names).
std::vector<fs::path> takenScreenshots(const PlannedBuilding& building) {
    std::vector<fs::path> files;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(building.data / "screenshots", error)) {
        if (entry.path().extension() == ".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

rv::assets::ImageData readShot(const fs::path& file) {
    auto image = rv::assets::loadImage(file, 3);
    INFO(rv::core::pathToUtf8(file));
    REQUIRE(image.has_value());
    return std::move(*image);
}

/// Pixels of `a` and `b` (the same size) within the box [min, max] whose channels differ by more than 24 (the picture
/// changed there, not the dithering).
std::size_t changedPixels(const rv::assets::ImageData& a, const rv::assets::ImageData& b, glm::ivec2 min,
                          glm::ivec2 max) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    std::size_t changed = 0;
    for (int y = std::max(0, min.y); y <= std::min(a.height - 1, max.y); ++y) {
        for (int x = std::max(0, min.x); x <= std::min(a.width - 1, max.x); ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(a.channels);
            int largest = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                largest = std::max(largest, std::abs(int(a.pixels[at + c]) - int(b.pixels[at + c])));
            }
            changed += largest > 24 ? 1 : 0;
        }
    }
    return changed;
}

/// The cabinet events of `screen`, in order.
std::vector<rv::app::VideoCabinetEventRecord> cabinetEvents(const rv::app::VideoRecord& video, std::string_view screen) {
    std::vector<rv::app::VideoCabinetEventRecord> events;
    std::copy_if(video.cabinetEvents.begin(), video.cabinetEvents.end(), std::back_inserter(events),
                 [&](const rv::app::VideoCabinetEventRecord& event) { return event.screen == screen; });
    return events;
}

std::vector<rv::app::VideoSlotEventRecord> slotEvents(const rv::app::VideoRecord& video, std::string_view action,
                                                      std::string_view gameId) {
    std::vector<rv::app::VideoSlotEventRecord> events;
    std::copy_if(video.slotEvents.begin(), video.slotEvents.end(), std::back_inserter(events),
                 [&](const rv::app::VideoSlotEventRecord& event) {
                     return event.action == action && event.gameId == gameId;
                 });
    return events;
}

/// Mean absolute difference of the channels of `a` and `b` in the window of `radius` around `center`.
double windowDifference(const rv::assets::ImageData& a, const rv::assets::ImageData& b, glm::ivec2 center,
                        int radius) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double sum = 0.0;
    std::size_t count = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(a.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(a.width - 1, center.x + radius); ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(a.channels);
            for (std::size_t c = 0; c < 3; ++c) {
                sum += std::abs(int(a.pixels[at + c]) - int(b.pixels[at + c]));
                ++count;
            }
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

/// emulators.json's "fake-return" profile (the fake emulator of this build, 2.5 s, background) assigned to mame.
void assignFakeEmulator(const PlannedBuilding& building) {
    const Json profile{{"id", "fake-return"},
                       {"executable", arg(fakeEmulatorExe())},
                       {"arguments", {"--sleep-ms", "2500", "--exit-timeout-ms", "30000", "--", "{rom}"}},
                       {"windowTimeoutSeconds", 0},
                       {"hideConsoleWindow", true},
                       {"systems", {{"mame", Json::object()}}}};
    const Json document{{"schemaVersion", 1}, {"profiles", Json::array({profile})}};
    fs::create_directories(building.data / "config");
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "emulators.json", document.dump(2) + "\n")
                .has_value());
    building.cli("assign_mame", {"assign", "mame", "--profile", "fake-return"});
}

/// The title of `game` in the arcade room's items, lower case: what the quick catalog's search finds it by.
std::string searchOf(const PlannedBuilding& building, const rv::world::RoomId& roomId, const rv::catalog::GameId& game) {
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::Room& room = roomIn(stored.plan, roomId.toString());
    const auto items = rv::app::readRoomItems(
        rv::app::roomItemsFile(building.data / "cache", rv::catalog::LibraryId::parse("esde").value(),
                               room.sections.empty() ? room.id.system() : room.sections.front()));
    REQUIRE(items.has_value());
    const rv::app::RoomItemText* text = items->find(game);
    REQUIRE(text != nullptr);
    REQUIRE_FALSE(text->title.empty());
    std::string search = text->title;
    std::transform(search.begin(), search.end(), search.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return search;
}

const rv::app::VideoScreenRecord* screenOf(const rv::app::VideoRecord& video, std::string_view id) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [id](const rv::app::VideoScreenRecord& screen) { return screen.screen == id; });
    return found != video.screens.end() ? &*found : nullptr;
}

/// The hero cabinet never followed a selection (ADR-0016 §6): every game it played came from its start choice or its
/// playlist.
void requireHeroUnselected(const rv::app::VideoRecord& video) {
    CHECK(video.selections == 0);
    for (const rv::app::VideoPlayedRecord& played : video.played) {
        INFO("the hero cabinet played " << played.gameId << " (" << played.origin << ") at " << played.atSeconds);
        CHECK(played.origin != "selection");
    }
}

} // namespace

TEST_CASE("cabinets: the inspected cabinet plays its clip in the hand, in Focus after Indietro, its cover after Back",
          "[gpu][video][cabinets]") {
    ArcadeBuilding building("inspect");
    // No attract here: after Back the cabinet stands in front of the walker, which would start it again after 1 s.
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}}}});
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    // Frame 40 interact: inspected at once (a face-out item), lifted into the hand; F12 at 80 and 84. Frame 100
    // ui_back (Indietro): Focus on it, lowered to its pulled-out pose; F12 at 140 and 144. Frame 160 back: free roam,
    // the cabinet put back; F12 at 200 and 204.
    const fs::path script = writeScript(
        building, "inspect.json",
        Json::array({step(40, 40, {{"interact", 1}}), step(80, 80, {{"screenshot", 1}}),
                     step(84, 84, {{"screenshot", 1}}), step(100, 100, {{"ui_back", 1}}),
                     step(140, 140, {{"screenshot", 1}}), step(144, 144, {{"screenshot", 1}}),
                     step(160, 160, {{"back", 1}}), step(200, 200, {{"screenshot", 1}}),
                     step(204, 204, {{"screenshot", 1}})}));
    const AppRun run =
        runArcade(building, "inspect", 210,
                  {"--spawn", spawnArgument(focusSpawn(item, first.shown.scene.camera)), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    // Its clip on the second slot (the hero cabinet keeps the first), shown in the hand from its first frame, its
    // cover again at the frame Back put it back.
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 2);
    CHECK(events[0].picture == "video");
    CHECK(events[0].gameId == item.game.toString());
    CHECK(events[0].screenClass == "direct");
    // Inspected on the frame of interact, its clip opened and its first frame shown on that frame (a lockstep run
    // waits for it); its cover again on the frame of Back.
    CHECK(events[0].frame == 40u);
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame == 160u);
    const auto opens = slotEvents(video, "open", item.game.toString());
    REQUIRE(opens.size() == 1);
    CHECK(opens.front().frame == 40u);
    CHECK(opens.front().screens == std::vector<std::string>{screen});
    const auto stops = slotEvents(video, "stop", item.game.toString());
    REQUIRE_FALSE(stops.empty());
    CHECK(stops.front().reason == "unwanted");
    CHECK(video.maxOpenDecoders == 2);
    CHECK(video.fallbacks.empty());
    // The hero cabinet (behind the walker: paused from the start, on its cover) kept its own game throughout.
    requireHeroUnselected(video);
    CHECK(video.gameId == first.heroGame);
    // The screenshots: the clip moving in the hand (80 and 84) and in Focus (140 and 144), the cover still after Back
    // (200 and 204), each around the middle of the view where the cabinet's screen is.
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 6);
    const glm::ivec2 middleMin(kShotSize.x * 3 / 10, kShotSize.y * 2 / 10);
    const glm::ivec2 middleMax(kShotSize.x * 7 / 10, kShotSize.y * 8 / 10);
    const std::size_t inHand = changedPixels(readShot(shots[0]), readShot(shots[1]), middleMin, middleMax);
    const std::size_t inFocus = changedPixels(readShot(shots[2]), readShot(shots[3]), middleMin, middleMax);
    const std::size_t putBack = changedPixels(readShot(shots[4]), readShot(shots[5]), middleMin, middleMax);
    WARN("pixels changed in 4 frames: in the hand " << inHand << ", in Focus " << inFocus << ", put back " << putBack);
    CHECK(inHand > 500u);
    CHECK(inFocus > 500u);
    CHECK(putBack < 50u);
}

TEST_CASE("cabinets: attract plays the nearest cabinet facing the eye after 1 s and stops once it leaves the view",
          "[gpu][video][cabinets]") {
    ArcadeBuilding building("attract");
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    const Viewpoint view = viewpointOf(item, first.shown.scene.camera);
    // F12 at frames 120 and 124 (attract playing); frames 130 to 159 a step back (it plays on: the hysteresis), F12 at
    // 170; frame 180 a half turn (the cabinet out of view); the run ends at 200.
    const float halfTurn = 3.14159265f / kRadiansPerPixel;
    const fs::path script = writeScript(
        building, "attract.json",
        Json::array({step(120, 120, {{"screenshot", 1}}), step(124, 124, {{"screenshot", 1}}),
                     step(130, 159, {{"move_forward", -1.0}}), step(170, 170, {{"screenshot", 1}}),
                     step(180, 180, {{"look_delta_x", halfTurn}})}));
    const AppRun run =
        runArcade(building, "attract", 200, {"--spawn", spawnArgument(view.spawn), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 2);
    CHECK(events[0].picture == "video");
    CHECK(events[0].screenClass == "ambient");
    CHECK(events[0].gameId == item.game.toString());
    // Its dwell from the first frame: asked for at frame 60, its first frame shown on that frame (a lockstep run waits
    // for it).
    CHECK(events[0].frame == static_cast<std::uint64_t>(kDwellFrames));
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame == 180u);
    // No other cabinet played; the open came at the dwell's end.
    CHECK(video.cabinetEvents.size() == 2);
    const auto opens = slotEvents(video, "open", item.game.toString());
    REQUIRE(opens.size() == 1);
    CHECK(opens.front().frame == static_cast<std::uint64_t>(kDwellFrames));
    requireHeroUnselected(video);
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 3);
    const rv::assets::ImageData shot = readShot(shots[0]);
    const rv::assets::ImageData later = readShot(shots[1]);
    const std::optional<glm::ivec2> face = projectToPixel(view.camera, kShotSize, view.face);
    REQUIRE(face.has_value());
    // The clip moves in the screen's window in 4 frames (lit is not enough: the cover is lit too).
    const glm::ivec2 reach(60, 60);
    const std::size_t moved = changedPixels(shot, later, *face - reach, *face + reach);
    WARN("the attract cabinet's screen at (" << face->x << ", " << face->y << "): mean luma "
                                             << windowLuma(shot, *face, 40) << ", " << moved
                                             << " pixels changed in 4 frames");
    CHECK(windowLuma(shot, *face, 40) > 30.0);
    CHECK(moved > 500u);
}

TEST_CASE("cabinets: with one decoder the inspected cabinet takes the slot and the hero cabinet keeps its paused frame",
          "[gpu][video][cabinets]") {
    ArcadeBuilding building("one_decoder");
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}}}});
    const FirstRun first = firstRun(building);
    const HeroInView beside = cabinetBesideHero(first.shown, first.heroGame);
    const rv::world::CollectionItem& item = *beside.item;
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    // The walker in front of a cabinet on the hero cabinet's wall, looking at it, the hero cabinet's screen at the left
    // edge of the view (its clip playing); frame 70 interact: inspected (the hero cabinet's slot taken), F12 at 120 and
    // 124 (the hero cabinet beside the item in the hand). Frame 130 ui_back (Focus), frame 170 back (free roam).
    const Viewpoint view = viewpointOf(item, first.shown.scene.camera, beside.distance, beside.lateral);
    const std::optional<glm::ivec2> heroPixel = beside.heroPixel;
    INFO("the hero cabinet's screen at (" << heroPixel->x << ", " << heroPixel->y << "), the walker " << beside.distance
                                          << " m in front of " << item.game.toString() << ", " << beside.lateral
                                          << " m along its wall");
    const Json steps = Json::array({step(70, 70, {{"interact", 1}}), step(120, 120, {{"screenshot", 1}}),
                                    step(124, 124, {{"screenshot", 1}}), step(130, 130, {{"ui_back", 1}}),
                                    step(170, 170, {{"back", 1}})});
    const fs::path script = writeScript(building, "inspect.json", steps);
    const AppRun run = runArcade(building, "one_decoder", 230,
                                 {"--max-decoders", "1", "--spawn", spawnArgument(view.spawn), "--input-script",
                                  arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 1);
    INFO(run.describe());
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 2);
    const rv::assets::ImageData kept = readShot(shots[0]);
    const rv::assets::ImageData keptLater = readShot(shots[1]);
    CHECK(video.maxOpenDecoders == 1);
    // The inspected cabinet (direct) takes the hero's slot at once: the hero's clip stops, still wanted ("needed").
    const auto heroStops = slotEvents(video, "stop", first.heroGame);
    REQUIRE_FALSE(heroStops.empty());
    CHECK(heroStops.front().reason == "needed");
    CHECK(heroStops.front().frame == 70u);
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 2);
    CHECK(events[0].picture == "video");
    CHECK(events[0].screenClass == "direct");
    // It opens once the hero's decoder thread ended (a frame or two: wall-clock time), its first frame shown then.
    const auto cabinetOpens = slotEvents(video, "open", item.game.toString());
    REQUIRE(cabinetOpens.size() == 1);
    CHECK(cabinetOpens.front().frame >= 71u);
    CHECK(cabinetOpens.front().frame <= 75u);
    CHECK(events[0].frame == cabinetOpens.front().frame);
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame == 170u);
    // Meanwhile the hero cabinet showed the frame it kept (ADR-0016 §6), then its clip opened again where it stopped once
    // the cabinet let the slot go.
    CHECK(video.heldFrameShows >= 90u);
    const auto heroOpens = slotEvents(video, "open", first.heroGame);
    REQUIRE(heroOpens.size() == 2);
    CHECK(heroOpens.back().frame >= 171u);
    CHECK(heroOpens.back().frame <= 175u);
    CHECK(heroOpens.back().fromSeconds.has_value()); // where it stopped
    const rv::app::VideoScreenRecord* hero = screenOf(video, kHeroScreen);
    REQUIRE(hero != nullptr);
    CHECK_FALSE(hero->heldFrame);
    requireHeroUnselected(video);

    // The kept frame on the screenshots: the same in both (paused), lit (not the tube's black), and not the hero
    // cabinet's still, which the same run without clips shows there (its screenshots moved aside first).
    const fs::path keptShots = building.dir / "kept_frame";
    fs::create_directories(keptShots);
    for (const fs::path& shot : shots) {
        fs::rename(shot, keptShots / shot.filename());
    }
    writeVideoSettings(building, Json{{"enabled", false}, {"cabinet", {{"enabled", false}}}});
    const AppRun still = runArcade(building, "one_decoder_still", 230,
                                   {"--max-decoders", "1", "--spawn", spawnArgument(view.spawn), "--input-script",
                                    arg(script)});
    requireArcade(still, 1);
    const std::vector<fs::path> stillShots = takenScreenshots(building);
    REQUIRE(stillShots.size() == 2);
    const rv::assets::ImageData stillShot = readShot(stillShots[0]);
    constexpr int kWindow = 20;
    const double paused = windowDifference(kept, keptLater, *heroPixel, kWindow);
    const double luma = windowLuma(kept, *heroPixel, kWindow);
    const double fromStill = windowDifference(kept, stillShot, *heroPixel, kWindow);
    WARN("the hero cabinet's kept frame: " << paused << " apart in 4 frames, mean luma " << luma << ", " << fromStill
                                           << " apart from its still");
    CHECK(paused == 0.0);
    CHECK(luma > 10.0);
    CHECK(fromStill > 8.0);
}

TEST_CASE("cabinets: no cabinet plays while the room editor runs, and attract comes back after it",
          "[gpu][video][cabinets][edit]") {
    ArcadeBuilding building("editor");
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    const Viewpoint view = viewpointOf(item, first.shown.scene.camera);
    // Attract from frame 60; frame 90 the directory (M), "Modifica stanza" (as world_edit_discard.json), the editor
    // begins at frame 108; frame 150 its menu, Esci senza salvare at frame 180; the run ends 120 frames after.
    const fs::path script =
        writeScript(building, "editor.json",
                    Json::array({step(90, 90, {{"map", 1}}), step(96, 96, {{"ui_home", 1}}),
                                 step(102, 102, {{"ui_up", 1}}), step(108, 108, {{"ui_activate", 1}}),
                                 step(150, 150, {{"confirm", 1}}), step(160, 160, {{"ui_down", 1}}),
                                 step(170, 170, {{"ui_down", 1}}), step(180, 180, {{"ui_activate", 1}})}));
    const AppRun run =
        runArcade(building, "editor", 300, {"--spawn", spawnArgument(view.spawn), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kRoom);
    CHECK(edit.end == "exited"); // nothing edited: Esci senza salvare leaves at once
    REQUIRE(edit.endFrame.has_value());
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 3);
    // Attract before the session; the cover from the frame the session began; attract again 1 s after it ended.
    CHECK(events[0].picture == "video");
    CHECK(events[0].frame < edit.enterFrame);
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame == edit.enterFrame);
    CHECK(events[2].picture == "video");
    CHECK(events[2].frame == *edit.endFrame + kDwellFrames);
    CHECK(video.cabinetEvents.size() == 3);
}

TEST_CASE("cabinets: a cabinet moved by the editor and saved plays attract at its new place, never over its cover",
          "[gpu][video][cabinets][edit]") {
    // I5 step 4's review: the editor's operation lays the room's collection out again in place (setItems), the save
    // writes the plan; after the session attract plays the moved cabinet's clip at its item's new transform, its static
    // front (the cover) hidden beneath it on every frame (world.cabinetCoverBeneathFrames).
    ArcadeBuilding building("editor_move");
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    // Frame 40 interact: inspected (its clip in the hand); frames 70, 76 ui_down: Informazioni, Sposta; frame 82
    // ui_activate: Sposta, the editor with the cabinet's game in the hand. Frame 110 ui_right: the next slot targeted,
    // the walker eased in front of it; frame 140 interact: the game moved there (or swapped). Frame 170 confirm: the
    // menu, Salva focused; frame 190 ui_activate: Salva, the session ends. Attract 1 s after; F12 at 300 and 304.
    const fs::path script = writeScript(
        building, "editor_move.json",
        Json::array({step(40, 40, {{"interact", 1}}), step(70, 70, {{"ui_down", 1}}), step(76, 76, {{"ui_down", 1}}),
                     step(82, 82, {{"ui_activate", 1}}), step(110, 110, {{"ui_right", 1}}),
                     step(140, 140, {{"interact", 1}}), step(170, 170, {{"confirm", 1}}),
                     step(190, 190, {{"ui_activate", 1}}), step(300, 300, {{"screenshot", 1}}),
                     step(304, 304, {{"screenshot", 1}})}));
    const AppRun run = runArcade(building, "editor_move", 320,
                                 {"--spawn", spawnArgument(focusSpawn(item, first.shown.scene.camera)),
                                  "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kRoom);
    CHECK(edit.heldAtStart == item.game.toString());
    CHECK(edit.end == "saved");
    CHECK(edit.written);
    REQUIRE(edit.operations.size() == 1);
    CHECK(edit.operations.front().refused.empty());
    CHECK(edit.operations.front().path == "in-place");
    REQUIRE(edit.endFrame.has_value());
    // The saved plan: the cabinet's game in another slot (a slot's pick id: its screen's id is that slot's now).
    const ShownRoom after = shownRoom(building, *run.report);
    const auto moved = std::find_if(after.layout.items.begin(), after.layout.items.end(),
                                    [&](const rv::world::CollectionItem& candidate) {
                                        return candidate.game == item.game;
                                    });
    REQUIRE(moved != after.layout.items.end());
    CHECK(moved->slot != item.slot);
    CHECK(moved->slotFrame != item.slotFrame);
    // In the hand before the session, its cover from the session's first frame; attract at its new place after it.
    const std::vector<rv::app::VideoCabinetEventRecord> before =
        cabinetEvents(video, rv::app::cabinetScreenId(kRoom, item.pickId));
    REQUIRE(before.size() == 2);
    CHECK(before[0].picture == "video");
    CHECK(before[0].screenClass == "direct");
    CHECK(before[0].gameId == item.game.toString());
    CHECK(before[1].picture == "cover");
    CHECK(before[1].frame == edit.enterFrame);
    const std::vector<rv::app::VideoCabinetEventRecord> events =
        cabinetEvents(video, rv::app::cabinetScreenId(kRoom, moved->pickId));
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().picture == "video");
    CHECK(events.back().screenClass == "ambient");
    CHECK(events.back().gameId == item.game.toString());
    CHECK(events.back().frame == *edit.endFrame + kDwellFrames);
    // Its clip drew on the dynamic path, its cover never beneath it.
    CHECK(run.report->world.cabinetClipFrames > 0u);
    CHECK(run.report->world.cabinetCoverBeneathFrames == 0u);
    // The screenshots: the clip moving in front of the walker, where the walker was eased (the new slot).
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 2);
    const glm::ivec2 middleMin(kShotSize.x * 3 / 10, kShotSize.y * 2 / 10);
    const glm::ivec2 middleMax(kShotSize.x * 7 / 10, kShotSize.y * 8 / 10);
    const std::size_t changed = changedPixels(readShot(shots[0]), readShot(shots[1]), middleMin, middleMax);
    WARN("pixels changed in 4 frames at the moved cabinet: " << changed);
    CHECK(changed > 500u);
}

TEST_CASE("cabinets: a launch from the inspected cabinet reopens its clip in the hand where it stopped",
          "[gpu][video][cabinets][launch]") {
    // I5 step 4's review: Avvia on the inspected cabinet's card launches the fake emulator (RV_LAUNCH_ALLOWLIST); the
    // cabinet stays in the hand (`direct`) through Launching and Returning, its clip stopped for the launch at its
    // position and opened again there after the return.
    ensureLaunchAllowList();
    ArcadeBuilding building("launch");
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}}}});
    assignFakeEmulator(building);
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    // Frame 40 interact: inspected, its clip in the hand; frame 80 confirm: Avvia (the fake runs 2.5 s, no frame
    // meanwhile); after the return Inspect of the same item again; F12 at 110 and 114.
    const fs::path script = writeScript(
        building, "launch.json",
        Json::array({step(40, 40, {{"interact", 1}}), step(80, 80, {{"confirm", 1}}),
                     step(110, 110, {{"screenshot", 1}}), step(114, 114, {{"screenshot", 1}})}));
    const AppRun run = runArcade(building, "launch", 130,
                                 {"--spawn", spawnArgument(focusSpawn(item, first.shown.scene.camera)),
                                  "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == item.game.toString());
    CHECK(launch.outcome == "returned");
    CHECK(report.world.interaction.state == "inspect");
    CHECK(report.world.interaction.focusedGame == item.game.toString());
    // Its clip: opened at the interact, stopped for the launch, opened again after the return where it stopped (within
    // the 40 frames it played: two thirds of a second, less what the decoder had not shown).
    const auto opens = slotEvents(video, "open", item.game.toString());
    REQUIRE(opens.size() == 2);
    CHECK(opens[0].frame == 40u);
    CHECK_FALSE(opens[0].fromSeconds.has_value());
    CHECK(opens[1].frame > launch.requestFrame);
    CHECK(opens[1].screens == std::vector<std::string>{screen});
    REQUIRE(opens[1].fromSeconds.has_value());
    CHECK(*opens[1].fromSeconds > 0.3);
    CHECK(*opens[1].fromSeconds <= 41.0 / 60.0);
    const auto stops = slotEvents(video, "stop", item.game.toString());
    REQUIRE_FALSE(stops.empty());
    CHECK(stops.front().reason == "launch");
    // Shown in the hand from the interact to the run's end: its cover never came back (its slot's last frame stayed up
    // while the game ran, the clip reopened on the frame after the return), and it was in view in the hand throughout
    // (Launching and Returning hold it: in view, at arm's length).
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 1);
    CHECK(events[0].picture == "video");
    CHECK(events[0].screenClass == "direct");
    CHECK(events[0].frame == 40u);
    const rv::app::VideoScreenRecord* cabinet = screenOf(video, screen);
    REQUIRE(cabinet != nullptr);
    CHECK(cabinet->picture == "video");
    CHECK(cabinet->screenClass == "direct");
    CHECK(cabinet->playing);
    CHECK(cabinet->visible);
    // The screenshots: its clip moving in the hand after the return.
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 2);
    const glm::ivec2 middleMin(kShotSize.x * 3 / 10, kShotSize.y * 2 / 10);
    const glm::ivec2 middleMax(kShotSize.x * 7 / 10, kShotSize.y * 8 / 10);
    const std::size_t changed = changedPixels(readShot(shots[0]), readShot(shots[1]), middleMin, middleMax);
    WARN("pixels changed in 4 frames in the hand after the return: " << changed);
    CHECK(changed > 500u);
}

TEST_CASE("cabinets: the quick catalog's selection of an arcade game plays on the hero cabinet",
          "[gpu][video][cabinets]") {
    // I5 step 4's review: the hero cabinet ignores the world's selection (a focused cabinet plays itself) but follows
    // the quick catalog's, as every hero screen does (a regression of I5 step 4 against main).
    ArcadeBuilding building("catalog");
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}}}});
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string search = searchOf(building, first.shown.layout.room, item.game);
    // From the room's doorway (the hero cabinet in view): frame 5 the quick catalog, frames 8 and 11 ui_up (the search
    // field), frame 14 the title typed (the list on that game); it stays open to the end.
    const fs::path script = writeScript(
        building, "catalog.json",
        Json::array({step(5, 5, {{"quick_catalog", 1}}), step(8, 8, {{"ui_up", 1}}), step(11, 11, {{"ui_up", 1}}),
                     Json{{"fromFrame", 14}, {"toFrame", 14}, {"text", search}}}));
    const AppRun run = runArcade(building, "catalog", 150, {"--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    INFO("searched " << search);
    CHECK(video.selections >= 1u);
    CHECK(video.gameId == item.game.toString());
    CHECK(video.origin == "selection");
    const auto selected = std::find_if(video.played.begin(), video.played.end(), [&](const auto& played) {
        return played.gameId == item.game.toString() && played.origin == "selection";
    });
    CHECK(selected != video.played.end());
    // Its clip opened on the hero cabinet.
    const auto opens = slotEvents(video, "open", item.game.toString());
    REQUIRE_FALSE(opens.empty());
    CHECK(std::find(opens.front().screens.begin(), opens.front().screens.end(), std::string(kHeroScreen)) !=
          opens.front().screens.end());
}

namespace {

/// The pixel box an item's slot box covers in a screenshot from `camera` (its eight corners projected; empty when one
/// is behind the camera).
std::optional<std::pair<glm::ivec2, glm::ivec2>> projectedBox(const rv::world::CollectionItem& item,
                                                              const rv::scene::CameraSetup& camera) {
    const rv::core::Aabb box = item.worldBounds();
    glm::ivec2 min(kShotSize);
    glm::ivec2 max(0);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? box.max.x : box.min.x, (corner & 2) != 0 ? box.max.y : box.min.y,
                              (corner & 4) != 0 ? box.max.z : box.min.z);
        const std::optional<glm::ivec2> pixel = projectToPixel(camera, kShotSize, point);
        if (!pixel) {
            return std::nullopt;
        }
        min = glm::min(min, *pixel);
        max = glm::max(max, *pixel);
    }
    return std::make_pair(min, max);
}

/// Pixels of `a` and `b` outside the box [min, max] whose channels differ by more than 24.
std::size_t changedOutside(const rv::assets::ImageData& a, const rv::assets::ImageData& b, glm::ivec2 min,
                           glm::ivec2 max) {
    return changedPixels(a, b, glm::ivec2(0), glm::ivec2(a.width - 1, a.height - 1)) - changedPixels(a, b, min, max);
}

} // namespace

TEST_CASE("cabinets: aiming at a cabinet's screen moves nothing, in free roam and in Focus (fix 2026-10-29)",
          "[gpu][video][cabinets][aim]") {
    // The user's report: aimed at, an arcade cabinet's screen moved. It was the face-out pull-out: after the hover's
    // 150 ms dwell the screen tipped 10 degrees out of its bezel (frames 10 to 24), and stood so in Focus. A cabinet's
    // screen is part of its fixture now: nothing moves while it is aimed at, and Focus after Indietro shows it where
    // it stands. No clip here (attract and showOnFocus off): only the cover, so the screenshots compare exactly.
    ArcadeBuilding building("aim");
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}, {"showOnFocus", false}}}});
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    // F12 at 6 (hovered since frame 2, before the dwell ended), 40 and 80 (long after it); interact at 90 (Inspect),
    // ui_back at 140 (Focus, lowered by 160), F12 at 190.
    const fs::path script =
        writeScript(building, "aim.json",
                    Json::array({step(6, 6, {{"screenshot", 1}}), step(40, 40, {{"screenshot", 1}}),
                                 step(80, 80, {{"screenshot", 1}}), step(90, 90, {{"interact", 1}}),
                                 step(140, 140, {{"ui_back", 1}}), step(190, 190, {{"screenshot", 1}})}));
    const AppRun run =
        runArcade(building, "aim", 200,
                  {"--spawn", spawnArgument(focusSpawn(item, first.shown.scene.camera)), "--input-script",
                   arg(script)});
    requireArcade(run, 2);
    INFO(run.describe());
    const rv::app::InteractionRecord& interaction = run.report->world.interaction;
    CHECK(interaction.state == "focus");
    CHECK(interaction.focusedGame == item.game.toString());
    // Hovered from the first frames on, never pulled out before the interact.
    bool hovered = false;
    for (const rv::app::InteractionEventRecord& event : interaction.events) {
        INFO("frame " << event.frame << ": " << event.event << " " << event.pickId << " " << event.state);
        if (event.event == "hover" && event.frame <= 6) {
            hovered = true;
        }
        if (event.frame < 90) {
            CHECK(event.event != "pull-out");
            CHECK(event.event != "pulled");
        }
    }
    CHECK(hovered);
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 4);
    const rv::assets::ImageData early = readShot(shots[0]);
    const glm::ivec2 whole(kShotSize.x - 1, kShotSize.y - 1);
    // Free roam, aimed at: the same picture at 6, 40 and 80 (it used to tip after frame 10).
    CHECK(changedPixels(early, readShot(shots[1]), glm::ivec2(0), whole) == 0u);
    CHECK(changedPixels(early, readShot(shots[2]), glm::ivec2(0), whole) == 0u);
    // In Focus after Indietro the walker stands where it stood (the spawn is the Focus pose) and the screen where it
    // stands: the cabinet the same (it stood tipped out of its bezel before the fix).
    const std::size_t focusChanged = changedPixels(early, readShot(shots[3]), glm::ivec2(0), whole);
    WARN("pixels changed in Focus against free roam: " << focusChanged);
    CHECK(focusChanged < 200u);
}

TEST_CASE("cabinets: attract swaps the cover for the clip on the screen alone, nothing around it moves (fix "
          "2026-10-29)",
          "[gpu][video][cabinets][aim]") {
    // The dynamic path's video front stands where the static front stood: across the frame the clip replaces the cover
    // only the screen's pixels change.
    ArcadeBuilding building("aim_attract");
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const Viewpoint view = viewpointOf(item, first.shown.scene.camera, 1.6f);
    // F12 at 50 (the cover, aimed at) and 70 (attract's clip since frame 60).
    const fs::path script = writeScript(
        building, "aim_attract.json",
        Json::array({step(50, 50, {{"screenshot", 1}}), step(70, 70, {{"screenshot", 1}})}));
    const AppRun run =
        runArcade(building, "aim_attract", 80, {"--spawn", spawnArgument(view.spawn), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    const std::vector<rv::app::VideoCabinetEventRecord> events =
        cabinetEvents(video, rv::app::cabinetScreenId(kRoom, item.pickId));
    REQUIRE(events.size() == 1);
    CHECK(events[0].picture == "video");
    CHECK(events[0].frame == static_cast<std::uint64_t>(kDwellFrames));
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 2);
    const auto box = projectedBox(item, view.camera);
    REQUIRE(box.has_value());
    const glm::ivec2 margin(4);
    const rv::assets::ImageData cover = readShot(shots[0]);
    const rv::assets::ImageData clip = readShot(shots[1]);
    // The screen changed (the clip), nothing outside its box did.
    CHECK(changedPixels(cover, clip, box->first, box->second) > 1000u);
    CHECK(changedOutside(cover, clip, box->first - margin, box->second + margin) == 0u);
}
