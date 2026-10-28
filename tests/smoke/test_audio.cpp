// The clips' sound (I5 step 5, ADR-0016 §9) on tests/smoke/scenes/screens_four.json's four CRTs, through the null
// output's offline mix (--audio-capture: the run's mix as a WAV, never the system's audio): generated clips
// (media::writeTestVideo, written into the case directory, never committed) carrying tones and beeps whose frames
// flash. The run reports tell when each frame of the audible clip showed (audio.presented), which screen was heard
// (audio.audible) and its levels (audio.levels).

#include "smoke_support.hpp"

#include <rv/audio/analysis.hpp>
#include <rv/audio/spatial.hpp>
#include <rv/audio/wav_file.hpp>
#include <rv/core/file_io.hpp>
#include <rv/media/test_video.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rv::smoke;
using Catch::Approx;
using Json = nlohmann::json;

namespace {

constexpr std::string_view kFixedDt = "0.016667";
constexpr std::string_view kGameA = "g000000000000a501";
constexpr std::string_view kGameB = "g000000000000a502";
constexpr int kRate = 48000;
/// Turning by this many mouse pixels at 0.0022 rad each turns the walker 40 degrees.
constexpr double kTurnPixels = 317.3;

std::filesystem::path sceneFile() {
    return sourceDir() / "tests" / "smoke" / "scenes" / "screens_four.json";
}

/// A 640 x 480 clip with a 16-bit PCM tone (.mkv).
rv::media::TestVideoSpec toneClip(double seconds, double frequency, int audioRate, int fps = 30) {
    rv::media::TestVideoSpec spec;
    spec.width = 640;
    spec.height = 480;
    spec.frameRate = fps;
    spec.durationSeconds = seconds;
    spec.quantizer = 5;
    spec.keyFrameInterval = fps;
    spec.audioSeconds = seconds;
    spec.audioSampleRate = audioRate;
    spec.audioChannels = 1;
    spec.toneHz = frequency;
    spec.toneAmplitude = 0.5;
    return spec;
}

void writeClip(const std::filesystem::path& path, const rv::media::TestVideoSpec& spec) {
    INFO("clip " << arg(path));
    REQUIRE(rv::media::writeTestVideo(path, spec).has_value());
}

Json game(std::string_view id, std::string_view title, std::string_view clip) {
    return Json{{"id", id}, {"title", title}, {"platform", "Portatile 16 bit"}, {"media", {{"video", clip}}}};
}

/// The catalog and video.json: crt_a plays `a`, crt_b and crt_d play `a` too (one slot), crt_c plays `c`; `extra`
/// global keys.
std::filesystem::path writeRoom(const std::filesystem::path& dir, Json games, std::string_view a, std::string_view c,
                                Json extra = Json::object()) {
    const auto one = [](std::string_view id) {
        return Json{{"playlist", "explicit"}, {"games", Json::array({id})}};
    };
    Json settings = std::move(extra);
    settings["rooms"] = Json{{"screens_four",
                              {{"hero", one(a)},
                               {"screens", {{"crt_b", one(a)}, {"crt_c", one(c)}, {"crt_d", one(a)}}}}}};
    const std::filesystem::path config = dir / "data" / "config";
    std::filesystem::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
    const Json catalog{{"schemaVersion", 1}, {"games", std::move(games)}};
    const std::filesystem::path file = dir / "catalog.json";
    REQUIRE(rv::core::writeTextFileAtomic(file, catalog.dump(2) + "\n").has_value());
    return file;
}

std::filesystem::path writeScript(const std::filesystem::path& dir, Json steps) {
    const std::filesystem::path file = dir / "script.json";
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

Json step(int from, int to, Json actions) {
    return Json{{"fromFrame", from}, {"toFrame", to}, {"actions", std::move(actions)}};
}

/// A fixed-step run of the four-CRT room with `catalog`, `frames` long, its mix captured.
AppRun runAudio(const std::filesystem::path& dir, const std::filesystem::path& catalog, int frames,
                std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--scene",      arg(sceneFile()),      "--mock-catalog",    arg(catalog),
                                       "--exit-after-frames", "--frames", std::to_string(frames),
                                       "--fixed-dt",   std::string(kFixedDt), "--screenshot-size", "640x360",
                                       "--audio",      "null",                "--audio-capture",   arg(dir / "mix.wav")};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), dir, "audio", std::move(arguments));
    skipWithoutOpenGl(run);
    return run;
}

