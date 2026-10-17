// NFR-01 on the real library (I2 step 13): retroverse.exe started with an already indexed catalog shows the catalog's
// first page within 5 s on the reference PC. Hidden ([.real-library]): ctest never runs it; scripts/real-library.ps1
// does (Release by default), with ES-DE closed, read-only snapshots of the library before and after, and the folders in
// the RV_REAL_* variables (test_import_real.cpp lists them). The library is imported into a database under the build
// tree (build/<preset>/real-library/app, made anew), then retroverse.exe of the same build runs twice, hidden, without
// vsync and without --fixed-dt (no lockstep: its frames never wait for the catalog worker): the quick catalog opened on
// frame 1, then scrolled through the 90,246 games (a held direction, then held pages). Its run report gives, from the
// process's creation, the catalog opened on the worker, the first list and the first page shown, the page fetches on the
// main thread and the frame times while the catalog showed games. The 5 s bound is checked; the rest is printed
// (informative: the gate measures on a quiet machine). Nothing may be launched (RV_LAUNCH_ALLOWLIST set and empty).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <windows.h>

#include <fmt/format.h>

#include <rv/app/run_report.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

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

/// The catalog's frames (about 2 ms each without vsync): QuickCatalog on frame 1, a held direction from frame 300 to
/// 2999 (the list scrolls a game per repeat, at the held cadence), held pages from frame 3100 to 5799, End on frame
/// 5900 (the last of the 90,246 games), Home on frame 6000.
constexpr std::string_view kScrollScript = R"([
  {"fromFrame": 1, "toFrame": 1, "actions": {"quick_catalog": 1}},
  {"fromFrame": 300, "toFrame": 2999, "actions": {"ui_down": 1}},
  {"fromFrame": 3100, "toFrame": 5799, "actions": {"ui_page_down": 1}},
  {"fromFrame": 5900, "toFrame": 5900, "actions": {"ui_end": 1}},
  {"fromFrame": 6000, "toFrame": 6000, "actions": {"ui_home": 1}}
]
)";
constexpr std::string_view kFrames = "6100";

std::string timesText(const std::optional<rv::app::TimeSummary>& times) {
    if (!times) {
        return "none";
    }
    return fmt::format("{} samples, p50 {:.3f} ms, p95 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms", times->count,
                       times->p50Ms, times->p95Ms, times->p99Ms, times->maxMs);
}

std::string msText(const std::optional<double>& ms) {
    return ms ? fmt::format("{:.0f} ms", *ms) : std::string("never");
}

} // namespace

TEST_CASE("app/catalog: the real library's catalog in retroverse.exe: NFR-01, page fetches and frame times",
          "[.real-library][real-library][app]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    const fs::path retroverse = rv::test::testProgramsDir() / "retroverse.exe";
    REQUIRE(fs::exists(retroverse));
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("esde").value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;

    // A user data directory under the build tree, never in the library: library.json and the imported catalog.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "app";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", profile, rv::esde::ExistingFile::Keep)
                .has_value());
    {
        const auto start = std::chrono::steady_clock::now();
        rv::core::PathOptions options;
        options.userDataOverride = data;
        rv::core::PathEnvironment environment = rv::core::PathEnvironment::fromProcess();
        const rv::core::Paths paths = rv::core::Paths::resolve(options, environment).value();
        auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
        REQUIRE(database.has_value());
        auto store = rv::catalog_db::SqliteCatalogStore::open(*database).value();
        const auto settings = rv::esde::readSettings(profile.esdeHome);
        REQUIRE(settings.has_value());
        rv::esde::EsdeLibrarySource source;
        rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
        rv::catalog::IdentityPairing pairing;
        rv::catalog::ImportService service(source, media, *store, &pairing);
        const auto result = service.run(profile, rv::catalog::ImportOptions{});
        REQUIRE(result.has_value());
        std::cout << fmt::format(
            "import: {:.1f} s\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    const fs::path script = folder / "scroll.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, kScrollScript).has_value());

    for (int run = 1; run <= 2; ++run) {
        const fs::path report = folder / fmt::format("run{}.json", run);
        rv::test::ChildRequest request;
        request.executable = retroverse;
        request.arguments = {"--scene",        "m0_testroom", "--hidden", "--no-vsync",
                             "--exit-after-frames", "--frames",   std::string(kFrames),
                             "--data-dir",     rv::core::pathToUtf8(data),
                             "--run-report",   rv::core::pathToUtf8(report),
                             "--input-script", rv::core::pathToUtf8(script)};
        request.workingDirectory = folder;
        request.stdoutFile = folder / fmt::format("run{}.log", run);
        request.stderrFile = request.stdoutFile;
        // Nothing may start: the allow-list set and empty; the run reads its own deployed assets.
        request.environment = {{"RV_LAUNCH_ALLOWLIST", std::string()}, {"RV_ASSET_ROOT", std::nullopt}};
        const auto outcome = rv::test::runChild(request);
        REQUIRE(outcome.has_value());
        REQUIRE_FALSE(outcome->timedOut);
        CHECK(outcome->exitCode == 0);
        const auto read = rv::app::readRunReport(report);
        REQUIRE(read.has_value());
        REQUIRE(read->catalog.has_value());
        const rv::app::CatalogRecord& catalog = *read->catalog;
        CHECK(catalog.source == "esde");
        CHECK(catalog.notice.empty());
        CHECK(catalog.games == 90'246);
        std::cout << fmt::format(
            "run {}: {} games, {} systems; from the process's creation: catalog opened {}, first list {}, first page "
            "shown {}; page fetches: {}; frames while the catalog showed games: {}; selected at the end: position "
            "{}\n",
            run, catalog.games, catalog.systems, msText(catalog.openedMs), msText(catalog.firstListMs),
            msText(catalog.firstPageMs), timesText(catalog.pages), timesText(catalog.frames),
            catalog.selectedPosition ? std::to_string(*catalog.selectedPosition) : std::string("none"));
        // NFR-01: an already indexed catalog shows its first page within 5 s of the start.
        REQUIRE(catalog.firstPageMs.has_value());
        CHECK(*catalog.firstPageMs < 5000.0);
        REQUIRE(catalog.pages.has_value());
        CHECK(catalog.pages->p95Ms < 5.0); // ADR-0008 §5's page budget
    }
}
