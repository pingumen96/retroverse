#pragma once

// The catalog database's connections (ADR-0008 §5). SQLite is private to rv_catalog_db: no SQLite type appears here.
//
// One database file, <user data>/database/retroverse.sqlite (databaseFile()), which a process opens once (Database):
// the process-level lock file beside it held shared, one writer connection used from one thread (the catalog worker in
// the app, the main thread of retroverse-cli), and read-only reader connections for the other threads. In WAL mode a
// reader sees the last committed snapshot while a write transaction is open; readers have no busy timeout, so the rare
// operation that would have to wait fails at once (Conflict) instead of blocking. Statements are prepared once and
// reset after each use (ScopedReset), so that none keeps a read transaction, and with it an old snapshot of the WAL,
// open. Errors are Expected values carrying SQLite's own message. Every connection opens its file through SQLite's
// "win32-longpath" VFS, and through the file's \\?\ form when its path is long enough to need it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <rv/core/expected.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::catalog_db {

/// The database's file name in core::Paths::databaseDir().
inline constexpr std::string_view kDatabaseFileName = "retroverse.sqlite";

/// PRAGMA application_id of a Retroverse catalog database ("RVCD"), set by its first migration: a file at the
/// database's path that carries another id, or none while it holds tables, is not opened (Database::open()).
inline constexpr std::int32_t kApplicationId = 0x52564344;

/// The writer's journal_size_limit: the WAL file is cut back to this size when a checkpoint has emptied it.
inline constexpr std::int64_t kJournalSizeLimitBytes = 64ll * 1024 * 1024;

/// The writer's page cache, in KiB (PRAGMA cache_size = -kWriterCacheKiB): an import writes rows in GameId order, which
/// is random, all over its tables; with SQLite's default 2 MiB the transaction spills and reads pages back constantly.
inline constexpr std::int64_t kWriterCacheKiB = 32 * 1024;

/// <user data>/database/retroverse.sqlite.
[[nodiscard]] std::filesystem::path databaseFile(const core::Paths& paths);

/// The process-level lock file of a database: "<database file>.lock", beside it.
[[nodiscard]] std::filesystem::path lockFileOf(const std::filesystem::path& databaseFile);

/// True when the SQLite this program runs with has FTS5 (sqlite3_compileoption_used("ENABLE_FTS5")): the search index
/// needs it (ADR-0008 §5), so Database::open() refuses to work without it.
[[nodiscard]] bool sqliteHasFts5() noexcept;

/// The version of the SQLite library in use ("3.53.4").
[[nodiscard]] std::string_view sqliteVersion() noexcept;

namespace detail {
struct StatementHandle;  ///< SQLite's statement, opaque outside database.cpp
struct ConnectionHandle; ///< SQLite's connection, opaque outside database.cpp
} // namespace detail

/// The kind of value a column of the current row holds.
enum class ColumnType : std::uint8_t {
    Integer,
    Real,
    Text,
    Blob,
    Null,
};

