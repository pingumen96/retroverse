#pragma once

// SessionStore (I2 step 12): the launch state of the catalog database (ADR-0008 §5, §6, §8, §9), on the tables of the
// schema area "launch" (schema_launch.cpp): play sessions (rv_play_sessions), Retroverse's launch choices per game and
// per system (rv_launch_game_choices, rv_launch_system_choices: retroverse-cli assign), the programs chosen for emulators
// (rv_launch_program_choices, I7 step 4) and launch verifications (rv_launch_verifications). They are state tables: `db rebuild-index` never touches them, and each refers to the game
// identity (rv_game_identities) it names, never to the index.
//
// Writes go through the process's writer connection (Database::writer(): one writer per process, one thread), each in
// one short write transaction (BEGIN IMMEDIATE), as upserts (never REPLACE). They never touch identities or index rows,
// and take no import lock: an import of this process simply runs before or after them. Another process's import can
// hold SQLite's write lock longer than the writer's busy timeout (a bulk import writes for 5 to 9 s on the reference
// library, docs/i2/step-11a.md): a write that meets such a busy database (isBusyError()) is tried again for as long as
// its BusyRetry says, and a caller that cannot wait keeps it and tries again later (SessionRecorder) rather than lose it.
// Reads work on the writer or on a reader (Database::openReader()), which never waits.
//
// A GameId that is an alias (rv_identity_aliases, I2 step 11b: a file moved back onto a key that had another identity
// meanwhile, a review resolved) stands for the identity it names: writes and reads that name a game resolve it, so a
// launch whose game became an alias while it ran is still recorded under the identity that holds the game's history.
//
// Sessions (ADR-0008 §9): insertSession() records an attempt when it begins (Running, its owner the recording process)
// or at once when it ended before it began (Blocked: the game was not ready right before the launch); updateSession()
// records its end. At every start of a process that records sessions, closeInterrupted() closes as Interrupted the open
// sessions whose owner is gone (the owner's process id and creation time: an id reused by a later process is not the
// owner), and only those: a session another live process (the app, another retroverse-cli) follows stays open.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/game_status.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/core/expected.hpp>
#include <rv/launcher/emulator_profile.hpp>

namespace rv::catalog_db {

class Connection;

/// How a write that meets a busy database (isBusyError()) is tried again.
struct BusyRetry {
    /// How long, after an attempt that failed so, to keep trying: each attempt already waits up to the connection's busy
    /// timeout; 0 makes one attempt.
    std::chrono::milliseconds retryFor{0};
    /// The pause between two attempts.
    std::chrono::milliseconds interval{250};
};

/// The BusyRetry of a write that must not be lost: two minutes, much more than the longest bulk import's write
/// transaction measured on the reference library (9 s, docs/i2/step-11a.md).
inline constexpr BusyRetry kPatientRetry{std::chrono::minutes(2), std::chrono::milliseconds(250)};

/// A choice as stored, with when it was made.
struct StoredChoice {
    launcher::LaunchChoice choice;
    std::chrono::system_clock::time_point chosenAt;

    bool operator==(const StoredChoice&) const = default;
};

/// A per-system choice of a library.
struct SystemChoiceRow {
    catalog::SystemName system;
    StoredChoice stored;

    bool operator==(const SystemChoiceRow&) const = default;
};

/// A program chosen for a %EMULATOR_X% of a library (ADR-0018 §5).
struct ProgramChoiceRow {
    /// X as the commands write it.
    std::string emulator;
    std::filesystem::path program;
    std::chrono::system_clock::time_point chosenAt;

    bool operator==(const ProgramChoiceRow&) const = default;
};

/// A launch verification: the latest session that returned normally with a choice for a system (ADR-0008 §8).
struct LaunchVerification {
    /// The store's id; 0 until stored.
    std::int64_t id = 0;
    catalog::LibraryId library;
    catalog::SystemName system;
    launcher::LaunchChoice choice;
    catalog::GameId game;
    /// The session (rv_play_sessions) that verified it.
    std::int64_t session = 0;
    /// It was a launch test (retroverse-cli launch --test), not an ordinary launch.
    bool test = false;
    /// The program that ran (LaunchRequest::executable).
    std::filesystem::path program;
    std::chrono::system_clock::time_point verifiedAt;

    bool operator==(const LaunchVerification&) const = default;
};

/// Which sessions sessions() lists.
struct SessionFilter {
    /// Only this game's (an alias stands for the game it names); nothing: every game's.
    std::optional<catalog::GameId> game;
    /// At most this many, the newest first; 0: no limit.
    std::size_t limit = 50;
};

/// A session as sessions() lists it, with what the catalog knows of its game.
struct ListedSession {
    catalog::PlaySession session;
    /// The game's identity: its library, system and relative path when last seen.
    catalog::LibraryId library;
    catalog::SystemName system;
    std::string relativePath;
    /// Its title, when the index holds a row for it.
    std::optional<std::string> title;

