// The catalog's refresh on its worker (I3 step 13r), GPU-free: "Aggiorna libreria" and --refresh-library as the app's
// catalog does them (EsdeAppCatalog over the CatalogWorker, driven as ProductUi drives it), on the standard fixture
// imported into a user data directory: the import as a job of the worker (ImportService with IdentityPairing), a ROM
// added and one removed since the import: after the commit the list runs again with the selection restored by GameId,
// the chips count the games now, the systems' readiness is resolved again; lists, readiness and a launch's resolution
// answered between the import's slices while it runs; stopping mid-refresh cancels it within the bound with nothing
// written; another import holding the import lock makes it Busy. The slicing itself (runSlicedImport) over a host that
// records what it was asked, the summary's outcomes, the names the document uses, the run report's record.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/app/catalog_worker.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/run_report.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/file_io.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/ui_assets.hpp>

#include "catalog_test_support.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rv::app::EsdeAppCatalog;
using rv::app::RefreshOutcome;
using rv::app::RefreshPhase;
using rv::catalog::GameId;
using rv::test::ImportedFixture;
using rv::test::settle;

namespace {

/// What ProductUi does with the catalog's changes each frame (a refresh's commit makes the model read the platforms
/// and the list again), until nothing asked of the worker is pending and no refresh runs. Returns whether a refresh
/// ended meanwhile.
bool settleRefresh(rv::app::AppCatalog& catalog, rv::ui::QuickCatalogModel& model) {
    const auto deadline = std::chrono::steady_clock::now() + rv::test::kSettleTimeout;
    bool ended = false;
    while (true) {
        const rv::app::CatalogChanges changes = catalog.poll();
        if (changes.opened || changes.refreshed) {
            model.refresh();
        } else if (changes.rowsChanged) {
            model.reread();
        }
        ended = ended || changes.refreshEnded;
        model.poll();
        if (!catalog.busy() && !catalog.refreshState().running) {
            return ended;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

/// The library source of the fixture, its load held for `delay` first (looking at the cancel flag every few
/// milliseconds), so that a test can act while the refresh runs. `loading` is set once the load began.
class SlowSource final : public rv::catalog::ILibrarySource {
public:
    SlowSource(std::chrono::milliseconds delay, std::shared_ptr<std::atomic<bool>> loading)
        : m_delay(delay), m_loading(std::move(loading)) {}

    rv::Expected<rv::catalog::LibrarySnapshot> load(const rv::catalog::LibraryProfile& profile,
                                                    const rv::catalog::LibraryLoadOptions& options) override {
        m_loading->store(true);
        const auto until = std::chrono::steady_clock::now() + m_delay;
        while (std::chrono::steady_clock::now() < until) {
            if (options.cancel != nullptr && options.cancel->load()) {
                return rv::makeError(rv::ErrorCode::Cancelled, "the load was cancelled");
            }
            std::this_thread::sleep_for(5ms);
        }
        return m_inner.load(profile, options);
    }

private:
    std::chrono::milliseconds m_delay;
    std::shared_ptr<std::atomic<bool>> m_loading;
    rv::esde::EsdeLibrarySource m_inner;
};

std::function<void(rv::app::CatalogWorker::Options&)> slowSource(std::chrono::milliseconds delay,
                                                                 std::shared_ptr<std::atomic<bool>> loading) {
    return [delay, loading](rv::app::CatalogWorker::Options& worker) {
        worker.makeSource = [delay, loading] { return std::make_unique<SlowSource>(delay, loading); };
    };
}

void waitFor(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + rv::test::kSettleTimeout;
    while (!flag.load()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

std::vector<rv::ui::CatalogEntry> allRows(rv::ui::CatalogSource& source) {
    std::vector<rv::ui::CatalogEntry> rows;
    REQUIRE(source.rows(0, source.count(), rows).has_value());
    return rows;
}

const rv::app::CatalogSystem* findSystem(const EsdeAppCatalog& catalog, std::string_view name) {
    return catalog.databaseSource().findSystem(name);
}

/// The fixture's games as the catalog database holds them now (Available Game entries of the library).
std::size_t storedGames(const ImportedFixture& f) {
    auto database = f.openDatabase();
    auto count = database.writer().queryInt(
        "SELECT COUNT(*) FROM idx_games WHERE state = 'game' AND availability = 'available'");
    REQUIRE(count.has_value());
    return static_cast<std::size_t>(count->value_or(0));
}

constexpr std::string_view kNewRom = "roms/snes/Fresh Find (USA).sfc";

void addRom(const ImportedFixture& f) {
    REQUIRE(rv::core::writeTextFileAtomic(f.fixture.path(std::string(kNewRom)), std::string(65536, 'Z')).has_value());
}

} // namespace

TEST_CASE("app/library_refresh: a refresh imports on the worker; the list runs again with the selection kept",
          "[app][library_refresh]") {
    const ImportedFixture f("rv-refresh-flow");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    REQUIRE(model.count() == 15);
    CHECK(catalog.refreshState().available);
    CHECK_FALSE(catalog.refreshState().running);
    CHECK_FALSE(catalog.refreshState().last.has_value());
    const GameId tennis = f.gameAt("gb", "Tiny Tennis (Europe).gb");
    REQUIRE(model.select(tennis.toString()));
    REQUIRE(findSystem(catalog, "snes") != nullptr);
    const std::size_t snesBefore = findSystem(catalog, "snes")->games;
    const std::size_t gbBefore = findSystem(catalog, "gb")->games;

    // A ROM added to snes and one removed from gb since the import (retroverse-cli import made the catalog).
    addRom(f);
    fs::remove(f.fixture.path("roms/gb/Hidden Gem (World).gb"));
    // Nothing changes before the refresh.
    settle(catalog, model);
    CHECK(model.count() == 15);

    REQUIRE(catalog.requestRefresh());
    CHECK(catalog.refreshState().running);
    CHECK_FALSE(catalog.requestRefresh()); // one at a time
    CHECK(settleRefresh(catalog, model));

    const rv::app::RefreshState state = catalog.refreshState();
    CHECK_FALSE(state.running);
    REQUIRE(state.last.has_value());
    const rv::app::RefreshSummary& summary = *state.last;
    INFO((summary.error ? summary.error->toString() : std::string()));
    CHECK(summary.outcome == RefreshOutcome::Done);
    CHECK(summary.added == 1);
    CHECK(summary.removed == 1);
    CHECK(summary.guarded == 0);
    CHECK(summary.entries > 15);
    CHECK(summary.milliseconds > 0.0);
    CHECK(summary.slicing.boundaries > 0); // the library's load ran on the helper thread at least

    // The list ran again: the new game is in it, the removed one is not, the selection is the same game.
    CHECK(model.count() == 15);
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selected()->id == tennis.toString());
    const std::vector<rv::ui::CatalogEntry> rows = allRows(catalog.source());
    const auto fresh = std::find_if(rows.begin(), rows.end(), [](const rv::ui::CatalogEntry& row) {
        return row.title.starts_with("Fresh Find");
    });
    REQUIRE(fresh != rows.end());
    CHECK(fresh->readiness == "ready"); // snes's readiness, resolved again after the commit
    CHECK(std::none_of(rows.begin(), rows.end(),
                       [](const rv::ui::CatalogEntry& row) { return row.title.starts_with("Hidden Gem"); }));
    // The chips count the games now.
    CHECK(findSystem(catalog, "snes")->games == snesBefore + 1);
    CHECK(findSystem(catalog, "gb")->games == gbBefore - 1);
    rv::app::CatalogRecord record;
    catalog.record(record);
    CHECK(record.games == 15);
    CHECK(storedGames(f) == 15);

    // psx's games are resolved per game again (the inject file of Solo Disc): their rows ask, the worker answers with
    // the new generation.
    model.setSearch("solo");
    settle(catalog, model);
    REQUIRE(model.selected() != nullptr);
    settle(catalog, model);
    CHECK(model.selected()->readiness == "requires-configuration");
    CHECK(model.selected()->blocker == "inject-file");

    // A refresh that finds nothing new changes nothing.
    REQUIRE(catalog.requestRefresh());
    CHECK(settleRefresh(catalog, model));
    REQUIRE(catalog.refreshState().last.has_value());
    CHECK(catalog.refreshState().last->outcome == RefreshOutcome::Done);
    CHECK(catalog.refreshState().last->added == 0);
    CHECK(catalog.refreshState().last->removed == 0);
    CHECK(catalog.refreshState().last->changed == 0);
}

TEST_CASE("app/library_refresh: lists, readiness and a launch are answered between the import's slices",
          "[app][library_refresh]") {
    const ImportedFixture f("rv-refresh-slices");
    auto loading = std::make_shared<std::atomic<bool>>(false);
    EsdeAppCatalog catalog(f.options(slowSource(1500ms, loading)));
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    addRom(f);

    REQUIRE(catalog.requestRefresh());
    waitFor(*loading);
    // While the library is read: a filter's list, the rows' readiness, a launch resolved.
    const auto asked = std::chrono::steady_clock::now();
    REQUIRE(model.setFilter("gb"));
    settle(catalog, model);
    const double listMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count();
    CHECK(catalog.refreshState().running); // answered while the refresh still runs
    CHECK(model.count() == 8);
    CHECK(listMs < 1000.0); // not behind the 1.5 s load
    REQUIRE(model.selected() != nullptr);
    REQUIRE(catalog.requestLaunch(1, *model.selected()));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    std::optional<rv::app::LaunchDecision> decision;
    while (!decision) {
        static_cast<void>(catalog.poll());
        decision = catalog.takeDecision();
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
    CHECK(catalog.refreshState().running);
    CHECK(decision->info.gameId == model.selected()->id);

    CHECK(settleRefresh(catalog, model));
    const rv::app::RefreshState state = catalog.refreshState();
    REQUIRE(state.last.has_value());
    CHECK(state.last->outcome == RefreshOutcome::Done);
    CHECK(state.last->added == 1);
    CHECK(state.last->slicing.served >= 2); // the list and the launch, at least
    // The lists asked while it ran waited far less than the import (lists within 100 ms during an import).
    REQUIRE(state.lastLists.has_value());
    CHECK(state.lastLists->count >= 1);
    CHECK(state.lastLists->maxMs < 100.0);
    // The filter is kept, and the list holds the gb games still.
    CHECK(model.activeFilter() == "gb");
    CHECK(model.count() == 8);
}

TEST_CASE("app/library_refresh: stopping mid-refresh cancels it within the bound, nothing written",
          "[app][library_refresh]") {
    const ImportedFixture f("rv-refresh-stop");
    auto loading = std::make_shared<std::atomic<bool>>(false);
    auto catalog = std::make_unique<EsdeAppCatalog>(f.options(slowSource(60s, loading)));
    rv::ui::QuickCatalogModel model(catalog->source(), 6);
    settle(*catalog, model);
    addRom(f);
    REQUIRE(catalog->requestRefresh());
    waitFor(*loading);

    const auto start = std::chrono::steady_clock::now();
    catalog->shutdown();
    const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(stopMs < 2000.0);
    REQUIRE(catalog->stopMilliseconds().has_value());
    CHECK(*catalog->stopMilliseconds() < 2000.0);
    const rv::app::RefreshState state = catalog->refreshState();
    CHECK_FALSE(state.running);
    CHECK_FALSE(state.available);
    REQUIRE(state.last.has_value());
    CHECK(state.last->outcome == RefreshOutcome::Cancelled);
    catalog.reset();
    // Nothing was written: the new ROM is not in the catalog, which passes its integrity check.
    CHECK(storedGames(f) == 15);
    auto database = f.openDatabase();
    CHECK(database.writer().queryText("PRAGMA integrity_check").value() == std::optional<std::string>("ok"));
}

TEST_CASE("app/library_refresh: another import holding the import lock makes the refresh busy",
          "[app][library_refresh]") {
    const ImportedFixture f("rv-refresh-busy");
    EsdeAppCatalog catalog(f.options());
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    // Another instance (as another process's retroverse-cli import) holds the import lock.
    auto other = f.openDatabase();
    auto store = rv::catalog_db::SqliteCatalogStore::open(other).value();
    auto lock = store->lockImport(rv::catalog::LibraryId::parse("esde").value());
    REQUIRE(lock.has_value());

    REQUIRE(catalog.requestRefresh());
    CHECK(settleRefresh(catalog, model));
    REQUIRE(catalog.refreshState().last.has_value());
    CHECK(catalog.refreshState().last->outcome == RefreshOutcome::Busy);
    CHECK(model.count() == 15);
    // Once it is released, the next refresh runs.
    lock->reset();
    REQUIRE(catalog.requestRefresh());
    CHECK(settleRefresh(catalog, model));
    CHECK(catalog.refreshState().last->outcome == RefreshOutcome::Done);
}

namespace {

/// A host that records what the sliced import asks of it; its helper work runs on a thread of its own.
class RecordingHost final : public rv::app::RefreshHost {
public:
    std::size_t serve(bool writing) override {
        ++serves;
        servesWhileWriting += writing ? 1 : 0;
        return 0;
    }
    std::size_t serveWhile(const std::function<void()>& work) override {
        ++offloads;
        std::thread helper(work);
        helper.join();
        return 0;
    }
    void progress(const rv::app::RefreshProgress& progress) override { phases.push_back(progress.phase); }
    void writing(bool open) override { transitions.push_back(open); }

    std::size_t serves = 0;
    std::size_t servesWhileWriting = 0;
    std::size_t offloads = 0;
    std::vector<RefreshPhase> phases;
    std::vector<bool> transitions;
};

} // namespace

TEST_CASE("app/library_refresh: the sliced import: offloaded reads, the write transaction told, the phases in order",
          "[app][library_refresh]") {
    const ImportedFixture f("rv-refresh-sliced");
    addRom(f);
    const rv::catalog::LibraryProfile library = rv::test::fixtureLibrary(f.fixture);
    const auto settings = rv::esde::readSettings(library.esdeHome);
    REQUIRE(settings.has_value());
    auto database = f.openDatabase();
    auto store = rv::catalog_db::SqliteCatalogStore::open(database).value();
    rv::esde::EsdeLibrarySource source;
    rv::esde::EsdeMediaResolver resolver(rv::esde::resolveMediaRoot(*settings, library));
    rv::catalog::IdentityPairing pairing;
    RecordingHost host;
    rv::app::RefreshSlicing slicing;
    const std::atomic<bool> cancel{false};
    const auto result = rv::app::runSlicedImport(rv::app::RefreshPorts{source, resolver, *store, &pairing}, library,
                                                 cancel, host, slicing);
    INFO((result ? std::string() : result.error().toString()));
    REQUIRE(result.has_value());
    CHECK(result->changes.added.size() == 1);
    // The load and the first media question of each of the three systems ran on the helper.
    CHECK(host.offloads >= 4);
    CHECK(slicing.boundaries >= host.offloads);
    CHECK(slicing.longestSliceMs >= 0.0);
    // The write transaction began and ended once.
    CHECK(host.transitions == std::vector<bool>{true, false});
    // The phases in the import's order, each posted once as it began.
    REQUIRE_FALSE(host.phases.empty());
    CHECK(host.phases.front() == RefreshPhase::Reading);
    CHECK(std::is_sorted(host.phases.begin(), host.phases.end()));
    CHECK(std::find(host.phases.begin(), host.phases.end(), RefreshPhase::Writing) != host.phases.end());

    // A cancel flag set before: the import ends Cancelled, the host told nothing of a transaction.
    RecordingHost cancelledHost;
    const std::atomic<bool> stop{true};
    const auto cancelled = rv::app::runSlicedImport(rv::app::RefreshPorts{source, resolver, *store, &pairing},
                                                    library, stop, cancelledHost, slicing);
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == rv::ErrorCode::Cancelled);
    CHECK(cancelledHost.transitions.empty());
}

TEST_CASE("app/library_refresh: the summary's outcome comes from the import's error", "[app][library_refresh]") {
    using Result = rv::Expected<rv::catalog::ImportResult>;
    CHECK(rv::app::summarizeImport(Result(rv::makeError(rv::ErrorCode::Cancelled, "stop"))).outcome ==
          RefreshOutcome::Cancelled);
    CHECK(rv::app::summarizeImport(Result(rv::makeError(rv::ErrorCode::Conflict, "another import runs"))).outcome ==
          RefreshOutcome::Busy);
    // library.json naming another library is no busy database: it fails until it is re-associated.
    CHECK(rv::app::summarizeImport(Result(rv::makeError(rv::ErrorCode::Conflict, "other library", "library.json")))
              .outcome == RefreshOutcome::Failed);
    const rv::app::RefreshSummary failed =
        rv::app::summarizeImport(Result(rv::makeError(rv::ErrorCode::NotFound, "no ES-DE home")));
    CHECK(failed.outcome == RefreshOutcome::Failed);
    REQUIRE(failed.error.has_value());
    CHECK(failed.error->message == "no ES-DE home");

    rv::catalog::ImportResult imported;
    imported.statistics.entries = 20;
    imported.changes.added.resize(2);
    imported.changes.removed.resize(1);
    imported.changes.metadataChanged.resize(3);
    rv::catalog::SystemChanges guarded;
    guarded.outcome = rv::catalog::SystemOutcome::KeptByGuard;
    imported.changes.systems = {guarded, rv::catalog::SystemChanges{}};
    const rv::app::RefreshSummary done = rv::app::summarizeImport(Result(imported));
    CHECK(done.outcome == RefreshOutcome::Done);
    CHECK_FALSE(done.error.has_value());
    CHECK(done.entries == 20);
    CHECK(done.added == 2);
    CHECK(done.removed == 1);
    CHECK(done.changed == 3);
    CHECK(done.guarded == 1);
}

TEST_CASE("app/library_refresh: the states and phases are the names the quick catalog's document uses",
          "[app][library_refresh]") {
    namespace qc = rv::ui::quick_catalog;
    CHECK(qc::kRefreshStates[0] == qc::kRefreshRunning);
    const std::vector<std::string_view> outcomes{
        rv::app::toString(RefreshOutcome::Done), rv::app::toString(RefreshOutcome::Cancelled),
        rv::app::toString(RefreshOutcome::Busy), rv::app::toString(RefreshOutcome::Failed)};
    CHECK(std::equal(outcomes.begin(), outcomes.end(), qc::kRefreshStates.begin() + 1, qc::kRefreshStates.end()));
    const std::vector<std::string_view> phases{
        rv::app::toString(RefreshPhase::Reading), rv::app::toString(RefreshPhase::Media),
        rv::app::toString(RefreshPhase::Comparing), rv::app::toString(RefreshPhase::Writing)};
    CHECK(std::equal(phases.begin(), phases.end(), qc::kRefreshPhases.begin(), qc::kRefreshPhases.end()));
    CHECK(rv::app::toString(rv::app::RefreshTrigger::CommandLine) == "command-line");
    CHECK(rv::app::toString(rv::app::RefreshTrigger::User) == "user");
}

TEST_CASE("app/library_refresh: the run report keeps the refreshes", "[app][library_refresh]") {
    rv::app::RunReport report;
    report.application = "Retroverse";
    rv::app::CatalogRecord catalog;
    catalog.source = "esde";
    rv::app::RefreshRecord record;
    record.trigger = "command-line";
    record.requestFrame = 3;
    rv::app::RefreshSummary summary;
    summary.entries = 22;
    summary.added = 1;
    summary.removed = 2;
    summary.changed = 3;
    summary.moved = 4;
    summary.reviews = 5;
    summary.guarded = 6;
    summary.milliseconds = 123.5;
    summary.slicing = rv::app::RefreshSlicing{40, 7, 12.25};
    rv::app::recordRefresh(record, summary, rv::app::TimeSummary{2, 1.5, 2.5, 2.5, 2.5});
    CHECK(record.outcome == "done");
    CHECK(record.error.empty());
    CHECK(record.slices == 40);
    CHECK(record.served == 7);
    record.endFrame = 9;
    record.stopMs = 15.0;
    rv::app::CatalogView view;
    view.selectedId = "0123456789abcdef";
    view.selectedPosition = 4;
    record.before = view;
    record.after = view;
    catalog.refreshes.push_back(record);
    rv::app::RefreshRecord refused;
    refused.trigger = "user";
    refused.outcome = "unavailable";
    refused.error = "the catalog is not open";
    catalog.refreshes.push_back(refused);
    report.catalog = catalog;

    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "test");
    INFO((parsed ? std::string() : parsed.error().toString()));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->catalog.has_value());
    CHECK(parsed->catalog->refreshes == catalog.refreshes);
}