/// A prepared statement (RAII: finalized when destroyed). Used from the thread of its connection. A statement may
/// outlive its Connection object: SQLite closes the connection once its last statement is finalized.
///
/// Parameters are bound by their 1-based index (?1 is 1); text and blobs are copied. A binding that fails (no such
/// parameter, a value too large) is reported by the next step() or run(). Column functions read the current row, the
/// 0-based column index of the result.
class Statement {
public:
    Statement() noexcept = default;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    /// False for a default-constructed or moved-from statement.
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }
    /// The SQL it was prepared from.
    [[nodiscard]] std::string_view sql() const noexcept;

    Statement& bindInt(int index, std::int64_t value) noexcept;
    Statement& bindReal(int index, double value) noexcept;
    Statement& bindText(int index, std::string_view text) noexcept;
    Statement& bindBlob(int index, std::span<const std::byte> bytes) noexcept;
    Statement& bindNull(int index) noexcept;
    /// bindInt(0 or 1).
    Statement& bindBool(int index, bool value) noexcept { return bindInt(index, value ? 1 : 0); }
    /// bindInt() of the value, or bindNull() when there is none.
    Statement& bindOptionalInt(int index, const std::optional<std::int64_t>& value) noexcept;
    /// bindText() of the value, or bindNull() when there is none.
    Statement& bindOptionalText(int index, const std::optional<std::string_view>& text) noexcept;

    /// Runs the statement to its next row: true when a row is ready (the column functions read it), false when the
    /// statement has run to its end. Errors: a binding that failed; SQLite's error with its message and the
    /// statement's SQL as context: Conflict (SQLITE_BUSY, SQLITE_LOCKED, a UNIQUE or PRIMARY KEY constraint),
    /// InvalidArgument (a FOREIGN KEY constraint, an SQL error), ValidationFailed (a CHECK, NOT NULL or type
    /// constraint), Cancelled (interrupted), ParseError (a damaged file, not a database), IoError (the file system).
    [[nodiscard]] Expected<bool> step();
    /// Steps to the end, ignoring the rows, then resets the statement (for statements that return none).
    [[nodiscard]] Status run();
    /// Resets the statement for its next use and clears its bindings (ends its read transaction). Never fails: an error
    /// of the last step was reported by it.
    void reset() noexcept;

    [[nodiscard]] int columnCount() const noexcept;
    [[nodiscard]] ColumnType columnType(int column) const noexcept;
    [[nodiscard]] bool isNull(int column) const noexcept { return columnType(column) == ColumnType::Null; }
    [[nodiscard]] std::int64_t columnInt(int column) const noexcept;
    [[nodiscard]] double columnReal(int column) const noexcept;
    /// The column as UTF-8 text, valid until the next step() or reset(); empty for NULL.
    [[nodiscard]] std::string_view columnText(int column) const noexcept;
    /// The column's bytes, valid until the next step() or reset(); empty for NULL.
    [[nodiscard]] std::span<const std::byte> columnBlob(int column) const noexcept;
    /// columnInt() of the column, or nothing when it is NULL.
    [[nodiscard]] std::optional<std::int64_t> columnOptionalInt(int column) const noexcept;

private:
    friend class Connection;
    explicit Statement(detail::StatementHandle* handle) noexcept : m_handle(handle) {}
    void noteBindResult(int index, int result) noexcept;

    detail::StatementHandle* m_handle = nullptr;
    /// The first binding that failed since the last reset().
    std::optional<Error> m_bindError;
};

/// Resets a statement when the scope ends (Statement::reset()): every use of a kept statement goes through one, so that
/// no statement keeps its read transaction, and an old snapshot of the WAL, open.
class [[nodiscard]] ScopedReset {
public:
    explicit ScopedReset(Statement& statement) noexcept : m_statement(statement) {}
    ~ScopedReset() { m_statement.reset(); }
    ScopedReset(const ScopedReset&) = delete;
    ScopedReset& operator=(const ScopedReset&) = delete;

private:
    Statement& m_statement;
};

/// What a connection is for.
enum class ConnectionRole : std::uint8_t {
    Writer, ///< read-write with a busy timeout: the process's one writer (Database::writer())
    Reader, ///< SQLITE_OPEN_READONLY, no busy timeout: never waits (Database::openReader())
    Memory, ///< a private in-memory database (Connection::openMemory())
};

/// A connection to the database (RAII: closed when destroyed). Used from one thread at a time; interrupt() alone may be
/// called from another thread.
class Connection {
public:
    Connection() noexcept = default;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    /// A private in-memory database, foreign keys enforced (the program's schema built to compare a file's with).
    [[nodiscard]] static Expected<Connection> openMemory();

