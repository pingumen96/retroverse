// retroverse-cli discover and library (I2 step 2) through the dispatcher, on the ES-DE fixtures: the text and JSON
// results; discover writes nothing; the scraper credentials are never shown, logged or recorded; library create's
// library.json and its refusals; library show.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
#include <rv/esde/discovery.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

constexpr std::string_view kUser = "fixture-scraper-user-Q7x4";
constexpr std::string_view kPassword = "fixture-scraper-password-K9w2";

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

RunResult runCli(const std::vector<std::string>& arguments, const fs::path& executableDir) {
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(executableDir));
    return RunResult{code, out.text(), err.text()};
}

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

/// Every folder under `root`, and every file with its size and the SHA-256 of its content: what "writes nothing"
/// compares (times are left out: NTFS updates the copies of a folder's times in its parent's listing lazily).
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

void checkNoCredentials(std::string_view text) {
    CHECK_FALSE(contains(text, kUser));
    CHECK_FALSE(contains(text, kPassword));
}

/// The names of a JSON array of objects, by key.
std::vector<std::string> namesOf(const nlohmann::json& array, std::string_view key) {
    std::vector<std::string> names;
    for (const auto& item : array) {
        names.push_back(item.at(std::string(key)).get<std::string>());
    }
    return names;
}

} // namespace

TEST_CASE("cli/discover: the standard fixture as JSON and as text; nothing written, no credential anywhere",
          "[cli][discover]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const rv::test::TempDir dir("rv-cli-discover");
    const LoggingReset loggingReset;
    const fs::path data = dir.file("data");
    const auto before = treeOf(fixture.root());
    const std::vector<std::string> common{"--data-dir",  utf8(data),
                                          "--log-level", "debug", // every log line reaches stderr too
                                          "discover",    "--home",
                                          utf8(fixture.root()), "--install", utf8(fixture.path("Programs/ES-DE"))};

    std::vector<std::string> json = common;
    json.push_back("--json");
    const RunResult jsonRun = runCli(json, dir.path());
    INFO(jsonRun.err);
    CHECK(jsonRun.code == ExitCode::Ok);
    const nlohmann::json report = parsed(jsonRun.out);
    CHECK(report["esdeHome"]["path"] == utf8(fixture.path("ES-DE")));
    CHECK(report["esdeHome"]["source"] == "explicit");
    CHECK(report["esdeHome"]["exists"] == true);
    CHECK(report["homeFolder"] == utf8(fixture.root()));
    CHECK(report["install"]["path"] == utf8(fixture.path("Programs/ES-DE")));
    CHECK(report["install"]["source"] == "explicit");
    CHECK(report["install"]["executableFound"] == true);
    CHECK(report["portable"]["file"].is_null());
    CHECK(report["settings"]["source"] == "file");
    // As the settings write it: the materializer's text of the fixture's folder.
    CHECK(report["settings"]["values"]["ROMDirectory"] == rv::core::pathToUtf8(fixture.root()) + "\\roms");
    CHECK(report["settings"]["values"]["ShowHiddenGames"] == true);
    CHECK(report["settings"]["values"]["UIMode"] == "full");
    CHECK(report["romRoot"]["path"] == utf8(fixture.path("roms")));
    CHECK(report["romRoot"]["source"] == "setting");
    CHECK(report["mediaRoot"]["path"] == utf8(fixture.path("media")));
    CHECK(report["findRules"]["emulators"] == 4);
    CHECK(report["findRules"]["cores"] == 1);
    CHECK(report["findRules"]["entries"] == 10);
    CHECK(report["systems"]["configured"] == 3);
    CHECK(report["systems"]["custom"] == 0);
    CHECK(report["systems"]["commands"] == 10);
    CHECK(namesOf(report["systems"]["withRomFolder"], "name") == std::vector<std::string>{"gb", "psx", "snes"});
    CHECK(report["systems"]["withRomFolder"][0]["romFolder"] == utf8(fixture.path("roms") / "gb"));
    CHECK(namesOf(report["gamelists"], "system") == std::vector<std::string>{"gb", "psx", "snes"});
    CHECK(report["gamelists"][1]["file"] == utf8(fixture.path("ES-DE") / "gamelists" / "psx" / "gamelist.xml"));
    CHECK(namesOf(report["issues"], "code") == std::vector<std::string>{"extension-without-dot"});

    const RunResult textRun = runCli(common, dir.path());
    CHECK(textRun.code == ExitCode::Ok);
    for (const std::string_view line :
         {"ES-DE home        ", "Home folder (~)   ", "Install folder    ", "Settings          ", "ROM root          ",
          "Media root        ", "Find rules        4 emulators, 1 core, 10 entries",
          "Systems           3 configured (0 custom), 10 commands; 3 with a ROM folder:", "Gamelists         3 found:",
          "  ROMDirectory                ", "  UIMode                      full", "Issues            1:"}) {
        CHECK(contains(textRun.out, line));
    }

    // Read-only: the fixture is as it was, nothing but the log file in the data directory.
    CHECK(treeOf(fixture.root()) == before);
    CHECK_FALSE(fs::exists(data / "config"));
    const auto log = rv::core::readTextFile(data / "logs" / "retroverse-cli.log");
    REQUIRE(log.has_value());
    CHECK(contains(*log, "discover: ES-DE home found (explicit)"));
    for (const std::string_view text : {std::string_view(jsonRun.out), std::string_view(jsonRun.err),
                                        std::string_view(textRun.out), std::string_view(textRun.err),
                                        std::string_view(*log)}) {
        checkNoCredentials(text);
    }
}

