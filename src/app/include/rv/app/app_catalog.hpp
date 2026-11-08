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
    /// Another process imported the library (I7 step 7): the systems were resolved again, the model reads the platforms
    /// and the list again, and the building follows.
    bool otherImport = false;
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
    /// Imports of another process followed (CatalogChanges::otherImport), in all.
    std::uint64_t otherImports = 0;
    /// Review rows paired (I7 step 8): an identity followed its file, the lists and the building follow as after another
    /// process's import.
    std::uint64_t reviewsPaired = 0;
};

/// Where "Calcola impronte" stands (I7 step 8): one runs (and is being stopped), its latest progress, the last one's end.
struct QuickHashState {
    bool running = false;
    bool cancelling = false;
    std::optional<catalog::QuickHashProgress> progress;
    std::optional<QuickHashEnded> last;
};

/// The run report's record of a refresh that ended: `summary`'s outcome, error, counts, time and slices, `lists`.
void recordRefresh(RefreshRecord& record, const RefreshSummary& summary, const std::optional<TimeSummary>& lists);

/// The quick catalog's games and launches. Main thread only.
/// A system and its logo file (I11 step 3: catalog::IdentityLogo), which the product UI shows (SystemLogos).
struct SystemLogoFile {
    std::string system;
    std::filesystem::path file;

    bool operator==(const SystemLogoFile&) const = default;
};

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
    /// Of the systems `request` names, or of every one (I7 step 7).
    virtual bool requestRefresh(RefreshRequest /*request*/ = {}) { return false; }
    /// What the file watcher watches for this catalog's library, once it opened and whenever it changed (taken once);
    /// nothing for a catalog without one (the mock, the wizard's before its import). I7 step 7.
    [[nodiscard]] virtual std::optional<esde::ChangeScopeMap> takeWatchMap() { return std::nullopt; }
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

    /// The directory's favourites and recently played games (I4 step 7b, ADR-0013 §10): asked of the catalog worker (at
    /// most `limit`; a request still queued is replaced), answered once by takeRecentGames(): the list, or the error of
    /// a list that could not be read (also when the worker ended before it answered). False, and nothing asked, for a
    /// catalog without such a list (the mock catalog).
    virtual bool requestRecentGames(std::size_t /*limit*/) { return false; }
    [[nodiscard]] virtual std::optional<Expected<std::vector<catalog::RecentGame>>> takeRecentGames() {
        return std::nullopt;
    }
    /// The emulators screen (I7 step 4, ADR-0018 §5): asked of the catalog worker, answered once by the take*() below; a
    /// false says nothing was asked: a catalog without them (the mock catalog, the wizard's before its import).
    virtual bool requestEmulatorTable() { return false; }
    [[nodiscard]] virtual std::optional<EmulatorTable> takeEmulatorTable() { return std::nullopt; }
    virtual bool requestSampleGames(const catalog::SystemName& /*system*/, std::size_t /*limit*/) { return false; }
    [[nodiscard]] virtual std::optional<EmulatorSamples> takeSampleGames() { return std::nullopt; }
    virtual bool requestChoiceChange(ChoiceChange /*change*/) { return false; }
    [[nodiscard]] virtual std::optional<ChoiceChanged> takeChoiceChanged() { return std::nullopt; }
    /// requestLaunch() of a launch test with `test`: the decision comes the same way; takeLaunchTest() says how it ended,
    /// once its session is stored.
    virtual bool requestLaunchTest(std::uint64_t /*token*/, const ui::CatalogEntry& /*entry*/,
                                   const LaunchTestChoice& /*test*/) {
        return false;
    }
    [[nodiscard]] virtual std::optional<LaunchTestResult> takeLaunchTest() { return std::nullopt; }
    /// The launch `token` was decided and never reached the flow: no session will say how a test of it ended.
    virtual void launchDropped(std::uint64_t /*token*/) {}
    /// The statistics screen and Informazioni's rows (I7 step 6, ADR-0018 §9, §11), likewise. A catalog without them answers
    /// false: the screen says no catalog is open, the rows do not show.
    virtual bool requestStatistics(std::size_t /*limit*/) { return false; }
    [[nodiscard]] virtual std::optional<StatisticsList> takeStatistics() { return std::nullopt; }
    virtual bool requestGameControls(const catalog::GameId& /*game*/) { return false; }
    virtual bool requestGameStatus(const catalog::GameId& /*game*/, catalog::GameStatus /*status*/) { return false; }
    [[nodiscard]] virtual std::optional<GameControls> takeGameControls() { return std::nullopt; }
    /// Where the folder browser starts looking for a program: ES-DE's install folder, when known; nothing: the drives.
    [[nodiscard]] virtual std::optional<std::filesystem::path> programStartFolder() const { return std::nullopt; }
    /// The coverage and library screens (I7 step 8, ADR-0018 §14), likewise: a false says nothing was asked (the mock
    /// catalog, the wizard's before its import). A review row resolved to one of its candidates (`pairWith`) or kept
    /// separate (nothing). The quick-hash pass on request: false while one runs or nothing can; its state.
    virtual bool requestCoverage() { return false; }
    [[nodiscard]] virtual std::optional<CoverageList> takeCoverage() { return std::nullopt; }
    virtual bool requestLibraryStatus() { return false; }
    [[nodiscard]] virtual std::optional<LibraryStatus> takeLibraryStatus() { return std::nullopt; }
    virtual bool requestReviewResolution(std::int64_t /*review*/, std::optional<catalog::GameId> /*pairWith*/) {
        return false;
    }
    [[nodiscard]] virtual std::optional<ReviewResolved> takeReviewResolved() { return std::nullopt; }
    virtual bool requestQuickHash() { return false; }
    virtual void cancelQuickHash() {}
    [[nodiscard]] virtual QuickHashState quickHashState() const { return {}; }
    /// A frame begins (ProductUi::update): a stepped quick-hash pass (lockstep runs) may read its next file.
    virtual void frameStarted() {}

    /// The full name of `system` as the catalog has it (its <fullname>); "" when it has none or the catalog does not know
    /// the system (the directory then shows its name).
    [[nodiscard]] virtual std::string systemFullName(const catalog::SystemName& /*system*/) const { return {}; }
    /// I11 step 3: the systems that have a logo file, in the chips' order.
    [[nodiscard]] virtual std::vector<SystemLogoFile> systemLogos() const { return {}; }

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

