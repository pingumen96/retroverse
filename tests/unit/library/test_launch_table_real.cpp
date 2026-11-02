// The launch table on the real library (I7 step 4, ADR-0018 §5, §16). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the folders
// in the RV_REAL_* variables (test_import_real.cpp lists them). Each system's readiness in the table is what `retroverse-cli
// profiles` says, read as a process's JSON; nothing is launched; the table's time is the emulators screen's on the worker.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/library.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/library/launch_library.hpp>
#include <rv/library/launch_table.hpp>

#include "support/child_process.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

} // namespace

TEST_CASE("launch table on the real library: every system's readiness as profiles says, within the screen's second",
          "[.real-library][real-library][emulators_real]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    REQUIRE(home.has_value());
    REQUIRE(install.has_value());
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "emulators";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    fs::create_directories(folder);

    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse(rv::catalog::LibraryId::kDefault).value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    REQUIRE(rv::esde::writeLibraryFile(folder / "config" / "library.json", profile, rv::esde::ExistingFile::Keep)
                .has_value());

    rv::core::PathOptions options;
    options.userDataOverride = folder;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const rv::core::Paths paths = rv::core::Paths::resolve(options, environment).value();
    auto library = rv::library::loadLaunchLibrary(profile, paths);
    REQUIRE(library.has_value());

    // As the worker makes it: the configuration read once, each row without its gamelist.
    const auto start = std::chrono::steady_clock::now();
    std::size_t rows = 0;
    for (const rv::catalog::SystemInfo& system : (*library)->systemInfos) {
        static_cast<void>(rv::library::systemLaunchRow(**library, system, {}));
        ++rows;
    }
    const double tableMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    const std::vector<rv::library::SystemLaunchRow> table = rv::library::launchTable(**library);
    const fs::path report = folder / "profiles.json";
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / "retroverse-cli.exe";
    request.arguments = {"--data-dir", rv::core::pathToUtf8(folder), "profiles", "--json", "--output",
                         rv::core::pathToUtf8(report)};
    request.workingDirectory = folder;
    request.stdoutFile = folder / "profiles.log";
    request.stderrFile = request.stdoutFile;
    request.environment = {{"RV_LAUNCH_ALLOWLIST", std::string()}}; // nothing may start
    const auto ran = rv::test::runChild(request);
    REQUIRE(ran.has_value());
    REQUIRE(ran->exitCode == 0);
    const nlohmann::json printed = nlohmann::json::parse(rv::core::readTextFile(report).value());

    std::map<std::string, std::size_t> counts;
    REQUIRE(printed.at("systems").size() == table.size());
    for (std::size_t index = 0; index < table.size(); ++index) {
        const std::string readiness(rv::catalog::toString(table[index].resolution.readiness()));
        INFO(table[index].system.name.str());
        CHECK(printed.at("systems").at(index).at("readiness") == readiness);
        CHECK(printed.at("systems").at(index).at("commands").size() == table[index].commands.size());
        ++counts[readiness];
    }
    std::string summary;
    for (const auto& [readiness, count] : counts) {
        summary += fmt::format(" {}={}", readiness, count);
    }
    std::cout << fmt::format("Real emulators: {} system(s) in {:.0f} ms,{}\n", rows, tableMs, summary);
    CHECK(tableMs < 1000.0);
}
