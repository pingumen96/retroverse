#include <rv/app/catalog_worker.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>
#include <variant>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/catalog_launch_library.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/ui/ui_assets.hpp>

namespace rv::app {
namespace {

namespace qc = ui::quick_catalog;
using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct QueryJob {
    std::uint64_t serial = 0;
    catalog::CatalogQuery query;
    /// When the main thread asked (CatalogList::waitedMs).
    Clock::time_point asked{};
};
struct ReadinessJob {
    std::vector<catalog::GameId> games;
};
struct ResolveJob {
    std::uint64_t token = 0;
    catalog::GameId game;
};
enum class SessionStep : std::uint8_t { Begin, Finish };
struct SessionJob {
    std::uint64_t token = 0;
    SessionStep step = SessionStep::Begin;
    catalog::PlaySession session;
    catalog::SystemName system;
    std::filesystem::path program;
};
/// The catalog's refresh (I3 step 13r): one at a time.
struct RefreshJob {};
/// The room's snapshot (I3 step 10b): the latest request only.
struct RoomJob {
    RoomSnapshotRequest request;
};
using Job = std::variant<QueryJob, ReadinessJob, ResolveJob, SessionJob, RefreshJob, RoomJob>;

/// What a slice boundary of a refresh may answer: never the refresh itself nor the room's snapshot (which waits for the
/// catalog the refresh makes); while the import's write transaction is open, only reads (lists on the worker's reader,
/// readiness), as launches and session writes use the writer.
[[nodiscard]] bool servableDuringRefresh(const Job& job, bool writing) noexcept {
    if (std::holds_alternative<RefreshJob>(job) || std::holds_alternative<RoomJob>(job)) {
        return false;
    }
    return !writing || std::holds_alternative<QueryJob>(job) || std::holds_alternative<ReadinessJob>(job);
}

/// The launch state of a resolution, as the card shows it.
[[nodiscard]] GameLaunchState launchStateOf(const catalog::LaunchResolution& resolution, bool verified) {
    GameLaunchState state;
    state.readiness = resolution.readiness();
    for (const catalog::ReadinessBlocker& blocker : resolution.translation.blockers) {
        if (blocker.readiness == state.readiness) {
            state.blocker = blocker.code;
            state.blockerMessage = blocker.message;
            break;
        }
    }
    state.verified = verified && state.readiness == catalog::LaunchReadiness::Ready;
    return state;
}

/// The readiness of a game whose launch cannot be resolved: the library's configuration cannot be read.
[[nodiscard]] GameLaunchState libraryUnreadable(const Error& error) {
    return GameLaunchState{catalog::LaunchReadiness::RequiresConfiguration, std::string(kLibraryUnreadableCode),
                           error.message, false};
}

} // namespace

std::string_view toString(CatalogState state) noexcept {
    switch (state) {
    case CatalogState::Opening:
        return "opening";
    case CatalogState::Ready:
        return "ready";
    case CatalogState::NoLibrary:
        return "no-library";
    case CatalogState::NoDatabase:
        return "no-database";
    case CatalogState::DataDirectory:
        return "data-directory";
    case CatalogState::LibraryMismatch:
        return "library-mismatch";
    case CatalogState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::string_view noticeOf(CatalogState state) noexcept {
    switch (state) {
    case CatalogState::Opening:
        return qc::kNoticeOpening;
    case CatalogState::Ready:
        return {};
    case CatalogState::NoLibrary:
        return qc::kNoticeNoLibrary;
    case CatalogState::NoDatabase:
        return qc::kNoticeNoDatabase;
    case CatalogState::DataDirectory:
        return qc::kNoticeDataDirectory;
    case CatalogState::LibraryMismatch:
        return qc::kNoticeLibraryMismatch;
    case CatalogState::Unavailable:
        return qc::kNoticeUnavailable;
    }
    return qc::kNoticeUnavailable;
}

/// What the main thread and the worker share, under `mutex` (the interruptible connection under its own).
struct CatalogWorker::Shared {
    mutable std::mutex mutex;
    std::condition_variable work;
    std::condition_variable ended;
    std::deque<Job> jobs;
    bool stopping = false;
    /// A job (or the opening) runs.
    bool busy = true;
    /// The thread function returned.
    bool finished = false;
    /// The opening's result was posted (a thread that fails before posts an Unavailable one).
    bool openedPosted = false;
    /// A refresh is queued or runs (refresh() refuses another).
    bool refreshing = false;
    /// The import's cancel flag (ImportOptions::cancel): set by stop().
    std::atomic<bool> cancel{false};
    CatalogResults results;
    std::function<void()> wake;

    /// The worker's query connection while it may run a statement: stop() interrupts it.
    std::mutex connectionMutex;
    catalog_db::Connection* interruptible = nullptr;
    /// The writer while a refresh's import holds its write transaction open: stop() interrupts it too (the import is
    /// rolled back), and only then, so that a session write is never interrupted.
    catalog_db::Connection* importWriter = nullptr;

    /// Applies `change` to the results, then wakes the host.
    template <typename Change>
    void post(Change&& change) {
        std::function<void()> wakeHost;
        {
            const std::lock_guard lock(mutex);
            change(results);
            wakeHost = wake;
        }
        if (wakeHost) {
            wakeHost();
        }
    }
};

namespace {

/// The worker's own state: the database and everything that reads or writes it, used on the worker's thread only.
class Runner final : public RefreshHost {
public:
    Runner(std::shared_ptr<CatalogWorker::Shared> shared, CatalogWorker::Options options)
        : m_shared(std::move(shared)), m_options(std::move(options)) {}
    /// Also when an exception ends run(): stop() never interrupts a connection that is gone.
    ~Runner() override {
        const std::lock_guard lock(m_shared->connectionMutex);
        m_shared->interruptible = nullptr;
        m_shared->importWriter = nullptr;
    }
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    void run() {
        open();
        if (m_state == CatalogState::Ready) {
            loop();
        }
        flushAtStop();
        close();
    }

private:
    using Shared = CatalogWorker::Shared;

    // --- Opening -------------------------------------------------------------------------------------------------

    void open() {
        const Clock::time_point start = Clock::now();
        CatalogOpened opened;
        opened.database = catalog_db::databaseFile(m_options.paths);
        const Status status = openCatalog(opened);
        if (!status) {
            opened.error = status.error();
            if (opened.state == CatalogState::Opening) {
                opened.state = CatalogState::Unavailable;
            }
        }
        m_state = opened.state;
        opened.milliseconds = millisecondsSince(start);
        opened.at = std::chrono::system_clock::now();
        if (m_state == CatalogState::Ready) {
            opened.firstList = firstList();
            spdlog::info("Catalog opened on the worker in {:.0f} ms: library '{}', {} system(s) with games{}",
                         opened.milliseconds, opened.library.str(), opened.systems.size(),
                         opened.interrupted.empty()
                             ? std::string()
                             : fmt::format(", {} play session(s) closed as interrupted", opened.interrupted.size()));
        } else {
            // The error names the user's folders (the library's, the data directory): at debug level only.
            spdlog::info("Catalog not opened: {}", toString(m_state));
            if (opened.error) {
                spdlog::debug("Catalog not opened: {}", opened.error->toString());
            }
        }
        m_shared->post([&](CatalogResults& results) {
            results.opened = std::move(opened);
            m_shared->openedPosted = true;
        });
    }

    /// The default list (every game, Title order), which the quick catalog asks for first: materialized with the opening
    /// so that the first page needs no round trip to the worker (NFR-01). Nothing when it cannot be read (the query then
    /// goes to the worker as any other).
    std::optional<CatalogList> firstList() {
        const Clock::time_point start = Clock::now();
        auto ids = m_query->run(catalog::CatalogQuery{});
        if (!ids) {
            spdlog::warn("Catalog: the first list cannot be read now: {}", ids.error().toString());
            return std::nullopt;
        }
        CatalogList list = listOf(0, std::move(*ids));
        list.milliseconds = millisecondsSince(start);
        list.at = std::chrono::system_clock::now();
        spdlog::debug("Catalog first list: {} game(s) in {:.1f} ms", list.ids.size(), list.milliseconds);
        return list;
    }

    /// A list of `ids`, with its index by GameId.
    [[nodiscard]] static CatalogList listOf(std::uint64_t serial, std::vector<catalog::GameId> ids) {
        CatalogList list;
        list.serial = serial;
        list.ids = std::move(ids);
        list.byId.reserve(list.ids.size());
        for (std::size_t position = 0; position < list.ids.size(); ++position) {
            list.byId.emplace_back(list.ids[position].value(), static_cast<std::uint32_t>(position));
        }
        std::sort(list.byId.begin(), list.byId.end());
        return list;
    }

    /// Opens the catalog into `opened`, its state set as it goes (an error leaves Opening: Unavailable).
    Status openCatalog(CatalogOpened& opened) {
        const std::filesystem::path libraryFile = m_options.paths.configDir() / esde::kLibraryFileName;
        std::error_code ignored;
        if (!std::filesystem::exists(libraryFile, ignored)) {
            opened.state = CatalogState::NoLibrary;
            return makeError(ErrorCode::NotFound, "no library.json: run 'retroverse-cli library create'",
                             core::pathToUtf8Lossy(libraryFile));
        }
        auto profile = esde::readLibraryFile(libraryFile);
        if (!profile) {
            return propagate(profile.error());
        }
        m_profile = std::move(*profile);
        opened.library = m_profile.id;
        // The data-directory guard (ADR-0008 §6), before anything is written into the database.
        if (const Status guarded =
                core::checkUserDataDirectory(m_options.paths.userDataRoot(), esde::readOnlyFolders(m_profile));
            !guarded) {
            opened.state = CatalogState::DataDirectory;
            return guarded;
        }
        if (!std::filesystem::exists(opened.database, ignored)) {
            opened.state = CatalogState::NoDatabase;
            return makeError(ErrorCode::NotFound, "no catalog database: run 'retroverse-cli import'",
                             core::pathToUtf8Lossy(opened.database));
        }
        catalog_db::DatabaseOptions options;
        options.create = false;
        options.busyTimeout = m_options.busyTimeout;
        options.lockWait = m_options.lockWait;
        auto database = catalog_db::openCatalogDatabase(opened.database, options);
        if (!database) {
            if (database.error().code == ErrorCode::NotFound) {
                opened.state = CatalogState::NoDatabase;
            }
            return propagate(database.error());
        }
        m_database = std::make_unique<catalog_db::Database>(std::move(*database));
        auto store = catalog_db::SessionStore::open(m_database->writer());
        if (!store) {
            return propagate(store.error());
        }
        m_store = std::move(*store);
        auto reader = m_database->openReader();
        if (!reader) {
            return propagate(reader.error());
        }
        auto query = catalog_db::SqliteCatalogQuery::open(std::move(*reader));
        if (!query) {
            return propagate(query.error());
        }
        {
            const std::lock_guard lock(m_shared->connectionMutex);
            m_query = std::move(*query);
            m_shared->interruptible = &m_query->connection();
        }
        auto pageReader = m_database->openReader();
        if (!pageReader) {
            return propagate(pageReader.error());
        }
        auto pages = catalog_db::SqliteCatalogQuery::open(std::move(*pageReader));
        if (!pages) {
            return propagate(pages.error());
        }
        opened.pages = std::move(*pages);

        // ADR-0008 §9: the sessions left open by owners that are gone are closed as interrupted, and only those.
        const catalog_db::OwnerAlive alive =
            m_options.ownerAlive ? m_options.ownerAlive : [](const catalog::SessionOwner& owner) {
                return platform_win::processLiveness(platform_win::ProcessIdentity{
                           owner.processId, owner.creationTime}) != platform_win::ProcessLiveness::Gone;
            };
        if (auto closed = m_store->closeInterrupted(alive, std::chrono::system_clock::now())) {
            opened.interrupted = std::move(*closed);
        } else {
            spdlog::warn("The play sessions left open by processes that are gone could not be closed now: {}",
                         closed.error().toString());
        }

        auto summaries = m_query->systems();
        if (!summaries) {
            return propagate(summaries.error());
        }
        const bool holdsLibrary = std::any_of(summaries->begin(), summaries->end(), [&](const auto& summary) {
            return summary.library == m_profile.id;
        });
        if (!summaries->empty() && !holdsLibrary) {
            opened.state = CatalogState::LibraryMismatch;
            return makeError(ErrorCode::Conflict,
                             fmt::format("library.json names the library '{}', which the catalog does not hold",
                                         m_profile.id.str()));
        }

        opened.systems = resolveSystems(*summaries);
        opened.libraryError = m_libraryError;
        opened.state = CatalogState::Ready;
        return ok();
    }

    /// The library's configuration read (ES-DE's settings and systems, the emulator profiles), the choices and
    /// verifications read, and each system with games of `summaries` resolved (its readiness): at the opening, and again
    /// after a refresh's commit. Returns the systems in the chips' order.
    std::vector<CatalogSystem> resolveSystems(const std::vector<catalog_db::SystemSummary>& summaries) {
        m_library.reset();
        m_libraryError.reset();
        auto library = loadCatalogLaunchLibrary(m_profile, m_options.paths);
        if (library) {
            m_library = std::move(*library);
            for (const catalog::Issue& issue : m_library->issues) {
                spdlog::debug("Catalog library: {}: {}", issue.code, issue.reason);
            }
        } else {
            spdlog::warn("Catalog: {}; its games cannot be launched", library.error().toString());
            m_libraryError = library.error();
        }
        readChoices();
        std::vector<CatalogSystem> systems;
        m_systems.clear();
        for (const catalog_db::SystemSummary& summary : summaries) {
            if (summary.library != m_profile.id || summary.games == 0) {
                continue;
            }
            systems.push_back(systemState(summary));
            m_systems.emplace(summary.system.name.str(), systems.back());
        }
        return systems;
    }

    /// assign's choices for the systems and the verifications, from the database.
    void readChoices() {
        m_assigned.clear();
        if (auto choices = m_store->systemChoices(m_profile.id)) {
            for (catalog_db::SystemChoiceRow& row : *choices) {
                m_assigned.emplace(row.system.str(), std::move(row.stored.choice));
            }
        } else if (choices.error().code != ErrorCode::InvalidArgument) {
            spdlog::warn("Catalog: the choices made with 'retroverse-cli assign' cannot be read: {}",
                         choices.error().toString());
        }
        if (auto verifications = m_store->verifications(m_profile.id)) {
            for (const catalog_db::LaunchVerification& verification : *verifications) {
                m_verified.insert(verificationKey(verification.system, verification.choice));
            }
        }
    }

    [[nodiscard]] static std::tuple<std::string, int, std::string> verificationKey(const catalog::SystemName& system,
                                                                                  const launcher::LaunchChoice& choice) {
        return {system.str(), static_cast<int>(choice.kind), choice.value};
    }

    [[nodiscard]] bool isVerified(const catalog::SystemName& system, const launcher::LaunchChoice& choice) const {
        return m_verified.contains(verificationKey(system, choice));
    }

    [[nodiscard]] std::optional<launcher::LaunchChoice> assignedFor(const catalog::SystemName& system) const {
        const auto found = m_assigned.find(system.str());
        return found == m_assigned.end() ? std::nullopt : std::optional<launcher::LaunchChoice>(found->second);
    }

    [[nodiscard]] CatalogSystem systemState(const catalog_db::SystemSummary& summary) const {
        CatalogSystem system;
        system.name = summary.system.name;
        system.fullName = summary.system.fullName.empty() ? summary.system.name.str() : summary.system.fullName;
        system.games = summary.games;
        if (!m_library) {
            system.launch = libraryUnreadable(*m_libraryError);
            return system;
        }
        const catalog::LaunchResolution resolution =
            resolveSystemLaunch(*m_library, summary.system, assignedFor(summary.system.name));
        system.choice = resolution.chosen.choice;
        system.perGame = resolvedPerGame(resolution);
        system.launch = launchStateOf(resolution, isVerified(system.name, system.choice));
        spdlog::debug("Catalog system {}: {} ({}){}", system.name.str(), catalog::toString(system.launch.readiness),
                      system.launch.blocker, system.perGame ? ", resolved per game" : "");
        return system;
    }

    // --- The loop ------------------------------------------------------------------------------------------------

    void loop() {
        while (true) {
            std::optional<Job> job;
            {
                std::unique_lock lock(m_shared->mutex);
                m_shared->busy = false;
                const auto ready = [&] { return m_shared->stopping || !m_shared->jobs.empty(); };
                if (pendingSessions()) {
                    m_shared->work.wait_for(lock, m_options.sessionRetryInterval, ready);
                } else {
                    m_shared->work.wait(lock, ready);
                }
                if (m_shared->stopping) {
                    return;
                }
                if (!m_shared->jobs.empty()) {
                    job = std::move(m_shared->jobs.front());
                    m_shared->jobs.pop_front();
                    m_shared->busy = true;
                }
            }
            if (job) {
                std::visit([this](auto& value) { handle(value); }, *job);
            }
            retryPendingSessions(false);
        }
    }

    void handle(QueryJob& job) {
        const Clock::time_point start = Clock::now();
        auto ids = m_options.runQuery ? m_options.runQuery(*m_query, job.query) : m_query->run(job.query);
        CatalogList list = listOf(job.serial, ids ? std::move(*ids) : std::vector<catalog::GameId>{});
        if (!ids) {
            list.error = ids.error();
            spdlog::warn("Catalog query failed: {}", ids.error().toString());
        }
        list.milliseconds = millisecondsSince(start);
        list.waitedMs = millisecondsSince(job.asked);
        list.at = std::chrono::system_clock::now();
        spdlog::debug("Catalog query {} ('{}', {} system(s)): {} game(s) in {:.1f} ms", job.serial, job.query.text,
                      job.query.systems.size(), list.ids.size(), list.milliseconds);
        m_shared->post([&](CatalogResults& results) {
            if (!results.list || results.list->serial <= list.serial) {
                results.list = std::move(list);
            }
        });
    }

    void handle(ReadinessJob& job) {
        std::vector<GameReadiness> states;
        states.reserve(job.games.size());
        for (const catalog::GameId game : job.games) {
            if (stopRequested()) {
                return;
            }
            if (auto state = gameState(game)) {
                states.push_back(std::move(*state));
            } else {
                // Answered all the same, so that the main thread does not wait for it.
                GameReadiness unknown;
                unknown.game = game;
                unknown.known = false;
                unknown.generation = m_generation;
                states.push_back(std::move(unknown));
            }
        }
        m_shared->post([&](CatalogResults& results) {
            results.readiness.insert(results.readiness.end(), std::make_move_iterator(states.begin()),
                                     std::make_move_iterator(states.end()));
        });
    }

    /// The readiness of `game` as the card shows it: its system's, unless it can differ (ADR-0008 §8, docs/i2/
    /// step-10.md): its availability, its <altemulator> (AlternativeEmulatorPerGame), Retroverse's choice for it, a
    /// folder game, a system whose resolution leaves something to each game. Nothing when the catalog no longer has it.
    std::optional<GameReadiness> gameState(catalog::GameId game) {
        auto details = m_query->details(game);
        if (!details || !*details) {
            if (!details) {
                spdlog::warn("Catalog: the readiness of {} cannot be read: {}", game.toString(),
                             details.error().toString());
            }
            return std::nullopt;
        }
        const catalog::CatalogEntry& entry = (*details)->indexed.entry;
        const auto system = m_systems.find(entry.system.str());
        GameReadiness readiness;
        readiness.game = game;
        readiness.generation = m_generation;
        if (!m_library) {
            readiness.state = libraryUnreadable(*m_libraryError);
        } else {
            auto choice = m_store->gameChoice(game);
            const std::optional<launcher::LaunchChoice> gameChoice =
                choice && *choice ? std::optional<launcher::LaunchChoice>((**choice).choice) : std::nullopt;
            const bool differs = system == m_systems.end() || system->second.perGame ||
                                 entry.availability != catalog::Availability::Available || gameChoice.has_value() ||
                                 (entry.file && entry.file->directory) ||
                                 (m_library->settings.alternativeEmulatorPerGame && !entry.altEmulator.empty());
            if (!differs) {
                readiness.state = system->second.launch;
            } else {
                const catalog::LaunchResolution resolution = resolveCatalogLaunch(
                    *m_library, **details, gameChoice, assignedFor(entry.system), GameFileCheck::Catalog);
                readiness.state = launchStateOf(resolution, isVerified(entry.system, resolution.chosen.choice));
            }
        }
        readiness.ownState = system == m_systems.end() || readiness.state != system->second.launch;
        return readiness;
    }

    void handle(ResolveJob& job) {
        ResolvedLaunch resolved;
        resolved.token = job.token;
        resolved.requested = job.game;
        if (const Status status = resolveLaunch(job.game, resolved); !status) {
            resolved.error = status.error();
            spdlog::warn("Launch of {} cannot be resolved: {}", job.game.toString(), status.error().toString());
        }
        m_shared->post([&](CatalogResults& results) { results.launches.push_back(std::move(resolved)); });
    }

    /// The launch of `requested` right before it starts (as retroverse-cli launch --game resolves it).
    Status resolveLaunch(catalog::GameId requested, ResolvedLaunch& resolved) {
        catalog::GameId id = requested;
        auto details = m_query->details(id);
        if (!details) {
            return propagate(details.error());
        }
        if (!*details) {
            // An alias (I2 step 11b) stands for the identity it names.
            auto alias = m_store->resolveGame(requested);
            if (!alias) {
                return propagate(alias.error());
            }
            if (*alias && **alias != requested) {
                id = **alias;
                details = m_query->details(id);
                if (!details) {
                    return propagate(details.error());
                }
            }
        }
        if (!*details) {
            return makeError(ErrorCode::NotFound, "the catalog no longer has this game (an import removed it)",
                             requested.toString());
        }
        const catalog_db::GameDetails& game = **details;
        const catalog::CatalogEntry& entry = game.indexed.entry;
        if (game.library != m_profile.id ||
            (entry.state != catalog::EntryState::Game && entry.state != catalog::EntryState::MissingFile)) {
            return makeError(ErrorCode::InvalidArgument, "not a game of the library", requested.toString());
        }
        resolved.title = entry.title;
        resolved.system = entry.system;
        auto choice = m_store->gameChoice(id);
        if (!choice) {
            return propagate(choice.error());
        }
        // The choices made since the catalog opened (another process's retroverse-cli assign) count too.
        readChoices();
        const std::optional<launcher::LaunchChoice> gameChoice =
            *choice ? std::optional<launcher::LaunchChoice>((**choice).choice) : std::nullopt;
        if (m_library) {
            resolved.resolution =
                resolveCatalogLaunch(*m_library, game, gameChoice, assignedFor(entry.system), GameFileCheck::Disk);
        } else {
            resolved.resolution.chosen.choice = gameChoice.value_or(launcher::LaunchChoice{});
            resolved.resolution.translation.blockers.push_back(catalog::ReadinessBlocker{
                catalog::LaunchReadiness::RequiresConfiguration, std::string(kLibraryUnreadableCode),
                m_libraryError->message});
        }
        if (const auto& request = resolved.resolution.translation.request) {
            resolved.guardAllowed = launcher::LaunchGuard::fromEnvironment().check(*request).allowed();
        }
        catalog::PlaySession& session = resolved.session;
        session.game = id;
        session.choice = resolved.resolution.chosen.choice;
        session.choiceSource = resolved.resolution.chosen.source;
        session.owner = m_options.owner;
        spdlog::info("Launch of {} ({} {}) resolved: {}", id.toString(), entry.system.str(),
                     session.choice.value.empty() ? std::string("the command") : session.choice.value,
                     catalog::toString(resolved.resolution.readiness()));
        return ok();
    }

    // --- The refresh (I3 step 13r) ----------------------------------------------------------------------------------

    void handle(RefreshJob& /*job*/) {
        const Clock::time_point start = Clock::now();
        spdlog::info("Catalog refresh: importing library '{}' on the worker", m_profile.id.str());
        CatalogRefreshed refreshed;
        RefreshSlicing slicing;
        const Expected<catalog::ImportResult> result = importLibrary(slicing);
        refreshed.summary = summarizeImport(result);
        refreshed.summary.slicing = slicing;
        if (refreshed.summary.outcome == RefreshOutcome::Done) {
            // The systems' readiness resolved again (their games, ES-DE's configuration and the emulators as they are
            // now, the choices): the opening's part after systems(). Answers resolved before are of an older generation.
            if (auto summaries = m_query->systems()) {
                refreshed.systems = resolveSystems(*summaries);
                refreshed.libraryError = m_libraryError;
                refreshed.generation = ++m_generation;
            } else {
                spdlog::warn("Catalog refresh: the systems cannot be read again: {}; their readiness stays",
                             summaries.error().toString());
            }
        }
        refreshed.summary.milliseconds = millisecondsSince(start);
        const RefreshSummary& summary = refreshed.summary;
        if (summary.outcome == RefreshOutcome::Done) {
            spdlog::info("Catalog refresh done in {:.0f} ms: {} entries, {} added, {} removed, {} changed, {} moved, {} "
                         "to review{}; {} slice(s), {} request(s) answered between them, the longest slice {:.1f} ms",
                         summary.milliseconds, summary.entries, summary.added, summary.removed, summary.changed,
                         summary.moved, summary.reviews,
                         summary.guarded == 0 ? std::string()
                                              : fmt::format(", {} system(s) kept by the guard", summary.guarded),
                         slicing.boundaries, slicing.served, slicing.longestSliceMs);
        } else {
            // The error may name the user's folders: at debug level only.
            spdlog::info("Catalog refresh {} after {:.0f} ms", toString(summary.outcome), summary.milliseconds);
            if (summary.error) {
                spdlog::debug("Catalog refresh: {}", summary.error->toString());
            }
        }
        m_shared->post([&](CatalogResults& results) {
            results.refreshProgress.reset();
            results.refreshed = std::move(refreshed);
            m_shared->refreshing = false;
        });
    }

    /// The import of the library the catalog opened with (library.json as read then), as retroverse-cli import makes
    /// it (EsdeLibrarySource, EsdeMediaResolver, IdentityPairing), on the worker's writer, sliced.
    Expected<catalog::ImportResult> importLibrary(RefreshSlicing& slicing) {
        const auto settings = esde::readSettings(m_profile.esdeHome);
        if (!settings) {
            return propagate(settings.error());
        }
        esde::EsdeMediaResolver resolver(esde::resolveMediaRoot(*settings, m_profile));
        auto store = catalog_db::SqliteCatalogStore::open(*m_database);
        if (!store) {
            return propagate(store.error());
        }
        std::unique_ptr<catalog::ILibrarySource> source =
            m_options.makeSource ? m_options.makeSource() : std::make_unique<esde::EsdeLibrarySource>();
        // Reconciliation (I2 step 11b): moved and renamed files keep their identity.
        catalog::IdentityPairing pairing;
        return runSlicedImport(RefreshPorts{*source, resolver, **store, &pairing}, m_profile, m_shared->cancel, *this,
                               slicing);
    }

    std::size_t serve(bool writing) override {
        std::size_t served = 0;
        while (true) {
            std::optional<Job> job;
            {
                const std::lock_guard lock(m_shared->mutex);
                if (m_shared->stopping) {
                    return served;
                }
                const auto found = std::find_if(m_shared->jobs.begin(), m_shared->jobs.end(),
                                                [&](const Job& queued) { return servableDuringRefresh(queued, writing); });
                if (found == m_shared->jobs.end()) {
                    break;
                }
                job = std::move(*found);
                m_shared->jobs.erase(found);
            }
            std::visit([this](auto& value) { handle(value); }, *job);
            ++served;
        }
        if (!writing) {
            retryPendingSessions(false);
        }
        return served;
    }

    std::size_t serveWhile(const std::function<void()>& work) override {
        bool done = false;
        std::exception_ptr failure;
        std::thread helper([&] {
            try {
                work();
            } catch (...) {
                failure = std::current_exception();
            }
            {
                const std::lock_guard lock(m_shared->mutex);
                done = true;
            }
            m_shared->work.notify_all();
        });
        std::size_t served = 0;
        while (true) {
            {
                std::unique_lock lock(m_shared->mutex);
                m_shared->work.wait(lock, [&] {
                    return done || m_shared->stopping ||
                           std::any_of(m_shared->jobs.begin(), m_shared->jobs.end(),
                                       [](const Job& queued) { return servableDuringRefresh(queued, false); });
                });
                if (done) {
                    break;
                }
                if (m_shared->stopping) {
                    // The cancel flag is set: the work ends at its next look at it.
                    m_shared->work.wait(lock, [&] { return done; });
                    break;
                }
            }
            served += serve(false);
        }
        helper.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return served;
    }

    void progress(const RefreshProgress& progress) override {
        m_shared->post([&](CatalogResults& results) { results.refreshProgress = progress; });
    }

    void writing(bool open) override {
        const std::lock_guard lock(m_shared->connectionMutex);
        m_shared->importWriter = open ? &m_database->writer() : nullptr;
    }

    // --- The room's snapshot (I3 step 10b) ------------------------------------------------------------------------

    void handle(RoomJob& job) {
        std::string fullName;
        if (const auto system = m_systems.find(job.request.system.str()); system != m_systems.end()) {
            fullName = system->second.fullName;
        }
        // ES-DE's settings as they are now: the media root it would use (library.json's override over them).
        const auto settings = esde::readSettings(m_profile.esdeHome);
        const esde::ResolvedRoot mediaRoot =
            esde::resolveMediaRoot(settings ? *settings : esde::EsdeSettings{}, m_profile);
        std::unique_ptr<catalog_db::SqliteCatalogStore> store;
        if (!job.request.placedGames.empty()) {
            if (auto opened = catalog_db::SqliteCatalogStore::open(*m_database)) {
                store = std::move(*opened);
            } else {
                spdlog::warn("Room snapshot: the aliases of the placed games cannot be read: {}",
                             opened.error().toString());
            }
        }
        RoomSnapshotResult result =
            takeRoomSnapshot(*m_query, store.get(), mediaRoot, m_profile.id, std::move(fullName), job.request);
        if (result.error) {
            spdlog::warn("Room snapshot of {} failed: {}", job.request.system.str(), result.error->toString());
        } else {
            spdlog::info("Room snapshot of {}: {} entries in {:.1f} ms (query {:.1f} ms, media {:.1f} ms, aliases "
                         "{:.1f} ms), media {}, {} with media paths, {} alias(es)",
                         job.request.system.str(), result.snapshot.entries.size(), result.milliseconds,
                         result.queryMs, result.mediaMs, result.aliasMs,
                         result.snapshot.mediaKnown ? "known" : "unknown", result.mediaPaths.size(),
                         result.aliases.size());
            if (!result.snapshot.mediaKnown) {
                // The reason names the media root, one of the user's folders: at debug level only.
                spdlog::debug("Room snapshot: media unknown: {}", result.snapshot.mediaUnknownReason);
            }
        }
        m_shared->post([&](CatalogResults& results) { results.room = std::move(result); });
    }

    // --- Play sessions -------------------------------------------------------------------------------------------

    struct Recording {
        std::unique_ptr<catalog_db::SessionRecorder> recorder;
        catalog::SystemName system;
        std::filesystem::path program;
        bool final = false;
    };

    void handle(SessionJob& job) {
        Recording& recording = m_recordings[job.token];
        if (job.step == SessionStep::Finish) {
            recording.final = true;
            recording.system = job.system;
            recording.program = job.program;
        }
        if (!recording.recorder) {
            recording.recorder = std::make_unique<catalog_db::SessionRecorder>(*m_store, job.session);
            // One attempt (the writer's busy timeout): a busy database keeps it pending.
            if (const Status begun = recording.recorder->begin(); !begun) {
                spdlog::warn("The play session of {} is not recorded yet: {}", job.session.game.toString(),
                             begun.error().toString());
            }
        } else {
            recording.recorder->update(job.session);
            if (const Status flushed = recording.recorder->flush(); !flushed) {
                spdlog::warn("The play session of {} is not recorded yet: {}", job.session.game.toString(),
                             flushed.error().toString());
            }
        }
        if (recording.final && !recording.recorder->pending()) {
            complete(job.token, recording, std::nullopt);
        }
    }

    [[nodiscard]] bool pendingSessions() const {
        return std::any_of(m_recordings.begin(), m_recordings.end(),
                           [](const auto& entry) { return entry.second.recorder && entry.second.recorder->pending(); });
    }

    /// Tries the pending writes again (every sessionRetryInterval while the loop waits); `atStop`: with the stop's
    /// patience, giving up on what still cannot be written.
    void retryPendingSessions(bool atStop) {
        if (!atStop && Clock::now() - m_lastRetry < m_options.sessionRetryInterval) {
            return;
        }
        m_lastRetry = Clock::now();
        std::vector<std::uint64_t> pending;
        for (const auto& [token, recording] : m_recordings) {
            if (recording.recorder && recording.recorder->pending()) {
                pending.push_back(token);
            }
        }
        for (const std::uint64_t token : pending) {
            Recording& recording = m_recordings.at(token);
            const Status written = atStop ? recording.recorder->finish(catalog_db::BusyRetry{m_options.stopPatience})
                                          : recording.recorder->flush();
            if (recording.final && !recording.recorder->pending()) {
                complete(token, recording, std::nullopt);
                continue;
            }
            if (atStop && recording.recorder->pending()) {
                const Error error =
                    written ? Error{ErrorCode::Conflict, "the database stayed busy", {}} : written.error();
                spdlog::error("The play session of {} could not be recorded: {}",
                              recording.recorder->session().game.toString(), error.toString());
                if (recording.final) {
                    complete(token, recording, error);
                }
            }
        }
    }

    /// A final session written (or given up on): its launch verification when it returned normally, the result.
    void complete(std::uint64_t token, Recording& recording, std::optional<Error> error) {
        StoredSession stored;
        stored.token = token;
        stored.session = recording.recorder->session();
        stored.error = std::move(error);
        std::optional<std::pair<catalog::SystemName, launcher::LaunchChoice>> verified;
        if (!stored.error && catalog::returnedNormally(stored.session)) {
            catalog_db::LaunchVerification verification;
            verification.library = m_profile.id;
            verification.system = recording.system;
            verification.choice = stored.session.choice;
            verification.game = stored.session.game;
            verification.session = stored.session.id;
            verification.test = stored.session.test;
            verification.program = recording.program;
            verification.verifiedAt = stored.session.ended.value_or(std::chrono::system_clock::now());
            if (auto recorded = m_store->recordVerification(verification); recorded) {
                stored.verified = true;
                m_verified.insert(verificationKey(verification.system, verification.choice));
                verified.emplace(verification.system, verification.choice);
                if (const auto system = m_systems.find(verification.system.str());
                    system != m_systems.end() && system->second.choice == verification.choice &&
                    system->second.launch.readiness == catalog::LaunchReadiness::Ready) {
                    system->second.launch.verified = true;
                }
            } else {
                spdlog::warn("The launch verification of {} could not be recorded: {}",
                             verification.system.str(), recorded.error().toString());
            }
        }
        spdlog::info("Play session {} of {} recorded: {}{}", stored.session.id, stored.session.game.toString(),
                     catalog::toString(stored.session.outcome), stored.verified ? ", verified" : "");
        m_recordings.erase(token);
        m_shared->post([&](CatalogResults& results) {
            results.sessions.push_back(std::move(stored));
            if (verified) {
                results.verified.push_back(std::move(*verified));
            }
        });
    }

    // --- Stopping ------------------------------------------------------------------------------------------------

    [[nodiscard]] bool stopRequested() const {
        const std::lock_guard lock(m_shared->mutex);
        return m_shared->stopping;
    }

    /// At the stop: the queued session jobs are done, the rest dropped; the pending writes get the stop's patience.
    void flushAtStop() {
        if (!m_store) {
            return;
        }
        std::deque<Job> jobs;
        {
            const std::lock_guard lock(m_shared->mutex);
            jobs.swap(m_shared->jobs);
            m_shared->busy = true;
        }
        for (Job& job : jobs) {
            if (auto* session = std::get_if<SessionJob>(&job)) {
                handle(*session);
            }
        }
        retryPendingSessions(true);
    }

    void close() {
        {
            const std::lock_guard lock(m_shared->connectionMutex);
            m_shared->interruptible = nullptr;
            m_shared->importWriter = nullptr;
            m_query.reset();
        }
        m_recordings.clear();
        m_store.reset();
        m_database.reset();
    }

    std::shared_ptr<Shared> m_shared;
    CatalogWorker::Options m_options;
    CatalogState m_state = CatalogState::Opening;
    catalog::LibraryProfile m_profile;
    std::unique_ptr<catalog_db::Database> m_database;
    std::unique_ptr<catalog_db::SessionStore> m_store;
    std::unique_ptr<catalog_db::SqliteCatalogQuery> m_query;
    std::unique_ptr<CatalogLaunchLibrary> m_library;
    std::optional<Error> m_libraryError;
    /// The systems with games, by name.
    std::map<std::string, CatalogSystem, std::less<>> m_systems;
    std::map<std::string, launcher::LaunchChoice, std::less<>> m_assigned;
    std::set<std::tuple<std::string, int, std::string>> m_verified;
    std::map<std::uint64_t, Recording> m_recordings;
    Clock::time_point m_lastRetry{};
    /// The systems' readiness generation (GameReadiness::generation).
    std::uint64_t m_generation = 0;
};

} // namespace

CatalogWorker::CatalogWorker(Options options) : m_shared(std::make_shared<Shared>()) {
    m_shared->wake = options.wake;
    m_thread = std::thread([shared = m_shared, options = std::move(options)]() mutable {
        std::optional<Error> failure;
        try {
            Runner(shared, std::move(options)).run();
        } catch (const std::exception& error) {
            spdlog::error("The catalog worker failed: {}", error.what());
            failure = Error{ErrorCode::Unknown, fmt::format("the catalog worker failed: {}", error.what()), {}};
        } catch (...) {
            spdlog::error("The catalog worker failed with an unknown exception");
            failure = Error{ErrorCode::Unknown, "the catalog worker failed", {}};
        }
        // Nothing it was asked is left waiting: an opening never posted is Unavailable, and the main thread learns that
        // no other answer will come (CatalogResults::ended). The state is set before the wake, which must not throw.
        try {
            shared->post([&](CatalogResults& results) {
                if (!shared->openedPosted) {
                    CatalogOpened opened;
                    opened.state = CatalogState::Unavailable;
                    opened.error = failure.value_or(Error{ErrorCode::Cancelled, "the catalog worker stopped", {}});
                    opened.at = std::chrono::system_clock::now();
                    results.opened = std::move(opened);
                    shared->openedPosted = true;
                }
                results.ended = true;
                shared->finished = true;
                shared->busy = false;
            });
        } catch (...) {
            spdlog::error("The catalog worker's last wake of the host failed");
        }
        shared->ended.notify_all();
    });
}

CatalogWorker::~CatalogWorker() {
    stop();
}

void CatalogWorker::query(std::uint64_t serial, catalog::CatalogQuery query) {
    {
        const std::lock_guard lock(m_shared->mutex);
        // Only the latest list matters (typing coalesces): a query still queued is replaced.
        std::erase_if(m_shared->jobs, [](const Job& job) { return std::holds_alternative<QueryJob>(job); });
        m_shared->jobs.emplace_back(QueryJob{serial, std::move(query), Clock::now()});
    }
    m_shared->work.notify_one();
}

void CatalogWorker::readiness(std::vector<catalog::GameId> games) {
    if (games.empty()) {
        return;
    }
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->jobs.emplace_back(ReadinessJob{std::move(games)});
    }
    m_shared->work.notify_one();
}

void CatalogWorker::resolve(std::uint64_t token, catalog::GameId game) {
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->jobs.emplace_back(ResolveJob{token, game});
    }
    m_shared->work.notify_one();
}

void CatalogWorker::beginSession(std::uint64_t token, catalog::PlaySession session) {
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->jobs.emplace_back(SessionJob{token, SessionStep::Begin, std::move(session), {}, {}});
    }
    m_shared->work.notify_one();
}

