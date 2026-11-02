#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The catalog worker of retroverse.exe (I2 step 13; ADR-0008 §5): one thread that owns the catalog database's writer
// and a catalog::ICatalogQuery on its own read-only connection, so that the main thread never waits on the database.
// It opens the catalog (library.json, the data-directory guard, the database, the play sessions left open by owners
// that are gone, the library's configuration and each system's launch readiness), then answers the main thread's
// requests in order: materialized lists (SqliteCatalogQuery::run), the launch readiness of games where it can differ
// from their system's, the resolution of a launch right before it starts, and the play sessions of the launches
// (catalog_db::SessionStore through SessionRecorder: a write that meets a busy database stays pending on the worker and
// is tried again, never on the main thread). The main thread reads pages through its own SqliteCatalogQuery on another
// read-only connection, which the worker opens for it (CatalogOpened::pages). No GL call happens on the worker, and
// one SDL call only: it wakes the host after it posted results through Options::wake, which in retroverse.exe is
// AppContext::wakeCallback(), SDL_PushEvent (documented as thread-safe; A1's process runner wakes the host the same
// way). Nothing else may call SDL from here. Every request is answered, also when the worker's thread fails (the
// results then say it ended: CatalogResults::ended). Stopping it interrupts a running query (sqlite3_interrupt through
// Connection::interrupt) and ends within a bound.
//
// Since I3 step 13r it also refreshes the catalog (refresh(): "Aggiorna libreria", --refresh-library): the import of
// retroverse-cli import (ImportService with IdentityPairing, its import lock) as a job on the same writer, in slices
// (library_refresh.hpp), so that lists, readiness, launches and session writes queued meanwhile are answered between
// two of its pieces of work (launches and session writes not while its write transaction is open). After its commit
// the systems' readiness is resolved again (the library's configuration and emulators read again, the choices and the
// systems' games), and the main thread learns it (CatalogResults::refreshed) with the readiness generation that drops
// the answers resolved before. stop() sets the import's cancel flag and interrupts the writer while the import writes:
// the import is rolled back or committed whole, within the stop's bound.
//
// Since I7 step 4 (ADR-0018 §5) it answers the emulators screen: the table of the systems with games (emulatorTable()), the
// games a launch test offers (sampleGames()), a launch test (resolve() with a LaunchTestChoice: that choice launches, the
// session records `test`, and only a return that was normal stores it: CatalogResults::tests) and the changes of what is
// stored (changeChoice()). What is stored changes the systems' readiness: they are resolved again without reading ES-DE's
// configuration (CatalogResults::resolved, a new readiness generation).
//
// Since I7 step 6 (ADR-0018 §9, §11) it answers the statistics screen (statistics(): SqliteCatalogQuery::playStatistics on
// the worker's reader) and Informazioni's rows (gameControls(), setGameStatus()), so that neither ever blocks a frame.
//
// Since I4 step 5a it opens, with the catalog, a third read-only connection for the building job (CatalogOpened::world,
// room_snapshot.hpp's WorldCatalogReader), which reads every system's snapshot on the app's worker pool: the room's
// snapshot is no job of this worker any more (I3 step 10b's), so that the lists never queue behind it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rv/app/emulators_data.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/catalog/statistics.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/change_scope.hpp>
#include <rv/core/paths.hpp>
#include <rv/launcher/emulator_profile.hpp>

namespace rv::app {

/// Where the app's ES-DE catalog stands.
enum class CatalogState : std::uint8_t {
    Opening,         ///< the worker is opening it
    Ready,           ///< open: lists, pages, launches
    NoLibrary,       ///< no <user data>/config/library.json (retroverse-cli library create)
    NoDatabase,      ///< no <user data>/database/retroverse.sqlite (retroverse-cli import)
    DataDirectory,   ///< the user data directory lies inside the library's folders (core::checkUserDataDirectory)
    LibraryMismatch, ///< library.json names a library the catalog does not hold (ADR-0008 §4: a re-association)
    Unavailable,     ///< it cannot be opened: library.json or the database cannot be read, a schema of another version
};

/// "opening", "ready", "no-library", "no-database", "data-directory", "library-mismatch", "unavailable".
[[nodiscard]] std::string_view toString(CatalogState state) noexcept;
/// The quick catalog's notice for `state` (ui::quick_catalog::kNotice*); empty for Ready.
[[nodiscard]] std::string_view noticeOf(CatalogState state) noexcept;

/// A game's launch readiness as the card shows it (ADR-0008 §8).
struct GameLaunchState {
    catalog::LaunchReadiness readiness = catalog::LaunchReadiness::Ready;
    /// The first blocker's code and its own sentence; empty when ready.
    std::string blocker;
    std::string blockerMessage;
    /// Its command or profile returned normally from a launch of its system (catalog_db::LaunchVerification).
    bool verified = false;

