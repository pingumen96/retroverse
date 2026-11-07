// The real library's programs (I11 step 12, ADR-0020 §1): which of emulators.json's programs each system's launch
// starts, as `retroverse-cli profiles` reports it, and what a launch in VR would change for each program in use.
// Hidden ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots
// of the ES-DE home, the install and the roots before and after (0 differences), and the folders in the RV_REAL_*
// variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][programs]'
// The library is recorded (library create, no import: profiles reads ES-DE's configuration) in a data directory under
// the build tree (build/<preset>/real-library/programs/data, emptied first). The figures go to stdout ("Real programs:"
// lines, for docs/i11/step-12.md); the report stays in that folder's output file, never under %TEMP% or in the
// repository.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <fmt/ranges.h>
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

TEST_CASE("cli/profiles: the programs the real library's launches start (I11 step 12)",
          "[cli][launch][programs][.real-library]") {
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
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "programs";
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

    const fs::path output = folder / "profiles.json.txt";
    const auto [listed, listedOut, listedErr] = run({"profiles", "--json", "--output", rv::core::pathToUtf8(output)});
    INFO(listedErr);
    REQUIRE(listed == ExitCode::Ok);
    auto text = rv::core::readTextFile(output);
    REQUIRE(text.has_value());
    const auto json = rv::core::parseJson(*text, "profiles");
    REQUIRE(json.has_value());

    // The programs in use, with their systems; the systems whose program is none of emulators.json's.
    std::map<std::string, std::vector<std::string>> inUse;
    for (const nlohmann::json& system : json->at("systems")) {
        const nlohmann::json& program = system.at("programProfile");
        const std::string id = program.is_null() ? std::string() : program.get<std::string>();
        std::string entry = system.at("name").get<std::string>();
        if (id.empty() && system.at("program").is_object()) {
            entry += " (" +
                     rv::core::pathToUtf8(
                         rv::core::pathFromUtf8(system.at("program").at("path").get<std::string>()).filename()) +
                     ")";
        } else if (id.empty()) {
            entry += " (" + system.at("readiness").get<std::string>() + ")";
        }
        inUse[id].push_back(std::move(entry));
    }
    std::cout << fmt::format("Real programs: {} system(s), {} program(s) known\n", json->at("systems").size(),
                             json->at("programs").size());
    for (const auto& [id, systems] : inUse) {
        std::cout << fmt::format("Real programs: {}: {} system(s): {}\n", id.empty() ? "(none known)" : id,
                                 systems.size(), fmt::join(systems, ", "));
    }
    CHECK(json->at("programs").size() >= 8);
    CHECK_FALSE(inUse.empty());
}
