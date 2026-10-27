// The hero CRT's video (I3 step 11, ADR-0012 §7; since I5 step 1 its playlist, ADR-0016 §3): the test room's CRT
// playing generated clips (media::writeTestVideo, written into the case directory, never committed) of a mock
// catalog's games, through app::ScreenService. The run reports tell what the screen did (the video section: the one
// decoder, the clip's frames, the fallbacks, the launches, the games played and the pictures shown); the screenshots
// show it (--video-freeze-pts holds one frame, the same in two runs).

#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"

#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/media/test_video.hpp>
#include <rv/scene/scene_description.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rv::smoke;
using Json = nlohmann::json;

namespace {

/// The fixed step of the scripted runs.
constexpr std::string_view kFixedDt = "0.016667";
constexpr double kStep = 0.016667;
/// The clips: 4:3 at 30 fps like the fixtures' (I3 step 1c), small and short.
constexpr int kClipFps = 30;
/// Budgets (docs/roadmap.md, I3 "Budgets"): the first frame within 2 s of the selection, after the 300 ms dwell; a
/// frame within 33.3 ms; the video's texture and ring within 16 MiB.
constexpr double kFirstFrameBudgetMs = 2000.0 - 300.0;
constexpr double kFrameBudgetMs = 33.3;
constexpr std::uint64_t kVideoMemoryBudget = 16ull * 1024ull * 1024ull;

/// Writes a 640 x 480 MPEG-4 test clip (the frame number burned in) of `seconds`.
void writeClip(const std::filesystem::path& path, double seconds, std::uint32_t seed) {
    rv::media::TestVideoSpec spec;
    spec.width = 640;
    spec.height = 480;
    spec.frameRate = kClipFps;
    spec.durationSeconds = seconds;
    spec.seed = seed;
    spec.quantizer = 5;
    spec.keyFrameInterval = kClipFps;
    INFO("clip " << arg(path));
    REQUIRE(rv::media::writeTestVideo(path, spec).has_value());
}

/// Writes an invented cover picture (abstract shapes and `title`) as a PNG.
void writePicture(const std::filesystem::path& path, std::string_view title, std::uint32_t seed) {
    const rv::assets::ImageData picture = rv::assets::makeCoverPlaceholder(640, 480, title, seed);
    INFO("picture " << arg(path));
    REQUIRE(rv::assets::writePng(path, picture).has_value());
}

/// A game of the mock catalog with `media` (paths relative to the case directory, where the catalog is).
Json videoGame(std::string_view id, std::string_view title, Json media = Json::object(),
               std::optional<std::string_view> lastPlayed = std::nullopt) {
    Json game{{"id", id}, {"title", title}, {"platform", "Portatile 16 bit"}};
    if (!media.empty()) {
        game["media"] = std::move(media);
    }
    if (lastPlayed) {
        game["lastPlayed"] = *lastPlayed;
    }
    return game;
}

/// Writes <caseDir>/catalog.json with `games` and returns it.
std::filesystem::path writeCatalog(const std::filesystem::path& caseDir, Json games) {
    const Json catalog{{"schemaVersion", 1}, {"games", std::move(games)}};
    const std::filesystem::path file = caseDir / "catalog.json";
    REQUIRE(rv::core::writeTextFileAtomic(file, catalog.dump(2) + "\n").has_value());
    return file;
}

/// Writes an input script of `steps` into the case directory and returns it.
std::filesystem::path writeScript(const std::filesystem::path& caseDir, std::string_view name, Json steps) {
    const std::filesystem::path file = caseDir / rv::core::pathFromUtf8(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

Json step(int from, int to, Json actions) {
    return Json{{"fromFrame", from}, {"toFrame", to}, {"actions", std::move(actions)}};
}

/// A frame-limited, fixed-step run of the test room with `catalog`, `frames` long, and `extra` arguments.
AppRun runVideo(const std::filesystem::path& caseDir, std::string_view name, const std::filesystem::path& catalog,
                int frames, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--mock-catalog",       arg(catalog), "--exit-after-frames", "--frames",
                                       std::to_string(frames), "--fixed-dt", std::string(kFixedDt)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), caseDir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    return run;
}

/// The checks of every run with the hero CRT: a clean run, the screen bound, one decoder at most.
const rv::app::VideoRecord& requireVideo(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.screen == "crt_tv");
    CHECK(video.maxDecoderThreads <= 1);
    CHECK(video.joinedAtExit);
    return video;
}

const rv::app::CpuSectionRecord* section(const rv::app::RunReport& report, std::string_view name) {
    const auto found = std::find_if(report.cpuSections.begin(), report.cpuSections.end(),
                                    [name](const rv::app::CpuSectionRecord& record) { return record.name == name; });
    return found != report.cpuSections.end() ? &*found : nullptr;
}

/// Pixels where a channel differs by more than kMarkedDifference (the picture changed there; the screen's bloom moves
/// dim pixels around it by a few steps), their bounding box, and those outside the region [regionMin, regionMax]
/// (FXAA may flip a few edge pixels far from a change of luma).
struct Difference {
    std::size_t pixels = 0;
    std::size_t outside = 0;
    glm::ivec2 min{0};
    glm::ivec2 max{0};
};
constexpr int kMarkedDifference = 24;

Difference difference(const rv::assets::ImageData& a, const rv::assets::ImageData& b, glm::ivec2 regionMin,
                      glm::ivec2 regionMax) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(a.channels == b.channels);
    Difference result;
    result.min = glm::ivec2(a.width, a.height);
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(a.channels);
            int largest = 0;
            for (std::size_t c = 0; c < static_cast<std::size_t>(a.channels); ++c) {
                largest = std::max(largest, std::abs(int(a.pixels[at + c]) - int(b.pixels[at + c])));
            }
            if (largest > kMarkedDifference) {
                ++result.pixels;
                result.min = glm::min(result.min, glm::ivec2(x, y));
                result.max = glm::max(result.max, glm::ivec2(x, y));
                if (x < regionMin.x || y < regionMin.y || x > regionMax.x || y > regionMax.y) {
                    ++result.outside;
                }
            }
        }
    }
    return result;
}