void CatalogWorker::finishSession(std::uint64_t token, catalog::PlaySession session, catalog::SystemName system,
                                  std::filesystem::path program) {
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->jobs.emplace_back(
            SessionJob{token, SessionStep::Finish, std::move(session), std::move(system), std::move(program)});
    }
    m_shared->work.notify_one();
}

bool CatalogWorker::refresh() {
    {
        const std::lock_guard lock(m_shared->mutex);
        if (m_shared->stopping || m_shared->finished || m_shared->refreshing) {
            return false;
        }
        m_shared->refreshing = true;
        m_shared->jobs.emplace_back(RefreshJob{});
    }
    m_shared->work.notify_one();
    return true;
}

void CatalogWorker::roomSnapshot(RoomSnapshotRequest request) {
    {
        const std::lock_guard lock(m_shared->mutex);
        if (m_shared->stopping || m_shared->finished) {
            return;
        }
        std::erase_if(m_shared->jobs, [](const Job& job) { return std::holds_alternative<RoomJob>(job); });
        m_shared->jobs.emplace_back(RoomJob{std::move(request)});
    }
    m_shared->work.notify_one();
}

CatalogResults CatalogWorker::take() {
    const std::lock_guard lock(m_shared->mutex);
    return std::exchange(m_shared->results, CatalogResults{});
}

