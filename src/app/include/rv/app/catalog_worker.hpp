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
// read-only connection, which the worker opens for it (CatalogOpened::pages). No GL or SDL call happens on the worker;
// it wakes the host (Options::wake) after it posted results. Stopping it interrupts a running query
// (sqlite3_interrupt through Connection::interrupt) and ends within a bound.

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

#include <rv/catalog/ids.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/expected.hpp>
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

/// The blocker code of a launch that cannot be resolved because the library's configuration cannot be read (ES-DE's
/// settings or systems: CatalogOpened::libraryError).
inline constexpr std::string_view kLibraryUnreadableCode = "library-unreadable";

/// A system of the catalog with games, as the quick catalog shows it: a filter chip, and what its games share.
struct CatalogSystem {
    catalog::SystemName name;
    /// Its <fullname>, the chip's label (its name when the catalog has none).
    std::string fullName;
    /// Available Game entries: what the chip's filter lists without a search (SqliteCatalogQuery::systems).
    std::size_t games = 0;
    /// The readiness its games share (catalog::resolveLaunch without a game), resolved when the catalog opened, and
    /// the choice it rests on.
    GameLaunchState launch;
    launcher::LaunchChoice choice;
    /// Its resolution leaves something to each game (an %INJECT% file, a %STARTDIR%=%GAMEDIR% folder): each game is
    /// resolved for its own readiness.
    bool perGame = false;

    bool operator==(const CatalogSystem&) const = default;
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
    /// The play sessions closed as interrupted (their owner was gone, ADR-0008 §9).
    std::vector<catalog::PlaySession> interrupted;
    /// ES-DE's configuration could not be read: the catalog can be browsed, nothing launches (kLibraryUnreadableCode).
    std::optional<Error> libraryError;
    /// How long the worker took to open it, and when it was done.
    double milliseconds = 0.0;
    std::chrono::system_clock::time_point at;
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
};

/// What the worker answered since the last CatalogWorker::take().
struct CatalogResults {
    std::optional<CatalogOpened> opened;
    /// The latest list.
    std::optional<CatalogList> list;
    std::vector<GameReadiness> readiness;
    std::vector<ResolvedLaunch> launches;
    std::vector<StoredSession> sessions;
    /// Choices that a session verified for their system: (system, choice).
    std::vector<std::pair<catalog::SystemName, launcher::LaunchChoice>> verified;

    [[nodiscard]] bool empty() const noexcept {
        return !opened && !list && readiness.empty() && launches.empty() && sessions.empty() && verified.empty();
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
        /// Called on the worker after it posted results (AppContext::wakeCallback()); never after stop() returned.
        std::function<void()> wake;
        /// The owner of the play sessions it records: this process (platform_win::currentProcessIdentity()).
        catalog::SessionOwner owner;
        /// Whether an open session's owner may still run (platform_win::processLiveness() unless set).
        catalog_db::OwnerAlive ownerAlive;
        RunQuery runQuery;
        /// The writer's busy timeout: a session write waits at most this long for another process's write
        /// transaction, then stays pending.
        std::chrono::milliseconds busyTimeout{1000};
        /// How long opening waits while another process migrates the database.
        std::chrono::milliseconds lockWait{2000};
        /// How often pending session writes are tried again.
        std::chrono::milliseconds sessionRetryInterval{2000};
        /// How long stop() tries the session writes still pending (the rest of its bound is the interrupt's).
        std::chrono::milliseconds stopPatience{3000};
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
    /// The launch readiness of `games` (CatalogResults::readiness): their system's, or resolved for each game where it
    /// can differ (its availability, its <altemulator>, Retroverse's choice for it, a folder game, a system whose
    /// resolution leaves something to each game).
    void readiness(std::vector<catalog::GameId> games);
    /// Resolves the launch of `game` right before it starts (its file on disk now, the database's choices).
    void resolve(std::uint64_t token, catalog::GameId game);
    /// The play session of the launch `token`: it began (Running), its state changed (the started process), it ended
    /// (its final state; a blocked attempt without beginSession()). `system` and `program` (the request's executable)
    /// record a launch verification when it returned normally.
    void beginSession(std::uint64_t token, catalog::PlaySession session);
    void updateSession(std::uint64_t token, catalog::PlaySession session);
    void finishSession(std::uint64_t token, catalog::PlaySession session, catalog::SystemName system,
                       std::filesystem::path program);

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
