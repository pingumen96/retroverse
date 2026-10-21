#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// What the quick catalog of retroverse.exe shows and launches from (I2 step 13): the ES-DE catalog of the user data
// directory (EsdeAppCatalog: the catalog database through the catalog worker, ADR-0008 §5) or the mock catalog of the
// tests (MockAppCatalog, --mock-catalog, which takes precedence). ProductUi drives either through AppCatalog: the
// games (a ui::CatalogSource), Avvia's decision (a ready launcher::LaunchRequest for LaunchFlow, or the message of a
// game that cannot start), and the play sessions of the launches.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/catalog_database_source.hpp>
#include <rv/app/catalog_worker.hpp>
#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/app/mock_catalog.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/app/run_report.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/core/paths.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/platform_win/system_probe.hpp>
#include <rv/ui/catalog_source.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>

namespace rv::app {

/// Avvia decided: a ready request (LaunchFlow::request) or the message of a game that cannot start (LaunchFlow::refuse).
struct LaunchDecision {
    /// The launch's token (AppCatalog::requestLaunch).
    std::uint64_t token = 0;
    LaunchInfo info;
    std::optional<launcher::LaunchRequest> request;
    /// Without a request: why it cannot start.
    LaunchMessage refusal;
};

/// What AppCatalog::poll() found.
struct CatalogChanges {
    /// The catalog's opening ended: the model reads the platforms and the list again (QuickCatalogModel::refresh()).
    bool opened = false;
    /// Rows already read show other data now (QuickCatalogModel::reread()).
    bool rowsChanged = false;
    /// A refresh ended (AppCatalog::refreshState().last says how; I3 step 13r).
    bool refreshEnded = false;
    /// It ended Done and the systems were resolved again: the model reads the platforms and the list again
    /// (QuickCatalogModel::refresh(): the selection restored by GameId).
    bool refreshed = false;
};

/// Where the catalog's refresh stands, for the quick catalog (I3 step 13r).
struct RefreshState {
    /// The catalog can refresh (open, its worker running): the quick catalog offers "Aggiorna libreria".
    bool available = false;
    /// One runs, and where it is (nothing before its first progress).
    bool running = false;
    std::optional<RefreshProgress> progress;
    /// The last one that ended, and the lists asked while it ran (from the request to the answer on the worker).
    std::optional<RefreshSummary> last;
    std::optional<TimeSummary> lastLists;
};

/// The run report's record of a refresh that ended: `summary`'s outcome, error, counts, time and slices, `lists`.
void recordRefresh(RefreshRecord& record, const RefreshSummary& summary, const std::optional<TimeSummary>& lists);

/// The quick catalog's games and launches. Main thread only.
class AppCatalog {
public:
    virtual ~AppCatalog() = default;

    [[nodiscard]] virtual ui::CatalogSource& source() noexcept = 0;
    /// What the list says instead of games (ui::quick_catalog::kNotice*); empty when nothing.
    [[nodiscard]] virtual std::string_view notice() const noexcept = 0;

    /// Avvia on `entry`, the run's launch `token`: its decision comes from takeDecision(), at once or once the catalog
    /// worker resolved it right before the launch. False, and nothing asked, while a decision is pending (deciding()).
    virtual bool requestLaunch(std::uint64_t token, const ui::CatalogEntry& entry) = 0;
    [[nodiscard]] virtual std::optional<LaunchDecision> takeDecision() = 0;
    /// A decision is pending: being made, or made and not taken yet (takeDecision()).
    [[nodiscard]] virtual bool deciding() const noexcept = 0;

    /// Once per frame and per wake of the host's idle wait: collects what the worker answered.
    virtual CatalogChanges poll() = 0;
    /// Something the screen waits for is with the worker (lockstep runs, --fixed-dt, wait for it between frames).
    [[nodiscard]] virtual bool busy() const noexcept = 0;