TEST_CASE("cli/discover: a portable installation, --output, problems and invalid options", "[cli][discover]") {
    const rv::test::Fixture fixture = materialize("esde/portable");
    const rv::test::TempDir dir("rv-cli-discover-portable");
    const LoggingReset loggingReset;
    const std::string data = utf8(dir.file("data"));

    // The install folder holds portable.txt: the ES-DE home is its ES-DE folder, the roots ES-DE's defaults there.
    const fs::path output = dir.file("out/discover.json");
    const RunResult portable = runCli({"--data-dir", data, "discover", "--install", utf8(fixture.path("ES-DE")),
                                       "--json", "--output", utf8(output)},
                                      dir.path());
    INFO(portable.err);
    CHECK(portable.code == ExitCode::Ok);
    CHECK(portable.out.empty());
    const auto written = rv::core::readTextFile(output);
    REQUIRE(written.has_value());
    const nlohmann::json report = parsed(*written);
    CHECK(report["esdeHome"]["path"] == utf8(fixture.path("ES-DE/ES-DE")));
    CHECK(report["esdeHome"]["source"] == "portable");
    CHECK(report["portable"]["inUse"] == true);
    CHECK(report["romRoot"]["path"] == utf8(fixture.path("ES-DE/ROMs")));
    CHECK(report["romRoot"]["source"] == "default");
    CHECK(report["mediaRoot"]["path"] == utf8(fixture.path("ES-DE/ES-DE/downloaded_media")));
    CHECK(namesOf(report["systems"]["withRomFolder"], "name") == std::vector<std::string>{"nes"});
    CHECK(report["systems"]["configured"] == 2);
    CHECK_FALSE(contains(*written, "portable-scraper"));
    CHECK_FALSE(contains(portable.err, "portable-scraper"));

    // A home without ES-DE: exit 1 with the problem named.
    const RunResult missing = runCli({"--data-dir", data, "discover", "--home", utf8(fixture.path("Users")),
                                      "--install", utf8(fixture.path("ES-DE"))},
                                     dir.path());
    CHECK(missing.code == ExitCode::ProblemsFound);
    CHECK(contains(missing.out, "error    home-missing"));

    for (const std::vector<std::string>& invalid :
         {std::vector<std::string>{"--data-dir", data, "discover", "--bogus"},
          std::vector<std::string>{"--data-dir", data, "discover", "--home="},
          std::vector<std::string>{"--data-dir", data, "discover", "--output="},
          std::vector<std::string>{"--data-dir", data, "discover", "extra"}}) {
        const RunResult result = runCli(invalid, dir.path());
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
        CHECK(contains(result.err, "retroverse-cli discover:"));
    }
    const RunResult help = runCli({"--data-dir", data, "discover", "--help"}, dir.path());
    CHECK(help.code == ExitCode::Ok);
    CHECK(contains(help.out, "--home <dir>"));
}

