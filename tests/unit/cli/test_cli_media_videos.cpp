// retroverse-cli media --check-videos (I5 step 7, ADR-0016 §8) through the dispatcher, on the video-verdicts fixture:
// each damaged kind of SC-08 gets its verdict in <user data>/cache/media/videos.json, the report counts them per kind
// and system and lists the failures; a second run checks nothing (it resumes), --recheck-failures checks the failures
// again, a clip whose file changed is checked again and a clip gone loses its verdict; the usage errors. Nothing is
// written under the media root.

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_verdicts.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const fs::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger on stdout afterwards.
class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(path.lexically_normal());
}

nlohmann::json parsed(const std::string& text) {
    auto json = rv::core::parseJson(text, "stdout");
    INFO(text);
    REQUIRE(json.has_value());
    return std::move(*json);
}

/// Every file under `root` with its size, time and the SHA-256 of its content: the media root before and after.
std::vector<std::string> treeOf(const fs::path& root) {
    const auto listed = rv::core::listTree(root);
    REQUIRE(listed.has_value());
    std::vector<std::string> tree;
    for (const auto& entry : *listed) {
        if (entry.kind != rv::core::TreeEntryKind::File) {
            tree.push_back(entry.relativePath + "/");
            continue;
        }
        const auto path = rv::core::extendedLengthPathBelow(root, entry.relativePath);
        REQUIRE(path.has_value());
        const auto bytes = rv::core::readFileShared(*path, 64ull * 1024ull * 1024ull);
        REQUIRE(bytes.has_value());
        tree.push_back(fmt::format("{} {} {} {}", entry.relativePath, entry.size,
                                   entry.lastWriteTime.time_since_epoch().count(), rv::core::sha256Hex(*bytes)));
    }
    return tree;
}

/// retroverse-cli with a user data directory of its own, the fixture's library recorded in it.
class Cli {
public:
    Cli(const rv::test::Fixture& fixture, std::string_view prefix) : m_dir(prefix), m_data(m_dir.file("data")) {
        const RunResult created = run(
            {"library", "create", "--home", utf8(fixture.root()), "--install", utf8(fixture.path("Programs/ES-DE"))});
        INFO(created.err);
        REQUIRE(created.code == ExitCode::Ok);
    }

    RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(m_data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }
    RunResult check(std::vector<std::string> arguments = {}) const {
        arguments.insert(arguments.begin(), {"media", "--check-videos"});
        return run(std::move(arguments));
    }
    [[nodiscard]] fs::path verdictsFile() const { return rv::media::videoVerdictsFile(m_data / "cache"); }

private:
    rv::test::TempDir m_dir;
    fs::path m_data;
};

/// The verdict of each clip of the fixture, as the file holds it: "<path> <verdict> <source>".
std::map<std::string, std::string> verdictsOf(const fs::path& file) {
    const auto verdicts = rv::media::readVideoVerdicts(file);
    REQUIRE(verdicts.has_value());
    std::map<std::string, std::string> found;
    for (const rv::media::VideoVerdict* verdict : verdicts->entries()) {
        found[verdict->path] =
            rv::media::verdictName(verdict->failure) + " " + std::string(rv::media::toString(verdict->source));
    }
    return found;
}

const std::map<std::string, std::string> kExpected{
    {"snes/videos/Amber Tide (USA).mp4", "playable check"},
    {"snes/videos/Brass Kettle (USA).mp4", "truncated check"},
    {"snes/videos/Cinder Bell (USA).mp4", "corrupt-data check"},
    {"snes/videos/Dune Lantern (USA).mp4", "invalid-file check"},
    {"snes/videos/Ember Quill (USA).avi", "unsupported-codec check"},
    {"snes/videos/Frost Gate (USA).mp4", "no-video-stream check"},
    {"snes/videos/Garnet Loom (USA).mp4", "invalid-file check"},
    {"snes/videos/Hazel Orbit (USA).mp4", "playable check"},
};

} // namespace

