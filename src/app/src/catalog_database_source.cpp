#include <rv/app/catalog_database_source.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/catalog/metadata.hpp>
#include <rv/ui/ui_assets.hpp>

namespace rv::app {
namespace {

namespace qc = ui::quick_catalog;

/// The page fetches whose times are kept (the percentiles of a long scroll).
constexpr std::size_t kMaxPageSamples = 100000;
/// Games whose own readiness is kept; past it the cache starts again (the rows read ask again).
constexpr std::size_t kMaxReadiness = 4096;

/// The launch state of a game that is not available: its availability alone (ADR-0008 §8: the first that applies).
[[nodiscard]] GameLaunchState unavailableState(catalog::Availability availability) {
    if (availability == catalog::Availability::Missing) {
        return GameLaunchState{catalog::LaunchReadiness::RomMissing, "rom-missing", {}, false};
    }
    return GameLaunchState{catalog::LaunchReadiness::SourceUnavailable, "source-unavailable", {}, false};
}

} // namespace

std::string_view stateKeyOf(catalog::LaunchReadiness readiness) noexcept {
    switch (readiness) {
    case catalog::LaunchReadiness::SourceUnavailable:
    case catalog::LaunchReadiness::RomMissing:
        return qc::kStateUnavailable;
    case catalog::LaunchReadiness::RequiresConfiguration:
        return qc::kStateNeedsConfiguration;
    case catalog::LaunchReadiness::EmulatorNotFound:
    case catalog::LaunchReadiness::CoreNotFound:
        return qc::kStateEmulatorMissing;
    case catalog::LaunchReadiness::Ready:
        return {};
    }
    return {};
}

CatalogDatabaseSource::CatalogDatabaseSource(CatalogWorker& worker) : m_worker(worker) {}

CatalogDatabaseSource::~CatalogDatabaseSource() = default;

CatalogDatabaseSource::Collected CatalogDatabaseSource::collect(CatalogResults& results) {
    Collected collected;
    if (results.opened) {
        CatalogOpened& opened = *results.opened;
        m_state = opened.state;
        m_openError = opened.state == CatalogState::Ready ? opened.libraryError : opened.error;
        m_library = opened.library;
        m_systems = std::move(opened.systems);
        m_pages = std::move(opened.pages);
        m_firstList = std::move(opened.firstList);
        m_interrupted = opened.interrupted.size();
        collected.opened = true;
        results.opened.reset();
        // A query asked while the catalog was opening goes to the worker now (or takes the first list).
        if (m_state == CatalogState::Ready && m_pendingSerial && !m_arrived) {
            dispatch();
        }
    }
    if (results.refreshed && results.refreshed->systems) {
        // A refresh resolved the systems again after its commit: their chips and readiness, a new generation. The
        // games' own readiness is asked again as their rows are read (QuickCatalogModel::refresh reads them).
        CatalogRefreshed& refreshed = *results.refreshed;
        m_systems = std::move(*refreshed.systems);
        refreshed.systems.reset();
        if (m_state == CatalogState::Ready) {
            m_openError = refreshed.libraryError;
        }
        m_generation = refreshed.generation;
        m_readiness.clear();
        m_asked.clear();
        collected.refreshed = true;
        collected.rowsChanged = true;
    }
    if (results.list) {
        // Only the list of the query still asked for: an older one was replaced by it.
        if (m_pendingSerial && results.list->serial == *m_pendingSerial) {
            m_arrived = std::move(*results.list);
        }
        results.list.reset();
    }
    for (GameReadiness& readiness : results.readiness) {
        if (readiness.generation != m_generation) {
            continue; // resolved before a refresh resolved the systems again: asked again when its row is read
        }
        m_asked.erase(readiness.game.value());
        if (!readiness.known) {
            continue; // gone, or unreadable now: its row keeps its system's, and asks again when read again
        }
        // Its rows showed its system's meanwhile (an available game: unavailable ones are never asked for).
        collected.rowsChanged = collected.rowsChanged || readiness.ownState;
        if (m_readiness.size() >= kMaxReadiness) {
            m_readiness.clear();
        }
        m_readiness[readiness.game.value()] = std::move(readiness.state);
    }
    results.readiness.clear();
    for (const auto& [system, choice] : results.verified) {
        for (CatalogSystem& shown : m_systems) {
            if (shown.name == system && shown.choice == choice &&
                shown.launch.readiness == catalog::LaunchReadiness::Ready && !shown.launch.verified) {
                shown.launch.verified = true;
                collected.rowsChanged = true;
            }
        }
        // The games' own states may rest on that choice too: they are asked again as their rows are read.
        if (!m_readiness.empty()) {
            m_readiness.clear();
            collected.rowsChanged = true;
        }
    }
    results.verified.clear();
    if (results.ended) {
        // Nothing asked of the worker will be answered any more: nothing is waited for (poll() fails a pending list).
        m_ended = true;
        m_asked.clear();
        results.ended = false;
    }
    return collected;
}

bool CatalogDatabaseSource::waiting() const noexcept {
    return !m_ended && (m_state == CatalogState::Opening || m_pendingSerial.has_value() || !m_asked.empty());
}

void CatalogDatabaseSource::dispatch() {
    if (m_firstList && m_pendingRequest == catalog::CatalogQuery{}) {
        // The default list the worker materialized with the opening: no round trip (NFR-01). It serves until a list is
        // shown, as the screen asks for it again once the catalog opened (QuickCatalogModel::refresh()).
        CatalogList list = *m_firstList;
        list.serial = *m_pendingSerial;
        m_arrived = std::move(list);
        return;
    }
    m_worker.query(*m_pendingSerial, m_pendingRequest);
}

std::optional<TimeSummary> CatalogDatabaseSource::pageTimes() const {
    std::vector<double> samples = m_pageMs;
    return summarizeTimes(samples);
}

const CatalogSystem* CatalogDatabaseSource::findSystem(std::string_view name) const {
    const auto found = std::find_if(m_systems.begin(), m_systems.end(),
                                    [&](const CatalogSystem& system) { return system.name.str() == name; });
    return found == m_systems.end() ? nullptr : &*found;
}

Expected<std::vector<ui::PlatformFilter>> CatalogDatabaseSource::platforms() {
    std::vector<ui::PlatformFilter> filters;
    filters.reserve(m_systems.size());
    for (const CatalogSystem& system : m_systems) {
        filters.push_back(ui::PlatformFilter{system.name.str(), system.fullName, system.games});
    }
    return filters;
}

Expected<ui::QueryState> CatalogDatabaseSource::query(const ui::CatalogSourceQuery& query) {
    if (m_state != CatalogState::Opening && m_state != CatalogState::Ready) {
        m_pendingSerial.reset();
        m_ids.clear();
        m_byId.clear();
        return makeError(ErrorCode::Unavailable, "the catalog is not open", std::string(toString(m_state)));
    }
    catalog::CatalogQuery request;
    request.text = query.text;
    if (!query.platform.empty()) {
        auto system = catalog::SystemName::parse(query.platform);
        if (!system || (m_state == CatalogState::Ready && findSystem(query.platform) == nullptr)) {
            m_pendingSerial.reset();
            m_ids.clear();
            m_byId.clear();
            return makeError(ErrorCode::InvalidArgument, "the catalog has no system of that name", query.platform);
        }
        request.systems.push_back(std::move(*system));
    }
    m_pendingSerial = ++m_serial;
    m_pendingQuery = query;
    m_pendingRequest = std::move(request);
    m_arrived.reset();
    if (m_state == CatalogState::Ready) {
        dispatch();
    } // else asked of the worker once the catalog opened (collect())
    return ui::QueryState::Pending;
}

Expected<ui::QueryState> CatalogDatabaseSource::poll() {
    if (!m_pendingSerial) {
        return ui::QueryState::Ready;
    }
    if (m_state != CatalogState::Opening && m_state != CatalogState::Ready) {
        // The catalog did not open: the query asked before it knew gets its error now.
        m_pendingSerial.reset();
        m_ids.clear();
        m_byId.clear();
        return makeError(ErrorCode::Unavailable, "the catalog is not open", std::string(toString(m_state)));
    }
    if (!m_arrived) {
        if (m_ended) {
            m_pendingSerial.reset();
            m_ids.clear();
            m_byId.clear();
            m_listed = true;
            return makeError(ErrorCode::Unavailable, "the catalog worker ended");
        }
        return ui::QueryState::Pending;
    }
    CatalogList list = std::move(*m_arrived);
    m_arrived.reset();
    m_pendingSerial.reset();
    m_listedQuery = m_pendingQuery;
    m_listed = true;
    // Later lists come from the worker: the first list may be stale by then (an import, a refresh).
    m_firstList.reset();
    if (list.error) {
        m_ids.clear();
        m_byId.clear();
        return propagate(std::move(*list.error));
    }
    m_ids = std::move(list.ids);
    m_byId = std::move(list.byId);
    return ui::QueryState::Ready;
}

Status CatalogDatabaseSource::rows(std::size_t first, std::size_t count, std::vector<ui::CatalogEntry>& out) {
    const std::span<const catalog::GameId> page = catalog::pageOf(m_ids, first, count);
    out.resize(page.size());
    if (page.empty()) {
        return ok();
    }
    if (!m_pages) {
        out.clear();
        return makeError(ErrorCode::Unavailable, "the catalog is not open");
    }
    const auto start = std::chrono::steady_clock::now();
    auto fetched = m_pages->fetch(page);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (m_pageMs.size() < kMaxPageSamples) {
        m_pageMs.push_back(ms);
    }
    if (!fetched) {
        out.clear();
        spdlog::warn("Catalog page of {} game(s) from position {} cannot be read: {}", page.size(), first,
                     fetched.error().toString());
        return propagate(fetched.error());
    }
    m_fetched = std::move(*fetched);
    // fetch() gives one row per id that has one, in the ids' order: an id without a row (an import removed it since
    // the list was materialized) keeps its place with its id alone.
    m_toAsk.clear();
    std::size_t next = 0;
    for (std::size_t index = 0; index < page.size(); ++index) {
        ui::CatalogEntry& entry = out[index];
        if (next < m_fetched.size() && m_fetched[next].id == page[index]) {
            fill(m_fetched[next], entry);
            ++next;
        } else {
            entry = ui::CatalogEntry{};
            entry.id = page[index].toString();
        }
    }
    if (!m_toAsk.empty()) {
        m_worker.readiness(m_toAsk);
    }
    return ok();
}

std::optional<ui::CatalogEntry> CatalogDatabaseSource::entryOf(const catalog::GameId& game) {
    if (!m_pages || !game.isValid()) {
        return std::nullopt;
    }
    auto fetched = m_pages->fetch(std::span<const catalog::GameId>(&game, 1));
    if (!fetched) {
        spdlog::warn("Catalog row of game {} cannot be read: {}", game.toString(), fetched.error().toString());
        return std::nullopt;
    }
    if (fetched->empty() || fetched->front().id != game) {
        return std::nullopt;
    }
    m_toAsk.clear();
    ui::CatalogEntry entry;
    fill(fetched->front(), entry);
    if (!m_toAsk.empty()) {
        m_worker.readiness(m_toAsk);
    }
    return entry;
}

void CatalogDatabaseSource::fill(const catalog::CatalogRow& row, ui::CatalogEntry& entry) {
    entry.id = row.id.toString();
    entry.title = row.title;
    entry.platform = row.systemFullName.empty() ? row.system.str() : row.systemFullName;
    entry.year = row.releaseDate ? row.releaseDate->year : 0;
    entry.developer = row.developer;
    entry.publisher = row.publisher;
    entry.genre = row.genre;
    entry.players = row.players ? catalog::formatPlayers(*row.players) : std::string();
    entry.description = row.description;
    GameLaunchState launch;
    if (row.availability != catalog::Availability::Available) {
        launch = unavailableState(row.availability);
    } else if (const auto own = m_readiness.find(row.id.value()); own != m_readiness.end()) {
        launch = own->second;
    } else {
        if (const CatalogSystem* system = findSystem(row.system.str())) {
            launch = system->launch;
        }
        if (m_asked.insert(row.id.value()).second) {
            m_toAsk.push_back(row.id);
        }
    }
    entry.state = std::string(stateKeyOf(launch.readiness));
    entry.readiness = std::string(catalog::toString(launch.readiness));
    entry.blocker = launch.blocker;
    entry.blockerDetail = launch.blockerMessage;
    entry.verified = launch.verified;
}

std::optional<std::size_t> CatalogDatabaseSource::position(std::string_view id) const {
    const auto game = catalog::GameId::parse(id);
    if (!game) {
        return std::nullopt;
    }
    const auto found = std::lower_bound(m_byId.begin(), m_byId.end(), std::make_pair(game->value(), std::uint32_t{0}));
    if (found == m_byId.end() || found->first != game->value()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace rv::app