    bool operator==(const ListedSession&) const = default;
};

/// Whether the owner of a session may still run: false only when it is known to be gone (platform_win::
/// processLiveness() in retroverse-cli). Called outside any transaction.
using OwnerAlive = std::function<bool(const catalog::SessionOwner& owner)>;

class SessionStore {
public:
    /// The store on `connection`: the database's writer to write (Database::writer()), any connection to read.
    /// `connection` must outlive the store. Errors: Conflict when the database's schema version is not
    /// programSchemaVersion() (older: migrate() it first; newer: written by a newer Retroverse), and SQLite's.
    [[nodiscard]] static Expected<std::unique_ptr<SessionStore>> open(Connection& connection);

    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    ~SessionStore();

    // --- Sessions ----------------------------------------------------------------------------------------------------

    /// Records a new session (its id must be 0) and returns its id. Its game is resolved through the aliases. Errors:
    /// InvalidArgument for a session that already has an id, an outcome its fields contradict (Running or Interrupted
    /// with an end time, an end time missing, Blocked without a readiness or with Ready, LeftRunning without a reason,
    /// an exit code without its kind or a kind without a code, an exit for an attempt that never started (Blocked, a
    /// refusal by the guard, the failures InvalidRequest and StartFailed), Crashed without one), text that is not
    /// UTF-8; NotFound when its game is neither an identity nor an alias; Conflict (busy) once `retry` is spent;
    /// SQLite's others.
    [[nodiscard]] Expected<std::int64_t> insertSession(const catalog::PlaySession& session, const BusyRetry& retry = {});
    /// Writes the end of the stored session `session.id` (every field but the game and the owner). Errors: NotFound for
    /// an id the store does not have; those of insertSession().
    [[nodiscard]] Status updateSession(const catalog::PlaySession& session, const BusyRetry& retry = {});
    /// Closes as Interrupted, at `now`, the open sessions whose owner `ownerAlive` says is gone, and returns them as
    /// closed (in id order). A session whose row changed meanwhile (its owner ended it) is left as it is.
    [[nodiscard]] Expected<std::vector<catalog::PlaySession>>
    closeInterrupted(const OwnerAlive& ownerAlive, std::chrono::system_clock::time_point now,
                     const BusyRetry& retry = {});
    /// The session `id`, or nothing.
    [[nodiscard]] Expected<std::optional<catalog::PlaySession>> session(std::int64_t id);
    /// The sessions `filter` selects, newest (highest id) first.
    [[nodiscard]] Expected<std::vector<ListedSession>> sessions(const SessionFilter& filter);

    // --- Choices -----------------------------------------------------------------------------------------------------

    /// Retroverse's choice for `game` (an alias stands for its game), or nothing.
    [[nodiscard]] Expected<std::optional<StoredChoice>> gameChoice(catalog::GameId game);
    /// Stores Retroverse's choice for `game` (replacing one it had), made at `chosenAt`. Whether the label or the profile
    /// exists is the caller's to check (retroverse-cli assign). Errors: InvalidArgument for an empty value or text that
    /// is not UTF-8; NotFound when `game` is neither an identity nor an alias; Conflict (busy); SQLite's others.
    [[nodiscard]] Status setGameChoice(catalog::GameId game, const launcher::LaunchChoice& choice,
                                       std::chrono::system_clock::time_point chosenAt, const BusyRetry& retry = {});
    /// Removes Retroverse's choice for `game`; true when it had one.
    [[nodiscard]] Expected<bool> clearGameChoice(catalog::GameId game, const BusyRetry& retry = {});

    /// Retroverse's choices for the systems of `library` (assign's), by system name.
    [[nodiscard]] Expected<std::vector<SystemChoiceRow>> systemChoices(const catalog::LibraryId& library);
    /// Retroverse's choice for `system` of `library`, or nothing.
    [[nodiscard]] Expected<std::optional<StoredChoice>> systemChoice(const catalog::LibraryId& library,
                                                                     const catalog::SystemName& system);
    /// Stores Retroverse's choice for `system` of `library`. Errors: InvalidArgument for an empty value, an empty system
    /// or a library the database does not hold (rv_libraries: an import records it); Conflict (busy); SQLite's others.
    [[nodiscard]] Status setSystemChoice(const catalog::LibraryId& library, const catalog::SystemName& system,
                                         const launcher::LaunchChoice& choice,
                                         std::chrono::system_clock::time_point chosenAt, const BusyRetry& retry = {});
    /// Removes Retroverse's choice for `system` of `library`; true when it had one.
    [[nodiscard]] Expected<bool> clearSystemChoice(const catalog::LibraryId& library, const catalog::SystemName& system,
                                                   const BusyRetry& retry = {});