    bool operator==(const GameLaunchState&) const = default;
};

/// How often the worker compares the newest import with the one it knows, with the file watcher (ADR-0018 §10).
inline constexpr std::chrono::milliseconds kOtherImportsInterval{5000};

/// The blocker code of a launch that cannot be resolved because the library's configuration cannot be read (ES-DE's
/// settings or systems: CatalogOpened::libraryError).
inline constexpr std::string_view kLibraryUnreadableCode = "library-unreadable";

/// A system of the catalog with games, as the quick catalog shows it: a filter chip, and what its games share.
struct CatalogSystem {
    catalog::SystemName name;
    /// Its <fullname>, the chip's label (its name when the catalog has none).
    std::string fullName;
    /// Its ES-DE platform tags (catalog::SystemInfo::platforms): world::classify()'s fallback for a system systems.json
    /// does not name (a custom system's room).
    std::vector<std::string> platforms;
    /// Available Game entries: what the chip's filter lists without a search (SqliteCatalogQuery::systems).
    std::size_t games = 0;
    /// The readiness its games share (catalog::resolveLaunch without a game), resolved when the catalog opened, and
    /// the choice it rests on.
    GameLaunchState launch;
    launcher::LaunchChoice choice;
    /// I7 step 4: where the choice comes from (catalog::toString(ChoiceSource)) and its name as people read it (a command's
    /// label, a profile's display name): the emulators screen's list, which resolves nothing again.
    std::string source;
    std::string choiceName;
    /// The program the choice finds now, when it does: a verification counts for that one alone.
    std::optional<std::filesystem::path> program;
    /// Its resolution leaves something to each game (an %INJECT% file, a %STARTDIR%=%GAMEDIR% folder): each game is
    /// resolved for its own readiness.
    bool perGame = false;

