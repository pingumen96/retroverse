// The ES-DE catalog of the app (I2 step 13), GPU-free: the catalog worker (rv::app::CatalogWorker) and the quick
// catalog's source over it (CatalogDatabaseSource, through EsdeAppCatalog) on the standard fixture imported into a user
// data directory (catalog::ImportService, as retroverse-cli import does), driven by a ui::QuickCatalogModel as the screen
// drives it: the catalog opened on the worker (the chips, the readiness of each system), lists materialized on the
// worker and pages fetched on the calling thread, search and filters, positions, a game's own readiness, the selection
// restored by GameId when an import changes the list under it, launches resolved right before they start, play
// sessions recorded on the worker (one that meets a busy database stays pending there, then is written), the sessions
// left open by owners that are gone closed at the start, the catalog that is not there (no library.json, no database,
// the data-directory guard, another library), and stopping while a query runs: interrupted, within a bound.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/app_catalog.hpp>
#include <rv/app/catalog_worker.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/ui_assets.hpp>

#include "catalog_test_support.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rv::app::CatalogState;
using rv::app::EsdeAppCatalog;
using rv::catalog::GameId;
using rv::test::fixtureLibrary;
using rv::test::ImportedFixture;
using rv::test::kSettleTimeout;
using rv::test::settle;