constexpr glm::ivec2 kShotSize{960, 540};

/// Where the test room's CRT (its prefab's bounds: pivot on the floor, 0.71 x 0.61 x 0.56 m at (1.62, 0.7, -4.67),
/// assets/style_tests/reference_scenes/m0_testroom.json) appears in a kShotSize screenshot from the scene camera, with
/// a margin for the bloom its screen casts around it.
std::pair<glm::ivec2, glm::ivec2> crtRegion() {
    const auto scene = rv::scene::loadSceneDescription(sourceDir() / "assets" / "style_tests" / "reference_scenes" /
                                                       "m0_testroom.json");
    REQUIRE(scene.has_value());
    constexpr glm::vec3 kPivot{1.62f, 0.7f, -4.67f};
    constexpr glm::vec3 kHalf{0.355f, 0.0f, 0.28f};
    constexpr float kHeight = 0.61f;
    constexpr int kBloomMargin = 48;
    glm::ivec2 min(kShotSize);
    glm::ivec2 max(0);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point =
            kPivot + glm::vec3((corner & 1) != 0 ? kHalf.x : -kHalf.x, (corner & 2) != 0 ? kHeight : 0.0f,
                               (corner & 4) != 0 ? kHalf.z : -kHalf.z);
        const std::optional<glm::ivec2> pixel = projectToPixel(scene->camera, kShotSize, point);
        REQUIRE(pixel.has_value());
        min = glm::min(min, *pixel);
        max = glm::max(max, *pixel);
    }
    return {glm::max(min - kBloomMargin, glm::ivec2(0)), glm::min(max + kBloomMargin, kShotSize - 1)};
}

} // namespace

