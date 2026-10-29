// The decoder scheduler on several screens (I5 step 2, ADR-0016 §2): tests/smoke/scenes/screens_four.json's four CRTs
// (crt_a the hero, crt_b to crt_d secondaries of the room "screens_four") playing generated clips
// (media::writeTestVideo, written into the case directory, never committed) of a mock catalog's games, each screen's
// games named by video.json's rooms.screens_four (explicit playlists of one game). The run reports tell what the
// scheduler did (the video section's screens, slots' events and upload figures; the process priority's changes). I5 step 4
// (ADR-0016 §7): a 16:9 and a portrait clip on the 4:3 tubes, letterboxed and pillarboxed, probed on the screenshot.

#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"

#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/media/test_video.hpp>
#include <rv/scene/scene_description.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
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
using Json = nlohmann::json;

namespace {

constexpr std::string_view kFixedDt = "0.016667";
constexpr double kStep = 0.016667;
constexpr glm::ivec2 kShotSize{1280, 720};
/// ADR-0016 §2: the uploads of one render frame.
constexpr std::uint64_t kUploadBudget = 4ull * 1024ull * 1024ull;
/// Turning by this many mouse pixels at 0.0022 rad each turns the walker 40 degrees: crt_c and crt_d leave the view.
constexpr double kTurnPixels = 317.3;

constexpr std::string_view kGameA = "g000000000000c001";
constexpr std::string_view kGameB = "g000000000000c002";
constexpr std::string_view kGameC = "g000000000000c003";

std::filesystem::path sceneFile() {
    return sourceDir() / "tests" / "smoke" / "scenes" / "screens_four.json";
}

/// Writes a test clip (the frame number burned in) of `seconds` at `size` and `fps`.
void writeClip(const std::filesystem::path& path, double seconds, std::uint32_t seed, glm::ivec2 size = {640, 480},
               int fps = 30) {
    rv::media::TestVideoSpec spec;
    spec.width = size.x;
    spec.height = size.y;
    spec.frameRate = fps;
    spec.durationSeconds = seconds;
    spec.seed = seed;
    spec.quantizer = 5;
    spec.keyFrameInterval = fps;
    INFO("clip " << arg(path));
    REQUIRE(rv::media::writeTestVideo(path, spec).has_value());
}

/// Writes an invented cover picture (abstract shapes and `title`) as a PNG.
void writePicture(const std::filesystem::path& path, std::string_view title, std::uint32_t seed) {
    const rv::assets::ImageData picture = rv::assets::makeCoverPlaceholder(640, 480, title, seed);
    REQUIRE(rv::assets::writePng(path, picture).has_value());
}

Json game(std::string_view id, std::string_view title, Json media, std::optional<std::string_view> lastPlayed = {}) {
    Json entry{{"id", id}, {"title", title}, {"platform", "Portatile 16 bit"}, {"media", std::move(media)}};
    if (lastPlayed) {
        entry["lastPlayed"] = *lastPlayed;
    }
    return entry;
}

std::filesystem::path writeCatalog(const std::filesystem::path& dir, Json games) {
    const Json catalog{{"schemaVersion", 1}, {"games", std::move(games)}};
    const std::filesystem::path file = dir / "catalog.json";
    REQUIRE(rv::core::writeTextFileAtomic(file, catalog.dump(2) + "\n").has_value());
    return file;
}

/// video.json in the case's data directory: each screen of the room plays the one game given (an explicit playlist),
/// plus `extra` global keys.
void writeScreens(const std::filesystem::path& dir, std::string_view a, std::string_view b, std::string_view c,
                  std::string_view d, Json extra = Json::object()) {
    const auto one = [](std::string_view id) {
        return Json{{"playlist", "explicit"}, {"games", Json::array({id})}};
    };
    Json settings = std::move(extra);
    settings["rooms"] = Json{{"screens_four",
                              {{"hero", one(a)},
                               {"screens", {{"crt_b", one(b)}, {"crt_c", one(c)}, {"crt_d", one(d)}}}}}};
    const std::filesystem::path config = dir / "data" / "config";
    std::filesystem::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
}

/// The three games of the room (A played last: the hero's start choice), each with a 640 x 480 clip and a cover.
std::filesystem::path writeRoom(const std::filesystem::path& dir) {
    writeClip(dir / "clips" / "a.mp4", 6.0, 71);
    writeClip(dir / "clips" / "b.mp4", 6.0, 72);
    writeClip(dir / "clips" / "c.mp4", 6.0, 73);
    writePicture(dir / "covers" / "a.png", "Arco", 81);
    writePicture(dir / "covers" / "b.png", "Bussola", 82);
    writePicture(dir / "covers" / "c.png", "Campana", 83);
    writeScreens(dir, kGameA, kGameA, kGameB, kGameC);
    return writeCatalog(
        dir, Json::array({game(kGameA, "Arco di Pietra", {{"video", "clips/a.mp4"}, {"cover", "covers/a.png"}},
                               "2026-10-01T10:00:00Z"),
                          game(kGameB, "Bussola d'Ottone", {{"video", "clips/b.mp4"}, {"cover", "covers/b.png"}}),
                          game(kGameC, "Campana di Rame", {{"video", "clips/c.mp4"}, {"cover", "covers/c.png"}})}));
}

Json step(int from, int to, Json actions) {
    return Json{{"fromFrame", from}, {"toFrame", to}, {"actions", std::move(actions)}};
}

std::filesystem::path writeScript(const std::filesystem::path& dir, std::string_view name, Json steps) {
    const std::filesystem::path file = dir / rv::core::pathFromUtf8(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

/// A frame-limited, fixed-step run of the four-CRT room with `catalog`, `frames` long, and `extra` arguments.
AppRun runScreens(const std::filesystem::path& dir, std::string_view name, const std::filesystem::path& catalog,
                  int frames, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--scene",      arg(sceneFile()),      "--mock-catalog", arg(catalog),
                                       "--exit-after-frames", "--frames", std::to_string(frames),
                                       "--fixed-dt",   std::string(kFixedDt), "--screenshot-size", "1280x720"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    return run;
}

/// The checks of every run: clean, four screens bound, never more decoders open or alive than the budget.
const rv::app::VideoRecord& requireScreens(const AppRun& run, std::uint64_t budget) {
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.screen == "crt_a");
    REQUIRE(video.screens.size() == 4);
    CHECK(video.maxDecoders == budget);
    CHECK(video.maxOpenDecoders <= budget);
    CHECK(video.maxDecoderThreads <= budget);
    CHECK(video.joinedAtExit);
    // A lockstep run never watches its late frames (--watch-late-frames aside): no drop to one decoder.
    CHECK(video.budgetDrops.empty());
    return video;
}

const rv::app::VideoScreenRecord& screenOf(const rv::app::VideoRecord& video, std::string_view id) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [id](const rv::app::VideoScreenRecord& screen) { return screen.screen == id; });
    REQUIRE(found != video.screens.end());
    return *found;
}