namespace {

std::vector<std::string> titlesOf(std::span<const rv::ui::CatalogEntry> rows) {
    std::vector<std::string> titles;
    for (const rv::ui::CatalogEntry& row : rows) {
        titles.push_back(row.title);
    }
    return titles;
}

/// The rows of the whole list, read through the source.
std::vector<rv::ui::CatalogEntry> allRows(rv::ui::CatalogSource& source) {
    std::vector<rv::ui::CatalogEntry> rows;
    REQUIRE(source.rows(0, source.count(), rows).has_value());
    return rows;
}

const rv::ui::CatalogEntry* findTitle(const std::vector<rv::ui::CatalogEntry>& rows, std::string_view title) {
    const auto found = std::find_if(rows.begin(), rows.end(),
                                    [&](const rv::ui::CatalogEntry& row) { return row.title == title; });
    return found == rows.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("app/catalog_worker: the imported catalog opens on the worker: chips, the whole list, pages and positions",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-open");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    CHECK(catalog.busy()); // the worker opens the catalog first
    settle(catalog, model);
    CHECK(catalog.notice().empty());
    REQUIRE(catalog.databaseSource().state() == CatalogState::Ready);

    // One chip per system with games, in the System order (the full names), with their games.
    const auto filters = model.filters();
    REQUIRE(filters.size() == 4);
    CHECK(filters[0].key.empty());
    CHECK(filters[0].count == 15);
    CHECK(filters[1] == rv::ui::PlatformFilter{"gb", "Nintendo Game Boy", 8});
    CHECK(filters[2] == rv::ui::PlatformFilter{"snes", "Nintendo SNES (Super Nintendo)", 5});
    CHECK(filters[3] == rv::ui::PlatformFilter{"psx", "Sony PlayStation", 2});

    // The whole list (the missing file left out), its first page in the model's window.
    REQUIRE(model.count() == 15);
    CHECK(model.window().size() == 6);
    CHECK(model.selectedPosition() == 0u);
    rv::ui::CatalogSource& source = catalog.source();
    const std::vector<rv::ui::CatalogEntry> rows = allRows(source);
    REQUIRE(rows.size() == 15);
    for (std::size_t position = 0; position < rows.size(); ++position) {
        CHECK(rv::ui::isValidEntryId(rows[position].id));
        CHECK(source.position(rows[position].id) == position);
    }
    CHECK_FALSE(source.position("g0000000000000001").has_value());
    CHECK_FALSE(source.position("not an id").has_value());
    // Pages cut at the list's end.
    std::vector<rv::ui::CatalogEntry> page;
    REQUIRE(source.rows(13, 30, page).has_value());
    CHECK(page.size() == 2);
    REQUIRE(source.rows(15, 5, page).has_value());
    CHECK(page.empty());

    // A row as the card shows it: title, platform, year, players, readiness.
    const rv::ui::CatalogEntry* tennis = findTitle(rows, "Tiny Tennis");
    REQUIRE(tennis != nullptr);
    CHECK(tennis->platform == "Nintendo Game Boy");
    CHECK(tennis->year > 0);
    CHECK(tennis->players == "1-2");
    CHECK(tennis->readiness == "ready");
    CHECK(tennis->state.empty());
    CHECK(tennis->blocker.empty());
    const auto times = catalog.databaseSource().pageTimes();
    REQUIRE(times.has_value());
    CHECK(times->count >= 3);

    rv::app::CatalogRecord record;
    catalog.record(record);
    CHECK(record.source == "esde");
    CHECK(record.notice.empty());
    CHECK(record.library == "esde");
    CHECK(record.systems == 3);
    CHECK(record.games == 15);
    CHECK(record.openedMs.has_value());
    CHECK(record.firstListMs.has_value());
}

TEST_CASE("app/catalog_worker: search and filters run on the worker, the selection kept by GameId",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-search");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");

    // The systems' full names are searched: the gb and snes games.
    model.setSearch("nintendo");
    CHECK(model.pending()); // the worker materializes it: the list shown stays until it is collected
    settle(catalog, model);
    CHECK(model.count() == 13);
    REQUIRE(model.select(tennis.toString()));
    // A filter keeps the selected game when its list holds it.
    REQUIRE(model.setFilter("gb"));
    settle(catalog, model);
    CHECK(model.count() == 8);
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selected()->id == tennis.toString());
    CHECK(model.selected()->title == "Tiny Tennis");
    // A word of its title narrows the list; another system's filter empties it.
    model.setSearch("tennis");
    settle(catalog, model);
    CHECK(model.count() == 1);
    REQUIRE(model.setFilter("psx"));
    settle(catalog, model);
    CHECK(model.count() == 0);
    CHECK(model.selected() == nullptr);
    CHECK_FALSE(model.error().has_value());
    // Diacritics are folded (FTS5's unicode61): "cafe" finds Café Racer.
    REQUIRE(model.setFilter(""));
    model.setSearch("cafe");
    settle(catalog, model);
    REQUIRE(model.count() == 1);
    CHECK(model.window().front().title == "Café Racer");
    // Typing coalesces: only the latest query's list is switched to.
    model.setSearch("p");
    model.setSearch("po");
    model.setSearch("pocket");
    settle(catalog, model);
    CHECK(model.count() == 2);
    CHECK(titlesOf(model.window()) == std::vector<std::string>{"Pocket Puzzle", "Pocket Puzzle (World)"});
    // A filter the catalog does not have is refused by the model (no chip), a query naming it by the source.
    CHECK_FALSE(model.setFilter("n64"));
    auto unknown = catalog.source().query(rv::ui::CatalogSourceQuery{"", "n64"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("app/catalog_worker: a game's readiness is its system's, or its own where it can differ",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-readiness");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    REQUIRE(model.setFilter("psx"));
    settle(catalog, model);
    REQUIRE(model.count() == 2);
    // psx's command (assigned "Fake Emulator (Inject)") leaves its %INJECT% file to each game: resolved per game on
    // the worker, the rows read again once known (QuickCatalogModel::reread).
    const auto& systems = catalog.databaseSource().systems();
    const auto psx = std::find_if(systems.begin(), systems.end(),
                                  [](const rv::app::CatalogSystem& system) { return system.name.str() == "psx"; });
    REQUIRE(psx != systems.end());
    CHECK(psx->perGame);
    CHECK(psx->launch.readiness == rv::catalog::LaunchReadiness::Ready);
    CHECK(model.contentRevision() >= 1);
    const std::vector<rv::ui::CatalogEntry> rows(model.window().begin(), model.window().end());
    const rv::ui::CatalogEntry* solo = findTitle(rows, "Solo Disc");
    const rv::ui::CatalogEntry* twin = findTitle(rows, "Twin Tracks");
    REQUIRE(solo != nullptr);
    REQUIRE(twin != nullptr);
    CHECK(solo->readiness == "requires-configuration");
    CHECK(solo->blocker == "inject-file");
    CHECK(solo->state == rv::ui::quick_catalog::kStateNeedsConfiguration);
    CHECK(solo->blockerDetail.find("%INJECT%") != std::string::npos);
    CHECK(twin->readiness == "ready");
    CHECK(twin->blocker.empty());

    // Every readiness key the source gives is one the card labels.
    for (const auto readiness :
         {rv::catalog::LaunchReadiness::SourceUnavailable, rv::catalog::LaunchReadiness::RomMissing,
          rv::catalog::LaunchReadiness::RequiresConfiguration, rv::catalog::LaunchReadiness::EmulatorNotFound,
          rv::catalog::LaunchReadiness::CoreNotFound, rv::catalog::LaunchReadiness::Ready}) {
        const auto& keys = rv::ui::quick_catalog::kReadinessKeys;
        CHECK(std::find(keys.begin(), keys.end(), rv::catalog::toString(readiness)) != keys.end());
    }
    CHECK(rv::app::stateKeyOf(rv::catalog::LaunchReadiness::RomMissing) == rv::ui::quick_catalog::kStateUnavailable);
    CHECK(rv::app::stateKeyOf(rv::catalog::LaunchReadiness::CoreNotFound) ==
          rv::ui::quick_catalog::kStateEmulatorMissing);
    CHECK(rv::app::stateKeyOf(rv::catalog::LaunchReadiness::Ready).empty());
}

TEST_CASE("app/catalog_worker: an import that changes the list under the screen: the selection restored by GameId",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-refresh");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    REQUIRE(model.setFilter("gb"));
    settle(catalog, model);
    REQUIRE(model.count() == 8);
    const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");
    const GameId hidden = f.gameAt("gb", "Hidden Gem (World).gb");
    REQUIRE(model.select(tennis.toString()));
    const std::size_t before = *model.selectedPosition();
    const std::optional<std::size_t> hiddenPosition = catalog.source().position(hidden.toString());
    REQUIRE(hiddenPosition.has_value());
    REQUIRE(*hiddenPosition < before);

    // Another process's import removes a game (its file deleted).
    fs::remove(f.fixture.path("roms/gb/Hidden Gem (World).gb"));
    f.importLibrary();
    // Until the list is materialized again, its position keeps a row with the id alone.
    std::vector<rv::ui::CatalogEntry> gone;
    REQUIRE(catalog.source().rows(*hiddenPosition, 1, gone).has_value());
    REQUIRE(gone.size() == 1);
    CHECK(gone.front().id == hidden.toString());
    CHECK(gone.front().title.empty());
    // The application refreshes: the same query on the worker, the selection restored by GameId on its new position.
    model.refresh();
    settle(catalog, model);
    CHECK(model.count() == 7);
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selected()->id == tennis.toString());
    CHECK(*model.selectedPosition() == before - 1);
    CHECK(model.activeFilter() == "gb");
}

TEST_CASE("app/catalog_worker: Avvia resolves the game on the worker right before the launch, and records its session",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-launch");
    // gb assigned a profile of the user's emulators.json that runs a program of the build (never started here).
    const nlohmann::json profile{{"id", "fake-return"},
                                 {"executable", rv::core::pathToUtf8(rv::test::testProgramsDir() / "rv_fake_emulator.exe")},
                                 {"arguments", {"--", "{rom}"}},
                                 {"hideConsoleWindow", true},
                                 {"systems", {{"gb", nlohmann::json::object()}}}};
    REQUIRE(rv::core::writeTextFileAtomic(f.data / "config" / "emulators.json",
                                          nlohmann::json{{"schemaVersion", 1}, {"profiles", {profile}}}.dump())
                .has_value());
    {
        auto database = f.openDatabase();
        auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
        REQUIRE(store->setSystemChoice(rv::catalog::LibraryId::parse("esde").value(),
                                       rv::catalog::SystemName::parse("gb").value(),
                                       rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::Profile, "fake-return"},
                                       std::chrono::system_clock::now())
                    .has_value());
    }
    const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");
    const GameId solo = f.gameAt("psx", "Solo Disc (USA).chd");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);

