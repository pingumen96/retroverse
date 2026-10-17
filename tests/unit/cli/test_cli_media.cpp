// retroverse-cli media (I2 step 8) through the dispatcher: the media of a ROM path of the standard fixture's systems
// (a file, a folder, a missing file, a gamelist's "./" form, another case) as text and JSON, the coverage of its
// library snapshot (the ROM scan of I2 step 7) over the four populations, the usage errors and --game until step 11;
// the media-roots fixture's empty, missing and unreachable media roots. The command only reads: the fixture's files
// are unchanged after it.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

/// The source tree's assets and no %LOCALAPPDATA%: a run never reaches the real user data (tests pass --data-dir).
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

rv::test::Fixture materialize(std::string_view name) {
    auto fixture = rv::test::Fixture::materialize(name);
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    return std::move(*fixture);
}

/// A path as the CLI prints it: '\' between the elements (fixture paths mix in the build folder's '/').
std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(path.lexically_normal());
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

nlohmann::json parsed(const std::string& text) {
    auto json = rv::core::parseJson(text, "stdout");
    INFO(text);
    REQUIRE(json.has_value());
    return std::move(*json);
}

/// Every folder under `root`, and every file with its size and the SHA-256 of its content.
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
        const auto bytes = rv::core::readFileShared(*path, 16ull * 1024ull * 1024ull);
        REQUIRE(bytes.has_value());
        tree.push_back(fmt::format("{} {} {}", entry.relativePath, entry.size, rv::core::sha256Hex(*bytes)));
    }
    return tree;
}

/// A drive letter with no drive behind it, or 0.
wchar_t unusedDriveLetter() {
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if ((drives & (1u << (letter - L'A'))) == 0) {
            return letter;
        }
    }
    return 0;
}

/// retroverse-cli with a user data directory of its own, the library of a fixture recorded in it.
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
    RunResult media(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), "media");
        return run(std::move(arguments));
    }
    [[nodiscard]] const rv::test::TempDir& dir() const noexcept { return m_dir; }

private:
    rv::test::TempDir m_dir;
    fs::path m_data;
};

/// The media items of a --path report by kind: "<extension> <path>", or "none".
std::vector<std::string> mediaItems(const nlohmann::json& report) {
    std::vector<std::string> items;
    for (const auto& item : report.at("media")) {
        items.push_back(item.at("present").get<bool>()
                            ? item.at("kind").get<std::string>() + " " + item.at("extension").get<std::string>() + " " +
                                  item.at("path").get<std::string>()
                            : item.at("kind").get<std::string>() + " none");
    }
    return items;
}

} // namespace