    [[nodiscard]] bool isOpen() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] ConnectionRole role() const noexcept { return m_role; }
    /// The database file (absolute, as given: never its \\?\ form); empty for an in-memory database.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    /// Runs `sql`, one or more statements without parameters, to its end (sqlite3_exec). Errors: the first statement
    /// that fails, as Statement::step() reports it.
    [[nodiscard]] Status execute(std::string_view sql);
    /// Prepares one statement; text after it must be blank or comments. Errors: SQLite's (an SQL error is
    /// InvalidArgument), InvalidArgument for no statement or more than one.
    [[nodiscard]] Expected<Statement> prepare(std::string_view sql);
    /// The first column of the first row `sql` returns as an integer; nothing when it returns no row or NULL.
    [[nodiscard]] Expected<std::optional<std::int64_t>> queryInt(std::string_view sql);
    /// The first column of the first row `sql` returns as text; nothing when it returns no row or NULL.
    [[nodiscard]] Expected<std::optional<std::string>> queryText(std::string_view sql);
    /// PRAGMA user_version: the schema version, 0 for a database no migration ran on.
    [[nodiscard]] Expected<int> userVersion();

    /// A transaction is open (SQLite is not in autocommit mode).
    [[nodiscard]] bool inTransaction() const noexcept;
    [[nodiscard]] std::int64_t lastInsertRowId() const noexcept;
    /// Rows changed by the last INSERT, UPDATE or DELETE.
    [[nodiscard]] std::int64_t changes() const noexcept;
    /// The busy timeout: how long a statement waits for another connection's lock before Conflict (0 for readers).
    [[nodiscard]] std::chrono::milliseconds busyTimeout() const noexcept { return m_busyTimeout; }

    /// Makes the statement this connection runs stop with Cancelled (sqlite3_interrupt), and the statements started
    /// before it returns; for shutdown. Safe from any thread, provided the connection stays open until it returns.
    void interrupt() noexcept;

private:
    friend class Database;

    /// Opens `file` (ignored for Memory) in `role`, foreign keys enforced, in SQLite's defensive mode (no SQL may
    /// corrupt the file on purpose), with `busyTimeout`; Database::open() adds the writer's journal pragmas.
    [[nodiscard]] static Expected<Connection> open(const std::filesystem::path& file, ConnectionRole role, bool create,
                                                   std::chrono::milliseconds busyTimeout,
                                                   std::shared_ptr<const void> keepAlive);

    Connection(detail::ConnectionHandle* handle, ConnectionRole role, std::filesystem::path path,
               std::chrono::milliseconds busyTimeout, std::shared_ptr<const void> keepAlive) noexcept;
    void close() noexcept;

    detail::ConnectionHandle* m_handle = nullptr;
    ConnectionRole m_role = ConnectionRole::Memory;
    std::filesystem::path m_path;
    std::chrono::milliseconds m_busyTimeout{0};
    /// What the connection keeps alive while it lives: a reader, the process-level lock of its database.
    std::shared_ptr<const void> m_keepAlive;
};

/// A transaction (RAII): BEGIN when made, ROLLBACK when destroyed unless committed or rolled back before.
class Transaction {
public:
    enum class Mode : std::uint8_t {
        /// BEGIN: a read transaction until the first write, so that several statements read one snapshot.
        Deferred,
        /// BEGIN IMMEDIATE: the write lock at once, waiting up to the busy timeout (Conflict after it).
        Immediate,
    };

    /// Errors: Conflict (Immediate: another connection writes past the busy timeout), InvalidArgument (a transaction
    /// is open on the connection already), and SQLite's others.
    [[nodiscard]] static Expected<Transaction> begin(Connection& connection, Mode mode);

    Transaction() noexcept = default;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    [[nodiscard]] bool active() const noexcept { return m_connection != nullptr; }
    /// COMMIT. The transaction ends either way: when COMMIT fails, it is rolled back. Errors: SQLite's;
    /// InvalidArgument when it has ended.
    [[nodiscard]] Status commit();
    /// ROLLBACK (nothing when it has ended).
    void rollback() noexcept;

private:
    explicit Transaction(Connection& connection) noexcept : m_connection(&connection) {}

    Connection* m_connection = nullptr;
};

/// How a holder holds the process-level lock.
enum class LockMode : std::uint8_t {
    Shared,    ///< the database is open in this process
    Exclusive, ///< a migration runs: no other holder exists
};