    rv::ui::CatalogEntry entry;
    entry.id = tennis.toString();
    entry.title = "Tiny Tennis";
    REQUIRE(catalog.requestLaunch(0, entry));
    CHECK(catalog.deciding());
    CHECK_FALSE(catalog.requestLaunch(1, entry)); // one decision at a time
    settle(catalog, model);
    // Made, not taken yet (ProductUi takes it after the frame's commands): still one decision at a time.
    CHECK(catalog.deciding());
    CHECK_FALSE(catalog.requestLaunch(1, entry));
    auto decision = catalog.takeDecision();
    REQUIRE(decision.has_value());
    CHECK(decision->token == 0);
    CHECK(decision->info.gameId == tennis.toString());
    CHECK(decision->info.title == "Tiny Tennis");
    CHECK(decision->info.profile == "fake-return");
    REQUIRE(decision->request.has_value());
    CHECK(decision->request->executable.filename() == "rv_fake_emulator.exe");
    CHECK(rv::core::pathFromUtf8(decision->request->arguments.back()).lexically_normal() ==
          f.fixture.path("roms/gb/Tiny Tennis (Europe).gb").lexically_normal());

    // Solo Disc is not ready: Avvia gets its message, and the blocked attempt is recorded on the worker.
    entry.id = solo.toString();
    entry.title = "Solo Disc";
    REQUIRE(catalog.requestLaunch(1, entry));
    settle(catalog, model);
    decision = catalog.takeDecision();
    REQUIRE(decision.has_value());
    CHECK_FALSE(decision->request.has_value());
    CHECK(decision->refusal.code == rv::app::launch_outcome::kNotReady);
    CHECK(decision->refusal.reason == "requires-configuration");
    catalog.sessionRefused(1);
    catalog.shutdown(); // the worker writes what is queued before it ends
    rv::app::LaunchRecord record;
    catalog.recordLaunch(1, record);
    REQUIRE(record.sessionId.has_value());
    CHECK(record.sessionError.empty());
    CHECK_FALSE(record.verified);
    auto database = f.openDatabase();
    auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
    const auto session = store->session(*record.sessionId).value();
    REQUIRE(session.has_value());
    CHECK(session->game == solo);
    CHECK(session->outcome == rv::catalog::PlaySessionOutcome::Blocked);
    CHECK(session->readiness == rv::catalog::LaunchReadiness::RequiresConfiguration);
    CHECK(session->choice.value == "Fake Emulator (Inject)");
    CHECK(session->owner.processId == ImportedFixture::testOwnerProcessId());
}

