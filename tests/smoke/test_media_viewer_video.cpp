// The media viewer's videos and manuals pages (I5 step 8, ADR-0016 §10) in the snes room of tests/fixtures/esde/snes-room
// (invented titles, generated media), planned and started at its doorway (--start-room), lockstep (--fixed-dt), hidden,
// the sound through the null output's offline mix (--audio-capture: never the system's audio). Before the import the
// case gives Amber Lantern (USA) a 1280 x 720 clip with a 1000 Hz tone and a manual (a few bytes named .pdf, never
// opened), and Cobalt Compass (USA) a clip with a 440 Hz tone, which video.json makes the hero CRT's playlist (it never
// follows a selection here). The quick catalog opens the viewer on Amber Lantern and pages to its clip:
//  - its frames advance on the page (the clip's frame counter read from F12 screenshots), and hold while it pauses;
//  - the world's decoders pause while the viewer is open (the hero's slot paused "viewer", no frame of the world
//    uploaded meanwhile) and play again once it closed;
//  - the offline mix holds the viewer's clip alone, at its full volume (its tone at the clip's level, the hero's 440 Hz
//    more than 40 dB below it), where the hero's was heard before;
//  - "Apri il manuale" is refused by the automation guard (RV_LAUNCH_ALLOWLIST lists the fake emulator alone): no
//    window can open.
// A damaged clip (Opal Oriel's unsupported codec, the world's clips off) shows the page's fallback with its reason: the
// decoder's on the first run, the verdict's on the next (the clip never opened again).

#include "building_support.hpp"
#include "smoke_support.hpp"
#include "support/scoped_environment.hpp"

#include <rv/assets/image_io.hpp>
#include <rv/audio/analysis.hpp>
#include <rv/audio/wav_file.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_frame.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

/// The window's size: the UI is laid out for it and drawn into the screenshots scaled uniformly (1920 x 1080).
constexpr double kWindowWidth = 1280.0;
constexpr int kRate = 48000;
constexpr double kViewerTone = 1000.0;
constexpr double kHeroTone = 440.0;
constexpr double kToneAmplitude = 0.5;

/// A clip with its frame counter and a 16-bit PCM tone (.mkv).
rv::media::TestVideoSpec toneClip(int width, int height, double seconds, double frequency) {
    rv::media::TestVideoSpec spec;
    spec.width = width;
    spec.height = height;
    spec.frameRate = 30;
    spec.durationSeconds = seconds;
    spec.quantizer = 5;
    spec.keyFrameInterval = 30;
    spec.audioSeconds = seconds;
    spec.audioSampleRate = 48000;
    spec.audioChannels = 1;
    spec.toneHz = frequency;
    spec.toneAmplitude = kToneAmplitude;
    return spec;
}

/// The snes room's fixture with Amber Lantern's 720p clip (1000 Hz) and its manual, Cobalt Compass's clip (440 Hz).
void prepareRoom(const rv::test::Fixture& fixture) {
    const fs::path videos = fixture.path("media/snes/videos");
    fs::remove(videos / "Amber Lantern (USA).mp4");
    fs::remove(videos / "Cobalt Compass (USA).mp4");
    REQUIRE(rv::media::writeTestVideo(videos / "Amber Lantern (USA).mkv", toneClip(1280, 720, 12.0, kViewerTone)));
    REQUIRE(rv::media::writeTestVideo(videos / "Cobalt Compass (USA).mkv", toneClip(640, 480, 12.0, kHeroTone)));
    const fs::path manuals = fixture.path("media/snes/manuals");
    fs::create_directories(manuals);
    REQUIRE(rv::media::writeRawFile(manuals / "Amber Lantern (USA).pdf", "%PDF-1.4\n% Retroverse test manual\n"));
}

std::string gameOf(const rv::app::RunReport& report, std::string_view file) {
    const auto library = rv::catalog::LibraryId::parse(report.world.library);
    REQUIRE(library.has_value());
    const auto key = rv::catalog::makeLookupKey(file);
    REQUIRE(key.has_value());
    const auto id = rv::catalog::stableGameId(*library, rv::catalog::SystemName::parse("snes").value(), key->key());
    REQUIRE(id.has_value());
    return id->toString();
}

void writeVideoJson(const PlannedBuilding& building, const Json& settings) {
    const fs::path config = building.data / "config";
    fs::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
}

Json step(int frame, Json actions) {
    return Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", std::move(actions)}};
}

Json typing(int frame, std::string_view text) {
    return Json{{"fromFrame", frame}, {"toFrame", frame}, {"text", text}};
}