    bool operator==(const CatalogSystem&) const = default;
};

/// A materialized list (CatalogWorker::query).
struct CatalogList {
    std::uint64_t serial = 0;
    /// The query failed (the list is then empty): the catalog cannot be read, interrupted (Cancelled), malformed text.
    std::optional<Error> error;
    std::vector<catalog::GameId> ids;
    /// The list's (GameId value, position) pairs sorted by the value: positions found without a scan.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> byId;
    /// How long the materialization took on the worker, and when it was done.
    double milliseconds = 0.0;
    std::chrono::system_clock::time_point at;
    /// From the query's request (CatalogWorker::query) to its answer: the time in the queue (behind other jobs, or a
    /// slice of a refresh) and the materialization.
    double waitedMs = 0.0;
};

/// The catalog opened, or why it is not (CatalogResults::opened).
struct CatalogOpened {
    CatalogState state = CatalogState::Opening;
    /// Why it is not Ready.
    std::optional<Error> error;
    catalog::LibraryId library;
    std::filesystem::path database;
    /// The systems with games, in the chips' order (SortOrder::System's).
    std::vector<CatalogSystem> systems;
    /// The main thread's queries, on a read-only connection of their own: fetch() of pages only (ADR-0008 §5).
    std::unique_ptr<catalog_db::SqliteCatalogQuery> pages;
    /// The building job's reader (I4 step 5a): a read-only connection of its own, lent to one job at a time.
    std::shared_ptr<WorldCatalogReader> world;
    /// The default list (every game, Title order: an empty catalog::CatalogQuery), materialized right after the
    /// opening, so that the quick catalog's first list needs no round trip (NFR-01); none when it could not be read.
    /// Its serial is 0.
    std::optional<CatalogList> firstList;
    /// The play sessions closed as interrupted (their owner was gone, ADR-0008 §9).
    std::vector<catalog::PlaySession> interrupted;
    /// ES-DE's configuration could not be read: the catalog can be browsed, nothing launches (kLibraryUnreadableCode).
    std::optional<Error> libraryError;
    /// How long the worker took to open it, and when it was done.
    double milliseconds = 0.0;
    std::chrono::system_clock::time_point at;
};

/// A launch resolved right before it starts (CatalogWorker::resolve).
struct ResolvedLaunch {
    std::uint64_t token = 0;
    /// The game asked for and the identity it stands for (an alias resolved).
    catalog::GameId requested;
    /// The game cannot be resolved: the catalog cannot be read, no longer has it, or it is no game of the library.
    std::optional<Error> error;
    std::string title;
    catalog::SystemName system;
    catalog::LaunchResolution resolution;
    /// The automation guard (RV_LAUNCH_ALLOWLIST) allows the request (true without one, or without a request).
    bool guardAllowed = true;
    /// What its play session records: the game (the identity), the choice, its source, the owner (Options::owner).
    catalog::PlaySession session;
};

/// A play session written to the end (CatalogWorker::finishSession), or given up on at the worker's stop.
struct StoredSession {
    std::uint64_t token = 0;
    catalog::PlaySession session;
    /// Why it could not be written.
    std::optional<Error> error;
    /// A launch verification was recorded with it (it returned normally, ADR-0008 §8).
    bool verified = false;
};

/// A game's launch readiness (CatalogWorker::readiness).
struct GameReadiness {
    catalog::GameId game;
    GameLaunchState state;
    /// It is not its system's (CatalogSystem::launch), which its rows showed until now.
    bool ownState = false;
    /// False: the catalog no longer has the game (an import removed it since its row was read) or its details could not
    /// be read; `state` says nothing then, and the game is asked again when its row is read again. Every game asked
    /// for, so that nothing waits for it.
    bool known = true;
    /// The systems' readiness it was resolved with: 0 from the opening, one more after each refresh resolved them again
    /// (CatalogRefreshed::generation). An answer of an older generation says nothing any more.
    std::uint64_t generation = 0;
};

/// A refresh ended (CatalogWorker::refresh): its summary and, after an import that ended Done, the systems' readiness
/// resolved again.
struct CatalogRefreshed {
    RefreshSummary summary;
    /// Done and resolved again: the systems with games now, in the chips' order (as CatalogOpened::systems), and the
    /// readiness generation they start; nothing (the previous ones stay) when the import did not end Done or the
    /// systems could not be read again.
    std::optional<std::vector<CatalogSystem>> systems;
    std::uint64_t generation = 0;
    /// ES-DE's configuration as read again (CatalogOpened::libraryError), with `systems`.
    std::optional<Error> libraryError;
    /// The systems a partial refresh imported (RefreshRequest::systems, I7 step 7); empty: every system.
    std::vector<catalog::SystemName> systemsAsked;
};

/// The systems resolved again after a change of what is stored (I7 step 4): as CatalogRefreshed::systems, its generation.
struct CatalogResolved {
    std::vector<CatalogSystem> systems;
    std::uint64_t generation = 0;
};

/// The directory's favourites and recently played games (CatalogWorker::recentGames, I4 step 7b): the catalog's
/// SqliteCatalogQuery::favoritesAndRecent on the worker's connection.
struct RecentGamesList {
    std::uint64_t serial = 0;
    /// The query failed (the list is then empty).
    std::optional<Error> error;
    std::vector<catalog::RecentGame> games;
    /// How long the query took on the worker.
    double milliseconds = 0.0;
};

/// The statistics screen's answer (CatalogWorker::statistics, I7 step 6).
struct StatisticsList {
    std::uint64_t serial = 0;
    /// The read failed.
    std::optional<Error> error;
    catalog::PlayStatistics statistics;
    /// How long the worker took (ADR-0018 §16: shown within 1 s of opening).
    double milliseconds = 0.0;
};

/// What Informazioni's rows show of a game (CatalogWorker::gameControls, I7 step 6).
struct GameControls {
    std::uint64_t serial = 0;
    catalog::GameId game;
    /// The catalog knows it.
    bool known = false;
    catalog::GameStatusState status;
    /// The last status written for it could not be stored, until the next write: the row keeps what it read.
    bool writeFailed = false;
    /// Its system, and what launches it: the choice as people read it, where it comes from
    /// (catalog::toString(ChoiceSource)), and the choice stored for this title, if one is.
    catalog::SystemName system;
    std::string choice;
    std::string source;
    std::optional<launcher::LaunchChoice> own;
};

/// What the worker answered since the last CatalogWorker::take().
struct CatalogResults {
    std::optional<CatalogOpened> opened;
    /// The latest list.
    std::optional<CatalogList> list;
    /// The latest favourites and recent games (I4 step 7b).
    std::optional<RecentGamesList> recentGames;
    std::vector<GameReadiness> readiness;
    std::vector<ResolvedLaunch> launches;
    std::vector<StoredSession> sessions;
    /// Choices that a session verified for their system: (system, choice).
    std::vector<std::pair<catalog::SystemName, launcher::LaunchChoice>> verified;
    /// The refresh running: where it is (the latest), and its end.
    std::optional<RefreshProgress> refreshProgress;
    std::optional<CatalogRefreshed> refreshed;
    /// I7 step 4: the emulators screen's answers: the latest table and samples, how launch tests and changes ended, the
    /// systems resolved again.
    std::optional<EmulatorTable> emulatorTable;
    std::optional<EmulatorSamples> emulatorSamples;
    std::vector<LaunchTestResult> tests;
    std::vector<ChoiceChanged> changes;
    std::optional<CatalogResolved> resolved;
    /// I7 step 6: the latest statistics, and Informazioni's rows.
    std::optional<StatisticsList> statistics;
    std::optional<GameControls> gameControls;
    /// I7 step 7 (ADR-0018 §10): what the file watcher watches for the library as resolved now (after the opening, after
    /// each refresh, after another process's import), and another process's import found (its systems resolved again,
    /// as a refresh's: the lists and the building asked again).
    std::optional<esde::ChangeScopeMap> watchMap;
    std::optional<CatalogRefreshed> otherImport;
    /// The worker's thread ended (stopped, or failed: an opening never posted is then posted as Unavailable): nothing
    /// asked of it will be answered any more. Posted last.
    bool ended = false;