TEST_CASE("app/catalog_worker: a session write that meets a busy database stays pending on the worker, then is written",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-busy");
    const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");
    rv::app::CatalogWorker::Options options;
    options.paths = f.paths;
    options.owner = rv::catalog::SessionOwner{ImportedFixture::testOwnerProcessId(), 1};
    options.busyTimeout = 50ms;
    options.sessionRetryInterval = 100ms;
    rv::app::CatalogWorker worker(options);
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    while (!worker.idle()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(worker.take().opened->state == CatalogState::Ready);

    // Another holder of the write lock (another process's bulk import holds it so).
    auto holder = f.openDatabase();
    auto transaction =
        rv::catalog_db::Transaction::begin(holder.writer(), rv::catalog_db::Transaction::Mode::Immediate);
    REQUIRE(transaction.has_value());

    rv::catalog::PlaySession session;
    session.game = tennis;
    session.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::Profile, "fake-return"};
    session.choiceSource = rv::catalog::ChoiceSource::RetroverseSystem;
    session.owner = options.owner;
    session.started = std::chrono::system_clock::now();
    worker.beginSession(7, session);
    session.outcome = rv::catalog::PlaySessionOutcome::Returned;
    session.ended = session.started + 2500ms;
    session.runTime = 2500ms;
    session.exitCode = 0;
    session.exitKind = rv::launcher::ExitKind::Success;
    worker.finishSession(7, session, rv::catalog::SystemName::parse("gb").value(), "C:\\emulator.exe");
    // Both calls only queue (the caller never waits). The worker is idle again once it has handled both jobs, each
    // with one attempt that met the busy database: how long that takes is the busy timeout as the system's sleeps
    // stretch it (SQLite's busy handler sleeps in steps of the timer's resolution, longer still on a loaded machine),
    // so the test waits for the worker rather than for a fixed time.
    while (!worker.idle()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
    rv::app::CatalogResults results = worker.take();
    CHECK(results.sessions.empty()); // kept pending on the worker
    CHECK(holder.writer().queryInt("SELECT count(*) FROM rv_play_sessions").value().value_or(-1) == 0); // not written

    transaction->rollback();
    while (results.sessions.empty()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(20ms);
        results = worker.take();
    }
    REQUIRE(results.sessions.size() == 1);
    const rv::app::StoredSession& stored = results.sessions.front();
    CHECK(stored.token == 7);
    CHECK_FALSE(stored.error.has_value());
    CHECK(stored.session.id != 0);
    CHECK(stored.verified); // a normal return verifies its choice for the system
    REQUIRE(results.verified.size() == 1);
    CHECK(results.verified.front().first.str() == "gb");
    CHECK(worker.stop());
}

TEST_CASE("app/catalog_worker: the sessions left open by owners that are gone are closed as interrupted at the start",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-interrupted");
    const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");
    {
        auto database = f.openDatabase();
        auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
        rv::catalog::PlaySession open;
        open.game = tennis;
        open.started = std::chrono::system_clock::now() - 1h;
        open.owner = rv::catalog::SessionOwner{11, 1}; // gone
        REQUIRE(store->insertSession(open).has_value());
        open.owner = rv::catalog::SessionOwner{12, 1}; // still running
        REQUIRE(store->insertSession(open).has_value());
    }
    EsdeAppCatalog catalog(f.options([](rv::app::CatalogWorker::Options& worker) {
        worker.ownerAlive = [](const rv::catalog::SessionOwner& owner) { return owner.processId != 11; };
    }));
    rv::ui::QuickCatalogModel model(catalog.source());
    settle(catalog, model);
    CHECK(catalog.databaseSource().interruptedSessions() == 1);
    rv::app::CatalogRecord record;
    catalog.record(record);
    CHECK(record.interruptedSessions == 1);
}

TEST_CASE("app/catalog_worker: a catalog that is not there says why, and its queries fail without waiting",
          "[app][catalog_worker]") {
    namespace qc = rv::ui::quick_catalog;
    SECTION("no library.json") {
        rv::test::TempDir dir("rv-catalog-nolibrary");
        EsdeAppCatalog::Options options;
        options.paths = ImportedFixture::pathsOf(dir.file("data"));
        EsdeAppCatalog catalog(options);
        rv::ui::QuickCatalogModel model(catalog.source());
        settle(catalog, model);
        CHECK(catalog.notice() == qc::kNoticeNoLibrary);
        CHECK(model.count() == 0);
        CHECK(model.error().has_value());
        CHECK(model.filters().size() == 1);
        // Nothing written into the data directory.
        CHECK_FALSE(fs::exists(dir.file("data")));
    }
    SECTION("library.json without a catalog database") {
        const ImportedFixture f("rv-catalog-nodatabase", false);
        EsdeAppCatalog catalog(f.options());
        rv::ui::QuickCatalogModel model(catalog.source());
        settle(catalog, model);
        CHECK(catalog.notice() == qc::kNoticeNoDatabase);
        CHECK_FALSE(fs::exists(rv::catalog_db::databaseFile(f.paths)));
    }
    SECTION("a user data directory inside the library's ROM root (the data-directory guard)") {
        const ImportedFixture f("rv-catalog-guard", false);
        const fs::path inside = f.fixture.path("roms/retroverse-data");
        REQUIRE(rv::esde::writeLibraryFile(inside / "config" / "library.json", fixtureLibrary(f.fixture),
                                           rv::esde::ExistingFile::Keep)
                    .has_value());
        EsdeAppCatalog::Options options;
        options.paths = ImportedFixture::pathsOf(inside);
        EsdeAppCatalog catalog(options);
        rv::ui::QuickCatalogModel model(catalog.source());
        settle(catalog, model);
        CHECK(catalog.notice() == qc::kNoticeDataDirectory);
        CHECK_FALSE(fs::exists(inside / "database"));
    }
    SECTION("library.json naming another library than the catalog's") {
        const ImportedFixture f("rv-catalog-mismatch");
        rv::catalog::LibraryProfile other = fixtureLibrary(f.fixture);
        other.id = rv::catalog::LibraryId::parse("other").value();
        REQUIRE(rv::esde::writeLibraryFile(f.data / "config" / "library.json", other, rv::esde::ExistingFile::Replace)
                    .has_value());
        EsdeAppCatalog catalog(f.options());
        rv::ui::QuickCatalogModel model(catalog.source());
        settle(catalog, model);
        CHECK(catalog.notice() == qc::kNoticeLibraryMismatch);
        CHECK(model.count() == 0);
    }
    for (const std::string_view notice : qc::kNotices) {
        CHECK_FALSE(notice.empty());
    }
}

TEST_CASE("app/catalog_worker: ES-DE's configuration unreadable: the catalog is browsed, nothing launches",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-unreadable");
    // Without its es_systems.xml (ES-DE uninstalled since the import), no command can be translated.
    fs::remove(f.fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml"));
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    CHECK(catalog.notice().empty());
    CHECK(model.count() == 15);
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selected()->readiness == "requires-configuration");
    CHECK(model.selected()->blocker == rv::app::kLibraryUnreadableCode);
    const auto& codes = rv::ui::quick_catalog::kBlockerCodes;
    CHECK(std::find(codes.begin(), codes.end(), rv::app::kLibraryUnreadableCode) != codes.end());
}

TEST_CASE("app/catalog_worker: stopping while a query runs interrupts it and ends within a bound",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-stop");
    std::atomic<bool> running{false};
    rv::app::CatalogWorker::Options options;
    options.paths = f.paths;
    // A query that would run for ever: a recursive count on the worker's own connection, which only the interrupt
    // (sqlite3_interrupt, ADR-0008 §5) ends.
    options.runQuery = [&running](rv::catalog_db::SqliteCatalogQuery& query,
                                  const rv::catalog::CatalogQuery&) -> rv::Expected<std::vector<GameId>> {
        running = true;
        auto endless = query.connection().queryInt(
            "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n) SELECT count(*) FROM n");
        if (!endless) {
            return rv::propagate(endless.error());
        }
        return std::vector<GameId>{};
    };
    rv::app::CatalogWorker worker(options);
    worker.query(1, rv::catalog::CatalogQuery{});
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    while (!running) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(50ms);
    const auto start = std::chrono::steady_clock::now();
    CHECK(worker.stop(5s));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < 2s);
    const rv::app::CatalogResults results = worker.take();
    REQUIRE(results.list.has_value());
    REQUIRE(results.list->error.has_value());
    CHECK(results.list->error->code == rv::ErrorCode::Cancelled);
    CHECK(worker.stop()); // idempotent
}