/// The process-level lock of a database (ADR-0008 §5): its lock file (lockFileOf()) locked with LockFileEx, so that
/// the system releases it when the process ends, however it ends. Every Retroverse process holds it shared while it has
/// the database open; a migration holds it exclusive, so that it never runs while another process has the database
/// open. A holder is one instance: two instances in one process exclude each other as two processes do.
///
/// Bytes of the lock file: byte 1 is the lock itself (shared or exclusive); byte 0 is its gate, taken exclusively for
/// an instant by a holder joining, and by a holder that wants to migrate from the moment it waits for the others to
/// leave to the end of its migration, so that nobody joins meanwhile and moving between shared and exclusive never
/// loses the lock; byte 2 is the import lock (lockImport()). The lock file is empty and stays in place: holders open it
/// without delete sharing, so that it can be neither deleted nor renamed while any holder has it open (a process that
/// then made a new one would lock it apart from the holders of the old one, and could migrate while they have the
/// database open).
class DatabaseLock {
public:
    /// What upgrade() found.
    enum class Upgrade : std::uint8_t {
        Exclusive, ///< the lock is exclusive
        GateTaken, ///< another holder has the gate (it waits to migrate, or joins this instant): still shared
    };

    /// Opens (creating it) the lock file and takes it shared, waiting up to `wait` while another holder has the gate
    /// (a migration). Errors: Conflict when it still has it after the wait; NotFound, Unavailable, IoError when the
    /// lock file cannot be opened or created (its folder must exist).
    [[nodiscard]] static Expected<DatabaseLock> acquireShared(const std::filesystem::path& lockFile,
                                                              std::chrono::milliseconds wait);

    DatabaseLock(DatabaseLock&& other) noexcept;
    DatabaseLock& operator=(DatabaseLock&& other) noexcept;
    DatabaseLock(const DatabaseLock&) = delete;
    DatabaseLock& operator=(const DatabaseLock&) = delete;
    /// Releases every byte it holds, then closes the lock file.
    ~DatabaseLock();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] LockMode mode() const noexcept { return m_mode; }
    /// False once the lock was lost (a rejoin() that failed): what the holder protected is unprotected.
    [[nodiscard]] bool held() const noexcept { return m_held; }

    /// From shared to exclusive: takes the gate, so that nobody joins from then on, then waits up to `wait` for the
    /// other holders to leave. GateTaken at once when another holder has the gate: when it wants to migrate too, the
    /// two would wait on each other; rejoin() lets it go first. Errors: Conflict when other holders still have the
    /// lock after the wait (this one then holds it shared again, the gate released).
    [[nodiscard]] Expected<Upgrade> upgrade(std::chrono::milliseconds wait);
    /// From exclusive back to shared, the gate released; the lock is never released in between (nothing when shared).
    void downgrade() noexcept;
    /// Leaves and joins again: releases the shared lock, so that the holder with the gate can migrate, then takes it
    /// again, waiting up to `wait` while that holder has the gate. Errors: as acquireShared(), the lock then lost
    /// (held() false); InvalidArgument when the lock is exclusive or not held.
    [[nodiscard]] Status rejoin(std::chrono::milliseconds wait);

    /// Takes the import lock without waiting. Errors: Conflict when another holder (another import, in this process or
    /// another one) has it, or this one already does.
    [[nodiscard]] Status lockImport();
    /// Releases the import lock (nothing when not held).
    void unlockImport() noexcept;
    [[nodiscard]] bool holdsImport() const noexcept { return m_import; }

private:
    DatabaseLock(void* file, std::filesystem::path path) noexcept : m_file(file), m_path(std::move(path)) {}
    void release() noexcept;
    /// Takes the lock shared through the gate, waiting up to `wait` while another holder has the gate.
    [[nodiscard]] Status join(std::chrono::milliseconds wait);

    void* m_file = nullptr; ///< the lock file's HANDLE
    std::filesystem::path m_path;
    LockMode m_mode = LockMode::Shared;
    bool m_held = false;   ///< byte 1 held (m_mode says how)
    bool m_gate = false;   ///< byte 0 held (exclusive)
    bool m_import = false; ///< byte 2 held (exclusive)
};