TEST_CASE("cli/media: the media of a ROM path of the standard fixture, as JSON and as text", "[cli][media]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const LoggingReset loggingReset;
    const auto before = treeOf(fixture.root());
    const Cli cli(fixture, "rv-cli-media");
    const auto media = [&](std::string_view relative) {
        return utf8(fixture.path("media") / rv::core::pathFromUtf8(relative));
    };

    // All 12 kinds of Alpha Quest.
    const RunResult alpha = cli.media({"--system", "snes", "--path", "Alpha Quest (USA).sfc", "--json"});
    INFO(alpha.err);
    REQUIRE(alpha.code == ExitCode::Ok);
    const nlohmann::json report = parsed(alpha.out);
    CHECK(report["system"] == "snes");
    CHECK(report["path"] == "Alpha Quest (USA).sfc");
    CHECK(report["romPath"] == "file");
    CHECK(report["mediaName"] == "Alpha Quest (USA)");
    CHECK(report["mediaRoot"]["path"] == utf8(fixture.path("media")));
    CHECK(report["mediaRoot"]["source"] == "setting");
    CHECK(mediaItems(report) == std::vector<std::string>{
                                    "covers .png " + media("snes/covers/Alpha Quest (USA).png"),
                                    "backcovers .png " + media("snes/backcovers/Alpha Quest (USA).png"),
                                    "3dboxes .png " + media("snes/3dboxes/Alpha Quest (USA).png"),
                                    "physicalmedia .png " + media("snes/physicalmedia/Alpha Quest (USA).png"),
                                    "screenshots .jpg " + media("snes/screenshots/Alpha Quest (USA).jpg"),
                                    "titlescreens .jpg " + media("snes/titlescreens/Alpha Quest (USA).jpg"),
                                    "marquees .png " + media("snes/marquees/Alpha Quest (USA).png"),
                                    "miximages .png " + media("snes/miximages/Alpha Quest (USA).png"),
                                    "fanart .jpg " + media("snes/fanart/Alpha Quest (USA).jpg"),
                                    "manuals .pdf " + media("snes/manuals/Alpha Quest (USA).pdf"),
                                    "videos .mov " + media("snes/videos/Alpha Quest (USA).mov"),
                                    "custom .png " + media("snes/custom/Alpha Quest (USA).png"),
                                });

    // The same as text.
    const RunResult text = cli.media({"--system", "snes", "--path", "Alpha Quest (USA).sfc"});
    CHECK(text.code == ExitCode::Ok);
    CHECK(contains(text.out, "System        snes (Nintendo SNES (Super Nintendo))\n"));
    CHECK(contains(text.out, "ROM path      Alpha Quest (USA).sfc (a file)\n"));
    CHECK(contains(text.out, "Media name    Alpha Quest (USA)\n"));
    CHECK(contains(text.out, "Media         12 of 12 kinds\n"));
    CHECK(contains(text.out, "  manuals       " + media("snes/manuals/Alpha Quest (USA).pdf") + "\n"));
    CHECK(text.err.empty());

    // A folder in a gamelist's "./" form keeps its whole name.
    const RunResult sports = cli.media({"--system", "snes", "--path", "./Sports Vol. 2", "--json"});
    REQUIRE(sports.code == ExitCode::Ok);
    const nlohmann::json folder = parsed(sports.out);
    CHECK(folder["path"] == "Sports Vol. 2");
    CHECK(folder["romPath"] == "folder");
    CHECK(folder["mediaName"] == "Sports Vol. 2");
    CHECK(folder["media"][0]["path"] == media("snes/covers/Sports Vol. 2.png"));
    CHECK(folder["media"][1]["present"] == false);
    CHECK(folder["media"][1]["extension"].is_null());
    CHECK(folder["media"][1]["path"].is_null());

    // A gamelist entry whose file is gone keeps its cover; the png comes before the jpg; names match in any case.
    const RunResult gone = cli.media({"--system", "gb", "--path", "Gone Fishing (USA).gb", "--json"});
    REQUIRE(gone.code == ExitCode::Ok);
    CHECK(parsed(gone.out)["romPath"] == "missing");
    CHECK(parsed(gone.out)["media"][0]["path"] == media("gb/covers/Gone Fishing (USA).png"));
    const RunResult tennis = cli.media({"--system", "gb", "--path", "Tiny Tennis (Europe).gb"});
    CHECK(contains(tennis.out, "Media         3 of 12 kinds\n"));
    CHECK(contains(tennis.out, "  covers        " + media("gb/covers/Tiny Tennis (Europe).png") + "\n"));
    CHECK(contains(tennis.out, "  backcovers    none\n"));
    // A path in another case is the file as the disk spells it, whose spelling ES-DE gives its media.
    const RunResult lower = cli.media({"--system", "gb", "--path", "collection a/pocket puzzle (world).gb", "--json"});
    REQUIRE(lower.code == ExitCode::Ok);
    CHECK(parsed(lower.out)["romPath"] == "file");
    CHECK(parsed(lower.out)["path"] == "Collection A/Pocket Puzzle (World).gb");
    CHECK(parsed(lower.out)["mediaName"] == "Collection A/Pocket Puzzle (World)");
    CHECK(parsed(lower.out)["media"][0]["path"] == media("gb/covers/Collection A/Pocket Puzzle (World).png"));
    const RunResult lowerGone = cli.media({"--system", "gb", "--path", "collection a/gone (world).gb", "--json"});
    REQUIRE(lowerGone.code == ExitCode::Ok);
    CHECK(parsed(lowerGone.out)["romPath"] == "missing");
    CHECK(parsed(lowerGone.out)["path"] == "Collection A/gone (world).gb"); // as far as the disk holds it
    const RunResult namesake = cli.media({"--system", "gb", "--path", "Collection B/Pocket Puzzle (World).gb"});
    CHECK(contains(namesake.out, "Media         0 of 12 kinds\n"));
    const RunResult flv = cli.media({"--system", "snes", "--path", "Beta Run (Europe).SMC"});
    CHECK(contains(flv.out, "  videos        none\n"));

    // --output: the results in the file, nothing on stdout.
    const fs::path output = cli.dir().file("out/media.json");
    const RunResult toFile =
        cli.media({"--system", "psx", "--path", "Twin Tracks (Europe).cue", "--json", "--output", utf8(output)});
    CHECK(toFile.code == ExitCode::Ok);
    CHECK(toFile.out.empty());
    const auto written = rv::core::readTextFile(output, 1u << 20);
    REQUIRE(written.has_value());
    CHECK(parsed(*written)["media"][10]["path"] == media("psx/videos/Twin Tracks (Europe).wmv"));

    // Usage errors.
    const auto refused = [&](std::vector<std::string> arguments, std::string_view part) {
        const RunResult result = cli.media(std::move(arguments));
        INFO(result.err);
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
        CHECK(contains(result.err, part));
    };
    refused({"--system", "snes", "--path", "../psx/Solo Disc (USA).chd"}, "leads out of the ROM folder of snes");
    refused({"--system", "snes", "--path", "C:a.sfc"}, "--path is no path below a ROM folder");
    refused({"--system", "nes", "--path", "Anything.nes"}, "no system named \"nes\"");
    refused({"--system", "bad/name", "--path", "x"}, "--system");
    refused({"--path", "Alpha Quest (USA).sfc"}, "give --system and --path");
    refused({"--system", "snes"}, "give --system and --path");
    refused({}, "give --system and --path");
    refused({"--coverage", "--path", "x"}, "--coverage takes no --path");
    refused({"--system", "snes", "--path", ""}, "--path must not be empty");
    refused({"--game", "g0000000000000001"}, "retroverse-cli media: --game: not implemented yet (I2 step 11)");
    refused({"--no-such-option"}, "--no-such-option");

    const RunResult help = cli.media({"--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK(contains(help.out, "--coverage"));

    // Nothing written in the library's folders.
    CHECK(treeOf(fixture.root()) == before);
}

TEST_CASE("cli/media: the coverage of the standard fixture's library snapshot", "[cli][media]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const LoggingReset loggingReset;
    const auto before = treeOf(fixture.root());
    const Cli cli(fixture, "rv-cli-coverage");

    const RunResult json = cli.media({"--coverage", "--json"});
    INFO(json.err);
    REQUIRE(json.code == ExitCode::Ok);
    const nlohmann::json report = parsed(json.out);
    CHECK(report["mediaRoot"]["path"] == utf8(fixture.path("media")));
    CHECK(report["source"] == "rom-scan");
    CHECK(report["kinds"].size() == 12);
    CHECK(report["kinds"][2] == "3dboxes");
    CHECK(report["listed"]["systems"] == 3);
    CHECK(report["listed"]["files"] == 25);
    const nlohmann::json& total = report["total"];
    // gb: Tiny Tennis, Pocket Puzzle A, Café Racer, Gone Fishing (missing), Wrong Way (outside); snes: Alpha Quest,
    // Mixed Case (no game), Gamma Hack, Delta Drift (its path differs in case); psx: Twin Tracks, Solo Disc.
    CHECK(total["gamelist-entries"]["entries"] == 11);
    CHECK(total["gamelist-entries"]["unknown"] == 0);
    CHECK(total["gamelist-entries"]["withAny"] == 6);
    CHECK(total["gamelist-entries"]["withKind"]["covers"] == 5);
    CHECK(total["gamelist-entries"]["withKind"]["videos"] == 4);
    CHECK(total["gamelist-entries"]["withKind"]["custom"] == 1);
    CHECK(total["gamelist-entries-with-file"]["entries"] == 9);
    CHECK(total["gamelist-entries-with-file"]["withKind"]["covers"] == 4);
    CHECK(total["scraped-games"]["entries"] == 8);
    CHECK(total["scraped-games"]["withKind"]["screenshots"] == 2);
    // Every game of the snapshot, scraped or not: the README's 15 (Twin Tracks' two tracks are disc members, in no
    // population). With media: Tiny Tennis (cover, screenshot, video), Pocket Puzzle A (cover), Café Racer and LOUD
    // CART (videos), Alpha Quest (all 12 kinds), Twin Tracks (cover, video).
    CHECK(total["games"]["entries"] == 15);
    CHECK(total["games"]["unknown"] == 0);
    CHECK(total["games"]["withAny"] == 6);
    CHECK(total["games"]["withKind"]["covers"] == 4);
    CHECK(total["games"]["withKind"]["backcovers"] == 1);
    CHECK(total["games"]["withKind"]["screenshots"] == 2);
    CHECK(total["games"]["withKind"]["videos"] == 5);
    CHECK(total["games"]["withKind"]["custom"] == 1);
    REQUIRE(report["systems"].size() == 3);
    CHECK(report["systems"][0]["system"] == "gb");
    CHECK(report["systems"][0]["mediaFolder"] == "listed");
    CHECK(report["systems"][0]["populations"]["gamelist-entries"]["entries"] == 5);
    CHECK(report["systems"][0]["populations"]["gamelist-entries"]["withKind"]["covers"] == 3);
    CHECK(report["systems"][0]["populations"]["games"]["entries"] == 8);
    CHECK(report["systems"][0]["populations"]["games"]["withKind"]["covers"] == 2);
    CHECK(report["systems"][0]["populations"]["games"]["withKind"]["videos"] == 3);
    CHECK(report["systems"][1]["system"] == "psx");
    CHECK(report["systems"][1]["populations"]["games"]["entries"] == 2);
    CHECK(report["systems"][2]["system"] == "snes");
    CHECK(report["systems"][2]["populations"]["gamelist-entries-with-file"]["entries"] == 4);
    CHECK(report["systems"][2]["populations"]["scraped-games"]["entries"] == 3);
    CHECK(report["systems"][2]["populations"]["games"]["entries"] == 5);
    CHECK(report["systems"][2]["populations"]["games"]["withAny"] == 1);
    std::vector<std::string> codes;
    for (const auto& issue : report["issues"]) {
        codes.push_back(issue["code"].get<std::string>());
        CHECK(issue["severity"] != "error");
    }
    CHECK(std::find(codes.begin(), codes.end(), "gamelist-path-outside-folder") != codes.end());
    CHECK(std::find(codes.begin(), codes.end(), "media-shadowed-file") != codes.end());
    CHECK(std::find(codes.begin(), codes.end(), "media-ignored-extension") != codes.end());

    const RunResult text = cli.media({"--coverage"});
    CHECK(text.code == ExitCode::Ok);
    CHECK(contains(text.out, "Media root    " + utf8(fixture.path("media")) + " (MediaDirectory \""));
    CHECK(contains(text.out, "Entries       23 in the library snapshot (the ROM folders listed with ES-DE's rules"));
    CHECK(contains(text.out, "Listed        3 system(s), 25 files in their media folders\n"));
    // 5 covers of 11 entries: 45.5%; 4 videos: 36.4%.
    CHECK(contains(text.out,
                   "gamelist-entries                  11   45.5    9.1    9.1    9.1   18.2    9.1    9.1    9.1"
                   "    9.1    9.1   36.4    9.1\n"));
    // 4 covers of 15 games: 26.7%; 5 videos: 33.3%.
    CHECK(contains(text.out,
                   "games                             15   26.7    6.7    6.7    6.7   13.3    6.7    6.7    6.7"
                   "    6.7    6.7   33.3    6.7\n"));
    CHECK(contains(text.out, "The gamelist entries whose file exists, per system\n"));
    CHECK(contains(text.out, "gb                   3   66.7    0.0    0.0    0.0   33.3    0.0    0.0    0.0    0.0"
                             "    0.0   66.7    0.0\n"));
    CHECK(contains(text.out, "snes                 4   25.0   25.0"));
    CHECK(contains(text.out, "All                  9   44.4"));
    CHECK(contains(text.out, "The games, per system\n"));
    CHECK(contains(text.out, "gb                   8   25.0    0.0    0.0    0.0   12.5    0.0    0.0    0.0    0.0"
                             "    0.0   37.5    0.0\n"));
    CHECK(contains(text.out, "snes                 5   20.0   20.0"));
    CHECK(contains(text.out, "All                 15   26.7"));
    CHECK_FALSE(contains(text.out, "until the ROM scan"));
    CHECK(contains(text.out, "Issues ("));

    // One system.
    const RunResult psx = cli.media({"--coverage", "--system", "psx", "--json"});
    REQUIRE(psx.code == ExitCode::Ok);
    const nlohmann::json one = parsed(psx.out);
    REQUIRE(one["systems"].size() == 1);
    CHECK(one["systems"][0]["system"] == "psx");
    CHECK(one["listed"]["systems"] == 1);
    CHECK(one["total"]["games"]["entries"] == 2);
    CHECK(one["total"]["games"]["withKind"]["videos"] == 1);
    CHECK(one["total"]["gamelist-entries"]["entries"] == 2);

    CHECK(treeOf(fixture.root()) == before);
}

TEST_CASE("cli/media: an empty media root, a missing one, an unreachable one; no library", "[cli][media]") {
    const rv::test::Fixture fixture = materialize("esde/media-roots");
    const LoggingReset loggingReset;
    const Cli cli(fixture, "rv-cli-media-roots");

    // Empty: no media, no error.
    const RunResult empty = cli.media({"--coverage", "--json"});
    INFO(empty.err);
    REQUIRE(empty.code == ExitCode::Ok);
    const nlohmann::json report = parsed(empty.out);
    REQUIRE(report["systems"].size() == 2);
    // gb has no gamelist: its one game is unscraped.
    CHECK(report["systems"][0]["system"] == "gb");
    CHECK(report["systems"][0]["mediaFolder"] == "missing");
    CHECK(report["systems"][0]["populations"]["gamelist-entries"]["entries"] == 0);
    CHECK(report["systems"][0]["populations"]["games"]["entries"] == 1);
    CHECK(report["systems"][1]["system"] == "nes");
    CHECK(report["systems"][1]["mediaFolder"] == "missing");
    CHECK(report["systems"][1]["populations"]["games"]["entries"] == 2);
    CHECK(report["total"]["gamelist-entries"]["entries"] == 2);
    CHECK(report["total"]["gamelist-entries-with-file"]["entries"] == 1);
    CHECK(report["total"]["scraped-games"]["entries"] == 1);
    CHECK(report["total"]["games"]["entries"] == 3);
    CHECK(report["total"]["games"]["withAny"] == 0);
    CHECK(report["total"]["gamelist-entries"]["withAny"] == 0);
    CHECK(report["total"]["gamelist-entries"]["unknown"] == 0);
    CHECK(report["issues"].empty());
    const RunResult emptyText = cli.media({"--coverage"});
    CHECK(contains(emptyText.out, "nes                  1    0.0    0.0"));
    CHECK(contains(emptyText.out, "gb                   1    0.0    0.0")); // only among the games
    CHECK(contains(emptyText.out, "nes                  2    0.0    0.0"));
    const RunResult rocket = cli.media({"--system", "nes", "--path", "Rocket Ranger (USA).nes"});
    CHECK(rocket.code == ExitCode::Ok);
    CHECK(contains(rocket.out, "Media         0 of 12 kinds\n"));

    // Missing: nothing can be known.
    const std::string committed =
        R"(name="MediaDirectory" value=")" + rv::core::pathToUtf8(fixture.root()) + R"(\media")";
    REQUIRE(fixture
                .replaceText("ES-DE/settings/es_settings.xml", committed,
                             R"(name="MediaDirectory" value=")" + rv::core::pathToUtf8(fixture.root()) +
                                 R"(\unplugged\media")")
                .has_value());
    const auto checkUnavailable = [&](std::string_view part) {
        const RunResult coverage = cli.media({"--coverage", "--json"});
        INFO(coverage.err);
        CHECK(coverage.code == ExitCode::ProblemsFound);
        const nlohmann::json unknown = parsed(coverage.out);
        CHECK(unknown["total"]["gamelist-entries"]["entries"] == 2);
        CHECK(unknown["total"]["gamelist-entries"]["unknown"] == 2);
        CHECK(unknown["total"]["games"]["entries"] == 3);
        CHECK(unknown["total"]["games"]["unknown"] == 3);
        REQUIRE(unknown["systems"].size() == 2);
        CHECK(unknown["systems"][0]["mediaFolder"].is_null());
        CHECK(unknown["systems"][1]["mediaFolder"].is_null());
        REQUIRE_FALSE(unknown["issues"].empty());
        CHECK(unknown["issues"][0]["code"] == "media-root-unavailable");
        CHECK(unknown["issues"][0]["severity"] == "error");
        CHECK(contains(unknown["issues"][0]["reason"].get<std::string>(), part));
        const RunResult text = cli.media({"--coverage"});
        CHECK(text.code == ExitCode::ProblemsFound);
        CHECK(contains(text.out, "nes                  0      -      -"));
        CHECK(contains(text.out, "(2 unknown)"));
        const RunResult path = cli.media({"--system", "nes", "--path", "Rocket Ranger (USA).nes"});
        CHECK(path.code == ExitCode::ProblemsFound);
        CHECK(path.out.empty());
        CHECK(contains(path.err, "the media of nes cannot be known"));
    };
    checkUnavailable("the media root does not exist");

    // On a drive letter with no drive behind it.
    if (const wchar_t letter = unusedDriveLetter(); letter != 0) {
        const std::string drive = rv::core::pathToUtf8(fs::path(std::wstring{letter, L':'}));
        REQUIRE(fixture
                    .replaceText("ES-DE/settings/es_settings.xml",
                                 rv::core::pathToUtf8(fixture.root()) + R"(\unplugged\media)", drive + R"(\media)")
                    .has_value());
        checkUnavailable("its drive or share is not available");
    }

    // No library.json.
    const rv::test::TempDir other("rv-cli-media-none");
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code =
        rv::cli::run(std::vector<std::string>{"--data-dir", utf8(other.file("data")), "media", "--coverage"}, out, err,
                     testEnvironment(other.path()));
    CHECK(code == ExitCode::UsageError);
    CHECK(contains(err.text(), "no library yet"));
    CHECK(out.text().empty());
}