TEST_CASE("app/catalog_worker: the first list comes with the opening; the list says the catalog opens until it is shown",
          "[app][catalog_worker]") {
    namespace qc = rv::ui::quick_catalog;
    const ImportedFixture f("rv-catalog-firstlist");
    std::atomic<int> queries{0};
    EsdeAppCatalog catalog(f.options([&queries](rv::app::CatalogWorker::Options& worker) {
        worker.runQuery = [&queries](rv::catalog_db::SqliteCatalogQuery& query,
                                     const rv::catalog::CatalogQuery& request) {
            ++queries;
            return query.run(request);
        };
    }));
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    // The opening collected, the model not polled yet: no list shown, so the list says the catalog is opening (never
    // "no games in the catalog").
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    while (true) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        if (catalog.poll().opened) {
            model.refresh();
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(catalog.databaseSource().state() == CatalogState::Ready);
    CHECK(catalog.notice() == qc::kNoticeOpening);
    CHECK_FALSE(catalog.databaseSource().listed());
    settle(catalog, model);
    CHECK(catalog.notice().empty());
    CHECK(model.count() == 15);
    // The default list came with the opening: no query on the worker, and NFR-01's first list is the opening's.
    CHECK(queries == 0);
    rv::app::CatalogRecord record;
    catalog.record(record);
    CHECK(record.firstListMs.has_value());
    // Later lists come from the worker, the default one too (the first list may be stale by then).
    REQUIRE(model.setFilter("gb"));
    settle(catalog, model);
    CHECK(queries == 1);
    REQUIRE(model.setFilter(""));
    settle(catalog, model);
    CHECK(queries == 2);
    CHECK(model.count() == 15);
}

TEST_CASE("app/catalog_worker: every request is answered: a game the catalog no longer has, a worker that failed",
          "[app][catalog_worker]") {
    const ImportedFixture f("rv-catalog-answered");
    SECTION("the readiness of a game the catalog does not have") {
        rv::app::CatalogWorker::Options options;
        options.paths = f.paths;
        options.owner = rv::catalog::SessionOwner{ImportedFixture::testOwnerProcessId(), 1};
        rv::app::CatalogWorker worker(options);
        const GameId gone = GameId::fromValue(0x0123456789abcdefull).value();
        const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");
        worker.readiness({gone, tennis});
        const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
        std::vector<rv::app::GameReadiness> answered;
        while (answered.size() < 2) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(5ms);
            rv::app::CatalogResults results = worker.take();
            answered.insert(answered.end(), results.readiness.begin(), results.readiness.end());
        }
        REQUIRE(answered.size() == 2);
        CHECK(answered[0].game == gone);
        CHECK_FALSE(answered[0].known); // answered all the same: nothing waits for it
        CHECK(answered[1].game == tennis);
        CHECK(answered[1].known);
        CHECK(worker.stop());
    }
    SECTION("a worker whose thread fails on a query: nothing waits, nothing launches") {
        EsdeAppCatalog catalog(f.options([](rv::app::CatalogWorker::Options& worker) {
            worker.runQuery = [](rv::catalog_db::SqliteCatalogQuery&,
                                 const rv::catalog::CatalogQuery&) -> rv::Expected<std::vector<GameId>> {
                throw std::runtime_error("a failure the worker does not expect");
            };
        }));
        rv::ui::QuickCatalogModel model(catalog.source(), 6);
        settle(catalog, model);
        REQUIRE(model.count() == 15); // the first list came with the opening
        model.setSearch("tennis");    // asked of the worker, whose thread fails on it
        settle(catalog, model);       // returns: the source no longer waits for the worker
        CHECK(catalog.databaseSource().ended());
        CHECK_FALSE(catalog.busy());
        CHECK(model.count() == 0);
        REQUIRE(model.error().has_value());
        CHECK(model.error()->code == rv::ErrorCode::Unavailable);
        rv::ui::CatalogEntry entry;
        entry.id = f.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
        entry.title = "Tiny Tennis";
        REQUIRE(catalog.requestLaunch(0, entry));
        auto decision = catalog.takeDecision();
        REQUIRE(decision.has_value());
        CHECK_FALSE(decision->request.has_value());
        CHECK(decision->refusal.reason == "unresolved");
        CHECK_FALSE(catalog.deciding());
    }
    SECTION("a worker whose thread fails while it opens: the opening is answered, unavailable") {
        {
            // A session left open, so that the opening asks whether its owner still runs: the question throws.
            auto database = f.openDatabase();
            auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
            rv::catalog::PlaySession open;
            open.game = f.gameAt("gb", "Tiny Tennis (Europe).gb");
            open.started = std::chrono::system_clock::now() - 1h;
            open.owner = rv::catalog::SessionOwner{11, 1};
            REQUIRE(store->insertSession(open).has_value());
        }
        EsdeAppCatalog catalog(f.options([](rv::app::CatalogWorker::Options& worker) {
            worker.ownerAlive = [](const rv::catalog::SessionOwner&) -> bool {
                throw std::runtime_error("a failure the worker does not expect");
            };
        }));
        rv::ui::QuickCatalogModel model(catalog.source(), 6);
        settle(catalog, model);
        CHECK_FALSE(catalog.busy());
        CHECK(catalog.databaseSource().ended());
        CHECK(catalog.databaseSource().state() == CatalogState::Unavailable);
        CHECK(catalog.notice() == rv::ui::quick_catalog::kNoticeUnavailable);
        CHECK(model.count() == 0);
    }
}