bool CatalogWorker::idle() const {
    const std::lock_guard lock(m_shared->mutex);
    return m_shared->finished || (!m_shared->busy && m_shared->jobs.empty());
}

bool CatalogWorker::stop(std::chrono::milliseconds bound) {
    if (!m_thread.joinable()) {
        return true;
    }
    const auto start = Clock::now();
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->stopping = true;
        // Lists, readiness and a refresh nobody will show: dropped; the session writes stay for the worker's last pass.
        std::erase_if(m_shared->jobs, [](const Job& job) { return !std::holds_alternative<SessionJob>(job); });
    }
    // A refresh running ends at its next look at the flag, its transaction rolled back.
    m_shared->cancel.store(true);
    m_shared->work.notify_one();
    {
        // A statement running on the worker's query connection stops with Cancelled (sqlite3_interrupt, ADR-0008 §5),
        // and so does the import's on the writer while it writes (its commit included: rolled back whole).
        const std::lock_guard lock(m_shared->connectionMutex);
        if (m_shared->interruptible != nullptr) {
            m_shared->interruptible->interrupt();
        }
        if (m_shared->importWriter != nullptr) {
            m_shared->importWriter->interrupt();
        }
    }
    bool ended = false;
    {
        std::unique_lock lock(m_shared->mutex);
        ended = m_shared->ended.wait_for(lock, bound, [this] { return m_shared->finished; });
        if (!ended) {
            m_shared->wake = nullptr; // the host may be gone by the time the worker posts again
        }
    }
    if (ended) {
        m_thread.join();
        spdlog::debug("Catalog worker stopped in {:.1f} ms", millisecondsSince(start));
        return true;
    }
    spdlog::warn("The catalog worker did not end within {} ms; it is left to end by itself", bound.count());
    m_thread.detach();
    return false;
}

} // namespace rv::app
