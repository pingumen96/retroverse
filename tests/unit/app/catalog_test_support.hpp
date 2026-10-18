#pragma once

// The standard ES-DE fixture imported into a user data directory, as the app's catalog tests use it (I2 step 13,
// I3 step 13r): tests/unit/app/test_catalog_worker.cpp, test_library_refresh.cpp.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <rv/app/app_catalog.hpp>
#include <rv/app/catalog_worker.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/ui/quick_catalog_model.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace rv::test {

inline constexpr auto kSettleTimeout = std::chrono::seconds(30);

inline rv::catalog::LibraryProfile fixtureLibrary(const rv::test::Fixture& fixture) {
    rv::catalog::LibraryProfile library;
    library.id = rv::catalog::LibraryId::parse("esde").value();
    library.esdeHome = fixture.path("ES-DE");
    library.installFolder = fixture.path("Programs/ES-DE");
    return library;
}

/// The standard fixture recorded as the library of a user data directory and imported into its catalog database, psx
/// assigned the fixture's "Fake Emulator (Inject)" command (Solo Disc's %INJECT% file is usable: inject-file).
struct ImportedFixture {
    rv::test::TempDir dir;
    rv::test::Fixture fixture;
    std::filesystem::path data;
    rv::core::Paths paths;

    explicit ImportedFixture(std::string_view prefix, bool import = true)
        : dir(prefix), fixture(rv::test::Fixture::materialize("esde/standard").value()), data(dir.file("data")),
          paths(pathsOf(data)) {
        REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", fixtureLibrary(fixture),
                                           rv::esde::ExistingFile::Keep)
                    .has_value());
        if (import) {
            importLibrary();
            auto database = openDatabase();
            auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
            REQUIRE(store->setSystemChoice(rv::catalog::LibraryId::parse("esde").value(),
                                           rv::catalog::SystemName::parse("psx").value(),
                                           rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand,
                                                                      "Fake Emulator (Inject)"},
                                           std::chrono::system_clock::now())
                        .has_value());
        }
    }

    static rv::core::Paths pathsOf(const std::filesystem::path& data) {
        rv::core::PathOptions options;
        options.userDataOverride = data;
        rv::core::PathEnvironment environment;
        environment.executableDir = rv::test::testProgramsDir();
        environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
        return rv::core::Paths::resolve(options, environment).value();
    }

    /// An import of the fixture as it is now (as retroverse-cli import makes one).
    void importLibrary() const {
        const rv::catalog::LibraryProfile library = fixtureLibrary(fixture);
        const auto settings = rv::esde::readSettings(library.esdeHome);
        REQUIRE(settings.has_value());
        auto database = openDatabase();
        auto store = rv::catalog_db::SqliteCatalogStore::open(database).value();
        rv::esde::EsdeLibrarySource source;
        rv::esde::EsdeMediaResolver resolver(rv::esde::resolveMediaRoot(*settings, library));
        rv::catalog::IdentityPairing pairing;
        rv::catalog::ImportService service(source, resolver, *store, &pairing);
        const auto result = service.run(library, rv::catalog::ImportOptions{});
        INFO((result ? std::string() : result.error().toString()));
        REQUIRE(result.has_value());
    }

    [[nodiscard]] rv::catalog_db::Database openDatabase() const {
        auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
        REQUIRE(database.has_value());
        return std::move(*database);
    }

    /// The GameId of the identity at `relativePath` of `system`.
    [[nodiscard]] rv::catalog::GameId gameAt(std::string_view system, std::string_view relativePath) const {
        auto database = openDatabase();
        auto statement = database.writer()
                             .prepare("SELECT id FROM rv_game_identities WHERE system = ?1 AND relative_path = ?2")
                             .value();
        statement.bindText(1, system).bindText(2, relativePath);
        REQUIRE(statement.step().value());
        return rv::catalog::GameId::fromValue(static_cast<std::uint64_t>(statement.columnInt(0))).value();
    }

    [[nodiscard]] rv::app::EsdeAppCatalog::Options
    options(std::function<void(rv::app::CatalogWorker::Options&)> adjust = {}) const {
        rv::app::EsdeAppCatalog::Options options;
        options.paths = paths;
        options.owner = rv::catalog::SessionOwner{testOwnerProcessId(), 1};
        options.adjust = std::move(adjust);
        return options;
    }

    static std::uint32_t testOwnerProcessId() { return 4'000'000'001u; }
};

/// What the screen does every frame, until nothing asked of the worker is left unanswered.
inline void settle(rv::app::AppCatalog& catalog, rv::ui::QuickCatalogModel& model) {
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    while (true) {
        const rv::app::CatalogChanges changes = catalog.poll();
        if (changes.opened) {
            model.refresh();
        }
        if (changes.rowsChanged) {
            model.reread();
        }
        model.poll();
        if (!catalog.busy()) {
            return;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace rv::test