TEST_CASE("video: the hero CRT plays a generated clip at its rate, its memory accounted", "[gpu][video]") {
    const std::filesystem::path dir = freshCaseDir("video/plays");
    writeClip(dir / "clips" / "a.mp4", 4.0, 11);
    const std::filesystem::path catalog = writeCatalog(
        dir, Json::array({videoGame("g000000000000b001", "Lanterna di Vetro", {{"video", "clips/a.mp4"}})}));
    const AppRun run = runVideo(dir, "plays", catalog, 150);
    const rv::app::VideoRecord& video = requireVideo(run);
    INFO(run.describe());
    // The start choice (the only game with a video), never a selection.
    CHECK(video.gameId == "g000000000000b001");
    CHECK(video.picture == "video");
    CHECK(video.phase == "running");
    CHECK(video.decoderState == "playing");
    CHECK(video.targets == 1);
    CHECK(video.selections == 0);
    CHECK(video.decodersStarted == 1);
    CHECK(video.maxDecoderThreads == 1);
    CHECK(video.fallbacks.empty());
    CHECK(video.launches.empty());
    // NFR-06: the first frame within 2 s (from the decoder's start: the start choice has no dwell).
    REQUIRE(video.firstFrameMs.has_value());
    CHECK(*video.firstFrameMs < kFirstFrameBudgetMs);
    // Frames advance at the clip's rate: about 30 per simulated second (the clock runs on the run's simulated time),
    // their timestamps as far apart as the simulated time between their uploads.
    REQUIRE(video.firstPtsSeconds.has_value());
    REQUIRE(video.lastPtsSeconds.has_value());
    REQUIRE(video.clipUploadSeconds > 1.0);
    const double ptsSpan = *video.lastPtsSeconds - *video.firstPtsSeconds;
    const double rate = static_cast<double>(video.clipFramesUploaded - 1) / video.clipUploadSeconds;
    INFO("frames " << video.clipFramesUploaded << " over " << video.clipUploadSeconds << " s, timestamps "
                   << *video.firstPtsSeconds << " to " << *video.lastPtsSeconds << " s, " << rate << " fps");
    CHECK(ptsSpan / video.clipUploadSeconds > 0.85);
    CHECK(ptsSpan / video.clipUploadSeconds < 1.15);
    CHECK(rate > 0.7 * kClipFps);
    CHECK(rate < kClipFps + 2.0);
    CHECK(video.framesUploaded == video.clipFramesUploaded);
    // The video's texture and its pixel buffer ring, in the renderer's accounting (requireCleanRun: equal).
    CHECK(video.videoBytes > 0);
    CHECK(video.videoBytes <= kVideoMemoryBudget);
    CHECK(run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::Video).bytes == video.videoBytes);
    // The uploads are timed (the budget, p99 0.5 ms in Release, is measured on a quiet machine: informative here).
    const rv::app::CpuSectionRecord* upload = section(*run.report, "videoUpload");
    REQUIRE(upload != nullptr);
    CHECK(upload->times.count >= video.clipFramesUploaded);
    WARN("videoUpload p50 " << upload->times.p50Ms << " ms, p99 " << upload->times.p99Ms << " ms, max "
                            << upload->times.maxMs << " ms over " << upload->times.count << " frame(s); first frame "
                            << *video.firstFrameMs << " ms; frame p99 "
                            << run.report->frameTimes.times.value_or(rv::app::TimeSummary{}).p99Ms << " ms");
}

TEST_CASE("video: out of the view the decoder pauses, the picture kept", "[gpu][video]") {
    const std::filesystem::path dir = freshCaseDir("video/out_of_view");
    writeClip(dir / "clips" / "a.mp4", 4.0, 13);
    const std::filesystem::path catalog = writeCatalog(
        dir, Json::array({videoGame("g000000000000b001", "Lanterna di Vetro", {{"video", "clips/a.mp4"}})}));
    // From the entrance facing the hero wall, the walker turns around (frames 0 to 9) to face the doorway.
    const AppRun run = runVideo(dir, "turned", catalog, 40,
                                {"--spawn", "0,0,3.2,0,-6", "--input-script", arg(inputScript("turn_around.json"))});
    const rv::app::VideoRecord& video = requireVideo(run);
    INFO(run.describe());
    CHECK(video.picture == "video");
    CHECK(video.decoderState == "paused");
    CHECK(video.pauses == 1);
    CHECK(video.decodersStarted == 1);
    // Frames shown before the turn; none uploaded while paused beyond the one the clock held.
    CHECK(video.clipFramesUploaded >= 1);
    CHECK(video.clipFramesUploaded <= 10);
}