/// The run's checks and its capture.
rv::audio::WavData requireMix(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->audio.has_value());
    const rv::app::AudioRecord& audio = *run.report->audio;
    CHECK(audio.device == "null");
    CHECK(audio.sampleRate == static_cast<std::uint64_t>(kRate));
    REQUIRE(audio.capture.has_value());
    REQUIRE(audio.captureStartSeconds.has_value());
    auto wav = rv::audio::readWavFile(*audio.capture);
    REQUIRE(wav.has_value());
    CHECK(wav->sampleRate == kRate);
    CHECK(wav->channels == 2);
    CHECK(wav->frames() == audio.framesMixed);
    CHECK(rv::audio::peakOf(wav->samples) <= 1.0);
    return std::move(*wav);
}

/// The frames of the capture from `from` to `to` seconds of the run, one channel.
std::vector<float> window(const rv::audio::WavData& wav, double captureStart, double from, double to, int channel) {
    const auto first = static_cast<std::size_t>(std::max(0.0, (from - captureStart) * kRate));
    const auto last = std::min(wav.frames(), static_cast<std::size_t>(std::max(0.0, (to - captureStart) * kRate)));
    std::vector<float> samples;
    for (std::size_t frame = first; frame < last; ++frame) {
        samples.push_back(wav.samples[frame * 2 + static_cast<std::size_t>(channel)]);
    }
    return samples;
}

/// The tone's amplitude over both channels, pan aside: sqrt((L^2 + R^2) / 2) of each channel's Goertzel amplitude.
double stereoAmplitude(const rv::audio::WavData& wav, double captureStart, double from, double to, double frequency) {
    const double left = rv::audio::toneAmplitude(window(wav, captureStart, from, to, 0), frequency, kRate);
    const double right = rv::audio::toneAmplitude(window(wav, captureStart, from, to, 1), frequency, kRate);
    return std::sqrt((left * left + right * right) / 2.0);
}

} // namespace

TEST_CASE("audio: a clip's beeps are heard within 20 ms of its flashes (the offline mix)", "[gpu][audio]") {
    const std::filesystem::path dir = freshCaseDir("audio/sync");
    rv::media::TestVideoSpec spec = toneClip(6.0, 1000.0, 44100);
    spec.beepPeriodSeconds = 1.0;
    spec.beepFirstSeconds = 0.5;
    spec.beepSeconds = 0.1;
    writeClip(dir / "clips" / "beeps.mkv", spec);
    const std::filesystem::path catalog =
        writeRoom(dir, Json::array({game(kGameA, "Arco di Pietra", "clips/beeps.mkv")}), kGameA, kGameA);
    const AppRun run = runAudio(dir, catalog, 300);
    const rv::audio::WavData wav = requireMix(run);
    INFO(run.describe());
    const rv::app::AudioRecord& audio = *run.report->audio;
    REQUIRE_FALSE(audio.audible.empty());
    CHECK(audio.audible.back().screen == "crt_a");
    CHECK(audio.audible.back().gameId == kGameA);
    REQUIRE(audio.clips.size() == 1);
    CHECK(audio.clips.front().sampleRate == 44100u);
    REQUIRE_FALSE(audio.presented.empty());
    // Each flash: when its first frame showed (the run's time).
    std::vector<double> flashes;
    for (const double beep : rv::media::testBeepTimes(spec)) {
        const auto shown = std::find_if(audio.presented.begin(), audio.presented.end(),
                                        [&](const rv::app::AudioPresentedRecord& frame) {
                                            return frame.ptsSeconds >= beep - 1e-4 && frame.ptsSeconds < beep + 0.05;
                                        });
        if (shown != audio.presented.end()) {
            flashes.push_back(shown->atSeconds);
        }
    }
    REQUIRE(flashes.size() >= 3);
    // Each beep: where it starts in the mix (both channels carry it; the louder one is read).
    const std::vector<float> left = window(wav, *audio.captureStartSeconds, 0.0, 1e9, 0);
    const std::vector<float> right = window(wav, *audio.captureStartSeconds, 0.0, 1e9, 1);
    const std::vector<float>& louder = rv::audio::peakOf(left) >= rv::audio::peakOf(right) ? left : right;
    const double threshold = 0.1 * rv::audio::peakOf(louder);
    REQUIRE(threshold > 0.005);
    const std::vector<double> beeps = rv::audio::onsets(louder, kRate, threshold, 0.5);
    REQUIRE(beeps.size() >= flashes.size());
    for (std::size_t index = 0; index < flashes.size(); ++index) {
        const double heard = *audio.captureStartSeconds + beeps[index];
        INFO("beep " << index << ": heard at " << heard << " s, flashed at " << flashes[index] << " s");
        CHECK(std::abs(heard - flashes[index]) <= 0.020);
    }
    CHECK(audio.syncP95Milliseconds <= 20.0);
    CHECK(audio.resyncs == 0);
    CHECK(audio.overflowFrames == 0);
}