    /// The programs chosen for the emulators of `library`, by emulator (ADR-0018 §5).
    [[nodiscard]] Expected<std::vector<ProgramChoiceRow>> programChoices(const catalog::LibraryId& library);
    /// Stores the program of `emulator` of `library`. Whether it may run is the caller's to check first
    /// (esde::checkProgramChoice()), and every translation checks it again. Errors: InvalidArgument for an empty emulator
    /// or program, text that is not UTF-8, or a library the database does not hold; Conflict (busy); SQLite's others.
    [[nodiscard]] Status setProgramChoice(const catalog::LibraryId& library, std::string_view emulator,
                                          const std::filesystem::path& program,
                                          std::chrono::system_clock::time_point chosenAt, const BusyRetry& retry = {});
    /// Removes the program of `emulator` of `library`; true when it had one.
    [[nodiscard]] Expected<bool> clearProgramChoice(const catalog::LibraryId& library, std::string_view emulator,
                                                    const BusyRetry& retry = {});

    // --- The games' statuses (I7 step 6, ADR-0018 §9) ------------------------------------------------------------------

    /// The status set for `game` (an alias stands for its game); nothing when it derives one.
    [[nodiscard]] Expected<std::optional<catalog::GameStatus>> gameStatus(catalog::GameId game);
    /// Sets the status of `game`, replacing one it had. Errors: NotFound when `game` is neither an identity nor an alias;
    /// Conflict (busy); SQLite's others.
    [[nodiscard]] Status setGameStatus(catalog::GameId game, catalog::GameStatus status,
                                       std::chrono::system_clock::time_point setAt, const BusyRetry& retry = {});

    // --- Verifications -----------------------------------------------------------------------------------------------

    /// Records `verification` as the latest of its library, system and choice (its id is ignored) and returns the row's
    /// id. Its game is resolved through the aliases. Errors: InvalidArgument for an empty system, a library the database
    /// does not hold or a session it does not have; NotFound for a game that is neither an identity nor an alias;
    /// Conflict (busy); SQLite's others.
    [[nodiscard]] Expected<std::int64_t> recordVerification(const LaunchVerification& verification,
                                                            const BusyRetry& retry = {});
    /// The verifications of `library`, by system, choice kind and value.
    [[nodiscard]] Expected<std::vector<LaunchVerification>> verifications(const catalog::LibraryId& library);

    // --- Games -------------------------------------------------------------------------------------------------------

    /// The identity `id` stands for: `id` itself when it is an identity, the game an alias names when it is an alias,
    /// nothing when it is neither.
    [[nodiscard]] Expected<std::optional<catalog::GameId>> resolveGame(catalog::GameId id);

    /// How many attempts of this store's writes met a busy database and were tried again (logs, tests). Readable on any
    /// thread, also while a write waits.
    [[nodiscard]] std::size_t busyRetries() const noexcept;

private:
    struct State;

    explicit SessionStore(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

/// Records one launch attempt through a SessionStore without losing a write to a busy database: a write that meets one
/// (once its quick retry is spent) is kept, and tried again by the next call, the latest state of the session winning.
/// The launch never waits long for the database: begin() tries once (its connection's busy timeout); the end is written
/// by finish() with the patience it is given.
class SessionRecorder {
public:
    /// `store` must outlive the recorder.
    SessionRecorder(SessionStore& store, catalog::PlaySession session) noexcept;

    /// Records the session as it is now (Running when the launch begins; a final outcome for an attempt that ended
    /// before it began), trying with `retry`. A busy database keeps it pending (Ok); other errors are returned (the
    /// session stays pending).
    [[nodiscard]] Status begin(const BusyRetry& retry = {});
    /// Replaces the session's state with `session` (its id is the recorder's), to be written by flush() or finish().
    void update(catalog::PlaySession session);
    /// Writes what is pending, if anything, trying with `retry`; Ok when nothing is pending afterwards or the database
    /// stayed busy (still pending), the error otherwise.
    [[nodiscard]] Status flush(const BusyRetry& retry = {});
    /// Writes what is pending with `retry`: Ok once the session is stored as it is; the last error when it could not
    /// be (Conflict when the database stayed busy for all of `retry`).
    [[nodiscard]] Status finish(const BusyRetry& retry = kPatientRetry);

    /// The session as last given, with its id once stored (0 before).
    [[nodiscard]] const catalog::PlaySession& session() const noexcept { return m_session; }
    /// Something is not stored yet.
    [[nodiscard]] bool pending() const noexcept { return m_pending; }
    /// The session's row exists (it was inserted).
    [[nodiscard]] bool stored() const noexcept { return m_session.id != 0; }

private:
    /// One write of the pending state with `retry`.
    [[nodiscard]] Status write(const BusyRetry& retry);

    SessionStore* m_store;
    catalog::PlaySession m_session;
    bool m_pending = true;
};

} // namespace rv::catalog_db
