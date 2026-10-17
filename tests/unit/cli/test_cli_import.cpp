// retroverse-cli import (I2 step 7: --dry-run) through the dispatcher, on the standard ES-DE fixture: the counts by
// system and state as text and JSON, --system, the refusals (no --dry-run, no library.json, an unknown system), and a
// dry run that writes nothing but its log file.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/library.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/discovery.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

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

RunResult runCli(const std::vector<std::string>& arguments, const fs::path& executableDir) {
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(executableDir));
    return RunResult{code, out.text(), err.text()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The files and folders under `root` (relative paths, files with their size).
std::vector<std::string> treeOf(const fs::path& root) {
    const auto listed = rv::core::listTree(root);
    REQUIRE(listed.has_value());
    std::vector<std::string> tree;
    for (const auto& entry : *listed) {
        tree.push_back(entry.relativePath +
                       (entry.kind == rv::core::TreeEntryKind::File ? " " + std::to_string(entry.size) : "/"));
    }
    return tree;
}

} // namespace

TEST_CASE("cli/import: --dry-run prints the standard fixture's entries by system and state", "[cli][import]") {
    const LoggingReset loggingReset;
    auto fixture = rv::test::Fixture::materialize("esde/standard");
    REQUIRE(fixture.has_value());
    const rv::test::TempDir dir("rv-cli-import");
    const fs::path data = dir.file("data");
    const std::vector<std::string> global{"--data-dir", rv::core::pathToUtf8(data)};
    const auto withGlobal = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), global.begin(), global.end());
        return arguments;
    };

    // Without a library.json there is nothing to read.
    const RunResult none = runCli(withGlobal({"import", "--dry-run"}), dir.path());
    CHECK(none.code == ExitCode::UsageError);
    CHECK(contains(none.err, "no library yet"));
    CHECK(none.out.empty());

    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture->path("ES-DE");
    profile.installFolder = fixture->path("Programs/ES-DE");
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", profile, rv::esde::ExistingFile::Keep));
    const std::vector<std::string> before = treeOf(fixture->root());

    SECTION("text") {
        const RunResult result = runCli(withGlobal({"import", "--dry-run"}), dir.path());
        INFO(result.err);
        CHECK(result.code == ExitCode::Ok);
        CHECK(contains(result.out, "(dry run: nothing is written)"));
        // System, entries, games, missing, not game, members, folders, outside, unavailable, groups, issues.
        CHECK(contains(result.out, "gb                     11        8        1        0        0        1        1"
                                   "        0        0        1  available, read"));
        // psx's issue: the "CUE" quirk of its extensions (es_systems.xml).
        CHECK(contains(result.out, "psx                     4        2        0        0        2        0        0"
                                   "        0        1        1  available, read"));
        CHECK(contains(result.out, "snes                    8        5        0        1        0        2        0"
                                   "        0        0        0  available, read"));
        CHECK(contains(result.out, "Total                  23       15        1        1        2        3        1"));
        CHECK(contains(result.out, "games as ES-DE lists them (the games and disc members on disk): 17"));
        CHECK(contains(result.out, "  snes: extension-not-configured 1"));
        CHECK(contains(result.out, "[warning] gamelist-path-outside-folder gb:"));
        CHECK(contains(result.out, "Read in "));
        CHECK(treeOf(fixture->root()) == before);
        // It writes nothing but its log file.
        const std::vector<std::string> written = treeOf(data);
        for (const std::string& path : written) {
            INFO(path);
            CHECK((path.starts_with("config") || path.starts_with("logs")));
        }
        CHECK(fs::exists(data / "logs" / "retroverse-cli.log"));
    }
    SECTION("JSON, one system") {
        const RunResult result = runCli(withGlobal({"import", "--dry-run", "--system", "psx", "--json"}), dir.path());
        INFO(result.err);
        CHECK(result.code == ExitCode::Ok);
        auto json = rv::core::parseJson(result.out, "stdout");
        REQUIRE(json.has_value());
        CHECK((*json)["dryRun"] == true);
        REQUIRE((*json)["systems"].size() == 1);
        const auto& psx = (*json)["systems"][0];
        CHECK(psx["name"] == "psx");
        CHECK(psx["gamelist"] == "read");
        CHECK(psx["entries"] == 4);
        CHECK(psx["games"] == 2);
        CHECK(psx["discMembers"] == 2);
        CHECK(psx["discGroups"] == 1);
        CHECK((*json)["totals"]["entries"] == 4);
        CHECK((*json)["statistics"]["sheetsRead"] == 1);
    }
    SECTION("refusals") {
        const RunResult stub = runCli(withGlobal({"import"}), dir.path());
        CHECK(stub.code == ExitCode::UsageError);
        CHECK(stub.err == "retroverse-cli import: not implemented yet (I2 step 11)\n");
        const RunResult unknown = runCli(withGlobal({"import", "--dry-run", "--system", "nosuch"}), dir.path());
        CHECK(unknown.code == ExitCode::UsageError);
        CHECK(contains(unknown.err, "nosuch"));
        CHECK(unknown.out.empty());
        const RunResult option = runCli(withGlobal({"import", "--dry-run", "--bogus"}), dir.path());
        CHECK(option.code == ExitCode::UsageError);
    }
}