fs::path writeScript(const PlannedBuilding& building, std::string_view name, const Json& steps) {
    const fs::path file = building.dir / (std::string(name) + ".json");
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

/// The quick catalog's steps that open the viewer on the game `search` finds, from `frame` on (the viewer opens at
/// frame + 16).
void openViewer(Json& steps, int frame, std::string_view search) {
    steps.push_back(step(frame, {{"quick_catalog", 1}}));
    steps.push_back(step(frame + 3, {{"ui_up", 1}}));
    steps.push_back(step(frame + 6, {{"ui_up", 1}}));
    steps.push_back(typing(frame + 9, search));
    steps.push_back(step(frame + 12, {{"ui_activate", 1}}));
    steps.push_back(step(frame + 16, {{"ui_media", 1}}));
}

AppRun runRoom(const PlannedBuilding& building, std::string_view name, int frames, std::vector<std::string> extra) {
    std::vector<std::string> arguments{"--start-room", building.roomOf("snes"), "--exit-after-frames", "--frames",
                                       std::to_string(frames), "--fixed-dt", "0.016667", "--audio", "null"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), building.dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    return run;
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

/// The clip's frame counter in `shot` within the viewer's video box (window pixels: x, y, width, height).
std::optional<std::uint32_t> counterIn(const fs::path& shot, const std::vector<double>& box) {
    auto image = rv::assets::loadImage(shot, 4);
    INFO(rv::core::pathToUtf8(shot));
    REQUIRE(image.has_value());
    REQUIRE(box.size() == 4);
    const double scale = static_cast<double>(image->width) / kWindowWidth;
    const int x0 = static_cast<int>(std::lround(box[0] * scale));
    const int y0 = static_cast<int>(std::lround(box[1] * scale));
    const int width = static_cast<int>(std::lround(box[2] * scale));
    const int height = static_cast<int>(std::lround(box[3] * scale));
    REQUIRE(x0 >= 0);
    REQUIRE(y0 >= 0);
    REQUIRE(x0 + width <= image->width);
    REQUIRE(y0 + height <= image->height);
    rv::media::VideoFrame frame;
    REQUIRE(frame.resize(rv::media::FrameSize{width, height}));
    for (int y = 0; y < height; ++y) {
        const std::size_t from = (static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(image->width) +
                                  static_cast<std::size_t>(x0)) *
                                 4u;
        std::copy_n(image->pixels.begin() + static_cast<std::ptrdiff_t>(from), static_cast<std::size_t>(width) * 4u,
                    frame.pixels() + static_cast<std::size_t>(y) * frame.strideBytes());
    }
    return rv::media::decodeFrameCounter(frame);
}

/// The events of the screens record of `kind`, as "<frame>:<kind>:<detail>".
std::vector<std::string> eventsOf(const rv::app::GameScreensRecord& screens, std::string_view event) {
    std::vector<std::string> found;
    for (const rv::app::GameScreenEventRecord& entry : screens.events) {
        if (entry.event == event) {
            found.push_back(std::to_string(entry.frame) + ":" + entry.kind + ":" + entry.detail);
        }
    }
    return found;
}

/// The capture's samples from `from` to `to` seconds of the run, one channel.
std::vector<float> window(const rv::audio::WavData& wav, double captureStart, double from, double to, int channel) {
    const auto first = static_cast<std::size_t>(std::max(0.0, (from - captureStart) * kRate));
    const auto last = std::min(wav.frames(), static_cast<std::size_t>(std::max(0.0, (to - captureStart) * kRate)));
    std::vector<float> samples;
    for (std::size_t frame = first; frame < last; ++frame) {
        samples.push_back(wav.samples[frame * 2 + static_cast<std::size_t>(channel)]);
    }
    return samples;
}

/// A tone's amplitude over both channels: sqrt((L^2 + R^2) / 2) of each channel's Goertzel amplitude.
double stereoAmplitude(const rv::audio::WavData& wav, double captureStart, double from, double to, double frequency) {
    const double left = rv::audio::toneAmplitude(window(wav, captureStart, from, to, 0), frequency, kRate);
    const double right = rv::audio::toneAmplitude(window(wav, captureStart, from, to, 1), frequency, kRate);
    return std::sqrt((left * left + right * right) / 2.0);
}

/// The slot events of the run's video record for `screen` with `action`, as "<frame>:<reason>".
std::vector<std::string> slotEventsOf(const rv::app::VideoRecord& video, std::string_view screen,
                                      std::string_view action) {
    std::vector<std::string> found;
    for (const rv::app::VideoSlotEventRecord& event : video.slotEvents) {
        if (event.action == action && std::find(event.screens.begin(), event.screens.end(), screen) != event.screens.end()) {
            found.push_back(std::to_string(event.frame) + ":" + event.reason);
        }
    }
    return found;
}

} // namespace

TEST_CASE("media viewer: the videos page plays its clip with its sound, the world paused under it; the manual is "
          "refused under the allowlist",
          "[gpu][world][media_viewer]") {
    // Nothing here may open a window: the guard must be on, naming the fake emulator alone (tests/smoke/CMakeLists.txt).
    const std::optional<std::wstring> allowList = rv::test::environmentVariable(L"RV_LAUNCH_ALLOWLIST");
    REQUIRE(allowList.has_value());
    REQUIRE(allowList->find(L"rv_fake_emulator.exe") != std::wstring::npos);
    REQUIRE(allowList->find(L"explorer") == std::wstring::npos);

    const PlannedBuilding building("media_viewer/videos", true, "esde/snes-room", prepareRoom);
    const AppRun first = runRoom(building, "first", 2, {});
    const std::string amber = gameOf(*first.report, "Amber Lantern (USA).sfc");
    const std::string cobalt = gameOf(*first.report, "Cobalt Compass (USA).sfc");
    // The hero CRT: Cobalt Compass after a 1 s start choice, never a selection.
    writeVideoJson(building, Json{{"rooms",
                                   {{building.roomOf("snes"),
                                     {{"hero",
                                       {{"playlist", "explicit"},
                                        {"games", Json::array({cobalt})},
                                        {"previewDuration", 1},
                                        {"showOnFocus", false}}}}}}}});

    // Frame 166: the viewer from the quick catalog on Amber Lantern; frames 170 to 185 to its clip (7 of 10).
    Json steps = Json::array();
    openViewer(steps, 150, "amber");
    for (int frame = 170; frame <= 185; frame += 3) {
        steps.push_back(step(frame, {{"ui_right", 1}}));
    }
    steps.push_back(step(230, {{"screenshot", 1}}));
    steps.push_back(step(290, {{"screenshot", 1}}));
    steps.push_back(step(300, {{"ui_activate", 1}})); // pause
    steps.push_back(step(330, {{"screenshot", 1}}));
    steps.push_back(step(350, {{"screenshot", 1}}));
    steps.push_back(step(360, {{"ui_activate", 1}})); // plays again
    steps.push_back(step(390, {{"screenshot", 1}}));
    steps.push_back(step(400, {{"ui_end", 1}}));      // the manual (10 of 10)
    steps.push_back(step(405, {{"ui_activate", 1}})); // Apri il manuale: refused
    steps.push_back(step(412, {{"screenshot", 1}}));
    steps.push_back(step(420, {{"ui_back", 1}})); // the viewer closes, the quick catalog stays
    const fs::path script = writeScript(building, "viewer", steps);
    const fs::path mix = building.dir / "mix.wav";
    const AppRun run = runRoom(building, "viewer", 470,
                               {"--input-script", arg(script), "--audio-capture", arg(mix)});
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::GameScreensRecord& screens = report.screens;
    const rv::app::VideoRecord& video = report.video;
    REQUIRE(screens.bound);
    REQUIRE(video.bound);

    // The pages: the clip after the title screen, the manual last.
    const std::vector<std::string> media = eventsOf(screens, "media");
    REQUIRE(media.size() == 1);
    CHECK(media.front() ==
          "166::covers,backcovers,physicalmedia,3dboxes,screenshots,titlescreens,videos,marquees,fanart,manuals");
    const std::vector<std::string> pages = eventsOf(screens, "page");
    CHECK(pages == std::vector<std::string>{"166:covers:1", "170:backcovers:2", "173:physicalmedia:3",
                                            "176:3dboxes:4", "179:screenshots:5", "182:titlescreens:6",
                                            "185:videos:7", "400:manuals:10"});

    // The clip: at 720 rows, its own texture, paused once and played again; the page's states in order.
    CHECK(video.viewer.clips == 1);
    CHECK(video.viewer.decodersOpened == 1);
    CHECK(video.viewer.rows == 720);
    CHECK(video.viewer.width == 1280);
    CHECK(video.viewer.height == 720);
    CHECK(video.viewer.pauses == 1);
    CHECK(video.viewer.failures == 0);
    CHECK(video.viewer.knownBad == 0);
    CHECK(video.viewer.game == amber);
    CHECK(video.viewer.framesUploaded > 60); // about 2.6 s of the clip at 30 fps, the pause aside
    const std::vector<std::string> states = eventsOf(screens, "video");
    REQUIRE(states.size() >= 4);
    CHECK(states[0].starts_with("185:videos:"));
    CHECK(std::find(states.begin(), states.end(), "300:videos:paused") != states.end());
    CHECK(std::count_if(states.begin(), states.end(),
                        [](const std::string& state) { return state.ends_with("playing 1280x720"); }) == 2);
    CHECK(screens.videoFrames > 150);
    REQUIRE(screens.videoBox.size() == 4);
    // A 16:9 clip fitted to the stage without stretching.
    CHECK(std::abs(screens.videoBox[2] / screens.videoBox[3] - 16.0 / 9.0) < 0.02);
    // The GPU memory of the viewer's clip: its 720p texture and pixel buffer ring (ADR-0016 §12: about 14 MiB).
    CHECK(video.viewer.maxGpuBytes >= 4ull * 1280ull * 720ull * 4ull);
    CHECK(video.viewer.maxGpuBytes <= 16ull * 1024ull * 1024ull);
    WARN("viewer clip: " << video.viewer.maxGpuBytes / (1024.0 * 1024.0) << " MiB at most; video peak "
                         << report.gpuMemory.usage(rv::gl::GpuMemoryKind::Video).peakBytes / (1024.0 * 1024.0)
                         << " MiB; all GPU memory peak " << report.gpuMemory.peakTotalBytes / (1024.0 * 1024.0)
                         << " MiB");

    // Frames advance on the page: the counter of the clip in the screenshots, 1 s apart at 30 fps; held while paused.
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 6);
    const std::optional<std::uint32_t> at230 = counterIn(shots[0], screens.videoBox);
    const std::optional<std::uint32_t> at290 = counterIn(shots[1], screens.videoBox);
    const std::optional<std::uint32_t> at330 = counterIn(shots[2], screens.videoBox);
    const std::optional<std::uint32_t> at350 = counterIn(shots[3], screens.videoBox);
    const std::optional<std::uint32_t> at390 = counterIn(shots[4], screens.videoBox);
    REQUIRE(at230.has_value());
    REQUIRE(at290.has_value());
    REQUIRE(at330.has_value());
    REQUIRE(at350.has_value());
    REQUIRE(at390.has_value());
    WARN("the clip's counter at frames 230, 290, 330 (paused), 350 (paused), 390: "
         << *at230 << ", " << *at290 << ", " << *at330 << ", " << *at350 << ", " << *at390);
    CHECK(*at230 > 0);
    CHECK(*at290 >= *at230 + 27);
    CHECK(*at290 <= *at230 + 33);
    CHECK(*at330 == *at350);
    CHECK(*at330 >= *at290 + 3);
    CHECK(*at330 <= *at290 + 7);
    CHECK(*at390 >= *at350 + 12);
    CHECK(*at390 <= *at350 + 18);

    // The world paused under the viewer: the hero's slot paused as it opened, nothing of the world uploaded while it was
    // open, and the hero's clip playing again once it closed.
    CHECK(video.viewer.worldFramesWhileOpen == 0);
    CHECK(video.viewer.openFrames >= 250);
    const std::vector<std::string> pauses = slotEventsOf(video, video.screen, "pause");
    CHECK(std::find(pauses.begin(), pauses.end(), "166:viewer") != pauses.end());
    const std::vector<std::string> resumes = slotEventsOf(video, video.screen, "resume");
    CHECK(std::find(resumes.begin(), resumes.end(), "420:in-view") != resumes.end());
    CHECK(video.maxOpenDecoders <= 2);
    CHECK(video.maxDecoderThreads <= 2);

    // The mix: the hero's tone heard before; under the viewer only the viewer's clip, at its full volume; silence while
    // it is paused.
    REQUIRE(report.audio.has_value());
    const rv::app::AudioRecord& audio = *report.audio;
    CHECK(audio.device == "null");
    REQUIRE(audio.captureStartSeconds.has_value());
    auto wav = rv::audio::readWavFile(*audio.capture);
    REQUIRE(wav.has_value());
    const double start = *audio.captureStartSeconds;
    bool heroHeard = false;
    bool viewerHeard = false;
    for (const rv::app::AudioAudibleRecord& audible : audio.audible) {
        heroHeard = heroHeard || (audible.screen == video.screen && audible.gameId == cobalt && audible.frame < 150);
        viewerHeard = viewerHeard || (audible.screen == "media-viewer" && audible.gameId == amber);
    }
    CHECK(heroHeard);
    CHECK(viewerHeard);
    const double heroBefore = stereoAmplitude(*wav, start, 1.6, 2.4, kHeroTone);
    const double viewerTone = stereoAmplitude(*wav, start, 3.9, 4.9, kViewerTone);
    const double heroUnder = stereoAmplitude(*wav, start, 3.9, 4.9, kHeroTone);
    const double pausedTone = stereoAmplitude(*wav, start, 5.4, 5.9, kViewerTone);
    WARN("the hero's tone before " << heroBefore << ", the viewer's " << viewerTone << ", the hero's under it "
                                   << heroUnder << ", the viewer's paused " << pausedTone);
    CHECK(heroBefore > 0.02);
    CHECK(std::abs(20.0 * std::log10(viewerTone / kToneAmplitude)) < 1.0); // full volume, no duck, no spatial loss
    CHECK(heroUnder < viewerTone * 0.01);                                  // more than 40 dB below
    CHECK(pausedTone < viewerTone * 0.01);

    // "Apri il manuale": its file found, explorer.exe refused by the automation guard (nothing started).
    const std::vector<std::string> manual = eventsOf(screens, "manual");
    REQUIRE(manual.size() == 2);
    CHECK(manual[0].ends_with(":manuals:found Amber Lantern (USA).pdf"));
    CHECK(manual[1] == "405:manuals:refused");
    auto log = rv::core::readTextFile(run.outputFile);
    REQUIRE(log.has_value());
    CHECK(log->find("the automation guard refused explorer.exe") != std::string::npos);
    CHECK(eventsOf(screens, "closed") == std::vector<std::string>{"420::media"});
    REQUIRE(report.catalog.has_value());
    CHECK(report.catalog->open);
    CHECK(report.launches.empty());
}