/// What the product UI holds while the wizard has not reached its page 4 (ADR-0018 §4): no catalog worker exists, no
/// game is listed, and the room's side says NoLibrary.
class PendingAppCatalog final : public AppCatalog {
public:
    PendingAppCatalog();

    [[nodiscard]] ui::CatalogSource& source() noexcept override { return m_source; }
    [[nodiscard]] std::string_view notice() const noexcept override;
    bool requestLaunch(std::uint64_t /*token*/, const ui::CatalogEntry& /*entry*/) override { return false; }
    [[nodiscard]] std::optional<LaunchDecision> takeDecision() override { return std::nullopt; }
    [[nodiscard]] bool deciding() const noexcept override { return false; }
    CatalogChanges poll() override { return {}; }
    [[nodiscard]] bool busy() const noexcept override { return false; }
    [[nodiscard]] CatalogState roomCatalogState() const noexcept override { return CatalogState::NoLibrary; }
    void record(CatalogRecord& record) const override;

private:
    ui::InMemoryCatalogSource m_source;
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
        /// CatalogWorker::Options::createDatabase: the wizard's page 4.
        bool createDatabase = false;
        /// CatalogWorker::Options::otherImportsInterval: another process's import followed (with the file watcher).
        std::optional<std::chrono::milliseconds> otherImportsInterval;
        /// CatalogWorker::Options::hashStepped (lockstep runs): the quick-hash pass reads one file per frame, and the
        /// frame waits for it (busy()).
        bool hashStepped = false;
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
    [[nodiscard]] bool busy() const noexcept override {
        return m_source.waiting() || m_resolving.has_value() || m_recentAsked.has_value() || m_tableAsked ||
               m_samplesAsked || m_changesAsked > 0 || m_testAwaited || m_statisticsAsked || m_controlsAsked > 0 ||
               m_coverageAsked || m_libraryAsked || m_reviewsAsked > 0 || m_hashAwaited;
    }
    [[nodiscard]] bool supportsRefresh() const noexcept override { return true; }
    bool requestRefresh(RefreshRequest request = {}) override;
    [[nodiscard]] std::optional<esde::ChangeScopeMap> takeWatchMap() override { return std::exchange(m_watchMap, {}); }
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
    bool requestRecentGames(std::size_t limit) override;
    [[nodiscard]] std::optional<Expected<std::vector<catalog::RecentGame>>> takeRecentGames() override {
        return std::exchange(m_recentAnswer, std::nullopt);
    }
    bool requestEmulatorTable() override;
    [[nodiscard]] std::optional<EmulatorTable> takeEmulatorTable() override { return std::exchange(m_table, {}); }
    bool requestSampleGames(const catalog::SystemName& system, std::size_t limit) override;
    [[nodiscard]] std::optional<EmulatorSamples> takeSampleGames() override { return std::exchange(m_samples, {}); }
    bool requestChoiceChange(ChoiceChange change) override;
    [[nodiscard]] std::optional<ChoiceChanged> takeChoiceChanged() override;
    bool requestLaunchTest(std::uint64_t token, const ui::CatalogEntry& entry, const LaunchTestChoice& test) override;
    [[nodiscard]] std::optional<LaunchTestResult> takeLaunchTest() override { return std::exchange(m_testResult, {}); }
    void launchDropped(std::uint64_t token) override;
    bool requestStatistics(std::size_t limit) override;
    [[nodiscard]] std::optional<StatisticsList> takeStatistics() override { return std::exchange(m_statistics, {}); }
    bool requestGameControls(const catalog::GameId& game) override;
    bool requestGameStatus(const catalog::GameId& game, catalog::GameStatus status) override;
    [[nodiscard]] std::optional<GameControls> takeGameControls() override { return std::exchange(m_gameControls, {}); }
    [[nodiscard]] std::optional<std::filesystem::path> programStartFolder() const override;
    bool requestCoverage() override;
    [[nodiscard]] std::optional<CoverageList> takeCoverage() override { return std::exchange(m_coverage, {}); }
    bool requestLibraryStatus() override;
    [[nodiscard]] std::optional<LibraryStatus> takeLibraryStatus() override { return std::exchange(m_library, {}); }
    bool requestReviewResolution(std::int64_t review, std::optional<catalog::GameId> pairWith) override;
    [[nodiscard]] std::optional<ReviewResolved> takeReviewResolved() override;
    bool requestQuickHash() override;
    void cancelQuickHash() override;
    [[nodiscard]] QuickHashState quickHashState() const override;
    void frameStarted() override;
    [[nodiscard]] std::string systemFullName(const catalog::SystemName& system) const override;
    [[nodiscard]] std::vector<SystemLogoFile> systemLogos() const override;
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
        /// A launch test.
        bool test = false;
    };

    /// Milliseconds from the process's creation to `at`.
    [[nodiscard]] double sinceProcessStart(std::chrono::system_clock::time_point at) const;
    void collectSessions(CatalogResults& results);
    void collectEmulators(CatalogResults& results);
    /// The refresh's progress and end out of `results` (`ended`: the worker's thread ended, so a refresh it did not
    /// answer never will be); true when it ended.
    bool collectRefresh(CatalogResults& results, bool ended);
    /// I7 step 8: the coverage, the library's state, review resolutions and the quick-hash pass out of `results`.
    void collectLibrary(CatalogResults& results);

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
    /// I7 step 7: the watcher's map until taken, and another process's imports followed.
    std::optional<esde::ChangeScopeMap> m_watchMap;
    std::uint64_t m_otherImports = 0;
    std::uint64_t m_reviewsPaired = 0;
    /// How long shutdown() waited for the worker to stop.
    std::optional<double> m_stopMs;
    /// The building job's reader (I4 step 5a), from the catalog's opening.
    std::shared_ptr<WorldCatalogReader> m_worldReader;
    /// The favourites and recent games asked of the worker (its serial) and its answer until taken (I4 step 7b).
    std::uint64_t m_recentSerial = 0;
    std::optional<std::uint64_t> m_recentAsked;
    std::optional<Expected<std::vector<catalog::RecentGame>>> m_recentAnswer;
    /// The emulators screen's questions out (the lockstep runs wait for them) and their answers until taken (I7 step 4).
    std::uint64_t m_emulatorSerial = 0;
    bool m_tableAsked = false;
    bool m_samplesAsked = false;
    std::size_t m_changesAsked = 0;
    /// A launch test's session ended, and how it did is not known yet.
    bool m_testAwaited = false;
    /// I7 step 6: the statistics and Informazioni's rows asked (the lockstep runs wait for them), their answers until taken.
    bool m_statisticsAsked = false;
    std::uint64_t m_statisticsSerial = 0;
    std::size_t m_controlsAsked = 0;
    std::uint64_t m_controlsSerial = 0;
    std::optional<StatisticsList> m_statistics;
    std::optional<GameControls> m_gameControls;
    /// I7 step 8: the coverage, the library's state and review resolutions asked (the lockstep runs wait for them), their
    /// answers until taken; the quick-hash pass: running, being stopped, its latest progress and end; in a stepped run, a
    /// file read since the frame let it (held) and a frame waiting for the next one (awaited).
    bool m_coverageAsked = false;
    std::uint64_t m_coverageSerial = 0;
    std::optional<CoverageList> m_coverage;
    bool m_libraryAsked = false;
    std::uint64_t m_librarySerial = 0;
    std::optional<LibraryStatus> m_library;
    std::size_t m_reviewsAsked = 0;
    std::uint64_t m_reviewSerial = 0;
    std::vector<ReviewResolved> m_reviewsResolved;
    bool m_hashRunning = false;
    bool m_hashCancelling = false;
    std::optional<catalog::QuickHashProgress> m_hashProgress;
    std::optional<QuickHashEnded> m_hashLast;
    bool m_hashHeld = false;
    bool m_hashAwaited = false;
    std::optional<EmulatorTable> m_table;
    std::optional<EmulatorSamples> m_samples;
    std::vector<ChoiceChanged> m_changes;
    std::optional<LaunchTestResult> m_testResult;
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