/// How Database::open() opens the database.
struct DatabaseOptions {
    /// Create the database, and the folders above it, when the file does not exist; otherwise NotFound.
    bool create = true;
    /// The writer's busy timeout: how long its statements wait while another process writes.
    std::chrono::milliseconds busyTimeout{5000};
    /// How long opening waits while another process migrates the database (holds its lock exclusive).
    std::chrono::milliseconds lockWait{10000};
    /// Switch the file to WAL journal mode, as every product path does. Off for a database opened to be looked at
    /// (`db check`, `db info`): the file keeps the journal mode it has (a backup put back by hand is in rollback
    /// journal mode, an empty file is no database yet), so that opening it writes nothing into it.
    bool switchToWal = true;
};

/// The catalog database of a process: its process-level lock (held shared while it is open), its one writer connection
/// and the read-only readers it opens. It does not migrate (migrator.hpp: migrate(), openCatalogDatabase()).
class Database {
public:
    /// Opens the database at `file` for this process: takes its lock shared (waiting up to options.lockWait while
    /// another process migrates it), opens the writer connection (WAL unless options.switchToWal is off, foreign keys
    /// enforced, synchronous FULL, journal_size_limit, a page cache of kWriterCacheKiB, the busy timeout; SQLite's
    /// defensive mode), checks that SQLite has FTS5 and that the file is a Retroverse catalog database or a new one; a
    /// file of another application is refused before anything is written to it. Errors: NotFound (no file,
    /// options.create off), Conflict (still migrated by another process after the wait; a file of another application,
    /// or holding tables without kApplicationId), Unsupported (SQLite without FTS5, a file system without WAL), IoError
    /// and the others SQLite reports.
    [[nodiscard]] static Expected<Database> open(const std::filesystem::path& file,
                                                 const DatabaseOptions& options = {});

    Database(Database&& other) noexcept = default;
    Database& operator=(Database&& other) noexcept = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database() = default;

    /// The database file, absolute.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    /// The process's one writer connection: used from one thread (the catalog worker, the CLI's main thread).
    [[nodiscard]] Connection& writer() noexcept { return m_writer; }
    /// The process-level lock this database holds.
    [[nodiscard]] DatabaseLock& lock() noexcept { return *m_lock; }

    /// A new read-only connection (SQLITE_OPEN_READONLY, no busy timeout): it reads the last committed snapshot and
    /// never waits on the writer or on another process; what would have to wait fails with Conflict at once. It keeps
    /// the process-level lock alive while it lives.
    [[nodiscard]] Expected<Connection> openReader() const;

    /// The writer's PRAGMA user_version.
    [[nodiscard]] Expected<int> schemaVersion() { return m_writer.userVersion(); }

    /// Copies the WAL into the database file and truncates it (wal_checkpoint(TRUNCATE)), waiting at most `wait` for
    /// readers of older snapshots. Errors: Conflict when they keep it from completing (left to the next checkpoint:
    /// nothing is lost), SQLite's others.
    [[nodiscard]] Status checkpoint(std::chrono::milliseconds wait);

    /// Writes a copy of the database as it is to `target`, a new file (VACUUM INTO): first under a temporary name
    /// ("<target>.partial", replaced when left by a copy cut short), written to its disk (FlushFileBuffers) and renamed
    /// to `target` once complete; the folders above it are created. Runs outside any transaction of the writer. Errors:
    /// Conflict when `target` exists, IoError when it cannot be written, SQLite's others.
    [[nodiscard]] Status backupTo(const std::filesystem::path& target);

    /// Interrupts the writer's running statement (Connection::interrupt()): for shutdown, from any thread.
    void interrupt() noexcept { m_writer.interrupt(); }

private:
    Database(std::filesystem::path path, std::shared_ptr<DatabaseLock> lock, Connection writer) noexcept;

    std::filesystem::path m_path;
    // Declared before the writer, so that the writer closes first.
    std::shared_ptr<DatabaseLock> m_lock;
    Connection m_writer;
};

} // namespace rv::catalog_db