    [[nodiscard]] bool empty() const noexcept {
        return !opened && !list && !recentGames && readiness.empty() && launches.empty() && sessions.empty() &&
               verified.empty() && !refreshProgress && !refreshed && !emulatorTable && !emulatorSamples &&
               tests.empty() && changes.empty() && !resolved && !statistics && !gameControls && !watchMap &&
               !otherImport && !ended;
    }
};

class CatalogWorker {
public:
    /// Materializes a query on the worker's connection: SqliteCatalogQuery::run by default; tests put a slow one in to
    /// check that stopping interrupts it.
    using RunQuery = std::function<Expected<std::vector<catalog::GameId>>(catalog_db::SqliteCatalogQuery& query,
                                                                          const catalog::CatalogQuery& request)>;

    struct Options {
        core::Paths paths;
        /// Called on the worker after it posted results (AppContext::wakeCallback(): SDL_PushEvent, thread-safe); never
        /// after stop() returned. It must be safe to call from another thread.
        std::function<void()> wake;
        /// The owner of the play sessions it records: this process (platform_win::currentProcessIdentity()).
        catalog::SessionOwner owner;
        /// Whether an open session's owner may still run (platform_win::processLiveness() unless set).
        catalog_db::OwnerAlive ownerAlive;
        /// The lists query() asks for (not the opening's first list).
        RunQuery runQuery;
        /// Makes and migrates the database when it is not there (the wizard's page 4, ADR-0018 §4); the other runs leave
        /// that to retroverse-cli import, and say NoDatabase.
        bool createDatabase = false;
        /// The library source a refresh imports from (esde::EsdeLibrarySource unless set; tests slow it down).
        std::function<std::unique_ptr<catalog::ILibrarySource>()> makeSource;
        /// The writer's busy timeout: a session write waits at most this long for another process's write
        /// transaction, then stays pending.
        std::chrono::milliseconds busyTimeout{1000};
        /// How long opening waits while another process migrates the database.
        std::chrono::milliseconds lockWait{2000};
        /// How often pending session writes are tried again.
        std::chrono::milliseconds sessionRetryInterval{2000};
        /// How long stop() tries the session writes still pending (the rest of its bound is the interrupt's).
        std::chrono::milliseconds stopPatience{3000};
        /// How often the newest import is compared with the one known, to follow another process's (ADR-0018 §10:
        /// 5 s, with the file watcher); nothing: never.
        std::optional<std::chrono::milliseconds> otherImportsInterval;
    };