TEST_CASE("media viewer: a damaged clip shows the page's fallback with its reason, from its verdict once known",
          "[gpu][world][media_viewer]") {
    const PlannedBuilding building("media_viewer/damaged", true, "esde/snes-room");
    // The world's clips off: only the viewer opens Opal Oriel's (an .avi whose codec no decoder knows).
    writeVideoJson(building, Json{{"enabled", false}});
    Json steps = Json::array();
    openViewer(steps, 40, "opal");
    steps.push_back(step(60, {{"ui_right", 1}})); // its clip (2 of 2)
    const fs::path script = writeScript(building, "damaged", steps);

    // The first run opens it: the decoder fails, the page says why, the verdict is kept.
    const fs::path firstShot = building.dir / "damaged_first.png";
    const AppRun first = runRoom(building, "damaged_first", 90,
                                 {"--input-script", arg(script), "--screenshot", arg(firstShot)});
    INFO(first.describe());
    const std::string opal = gameOf(*first.report, "Opal Oriel (Europe).sfc");
    CHECK(eventsOf(first.report->screens, "media") == std::vector<std::string>{"56::covers,videos"});
    const std::vector<std::string> failed = eventsOf(first.report->screens, "video");
    REQUIRE_FALSE(failed.empty());
    CHECK(failed.back().ends_with(":videos:failed unsupported-codec"));
    CHECK(first.report->video.viewer.decodersOpened == 1);
    CHECK(first.report->video.viewer.failures == 1);
    CHECK(first.report->video.viewer.state == "failed");
    CHECK(first.report->video.viewer.reason == "unsupported-codec");
    CHECK(first.report->video.viewer.game == opal);
    REQUIRE(first.report->video.verdicts.given.size() == 1);
    CHECK(first.report->video.verdicts.given.front().gameId == opal);
    CHECK(first.report->video.verdicts.given.front().verdict == "unsupported-codec");
    CHECK(first.report->screens.videoFrames == 0);
    static_cast<void>(requireScreenshot(first, firstShot, {1920, 1080}));

    // The next run never opens it: the verdict's reason from the page's first frame.
    const fs::path secondShot = building.dir / "damaged_known.png";
    const AppRun second = runRoom(building, "damaged_known", 90,
                                  {"--input-script", arg(script), "--screenshot", arg(secondShot)});
    INFO(second.describe());
    CHECK(eventsOf(second.report->screens, "video") == std::vector<std::string>{"60:videos:verdict unsupported-codec"});
    CHECK(second.report->video.viewer.decodersOpened == 0);
    CHECK(second.report->video.viewer.knownBad == 1);
    CHECK(second.report->video.viewer.reason == "unsupported-codec");
    CHECK(second.report->video.decodersStarted == 0);
    static_cast<void>(requireScreenshot(second, secondShot, {1920, 1080}));
}
