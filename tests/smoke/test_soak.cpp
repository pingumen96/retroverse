// The hour-long soak with the renderer (I5 step 7; ADR-0004 point 5 left "I5's hour-long soak settles the rest"):
// tests/smoke/scenes/screens_four.json's four CRTs playing generated clips of a mock catalog in real time (no fixed
// step, as fast as the hidden window presents), each screen's shuffle playlist moving on every 4 s, so that the two
// decoder slots open and close a clip about every two seconds for the whole run; one clip is cut short and fails each
// time it plays. The process's private bytes are sampled every 10 s from outside (GetProcessMemoryInfo) into
// <case>/memory.csv, and the window is closed after RV_SOAK_SECONDS (3600 by default). Hidden ([.soak]) and
// informative: run it by hand in Release,
//   build/msvc-x64-release/bin/rv_smoke_tests.exe "[soak]"
// (the app runs with RV_AUDIO=null whatever the environment). Its verdict: after the first 5 minutes, the private bytes
// grow by less than 32 MiB (the mean of the last 5 minutes against the mean of minutes 5 to 10), and the decoders stay
// within their budget and are joined at the exit.

#include "launch_support.hpp"
#include "smoke_support.hpp"

#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/media/test_video.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace rv::smoke;
using Json = nlohmann::json;

namespace {

constexpr double kMiB = 1024.0 * 1024.0;
constexpr std::chrono::seconds kSampleInterval{10};

struct Sample {
    double seconds = 0.0;
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSet = 0;
};

std::optional<Sample> sampleOf(std::uint32_t processId, double seconds) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (process == nullptr) {
        return std::nullopt;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    const BOOL read = GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                           sizeof(counters));
    CloseHandle(process);
    if (read == FALSE) {
        return std::nullopt;
    }
    return Sample{seconds, counters.PrivateUsage, counters.WorkingSetSize};
}

double meanPrivate(const std::vector<Sample>& samples, double from, double to) {
    double sum = 0.0;
    std::size_t count = 0;
    for (const Sample& sample : samples) {
        if (sample.seconds >= from && sample.seconds < to) {
            sum += static_cast<double>(sample.privateBytes);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

void writeClip(const std::filesystem::path& path, double seconds, std::uint32_t seed) {
    rv::media::TestVideoSpec spec;
    spec.width = 640;
    spec.height = 480;
    spec.frameRate = 30;
    spec.durationSeconds = seconds;
    spec.seed = seed;
    spec.quantizer = 5;
    spec.keyFrameInterval = 30;
    REQUIRE(rv::media::writeTestVideo(path, spec).has_value());
}

} // namespace

TEST_CASE("soak: an hour of four screens playing and switching clips, the private bytes flat", "[.soak][gpu]") {
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir("soak");
    double soakSeconds = 3600.0;
    if (const char* text = std::getenv("RV_SOAK_SECONDS"); text != nullptr && std::atof(text) > 0.0) {
        soakSeconds = std::atof(text);
    }

    // Five games with a clip and a cover; the fifth clip is cut short (it fails at its end, every time it plays).
    Json games = Json::array();
    for (int index = 0; index < 5; ++index) {
        const std::string name = fmt::format("{}", static_cast<char>('a' + index));
        writeClip(dir / "clips" / (name + ".mp4"), 6.0, 91 + static_cast<std::uint32_t>(index));
        REQUIRE(rv::assets::writePng(dir / "covers" / (name + ".png"),
                                     rv::assets::makeCoverPlaceholder(640, 480, "Soak " + name,
                                                                      101 + static_cast<std::uint32_t>(index)))
                    .has_value());
        games.push_back(Json{{"id", fmt::format("g000000000000d00{}", index + 1)},
                             {"title", "Soak " + name},
                             {"platform", "Portatile 16 bit"},
                             {"media", {{"video", "clips/" + name + ".mp4"}, {"cover", "covers/" + name + ".png"}}}});
    }
    REQUIRE(rv::media::writeTruncatedCopy(dir / "clips" / "e.mp4", dir / "clips" / "e_cut.mp4", 0.6).has_value());
    games[4]["media"]["video"] = "clips/e_cut.mp4";
    const std::filesystem::path catalog = dir / "catalog.json";
    REQUIRE(rv::core::writeTextFileAtomic(catalog, Json{{"schemaVersion", 1}, {"games", games}}.dump(2) + "\n")
                .has_value());
    // Every screen's shuffle moves on every 4 s.
    const Json settings{{"hero", {{"previewDuration", 4}}}, {"secondary", {{"previewDuration", 4}}}};
    std::filesystem::create_directories(dir / "data" / "config");
    REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "config" / "video.json", settings.dump(2) + "\n").has_value());

    StartedApp app(dir, "soak",
                   {"--scene", arg(sourceDir() / "tests" / "smoke" / "scenes" / "screens_four.json"), "--mock-catalog",
                    arg(catalog), "--exit-after-frames", "--frames", "1000000000", "--width", "960", "--height", "540"});
    std::vector<Sample> samples;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    while (elapsed() < soakSeconds && processRunning(app.processId(), "retroverse.exe")) {
        if (const std::optional<Sample> sample = sampleOf(app.processId(), elapsed())) {
            samples.push_back(*sample);
        }
        std::this_thread::sleep_for(kSampleInterval);
    }
    REQUIRE(processRunning(app.processId(), "retroverse.exe")); // it ran the whole soak
    // Closed as the user closes it: the run ends, its decoders joined.
    bool closed = false;
    for (int attempt = 0; attempt < 50 && !closed; ++attempt) {
        closed = app.closeWindow();
        if (!closed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    REQUIRE(closed);
    const AppRun run = app.finish();
    INFO(run.describe());
    CHECK(run.exitCode == 0);
    REQUIRE(run.report);
    const rv::app::VideoRecord& video = run.report->video;

    std::string csv = "seconds,private_bytes,working_set\n";
    for (const Sample& sample : samples) {
        csv += fmt::format("{:.1f},{},{}\n", sample.seconds, sample.privateBytes, sample.workingSet);
    }
    REQUIRE(rv::core::writeTextFileAtomic(dir / "memory.csv", csv).has_value());
    const double warm = meanPrivate(samples, 300.0, 600.0);
    const double last = meanPrivate(samples, soakSeconds - 300.0, soakSeconds + 1.0);
    double peak = 0.0;
    for (const Sample& sample : samples) {
        peak = std::max(peak, static_cast<double>(sample.privateBytes));
    }
    std::cout << fmt::format("Soak: {:.0f} s, {} samples; private bytes {:.1f} MiB (minutes 5-10) -> {:.1f} MiB (last 5 "
                             "minutes), {:+.1f} MiB, peak {:.1f} MiB; {} decoders started, at most {} open, {} "
                             "fallback(s), frames {}\n",
                             soakSeconds, samples.size(), warm / kMiB, last / kMiB, (last - warm) / kMiB, peak / kMiB,
                             video.decodersStarted, video.maxOpenDecoders, video.fallbacks.size(),
                             run.report->framesRendered);
    CHECK(video.maxOpenDecoders <= 2);
    CHECK(video.maxDecoderThreads <= 2);
    CHECK(video.joinedAtExit);
    CHECK(video.decodersStarted > static_cast<std::uint64_t>(soakSeconds / 10.0));
    if (soakSeconds >= 1200.0) {
        CHECK((last - warm) / kMiB < 32.0);
    }
}