TEST_CASE("video: --video-freeze-pts holds one frame, the same in two runs", "[gpu][video]") {
    const std::filesystem::path dir = freshCaseDir("video/freeze");
    writeClip(dir / "clips" / "a.mp4", 4.0, 12);
    const std::filesystem::path catalog = writeCatalog(
        dir, Json::array({videoGame("g000000000000b001", "Lanterna di Vetro", {{"video", "clips/a.mp4"}})}));
    const auto frozenShot = [&](std::string_view name, std::string_view seconds) {
        const std::filesystem::path shot = dir / (std::string(name) + ".png");
        const AppRun run = runVideo(
            dir, name, catalog, 40,
            {"--video-freeze-pts", std::string(seconds), "--screenshot", arg(shot), "--screenshot-size", "960x540"});
        const rv::app::VideoRecord& video = requireVideo(run);
        INFO(run.describe());
        CHECK(video.picture == "video");
        REQUIRE(video.freezePtsSeconds.has_value());
        CHECK(*video.freezePtsSeconds == std::stod(std::string(seconds)));
        CHECK(video.frozenFrameShown);
        // One frame, the one showing that time, held.
        CHECK(video.clipFramesUploaded == 1);
        REQUIRE(video.firstPtsSeconds.has_value());
        CHECK(std::abs(*video.firstPtsSeconds - *video.freezePtsSeconds) < 1.0 / kClipFps);
        CHECK(video.lastPtsSeconds == video.firstPtsSeconds);
        CHECK(video.decoderState == "paused");
        return requireScreenshot(run, shot, kShotSize);
    };
    const rv::assets::ImageData first = frozenShot("frozen_a", "1.0");
    const rv::assets::ImageData second = frozenShot("frozen_b", "1.0");
    // Byte for byte the same picture (G08a's premise).
    CHECK(first.pixels == second.pixels);

    // Another time shows another frame, on the screen only.
    const rv::assets::ImageData later = frozenShot("frozen_later", "2.5");
    const auto [crtMin, crtMax] = crtRegion();
    INFO("the CRT and its glow in [" << crtMin.x << ", " << crtMin.y << "] - [" << crtMax.x << ", " << crtMax.y << "]");
    const Difference moved = difference(first, later, crtMin, crtMax);
    INFO("changed " << moved.pixels << " pixel(s) in [" << moved.min.x << ", " << moved.min.y << "] - [" << moved.max.x
                    << ", " << moved.max.y << "], " << moved.outside << " outside the CRT");
    CHECK(moved.pixels > 200);
    CHECK(moved.outside * 100 <= moved.pixels);

    // Without a game to show, the screen keeps its own picture (the colour bars), as golden G01 shows it: the video
    // replaced it, there only. (Since I5 step 1 a game without media shows its title card: no game at all here.)
    const std::filesystem::path idleCatalog = writeCatalog(dir, Json::array());
    const std::filesystem::path idleShot = dir / "idle.png";
    const AppRun idle =
        runVideo(dir, "idle", idleCatalog, 40,
                 {"--video-freeze-pts", "1.0", "--screenshot", arg(idleShot), "--screenshot-size", "960x540"});
    const rv::app::VideoRecord& idleVideo = requireVideo(idle);
    CHECK(idleVideo.picture == "idle-pattern");
    CHECK(idleVideo.gameId.empty());
    CHECK(idleVideo.decodersStarted == 0);
    CHECK(idleVideo.videoBytes == 0);
    const Difference shown = difference(requireScreenshot(idle, idleShot, kShotSize), first, crtMin, crtMax);
    INFO("changed " << shown.pixels << " pixel(s) in [" << shown.min.x << ", " << shown.min.y << "] - [" << shown.max.x
                    << ", " << shown.max.y << "], " << shown.outside << " outside the CRT");
    CHECK(shown.pixels > 1000);
    CHECK(shown.outside * 100 <= shown.pixels);
}