TEST_CASE("cli/library: create records the library, show prints it; what create refuses", "[cli][library]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const rv::test::TempDir dir("rv-cli-library");
    const LoggingReset loggingReset;
    const fs::path data = dir.file("data");
    const fs::path file = data / "config" / "library.json";
    const std::vector<std::string> where{"--home", utf8(fixture.root()), "--install",
                                         utf8(fixture.path("Programs/ES-DE"))};
    const auto library = [&](std::vector<std::string> arguments, const fs::path& dataDir) {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(dataDir), "library"});
        return runCli(arguments, dir.path());
    };
    const auto with = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.end(), where.begin(), where.end());
        return arguments;
    };

    const RunResult none = library({"show"}, data);
    CHECK(none.code == ExitCode::ProblemsFound);
    CHECK(contains(none.err, "no library yet"));

    const RunResult created = library(with({"create"}), data);
    INFO(created.err);
    CHECK(created.code == ExitCode::Ok);
    CHECK(contains(created.out, "Library           esde ("));
    CHECK(contains(created.out, "ROM root          " + utf8(fixture.path("roms"))));
    const auto profile = rv::esde::readLibraryFile(file);
    REQUIRE(profile.has_value());
    CHECK(profile->id.str() == "esde");
    CHECK(profile->esdeHome == fixture.path("ES-DE"));
    CHECK(profile->installFolder == fixture.path("Programs/ES-DE"));
    CHECK_FALSE(profile->romRoot.has_value());
    const auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    checkNoCredentials(*text);
    checkNoCredentials(created.out);

    // Never replaced without --force.
    const RunResult again = library(with({"create", "--id", "other"}), data);
    CHECK(again.code == ExitCode::UsageError);
    CHECK(contains(again.err, "--force replaces it"));
    const auto unchanged = rv::core::readTextFile(file);
    REQUIRE(unchanged.has_value());
    CHECK(*unchanged == *text);
    const RunResult forced = library(with({"create", "--force", "--id", "my-games", "--rom-root",
                                           utf8(fixture.path("roms")), "--json"}),
                                     data);
    CHECK(forced.code == ExitCode::Ok);
    CHECK(parsed(forced.out)["id"] == "my-games");
    CHECK(rv::esde::readLibraryFile(file)->romRoot == fixture.path("roms"));

    const RunResult shown = library({"show", "--json"}, data);
    CHECK(shown.code == ExitCode::Ok);
    const nlohmann::json report = parsed(shown.out);
    CHECK(report["file"] == utf8(file));
    CHECK(report["id"] == "my-games");
    CHECK(report["adapter"] == "esde");
    CHECK(report["esdeHome"] == utf8(fixture.path("ES-DE")));
    CHECK(report["romRoot"] == utf8(fixture.path("roms")));
    CHECK(report["mediaRoot"].is_null());
    CHECK(report["effectiveRomRoot"]["source"] == "override");
    CHECK(report["effectiveMediaRoot"]["path"] == utf8(fixture.path("media")));
    CHECK(report["effectiveMediaRoot"]["source"] == "setting");
    const RunResult plain = library({}, data); // no action: show
    CHECK(plain.code == ExitCode::Ok);
    CHECK(contains(plain.out, "Library           my-games ("));

    // A user data directory inside the library's folders, an ES-DE home that is not there, an invalid id or action.
    for (const fs::path& inside : {fixture.path("roms/data"), fixture.path("media/rv"), fixture.path("ES-DE/rv"),
                                   fixture.path("Programs/ES-DE")}) {
        const RunResult refused = library(with({"create"}), inside);
        CHECK(refused.code == ExitCode::UsageError);
        CHECK(contains(refused.err, "may not lie inside the library's folders"));
        CHECK_FALSE(fs::exists(inside / "config" / "library.json"));
    }
    const RunResult noHome = library({"create", "--home", utf8(fixture.path("roms")), "--install",
                                      utf8(fixture.path("Programs/ES-DE"))},
                                     dir.file("data2"));
    CHECK(noHome.code == ExitCode::UsageError);
    CHECK(contains(noHome.err, "does not exist ("));
    CHECK(contains(noHome.err, "): give ES-DE's home folder with --home"));
    // The likely mistake, --home naming the ES-DE home itself, is named.
    const RunResult itself = library({"create", "--home", utf8(fixture.path("ES-DE")), "--install",
                                      utf8(fixture.path("Programs/ES-DE"))},
                                     dir.file("data4"));
    CHECK(itself.code == ExitCode::UsageError);
    CHECK(contains(itself.err, "looks like the ES-DE home itself"));
    CHECK(library(with({"create", "--id", "Bad Id"}), dir.file("data3")).code == ExitCode::UsageError);
    CHECK(library({"frobnicate"}, data).code == ExitCode::UsageError);
    CHECK(library({"show", "--bogus"}, data).code == ExitCode::UsageError);

    // A library.json that cannot be read.
    REQUIRE(rv::core::writeTextFileAtomic(file, "{ not json").has_value());
    const RunResult broken = library({"show"}, data);
    CHECK(broken.code == ExitCode::RuntimeFailure);
    CHECK(contains(broken.err, "parse-error"));
}