TEST_CASE("audio: of two screens only the audible one is in the mix; turned away, the other takes over",
          "[gpu][audio]") {
    const std::filesystem::path dir = freshCaseDir("audio/two_screens");
    writeClip(dir / "clips" / "a.mkv", toneClip(12.0, 440.0, 48000));
    writeClip(dir / "clips" / "b.mkv", toneClip(12.0, 1000.0, 96000));
    const std::filesystem::path catalog =
        writeRoom(dir,
                  Json::array({game(kGameA, "Arco di Pietra", "clips/a.mkv"),
                               game(kGameB, "Bussola d'Ottone", "clips/b.mkv")}),
                  kGameA, kGameB);
    // Frame 240: 40 degrees to the right (crt_a and crt_b leave the view, crt_c stays): crt_c is heard.
    constexpr int kTurn = 240;
    const std::filesystem::path script =
        writeScript(dir, Json::array({step(kTurn, kTurn, {{"look_delta_x", kTurnPixels}})}));
    const AppRun run = runAudio(dir, catalog, 480, {"--input-script", arg(script)});
    const rv::audio::WavData wav = requireMix(run);
    INFO(run.describe());
    const rv::app::AudioRecord& audio = *run.report->audio;
    const double start = *audio.captureStartSeconds;
    const double turn = kTurn * 0.016667;
    // Facing the wall: the hero (crt_a, the room's class) is heard; crt_c's tone more than 40 dB below it.
    const double heroA = stereoAmplitude(wav, start, 1.5, turn, 440.0);
    const double heroB = stereoAmplitude(wav, start, 1.5, turn, 1000.0);
    INFO("facing: 440 Hz " << heroA << ", 1000 Hz " << heroB);
    CHECK(heroA > 0.05);
    CHECK(rv::audio::decibelsFromGain(heroB / heroA) < -40.0);
    // Turned: crt_c is heard (after the 300 ms cross-fade), crt_a's tone more than 40 dB below it.
    const double turnedA = stereoAmplitude(wav, start, turn + 1.0, 480 * 0.016667, 440.0);
    const double turnedB = stereoAmplitude(wav, start, turn + 1.0, 480 * 0.016667, 1000.0);
    INFO("turned: 440 Hz " << turnedA << ", 1000 Hz " << turnedB);
    CHECK(turnedB > 0.05);
    CHECK(rv::audio::decibelsFromGain(turnedA / turnedB) < -40.0);
    // The audible screen's changes as the report tells them.
    std::vector<std::string> heard;
    for (const rv::app::AudioAudibleRecord& change : audio.audible) {
        if (heard.empty() || heard.back() != change.screen) {
            heard.push_back(change.screen);
        }
    }
    REQUIRE_FALSE(heard.empty());
    CHECK(heard.back() == "crt_c");
    CHECK(std::find(heard.begin(), heard.end(), "crt_a") != heard.end());
    REQUIRE(audio.clips.size() == 2);
    CHECK(audio.clips.back().sampleRate == 96000u); // B's 96 kHz sound resampled into the 48 kHz mix
}