    /// The worker's default bound for stopping (the destructor's).
    static constexpr std::chrono::milliseconds kStopBound{5000};

    /// Starts the thread, which opens the catalog first (CatalogResults::opened).
    explicit CatalogWorker(Options options);
    /// stop(kStopBound).
    ~CatalogWorker();
    CatalogWorker(const CatalogWorker&) = delete;
    CatalogWorker& operator=(const CatalogWorker&) = delete;

    /// Materializes `query`; a query still queued is replaced (only the latest list matters). Answered with `serial`.
    void query(std::uint64_t serial, catalog::CatalogQuery query);
    /// The directory's favourites and recent games, at most `limit` (catalog::RecentGamesQuery, ES-DE's <lastplayed>
    /// read in the machine's time zone: catalog::machineLocalTimeToUtc(), made once on the worker; UTC when the time
    /// zone database cannot be read, logged); one still queued is replaced. Answered with `serial`
    /// (CatalogResults::recentGames). I4 step 7b.
    void recentGames(std::uint64_t serial, std::size_t limit);
    /// The launch readiness of `games` (CatalogResults::readiness): their system's, or resolved for each game where it
    /// can differ (its availability, its <altemulator>, Retroverse's choice for it, a folder game, a system whose
    /// resolution leaves something to each game).
    void readiness(std::vector<catalog::GameId> games);
    /// Resolves the launch of `game` right before it starts (its file on disk now, the database's choices); `test`: a
    /// launch test, with that choice (CatalogResults::tests says how it ended, once its session ended).
    void resolve(std::uint64_t token, catalog::GameId game, std::optional<LaunchTestChoice> test = std::nullopt);
    /// The table of the systems with games (CatalogResults::emulatorTable), answered with `serial`: what they stand on as
    /// resolved already (ADR-0018 §16: at once), without their options.
    void emulatorTable(std::uint64_t serial);
    /// The options of `system`, each translated now, and the games a launch test of it offers, at most `limit`
    /// (CatalogResults::emulatorSamples).
    void sampleGames(std::uint64_t serial, catalog::SystemName system, std::size_t limit);
    /// The launch test `token` will have no session (a launch refused before one exists): its choice is forgotten.
    void forgetTest(std::uint64_t token);
    /// Changes what is stored (CatalogResults::changes), then resolves the systems again (::resolved).
    void changeChoice(std::uint64_t serial, ChoiceChange change);
    /// What was played (CatalogResults::statistics), `limit` games per list; one still queued is replaced.
    void statistics(std::uint64_t serial, std::size_t limit);
    /// The status and the launch choice of `game` (CatalogResults::gameControls); one still queued is replaced.
    void gameControls(std::uint64_t serial, catalog::GameId game);
    /// Sets the status of `game`, then answers as gameControls().
    void setGameStatus(std::uint64_t serial, catalog::GameId game, catalog::GameStatus status);
    /// The play session of the launch `token`: it began (Running, the started process), it ended (its final state; a
    /// blocked attempt without beginSession()). `system` and `program` (the request's executable) record a launch
    /// verification when it returned normally.
    void beginSession(std::uint64_t token, catalog::PlaySession session);
    void finishSession(std::uint64_t token, catalog::PlaySession session, catalog::SystemName system,
                       std::filesystem::path program);

    /// Refreshes the catalog from the library (the import, sliced; CatalogResults::refreshProgress, ::refreshed), of the
    /// systems `request` names or of every one. False, and nothing asked, while a refresh is queued or runs, or once the
    /// worker stops or ended. Only once the catalog opened Ready: a catalog that did not open answers nothing more.
    bool refresh(RefreshRequest request = {});

    /// What it answered since the last call (main thread).
    [[nodiscard]] CatalogResults take();
    /// Nothing queued or running: every request so far is answered in take() (lockstep runs wait for it).
    [[nodiscard]] bool idle() const;

    /// Ends the worker: queued lists and readiness are dropped, a running query is interrupted, the session writes
    /// queued and pending are tried for Options::stopPatience; then the thread ends. True when it ended within `bound`
    /// (joined); otherwise it is left to end by itself (detached, its wake callback dropped) and a warning is logged.
    /// Idempotent.
    bool stop(std::chrono::milliseconds bound = kStopBound);

    /// What the main thread and the worker's thread share (catalog_worker.cpp).
    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
    std::thread m_thread;
};

} // namespace rv::app