TEST_CASE("cli/media --check-videos: each damaged kind of clip gets its verdict", "[cli][media][video_verdicts]") {
    auto materialized = rv::test::Fixture::materialize("esde/video-verdicts");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    const rv::test::Fixture& fixture = *materialized;
    const LoggingReset loggingReset;
    const auto mediaBefore = treeOf(fixture.path("media"));
    const Cli cli(fixture, "rv-cli-videos");

    const RunResult first = cli.check({"--json", "--jobs", "3"});
    INFO(first.err);
    REQUIRE(first.code == ExitCode::Ok);
    const nlohmann::json report = parsed(first.out);
    CHECK(report["jobs"] == 3);
    CHECK(report["verdictsFile"] == utf8(cli.verdictsFile()));
    CHECK(report["total"]["clips"] == 8);
    CHECK(report["total"]["checked"] == 8);
    CHECK(report["total"]["known"] == 0);
    CHECK(report["total"]["unchecked"] == 0);
    const nlohmann::json& counts = report["total"]["verdicts"];
    CHECK(counts["playable"] == 2);
    CHECK(counts["invalid-file"] == 2);
    CHECK(counts["no-video-stream"] == 1);
    CHECK(counts["unsupported-codec"] == 1);
    CHECK(counts["corrupt-data"] == 1);
    CHECK(counts["truncated"] == 1);
    CHECK(counts["decoder-error"] == 0);
    REQUIRE(report["systems"].size() == 1);
    CHECK(report["systems"][0]["system"] == "snes");
    CHECK(report["systems"][0]["videosFolder"] == "listed");
    CHECK(report["systems"][0]["clips"] == 8);
    CHECK(report["failures"].size() == 6);
    for (const auto& failure : report["failures"]) {
        INFO(failure.dump());
        CHECK(failure["detail"].get<std::string>() != ""); // the decoder's reason, for a clip checked now
        CHECK(kExpected.at(failure["path"].get<std::string>()) ==
              failure["verdict"].get<std::string>() + " " + failure["source"].get<std::string>());
    }
    CHECK(report["incomplete"].empty());
    CHECK(report["removed"] == 0);

    // The verdicts file: every clip's, keyed by its path under the media root and its file's facts.
    CHECK(verdictsOf(cli.verdictsFile()) == kExpected);
    const auto verdicts = rv::media::readVideoVerdicts(cli.verdictsFile()).value();
    const rv::media::VideoVerdict* amber = verdicts.find("snes/videos/Amber Tide (USA).mp4");
    REQUIRE(amber != nullptr);
    const auto facts = rv::core::listedFileFacts(fixture.path("media/snes/videos/Amber Tide (USA).mp4"));
    REQUIRE(facts.has_value());
    CHECK(amber->file == *facts);
    CHECK(amber->codec == "mpeg4");
    CHECK(amber->width == 640);
    CHECK(amber->height == 480);
    CHECK(amber->durationMs == 3000);
    const rv::media::VideoVerdict* hazel = verdicts.find("snes/videos/Hazel Orbit (USA).mp4");
    REQUIRE(hazel != nullptr);
    CHECK(hazel->durationMs == 12000);
    CHECK(hazel->decodeErrors == 0);

    // The same as text: the table and the failures.
    const RunResult text = cli.check();
    REQUIRE(text.code == ExitCode::Ok);
    INFO(text.out);
    CHECK(
        text.out.find("System           Clips  playable   invalid  no-video     codec   corrupt truncated   decoder") !=
        std::string::npos);
    CHECK(
        text.out.find("snes                 8         2         2         1         1         1         1         0") !=
        std::string::npos);
    CHECK(text.out.find("Failures (6)") != std::string::npos);
    CHECK(text.out.find("  truncated         snes/videos/Brass Kettle (USA).mp4") != std::string::npos);

    // Nothing was written under the media root.
    CHECK(treeOf(fixture.path("media")) == mediaBefore);
}

