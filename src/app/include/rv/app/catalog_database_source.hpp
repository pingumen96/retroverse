#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/catalog_worker.hpp>
#include <rv/app/run_report.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/core/expected.hpp>
#include <rv/ui/catalog_source.hpp>

namespace rv::app {

/// The quick catalog's games from the ES-DE catalog database (I2 step 13; ADR-0008 §5; docs/i2/step-6.md's source
/// contract): a ui::CatalogSource over a CatalogWorker, on the main thread, never waiting on the database.
///  - query() hands the materialization to the worker (SqliteCatalogQuery::run on its connection) and answers Pending;
///    poll() switches to the worker's list once collect() brought it (only the latest query's list), with its index by
///    GameId for position();
///  - rows() fetches a page by ids on the main thread's own read-only connection (CatalogOpened::pages: within
///    ADR-0008's 5 ms), timed (pageTimes());
///  - platforms(): one chip per system with games (CatalogSystem), its key the ES-DE system's name, its label the full
///    name;
///  - each row carries its launch readiness: an unavailable game's from its availability, else the game's own once the
///    worker resolved it (CatalogWorker::readiness, asked for the rows read), its system's until then; collect() says
///    when rows already read would show other data now (QuickCatalogModel::reread()).
/// Before the catalog opened queries stay Pending; a catalog that did not open answers them with Unavailable (the list
/// empty; CatalogState says why).
class CatalogDatabaseSource final : public ui::CatalogSource {
public:
    /// `worker` must outlive the source.
    explicit CatalogDatabaseSource(CatalogWorker& worker);
    ~CatalogDatabaseSource() override;
    CatalogDatabaseSource(const CatalogDatabaseSource&) = delete;
    CatalogDatabaseSource& operator=(const CatalogDatabaseSource&) = delete;

    /// What collect() found.
    struct Collected {
        /// The catalog's opening ended (CatalogResults::opened): read the platforms and the list again.
        bool opened = false;
        /// Rows already read may show other data now (a game's or a system's readiness).
        bool rowsChanged = false;
    };
    /// Takes its part of the worker's answers out of `results` (the opening, the latest list, readiness, verified
    /// choices); launches and sessions stay for the caller. Main thread, before the model polls.
    Collected collect(CatalogResults& results);

    [[nodiscard]] CatalogState state() const noexcept { return m_state; }
    /// Why the catalog did not open; and why its launches cannot be resolved (ES-DE's configuration unreadable).
    [[nodiscard]] const std::optional<Error>& openError() const noexcept { return m_openError; }
    [[nodiscard]] const catalog::LibraryId& library() const noexcept { return m_library; }
    [[nodiscard]] const std::vector<CatalogSystem>& systems() const noexcept { return m_systems; }
    [[nodiscard]] std::size_t interruptedSessions() const noexcept { return m_interrupted; }
    /// Something the screen waits for is with the worker: the opening, a list, readiness.
    [[nodiscard]] bool waiting() const noexcept;
    /// The page fetches so far; nothing before the first.
    [[nodiscard]] std::optional<PageFetchTimes> pageTimes() const;
    /// The list a query was materialized from last: its search text and system.
    [[nodiscard]] const ui::CatalogSourceQuery& listedQuery() const noexcept { return m_listedQuery; }

    [[nodiscard]] Expected<std::vector<ui::PlatformFilter>> platforms() override;
    [[nodiscard]] Expected<ui::QueryState> query(const ui::CatalogSourceQuery& query) override;
    [[nodiscard]] Expected<ui::QueryState> poll() override;
    [[nodiscard]] std::size_t count() const noexcept override { return m_ids.size(); }
    [[nodiscard]] Status rows(std::size_t first, std::size_t count, std::vector<ui::CatalogEntry>& out) override;
    [[nodiscard]] std::optional<std::size_t> position(std::string_view id) const override;

    /// The readiness a row of `system` shows while its game's own is not known, and the system's chip; null for a
    /// system without games.
    [[nodiscard]] const CatalogSystem* findSystem(std::string_view name) const;

private:
    /// Fills `entry` from `row` (text, year, players, readiness).
    void fill(const catalog::CatalogRow& row, ui::CatalogEntry& entry);

    CatalogWorker& m_worker;
    CatalogState m_state = CatalogState::Opening;
    std::optional<Error> m_openError;
    catalog::LibraryId m_library;
    std::vector<CatalogSystem> m_systems;
    std::unique_ptr<catalog_db::SqliteCatalogQuery> m_pages;
    std::size_t m_interrupted = 0;

    /// The list the UI reads, its index by GameId value, and the query it came from.
    std::vector<catalog::GameId> m_ids;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> m_byId;
    ui::CatalogSourceQuery m_listedQuery;
    /// The query with the worker, and the list it brought (collect()) until poll() switches to it.
    std::uint64_t m_serial = 0;
    std::optional<std::uint64_t> m_pendingSerial;
    ui::CatalogSourceQuery m_pendingQuery;
    std::optional<CatalogList> m_arrived;

    /// The games' own readiness (by GameId value), and the games asked of the worker.
    std::map<std::uint64_t, GameLaunchState> m_readiness;
    std::set<std::uint64_t> m_asked;
    /// Page fetch times, milliseconds (the first kMaxPageSamples).
    std::vector<double> m_pageMs;
    std::vector<catalog::GameId> m_toAsk;
    std::vector<catalog::CatalogRow> m_fetched;
};

/// The state key (ui::quick_catalog::kState*) the documents label for a readiness: "unavailable" for a source
/// unavailable or a ROM missing, "needs-configuration", "emulator-missing" for an emulator or core not found, empty when
/// ready.
[[nodiscard]] std::string_view stateKeyOf(catalog::LaunchReadiness readiness) noexcept;

} // namespace rv::app
