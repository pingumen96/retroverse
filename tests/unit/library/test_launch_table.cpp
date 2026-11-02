// The launch table (I7 step 4, ADR-0018 §5): what `retroverse-cli profiles` prints, read through rv_library: the rows against
// the command's JSON on the standard fixture, the suggestion's order, a system the library no longer configures; the
// programs a folder lists.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/library/folder_listing.hpp>
#include <rv/library/launch_library.hpp>
#include <rv/library/launch_table.hpp>

#include "support/fixture.hpp"
#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::catalog::LaunchReadiness;

namespace {

rv::catalog::LibraryProfile libraryOf(const rv::test::Fixture& fixture) {
    rv::catalog::LibraryProfile library;
    library.id = rv::catalog::LibraryId::parse("esde").value();
    library.esdeHome = fixture.path("ES-DE");
    library.installFolder = fixture.path("Programs/ES-DE");
    return library;
}

rv::core::Paths pathsOf(const fs::path& data) {
    rv::core::PathOptions options;
    options.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return rv::core::Paths::resolve(options, environment).value();
}

} // namespace

TEST_CASE("library/launch table: its rows are what retroverse-cli profiles prints", "[library][launch_table]") {
    const auto fixture = rv::test::Fixture::materialize("esde/standard").value();
    const rv::test::TempDir data("rv-table");
    const rv::core::Paths paths = pathsOf(data.path());
    REQUIRE(rv::esde::writeLibraryFile(data.file("config/library.json"), libraryOf(fixture),
                                       rv::esde::ExistingFile::Keep)
                .has_value());
    auto library = rv::library::loadLaunchLibrary(libraryOf(fixture), paths);
    REQUIRE(library.has_value());
    const std::vector<rv::library::SystemLaunchRow> table = rv::library::launchTable(**library);

    const fs::path out = data.file("profiles.json");
    rv::cli::Output stdOut = rv::cli::Output::memory();
    rv::cli::Output stdErr = rv::cli::Output::memory();
    const std::vector<std::string> arguments{"--data-dir", rv::core::pathToUtf8(data.path()), "profiles", "--json",
                                             "--output",   rv::core::pathToUtf8(out)};
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    REQUIRE(rv::cli::run(arguments, stdOut, stdErr, environment) == rv::cli::ExitCode::Ok);
    const nlohmann::json report = nlohmann::json::parse(rv::core::readTextFile(out).value());

    const auto& systems = report.at("systems");
    REQUIRE(systems.size() == table.size());
    for (std::size_t index = 0; index < table.size(); ++index) {
        const rv::library::SystemLaunchRow& row = table[index];
        const nlohmann::json& shown = systems.at(index);
        INFO(row.system.name.str());
        CHECK(shown.at("name") == row.system.name.str());
        CHECK(shown.at("readiness").get<std::string>() == rv::catalog::toString(row.resolution.readiness()));
        CHECK(shown.at("chosen").at("value") == row.resolution.chosen.choice.value);
        CHECK(shown.at("chosen").at("source").get<std::string>() == rv::catalog::toString(row.resolution.chosen.source));
        REQUIRE(shown.at("commands").size() == row.commands.size());
        for (std::size_t option = 0; option < row.commands.size(); ++option) {
            CHECK(shown.at("commands").at(option).at("label") == row.commands[option].choice.value);
            CHECK(shown.at("commands").at(option).at("command") == row.commands[option].text);
            CHECK(shown.at("commands").at(option).at("readiness").get<std::string>() ==
                  rv::catalog::toString(row.commands[option].translation.readiness()));
        }
        REQUIRE(shown.at("profiles").size() == row.profiles.size());
        for (std::size_t option = 0; option < row.profiles.size(); ++option) {
            CHECK(shown.at("profiles").at(option).at("id") == row.profiles[option].choice.value);
        }
    }
}

TEST_CASE("library/launch table: the ready options first; a system no longer configured has none",
          "[library][launch_table]") {
    const auto fixture = rv::test::Fixture::materialize("esde/standard").value();
    const rv::test::TempDir data("rv-table");
    auto library = rv::library::loadLaunchLibrary(libraryOf(fixture), pathsOf(data.path()));
    REQUIRE(library.has_value());
    const auto rows = rv::library::launchTable(**library, "gb");
    REQUIRE(rows.size() == 1);
    const std::vector<rv::library::LaunchOption> options = rv::library::suggestedOptions(rows.front());
    REQUIRE(options.size() == rows.front().commands.size() + rows.front().profiles.size());
    const auto readyOf = [](const rv::library::LaunchOption& option) {
        return option.translation.readiness() == LaunchReadiness::Ready;
    };
    CHECK(std::is_partitioned(options.begin(), options.end(), readyOf));
    CHECK(readyOf(options.front()));
    CHECK_FALSE(readyOf(options.back()));

    rv::catalog::SystemInfo gone;
    gone.name = rv::catalog::SystemName::parse("nothere").value();
    const rv::library::SystemLaunchRow row = rv::library::systemLaunchRow(**library, gone, {});
    CHECK(row.resolution.readiness() == LaunchReadiness::RequiresConfiguration);
    CHECK(row.commands.empty());
    CHECK(row.profiles.empty());
    REQUIRE_FALSE(row.resolution.translation.blockers.empty());
    CHECK(row.resolution.translation.blockers.front().code == "system-not-configured");
}

TEST_CASE("library/folders: a folder's programs come after its folders, in order, and only those", "[library]") {
    const rv::test::TempDir dir("rv-programs");
    fs::create_directories(dir.file("Zeta"));
    fs::create_directories(dir.file("alpha"));
    rv::test::writeProgramStub(dir.file("Beta.EXE"));
    rv::test::writeProgramStub(dir.file("aleph.exe"));
    rv::test::writeProgramStub(dir.file("notes.txt"));
    const auto programs = rv::library::listPrograms(dir.path());
    REQUIRE(programs.has_value());
    std::vector<std::string> names;
    for (const rv::library::ListedFolder& entry : *programs) {
        names.push_back(entry.name + (entry.program ? "*" : ""));
    }
    CHECK(names == std::vector<std::string>{"alpha", "Zeta", "aleph.exe*", "Beta.EXE*"});
    const auto folders = rv::library::listFolders(dir.path());
    REQUIRE(folders.has_value());
    CHECK(folders->size() == 2);
    CHECK(rv::library::listPrograms(dir.file("gone")).error().code == rv::ErrorCode::NotFound);
}
