// The file watcher on the real library (I7 step 7, ADR-0018 §10). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does (Release by default), with ES-DE closed, read-only snapshots of the library before and
// after (0 differences: the watcher only reads), and the folders in the RV_REAL_* variables. The library is imported into
// a catalog under the build tree, as retroverse-cli import makes it; the app's catalog opens it, and the watcher arms
// the real roots (the ROM root, the system folders outside it, the media root, the ES-DE home's gamelists, custom_systems
// and settings) for a minute: each root's mode is printed (notify, or poll where notifications are refused), and what
// changed meanwhile (nothing is expected, ES-DE being closed). Then the biggest system's partial refresh is timed beside
// a refresh of every system (NFR-05: a partial refresh plus the 2 s window within 10 s). Delivery on the ROM root's own
// volume needs a file added there, which no test may do: the user's trial proves it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <windows.h>

#include <fmt/format.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/library_watcher.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/platform_win/directory_watcher.hpp>
#include <rv/ui/quick_catalog_model.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/// What ProductUi does each frame, until nothing is pending and no refresh runs.
void settle(rv::app::AppCatalog& catalog, rv::ui::QuickCatalogModel& model) {
    const auto deadline = std::chrono::steady_clock::now() + 10min;
    while (true) {
        const rv::app::CatalogChanges changes = catalog.poll();
        if (changes.opened || changes.refreshed || changes.otherImport) {
            model.refresh();
        } else if (changes.rowsChanged) {
            model.reread();
        }
        model.poll();
        if (!catalog.busy() && !catalog.refreshState().running &&
            catalog.roomCatalogState() != rv::app::CatalogState::Opening) {
            return;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
}

rv::app::RefreshSummary refreshOf(rv::app::EsdeAppCatalog& catalog, rv::ui::QuickCatalogModel& model,
                                  rv::app::RefreshRequest request) {
    REQUIRE(catalog.requestRefresh(std::move(request)));
    settle(catalog, model);
    REQUIRE(catalog.refreshState().last.has_value());
    return *catalog.refreshState().last;
}

} // namespace

TEST_CASE("watcher on the real library: armed on its roots for a minute; the biggest system's partial refresh timed",
          "[.real-library][real-library][watcher_real]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    REQUIRE(home.has_value());
    REQUIRE(install.has_value());

    // A user data directory under the build tree, never in the library.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "watcher";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse(rv::catalog::LibraryId::kDefault).value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", profile, rv::esde::ExistingFile::Replace)
                .has_value());
    rv::core::PathOptions pathOptions;
    pathOptions.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const rv::core::Paths paths = rv::core::Paths::resolve(pathOptions, environment).value();

    // The import, as retroverse-cli import makes it.
    {
        const auto start = std::chrono::steady_clock::now();
        const auto settings = rv::esde::readSettings(profile.esdeHome);
        REQUIRE(settings.has_value());
        rv::catalog_db::DatabaseOptions options;
        options.create = true;
        auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths), options);
        REQUIRE(database.has_value());
        auto store = rv::catalog_db::SqliteCatalogStore::open(*database).value();
        rv::esde::EsdeLibrarySource source;
        rv::esde::EsdeMediaResolver resolver(rv::esde::resolveMediaRoot(*settings, profile));
        rv::catalog::IdentityPairing pairing;
        rv::catalog::ImportService service(source, resolver, *store, &pairing);
        const auto result = service.run(profile, rv::catalog::ImportOptions{});
        INFO((result ? std::string() : result.error().toString()));
        REQUIRE(result.has_value());
        std::cout << fmt::format("import: {} entries in {:.1f} s\n", result->statistics.entries, secondsSince(start));
    }

    rv::app::EsdeAppCatalog::Options options;
    options.paths = paths;
    options.owner = rv::catalog::SessionOwner{4'000'000'003u, 1};
    options.otherImportsInterval = rv::app::kOtherImportsInterval;
    rv::app::EsdeAppCatalog catalog(options);
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    REQUIRE(catalog.roomCatalogState() == rv::app::CatalogState::Ready);
    std::optional<rv::esde::ChangeScopeMap> map = catalog.takeWatchMap();
    REQUIRE(map.has_value());

    // Armed for a minute on the real roots.
    rv::platform_win::DirectoryWatcher directory;
    rv::app::LibraryWatcher watcher(directory);
    const auto armed = std::chrono::steady_clock::now();
    watcher.arm(std::move(*map));
    while (!directory.settled()) {
        REQUIRE(secondsSince(armed) < 60.0);
        std::this_thread::sleep_for(10ms);
    }
    std::cout << fmt::format("armed in {:.2f} s\n", secondsSince(armed));
    std::size_t refreshes = 0;
    const auto watching = std::chrono::steady_clock::now();
    while (secondsSince(watching) < 60.0) {
        const auto now = std::chrono::steady_clock::now();
        if (const auto due = watcher.poll(now, false)) {
            ++refreshes;
            static_cast<void>(refreshOf(catalog, model, rv::app::LibraryWatcher::requestOf(*due)));
            watcher.started(now);
        }
        static_cast<void>(catalog.poll());
        std::this_thread::sleep_for(50ms);
    }
    rv::app::WatcherRecord record;
    watcher.record(record);
    for (const rv::app::WatchRootRecord& root : record.roots) {
        std::cout << fmt::format("root {}: {}{}\n", root.name, root.mode,
                                 root.reason.empty() ? std::string() : " (" + root.reason + ")");
        CHECK((root.mode == "notify" || root.mode == "poll"));
    }
    std::cout << fmt::format("a minute: {} change(s), {} mapped, {} ignored, {} overflow(s), {} refresh(es)\n",
                             record.changes, record.mapped, record.ignored, record.overflows, refreshes);
    const auto rom = std::find_if(record.roots.begin(), record.roots.end(),
                                  [](const rv::app::WatchRootRecord& root) { return root.name == "rom"; });
    REQUIRE(rom != record.roots.end());

    // The biggest system's partial refresh, beside a refresh of every system (both warm).
    const auto systems = catalog.databaseSource().systems();
    REQUIRE_FALSE(systems.empty());
    const auto biggest = std::max_element(systems.begin(), systems.end(),
                                          [](const auto& a, const auto& b) { return a.games < b.games; });
    rv::app::RefreshRequest partial;
    partial.trigger = rv::app::RefreshTrigger::Watcher;
    partial.systems = {biggest->name};
    const rv::app::RefreshSummary first = refreshOf(catalog, model, partial);
    CHECK(first.outcome == rv::app::RefreshOutcome::Done);
    const rv::app::RefreshSummary again = refreshOf(catalog, model, partial);
    CHECK(again.outcome == rv::app::RefreshOutcome::Done);
    const rv::app::RefreshSummary whole =
        refreshOf(catalog, model, rv::app::RefreshRequest{rv::app::RefreshTrigger::Watcher, {}, false});
    CHECK(whole.outcome == rv::app::RefreshOutcome::Done);
    std::cout << fmt::format("partial refresh of {} ({} games): {:.0f} ms, again {:.0f} ms ({} entries, {} added, {} "
                             "removed, {} changed); every system: {:.0f} ms ({} entries)\n",
                             biggest->name.str(), biggest->games, first.milliseconds, again.milliseconds,
                             again.entries, again.added, again.removed, again.changed, whole.milliseconds,
                             whole.entries);
    CHECK(again.added == 0);
    CHECK(again.removed == 0);
    CHECK(again.entries < whole.entries);
    catalog.shutdown();
}