TEST_CASE("video: missing, empty, corrupt, truncated and unsupported files fall to the next picture", "[gpu][video]") {
    const std::filesystem::path dir = freshCaseDir("video/fallbacks");
    const std::filesystem::path clips = dir / "clips";
    const std::filesystem::path pictures = dir / "pictures";
    writeClip(clips / "clean.mp4", 2.0, 21);
    writeClip(clips / "clean.avi", 2.0, 22);
    REQUIRE(rv::media::writeRawFile(clips / "empty.mp4", "").has_value());
    REQUIRE(rv::media::writeCorruptedCopy(clips / "clean.mp4", clips / "corrupt.mp4", 7).has_value());
    REQUIRE(rv::media::writeTruncatedCopy(clips / "clean.mp4", clips / "truncated.mp4", 0.5).has_value());
    REQUIRE(rv::media::writeUnknownCodecCopy(clips / "clean.avi", clips / "unsupported.avi").has_value());
    REQUIRE(rv::media::writeRawFile(clips / "clip.flv", "FLV\x01").has_value());
    writePicture(pictures / "screenshot.png", "Schermata", 31);
    writePicture(pictures / "title.png", "Titolo", 32);
    writePicture(pictures / "cover.png", "Copertina", 33);
    REQUIRE(rv::media::writeRawFile(pictures / "not_an_image.png", "not an image\n").has_value());
    {
        auto bytes = rv::core::readFileShared(pictures / "cover.png", 64ull * 1024ull * 1024ull);
        REQUIRE(bytes.has_value());
        REQUIRE(
            rv::media::writeRawFile(pictures / "truncated.png", std::string_view(*bytes).substr(0, bytes->size() / 2))
                .has_value());
    }

    struct Case {
        std::string_view name;
        Json media;
        std::string_view picture;
        std::vector<std::pair<std::string_view, std::string_view>> fallbacks;
        std::uint64_t decodersStarted = 1;
        /// The game is selected in the quick catalog (a file ES-DE would not read is no start choice).
        bool select = false;
    };
    const std::vector<Case> cases{
        {"missing",
         {{"video", "clips/missing.mp4"}, {"cover", "pictures/cover.png"}},
         "cover",
         {{"video", "file-not-found"}}},
        {"empty",
         {{"video", "clips/empty.mp4"}, {"screenshot", "pictures/screenshot.png"}},
         "screenshot",
         {{"video", "invalid-file"}}},
        {"corrupt",
         {{"video", "clips/corrupt.mp4"}, {"screenshot", "pictures/absent.png"}, {"titlescreen", "pictures/title.png"}},
         "title-screen",
         {{"screenshot", "not-found"}, {"video", "corrupt-data"}}},
        {"truncated",
         {{"video", "clips/truncated.mp4"},
          {"screenshot", "pictures/not_an_image.png"},
          {"titlescreen", "pictures/truncated.png"},
          {"cover", "pictures/cover.png"}},
         "cover",
         {{"screenshot", "unsupported"}, {"title-screen", "parse-error"}, {"video", "truncated"}}},
        // I5 step 1: the chain ends at the game's title card (ADR-0016 §8), never the colour bars.
        {"unsupported_codec", {{"video", "clips/unsupported.avi"}}, "title-card", {{"video", "unsupported-codec"}}},
        {"not_playable",
         {{"video", "clips/clip.flv"}, {"cover", "pictures/cover.png"}},
         "cover",
         {{"video", "not-playable"}},
         0,
         true},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const std::string name(c.name);
            const std::filesystem::path catalog =
                writeCatalog(dir, Json::array({videoGame("g000000000000c001", "Ponte di Carta", c.media)}));
            std::vector<std::string> extra;
            if (c.select) {
                // The quick catalog opens on its first game, which becomes the CRT's selection after the dwell.
                extra = {"--input-script", arg(writeScript(dir, name + "_script.json",
                                                           Json::array({step(2, 2, {{"quick_catalog", 1}})})))};
            }
            const AppRun run = runVideo(dir, name, catalog, 180, extra);
            const rv::app::VideoRecord& video = requireVideo(run);
            INFO(run.describe());
            CHECK(video.gameId == "g000000000000c001");
            CHECK(video.picture == c.picture);
            CHECK(video.decodersStarted == c.decodersStarted);
            std::vector<std::pair<std::string, std::string>> fallbacks;
            for (const rv::app::VideoFallbackRecord& fallback : video.fallbacks) {
                CHECK(fallback.gameId == "g000000000000c001");
                fallbacks.emplace_back(fallback.picture, fallback.reason);
            }
            // The stills fail on the worker while the decoder opens: compared as a set.
            std::vector<std::pair<std::string, std::string>> expected;
            for (const auto& [picture, reason] : c.fallbacks) {
                expected.emplace_back(picture, reason);
            }
            std::sort(fallbacks.begin(), fallbacks.end());
            std::sort(expected.begin(), expected.end());
            CHECK(fallbacks == expected);
            CHECK(video.decoderState == "none"); // the failed decoder was stopped and its thread ended
            // A still shown is a hero texture of the renderer's accounting.
            CHECK(run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::HeroTextures).bytes == video.stillBytes);
            CHECK((video.stillBytes > 0) == (c.picture != "idle-pattern"));
            // The budget (no frame over 33.3 ms while falling back) is measured on a quiet machine in Release, without
            // --fixed-dt (whose lockstep waits for the stills): informative here.
            const rv::app::TimeSummary frames = run.report->frameTimes.times.value_or(rv::app::TimeSummary{});
            WARN(c.name << ": frame max " << frames.maxMs << " ms, p99 " << frames.p99Ms << " ms (budget "
                        << kFrameBudgetMs << " ms)");
        }
    }
}