TEST_CASE("app/catalog_worker: the directory's favourites and recent games come from the worker, as the query ranks them",
          "[app][catalog_worker]") {
    // I4 step 7b: AppCatalog::requestRecentGames on the catalog worker (SqliteCatalogQuery::favoritesAndRecent, ES-DE's
    // <lastplayed> in the machine's zone), answered once; a later request replaces one still queued.
    const ImportedFixture f("rv-catalog-recent");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    CHECK_FALSE(catalog.takeRecentGames().has_value());
    REQUIRE(catalog.requestRecentGames(rv::catalog::kDefaultRecentGames));
    REQUIRE(catalog.requestRecentGames(rv::catalog::kDefaultRecentGames));
    CHECK(catalog.busy()); // a lockstep frame waits for it
    settle(catalog, model);
    auto answer = catalog.takeRecentGames();
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK_FALSE(catalog.takeRecentGames().has_value()); // once
    // The same list the query gives on a reader of the catalog.
    auto database = f.openDatabase();
    auto reader = database.openReader();
    REQUIRE(reader.has_value());
    auto query = rv::catalog_db::SqliteCatalogQuery::open(std::move(*reader));
    REQUIRE(query.has_value());
    rv::catalog::RecentGamesQuery recent;
    recent.localToUtc =
        rv::catalog::machineLocalTimeToUtc().value_or(rv::catalog::fixedOffsetLocalTimeToUtc(std::chrono::minutes(0)));
    auto expected = (*query)->favoritesAndRecent(recent);
    REQUIRE(expected.has_value());
    REQUIRE_FALSE(expected->empty());
    REQUIRE((*answer)->size() == expected->size());
    for (std::size_t index = 0; index < expected->size(); ++index) {
        CHECK((**answer)[index].id == (*expected)[index].id);
        CHECK((**answer)[index].favorite == (*expected)[index].favorite);
    }
    CHECK((**answer).front().favorite); // the fixture's favourite first
    // The systems' full names for the directory's rooms ("" for a system the catalog does not know).
    CHECK_FALSE(catalog.systemFullName(rv::catalog::SystemName::parse("gb").value()).empty());
    CHECK(catalog.systemFullName(rv::catalog::SystemName::parse("vectrex").value()).empty());
    // After the worker stopped, a request asks nothing.
    catalog.shutdown();
    CHECK_FALSE(catalog.requestRecentGames(rv::catalog::kDefaultRecentGames));
}