TEST_CASE("cli/media --check-videos: resumes, checks a changed clip again and forgets a clip gone",
          "[cli][media][video_verdicts]") {
    auto materialized = rv::test::Fixture::materialize("esde/video-verdicts");
    REQUIRE(materialized.has_value());
    const rv::test::Fixture& fixture = *materialized;
    const LoggingReset loggingReset;
    const Cli cli(fixture, "rv-cli-videos-resume");
    REQUIRE(cli.check({"--system", "snes"}).code == ExitCode::Ok);
    const auto firstText = rv::core::readTextFile(cli.verdictsFile());
    REQUIRE(firstText.has_value());

    // Nothing changed: nothing checked, the file's bytes kept.
    const RunResult again = cli.check({"--json"});
    REQUIRE(again.code == ExitCode::Ok);
    const nlohmann::json resumed = parsed(again.out);
    CHECK(resumed["total"]["checked"] == 0);
    CHECK(resumed["total"]["known"] == 8);
    CHECK(resumed["failures"].size() == 6);
    for (const auto& failure : resumed["failures"]) {
        CHECK(failure["detail"] == ""); // known: no reason of a check made now
    }
    CHECK(rv::core::readTextFile(cli.verdictsFile()).value() == *firstText);

    // --recheck-failures: the six failures checked again (their files unchanged), the same verdicts, each with the
    // decoder's reason now; the playable clips left alone.
    const RunResult failuresAgain = cli.check({"--json", "--recheck-failures"});
    REQUIRE(failuresAgain.code == ExitCode::Ok);
    const nlohmann::json recheckedFailures = parsed(failuresAgain.out);
    CHECK(recheckedFailures["total"]["checked"] == 6);
    CHECK(recheckedFailures["total"]["known"] == 2);
    CHECK(recheckedFailures["total"]["verdicts"]["playable"] == 2);
    REQUIRE(recheckedFailures["failures"].size() == 6);
    for (const auto& failure : recheckedFailures["failures"]) {
        CHECK(failure["detail"] != "");
    }

    // The truncated clip is replaced by a whole one (another size and time): checked again, playable now.
    const fs::path brass = fixture.path("media/snes/videos/Brass Kettle (USA).mp4");
    rv::media::TestVideoSpec spec;
    spec.width = 320;
    spec.height = 240;
    spec.durationSeconds = 2.0;
    spec.seed = 9;
    REQUIRE(rv::media::writeTestVideo(brass, spec));
    // Only the time changed: checked again too.
    const fs::path amber = fixture.path("media/snes/videos/Amber Tide (USA).mp4");
    fs::last_write_time(amber, fs::last_write_time(amber) + std::chrono::hours(1));
    const RunResult changed = cli.check({"--json"});
    REQUIRE(changed.code == ExitCode::Ok);
    const nlohmann::json rechecked = parsed(changed.out);
    CHECK(rechecked["total"]["checked"] == 2);
    CHECK(rechecked["total"]["known"] == 6);
    CHECK(rechecked["total"]["verdicts"]["playable"] == 3);
    CHECK(rechecked["total"]["verdicts"]["truncated"] == 0);
    CHECK(verdictsOf(cli.verdictsFile()).at("snes/videos/Brass Kettle (USA).mp4") == "playable check");

    // A clip gone: its verdict removed.
    REQUIRE(fs::remove(fixture.path("media/snes/videos/Cinder Bell (USA).mp4")));
    const RunResult gone = cli.check({"--json"});
    REQUIRE(gone.code == ExitCode::Ok);
    const nlohmann::json pruned = parsed(gone.out);
    CHECK(pruned["total"]["clips"] == 7);
    CHECK(pruned["removed"] == 1);
    CHECK_FALSE(verdictsOf(cli.verdictsFile()).contains("snes/videos/Cinder Bell (USA).mp4"));
    CHECK(verdictsOf(cli.verdictsFile()).size() == 7);
}

TEST_CASE("cli/media --check-videos: usage errors", "[cli][media][video_verdicts]") {
    auto materialized = rv::test::Fixture::materialize("esde/video-verdicts");
    REQUIRE(materialized.has_value());
    const LoggingReset loggingReset;
    const Cli cli(*materialized, "rv-cli-videos-usage");
    for (const std::vector<std::string>& arguments :
         {std::vector<std::string>{"media", "--jobs", "2", "--coverage"},
          std::vector<std::string>{"media", "--coverage", "--recheck-failures"},
          std::vector<std::string>{"media", "--check-videos", "--jobs", "0"},
          std::vector<std::string>{"media", "--check-videos", "--jobs", "9"},
          std::vector<std::string>{"media", "--check-videos", "--jobs", "two"},
          std::vector<std::string>{"media", "--check-videos", "--coverage"},
          std::vector<std::string>{"media", "--check-videos", "--system", "snes", "--path", "Amber Tide (USA).sfc"},
          std::vector<std::string>{"media", "--check-videos", "--game", "g0000000000000001"},
          std::vector<std::string>{"media", "--check-videos", "--system", "nes"}}) {
        const RunResult result = cli.run(arguments);
        INFO(fmt::format("{}: {}", fmt::join(arguments, " "), result.err));
        CHECK(result.code == ExitCode::UsageError);
    }
    CHECK_FALSE(fs::exists(cli.verdictsFile()));
}
