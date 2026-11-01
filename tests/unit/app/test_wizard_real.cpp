// The wizard on the real library (I7 step 2, ADR-0018 §4). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does (Release by default), with ES-DE closed, read-only snapshots of the library before and
// after, and the folders in the RV_REAL_* variables (test_import_real.cpp lists them). retroverse.exe of the same build,
// hidden and frame-limited, takes the seven pages from --esde-home with the pad's actions alone, its data under the build
// tree: the preview's counts are those of library::previewLibrary(), import --dry-run's reading, read here beside it. Nothing
// may be launched (RV_LAUNCH_ALLOWLIST set and empty).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include <fmt/format.h>

#include <rv/app/run_report.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/library/library_preview.hpp>

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

/// Avanti six times, Entra: a lockstep run waits for what each page waits for.
constexpr std::string_view kPagesScript = R"([
  {"fromFrame": 5, "toFrame": 5, "actions": {"ui_activate": 1}},
  {"fromFrame": 10, "toFrame": 10, "actions": {"ui_activate": 1}},
  {"fromFrame": 15, "toFrame": 15, "actions": {"ui_activate": 1}},
  {"fromFrame": 20, "toFrame": 20, "actions": {"ui_activate": 1}},
  {"fromFrame": 25, "toFrame": 25, "actions": {"ui_activate": 1}},
  {"fromFrame": 30, "toFrame": 30, "actions": {"ui_activate": 1}},
  {"fromFrame": 35, "toFrame": 35, "actions": {"ui_activate": 1}}
]
)";

} // namespace

TEST_CASE("wizard on the real library: the preview and the first import from --esde-home",
          "[.real-library][real-library][wizard_real]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    REQUIRE(home.has_value());
    REQUIRE(install.has_value());
    const fs::path retroverse = rv::test::testProgramsDir() / "retroverse.exe";

    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse(rv::catalog::LibraryId::kDefault).value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    const auto start = std::chrono::steady_clock::now();
    const auto preview = rv::library::previewLibrary(profile);
    REQUIRE(preview.has_value());
    std::cout << fmt::format("preview: {:.1f} s, {} entries, {} games\n",
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                             preview->total.entries, preview->total.games);

    // A user data directory under the build tree, never in the library.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "wizard";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    fs::create_directories(folder);
    const fs::path script = folder / "pages.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, kPagesScript).has_value());
    const fs::path report = folder / "run.json";
    rv::test::ChildRequest request;
    request.executable = retroverse;
    request.arguments = {"--wizard",
                         "--esde-home",
                         rv::core::pathToUtf8(*home),
                         "--esde-install",
                         rv::core::pathToUtf8(*install),
                         "--hidden",
                         "--exit-after-frames",
                         "--frames",
                         "60",
                         "--fixed-dt",
                         "0.016667",
                         "--data-dir",
                         rv::core::pathToUtf8(folder / "data"),
                         "--run-report",
                         rv::core::pathToUtf8(report),
                         "--input-script",
                         rv::core::pathToUtf8(script)};
    request.workingDirectory = folder;
    request.stdoutFile = folder / "run.log";
    request.stderrFile = request.stdoutFile;
    request.timeout = std::chrono::minutes(40);
    // Nothing may start: the allow-list set and empty; the run reads its own deployed assets.
    request.environment = {{"RV_LAUNCH_ALLOWLIST", std::string()}, {"RV_ASSET_ROOT", std::nullopt}, {"RV_AUDIO", "null"}};
    const auto outcome = rv::test::runChild(request);
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->timedOut);
    CHECK(outcome->exitCode == 0);
    const auto read = rv::app::readRunReport(report);
    REQUIRE(read.has_value());
    REQUIRE(read->wizard.has_value());
    const rv::app::WizardRecord& wizard = *read->wizard;
    CHECK(wizard.entered);
    CHECK(wizard.previewWork == "done");
    CHECK(wizard.importWork == "done");
    CHECK(wizard.systems == preview->snapshot.systems.size());
    CHECK(wizard.entries == preview->total.entries);
    CHECK(wizard.games == preview->total.games);
    CHECK(wizard.esdeGames == preview->total.esde);
    CHECK(wizard.notAGame == preview->total.notAGame);
    CHECK(wizard.discMembers == preview->total.members);
    REQUIRE(read->catalog.has_value());
    REQUIRE(read->catalog->refreshes.size() == 1);
    CHECK(read->catalog->refreshes.front().trigger == "wizard");
    CHECK(read->catalog->refreshes.front().outcome == "done");
    CHECK(read->world.state == "room");
    std::cout << fmt::format("wizard: {} systems, {} with games, {} games, {} with media, {} not games, {} errors; the "
                             "first import: {} added in {:.1f} s\n",
                             wizard.systems, wizard.systemsWithGames, wizard.games, wizard.media, wizard.notAGame,
                             wizard.errors, wizard.added, read->catalog->refreshes.front().ms.value_or(0.0) / 1000.0);
}
