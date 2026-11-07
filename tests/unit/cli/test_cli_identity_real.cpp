// The real library's identities (I11 step 1, ADR-0019 §2): every system of the catalog with the name, logo and
// description read from the real ES-DE's themes (linear-es-de in the install, the active theme in the ES-DE home).
// Hidden ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots
// of the ES-DE home, the install and the roots before and after (0 differences), and the folders in the RV_REAL_*
// variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][identity]'
// The library is imported into a new catalog in a data directory under the build tree
// (build/<preset>/real-library/identity/data, emptied first). The figures go to stdout ("Real identity:" lines, for
// docs/i11/step-1.md); the identities themselves stay in that folder's output file, never under %TEMP% or in the
// repository.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
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
#include <rv/core/text.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

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

} // namespace

TEST_CASE("cli/identity: the real library's systems named, with logos and descriptions from ES-DE's themes",
          "[cli][identity][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    const LoggingReset loggingReset;
    // Under the build tree, never in the library nor in %TEMP%.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "identity";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    fs::create_directories(folder, removed);
    REQUIRE_FALSE(removed);
    rv::core::PathEnvironment environment;
    environment.executableDir = folder;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(folder / "data")});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return std::make_tuple(code, out.text(), err.text());
    };

    const auto [created, createdOut, createdErr] =
        run({"library", "create", "--home", rv::core::pathToUtf8(home->parent_path()), "--install",
             rv::core::pathToUtf8(*install), "--rom-root", rv::core::pathToUtf8(*romRoot), "--media-root",
             rv::core::pathToUtf8(*mediaRoot)});
    INFO(createdErr);
    REQUIRE(created == ExitCode::Ok);
    const auto [imported, importedOut, importedErr] = run({"import"});
    INFO(importedErr);
    REQUIRE(imported == ExitCode::Ok);

    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const fs::path output = folder / "identity.json.txt";
    const auto [shown, shownOut, shownErr] =
        run({"identity", "show", "--json", "--output", rv::core::pathToUtf8(output)});
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    INFO(shownErr);
    REQUIRE(shown == ExitCode::Ok);
    auto text = rv::core::readTextFile(output);
    REQUIRE(text.has_value());
    const auto json = rv::core::parseJson(*text, "identity show");
    REQUIRE(json.has_value());

    REQUIRE_FALSE((*json)["metadataThemes"].empty()); // linear-es-de in the install
    std::size_t systems = 0;
    std::size_t named = 0;
    std::size_t logos = 0;
    std::size_t svg = 0;
    std::size_t italian = 0;
    std::size_t coloured = 0;
    std::vector<std::string> fullNameOnly;
    for (const auto& system : (*json)["systems"]) {
        ++systems;
        CHECK_FALSE(system["name"]["value"].get<std::string>().empty());
        if (system["name"]["source"] == "theme" || system["name"]["source"] == "override") {
            ++named;
        } else {
            fullNameOnly.push_back(system["system"].get<std::string>());
        }
        if (system["logo"]["source"] != "none") {
            ++logos;
            svg += system["logo"]["format"] == "svg" ? 1 : 0;
        }
        italian += system["descriptionLanguage"] == "it_IT" ? 1 : 0;
        coloured += system["colors"].empty() ? 0 : 1;
    }
    CHECK(systems > 0);
    CHECK(named > 0);
    std::cout << fmt::format("Real identity: {} systems, {} named by a theme, {} with a logo ({} SVG), {} with an "
                             "Italian description, {} with colours; identity show in {:.2f} s\n",
                             systems, named, logos, svg, italian, coloured, seconds);
    std::string unnamed;
    for (const std::string& system : fullNameOnly) {
        unnamed += unnamed.empty() ? system : ", " + system;
    }
    std::cout << fmt::format("Real identity: named by ES-DE's full name only: {}\n",
                             unnamed.empty() ? std::string("none") : unnamed);
}