TEST_CASE("app/catalog_worker: every blocker code the launch resolution reports has the card's sentence",
          "[app][catalog_worker]") {
    // The codes rv_catalog's resolution, rv_esde's translation, rv_library and the app report: each string literal that follows a
    // LaunchReadiness value (block(LaunchReadiness::X, "code", ...), ReadinessBlocker{LaunchReadiness::X, "code", ...})
    // or is assigned to a code (the emulator profiles' blockers: code = "profile-template").
    const fs::path root = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "src";
    const std::regex blocker(R"re((?:LaunchReadiness::[A-Za-z]+,\s*|\bcode = )"([a-z][a-z0-9-]*)")re");
    std::set<std::string> codes;
    for (const char* folder : {"catalog/src", "esde/src", "library/src", "app/src"}) {
        for (const fs::directory_entry& file : fs::directory_iterator(root / folder)) {
            if (file.path().extension() != ".cpp") {
                continue;
            }
            const std::string text = rv::core::readTextFile(file.path()).value();
            for (auto match = std::sregex_iterator(text.begin(), text.end(), blocker); match != std::sregex_iterator();
                 ++match) {
                codes.insert((*match)[1].str());
            }
        }
    }
    CHECK(codes.size() >= 32); // the scan finds them all but the app's constant (the card names 33)
    const auto& known = rv::ui::quick_catalog::kBlockerCodes;
    for (const std::string& code : codes) {
        INFO(code);
        CHECK(std::find(known.begin(), known.end(), code) != known.end());
    }
}
