// The real library's clips through retroverse-cli media --check-videos (I5 step 7, ADR-0016 §8: the library-wide check
// of ADR-0004's thresholds). Hidden ([.real-library]): scripts/real-library.ps1 runs it with ES-DE closed, read-only
// snapshots of the library before and after (0 differences: the check only reads the clips), and the folders in the
// RV_REAL_* variables. The SNES clips by default; every system's with RV_REAL_FULL_VIDEO_SCAN=1 (real-library.ps1
// -FullVideoScan: hours). The data directory, build/<preset>/real-library/videos/data, is kept between runs: a run
// resumes where the previous one stopped (delete the folder to check every clip again). The reports (JSON and text) are
// left there; the verdicts per kind and the failures are printed, for the step note. Nothing is expected of the
// verdicts but that every clip gets one.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/library.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>

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

TEST_CASE("cli/media --check-videos: the real library's clips, the SNES ones or every system's",
          "[.real-library][real-library][videos]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    const bool full = pathVariable(L"RV_REAL_FULL_VIDEO_SCAN").has_value();
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("real").value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;

    // The data directory under the build tree, never in the library; kept, so that a run resumes.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "videos";
    const fs::path data = folder / "data";
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / std::string(rv::esde::kLibraryFileName), profile,
                                       rv::esde::ExistingFile::Replace)
                .has_value());
    const LoggingReset loggingReset;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(data), "media", "--check-videos"});
        if (!full) {
            arguments.insert(arguments.end(), {"--system", "snes"});
        }
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        INFO(err.text());
        CHECK(code == ExitCode::Ok);
        return out.text();
    };

    const std::string scope = full ? "library" : "snes";
    const auto start = std::chrono::steady_clock::now();
    const std::string json = run({"--json", "--jobs", full ? "3" : "2"});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(rv::core::writeTextFileAtomic(folder / (scope + ".json"), json).has_value());
    const auto report = rv::core::parseJson(json, "media --check-videos");
    REQUIRE(report.has_value());
    // The same as text (every clip known now): the table and the failures, for the step note.
    const std::string text = run({});
    REQUIRE(rv::core::writeTextFileAtomic(folder / (scope + ".txt"), text).has_value());

    const nlohmann::json& total = (*report)["total"];
    std::cout << fmt::format("Real videos ({}): {} clip(s), {} checked now in {:.1f} s, {} known, {} without a "
                             "verdict\n",
                             scope, total["clips"].get<std::uint64_t>(), total["checked"].get<std::uint64_t>(), seconds,
                             total["known"].get<std::uint64_t>(), total["unchecked"].get<std::uint64_t>());
    for (const auto& [verdict, count] : total["verdicts"].items()) {
        std::cout << fmt::format("Real videos ({}): {} {}\n", scope, verdict, count.get<std::uint64_t>());
    }
    std::cout << text;
    CHECK(total["clips"].get<std::uint64_t>() > 0);
    CHECK(total["unchecked"] == 0);
    CHECK((*report)["incomplete"].empty());
}