    /// The catalog's refresh from the library (I3 step 13r): whether this catalog has one at all (the ES-DE catalog; the
    /// mock catalog has nothing to refresh), starting one (false, and nothing started, while one runs or the catalog is
    /// not open), where it stands; how long the end of the run waited for the worker (shutdown()).
    [[nodiscard]] virtual bool supportsRefresh() const noexcept { return false; }
    virtual bool requestRefresh() { return false; }
    [[nodiscard]] virtual RefreshState refreshState() const { return {}; }
    [[nodiscard]] virtual std::optional<double> stopMilliseconds() const { return std::nullopt; }

    /// The play session of the launch `token`: it began (LaunchFlowEvents::started), it ended (::ended), or it was
    /// refused before anything started (the decision's refusal).
    virtual void sessionStarted(std::uint64_t /*token*/, const launcher::LaunchSession& /*session*/) {}
    virtual void sessionEnded(std::uint64_t /*token*/, const launcher::LaunchSession& /*session*/,
                              LaunchEndCause /*cause*/, bool /*stoppedAwaiting*/) {}
    virtual void sessionRefused(std::uint64_t /*token*/) {}

    /// The generated building's part of the catalog (I3 step 10b, I4 step 5a): where the catalog stands (Opening until
    /// its worker answered; a catalog without rooms, the mock, says Unavailable), its library, the read-only connection
    /// the building job reads every system's snapshot on (room_snapshot.hpp; none until the catalog opened Ready), and
    /// whether its worker ended.
    [[nodiscard]] virtual CatalogState roomCatalogState() const noexcept { return CatalogState::Unavailable; }
    [[nodiscard]] virtual catalog::LibraryId roomLibrary() const { return {}; }
    [[nodiscard]] virtual std::shared_ptr<WorldCatalogReader> worldReader() const { return nullptr; }
    [[nodiscard]] virtual bool workerEnded() const noexcept { return true; }

    /// The catalog's row of `game` as the quick catalog shows it (its readiness and first blocker included), read at
    /// once on the main thread (a page of one; the readiness the game's system has until the worker resolved the
    /// game's own: poll()'s rowsChanged tells when to read it again), for the world's card (I3 step 14a). Nothing when
    /// the catalog lacks the game, cannot be read or has no rows (the mock catalog).
    [[nodiscard]] virtual std::optional<ui::CatalogEntry> gameEntry(const catalog::GameId& /*game*/) {
        return std::nullopt;
    }
    /// Everything the catalog holds about `game` (catalog_db::SqliteCatalogQuery::details: its key, metadata and stored
    /// media), read at once on the main thread for the media viewer and Informazioni (I3 step 14b). Nothing when the
    /// catalog lacks the game, cannot be read or has no database (the mock catalog).
    [[nodiscard]] virtual std::optional<catalog_db::GameDetails> gameDetails(const catalog::GameId& /*game*/) {
        return std::nullopt;
    }

    /// A frame showed a page of the catalog (the quick catalog open with rows bound).
    virtual void pageShown() {}
    /// The end of the run: what is pending is finished within a bound (the worker ends: CatalogWorker::stop).
    virtual void shutdown() {}
    /// The run report: the catalog's own fields (source, file, games, notice, times, ...), and a launch's session.
    virtual void record(CatalogRecord& record) const = 0;
    virtual void recordLaunch(std::uint64_t /*token*/, LaunchRecord& /*record*/) const {}
};

/// The mock catalog's games (--mock-catalog, I2 step A1): an in-memory source, each game planned with its emulator
/// profile (launcher::planLaunch: the shipped and the user's profiles, the catalog's over them) at once.
class MockAppCatalog final : public AppCatalog {
public:
    MockAppCatalog(MockCatalog catalog, launcher::EmulatorProfiles baseProfiles);

    [[nodiscard]] ui::CatalogSource& source() noexcept override { return m_source; }
    [[nodiscard]] std::string_view notice() const noexcept override { return {}; }
    bool requestLaunch(std::uint64_t token, const ui::CatalogEntry& entry) override;
    [[nodiscard]] std::optional<LaunchDecision> takeDecision() override { return std::exchange(m_decision, {}); }
    [[nodiscard]] bool deciding() const noexcept override { return m_decision.has_value(); }
    CatalogChanges poll() override { return {}; }
    [[nodiscard]] bool busy() const noexcept override { return false; }
    void record(CatalogRecord& record) const override;