std::vector<rv::app::VideoSlotEventRecord> eventsOf(const rv::app::VideoRecord& video, std::string_view action,
                                                    std::string_view gameId) {
    std::vector<rv::app::VideoSlotEventRecord> events;
    std::copy_if(video.slotEvents.begin(), video.slotEvents.end(), std::back_inserter(events),
                 [&](const rv::app::VideoSlotEventRecord& event) {
                     return event.action == action && event.gameId == gameId;
                 });
    return events;
}

} // namespace

TEST_CASE("screens: 3 clips on 4 screens play on 2 decoders, two screens sharing one texture", "[gpu][video][screens]") {
    const std::filesystem::path dir = freshCaseDir("screens/four");
    const std::filesystem::path catalog = writeRoom(dir);
    const std::filesystem::path shot = dir / "four.png";
    const AppRun run = runScreens(dir, "four", catalog, 120, {"--screenshot", arg(shot)});
    const rv::app::VideoRecord& video = requireScreens(run, 2);
    INFO(run.describe());
    CHECK(video.maxOpenDecoders == 2);
    CHECK(video.decodersStarted == 2);
    CHECK(video.fallbacks.empty());
    // The hero's clip A on slot 0, shared with crt_b: one decoder, one texture.
    const rv::app::VideoScreenRecord& a = screenOf(video, "crt_a");
    const rv::app::VideoScreenRecord& b = screenOf(video, "crt_b");
    const rv::app::VideoScreenRecord& c = screenOf(video, "crt_c");
    const rv::app::VideoScreenRecord& d = screenOf(video, "crt_d");
    CHECK(a.kind == "hero");
    CHECK(a.screenClass == "room");
    CHECK(b.kind == "secondary");
    CHECK(b.screenClass == "ambient");
    CHECK(a.gameId == kGameA);
    CHECK(b.gameId == kGameA);
    CHECK(c.gameId == kGameB);
    CHECK(d.gameId == kGameC);
    for (const auto* screen : {&a, &b, &c}) {
        INFO(screen->screen);
        CHECK(screen->picture == "video");
        CHECK(screen->playing);
        CHECK(screen->visible);
    }
    CHECK(a.slot == std::optional<std::uint64_t>(0));
    CHECK(b.slot == std::optional<std::uint64_t>(0));
    CHECK(a.textureId != 0);
    CHECK(a.textureId == b.textureId);
    // B, nearer than C, on the second slot; C without a slot shows its cover.
    CHECK(c.slot == std::optional<std::uint64_t>(1));
    CHECK(c.textureId != a.textureId);
    CHECK_FALSE(d.slot.has_value());
    CHECK_FALSE(d.playing);
    CHECK(d.picture == "cover");
    CHECK(d.textureId != 0);
    CHECK(eventsOf(video, "open", kGameC).empty());
    // Two video textures in the renderer's accounting, the stills beside them.
    CHECK(run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::Video).bytes == video.videoBytes);
    CHECK(video.maxUploadBytes <= kUploadBudget);
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
}