TEST_CASE("video: a selection switches the clip after its dwell, stepping faster does not, never two decoders",
          "[gpu][video]") {
    const std::filesystem::path dir = freshCaseDir("video/selection");
    writeClip(dir / "clips" / "a.mp4", 4.0, 41);
    writeClip(dir / "clips" / "b.mp4", 4.0, 42);
    writeClip(dir / "clips" / "c.mp4", 4.0, 43);
    // C was played last: the start choice. The quick catalog lists A, B, C.
    const std::filesystem::path catalog = writeCatalog(
        dir,
        Json::array(
            {videoGame("g000000000000d001", "Arco di Pietra", {{"video", "clips/a.mp4"}}, "2026-09-01T10:00:00Z"),
             videoGame("g000000000000d002", "Bussola d'Ottone", {{"video", "clips/b.mp4"}}),
             videoGame("g000000000000d003", "Campana di Rame", {{"video", "clips/c.mp4"}}, "2026-10-01T10:00:00Z")}));
    // Frame 2: the catalog opens on A (selected after its dwell, frame 20); frame 40: B (frame 58); frames 70 and 74:
    // C, then back to B within 67 ms: no switch.
    const std::filesystem::path script =
        writeScript(dir, "select.json",
                    Json::array({step(2, 2, {{"quick_catalog", 1}}), step(40, 40, {{"ui_down", 1}}),
                                 step(70, 70, {{"ui_down", 1}}), step(74, 74, {{"ui_up", 1}})}));
    const AppRun run = runVideo(dir, "selection", catalog, 110, {"--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireVideo(run);
    INFO(run.describe());
    REQUIRE(run.report->catalog.has_value());
    CHECK(run.report->catalog->selectedId == "g000000000000d002");
    CHECK(video.gameId == "g000000000000d002");
    CHECK(video.picture == "video");
    CHECK(video.targets == 3); // C, A, B
    CHECK(video.selections == 2);
    CHECK(video.decodersStarted == 3);
    CHECK(video.maxDecoderThreads == 1);
    CHECK(video.fallbacks.empty());
    // NFR-06: B's first frame within 2 s of its selection (its dwell included).
    REQUIRE(video.firstFrameMs.has_value());
    CHECK(*video.firstFrameMs < kFirstFrameBudgetMs);
    WARN("first frame of the selected clip after " << *video.firstFrameMs << " ms (+ the 300 ms dwell)");
}

TEST_CASE("video: the decoder is stopped and joined before a launch and reopened at its position after the return",
          "[gpu][video][launch]") {
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir("video/launch");
    writeClip(dir / "clips" / "a.mp4", 4.0, 51);
    const std::filesystem::path catalog = writeFakeCatalog(dir, {fake_games::kReturns});
    {
        auto document = rv::core::readJsonFile(catalog);
        REQUIRE(document.has_value());
        (*document)["games"][0]["media"] = Json{{"video", "clips/a.mp4"}};
        REQUIRE(rv::core::writeTextFileAtomic(catalog, document->dump(2) + "\n").has_value());
    }
    // The clip plays from the start; frame 30 opens the catalog, frame 36 launches (the game runs 2.5 s), frames 42 and
    // 48 leave the catalog.
    const std::filesystem::path script =
        writeScript(dir, "launch.json",
                    Json::array({step(30, 30, {{"quick_catalog", 1}}), step(36, 36, {{"ui_activate", 1}}),
                                 step(42, 42, {{"ui_back", 1}}), step(48, 48, {{"ui_back", 1}})}));
    const AppRun run = runVideo(dir, "launch", catalog, 90, {"--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireVideo(run);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    CHECK(run.report->launches.front().state == "Returned");
    CHECK(run.report->launches.front().startFrame == 37u);

    REQUIRE(video.launches.size() == 1);
    const rv::app::VideoLaunchRecord& stop = video.launches.front();
    CHECK(stop.decoderRan);
    // Joined within the bound: no decoder thread ran once the hand-over began.
    CHECK(stop.threadsRunning == 0);
    CHECK(stop.stopMs <= 1000.0);
    // Stopped about 36 frames into the clip, reopened there.
    REQUIRE(stop.positionSeconds.has_value());
    CHECK(*stop.positionSeconds > 0.2);
    CHECK(*stop.positionSeconds < 36 * kStep + 0.1);
    CHECK(stop.reopened);
    CHECK(stop.reopenedAtSeconds == stop.positionSeconds);
    CHECK(video.decodersStarted == 2);
    CHECK(video.maxDecoderThreads == 1);
    CHECK(video.picture == "video");
    CHECK(video.decoderState == "playing");
    // The reopened clip's frames start where it stopped.
    REQUIRE(video.firstPtsSeconds.has_value());
    CHECK(*video.firstPtsSeconds >= *stop.positionSeconds - 1.5 / kClipFps);
    CHECK(*video.firstPtsSeconds < *stop.positionSeconds + 0.5);
    WARN("decoder stopped for the launch in " << stop.stopMs << " ms at " << *stop.positionSeconds << " s");
}

// --- I5 step 1 (ADR-0016 §3, §8): the playlist, the picture held between clips, the title card ----------------------

namespace {

/// The playlist's room: a game without media first (the quick catalog opens on it), then three games with 3 s clips,
/// in the order of tests/unit/app/test_screen_policy.cpp's playlistRoom(). video.json gives the hero 2 s a game.
std::filesystem::path writePlaylistRoom(const std::filesystem::path& dir) {
    writeClip(dir / "clips" / "e001.mp4", 3.0, 61);
    writeClip(dir / "clips" / "e002.mp4", 3.0, 62);
    writeClip(dir / "clips" / "e003.mp4", 3.0, 63);
    const std::filesystem::path config = dir / "data" / "config";
    std::filesystem::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", R"({"hero": {"previewDuration": 2}})"
                                                                   "\n")
                .has_value());
    Json silent = videoGame("g000000000000e009", "Acquerello Muto");
    silent["year"] = 1994;
    return writeCatalog(
        dir, Json::array({silent, videoGame("g000000000000e001", "Bussola d'Ottone", {{"video", "clips/e001.mp4"}}),
                          videoGame("g000000000000e002", "Campana di Rame", {{"video", "clips/e002.mp4"}}),
                          videoGame("g000000000000e003", "Duna di Vetro", {{"video", "clips/e003.mp4"}})}));
}

/// The F12 screenshots of a run (<data>/screenshots), in the order they were taken (their names).
std::vector<std::filesystem::path> takenScreenshots(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "data" / "screenshots", error)) {
        if (entry.path().extension() == ".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// The screen's own picture in the test room: the colour bars a run without a game shows, from the scene camera.
rv::assets::ImageData idleShot(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "idle");
    const std::filesystem::path catalog = writeCatalog(dir / "idle", Json::array());
    const std::filesystem::path shot = dir / "idle" / "idle.png";
    const AppRun idle =
        runVideo(dir / "idle", "idle", catalog, 20, {"--screenshot", arg(shot), "--screenshot-size", "960x540"});
    const rv::app::VideoRecord& video = requireVideo(idle);
    CHECK(video.picture == "idle-pattern");
    return requireScreenshot(idle, shot, kShotSize);
}

} // namespace

TEST_CASE("video: the hero screen plays its playlist, holding the last frame between clips, never its pattern",
          "[gpu][video][playlist]") {
    const std::filesystem::path dir = freshCaseDir("video/playlist");
    const std::filesystem::path catalog = writePlaylistRoom(dir);
    // F12 screenshots around the first two switches (frames 120 and 240, the policy's table), every other frame (a press
    // needs a release between).
    Json steps = Json::array();
    for (const int center : {120, 240}) {
        for (int frame = center - 2; frame <= center + 6; frame += 2) {
            steps.push_back(step(frame, frame, {{"screenshot", 1}}));
        }
    }
    const AppRun run = runVideo(dir, "playlist", catalog, 450,
                                {"--input-script", arg(writeScript(dir, "shots.json", steps)), "--screenshot-size",
                                 "960x540"});
    const rv::app::VideoRecord& video = requireVideo(run);
    INFO(run.describe());
    CHECK(video.playlistMode == "shuffle");
    CHECK(video.playlistRoom == "m0_testroom");
    CHECK(video.previewSeconds == 2.0);
    CHECK(video.playlistOrder ==
          std::vector<std::string>{"g000000000000e002", "g000000000000e003", "g000000000000e001"});
    // The start choice, then the playlist in the unit table's order (test_screen_policy.cpp: frames 0, 120, 240, 360),
    // 2 s each.
    const std::vector<std::pair<std::string, std::string>> expected{{"g000000000000e001", "start"},
                                                                    {"g000000000000e002", "playlist"},
                                                                    {"g000000000000e003", "playlist"},
                                                                    {"g000000000000e001", "playlist"}};
    std::vector<std::pair<std::string, std::string>> played;
    for (const rv::app::VideoPlayedRecord& entry : video.played) {
        played.emplace_back(entry.gameId, entry.origin);
    }
    CHECK(played == expected);
    REQUIRE(video.played.size() == expected.size());
    for (std::size_t i = 1; i < video.played.size(); ++i) {
        CHECK(std::abs(video.played[i].atSeconds - video.played[i - 1].atSeconds - 2.0) < 1.5 * kStep);
    }
    CHECK(video.decodersStarted == 4);
    CHECK(video.maxDecoderThreads == 1);
    CHECK(video.fallbacks.empty());
    // No frame of a switch shows the screen's own pattern: after the first clip's first frame, every picture shown is
    // a clip, and the frames between two clips held the last one's.
    std::size_t firstVideo = video.shown.size();
    for (std::size_t i = 0; i < video.shown.size(); ++i) {
        if (video.shown[i].picture == "video") {
            firstVideo = std::min(firstVideo, i);
        }
        if (i > firstVideo) {
            INFO("shown " << i << ": " << video.shown[i].gameId << " " << video.shown[i].picture);
            CHECK(video.shown[i].picture == "video");
        }
    }
    CHECK(video.shown.size() - firstVideo == 4);
    CHECK(video.heldFrames > 0);
    WARN("frames held between clips: " << video.heldFrames << "; frames of the pattern before the first frame: "
                                       << video.idleFramesWithGame);

    // The probes: each screenshot of a switch shows a picture on the tube, never the colour bars.
    const rv::assets::ImageData bars = idleShot(dir);
    const auto [crtMin, crtMax] = crtRegion();
    const std::vector<std::filesystem::path> shots = takenScreenshots(dir);
    REQUIRE(shots.size() == steps.size());
    for (const std::filesystem::path& file : shots) {
        INFO(arg(file));
        auto shot = rv::assets::loadImage(file);
        REQUIRE(shot.has_value());
        REQUIRE(shot->width == kShotSize.x);
        const Difference fromBars = difference(*shot, bars, crtMin, crtMax);
        INFO("differs from the colour bars in " << fromBars.pixels << " pixel(s)");
        CHECK(fromBars.pixels > 1000);
    }
}

TEST_CASE("video: a game without media shows its title card, and the playlist comes back after it",
          "[gpu][video][playlist]") {
    const std::filesystem::path dir = freshCaseDir("video/title_card");
    const std::filesystem::path catalog = writePlaylistRoom(dir);
    // The quick catalog opens on the game without media (selected after the dwell); closed at frame 60: its turn lasts
    // 2 s more (an F12 screenshot of its card at frame 120, the catalog gone), then the start choice it interrupted
    // plays again.
    const std::filesystem::path script =
        writeScript(dir, "card.json",
                    Json::array({step(2, 2, {{"quick_catalog", 1}}), step(60, 60, {{"ui_back", 1}}),
                                 step(120, 120, {{"screenshot", 1}})}));
    const AppRun run =
        runVideo(dir, "card", catalog, 240, {"--input-script", arg(script), "--screenshot-size", "960x540"});
    const rv::app::VideoRecord& video = requireVideo(run);
    INFO(run.describe());
    std::vector<std::pair<std::string, std::string>> played;
    for (const rv::app::VideoPlayedRecord& entry : video.played) {
        played.emplace_back(entry.gameId, entry.origin);
    }
    const std::vector<std::pair<std::string, std::string>> expected{
        {"g000000000000e001", "start"}, {"g000000000000e009", "selection"}, {"g000000000000e001", "start"}};
    CHECK(played == expected);
    REQUIRE(video.played.size() == 3);
    // Let go at frame 60: 2 s later (the first frame let go starts the hold).
    CHECK(std::abs(video.played[2].atSeconds - (61 * kStep + 2.0)) < 2.5 * kStep);
    CHECK(video.selections == 1);
    CHECK(video.fallbacks.empty()); // no media to fail: the card is the chain's whole
    const auto card = std::find_if(video.shown.begin(), video.shown.end(), [](const rv::app::VideoShownRecord& entry) {
        return entry.gameId == "g000000000000e009";
    });
    REQUIRE(card != video.shown.end());
    CHECK(card->picture == "title-card");
    for (const rv::app::VideoShownRecord& entry : video.shown) {
        CHECK((entry.picture != "idle-pattern" || &entry == &video.shown.front()));
    }
    CHECK(video.picture == "video");

    const std::vector<std::filesystem::path> shots = takenScreenshots(dir);
    REQUIRE(shots.size() == 1);
    auto shot = rv::assets::loadImage(shots.front());
    REQUIRE(shot.has_value());
    const auto [crtMin, crtMax] = crtRegion();
    const Difference fromBars = difference(*shot, idleShot(dir), crtMin, crtMax);
    INFO("the card differs from the colour bars in " << fromBars.pixels << " pixel(s)");
    CHECK(fromBars.pixels > 1000);
}