    [[nodiscard]] const launcher::EmulatorProfiles& profiles() const noexcept { return m_profiles; }

private:
    MockCatalog m_catalog;
    launcher::EmulatorProfiles m_profiles;
    platform_win::Win32SystemProbe m_probe;
    ui::InMemoryCatalogSource m_source;
    std::optional<LaunchDecision> m_decision;
};

/// The ES-DE catalog of the user data directory: the catalog worker (CatalogWorker, started at construction) and the
/// source over it (CatalogDatabaseSource). Avvia resolves the game again on the worker right before the launch (its
/// file on disk now, the database's choices: ADR-0008 §8, docs/i2/step-12.md); the play sessions are recorded on the
/// worker (catalog_db::SessionRecorder, the owner this process). A refresh (requestRefresh) imports the library on the
/// worker, in slices, the list answered meanwhile; its end brings the systems resolved again (poll(): refreshed).
class EsdeAppCatalog final : public AppCatalog {
public:
    struct Options {
        core::Paths paths;
        /// The host's wake (AppContext::wakeCallback()), called from the worker.
        std::function<void()> wake;
        /// The play sessions' owner (platform_win::currentProcessIdentity()).
        catalog::SessionOwner owner;
        /// When the process was created (FILETIME ticks): the run report's times (NFR-01) count from it.
        std::uint64_t processCreated = 0;
        /// Tests: the worker's own options over the defaults (its query, its owner check).
        std::function<void(CatalogWorker::Options&)> adjust;
    };

    explicit EsdeAppCatalog(Options options);
    ~EsdeAppCatalog() override;

    [[nodiscard]] ui::CatalogSource& source() noexcept override { return m_source; }
    /// "opening" also once the catalog opened, until its first list is shown (never "no games" before it).
    [[nodiscard]] std::string_view notice() const noexcept override;
    bool requestLaunch(std::uint64_t token, const ui::CatalogEntry& entry) override;
    [[nodiscard]] std::optional<LaunchDecision> takeDecision() override { return std::exchange(m_decision, {}); }
    [[nodiscard]] bool deciding() const noexcept override { return m_resolving.has_value() || m_decision.has_value(); }
    CatalogChanges poll() override;
    [[nodiscard]] bool busy() const noexcept override { return m_source.waiting() || m_resolving.has_value(); }
    [[nodiscard]] bool supportsRefresh() const noexcept override { return true; }
    bool requestRefresh() override;
    [[nodiscard]] RefreshState refreshState() const override;
    [[nodiscard]] std::optional<double> stopMilliseconds() const override { return m_stopMs; }
    void sessionStarted(std::uint64_t token, const launcher::LaunchSession& session) override;
    void sessionEnded(std::uint64_t token, const launcher::LaunchSession& session, LaunchEndCause cause,
                      bool stoppedAwaiting) override;
    void sessionRefused(std::uint64_t token) override;
    void shutdown() override;
    void record(CatalogRecord& record) const override;
    void recordLaunch(std::uint64_t token, LaunchRecord& record) const override;

    [[nodiscard]] CatalogState roomCatalogState() const noexcept override { return m_source.state(); }
    [[nodiscard]] catalog::LibraryId roomLibrary() const override { return m_source.library(); }
    [[nodiscard]] std::shared_ptr<WorldCatalogReader> worldReader() const override { return m_worldReader; }
    [[nodiscard]] bool workerEnded() const noexcept override { return m_stopped || m_source.ended(); }

    [[nodiscard]] std::optional<ui::CatalogEntry> gameEntry(const catalog::GameId& game) override {
        return m_source.entryOf(game);
    }
    [[nodiscard]] std::optional<catalog_db::GameDetails> gameDetails(const catalog::GameId& game) override {
        return m_source.detailsOf(game);
    }
    /// The first page shown is NFR-01's time (CatalogRecord::firstPageMs).
    void pageShown() override;
    [[nodiscard]] const CatalogDatabaseSource& databaseSource() const noexcept { return m_source; }

private:
    /// A launch's session as it is being recorded.
    struct Launch {
        /// The title the quick catalog showed for it.
        std::string title;
        catalog::PlaySession session;
        catalog::SystemName system;
        catalog::LaunchResolution resolution;
        bool guardAllowed = true;
        /// The monotonic time its session began (SessionTimes::begun).
        std::optional<launcher::LaunchClock::time_point> begun;
        /// The session as stored, once the worker wrote it (or gave up).
        std::optional<StoredSession> stored;
    };