TEST_CASE("screens: a screen turned away pauses within a frame, stops after 10 s and reopens where it stopped",
          "[gpu][video][screens]") {
    const std::filesystem::path dir = freshCaseDir("screens/turned");
    const std::filesystem::path catalog = writeRoom(dir);
    // Frame 60: 40 degrees to the left (crt_c and crt_d out of view); frame 720: back. F12 screenshots before the turn,
    // while turned, and after the return.
    constexpr int kTurn = 60;
    constexpr int kBack = 720;
    const std::filesystem::path script = writeScript(
        dir, "turn.json",
        Json::array({step(50, 50, {{"screenshot", 1}}), step(kTurn, kTurn, {{"look_delta_x", -kTurnPixels}}),
                     step(300, 300, {{"screenshot", 1}}), step(kBack, kBack, {{"look_delta_x", kTurnPixels}}),
                     step(kBack + 1, kBack + 1, {{"screenshot", 1}}), step(kBack + 50, kBack + 50, {{"screenshot", 1}})}));
    const AppRun run = runScreens(dir, "turned", catalog, kBack + 60, {"--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireScreens(run, 2);
    INFO(run.describe());
    // crt_c's slot (B) paused on the frame of the turn, its frame kept; the hero's (A, still in view) never.
    const auto pauses = eventsOf(video, "pause", kGameB);
    REQUIRE(pauses.size() == 1);
    CHECK(pauses.front().frame == static_cast<std::uint64_t>(kTurn));
    CHECK(pauses.front().slot == 1u);
    CHECK(eventsOf(video, "pause", kGameA).empty());
    // Stopped 10 s later (out of view), never taken by crt_d (out of view too).
    const auto stops = eventsOf(video, "stop", kGameB);
    REQUIRE_FALSE(stops.empty());
    CHECK(stops.front().reason == "out-of-view");
    const double outOfView = stops.front().atSeconds - pauses.front().atSeconds;
    INFO("stopped " << outOfView << " s after the pause");
    CHECK(outOfView >= 10.0 - 1e-6);
    CHECK(outOfView < 10.0 + 1.5 * kStep);
    CHECK(eventsOf(video, "open", kGameC).empty());
    // Back in view: B opens again at once, at its position.
    const auto opens = eventsOf(video, "open", kGameB);
    REQUIRE(opens.size() == 2);
    CHECK(opens.back().frame == static_cast<std::uint64_t>(kBack));
    CHECK(opens.back().reason == "wanted");
    const rv::app::VideoScreenRecord& c = screenOf(video, "crt_c");
    CHECK(c.playing);
    CHECK(c.picture == "video");
    CHECK(c.visible);
    CHECK(screenOf(video, "crt_a").picture == "video");
    CHECK(video.decodersStarted == 3);
    CHECK(video.maxOpenDecoders == 2);
}

TEST_CASE("screens: --max-decoders 1 plays the hero's clip alone, the others on their stills", "[gpu][video][screens]") {
    const std::filesystem::path dir = freshCaseDir("screens/one_decoder");
    const std::filesystem::path catalog = writeRoom(dir);
    const std::filesystem::path shot = dir / "one.png";
    const AppRun run = runScreens(dir, "one", catalog, 90, {"--max-decoders", "1", "--screenshot", arg(shot)});
    const rv::app::VideoRecord& video = requireScreens(run, 1);
    INFO(run.describe());
    CHECK(video.maxOpenDecoders == 1);
    CHECK(video.maxDecoderThreads == 1);
    CHECK(video.decodersStarted == 1);
    CHECK(screenOf(video, "crt_a").playing);
    CHECK(screenOf(video, "crt_b").playing); // the same clip
    for (const std::string_view id : {"crt_c", "crt_d"}) {
        INFO(id);
        const rv::app::VideoScreenRecord& screen = screenOf(video, id);
        CHECK_FALSE(screen.slot.has_value());
        CHECK(screen.picture == "cover");
    }
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
}

TEST_CASE("screens: minimized, the process goes back to normal and the slots pause at once and stop after 10 s",
          "[gpu][video][screens]") {
    const std::filesystem::path dir = freshCaseDir("screens/minimized");
    const std::filesystem::path catalog = writeRoom(dir);
    // After frame 30 the host takes the interactive run's minimized branch for 11 s (waits of 0.1 s counted under
    // --fixed-dt): no frame renders meanwhile, the window stays hidden and untouched.
    constexpr std::uint64_t kMinimized = 30;
    const AppRun run = runScreens(dir, "minimized", catalog, 90, {"--simulate-minimized", "30:11"});
    const rv::app::VideoRecord& video = requireScreens(run, 2);
    INFO(run.describe());
    CHECK(run.report->framesRendered == 90u);
    // Both slots (A on crt_a and crt_b, B on crt_c) paused on the first wait, stopped 10 s later, reopened after.
    for (const std::string_view game : {kGameA, kGameB}) {
        INFO("game " << game);
        const auto pauses = eventsOf(video, "pause", game);
        REQUIRE(pauses.size() == 1);
        CHECK(pauses.front().frame == kMinimized);
        CHECK(pauses.front().reason == "minimized");
        const auto stops = eventsOf(video, "stop", game);
        REQUIRE(stops.size() == 2); // minimized, then at the exit
        CHECK(stops.front().reason == "minimized");
        CHECK(stops.front().frame == kMinimized);
        CHECK(stops.back().reason == "shutdown");
        const double paused = stops.front().atSeconds - pauses.front().atSeconds;
        INFO("stopped " << paused << " s after the pause");
        CHECK(paused >= 10.0 - 1e-6);
        CHECK(paused < 10.1 + 1e-6);
        const auto opens = eventsOf(video, "open", game);
        REQUIRE(opens.size() == 2);
        CHECK(opens.back().frame >= kMinimized);
        CHECK(opens.back().atSeconds > stops.front().atSeconds);
    }
    for (const std::string_view id : {"crt_a", "crt_b", "crt_c"}) {
        INFO(id);
        CHECK(screenOf(video, id).playing);
        CHECK(screenOf(video, id).visible);
    }
    // The priority: above normal while rendering, normal while minimized, above normal again, normal at the exit.
    REQUIRE(run.report->processPriority.has_value());
    const std::vector<rv::app::PriorityChangeRecord>& changes = run.report->processPriority->changes;
    REQUIRE(changes.size() == 4);
    CHECK(changes[0].priority == "above-normal");
    CHECK(changes[1].priority == "normal");
    CHECK(changes[1].reason == "minimized");
    CHECK(changes[1].frame == kMinimized);
    CHECK(changes[2].priority == "above-normal");
    CHECK(changes[2].reason == "rendering");
    CHECK(changes[2].frame == kMinimized);
    CHECK(changes[3].reason == "exit");
    for (const rv::app::PriorityChangeRecord& change : changes) {
        CHECK(change.applied);
    }
}

TEST_CASE("screens: two 720p clips due together never upload more than 4 MiB in a frame", "[gpu][video][screens]") {
    const std::filesystem::path dir = freshCaseDir("screens/uploads");
    // 60 fps: both clips have a new frame due on every render frame; 720 rows (video.json's maxRows): 3.5 MiB each.
    writeClip(dir / "clips" / "x.mp4", 3.0, 91, {1280, 720}, 60);
    writeClip(dir / "clips" / "y.mp4", 3.0, 92, {1280, 720}, 60);
    writeScreens(dir, kGameA, kGameA, kGameB, kGameB, Json{{"maxRows", 720}});
    const std::filesystem::path catalog = writeCatalog(
        dir, Json::array({game(kGameA, "Arco di Pietra", {{"video", "clips/x.mp4"}}, "2026-10-01T10:00:00Z"),
                          game(kGameB, "Bussola d'Ottone", {{"video", "clips/y.mp4"}})}));
    const AppRun run = runScreens(dir, "uploads", catalog, 150);
    const rv::app::VideoRecord& video = requireScreens(run, 2);
    INFO(run.describe());
    constexpr std::uint64_t kFrameBytes = 1280ull * 720ull * 4ull;
    CHECK(video.maxOpenDecoders == 2);
    CHECK(screenOf(video, "crt_a").playing);
    CHECK(screenOf(video, "crt_c").playing);
    CHECK(screenOf(video, "crt_a").picture == "video");
    CHECK(screenOf(video, "crt_c").picture == "video");
    // One 720p frame a render frame: the other waits for the next.
    CHECK(video.maxUploadBytes == kFrameBytes);
    CHECK(video.maxUploadBytes <= kUploadBudget);
    CHECK(video.oversizeUploads == 0);
    CHECK(video.deferredUploads > 0);
    WARN("uploads: " << video.framesUploaded << " frame(s), " << video.deferredUploads << " put off a frame, at most "
                     << video.maxUploadBytes << " bytes in a frame; frame p99 "
                     << run.report->frameTimes.times.value_or(rv::app::TimeSummary{}).p99Ms << " ms");
}

TEST_CASE("screens: the process runs above normal while rendering and normal during the fake emulator's game",
          "[gpu][video][screens][launch]") {
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir("screens/priority");
    writeClip(dir / "clips" / "a.mp4", 4.0, 74);
    const std::filesystem::path catalog = writeFakeCatalog(dir, {fake_games::kReturns});
    {
        auto document = rv::core::readJsonFile(catalog);
        REQUIRE(document.has_value());
        (*document)["games"][0]["media"] = Json{{"video", "clips/a.mp4"}};
        REQUIRE(rv::core::writeTextFileAtomic(catalog, document->dump(2) + "\n").has_value());
    }
    // Frame 30 opens the catalog, frame 36 launches (the game runs 2.5 s), frames 42 and 48 leave the catalog.
    const std::filesystem::path script =
        writeScript(dir, "launch.json",
                    Json::array({step(30, 30, {{"quick_catalog", 1}}), step(36, 36, {{"ui_activate", 1}}),
                                 step(42, 42, {{"ui_back", 1}}), step(48, 48, {{"ui_back", 1}})}));
    const AppRun run = runScreens(dir, "priority", catalog, 90, {"--input-script", arg(script)});
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    const rv::app::LaunchRecord& launch = run.report->launches.front();
    CHECK(launch.state == "Returned");
    REQUIRE(launch.startFrame.has_value());
    REQUIRE(launch.endFrame.has_value());
    REQUIRE(run.report->processPriority.has_value());
    const std::vector<rv::app::PriorityChangeRecord>& changes = run.report->processPriority->changes;
    REQUIRE(changes.size() == 4);
    for (const rv::app::PriorityChangeRecord& change : changes) {
        INFO("frame " << change.frame << ": " << change.priority << " (" << change.reason << ")");
        CHECK(change.applied);
    }
    // Above normal from the first frame; normal before the emulator started, through its game; above normal again
    // from the first frame after the return; normal at the exit.
    CHECK(changes[0].frame <= 1u);
    CHECK(changes[0].priority == "above-normal");
    CHECK(changes[0].reason == "rendering");
    CHECK(changes[1].priority == "normal");
    CHECK(changes[1].reason == "launch");
    CHECK(changes[1].frame <= *launch.startFrame);
    CHECK(changes[2].priority == "above-normal");
    CHECK(changes[2].reason == "rendering");
    CHECK(changes[2].frame >= *launch.endFrame);
    CHECK(changes[3].priority == "normal");
    CHECK(changes[3].reason == "exit");
    // The decoders were stopped for the game, as with the hero alone.
    REQUIRE(run.report->video.launches.size() == 1);
    CHECK(run.report->video.launches.front().threadsRunning == 0);
    CHECK(run.report->video.budgetDrops.empty());
}

namespace {

/// The point of crt_<id>'s screen at its texture coordinates (u right, v down: uv (0, 0) its top-left), in the scene:
/// its prefab's ScreenSurface (the screen's rectangle, a little in front of its rim plane: the tube bulges) at the CRT's
/// position in screens_four.json.
glm::vec3 screenPoint(const rv::scene::SceneDescription& scene, std::string_view id, float u, float v) {
    const auto object = std::find_if(scene.objects.begin(), scene.objects.end(),
                                     [id](const rv::scene::SceneObject& candidate) { return candidate.id == id; });
    REQUIRE(object != scene.objects.end());
    auto crt = rv::geometry::buildCrt(rv::geometry::CrtParams{});
    REQUIRE(crt.has_value());
    const rv::geometry::Socket* surface = rv::geometry::findSocket(crt->sockets, "ScreenSurface");
    REQUIRE(surface != nullptr);
    const rv::core::Aabb& rect = surface->bounds;
    const glm::vec3 local(rect.min.x + u * (rect.max.x - rect.min.x), rect.max.y - v * (rect.max.y - rect.min.y),
                          0.5f * rect.max.z);
    const glm::mat4 model = object->instanceTransforms().front();
    return glm::vec3(model * surface->transform * glm::vec4(local, 1.0f));
}

/// The mean luma of the 7 x 7 window at each of `points` of crt_<id>'s screen (u, v) seen from the scene's camera.
std::vector<double> screenProbes(const rv::assets::ImageData& image, const rv::scene::SceneDescription& scene,
                                 std::string_view id, const std::vector<glm::vec2>& points) {
    std::vector<double> lumas;
    for (const glm::vec2& point : points) {
        const std::optional<glm::ivec2> pixel =
            rv::smoke::projectToPixel(scene.camera, kShotSize, screenPoint(scene, id, point.x, point.y));
        REQUIRE(pixel.has_value());
        lumas.push_back(rv::smoke::windowLuma(image, *pixel, 3));
    }
    return lumas;
}

} // namespace

TEST_CASE("screens: a 16:9 clip on a 4:3 tube shows black bars above and below, a portrait one at its sides",
          "[gpu][video][screens][aspect]") {
    const std::filesystem::path dir = freshCaseDir("screens/aspect");
    // crt_a and crt_c the 16:9 clip (1280 x 720, decoded at 480 rows), crt_b and crt_d a portrait 3:4 one (360 x 480);
    // frozen at 1.0 s so that the probes see one frame.
    writeClip(dir / "clips" / "wide.mp4", 3.0, 75, {1280, 720});
    writeClip(dir / "clips" / "tall.mp4", 3.0, 76, {360, 480});
    writeScreens(dir, kGameA, kGameB, kGameA, kGameB);
    const std::filesystem::path catalog = writeCatalog(
        dir, Json::array({game(kGameA, "Arco di Pietra", {{"video", "clips/wide.mp4"}}, "2026-10-01T10:00:00Z"),
                          game(kGameB, "Bussola d'Ottone", {{"video", "clips/tall.mp4"}})}));
    const std::filesystem::path shot = dir / "aspect.png";
    const AppRun run =
        runScreens(dir, "aspect", catalog, 20, {"--video-freeze-pts", "1", "--screenshot", arg(shot)});
    const rv::app::VideoRecord& video = requireScreens(run, 2);
    INFO(run.describe());
    // ADR-0016 §7: letterboxed (the whole width, 0.75 of the height) and pillarboxed (the whole height, 0.5625 of the
    // width), centred, never stretched.
    const auto rectOf = [&](std::string_view id) {
        const rv::app::VideoScreenRecord& screen = screenOf(video, id);
        CHECK(screen.picture == "video");
        REQUIRE(screen.pictureRect.size() == 4u);
        return screen.pictureRect;
    };
    const std::vector<double> wide = rectOf("crt_a");
    CHECK(wide[0] == 0.0);
    CHECK(std::abs(wide[1] - 0.125) < 0.002);
    CHECK(wide[2] == 1.0);
    CHECK(std::abs(wide[3] - 0.75) < 0.002);
    const std::vector<double> tall = rectOf("crt_b");
    CHECK(std::abs(tall[0] - 0.21875) < 0.002);
    CHECK(tall[1] == 0.0);
    CHECK(std::abs(tall[2] - 0.5625) < 0.002);
    CHECK(tall[3] == 1.0);
    CHECK(rectOf("crt_c") == wide);
    CHECK(rectOf("crt_d") == tall);
    // The probes: the bars are the lit tube's off-black (I5 step 4's review: crt.glsl's RV_CRT_BLACK_LEVEL, its glass's
    // faint sheen over it): dark, yet never a hole; the picture between them lit.
    const auto scene = rv::scene::loadSceneDescription(sceneFile());
    REQUIRE(scene.has_value());
    const rv::assets::ImageData image = requireScreenshot(run, shot, kShotSize);
    const std::vector<glm::vec2> letterbox{{0.3f, 0.05f}, {0.5f, 0.05f}, {0.7f, 0.05f},
                                           {0.3f, 0.95f}, {0.5f, 0.95f}, {0.7f, 0.95f}};
    const std::vector<glm::vec2> pillarbox{{0.07f, 0.4f}, {0.07f, 0.5f}, {0.07f, 0.6f},
                                           {0.93f, 0.4f}, {0.93f, 0.5f}, {0.93f, 0.6f}};
    const std::vector<glm::vec2> middle{{0.4f, 0.3f}, {0.5f, 0.3f}, {0.6f, 0.3f}, {0.4f, 0.5f}, {0.5f, 0.5f},
                                        {0.6f, 0.5f}, {0.4f, 0.7f}, {0.5f, 0.7f}, {0.6f, 0.7f}};
    for (const std::string_view id : {"crt_a", "crt_c"}) {
        const std::vector<double> bars = screenProbes(image, *scene, id, letterbox);
        const std::vector<double> picture = screenProbes(image, *scene, id, middle);
        const double darkest = *std::max_element(bars.begin(), bars.end());
        const double faintest = *std::min_element(bars.begin(), bars.end());
        double lit = 0.0;
        for (const double luma : picture) {
            lit += luma / static_cast<double>(picture.size());
        }
        WARN(id << ": bars " << faintest << " to " << darkest << " luma, the picture's mean " << lit);
        CHECK(darkest < 36.0);
        CHECK(faintest > 6.0);
        CHECK(lit > 3.0 * darkest);
    }
    for (const std::string_view id : {"crt_b", "crt_d"}) {
        const std::vector<double> bars = screenProbes(image, *scene, id, pillarbox);
        const std::vector<double> picture = screenProbes(image, *scene, id, middle);
        const double darkest = *std::max_element(bars.begin(), bars.end());
        const double faintest = *std::min_element(bars.begin(), bars.end());
        double lit = 0.0;
        for (const double luma : picture) {
            lit += luma / static_cast<double>(picture.size());
        }
        WARN(id << ": bars " << faintest << " to " << darkest << " luma, the picture's mean " << lit);
        CHECK(darkest < 36.0);
        CHECK(faintest > 6.0);
        CHECK(lit > 3.0 * darkest);
    }
}