TEST_CASE("audio: walking away lowers the level as the gain table says", "[gpu][audio]") {
    const std::filesystem::path dir = freshCaseDir("audio/walk");
    writeClip(dir / "clips" / "a.mkv", toneClip(12.0, 440.0, 48000));
    // A proximity of 7.5 m: the fade of its last 2 m within the room's depth.
    constexpr double kProximity = 7.5;
    const std::filesystem::path catalog = writeRoom(dir, Json::array({game(kGameA, "Arco di Pietra", "clips/a.mkv")}),
                                                    kGameA, kGameA, Json{{"proximityMeters", kProximity}});
    // From frame 90 the walker backs away from the wall for 60 frames (2.5 m at 2.5 m/s), then stands.
    const std::filesystem::path script = writeScript(dir, Json::array({step(90, 149, {{"move_forward", -1.0}})}));
    const AppRun run = runAudio(dir, catalog, 240, {"--input-script", arg(script)});
    const rv::audio::WavData wav = requireMix(run);
    INFO(run.describe());
    const rv::app::AudioRecord& audio = *run.report->audio;
    REQUIRE(audio.levels.size() >= 10);
    // The report's gains follow the table at the distances it records, and fall as the walker backs away.
    double nearest = 1e9;
    double farthest = 0.0;
    for (const rv::app::AudioLevelRecord& level : audio.levels) {
        INFO("at " << level.atSeconds << " s: " << level.distance << " m, gain " << level.spatialGain);
        CHECK(level.screen == "crt_a");
        CHECK(level.spatialGain == Approx(rv::audio::spatialGain(level.distance, kProximity)).margin(1e-9));
        nearest = std::min(nearest, level.distance);
        farthest = std::max(farthest, level.distance);
    }
    CHECK(farthest - nearest > 2.0);
    // The mix's level against the table, where the walker stood still for a while (before and after the walk), and the
    // ratio between them, within 0.5 dB.
    const auto levelAt = [&](double seconds) {
        const auto found = std::min_element(audio.levels.begin(), audio.levels.end(),
                                            [&](const rv::app::AudioLevelRecord& a, const rv::app::AudioLevelRecord& b) {
                                                return std::abs(a.atSeconds - seconds) < std::abs(b.atSeconds - seconds);
                                            });
        return *found;
    };
    const double near = 1.2;
    const double far = 3.7;
    const rv::app::AudioLevelRecord before = levelAt(near);
    const rv::app::AudioLevelRecord after = levelAt(far);
    const double start = *audio.captureStartSeconds;
    const double heardBefore = stereoAmplitude(wav, start, near - 0.2, near + 0.2, 440.0) / 0.5;
    const double heardAfter = stereoAmplitude(wav, start, far - 0.2, far + 0.2, 440.0) / 0.5;
    INFO("near: " << before.distance << " m, table " << before.spatialGain << ", heard " << heardBefore);
    INFO("far: " << after.distance << " m, table " << after.spatialGain << ", heard " << heardAfter);
    CHECK(after.spatialGain < before.spatialGain);
    CHECK(rv::audio::decibelsFromGain(heardBefore / before.gain) == Approx(0.0).margin(0.5));
    CHECK(rv::audio::decibelsFromGain(heardAfter / after.gain) == Approx(0.0).margin(0.5));
    CHECK(rv::audio::decibelsFromGain(heardAfter / heardBefore) ==
          Approx(rv::audio::decibelsFromGain(after.spatialGain / before.spatialGain)).margin(0.5));
}

TEST_CASE("audio: F8 mutes and sounds again; the quick catalog ducks the sound", "[gpu][audio]") {
    const std::filesystem::path dir = freshCaseDir("audio/mute");
    writeClip(dir / "clips" / "a.mkv", toneClip(8.0, 440.0, 48000));
    const std::filesystem::path catalog =
        writeRoom(dir, Json::array({game(kGameA, "Arco di Pietra", "clips/a.mkv")}), kGameA, kGameA);
    // Frame 90: muted; 150: sound again; 210: the quick catalog opens (the duck); 300: closed.
    const std::filesystem::path script = writeScript(
        dir, Json::array({step(90, 90, {{"toggle_mute", 1}}), step(150, 150, {{"toggle_mute", 1}}),
                          step(210, 210, {{"quick_catalog", 1}}), step(300, 300, {{"quick_catalog", 1}})}));
    const AppRun run = runAudio(dir, catalog, 360, {"--input-script", arg(script)});
    const rv::audio::WavData wav = requireMix(run);
    INFO(run.describe());
    const rv::app::AudioRecord& audio = *run.report->audio;
    CHECK(audio.muteToggles == 2);
    CHECK_FALSE(audio.muted);
    CHECK(audio.duckChanges == 2);
    CHECK(audio.duckedFrames == 90);
    const double start = *audio.captureStartSeconds;
    const double frame = 0.016667;
    const double open = stereoAmplitude(wav, start, 60 * frame, 88 * frame, 440.0);
    const double muted = stereoAmplitude(wav, start, 92 * frame, 148 * frame, 440.0);
    const double ducked = stereoAmplitude(wav, start, 230 * frame, 298 * frame, 440.0);
    const double again = stereoAmplitude(wav, start, 320 * frame, 358 * frame, 440.0);
    INFO("open " << open << ", muted " << muted << ", ducked " << ducked << ", again " << again);
    CHECK(open > 0.05);
    CHECK(muted < open * 1e-3);
    CHECK(rv::audio::decibelsFromGain(ducked / open) == Approx(rv::audio::kDuckDecibels).margin(0.5));
    CHECK(again == Approx(open).epsilon(0.05));
}