    /// Milliseconds from the process's creation to `at`.
    [[nodiscard]] double sinceProcessStart(std::chrono::system_clock::time_point at) const;
    void collectSessions(CatalogResults& results);
    /// The refresh's progress and end out of `results` (`ended`: the worker's thread ended, so a refresh it did not
    /// answer never will be); true when it ended.
    bool collectRefresh(CatalogResults& results, bool ended);

    Options m_options;
    CatalogWorker m_worker;
    CatalogDatabaseSource m_source;
    std::optional<std::uint64_t> m_resolving;
    std::optional<LaunchDecision> m_decision;
    std::map<std::uint64_t, Launch> m_launches;
    std::optional<double> m_openedMs;
    std::optional<double> m_firstListMs;
    std::optional<double> m_firstPageMs;
    bool m_stopped = false;
    /// The refresh: running, its latest progress, the lists' waits while it runs, the last one's end.
    bool m_refreshRunning = false;
    std::optional<RefreshProgress> m_refreshProgress;
    std::vector<double> m_refreshLists;
    std::optional<RefreshSummary> m_lastRefresh;
    std::optional<TimeSummary> m_lastRefreshLists;
    /// How long shutdown() waited for the worker to stop.
    std::optional<double> m_stopMs;
    /// The building job's reader (I4 step 5a), from the catalog's opening.
    std::shared_ptr<WorldCatalogReader> m_worldReader;
    /// Every list's wait (CatalogRecord::lists), at most kMaxListWaits.
    static constexpr std::size_t kMaxListWaits = 100000;
    std::vector<double> m_listWaits;
};

/// The Running session of a launch that began (`session` as the resolution gave it: the game, the choice, the owner):
/// its wall-clock start (taken back from `launch`'s SessionTimes::begun to `now`), the started process.
[[nodiscard]] catalog::PlaySession startedPlaySession(catalog::PlaySession session,
                                                      const launcher::LaunchSession& launch,
                                                      std::chrono::system_clock::time_point wallNow,
                                                      launcher::LaunchClock::time_point now);

/// The final session of a launch that ended (as retroverse-cli launch --game records it, docs/i2/step-12.md): its
/// outcome (catalog::recordOutcome, with the owner's reason: none for the session's own end and the user's return,
/// Quit, WaitLimit), its end time (the authoritative exit when the game ended by itself, else the moment the session
/// ended), how long the game ran, a session stopped while it awaited the user's confirmation left running, a refusal by
/// the automation guard.
[[nodiscard]] catalog::PlaySession finishedPlaySession(catalog::PlaySession running,
                                                       const launcher::LaunchSession& launch, LaunchEndCause cause,
                                                       bool stoppedAwaiting, bool guardAllowed,
                                                       launcher::LaunchClock::time_point now);

/// The Blocked session of a game that was not ready right before the launch: its readiness, the blockers' codes and
/// messages ("<code>: <message>; ..."), started and ended at `wallNow`.
[[nodiscard]] catalog::PlaySession blockedPlaySession(catalog::PlaySession session,
                                                      const catalog::LaunchResolution& resolution,
                                                      std::chrono::system_clock::time_point wallNow);

/// The message of a game the worker resolved and that cannot start: launch_outcome::kNotReady with its readiness as
/// the reason and its first blocker's words as the detail; for a game that cannot be resolved at all (the catalog
/// cannot be read, no longer has it), kNotReady with the reason "unresolved" and the error's message.
[[nodiscard]] LaunchMessage refusalOf(const ResolvedLaunch& resolved);

/// FILETIME ticks (100 ns since 1601-01-01 UTC) of a wall-clock time, as process creation times are given.
[[nodiscard]] std::uint64_t fileTimeTicksOf(std::chrono::system_clock::time_point time) noexcept;

} // namespace rv::app
